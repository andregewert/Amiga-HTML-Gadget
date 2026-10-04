/*
 * html_class.c - BOOPSI dispatcher of html.gadget
 *
 * Rendering goes through an off-screen bitmap with its own layer, so the
 * gadget clips correctly and scrolls without flicker. The layout engine is
 * recursive and may be called on intuition's (small) input.device stack,
 * therefore it always runs on a private stack via StackSwap().
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
#include <graphics/gfxmacros.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/layers.h>
#include <graphics/clip.h>
#include <graphics/view.h>
#include <graphics/scale.h>
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
#include <proto/layers.h>
#include <proto/utility.h>
#include <proto/diskfont.h>
#include <diskfont/diskfont.h>
#include <proto/datatypes.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

#include "gadgets/html.h"
#include "html_core.h"
#include "html_private.h"
#include "html_export.h"
#include "html_print.h"

#define LAYOUT_STACK  (64 * 1024)
#define MAXPENS       64
#define FRAMEW        1           /* frame thickness */

#define DEF_LINK   0x0000EEUL
#define DEF_VLINK  0x551A8BUL
#define DEF_ALINK  0xFF0000UL
#define COL_LIGHT  0xE0E0E0UL
#define COL_DARK   0x808080UL

/* a picture loaded through datatypes.library for an <img> */
struct HImage {
    struct HImage *next;
    char           path[256];
    LONG           reqw, reqh;       /* size requested by width/height (0 = natural) */
    Object        *dto;
    struct BitMap *bm;
    PLANEPTR       mask;
    LONG           w, h;
    struct Screen *scr;              /* screen the picture is remapped for */
    struct BitMap *ownbm;            /* scaled or enlarged copy made by us, or NULL */
    PLANEPTR       ownmask;
    LONG           maskw, maskh;
    BOOL           tile;             /* background picture (tiled) */
};

struct PenEntry {
    ULONG rgb;
    LONG  pen;
};

struct HTMLData {
    struct SignalSemaphore lock;
    struct HDoc      *doc;
    struct HLayout   *lay;
    STRPTR            source;
    struct HImage    *images;            /* pictures of the current document */
    struct HImage    *bgimg;             /* <body background> (in 'images') */
    BOOL              loadimages;
    LONG              imgtotal, imgloaded;
    char              imgerror[200];     /* why the first picture failed, "" if none */
    char              basedir[256];
    STRPTR            linkurl;

    LONG              top, left;
    LONG              visw, vish;
    LONG              laywidth;          /* width of the current layout, -1 = none */
    LONG              margin;
    BOOL              syscolors, autoanchors, frame;
    LONG              activelink;        /* link under the pressed mouse button */
    BOOL              pressed;
    BOOL              havebox;           /* size known (GM_LAYOUT/GM_RENDER seen) */
    struct HSel       sel;               /* text selection */
    BOOL              selecting;         /* mouse button held for selecting */
    ULONG             clicksecs, clickmicros;
    LONG              clickitem;
    STRPTR            seltext;           /* returned by HTML_SelectedText */
    char              anchor[128];       /* pending HTML_Anchor */

    /* fonts */
    char              propname[64], fixname[64];
    UWORD             propsize, fixsize;
    struct FontSizes *propsizes, *fixsizes;
    struct TextFont  *own[HF_NUM];       /* fonts we opened */
    struct TextFont  *use[HF_NUM];       /* fonts used (may be fallbacks) */
    struct RastPort   mrp;               /* for measuring */
    struct TextFont  *mfont;
    int               mstyle;
    struct HEnv       env;

    /* pens */
    struct ColorMap  *cm;
    struct PenEntry   pens[MAXPENS];
    int               npens;

    /* off-screen buffer */
    struct BitMap    *bm;
    struct Layer_Info *li;
    struct Layer     *layer;
    LONG              bmw, bmh;
    struct BitMap    *friend;

    struct Screen    *scr;               /* screen of the last GM_RENDER (for printing) */
    struct BMPrint   *print;             /* HTMLM_PrintBegin .. HTMLM_PrintEnd */
};

const ULONG html_inst_size = sizeof(struct HTMLData);

static void print_free(struct HTMLData *d);

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

/* gcc may emit calls to these for structure copies */
void *memcpy(void *d, const void *s, size_t n) { h_memcpy(d, s, (long)n); return d; }
void *memset(void *d, int c, size_t n) { h_memset(d, c, (long)n); return d; }

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
    "   jsr     -732(a6)\n"          /* StackSwap() */
    "   move.l  d2,-(sp)\n"
    "   jsr     (a3)\n"
    "   addq.l  #4,sp\n"
    "   move.l  d0,d3\n"
    "   move.l  _SysBase,a6\n"
    "   move.l  a2,a0\n"
    "   jsr     -732(a6)\n"          /* StackSwap() back */
    "   move.l  d3,d0\n"
    "   movem.l (sp)+,d2-d3/a2-a3/a6\n"
    "   rts\n");

