/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* host test of the bitmap printing of htmlttf.gadget (htmlttf_print.c):
 * renders the pages of a document for a printer resolution in strips, as
 * printer.device would ask for them, and writes <out>-<page>.ppm.
 *
 *   ttfprint page.html dpi out fontdir fontset
 * Pictures: <src>.rgba ("w h\n" + RGBA), as for ttfpreview.            */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/htmlttf_print.h"

struct Blk { struct Blk *next; };
void *hsys_pool_create(void) { return calloc(1, sizeof(struct Blk)); }
void hsys_pool_delete(void *pool)
{
    struct Blk *b = ((struct Blk *)pool)->next, *n;
    for (; b; b = n) { n = b->next; free(b); }
    free(pool);
}
void *hsys_alloc(void *pool, long size)
{
    struct Blk *b = calloc(1, sizeof(struct Blk) + size + 16);
    if (!b) return 0;
    b->next = ((struct Blk *)pool)->next;
    ((struct Blk *)pool)->next = b;
    return (char *)b + 16;
}
void hsys_free(void *pool, void *mem, long size) { (void)pool; (void)mem; (void)size; }
void *tr_alloc(long size) { return calloc(1, size > 0 ? size : 4); }
void tr_free(void *mem) { free(mem); }

static struct TRender R;

static struct TImage *load_rgba(const char *basedir, const char *src)
{
    char p[600]; FILE *g; int w, h; long k;
    struct TImage *im;
    unsigned char *raw;
    snprintf(p, sizeof(p), "%s%s.rgba", basedir, src);
    if (!(g = fopen(p, "rb")) || fscanf(g, "%d %d\n", &w, &h) != 2) { if (g) fclose(g); return 0; }
    im = tr_alloc(sizeof(*im));
    raw = malloc(w * h * 4);
    if (fread(raw, 1, w * h * 4, g)) {}
    fclose(g);
    im->pix = tr_alloc(w * h * 4);
    for (k = 0; k < w * h; k++)
        im->pix[k] = ((tr_u32)raw[k*4+3] << 24) | (raw[k*4] << 16) | (raw[k*4+1] << 8) | raw[k*4+2];
    free(raw);
    im->w = w; im->h = h;
    return im;
}
static long tw(void *u, int font, int style, const char *s, long len) { (void)u; return tr_text_width(&R, font, style, s, len); }

/* like the gadget: read the whole file, open it as memory face */
static FT_Face load_face(FT_Library ft, const char *dir, const char *file)
{
    char path[512]; FT_Face f; FILE *fp; long n; unsigned char *mem;
    if (!file) return 0;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (!(fp = fopen(path, "rb"))) return 0;
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    mem = malloc(n);
    if (!mem || fread(mem, 1, n, fp) != (size_t)n) { fclose(fp); free(mem); return 0; }
    fclose(fp);
    return FT_New_Memory_Face(ft, mem, n, 0, &f) ? 0 : f;
}

