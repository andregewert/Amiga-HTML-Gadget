/*
 * html_export.c - HTMLM_Export: PostScript/PDF output, Amiga side
 *
 * Shared by html.gadget and htmlttf.gadget: reads the tags, writes the
 * output of html_print() buffered to the DOS file handle and calls the
 * progress hook. html.gadget keeps its pictures only remapped for the
 * screen, so html_export_load_image() loads them again with their
 * original pixels (as htmlttf.gadget does for the screen anyway).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/datatypes.h>

#include <gadgets/html.h>

#include "html_core.h"
#include "html_print.h"
#include "html_private.h"
#include "html_export.h"

struct ExpCtx {
    BPTR         fh;
    LONG         err;               /* IoErr() of a failed Write() */
    Object      *gadget;
    struct Hook *hook;
    html_img_fn  img;
    html_free_fn imgfree;
    void        *imguser;
    LONG         n;
    char         buf[4096];
};

static void flush_out(struct ExpCtx *x)
{
    if (x->n && !x->err && Write(x->fh, x->buf, x->n) != x->n) x->err = IoErr() ? IoErr() : ERROR_DISK_FULL;
    x->n = 0;
}

static void write_cb(void *user, const char *data, long len)
{
    struct ExpCtx *x = user;
    while (len > 0 && !x->err) {
        long k = (long)sizeof(x->buf) - x->n;
        if (k > len) k = len;
        CopyMem((APTR)data, x->buf + x->n, k);
        x->n += k;
        data += k;
        len -= k;
        if (x->n == (LONG)sizeof(x->buf)) flush_out(x);
    }
}

static int progress_cb(void *user, long page, long pages)
{
    struct ExpCtx *x = user;
    struct HTMLExportProgress p;
    if (x->err) return 1;           /* a write error stops it */
    if (!x->hook) return 0;
    p.Page = page;
    p.Pages = pages;
    return CallHookPkt(x->hook, x->gadget, &p) != 0;
}

static int image_cb(void *user, void *img, struct HPrintImage *pi)
{
    struct ExpCtx *x = user;
    return x->img ? x->img(x->imguser, img, pi) : 0;
}

static void image_free_cb(void *user, struct HPrintImage *pi)
{
    struct ExpCtx *x = user;
    if (x->imgfree) x->imgfree(x->imguser, pi);
}

LONG html_export(struct HDoc *doc, Object *gadget, struct TagItem *tags, int fit_images,
                 html_img_fn img, html_free_fn imgfree, void *imguser)
{
    struct HPrintOpts o;
    struct ExpCtx *x;
    LONG *pages = (LONG *)GetTagData(HTMLEX_Pages, 0, tags), n;
    long total = 0;

    if (pages) *pages = 0;
    if (!doc) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return -1; }
    if (!(x = AllocVec(sizeof(*x), MEMF_ANY | MEMF_CLEAR))) { SetIoErr(ERROR_NO_FREE_STORE); return -1; }
    x->fh = (BPTR)GetTagData(HTMLEX_File, 0, tags);
    x->gadget = gadget;
    x->hook = (struct Hook *)GetTagData(HTMLEX_ProgressHook, 0, tags);
    x->img = img;
    x->imgfree = imgfree;
    x->imguser = imguser;
    if (!x->fh) {
        FreeVec(x);
        SetIoErr(ERROR_REQUIRED_ARG_MISSING);
        return -1;
    }

    h_memset(&o, 0, sizeof(o));
    o.format = GetTagData(HTMLEX_Format, HTMLEXF_PS, tags) == HTMLEXF_PDF ? HP_PDF : HP_PS;
    o.ps_level = GetTagData(HTMLEX_PSLevel, 2, tags) == 1 ? 1 : 2;
    o.paper_w = GetTagData(HTMLEX_PaperWidth, 595, tags);
    o.paper_h = GetTagData(HTMLEX_PaperHeight, 842, tags);
    o.margin[0] = GetTagData(HTMLEX_MarginLeft, 57, tags);
    o.margin[1] = GetTagData(HTMLEX_MarginTop, 57, tags);
    o.margin[2] = GetTagData(HTMLEX_MarginRight, 57, tags);
    o.margin[3] = GetTagData(HTMLEX_MarginBottom, 57, tags);
    o.font_size = GetTagData(HTMLEX_FontSize, 100, tags);
    o.serif = GetTagData(HTMLEX_Serif, FALSE, tags) != 0;
    o.backgrounds = GetTagData(HTMLEX_Backgrounds, TRUE, tags) != 0;
    o.fit_images = fit_images;
    o.footer = (const char *)GetTagData(HTMLEX_Footer, 0, tags);
    o.title = (const char *)GetTagData(HTMLEX_Title, (ULONG)doc->title, tags);
    o.first = GetTagData(HTMLEX_FirstPage, 0, tags);
    o.last = GetTagData(HTMLEX_LastPage, 0, tags);
    o.write = write_cb;
    o.progress = progress_cb;
    o.image = image_cb;
    o.image_free = image_free_cb;
    o.user = x;

    n = html_print(doc, &o, &total);
    flush_out(x);
    if (pages) *pages = total;
    if (x->err) {
        SetIoErr(x->err);
        n = -1;
    } else if (n == HP_ERROR) {
        SetIoErr(ERROR_NO_FREE_STORE);
        n = -1;
    } else if (n == HP_ABORTED) n = -2;
    FreeVec(x);
    return n;
}

