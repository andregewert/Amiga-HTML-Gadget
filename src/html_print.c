/*
 * html_print.c - PostScript and PDF output of a document, see html_print.h
 *
 * Units: the layout runs in "pixels" of 1/96 inch (0.75 pt), so sizes in
 * HTML attributes (image sizes, table widths) come out as in a browser.
 * Text is measured with the widths of the standard PostScript fonts.
 *
 * One drawing routine serves both formats: the PostScript prolog defines
 * procedures with the names of the PDF operators (m l c h f S re rg RG w
 * J q Q W n BT ET Tf Td Tj), so the page contents are the same text.
 * Coordinates are written in points with two decimals.
 *
 * Pages are broken where no text line, picture, rule or checkbox is cut;
 * backgrounds and frames may continue on the next page.
 *
 * Pictures: the pixels come from the image callback as ARGB. PDF gets an
 * image object per picture (transparency as soft mask), PostScript the
 * pixels with colorimage at every place, composed onto white. Tiled
 * background pictures are not printed. Compression: PDF LZW with PNG
 * predictors, PostScript level 2 LZW in ASCII85 (no predictors before
 * level 3), level 1 plain hex. JPEG files (HPrintImage.jpeg) are passed
 * on as they are with DCTDecode: in PDF and in PostScript level 2, there
 * only baseline JPEGs (progressive ones need level 3).
 *
 * No C library is used apart from the memory hooks of the core.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_print.h"

#define PEN_TEXT        2           /* as in html_layout.c */
#define PEN_BACKGROUND  7
#define DEF_LINK        0x0000CCUL  /* links without <body link> */
#define C_RULE          0x999999UL  /* shaded rules, frames, missing pictures */
#define C_BORDER        0x767676UL  /* checkboxes, as html_check.c */
#define C_FIELD         0xFFFFFFUL
#define C_ACCENT        0x0969DAUL
#define C_MARK          0xFFFFFFUL

/* fonts: family * 4 + bold * 2 + italic */
enum { FAM_SANS, FAM_SERIF, FAM_MONO };
static const char *const font_names[12] = {
    "Helvetica", "Helvetica-Oblique", "Helvetica-Bold", "Helvetica-BoldOblique",
    "Times-Roman", "Times-Italic", "Times-Bold", "Times-BoldItalic",
    "Courier", "Courier-Oblique", "Courier-Bold", "Courier-BoldOblique"
};
/* ascender and descender of the families in 1/1000 em */
static const short fam_asc[3] = { 718, 683, 629 }, fam_desc[3] = { 207, 217, 157 };
/* the size levels 1..7 of the layout in 1/12 of the normal size (as htmlttf) */
static const unsigned char level_f[7] = { 8, 10, 12, 14, 18, 24, 36 };

struct Out {
    const struct HPrintOpts *o;
    void  *pool;
    long   pos;                     /* bytes written so far (PDF offsets) */
    char   buf[512];
    long   n;
    /* PDF page contents are collected first (their length is needed) */
    char  *mem;
    long   memlen, memsize;
    int    tomem, oom;
};

struct Ctx {
    struct Out      out;
    const struct HPrintOpts *opt;
    void          **imgs;           /* PDF: the pictures with an image object */
    long            nimgs;
    int             pdf14;          /* a soft mask was written: PDF 1.4 */
    long           *lzwkey;         /* LZW hash table, allocated when needed */
    short          *lzwcode;
    struct HDoc    *doc;
    struct HLayout *lay;
    int             prop;           /* family of proportional text */
    long            size10[HF_NUM]; /* font sizes in 1/10 pt */
    long            top;            /* layout y of the page top */
    long            px0, py0;       /* page origin in 1/100 pt: left, top of the content */
    unsigned long   fill;           /* current fill colour, ~0 = unknown */
    int             font;           /* current font, -1 = unknown */
    long            fsize;
};

/*****************************************************************************/
/* output                                                                    */

static void flush(struct Out *w)
{
    if (!w->n) return;
    if (w->tomem) {
        if (w->memlen + w->n > w->memsize) {
            long size = (w->memlen + w->n) * 2 + 4096;
            char *m = hsys_alloc(w->pool, size);
            if (!m) { w->oom = 1; w->n = 0; return; }
            if (w->mem) {
                long i;
                for (i = 0; i < w->memlen; i++) m[i] = w->mem[i];
                hsys_free(w->pool, w->mem, w->memsize);
            }
            w->mem = m;
            w->memsize = size;
        }
        {
            long i;
            for (i = 0; i < w->n; i++) w->mem[w->memlen + i] = w->buf[i];
        }
        w->memlen += w->n;
    } else {
        w->o->write(w->o->user, w->buf, w->n);
        w->pos += w->n;
    }
    w->n = 0;
}

static void putc_(struct Out *w, char c)
{
    if (w->n == sizeof(w->buf)) flush(w);
    w->buf[w->n++] = c;
}

static void puts_(struct Out *w, const char *s)
{
    while (*s) putc_(w, *s++);
}

static void put_long(struct Out *w, long v)
{
    char d[12];
    int i = 0;
    unsigned long u;
    if (v < 0) { putc_(w, '-'); u = (unsigned long)-v; } else u = (unsigned long)v;
    do d[i++] = (char)('0' + u % 10); while ((u /= 10) && i < 11);
    while (i) putc_(w, d[--i]);
}

/* v in 1/100: "12.5" */
static void put_fix(struct Out *w, long v)
{
    long a;
    if (v < 0) { putc_(w, '-'); v = -v; }
    a = v % 100;
    put_long(w, v / 100);
    if (a) {
        putc_(w, '.');
        putc_(w, (char)('0' + a / 10));
        if (a % 10) putc_(w, (char)('0' + a % 10));
    }
}

static void put_sp(struct Out *w, const char *s)
{
    putc_(w, ' ');
    puts_(w, s);
}

/* a string in PostScript / PDF syntax; bytes outside ISO-8859-1 text
 * become '?'                                                          */
static void put_string(struct Out *w, const char *s, long len)
{
    putc_(w, '(');
    while (len-- > 0) {
        unsigned char c = (unsigned char)*s++;
        if (c < 32 || (c >= 127 && c < 160)) c = '?';
        if (c == '(' || c == ')' || c == '\\') {
            putc_(w, '\\');
            putc_(w, (char)c);
        } else if (c >= 128) {
            putc_(w, '\\');
            putc_(w, (char)('0' + (c >> 6)));
            putc_(w, (char)('0' + ((c >> 3) & 7)));
            putc_(w, (char)('0' + (c & 7)));
        } else putc_(w, (char)c);
    }
    putc_(w, ')');
}

/*****************************************************************************/
/* metrics                                                                   */

static const unsigned short *width_table(int fam, int bold, int italic)
{
    if (fam == FAM_SANS) return bold ? hp_w_helvetica_bold : hp_w_helvetica;
    if (bold) return italic ? hp_w_times_bolditalic : hp_w_times_bold;
    return italic ? hp_w_times_italic : hp_w_times_roman;
}

static int family(struct Ctx *c, int font)
{
    return font >= 7 ? FAM_MONO : c->prop;
}

