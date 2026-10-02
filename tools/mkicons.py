#!/usr/bin/env python3
# mkicons.py - Workbench icons of the html.gadget distribution
#
# Copyright (c) 2026 André Gewert <agewert@ubergeek.de>, MIT License
#
# Three icon styles, all drawn here (no picture files needed):
#   Classic    4 colour planar image (Workbench 2.x/3.x palette)
#   GlowIcons  OS 3.5+ colour icon: IFF FORM ICON appended to the DiskObject,
#              selected image with the typical orange glow
#   NewIcons   colour image encoded in IM1=/IM2= tool types
# Glow and NewIcons icons keep the classic picture as planar fallback image.
#
# Run directly (or "make icons") to write sample icons of all styles to
# icons/<Style>/ and a preview picture icons/preview.png. mkdist.py uses
# write_icon() to give every file of the archive its icon.

import os, struct, zlib

WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_POS = 0x80000000
STYLES = ('Classic', 'GlowIcons', 'NewIcons')


# ------------------------------------------------------ classic pictures ---

class Canvas:
    """ 4 colour picture: 0 grey, 1 black, 2 white, 3 blue (WB palette) """
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[0] * w for _ in range(h)]

    def rect(self, x0, y0, x1, y1, c):
        for y in range(max(y0, 0), min(y1, self.h - 1) + 1):
            for x in range(max(x0, 0), min(x1, self.w - 1) + 1):
                self.p[y][x] = c

    def frame(self, x0, y0, x1, y1, light, dark):
        self.rect(x0, y0, x1, y0, light)
        self.rect(x0, y0, x0, y1, light)
        self.rect(x0, y1, x1, y1, dark)
        self.rect(x1, y0, x1, y1, dark)

    def planes(self):
        bpr = (self.w + 15) // 16 * 2
        out = b''
        for bit in (0, 1):
            for row in self.p:
                line = bytearray(bpr)
                for x, c in enumerate(row):
                    if c & (1 << bit):
                        line[x >> 3] |= 0x80 >> (x & 7)
                out += bytes(line)
        return out


def pic_drawer():
    c = Canvas(40, 22)
    c.rect(2, 2, 37, 20, 2)
    c.frame(2, 2, 37, 20, 2, 1)
    c.rect(5, 6, 34, 17, 0)
    c.frame(5, 6, 34, 17, 1, 2)
    c.rect(15, 10, 24, 12, 3)
    c.frame(15, 10, 24, 12, 1, 1)
    return c


def pic_page(accent=3):
    c = Canvas(32, 24)
    c.rect(4, 1, 25, 22, 2)
    c.frame(4, 1, 25, 22, 1, 1)
    c.rect(20, 1, 25, 6, 0)
    c.rect(20, 1, 20, 6, 1)
    c.rect(20, 6, 25, 6, 1)
    for i, y in enumerate(range(5, 21, 3)):
        c.rect(7, y, 17 if y < 8 else 22 - (i % 3) * 3, y, 1 if i else accent)
    return c


def pic_demo():
    c = Canvas(40, 24)
    c.rect(1, 1, 38, 22, 2)
    c.frame(1, 1, 38, 22, 1, 1)
    c.rect(2, 2, 37, 5, 3)
    c.rect(4, 8, 20, 9, 1)                 # heading
    for y in (12, 15, 18):
        c.rect(4, y, 33 if y != 18 else 24, y, 1)
    c.rect(26, 8, 33, 9, 3)                # "link"
    return c


def pic_install():
    c = Canvas(40, 24)
    c.rect(6, 10, 33, 22, 0)
    c.frame(6, 10, 33, 22, 2, 1)
    c.rect(7, 11, 32, 21, 3)
    c.rect(17, 1, 22, 9, 1)                # arrow shaft
    for i in range(6):
        c.rect(14 + i, 10 + i, 25 - i, 10 + i, 1)
    c.rect(18, 2, 21, 9, 2)
    for i in range(5):
        c.rect(15 + i, 11 + i, 24 - i, 11 + i, 2)
    return c


def pic_tiles():
    c = Canvas(36, 24)
    for x, y, col in ((3, 2, 2), (12, 7, 3), (21, 12, 2)):
        c.rect(x, y, x + 11, y + 9, col)
        c.frame(x, y, x + 11, y + 9, 2, 1)
        c.rect(x, y, x + 11, y, 1)
        c.rect(x, y, x, y + 9, 1)
    return c


CLASSIC = {'drawer': pic_drawer, 'readme': pic_page, 'license': lambda: pic_page(1),
           'demo': pic_demo, 'install': pic_install, 'tiles': pic_tiles}


