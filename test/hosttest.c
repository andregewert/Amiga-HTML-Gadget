/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* host test for the html.gadget core: parses a file, lays it out with a
 * fake fixed-metrics font and dumps the draw items */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/html_core.h"

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

static long tw(void *u, int font, int style, const char *s, long len)
{
    (void)u; (void)s;
    long cw = font >= 7 ? 8 : 6 + (font % 7) / 2;
    return len * cw + ((style & HS_BOLD) ? 1 : 0);
}

int main(int argc, char **argv)
{
    static char buf[1 << 20];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    long len, width = argc > 2 ? atol(argv[2]) : 400, i;
    struct HDoc *doc;
    struct HLayout *lay;
    struct HEnv env;
    int quiet = argc > 3;

    if (!f) return 1;
    len = fread(buf, 1, sizeof(buf) - 1, f);
    doc = html_parse(buf, len);
    if (!doc) { puts("parse failed"); return 1; }
    memset(&env, 0, sizeof(env));
    env.text_width = tw;
    for (i = 0; i < HF_NUM; i++) {
        static const short h[7] = { 6, 7, 8, 10, 12, 16, 24 };
        env.font_height[i] = i >= 7 ? 8 : h[i];
        env.font_baseline[i] = env.font_height[i] * 3 / 4;
    }
    {   /* pretend every <img src> could be loaded as 40x30 picture */
        struct HNode *n = doc->root;
        while (n) {
            if (n->tag == T_IMG && html_attr(n, "src") && !html_attr(n, "noload")) {
                n->img = n; n->iw = 40; n->ih = 30;
            }
            if (n->first) n = n->first;
            else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
        }
    }
    env.margin = 8;
    env.generation = 1;
    printf("title: %s  utf8=%d links=%ld fonts=%lx\n", doc->title ? doc->title : "(none)",
           doc->utf8, doc->nlinks, html_fonts_used(doc));
    lay = html_layout(doc, &env, width);
    if (!lay) { puts("layout failed"); return 1; }
    printf("size %ld x %ld, %ld items, %ld anchors\n", lay->width, lay->height, lay->nitems, lay->nanchors);
    if (!quiet)
    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        static const char *tn[] = { "TEXT", "RECT", "HR", "FRAME", "BULLET", "IMAGE" };
        printf("%-6s x=%4ld y=%4ld w=%4ld h=%3ld b=%2d f=%2d st=%02x l=%2d c=%08lx", tn[it->type],
               it->x, it->y, it->w, it->h, it->base, it->font, it->style, it->link, it->color);
        if (it->type == IT_TEXT) printf(" '%.*s'", (int)it->len, it->s);
        putchar('\n');
    }
    if (argc > 4) {               /* selection test */
        struct HSel sel;
        long it, ch, n;
        char *txt;
        html_sel_all(&sel, lay);
        n = html_sel_text(&sel, lay, 0);
        txt = malloc(n + 1);
        html_sel_text(&sel, lay, txt);
        printf("--- select all: %ld chars ---\n%s\n--- end ---\n", n, txt);
        free(txt);
        {
            static const long pts[][2] = { { 60, 67 }, { 5, 67 }, { 390, 67 }, { 200, 5000 }, { 30, 175 } };
            int k;
            for (k = 0; k < 5; k++)
                if (html_sel_pos(lay, &env, pts[k][0], pts[k][1], &it, &ch))
                    printf("pos(%ld,%ld) -> item %ld '%.*s' char %ld\n", pts[k][0], pts[k][1], it,
                           (int)lay->items[it].len, lay->items[it].s, ch);
        }
        sel.active = 1; sel.i0 = 8; sel.c0 = 3; sel.i1 = 11; sel.c1 = 2;
        n = html_sel_text(&sel, lay, 0);
        txt = malloc(n + 1);
        html_sel_text(&sel, lay, txt);
        printf("partial: '%s'\n", txt);
        free(txt);
    }
    for (i = 0; i < lay->nanchors; i++) printf("anchor %s @%ld\n", lay->anchors[i].name, lay->anchors[i].y);
    for (i = 0; i < doc->nlinks; i++) printf("link %ld: %s\n", i, doc->links[i]);
    html_free_layout(lay);
    html_free_doc(doc);
    return 0;
}
