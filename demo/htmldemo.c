/*
 * HTMLDemo - a small file browser built on html.gadget
 *
 *   HTMLDemo [FILE] <name.html> [TTF] [FONTSET Vera|DejaVu|Noto] [SIZE n]
 *
 * TTF uses htmlttf.gadget (FreeType renderer) instead of html.gadget.
 * Shows how to create the class, connect scrollers via ICA_TARGET,
 * follow links (WMHI_GADGETUP) and scroll with keys and mouse wheel.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/icclass.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/scroller.h>
#include <gadgets/html.h>
#include <gadgets/htmlttf.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/scroller.h>
#include <proto/html.h>
#include <clib/alib_protos.h>

#include <string.h>

static const char version[] = "$VER: HTMLDemo 1.0 (30.9.2026)";

/* initialised explicitly: as COMMON symbols they would pull in the
 * auto-open stubs of libstubs.a, which try to open "window.library" */
struct Library *WindowBase = NULL, *LayoutBase = NULL, *ButtonBase = NULL,
               *ScrollerBase = NULL, *HTMLBase = NULL;

enum { GID_HTML = 1, GID_VSCROLL, GID_HSCROLL, GID_BACK, GID_STATUS };
enum { MID_BACK = 1, MID_QUIT, MID_COPY, MID_SELALL };

