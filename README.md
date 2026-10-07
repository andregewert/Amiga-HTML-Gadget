# html.gadget – a ReAction class for simple HTML 4 (AmigaOS 3.2)

*[Deutsche Fassung](README.de.md)*

`html.gadget` and `htmlttf.gadget` are public BOOPSI gadget classes (superclass
`gadgetclass`) for **AmigaOS 3.2**. They can be used as ReAction gadgets inside
`layout.gadget` and display simple HTML 4 documents: online help, readme files,
"about" pages or small offline browsers.

* **html.gadget** draws text with the system's bitmap fonts. Larger HTML font sizes are
  rounded to sizes that really exist as bitmap fonts, so no ugly scaled fonts appear.
  Runs on every AmigaOS 3.2 system, 68000 and up.
* **htmlttf.gadget** is a drop-in alternative with the same attributes. It renders
  anti-aliased TrueType fonts through a built-in FreeType 2.3.8 and composites
  everything in 32 bit (pictures with alpha channel). RTG is recommended.

![htmlttf.gadget with Bitstream Vera on AmigaOS 3.2 (HTMLDemo)](screenshot-amiga.png)

*htmlttf.gadget with Bitstream Vera on AmigaOS 3.2 (HTMLDemo TTF)*

## Features

| Area | Supported |
|---|---|
| Structure | `p`, `br`, `div`/`p`/`h1-6 align=…`, `center`, `blockquote`, `address`, `pre`/`xmp`/`listing` (tabs), `hr` (width, size, align, noshade, color) |
| Headings | `h1`–`h6` (larger fonts, bold) |
| Text styles | `b strong i em cite var dfn u ins s strike del tt code kbd samp big small sub sup q nobr` |
| Fonts | `<font size="1-7/+n/-n" color="…" face="courier…">` |
| Lists | `ul` (disc/circle/square, nested), `ol` (type 1/a/A/i/I, start, value), `type="none"` on the list or item hides the marker (a checkbox at the start of the item takes its place, as in task lists), `dl`/`dt`/`dd`, `menu`, `dir` |
| Links | `<a href>` (link/vlink/alink colours, visited links), anchors `<a name>` and `id="…"`, `#anchor` links are handled by the gadget |
| Tables | automatic column widths, `colspan`, `rowspan`, `border`, `cellpadding`, `cellspacing`, `width` (px/%), `align`, `valign`, `bgcolor`, `nowrap`, `caption`, `th`, nested tables |
| Backgrounds | `bgcolor` on `body`, `table`, `tr`, `td`, `th`; tiled background pictures on `body` (scrolls with the page), `table`, `td`, `th` |
| Pictures | `img` (and `input type=image`) in every format a **datatype** exists for; GIF transparency, PNG alpha (htmlttf.gadget); scaled with `width`/`height`; `alt` text for missing pictures; `align=left/right` floats the picture and text flows around it (`hspace`, `vspace`, `br clear=left/right/all`) |
| Printing | `HTMLM_Export` (V1.2) writes the document as **PostScript or PDF** for a paper size: standard PostScript fonts (Helvetica/Times, Courier), text stays text, pictures with original pixels and transparency (compressed with LZW, JPEG files as they are, optionally scaled down to a resolution), PostScript level 2 or 1, page numbers, page breaks between lines; to a file, `PRT:` or a PostScript handler |
| Selection | drag with the mouse (auto-scrolls at the edges), double click selects a word; copy to the clipboard as IFF FTXT |
| Character set | Latin-1; UTF-8 documents are detected and converted. All HTML 4 Latin-1 entities, `&#nnn;`, `&#xhh;` |
| Forms | `input` is drawn as a placeholder box (not usable); checkboxes and radio buttons show their state (`checked`) with anti-aliased graphics in htmlttf.gadget, sized to the font, read only (e.g. Markdown task lists) |

Not supported: CSS, JavaScript, network access, frames, text flowing around
tables (`align=left/right` only positions them), usable forms.

## Installation

Download `html_gadget.lha` (Aminet: `dev/gui/html_gadget.lha`) and run the `Install`
script, or copy the classes manually:

```
Copy Classes/Gadgets/html.gadget SYS:Classes/Gadgets/
Copy Classes/Gadgets/htmlttf.gadget SYS:Classes/Gadgets/          ; 68000/68010
Copy Classes/Gadgets/68020/htmlttf.gadget SYS:Classes/Gadgets/    ; 68020-68060
```

htmlttf.gadget comes in a second build for 68020 to 68060, which `Install` picks
automatically; it does without the 64 bit multiplications and divisions the 68060 only
emulates. html.gadget spends its time in graphics.library and exists only as 68000 build.

The demo runs directly from the archive: `Demo/HTMLDemo`, or `HTMLDemo TTF` for
htmlttf.gadget (options `FONTSET Vera|DejaVu|Noto`, `SIZE n`). Started from the Workbench it
reads the same options from its tool types (`TTF`, `FONTSET=…`, `SIZE=…`, `FILE=…`; the icon
contains them disabled in parentheses) and opens a project whose default tool is HTMLDemo.

