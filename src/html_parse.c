/*
 * html_parse.c - tolerant HTML 4 tokenizer and tree builder
 *
 * Produces a simplified node tree. <html>, <head> and <body> are not
 * represented as nodes; body attributes go into struct HDoc. Missing end
 * tags are implied the way HTML 4 browsers do it for the common cases
 * (p, li, dt/dd, tr, td/th, headings, option, a).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include "html_core.h"

#define MAXDEPTH 200

/* tag flags */
#define F_VOID   0x01   /* no content, no end tag */
#define F_BLOCK  0x02   /* start tag implicitly closes an open <p> */
#define F_RAW    0x04   /* raw text content up to the end tag */
#define F_SCOPE  0x08   /* end tags of unrelated elements do not cross it */

struct TagDef {
    const char   *name;
    unsigned char flags;
};

/* indexed by T_* (must stay in enum order) */
static const struct TagDef tagdefs[T_MAX] = {
    { "",           0 },                    /* T_TEXT */
    { "",           0 },                    /* T_UNKNOWN */
    { "a",          0 },
    { "abbr",       0 },
    { "acronym",    0 },
    { "address",    F_BLOCK },
    { "b",          0 },
    { "base",       F_VOID },
    { "big",        0 },
    { "blockquote", F_BLOCK },
    { "body",       0 },
    { "br",         F_VOID },
    { "caption",    F_SCOPE },
    { "center",     F_BLOCK },
    { "cite",       0 },
    { "code",       0 },
    { "col",        F_VOID },
    { "colgroup",   0 },
    { "dd",         F_BLOCK },
    { "del",        0 },
    { "dfn",        0 },
    { "dir",        F_BLOCK },
    { "div",        F_BLOCK },
    { "dl",         F_BLOCK },
    { "dt",         F_BLOCK },
    { "em",         0 },
    { "font",       0 },
    { "form",       F_BLOCK },
    { "h1",         F_BLOCK },
    { "h2",         F_BLOCK },
    { "h3",         F_BLOCK },
    { "h4",         F_BLOCK },
    { "h5",         F_BLOCK },
    { "h6",         F_BLOCK },
    { "head",       0 },
    { "hr",         F_VOID | F_BLOCK },
    { "html",       0 },
    { "i",          0 },
    { "img",        F_VOID },
    { "input",      F_VOID },
    { "ins",        0 },
    { "kbd",        0 },
    { "li",         F_BLOCK },
    { "link",       F_VOID },
    { "menu",       F_BLOCK },
    { "meta",       F_VOID },
    { "nobr",       0 },
    { "noscript",   0 },
    { "ol",         F_BLOCK },
    { "p",          F_BLOCK },
    { "param",      F_VOID },
    { "pre",        F_BLOCK },
    { "q",          0 },
    { "s",          0 },
    { "samp",       0 },
    { "script",     F_RAW },
    { "small",      0 },
    { "span",       0 },
    { "strike",     0 },
    { "strong",     0 },
    { "style",      F_RAW },
    { "sub",        0 },
    { "sup",        0 },
    { "table",      F_BLOCK | F_SCOPE },
    { "tbody",      0 },
    { "td",         F_SCOPE },
    { "tfoot",      0 },
    { "th",         F_SCOPE },
    { "thead",      0 },
    { "title",      F_RAW },
    { "tr",         0 },
    { "tt",         0 },
    { "u",          0 },
    { "ul",         F_BLOCK },
    { "var",        0 },
    { "area",       F_VOID },
    { "map",        0 },
    { "option",     0 },
    { "select",     0 },
    { "textarea",   F_RAW },
    { "xmp",        F_BLOCK | F_RAW },
    { "listing",    F_BLOCK | F_RAW },
    { "plaintext",  F_BLOCK | F_RAW },
    { "frameset",   0 },
    { "frame",      F_VOID },
    { "noframes",   0 },
    { "iframe",     0 },
    { "object",     F_SCOPE },
    { "applet",     F_SCOPE },
    { "label",      0 },
    { "fieldset",   F_BLOCK },
    { "legend",     0 },
    { "button",     F_SCOPE },
};

/* ------------------------------------------------------------------ */
/* small libc replacements                                             */

