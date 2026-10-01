#ifndef GADGETS_HTMLTTF_H
#define GADGETS_HTMLTTF_H
/*
**  $VER: htmlttf.h 1.0 (30.09.2026)
**  Copyright (c) 2026 André Gewert <agewert@ubergeek.de>, MIT License
**
**  htmlttf.gadget - the html.gadget renderer with FreeType fonts
**
**  Same parser, layout and attributes as html.gadget (see gadgets/html.h),
**  but text is rendered anti-aliased with TrueType fonts (Bitstream Vera,
**  DejaVu or Noto) through a statically linked FreeType 2.3.8, pictures
**  are composited in true colour with alpha channel. On RTG screens
**  (> 8 bit) the result is written with cybergraphics WritePixelArray(),
**  on palette screens it is mapped to pens.
**
**  Public class name: "htmlttf.gadget"   (superclass: gadgetclass)
**  Library:           SYS:Classes/Gadgets/htmlttf.gadget
**
**  All HTML_... attributes of html.gadget work the same, except
**  HTML_Font/HTML_FixedFont (ignored: see HTMLTTF_Size, HTMLTTF_FontSet).
*/

#ifndef GADGETS_HTML_H
#include <gadgets/html.h>
#endif

#define HTMLTTF_CLASSNAME   "htmlttf.gadget"
#define HTMLTTF_VERSION     1

#define HTMLTTF_Dummy       (HTML_Dummy + 0x100)

/* Directory with the .ttf files. Default: searched in PROGDIR:fonts/,
 * FONTS:_TrueType/, FONTS:TrueType/ and FONTS:.                      (I)  */
#define HTMLTTF_FontDir     (HTMLTTF_Dummy + 1)   /* STRPTR */

/* Font family: "Vera", "DejaVu" or "Noto". Default: the first one
 * found in that order.                                               (I)  */
#define HTMLTTF_FontSet     (HTMLTTF_Dummy + 2)   /* STRPTR */

/* Pixel size (em) of normal body text. Default: derived from the
 * default public screen's font.                                      (I)  */
#define HTMLTTF_Size        (HTMLTTF_Dummy + 3)   /* LONG */

/* Font family actually in use, NULL if no TrueType font was found
 * (then nothing but pictures and boxes can be drawn).                (G)  */
#define HTMLTTF_FontSetName (HTMLTTF_Dummy + 4)   /* STRPTR */

#endif /* GADGETS_HTMLTTF_H */