static struct NewMenu menus[] = {
    { NM_TITLE, (STRPTR)"Projekt",          0,   0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Zurück",        (STRPTR)"B", 0, 0, (APTR)MID_BACK },
    { NM_ITEM,  NM_BARLABEL,                 0,   0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Beenden",           (STRPTR)"Q", 0, 0, (APTR)MID_QUIT },
    { NM_TITLE, (STRPTR)"Bearbeiten",        0,   0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Kopieren",          (STRPTR)"C", 0, 0, (APTR)MID_COPY },
    { NM_ITEM,  (STRPTR)"Alles auswählen", (STRPTR)"A", 0, 0, (APTR)MID_SELALL },
    { NM_END,   0, 0, 0, 0, 0 }
};

#define MAXHIST 32

static Object *winobj, *html, *vscroll, *hscroll, *status, *backbut;
static struct Window *win;
static STRPTR history[MAXHIST];
static int nhist;
static char homefile[256];
static char titlesuffix[40];

static const char fallback_doc[] =
    "<html><head><title>HTMLDemo</title></head><body>"
    "<h1>HTMLDemo</h1>"
    "<p>Keine Datei gefunden. Aufruf: <tt>HTMLDemo datei.html</tt></p>"
    "<p><b>html.gadget</b> stellt einfaches <i>HTML 4</i> dar.</p>"
    "</body></html>";

static struct TagItem vscroll_map[] = { { SCROLLER_Top, HTML_Top }, { TAG_DONE, 0 } };
static struct TagItem hscroll_map[] = { { SCROLLER_Top, HTML_Left }, { TAG_DONE, 0 } };
static struct TagItem html_map[] = {
    { HTML_Top,     SCROLLER_Top },
    { HTML_Total,   SCROLLER_Total },
    { HTML_Visible, SCROLLER_Visible },
    { TAG_DONE, 0 }
};

static void set_status(CONST_STRPTR text)
{
    SetGadgetAttrs((struct Gadget *)status, win, NULL, GA_Text, (ULONG)text, TAG_DONE);
}

static void sync_hscroll(void)
{
    ULONG left = 0, total = 0, vis = 0;
    GetAttr(HTML_Left, html, &left);
    GetAttr(HTML_TotalWidth, html, &total);
    GetAttr(HTML_VisibleWidth, html, &vis);
    SetGadgetAttrs((struct Gadget *)hscroll, win, NULL,
                   SCROLLER_Total, total, SCROLLER_Visible, vis, SCROLLER_Top, left, TAG_DONE);
}

static void update_title(void)
{
    STRPTR title = NULL;
    GetAttr(HTML_Title, html, (ULONG *)&title);
    static char wtitle[200];
    strncpy(wtitle, title && *title ? (char *)title : "HTMLDemo", sizeof(wtitle) - 41);
    wtitle[sizeof(wtitle) - 41] = 0;
    strcat(wtitle, titlesuffix);
    SetAttrs(winobj, WA_Title, (ULONG)wtitle, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)backbut, win, NULL, GA_Disabled, nhist < 2, TAG_DONE);
    sync_hscroll();
}

static STRPTR dupstr(CONST_STRPTR s)
{
    STRPTR d = AllocVec(strlen((const char *)s) + 1, MEMF_ANY);
    if (d) strcpy((char *)d, (const char *)s);
    return d;
}

static char *utoa_(char *p, ULONG v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

/* loads 'path' (may contain "#anchor"), optionally recording it in the history */
static BOOL show_file(CONST_STRPTR path, BOOL record)
{
    char file[512];
    char *hash;
    BPTR lock;

    strncpy(file, (const char *)path, sizeof(file) - 1);
    file[sizeof(file) - 1] = 0;
    if ((hash = strchr(file, '#'))) *hash++ = 0;

    if (!(lock = Lock((STRPTR)file, ACCESS_READ))) {
        set_status((CONST_STRPTR)"Datei nicht gefunden");
        return FALSE;
    }
    UnLock(lock);

    SetGadgetAttrs((struct Gadget *)html, win, NULL, HTML_File, (ULONG)file, TAG_DONE);
    if (hash && *hash)
        SetGadgetAttrs((struct Gadget *)html, win, NULL, HTML_Anchor, (ULONG)hash, TAG_DONE);

    if (record) {
        if (nhist == MAXHIST) {
            FreeVec(history[0]);
            memmove(history, history + 1, (MAXHIST - 1) * sizeof(STRPTR));
            nhist--;
        }
        if ((history[nhist] = dupstr(path))) nhist++;
    }
    set_status(path);
    {
        static char msg[300];
        ULONG total = 0, loaded = 0;
        STRPTR err = NULL;
        GetAttr(HTML_ImagesTotal, html, &total);
        GetAttr(HTML_ImagesLoaded, html, &loaded);
        GetAttr(HTML_ImageError, html, (ULONG *)&err);
        if (err) {
            char *m = msg;
            m = utoa_(m, loaded);
            *m++ = '/';
            m = utoa_(m, total);
            strcpy(m, " Bilder - ");
            strncat(msg, (char *)err, sizeof(msg) - strlen(msg) - 1);
            set_status((CONST_STRPTR)msg);
        }
    }
    update_title();
    return TRUE;
}

static void follow_link(void)
{
    STRPTR url = NULL, base = NULL;
    char path[512];

    GetAttr(HTML_LinkURL, html, (ULONG *)&url);
    GetAttr(HTML_BaseDir, html, (ULONG *)&base);
    if (!url) return;

    if (url[0] == '#') {                       /* handled by HTML_AutoAnchors */
        set_status(url);
        return;
    }
    if (strstr((const char *)url, "://") || !strncmp((const char *)url, "mailto:", 7)) {
        set_status((CONST_STRPTR)"Externe Links werden von der Demo nicht geladen");
        return;
    }
    strncpy(path, base ? (const char *)base : "", sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    if (!strncmp((const char *)url, "file:", 5)) url += 5;
    AddPart((STRPTR)path, url, sizeof(path));
    show_file((CONST_STRPTR)path, TRUE);
}

static void go_back(void)
{
    if (nhist < 2) return;
    FreeVec(history[--nhist]);
    show_file(history[nhist - 1], FALSE);
}

static void scroll_by(LONG dy, LONG dx)
{
    ULONG top = 0, left = 0;
    LONG t, l;

    GetAttr(HTML_Top, html, &top);
    GetAttr(HTML_Left, html, &left);
    t = (LONG)top + dy;
    l = (LONG)left + dx;
    if (t < 0) t = 0;
    if (l < 0) l = 0;
    SetGadgetAttrs((struct Gadget *)html, win, NULL, HTML_Top, t, HTML_Left, l, TAG_DONE);
    if (dx) sync_hscroll();
}

static void copy_selection(void)
{
    ULONG has = FALSE;
    GetAttr(HTML_HasSelection, html, &has);
    if (has) {
        SetGadgetAttrs((struct Gadget *)html, win, NULL, HTML_Copy, TRUE, TAG_DONE);
        set_status((CONST_STRPTR)"Markierung in die Zwischenablage kopiert");
    } else {
        set_status((CONST_STRPTR)"Nichts markiert (Text mit der Maus markieren)");
    }
}

static void page(int dir)
{
    ULONG vis = 0, lh = 8;
    GetAttr(HTML_Visible, html, &vis);
    GetAttr(HTML_LineHeight, html, &lh);
    scroll_by(dir * ((LONG)vis - (LONG)lh), 0);
}

int main(void)
{
    struct RDArgs *rda;
    LONG args[4] = { 0, 0, 0, 0 };     /* FILE, TTF, FONTSET, SIZE */
    ULONG sigmask, result;
    UWORD code;
    BOOL done = FALSE;
    Class *htmlclass;
    int rc = RETURN_FAIL;

    if (!(rda = ReadArgs((STRPTR)"FILE,TTF/S,FONTSET/K,SIZE/K/N", args, NULL))) {
        PrintFault(IoErr(), (STRPTR)"HTMLDemo");
        return RETURN_FAIL;
    }

    WindowBase   = OpenLibrary((STRPTR)"window.class", 44);
    LayoutBase   = OpenLibrary((STRPTR)"gadgets/layout.gadget", 44);
    ButtonBase   = OpenLibrary((STRPTR)"gadgets/button.gadget", 44);
    ScrollerBase = OpenLibrary((STRPTR)"gadgets/scroller.gadget", 44);
    /* both classes have xxx_GetClass() at the same offset, so the
     * HTML_GetClass() stub works for either library                 */
    if (args[1]) {
        HTMLBase = OpenLibrary((STRPTR)"gadgets/htmlttf.gadget", 1);
        if (!HTMLBase) HTMLBase = OpenLibrary((STRPTR)"PROGDIR:htmlttf.gadget", 1);
        if (!HTMLBase) HTMLBase = OpenLibrary((STRPTR)"PROGDIR:/Classes/Gadgets/htmlttf.gadget", 1);
    } else {
        HTMLBase = OpenLibrary((STRPTR)"gadgets/html.gadget", 1);
        if (!HTMLBase) HTMLBase = OpenLibrary((STRPTR)"PROGDIR:html.gadget", 1);
        if (!HTMLBase) HTMLBase = OpenLibrary((STRPTR)"PROGDIR:/Classes/Gadgets/html.gadget", 1);
    }

    if (!WindowBase || !LayoutBase || !ButtonBase || !ScrollerBase) {
        Printf((STRPTR)"ReAction-Klassen (V44+) fehlen.\n");
        goto out;
    }
    if (!HTMLBase) {
        Printf((STRPTR)"%s nicht gefunden (SYS:Classes/Gadgets/ oder Programmverzeichnis).\n",
               (ULONG)(args[1] ? "htmlttf.gadget" : "html.gadget"));
        goto out;
    }
    htmlclass = HTML_GetClass();

    html = NewObject(htmlclass, NULL,
        GA_ID,           GID_HTML,
        GA_RelVerify,    TRUE,
        HTML_Text,       (ULONG)fallback_doc,
        args[2] ? HTMLTTF_FontSet : TAG_IGNORE, (ULONG)args[2],
        args[3] ? HTMLTTF_Size : TAG_IGNORE,    args[3] ? *(LONG *)args[3] : 0,
        TAG_DONE);
    if (html && args[1]) {
        STRPTR set = NULL;
        GetAttr(HTMLTTF_FontSetName, html, (ULONG *)&set);
        strcpy(titlesuffix, " [TTF: ");
        strcat(titlesuffix, set ? (char *)set : "kein Font!");
        strcat(titlesuffix, "]");
    }
    vscroll = NewObject(SCROLLER_GetClass(), NULL,
        GA_ID,                GID_VSCROLL,
        GA_RelVerify,         TRUE,
        SCROLLER_Orientation, SORIENT_VERT,
        SCROLLER_Arrows,      TRUE,
        SCROLLER_ArrowDelta,  16,
        ICA_TARGET,           (ULONG)html,
        ICA_MAP,              (ULONG)vscroll_map,
        TAG_DONE);
    hscroll = NewObject(SCROLLER_GetClass(), NULL,
        GA_ID,                GID_HSCROLL,
        GA_RelVerify,         TRUE,
        SCROLLER_Orientation, SORIENT_HORIZ,
        SCROLLER_Arrows,      TRUE,
        SCROLLER_ArrowDelta,  16,
        ICA_TARGET,           (ULONG)html,
        ICA_MAP,              (ULONG)hscroll_map,
        TAG_DONE);
    backbut = NewObject(BUTTON_GetClass(), NULL,
        GA_ID, GID_BACK, GA_RelVerify, TRUE, GA_Text, (ULONG)"_Zurück", GA_Disabled, TRUE, TAG_DONE);
    status = NewObject(BUTTON_GetClass(), NULL,
        GA_ID, GID_STATUS, GA_ReadOnly, TRUE, GA_Text, (ULONG)"",
        BUTTON_Justification, BCJ_LEFT, TAG_DONE);

    if (!html || !vscroll || !hscroll || !backbut || !status) {
        Printf((STRPTR)"Objekte konnten nicht erzeugt werden.\n");
        goto out;
    }
    SetAttrs(html, ICA_TARGET, (ULONG)vscroll, ICA_MAP, (ULONG)html_map, TAG_DONE);

    winobj = NewObject(WINDOW_GetClass(), NULL,
        WA_Title,          (ULONG)"HTMLDemo",
        WA_Activate,       TRUE,
        WA_DepthGadget,    TRUE,
        WA_DragBar,        TRUE,
        WA_CloseGadget,    TRUE,
        WA_SizeGadget,     TRUE,
        WA_SizeBBottom,    TRUE,
        WA_InnerWidth,     560,
        WA_InnerHeight,    380,
        WA_IDCMP,          IDCMP_RAWKEY | IDCMP_VANILLAKEY,
        WINDOW_Position,   WPOS_CENTERSCREEN,
        WINDOW_NewMenu,    (ULONG)menus,
        WA_NewLookMenus,   TRUE,
        WINDOW_ParentGroup, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
            LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
            LAYOUT_SpaceOuter,  TRUE,
            LAYOUT_DeferLayout, TRUE,

            LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
                LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                LAYOUT_AddChild, (ULONG)backbut,
                CHILD_WeightedWidth, 0,
                LAYOUT_AddChild, (ULONG)status,
                TAG_DONE),
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
                LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                LAYOUT_InnerSpacing, 0,
                LAYOUT_AddChild, (ULONG)html,
                LAYOUT_AddChild, (ULONG)vscroll,
                CHILD_WeightedWidth, 0,
                TAG_DONE),
            LAYOUT_AddChild, (ULONG)hscroll,
            CHILD_WeightedHeight, 0,
            TAG_DONE),
        TAG_DONE);

