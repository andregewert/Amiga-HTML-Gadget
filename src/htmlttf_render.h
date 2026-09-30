/*
 * htmlttf_render.h - platform independent renderer of htmlttf.gadget
 *
 * FreeType glyph cache and software compositing of the layout items into
 * a 32 bit ARGB buffer. Used by the Amiga class and by the host preview
 * (test/ttfpreview.c), so the rendering can be checked without emulator.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#ifndef HTMLTTF_RENDER_H
#define HTMLTTF_RENDER_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include "html_core.h"

/* 32 bit ARGB pixel (unsigned long is 64 bit on a host) */
typedef unsigned int tr_u32;

#define TR_NVAR 8               /* fixed * 4 + bold * 2 + italic */

/* provided by the platform (AllocVec/FreeVec on the Amiga) */
void *tr_alloc(long size);      /* zeroed */
void  tr_free(void *mem);

struct TGlyph {
    unsigned char *bits;        /* 8 bit coverage, pitch = w */
    short          left, top;   /* offset from pen position / baseline */
    unsigned short w, h;
    short          adv;
    unsigned char  state;       /* 0 = not loaded, 1 = ok, 2 = failed */
};

/* one font variant in one pixel size with a glyph cache for Latin-1 */
struct TInst {
    FT_Face        face;
    unsigned short px;
    unsigned char  synth;       /* 1 = embolden, 2 = slant (variant missing) */
    struct TGlyph  g[256];
};

/* a picture as ARGB, already scaled to its display size (HNode.img) */
struct TImage {
    struct TImage *next;
    char           path[256];
    long           reqw, reqh;
    tr_u32        *pix;
    long           w, h;
    int            opaque;
};

struct TRender {
    FT_Face        faces[TR_NVAR];      /* NULL = variant not available */
    struct TInst  *inst[TR_NVAR][7];
    long           basepx;              /* em size of body text */
    /* colours, resolved by the platform before rendering */
    unsigned long  penrgb[16];          /* DrawInfo pens as 0xRRGGBB */
    unsigned long  link, vlink, alink;
    struct HDoc   *doc;
    long           activelink;
    int            pressed;
    const struct HSel *sel;             /* text selection or NULL */
    struct TImage *bgimg;               /* <body background> or NULL */
    struct HEnv   *env;                 /* for measuring the selection */
};

struct TStrip {
    tr_u32        *buf;                 /* ARGB, w * h */
    long           w, h;
    long           dx, dy;              /* document coords of buf[0] */
};

struct TInst  *tr_get_inst(struct TRender *r, int font, int style);
struct TGlyph *tr_get_glyph(struct TInst *in, unsigned char c);
void           tr_free_insts(struct TRender *r);
long           tr_text_width(struct TRender *r, int font, int style, const char *s, long len);
void           tr_metrics(struct TRender *r, struct HEnv *env);
long           tr_level_px(long base, int level);
void           tr_render_strip(struct TRender *r, struct HLayout *lay, int syscolors, struct TStrip *s);
tr_u32        *tr_scale_argb(tr_u32 *src, long sw, long sh, long dw, long dh);

#endif
