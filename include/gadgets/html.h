#ifndef GADGETS_HTML_H
#define GADGETS_HTML_H
/*
**  $VER: html.h 1.2 (04.10.2026)
**  Copyright (c) 2026 André Gewert <agewert@ubergeek.de>, MIT License
**
**  Definitions for the html.gadget ReAction class (AmigaOS 3.2)
**
**  html.gadget renders a simple subset of HTML 4:
**    headings, paragraphs, line breaks, lists (ul/ol/dl), pre,
**    blockquote, center/div align, hr, text styles (b/i/u/tt/strike...),
**    <font color size face>, links + anchors, simple tables,
**    <img> via datatypes.library, entities (Latin-1), UTF-8 input.
**
**  Public class name: "html.gadget"   (superclass: gadgetclass)
**  Library:           SYS:Classes/Gadgets/html.gadget
*/

#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif

#define HTML_CLASSNAME   "html.gadget"
#define HTML_VERSION     1

/*****************************************************************************/

#define HTML_Dummy          (TAG_USER + 0x04A70000)

/* HTML source text (Latin-1 or UTF-8). The string is copied.       (ISG)  */
#define HTML_Text           (HTML_Dummy + 1)     /* STRPTR */

/* Load HTML source from a file.                                     (IS)   */
#define HTML_File           (HTML_Dummy + 2)     /* STRPTR */

/* Contents of the document's <title> element or NULL.               (G)    */
#define HTML_Title          (HTML_Dummy + 3)     /* STRPTR */

/* Vertical scroll position in pixels.                               (ISGNU) */
#define HTML_Top            (HTML_Dummy + 4)     /* LONG */

/* Height of the formatted document in pixels.                       (GN)   */
#define HTML_Total          (HTML_Dummy + 5)     /* LONG */

/* Height of the visible area in pixels.                             (GN)   */
#define HTML_Visible        (HTML_Dummy + 6)     /* LONG */

/* Horizontal scroll position / total width / visible width.        */
#define HTML_Left           (HTML_Dummy + 7)     /* LONG (ISGNU) */
#define HTML_TotalWidth     (HTML_Dummy + 8)     /* LONG (GN)    */
#define HTML_VisibleWidth   (HTML_Dummy + 9)     /* LONG (GN)    */

/* HREF of the link the user clicked last. When a link is clicked the
 * gadget sends IDCMP_GADGETUP (WMHI_GADGETUP) with Code = link index.  (G) */
#define HTML_LinkURL        (HTML_Dummy + 10)    /* STRPTR */

/* Scroll to the named anchor (<a name="..."> or id="...").          (S)    */
#define HTML_Anchor         (HTML_Dummy + 11)    /* STRPTR */

/* Proportional base font. Default: font of the default public screen. (I) */
#define HTML_Font           (HTML_Dummy + 12)    /* struct TextAttr * */

/* Fixed width font for <pre>, <tt>, <code>... Default: system font.  (I)  */
#define HTML_FixedFont      (HTML_Dummy + 13)    /* struct TextAttr * */

/* TRUE: documents without explicit colors use the screen's
 * BACKGROUNDPEN/TEXTPEN instead of black on white.  Default FALSE. (ISG)  */
#define HTML_SystemColors   (HTML_Dummy + 14)    /* BOOL */

/* Page margin in pixels. Default 8.                                 (ISG)  */
#define HTML_Margin         (HTML_Dummy + 15)    /* LONG */

/* Line height of the base font, handy as scroll step.               (G)    */
#define HTML_LineHeight     (HTML_Dummy + 16)    /* LONG */

/* TRUE: links of the form "#name" scroll the gadget by itself.
 * A GADGETUP is sent anyway. Default TRUE.                          (ISG)  */
#define HTML_AutoAnchors    (HTML_Dummy + 17)    /* BOOL */

/* Draw a recessed frame around the gadget. Default TRUE.            (I)    */
#define HTML_Frame          (HTML_Dummy + 18)    /* BOOL */

/* Number of links in the document.                                  (G)    */
#define HTML_NumLinks       (HTML_Dummy + 19)    /* LONG */