static BOOL call_big_stack(ULONG (*func)(APTR), APTR arg)
{
    struct StackSwapStruct sss;
    UBYTE *stack;

    if (!(stack = AllocVec(LAYOUT_STACK, MEMF_ANY))) return FALSE;
    sss.stk_Lower = stack;
    sss.stk_Upper = (ULONG)(stack + LAYOUT_STACK);
    sss.stk_Pointer = (APTR)(stack + LAYOUT_STACK);
    html_swapcall(&sss, func, arg);
    FreeVec(stack);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */

static long text_width_cb(void *user, int font, int style, const char *s, long len)
{
    struct HTMLData *d = user;
    struct TextFont *tf = d->use[font];
    struct TextExtent te;
    long w;

    if (d->mfont != tf) {
        SetFont(&d->mrp, tf);
        d->mfont = tf;
        d->mstyle = -1;
    }
    if (d->mstyle != style) {
        SetSoftStyle(&d->mrp, style, AskSoftStyle(&d->mrp));
        d->mstyle = style;
    }
    if (len > 32000) len = 32000;
    TextExtent(&d->mrp, (STRPTR)s, len, &te);
    w = te.te_Width;
    if (te.te_Extent.MaxX + 1 > w) w = te.te_Extent.MaxX + 1;
    return w;
}

/* designed sizes of a bitmap font family, so that bigger HTML font sizes
 * use real fonts instead of ugly scaled ones                             */
#define MAXSIZES 32
struct FontSizes {
    UWORD n;
    BOOL  scalable;              /* outline font (.otag): every size is fine */
    UWORD size[MAXSIZES];        /* ascending */
};

static void add_size(struct FontSizes *fs, UWORD s)
{
    int i, j;
    for (i = 0; i < fs->n; i++) {
        if (fs->size[i] == s) return;
        if (fs->size[i] > s) break;
    }
    if (fs->n == MAXSIZES) return;
    for (j = fs->n; j > i; j--) fs->size[j] = fs->size[j - 1];
    fs->size[i] = s;
    fs->n++;
}

/* app context: asks diskfont.library which sizes of 'name' exist */
static void scan_sizes(CONST_STRPTR name, struct FontSizes *fs)
{
    struct AvailFontsHeader *afh;
    struct AvailFonts *af;
    LONG bufsize = 4096, more;
    int i;

    h_memset(fs, 0, sizeof(*fs));
    if (!DiskfontBase) return;
    for (;;) {
        if (!(afh = AllocVec(bufsize, MEMF_ANY))) return;
        more = AvailFonts(afh, bufsize, AFF_MEMORY | AFF_DISK);
        if (!more) break;
        FreeVec(afh);
        bufsize += more;
    }
    af = (struct AvailFonts *)(afh + 1);
    for (i = 0; i < afh->afh_NumEntries; i++, af++) {
        struct TextAttr *ta = &af->af_Attr;
        if (!ta->ta_Name || h_stricmp((const char *)ta->ta_Name, (const char *)name)) continue;
        if (ta->ta_Flags & FPF_REMOVED) continue;
        if (af->af_Type & AFF_OTAG) { fs->scalable = TRUE; continue; }
        if (af->af_Type & AFF_SCALED) continue;
        /* disk fonts are designed; in memory only designed ones count,
         * not sizes somebody else had scaled before                     */
        if ((af->af_Type & AFF_DISK) || (ta->ta_Flags & FPF_DESIGNED)) add_size(fs, ta->ta_YSize);
    }
    FreeVec(afh);
}

/* nearest designed size (ties: the smaller one); 0 = no preference */
static UWORD snap_size(struct FontSizes *fs, UWORD want)
{
    UWORD best = 0;
    int i;
    if (!fs || fs->scalable || !fs->n) return 0;
    for (i = 0; i < fs->n; i++) {
        UWORD s = fs->size[i];
        LONG d = (LONG)s - want, bd = (LONG)best - want;
        if (d < 0) d = -d;
        if (bd < 0) bd = -bd;
        if (!best || d < bd) best = s;
    }
    return best;
}

static UWORD level_size(UWORD base, int level)
{
    static const UBYTE f[7] = { 8, 10, 12, 14, 18, 24, 36 };   /* in 1/12 */
    UWORD s = (UWORD)((base * f[level - 1] + 6) / 12);
    UWORD min = base < 8 ? base : 8;
    return s < min ? min : s;
}

static struct TextFont *open_font(STRPTR name, UWORD size, struct FontSizes *fs)
{
    struct TextAttr ta;
    struct TextFont *tf = NULL;
    UWORD snapped = snap_size(fs, size);

    ta.ta_Name = name;
    ta.ta_YSize = snapped ? snapped : size;
    ta.ta_Style = FS_NORMAL;
    ta.ta_Flags = snapped ? FPF_DESIGNED : 0;
    if (DiskfontBase) tf = OpenDiskFont(&ta);
    if (!tf && snapped) {                 /* should not happen, but be tolerant */
        ta.ta_Flags = 0;
        if (DiskfontBase) tf = OpenDiskFont(&ta);
    }
    if (!tf) tf = OpenFont(&ta);
    return tf;
}

static void copy_name(char *dst, CONST_STRPTR src, int max)
{
    int i;
    for (i = 0; src && src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* opens all fonts in 'mask' that are not open yet; app context only */
static void open_fonts(struct HTMLData *d, ULONG mask)
{
    struct TextFont *tf[HF_NUM];
    int i, changed = 0;

    html_open_diskfont();
    mask |= 1UL << HF_INDEX(0, 3);
    for (i = 0; i < HF_NUM; i++) {
        tf[i] = NULL;
        if (!(mask & (1UL << i)) || d->own[i]) continue;
        if (i < 7) tf[i] = open_font((STRPTR)d->propname, level_size(d->propsize, i + 1), d->propsizes);
        else       tf[i] = open_font((STRPTR)d->fixname, level_size(d->fixsize, i - 6), d->fixsizes);
        if (tf[i]) changed = 1;
    }
    if (!changed && d->env.font_height[HF_INDEX(0, 3)]) return;

    ObtainSemaphore(&d->lock);
    for (i = 0; i < HF_NUM; i++) if (tf[i]) d->own[i] = tf[i];
    for (i = 0; i < HF_NUM; i++) {
        struct TextFont *f = d->own[i];
        if (!f) f = d->own[i < 7 ? HF_INDEX(0, 3) : HF_INDEX(1, 3)];
        if (!f) f = d->own[HF_INDEX(0, 3)];
        if (!f) f = GfxBase->DefaultFont;
        d->use[i] = f;
        d->env.font_height[i] = f->tf_YSize;
        d->env.font_baseline[i] = f->tf_Baseline;
    }
    d->mfont = NULL;
    d->env.generation++;
    if (!d->env.generation) d->env.generation = 1;
    d->laywidth = -1;
    ReleaseSemaphore(&d->lock);
}

static void close_fonts(struct HTMLData *d)
{
    int i;
    for (i = 0; i < HF_NUM; i++)
        if (d->own[i]) { CloseFont(d->own[i]); d->own[i] = NULL; }
}

/* ------------------------------------------------------------------ */
/* pens                                                                */

static void release_pens(struct HTMLData *d)
{
    int i;
    if (d->cm)
        for (i = 0; i < d->npens; i++)
            if (d->pens[i].pen >= 0) ReleasePen(d->cm, d->pens[i].pen);
    d->npens = 0;
    d->cm = NULL;
}

static LONG get_pen(struct HTMLData *d, struct GadgetInfo *gi, ULONG col, int deflt)
{
    struct ColorMap *cm = gi->gi_Screen->ViewPort.ColorMap;
    UWORD *dpens = gi->gi_DrInfo->dri_Pens;
    ULONG r, g, b;
    LONG pen;
    int i;

    if (col & COL_PEN) return dpens[col & 0xFF];
    if (!COL_IS_RGB(col)) return dpens[deflt];
    if (cm != d->cm) {
        release_pens(d);
        d->cm = cm;
    }
    for (i = 0; i < d->npens; i++)
        if (d->pens[i].rgb == col) return d->pens[i].pen >= 0 ? d->pens[i].pen : dpens[deflt];

    r = (col >> 16) & 0xFF; g = (col >> 8) & 0xFF; b = col & 0xFF;
    pen = ObtainBestPen(cm, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL,
                        OBP_Precision, PRECISION_IMAGE, TAG_DONE);
    if (d->npens < MAXPENS) {
        d->pens[d->npens].rgb = col;
        d->pens[d->npens].pen = pen;
        d->npens++;
    } else if (pen >= 0) {
        ReleasePen(cm, pen);          /* cache full: use and forget */
        pen = FindColor(cm, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL, -1);
    }
    return pen >= 0 ? pen : dpens[deflt];
}

static ULONG link_color(struct HTMLData *d, LONG link)
{
    struct HDoc *doc = d->doc;
    if (link >= 0 && link == d->activelink && d->pressed)
        return doc->alink != COL_NONE ? doc->alink : DEF_ALINK;
    if (link >= 0 && link < doc->nlinks && doc->visited[link])
        return doc->vlink != COL_NONE ? doc->vlink : DEF_VLINK;
    return doc->link != COL_NONE ? doc->link : DEF_LINK;
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

/* returns TRUE if the scroll position was changed by a pending anchor */
static BOOL ensure_layout(struct HTMLData *d)
{
    struct LayoutArgs la;

    if (!d->doc || !d->havebox) return FALSE;
    if (d->laywidth != d->visw) {
        if (d->lay) { html_free_layout(d->lay); d->lay = NULL; }
        la.d = d;
        la.width = d->visw;
        la.result = NULL;
        call_big_stack(layout_func, &la);
        d->lay = la.result;
        d->laywidth = d->visw;
        d->sel.active = 0;                /* item numbers changed */
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

/* ------------------------------------------------------------------ */
/* rendering                                                           */

static void free_buffer(struct HTMLData *d)
{
    if (d->layer) { DeleteLayer(0, d->layer); d->layer = NULL; }
    if (d->li) { DisposeLayerInfo(d->li); d->li = NULL; }
    if (d->bm) { WaitBlit(); FreeBitMap(d->bm); d->bm = NULL; }
    d->bmw = d->bmh = 0;
}

static struct RastPort *get_buffer(struct HTMLData *d, struct GadgetInfo *gi)
{
    struct BitMap *friend = gi->gi_Screen->RastPort.BitMap;

    if (d->layer && d->bmw == d->visw && d->bmh == d->vish && d->friend == friend)
        return d->layer->rp;
    free_buffer(d);
    d->bm = AllocBitMap(d->visw, d->vish, GetBitMapAttr(friend, BMA_DEPTH), BMF_MINPLANES, friend);
    if (d->bm && (d->li = NewLayerInfo()))
        d->layer = CreateUpfrontLayer(d->li, d->bm, 0, 0, d->visw - 1, d->vish - 1, LAYERSIMPLE, NULL);
    if (!d->layer) { free_buffer(d); return NULL; }
    d->bmw = d->visw;
    d->bmh = d->vish;
    d->friend = friend;
    return d->layer->rp;
}

static WORD clampw(LONG v)
{
    return (WORD)(v < -1000 ? -1000 : v > 20000 ? 20000 : v);
}

static void rect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1)
{
    if (x1 < x0 || y1 < y0) return;
    RectFill(rp, clampw(x0), clampw(y0), clampw(x1), clampw(y1));
}

static void bevel(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h, LONG light, LONG dark)
{
    if (w < 2 || h < 2) return;
    SetAPen(rp, light);
    rect(rp, x, y, x + w - 1, y);
    rect(rp, x, y, x, y + h - 1);
    SetAPen(rp, dark);
    rect(rp, x + 1, y + h - 1, x + w - 1, y + h - 1);
    rect(rp, x + w - 1, y + 1, x + w - 1, y + h - 1);
}

/* tiles a background picture over rp rectangle x0..x1/y0..y1 (already
 * clipped to the visible area); ox/oy is the pattern origin             */
static void tile_rp(struct RastPort *rp, struct HImage *im, LONG ox, LONG oy,
                    LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG tx, ty, sx, sy;

    if (x0 > x1 || y0 > y1 || im->w <= 0 || im->h <= 0) return;
    sx = (x0 - ox) % im->w; if (sx < 0) sx += im->w;
    sy = (y0 - oy) % im->h; if (sy < 0) sy += im->h;
    for (ty = y0 - sy; ty <= y1; ty += im->h) {
        for (tx = x0 - sx; tx <= x1; tx += im->w) {
            LONG bx = tx < x0 ? x0 : tx, by = ty < y0 ? y0 : ty;
            LONG ex = tx + im->w - 1 > x1 ? x1 : tx + im->w - 1;
            LONG ey = ty + im->h - 1 > y1 ? y1 : ty + im->h - 1;
            if (ex < bx || ey < by) continue;
            if (im->mask)
                BltMaskBitMapRastPort(im->bm, bx - tx, by - ty, rp, bx, by, ex - bx + 1, ey - by + 1,
                                      0xE0, im->mask);
            else
                BltBitMapRastPort(im->bm, bx - tx, by - ty, rp, bx, by, ex - bx + 1, ey - by + 1, 0xC0);
        }
    }
}

static BOOL image_usable(struct HImage *im, struct GadgetInfo *gi)
{
    return im && im->bm && im->scr == gi->gi_Screen;
}

/* IT_CHECK without blending: the pixels covered at least half */
struct CheckCtx {
    struct HTMLData   *d;
    struct GadgetInfo *gi;
    struct RastPort   *rp;
    LONG               dx, dy;
    ULONG              rgb;
    LONG               pen;
};

static void check_span(void *ctx, long x0, long x1, long y, unsigned long rgb, unsigned alpha)
{
    struct CheckCtx *c = ctx;
    if (alpha < 128) return;
    if (rgb != c->rgb || c->pen < 0) {
        c->rgb = rgb;
        c->pen = get_pen(c->d, c->gi, rgb, TEXTPEN);
        SetAPen(c->rp, c->pen);
    }
    rect(c->rp, x0 + c->dx, y + c->dy, x1 + c->dx, y + c->dy);
}

/* draws the document into rp; (ox,oy) is where the visible area starts */
static void draw_content(struct HTMLData *d, struct GadgetInfo *gi, struct RastPort *rp, LONG ox, LONG oy)
{
    struct HLayout *lay = d->lay;
    LONG light, dark, i, dx = ox - d->left, dy = oy - d->top;
    LONG ytop = d->top, ybot = d->top + d->vish, xl = d->left, xr = d->left + d->visw;
    ULONG bg = lay ? lay->bgcolor : (d->syscolors ? COL_PEN | BACKGROUNDPEN : 0xFFFFFF);

    SetDrMd(rp, JAM1);
    SetAPen(rp, get_pen(d, gi, bg, BACKGROUNDPEN));
    rect(rp, ox, oy, ox + d->visw - 1, oy + d->vish - 1);
    if (lay && image_usable(d->bgimg, gi))          /* <body background>, scrolls with the page */
        tile_rp(rp, d->bgimg, dx, dy, ox, oy, ox + d->visw - 1, oy + d->vish - 1);
    if (!lay) return;

    light = get_pen(d, gi, COL_LIGHT, SHINEPEN);
    dark = get_pen(d, gi, COL_DARK, SHADOWPEN);

    for (i = 0; i < lay->nitems; i++) {
        struct HItem *it = &lay->items[i];
        ULONG col = it->color;
        LONG x, y;

        if (it->y >= ybot || it->y + it->h <= ytop || it->x >= xr || it->x + it->w <= xl) continue;
        if (col == COL_LINK) col = link_color(d, it->link);
        x = it->x + dx;
        y = it->y + dy;

        switch (it->type) {
        case IT_TEXT: {
            struct TextFont *tf = d->use[it->font];
            long c0, c1;
            BOOL part = html_sel_part(&d->sel, lay, i, &c0, &c1);
            if (part) {                   /* selection background */
                LONG hx0 = it->x + html_char_x(&d->env, it, c0) + dx;
                LONG hx1 = html_sel_right(&d->sel, lay, &d->env, i, c1) + dx;
                SetAPen(rp, gi->gi_DrInfo->dri_Pens[FILLPEN]);
                rect(rp, hx0, y, hx1 - 1, y + it->h - 1);
            }
            SetFont(rp, tf);
            /* bold/italic by the font system, the underline is drawn below */
            SetSoftStyle(rp, it->style & (FSF_BOLD | FSF_ITALIC), AskSoftStyle(rp));
            SetAPen(rp, get_pen(d, gi, col, TEXTPEN));
            Move(rp, clampw(x), clampw(y + it->base));
            Text(rp, (STRPTR)it->s, it->len > 32000 ? 32000 : it->len);
            if (part && c1 > c0) {        /* selected characters again in FILLTEXTPEN */
                SetAPen(rp, gi->gi_DrInfo->dri_Pens[FILLTEXTPEN]);
                Move(rp, clampw(x + html_char_x(&d->env, it, c0)), clampw(y + it->base));
                Text(rp, (STRPTR)it->s + c0, c1 - c0);
            }
            SetAPen(rp, get_pen(d, gi, col, TEXTPEN));
            if (it->style & HS_UNDERLINED) {  /* without gaps between the words of a link */
                LONG uy = y + it->base + 1;
                rect(rp, x, uy, html_underline_end(lay, i) + dx - 1, uy);
            }
            if (it->style & HS_STRIKE) {
                LONG sy = y + it->base - tf->tf_Baseline / 3;
                rect(rp, x, sy, x + it->w - 1, sy);
            }
            break;
        }
        case IT_RECT:
            if (col != COL_NONE) {
                SetAPen(rp, get_pen(d, gi, col, BACKGROUNDPEN));
                rect(rp, x, y, x + it->w - 1, y + it->h - 1);
            }
            if (image_usable(it->img, gi)) {                /* table/cell background */
                LONG x0 = x < ox ? ox : x, y0 = y < oy ? oy : y;
                LONG x1 = x + it->w - 1, y1 = y + it->h - 1;
                if (x1 > ox + d->visw - 1) x1 = ox + d->visw - 1;
                if (y1 > oy + d->vish - 1) y1 = oy + d->vish - 1;
                tile_rp(rp, it->img, x, y, x0, y0, x1, y1);
            }
            break;
        case IT_HRULE:
            if (it->style & HR_NOSHADE) {
                SetAPen(rp, get_pen(d, gi, col, TEXTPEN));
                rect(rp, x, y, x + it->w - 1, y + it->h - 1);
            } else {
                bevel(rp, x, y, it->w, it->h, dark, light);
            }
            break;
        case IT_FRAME:
            if (it->style & FR_RAISED) bevel(rp, x, y, it->w, it->h, light, dark);
            else bevel(rp, x, y, it->w, it->h, dark, light);
            break;
        case IT_IMAGE: {
            struct HImage *im = it->img;
            if (im && im->bm && im->scr == gi->gi_Screen) {
                LONG sx = 0, sy = 0, bx = x, by = y;
                LONG w = it->w < im->w ? it->w : im->w, h = it->h < im->h ? it->h : im->h;
                if (bx < ox) { sx += ox - bx; w -= ox - bx; bx = ox; }
                if (by < oy) { sy += oy - by; h -= oy - by; by = oy; }
                if (bx + w > ox + d->visw) w = ox + d->visw - bx;
                if (by + h > oy + d->vish) h = oy + d->vish - by;
                if (w > 0 && h > 0) {
                    if (im->mask)
                        BltMaskBitMapRastPort(im->bm, sx, sy, rp, bx, by, w, h, 0xE0, im->mask);
                    else
                        BltBitMapRastPort(im->bm, sx, sy, rp, bx, by, w, h, 0xC0);
                }
            } else {
                bevel(rp, x, y, it->w, it->h, dark, light);
            }
            break;
        }
        case IT_CHECK: {
            struct CheckCtx c;
            c.d = d;
            c.gi = gi;
            c.rp = rp;
            c.dx = dx;
            c.dy = dy;
            c.rgb = 0;
            c.pen = -1;
            html_check_paint(it, check_span, &c);
            break;
        }
        case IT_BULLET: {
            LONG s = it->w;
            SetAPen(rp, get_pen(d, gi, col, TEXTPEN));
            if (it->style == BUL_SQUARE) {
                rect(rp, x, y, x + s - 1, y + s - 1);
            } else if (it->style == BUL_CIRCLE) {
                rect(rp, x + 1, y, x + s - 2, y);
                rect(rp, x + 1, y + s - 1, x + s - 2, y + s - 1);
                rect(rp, x, y + 1, x, y + s - 2);
                rect(rp, x + s - 1, y + 1, x + s - 1, y + s - 2);
            } else {
                rect(rp, x + 1, y, x + s - 2, y + s - 1);
                rect(rp, x, y + 1, x + s - 1, y + s - 2);
            }
            break;
        }
        }
    }
    SetSoftStyle(rp, 0, AskSoftStyle(rp));
}

static void render(struct HTMLData *d, struct Gadget *g, struct GadgetInfo *gi, struct RastPort *rp, BOOL full)
{
    struct IBox box;
    struct RastPort *brp;
    LONG ix, iy;

    if (!gi || !rp) return;
    gadget_box(g, gi, &box);
    if (box.Width <= 2 * inset(d) || box.Height <= 2 * inset(d)) return;

    ObtainSemaphore(&d->lock);
    d->scr = gi->gi_Screen;
    update_visible(d, &box);
    ensure_layout(d);
    ix = box.Left + inset(d);
    iy = box.Top + inset(d);

    if (full && d->frame) {
        UWORD *pens = gi->gi_DrInfo->dri_Pens;
        bevel(rp, box.Left, box.Top, box.Width, box.Height, pens[SHADOWPEN], pens[SHINEPEN]);
    }

    if ((brp = get_buffer(d, gi))) {
        draw_content(d, gi, brp, 0, 0);
        BltBitMapRastPort(d->bm, 0, 0, rp, ix, iy, d->visw, d->vish, 0xC0);
    } else if (rp->Layer) {
        /* not enough memory for the buffer: draw directly with clipping */
        struct Region *reg = NewRegion(), *old;
        struct Rectangle r;
        if (reg) {
            r.MinX = ix; r.MinY = iy; r.MaxX = ix + d->visw - 1; r.MaxY = iy + d->vish - 1;
            OrRectRegion(reg, &r);
            old = InstallClipRegion(rp->Layer, reg);
            draw_content(d, gi, rp, ix, iy);
            InstallClipRegion(rp->Layer, old);
            DisposeRegion(reg);
        }
    }
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
/* documents                                                           */

/* ------------------------------------------------------------------ */
/* pictures (datatypes)                                                */

static void free_images(struct HImage *im)
{
    while (im) {
        struct HImage *next = im->next;
        if (im->ownbm) { WaitBlit(); FreeBitMap(im->ownbm); }
        if (im->ownmask) FreeRaster(im->ownmask, im->maskw, im->maskh);
        if (im->dto) DisposeDTObject(im->dto);
        FreeVec(im);
        im = next;
    }
}

/* builds an AmigaDOS path for src relative to base; FALSE for URLs */
static BOOL resolve_path(CONST_STRPTR base, const char *src, char *out, LONG max)
{
    char rel[256];
    const char *p;
    LONG n = 0;

    if (!src || !*src || !h_strnicmp(src, "data:", 5)) return FALSE;
    for (p = src; *p; p++) if (p[0] == ':' && p[1] == '/' && p[2] == '/') break;
    if (*p) {
        if (h_strnicmp(src, "file:///", 8)) return FALSE;     /* http:// etc. */
        src += 8;                                               /* file:///Work/x -> Work/x */
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
    struct HImage *list;
    struct HImage *bgimg;
    LONG           total, loaded;
    char           err[200];
};

/* "<path>: <step>: <reason>" into il->err */
static void image_error(struct ImgLoad *il, const char *path, const char *step, LONG code)
{
    char reason[100];
    LONG n = 0;
    const char *p;

    if (il->err[0]) return;          /* keep the first error */
    reason[0] = 0;
    if (code >= DTERROR_UNKNOWN_DATATYPE && DataTypesBase) {
        /* some datatypes texts are format strings ("... %s ...") */
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
    }
    else if (code)
        Fault(code, NULL, (STRPTR)reason, sizeof(reason));
    for (p = path; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    for (p = ": "; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    for (p = step; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    if (reason[0]) {
        for (p = ": "; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
        for (p = reason; *p && n < (LONG)sizeof(il->err) - 1; p++) il->err[n++] = *p;
    }
    il->err[n] = 0;
}

/* scales the picture with graphics.library, used when picture.datatype
 * did not honour PDTM_SCALE                                              */
static void scale_image(struct HImage *im, LONG nw, LONG nh)
{
    struct BitScaleArgs bsa;
    struct BitMap *dst, smask, dmask;
    ULONG depth = GetBitMapAttr(im->bm, BMA_DEPTH);
    BOOL planar = (GetBitMapAttr(im->bm, BMA_FLAGS) & BMF_STANDARD) != 0;

    if (im->w < 1 || im->h < 1 || im->w > 16383 || im->h > 16383 || nw > 16383 || nh > 16383) return;
    /* planar: a plain (non-interleaved) bitmap, so the mask layout matches;
     * RTG: a friend bitmap in the screen's pixel format                    */
    if (!(dst = AllocBitMap(nw, nh, depth, BMF_CLEAR | BMF_MINPLANES, planar ? NULL : im->bm))) return;

    h_memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_SrcWidth = im->w;
    bsa.bsa_SrcHeight = im->h;
    bsa.bsa_XSrcFactor = im->w;
    bsa.bsa_YSrcFactor = im->h;
    bsa.bsa_XDestFactor = nw;
    bsa.bsa_YDestFactor = nh;
    bsa.bsa_SrcBitMap = im->bm;
    bsa.bsa_DestBitMap = dst;
    BitMapScale(&bsa);

    if (im->mask) {
        PLANEPTR nm = AllocRaster(nw, nh);
        if (nm) {
            h_memset(nm, 0, RASSIZE(nw, nh));
            InitBitMap(&smask, 1, im->w, im->h);
            smask.Planes[0] = im->mask;
            InitBitMap(&dmask, 1, nw, nh);
            dmask.Planes[0] = nm;
            bsa.bsa_SrcBitMap = &smask;
            bsa.bsa_DestBitMap = &dmask;
            BitMapScale(&bsa);
            im->ownmask = nm;
            im->maskw = nw;
            im->maskh = nh;
        }
        im->mask = nm;             /* without memory: drawn without mask */
    }
    WaitBlit();
    im->ownbm = dst;
    im->bm = dst;
    im->w = nw;
    im->h = nh;
}

/* small background tiles are copied into a bigger bitmap once, so that
 * drawing needs a few blits instead of hundreds                          */
static void expand_tile(struct HImage *im)
{
    struct BitMap *dst;
    ULONG depth = GetBitMapAttr(im->bm, BMA_DEPTH);
    BOOL planar = (GetBitMapAttr(im->bm, BMA_FLAGS) & BMF_STANDARD) != 0;
    LONG nw, nh, x, y;

    if (im->mask || im->w <= 0 || im->h <= 0 || (im->w >= 64 && im->h >= 64)) return;
    nw = im->w * ((128 + im->w - 1) / im->w);
    nh = im->h * ((128 + im->h - 1) / im->h);
    if (!(dst = AllocBitMap(nw, nh, depth, BMF_MINPLANES, planar ? NULL : im->bm))) return;
    for (y = 0; y < nh; y += im->h)
        for (x = 0; x < nw; x += im->w)
            BltBitMap(im->bm, 0, 0, dst, x, y, im->w, im->h, 0xC0, 0xFF, NULL);
    WaitBlit();
    im->ownbm = dst;
    im->bm = dst;
    im->w = nw;
    im->h = nh;
}

static struct HImage *load_image(struct ImgLoad *il, const char *path, LONG reqw, LONG reqh,
                                 struct Screen *scr, BOOL tile)
{
    struct HImage **list = &il->list;
    struct HImage *im;
    struct BitMapHeader *bmhd = NULL;
    struct gpLayout gpl;
    struct BitMap *bm = NULL;

    for (im = *list; im; im = im->next)
        if (im->reqw == reqw && im->reqh == reqh && im->tile == tile && !h_stricmp(im->path, path))
            return im->bm ? im : NULL;

    if (!(im = AllocVec(sizeof(*im), MEMF_ANY | MEMF_CLEAR))) return NULL;
    copy_name(im->path, (CONST_STRPTR)path, sizeof(im->path));
    im->reqw = reqw;
    im->reqh = reqh;
    im->tile = tile;
    im->scr = scr;
    im->next = *list;
    *list = im;

    im->dto = NewDTObject((APTR)path,
        DTA_GroupID,           GID_PICTURE,
        PDTA_Remap,            TRUE,
        PDTA_Screen,           (ULONG)scr,
        PDTA_DestMode,         PMODE_V43,
        PDTA_UseFriendBitMap,  TRUE,
        PDTA_FreeSourceBitMap, TRUE,
        OBP_Precision,         PRECISION_IMAGE,
        TAG_DONE);
    if (!im->dto) {
        image_error(il, path, "NewDTObject", IoErr());
        return NULL;
    }
    GetDTAttrs(im->dto, PDTA_BitMapHeader, (ULONG)&bmhd, TAG_DONE);
    if (bmhd) {
        im->w = bmhd->bmh_Width;
        im->h = bmhd->bmh_Height;
    }

    /* width/height attributes: let the datatype scale (V45+); if it
     * cannot, the picture is shown in its natural size and clipped      */
    if (bmhd && reqw > 0 && reqh > 0 && (reqw != im->w || reqh != im->h) && reqw <= 4000 && reqh <= 4000) {
        struct pdtScale ps;
        ps.MethodID = PDTM_SCALE;
        ps.ps_NewWidth = reqw;
        ps.ps_NewHeight = reqh;
        ps.ps_Flags = 0;
        DoDTMethodA(im->dto, NULL, NULL, (Msg)&ps);
    }

    /* remap for the screen; the return value is not a reliable success
     * indicator, the destination bitmap is                              */
    gpl.MethodID = DTM_PROCLAYOUT;
    gpl.gpl_GInfo = NULL;
    gpl.gpl_Initial = 1;
    SetIoErr(0);
    DoDTMethodA(im->dto, NULL, NULL, (Msg)&gpl);
    bmhd = NULL;
    GetDTAttrs(im->dto, PDTA_BitMapHeader, (ULONG)&bmhd, PDTA_DestBitMap, (ULONG)&bm,
               PDTA_MaskPlane, (ULONG)&im->mask, TAG_DONE);
    if (!bm) GetDTAttrs(im->dto, PDTA_BitMap, (ULONG)&bm, TAG_DONE);
    if (!bm) {
        image_error(il, path, "DTM_PROCLAYOUT liefert keine Bitmap", IoErr());
        im->mask = NULL;
        return NULL;
    }
    im->bm = bm;
    if (bmhd) { im->w = bmhd->bmh_Width; im->h = bmhd->bmh_Height; }
    if (bmhd && bmhd->bmh_Masking != mskHasMask && bmhd->bmh_Masking != mskHasTransparentColor)
        im->mask = NULL;
    if (im->w > (LONG)GetBitMapAttr(im->bm, BMA_WIDTH)) im->w = GetBitMapAttr(im->bm, BMA_WIDTH);
    if (im->h > (LONG)GetBitMapAttr(im->bm, BMA_HEIGHT)) im->h = GetBitMapAttr(im->bm, BMA_HEIGHT);
    if (tile) {
        expand_tile(im);
        return im;
    }

    /* only one of width/height given: keep the aspect ratio */
    if (im->w > 0 && im->h > 0 && (reqw > 0 || reqh > 0)) {
        LONG tw = reqw > 0 ? reqw : im->w * reqh / im->h;
        LONG th = reqh > 0 ? reqh : im->h * reqw / im->w;
        if (tw > 0 && th > 0 && tw <= 4000 && th <= 4000 && (tw != im->w || th != im->h))
            scale_image(im, tw, th);
    }
    return im;
}

/* loads the pictures of all <img src> (and <input type=image>) of doc */
static void load_images(struct ImgLoad *il, struct HDoc *doc, CONST_STRPTR basedir, struct Screen *scr)
{
    struct HImage *im;
    struct HNode *n = doc->root;
    char path[256];

    if (!html_open_datatypes()) {
        copy_name(il->err, "datatypes.library nicht gefunden", sizeof(il->err));
        return;
    }
    /* page background, tiled in its natural size */
    if (doc->background && resolve_path(basedir, doc->background, path, sizeof(path))) {
        il->total++;
        if ((il->bgimg = load_image(il, path, 0, 0, scr, TRUE))) il->loaded++;
    }
    while (n) {
        const char *src, *a;
        /* table and cell backgrounds */
        if ((n->tag == T_TABLE || n->tag == T_TD || n->tag == T_TH) &&
            (src = html_attr(n, "background")) && resolve_path(basedir, src, path, sizeof(path))) {
            il->total++;
            if ((im = load_image(il, path, 0, 0, scr, TRUE))) {
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
            if ((im = load_image(il, path, w > 0 ? w : 0, h > 0 ? h : 0, scr, FALSE))) {
                n->img = im;
                n->iw = im->w;
                n->ih = im->h;
                il->loaded++;
            }
        }
        /* iterative pre-order walk */
        if (n->first) n = n->first;
        else {
            while (n && !n->next) n = n->parent;
            if (n) n = n->next;
        }
    }
}

struct ParseArgs {
    CONST_STRPTR  src;
    LONG          len;
    struct HDoc  *doc;
    ULONG         fonts;
};

static ULONG parse_func(APTR arg)
{
    struct ParseArgs *pa = arg;
    if ((pa->doc = html_parse(pa->src, pa->len))) pa->fonts = html_fonts_used(pa->doc);
    return 0;
}

/* app context: parse, open fonts, then swap the document in */
static BOOL set_document(struct HTMLData *d, CONST_STRPTR src, LONG len, CONST_STRPTR basedir,
                         struct GadgetInfo *gi)
{
    struct ParseArgs pa;
    struct ImgLoad il;
    struct HImage *oldimages;
    STRPTR copy;

    if (!src) { src = (CONST_STRPTR)""; len = 0; }
    if (len < 0) len = h_strlen((const char *)src);
    if (!(copy = AllocVec(len + 1, MEMF_ANY))) return FALSE;
    h_memcpy(copy, src, len);
    copy[len] = 0;

    pa.src = copy;
    pa.len = len;
    pa.doc = NULL;
    pa.fonts = 0;
    call_big_stack(parse_func, &pa);
    if (!pa.doc) { FreeVec(copy); return FALSE; }
    open_fonts(d, pa.fonts);

    /* pictures are remapped for the gadget's screen; before the window is
     * open that is the default public screen                             */
    h_memset(&il, 0, sizeof(il));
    if (d->loadimages) {
        if (gi && gi->gi_Screen) {
            load_images(&il, pa.doc, basedir, gi->gi_Screen);
        } else {
            struct Screen *scr = LockPubScreen(NULL);
            if (scr) {
                load_images(&il, pa.doc, basedir, scr);
                UnlockPubScreen(NULL, scr);
            }
        }
    }

    ObtainSemaphore(&d->lock);
    print_free(d);                      /* belongs to the old document */
    if (d->lay) { html_free_layout(d->lay); d->lay = NULL; }
    html_free_doc(d->doc);
    if (d->source) FreeVec(d->source);
    d->doc = pa.doc;
    oldimages = d->images;
    d->images = il.list;
    d->bgimg = il.bgimg;
    d->imgtotal = il.total;
    d->imgloaded = il.loaded;
    copy_name(d->imgerror, il.err, sizeof(d->imgerror));
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

static BOOL load_file(struct HTMLData *d, CONST_STRPTR name, struct GadgetInfo *gi)
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
        ok = set_document(d, buf, got, (CONST_STRPTR)dir, gi);
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
            if (!update) { set_document(d, (CONST_STRPTR)data, -1, (CONST_STRPTR)"", gi); newdoc = TRUE; }
            break;
        case HTML_File:
            if (!update) { if (load_file(d, (CONST_STRPTR)data, gi)) newdoc = TRUE; }
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
        redraw(o, gi);            /* also lays out if needed */
        /* values coming in via OM_UPDATE (e.g. from a scroller) are not
         * echoed back, that would fight with the user dragging the knob */
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
    case HTML_Text:         *store = (ULONG)d->source; return 1;
    case HTML_Title:        *store = (ULONG)(d->doc ? d->doc->title : NULL); return 1;
    case HTML_Top:          *store = d->top; return 1;
    case HTML_Total:        *store = d->lay ? d->lay->height : 0; return 1;
    case HTML_Visible:      *store = d->vish; return 1;
    case HTML_Left:         *store = d->left; return 1;
    case HTML_TotalWidth:   *store = d->lay ? d->lay->width : 0; return 1;
    case HTML_VisibleWidth: *store = d->visw; return 1;
    case HTML_LinkURL:      *store = (ULONG)d->linkurl; return 1;
    case HTML_SystemColors: *store = d->syscolors; return 1;
    case HTML_Margin:       *store = d->margin; return 1;
    case HTML_LineHeight:   *store = d->env.font_height[HF_INDEX(0, 3)] + 1; return 1;
    case HTML_AutoAnchors:  *store = d->autoanchors; return 1;
    case HTML_LoadImages:   *store = d->loadimages; return 1;
    case HTML_ImagesTotal:  *store = d->imgtotal; return 1;
    case HTML_ImagesLoaded: *store = d->imgloaded; return 1;
    case HTML_ImageError:   *store = (ULONG)(d->imgerror[0] ? d->imgerror : NULL); return 1;
    case HTML_NumLinks:     *store = d->doc ? d->doc->nlinks : 0; return 1;
    case HTML_BaseDir:      *store = (ULONG)d->basedir; return 1;
    case HTML_HasSelection: *store = !html_sel_empty(&d->sel); return 1;
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
    }
    return dsm(cl, o, (Msg)msg);
}

/* ------------------------------------------------------------------ */
/* object life cycle                                                   */

static Object *om_new(Class *cl, Object *o, struct opSet *msg)
{
    struct HTMLData *d;
    struct TextAttr *ta;
    struct Screen *scr;
    Object *obj;
    int i;

    if (!(obj = (Object *)dsm(cl, o, (Msg)msg))) return NULL;
    d = INST_DATA(cl, obj);
    h_memset(d, 0, sizeof(*d));
    InitSemaphore(&d->lock);
    InitRastPort(&d->mrp);
    d->laywidth = -1;
    d->activelink = -1;
    d->margin = GetTagData(HTML_Margin, 8, msg->ops_AttrList);
    d->syscolors = GetTagData(HTML_SystemColors, FALSE, msg->ops_AttrList) ? TRUE : FALSE;
    d->autoanchors = GetTagData(HTML_AutoAnchors, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->frame = GetTagData(HTML_Frame, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->loadimages = GetTagData(HTML_LoadImages, TRUE, msg->ops_AttrList) ? TRUE : FALSE;
    d->visw = d->vish = 1;

    /* fonts */
    if ((ta = (struct TextAttr *)GetTagData(HTML_Font, 0, msg->ops_AttrList))) {
        copy_name(d->propname, ta->ta_Name, sizeof(d->propname));
        d->propsize = ta->ta_YSize;
    } else if ((scr = LockPubScreen(NULL))) {
        copy_name(d->propname, scr->Font->ta_Name, sizeof(d->propname));
        d->propsize = scr->Font->ta_YSize;
        UnlockPubScreen(NULL, scr);
    }
    if ((ta = (struct TextAttr *)GetTagData(HTML_FixedFont, 0, msg->ops_AttrList))) {
        copy_name(d->fixname, ta->ta_Name, sizeof(d->fixname));
        d->fixsize = ta->ta_YSize;
    } else {
        struct TextFont *df = GfxBase->DefaultFont;
        copy_name(d->fixname, df->tf_Message.mn_Node.ln_Name, sizeof(d->fixname));
        d->fixsize = df->tf_YSize;
    }
    if (!d->propname[0] || !d->propsize) { copy_name(d->propname, "topaz.font", 64); d->propsize = 8; }
    if (!d->fixname[0] || !d->fixsize) { copy_name(d->fixname, "topaz.font", 64); d->fixsize = 8; }

    /* available sizes; without HTML_FixedFont prefer courier.font in a
     * size matching the body text over the (often small) system font     */
    html_open_diskfont();
    d->propsizes = AllocVec(sizeof(struct FontSizes), MEMF_ANY | MEMF_CLEAR);
    d->fixsizes = AllocVec(sizeof(struct FontSizes), MEMF_ANY | MEMF_CLEAR);
    if (d->propsizes) scan_sizes((CONST_STRPTR)d->propname, d->propsizes);
    if (d->fixsizes) {
        if (!GetTagData(HTML_FixedFont, 0, msg->ops_AttrList) && h_stricmp(d->propname, "courier.font")) {
            scan_sizes((CONST_STRPTR)"courier.font", d->fixsizes);
            if (d->fixsizes->n || d->fixsizes->scalable) {
                UWORD s = snap_size(d->fixsizes, d->propsize);
                if (!s) s = d->propsize;
                if (s <= d->propsize + 2) {
                    copy_name(d->fixname, "courier.font", sizeof(d->fixname));
                    d->fixsize = s;
                }
            }
        }
        if (h_stricmp(d->fixname, "courier.font")) scan_sizes((CONST_STRPTR)d->fixname, d->fixsizes);
    }

    d->env.user = d;
    d->env.text_width = text_width_cb;
    d->env.margin = d->margin;
    d->env.system_colors = d->syscolors;
    for (i = 0; i < HF_NUM; i++) d->use[i] = GfxBase->DefaultFont;
    open_fonts(d, 1UL << HF_INDEX(0, 3));

    ((struct Gadget *)obj)->Activation |= GACT_RELVERIFY;

    {   /* the remaining attributes (document, position) */
        struct opSet ops = *msg;
        ops.MethodID = OM_SET;
        ops.ops_GInfo = NULL;
        set_attrs(cl, obj, &ops);
    }
    if (!d->doc) set_document(d, (CONST_STRPTR)"", 0, (CONST_STRPTR)"", NULL);
    return obj;
}

static void om_dispose(Class *cl, Object *o)
{
    struct HTMLData *d = INST_DATA(cl, o);

    print_free(d);
    free_buffer(d);
    release_pens(d);
    if (d->lay) html_free_layout(d->lay);
    html_free_doc(d->doc);
    free_images(d->images);
    if (d->source) FreeVec(d->source);
    if (d->seltext) FreeVec(d->seltext);
    close_fonts(d);
    if (d->propsizes) FreeVec(d->propsizes);
    if (d->fixsizes) FreeVec(d->fixsizes);
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

    /* links and text selection: the whole content area is active */
    l = msg->gpht_Mouse.X >= inset(d) && msg->gpht_Mouse.Y >= inset(d) &&
        msg->gpht_Mouse.X < inset(d) + d->visw && msg->gpht_Mouse.Y < inset(d) + d->vish;
    return l ? GMR_GADGETHIT : 0;
}

/* semaphore held: moves the selection cursor to the mouse position */
static BOOL sel_track(struct HTMLData *d, WORD mx, WORD my)
{
    long it, ch;
    if (!d->lay || !html_sel_pos(d->lay, &d->env, mx - inset(d) + d->left, my - inset(d) + d->top, &it, &ch))
        return FALSE;
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

    /* text selection; a double click selects the word */
    had = !html_sel_empty(&d->sel);
    d->selecting = FALSE;
    if (d->lay && html_sel_pos(d->lay, &d->env, msg->gpi_Mouse.X - inset(d) + d->left,
                               msg->gpi_Mouse.Y - inset(d) + d->top, &it, &ch)) {
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
        if (ie->ie_Class == IECLASS_TIMER) {       /* drag beyond the edge: scroll */
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

/* the pictures are only remapped for the screen: load them again */
static int export_image(void *user, void *img, struct HPrintImage *pi)
{
    return html_export_load_image(((struct HImage *)img)->path, pi);
}

static void export_image_free(void *user, struct HPrintImage *pi)
{
    html_export_free_image(pi);
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
    a->result = html_export(a->d->doc, a->o, a->tags, export_image, export_image_free, a->d);
    return 0;
}

/* on the gadget's own stack, as parsing and layout: the layout is
 * recursive (nested tables), the caller's stack may be small        */
static LONG export_doc(Class *cl, Object *o, struct hmExport *msg)
{
    struct ExportArgs a;
    a.d = INST_DATA(cl, o);
    a.o = o;
    a.tags = msg->hme_Tags;
    a.result = -1;
    ObtainSemaphore(&a.d->lock);
    if (!call_big_stack(export_func, &a)) SetIoErr(ERROR_NO_FREE_STORE);
    ReleaseSemaphore(&a.d->lock);
    return a.result;
}

/* ------------------------------------------------------------------ */
/* bitmap printing                                                     */

/* The pages with the bitmap fonts of the gadget: laid out for the paper
 * at the resolution that fits the fonts (normal text of HTMLEX_FontSize
 * with the line height of the screen font), drawn with draw_content()
 * into a bitmap of the screen the gadget was shown on last, read back
 * as RGB. The printer driver scales it to the paper.                  */
struct BMPrint {
    void           *pool;
    struct HLayout *lay;
    long           *tops, npages;
    LONG            height;         /* page height (layout pixels) */
    LONG            pw, ph;         /* sheet in pixels */
    LONG            ml, mt, mb, cw; /* margins and content width in pixels */
    const char     *footer;
    char            ftext[64];
};

static void print_free(struct HTMLData *d)
{
    struct BMPrint *p = d->print;
    if (!p) return;
    if (p->lay) html_free_layout(p->lay);
    if (p->pool) hsys_pool_delete(p->pool);
    FreeVec(p);
    d->print = NULL;
}

struct BMBegin {
    struct HTMLData *d;
    struct HEnv      env;
    LONG             width;
};

static ULONG print_layout_func(APTR arg)
{
    struct BMBegin *b = arg;
    struct BMPrint *p = b->d->print;
    p->lay = html_layout(b->d->doc, &b->env, b->width);
    if (p->lay) p->tops = html_print_paginate(p->lay, p->pool, p->height, &p->npages);
    return 0;
}

static LONG print_begin(Class *cl, Object *o, struct hmPrintBegin *msg)
{
    struct HTMLData *d = INST_DATA(cl, o);
    struct TagItem *tags = msg->hmpb_Tags;
    LONG *pages = (LONG *)GetTagData(HTMLEX_Pages, 0, tags);
    LONG *pwp = (LONG *)GetTagData(HTMLEX_PageWidth, 0, tags);
    LONG *php = (LONG *)GetTagData(HTMLEX_PageHeight, 0, tags);
    LONG paper_w = GetTagData(HTMLEX_PaperWidth, 595, tags), paper_h = GetTagData(HTMLEX_PaperHeight, 842, tags);
    LONG ml = GetTagData(HTMLEX_MarginLeft, 57, tags), mt = GetTagData(HTMLEX_MarginTop, 57, tags);
    LONG mr = GetTagData(HTMLEX_MarginRight, 57, tags), mb = GetTagData(HTMLEX_MarginBottom, 57, tags);
    LONG size10 = GetTagData(HTMLEX_FontSize, 100, tags), dpi, i, result = -1;
    BOOL bg = GetTagData(HTMLEX_Backgrounds, TRUE, tags) != 0;
    struct BMBegin b;
    struct BMPrint *p;

    if (pages) *pages = 0;
    ObtainSemaphore(&d->lock);
    print_free(d);
    if (!d->doc || !d->scr || !d->use[HF_INDEX(0, 3)] || size10 <= 0 ||
        !(p = d->print = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR)))
        goto out;
    if (!(p->pool = hsys_pool_create())) goto out;
    /* line height of the screen font = 1.2 em of the requested size */
    dpi = d->env.font_height[HF_INDEX(0, 3)] * 600 / size10;
    if (dpi < 40) dpi = 40;
    if (dpi > 600) dpi = 600;
    p->footer = (const char *)GetTagData(HTMLEX_Footer, 0, tags);
    p->pw = paper_w * dpi / 72;
    p->ph = paper_h * dpi / 72;
    p->ml = ml * dpi / 72;
    p->mt = mt * dpi / 72;
    p->mb = mb * dpi / 72;
    p->cw = p->pw - p->ml - mr * dpi / 72;
    p->height = p->ph - p->mt - p->mb;
    b.d = d;
    b.env = d->env;
    b.env.margin = 0;
    b.env.system_colors = 0;        /* black on white */
    b.width = p->cw;
    if (b.width < 50 || p->height < 50 || !call_big_stack(print_layout_func, &b) || !p->lay || !p->tops)
        goto out;
    for (i = 0; i < p->lay->nitems; i++) {
        struct HItem *it = &p->lay->items[i];
        if (it->type == IT_RECT) {
            it->img = NULL;             /* no tiled pictures */
            if (!bg) it->color = COL_NONE;
        }
    }
    if (!bg || !COL_IS_RGB(p->lay->bgcolor)) p->lay->bgcolor = 0xFFFFFF;
    if (pages) *pages = p->npages;
    if (pwp) *pwp = p->pw;
    if (php) *php = p->ph;
    result = p->npages;
out:
    if (result < 0) print_free(d);
    ReleaseSemaphore(&d->lock);
    return result;
}

struct BMRender {
    struct HTMLData      *d;
    struct hmPrintRender *m;
    BOOL                  ok;
};

static void clip_to(struct Layer *layer, LONG x0, LONG y0, LONG x1, LONG y1)
{
    struct Region *reg = NewRegion(), *old;
    struct Rectangle r;
    if (!reg) return;
    r.MinX = x0; r.MinY = y0; r.MaxX = x1 - 1; r.MaxY = y1 - 1;
    OrRectRegion(reg, &r);
    if ((old = InstallClipRegion(layer, reg))) DisposeRegion(old);
}

static ULONG print_render_func(APTR arg)
{
    struct BMRender *a = arg;
    struct HTMLData *d = a->d;
    struct BMPrint *p = d->print;
    struct hmPrintRender *m = a->m;
    LONG x = m->hmpr_X, y = m->hmpr_Y, w = m->hmpr_Width, h = m->hmpr_Height, page = m->hmpr_Page;
    struct BitMap *friend = d->scr->RastPort.BitMap, *bm;
    ULONG depth = GetBitMapAttr(friend, BMA_DEPTH), *buf = m->hmpr_Buffer;
    struct Layer_Info *li = NULL;
    struct Layer *layer = NULL;
    struct GadgetInfo gi;
    struct RastPort *rp;
    LONG top, bottom, cx0, cy0, cx1, cy1, i;

    if (w <= 0 || h <= 0 || page < 1 || page > p->npages) return 0;
    if (!(bm = AllocBitMap(w, h, depth, BMF_MINPLANES, friend))) return 0;
    if (!(li = NewLayerInfo()) || !(layer = CreateUpfrontLayer(li, bm, 0, 0, w - 1, h - 1, LAYERSIMPLE, NULL)))
        goto out;
    rp = layer->rp;
    h_memset(&gi, 0, sizeof(gi));
    gi.gi_Screen = d->scr;
    if (!(gi.gi_DrInfo = GetScreenDrawInfo(d->scr))) goto out;

    SetDrMd(rp, JAM1);
    SetAPen(rp, get_pen(d, &gi, 0xFFFFFF, BACKGROUNDPEN));
    RectFill(rp, 0, 0, w - 1, h - 1);

    /* the content of the page, clipped to its area */
    top = p->tops[page - 1];
    bottom = page < p->npages ? p->tops[page] : top + p->height;
    cx0 = x > p->ml ? x : p->ml;
    cy0 = y > p->mt ? y : p->mt;
    cx1 = x + w < p->ml + p->cw ? x + w : p->ml + p->cw;
    cy1 = y + h < p->mt + (bottom - top) ? y + h : p->mt + (bottom - top);
    if (cx0 < cx1 && cy0 < cy1) {
        struct HLayout *lay = d->lay;
        LONG otop = d->top, oleft = d->left, ow = d->visw, oh = d->vish;
        BOOL sel = d->sel.active, pressed = d->pressed;
        struct HImage *bgimg = d->bgimg;
        clip_to(layer, cx0 - x, cy0 - y, cx1 - x, cy1 - y);
        d->lay = p->lay;
        d->left = cx0 - p->ml;
        d->top = top + cy0 - p->mt;
        d->visw = cx1 - cx0;
        d->vish = cy1 - cy0;
        d->sel.active = 0;
        d->pressed = FALSE;
        d->bgimg = NULL;
        draw_content(d, &gi, rp, cx0 - x, cy0 - y);
        d->lay = lay;
        d->top = otop;
        d->left = oleft;
        d->visw = ow;
        d->vish = oh;
        d->sel.active = sel;
        d->pressed = pressed;
        d->bgimg = bgimg;
    }

    /* the page number, centred in the bottom margin */
    if (p->footer && *p->footer && y + h > p->ph - p->mb && y < p->ph) {
        struct TextFont *tf = d->use[HF_INDEX(0, 2)] ? d->use[HF_INDEX(0, 2)] : d->use[HF_INDEX(0, 3)];
        long len = html_print_footer(p->ftext, sizeof(p->ftext), p->footer, page, p->npages);
        clip_to(layer, 0, (p->ph - p->mb > y ? p->ph - p->mb : y) - y, w, (p->ph < y + h ? p->ph : y + h) - y);
        SetFont(rp, tf);
        SetSoftStyle(rp, 0, AskSoftStyle(rp));
        SetAPen(rp, get_pen(d, &gi, 0x000000, TEXTPEN));
        Move(rp, (p->pw - TextLength(rp, (STRPTR)p->ftext, len)) / 2 - x, p->ph - p->mb / 2 - y);
        Text(rp, (STRPTR)p->ftext, len);
    }
    InstallClipRegion(layer, NULL);
    WaitBlit();

    /* read the pixels back as RGB */
    if (depth > 8 && html_open_cybergfx() && GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX)) {
        ReadPixelArray(buf, 0, 0, w * 4, rp, 0, 0, w, h, RECTFMT_ARGB);
        for (i = 0; i < w * h; i++) buf[i] &= 0xFFFFFFUL;
        a->ok = TRUE;
    } else if (depth <= 8) {
        LONG mod = (w + 15) & ~15, n = 1L << depth, row;
        UBYTE *pens = AllocVec(mod * 16, MEMF_ANY);
        ULONG *cols = AllocVec(n * 3 * sizeof(ULONG), MEMF_ANY);
        struct BitMap *tbm = AllocBitMap(mod, 1, depth, 0, NULL);
        struct RastPort trp;
        if (pens && cols && tbm) {
            GetRGB32(d->scr->ViewPort.ColorMap, 0, n, cols);
            trp = *rp;
            trp.Layer = NULL;
            trp.BitMap = tbm;
            /* 16 rows at a time */
            for (row = 0; row < h; row += 16) {
                LONG k = h - row < 16 ? h - row : 16, yy, xx;
                ReadPixelArray8(rp, 0, row, w - 1, row + k - 1, pens, &trp);
                for (yy = 0; yy < k; yy++)
                    for (xx = 0; xx < w; xx++) {
                        UBYTE c = pens[yy * mod + xx];
                        buf[(row + yy) * w + xx] = (cols[c * 3] >> 24) << 16 | (cols[c * 3 + 1] >> 24) << 8 |
                                                   (cols[c * 3 + 2] >> 24);
                    }
            }
            a->ok = TRUE;
        }
        if (tbm) FreeBitMap(tbm);
        if (cols) FreeVec(cols);
        if (pens) FreeVec(pens);
    }
out:
    if (gi.gi_DrInfo) FreeScreenDrawInfo(d->scr, gi.gi_DrInfo);
    if (layer) DeleteLayer(0, layer);
    if (li) DisposeLayerInfo(li);
    WaitBlit();
    FreeBitMap(bm);
    return 0;
}

/* may run on the printer's task; the gadget is locked meanwhile */
static ULONG print_render(Class *cl, Object *o, struct hmPrintRender *msg)
{
    struct BMRender a;
    a.d = INST_DATA(cl, o);
    a.m = msg;
    a.ok = FALSE;
    ObtainSemaphore(&a.d->lock);
    if (a.d->print && a.d->scr) call_big_stack(print_render_func, &a);
    ReleaseSemaphore(&a.d->lock);
    return (ULONG)a.ok;
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
        print_free(d);
        ReleaseSemaphore(&d->lock);
        return 0;
    }
    }
    return dsm(cl, o, msg);
}
