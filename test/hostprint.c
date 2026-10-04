/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* host test of the print engine: writes an HTML file as PostScript or
 * PDF (format from the file name)
 *
 *   hostprint <page.html> <out.ps|out.pdf> [serif] [letter] [nobg]
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
        fprintf(stderr, "usage: hostprint page.html out.ps|out.pdf [serif] [letter] [nobg]\n");
        return 1;
    }
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!(doc = html_parse(buf, len))) { fprintf(stderr, "parse failed\n"); return 1; }
    if (!(w = fopen(argv[2], "wb"))) return 1;

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
    }
    o.write = out;
    o.user = w;
    n = html_print(doc, &o, &pages);
    fclose(w);
    html_free_doc(doc);
    if (n < 0) { fprintf(stderr, "html_print failed: %ld\n", n); return 1; }
    printf("%s: %ld of %ld pages\n", argv[2], n, pages);
    return 0;
}
