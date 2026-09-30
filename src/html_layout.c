/*
 * html_layout.c - turns the node tree into positioned draw items
 *
 * Block layout with margin collapsing, inline line breaking at blanks,
 * lists, <pre>, <hr> and a simple automatic table layout (colspan,
 * cellpadding/cellspacing/border/width, align/valign, bgcolor).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_core.h"

#define MAXCOLS   64
#define HUGEWIDTH 1000000L

/* DrawInfo pen numbers (intuition/screens.h) */
#define PEN_TEXT        2
#define PEN_BACKGROUND  7

enum { FK_TEXT, FK_BOX };

struct Frag {
    long          x, w;
    const char   *s;
    long          len;
    unsigned long color;
    short         link;
    short         asc, desc, voff;
    unsigned char font, style, kind, flags;
    void         *img;
};

struct Style {
    unsigned char fixed, size, style, pre, nowrap, align, listdepth, pad;
    short         link, voff;
    unsigned long color;
};

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

struct Bullet {
    int           active;
    int           kind;          /* BUL_* or -1 for text */
    const char   *text;
    long          textlen, textw;
    long          x;             /* content left edge of the <li> */
    int           font;
    unsigned long color;
};

struct Box {
    long          x0, width, y;
    long          curx;
    long          group;         /* first frag of the current unbreakable word */
    long          space;         /* pending blank width */
    long          margin;        /* pending vertical margin */
    int           at_top;        /* swallow margins until first content */
    int           align;
    int           listkind;      /* 0 = ul, else ol type char */
    long          counter;
    struct Bullet bullet;
};

struct LCtx {
    struct HLayout *lay;
    struct HDoc    *doc;
    struct HEnv    *env;
    struct Frag    *frags;
    long            nfr, maxfr;
    int             measure;
    int             soft;           /* next flushed line continues a wrapped one */
    long            max_right;
    int             oom;
    long            em;
    unsigned long   textcol;
    short           spacew[HF_NUM];
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void *grow(struct LCtx *L, void *arr, long *max, long n, long elsize)
{
    long nm = *max ? *max * 2 : 64;
    void *na;

    (void)n;
    if (!(na = hsys_alloc(L->lay->pool, nm * elsize))) { L->oom = 1; return 0; }
    if (arr) {
        h_memcpy(na, arr, *max * elsize);
        hsys_free(L->lay->pool, arr, *max * elsize);
    }
    *max = nm;
    return na;
}

static struct HItem *emit(struct LCtx *L, int type)
{
    struct HLayout *lay = L->lay;
    struct HItem *it;

