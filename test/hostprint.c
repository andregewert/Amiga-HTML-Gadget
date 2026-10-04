/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* host test of the print engine: writes an HTML file as PostScript or
 * PDF (format from the file name)
 *
 *   hostprint <page.html> <out.ps|out.pdf> [serif] [letter] [nobg] [ps1]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/html_print.h"

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

/* every <img src> gets a test picture: a checkered ball, transparent
 * around it (as the Boing ball), in the size of width/height or 96x64 */
static void fake_images(struct HDoc *doc)
{
    struct HNode *n = doc->root;
    while (n) {
        if (n->tag == T_IMG && html_attr(n, "src") && !html_attr(n, "noload")) {
            const char *w = html_attr(n, "width"), *h = html_attr(n, "height");
            n->img = n;
            n->iw = w ? atol(w) : 96;
            n->ih = h ? atol(h) : 64;
        }
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }
}

static int image(void *user, void *img, struct HPrintImage *pi)
{
    struct HNode *n = img;
    unsigned long *p;
    long x, y, w = n->iw, h = n->ih;
    (void)user;
    if (w <= 0 || h <= 0 || !(p = malloc(w * h * sizeof(*p)))) return 0;
    if (html_attr(n, "noise")) {        /* noise="1": random pixels, alpha in steps */
        unsigned long r = 12345;
        for (y = 0; y < h * w; y++) {
            r = r * 1103515245UL + 12345;
            p[y] = ((r >> 8) & 0xFFFFFFUL) | ((unsigned long)((y / w) * 255 / h) << 24);
        }
    } else
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            long dx = 2 * x - w, dy = 2 * y - h, r = w < h ? w : h;
            int in = dx * dx + dy * dy <= r * r;
            int check = ((x * 8 / w) + (y * 6 / h)) & 1;
            p[y * w + x] = in ? (check ? 0xFFE00000UL : 0xFFFFFFFFUL) : 0x00000000UL;
        }
    pi->w = w;
    pi->h = h;
    pi->argb = p;
    pi->priv = p;
    if (html_attr(n, "jpeg")) {         /* jpeg="file": its bytes, passed on as they are */
        FILE *f = fopen(html_attr(n, "jpeg"), "rb");
        long len;
        unsigned char *d;
        if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) &&
            (d = malloc(len)) && fread(d, 1, len, f) == (size_t)len) {
            pi->jpeg = d;
            pi->jpeglen = len;
        }
        if (f) fclose(f);
    }
    return 1;
}

static void image_free(void *user, struct HPrintImage *pi)
{
    (void)user;
    free(pi->priv);
    free((void *)pi->jpeg);
}

static void out(void *user, const char *data, long len)
{
    fwrite(data, 1, len, (FILE *)user);
}

int main(int argc, char **argv)
{
    static char buf[1 << 20];
    struct HPrintOpts o;
    struct HDoc *doc;
    FILE *f, *w;
    long len, n, pages;
    int i;

    if (argc < 3 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: hostprint page.html out.ps|out.pdf [serif] [letter] [nobg] [ps1]\n");
        return 1;
    }
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!(doc = html_parse(buf, len))) { fprintf(stderr, "parse failed\n"); return 1; }
    if (!(w = fopen(argv[2], "wb"))) return 1;
    fake_images(doc);

    memset(&o, 0, sizeof(o));
    n = strlen(argv[2]);
    o.format = n > 4 && !strcmp(argv[2] + n - 4, ".pdf") ? HP_PDF : HP_PS;
    o.paper_w = 595;
    o.paper_h = 842;
    o.margin[0] = o.margin[1] = o.margin[2] = o.margin[3] = 57;     /* 20 mm */
    o.font_size = 100;
    o.backgrounds = 1;
    o.footer = "%p / %n";
    o.title = doc->title ? doc->title : argv[1];
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "serif")) o.serif = 1;
        else if (!strcmp(argv[i], "letter")) { o.paper_w = 612; o.paper_h = 792; }
        else if (!strcmp(argv[i], "nobg")) o.backgrounds = 0;
        else if (!strcmp(argv[i], "ps1")) o.ps_level = 1;
    }
    o.write = out;
    o.image = image;
    o.image_free = image_free;
    o.user = w;
    n = html_print(doc, &o, &pages);
    fclose(w);
    html_free_doc(doc);
    if (n < 0) { fprintf(stderr, "html_print failed: %ld\n", n); return 1; }
    printf("%s: %ld of %ld pages\n", argv[2], n, pages);
    return 0;
}
