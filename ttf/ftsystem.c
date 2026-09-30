/*
 * ftsystem.c - FreeType system interface for htmlttf.gadget
 *
 * Memory through ftlibc.c (exec), no file streams: the gadget loads the
 * font files itself (in the application's context) and opens the faces
 * with FT_New_Memory_Face(), so FreeType never does I/O - important,
 * because glyphs may be rendered on intuition's input.device task.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include FT_INTERNAL_DEBUG_H
#include FT_INTERNAL_STREAM_H
#include FT_SYSTEM_H
#include FT_ERRORS_H
#include FT_TYPES_H

static void *ft_alloc(FT_Memory memory, long size)
{
    (void)memory;
    return ftl_malloc(size);
}

static void *ft_realloc(FT_Memory memory, long cur_size, long new_size, void *block)
{
    (void)memory; (void)cur_size;
    return ftl_realloc(block, new_size);
}

static void ft_free(FT_Memory memory, void *block)
{
    (void)memory;
    ftl_free(block);
}

FT_BASE_DEF( FT_Error )
FT_Stream_Open( FT_Stream stream, const char *filepathname )
{
    (void)stream; (void)filepathname;
    return FT_Err_Cannot_Open_Resource;
}

FT_BASE_DEF( FT_Memory )
FT_New_Memory( void )
{
    FT_Memory memory = (FT_Memory)ftl_malloc(sizeof(*memory));
    if (memory) {
        memory->user = 0;
        memory->alloc = ft_alloc;
        memory->realloc = ft_realloc;
        memory->free = ft_free;
    }
    return memory;
}

FT_BASE_DEF( void )
FT_Done_Memory( FT_Memory memory )
{
    ftl_free(memory);
}