    if (L->measure || L->oom) return 0;
    if (lay->nitems == lay->maxitems) {
        struct HItem *na = grow(L, lay->items, &lay->maxitems, lay->nitems, sizeof(struct HItem));
        if (!na) return 0;
        lay->items = na;
    }
    it = &lay->items[lay->nitems++];
    h_memset(it, 0, sizeof(*it));
    it->type = (unsigned char)type;
    it->link = -1;
    return it;
}

static long fheight(struct LCtx *L, int f)   { return L->env->font_height[f]; }
static long fbase(struct LCtx *L, int f)     { return L->env->font_baseline[f]; }
static int  font_of(const struct Style *st)  { return HF_INDEX(st->fixed, st->size); }

static long twidth(struct LCtx *L, int font, int style, const char *s, long len)
{
    if (len <= 0) return 0;
    return L->env->text_width(L->env->user, font, style & 7, s, len);
}

static long spacew(struct LCtx *L, int font)
{
    if (!L->spacew[font]) L->spacew[font] = (short)twidth(L, font, 0, " ", 1);
    if (!L->spacew[font]) L->spacew[font] = 1;
    return L->spacew[font];
}

static void add_anchor(struct LCtx *L, const char *name, long y)
{
    struct HLayout *lay = L->lay;

    if (L->measure || L->oom || !name || !*name) return;
    if (lay->nanchors == lay->maxanchors) {
        struct HAnchor *na = grow(L, lay->anchors, &lay->maxanchors, lay->nanchors, sizeof(struct HAnchor));
        if (!na) return;
        lay->anchors = na;
    }
    lay->anchors[lay->nanchors].name = (char *)name;
    lay->anchors[lay->nanchors].y = y;
    lay->nanchors++;
}

/* parses "123" or "50%"; returns pixels, pct -> percentage of 'ref' */
static long parse_length(const char *s, long ref, int *is_pct)
{
    long v;
    const char *p;

    if (is_pct) *is_pct = 0;
    if (!s) return -1;
    v = h_atol(s);
    for (p = s; *p; p++) {
        if (*p == '%') {
            if (is_pct) *is_pct = 1;
            return ref * v / 100;
        }
    }
    return v;
}

static int parse_align(const char *s, int def)
{
    if (!s) return def;
    if (!h_stricmp(s, "center") || !h_stricmp(s, "middle")) return AL_CENTER;
    if (!h_stricmp(s, "right")) return AL_RIGHT;
    if (!h_stricmp(s, "left") || !h_stricmp(s, "justify")) return AL_LEFT;
    return def;
}

/* ------------------------------------------------------------------ */
/* styles                                                              */

static const unsigned char heading_size[6] = { 6, 5, 4, 3, 2, 1 };

static int clamp_size(int s) { return s < 1 ? 1 : s > 7 ? 7 : s; }

static void apply_style(struct LCtx *L, struct HNode *n, struct Style *st)
{
    const char *a;

    switch (n->tag) {
    case T_B: case T_STRONG: case T_TH:
        st->style |= HS_BOLD; break;
    case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: case T_ADDRESS:
        st->style |= HS_ITALIC; break;
    case T_U: case T_INS:
        st->style |= HS_UNDERLINED; break;
    case T_S: case T_STRIKE: case T_DEL:
        st->style |= HS_STRIKE; break;
    case T_TT: case T_CODE: case T_KBD: case T_SAMP:
        st->fixed = 1; break;
    case T_PRE: case T_XMP: case T_LISTING: case T_PLAINTEXT:
        st->fixed = 1; st->pre = 1; break;
    case T_BIG:
        st->size = (unsigned char)clamp_size(st->size + 1); break;
    case T_SMALL:
        st->size = (unsigned char)clamp_size(st->size - 1); break;
    case T_SUB: case T_SUP:
        if (L) {
            long h = fheight(L, font_of(st));
            st->voff = (short)(st->voff + (n->tag == T_SUP ? -h / 3 : h / 4));
        }
        st->size = (unsigned char)clamp_size(st->size - 1);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        st->size = heading_size[n->tag - T_H1];
        st->style |= HS_BOLD;
        break;
    case T_NOBR:
        st->nowrap = 1; break;
    case T_CENTER:
        st->align = AL_CENTER; break;
    case T_FONT:
        if ((a = html_attr(n, "size"))) {
            while (h_isspace((unsigned char)*a)) a++;
            if (*a == '+' || *a == '-') st->size = (unsigned char)clamp_size(3 + h_atol(a));
            else if (*a) st->size = (unsigned char)clamp_size(h_atol(a));
        }
        if ((a = html_attr(n, "color"))) {
            unsigned long c = html_parse_color(a);
            if (c != COL_NONE) st->color = c;
        }
        if ((a = html_attr(n, "face"))) {
            const char *p;
            for (p = a; *p; p++)
                if (!h_strnicmp(p, "courier", 7) || !h_strnicmp(p, "mono", 4)) { st->fixed = 1; break; }
        }
        break;
    case T_A:
        if (n->link >= 0) {
            st->link = n->link;
            st->color = COL_LINK;
            st->style |= HS_UNDERLINED;
        }
        break;
    case T_TD:
        if (html_attr(n, "nowrap")) st->nowrap = 1;
        break;
    }
}

static void init_style(struct LCtx *L, struct Style *st)
{
    h_memset(st, 0, sizeof(*st));
    st->size = 3;
    st->link = -1;
    st->color = L ? L->textcol : 0;
}

/* ------------------------------------------------------------------ */
/* lines                                                               */

static void add_margin(struct Box *b, long m)
{
    if (m > b->margin) b->margin = m;
}

static void apply_margin(struct Box *b)
{
    if (!b->at_top) b->y += b->margin;
    b->margin = 0;
    b->at_top = 0;
}

static void emit_bullet(struct LCtx *L, struct Bullet *bu, long y, long base)
{
    struct HItem *it;

    bu->active = 0;
    if (bu->kind < 0) {
        if ((it = emit(L, IT_TEXT))) {
            it->font = (unsigned char)bu->font;
            it->x = bu->x - L->em / 3 - bu->textw;
            it->y = y;
            it->w = bu->textw;
            it->h = base + fheight(L, bu->font) - fbase(L, bu->font);
            it->base = (short)base;
            it->color = bu->color;
            it->s = bu->text;
            it->len = bu->textlen;
            it->flags = IF_NOSEL;
        }
    } else {
        long fb = fbase(L, bu->font);
        long sz = fb / 2;
        if (sz < 3) sz = 3;
        if ((it = emit(L, IT_BULLET))) {
            it->style = (unsigned char)bu->kind;
            it->w = it->h = sz;
            it->x = bu->x - L->em / 2 - sz;
            if (it->x < 0) it->x = 0;
            it->y = y + base - fb / 2 - sz / 2 - (fb > 6 ? 1 : 0);
            it->color = bu->color;
        }
    }
}

/* flushes the first n fragments as one line */
static void flush_n(struct LCtx *L, struct Box *b, long n)
{
    struct Frag *f = L->frags;
    long asc = 0, desc = 0, lh, linew, dx = 0, i, shift;

    if (n <= 0) return;
    apply_margin(b);

    for (i = 0; i < n; i++) {
        long a = f[i].asc - f[i].voff, d = f[i].desc + f[i].voff;
        if (a > asc) asc = a;
        if (d > desc) desc = d;
    }
    if (b->bullet.active) {
        long a = fbase(L, b->bullet.font), d = fheight(L, b->bullet.font) - a;
        if (a > asc) asc = a;
        if (d > desc) desc = d;
    }
    lh = asc + desc + 1;
    linew = f[n - 1].x + f[n - 1].w;
    if (linew < b->width) {
        if (b->align == AL_CENTER) dx = (b->width - linew) / 2;
        else if (b->align == AL_RIGHT) dx = b->width - linew;
    }

    if (!L->measure) {
        int soft = L->soft;
        for (i = 0; i < n; i++) {
            struct HItem *it;
            long x = b->x0 + dx + f[i].x;
            if (f[i].kind == FK_TEXT) {
                if ((it = emit(L, IT_TEXT))) {
                    if (soft) { it->flags = IF_SOFT; soft = 0; }
                    it->font = f[i].font;
                    it->style = f[i].style;
                    it->link = f[i].link;
                    it->x = x;
                    it->y = b->y;
                    it->w = f[i].w;
                    it->h = lh;
                    it->base = (short)(asc + f[i].voff);
                    it->color = f[i].color;
                    it->s = f[i].s;
                    it->len = f[i].len;
                }
            } else if (f[i].img) {     /* FK_BOX with an image */
                if ((it = emit(L, IT_IMAGE))) {
                    it->link = f[i].link;
                    it->x = x;
                    it->y = b->y + asc + f[i].voff - f[i].asc;
                    it->w = f[i].w;
                    it->h = f[i].asc;
                    it->img = f[i].img;
                }
            } else {        /* FK_BOX: frame sitting on the baseline, optional label */
                long top = b->y + asc + f[i].voff - f[i].asc;
                if ((it = emit(L, IT_FRAME))) {
                    it->style = f[i].flags;
                    it->link = f[i].link;
                    it->x = x;
                    it->y = top;
                    it->w = f[i].w;
                    it->h = f[i].asc;
                    it->color = f[i].color;
                }
                if (f[i].len) {
                    long tw = twidth(L, f[i].font, f[i].style, f[i].s, f[i].len);
                    long th = fheight(L, f[i].font);
                    if (tw <= f[i].w - 4 && th <= f[i].asc - 2 && (it = emit(L, IT_TEXT))) {
                        it->font = f[i].font;
                        it->style = f[i].style & ~HS_UNDERLINED;
                        it->link = f[i].link;
                        it->x = x + (f[i].w - tw) / 2;
                        it->y = top + (f[i].asc - th) / 2;
                        it->w = tw;
                        it->h = th;
                        it->base = (short)fbase(L, f[i].font);
                        it->color = f[i].color;
                        it->s = f[i].s;
                        it->len = f[i].len;
                        it->flags = IF_NOSEL;
                    }
                }
            }
        }
    }
    L->soft = 0;
    if (b->bullet.active) emit_bullet(L, &b->bullet, b->y, asc);
    if (b->x0 + dx + linew > L->max_right) L->max_right = b->x0 + dx + linew;
    b->y += lh;

    /* keep the remaining fragments, moved to the line start */
    shift = n < L->nfr ? f[n].x : 0;
    for (i = n; i < L->nfr; i++) {
        f[i - n] = f[i];
        f[i - n].x -= shift;
    }
    L->nfr -= n;
    b->curx -= shift;
    if (!L->nfr) b->curx = 0;
    b->group = 0;
}

/* ends the current line. If it is empty and empty_h > 0, an empty line
 * of that height is produced instead.                                  */
static void flush_line(struct LCtx *L, struct Box *b, long empty_h)
{
    b->space = 0;
    if (L->nfr) {
        flush_n(L, b, L->nfr);
    } else if (empty_h > 0) {
        apply_margin(b);
        if (b->bullet.active) emit_bullet(L, &b->bullet, b->y, fbase(L, b->bullet.font));
        b->y += empty_h + 1;
    }
    b->curx = 0;
    b->group = 0;
}

static struct Frag *new_frag(struct LCtx *L)
{
    struct Frag *f;
    if (L->oom) return 0;
    if (L->nfr == L->maxfr) {
        struct Frag *na = grow(L, L->frags, &L->maxfr, L->nfr, sizeof(struct Frag));
        if (!na) return 0;
        L->frags = na;
    }
    f = &L->frags[L->nfr++];
    h_memset(f, 0, sizeof(*f));
    return f;
}

/* places an unbreakable piece, wrapping the line if needed */
static struct Frag *place(struct LCtx *L, struct Box *b, const struct Style *st, long w)
{
    int brk = b->space && L->nfr > 0;
    long x = b->curx + (brk ? b->space : 0);
    struct Frag *f;

