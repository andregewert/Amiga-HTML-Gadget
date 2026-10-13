/*
 * html_argb.c - ARGB pictures, shared by both gadgets
 *
 * Resampling of 32 bit ARGB pixels (0xAARRGGBB in an unsigned int): the
 * scaled pictures of htmlttf.gadget, and of html.gadget on palette
 * screens, where pictures are scaled in full colour before they get the
 * screen's pens. Integer arithmetic only.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_core.h"

/* resamples ARGB sw x sh into dst (dw x dh): area average when shrinking,
 * bilinear when enlarging. Colours are weighted with alpha so transparent
 * pixels do not bleed.                                                   */
void html_scale_argb(const unsigned int *src, long sw, long sh, unsigned int *dst, long dw, long dh)
{
    unsigned int *o = dst;
    long x, y;

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
}
