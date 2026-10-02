/*
 * html_check.c - read-only checkboxes and radio buttons (IT_CHECK)
 *
 * Unchecked: white square with a grey border and rounded corners;
 * checked: filled with the accent colour, a white tick on top. Radio
 * buttons are circles with a dot. Everything scales with the size of the
 * item (the base line height of the font).
 *
 * The shapes are sampled 4 x 4 times per pixel; the renderers get runs of
 * pixels with their coverage and blend them (htmlttf.gadget) or draw the
 * ones covered at least half (html.gadget). Integer arithmetic only, in
 * 1/8 pixel units, no 64 bit products (the 68060 emulates them).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_core.h"

#define U        8                  /* units per pixel */
#define MAXSIZE  128                /* keeps all products in 32 bit */

#define C_BORDER 0x767676UL         /* 4.5:1 on white */
#define C_FIELD  0xFFFFFFUL
#define C_ACCENT 0x0969DAUL
#define C_MARK   0xFFFFFFUL

enum { SH_RRECT, SH_CIRCLE, SH_TICK };

struct Shape {
    int           kind;
    long          inset;            /* units */
    long          r;                /* corner radius / circle radius, units */
    unsigned long rgb;
};

struct Tick {
    long ax, ay, bx, by, cx, cy;    /* the two strokes a-b and b-c */
    long len1, len2;                /* their lengths */
    long half;                      /* half the stroke width */
};

static long isqrt(long v)
{
    long r = 0, bit = 1L << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static long absl(long v) { return v < 0 ? -v : v; }

/* inside the rounded rectangle size x size, inset 'in', corner radius r */
static int in_rrect(long x, long y, long size, long in, long r)
{
    long x0 = in, y0 = in, x1 = size - in, y1 = size - in, cx, cy;
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
    cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x;
    cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static int in_circle(long x, long y, long size, long r)
{
    long c = size / 2;
    return (x - c) * (x - c) + (y - c) * (y - c) <= r * r;
}

/* distance of (x,y) to the segment a-b at most 'half' */
static int near_seg(long x, long y, long ax, long ay, long bx, long by, long len, long half)
{
    long vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
    long dot = wx * vx + wy * vy;
    if (dot <= 0) return wx * wx + wy * wy <= half * half;
    if (dot >= len * len) return (x - bx) * (x - bx) + (y - by) * (y - by) <= half * half;
    return absl(wx * vy - wy * vx) / len <= half;
}

static int inside(const struct Shape *sh, const struct Tick *t, long x, long y, long size)
{
    switch (sh->kind) {
    case SH_RRECT:  return in_rrect(x, y, size, sh->inset, sh->r);
    case SH_CIRCLE: return in_circle(x, y, size, sh->r);
    default:
        return near_seg(x, y, t->ax, t->ay, t->bx, t->by, t->len1, t->half) ||
               near_seg(x, y, t->bx, t->by, t->cx, t->cy, t->len2, t->half);
    }
}

void html_check_paint(const struct HItem *it, html_span_fn span, void *ctx)
{
    struct Shape sh[3];
    struct Tick t;
    long s = it->h < it->w ? it->h : it->w, size, bw, i, n = 0, px, py;
    int checked = (it->style & CK_CHECKED) != 0;

    if (s < 4) return;
    if (s > MAXSIZE) s = MAXSIZE;
    t.ax = t.ay = t.bx = t.by = t.cx = t.cy = t.half = 0;    /* used for ticks only */
    t.len1 = t.len2 = 1;
    size = s * U;
    bw = (s >= 24 ? 2 : 1) * U;                        /* border width */

    if (it->style & CK_RADIO) {
        sh[n].kind = SH_CIRCLE; sh[n].inset = 0; sh[n].r = size / 2;
        sh[n++].rgb = checked ? C_ACCENT : C_BORDER;
        sh[n].kind = SH_CIRCLE; sh[n].inset = bw; sh[n].r = size / 2 - bw;
        sh[n++].rgb = C_FIELD;
        if (checked) {
            sh[n].kind = SH_CIRCLE; sh[n].inset = 0; sh[n].r = size * 27 / 100;
            sh[n++].rgb = C_ACCENT;
        }
    } else {
        long r = (s + 2) / 5 * U;                      /* corner radius */
        sh[n].kind = SH_RRECT; sh[n].inset = 0; sh[n].r = r;
        sh[n++].rgb = checked ? C_ACCENT : C_BORDER;
        if (!checked) {
            sh[n].kind = SH_RRECT; sh[n].inset = bw; sh[n].r = r > bw ? r - bw : 0;
            sh[n++].rgb = C_FIELD;
        } else {
            t.ax = size * 25 / 100; t.ay = size * 52 / 100;
            t.bx = size * 43 / 100; t.by = size * 71 / 100;
            t.cx = size * 77 / 100; t.cy = size * 31 / 100;
            t.len1 = isqrt((t.bx - t.ax) * (t.bx - t.ax) + (t.by - t.ay) * (t.by - t.ay));
            t.len2 = isqrt((t.cx - t.bx) * (t.cx - t.bx) + (t.cy - t.by) * (t.cy - t.by));
            if (t.len1 < 1) t.len1 = 1;
            if (t.len2 < 1) t.len2 = 1;
            t.half = size / 15;                        /* stroke s/7.5 wide, */
            if (t.half < U) t.half = U;                /* at least 2 pixels */
            sh[n].kind = SH_TICK; sh[n].inset = 0; sh[n].r = 0;
            sh[n++].rgb = C_MARK;
        }
    }

    for (i = 0; i < n; i++)
        for (py = 0; py < s; py++) {
            long start = 0;
            unsigned last = 0;
            for (px = 0; px <= s; px++) {
                unsigned a = 0;
                if (px < s) {
                    int sx, sy, cnt = 0;
                    for (sy = 0; sy < 4; sy++)
                        for (sx = 0; sx < 4; sx++)
                            cnt += inside(&sh[i], &t, px * U + sx * 2 + 1, py * U + sy * 2 + 1, size);
                    a = cnt * 255 / 16;
                }
                if (a != last || px == s) {
                    if (last) span(ctx, it->x + start, it->x + px - 1, it->y + py, sh[i].rgb, last);
                    start = px;
                    last = a;
                }
            }
        }
}