    if (!winobj || !(win = (struct Window *)DoMethod(winobj, WM_OPEN))) {
        Printf((STRPTR)"Fenster konnte nicht geöffnet werden.\n");
        goto out;
    }

    /* start document */
    if (args[0]) strncpy(homefile, (const char *)args[0], sizeof(homefile) - 1);
    else strcpy(homefile, "PROGDIR:example.html");
    if (!show_file((CONST_STRPTR)homefile, TRUE)) update_title();

    GetAttr(WINDOW_SigMask, winobj, &sigmask);
    while (!done) {
        ULONG sig = Wait(sigmask | SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) done = TRUE;
        while ((result = DoMethod(winobj, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG) {
            switch (result & WMHI_CLASSMASK) {
            case WMHI_CLOSEWINDOW:
                done = TRUE;
                break;
            case WMHI_GADGETUP:
                switch (result & WMHI_GADGETMASK) {
                case GID_HTML:
                    follow_link();
                    break;
                case GID_BACK:
                    go_back();
                    break;
                case GID_HSCROLL:
                case GID_VSCROLL:
                    break;
                }
                break;
            case WMHI_NEWSIZE:
                sync_hscroll();
                break;
            case WMHI_MENUPICK: {
                UWORD num = result & WMHI_MENUMASK;
                while (num != MENUNULL && !done) {
                    struct MenuItem *item = ItemAddress(win->MenuStrip, num);
                    if (!item) break;
                    switch ((ULONG)GTMENUITEM_USERDATA(item)) {
                    case MID_BACK:   go_back(); break;
                    case MID_QUIT:   done = TRUE; break;
                    case MID_COPY:   copy_selection(); break;
                    case MID_SELALL:
                        SetGadgetAttrs((struct Gadget *)html, win, NULL, HTML_SelectAll, TRUE, TAG_DONE);
                        break;
                    }
                    num = item->NextSelect;
                }
                break;
            }
            case WMHI_VANILLAKEY:
                switch (result & WMHI_KEYMASK) {
                case ' ': page(1); break;
                case 8:   go_back(); break;          /* Backspace */
                case 27:  done = TRUE; break;        /* Esc */
                }
                break;
            case WMHI_RAWKEY:
                switch (result & WMHI_KEYMASK) {
                case 0x4C: scroll_by(-16, 0); break;  /* cursor up */
                case 0x4D: scroll_by(16, 0); break;   /* cursor down */
                case 0x4F: scroll_by(0, -16); break;  /* cursor left */
                case 0x4E: scroll_by(0, 16); break;   /* cursor right */
                case 0x7A: scroll_by(-32, 0); break;  /* mouse wheel up */
                case 0x7B: scroll_by(32, 0); break;   /* mouse wheel down */
                case 0x3F: page(-1); break;           /* numpad 9 / PgUp */
                case 0x1F: page(1); break;            /* numpad 3 / PgDn */
                }
                break;
            }
        }
    }
    rc = RETURN_OK;

out:
    if (winobj) DisposeObject(winobj);          /* disposes all gadgets */
    else {
        if (html) DisposeObject(html);
        if (vscroll) DisposeObject(vscroll);
        if (hscroll) DisposeObject(hscroll);
        if (backbut) DisposeObject(backbut);
        if (status) DisposeObject(status);
    }
    while (nhist) FreeVec(history[--nhist]);
    if (HTMLBase) CloseLibrary(HTMLBase);
    if (ScrollerBase) CloseLibrary(ScrollerBase);
    if (ButtonBase) CloseLibrary(ButtonBase);
    if (LayoutBase) CloseLibrary(LayoutBase);
    if (WindowBase) CloseLibrary(WindowBase);
    FreeArgs(rda);
    (void)version;
    return rc;
}
