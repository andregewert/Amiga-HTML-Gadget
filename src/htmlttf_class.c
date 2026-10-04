/*
 * htmlttf_class.c - BOOPSI dispatcher of htmlttf.gadget
 *
 * Alternative renderer for the html.gadget core: text is drawn with
 * FreeType (anti-aliased TrueType fonts), everything is composited into
 * a 32 bit ARGB strip buffer and then written to the window, with
 * WritePixelArray() on RTG screens or mapped to pens on palette screens.
 *
 * Contexts: font files and pictures are only loaded in the application's
 * context (OM_NEW/OM_SET). FreeType itself never does I/O (memory faces)
 * and is only called with the object's semaphore held, on a private
 * stack, so it may run on intuition's input.device task as well.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <intuition/cghooks.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <devices/inputevent.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <dos/dos.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/datatypes.h>
#include <proto/cybergraphics.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "gadgets/htmlttf.h"
#include "html_core.h"
#include "html_private.h"
#include "html_export.h"
#include "htmlttf_print.h"
#include "htmlttf_render.h"

#ifndef RECTFMT_ARGB
#define RECTFMT_ARGB 2
#endif

#define BIG_STACK     (64 * 1024)
#define STRIP_H       32          /* rows rendered per pass */
#define FRAMEW        1
#define MAXPENS       128         /* pens obtained on palette screens */
#define NVAR          TR_NVAR

#define DEF_LINK   0x0000EEUL
#define DEF_VLINK  0x551A8BUL
#define DEF_ALINK  0xFF0000UL

/* font files of the supported families, NVAR per family (NULL = none) */
static const char *const fontsets[][NVAR + 1] = {
    { "Vera", "Vera.ttf", "VeraIt.ttf", "VeraBd.ttf", "VeraBI.ttf",
      "VeraMono.ttf", "VeraMoIt.ttf", "VeraMoBd.ttf", "VeraMoBI.ttf" },
    { "DejaVu", "DejaVuSans.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-BoldOblique.ttf",
      "DejaVuSansMono.ttf", "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-BoldOblique.ttf" },
    { "Noto", "NotoSans-Regular.ttf", "NotoSans-Italic.ttf", "NotoSans-Bold.ttf", "NotoSans-BoldItalic.ttf",
      "NotoSansMono-Regular.ttf", 0, "NotoSansMono-Bold.ttf", 0 },
};
#define NSETS (sizeof(fontsets) / sizeof(fontsets[0]))

static const char *const fontdirs[] = { "PROGDIR:fonts", "FONTS:_TrueType", "FONTS:TrueType", "FONTS:" };
#define NDIRS (sizeof(fontdirs) / sizeof(fontdirs[0]))

/* ------------------------------------------------------------------ */
/* data                                                                */

struct TFace {
    UBYTE  *data;               /* the .ttf file in memory */
    LONG    size;
    FT_Face face;
    BOOL    tried;
};

struct HTMLData {
    struct SignalSemaphore lock;
    struct HDoc      *doc;
    struct HLayout   *lay;
    STRPTR            source;
    struct TImage    *images;
    struct TImage    *bgimg;             /* <body background> (in 'images') */
    BOOL              loadimages;
    LONG              imgtotal, imgloaded;
    char              imgerror[200];
    char              basedir[256];
    STRPTR            linkurl;

    LONG              top, left;
    LONG              visw, vish;
    LONG              laywidth;
    LONG              margin;
    BOOL              syscolors, autoanchors, frame;
    LONG              activelink;
    BOOL              pressed;
    BOOL              havebox;
    char              anchor[128];
    struct HSel       sel;
    BOOL              selecting;
    ULONG             clicksecs, clickmicros;
    LONG              clickitem;
    STRPTR            seltext;

    /* FreeType */
    FT_Library        ft;
    char              fontdir[256];
    int               fontset;           /* index into fontsets, -1 = none */
    struct TFace      faces[NVAR];
    struct TRender    r;                 /* glyph cache, faces, colours */
    struct HEnv       env;

    /* output */
    UBYTE            *stack;             /* private stack for FreeType/layout */
    struct TPrint    *print;             /* HTMLM_PrintBegin .. HTMLM_PrintEnd */
    tr_u32           *strip;
    LONG              stripw;
    UBYTE            *chunky;
    UWORD            *penmap;            /* 15 bit RGB -> pen, 0xFFFF = unknown */
    struct ColorMap  *cm;
    LONG              pens[MAXPENS];
    int               npens;
};

const ULONG html_inst_size = sizeof(struct HTMLData);

/* ------------------------------------------------------------------ */
/* core hooks: memory                                                  */

void *hsys_pool_create(void)
{
    return CreatePool(MEMF_ANY, 16384, 4096);
}

void hsys_pool_delete(void *pool)
{
    DeletePool(pool);
}

void *hsys_alloc(void *pool, long size)
{
    void *m;
    if (size <= 0) size = 4;
    if ((m = AllocPooled(pool, size))) h_memset(m, 0, size);
    return m;
}

void hsys_free(void *pool, void *mem, long size)
{
    if (mem) FreePooled(pool, mem, size <= 0 ? 4 : size);
}

void *memcpy(void *d, const void *s, size_t n) { h_memcpy(d, s, (long)n); return d; }
void *memset(void *d, int c, size_t n) { h_memset(d, c, (long)n); return d; }

void *tr_alloc(long size) { return AllocVec(size > 0 ? size : 4, MEMF_ANY | MEMF_CLEAR); }
void tr_free(void *mem) { if (mem) FreeVec(mem); }