# --------------------------------------------------- true colour drawing ---

def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def dim(c, f):
    return tuple(int(round(v * f)) for v in c)


class RGB:
    """ true colour picture, None = transparent """
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[None] * w for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c(x, y) if callable(c) else c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def poly(self, pts, fill, outline=None):
        """ fills every pixel whose centre is inside or on the edge """
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            for x in range(int(min(xs)), int(max(xs)) + 1):
                if inside(pts, x, y):
                    self.put(x, y, fill(x, y) if callable(fill) else fill)
        if outline:
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                self.line(ax, ay, bx, by, outline)

    def disc(self, cx, cy, r, fill, outline=None):
        for y in range(cy - r - 1, cy + r + 2):
            for x in range(cx - r - 1, cx + r + 2):
                d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
                if d <= r + 0.3:
                    if outline and d > r - 0.7:
                        self.put(x, y, outline)
                    else:
                        self.put(x, y, fill(x, y) if callable(fill) else fill)

    def copy(self):
        c = RGB(self.w, self.h)
        c.p = [row[:] for row in self.p]
        return c

    def colours(self):
        return sorted({c for row in self.p for c in row if c is not None})


def inside(pts, x, y):
    n, hit = len(pts), False
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            hit = not hit
        # on the edge counts as inside
        vx, vy, l2 = bx - ax, by - ay, (bx - ax) ** 2 + (by - ay) ** 2
        t = 0 if l2 == 0 else max(0, min(1, ((x - ax) * vx + (y - ay) * vy) / l2))
        if (x - ax - t * vx) ** 2 + (y - ay - t * vy) ** 2 <= 0.25:
            return True
    return hit


# ------------------------------------------------------------- styles ---

class Style:
    """ GlowIcons: soft gradients, outlines in a dark shade of the colour.
        NewIcons:  black outlines, two tones per colour (few colours). """
    def __init__(self, glow):
        self.glow = glow

    def ramp(self, light, dark):
        """ shading function for a box: light at the top, dark at the bottom
            (slightly darker to the right), 5 (Glow) or 2 (NewIcons) tones """
        steps = 5 if self.glow else 2
        tones = [mix(light, dark, i / (steps - 1)) for i in range(steps)]

        def shade(x0, y0, x1, y1):
            span = max(1, (x1 - x0) * 0.08 + (y1 - y0))
            def f(x, y):
                t = ((x - x0) * 0.08 + (y - y0)) / span
                return tones[max(0, min(steps - 1, int(t * steps)))]
            return f
        return shade

    def line(self, c):
        return dim(c, 0.45) if self.glow else BLACK


BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
PAPER = ((255, 255, 255), (206, 210, 222))
TEXT = (96, 100, 116)
BLUE = ((96, 150, 255), (24, 64, 176))
FOLDER = ((255, 226, 140), (214, 150, 48))
RED = ((255, 96, 80), (176, 24, 24))
GREEN = ((120, 230, 90), (24, 140, 40))
BOX = ((230, 190, 130), (150, 100, 50))
GREY = ((220, 220, 226), (140, 140, 152))


def m_drawer(s, c, opened=False):
    sh = s.ramp(*FOLDER)
    back = [(3, 5), (15, 5), (17, 8), (38, 8), (38, 33), (3, 33)]
    c.poly(back, dim(FOLDER[1], 0.85), s.line(FOLDER[1]))
    if opened:
        c.poly([(8, 10), (32, 10), (32, 24), (8, 24)], WHITE, s.line(GREY[1]))
        for y in (13, 16, 19):
            c.line(11, y, 28, y, TEXT)
        front = [(0, 18), (35, 18), (38, 34), (3, 34)]
    else:
        front = [(3, 13), (38, 13), (38, 34), (3, 34)]
    c.poly(front, sh(0, front[0][1], 38, 34), s.line(FOLDER[1]))
    hl = mix(FOLDER[0], WHITE, 0.6)
    c.line(front[0][0] + 1, front[0][1] + 1, front[1][0] - 1, front[1][1] + 1, hl)