The archive comes with classic 4 colour icons. `Icons/` contains complete sets in
**GlowIcons** and **NewIcons** style (both shown natively by the icon.library of OS 3.2):
double click `UseGlowIcons`, `UseNewIcons` or `UseClassic`.

![Icon styles: Classic, GlowIcons, NewIcons (normal and selected)](icons/preview.png)

## Usage

```c
#include <gadgets/html.h>
#include <proto/html.h>

struct Library *HTMLBase = OpenLibrary("gadgets/html.gadget", 1);

Object *html = NewObject(HTML_GetClass(), NULL,   /* or NewObject(NULL, "html.gadget", ...) */
    GA_ID,        GID_HTML,
    GA_RelVerify, TRUE,
    HTML_File,    "PROGDIR:help.html",
    TAG_DONE);
```

A link click is reported as `WMHI_GADGETUP` with Code = link index; the target is in
`HTML_LinkURL`, the directory of the loaded file in `HTML_BaseDir`. Scrollers are connected
with ICA (see `demo/htmldemo.c`):

```c
struct TagItem html2scroller[] = {
    { HTML_Top, SCROLLER_Top }, { HTML_Total, SCROLLER_Total },
    { HTML_Visible, SCROLLER_Visible }, { TAG_DONE }
};
struct TagItem scroller2html[] = { { SCROLLER_Top, HTML_Top }, { TAG_DONE } };
```

To copy selected text, call `SetGadgetAttrs(html, win, NULL, HTML_Copy, TRUE, TAG_DONE)`
from a menu item (Amiga-C).

### Stack

The gadget swaps to its own 64 KB stack for the work that needs much of it: parsing, layout,
the FreeType output of htmlttf.gadget and `HTMLM_Export` (Intuition may call the gadget on the
input.device task). Other work runs on the stack of the **calling application**: setting
`HTML_File` or `HTML_Text` loads the pictures through datatypes.library and opens fonts with
diskfont.library, and window.class/layout.gadget need stack as well. Give the application
**at least 16 KB, better 32 KB**; too little stack typically crashes while a document or its
pictures are loaded (software error 80000003/80000004).

Started from the Workbench a program gets the stack of its icon, a program without its own
icon only 4 KB; in the Shell the `Stack` command sets it. The application can secure its stack
in the code, independent of how it is started:

```c
/* libnix (bebbo's amiga-gcc, -noixemul), link with -Wl,-u,___stkinit:
 * the startup code swaps to a stack of this size if the current one is smaller */
unsigned long __stack = 32768;
```

SAS/C honours `long __stack = 32768;` as well; with any compiler the GUI code can be called
through `exec.library/StackSwap()`, as the gadget does itself. HTMLDemo uses the libnix way.

### Attributes

| Tag | Type | Use | Meaning |
|---|---|---|---|
| `HTML_Text` | STRPTR | ISG | HTML source (copied) |
| `HTML_File` | STRPTR | IS | load an HTML file |
| `HTML_Title` | STRPTR | G | contents of `<title>` |
| `HTML_Top` / `HTML_Left` | LONG | ISGNU | scroll position in pixels |
| `HTML_Total` / `HTML_TotalWidth` | LONG | GN | document size |
| `HTML_Visible` / `HTML_VisibleWidth` | LONG | GN | visible area |
| `HTML_LinkURL` | STRPTR | G | `href` of the link clicked last |
| `HTML_Anchor` | STRPTR | S | scroll to an anchor |
| `HTML_Font` / `HTML_FixedFont` | struct TextAttr * | I | proportional / fixed base font (html.gadget) |
| `HTML_SystemColors` | BOOL | ISG | screen colours instead of black on white for plain documents |
| `HTML_Margin` | LONG | ISG | page margin (default 8) |
| `HTML_TableGrid` | BOOL | ISG | thin light grey lines in tables without `border`, as on GitHub (default FALSE, V1.2) |
| `HTML_CodeStyle` | BOOL | ISG | grey background for code blocks and inline code, grey bar for quotes, as on GitHub (default FALSE, V1.2) |
| `HTML_FitImages` | BOOL | ISG | scale pictures wider than the text down to its width, like `max-width: 100%`; also when printing (default FALSE, V1.2) |
| `HTML_LineHeight` | LONG | G | line height, e.g. as scroll step |
| `HTML_AutoAnchors` | BOOL | ISG | handle `#anchor` links internally (default TRUE) |
| `HTML_Frame` | BOOL | I | recessed frame (default TRUE) |
| `HTML_NumLinks` | LONG | G | number of links |
| `HTML_BaseDir` | STRPTR | G | directory of the last `HTML_File` |
| `HTML_LoadImages` | BOOL | ISG | load pictures (default TRUE) |
| `HTML_ImagesTotal` / `HTML_ImagesLoaded` | LONG | G | pictures in the document / loaded |
| `HTML_ImageError` | STRPTR | G | why the first picture failed, or NULL |
| `HTML_Copy` | BOOL | S | copy the selection to clipboard unit 0 |
| `HTML_SelectAll` / `HTML_ClearSelection` | BOOL | S | select all / clear the selection |
| `HTML_HasSelection` | BOOL | G | is text selected? |
| `HTML_SelectedText` | STRPTR | G | selected text (Latin-1), owned by the gadget |
| `HTMLM_Export` | method | | writes the document as PostScript or PDF, `HTMLEX_…` tags (V1.2) |
| `HTMLM_PrintBegin` / `HTMLM_PrintRender` / `HTMLM_PrintEnd` | methods | | bitmap printing: pages as pixels, e.g. for printer.device `DRPA_SourceHook`; htmlttf.gadget in the printer's resolution, html.gadget with its bitmap fonts (V1.2) |
| `HTMLTTF_FontDir` | STRPTR | I | directory of the `.ttf` files (htmlttf.gadget) |
| `HTMLTTF_FontSet` | STRPTR | I | `"Vera"`, `"DejaVu"` or `"Noto"` (htmlttf.gadget) |
| `HTMLTTF_Size` | LONG | I | pixel size of body text (htmlttf.gadget) |
| `HTMLTTF_FontSetName` | STRPTR | G | font family in use (htmlttf.gadget) |

