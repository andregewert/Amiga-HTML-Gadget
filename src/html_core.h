/*
 * html_core.h - platform independent part of html.gadget
 *
 * Parser (HTML -> node tree) and layout engine (node tree -> positioned
 * draw items). Nothing in here touches the Amiga OS directly except
 * through the hooks in html_sys.h, so the core can be tested on a host.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#ifndef HTML_CORE_H
#define HTML_CORE_H

#include <stddef.h>

/* ---- memory / system hooks (implemented by html_class.c or the test) -- */

void *hsys_pool_create(void);
void  hsys_pool_delete(void *pool);
void *hsys_alloc(void *pool, long size);          /* zeroed memory */
void  hsys_free(void *pool, void *mem, long size);

/* ---- colors ---------------------------------------------------------- */

/* 0x00RRGGBB  : RGB colour
 * COL_PEN|n   : DrawInfo pen n (TEXTPEN, BACKGROUNDPEN...)
 * COL_LINK    : link colour (link/vlink/alink chosen at render time)
 * COL_NONE    : transparent / not set                                   */
#define COL_PEN   0x01000000UL
#define COL_LINK  0x02000000UL
#define COL_NONE  0xFF000000UL
#define COL_IS_RGB(c) (((c) & 0xFF000000UL) == 0)

/* ---- tags ------------------------------------------------------------ */

enum {
    T_TEXT = 0, T_UNKNOWN,
    T_A, T_ABBR, T_ACRONYM, T_ADDRESS, T_B, T_BASE, T_BIG, T_BLOCKQUOTE,
    T_BODY, T_BR, T_CAPTION, T_CENTER, T_CITE, T_CODE, T_COL, T_COLGROUP,
    T_DD, T_DEL, T_DFN, T_DIR, T_DIV, T_DL, T_DT, T_EM, T_FONT, T_FORM,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_HEAD, T_HR, T_HTML, T_I, T_IMG,
    T_INPUT, T_INS, T_KBD, T_LI, T_LINK, T_MENU, T_META, T_NOBR, T_NOSCRIPT,
    T_OL, T_P, T_PARAM, T_PRE, T_Q, T_S, T_SAMP, T_SCRIPT, T_SMALL, T_SPAN,
    T_STRIKE, T_STRONG, T_STYLE, T_SUB, T_SUP, T_TABLE, T_TBODY, T_TD,
    T_TFOOT, T_TH, T_THEAD, T_TITLE, T_TR, T_TT, T_U, T_UL, T_VAR,
    T_AREA, T_MAP, T_OPTION, T_SELECT, T_TEXTAREA, T_XMP, T_LISTING,
    T_PLAINTEXT, T_FRAMESET, T_FRAME, T_NOFRAMES, T_IFRAME, T_OBJECT,
    T_APPLET, T_LABEL, T_FIELDSET, T_LEGEND, T_BUTTON,
    T_MAX
};

/* ---- document tree --------------------------------------------------- */

struct HAttr {
    struct HAttr *next;
    char         *name;      /* lower case */
    char         *value;     /* entity decoded, never NULL */
};

struct HNode {
    struct HNode *parent, *first, *last, *next;
    struct HAttr *attrs;
    char         *text;      /* T_TEXT only */
    long          len;
    short         tag;
    short         link;      /* T_A with href: link index, else -1 */
    unsigned long cgen;      /* table cell min/max width cache */
    long          cmin, cmax;
    void         *img;       /* T_IMG: picture, T_TABLE/T_TD/T_TH: background
                              * picture; loaded by the class, or NULL */
    long          iw, ih;    /* its natural size */
};

struct HAnchor {
    char *name;
    long  y;
};

struct HDoc {
    void          *pool;
    struct HNode  *root;
    char          *title;
    unsigned long  bgcolor, text, link, vlink, alink;   /* COL_NONE = default */
    const char    *background;  /* <body background="..."> or NULL */
    char         **links;     /* href of each link */
    unsigned char *visited;
    long           nlinks, maxlinks;
    int            utf8;
};

struct HDoc *html_parse(const char *src, long len);
void         html_free_doc(struct HDoc *doc);
const char  *html_attr(struct HNode *n, const char *name);
unsigned long html_parse_color(const char *s);   /* COL_NONE if invalid */

/* ---- fonts ----------------------------------------------------------- */