/* Load <img> pictures through datatypes.library (remapped for the
 * gadget's screen; before the window is open the default public screen
 * is used). width/height scale the picture (PDTM_SCALE or BitMapScale()).
 * Takes effect for the next document. Default TRUE.                 (ISG) */
#define HTML_LoadImages     (HTML_Dummy + 21)    /* BOOL */

/* Pictures of the current document: number of <img> with a local src,
 * how many of them were loaded, and why the first failing one failed
 * (NULL if none failed). Handy for diagnosing missing datatypes or paths.   (G) */
#define HTML_ImagesTotal    (HTML_Dummy + 22)    /* LONG */
#define HTML_ImagesLoaded   (HTML_Dummy + 23)    /* LONG */
#define HTML_ImageError     (HTML_Dummy + 24)    /* STRPTR */

/* Text selection: the user selects text by dragging with the left mouse
 * button (a double click selects a word). No GADGETUP is sent for that.   */

/* Copies the selection to clipboard unit 0 (IFF FTXT). Only with
 * OM_SET/SetGadgetAttrs() from the application's process.         (S)    */
#define HTML_Copy           (HTML_Dummy + 25)    /* BOOL */

/* Selects the whole document / removes the selection.             (S)    */
#define HTML_SelectAll      (HTML_Dummy + 26)    /* BOOL */
#define HTML_ClearSelection (HTML_Dummy + 27)    /* BOOL */

/* Is some text selected?                                          (G)    */
#define HTML_HasSelection   (HTML_Dummy + 28)    /* BOOL */

/* The selected text (Latin-1, lines separated by LF) or NULL. The string
 * belongs to the gadget and is valid until the next get of this
 * attribute or until the gadget is disposed.                      (G)    */
#define HTML_SelectedText   (HTML_Dummy + 29)    /* STRPTR */

/* Directory part of the last HTML_File (for resolving relative links),
 * "" if the text was set with HTML_Text.                            (G)    */
#define HTML_BaseDir        (HTML_Dummy + 20)    /* STRPTR */

/* TRUE: pictures wider than the space they are in are scaled down to
 * that width, keeping their proportions, like max-width: 100% in CSS -
 * handy for Markdown documents with big screenshots. The space is the
 * text width of the gadget (its width minus HTML_Margin on both sides)
 * or of the list, quote or float the picture is in; pictures in tables
 * keep their size. Applies to width/height attributes as well. Printing
 * and export (HTMLM_Export, HTMLM_PrintBegin) use the setting too, with
 * the text width of the page. FALSE (the default) shows pictures in
 * their size like a browser does. (V1.2)                            (ISG)  */
#define HTML_FitImages      (HTML_Dummy + 30)    /* BOOL */

/*****************************************************************************/
/* Printing and export (V1.2)                                                */

/* Writes the document as PostScript or PDF to a DOS file handle. The
 * document is laid out again for the paper with the standard PostScript
 * fonts (Helvetica or Times, Courier), text stays text, pictures are
 * included with their original pixels. Call it with DoMethod() (not
 * DoGadgetMethod()) from the application's process.
 *
 * Result: number of pages written, 0 if the page range was empty, -1 on
 * an error (IoErr(): write error or ERROR_NO_FREE_STORE), -2 if the
 * progress hook stopped it.                                              */
#define HTMLM_Export        (HTML_Dummy + 0x100)

struct hmExport {
    ULONG           MethodID;
    struct TagItem *hme_Tags;
};