/* width of the string in 1/1000 em */
static long em_width(int fam, int style, const char *s, long len)
{
    const unsigned short *t;
    long w = 0;
    if (fam == FAM_MONO) return len * 600;
    t = width_table(fam, (style & HS_BOLD) != 0, (style & HS_ITALIC) != 0);
    while (len-- > 0) {
        unsigned char ch = (unsigned char)*s++;
        if (ch >= 32) w += t[ch - 32];
    }
    return w;
}

static long text_width_cb(void *user, int font, int style, const char *s, long len)
{
    struct Ctx *c = user;
    /* em/1000 * size(pt) / 0.75 pt per pixel */
    long long w = (long long)em_width(family(c, font), style, s, len) * c->size10[font];
    return (long)((w + 3750) / 7500);
}

/*****************************************************************************/
/* drawing primitives (the same operators in PS and PDF)                     */

/* layout pixels -> 1/100 pt on the page */
static long X(struct Ctx *c, long x) { return c->px0 + x * 75; }
static long Y(struct Ctx *c, long y) { return c->py0 - (y - c->top) * 75; }

static void put_xy(struct Out *w, long x, long y)
{
    put_fix(w, x);
    putc_(w, ' ');
    put_fix(w, y);
}

/* 0..255 -> "0.537" */
static void put_comp(struct Out *w, unsigned long v)
{
    long k = (long)(v * 1000 + 127) / 255;
    if (k >= 1000) { putc_(w, '1'); return; }
    putc_(w, '0');
    if (!k) return;
    putc_(w, '.');
    putc_(w, (char)('0' + k / 100));
    k %= 100;
    if (k) {
        putc_(w, (char)('0' + k / 10));
        if (k % 10) putc_(w, (char)('0' + k % 10));
    }
}

static void put_rgb(struct Out *w, unsigned long rgb, const char *op)
{
    put_comp(w, rgb >> 16 & 255); putc_(w, ' ');
    put_comp(w, rgb >> 8 & 255); putc_(w, ' ');
    put_comp(w, rgb & 255);
    put_sp(w, op);
    putc_(w, '\n');
}

static void fill_colour(struct Ctx *c, unsigned long rgb)
{
    if (rgb == c->fill) return;
    c->fill = rgb;
    put_rgb(&c->out, rgb, "rg");
}

/* rectangle in layout pixels, filled */
static void fill_rect(struct Ctx *c, long x, long y, long w, long h, unsigned long rgb)
{
    struct Out *o = &c->out;
    if (w <= 0 || h <= 0) return;
    fill_colour(c, rgb);
    put_xy(o, X(c, x), Y(c, y + h));
    putc_(o, ' ');
    put_xy(o, w * 75, h * 75);
    puts_(o, " re f\n");
}

/* rectangle in 1/100 pt, filled */
static void fill_pt(struct Ctx *c, long x, long y, long w, long h, unsigned long rgb)
{
    struct Out *o = &c->out;
    if (w <= 0 || h <= 0) return;
    fill_colour(c, rgb);
    put_xy(o, x, y);
    putc_(o, ' ');
    put_xy(o, w, h);
    puts_(o, " re f\n");
}

/* the stroke colour; in PostScript RG and rg are both setrgbcolor, so
 * the fill colour is unknown afterwards                               */
static void stroke_colour(struct Ctx *c, unsigned long rgb)
{
    put_rgb(&c->out, rgb, "RG");
    if (c->opt->format != HP_PDF) c->fill = ~0UL;
}

static void stroke_rect(struct Ctx *c, long x, long y, long w, long h, unsigned long rgb)
{
    struct Out *o = &c->out;
    if (w <= 0 || h <= 0) return;
    stroke_colour(c, rgb);
    puts_(o, "0.5 w ");
    put_xy(o, X(c, x) + 25, Y(c, y + h) + 25);
    putc_(o, ' ');
    put_xy(o, w * 75 - 50, h * 75 - 50);
    puts_(o, " re S\n");
}

static void move_to(struct Out *o, long x, long y) { put_xy(o, x, y); puts_(o, " m\n"); }
static void line_to(struct Out *o, long x, long y) { put_xy(o, x, y); puts_(o, " l\n"); }

static void curve_to(struct Out *o, long x1, long y1, long x2, long y2, long x3, long y3)
{
    put_xy(o, x1, y1); putc_(o, ' ');
    put_xy(o, x2, y2); putc_(o, ' ');
    put_xy(o, x3, y3); puts_(o, " c\n");
}

/* closed path of a rectangle with rounded corners, 1/100 pt; r = 0: square */
static void rrect_path(struct Out *o, long x0, long y0, long x1, long y1, long r)
{
    long k = r * 55 / 100;          /* bezier approximation of a quarter circle */
    move_to(o, x0 + r, y0);
    line_to(o, x1 - r, y0);
    if (r) curve_to(o, x1 - r + k, y0, x1, y0 + r - k, x1, y0 + r);
    line_to(o, x1, y1 - r);
    if (r) curve_to(o, x1, y1 - r + k, x1 - r + k, y1, x1 - r, y1);
    line_to(o, x0 + r, y1);
    if (r) curve_to(o, x0 + r - k, y1, x0, y1 - r + k, x0, y1 - r);
    line_to(o, x0, y0 + r);
    if (r) curve_to(o, x0, y0 + r - k, x0 + r - k, y0, x0 + r, y0);
    puts_(o, "h\n");
}

/* filled circle: centre and radius in 1/100 pt */
static void circle(struct Ctx *c, long cx, long cy, long r, unsigned long rgb)
{
    fill_colour(c, rgb);
    rrect_path(&c->out, cx - r, cy - r, cx + r, cy + r, r);
    puts_(&c->out, "f\n");
}

static void set_font(struct Ctx *c, int font, long size10)
{
    struct Out *o = &c->out;
    if (font == c->font && size10 == c->fsize) return;
    c->font = font;
    c->fsize = size10;
    puts_(o, "/F");
    put_long(o, font + 1);
    putc_(o, ' ');
    put_fix(o, size10 * 10);
    puts_(o, " Tf\n");
}

/*****************************************************************************/
/* items                                                                     */

static unsigned long colour(struct Ctx *c, unsigned long col)
{
    if (col == COL_LINK) return c->doc->link != COL_NONE ? c->doc->link : DEF_LINK;
    if (COL_IS_RGB(col)) return col;
    if (col == (COL_PEN | PEN_BACKGROUND)) return 0xFFFFFF;
    return 0x000000;
}

static int font_id(struct Ctx *c, struct HItem *it)
{
    return family(c, it->font) * 4 + ((it->style & HS_BOLD) ? 2 : 0) + ((it->style & HS_ITALIC) ? 1 : 0);
}

static void draw_text(struct Ctx *c, long i)
{
    struct HItem *it = &c->lay->items[i];
    struct Out *o = &c->out;
    long size10 = c->size10[it->font], bx = X(c, it->x), by = Y(c, it->y + it->base);
    unsigned long rgb = colour(c, it->color);
    long thick = size10 / 2;        /* 0.05 em in 1/100 pt */

    if (thick < 40) thick = 40;
    fill_colour(c, rgb);
    set_font(c, font_id(c, it), size10);
    puts_(o, "BT ");
    put_xy(o, bx, by);
    puts_(o, " Td ");
    put_string(o, it->s, it->len);
    puts_(o, " Tj ET\n");
    if (it->style & HS_UNDERLINED) {
        long ex = X(c, html_underline_end(c->lay, i));
        fill_pt(c, bx, by - size10 - thick, ex - bx, thick, rgb);
    }
    if (it->style & HS_STRIKE)
        fill_pt(c, bx, by + size10 * 28 / 10, X(c, it->x + it->w) - bx, thick, rgb);
}

