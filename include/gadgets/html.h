#ifndef GADGETS_HTML_H
#define GADGETS_HTML_H
/*
**  $VER: html.h 1.0 (30.09.2026)
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

/*****************************************************************************/

#endif /* GADGETS_HTML_H */