/* font index = fixed * 7 + (size - 1); size 1..7, 3 = normal */
#define HF_NUM        14
#define HF_INDEX(fixed, size) ((fixed) * 7 + (size) - 1)

/* soft styles (same bits as graphics/text.h FSF_*) */
#define HS_UNDERLINED 1
#define HS_BOLD       2
#define HS_ITALIC     4
#define HS_STRIKE     0x80    /* drawn by us */

/* returns bit mask of all font indexes the document needs */
unsigned long html_fonts_used(struct HDoc *doc);

/* ---- layout ---------------------------------------------------------- */

enum { IT_TEXT, IT_RECT, IT_HRULE, IT_FRAME, IT_BULLET, IT_IMAGE };

/* bullet kinds (IT_BULLET, kept in 'style') */
enum { BUL_DISC, BUL_CIRCLE, BUL_SQUARE };

/* hrule / frame flags (kept in 'style') */
#define HR_NOSHADE   1
#define FR_RAISED    2

struct HItem {
    unsigned char type;
    unsigned char font;       /* IT_TEXT */
    unsigned char style;      /* HS_* for text, see above for others */
    unsigned char flags;      /* IF_* */
    short         link;       /* link index or -1 */
    short         base;       /* baseline offset from y (IT_TEXT) */
    long          x, y, w, h;
    unsigned long color;
    const char   *s;
    long          len;
    void         *img;        /* IT_IMAGE: HNode.img; IT_RECT: tiled background or NULL */
};

#define IF_NOSEL     1      /* IT_TEXT that cannot be selected (list markers, alt texts) */
#define IF_SOFT      2      /* first word of a line created by automatic wrapping */

struct HEnv {
    void *user;
    /* width in pixels of s[0..len-1] in font 'font' using soft style */
    long (*text_width)(void *user, int font, int style, const char *s, long len);
    short font_height[HF_NUM];
    short font_baseline[HF_NUM];
    long  margin;
    int   system_colors;
    unsigned long generation;   /* change when fonts change (never 0) */
};

struct HLayout {
    void           *pool;
    struct HItem   *items;
    long            nitems, maxitems;
    struct HAnchor *anchors;
    long            nanchors, maxanchors;
    long            width, height;         /* document size in pixels */
    unsigned long   bgcolor;               /* resolved page background */
};

/* lays out 'doc' for a viewport of 'width' pixels. Returns NULL on
 * out-of-memory. Free with html_free_layout().                          */
struct HLayout *html_layout(struct HDoc *doc, struct HEnv *env, long width);
void            html_free_layout(struct HLayout *lay);
long            html_find_anchor(struct HLayout *lay, const char *name); /* -1 */

/* ---- selection (html_select.c) --------------------------------------- */

/* a position is (item index, character offset); anchor and cursor are
 * kept as the user dragged them, html_sel_order() sorts them            */
struct HSel {
    long i0, c0;              /* anchor */
    long i1, c1;              /* cursor */
    int  active;
};

int  html_sel_pos(struct HLayout *lay, struct HEnv *env, long x, long y, long *item, long *ch);
void html_sel_order(const struct HSel *s, long *si, long *sc, long *ei, long *ec);
int  html_sel_empty(const struct HSel *s);
/* selected characters [*c0, *c1) of item i; returns 0 if none */
int  html_sel_part(const struct HSel *s, struct HLayout *lay, long i, long *c0, long *c1);
/* x offset of character ch inside item */
long html_char_x(struct HEnv *env, struct HItem *it, long ch);
/* right end of the highlight of item i: bridges the gap to the next word */
long html_sel_right(const struct HSel *s, struct HLayout *lay, struct HEnv *env, long i, long c1);
void html_sel_all(struct HSel *s, struct HLayout *lay);
/* selected text (Latin-1, lines separated by \n); buf may be NULL to get
 * the length only; returns the length without terminating 0            */
long html_sel_text(const struct HSel *s, struct HLayout *lay, char *buf);

/* ---- tiny libc replacements (the class links without libc) ----------- */

long  h_strlen(const char *s);
int   h_stricmp(const char *a, const char *b);
int   h_strnicmp(const char *a, const char *b, long n);
void  h_memcpy(void *d, const void *s, long n);
void  h_memset(void *d, int c, long n);
int   h_tolower(int c);
int   h_isspace(int c);
long  h_atol(const char *s);
const char *h_strchr(const char *s, int c);

#endif