int main(int argc, char **argv)
{
    static char buf[1 << 20];
    const char *html = argc > 1 ? argv[1] : "demo/example.html";
    long dpi = argc > 2 ? atol(argv[2]) : 100, len, pages, pw, ph, page;
    const char *out = argc > 3 ? argv[3] : "print";
    const char *dir = argc > 4 ? argv[4] : "demo/fonts";
    const char *set = argc > 5 ? argv[5] : "Vera";
    static const char *vera[8] = { "Vera.ttf", "VeraIt.ttf", "VeraBd.ttf", "VeraBI.ttf", "VeraMono.ttf", "VeraMoIt.ttf", "VeraMoBd.ttf", "VeraMoBI.ttf" };
    static const char *noto[8] = { "NotoSans-Regular.ttf", "NotoSans-Italic.ttf", "NotoSans-Bold.ttf", "NotoSans-BoldItalic.ttf", "NotoSansMono-Regular.ttf", 0, "NotoSansMono-Bold.ttf", 0 };
    const char **files = strcmp(set, "Noto") ? vera : noto;
    char basedir[512];
    FT_Library ft;
    struct HDoc *doc;
    struct HNode *n;
    struct TPrintOpts o;
    struct TPrint *tp;
    FILE *f = fopen(html, "rb");
    int i;

    if (!f) return 1;
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    strcpy(basedir, html);
    if (strrchr(basedir, '/')) strrchr(basedir, '/')[1] = 0; else basedir[0] = 0;

    FT_Init_FreeType(&ft);
    for (i = 0; i < 8; i++) R.faces[i] = load_face(ft, dir, files[i]);
    R.basepx = 12;
    R.link = 0x0000EE; R.vlink = 0x551A8B; R.alink = 0xFF0000;
    for (i = 0; i < 16; i++) R.penrgb[i] = 0xAAAAAA;
    R.penrgb[2] = 0; R.penrgb[7] = 0xFFFFFF;

    doc = html_parse(buf, len);
    for (n = doc->root; n; ) {           /* pictures */
        const char *src;
        if (n->tag == T_IMG && (src = html_attr(n, "src"))) {
            char p[600]; FILE *g; int w, h;
            snprintf(p, sizeof(p), "%s%s.rgba", basedir, src);
            if ((g = fopen(p, "rb")) && fscanf(g, "%d %d\n", &w, &h) == 2) {
                struct TImage *im = tr_alloc(sizeof(*im));
                unsigned char *raw = malloc(w * h * 4);
                long k;
                if (fread(raw, 1, w * h * 4, g)) {}
                im->pix = tr_alloc(w * h * 4);
                for (k = 0; k < w * h; k++)
                    im->pix[k] = ((tr_u32)raw[k*4+3] << 24) | (raw[k*4] << 16) | (raw[k*4+1] << 8) | raw[k*4+2];
                free(raw);
                im->w = w; im->h = h;
                n->img = im; n->iw = im->w; n->ih = im->h;
            }
            if (g) fclose(g);
        }
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }

    memset(&o, 0, sizeof(o));
    o.dpi = dpi;
    o.paper_w = 595; o.paper_h = 842;
    o.margin[0] = o.margin[1] = o.margin[2] = o.margin[3] = 57;
    o.font_size = 100;
    o.backgrounds = 1;
    o.footer = "%p / %n";
    if (!(tp = tp_begin(doc, &R, &o, &pages, &pw, &ph))) { puts("tp_begin failed"); return 1; }
    printf("%ld pages of %ld x %ld pixels\n", pages, pw, ph);
    for (page = 1; page <= pages; page++) {
        char name[600];
        tr_u32 *strip = malloc(pw * 64 * 4);
        long y;
        FILE *o2;
        snprintf(name, sizeof(name), "%s-%ld.ppm", out, page);
        o2 = fopen(name, "wb");
        fprintf(o2, "P6\n%ld %ld\n255\n", pw, ph);
        /* in strips of 64 rows, the last one shorter */
        for (y = 0; y < ph; y += 64) {
            long h = ph - y < 64 ? ph - y : 64, k;
            if (!tp_render(tp, page, 0, y, pw, h, strip)) { puts("tp_render failed"); return 1; }
            for (k = 0; k < pw * h; k++) {
                tr_u32 p = strip[k];
                fputc((p >> 16) & 0xFF, o2); fputc((p >> 8) & 0xFF, o2); fputc(p & 0xFF, o2);
            }
        }
        fclose(o2);
        free(strip);
    }
    tp_end(tp);
    for (n = doc->root; n; ) {
        if (n->img) { free(((struct TImage *)n->img)->pix); free(n->img); }
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }
    html_free_doc(doc);
    tr_free_insts(&R);
    for (i = 0; i < 8; i++) if (R.faces[i]) FT_Done_Face(R.faces[i]);
    FT_Done_FreeType(ft);
    return 0;
}
