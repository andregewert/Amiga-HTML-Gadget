/*
 * html_clip.c - writes text to the clipboard (IFF FTXT/CHRS, unit 0)
 *
 * Plain clipboard.device I/O, no iffparse.library needed. Must be called
 * from a process/task that may wait (application context).
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/clipboard.h>
#include <proto/exec.h>

#include "html_private.h"

static BOOL clip_write(struct IOClipReq *io, const void *data, ULONG len)
{
    io->io_Command = CMD_WRITE;
    io->io_Data = (STRPTR)data;
    io->io_Length = len;
    DoIO((struct IORequest *)io);
    return io->io_Error == 0 && io->io_Actual == len;
}

BOOL html_write_clip(const char *text, LONG len)
{
    struct MsgPort *port;
    struct IOClipReq *io;
    BOOL ok = FALSE;
    ULONG hdr[5];
    static const UBYTE pad = 0;

    if (!(port = CreateMsgPort())) return FALSE;
    if ((io = (struct IOClipReq *)CreateIORequest(port, sizeof(*io)))) {
        if (!OpenDevice((STRPTR)"clipboard.device", 0, (struct IORequest *)io, 0)) {
            io->io_Offset = 0;
            io->io_ClipID = 0;
            hdr[0] = 0x464F524DUL;                       /* FORM */
            hdr[1] = 12 + len + (len & 1);
            hdr[2] = 0x46545854UL;                       /* FTXT */
            hdr[3] = 0x43485253UL;                       /* CHRS */
            hdr[4] = len;
            ok = clip_write(io, hdr, sizeof(hdr)) && clip_write(io, text, len);
            if (ok && (len & 1)) ok = clip_write(io, &pad, 1);
            io->io_Command = CMD_UPDATE;                 /* ends the write */
            DoIO((struct IORequest *)io);
            CloseDevice((struct IORequest *)io);
        }
        DeleteIORequest((struct IORequest *)io);
    }
    DeleteMsgPort(port);
    return ok;
}
