/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* host preview of htmlttf.gadget: parse + layout + FreeType renderer,
 * writes the whole page as PPM. Pictures: <src>.rgba ("w h\n" + RGBA). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/htmlttf_render.h"

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
    long width = argc > 2 ? atol(argv[2]) : 560, len, y;
    const char *out = argc > 3 ? argv[3] : "preview.ppm";
    const char *dir = argc > 4 ? argv[4] : "demo/fonts";
    const char *set = argc > 5 ? argv[5] : "Vera";
    static const char *vera[8] = { "Vera.ttf", "VeraIt.ttf", "VeraBd.ttf", "VeraBI.ttf", "VeraMono.ttf", "VeraMoIt.ttf", "VeraMoBd.ttf", "VeraMoBI.ttf" };
    static const char *noto[8] = { "NotoSans-Regular.ttf", "NotoSans-Italic.ttf", "NotoSans-Bold.ttf", "NotoSans-BoldItalic.ttf", "NotoSansMono-Regular.ttf", 0, "NotoSansMono-Bold.ttf", 0 };
    const char **files = strcmp(set, "Noto") ? vera : noto;
    char basedir[512];
    FT_Library ft;
    struct HDoc *doc;
    struct HLayout *lay;
    struct HEnv env;
    struct HNode *n;
    FILE *f = fopen(html, "rb");
    int i;

    if (!f) return 1;
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    strcpy(basedir, html);
    if (strrchr(basedir, '/')) strrchr(basedir, '/')[1] = 0; else basedir[0] = 0;

    FT_Init_FreeType(&ft);
    for (i = 0; i < 8; i++) R.faces[i] = load_face(ft, dir, files[i]);
    R.basepx = argc > 6 ? atol(argv[6]) : 12;
    R.link = 0x0000EE; R.vlink = 0x551A8B; R.alink = 0xFF0000;
    for (i = 0; i < 16; i++) R.penrgb[i] = 0xAAAAAA;
    R.penrgb[2] = 0; R.penrgb[7] = 0xFFFFFF;

    doc = html_parse(buf, len);
    R.doc = doc; R.activelink = -1;
    if (doc->background) R.bgimg = load_rgba(basedir, doc->background);
    for (n = doc->root; n; ) {           /* pictures */
        const char *src;
        if ((n->tag == T_TD || n->tag == T_TABLE) && (src = html_attr(n, "background")))
            n->img = load_rgba(basedir, src);
        if (n->tag == T_IMG && (src = html_attr(n, "src"))) {
            char p[600]; FILE *g; int w, h;
            snprintf(p, sizeof(p), "%s%s.rgba", basedir, src);
            if ((g = fopen(p, "rb")) && fscanf(g, "%d %d\n", &w, &h) == 2) {
                struct TImage *im = tr_alloc(sizeof(*im));
                unsigned char *raw = malloc(w * h * 4);
                const char *a;
                long rw = 0, rh = 0, k;
                if (fread(raw, 1, w * h * 4, g)) {}
                im->pix = tr_alloc(w * h * 4);
                for (k = 0; k < w * h; k++)
                    im->pix[k] = ((tr_u32)raw[k*4+3] << 24) | (raw[k*4] << 16) | (raw[k*4+1] << 8) | raw[k*4+2];
                free(raw);
                im->w = w; im->h = h;
                if ((a = html_attr(n, "width"))) rw = atol(a);
                if ((a = html_attr(n, "height"))) rh = atol(a);
                if (rw > 0 || rh > 0) {
                    long tw2 = rw > 0 ? rw : w * rh / h, th2 = rh > 0 ? rh : h * rw / w;
                    tr_u32 *np = tr_scale_argb(im->pix, w, h, tw2, th2);
                    free(im->pix); im->pix = np; im->w = tw2; im->h = th2;
                }
                n->img = im; n->iw = im->w; n->ih = im->h;
            }
            if (g) fclose(g);
        }
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }

    memset(&env, 0, sizeof(env));
    env.text_width = tw;
    env.margin = 8;
    tr_metrics(&R, &env);
    lay = html_layout(doc, &env, width);
    {   /* optional test selection: argv[7] = "first,last" item */
        static struct HSel sel;
        if (argc > 7) {
            sel.active = 1;
            sel.i0 = atol(argv[7]); sel.c0 = 2;
            sel.i1 = atol(strchr(argv[7], ',') + 1); sel.c1 = 3;
            R.sel = &sel;
            R.env = &env;
            R.penrgb[5] = 0x3B6BC0; R.penrgb[6] = 0xFFFFFF;
        }
    }
    {
        struct TStrip s;
        FILE *o = fopen(out, "wb");
        s.w = width; s.h = 1; s.dx = 0;
        s.buf = calloc(width, 4);
        fprintf(o, "P6\n%ld %ld\n255\n", width, lay->height);
        for (y = 0; y < lay->height; y++) {
            long x;
            s.dy = y;
            tr_render_strip(&R, lay, 0, &s);
            for (x = 0; x < width; x++) {
                tr_u32 p = s.buf[x];
                fputc((p >> 16) & 0xFF, o); fputc((p >> 8) & 0xFF, o); fputc(p & 0xFF, o);
            }
        }
        fclose(o);
        free(s.buf);
    }
    printf("%ld x %ld, %ld items, font height %d/%d\n", width, lay->height, lay->nitems, env.font_height[2], env.font_baseline[2]);
    for (n = doc->root; n; ) {
        if (n->img && n->tag != T_TD && n->tag != T_TABLE) { free(((struct TImage *)n->img)->pix); free(n->img); }
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }
    html_free_layout(lay);
    html_free_doc(doc);
    tr_free_insts(&R);
    for (i = 0; i < 8; i++) if (R.faces[i]) FT_Done_Face(R.faces[i]);
    FT_Done_FreeType(ft);
    return 0;
}