static void draw_bullet(struct Ctx *c, struct HItem *it)
{
    long s = it->w * 75, x = X(c, it->x), y = Y(c, it->y + it->w);
    unsigned long rgb = colour(c, it->color);
    struct Out *o = &c->out;

    if (it->style == BUL_SQUARE) fill_pt(c, x, y, s, s, rgb);
    else if (it->style == BUL_CIRCLE) {
        stroke_colour(c, rgb);
        puts_(o, "0.6 w\n");
        rrect_path(o, x + 30, y + 30, x + s - 30, y + s - 30, s / 2 - 30);
        puts_(o, "S\n");
    } else circle(c, x + s / 2, y + s / 2, s / 2, rgb);
}

/* checkbox and radio button, the shapes of html_check.c */
static void draw_check(struct Ctx *c, struct HItem *it)
{
    struct Out *o = &c->out;
    long s = (it->h < it->w ? it->h : it->w) * 75;
    long x0 = X(c, it->x), y1 = Y(c, it->y), y0 = y1 - s, x1 = x0 + s;
    long bw = s >= 24 * 75 ? 150 : 75;
    int checked = (it->style & CK_CHECKED) != 0;

    if (it->style & CK_RADIO) {
        circle(c, x0 + s / 2, y0 + s / 2, s / 2, checked ? C_ACCENT : C_BORDER);
        circle(c, x0 + s / 2, y0 + s / 2, s / 2 - bw, C_FIELD);
        if (checked) circle(c, x0 + s / 2, y0 + s / 2, s * 27 / 100, C_ACCENT);
        return;
    }
    {
        long r = s / 5;
        fill_colour(c, checked ? C_ACCENT : C_BORDER);
        rrect_path(o, x0, y0, x1, y1, r);
        puts_(o, "f\n");
        if (!checked) {
            fill_colour(c, C_FIELD);
            rrect_path(o, x0 + bw, y0 + bw, x1 - bw, y1 - bw, r > bw ? r - bw : 0);
            puts_(o, "f\n");
        } else {
            stroke_colour(c, C_MARK);
            put_fix(o, s * 2 / 15);
            puts_(o, " w 1 J 1 j\n");
            move_to(o, x0 + s * 25 / 100, y1 - s * 52 / 100);
            line_to(o, x0 + s * 43 / 100, y1 - s * 71 / 100);
            line_to(o, x0 + s * 77 / 100, y1 - s * 31 / 100);
            puts_(o, "S 0 J 0 j\n");
        }
    }
}

static void put_hex(struct Out *w, unsigned v)
{
    static const char hex[] = "0123456789ABCDEF";
    putc_(w, hex[v >> 4 & 15]);
    putc_(w, hex[v & 15]);
}

/*****************************************************************************/
/* compression: LZW (PDF and PostScript level 2), ASCII85 (PostScript)       */

#define LZW_HSIZE 5021              /* prime, > 4096 / 0.8 */

struct Enc {
    struct Out   *w;
    int           a85;              /* bytes go through ASCII85 */
    unsigned long tuple;
    int           tn, col;
    long         *key;
    short        *code;
    unsigned long bits;
    int           nbits, width;
    long          next, prefix;
};

static void a85_group(struct Enc *e, int n)
{
    char d[5];
    unsigned long t = e->tuple & 0xFFFFFFFFUL;
    int i;

    if (n == 4 && !t) {
        putc_(e->w, 'z');
        e->col++;
    } else {
        for (i = 4; i >= 0; i--) { d[i] = (char)('!' + t % 85); t /= 85; }
        for (i = 0; i <= n; i++) putc_(e->w, d[i]);
        e->col += n + 1;
    }
    if (e->col >= 75) { putc_(e->w, '\n'); e->col = 0; }
    e->tuple = 0;
    e->tn = 0;
}

static void enc_byte(struct Enc *e, unsigned c)
{
    if (!e->a85) { putc_(e->w, (char)c); return; }
    e->tuple = (e->tuple << 8 | (c & 255)) & 0xFFFFFFFFUL;
    if (++e->tn == 4) a85_group(e, 4);
}

static void a85_end(struct Enc *e)
{
    int n = e->tn;
    if (n) {
        e->tuple = (e->tuple << (8 * (4 - n))) & 0xFFFFFFFFUL;
        a85_group(e, n);
    }
    puts_(e->w, "~>\n");
}

static void lzw_put(struct Enc *e, long code)
{
    e->bits = (e->bits << e->width | (unsigned long)code) & 0xFFFFFFUL;
    e->nbits += e->width;
    while (e->nbits >= 8) {
        e->nbits -= 8;
        enc_byte(e, (unsigned)(e->bits >> e->nbits) & 255);
    }
}

static void lzw_reset(struct Enc *e)
{
    long i;
    for (i = 0; i < LZW_HSIZE; i++) e->key[i] = -1;
    e->width = 9;
    e->next = 258;
}

/* the encoder of LZWDecode with EarlyChange 1: the decoder adds a code
 * one step later than the encoder, so the width grows when the next
 * code no longer fits                                                  */
static int lzw_begin(struct Ctx *c, struct Enc *e, int a85)
{
    if (!c->lzwkey) {
        c->lzwkey = hsys_alloc(c->out.pool, LZW_HSIZE * sizeof(long));
        c->lzwcode = hsys_alloc(c->out.pool, LZW_HSIZE * sizeof(short));
        if (!c->lzwkey || !c->lzwcode) { c->lzwkey = 0; c->out.oom = 1; return 0; }
    }
    e->w = &c->out;
    e->a85 = a85;
    e->tuple = 0;
    e->tn = e->col = 0;
    e->key = c->lzwkey;
    e->code = c->lzwcode;
    e->bits = 0;
    e->nbits = 0;
    e->prefix = -1;
    lzw_reset(e);
    lzw_put(e, 256);                /* clear table */
    return 1;
}

static void lzw_byte(struct Enc *e, unsigned ch)
{
    long k, h;

    ch &= 255;
    if (e->prefix < 0) { e->prefix = ch; return; }
    k = e->prefix << 8 | ch;
    h = (long)((ch << 4) ^ (unsigned long)e->prefix) % LZW_HSIZE;
    while (e->key[h] >= 0) {
        if (e->key[h] == k) { e->prefix = e->code[h]; return; }
        if (++h == LZW_HSIZE) h = 0;
    }
    lzw_put(e, e->prefix);
    e->key[h] = k;
    e->code[h] = (short)e->next++;
    if (e->next >= 4094) {          /* table full: start again */
        lzw_put(e, 256);
        lzw_reset(e);
    } else if (e->next >= (1L << e->width) && e->width < 12) e->width++;
    e->prefix = ch;
}