def m_page(s, c, seal=False):
    sheet = [(8, 2), (27, 2), (34, 9), (34, 35), (8, 35)]
    c.poly(sheet, s.ramp(*PAPER)(8, 2, 34, 35), s.line(GREY[1]))
    c.poly([(27, 2), (27, 9), (34, 9)], GREY[0], s.line(GREY[1]))
    c.rect(12, 7, 22, 8, BLUE[1])
    for i, y in enumerate(range(12, 32, 3)):
        if seal and y > 22:
            break
        c.line(12, y, 30 - (i % 3) * 4, y, TEXT)
    if seal:
        c.poly([(24, 30), (27, 30), (26, 37), (24, 35)], RED[1], s.line(RED[1]))
        c.poly([(29, 30), (32, 30), (32, 35), (30, 37)], RED[1], s.line(RED[1]))
        c.disc(28, 28, 5, s.ramp(*RED)(23, 23, 33, 33), s.line(RED[1]))
        c.disc(28, 28, 2, mix(RED[0], WHITE, 0.5))


def m_demo(s, c):
    c.rect(2, 4, 39, 34, s.line(GREY[1]))
    c.rect(3, 5, 38, 8, s.ramp(*BLUE)(3, 5, 38, 8))
    c.rect(4, 6, 6, 7, WHITE)
    c.rect(3, 9, 38, 33, s.ramp(WHITE, PAPER[1])(3, 9, 38, 33))
    c.rect(6, 12, 19, 13, s.line(GREY[1]) if s.glow else BLACK)
    for y, x1 in ((17, 22), (20, 20), (23, 22)):
        c.line(6, y, x1, y, TEXT)
    c.line(6, 27, 15, 27, BLUE[1])
    c.line(6, 28, 15, 28, BLUE[1])

    def boing(x, y):
        return RED[1] if ((x - 25) // 3 + (y - 16) // 3) % 2 else WHITE
    c.disc(31, 22, 6, boing, s.line(RED[1]))


def m_install(s, c):
    c.poly([(6, 18), (12, 13), (36, 13), (30, 18)], dim(BOX[1], 0.55), s.line(BOX[1]))
    c.poly([(30, 18), (36, 13), (36, 30), (30, 35)], dim(BOX[1], 0.85), s.line(BOX[1]))
    c.poly([(6, 18), (30, 18), (30, 35), (6, 35)], s.ramp(*BOX)(6, 18, 30, 35), s.line(BOX[1]))
    c.poly([(18, 1), (24, 1), (24, 9), (29, 9), (21, 18), (13, 9), (18, 9)],
           s.ramp(*GREEN)(13, 1, 29, 18), s.line(GREEN[1]))


def m_tiles(s, c):
    for x, y, col in ((3, 3, RED), (13, 11, GREEN), (23, 19, BLUE)):
        c.rect(x, y, x + 14, y + 14, s.line(col[1]))
        c.rect(x + 1, y + 1, x + 13, y + 13, s.ramp(*col)(x, y, x + 14, y + 14))
        c.line(x + 2, y + 2, x + 10, y + 2, mix(col[0], WHITE, 0.6))


MOTIFS = {'drawer': m_drawer, 'readme': m_page, 'license': lambda s, c: m_page(s, c, True),
          'demo': m_demo, 'install': m_install, 'tiles': m_tiles}

W, H = 42, 38          # motif size; NewIcons need a width that is a multiple of 14
GLOW_MARGIN = 3


def draw(role, glow, selected):
    s = Style(glow)
    c = RGB(W, H)
    if role == 'drawer':
        m_drawer(s, c, opened=selected)
    else:
        MOTIFS[role](s, c)
    if not glow:
        return c
    g = RGB(W + 2 * GLOW_MARGIN, H + 2 * GLOW_MARGIN)
    for y in range(H):
        for x in range(W):
            g.p[y + GLOW_MARGIN][x + GLOW_MARGIN] = c.p[y][x]
    if selected:
        add_glow(g)
    return g


GLOW = ((255, 250, 170), (255, 210, 70), (255, 150, 30))


def add_glow(c):
    solid = [(x, y) for y in range(c.h) for x in range(c.w) if c.p[y][x] is not None]
    for y in range(c.h):
        for x in range(c.w):
            if c.p[y][x] is None:
                d = min((((x - a) ** 2 + (y - b) ** 2) ** 0.5 for a, b in solid
                         if abs(x - a) <= 3 and abs(y - b) <= 3), default=9)
                if d <= 3.2:
                    c.p[y][x] = GLOW[0 if d <= 1.5 else 1 if d <= 2.3 else 2]


# ------------------------------------------------------ GlowIcons (IFF) ---

class Bits:
    def __init__(self):
        self.acc, self.n, self.out = 0, 0, bytearray()

    def put(self, v, bits):
        self.acc = (self.acc << bits) | v
        self.n += bits
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def done(self):
        if self.n:
            self.out.append((self.acc << (8 - self.n)) & 0xFF)
        return bytes(self.out)


def rle(values, depth):
    """ PackBits on a bit stream of 'depth' bit values (ColorIcon format) """
    b, i, n = Bits(), 0, len(values)
    while i < n:
        run = 1
        while i + run < n and run < 128 and values[i + run] == values[i]:
            run += 1
        if run >= 3:
            b.put((257 - run) & 0xFF, 8)
            b.put(values[i], depth)
            i += run
            continue
        j = i
        while j < n and j - i < 128 and not (j + 2 < n and values[j] == values[j + 1] == values[j + 2]):
            j += 1
        b.put(j - i - 1, 8)
        for v in values[i:j]:
            b.put(v, depth)
        i = j
    return b.done()


def chunk(cid, data):
    return cid + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def glow_form(normal, selected):
    pal = [None] + sorted(set(normal.colours()) | set(selected.colours()))
    assert len(pal) <= 256
    index = {c: i for i, c in enumerate(pal)}
    depth = max(1, (len(pal) - 1).bit_length())
    palbytes = rle([0, 0, 0] + [v for c in pal[1:] for v in c], 8)
    body = struct.pack('>BBBBH', normal.w - 1, normal.h - 1, 1, 0x11, len(pal) * 3 - 1)
    data = chunk(b'FACE', body)
    for img in (normal, selected):
        pix = rle([index[c] for row in img.p for c in row], depth)
        data += chunk(b'IMAG', struct.pack('>BBBBBBHH', 0, len(pal) - 1, 3, 1, 1, depth,
                                           len(pix) - 1, len(palbytes) - 1) + pix + palbytes)
    return chunk(b'FORM', b'ICON' + data)


# ---------------------------------------------------- NewIcons (tooltypes) ---

NI_COLOURS = 14        # 14 colours: palette ends exactly on a 7 bit boundary
NI_ROWS = 5            # rows per tool type line: 5 * 42 px * 4 bit = 120 chars


def ni_chars(bits):
    out = ''
    for i in range(0, len(bits), 7):
        v = int(bits[i:i + 7].ljust(7, '0'), 2)
        out += chr(v + 0x20 if v < 0x50 else v + 0x51)
    return out


def ni_image(tag, img):
    cols = img.colours()
    assert len(cols) < NI_COLOURS, '%d colours' % len(cols)
    pal = [BLACK] + cols + [BLACK] * (NI_COLOURS - 1 - len(cols))
    index = {c: i for i, c in enumerate(pal[:len(cols) + 1]) if i}
    head = 'B' + chr(img.w + 0x21) + chr(img.h + 0x21) + \
        chr((NI_COLOURS >> 6) + 0x21) + chr((NI_COLOURS & 0x3F) + 0x21)
    lines = [head + ni_chars(''.join('{:08b}'.format(v) for c in pal for v in c))]
    for y in range(0, img.h, NI_ROWS):
        bits = ''.join('{:04b}'.format(0 if c is None else index[c])
                       for row in img.p[y:y + NI_ROWS] for c in row)
        lines.append(ni_chars(bits))
    return [tag + l for l in lines]


def ni_tooltypes(normal, selected):
    return (' ', "*** DON'T EDIT THE FOLLOWING LINES!! ***") + \
        tuple(ni_image('IM1=', normal) + ni_image('IM2=', selected))


def ni_pressed(img):
    c = img.copy()
    c.p = [[None if v is None else dim(v, 0.7) for v in row] for row in c.p]
    return c


# --------------------------------------------------------- DiskObject ---

def diskobject(kind, pic, default_tool=None, tooltypes=(), stack=0, extra=b''):
    w, h = pic.w, pic.h
    drawer = kind in (WBDRAWER, WBDISK)
    gadget = struct.pack('>IhhhhHHHIIIiIHI',
                         0, 0, 0, w, h,
                         0x0004,          # GFLG_GADGIMAGE, complement highlight
                         0x0003,          # RELVERIFY | GADGIMMEDIATE
                         0x0001,          # BOOLGADGET
                         1, 0, 0, 0, 0, 0,
                         1)               # UserData: WB 2.x revision
    data = struct.pack('>HH', 0xE310, 1) + gadget
    data += struct.pack('>BBIIIIIII', kind, 0,
                        1 if default_tool else 0,
                        1 if tooltypes else 0,
                        NO_POS, NO_POS,
                        1 if drawer else 0,
                        0, stack)
    assert len(data) == 78
    if drawer:
        # OldDrawerData: NewWindow (48 bytes) + CurrentX/Y
        data += struct.pack('>hhhhBBIIIIIIIhhhhH', 60, 40, 420, 200, 255, 255,
                            0, 0, 0, 0, 0, 0, 0, 90, 40, -1, -1, 1)
        data += struct.pack('>ii', 0, 0)
    img = pic.planes()
    data += struct.pack('>hhhhhIBBI', 0, 0, w, h, 2, 1, 3, 0, 0) + img
    if default_tool:
        t = default_tool.encode('latin-1') + b'\0'
        data += struct.pack('>I', len(t)) + t
    if tooltypes:
        data += struct.pack('>I', (len(tooltypes) + 1) * 4)
        for tt in tooltypes:
            t = tt.encode('latin-1') + b'\0'
            data += struct.pack('>I', len(t)) + t
    if drawer:
        data += struct.pack('>IH', 0, 0)       # DrawerData2: flags, view modes
    return data + extra


def make_icon(style, role, kind, default_tool=None, tooltypes=(), stack=0):
    pic = CLASSIC[role]()
    if style == 'GlowIcons':
        extra = glow_form(draw(role, True, False), draw(role, True, True))
        return diskobject(kind, pic, default_tool, tooltypes, stack, extra)
    if style == 'NewIcons':
        n = draw(role, False, False)
        sel = draw(role, False, True) if role == 'drawer' else ni_pressed(n)
        return diskobject(kind, pic, default_tool, tuple(tooltypes) + ni_tooltypes(n, sel), stack)
    return diskobject(kind, pic, default_tool, tooltypes, stack)


def write_icon(path, style, role, kind, **kw):
    """ writes path + '.info' """
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path + '.info', 'wb') as f:
        f.write(make_icon(style, role, kind, **kw))


