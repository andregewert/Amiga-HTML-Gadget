/*
 * html_select.c - text selection on the laid out document
 *
 * Positions are (item, character). Items are in reading order except for
 * list markers, which are flagged IF_NOSEL and never selected.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_core.h"

static int selectable(struct HItem *it)
{
    return it->type == IT_TEXT && !(it->flags & IF_NOSEL) && it->len > 0;
}

long html_char_x(struct HEnv *env, struct HItem *it, long ch)
{
    if (ch <= 0) return 0;
    if (ch >= it->len) return it->w;
    return env->text_width(env->user, it->font, it->style & 7, it->s, ch);
}

/* character in item nearest to document x */
static long char_at(struct HEnv *env, struct HItem *it, long x)
{
    long k, prev = 0;
    x -= it->x;
    if (x <= 0) return 0;
    for (k = 1; k <= it->len; k++) {
        long w = html_char_x(env, it, k);
        if (x < (prev + w) / 2) return k - 1;
        prev = w;
    }
    return it->len;
}

int html_sel_pos(struct HLayout *lay, struct HEnv *env, long x, long y, long *item, long *ch)
{
    long i, left = -1, right = -1, above = -1, first = -1;

    if (!lay) return 0;
    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        if (!selectable(it)) continue;
        if (first < 0) first = i;
        if (y >= it->y && y < it->y + it->h) {                 /* on this line */
            if (x >= it->x && x < it->x + it->w) {
                *item = i;
                *ch = char_at(env, it, x);
                return 1;
            }
            if (it->x + it->w <= x) {
                if (left < 0 || it->x > lay->items[left].x) left = i;
            } else if (right < 0 || it->x < lay->items[right].x) {
                right = i;
            }
        } else if (it->y + it->h <= y) {
            above = i;                                          /* last item above the point */
        }
    }
    if (left >= 0) { *item = left; *ch = lay->items[left].len; return 1; }
    if (right >= 0) { *item = right; *ch = 0; return 1; }
    if (above >= 0) { *item = above; *ch = lay->items[above].len; return 1; }
    if (first >= 0) { *item = first; *ch = 0; return 1; }
    return 0;
}

void html_sel_order(const struct HSel *s, long *si, long *sc, long *ei, long *ec)
{
    if (s->i0 < s->i1 || (s->i0 == s->i1 && s->c0 <= s->c1)) {
        *si = s->i0; *sc = s->c0; *ei = s->i1; *ec = s->c1;
    } else {
        *si = s->i1; *sc = s->c1; *ei = s->i0; *ec = s->c0;
    }
}

int html_sel_empty(const struct HSel *s)
{
    return !s->active || (s->i0 == s->i1 && s->c0 == s->c1);
}

int html_sel_part(const struct HSel *s, struct HLayout *lay, long i, long *c0, long *c1)
{
    long si, sc, ei, ec;
    struct HItem *it;

    if (html_sel_empty(s) || !lay || i < 0 || i >= lay->nitems) return 0;
    it = &lay->items[i];
    if (!selectable(it)) return 0;
    html_sel_order(s, &si, &sc, &ei, &ec);
    if (i < si || i > ei) return 0;
    *c0 = i == si ? sc : 0;
    *c1 = i == ei ? ec : it->len;
    if (*c0 > it->len) *c0 = it->len;
    if (*c1 > it->len) *c1 = it->len;
    return *c1 > *c0 || (i != ei && *c0 == it->len);
}

long html_sel_right(const struct HSel *s, struct HLayout *lay, struct HEnv *env, long i, long c1)
{
    struct HItem *it = &lay->items[i];
    long j, n0, n1;

    if (c1 < it->len) return it->x + html_char_x(env, it, c1);
    for (j = i + 1; j < lay->nitems; j++) {           /* next selectable item */
        struct HItem *nx = &lay->items[j];
        if (!selectable(nx)) continue;
        if (nx->y == it->y && nx->x > it->x + it->w && html_sel_part(s, lay, j, &n0, &n1) && n0 == 0)
            return nx->x;
        break;
    }
    return it->x + it->w;
}

void html_sel_all(struct HSel *s, struct HLayout *lay)
{
    long i, first = -1, last = -1;
    s->active = 0;
    if (!lay) return;
    for (i = 0; i < lay->nitems; i++)
        if (selectable(&lay->items[i])) { if (first < 0) first = i; last = i; }
    if (first < 0) return;
    s->i0 = first; s->c0 = 0;
    s->i1 = last;  s->c1 = lay->items[last].len;
    s->active = 1;
}

long html_sel_text(const struct HSel *s, struct HLayout *lay, char *buf)
{
    long i, n = 0, si, sc, ei, ec;
    struct HItem *prev = 0;

    if (buf) buf[0] = 0;
    if (html_sel_empty(s) || !lay) return 0;
    html_sel_order(s, &si, &sc, &ei, &ec);
    for (i = si; i <= ei && i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        long c0, c1, k;
        if (!selectable(it)) continue;
        c0 = i == si ? sc : 0;
        c1 = i == ei ? ec : it->len;
        if (prev) {
            if (it->y == prev->y) {
                long gap = it->x - (prev->x + prev->w);
                /* wide gaps (table columns, tabs in <pre>) become a TAB */
                if (gap > prev->h) { if (buf) buf[n] = '\t'; n++; }
                else if (gap > 0) { if (buf) buf[n] = ' '; n++; }
            } else if (it->flags & IF_SOFT) {       /* automatic wrap: same paragraph */
                if (buf) buf[n] = ' ';
                n++;
            } else {
                /* new line; an empty line between blocks */
                if (buf) buf[n] = '\n';
                n++;
                if (it->y - (prev->y + prev->h) >= prev->h / 2 && it->y > prev->y) {
                    if (buf) buf[n] = '\n';
                    n++;
                }
            }
        }
        for (k = c0; k < c1 && k < it->len; k++) {
            if (buf) buf[n] = (it->s[k] == '\xA0') ? ' ' : it->s[k];
            n++;
        }
        prev = it;
    }
    if (buf) buf[n] = 0;
    return n;
}