static void lzw_end(struct Enc *e)
{
    if (e->prefix >= 0) {
        lzw_put(e, e->prefix);
        if (++e->next >= (1L << e->width) && e->width < 12) e->width++;
    }
    lzw_put(e, 257);                /* end of data */
    if (e->nbits) enc_byte(e, (unsigned)(e->bits << (8 - e->nbits)) & 255);
    if (e->a85) a85_end(e);
}

/* JPEG files written as they are: size and components from the frame
 * header. 1 or 3 components (grey, YCbCr), 8 bit, baseline or
 * extended (SOF0/1), progressive (SOF2) only if allowed.             */
static int jpeg_info(const unsigned char *d, long len, int progressive_ok, long *w, long *h, int *comps)
{
    long i = 2, seg;

    if (!d || len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    while (i + 4 <= len) {
        unsigned m;
        if (d[i] != 0xFF) return 0;
        m = d[i + 1];
        if (m == 0xFF) { i++; continue; }           /* fill byte */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        seg = (long)d[i + 2] << 8 | d[i + 3];
        if (seg < 2 || i + 2 + seg > len) return 0;
        if (m == 0xC0 || m == 0xC1 || (m == 0xC2 && progressive_ok)) {
            if (seg < 8 || d[i + 4] != 8) return 0;
            *h = (long)d[i + 5] << 8 | d[i + 6];
            *w = (long)d[i + 7] << 8 | d[i + 8];
            *comps = d[i + 9];
            return *w > 0 && *h > 0 && (*comps == 1 || *comps == 3);
        }
        if ((m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) || m == 0xDA || m == 0xD9)
            return 0;               /* other coding, or no frame before the scan */
        i += 2 + seg;
    }
    return 0;
}

/* the picture of an item from the callback; FALSE if not there */
static int get_image(struct Ctx *c, void *img, struct HPrintImage *pi)
{
    const struct HPrintOpts *o = c->opt;

    pi->w = pi->h = 0;
    pi->argb = 0;
    pi->priv = 0;
    pi->jpeg = 0;
    pi->jpeglen = 0;
    if (!img || !o->image || !o->image(o->user, img, pi)) return 0;
    if (pi->w <= 0 || pi->h <= 0 || !pi->argb) {
        if (o->image_free) o->image_free(o->user, pi);
        return 0;
    }
    return 1;
}

/* a pixel composed onto white, byte k (0 = red) */
static unsigned on_white(unsigned long v, int k)
{
    unsigned a = (unsigned)(v >> 24) & 255, white = 255 * (255 - a) + 127;
    return (((unsigned)(v >> (16 - 8 * k)) & 255) * a + white) / 255;
}

/* the largest box of a picture in the layout (pixels of 1/96 inch) */
static void image_box(struct Ctx *c, void *img, long *bw, long *bh)
{
    long i;
    *bw = *bh = 0;
    for (i = 0; i < c->lay->nitems; i++) {
        struct HItem *it = &c->lay->items[i];
        if (it->type == IT_IMAGE && it->img == img) {
            if (it->w > *bw) *bw = it->w;
            if (it->h > *bh) *bh = it->h;
        }
    }
}

/* Pictures with more pixels per inch on paper than HPrintOpts.image_dpi
 * are scaled down to it (area average, alpha weighted); the copy comes
 * from the pool and is freed with shrink_free(). FALSE: as it is.      */
struct Small { unsigned long *pix; long size; };

static int shrink_image(struct Ctx *c, struct HPrintImage *pi, long bw, long bh, struct Small *sm)
{
    long dpi = c->opt->image_dpi, tw, th, nw, nh, x, y;
    unsigned long *d;

    sm->pix = 0;
    sm->size = 0;
    if (dpi <= 0 || bw <= 0 || bh <= 0) return 0;
    tw = (bw * dpi + 95) / 96;
    th = (bh * dpi + 95) / 96;
    nw = pi->w > tw ? tw : pi->w;
    nh = pi->h > th ? th : pi->h;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == pi->w && nh == pi->h) return 0;
    sm->size = nw * nh * (long)sizeof(unsigned long);
    if (!(sm->pix = hsys_alloc(c->out.pool, sm->size))) return 0;
    d = sm->pix;
    for (y = 0; y < nh; y++) {
        long y0 = y * pi->h / nh, y1 = (y + 1) * pi->h / nh, sy;
        if (y1 <= y0) y1 = y0 + 1;
        sy = (y1 - y0) / 32 + 1;            /* at most 32 x 32 samples: sums stay in 32 bit */
        for (x = 0; x < nw; x++) {
            long x0 = x * pi->w / nw, x1 = (x + 1) * pi->w / nw, sx = 0, xx, yy;
            unsigned long n = 0, as = 0, rs = 0, gs = 0, bs = 0;
            if (x1 <= x0) x1 = x0 + 1;
            sx = (x1 - x0) / 32 + 1;
            for (yy = y0; yy < y1; yy += sy)
                for (xx = x0; xx < x1; xx += sx) {
                    unsigned long v = pi->argb[yy * pi->w + xx], a = v >> 24 & 255;
                    as += a;
                    rs += (v >> 16 & 255) * a;
                    gs += (v >> 8 & 255) * a;
                    bs += (v & 255) * a;
                    n++;
                }
            *d++ = as ? (as / n) << 24 | (rs / as) << 16 | (gs / as) << 8 | bs / as : 0;
        }
    }
    pi->argb = sm->pix;
    pi->w = nw;
    pi->h = nh;
    return 1;
}

static void shrink_free(struct Ctx *c, struct Small *sm)
{
    if (sm->pix) hsys_free(c->out.pool, sm->pix, sm->size);
    sm->pix = 0;
}

/* a JPEG file is passed on as it is unless it has more pixels than the
 * limit of HPrintOpts.image_dpi allows for the box                     */
static int jpeg_fits(struct Ctx *c, long jw, long jh, long bw, long bh)
{
    long dpi = c->opt->image_dpi;
    return dpi <= 0 || bw <= 0 || bh <= 0 || (jw <= (bw * dpi + 95) / 96 && jh <= (bh * dpi + 95) / 96);
}

/* index of a picture with a PDF image object, -1 if none */
static long pdf_image(struct Ctx *c, void *img)
{
    long k;
    for (k = 0; k < c->nimgs; k++)
        if (c->imgs[k] == img) return k;
    return -1;
}