    b->space = 0;
    if (!st->nowrap && !st->pre && x + w > b->width) {
        if (brk) {
            flush_n(L, b, L->nfr);
            L->soft = 1;
            x = 0;
        } else if (b->group > 0) {
            flush_n(L, b, b->group);
            L->soft = 1;
            x = b->curx;
        }
    }
    if (brk) b->group = L->nfr;
    if (!(f = new_frag(L))) return 0;
    f->x = x;
    f->w = w;
    f->link = st->link;
    f->color = st->color;
    f->voff = st->voff;
    b->curx = x + w;
    return f;
}

static void add_piece(struct LCtx *L, struct Box *b, const struct Style *st, const char *s, long len)
{
    int font = font_of(st);
    long w = twidth(L, font, st->style, s, len);
    struct Frag *f;

    if (!(f = place(L, b, st, w))) return;
    f->kind = FK_TEXT;
    f->s = s;
    f->len = len;
    f->font = (unsigned char)font;
    f->style = st->style;
    f->asc = (short)fbase(L, font);
    f->desc = (short)(fheight(L, font) - f->asc);
}

static void add_text(struct LCtx *L, struct Box *b, const struct Style *st, const char *s, long len)
{
    const char *end = s + len, *w0;
    int font = font_of(st);

    if (st->pre) {
        while (s < end) {
            if (*s == '\n') {
                flush_line(L, b, fheight(L, font));
                s++;
            } else if (*s == '\t') {
                long tab = 8 * spacew(L, font);
                b->curx = (b->curx / tab + 1) * tab;
                s++;
            } else {
                w0 = s;
                while (s < end && *s != '\n' && *s != '\t') s++;
                add_piece(L, b, st, w0, s - w0);
            }
        }
        return;
    }
    while (s < end) {
        if (h_isspace((unsigned char)*s)) {
            while (s < end && h_isspace((unsigned char)*s)) s++;
            if (L->nfr > 0 || b->curx > 0) b->space = spacew(L, font);
            continue;
        }
        w0 = s;
        while (s < end && !h_isspace((unsigned char)*s)) s++;
        add_piece(L, b, st, w0, s - w0);
    }
}

/* inline box (img placeholder, form element) */
static void add_box(struct LCtx *L, struct Box *b, const struct Style *st,
                    long w, long h, int flags, const char *label, void *img)
{
    struct Frag *f;
    int font = font_of(st);

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4000) w = 4000;
    if (h > 4000) h = 4000;
    if (!(f = place(L, b, st, w))) return;
    f->kind = FK_BOX;
    f->asc = (short)h;
    f->desc = 0;
    f->flags = (unsigned char)flags;
    f->font = (unsigned char)font;
    f->style = st->style;
    f->s = label;
    f->len = label ? h_strlen(label) : 0;
    f->img = img;
}

