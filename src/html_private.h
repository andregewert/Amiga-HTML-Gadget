/*
 * html_private.h - glue between library and class code
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#ifndef HTML_PRIVATE_H
#define HTML_PRIVATE_H

#include <exec/types.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <utility/hooks.h>
#include <proto/utility.h>

#ifndef LIBNAME
#define LIBNAME     "html.gadget"
#endif
#define LIBVERSION  1
#define LIBREVISION 2
#define LIBDATE     "04.10.2026"

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *UtilityBase;
extern struct Library       *LayersBase;
extern struct Library       *DiskfontBase;
extern struct Library       *DataTypesBase;

extern const ULONG html_inst_size;

void  html_open_diskfont(void);
BOOL  html_open_datatypes(void);
BOOL  html_write_clip(const char *text, LONG len);

#ifdef HTML_TTF
extern struct Library       *CyberGfxBase;
BOOL  html_open_cybergfx(void);
#endif
ULONG html_dispatcher(Class *cl __asm("a0"), Object *o __asm("a2"), Msg msg __asm("a1"));

/* amiga.lib replacements (the library links without it) */
static inline ULONG dm(Object *o, Msg msg)
{
    return CallHookPkt(&OCLASS(o)->cl_Dispatcher, o, msg);
}

static inline ULONG dsm(Class *cl, Object *o, Msg msg)
{
    return CallHookPkt(&cl->cl_Super->cl_Dispatcher, o, msg);
}

#endif