/* a picture in the box of the item; FALSE if its pixels are not there */
static int draw_image(struct Ctx *c, struct HItem *it)
{
    const struct HPrintOpts *o = c->opt;
    struct Out *w = &c->out;
    struct HPrintImage pi;
    struct Enc e;
    struct Small sm;
    long k = -1, x, y, jw, jh;
    int comps, usejpeg;

    if (!it->img || !o->image) return 0;
    if (o->format == HP_PDF && (k = pdf_image(c, it->img)) < 0) return 0;
    if (o->format != HP_PDF && !get_image(c, it->img, &pi)) return 0;
    sm.pix = 0;
    puts_(w, "q ");
    put_fix(w, it->w * 75); puts_(w, " 0 0 ");
    put_fix(w, it->h * 75); putc_(w, ' ');
    put_xy(w, X(c, it->x), Y(c, it->y + it->h));
    puts_(w, " cm\n");
    if (o->format == HP_PDF) {
        puts_(w, "/Im");
        put_long(w, k + 1);
        puts_(w, " Do Q\n");
        return 1;
    }
    usejpeg = o->ps_level != 1 && jpeg_info(pi.jpeg, pi.jpeglen, 0, &jw, &jh, &comps) &&
              jpeg_fits(c, jw, jh, it->w, it->h);
    if (!usejpeg) shrink_image(c, &pi, it->w, it->h, &sm);
    if (usejpeg) {
        /* level 2: the JPEG file as it is */
        puts_(w, "/HPa currentfile /ASCII85Decode filter def /HPf HPa /DCTDecode filter def\n");
        put_long(w, jw); putc_(w, ' '); put_long(w, jh);
        puts_(w, " 8 ["); put_long(w, jw); puts_(w, " 0 0 "); put_long(w, -jh);
        puts_(w, " 0 "); put_long(w, jh);
        puts_(w, comps == 3 ? "] HPf false 3 colorimage\n" : "] HPf image\n");
        e.w = w;
        e.a85 = 1;
        e.tuple = 0;
        e.tn = e.col = 0;
        for (x = 0; x < pi.jpeglen; x++) enc_byte(&e, pi.jpeg[x]);
        a85_end(&e);
        puts_(w, "HPf flushfile HPa flushfile\n");
    } else if (o->ps_level != 1) {
        /* level 2: LZW in ASCII85, composed onto white */
        puts_(w, "/HPa currentfile /ASCII85Decode filter def /HPf HPa /LZWDecode filter def\n");
        put_long(w, pi.w); putc_(w, ' '); put_long(w, pi.h);
        puts_(w, " 8 ["); put_long(w, pi.w); puts_(w, " 0 0 "); put_long(w, -pi.h);
        puts_(w, " 0 "); put_long(w, pi.h);
        puts_(w, "] HPf false 3 colorimage\n");
        if (lzw_begin(c, &e, 1)) {
            for (y = 0; y < pi.w * pi.h; y++)
                for (x = 0; x < 3; x++) lzw_byte(&e, on_white(pi.argb[y], (int)x));
            lzw_end(&e);
        }
        puts_(w, "HPf flushfile HPa flushfile\n");
    } else {
        /* level 1: the pixels in hex, composed onto white */
        puts_(w, "/HProw "); put_long(w, pi.w * 3); puts_(w, " string def\n");
        put_long(w, pi.w); putc_(w, ' '); put_long(w, pi.h);
        puts_(w, " 8 ["); put_long(w, pi.w); puts_(w, " 0 0 "); put_long(w, -pi.h);
        puts_(w, " 0 "); put_long(w, pi.h);
        puts_(w, "] {currentfile HProw readhexstring pop} false 3 colorimage\n");
        for (y = 0; y < pi.h; y++) {
            const unsigned long *p = pi.argb + y * pi.w;
            for (x = 0; x < pi.w; x++) {
                put_hex(w, on_white(p[x], 0));
                put_hex(w, on_white(p[x], 1));
                put_hex(w, on_white(p[x], 2));
                if ((x & 15) == 15) putc_(w, '\n');
            }
            putc_(w, '\n');
        }
    }
    puts_(w, "Q\n");
    shrink_free(c, &sm);
    if (o->image_free) o->image_free(o->user, &pi);
    return 1;
}

static void draw_item(struct Ctx *c, long i, const struct HPrintOpts *opt)
{
    struct HItem *it = &c->lay->items[i];

    switch (it->type) {
    case IT_TEXT:
        draw_text(c, i);
        break;
    case IT_RECT:
        if (opt->backgrounds && it->color != COL_NONE)
            fill_rect(c, it->x, it->y, it->w, it->h, colour(c, it->color));
        break;
    case IT_HRULE:
        fill_rect(c, it->x, it->y, it->w, it->h,
                  (it->style & HR_NOSHADE) ? colour(c, it->color) : C_RULE);
        break;
    case IT_IMAGE:
        if (!draw_image(c, it)) stroke_rect(c, it->x, it->y, it->w, it->h, C_RULE);
        break;
    case IT_FRAME:
        stroke_rect(c, it->x, it->y, it->w, it->h, it->color != COL_NONE ? it->color : C_RULE);
        break;
    case IT_BULLET:
        draw_bullet(c, it);
        break;
    case IT_CHECK:
        draw_check(c, it);
        break;
    }
}

/*****************************************************************************/
/* pages                                                                     */

struct Span { long a, b; int head; };    /* head: a line of a heading */

/* heap sort of the spans by their start */
static void sift(struct Span *s, long i, long n)
{
    for (;;) {
        long m = i, l = 2 * i + 1, r = l + 1;
        struct Span t;
        if (l < n && s[l].a > s[m].a) m = l;
        if (r < n && s[r].a > s[m].a) m = r;
        if (m == i) return;
        t = s[i]; s[i] = s[m]; s[m] = t;
        i = m;
    }
}

static void sort_spans(struct Span *s, long n)
{
    long i;
    for (i = n / 2 - 1; i >= 0; i--) sift(s, i, n);
    for (i = n - 1; i > 0; i--) {
        struct Span t = s[0]; s[0] = s[i]; s[i] = t;
        sift(s, 0, i);
    }
}

static int atomic(struct HItem *it)
{
    return it->type == IT_TEXT || it->type == IT_IMAGE || it->type == IT_HRULE ||
           it->type == IT_BULLET || it->type == IT_CHECK;
}

/* a heading line: bold text bigger than normal */
static int heading(struct HItem *it)
{
    return it->type == IT_TEXT && it->font < 7 && it->font % 7 + 1 > 3 && (it->style & HS_BOLD);
}

/* Page tops in layout pixels; *count gets the number of pages. The
 * spans of the items that must not be cut are merged; a page ends at
 * the start of the span that reaches over its bottom, or before a
 * heading right above it (a heading stays with its text).           */
long *html_print_paginate(struct HLayout *lay, void *pool, long height, long *count)
{
    struct Span *s;
    long n = 0, m = 0, i, top = 0, np = 0, max, *tops;

    if (!(s = hsys_alloc(pool, (lay->nitems + 1) * sizeof(*s)))) return NULL;
    for (i = 0; i < lay->nitems; i++)
        if (atomic(&lay->items[i]) && lay->items[i].h > 0) {
            s[n].a = lay->items[i].y;
            s[n].b = lay->items[i].y + lay->items[i].h;
            s[n++].head = heading(&lay->items[i]);
        }
    sort_spans(s, n);
    for (i = 0; i < n; i++) {               /* merge overlapping spans */
        if (m && s[i].a < s[m - 1].b) {
            if (s[i].b > s[m - 1].b) s[m - 1].b = s[i].b;
            s[m - 1].head |= s[i].head;
        } else s[m++] = s[i];
    }
    max = lay->height / (height > 0 ? height : 1) * 2 + m + 2;
    if (!(tops = hsys_alloc(pool, max * sizeof(long)))) return NULL;
    i = 0;
    do {
        long bottom = top + height;
        tops[np++] = top;
        while (i < m && s[i].b <= top) i++;
        {
            long j = i;
            while (j < m && s[j].b <= bottom) j++;
            /* span j reaches over the bottom: the page ends before it */
            if (j < m && s[j].a < bottom && s[j].a > top) bottom = s[j].a;
            /* the next page starts with span j: a heading before it (all
             * its lines) goes along, unless it is all there is on the page */
            if (j < m && j > i && s[j - 1].head) {
                long k = j - 1;
                while (k > i && s[k - 1].head) k--;
                if (s[k].a > top) bottom = s[k].a;
            }
        }
        top = bottom;
    } while (top < lay->height && np < max);
    *count = np;
    return tops;
}

