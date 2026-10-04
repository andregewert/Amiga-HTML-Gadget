/*
 * htmlttf_render.c - platform independent renderer of htmlttf.gadget
 *
 * Glyphs come from FreeType (anti-aliased, light autohinting) and are
 * cached per font variant and pixel size. Layout items are composited
 * into 32 bit ARGB strips; the platform writes them to the screen.
 * Must be called with the owner's lock held (FreeType is not reentrant).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "htmlttf_render.h"
#include FT_SYNTHESIS_H

#define DEF_BG     0xFFFFFFUL
#define COL_LIGHT  0xE0E0E0UL
#define COL_DARK   0x808080UL
#define PEN_TEXT        2
#define PEN_FILL        5
#define PEN_FILLTEXT    6
#define PEN_BACKGROUND  7

/* coverage gamma 1/1.4: gives anti-aliased stems more weight */
static const unsigned char gamma_tab[256] = {
      0,   5,   8,  11,  13,  15,  18,  20,  22,  23,  25,  27,  29,  30,  32,  34,
     35,  37,  38,  40,  41,  43,  44,  46,  47,  49,  50,  51,  53,  54,  55,  57,
     58,  59,  61,  62,  63,  64,  66,  67,  68,  69,  71,  72,  73,  74,  75,  77,
     78,  79,  80,  81,  82,  84,  85,  86,  87,  88,  89,  90,  91,  92,  94,  95,
     96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 134, 135, 136, 137, 138, 139, 140, 141, 142,
    143, 144, 145, 145, 146, 147, 148, 149, 150, 151, 152, 153, 153, 154, 155, 156,
    157, 158, 159, 160, 160, 161, 162, 163, 164, 165, 166, 166, 167, 168, 169, 170,
    171, 171, 172, 173, 174, 175, 176, 176, 177, 178, 179, 180, 180, 181, 182, 183,
    184, 185, 185, 186, 187, 188, 189, 189, 190, 191, 192, 193, 193, 194, 195, 196,
    196, 197, 198, 199, 200, 200, 201, 202, 203, 203, 204, 205, 206, 207, 207, 208,
    209, 210, 210, 211, 212, 213, 213, 214, 215, 216, 216, 217, 218, 219, 219, 220,
    221, 222, 222, 223, 224, 225, 225, 226, 227, 228, 228, 229, 230, 230, 231, 232,
    233, 233, 234, 235, 235, 236, 237, 238, 238, 239, 240, 240, 241, 242, 243, 243,
    244, 245, 245, 246, 247, 248, 248, 249, 250, 250, 251, 252, 252, 253, 254, 255
};

/* ------------------------------------------------------------------ */
/* fonts                                                               */

long tr_level_px(long base, int level)
{
    static const unsigned char f[7] = { 8, 10, 12, 14, 18, 24, 36 };   /* in 1/12 */
    long s = (base * f[level - 1] + 6) / 12;
    return s < 7 ? 7 : s;
}

static int var_index(int fixed, int style)
{
    return (fixed ? 4 : 0) + ((style & HS_BOLD) ? 2 : 0) + ((style & HS_ITALIC) ? 1 : 0);
}

struct TInst *tr_get_inst(struct TRender *r, int font, int style)
{
    int fixed = font >= 7, level = font % 7 + 1;
    int v = var_index(fixed, style), use = v;
    struct TInst *in;

    if ((in = r->inst[v][level - 1])) return in;
    /* fall back: without italic, without bold, the sans regular */
    if (!r->faces[use]) use = v & ~1;
    if (!r->faces[use]) use = v & ~3;
    if (!r->faces[use]) use = 0;
    if (!r->faces[use]) return 0;

    if (!(in = tr_alloc(sizeof(*in)))) return 0;
    in->face = r->faces[use];
    in->px = (unsigned short)tr_level_px(r->basepx, level);
    if ((v & 2) && !(use & 2)) in->synth |= 1;
    if ((v & 1) && !(use & 1)) in->synth |= 2;
    r->inst[v][level - 1] = in;
    return in;
}

struct TGlyph *tr_get_glyph(struct TInst *in, unsigned char c)
{
    struct TGlyph *g = &in->g[c];
    FT_GlyphSlot slot;
    FT_UInt idx;

