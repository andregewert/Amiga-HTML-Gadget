/*
 * htmlttf_print.c - bitmap printing with the FreeType renderer, see
 * htmlttf_print.h
 *
 * Two copies of the layout: one in 1/96 inch (as html_print.c, so sizes
 * in HTML attributes come out as on paper and the page breaks are the
 * same), measured with the fonts in printer size; and one scaled to
 * printer pixels for drawing with tr_render_strip(). Pictures are left out
 * of the scaled copy and drawn afterwards, scaled while drawing (nearest
 * pixel), so a picture needs no copy in printer size. Tiled background
 * pictures are not printed (as in PostScript/PDF).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "htmlttf_print.h"
#include "html_print.h"

struct TPrint {
    struct TRender  r;              /* fonts in printer size */
    struct HEnv     env;            /* layout in 1/96 inch */
    void           *pool;
    struct HLayout *lay;            /* 1/96 inch */
    struct HLayout  slay;           /* printer pixels, + 1 item for the footer */
    struct TImage **imgs;           /* the picture of each item of slay, or NULL */
    long           *tops, npages, height;      /* page tops, page height (1/96 inch) */
    long            dpi;
    long            pw, ph;         /* sheet in printer pixels */
    long            ml, mt, mb;     /* margins in printer pixels */
    long            cw;             /* content width in printer pixels */
    const char     *footer;
    char            ftext[64];
};

/* 1/96 inch -> printer pixels */
static long S(struct TPrint *p, long v)
{
    return (v * p->dpi + (v < 0 ? -48 : 48)) / 96;
}

static long text_width_cb(void *user, int font, int style, const char *s, long len)
{
    struct TPrint *p = user;
    long w = tr_text_width(&p->r, font, style, s, len);
    return (w * 96 + p->dpi / 2) / p->dpi;
}

struct TPrint *tp_begin(struct HDoc *doc, const struct TRender *screen, const struct TPrintOpts *o,
                        long *pages, long *w, long *h)
{
    struct TPrint *p;
    struct HEnv penv;
    long i, width, n;

    *pages = *w = *h = 0;
    if (!(p = tr_alloc(sizeof(*p)))) return 0;
    if (!(p->pool = hsys_pool_create())) { tr_free(p); return 0; }
    p->dpi = o->dpi > 0 ? o->dpi : 150;
    p->footer = o->footer;

    /* the screen's fonts and colours, own glyph caches in printer size */
    p->r = *screen;
    for (i = 0; i < TR_NVAR; i++) {
        long l;
        for (l = 0; l < 7; l++) p->r.inst[i][l] = 0;
    }
    p->r.basepx = (o->font_size > 0 ? o->font_size : 100) * p->dpi / 720;
    p->r.doc = doc;
    p->r.activelink = -1;
    p->r.pressed = 0;
    p->r.sel = 0;
    p->r.bgimg = 0;
    p->r.env = 0;

    h_memset(&penv, 0, sizeof(penv));
    tr_metrics(&p->r, &penv);
    for (i = 0; i < HF_NUM; i++) {
        p->env.font_height[i] = (short)((penv.font_height[i] * 96 + p->dpi / 2) / p->dpi);
        p->env.font_baseline[i] = (short)((penv.font_baseline[i] * 96 + p->dpi / 2) / p->dpi);
    }
    p->env.user = p;
    p->env.text_width = text_width_cb;
    p->env.fit_images = o->fit_images;
    p->env.generation = 1;

    width = (o->paper_w - o->margin[0] - o->margin[2]) * 4 / 3;
    p->height = (o->paper_h - o->margin[1] - o->margin[3]) * 4 / 3;
    if (width < 50 || p->height < 50 || !(p->lay = html_layout(doc, &p->env, width)) ||
        !(p->tops = html_print_paginate(p->lay, p->pool, p->height, &p->npages)))
        goto fail;

    /* the copy in printer pixels */
    n = p->lay->nitems;
    p->slay = *p->lay;
    if (!(p->slay.items = hsys_alloc(p->pool, (n + 1) * sizeof(struct HItem))) ||
        !(p->imgs = hsys_alloc(p->pool, (n + 1) * sizeof(struct TImage *))))
        goto fail;
    for (i = 0; i < n; i++) {
        struct HItem *it = &p->slay.items[i];
        *it = p->lay->items[i];
        it->x = S(p, it->x);
        it->y = S(p, it->y);
        it->w = S(p, p->lay->items[i].x + p->lay->items[i].w) - it->x;
        it->h = S(p, p->lay->items[i].y + p->lay->items[i].h) - it->y;
        it->base = (short)S(p, it->base);
        if (it->type == IT_IMAGE) {
            p->imgs[i] = it->img;           /* drawn by us */
            it->type = IT_RECT;
            it->color = COL_NONE;
            it->img = 0;
        } else if (it->type == IT_RECT) {
            it->img = 0;                    /* no tiled pictures */
            if (!o->backgrounds) it->color = COL_NONE;
        }
    }
    p->slay.nitems = n;
    p->slay.width = S(p, p->lay->width);
    p->slay.height = S(p, p->lay->height);
    if (!o->backgrounds || !COL_IS_RGB(p->slay.bgcolor)) p->slay.bgcolor = 0xFFFFFF;

    p->pw = o->paper_w * p->dpi / 72;
    p->ph = o->paper_h * p->dpi / 72;
    p->ml = o->margin[0] * p->dpi / 72;
    p->mt = o->margin[1] * p->dpi / 72;
    p->mb = o->margin[3] * p->dpi / 72;
    p->cw = p->pw - p->ml - o->margin[2] * p->dpi / 72;
    *pages = p->npages;
    *w = p->pw;
    *h = p->ph;
    return p;

fail:
    tp_end(p);
    return 0;
}