# ------------------------------------------------------------ preview ---

WB_PEN = ((170, 170, 170), BLACK, WHITE, (102, 136, 187))


def classic_rgb(pic, selected):
    c = RGB(pic.w, pic.h)
    for y in range(pic.h):
        for x in range(pic.w):
            v = pic.p[y][x]
            c.p[y][x] = WB_PEN[v ^ 3 if selected else v]
    return c


def preview(path, scale=3):
    roles = list(MOTIFS)
    cell_w, cell_h, gap = 2 * (W + 2 * GLOW_MARGIN) + 8, H + 2 * GLOW_MARGIN, 10
    pw = (cell_w + gap) * len(roles) + gap
    ph = (cell_h + gap) * len(STYLES) + gap
    bg = WB_PEN[0]
    out = [[bg] * pw for _ in range(ph)]
    for r, style in enumerate(STYLES):
        for k, role in enumerate(roles):
            for sel in (0, 1):
                if style == 'Classic':
                    img = classic_rgb(CLASSIC[role](), sel)
                elif style == 'GlowIcons':
                    img = draw(role, True, sel)
                else:
                    img = draw(role, False, False)
                    img = (draw(role, False, True) if role == 'drawer' else ni_pressed(img)) if sel else img
                ox = gap + k * (cell_w + gap) + sel * (cell_w // 2)
                oy = gap + r * (cell_h + gap)
                for y in range(img.h):
                    for x in range(img.w):
                        if img.p[y][x] is not None:
                            out[oy + y][ox + x] = img.p[y][x]
    raw = b''.join(b'\0' + b''.join(bytes(c) * scale for c in row) for row in out for _ in range(scale))

    def png_chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' +
                png_chunk(b'IHDR', struct.pack('>IIBBBBB', pw * scale, ph * scale, 8, 2, 0, 0, 0)) +
                png_chunk(b'IDAT', zlib.compress(raw, 9)) + png_chunk(b'IEND', b''))


SAMPLES = (('Drawer', 'drawer', WBDRAWER, {}),
           ('ReadMe', 'readme', WBPROJECT, {'default_tool': 'SYS:Utilities/MultiView'}),
           ('License', 'license', WBPROJECT, {'default_tool': 'SYS:Utilities/MultiView'}),
           ('HTMLDemo', 'demo', WBTOOL, {'stack': 16384}),
           ('Install', 'install', WBPROJECT, {'default_tool': 'Installer'}),
           ('Script', 'tiles', WBPROJECT, {'default_tool': 'C:IconX'}))


def main():
    root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'icons')
    for style in STYLES:
        for name, role, kind, kw in SAMPLES:
            write_icon(os.path.join(root, style, name), style, role, kind, **kw)
    preview(os.path.join(root, 'preview.png'))
    print('icons written to', root)


if __name__ == '__main__':
    main()