#define HTMLEX_Dummy        (HTML_Dummy + 0x200)
#define HTMLEX_File         (HTMLEX_Dummy + 1)   /* BPTR, required */
#define HTMLEX_Format       (HTMLEX_Dummy + 2)   /* HTMLEXF_PS (default) or HTMLEXF_PDF */
#define HTMLEX_PaperWidth   (HTMLEX_Dummy + 3)   /* LONG points, default 595 (A4) */
#define HTMLEX_PaperHeight  (HTMLEX_Dummy + 4)   /* LONG points, default 842 (A4) */
#define HTMLEX_MarginLeft   (HTMLEX_Dummy + 5)   /* LONG points, default 57 (20 mm) */
#define HTMLEX_MarginTop    (HTMLEX_Dummy + 6)
#define HTMLEX_MarginRight  (HTMLEX_Dummy + 7)
#define HTMLEX_MarginBottom (HTMLEX_Dummy + 8)
#define HTMLEX_FontSize     (HTMLEX_Dummy + 9)   /* LONG 1/10 points, default 100 */
#define HTMLEX_Serif        (HTMLEX_Dummy + 10)  /* BOOL text in Times, default FALSE */
#define HTMLEX_Backgrounds  (HTMLEX_Dummy + 11)  /* BOOL print background colours, default TRUE */
#define HTMLEX_Footer       (HTMLEX_Dummy + 12)  /* STRPTR page footer, %p page, %n pages; default none */
#define HTMLEX_Title        (HTMLEX_Dummy + 13)  /* STRPTR, default the document title */
#define HTMLEX_FirstPage    (HTMLEX_Dummy + 14)  /* LONG from 1, default 1 */
#define HTMLEX_LastPage     (HTMLEX_Dummy + 15)  /* LONG, default the last page */
#define HTMLEX_ProgressHook (HTMLEX_Dummy + 16)  /* struct Hook *, object: the gadget,
                                                    message: struct HTMLExportProgress;
                                                    return non-zero to stop */
#define HTMLEX_Pages        (HTMLEX_Dummy + 17)  /* LONG *, gets the number of pages */
#define HTMLEX_DPI          (HTMLEX_Dummy + 18)  /* LONG, HTMLM_PrintBegin: printer pixels per inch, default 150 */
#define HTMLEX_PageWidth    (HTMLEX_Dummy + 19)  /* LONG *, HTMLM_PrintBegin: gets the sheet width in pixels */
#define HTMLEX_PageHeight   (HTMLEX_Dummy + 20)  /* LONG *, HTMLM_PrintBegin: gets the sheet height in pixels */
#define HTMLEX_PSLevel      (HTMLEX_Dummy + 21)  /* LONG PostScript level 1 or 2, default 2: level 2
                                                  * compresses pictures (LZW, ASCII85, JPEG files
                                                  * as they are), level 1 writes them in hex      */
#define HTMLEX_ImageDPI     (HTMLEX_Dummy + 22)  /* LONG most pixels per inch of a picture on paper;
                                                  * pictures with more are scaled down (also JPEG
                                                  * files), 0 = as they are (default)             */

#define HTMLEXF_PS          0
#define HTMLEXF_PDF         1

struct HTMLExportProgress {
    LONG Page;                      /* the page about to be written, from 1 */
    LONG Pages;                     /* pages of the document */
};

/* Bitmap printing (V1.2): the pages as pixels of the whole sheet, in
 * any rectangles, e.g. for printer.device PRD_DUMPRPORTTAGS with
 * DRPA_SourceHook. htmlttf.gadget renders for the printer resolution
 * (HTMLEX_DPI); html.gadget uses its bitmap fonts at the resolution that
 * fits their size and must have been shown on a screen that is still
 * open; HTMLEX_PageWidth/Height tell the size in pixels. HTMLM_PrintBegin lays the document out for the paper
 * (tags as for HTMLM_Export: paper, margins, font size, backgrounds,
 * footer, plus HTMLEX_DPI, HTMLEX_Pages, HTMLEX_PageWidth/Height) and
 * returns the number of pages or -1. HTMLM_PrintRender fills a buffer
 * with 0x00RRGGBB pixels, TRUE if done; it may be called from another
 * task (the printer's). HTMLM_PrintEnd frees the print layout; setting
 * a new document or disposing the gadget does so as well.            */
#define HTMLM_PrintBegin    (HTML_Dummy + 0x101)
#define HTMLM_PrintRender   (HTML_Dummy + 0x102)
#define HTMLM_PrintEnd      (HTML_Dummy + 0x103)

struct hmPrintBegin {
    ULONG           MethodID;
    struct TagItem *hmpb_Tags;
};

struct hmPrintRender {
    ULONG  MethodID;
    LONG   hmpr_Page;               /* from 1 */
    LONG   hmpr_X, hmpr_Y;          /* rectangle on the sheet, in pixels */
    LONG   hmpr_Width, hmpr_Height;
    ULONG *hmpr_Buffer;             /* Width * Height pixels 0x00RRGGBB, row by row */
};

/*****************************************************************************/

#endif /* GADGETS_HTML_H */
