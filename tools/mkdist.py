#!/usr/bin/env python3
# mkdist.py - builds the Aminet archive html_gadget.lha from the built tree
#
# Copyright (c) 2026 Andre Gewert <agewert@ubergeek.de>, MIT License
#
# Creates dist/html_gadget/ with icons (.info, generated here as classic
# 4 colour DiskObjects), the Installer script, docs, developer files and
# sources, then packs it with lha. Run "make dist".

import os, shutil, struct, subprocess, glob, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FT = os.environ.get('FT', os.path.expanduser('~/AmiLib/freetype-2.3.8'))
DIST = os.path.join(ROOT, 'dist')
PKG = os.path.join(DIST, 'html_gadget')

# ---------------------------------------------------------------- icons ---

WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_POS = 0x80000000


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


def diskobject(kind, pic, default_tool=None, tooltypes=(), stack=0):
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
    return data


def icon(path, *args, **kw):
    with open(path + '.info', 'wb') as f:
        f.write(diskobject(*args, **kw))

# ------------------------------------------------------------- content ---


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst, ignore=()):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(*ignore), dirs_exist_ok=True)


def to_latin1_text(md):
    table = {'„': '"', '“': '"', '”': '"', '–': '-', '—': '-',
             '…': '...', '→': '->', '≤': '<=', '≥': '>=', '’': "'"}
    return ''.join(table.get(ch, ch) for ch in md).encode('latin-1', 'replace')


def main():
    b = lambda *p: os.path.join(ROOT, *p)
    shutil.rmtree(DIST, ignore_errors=True)
    os.makedirs(PKG)

    copy(b('package', 'html_gadget.readme'), os.path.join(PKG, 'html_gadget.readme'))
    copy(b('package', 'Install'), os.path.join(PKG, 'Install'))
    copy(b('LICENSE'), os.path.join(PKG, 'LICENSE'))
    copy(os.path.join(FT, 'docs', 'FTL.TXT'), os.path.join(PKG, 'Licenses', 'FTL.TXT'))
    copy(b('demo', 'fonts', 'Vera-COPYRIGHT.TXT'), os.path.join(PKG, 'Licenses', 'Vera-COPYRIGHT.TXT'))

    for g in ('html.gadget', 'htmlttf.gadget'):
        copy(b('bin', g), os.path.join(PKG, 'Classes', 'Gadgets', g))

    demo = os.path.join(PKG, 'Demo')
    copy(b('bin', 'HTMLDemo'), os.path.join(demo, 'HTMLDemo'))
    for f in glob.glob(b('demo', '*.html')) + glob.glob(b('demo', '*.gif')) + glob.glob(b('demo', '*.iff')):
        copy(f, os.path.join(demo, os.path.basename(f)))
    for f in glob.glob(b('demo', 'fonts', 'Vera*')):
        copy(f, os.path.join(demo, 'fonts', os.path.basename(f)))

    dev = os.path.join(PKG, 'Developer')
    for sub in ('gadgets', 'proto', 'inline', 'clib'):
        copytree(b('include', sub), os.path.join(dev, 'Include', sub))
    copytree(b('include', 'fd'), os.path.join(dev, 'FD'))
    copytree(b('sfd'), os.path.join(dev, 'SFD'))
    copytree(b('doc'), os.path.join(dev, 'Autodocs'))

    docs = os.path.join(PKG, 'Docs')
    os.makedirs(docs)
    with open(b('README.md'), encoding='utf-8') as f:
        text = to_latin1_text(f.read())
    with open(os.path.join(docs, 'LiesMich.txt'), 'wb') as f:
        f.write(text)

    src = os.path.join(PKG, 'Source')
    for d in ('src', 'ttf', 'include', 'sfd', 'doc', 'package', 'tools'):
        copytree(b(d), os.path.join(src, d), ignore=('__pycache__',))
    copytree(b('demo'), os.path.join(src, 'demo'), ignore=('fonts',))
    for f in glob.glob(b('test', '*.c')):
        copy(f, os.path.join(src, 'test', os.path.basename(f)))
    for f in ('Makefile', 'README.md', 'LICENSE'):
        copy(b(f), os.path.join(src, f))

    # icons
    mv = 'SYS:Utilities/MultiView'
    icon(PKG, WBDRAWER, pic_drawer())
    for d in ('Classes', 'Demo', 'Developer', 'Docs', 'Source', 'Licenses'):
        icon(os.path.join(PKG, d), WBDRAWER, pic_drawer())
    icon(os.path.join(PKG, 'Install'), WBPROJECT, pic_install(), default_tool='Installer',
         tooltypes=('APPNAME=html.gadget', 'MINUSER=AVERAGE'))
    icon(os.path.join(PKG, 'html_gadget.readme'), WBPROJECT, pic_page(), default_tool=mv)
    icon(os.path.join(PKG, 'LICENSE'), WBPROJECT, pic_page(1), default_tool=mv)
    icon(os.path.join(docs, 'LiesMich.txt'), WBPROJECT, pic_page(), default_tool=mv)
    icon(os.path.join(demo, 'HTMLDemo'), WBTOOL, pic_demo(), stack=16384)
    icon(os.path.join(dev, 'Autodocs'), WBDRAWER, pic_drawer())

    # archive
    arc = os.path.join(DIST, 'html_gadget.lha')
    files = ['html_gadget.info']
    for dirpath, dirnames, filenames in os.walk(PKG):
        dirnames.sort()
        for fn in sorted(filenames):
            files.append(os.path.relpath(os.path.join(dirpath, fn), DIST))
    # an LhA that can create archives: jlha (Debian: jlha-utils) or lha for UNIX
    tool = shutil.which('jlha') or 'lha'
    subprocess.run([tool, 'ao5q', arc] + files, cwd=DIST, check=True)
    shutil.copy2(os.path.join(PKG, 'html_gadget.readme'), os.path.join(DIST, 'html_gadget.readme'))
    print('created', arc, os.path.getsize(arc), 'bytes')


if __name__ == '__main__':
    main()