    if (g->state) return g->state == 1 ? g : 0;
    g->state = 2;
    if (FT_Set_Pixel_Sizes(in->face, 0, in->px)) return 0;
    idx = FT_Get_Char_Index(in->face, c);
    if (!idx && c == 160) idx = FT_Get_Char_Index(in->face, ' ');
    /* IGNORE_GLOBAL_ADVANCE_WIDTH: FreeType 2.3.8 would give every glyph of
     * a fixed pitch font advanceWidthMax (1.8 em for Noto Sans Mono)     */
    if (FT_Load_Glyph(in->face, idx, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT |
                      FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH)) return 0;
    slot = in->face->glyph;
    if (in->synth & 1) FT_GlyphSlot_Embolden(slot);
    if (in->synth & 2) FT_GlyphSlot_Oblique(slot);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return 0;

    g->adv = (short)((slot->advance.x + 32) >> 6);
    g->left = (short)slot->bitmap_left;
    g->top = (short)slot->bitmap_top;
    g->w = (unsigned short)slot->bitmap.width;
    g->h = (unsigned short)slot->bitmap.rows;
    if (g->w && g->h) {
        long y;
        if (!(g->bits = tr_alloc(g->w * g->h))) return 0;
        for (y = 0; y < g->h; y++)
            h_memcpy(g->bits + y * g->w, slot->bitmap.buffer + y * slot->bitmap.pitch, g->w);
    }
    g->state = 1;
    return g;
}

void tr_free_insts(struct TRender *r)
{
    int v, l, c;
    for (v = 0; v < TR_NVAR; v++)
        for (l = 0; l < 7; l++) {
            struct TInst *in = r->inst[v][l];
            if (!in) continue;
            for (c = 0; c < 256; c++) if (in->g[c].bits) tr_free(in->g[c].bits);
            tr_free(in);
            r->inst[v][l] = 0;
        }
}

long tr_text_width(struct TRender *r, int font, int style, const char *s, long len)
{
    struct TInst *in = tr_get_inst(r, font, style);
    long w = 0;

    if (!in) return len * 6;
    while (len-- > 0) {
        struct TGlyph *g = tr_get_glyph(in, (unsigned char)*s++);
        if (g) w += g->adv;
    }
    return w;
}

void tr_metrics(struct TRender *r, struct HEnv *env)
{
    int i;
    for (i = 0; i < HF_NUM; i++) {
        struct TInst *in = tr_get_inst(r, i, 0);
        long asc = 0, desc = 0;
        if (in && !FT_Set_Pixel_Sizes(in->face, 0, in->px)) {
            asc = (in->face->size->metrics.ascender + 63) >> 6;
            desc = (-in->face->size->metrics.descender + 63) >> 6;
        }
        if (asc <= 0) asc = tr_level_px(r->basepx, i % 7 + 1);
        if (desc < 0) desc = 0;
        env->font_height[i] = (short)(asc + desc);
        env->font_baseline[i] = (short)asc;
    }
    env->generation++;
    if (!env->generation) env->generation = 1;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */

static unsigned long link_color(struct TRender *r, long link)
{
    struct HDoc *doc = r->doc;
    if (link >= 0 && link == r->activelink && r->pressed)
        return doc->alink != COL_NONE ? doc->alink : r->alink;
    if (link >= 0 && link < doc->nlinks && doc->visited[link])
        return doc->vlink != COL_NONE ? doc->vlink : r->vlink;
    return doc->link != COL_NONE ? doc->link : r->link;
}

static unsigned long to_rgb(struct TRender *r, unsigned long col, int deflt_pen, long link)
{
    if (col == COL_LINK) return link_color(r, link);
    if (col & COL_PEN) return r->penrgb[col & 0x0F];
    if (!COL_IS_RGB(col)) return r->penrgb[deflt_pen];
    return col;
}

/* ------------------------------------------------------------------ */
/* compositing                                                         */

static void s_fill(struct TStrip *s, long x0, long y0, long x1, long y1, unsigned long rgb)
{
    long x, y;
    x0 -= s->dx; x1 -= s->dx; y0 -= s->dy; y1 -= s->dy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    rgb |= 0xFF000000UL;
    for (y = y0; y <= y1; y++) {
        tr_u32 *p = s->buf + y * s->w + x0;
        for (x = x0; x <= x1; x++) *p++ = rgb;
    }
}

static unsigned long mix(unsigned long dst, unsigned long src, unsigned long a)
{
    unsigned long ia = 255 - a;
    unsigned long r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia + 127) / 255;
    unsigned long g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia + 127) / 255;
    unsigned long b = ((src & 0xFF) * a + (dst & 0xFF) * ia + 127) / 255;
    return 0xFF000000UL | (r << 16) | (g << 8) | b;
}