/* Loads the picture 'path' with its original pixels as ARGB: alpha from
 * the alpha channel (PNG) or the transparent colour (GIF), else opaque. */
int html_export_load_image(const char *path, struct HPrintImage *pi)
{
    struct BitMapHeader *bmhd = NULL;
    struct pdtBlitPixelArray pa;
    Object *dto;
    ULONG alpha = 0, *pix;
    LONG w, h, i;
    UBYTE *lut;

    if (!html_open_datatypes()) return 0;
    if (!(dto = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE,
                            PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE)))
        return 0;
    GetDTAttrs(dto, PDTA_BitMapHeader, (ULONG)&bmhd, TAG_DONE);
    if (!bmhd || !bmhd->bmh_Width || !bmhd->bmh_Height ||
        !(pix = AllocVec(bmhd->bmh_Width * bmhd->bmh_Height * 4, MEMF_ANY))) {
        DisposeDTObject(dto);
        return 0;
    }
    w = bmhd->bmh_Width;
    h = bmhd->bmh_Height;
    pa.MethodID = PDTM_READPIXELARRAY;
    pa.pbpa_PixelData = pix;
    pa.pbpa_PixelFormat = PBPAFMT_ARGB;
    pa.pbpa_PixelArrayMod = w * 4;
    pa.pbpa_Left = 0;
    pa.pbpa_Top = 0;
    pa.pbpa_Width = w;
    pa.pbpa_Height = h;
    if (!dm(dto, (Msg)&pa)) {
        FreeVec(pix);
        DisposeDTObject(dto);
        return 0;
    }
    if (GetDTAttrs(dto, PDTA_AlphaChannel, (ULONG)&alpha, TAG_DONE) != 1) alpha = 0;
    if (!alpha)
        for (i = 0; i < w * h; i++) pix[i] |= 0xFF000000UL;
    if (bmhd->bmh_Masking == mskHasTransparentColor && (lut = AllocVec(w * h, MEMF_ANY))) {
        pa.pbpa_PixelData = lut;
        pa.pbpa_PixelFormat = PBPAFMT_LUT8;
        pa.pbpa_PixelArrayMod = w;
        if (dm(dto, (Msg)&pa))
            for (i = 0; i < w * h; i++)
                if (lut[i] == bmhd->bmh_Transparent) pix[i] = 0;
        FreeVec(lut);
    }
    DisposeDTObject(dto);
    pi->w = w;
    pi->h = h;
    pi->argb = (const unsigned long *)pix;
    pi->priv = pix;
    html_export_load_jpeg(path, pi);
    return 1;
}

void html_export_free_image(struct HPrintImage *pi)
{
    if (pi->priv) FreeVec(pi->priv);
    pi->priv = NULL;
    html_export_free_jpeg(pi);
}

/* If 'path' is a JPEG file, its bytes go to pi->jpeg (passed on to the
 * PDF or PostScript as they are). Returns TRUE if so.                  */
int html_export_load_jpeg(const char *path, struct HPrintImage *pi)
{
    UBYTE head[3], *mem;
    LONG size;
    BPTR fh;

    pi->jpeg = NULL;
    pi->jpeglen = 0;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return 0;
    if (Read(fh, head, 3) == 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF &&
        Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 3 &&
        size < 0x1000000 && (mem = AllocVec(size, MEMF_ANY))) {
        if (Read(fh, mem, size) == size) {
            pi->jpeg = mem;
            pi->jpeglen = size;
        } else FreeVec(mem);
    }
    Close(fh);
    return pi->jpeg != NULL;
}

void html_export_free_jpeg(struct HPrintImage *pi)
{
    if (pi->jpeg) FreeVec((APTR)pi->jpeg);
    pi->jpeg = NULL;
    pi->jpeglen = 0;
}