/* footer text with %p and %n replaced */
unsigned long html_print_generation(void)
{
    static unsigned long gen = 0x80000000UL;
    if (++gen < 0x80000000UL) gen = 0x80000000UL;
    return gen;
}

long html_print_footer(char *buf, long size, const char *fmt, long page, long pages)
{
    long n = 0;
    while (*fmt && n < size - 12) {
        if (fmt[0] == '%' && (fmt[1] == 'p' || fmt[1] == 'n')) {
            char d[12];
            long v = fmt[1] == 'p' ? page : pages;
            int k = 0;
            do d[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11);
            while (k) buf[n++] = d[--k];
            fmt += 2;
        } else buf[n++] = *fmt++;
    }
    buf[n] = 0;
    return n;
}

static void draw_page(struct Ctx *c, const struct HPrintOpts *o, long top, long bottom, long page, long pages)
{
    struct HLayout *lay = c->lay;
    struct Out *w = &c->out;
    long cw = o->paper_w - o->margin[0] - o->margin[2], ch = o->paper_h - o->margin[1] - o->margin[3];
    long i;

    c->top = top;
    c->px0 = o->margin[0] * 100;
    c->py0 = (o->paper_h - o->margin[1]) * 100;
    c->fill = ~0UL;
    c->font = -1;

    puts_(w, "q\n");
    /* clip to the content area */
    put_xy(w, o->margin[0] * 100, o->margin[3] * 100);
    putc_(w, ' ');
    put_xy(w, cw * 100, ch * 100);
    puts_(w, " re W n\n");
    if (o->backgrounds && COL_IS_RGB(lay->bgcolor) && lay->bgcolor != 0xFFFFFF)
        fill_pt(c, o->margin[0] * 100, o->margin[3] * 100, cw * 100, ch * 100, lay->bgcolor);
    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        if (it->y >= bottom || it->y + it->h <= top) continue;
        /* a line on the next page is drawn there */
        if (atomic(it) && it->y < top) continue;
        if (atomic(it) && it->y + it->h > bottom && it->y > top) continue;
        draw_item(c, i, o);
    }
    puts_(w, "Q\n");

    if (o->footer && *o->footer) {
        char text[64];
        long len = html_print_footer(text, sizeof(text), o->footer, page, pages);
        long size10 = 80, tw = em_width(FAM_SANS, 0, text, len) * size10 / 100;    /* 1/100 pt */
        c->fill = ~0UL;
        c->font = -1;
        fill_colour(c, 0x000000);
        set_font(c, 0, size10);
        puts_(w, "BT ");
        put_xy(w, o->margin[0] * 100 + (cw * 100 - tw) / 2, o->margin[3] * 50 - size10 * 3);
        puts_(w, " Td ");
        put_string(w, text, len);
        puts_(w, " Tj ET\n");
    }
}

/*****************************************************************************/
/* file formats                                                              */

static const char ps_prolog[] =
    "/HPEnc 256 array def 0 1 255 {HPEnc exch /.notdef put} for\n"
    "HPEnc 32 [";

static const char ps_procs[] =
    "] putinterval\n"
    "/HPRe {findfont dup length dict begin {1 index /FID ne {def} {pop pop} ifelse} forall\n"
    " /Encoding HPEnc def currentdict end definefont pop} bind def\n"
    "/m {moveto} bind def /l {lineto} bind def /c {curveto} bind def /h {closepath} bind def\n"
    "/f {fill} bind def /S {stroke} bind def /n {newpath} bind def /W {clip} bind def\n"
    "/w {setlinewidth} bind def /J {setlinecap} bind def /j {setlinejoin} bind def\n"
    "/q {gsave} bind def /Q {grestore} bind def\n"
    "/rg {setrgbcolor} bind def /RG {setrgbcolor} bind def /cm {6 array astore concat} bind def\n"
    "/re {4 2 roll moveto 1 index 0 rlineto 0 exch rlineto neg 0 rlineto closepath} bind def\n"
    "/BT {} def /ET {} def /Td {moveto} bind def /Tj {show} bind def\n"
    "/Tf {exch findfont exch scalefont setfont} bind def\n";

static void write_ps(struct Ctx *c, const struct HPrintOpts *o, long *tops, long np, long height,
                     long first, long last)
{
    struct Out *w = &c->out;
    long p, i;

    puts_(w, "%!PS-Adobe-3.0\n%%Creator: html.gadget\n");
    if (o->ps_level != 1) puts_(w, "%%LanguageLevel: 2\n");
    if (o->title) {
        for (i = 0; o->title[i]; i++) ;
        puts_(w, "%%Title: ");
        put_string(w, o->title, i);
        putc_(w, '\n');
    }
    puts_(w, "%%BoundingBox: 0 0 ");
    put_long(w, o->paper_w); putc_(w, ' '); put_long(w, o->paper_h);
    puts_(w, "\n%%Pages: ");
    put_long(w, last - first + 1);
    puts_(w, "\n%%DocumentNeededResources: font Helvetica Helvetica-Bold Helvetica-Oblique\n"
             "%%+ font Helvetica-BoldOblique Times-Roman Times-Bold Times-Italic Times-BoldItalic\n"
             "%%+ font Courier Courier-Bold Courier-Oblique Courier-BoldOblique\n"
             "%%EndComments\n%%BeginProlog\n");
    puts_(w, ps_prolog);
    for (i = 0; i < 224; i++) {
        putc_(w, (i % 8) ? ' ' : '\n');
        putc_(w, '/');
        puts_(w, hp_glyph_names[i]);
    }
    puts_(w, ps_procs);
    for (i = 0; i < 12; i++) {
        puts_(w, "/F");
        put_long(w, i + 1);
        puts_(w, " /");
        puts_(w, font_names[i]);
        puts_(w, " HPRe\n");
    }
    puts_(w, "%%EndProlog\n%%BeginSetup\n"
             "/setpagedevice where {pop << /PageSize [");
    put_long(w, o->paper_w); putc_(w, ' '); put_long(w, o->paper_h);
    puts_(w, "] >> setpagedevice} if\n%%EndSetup\n");

    for (p = first; p <= last; p++) {
        if (o->progress && o->progress(o->user, p, np)) { w->oom = 2; return; }
        puts_(w, "%%Page: ");
        put_long(w, p); putc_(w, ' '); put_long(w, p - first + 1);
        puts_(w, "\n");
        draw_page(c, o, tops[p - 1], p < np ? tops[p] : tops[p - 1] + height, p, np);
        puts_(w, "showpage\n");
        if (w->oom) return;
    }
    puts_(w, "%%Trailer\n%%EOF\n");
}

static void pdf_obj(struct Out *w, long *offs, long num)
{
    flush(w);
    offs[num] = w->pos;
    put_long(w, num);
    puts_(w, " 0 obj\n");
}

