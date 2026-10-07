/*
 * html_export.h - HTMLM_Export, shared by both classes (html_export.c)
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#ifndef HTML_EXPORT_H
#define HTML_EXPORT_H

#include "html_print.h"

typedef int  (*html_img_fn)(void *user, void *img, struct HPrintImage *pi);
typedef void (*html_free_fn)(void *user, struct HPrintImage *pi);

/* the method; img/imgfree give the pixels of HNode.img, genv is the
 * gadget's HEnv (fit_images, table_grid, code_style are taken over)    */
LONG html_export(struct HDoc *doc, Object *gadget, struct TagItem *tags, const struct HEnv *genv,
                 html_img_fn img, html_free_fn imgfree, void *imguser);

/* a picture file with its original pixels (for html.gadget), and the
 * file's bytes if it is a JPEG (html_export_free_image() frees both)  */
int  html_export_load_image(const char *path, struct HPrintImage *pi);
void html_export_free_image(struct HPrintImage *pi);
/* only the bytes of a JPEG file (for htmlttf.gadget, which has the pixels) */
int  html_export_load_jpeg(const char *path, struct HPrintImage *pi);
void html_export_free_jpeg(struct HPrintImage *pi);

#endif