long h_strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
int  h_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int  h_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

int h_stricmp(const char *a, const char *b)
{
    int ca, cb;
    do {
        ca = h_tolower((unsigned char)*a++);
        cb = h_tolower((unsigned char)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

int h_strnicmp(const char *a, const char *b, long n)
{
    int ca = 0, cb = 0;
    while (n-- > 0) {
        ca = h_tolower((unsigned char)*a++);
        cb = h_tolower((unsigned char)*b++);
        if (!ca || ca != cb) break;
    }
    return ca - cb;
}

void h_memcpy(void *d, const void *s, long n)
{
    char *dd = d; const char *ss = s;
    while (n-- > 0) *dd++ = *ss++;
}

void h_memset(void *d, int c, long n)
{
    char *dd = d;
    while (n-- > 0) *dd++ = (char)c;
}

const char *h_strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return s;
    return 0;
}

long h_atol(const char *s)
{
    long v = 0; int neg = 0;
    while (h_isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_alnum(int c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = h_tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* ------------------------------------------------------------------ */
/* character set handling                                              */

/* Latin-1 replacement for a Unicode code point. Writes 0..3 chars. */
static int put_codepoint(unsigned long cp, char *out)
{
    static const struct { unsigned short cp; char s[4]; } map[] = {
        { 0x0152, "OE" }, { 0x0153, "oe" }, { 0x0160, "S" },  { 0x0161, "s" },
        { 0x0178, "Y" },  { 0x0192, "f" },  { 0x02C6, "^" },  { 0x02DC, "~" },
        { 0x2002, " " },  { 0x2003, " " },  { 0x2009, " " },  { 0x200B, "" },
        { 0x200C, "" },   { 0x200D, "" },   { 0x2010, "-" },  { 0x2011, "-" },
        { 0x2013, "-" },  { 0x2014, "-" },  { 0x2018, "'" },  { 0x2019, "'" },
        { 0x201A, "'" },  { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" },
        { 0x2020, "+" },  { 0x2021, "+" },  { 0x2022, "\xB7" }, { 0x2026, "..." },
        { 0x2030, "%o" }, { 0x2039, "<" },  { 0x203A, ">" },  { 0x20AC, "EUR" },
        { 0x2122, "TM" }, { 0x2190, "<-" }, { 0x2192, "->" }, { 0x2212, "-" },
        { 0xFEFF, "" },
    };
    int i, n;

    if (cp == 0xAD) return 0;                      /* soft hyphen */
    if (cp < 256) {
        if (cp < 32 && cp != '\n' && cp != '\t') cp = ' ';
        *out = (char)cp;
        return 1;
    }
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (map[i].cp == cp) {
            for (n = 0; map[i].s[n]; n++) out[n] = map[i].s[n];
            return n;
        }
    }
    *out = '?';
    return 1;
}

/* returns length of a valid UTF-8 sequence at p (2..4), 0 if invalid */
static int utf8_seq(const unsigned char *p, const unsigned char *end, unsigned long *cp)
{
    int n, i;
    unsigned long c = p[0];

    if (c >= 0xC2 && c <= 0xDF) { n = 2; c &= 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; c &= 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; c &= 0x07; }
    else return 0;
    if (end - p < n) return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        c = (c << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && c < 0x800) || (n == 4 && c < 0x10000)) return 0;
    *cp = c;
    return n;
}

/* Copies the source, removes CRs and converts UTF-8 if the text is
 * valid UTF-8 containing at least one multibyte sequence.            */
static char *prepare_source(void *pool, const char *src, long len, long *outlen, int *utf8)
{
    const unsigned char *p = (const unsigned char *)src, *end = p + len;
    unsigned long cp;
    char *buf, *o;
    int multi = 0, valid = 1, n;

    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { multi = 1; p += 3; }
    {
        const unsigned char *q = p;
        while (q < end) {
            if (*q < 0x80) { q++; continue; }
            if (!(n = utf8_seq(q, end, &cp))) { valid = 0; break; }
            multi = 1;
            q += n;
        }
    }
    *utf8 = valid && multi;

    if (!(buf = hsys_alloc(pool, (end - p) + 1))) return 0;
    o = buf;
    while (p < end) {
        if (*p == '\r') {
            *o++ = '\n';
            p++;
            if (p < end && *p == '\n') p++;
        } else if (*utf8 && *p >= 0x80 && (n = utf8_seq(p, end, &cp))) {
            o += put_codepoint(cp, o);
            p += n;
        } else if (*p == 0) {
            *o++ = ' '; p++;
        } else {
            *o++ = (char)*p++;
        }
    }
    *o = 0;
    *outlen = o - buf;
    return buf;
}

/* ------------------------------------------------------------------ */
/* entities                                                            */

static const char *const latin1_names[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect",
    "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide",
    "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};

static const struct { const char *name; unsigned short cp; } other_entities[] = {
    { "quot", 34 }, { "amp", 38 }, { "lt", 60 }, { "gt", 62 }, { "apos", 39 },
    { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 }, { "scaron", 0x161 },
    { "Yuml", 0x178 }, { "fnof", 0x192 }, { "circ", 0x2C6 }, { "tilde", 0x2DC },
    { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200C },
    { "zwj", 0x200D }, { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "sbquo", 0x201A }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "bdquo", 0x201E }, { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 },
    { "hellip", 0x2026 }, { "permil", 0x2030 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A },
    { "euro", 0x20AC }, { "trade", 0x2122 }, { "larr", 0x2190 }, { "rarr", 0x2192 },
    { "minus", 0x2212 },
};

static int name_eq(const char *name, const char *s, long n)
{
    long i;
    for (i = 0; i < n; i++) if (name[i] != s[i]) return 0;
    return name[n] == 0;
}

/* Tries to decode an entity at s ('&' already consumed, s points after
 * it). Returns number of source chars consumed (0 = not an entity) and
 * writes the replacement to out / *outn.                               */
static long decode_entity(const char *s, const char *end, char *out, int *outn)
{
    const char *p = s;
    unsigned long cp = 0;
    int i, found = 0;

    if (p < end && *p == '#') {
        int digits = 0;
        p++;
        if (p < end && (*p == 'x' || *p == 'X')) {
            p++;
            while (p < end && hexval((unsigned char)*p) >= 0) {
                if (cp < 0x110000) cp = cp * 16 + hexval((unsigned char)*p);
                p++; digits++;
            }
        } else {
            while (p < end && *p >= '0' && *p <= '9') {
                if (cp < 0x110000) cp = cp * 10 + (*p - '0');
                p++; digits++;
            }
        }
        if (!digits) return 0;
        /* Windows-1252 range commonly used in old pages */
        if (cp >= 0x80 && cp <= 0x9F) {
            static const unsigned short cp1252[32] = {
                0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0, 0,
                0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0, 0x0178 };
            cp = cp1252[cp - 0x80] ? cp1252[cp - 0x80] : '?';
        }
        found = 1;
    } else {
        const char *n0 = p;
        long nl;
        while (p < end && is_alnum((unsigned char)*p) && p - n0 < 10) p++;
        nl = p - n0;
        if (!nl) return 0;
        for (i = 0; i < 96 && !found; i++)
            if (name_eq(latin1_names[i], n0, nl)) { cp = 160 + i; found = 1; }
        for (i = 0; i < (int)(sizeof(other_entities) / sizeof(other_entities[0])) && !found; i++)
            if (name_eq(other_entities[i].name, n0, nl)) { cp = other_entities[i].cp; found = 1; }
        if (!found) return 0;
    }
    if (p < end && *p == ';') p++;
    *outn = put_codepoint(cp, out);
    return p - s;
}

/* decodes entities of s[0..len-1] into a new pool string */
static char *decode_text(void *pool, const char *s, long len, long *outlen, int decode)
{
    char *buf = hsys_alloc(pool, len + 1), *o;
    const char *end = s + len;
    int n;
    long used;

    if (!buf) return 0;
    o = buf;
    while (s < end) {
        if (decode && *s == '&' && (used = decode_entity(s + 1, end, o, &n))) {
            o += n;
            s += used + 1;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
    if (outlen) *outlen = o - buf;
    return buf;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */

unsigned long html_parse_color(const char *s)
{
    static const struct { const char *name; unsigned long rgb; } names[] = {
        { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 },
        { "grey", 0x808080 },  { "white", 0xFFFFFF },  { "maroon", 0x800000 },
        { "red", 0xFF0000 },   { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
        { "green", 0x008000 }, { "lime", 0x00FF00 },   { "olive", 0x808000 },
        { "yellow", 0xFFFF00 },{ "navy", 0x000080 },   { "blue", 0x0000FF },
        { "teal", 0x008080 },  { "aqua", 0x00FFFF },   { "orange", 0xFFA500 },
        { "magenta", 0xFF00FF }, { "cyan", 0x00FFFF }, { "brown", 0xA52A2A },
        { "darkblue", 0x00008B }, { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 },
        { "lightgrey", 0xD3D3D3 }, { "lightgray", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 },
        { "darkgrey", 0xA9A9A9 }, { "gold", 0xFFD700 }, { "pink", 0xFFC0CB },
    };
    unsigned long v = 0;
    long len, i;
    const char *p;

    if (!s) return COL_NONE;
    while (h_isspace((unsigned char)*s)) s++;
    for (i = 0; i < (long)(sizeof(names) / sizeof(names[0])); i++)
        if (!h_stricmp(names[i].name, s)) return names[i].rgb;
    if (*s == '#') s++;
    for (p = s; hexval((unsigned char)*p) >= 0; p++) ;
    len = p - s;
    if (len == 3) {
        for (i = 0; i < 3; i++) v = (v << 8) | (hexval((unsigned char)s[i]) * 17);
        return v;
    }
    if (len >= 6) {
        for (i = 0; i < 6; i++) v = (v << 4) | hexval((unsigned char)s[i]);
        return v;
    }
    return COL_NONE;
}

/* ------------------------------------------------------------------ */
/* tree building                                                       */

struct PState {
    struct HDoc  *doc;
    struct HNode *stack[MAXDEPTH];
    int           sp;
    int           skip_nl;      /* drop the newline following <pre> */
    int           oom;
};

static int lookup_tag(const char *name, long len)
{
    int i;
    for (i = T_A; i < T_MAX; i++)
        if (h_strnicmp(tagdefs[i].name, name, len) == 0 && tagdefs[i].name[len] == 0)
            return i;
    return T_UNKNOWN;
}

static struct HNode *new_node(struct PState *ps, int tag)
{
    struct HNode *n = hsys_alloc(ps->doc->pool, sizeof(*n));
    struct HNode *cur = ps->stack[ps->sp - 1];

    if (!n) { ps->oom = 1; return 0; }
    n->tag = tag;
    n->link = -1;
    n->parent = cur;
    if (cur->last) cur->last->next = n; else cur->first = n;
    cur->last = n;
    return n;
}

const char *html_attr(struct HNode *n, const char *name)
{
    struct HAttr *a;
    for (a = n->attrs; a; a = a->next)
        if (!h_stricmp(a->name, name)) return a->value;
    return 0;
}

static int is_heading(int t) { return t >= T_H1 && t <= T_H6; }

/* closes elements up to and including the topmost one for which 'match'
 * is true, but does not cross an element for which 'stop' is true.     */
static int close_to(struct PState *ps, int (*match)(int), int (*stop)(int))
{
    int i;
    for (i = ps->sp - 1; i > 0; i--) {
        int t = ps->stack[i]->tag;
        if (match(t)) { ps->sp = i; return 1; }
        if (stop && stop(t)) return 0;
    }
    return 0;
}

static int m_p(int t)       { return t == T_P; }
static int m_li(int t)      { return t == T_LI; }
static int m_dtdd(int t)    { return t == T_DT || t == T_DD; }
static int m_cell(int t)    { return t == T_TD || t == T_TH; }
static int m_tr(int t)      { return t == T_TR; }
static int m_caption(int t) { return t == T_CAPTION; }
static int m_sect(int t)    { return t == T_THEAD || t == T_TBODY || t == T_TFOOT; }
static int m_heading(int t) { return is_heading(t); }
static int m_option(int t)  { return t == T_OPTION; }
static int m_a(int t)       { return t == T_A; }

static int s_scope(int t)   { return (tagdefs[t].flags & F_SCOPE) != 0; }
static int s_list(int t)    { return t == T_UL || t == T_OL || t == T_MENU || t == T_DIR || s_scope(t); }
static int s_dl(int t)      { return t == T_DL || s_scope(t); }
static int s_row(int t)     { return t == T_TR || t == T_TABLE; }
static int s_table(int t)   { return t == T_TABLE || m_sect(t); }
static int s_tableonly(int t) { return t == T_TABLE; }
static int s_block(int t)   { return s_scope(t) || (tagdefs[t].flags & F_BLOCK && t != T_P && !is_heading(t)); }

static void add_link(struct PState *ps, struct HNode *n, const char *href)
{
    struct HDoc *doc = ps->doc;

    if (doc->nlinks >= 0x7FFF) return;
    if (doc->nlinks == doc->maxlinks) {
        long nm = doc->maxlinks ? doc->maxlinks * 2 : 32;
        char **nl = hsys_alloc(doc->pool, nm * sizeof(char *));
        unsigned char *nv = hsys_alloc(doc->pool, nm);
        if (!nl || !nv) { ps->oom = 1; return; }
        if (doc->links) {
            h_memcpy(nl, doc->links, doc->nlinks * sizeof(char *));
            hsys_free(doc->pool, doc->links, doc->maxlinks * sizeof(char *));
            hsys_free(doc->pool, doc->visited, doc->maxlinks);
        }
        doc->links = nl;
        doc->visited = nv;
        doc->maxlinks = nm;
    }
    n->link = (short)doc->nlinks;
    doc->links[doc->nlinks++] = (char *)href;
}

static void push(struct PState *ps, struct HNode *n)
{
    if (ps->sp < MAXDEPTH) ps->stack[ps->sp++] = n;
}

static void open_tag(struct PState *ps, int tag, struct HAttr *attrs)
{
    struct HNode *n;
    int cur;

    if (tag == T_HTML || tag == T_HEAD) return;
    if (tag == T_BODY) {
        struct HNode tmp;
        tmp.attrs = attrs;
        ps->doc->bgcolor = html_parse_color(html_attr(&tmp, "bgcolor"));
        ps->doc->text    = html_parse_color(html_attr(&tmp, "text"));
        ps->doc->link    = html_parse_color(html_attr(&tmp, "link"));
        ps->doc->vlink   = html_parse_color(html_attr(&tmp, "vlink"));
        ps->doc->alink   = html_parse_color(html_attr(&tmp, "alink"));
        ps->doc->background = html_attr(&tmp, "background");
        return;
    }

    if (tagdefs[tag].flags & F_BLOCK) close_to(ps, m_p, s_block);

    switch (tag) {
    case T_LI:
        close_to(ps, m_li, s_list);
        break;
    case T_DT: case T_DD:
        close_to(ps, m_dtdd, s_dl);
        break;
    case T_TR:
        close_to(ps, m_tr, s_table);
        close_to(ps, m_caption, s_tableonly);
        break;
    case T_THEAD: case T_TBODY: case T_TFOOT:
        close_to(ps, m_tr, s_table);
        close_to(ps, m_sect, s_tableonly);
        close_to(ps, m_caption, s_tableonly);
        break;
    case T_TD: case T_TH:
        close_to(ps, m_cell, s_row);
        cur = ps->stack[ps->sp - 1]->tag;
        if (cur == T_TABLE || m_sect(cur)) {      /* implied <tr> */
            if (!(n = new_node(ps, T_TR))) return;
            push(ps, n);
        }
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        close_to(ps, m_heading, s_block);
        break;
    case T_OPTION:
        close_to(ps, m_option, 0);
        break;
    case T_A:
        close_to(ps, m_a, s_scope);
        break;
    case T_CAPTION: case T_COLGROUP:
        close_to(ps, m_tr, s_table);
        close_to(ps, m_sect, s_tableonly);
        break;
    }

    if (!(n = new_node(ps, tag))) return;
    n->attrs = attrs;
    if (tag == T_A) {
        const char *href = html_attr(n, "href");
        if (href) add_link(ps, n, href);
    }
    if (!(tagdefs[tag].flags & F_VOID)) push(ps, n);
    if (tag == T_PRE || tag == T_LISTING) ps->skip_nl = 1;
}

/* true if an open <t> must not be closed by an end tag of 'closing' */
static int close_stops(int closing, int t)
{
    switch (closing) {
    case T_TABLE: return 0;
    case T_TR: case T_THEAD: case T_TBODY: case T_TFOOT: case T_CAPTION:
        return t == T_TABLE;
    case T_TD: case T_TH:
        return t == T_TABLE || t == T_TR;
    case T_LI:
        return s_list(t);
    default:
        return s_scope(t);
    }
}

static void close_tag(struct PState *ps, int tag)
{
    int i;

    if (tag == T_HTML || tag == T_HEAD || tag == T_BODY || tag == T_UNKNOWN) return;
    if (tag == T_BR) { open_tag(ps, T_BR, 0); return; }
    if (is_heading(tag)) { close_to(ps, m_heading, s_scope); return; }
    for (i = ps->sp - 1; i > 0; i--) {
        int t = ps->stack[i]->tag;
        if (t == tag) { ps->sp = i; return; }
        if (close_stops(tag, t)) return;
    }
}

static void add_text(struct PState *ps, const char *s, long len, int decode)
{
    struct HNode *n;

    if (ps->skip_nl) {
        ps->skip_nl = 0;
        if (len && *s == '\n') { s++; len--; }
    }
    if (len <= 0) return;
    if (!(n = new_node(ps, T_TEXT))) return;
    if (!(n->text = decode_text(ps->doc->pool, s, len, &n->len, decode))) ps->oom = 1;
}

/* finds "</name" case-insensitively, returns pointer to '<' or end */
static const char *find_end_tag(const char *p, const char *end, const char *name)
{
    long nl = h_strlen(name);
    for (; p < end; p++) {
        if (p[0] == '<' && p + 1 < end && p[1] == '/' && end - p - 2 >= nl &&
            !h_strnicmp(p + 2, name, nl) &&
            (p + 2 + nl == end || !is_alnum((unsigned char)p[2 + nl])))
            return p;
    }
    return end;
}

static const char *skip_to_gt(const char *p, const char *end)
{
    while (p < end && *p != '>') p++;
    return p < end ? p + 1 : end;
}

/* parses attributes, p points behind the tag name; returns pointer
 * behind '>' */
static const char *parse_attrs(struct PState *ps, const char *p, const char *end, struct HAttr **list)
{
    struct HAttr *first = 0, *last = 0;
    void *pool = ps->doc->pool;

    for (;;) {
        const char *n0, *v0;
        long nl, vl = 0;
        struct HAttr *a;
        char quote = 0;
        long i;

        while (p < end && (h_isspace((unsigned char)*p) || *p == '/')) p++;
        if (p >= end) break;
        if (*p == '>') { p++; break; }
        n0 = p;
        while (p < end && !h_isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
        nl = p - n0;
        if (!nl) { p++; continue; }         /* stray '=' */
        while (p < end && h_isspace((unsigned char)*p)) p++;
        v0 = p;
        if (p < end && *p == '=') {
            p++;
            while (p < end && h_isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                quote = *p++;
                v0 = p;
                while (p < end && *p != quote) p++;
                vl = p - v0;
                if (p < end) p++;
            } else {
                v0 = p;
                while (p < end && !h_isspace((unsigned char)*p) && *p != '>') p++;
                vl = p - v0;
            }
        }
        if (!(a = hsys_alloc(pool, sizeof(*a))) || !(a->name = hsys_alloc(pool, nl + 1))) {
            ps->oom = 1;
            break;
        }
        for (i = 0; i < nl; i++) a->name[i] = (char)h_tolower((unsigned char)n0[i]);
        if (!(a->value = decode_text(pool, v0, vl, 0, 1))) { ps->oom = 1; break; }
        /* attribute values: newlines/tabs become blanks, trim */
        {
            char *v = a->value, *e;
            for (e = v; *e; e++) if (*e == '\n' || *e == '\t') *e = ' ';
            while (e > v && e[-1] == ' ') *--e = 0;
            while (*a->value == ' ') a->value++;
        }
        if (last) last->next = a; else first = a;
        last = a;
    }
    *list = first;
    return p;
}

static void set_title(struct PState *ps, const char *s, long len)
{
    char *t, *o, *i;
    int sp = 0;

    if (ps->doc->title) return;
    if (!(t = decode_text(ps->doc->pool, s, len, 0, 1))) { ps->oom = 1; return; }
    for (i = o = t; *i; i++) {
        if (h_isspace((unsigned char)*i)) { sp = 1; continue; }
        if (sp && o > t) *o++ = ' ';
        sp = 0;
        *o++ = *i;
    }
    *o = 0;
    ps->doc->title = t;
}

struct HDoc *html_parse(const char *src, long len)
{
    void *pool;
    struct HDoc *doc;
    struct PState *ps;
    const char *p, *end, *text0;
    long slen;
    char *buf;

    if (!(pool = hsys_pool_create())) return 0;
    doc = hsys_alloc(pool, sizeof(*doc));
    ps = hsys_alloc(pool, sizeof(*ps));
    if (!doc || !ps) { hsys_pool_delete(pool); return 0; }
    doc->pool = pool;
    doc->bgcolor = doc->text = doc->link = doc->vlink = doc->alink = COL_NONE;
    if (!(doc->root = hsys_alloc(pool, sizeof(struct HNode)))) goto fail;
    doc->root->tag = T_BODY;
    doc->root->link = -1;
    if (!src) src = "";
    if (len < 0) len = h_strlen(src);
    if (!(buf = prepare_source(pool, src, len, &slen, &doc->utf8))) goto fail;

    ps->doc = doc;
    ps->stack[0] = doc->root;
    ps->sp = 1;

    p = buf;
    end = buf + slen;
    text0 = p;

    while (p < end && !ps->oom) {
        if (*p != '<') { p++; continue; }

        /* markup candidate */
        if (p + 1 < end && (is_alpha((unsigned char)p[1]) ||
            p[1] == '!' || p[1] == '?' ||
            (p[1] == '/' && p + 2 < end && is_alpha((unsigned char)p[2])))) {
            add_text(ps, text0, p - text0, 1);
            if (p[1] == '!') {
                if (end - p >= 4 && p[2] == '-' && p[3] == '-') {     /* comment */
                    const char *q = p + 4;
                    while (q + 2 < end && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) q++;
                    p = (q + 2 < end) ? q + 3 : end;
                } else {
                    p = skip_to_gt(p, end);                         /* <!DOCTYPE ...> */
                }
            } else if (p[1] == '?') {
                p = skip_to_gt(p, end);
            } else {
                int closing = p[1] == '/';
                const char *n0 = p + 1 + closing, *q = n0;
                int tag;
                while (q < end && is_alnum((unsigned char)*q)) q++;
                tag = lookup_tag(n0, q - n0);
                if (closing) {
                    p = skip_to_gt(q, end);
                    close_tag(ps, tag);
                } else {
                    struct HAttr *attrs = 0;
                    p = parse_attrs(ps, q, end, &attrs);
                    if (tag == T_TITLE || tag == T_SCRIPT || tag == T_STYLE || tag == T_TEXTAREA) {
                        const char *e = find_end_tag(p, end, tagdefs[tag].name);
                        if (tag == T_TITLE) set_title(ps, p, e - p);
                        p = skip_to_gt(e, end);
                    } else if (tagdefs[tag].flags & F_RAW) {       /* xmp, listing, plaintext */
                        const char *e = tag == T_PLAINTEXT ? end : find_end_tag(p, end, tagdefs[tag].name);
                        open_tag(ps, tag, attrs);
                        add_text(ps, p, e - p, 0);
                        close_tag(ps, tag);
                        p = skip_to_gt(e, end);
                    } else {
                        open_tag(ps, tag, attrs);
                    }
                }
            }
            text0 = p;
            continue;
        }
        p++;     /* literal '<' */
    }
    add_text(ps, text0, p - text0, 1);

    if (ps->oom) goto fail;
    hsys_free(pool, ps, sizeof(*ps));
    return doc;

fail:
    hsys_pool_delete(pool);
    return 0;
}

void html_free_doc(struct HDoc *doc)
{
    if (doc) hsys_pool_delete(doc->pool);
}