/* the pictures of the layout, each once */
static void collect_images(struct Ctx *c)
{
    struct HLayout *lay = c->lay;
    long i, n = 0;

    for (i = 0; i < lay->nitems; i++)
        if (lay->items[i].type == IT_IMAGE && lay->items[i].img) n++;
    if (!n || !(c->imgs = hsys_alloc(c->out.pool, n * sizeof(void *)))) return;
    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        if (it->type == IT_IMAGE && it->img && pdf_image(c, it->img) < 0) c->imgs[c->nimgs++] = it->img;
    }
}

/* the stream of an image object, its length as object num + 1 */
static void pdf_stream_begin(struct Out *w, long num, long *start)
{
    puts_(w, " /Length ");
    put_long(w, num + 1);
    puts_(w, " 0 R >>\nstream\n");
    flush(w);
    *start = w->pos;
}

static void pdf_stream_end(struct Out *w, long *offs, long num, long start)
{
    long len;
    flush(w);
    len = w->pos - start;
    puts_(w, "\nendstream\nendobj\n");
    pdf_obj(w, offs, num + 1);
    put_long(w, len);
    puts_(w, "\nendobj\n");
}

/* rows with PNG predictors (per row the best of None, Sub and Up), LZW:
 * 'bpp' bytes per pixel, 3 = RGB, 1 = the alpha channel               */
static void pdf_lzw_rows(struct Ctx *c, const struct HPrintImage *pi, int bpp)
{
    struct Enc e;
    unsigned char *cur, *prev, *t;
    long rowlen = pi->w * bpp, x, y, i;

    if (!(cur = hsys_alloc(c->out.pool, 2 * rowlen)) || !lzw_begin(c, &e, 0)) { c->out.oom = 1; return; }
    prev = cur + rowlen;
    for (i = 0; i < rowlen; i++) prev[i] = 0;
    for (y = 0; y < pi->h; y++) {
        const unsigned long *p = pi->argb + y * pi->w;
        unsigned long sum_none = 0, sum_sub = 0, sum_up = 0;
        int f;
        for (x = 0; x < pi->w; x++) {
            unsigned long v = p[x];
            if (bpp == 3) {
                cur[3 * x] = (unsigned char)(v >> 16);
                cur[3 * x + 1] = (unsigned char)(v >> 8);
                cur[3 * x + 2] = (unsigned char)v;
            } else cur[x] = (unsigned char)(v >> 24);
        }
        for (i = 0; i < rowlen; i++) {
            signed char s = (signed char)(cur[i] - (i >= bpp ? cur[i - bpp] : 0));
            signed char u = (signed char)(cur[i] - prev[i]);
            signed char n = (signed char)cur[i];
            sum_sub += s < 0 ? -s : s;
            sum_up += u < 0 ? -u : u;
            sum_none += n < 0 ? -n : n;
        }
        f = sum_up <= sum_sub && sum_up <= sum_none ? 2 : sum_sub <= sum_none ? 1 : 0;
        lzw_byte(&e, (unsigned)f);
        for (i = 0; i < rowlen; i++)
            lzw_byte(&e, f == 2 ? (unsigned)(cur[i] - prev[i]) & 255 :
                         f == 1 ? (unsigned)(cur[i] - (i >= bpp ? cur[i - bpp] : 0)) & 255 : cur[i]);
        t = prev; prev = cur; cur = t;
    }
    lzw_end(&e);
}

/* Image object 'num' with its length num + 1, and its soft mask num + 2
 * (length num + 3) if the picture is not opaque. JPEG files are written
 * as they are. FALSE if the pixels are not available.                */
static int pdf_image_obj(struct Ctx *c, long *offs, long num, void *img)
{
    const struct HPrintOpts *o = c->opt;
    struct Out *w = &c->out;
    struct HPrintImage pi;
    struct Small sm;
    long i, n, start, jw, jh, bw, bh;
    int alpha = 0, comps;

    if (!get_image(c, img, &pi)) return 0;
    image_box(c, img, &bw, &bh);            /* used once: the largest place counts */
    sm.pix = 0;
    pdf_obj(w, offs, num);
    puts_(w, "<< /Type /XObject /Subtype /Image /Width ");
    if (jpeg_info(pi.jpeg, pi.jpeglen, 1, &jw, &jh, &comps) && jpeg_fits(c, jw, jh, bw, bh)) {
        put_long(w, jw);
        puts_(w, " /Height ");
        put_long(w, jh);
        puts_(w, comps == 3 ? " /ColorSpace /DeviceRGB" : " /ColorSpace /DeviceGray");
        puts_(w, " /BitsPerComponent 8 /Filter /DCTDecode");
        pdf_stream_begin(w, num, &start);
        flush(w);
        o->write(o->user, (const char *)pi.jpeg, pi.jpeglen);
        w->pos += pi.jpeglen;
        pdf_stream_end(w, offs, num, start);
    } else {
        shrink_image(c, &pi, bw, bh, &sm);
        n = pi.w * pi.h;
        for (i = 0; i < n && !alpha; i++) alpha = (pi.argb[i] >> 24 & 255) != 255;
        put_long(w, pi.w);
        puts_(w, " /Height ");
        put_long(w, pi.h);
        puts_(w, " /ColorSpace /DeviceRGB /BitsPerComponent 8");
        if (alpha) {
            c->pdf14 = 1;                   /* soft masks are PDF 1.4 */
            puts_(w, " /SMask ");
            put_long(w, num + 2);
            puts_(w, " 0 R");
        }
        puts_(w, " /Filter /LZWDecode /DecodeParms << /Predictor 15 /Colors 3 /Columns ");
        put_long(w, pi.w);
        puts_(w, " >>");
        pdf_stream_begin(w, num, &start);
        pdf_lzw_rows(c, &pi, 3);
        pdf_stream_end(w, offs, num, start);
        if (alpha) {
            pdf_obj(w, offs, num + 2);
            puts_(w, "<< /Type /XObject /Subtype /Image /Width ");
            put_long(w, pi.w);
            puts_(w, " /Height ");
            put_long(w, pi.h);
            puts_(w, " /ColorSpace /DeviceGray /BitsPerComponent 8");
            puts_(w, " /Filter /LZWDecode /DecodeParms << /Predictor 15 /Colors 1 /Columns ");
            put_long(w, pi.w);
            puts_(w, " >>");
            pdf_stream_begin(w, num + 2, &start);
            pdf_lzw_rows(c, &pi, 1);
            pdf_stream_end(w, offs, num + 2, start);
        }
    }
    shrink_free(c, &sm);
    if (o->image_free) o->image_free(o->user, &pi);
    return 1;
}