void tp_end(struct TPrint *p)
{
    if (!p) return;
    tr_free_insts(&p->r);
    if (p->lay) html_free_layout(p->lay);
    hsys_pool_delete(p->pool);
    tr_free(p);
}

static tr_u32 blend(tr_u32 d, tr_u32 s, unsigned long a)
{
    unsigned long r = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
    unsigned long g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
    unsigned long b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return (tr_u32)(0xFF000000UL | r << 16 | g << 8 | b);
}

/* a picture into its box of item i, scaled while drawing */
static void draw_image(struct TPrint *p, struct TStrip *s, long i)
{
    struct HItem *it = &p->slay.items[i];
    struct TImage *im = p->imgs[i];
    long x0, y0, x1, y1, x, y;

    if (!im || !im->pix || im->w <= 0 || im->h <= 0 || it->w <= 0 || it->h <= 0) return;
    x0 = it->x > s->dx ? it->x : s->dx;
    y0 = it->y > s->dy ? it->y : s->dy;
    x1 = it->x + it->w < s->dx + s->w ? it->x + it->w : s->dx + s->w;
    y1 = it->y + it->h < s->dy + s->h ? it->y + it->h : s->dy + s->h;
    for (y = y0; y < y1; y++) {
        tr_u32 *row = im->pix + (y - it->y) * im->h / it->h * im->w;
        tr_u32 *dst = s->buf + (y - s->dy) * s->w + (x0 - s->dx);
        for (x = x0; x < x1; x++, dst++) {
            tr_u32 px = row[(x - it->x) * im->w / it->w];
            unsigned long a = px >> 24;
            if (im->opaque || a == 255) *dst = px | 0xFF000000UL;
            else if (a) *dst = blend(*dst, px, a);
        }
    }
}

/* renders the rectangle (x, y, w, h) of 'lay' into buf (row pitch 'mod')
 * through a strip; dx/dy: document position of the rectangle         */
static int render_part(struct TPrint *p, struct HLayout *lay, long dx, long dy, long w, long h,
                       tr_u32 *buf, long mod, int images)
{
    struct TStrip s;
    long y, x, i;

    if (w <= 0 || h <= 0) return 1;
    if (!(s.buf = tr_alloc(w * h * 4))) return 0;
    s.w = w;
    s.h = h;
    s.dx = dx;
    s.dy = dy;
    tr_render_strip(&p->r, lay, 0, &s);
    if (images)
        for (i = 0; i < lay->nitems; i++)
            if (p->imgs[i]) draw_image(p, &s, i);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            buf[y * mod + x] = s.buf[y * w + x] & 0xFFFFFFUL;
    tr_free(s.buf);
    return 1;
}

int tp_render(struct TPrint *p, long page, long x, long y, long w, long h, tr_u32 *buf)
{
    long i, top, bottom, ch, cx0, cy0, cx1, cy1;

    for (i = 0; i < w * h; i++) buf[i] = 0xFFFFFF;
    if (page < 1 || page > p->npages) return 1;
    top = p->tops[page - 1];
    bottom = page < p->npages ? p->tops[page] : top + p->height;
    ch = S(p, bottom) - S(p, top);

    /* the content area of the page, clipped to the rectangle */
    cx0 = x > p->ml ? x : p->ml;
    cy0 = y > p->mt ? y : p->mt;
    cx1 = x + w < p->ml + p->cw ? x + w : p->ml + p->cw;
    cy1 = y + h < p->mt + ch ? y + h : p->mt + ch;
    if (cx0 < cx1 && cy0 < cy1 &&
        !render_part(p, &p->slay, cx0 - p->ml, S(p, top) + cy0 - p->mt, cx1 - cx0, cy1 - cy0,
                     buf + (cy0 - y) * w + (cx0 - x), w, 1))
        return 0;

    /* the page number, centred in the bottom margin */
    if (p->footer && *p->footer) {
        struct HItem *fi = &p->slay.items[p->slay.nitems];
        struct HLayout fl;
        struct TInst *in;
        long fy0, fy1, len = html_print_footer(p->ftext, sizeof(p->ftext), p->footer, page, p->npages);

        h_memset(fi, 0, sizeof(*fi));
        fi->type = IT_TEXT;
        fi->font = HF_INDEX(0, 2);
        fi->link = -1;
        fi->color = 0x000000;
        fi->s = p->ftext;
        fi->len = len;
        fi->w = tr_text_width(&p->r, fi->font, 0, p->ftext, len);
        fi->x = (p->pw - fi->w) / 2;
        in = tr_get_inst(&p->r, fi->font, 0);
        fi->h = in ? in->px * 3 / 2 : 12;
        fi->base = (short)(in ? in->px : 10);
        fi->y = p->ph - p->mb / 2 - fi->base;
        fl = p->slay;
        fl.items = fi;
        fl.nitems = 1;
        fl.bgcolor = 0xFFFFFF;
        fy0 = y > p->ph - p->mb ? y : p->ph - p->mb;
        fy1 = y + h < p->ph ? y + h : p->ph;
        if (fy0 < fy1 && !render_part(p, &fl, x, fy0, w, fy1 - fy0, buf + (fy0 - y) * w, w, 0))
            return 0;
    }
    return 1;
}
