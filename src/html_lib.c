/*
 * html_lib.c - library frame of html.gadget
 *
 * A classic ReAction class library: OpenLibrary("gadgets/html.gadget")
 * creates and publishes the BOOPSI class "html.gadget"; the only
 * library function is HTML_GetClass().
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/execbase.h>
#include <intuition/classes.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/intuition.h>

#include "html_private.h"

/* must be the first code in the file: running the library as a program
 * returns immediately                                                    */
__asm__(
    "   .text\n"
    "   .globl _LibStart\n"
    "_LibStart:\n"
    "   moveq   #-1,d0\n"
    "   rts\n");

struct ExecBase      *SysBase;
struct DosLibrary    *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *UtilityBase;
struct Library       *LayersBase;
struct Library       *DiskfontBase;
struct Library       *DataTypesBase;
#ifdef HTML_TTF
struct Library       *CyberGfxBase;
#endif

static struct SignalSemaphore DiskfontLock;
static BPTR SegList;

#define STR(x) #x
#define XSTR(x) STR(x)

static const char LibName[] = LIBNAME;
/* the 68020 build (-m68020-60) says so in its version string */
#ifdef __mc68020__
#define LIBCPU " 68020+"
#else
#define LIBCPU ""
#endif
static const char LibId[] = "$VER: " LIBNAME " " XSTR(LIBVERSION) "." XSTR(LIBREVISION) " (" LIBDATE ")" LIBCPU "\r\n";

struct HTMLBase {
    struct ClassLibrary cl;
};

static struct Library *LibInit(struct Library *base __asm("d0"), BPTR seglist __asm("a0"),
                               struct ExecBase *sysbase __asm("a6"));
static struct Library *LibOpen(struct Library *base __asm("a6"));
static BPTR LibClose(struct Library *base __asm("a6"));
static BPTR LibExpunge(struct Library *base __asm("a6"));
static ULONG LibNull(void);
static Class *LibGetClass(struct Library *base __asm("a6"));

static const APTR FuncTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)LibGetClass,
    (APTR)-1
};

static const ULONG InitTable[4] = {
    sizeof(struct HTMLBase),
    (ULONG)FuncTable,
    0,
    (ULONG)LibInit
};

const struct Resident ROMTag = {
    RTC_MATCHWORD,
    (struct Resident *)&ROMTag,
    (APTR)(&ROMTag + 1),
    RTF_AUTOINIT,
    LIBVERSION,
    NT_LIBRARY,
    0,
    (char *)LibName,
    (char *)LibId + 6,
    (APTR)InitTable
};

static void close_libs(void)
{
#ifdef HTML_TTF
    if (CyberGfxBase)  CloseLibrary(CyberGfxBase);
    CyberGfxBase = NULL;
#endif
    if (DataTypesBase) CloseLibrary(DataTypesBase);
    if (DiskfontBase)  CloseLibrary(DiskfontBase);
    if (LayersBase)    CloseLibrary(LayersBase);
    if (UtilityBase)   CloseLibrary(UtilityBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (DOSBase)       CloseLibrary((struct Library *)DOSBase);
    DataTypesBase = DiskfontBase = LayersBase = UtilityBase = NULL;
    GfxBase = NULL; IntuitionBase = NULL; DOSBase = NULL;
}

static struct Library *LibInit(struct Library *base __asm("d0"), BPTR seglist __asm("a0"),
                               struct ExecBase *sysbase __asm("a6"))
{
    struct HTMLBase *hb = (struct HTMLBase *)base;
    Class *cl;

    SysBase = sysbase;
    SegList = seglist;
    base->lib_Node.ln_Type = NT_LIBRARY;
    base->lib_Node.ln_Name = (char *)LibName;
    base->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib_Version = LIBVERSION;
    base->lib_Revision = LIBREVISION;
    base->lib_IdString = (APTR)(LibId + 6);
    InitSemaphore(&DiskfontLock);

    if (sysbase->LibNode.lib_Version < 39 ||
#ifdef __mc68020__
        !(sysbase->AttnFlags & AFF_68020) ||  /* 68020 build on a 68000/010 */
#endif
        !(DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39)) ||
        !(IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39)) ||
        !(GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39)) ||
        !(UtilityBase = OpenLibrary("utility.library", 39)) ||
        !(LayersBase = OpenLibrary("layers.library", 39)))
        goto fail;

    if (!(cl = MakeClass((STRPTR)LibName, (STRPTR)"gadgetclass", NULL, html_inst_size, 0)))
        goto fail;
    cl->cl_Dispatcher.h_Entry = (ULONG (*)())html_dispatcher;
    cl->cl_UserData = (ULONG)base;
    AddClass(cl);
    hb->cl.cl_Class = cl;
    return base;

fail:
    close_libs();
    FreeMem((UBYTE *)base - base->lib_NegSize, base->lib_NegSize + base->lib_PosSize);
    return NULL;
}

static struct Library *LibOpen(struct Library *base __asm("a6"))
{
    base->lib_OpenCnt++;
    base->lib_Flags &= ~LIBF_DELEXP;
    return base;
}

static BPTR LibExpunge(struct Library *base __asm("a6"))
{
    struct HTMLBase *hb = (struct HTMLBase *)base;
    Class *cl = hb->cl.cl_Class;
    BPTR seg;

    if (base->lib_OpenCnt) {
        base->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    if (cl) {
        RemoveClass(cl);
        if (!FreeClass(cl)) {         /* objects still alive */
            AddClass(cl);
            base->lib_Flags |= LIBF_DELEXP;
            return 0;
        }
        hb->cl.cl_Class = NULL;
    }
    seg = SegList;
    Remove(&base->lib_Node);
    close_libs();
    FreeMem((UBYTE *)base - base->lib_NegSize, base->lib_NegSize + base->lib_PosSize);
    return seg;
}

static BPTR LibClose(struct Library *base __asm("a6"))
{
    if (--base->lib_OpenCnt == 0 && (base->lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

static ULONG LibNull(void)
{
    return 0;
}

static Class *LibGetClass(struct Library *base __asm("a6"))
{
    return ((struct HTMLBase *)base)->cl.cl_Class;
}

/* datatypes.library is disk based as well */
BOOL html_open_datatypes(void)
{
    ObtainSemaphore(&DiskfontLock);
    if (!DataTypesBase) DataTypesBase = OpenLibrary("datatypes.library", 39);
    ReleaseSemaphore(&DiskfontLock);
    return DataTypesBase != NULL;
}

#ifdef HTML_TTF
/* cybergraphics.library (RTG): WritePixelArray() for true colour output */
BOOL html_open_cybergfx(void)
{
    ObtainSemaphore(&DiskfontLock);
    if (!CyberGfxBase) CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    ReleaseSemaphore(&DiskfontLock);
    return CyberGfxBase != NULL;
}
#endif

/* diskfont.library is disk based: open it lazily from a process */
void html_open_diskfont(void)
{
    ObtainSemaphore(&DiskfontLock);
    if (!DiskfontBase) DiskfontBase = OpenLibrary("diskfont.library", 39);
    ReleaseSemaphore(&DiskfontLock);
}