/* ------------------------------------------------------------------ */
/* blocks                                                              */

static void layout_children(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st);
static void layout_node(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st);

static void block(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st,
                  long mtop, long mbottom, long ileft, long iright)
{
    long sx = b->x0, sw = b->width;
    int salign = b->align;

    flush_line(L, b, 0);
    add_margin(b, mtop);
    b->x0 += ileft;
    b->width -= ileft + iright;
    if (b->width < 1) b->width = 1;
    b->align = parse_align(html_attr(n, "align"), st->align != AL_LEFT ? st->align : b->align);
    layout_children(L, b, n, st);
    flush_line(L, b, 0);
    b->x0 = sx;
    b->width = sw;
    b->align = salign;
    add_margin(b, mbottom);
}

static char *make_number(struct LCtx *L, long num, int type)
{
    char tmp[24], *out;
    int n = 0, i;

    if (num < 1 && type != '1') type = '1';
    if (type == 'a' || type == 'A') {
        char rev[12]; int k = 0;
        while (num > 0 && k < 10) { num--; rev[k++] = (char)((type == 'a' ? 'a' : 'A') + num % 26); num /= 26; }
        while (k) tmp[n++] = rev[--k];
    } else if ((type == 'i' || type == 'I') && num < 4000) {
        static const short val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        static const char *const sym[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
        for (i = 0; i < 13; i++)
            while (num >= val[i]) {
                const char *p;
                for (p = sym[i]; *p; p++) tmp[n++] = (char)(type == 'I' ? *p - 32 : *p);
                num -= val[i];
            }
    } else {
        char rev[12]; int k = 0, neg = num < 0;
        unsigned long u = neg ? -num : num;
        do { rev[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 11);
        if (neg) tmp[n++] = '-';
        while (k) tmp[n++] = rev[--k];
    }
    tmp[n++] = '.';
    if (!(out = hsys_alloc(L->lay->pool, n + 1))) { L->oom = 1; return 0; }
    h_memcpy(out, tmp, n);
    return out;
}

static void list_item(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    struct Bullet *bu = &b->bullet;
    const char *a;

    flush_line(L, b, 0);
    if (bu->active) {                         /* <li><li>: previous marker alone */
        apply_margin(b);
        emit_bullet(L, bu, b->y, fbase(L, bu->font));
    }
    h_memset(bu, 0, sizeof(*bu));
    bu->active = 1;
    bu->x = b->x0;
    bu->font = font_of(st);
    bu->color = st->color == COL_LINK ? L->textcol : st->color;

    if ((a = html_attr(n, "value"))) b->counter = h_atol(a);
    if (b->listkind) {
        int type = b->listkind;
        if ((a = html_attr(n, "type")) && *a) type = *a;
        bu->kind = -1;
        if ((bu->text = make_number(L, b->counter, type))) {
            bu->textlen = h_strlen(bu->text);
            bu->textw = twidth(L, bu->font, 0, bu->text, bu->textlen);
        } else {
            bu->active = 0;
        }
        b->counter++;
    } else {
        bu->kind = st->listdepth <= 1 ? BUL_DISC : st->listdepth == 2 ? BUL_CIRCLE : BUL_SQUARE;
        if ((a = html_attr(n, "type")) || (n->parent && (a = html_attr(n->parent, "type")))) {
            if (!h_stricmp(a, "circle")) bu->kind = BUL_CIRCLE;
            else if (!h_stricmp(a, "square")) bu->kind = BUL_SQUARE;
            else if (!h_stricmp(a, "disc")) bu->kind = BUL_DISC;
        }
    }
    layout_children(L, b, n, st);
    flush_line(L, b, 0);
    if (bu->active) flush_line(L, b, fheight(L, bu->font));
}

static void list(struct LCtx *L, struct Box *b, struct HNode *n, struct Style *st)
{
    long sx = b->x0, sw = b->width, sc = b->counter;
    int sk = b->listkind;
    long indent = L->em * 5 / 2, m = 0;
    const char *a;

    flush_line(L, b, 0);
    if (st->listdepth == 0) m = fheight(L, font_of(st));
    add_margin(b, m);
    st->listdepth++;
    b->x0 += indent;
    b->width -= indent;
    if (b->width < 1) b->width = 1;
    b->listkind = 0;
    b->counter = 1;
    if (n->tag == T_OL) {
        b->listkind = '1';
        if ((a = html_attr(n, "type")) && *a) b->listkind = *a;
        if ((a = html_attr(n, "start"))) b->counter = h_atol(a);
    }
    layout_children(L, b, n, st);
    flush_line(L, b, 0);
    b->x0 = sx;
    b->width = sw;
    b->listkind = sk;
    b->counter = sc;
    add_margin(b, m);
}

static void hrule(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    struct HItem *it;
    int pct, noshade = html_attr(n, "noshade") != 0;
    long w = parse_length(html_attr(n, "width"), b->width, &pct);
    long h = parse_length(html_attr(n, "size"), 0, 0);
    long x = b->x0, lh = fheight(L, font_of(st));
    int al = parse_align(html_attr(n, "align"), AL_CENTER);
    unsigned long c = html_parse_color(html_attr(n, "color"));

    flush_line(L, b, 0);
    add_margin(b, lh / 2);
    apply_margin(b);
    if (w <= 0 || w > b->width) w = b->width;
    if (h <= 0) h = 2;
    if (h > 200) h = 200;
    if (al == AL_CENTER) x += (b->width - w) / 2;
    else if (al == AL_RIGHT) x += b->width - w;
    if ((it = emit(L, IT_HRULE))) {
        it->x = x;
        it->y = b->y;
        it->w = w;
        it->h = h;
        it->style = (unsigned char)((noshade || c != COL_NONE) ? HR_NOSHADE : 0);
        it->color = c != COL_NONE ? c : L->textcol;
    }
    if (!pct && x + w > L->max_right) L->max_right = x + w;
    b->y += h;
    add_margin(b, lh / 2);
}

static void image(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    const char *alt = html_attr(n, "alt");
    long w = parse_length(html_attr(n, "width"), b->width, 0);
    long h = parse_length(html_attr(n, "height"), 0, 0);
    int font = font_of(st);
    const char *label;

    if (n->img && n->iw > 0 && n->ih > 0) {             /* loaded picture */
        if (w <= 0 && h <= 0) { w = n->iw; h = n->ih; }
        else if (w <= 0) w = n->iw * h / n->ih;
        else if (h <= 0) h = n->ih * w / n->iw;
        add_box(L, b, st, w, h, 0, 0, n->img);
        return;
    }
    if (alt && !*alt && w <= 0 && h <= 0) return;      /* alt="" -> decorative, skip */
    label = (alt && *alt) ? alt : 0;
    if (w <= 0) w = label ? twidth(L, font, st->style, label, h_strlen(label)) + 8 : 16;
    if (h <= 0) h = label ? fheight(L, font) + 4 : 16;
    add_box(L, b, st, w, h, 0, label, 0);
}

static void input(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    const char *type = html_attr(n, "type"), *val = html_attr(n, "value");
    int font = font_of(st);
    long h = fheight(L, font) + 4, cw = spacew(L, font);

    if (!type) type = "text";
    if (!h_stricmp(type, "hidden")) return;
    if (!h_stricmp(type, "submit") || !h_stricmp(type, "reset") || !h_stricmp(type, "button")) {
        if (!val || !*val) val = !h_stricmp(type, "reset") ? "Reset" : "Submit";
        add_box(L, b, st, twidth(L, font, 0, val, h_strlen(val)) + 12, h, FR_RAISED, val, 0);
    } else if (!h_stricmp(type, "checkbox") || !h_stricmp(type, "radio")) {
        add_box(L, b, st, fbase(L, font) + 2, fbase(L, font) + 2, 0, 0, 0);
    } else if (!h_stricmp(type, "image")) {
        image(L, b, n, st);
    } else {
        long size = h_atol(html_attr(n, "size") ? html_attr(n, "size") : "20");
        if (size < 1) size = 20;
        if (size > 100) size = 100;
        add_box(L, b, st, size * cw + 6, h, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* tables                                                              */

struct TCell {
    struct HNode *n;
    int           col, span;
    long          min, max, pct;
    long          h;
    long          istart, iend, bgidx;
    unsigned long bg;
    int           valign;
};

struct TRow {
    struct HNode *n;
    long          first, ncells;
};

static long cell_content(struct LCtx *L, struct HNode *n, const struct Style *st,
                         long x0, long width, long y, int align)
{
    struct Box cb;

    h_memset(&cb, 0, sizeof(cb));
    cb.x0 = x0;
    cb.width = width < 1 ? 1 : width;
    cb.y = y;
    cb.at_top = 1;
    cb.align = align;
    layout_children(L, &cb, n, st);
    flush_line(L, &cb, 0);
    if (cb.bullet.active) flush_line(L, &cb, fheight(L, cb.bullet.font));
    return cb.y - y;
}

static void cell_style(struct LCtx *L, struct HNode *cell, const struct Style *st, struct Style *cs, int *align)
{
    *cs = *st;
    apply_style(L, cell, cs);
    cs->align = AL_LEFT;
    *align = parse_align(html_attr(cell, "align"),
             parse_align(cell->parent ? html_attr(cell->parent, "align") : 0,
                         cell->tag == T_TH ? AL_CENTER : AL_LEFT));
}

static void table(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    struct HNode *c, *r, *caption = 0;
    struct TRow *rows = 0;
    struct TCell *cells = 0;
    long nrows = 0, ncells = 0, ncols = 0, i, j, k;
    long *colmin, *colmax, *colw, *colx;
    long border, spacing, padding, cb, overhead, avail, tw, tx, ty, y0, twant = -1;
    long summin = 0, summax = 0;
    unsigned long tbg;
    const char *a;
    int pct = 0, align;
    long tbgidx = -1;
    long *colmem = 0;
    void *pool = L->lay->pool;
    struct Style ts = *st;

    flush_line(L, b, 0);
    ts.align = AL_LEFT;
    ts.listdepth = 0;

    /* attributes */
    a = html_attr(n, "border");
    border = a ? (*a ? h_atol(a) : 1) : 0;
    if (border < 0) border = 0;
    if (border > 20) border = 20;
    a = html_attr(n, "cellspacing");
    spacing = a ? h_atol(a) : 2;
    a = html_attr(n, "cellpadding");
    padding = a ? h_atol(a) : 1;
    if (spacing < 0) spacing = 0;
    if (padding < 0) padding = 0;
    cb = border ? 1 : 0;
    tbg = html_parse_color(html_attr(n, "bgcolor"));
    if ((a = html_attr(n, "width"))) {
        twant = parse_length(a, b->width, &pct);
        if (pct && L->measure) twant = -1;     /* % of an unknown width */
    }

    /* count rows and cells */
    for (i = 0; i < 2; i++) {
        nrows = ncells = 0;
        for (c = n->first; c; c = c->next) {
            struct HNode *rs = (c->tag == T_THEAD || c->tag == T_TBODY || c->tag == T_TFOOT) ? c->first : c;
            for (r = rs; r; r = (rs == c ? 0 : r->next)) {
                if (r->tag == T_CAPTION && !caption) caption = r;
                if (r->tag != T_TR) continue;
                if (i) { rows[nrows].n = r; rows[nrows].first = ncells; }
                {
                    struct HNode *d;
                    long cnt = 0;
                    for (d = r->first; d; d = d->next) {
                        if (d->tag != T_TD && d->tag != T_TH) continue;
                        if (i) cells[ncells + cnt].n = d;
                        cnt++;
                    }
                    if (i) rows[nrows].ncells = cnt;
                    ncells += cnt;
                }
                nrows++;
            }
        }
        if (!i) {
            if (!nrows || !ncells) break;
            rows = hsys_alloc(pool, nrows * sizeof(*rows));
            cells = hsys_alloc(pool, ncells * sizeof(*cells));
            if (!rows || !cells) { L->oom = 1; goto done; }
        }
    }

    if (!(colmem = hsys_alloc(pool, 4 * MAXCOLS * sizeof(long)))) { L->oom = 1; goto done; }
    colmin = colmem;
    colmax = colmem + MAXCOLS;
    colw = colmem + 2 * MAXCOLS;
    colx = colmem + 3 * MAXCOLS;

    add_margin(b, 0);
    apply_margin(b);
    y0 = b->y;

    if (!rows) {                               /* empty table: just the caption */
        if (caption) {
            struct Style cs = ts;
            b->y += cell_content(L, caption, &cs, b->x0, b->width, b->y, AL_CENTER);
        }
        goto done;
    }

    /* column assignment and min/max widths */
    for (i = 0; i < MAXCOLS; i++) colmin[i] = colmax[i] = 0;
    for (i = 0; i < nrows; i++) {
        int col = 0;
        for (j = 0; j < rows[i].ncells; j++) {
            struct TCell *ce = &cells[rows[i].first + j];
            struct Style cs;
            int cal;
            long span = (a = html_attr(ce->n, "colspan")) ? h_atol(a) : 1;
            long w;
            if (span < 1) span = 1;
            if (col + span > MAXCOLS) span = MAXCOLS - col;
            if (span < 1) { ce->span = 0; continue; }
            ce->col = col;
            ce->span = (int)span;
            col += (int)span;
            if (col > ncols) ncols = col;

            cell_style(L, ce->n, &ts, &cs, &cal);
            if (ce->n->cgen == L->env->generation) {        /* widths do not depend on the viewport */
                ce->min = ce->n->cmin;
                ce->max = ce->n->cmax;
            } else {
                long save = L->max_right;
                L->measure++;
                L->max_right = 0;
                cell_content(L, ce->n, &cs, 0, 1, 0, AL_LEFT);
                ce->min = L->max_right;
                L->max_right = 0;
                cell_content(L, ce->n, &cs, 0, HUGEWIDTH, 0, AL_LEFT);
                ce->max = L->max_right;
                L->max_right = save;
                L->measure--;
                ce->n->cmin = ce->min;
                ce->n->cmax = ce->max;
                ce->n->cgen = L->env->generation;
            }
            if (cs.nowrap) ce->min = ce->max;
            ce->min += 2 * padding;
            ce->max += 2 * padding;
            if (ce->max < ce->min) ce->max = ce->min;
            w = parse_length(html_attr(ce->n, "width"), 100, &pct);
            if (w > 0 && pct) ce->pct = w;
            else if (w > 0)                              /* width="n" is a preference */
                ce->max = w > ce->min ? w : ce->min;
            if (span == 1) {
                if (ce->min > colmin[ce->col]) colmin[ce->col] = ce->min;
                if (ce->max > colmax[ce->col]) colmax[ce->col] = ce->max;
            }
        }
    }
    /* spanning cells: distribute what is missing evenly */
    for (i = 0; i < ncells; i++) {
        struct TCell *ce = &cells[i];
        long smin = 0, smax = 0, extra = (ce->span - 1) * (spacing + 2 * cb);
        if (ce->span <= 1) continue;
        for (k = ce->col; k < ce->col + ce->span; k++) { smin += colmin[k]; smax += colmax[k]; }
        if (ce->min > smin + extra)
            for (k = ce->col; k < ce->col + ce->span; k++) colmin[k] += (ce->min - smin - extra + ce->span - 1) / ce->span;
        if (ce->max > smax + extra)
            for (k = ce->col; k < ce->col + ce->span; k++) colmax[k] += (ce->max - smax - extra + ce->span - 1) / ce->span;
    }
    /* width="n%" on single cells, relative to the table's inner width */
    overhead = 2 * border + (ncols + 1) * spacing + ncols * 2 * cb;
    avail = (twant > 0 ? twant : b->width) - overhead;
    if (!L->measure) {
        for (i = 0; i < ncells; i++) {
            struct TCell *ce = &cells[i];
            if (ce->span == 1 && ce->pct > 0) {
                long need = avail * ce->pct / 100;
                if (need < colmin[ce->col]) need = colmin[ce->col];
                colmin[ce->col] = colmax[ce->col] = need;
            }
        }
    }
    for (k = 0; k < ncols; k++) {
        if (colmax[k] < colmin[k]) colmax[k] = colmin[k];
        summin += colmin[k];
        summax += colmax[k];
    }

    /* final column widths */
    if (summax <= avail) {
        for (k = 0; k < ncols; k++) colw[k] = colmax[k];
        if (twant > 0 && avail > summax) {
            long extra = avail - summax, given = 0;
            for (k = 0; k < ncols; k++) {
                long e = summax ? extra * colmax[k] / summax : extra / ncols;
                colw[k] += e;
                given += e;
            }
            colw[ncols - 1] += extra - given;
        }
    } else if (summin >= avail) {
        for (k = 0; k < ncols; k++) colw[k] = colmin[k];
    } else {
        long d = summax - summin, room = avail - summin, given = 0;
        for (k = 0; k < ncols; k++) {
            long e = (long)((long long)(colmax[k] - colmin[k]) * room / d);
            colw[k] = colmin[k] + e;
            given += e;
        }
        colw[ncols - 1] += room - given;
    }
    tw = overhead;
    for (k = 0; k < ncols; k++) tw += colw[k];

    align = parse_align(html_attr(n, "align"), b->align);
    tx = b->x0;
    if (tw < b->width) {
        if (align == AL_CENTER) tx += (b->width - tw) / 2;
        else if (align == AL_RIGHT) tx += b->width - tw;
    }
    colx[0] = tx + border + spacing;
    for (k = 1; k < ncols; k++) colx[k] = colx[k - 1] + colw[k - 1] + 2 * cb + spacing;

    /* caption above the table */
    if (caption) {
        struct Style cs = ts;
        b->y += cell_content(L, caption, &cs, tx, tw, b->y, AL_CENTER);
        y0 = b->y;
    }

    if (tbg != COL_NONE || n->img) {
        struct HItem *it = emit(L, IT_RECT);
        if (it) {
            it->x = tx; it->y = y0; it->w = tw; it->color = tbg;
            it->img = n->img;
            tbgidx = L->lay->nitems - 1;
        }
    }

    ty = y0 + border + spacing;
    for (i = 0; i < nrows; i++) {
        long rowh = 0;
        unsigned long rbg = html_parse_color(html_attr(rows[i].n, "bgcolor"));
        const char *rva = html_attr(rows[i].n, "valign");

        for (j = 0; j < rows[i].ncells; j++) {
            struct TCell *ce = &cells[rows[i].first + j];
            struct Style cs;
            int cal;
            long cw, cx, h;

            if (!ce->span) continue;
            cw = colw[ce->col];
            for (k = ce->col + 1; k < ce->col + ce->span; k++) cw += colw[k] + spacing + 2 * cb;
            cx = colx[ce->col];
            ce->bg = html_parse_color(html_attr(ce->n, "bgcolor"));
            if (ce->bg == COL_NONE) ce->bg = rbg;
            a = html_attr(ce->n, "valign");
            if (!a) a = rva;
            ce->valign = a ? (!h_stricmp(a, "top") ? 0 : !h_stricmp(a, "bottom") ? 2 : 1) : 1;
            ce->bgidx = -1;
            if (ce->bg != COL_NONE || ce->n->img) {
                struct HItem *it = emit(L, IT_RECT);
                if (it) {
                    it->x = cx + cb; it->y = ty + cb; it->w = cw; it->color = ce->bg;
                    it->img = ce->n->img;
                    ce->bgidx = L->lay->nitems - 1;
                }
            }
            ce->istart = L->lay->nitems;
            cell_style(L, ce->n, &ts, &cs, &cal);
            h = cell_content(L, ce->n, &cs, cx + cb + padding, cw - 2 * padding, ty + cb + padding, cal);
            ce->iend = L->lay->nitems;
            ce->h = h + 2 * padding + 2 * cb;
            ce->min = cw;                                 /* remember width for the frame */
            ce->max = cx;
            if (ce->h > rowh) rowh = ce->h;
        }
        /* vertical alignment, backgrounds, cell frames */
        for (j = 0; j < rows[i].ncells; j++) {
            struct TCell *ce = &cells[rows[i].first + j];
            long off, m;
            if (!ce->span) continue;
            off = ce->valign == 0 ? 0 : ce->valign == 2 ? rowh - ce->h : (rowh - ce->h) / 2;
            if (off && !L->measure)
                for (m = ce->istart; m < ce->iend; m++) L->lay->items[m].y += off;
            if (ce->bgidx >= 0 && !L->measure) L->lay->items[ce->bgidx].h = rowh - 2 * cb;
            if (cb) {
                struct HItem *it = emit(L, IT_FRAME);
                if (it) {
                    it->x = ce->max; it->y = ty; it->w = ce->min + 2; it->h = rowh;
                    it->color = COL_NONE;
                }
            }
        }
        ty += rowh + spacing;
    }
    ty += border;
    if (tbgidx >= 0 && !L->measure) L->lay->items[tbgidx].h = ty - y0;
    for (k = 0; k < border; k++) {
        struct HItem *it = emit(L, IT_FRAME);
        if (it) {
            it->x = tx + k; it->y = y0 + k; it->w = tw - 2 * k; it->h = ty - y0 - 2 * k;
            it->style = FR_RAISED;
            it->color = COL_NONE;
        }
    }
    if (tx + tw > L->max_right) L->max_right = tx + tw;
    b->y = ty;

done:
    if (colmem) hsys_free(pool, colmem, 4 * MAXCOLS * sizeof(long));
    if (rows) hsys_free(pool, rows, nrows * sizeof(*rows));
    if (cells) hsys_free(pool, cells, ncells * sizeof(*cells));
}

/* ------------------------------------------------------------------ */
/* tree walk                                                           */

static void layout_children(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    struct HNode *c;
    for (c = n->first; c && !L->oom; c = c->next) layout_node(L, b, c, st);
}

static void layout_node(struct LCtx *L, struct Box *b, struct HNode *n, const struct Style *st)
{
    struct Style s;
    long lh;
    const char *a;

    if (n->tag == T_TEXT) {
        add_text(L, b, st, n->text, n->len);
        return;
    }
    s = *st;
    apply_style(L, n, &s);
    lh = fheight(L, font_of(&s));

    if ((a = html_attr(n, "id"))) add_anchor(L, a, b->y + (b->at_top ? 0 : b->margin));
    if (n->tag == T_A && (a = html_attr(n, "name"))) add_anchor(L, a, b->y + (b->at_top ? 0 : b->margin));

    switch (n->tag) {
    case T_HEAD: case T_TITLE: case T_SCRIPT: case T_STYLE: case T_SELECT:
    case T_OPTION: case T_TEXTAREA: case T_MAP: case T_AREA: case T_FRAMESET:
    case T_FRAME: case T_PARAM: case T_COL: case T_COLGROUP: case T_BASE:
    case T_LINK: case T_META:
        return;
    case T_BR:
        flush_line(L, b, lh);
        return;
    case T_IMG:
        image(L, b, n, &s);
        return;
    case T_INPUT:
        input(L, b, n, &s);
        return;
    case T_HR:
        hrule(L, b, n, &s);
        return;
    case T_TABLE:
        table(L, b, n, &s);
        return;
    case T_P:
        block(L, b, n, &s, lh, lh, 0, 0);
        return;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        block(L, b, n, &s, lh * 2 / 3, lh * 2 / 3, 0, 0);
        return;
    case T_PRE: case T_XMP: case T_LISTING: case T_PLAINTEXT:
        block(L, b, n, &s, lh, lh, 0, 0);
        return;
    case T_BLOCKQUOTE:
        block(L, b, n, &s, lh, lh, L->em * 5 / 2, L->em * 5 / 2);
        return;
    case T_DL:
        block(L, b, n, &s, (n->parent && n->parent->tag == T_DD) ? 0 : lh,
              (n->parent && n->parent->tag == T_DD) ? 0 : lh, 0, 0);
        return;
    case T_DD:
        block(L, b, n, &s, 0, 0, L->em * 5 / 2, 0);
        return;
    case T_UL: case T_OL: case T_MENU: case T_DIR:
        list(L, b, n, &s);
        return;
    case T_LI:
        list_item(L, b, n, &s);
        return;
    case T_FORM:
        block(L, b, n, &s, 0, lh, 0, 0);
        return;
    case T_DIV: case T_CENTER: case T_ADDRESS: case T_DT: case T_FIELDSET:
    case T_NOFRAMES: case T_CAPTION: case T_TR: case T_TD: case T_TH:
    case T_THEAD: case T_TBODY: case T_TFOOT: case T_LEGEND:
        block(L, b, n, &s, 0, 0, 0, 0);
        return;
    case T_Q:
        add_text(L, b, &s, "\"", 1);
        layout_children(L, b, n, &s);
        add_text(L, b, &s, "\"", 1);
        return;
    default:
        layout_children(L, b, n, &s);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* public                                                              */

struct HLayout *html_layout(struct HDoc *doc, struct HEnv *env, long width)
{
    struct LCtx L;
    struct HLayout *lay;
    struct Box root;
    struct Style st;
    void *pool;

    if (!(pool = hsys_pool_create())) return 0;
    if (!(lay = hsys_alloc(pool, sizeof(*lay)))) { hsys_pool_delete(pool); return 0; }
    lay->pool = pool;

    h_memset(&L, 0, sizeof(L));
    L.lay = lay;
    L.doc = doc;
    L.env = env;
    L.em = env->font_height[HF_INDEX(0, 3)];
    if (L.em < 6) L.em = 6;
    L.textcol = doc->text != COL_NONE ? doc->text : env->system_colors ? (COL_PEN | PEN_TEXT) : 0x000000;
    lay->bgcolor = doc->bgcolor != COL_NONE ? doc->bgcolor :
                   env->system_colors ? (COL_PEN | PEN_BACKGROUND) : 0xFFFFFF;

    h_memset(&root, 0, sizeof(root));
    root.x0 = env->margin;
    root.width = width - 2 * env->margin;
    if (root.width < 1) root.width = 1;
    root.y = env->margin;
    root.at_top = 1;
    init_style(&L, &st);

    layout_children(&L, &root, doc->root, &st);
    flush_line(&L, &root, 0);
    if (root.bullet.active) flush_line(&L, &root, fheight(&L, root.bullet.font));

    if (L.oom) {
        hsys_pool_delete(pool);
        return 0;
    }
    lay->width = L.max_right + env->margin;
    if (lay->width < width) lay->width = width;
    lay->height = root.y + env->margin;
    return lay;
}

void html_free_layout(struct HLayout *lay)
{
    if (lay) hsys_pool_delete(lay->pool);
}

long html_find_anchor(struct HLayout *lay, const char *name)
{
    long i;
    if (!lay || !name) return -1;
    if (*name == '#') name++;
    for (i = 0; i < lay->nanchors; i++)
        if (!h_stricmp(lay->anchors[i].name, name)) return lay->anchors[i].y;
    return -1;
}

/* ------------------------------------------------------------------ */
/* which fonts are needed                                              */

static void fonts_walk(struct HNode *n, const struct Style *st, unsigned long *mask)
{
    struct HNode *c;
    for (c = n->first; c; c = c->next) {
        if (c->tag == T_TEXT) {
            *mask |= 1UL << font_of(st);
        } else {
            struct Style s = *st;
            apply_style(0, c, &s);
            if (c->tag == T_IMG || c->tag == T_INPUT || c->tag == T_BR ||
                c->tag == T_LI || c->tag == T_HR || c->tag == T_OL || c->tag == T_UL)
                *mask |= 1UL << font_of(&s);
            fonts_walk(c, &s, mask);
        }
    }
}

unsigned long html_fonts_used(struct HDoc *doc)
{
    struct Style st;
    unsigned long mask = 1UL << HF_INDEX(0, 3);
    init_style(0, &st);
    fonts_walk(doc->root, &st, &mask);
    return mask;
}
