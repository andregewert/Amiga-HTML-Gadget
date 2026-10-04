/*
 * htmlttf_print.h - bitmap printing with the FreeType renderer
 *
 * Renders the pages of a document for a printer resolution, as pixels of
 * the whole sheet (margins white, page number in the bottom margin), in
 * any pieces (printer.device asks for strips through DRPA_SourceHook).
 * The layout runs in 1/96 inch like the PostScript/PDF output, with the
 * same page breaks; the fonts of the screen renderer are used in the
 * size for the printer. Platform independent (host test: test/ttfprint).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#ifndef HTMLTTF_PRINT_H
#define HTMLTTF_PRINT_H

#include "htmlttf_render.h"

struct TPrintOpts {
    long        dpi;                /* printer pixels per inch */
    long        paper_w, paper_h;   /* points */
    long        margin[4];          /* left, top, right, bottom in points */
    long        font_size;          /* normal text in 1/10 points */
    int         backgrounds;        /* print background colours */
    int         fit_images;         /* pictures not wider than the text (HEnv.fit_images) */
    const char *footer;             /* "%p / %n" or NULL */
};

struct TPrint;

/* Lays the document out for the printer. 'screen' gives the fonts (faces)
 * and colours. Returns NULL without memory; *pages gets the number of
 * pages, *w and *h the size of a page in printer pixels.               */
struct TPrint *tp_begin(struct HDoc *doc, const struct TRender *screen, const struct TPrintOpts *o,
                        long *pages, long *w, long *h);

/* Fills buf (w * h pixels 0x00RRGGBB, row by row) with the rectangle
 * x/y/w/h of page 'page' (from 1). FALSE without memory.              */
int  tp_render(struct TPrint *p, long page, long x, long y, long w, long h, tr_u32 *buf);

void tp_end(struct TPrint *p);

#endif