static void write_pdf(struct Ctx *c, const struct HPrintOpts *o, long *tops, long np, long height,
                      long first, long last)
{
    struct Out *w = &c->out;
    long count = last - first + 1, pg, nobj, *offs, p, i, k, xref;

    if (o->image) collect_images(c);
    pg = 17 + 4 * c->nimgs;
    nobj = pg - 1 + 2 * count;
    if (!(offs = hsys_alloc(w->pool, (nobj + 1) * sizeof(long)))) { w->oom = 1; return; }
    /* PDF 1.3; a soft mask (transparent picture) needs 1.4, which the
     * catalog says then (/Version, written after the pictures)          */
    puts_(w, "%PDF-1.3\n%\xe2\xe3\xcf\xd3\n");

    /* pictures first: those without pixels are taken out of the list */
    for (i = k = 0; i < c->nimgs; i++) {
        if (pdf_image_obj(c, offs, 17 + 4 * k, c->imgs[i])) c->imgs[k++] = c->imgs[i];
        if (w->oom) return;
    }
    c->nimgs = k;

    pdf_obj(w, offs, 1);
    puts_(w, c->pdf14 ? "<< /Type /Catalog /Version /1.4 /Pages 2 0 R >>\nendobj\n"
                      : "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    pdf_obj(w, offs, 2);
    puts_(w, "<< /Type /Pages /Count ");
    put_long(w, count);
    puts_(w, " /Kids [");
    for (i = 0; i < count; i++) {
        put_long(w, pg + 2 * i);
        puts_(w, " 0 R ");
    }
    puts_(w, "] >>\nendobj\n");
    pdf_obj(w, offs, 3);
    puts_(w, "<< /Producer (html.gadget)");
    if (o->title) {
        long len = 0;
        while (o->title[len]) len++;
        puts_(w, " /Title ");
        put_string(w, o->title, len);
    }
    puts_(w, " >>\nendobj\n");
    pdf_obj(w, offs, 4);
    puts_(w, "<< /Font <<");
    for (i = 0; i < 12; i++) {
        puts_(w, " /F");
        put_long(w, i + 1);
        putc_(w, ' ');
        put_long(w, 5 + i);
        puts_(w, " 0 R");
    }
    puts_(w, " >>");
    if (c->nimgs) {
        puts_(w, " /XObject <<");
        for (i = 0; i < c->nimgs; i++) {
            puts_(w, " /Im");
            put_long(w, i + 1);
            putc_(w, ' ');
            put_long(w, 17 + 4 * i);
            puts_(w, " 0 R");
        }
        puts_(w, " >>");
    }
    puts_(w, " >>\nendobj\n");
    for (i = 0; i < 12; i++) {
        pdf_obj(w, offs, 5 + i);
        puts_(w, "<< /Type /Font /Subtype /Type1 /BaseFont /");
        puts_(w, font_names[i]);
        puts_(w, " /Encoding /WinAnsiEncoding >>\nendobj\n");
    }

    for (p = first; p <= last; p++) {
        long num = pg + 2 * (p - first);
        if (o->progress && o->progress(o->user, p, np)) { w->oom = 2; return; }
        pdf_obj(w, offs, num);
        puts_(w, "<< /Type /Page /Parent 2 0 R /Resources 4 0 R /MediaBox [0 0 ");
        put_long(w, o->paper_w); putc_(w, ' '); put_long(w, o->paper_h);
        puts_(w, "] /Contents ");
        put_long(w, num + 1);
        puts_(w, " 0 R >>\nendobj\n");

        /* the contents into memory: the length comes first */
        flush(w);
        w->tomem = 1;
        w->memlen = 0;
        draw_page(c, o, tops[p - 1], p < np ? tops[p] : tops[p - 1] + height, p, np);
        flush(w);
        w->tomem = 0;
        if (w->oom) return;
        pdf_obj(w, offs, num + 1);
        puts_(w, "<< /Length ");
        put_long(w, w->memlen);
        puts_(w, " >>\nstream\n");
        flush(w);
        o->write(o->user, w->mem, w->memlen);
        w->pos += w->memlen;
        puts_(w, "\nendstream\nendobj\n");
    }
    flush(w);
    xref = w->pos;
    puts_(w, "xref\n0 ");
    put_long(w, nobj + 1);
    puts_(w, "\n0000000000 65535 f \n");
    for (i = 1; i <= nobj; i++) {
        char d[11];
        long v = offs[i];
        int j;
        for (j = 9; j >= 0; j--) { d[j] = (char)('0' + v % 10); v /= 10; }
        d[10] = 0;
        puts_(w, offs[i] ? d : "0000000000");
        puts_(w, offs[i] ? " 00000 n \n" : " 00001 f \n");
    }
    puts_(w, "trailer\n<< /Size ");
    put_long(w, nobj + 1);
    puts_(w, " /Root 1 0 R /Info 3 0 R >>\nstartxref\n");
    put_long(w, xref);
    puts_(w, "\n%%EOF\n");
}

/*****************************************************************************/

long html_print(struct HDoc *doc, const struct HPrintOpts *o, long *pages)
{
    struct Ctx c;
    struct HEnv env;
    long width, height, np = 0, first, last, *tops, i, result = HP_ERROR;
    long base = o->font_size > 0 ? o->font_size : 100;

    if (pages) *pages = 0;
    for (i = 0; i < (long)sizeof(c); i++) ((char *)&c)[i] = 0;
    for (i = 0; i < (long)sizeof(env); i++) ((char *)&env)[i] = 0;
    env.fit_images = o->fit_images;
    env.table_grid = o->table_grid;
    env.code_style = o->code_style;
    c.out.o = o;
    c.opt = o;
    c.doc = doc;
    c.prop = o->serif ? FAM_SERIF : FAM_SANS;
    if (!(c.out.pool = hsys_pool_create())) return HP_ERROR;

    /* fonts: sizes and line heights (1.2 em) in layout pixels */
    for (i = 0; i < HF_NUM; i++) {
        int fam = i >= 7 ? FAM_MONO : c.prop;
        long s10 = (base * level_f[i % 7] + 6) / 12, h, asc;
        if (fam == FAM_MONO) s10 = s10 * 9 / 10;  /* Courier looks bigger */
        c.size10[i] = s10;
        h = (s10 * 4 + 12) / 25;                    /* 1.2 em / 0.75 pt per pixel */
        asc = (h * 1000 - s10 * (fam_asc[fam] + fam_desc[fam]) * 4 / 30) / 2000 +
              (s10 * fam_asc[fam] * 4 + 15000) / 30000;
        env.font_height[i] = (short)h;
        env.font_baseline[i] = (short)asc;
    }
    env.user = &c;
    env.text_width = text_width_cb;
    /* its own value: cells measured for the screen (generation from 1
     * up) or another print must not be taken from the cache          */
    env.generation = html_print_generation();

    width = (o->paper_w - o->margin[0] - o->margin[2]) * 4 / 3;
    height = (o->paper_h - o->margin[1] - o->margin[3]) * 4 / 3;
    if (width < 50 || height < 50 || !(c.lay = html_layout(doc, &env, width))) goto out;
    if (!(tops = html_print_paginate(c.lay, c.out.pool, height, &np))) goto out;
    if (pages) *pages = np;

    first = o->first > 0 ? o->first : 1;
    last = o->last > 0 && o->last < np ? o->last : np;
    if (first > last) { result = 0; goto out; }

    if (o->format == HP_PDF) write_pdf(&c, o, tops, np, height, first, last);
    else write_ps(&c, o, tops, np, height, first, last);
    if (c.out.oom == 2) result = HP_ABORTED;
    else if (!c.out.oom) {
        flush(&c.out);
        result = last - first + 1;
    }
out:
    if (c.lay) html_free_layout(c.lay);
    hsys_pool_delete(c.out.pool);
    return result;
}