The full reference is in the Autodocs: [`doc/html_gadget.doc`](doc/html_gadget.doc) and
[`doc/htmlttf_gadget.doc`](doc/htmlttf_gadget.doc).

## Building

Cross compiled on Linux with [bebbo's amiga-gcc](https://codeberg.org/bebbo/amiga-gcc) in
`/opt/amiga` (with NDK 3.2). htmlttf.gadget additionally needs the FreeType 2.3.8 sources
(Aminet `dev/lib/freetype-2.3.8`, default path `~/AmiLib/freetype-2.3.8`, set with
`make FT=...`).

```
make            # bin/html.gadget, bin/htmlttf.gadget, bin/HTMLDemo, demo pages
make check      # parser + layout tests on the host (AddressSanitizer), compared to test/*.expected
make check-update  # accept an intended layout change as new reference
make preview    # renders demo/example.html with the FreeType renderer to preview.ppm
make dist       # Aminet archive dist/html_gadget.lha (icons, Installer script, Autodocs)
make icons      # sample icons of all styles in icons/ and icons/preview.png
make CPU=-m68020
```

`make` also builds `bin/68020/htmlttf.gadget` (`-m68020-60 -mtune=68060`).

All Amiga sources and HTML examples are encoded in **ISO-8859-1** (the Amiga character
set); `make` stops (`make charcheck`) if UTF-8 sneaks in. GitHub shows the umlauts in
those files as replacement characters – the files themselves are correct.

The Bitstream Vera fonts are in `demo/fonts/`. Noto is not part of the repository because
of its size: copy `NotoSans-Regular/-Bold/-Italic/-BoldItalic.ttf` and
`NotoSansMono-Regular/-Bold.ttf` from <https://notofonts.github.io/> to `demo/fonts/`.

## Source layout

| File | Contents |
|---|---|
| `src/html_parse.c` | tolerant tokenizer and tree builder (implied end tags like HTML 4 browsers), entities, UTF-8 |
| `src/html_layout.c` | block and line layout, lists, tables → list of positioned draw items |
| `src/html_select.c` | text selection: mouse position → character, range, text extraction |
| `src/html_class.c` | BOOPSI dispatcher of html.gadget: bitmap fonts, pens, pictures, off-screen rendering |
| `src/htmlttf_class.c` | BOOPSI dispatcher of htmlttf.gadget: fonts, ARGB pictures, output |
| `src/htmlttf_render.c` | platform independent FreeType renderer: glyph cache, compositing, scaling |
| `src/html_clip.c` | clipboard writer (IFF FTXT) |
| `src/html_print.c`, `src/html_afm.c` | platform independent print engine: layout with PostScript font metrics, page breaks, PostScript and PDF output |
| `src/html_export.c` | `HTMLM_Export` for both classes: tags, DOS output, pictures with original pixels |
| `src/htmlttf_print.c` | platform independent bitmap printing of htmlttf.gadget (pages in strips for a printer resolution) |
| `src/html_lib.c` | library frame (RomTag, Init/Open/Close/Expunge, `*_GetClass`) for both classes |
| `ttf/` | FreeType configuration, C library shim and system interface |
| `demo/` | HTMLDemo and example pages |
| `test/` | host test and preview programs |
| `tools/mkdist.py`, `package/` | Aminet packaging |

Parser, layout and the FreeType renderer are platform independent and tested on the host;
the renderer output was verified to be pixel-identical on an emulated 68k (vamos).
Layout and FreeType run on a private 64 KB stack (`StackSwap()`), because intuition may call
the gadget on the input.device task; a semaphore protects the data between application and
intuition context, and files and fonts are only opened in the application's context.

## License

MIT License, Copyright (c) 2026 André Gewert \<agewert@ubergeek.de\> – see [`LICENSE`](LICENSE).

htmlttf.gadget contains FreeType 2.3.8: Portions of this software are copyright © 2009
The FreeType Project (www.freetype.org). All rights reserved.
Bitstream Vera fonts: Copyright (c) 2003 by Bitstream, Inc., see `demo/fonts/Vera-COPYRIGHT.TXT`.
