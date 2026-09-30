/*
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
/* FreeType's ftbase.c amalgamation; ftobjs.c of 2.3.8 uses PS_FontInfoRec
 * without including t1tables.h when the Type1 driver is not configured */
#include <ft2build.h>
#include FT_TYPE1_TABLES_H
#include "ftbase.c"