static void copy_name(char *dst, CONST_STRPTR src, int max)
{
    int i;
    for (i = 0; src && src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ------------------------------------------------------------------ */
/* running code on a private stack                                     */

ULONG html_swapcall(struct StackSwapStruct *sss, ULONG (*func)(APTR), APTR arg);
__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl _html_swapcall\n"
    "_html_swapcall:\n"
    "   movem.l d2-d3/a2-a3/a6,-(sp)\n"
    "   move.l  24(sp),a2\n"
    "   move.l  28(sp),a3\n"
    "   move.l  32(sp),d2\n"
    "   move.l  _SysBase,a6\n"
    "   move.l  a2,a0\n"
    "   jsr     -732(a6)\n"
    "   move.l  d2,-(sp)\n"
    "   jsr     (a3)\n"
    "   addq.l  #4,sp\n"
    "   move.l  d0,d3\n"
    "   move.l  _SysBase,a6\n"
    "   move.l  a2,a0\n"
    "   jsr     -732(a6)\n"
    "   move.l  d3,d0\n"
    "   movem.l (sp)+,d2-d3/a2-a3/a6\n"
    "   rts\n");

/* 'stack' may be the object's stack (only with the semaphore held) or
 * NULL for a temporary one                                              */
static BOOL call_big_stack(UBYTE *stack, ULONG (*func)(APTR), APTR arg)
{
    struct StackSwapStruct sss;
    BOOL own = FALSE;

    if (!stack) {
        if (!(stack = AllocVec(BIG_STACK, MEMF_ANY))) return FALSE;
        own = TRUE;
    }
    sss.stk_Lower = stack;
    sss.stk_Upper = (ULONG)(stack + BIG_STACK);
    sss.stk_Pointer = (APTR)(stack + BIG_STACK);
    html_swapcall(&sss, func, arg);
    if (own) FreeVec(stack);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* fonts (FreeType)                                                    */

static long text_width_cb(void *user, int font, int style, const char *s, long len)
{
    struct HTMLData *d = user;
    return tr_text_width(&d->r, font, style, s, len);
}

/* semaphore held: line metrics of every font index */
static void update_metrics(struct HTMLData *d)
{
    tr_metrics(&d->r, &d->env);
    d->laywidth = -1;
}

static BOOL file_exists(CONST_STRPTR name)
{
    BPTR l = Lock((STRPTR)name, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

static void font_path(struct HTMLData *d, const char *file, char *out, LONG max)
{
    copy_name(out, (CONST_STRPTR)d->fontdir, max);
    AddPart((STRPTR)out, (STRPTR)file, max);
}

/* app context: chooses directory and family */
static void find_fontset(struct HTMLData *d, CONST_STRPTR dir, CONST_STRPTR set)
{
    char path[300];
    int s, i;

    d->fontset = -1;
    for (s = 0; s < (int)NSETS; s++) {
        if (set && h_stricmp((const char *)set, fontsets[s][0])) continue;
        if (dir) {
            copy_name(d->fontdir, dir, sizeof(d->fontdir));
            font_path(d, fontsets[s][1], path, sizeof(path));
            if (file_exists((CONST_STRPTR)path)) { d->fontset = s; return; }
            continue;
        }
        for (i = 0; i < (int)NDIRS; i++) {
            copy_name(d->fontdir, (CONST_STRPTR)fontdirs[i], sizeof(d->fontdir));
            font_path(d, fontsets[s][1], path, sizeof(path));
            if (file_exists((CONST_STRPTR)path)) { d->fontset = s; return; }
        }
    }
}

/* app context: reads a font file; returns FALSE if missing */
static BOOL read_font(struct HTMLData *d, int v, struct TFace *tf)
{
    const char *file = d->fontset >= 0 ? fontsets[d->fontset][v + 1] : 0;
    char path[300];
    BPTR fh;
    LONG size;

    if (!file) return FALSE;
    font_path(d, file, path, sizeof(path));
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return FALSE;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        (tf->data = AllocVec(size, MEMF_ANY))) {
        if (Read(fh, tf->data, size) == size) tf->size = size;
        else { FreeVec(tf->data); tf->data = NULL; }
    }
    Close(fh);
    return tf->data != NULL;
}

/* FreeType needs several KB of stack (opening a face, setting sizes);
 * applications often run with the shell's 4 KB, so every FreeType call
 * runs on the object's private stack                                    */
static void free_faces(struct HTMLData *d);

struct FaceArgs {
    struct HTMLData *d;
    struct TFace    *nf;
    BOOL             any;
};

static ULONG faces_func(APTR arg)
{
    struct FaceArgs *fa = arg;
    struct HTMLData *d = fa->d;
    struct TFace *nf = fa->nf;
    int v;

    for (v = 0; v < NVAR; v++) {
        if (!nf[v].tried) continue;
        d->faces[v].tried = TRUE;
        if (nf[v].data) {
            if (!FT_New_Memory_Face(d->ft, nf[v].data, nf[v].size, 0, &d->faces[v].face)) {
                d->faces[v].data = nf[v].data;
                d->faces[v].size = nf[v].size;
                d->r.faces[v] = d->faces[v].face;
            } else {
                d->faces[v].face = NULL;
                FreeVec(nf[v].data);
            }
        }
    }
    if (fa->any) {
        tr_free_insts(&d->r);   /* fallbacks may now resolve to real faces */
        update_metrics(d);
    }
    return 0;
}

static ULONG metrics_func(APTR arg)
{
    update_metrics(arg);
    return 0;
}

static ULONG init_func(APTR arg)
{
    struct HTMLData *d = arg;
    if (FT_Init_FreeType(&d->ft)) d->ft = NULL;
    return 0;
}

static ULONG done_func(APTR arg)
{
    struct HTMLData *d = arg;
    free_faces(d);
    if (d->ft) FT_Done_FreeType(d->ft);
    d->ft = NULL;
    return 0;
}

/* app context: loads the variants in 'mask' (bit = variant index) */
static void load_faces(struct HTMLData *d, ULONG mask)
{
    struct TFace nf[NVAR];
    struct FaceArgs fa;
    int v;
    BOOL any = FALSE;

    if (!d->ft || d->fontset < 0) return;
    mask |= 1;                                     /* sans regular always */
    for (v = 0; v < NVAR; v++) {
        h_memset(&nf[v], 0, sizeof(nf[v]));
        if (!(mask & (1UL << v)) || d->faces[v].tried) continue;
        nf[v].tried = TRUE;
        if (read_font(d, v, &nf[v])) any = TRUE;
    }

    fa.d = d;
    fa.nf = nf;
    fa.any = any;
    ObtainSemaphore(&d->lock);
    call_big_stack(d->stack, faces_func, &fa);
    ReleaseSemaphore(&d->lock);
}

static void free_faces(struct HTMLData *d)
{
    int v;
    tr_free_insts(&d->r);
    for (v = 0; v < NVAR; v++) {
        if (d->faces[v].face) FT_Done_Face(d->faces[v].face);
        if (d->faces[v].data) FreeVec(d->faces[v].data);
        h_memset(&d->faces[v], 0, sizeof(d->faces[v]));
        d->r.faces[v] = NULL;
    }
}

/* which variants does the document use (roughly: any bold, italic, fixed) */
static ULONG faces_used(struct HDoc *doc)
{
    struct HNode *n = doc->root;
    BOOL b = FALSE, i = FALSE, f = FALSE;
    ULONG mask = 1;

    while (n) {
        switch (n->tag) {
        case T_B: case T_STRONG: case T_TH:
        case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
            b = TRUE; break;
        case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: case T_ADDRESS:
            i = TRUE; break;
        case T_TT: case T_CODE: case T_KBD: case T_SAMP: case T_PRE:
        case T_XMP: case T_LISTING: case T_PLAINTEXT:
            f = TRUE; break;
        case T_FONT: {
            const char *face = html_attr(n, "face"), *p;
            if (face) for (p = face; *p; p++)
                if (!h_strnicmp(p, "courier", 7) || !h_strnicmp(p, "mono", 4)) f = TRUE;
            break;
        }
        }
        if (n->first) n = n->first;
        else {
            while (n && !n->next) n = n->parent;
            if (n) n = n->next;
        }
    }
    if (b) mask |= 1 << 2;
    if (i) mask |= 1 << 1;
    if (b && i) mask |= 1 << 3;
    if (f) {
        mask |= 1 << 4;
        if (b) mask |= 1 << 6;
        if (i) mask |= 1 << 5;
        if (b && i) mask |= 1 << 7;
    }
    return mask;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */

static ULONG pen_rgb(struct GadgetInfo *gi, LONG pen)
{
    ULONG rgb[3];
    GetRGB32(gi->gi_Screen->ViewPort.ColorMap, pen, 1, rgb);
    return ((rgb[0] >> 8) & 0xFF0000) | ((rgb[1] >> 16) & 0xFF00) | (rgb[2] >> 24);
}

static void release_pens(struct HTMLData *d)
{
    int i;
    if (d->cm) for (i = 0; i < d->npens; i++) ReleasePen(d->cm, d->pens[i]);
    d->npens = 0;
    d->cm = NULL;
    if (d->penmap) { FreeVec(d->penmap); d->penmap = NULL; }
}

/* palette screens: ARGB -> pen */
static UBYTE map_pen(struct HTMLData *d, ULONG argb)
{
    ULONG key = ((argb >> 9) & 0x7C00) | ((argb >> 6) & 0x03E0) | ((argb >> 3) & 0x001F);
    UWORD p = d->penmap[key];

    if (p == 0xFFFF) {
        ULONG r = ((argb >> 16) & 0xF8) * 0x01010101UL, g = ((argb >> 8) & 0xF8) * 0x01010101UL,
              b = (argb & 0xF8) * 0x01010101UL;
        LONG pen = -1;
        if (d->npens < MAXPENS) {
            pen = ObtainBestPen(d->cm, r, g, b, OBP_Precision, PRECISION_IMAGE, TAG_DONE);
            if (pen >= 0) d->pens[d->npens++] = pen;
        }
        if (pen < 0) pen = FindColor(d->cm, r, g, b, -1);
        p = d->penmap[key] = (UWORD)(pen < 0 ? 0 : pen);
    }
    return (UBYTE)p;
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */

static void gadget_box(struct Gadget *g, struct GadgetInfo *gi, struct IBox *b)
{
    b->Left = g->LeftEdge;
    b->Top = g->TopEdge;
    b->Width = g->Width;
    b->Height = g->Height;
    if (gi) {
        if (g->Flags & GFLG_RELRIGHT)  b->Left += gi->gi_Domain.Width - 1;
        if (g->Flags & GFLG_RELBOTTOM) b->Top += gi->gi_Domain.Height - 1;
        if (g->Flags & GFLG_RELWIDTH)  b->Width += gi->gi_Domain.Width;
        if (g->Flags & GFLG_RELHEIGHT) b->Height += gi->gi_Domain.Height;
    }
}

static LONG inset(struct HTMLData *d) { return d->frame ? FRAMEW : 0; }

static void update_visible(struct HTMLData *d, struct IBox *box)
{
    d->visw = box->Width - 2 * inset(d);
    d->vish = box->Height - 2 * inset(d);
    if (d->visw < 1) d->visw = 1;
    if (d->vish < 1) d->vish = 1;
    d->havebox = TRUE;
}

static void clamp_scroll(struct HTMLData *d)
{
    LONG maxt = (d->lay ? d->lay->height : 0) - d->vish;
    LONG maxl = (d->lay ? d->lay->width : 0) - d->visw;
    if (d->top > maxt) d->top = maxt;
    if (d->top < 0) d->top = 0;
    if (d->left > maxl) d->left = maxl;
    if (d->left < 0) d->left = 0;
}

/* ------------------------------------------------------------------ */
/* layout (call with the semaphore held)                               */

struct LayoutArgs {
    struct HTMLData *d;
    LONG             width;
    struct HLayout  *result;
};

static ULONG layout_func(APTR arg)
{
    struct LayoutArgs *la = arg;
    la->result = html_layout(la->d->doc, &la->d->env, la->width);
    return 0;
}

static BOOL ensure_layout(struct HTMLData *d)
{
    struct LayoutArgs la;

    if (!d->doc || !d->havebox) return FALSE;
    if (d->laywidth != d->visw) {
        if (d->lay) { html_free_layout(d->lay); d->lay = NULL; }
        la.d = d;
        la.width = d->visw;
        la.result = NULL;
        call_big_stack(d->stack, layout_func, &la);
        d->lay = la.result;
        d->laywidth = d->visw;
        d->sel.active = 0;
        d->selecting = FALSE;
    }
    if (d->anchor[0] && d->lay) {
        LONG y = html_find_anchor(d->lay, d->anchor);
        d->anchor[0] = 0;
        if (y >= 0) { d->top = y; clamp_scroll(d); return TRUE; }
    }
    clamp_scroll(d);
    return FALSE;
}

struct RenderArgs {
    struct HTMLData   *d;
    struct GadgetInfo *gi;
    struct RastPort   *rp;
    LONG               ix, iy;
};

static ULONG render_func(APTR arg)
{
    struct RenderArgs *ra = arg;
    struct HTMLData *d = ra->d;
    struct BitMap *bm = ra->gi->gi_Screen->RastPort.BitMap;
    BOOL truecolor = CyberGfxBase && GetBitMapAttr(bm, BMA_DEPTH) > 8;
    struct TStrip s;
    LONG y, i;

    /* colours for the renderer */
    for (i = 0; i < 16 && i < ra->gi->gi_DrInfo->dri_NumPens; i++)
        d->r.penrgb[i] = pen_rgb(ra->gi, ra->gi->gi_DrInfo->dri_Pens[i]);
    d->r.link = DEF_LINK;
    d->r.vlink = DEF_VLINK;
    d->r.alink = DEF_ALINK;
    d->r.doc = d->doc;
    d->r.activelink = d->activelink;
    d->r.pressed = d->pressed;
    d->r.sel = &d->sel;
    d->r.bgimg = d->bgimg;
    d->r.env = &d->env;

    if (d->stripw != d->visw) {
        if (d->strip) FreeVec(d->strip);
        if (d->chunky) FreeVec(d->chunky);
        d->chunky = NULL;
        d->stripw = 0;
        if (!(d->strip = AllocVec(d->visw * STRIP_H * 4, MEMF_ANY))) return 0;
        d->stripw = d->visw;
    }
    if (!truecolor) {
        struct ColorMap *cm = ra->gi->gi_Screen->ViewPort.ColorMap;
        if (cm != d->cm) release_pens(d);
        d->cm = cm;
        if (!d->penmap) {
            if (!(d->penmap = AllocVec(32768 * sizeof(UWORD), MEMF_ANY))) return 0;
            h_memset(d->penmap, 0xFF, 32768 * sizeof(UWORD));
        }
        if (!d->chunky && !(d->chunky = AllocVec(d->visw * STRIP_H, MEMF_ANY))) return 0;
    }

    s.buf = d->strip;
    s.w = d->visw;
    s.dx = d->left;
    for (y = 0; y < d->vish; y += STRIP_H) {
        s.h = d->vish - y < STRIP_H ? d->vish - y : STRIP_H;
        s.dy = d->top + y;
        tr_render_strip(&d->r, d->lay, d->syscolors, &s);
        if (truecolor) {
            WritePixelArray(s.buf, 0, 0, s.w * 4, ra->rp, ra->ix, ra->iy + y, s.w, s.h, RECTFMT_ARGB);
        } else {
            LONG n = s.w * s.h;
            for (i = 0; i < n; i++) d->chunky[i] = map_pen(d, s.buf[i]);
            WriteChunkyPixels(ra->rp, ra->ix, ra->iy + y, ra->ix + s.w - 1, ra->iy + y + s.h - 1,
                              d->chunky, s.w);
        }
    }
    return 1;
}

static void bevel_rp(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h, LONG light, LONG dark)
{
    if (w < 2 || h < 2) return;
    SetAPen(rp, light);
    RectFill(rp, x, y, x + w - 1, y);
    RectFill(rp, x, y, x, y + h - 1);
    SetAPen(rp, dark);
    RectFill(rp, x + 1, y + h - 1, x + w - 1, y + h - 1);
    RectFill(rp, x + w - 1, y + 1, x + w - 1, y + h - 1);
}

static void render(struct HTMLData *d, struct Gadget *g, struct GadgetInfo *gi, struct RastPort *rp, BOOL full)
{
    struct IBox box;
    struct RenderArgs ra;

    if (!gi || !rp) return;
    gadget_box(g, gi, &box);
    if (box.Width <= 2 * inset(d) || box.Height <= 2 * inset(d)) return;

    ObtainSemaphore(&d->lock);
    update_visible(d, &box);
    ensure_layout(d);
    if (full && d->frame) {
        UWORD *pens = gi->gi_DrInfo->dri_Pens;
        SetDrMd(rp, JAM1);
        bevel_rp(rp, box.Left, box.Top, box.Width, box.Height, pens[SHADOWPEN], pens[SHINEPEN]);
    }
    ra.d = d;
    ra.gi = gi;
    ra.rp = rp;
    ra.ix = box.Left + inset(d);
    ra.iy = box.Top + inset(d);
    call_big_stack(d->stack, render_func, &ra);
    ReleaseSemaphore(&d->lock);
}

static void redraw(Object *o, struct GadgetInfo *gi)
{
    struct RastPort *rp;
    if (gi && (rp = ObtainGIRPort(gi))) {
        struct gpRender gpr;
        gpr.MethodID = GM_RENDER;
        gpr.gpr_GInfo = gi;
        gpr.gpr_RPort = rp;
        gpr.gpr_Redraw = GREDRAW_UPDATE;
        dm(o, (Msg)&gpr);
        ReleaseGIRPort(rp);
    }
}

/* ------------------------------------------------------------------ */
/* notification                                                        */

static void notify(Class *cl, Object *o, struct GadgetInfo *gi, ULONG flags)
{
    struct HTMLData *d = INST_DATA(cl, o);
    struct TagItem tags[8];
    struct opUpdate opu;

    tags[0].ti_Tag = HTML_Top;          tags[0].ti_Data = d->top;
    tags[1].ti_Tag = HTML_Total;        tags[1].ti_Data = d->lay ? d->lay->height : 0;
    tags[2].ti_Tag = HTML_Visible;      tags[2].ti_Data = d->vish;
    tags[3].ti_Tag = HTML_Left;         tags[3].ti_Data = d->left;
    tags[4].ti_Tag = HTML_TotalWidth;   tags[4].ti_Data = d->lay ? d->lay->width : 0;
    tags[5].ti_Tag = HTML_VisibleWidth; tags[5].ti_Data = d->visw;
    tags[6].ti_Tag = GA_ID;             tags[6].ti_Data = ((struct Gadget *)o)->GadgetID;
    tags[7].ti_Tag = TAG_DONE;

    opu.MethodID = OM_NOTIFY;
    opu.opu_AttrList = tags;
    opu.opu_GInfo = gi;
    opu.opu_Flags = flags;
    dsm(cl, o, (Msg)&opu);
}

/* ------------------------------------------------------------------ */
/* pictures (datatypes -> ARGB)                                        */

static void free_images(struct TImage *im)
{
    while (im) {
        struct TImage *next = im->next;
        tr_free(im->pix);
        FreeVec(im);
        im = next;
    }
}

static BOOL resolve_path(CONST_STRPTR base, const char *src, char *out, LONG max)
{
    char rel[256];
    const char *p;
    LONG n = 0;

    if (!src || !*src || !h_strnicmp(src, "data:", 5)) return FALSE;
    for (p = src; *p; p++) if (p[0] == ':' && p[1] == '/' && p[2] == '/') break;
    if (*p) {
        if (h_strnicmp(src, "file:///", 8)) return FALSE;
        src += 8;
    }
    while (*src && *src != '?' && *src != '#' && n < (LONG)sizeof(rel) - 2) {
        if (src[0] == '.' && src[1] == '/') { src += 2; continue; }
        if (src[0] == '.' && src[1] == '.' && src[2] == '/') { rel[n++] = '/'; src += 3; continue; }
        rel[n++] = *src++;
    }
    rel[n] = 0;
    if (!n) return FALSE;
    copy_name(out, base ? base : (CONST_STRPTR)"", max);
    return AddPart((STRPTR)out, (STRPTR)rel, max) ? TRUE : FALSE;
}

struct ImgLoad {
    struct TImage *list;
    struct TImage *bgimg;
    LONG           total, loaded;
    char           err[200];
};

static void image_error(struct ImgLoad *il, const char *path, const char *step, LONG code)
{
    char reason[100];
    LONG n = 0;
    const char *p;

    if (il->err[0]) return;
    reason[0] = 0;
    if (code >= DTERROR_UNKNOWN_DATATYPE && DataTypesBase) {
        CONST_STRPTR fmt = GetDTString(code), file = FilePart((STRPTR)path);
        LONG k = 0;
        while (fmt && *fmt && k < (LONG)sizeof(reason) - 1) {
            if (fmt[0] == '%' && fmt[1] == 's') {
                CONST_STRPTR f;
                for (f = file; *f && k < (LONG)sizeof(reason) - 1; f++) reason[k++] = *f;
                fmt += 2;
            } else {
                reason[k++] = *fmt++;
            }
        }
        reason[k] = 0;
    } else if (code) {
        Fault(code, NULL, (STRPTR)reason, sizeof(reason));
    }
    for (p = path; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    for (p = ": "; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    for (p = step; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    if (reason[0]) {
        for (p = ": "; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
        for (p = reason; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    }
    il->err[n] = 0;
}

static struct TImage *load_image(struct ImgLoad *il, const char *path, LONG reqw, LONG reqh)
{
    struct TImage *im;
    struct BitMapHeader *bmhd = NULL;
    struct pdtBlitPixelArray pa;
    Object *dto;
    ULONG alpha = 0;
    LONG w, h, i;
    UBYTE *lut = NULL;

    for (im = il->list; im; im = im->next)
        if (im->reqw == reqw && im->reqh == reqh && !h_stricmp(im->path, path))
            return im->pix ? im : NULL;
    if (!(im = AllocVec(sizeof(*im), MEMF_ANY | MEMF_CLEAR))) return NULL;
    copy_name(im->path, (CONST_STRPTR)path, sizeof(im->path));
    im->reqw = reqw;
    im->reqh = reqh;
    im->next = il->list;
    il->list = im;

    if (!(dto = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE,
                            PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE))) {
        image_error(il, path, "NewDTObject", IoErr());
        return NULL;
    }
    GetDTAttrs(dto, PDTA_BitMapHeader, (ULONG)&bmhd, TAG_DONE);
    if (!bmhd || !bmhd->bmh_Width || !bmhd->bmh_Height) {
        image_error(il, path, "keine Bildgroesse", 0);
        DisposeDTObject(dto);
        return NULL;
    }
    w = bmhd->bmh_Width;
    h = bmhd->bmh_Height;
    if (!(im->pix = tr_alloc(w * h * 4))) {
        image_error(il, path, "kein Speicher", 0);
        DisposeDTObject(dto);
        return NULL;
    }
    pa.MethodID = PDTM_READPIXELARRAY;
    pa.pbpa_PixelData = im->pix;
    pa.pbpa_PixelFormat = PBPAFMT_ARGB;
    pa.pbpa_PixelArrayMod = w * 4;
    pa.pbpa_Left = 0;
    pa.pbpa_Top = 0;
    pa.pbpa_Width = w;
    pa.pbpa_Height = h;
    if (!dm(dto, (Msg)&pa)) {
        image_error(il, path, "PDTM_READPIXELARRAY", IoErr());
        tr_free(im->pix);
        im->pix = NULL;
        DisposeDTObject(dto);
        return NULL;
    }

    /* alpha: real alpha channel (PNG), transparent colour (GIF) or opaque */
    if (GetDTAttrs(dto, PDTA_AlphaChannel, (ULONG)&alpha, TAG_DONE) != 1) alpha = 0;
    if (!alpha)
        for (i = 0; i < w * h; i++) im->pix[i] |= 0xFF000000UL;
    if (bmhd->bmh_Masking == mskHasTransparentColor && (lut = AllocVec(w * h, MEMF_ANY))) {
        pa.pbpa_PixelData = lut;
        pa.pbpa_PixelFormat = PBPAFMT_LUT8;
        pa.pbpa_PixelArrayMod = w;
        if (dm(dto, (Msg)&pa)) {
            for (i = 0; i < w * h; i++)
                if (lut[i] == bmhd->bmh_Transparent) im->pix[i] = 0;
            alpha = 1;
        }
        FreeVec(lut);
    }
    DisposeDTObject(dto);
    im->opaque = !alpha;
    im->w = w;
    im->h = h;

    if (reqw > 0 || reqh > 0) {
        LONG tw = reqw > 0 ? reqw : w * reqh / h;
        LONG th = reqh > 0 ? reqh : h * reqw / w;
        if (tw > 0 && th > 0 && tw <= 4000 && th <= 4000 && (tw != w || th != h)) {
            tr_u32 *np = tr_scale_argb(im->pix, w, h, tw, th);
            if (np) {
                tr_free(im->pix);
                im->pix = np;
                im->w = tw;
                im->h = th;
            }
        }
    }
    return im;
}

static void load_images(struct ImgLoad *il, struct HDoc *doc, CONST_STRPTR basedir)
{
    struct TImage *im;
    struct HNode *n = doc->root;
    char path[256];

    if (!html_open_datatypes()) {
        copy_name(il->err, (CONST_STRPTR)"datatypes.library nicht gefunden", sizeof(il->err));
        return;
    }
    /* page background, tiled in its natural size */
    if (doc->background && resolve_path(basedir, doc->background, path, sizeof(path))) {
        il->total++;
        if ((il->bgimg = load_image(il, path, 0, 0))) il->loaded++;
    }
    while (n) {
        const char *src, *a;
        /* table and cell backgrounds */
        if ((n->tag == T_TABLE || n->tag == T_TD || n->tag == T_TH) &&
            (src = html_attr(n, "background")) && resolve_path(basedir, src, path, sizeof(path))) {
            il->total++;
            if ((im = load_image(il, path, 0, 0))) {
                n->img = im;
                il->loaded++;
            }
        }
        if ((n->tag == T_IMG || (n->tag == T_INPUT && (a = html_attr(n, "type")) && !h_stricmp(a, "image"))) &&
            (src = html_attr(n, "src")) && resolve_path(basedir, src, path, sizeof(path))) {
            LONG w = -1, h = -1;
            if ((a = html_attr(n, "width")) && !h_strchr(a, '%')) w = h_atol(a);
            if ((a = html_attr(n, "height")) && !h_strchr(a, '%')) h = h_atol(a);
            il->total++;
            if ((im = load_image(il, path, w > 0 ? w : 0, h > 0 ? h : 0))) {
                n->img = im;
                n->iw = im->w;
                n->ih = im->h;
                il->loaded++;
            }
        }
        if (n->first) n = n->first;
        else {
            while (n && !n->next) n = n->parent;
            if (n) n = n->next;
        }
    }
}

/* ------------------------------------------------------------------ */
/* documents                                                           */

struct ParseArgs {
    CONST_STRPTR  src;
    LONG          len;
    struct HDoc  *doc;
};

static ULONG parse_func(APTR arg)
{
    struct ParseArgs *pa = arg;
    pa->doc = html_parse((const char *)pa->src, pa->len);
    return 0;
}

static BOOL set_document(struct HTMLData *d, CONST_STRPTR src, LONG len, CONST_STRPTR basedir)
{
    struct ParseArgs pa;
    struct ImgLoad il;
    struct TImage *oldimages;
    STRPTR copy;

    if (!src) { src = (CONST_STRPTR)""; len = 0; }
    if (len < 0) len = h_strlen((const char *)src);
    if (!(copy = AllocVec(len + 1, MEMF_ANY))) return FALSE;
    h_memcpy(copy, src, len);
    copy[len] = 0;

    pa.src = copy;
    pa.len = len;
    pa.doc = NULL;
    call_big_stack(NULL, parse_func, &pa);
    if (!pa.doc) { FreeVec(copy); return FALSE; }
    load_faces(d, faces_used(pa.doc));

    h_memset(&il, 0, sizeof(il));
    if (d->loadimages) load_images(&il, pa.doc, basedir);

    ObtainSemaphore(&d->lock);
    if (d->print) { tp_end(d->print); d->print = NULL; }       /* belongs to the old document */
    if (d->lay) { html_free_layout(d->lay); d->lay = NULL; }
    html_free_doc(d->doc);
    if (d->source) FreeVec(d->source);
    d->doc = pa.doc;
    oldimages = d->images;
    d->images = il.list;
    d->bgimg = il.bgimg;
    d->imgtotal = il.total;
    d->imgloaded = il.loaded;
    copy_name(d->imgerror, (CONST_STRPTR)il.err, sizeof(d->imgerror));
    d->source = copy;
    d->laywidth = -1;
    d->top = d->left = 0;
    d->linkurl = NULL;
    d->activelink = -1;
    d->pressed = FALSE;
    d->sel.active = 0;
    d->selecting = FALSE;
    copy_name(d->basedir, basedir, sizeof(d->basedir));
    ReleaseSemaphore(&d->lock);
    free_images(oldimages);
    return TRUE;
}

static BOOL load_file(struct HTMLData *d, CONST_STRPTR name)
{
    BPTR fh;
    LONG size, got = -1;
    STRPTR buf = NULL;
    BOOL ok = FALSE;
    char dir[256];

    if (!name || !(fh = Open((STRPTR)name, MODE_OLDFILE))) return FALSE;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) >= 0) {
        if ((buf = AllocVec(size + 1, MEMF_ANY))) got = Read(fh, buf, size);
    }
    Close(fh);
    if (buf && got >= 0) {
        LONG n = (LONG)(PathPart((STRPTR)name) - (STRPTR)name);
        if (n > (LONG)sizeof(dir) - 1) n = sizeof(dir) - 1;
        h_memcpy(dir, name, n);
        dir[n] = 0;
        ok = set_document(d, buf, got, (CONST_STRPTR)dir);
    }
    if (buf) FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* links                                                               */

static LONG link_at(struct HTMLData *d, WORD mx, WORD my)
{
    struct HLayout *lay = d->lay;
    LONG x = mx - inset(d) + d->left, y = my - inset(d) + d->top, i;

    if (!lay || mx < inset(d) || my < inset(d) || mx >= inset(d) + d->visw || my >= inset(d) + d->vish)
        return -1;
    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        if (it->link >= 0 && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)
            return it->link;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* attributes                                                          */

/* app context: selection -> clipboard */
static void copy_selection(struct HTMLData *d)
{
    STRPTR buf = NULL;
    LONG n;

    ObtainSemaphore(&d->lock);
    if ((n = html_sel_text(&d->sel, d->lay, NULL)) > 0 && (buf = AllocVec(n + 1, MEMF_ANY)))
        html_sel_text(&d->sel, d->lay, (char *)buf);
    ReleaseSemaphore(&d->lock);
    if (buf) {
        html_write_clip((const char *)buf, n);
        FreeVec(buf);
    }
}

static ULONG set_attrs(Class *cl, Object *o, struct opSet *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    struct GadgetInfo *gi = msg->ops_GInfo;
    struct TagItem *tstate = msg->ops_AttrList, *tag;
    BOOL update = msg->MethodID == OM_UPDATE, newdoc = FALSE, redo = FALSE, moved = FALSE, repaint = FALSE;
    LONG oldtop = d->top, oldleft = d->left;
    ULONG rv = 0;

    while ((tag = NextTagItem(&tstate))) {
        ULONG data = tag->ti_Data;
        switch (tag->ti_Tag) {
        case HTML_Text:
            if (!update) { set_document(d, (CONST_STRPTR)data, -1, (CONST_STRPTR)""); newdoc = TRUE; }
            break;
        case HTML_File:
            if (!update) { if (load_file(d, (CONST_STRPTR)data)) newdoc = TRUE; }
            break;
        case HTML_Top:
            d->top = (LONG)data;
            moved = TRUE;
            break;
        case HTML_Left:
            d->left = (LONG)data;
            moved = TRUE;
            break;
        case HTML_Anchor:
            if (!update && data) {
                ObtainSemaphore(&d->lock);
                copy_name(d->anchor, (CONST_STRPTR)data, sizeof(d->anchor));
                if (d->anchor[0] == '#') copy_name(d->anchor, (CONST_STRPTR)data + 1, sizeof(d->anchor));
                if (ensure_layout(d)) moved = TRUE;
                ReleaseSemaphore(&d->lock);
            }
            break;
        case HTML_SystemColors:
            d->syscolors = data ? TRUE : FALSE;
            d->env.system_colors = d->syscolors;
            d->laywidth = -1;
            redo = TRUE;
            break;
        case HTML_Margin:
            d->margin = (LONG)data < 0 ? 0 : (LONG)data;
            d->env.margin = d->margin;
            d->laywidth = -1;
            redo = TRUE;
            break;
        case HTML_AutoAnchors:
            d->autoanchors = data ? TRUE : FALSE;
            break;
        case HTML_FitImages:
            d->env.fit_images = data ? TRUE : FALSE;
            d->laywidth = -1;
            redo = TRUE;
            break;
        case HTML_LoadImages:
            d->loadimages = data ? TRUE : FALSE;
            break;
        case HTML_Copy:
            if (!update && data) copy_selection(d);
            break;
        case HTML_SelectAll:
            if (data) {
                ObtainSemaphore(&d->lock);
                ensure_layout(d);
                html_sel_all(&d->sel, d->lay);
                d->selecting = FALSE;
                ReleaseSemaphore(&d->lock);
                repaint = TRUE;
            }
            break;
        case HTML_ClearSelection:
            if (data && d->sel.active) {
                d->sel.active = 0;
                d->selecting = FALSE;
                repaint = TRUE;
            }
            break;
        }
    }

    ObtainSemaphore(&d->lock);
    clamp_scroll(d);
    if (d->top != oldtop || d->left != oldleft) moved = TRUE;
    else if (!newdoc && !redo) moved = FALSE;
    ReleaseSemaphore(&d->lock);

    if (gi && repaint && !(newdoc || redo || moved)) {
        redraw(o, gi);
        rv = 1;
    }
    if (gi && (newdoc || redo || moved)) {
        redraw(o, gi);
        if (!update || newdoc || redo) notify(cl, o, gi, 0);
        rv = 1;
    }
    return rv;
}

static ULONG get_attr(Class *cl, Object *o, struct opGet *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    ULONG *store = msg->opg_Storage;

    switch (msg->opg_AttrID) {
    case HTML_Text:          *store = (ULONG)d->source; return 1;
    case HTML_Title:         *store = (ULONG)(d->doc ? d->doc->title : NULL); return 1;
    case HTML_Top:           *store = d->top; return 1;
    case HTML_Total:         *store = d->lay ? d->lay->height : 0; return 1;
    case HTML_Visible:       *store = d->vish; return 1;
    case HTML_Left:          *store = d->left; return 1;
    case HTML_TotalWidth:    *store = d->lay ? d->lay->width : 0; return 1;
    case HTML_VisibleWidth:  *store = d->visw; return 1;
    case HTML_LinkURL:       *store = (ULONG)d->linkurl; return 1;
    case HTML_SystemColors:  *store = d->syscolors; return 1;
    case HTML_Margin:        *store = d->margin; return 1;
    case HTML_LineHeight:    *store = d->env.font_height[HF_INDEX(0, 3)] + 1; return 1;
    case HTML_AutoAnchors:   *store = d->autoanchors; return 1;
    case HTML_FitImages:    *store = d->env.fit_images; return 1;
    case HTML_LoadImages:    *store = d->loadimages; return 1;
    case HTML_ImagesTotal:   *store = d->imgtotal; return 1;
    case HTML_ImagesLoaded:  *store = d->imgloaded; return 1;
    case HTML_ImageError:    *store = (ULONG)(d->imgerror[0] ? d->imgerror : NULL); return 1;
    case HTML_NumLinks:      *store = d->doc ? d->doc->nlinks : 0; return 1;
    case HTML_BaseDir:       *store = (ULONG)d->basedir; return 1;
    case HTML_HasSelection:  *store = !html_sel_empty(&d->sel); return 1;
    case HTML_SelectedText: {
        LONG n;
        ObtainSemaphore(&d->lock);
        if (d->seltext) { FreeVec(d->seltext); d->seltext = NULL; }
        if ((n = html_sel_text(&d->sel, d->lay, NULL)) > 0 && (d->seltext = AllocVec(n + 1, MEMF_ANY)))
            html_sel_text(&d->sel, d->lay, (char *)d->seltext);
        ReleaseSemaphore(&d->lock);
        *store = (ULONG)d->seltext;
        return 1;
    }
    case HTMLTTF_Size:       *store = d->r.basepx; return 1;
    case HTMLTTF_FontDir:    *store = (ULONG)d->fontdir; return 1;
    case HTMLTTF_FontSetName:*store = (ULONG)(d->fontset >= 0 ? fontsets[d->fontset][0] : NULL); return 1;
    }
    return dsm(cl, o, (Msg)msg);
}

/* ------------------------------------------------------------------ */
/* object life cycle                                                   */

static Object *om_new(Class *cl, Object *o, struct opSet *msg)
{
    struct HTMLData *d;
    struct Screen *scr;
    Object *obj;

    if (!(obj = (Object *)dsm(cl, o, (Msg)msg))) return NULL;
    d = INST_DATA(cl, obj);
    h_memset(d, 0, sizeof(*d));
    InitSemaphore(&d->lock);
    d->laywidth = -1;
    d->activelink = -1;
    d->fontset = -1;
    d->margin = GetTagData(HTML_Margin, 8, msg->ops_AttrList);
    d->syscolors = GetTagData(HTML_SystemColors, FALSE, msg->ops_AttrList) ? TRUE : FALSE;
    d->autoanchors = GetTagData(HTML_AutoAnchors, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->frame = GetTagData(HTML_Frame, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->loadimages = GetTagData(HTML_LoadImages, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->visw = d->vish = 1;
    d->stack = AllocVec(BIG_STACK, MEMF_ANY);

    /* body text size: explicit, or a bit smaller than the screen font's
     * line height (TrueType sizes are em sizes)                          */
    d->r.basepx = GetTagData(HTMLTTF_Size, 0, msg->ops_AttrList);
    if (d->r.basepx <= 0) {
        LONG ys = 13;
        if ((scr = LockPubScreen(NULL))) {
            ys = scr->Font->ta_YSize;
            UnlockPubScreen(NULL, scr);
        }
        d->r.basepx = (ys * 12 + 6) / 13;
    }
    if (d->r.basepx < 8) d->r.basepx = 8;
    if (d->r.basepx > 64) d->r.basepx = 64;

    html_open_cybergfx();
    if (d->stack) call_big_stack(d->stack, init_func, d);
    find_fontset(d, (CONST_STRPTR)GetTagData(HTMLTTF_FontDir, 0, msg->ops_AttrList),
                 (CONST_STRPTR)GetTagData(HTMLTTF_FontSet, 0, msg->ops_AttrList));

    d->env.user = d;
    d->env.text_width = text_width_cb;
    d->env.margin = d->margin;
    d->env.fit_images = GetTagData(HTML_FitImages, FALSE, msg->ops_AttrList) ? TRUE : FALSE;
    d->env.system_colors = d->syscolors;
    ObtainSemaphore(&d->lock);
    call_big_stack(d->stack, metrics_func, d);
    ReleaseSemaphore(&d->lock);

    ((struct Gadget *)obj)->Activation |= GACT_RELVERIFY;

    {
        struct opSet ops = *msg;
        ops.MethodID = OM_SET;
        ops.ops_GInfo = NULL;
        set_attrs(cl, obj, &ops);
    }
    if (!d->doc) set_document(d, (CONST_STRPTR)"", 0, (CONST_STRPTR)"");
    return obj;
}

static void om_dispose(Class *cl, Object *o)
{
    struct HTMLData *d = INST_DATA(cl, o);

    release_pens(d);
    if (d->print) tp_end(d->print);
    if (d->strip) FreeVec(d->strip);
    if (d->chunky) FreeVec(d->chunky);
    if (d->lay) html_free_layout(d->lay);
    html_free_doc(d->doc);
    free_images(d->images);
    if (d->source) FreeVec(d->source);
    if (d->seltext) FreeVec(d->seltext);
    call_big_stack(d->stack, done_func, d);
    if (d->stack) FreeVec(d->stack);
}

/* ------------------------------------------------------------------ */
/* gadget methods                                                      */

static ULONG gm_layout(Class *cl, Object *o, struct gpLayout *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    struct IBox box;

    gadget_box((struct Gadget *)o, msg->gpl_GInfo, &box);
    ObtainSemaphore(&d->lock);
    update_visible(d, &box);
    ensure_layout(d);
    clamp_scroll(d);
    ReleaseSemaphore(&d->lock);
    notify(cl, o, msg->gpl_GInfo, 0);
    return 1;
}

static ULONG gm_domain(Class *cl, Object *o, struct gpDomain *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    WORD w = 64, h = 32;

    switch (msg->gpd_Which) {
    case GDOMAIN_NOMINAL: w = 320; h = 200; break;
    case GDOMAIN_MAXIMUM: w = 16383; h = 16383; break;
    }
    msg->gpd_Domain.Left = msg->gpd_Domain.Top = 0;
    msg->gpd_Domain.Width = w + 2 * inset(d);
    msg->gpd_Domain.Height = h + 2 * inset(d);
    return 1;
}

static ULONG gm_hittest(Class *cl, Object *o, struct gpHitTest *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    LONG l;

    l = msg->gpht_Mouse.X >= inset(d) && msg->gpht_Mouse.Y >= inset(d) &&
        msg->gpht_Mouse.X < inset(d) + d->visw && msg->gpht_Mouse.Y < inset(d) + d->vish;
    return l ? GMR_GADGETHIT : 0;
}

/* mouse -> text position; measuring may load glyphs, so FreeType runs on
 * the private stack (semaphore held)                                    */
struct PosArgs {
    struct HTMLData *d;
    long             x, y, item, ch;
    int              ok;
};

static ULONG pos_func(APTR arg)
{
    struct PosArgs *pa = arg;
    pa->ok = pa->d->lay && html_sel_pos(pa->d->lay, &pa->d->env, pa->x, pa->y, &pa->item, &pa->ch);
    return 0;
}

static BOOL sel_pos(struct HTMLData *d, WORD mx, WORD my, long *item, long *ch)
{
    struct PosArgs pa;
    pa.d = d;
    pa.x = mx - inset(d) + d->left;
    pa.y = my - inset(d) + d->top;
    pa.ok = 0;
    call_big_stack(d->stack, pos_func, &pa);
    *item = pa.item;
    *ch = pa.ch;
    return pa.ok;
}

static BOOL sel_track(struct HTMLData *d, WORD mx, WORD my)
{
    long it, ch;
    if (!sel_pos(d, mx, my, &it, &ch)) return FALSE;
    if (it == d->sel.i1 && ch == d->sel.c1) return FALSE;
    d->sel.i1 = it;
    d->sel.c1 = ch;
    return TRUE;
}

static ULONG gm_goactive(Class *cl, Object *o, struct gpInput *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    LONG l;

    struct InputEvent *ie = msg->gpi_IEvent;
    long it, ch;
    BOOL had;

    if (!ie) return GMR_NOREUSE;
    ObtainSemaphore(&d->lock);
    l = link_at(d, msg->gpi_Mouse.X, msg->gpi_Mouse.Y);
    d->activelink = l;
    d->pressed = l >= 0;
    if (l >= 0) {
        ReleaseSemaphore(&d->lock);
        redraw(o, msg->gpi_GInfo);
        return GMR_MEACTIVE;
    }

    had = !html_sel_empty(&d->sel);
    d->selecting = FALSE;
    if (sel_pos(d, msg->gpi_Mouse.X, msg->gpi_Mouse.Y, &it, &ch)) {
        if (it == d->clickitem &&
            DoubleClick(d->clicksecs, d->clickmicros, ie->ie_TimeStamp.tv_secs, ie->ie_TimeStamp.tv_micro)) {
            d->sel.i0 = d->sel.i1 = it;
            d->sel.c0 = 0;
            d->sel.c1 = d->lay->items[it].len;
            d->sel.active = 1;
            d->clicksecs = 0;
        } else {
            d->sel.i0 = d->sel.i1 = it;
            d->sel.c0 = d->sel.c1 = ch;
            d->sel.active = 1;
            d->selecting = TRUE;
            d->clicksecs = ie->ie_TimeStamp.tv_secs;
            d->clickmicros = ie->ie_TimeStamp.tv_micro;
        }
        d->clickitem = it;
    } else {
        d->sel.active = 0;
    }
    ReleaseSemaphore(&d->lock);
    if (had || !html_sel_empty(&d->sel)) redraw(o, msg->gpi_GInfo);
    return d->selecting ? GMR_MEACTIVE : GMR_NOREUSE;
}

static ULONG gm_handleinput(Class *cl, Object *o, struct gpInput *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    struct InputEvent *ie = msg->gpi_IEvent;
    ULONG rv = GMR_MEACTIVE;
    BOOL over, jump = FALSE;

    if (d->selecting) {
        BOOL changed = FALSE, scrolled = FALSE;
        WORD mx = msg->gpi_Mouse.X, my = msg->gpi_Mouse.Y;

        if (ie->ie_Class == IECLASS_RAWMOUSE && ie->ie_Code == SELECTUP) {
            d->selecting = FALSE;
            if (html_sel_empty(&d->sel)) d->sel.active = 0;
            return GMR_NOREUSE;
        }
        if (ie->ie_Class == IECLASS_RAWMOUSE && ie->ie_Code == MENUDOWN) {
            d->selecting = FALSE;
            return GMR_REUSE;
        }
        ObtainSemaphore(&d->lock);
        if (ie->ie_Class == IECLASS_TIMER) {
            LONG step = d->env.font_height[HF_INDEX(0, 3)] + 1, ot = d->top, ol = d->left;
            if (my < inset(d)) d->top -= step;
            else if (my >= inset(d) + d->vish) d->top += step;
            if (mx < inset(d)) d->left -= step;
            else if (mx >= inset(d) + d->visw) d->left += step;
            clamp_scroll(d);
            scrolled = d->top != ot || d->left != ol;
        }
        if (ie->ie_Class == IECLASS_RAWMOUSE || scrolled) changed = sel_track(d, mx, my);
        ReleaseSemaphore(&d->lock);
        if (changed || scrolled) redraw(o, msg->gpi_GInfo);
        if (scrolled) notify(cl, o, msg->gpi_GInfo, 0);
        return GMR_MEACTIVE;
    }

    if (ie->ie_Class != IECLASS_RAWMOUSE) return rv;

    ObtainSemaphore(&d->lock);
    over = link_at(d, msg->gpi_Mouse.X, msg->gpi_Mouse.Y) == d->activelink;
    ReleaseSemaphore(&d->lock);

    switch (ie->ie_Code) {
    case SELECTUP:
        rv = GMR_NOREUSE;
        ObtainSemaphore(&d->lock);
        if (over && d->activelink >= 0 && d->doc && d->activelink < d->doc->nlinks) {
            d->linkurl = (STRPTR)d->doc->links[d->activelink];
            d->doc->visited[d->activelink] = 1;
            if (d->autoanchors && d->linkurl[0] == '#') {
                LONG y = html_find_anchor(d->lay, (const char *)d->linkurl + 1);
                if (y >= 0) { d->top = y; clamp_scroll(d); jump = TRUE; }
            }
            *msg->gpi_Termination = d->activelink;
            rv |= GMR_VERIFY;
        }
        ReleaseSemaphore(&d->lock);
        break;
    case MENUDOWN:
        rv = GMR_REUSE;
        break;
    default:
        if (over != d->pressed) {
            d->pressed = over;
            redraw(o, msg->gpi_GInfo);
        }
        break;
    }
    if (jump) notify(cl, o, msg->gpi_GInfo, 0);
    return rv;
}

static ULONG gm_goinactive(Class *cl, Object *o, struct gpGoInactive *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    d->selecting = FALSE;
    if (d->activelink >= 0) {
        d->pressed = FALSE;
        redraw(o, msg->gpgi_GInfo);
    }
    d->activelink = -1;
    return 0;
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* HTMLM_Export                                                        */

/* the pictures are there with their pixels (ARGB, alpha set) */
static int export_image(void *user, void *img, struct HPrintImage *pi)
{
    struct TImage *im = img;
    if (!im->pix) return 0;
    pi->w = im->w;
    pi->h = im->h;
    pi->argb = (const unsigned long *)im->pix;
    pi->priv = NULL;
    html_export_load_jpeg(im->path, pi);
    return 1;
}

static void export_image_free(void *user, struct HPrintImage *pi)
{
    html_export_free_jpeg(pi);
}

struct ExportArgs {
    struct HTMLData *d;
    Object          *o;
    struct TagItem  *tags;
    LONG             result;
};

static ULONG export_func(APTR arg)
{
    struct ExportArgs *a = arg;
    a->result = html_export(a->d->doc, a->o, a->tags, a->d->env.fit_images, export_image, export_image_free, a->d);
    return 0;
}

/* on the gadget's own stack (semaphore held), as parsing and layout:
 * the layout is recursive (nested tables), the caller's stack may be
 * small                                                               */
static LONG export_doc(Class *cl, Object *o, struct hmExport *msg)
{
    struct ExportArgs a;
    a.d = INST_DATA(cl, o);
    a.o = o;
    a.tags = msg->hme_Tags;
    a.result = -1;
    ObtainSemaphore(&a.d->lock);
    if (!call_big_stack(a.d->stack, export_func, &a)) SetIoErr(ERROR_NO_FREE_STORE);
    ReleaseSemaphore(&a.d->lock);
    return a.result;
}

/* ------------------------------------------------------------------ */
/* bitmap printing                                                     */

struct PrintArgs {
    struct HTMLData      *d;
    struct TPrintOpts     opt;
    struct hmPrintRender *msg;
    long                  pages, w, h;
    LONG                  result;
};

static ULONG print_begin_func(APTR arg)
{
    struct PrintArgs *a = arg;
    struct HTMLData *d = a->d;
    d->print = tp_begin(d->doc, &d->r, &a->opt, &a->pages, &a->w, &a->h);
    a->result = d->print ? a->pages : -1;
    return 0;
}

static ULONG print_render_func(APTR arg)
{
    struct PrintArgs *a = arg;
    struct hmPrintRender *m = a->msg;
    a->result = tp_render(a->d->print, m->hmpr_Page, m->hmpr_X, m->hmpr_Y, m->hmpr_Width, m->hmpr_Height,
                          (tr_u32 *)m->hmpr_Buffer);
    return 0;
}

static LONG print_begin(Class *cl, Object *o, struct hmPrintBegin *msg)
{
    struct TagItem *tags = msg->hmpb_Tags;
    LONG *pages = (LONG *)GetTagData(HTMLEX_Pages, 0, tags);
    LONG *pw = (LONG *)GetTagData(HTMLEX_PageWidth, 0, tags);
    LONG *ph = (LONG *)GetTagData(HTMLEX_PageHeight, 0, tags);
    struct PrintArgs a;

    h_memset(&a, 0, sizeof(a));
    a.d = INST_DATA(cl, o);
    a.result = -1;
    a.opt.dpi = GetTagData(HTMLEX_DPI, 150, tags);
    a.opt.paper_w = GetTagData(HTMLEX_PaperWidth, 595, tags);
    a.opt.paper_h = GetTagData(HTMLEX_PaperHeight, 842, tags);
    a.opt.margin[0] = GetTagData(HTMLEX_MarginLeft, 57, tags);
    a.opt.margin[1] = GetTagData(HTMLEX_MarginTop, 57, tags);
    a.opt.margin[2] = GetTagData(HTMLEX_MarginRight, 57, tags);
    a.opt.margin[3] = GetTagData(HTMLEX_MarginBottom, 57, tags);
    a.opt.font_size = GetTagData(HTMLEX_FontSize, 100, tags);
    a.opt.backgrounds = GetTagData(HTMLEX_Backgrounds, TRUE, tags) != 0;
    a.opt.fit_images = a.d->env.fit_images;
    a.opt.footer = (const char *)GetTagData(HTMLEX_Footer, 0, tags);

    ObtainSemaphore(&a.d->lock);
    if (a.d->print) tp_end(a.d->print);
    a.d->print = NULL;
    if (a.d->doc && a.d->stack) call_big_stack(a.d->stack, print_begin_func, &a);
    ReleaseSemaphore(&a.d->lock);
    if (pages) *pages = a.pages;
    if (pw) *pw = a.w;
    if (ph) *ph = a.h;
    return a.result;
}

/* may run on the printer's task: the semaphore keeps the screen output
 * away from the shared FreeType faces meanwhile                         */
static ULONG print_render(Class *cl, Object *o, struct hmPrintRender *msg)
{
    struct PrintArgs a;
    a.d = INST_DATA(cl, o);
    a.msg = msg;
    a.result = FALSE;
    ObtainSemaphore(&a.d->lock);
    if (a.d->print && a.d->stack) call_big_stack(a.d->stack, print_render_func, &a);
    ReleaseSemaphore(&a.d->lock);
    return (ULONG)a.result;
}

ULONG html_dispatcher(Class *cl __asm("a0"), Object *o __asm("a2"), Msg msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)om_new(cl, o, (struct opSet *)msg);
    case OM_DISPOSE:
        om_dispose(cl, o);
        break;
    case OM_SET:
    case OM_UPDATE:
        return dsm(cl, o, msg) + set_attrs(cl, o, (struct opSet *)msg);
    case OM_GET:
        return get_attr(cl, o, (struct opGet *)msg);
    case GM_DOMAIN:
        return gm_domain(cl, o, (struct gpDomain *)msg);
    case GM_LAYOUT:
        return gm_layout(cl, o, (struct gpLayout *)msg);
    case GM_RENDER:
        render(INST_DATA(cl, o), (struct Gadget *)o, ((struct gpRender *)msg)->gpr_GInfo,
               ((struct gpRender *)msg)->gpr_RPort, ((struct gpRender *)msg)->gpr_Redraw == GREDRAW_REDRAW);
        return 1;
    case GM_HITTEST:
        return gm_hittest(cl, o, (struct gpHitTest *)msg);
    case GM_GOACTIVE:
        return gm_goactive(cl, o, (struct gpInput *)msg);
    case GM_HANDLEINPUT:
        return gm_handleinput(cl, o, (struct gpInput *)msg);
    case GM_GOINACTIVE:
        return gm_goinactive(cl, o, (struct gpGoInactive *)msg);
    case HTMLM_Export:
        return (ULONG)export_doc(cl, o, (struct hmExport *)msg);
    case HTMLM_PrintBegin:
        return (ULONG)print_begin(cl, o, (struct hmPrintBegin *)msg);
    case HTMLM_PrintRender:
        return print_render(cl, o, (struct hmPrintRender *)msg);
    case HTMLM_PrintEnd: {
        struct HTMLData *d = INST_DATA(cl, o);
        ObtainSemaphore(&d->lock);
        if (d->print) tp_end(d->print);
        d->print = NULL;
        ReleaseSemaphore(&d->lock);
        return 0;
    }
    }
    return dsm(cl, o, msg);
}