/* IT_CHECK: a run of pixels blended with its coverage */
static void s_span(void *ctx, long x0, long x1, long y, unsigned long rgb, unsigned alpha)
{
    struct TStrip *s = ctx;
    tr_u32 *p;
    long x;

    y -= s->dy;
    x0 -= s->dx;
    x1 -= s->dx;
    if (y < 0 || y >= s->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    rgb |= 0xFF000000UL;
    for (p = s->buf + y * s->w + x0, x = x0; x <= x1; x++, p++)
        *p = alpha >= 255 ? rgb : mix(*p, rgb, alpha);
}

static void s_glyph(struct TStrip *s, struct TGlyph *g, long x, long y, unsigned long rgb)
{
    long gx, gy, x0 = x - s->dx, y0 = y - s->dy;
    long sx = 0, sy = 0, w = g->w, h = g->h;

    if (x0 < 0) { sx = -x0; w -= sx; x0 = 0; }
    if (y0 < 0) { sy = -y0; h -= sy; y0 = 0; }
    if (x0 + w > s->w) w = s->w - x0;
    if (y0 + h > s->h) h = s->h - y0;
    if (w <= 0 || h <= 0) return;
    rgb |= 0xFF000000UL;
    for (gy = 0; gy < h; gy++) {
        unsigned char *src = g->bits + (sy + gy) * g->w + sx;
        tr_u32 *dst = s->buf + (y0 + gy) * s->w + x0;
        for (gx = 0; gx < w; gx++, dst++) {
            unsigned char c = src[gx];
            if (!c) continue;
            if (c == 255) *dst = rgb;
            else *dst = mix(*dst, rgb, gamma_tab[c]);
        }
    }
}

static void s_image(struct TStrip *s, struct TImage *im, long x, long y, long bw, long bh)
{
    long ix, iy, x0 = x - s->dx, y0 = y - s->dy;
    long sx = 0, sy = 0, w = bw < im->w ? bw : im->w, h = bh < im->h ? bh : im->h;

    if (x0 < 0) { sx = -x0; w -= sx; x0 = 0; }
    if (y0 < 0) { sy = -y0; h -= sy; y0 = 0; }
    if (x0 + w > s->w) w = s->w - x0;
    if (y0 + h > s->h) h = s->h - y0;
    if (w <= 0 || h <= 0) return;
    for (iy = 0; iy < h; iy++) {
        tr_u32 *src = im->pix + (sy + iy) * im->w + sx;
        tr_u32 *dst = s->buf + (y0 + iy) * s->w + x0;
        if (im->opaque) {
            h_memcpy(dst, src, w * 4);
            continue;
        }
        for (ix = 0; ix < w; ix++, dst++) {
            unsigned long p = src[ix], a = p >> 24;
            if (!a) continue;
            if (a == 255) *dst = p;
            else *dst = mix(*dst, p, a);
        }
    }
}

/* tiles 'im' over the document rectangle x0..x1/y0..y1, pattern origin ox/oy */
static void s_tile(struct TStrip *s, struct TImage *im, long ox, long oy,
                   long x0, long y0, long x1, long y1)
{
    long x, y;

    if (!im || !im->pix || im->w <= 0 || im->h <= 0) return;
    if (x0 < s->dx) x0 = s->dx;
    if (y0 < s->dy) y0 = s->dy;
    if (x1 > s->dx + s->w - 1) x1 = s->dx + s->w - 1;
    if (y1 > s->dy + s->h - 1) y1 = s->dy + s->h - 1;
    if (x0 > x1 || y0 > y1) return;
    for (y = y0; y <= y1; y++) {
        long sy = (y - oy) % im->h, sx;
        tr_u32 *src, *dst = s->buf + (y - s->dy) * s->w + (x0 - s->dx);
        if (sy < 0) sy += im->h;
        src = im->pix + sy * im->w;
        sx = (x0 - ox) % im->w;
        if (sx < 0) sx += im->w;
        for (x = x0; x <= x1; x++, dst++) {
            tr_u32 p = src[sx];
            unsigned long a = p >> 24;
            if (im->opaque || a == 255) *dst = p | 0xFF000000UL;
            else if (a) *dst = mix(*dst, p, a);
            if (++sx == im->w) sx = 0;
        }
    }
}

static void s_bevel(struct TStrip *s, long x, long y, long w, long h, unsigned long light, unsigned long dark)
{
    if (w < 2 || h < 2) return;
    s_fill(s, x, y, x + w - 1, y, light);
    s_fill(s, x, y, x, y + h - 1, light);
    s_fill(s, x + 1, y + h - 1, x + w - 1, y + h - 1, dark);
    s_fill(s, x + w - 1, y + 1, x + w - 1, y + h - 1, dark);
}

/* 'uend': right end of the underline (reaches the next word of a link);
 * characters [c0, c1) are selected and drawn in 'selrgb'                 */
static void s_text(struct TRender *r, struct TStrip *s, struct HItem *it, unsigned long rgb, long uend,
                   long c0, long c1, unsigned long selrgb)
{
    struct TInst *in = tr_get_inst(r, it->font, it->style);
    long x = it->x, base = it->y + it->base, i;

    if (in)
        for (i = 0; i < it->len; i++) {
            struct TGlyph *g = tr_get_glyph(in, (unsigned char)it->s[i]);
            if (!g) continue;
            if (g->bits && x + g->left < s->dx + s->w && x + g->left + g->w > s->dx)
                s_glyph(s, g, x + g->left, base - g->top, (i >= c0 && i < c1) ? selrgb : rgb);
            x += g->adv;
        }
    if (it->style & HS_UNDERLINED) {
        long uy = base + 1 + (in ? in->px / 12 : 0);
        s_fill(s, it->x, uy, uend - 1, uy + (in && in->px >= 20 ? 1 : 0), rgb);
    }
    if (it->style & HS_STRIKE) {
        long sy = base - (in ? in->px * 3 / 10 : 3);
        s_fill(s, it->x, sy, it->x + it->w - 1, sy, rgb);
    }
}

void tr_render_strip(struct TRender *r, struct HLayout *lay, int syscolors, struct TStrip *s)
{
    unsigned long bg = to_rgb(r, lay ? lay->bgcolor : (syscolors ? COL_PEN | PEN_BACKGROUND : DEF_BG),
                              PEN_BACKGROUND, -1);
    long i, ytop = s->dy, ybot = s->dy + s->h, xl = s->dx, xr = s->dx + s->w;

    s_fill(s, s->dx, s->dy, s->dx + s->w - 1, s->dy + s->h - 1, bg);
    if (r->bgimg) s_tile(s, r->bgimg, 0, 0, s->dx, s->dy, s->dx + s->w - 1, s->dy + s->h - 1);
    if (!lay) return;

    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        if (it->type == IT_TEXT) {
            /* the underline may lie below the box and reach to the next
             * word, glyphs may stick out: a margin of one line height   */
            if (it->y >= ybot || it->y + 2 * it->h <= ytop || it->x >= xr) continue;
        } else if (it->type == IT_BULLET) {
            /* drawn it->w high; it->h may be a pixel less after scaling */
            long sz = it->w > it->h ? it->w : it->h;
            if (it->y >= ybot || it->y + sz <= ytop || it->x >= xr || it->x + sz <= xl) continue;
        } else if (it->y >= ybot || it->y + it->h <= ytop || it->x >= xr || it->x + it->w <= xl) continue;

        switch (it->type) {
        case IT_TEXT: {
            long uend = html_underline_end(lay, i);
            {
                long c0 = -1, c1 = -1;
                if (r->sel && r->env && html_sel_part(r->sel, lay, i, &c0, &c1)) {
                    long hx0 = it->x + html_char_x(r->env, it, c0);
                    long hx1 = html_sel_right(r->sel, lay, r->env, i, c1);
                    s_fill(s, hx0, it->y, hx1 - 1, it->y + it->h - 1, r->penrgb[PEN_FILL]);
                }
                s_text(r, s, it, to_rgb(r, it->color, PEN_TEXT, it->link), uend, c0, c1, r->penrgb[PEN_FILLTEXT]);
            }
            break;
        }
        case IT_RECT:
            if (it->color != COL_NONE)
                s_fill(s, it->x, it->y, it->x + it->w - 1, it->y + it->h - 1,
                       to_rgb(r, it->color, PEN_BACKGROUND, -1));
            if (it->img)
                s_tile(s, it->img, it->x, it->y, it->x, it->y, it->x + it->w - 1, it->y + it->h - 1);
            break;
        case IT_HRULE:
            if (it->style & HR_NOSHADE)
                s_fill(s, it->x, it->y, it->x + it->w - 1, it->y + it->h - 1,
                       to_rgb(r, it->color, PEN_TEXT, -1));
            else
                s_bevel(s, it->x, it->y, it->w, it->h, COL_DARK, COL_LIGHT);
            break;
        case IT_FRAME:
            if (it->style & FR_RAISED) s_bevel(s, it->x, it->y, it->w, it->h, COL_LIGHT, COL_DARK);
            else s_bevel(s, it->x, it->y, it->w, it->h, COL_DARK, COL_LIGHT);
            break;
        case IT_IMAGE:
            if (it->img) s_image(s, it->img, it->x, it->y, it->w, it->h);
            else s_bevel(s, it->x, it->y, it->w, it->h, COL_DARK, COL_LIGHT);
            break;
        case IT_CHECK:
            html_check_paint(it, s_span, s);
            break;
        case IT_BULLET: {
            long x = it->x, y = it->y, sz = it->w;
            unsigned long c = to_rgb(r, it->color, PEN_TEXT, -1);
            if (it->style == BUL_SQUARE) {
                s_fill(s, x, y, x + sz - 1, y + sz - 1, c);
            } else if (it->style == BUL_CIRCLE) {
                s_fill(s, x + 1, y, x + sz - 2, y, c);
                s_fill(s, x + 1, y + sz - 1, x + sz - 2, y + sz - 1, c);
                s_fill(s, x, y + 1, x, y + sz - 2, c);
                s_fill(s, x + sz - 1, y + 1, x + sz - 1, y + sz - 2, c);
            } else {
                s_fill(s, x + 1, y, x + sz - 2, y + sz - 1, c);
                s_fill(s, x, y + 1, x + sz - 1, y + sz - 2, c);
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* pictures                                                            */

/* resamples ARGB: area average when shrinking, bilinear when enlarging.
 * Colours are weighted with alpha so transparent pixels do not bleed.   */
tr_u32 *tr_scale_argb(tr_u32 *src, long sw, long sh, long dw, long dh)
{
    tr_u32 *dst = tr_alloc(dw * dh * 4), *o;
    long x, y;

    if (!dst) return NULL;
    o = dst;
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            unsigned long asum = 0, rs = 0, gs = 0, bs = 0, aout, div;
            if (dw <= sw && dh <= sh) {
                /* box filter: plain sums, weight 1 per source pixel */
                long x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh, xx, yy;
                long stx, sty;
                unsigned long cnt = 0;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                /* at most 64x64 samples per pixel: keeps the sums in 32 bit */
                stx = (x1 - x0) / 64 + 1;
                sty = (y1 - y0) / 64 + 1;
                for (yy = y0; yy < y1; yy += sty)
                    for (xx = x0; xx < x1; xx += stx) {
                        unsigned long p = src[yy * sw + xx], pa = p >> 24;
                        asum += pa;
                        rs += ((p >> 16) & 0xFF) * pa;
                        gs += ((p >> 8) & 0xFF) * pa;
                        bs += (p & 0xFF) * pa;
                        cnt++;
                    }
                aout = asum / cnt;
                div = asum;
            } else {
                /* bilinear: weights 0..256 per axis, 65536 in total */
                long fx = ((2 * x + 1) * sw * 32768L) / dw - 32768L;
                long fy = ((2 * y + 1) * sh * 32768L) / dh - 32768L;
                long ix0, iy0, ix1, iy1;
                unsigned long wx, wy, k;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                ix0 = fx >> 16; iy0 = fy >> 16;
                if (ix0 >= sw) ix0 = sw - 1;
                if (iy0 >= sh) iy0 = sh - 1;
                wx = (fx >> 8) & 0xFF; wy = (fy >> 8) & 0xFF;
                ix1 = ix0 + 1 < sw ? ix0 + 1 : ix0;
                iy1 = iy0 + 1 < sh ? iy0 + 1 : iy0;
                for (k = 0; k < 4; k++) {
                    long px = (k & 1) ? ix1 : ix0, py = (k & 2) ? iy1 : iy0;
                    unsigned long wgt = (((k & 1) ? wx : 256 - wx) * ((k & 2) ? wy : 256 - wy)) >> 8;  /* 0..256 */
                    unsigned long p = src[py * sw + px], pw = (p >> 24) * wgt;                    /* alpha*256 */
                    asum += pw;
                    rs += ((p >> 16) & 0xFF) * pw;
                    gs += ((p >> 8) & 0xFF) * pw;
                    bs += (p & 0xFF) * pw;
                }
                aout = asum >> 8;
                div = asum;
            }
            if (!div || !aout) { *o++ = 0; continue; }
            rs /= div; gs /= div; bs /= div;
            if (aout > 255) aout = 255;
            *o++ = (aout << 24) | ((rs > 255 ? 255 : rs) << 16) | ((gs > 255 ? 255 : gs) << 8) | (bs > 255 ? 255 : bs);
        }
    }
    return dst;
}

