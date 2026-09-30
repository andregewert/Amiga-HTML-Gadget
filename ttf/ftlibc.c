/*
 * ftlibc.c - the few C library functions FreeType needs, for a shared
 * library without C runtime. Memory comes from exec (AllocVec), which is
 * safe in every task context.
 *
 * Copyright (c) 2026 André Gewert <agewert@ubergeek.de>
 * Released under the MIT License, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "ftlibc.h"

void *ftl_memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

int ftl_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

void *ftl_memcpy(void *d, const void *s, size_t n)
{
    CopyMem((APTR)s, d, n);
    return d;
}

void *ftl_memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = d;
    const unsigned char *ss = s;
    if (dd < ss || dd >= ss + n) { CopyMem((APTR)s, d, n); return d; }
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}

void *ftl_memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

size_t ftl_strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
char *ftl_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *ftl_strcat(char *d, const char *s) { ftl_strcpy(d + ftl_strlen(d), s); return d; }

int ftl_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int ftl_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

char *ftl_strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *ftl_strrchr(const char *s, int c)
{
    const char *r = 0;
    do { if (*s == (char)c) r = s; } while (*s++);
    return (char *)r;
}

char *ftl_strstr(const char *h, const char *n)
{
    size_t l = ftl_strlen(n);
    for (; *h; h++) if (!ftl_strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}

long ftl_labs(long v) { return v < 0 ? -v : v; }

long ftl_atol(const char *s)
{
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* simple shell sort, FreeType only sorts small arrays */
void ftl_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *b = base, tmp;
    size_t gap, i, j, k;

    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++)
            for (j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
                for (k = 0; k < size; k++) {
                    tmp = b[(j - gap) * size + k];
                    b[(j - gap) * size + k] = b[j * size + k];
                    b[j * size + k] = tmp;
                }
}

/* memory: size is kept in front of the block for realloc */
void *ftl_malloc(size_t n)
{
    ULONG *p = AllocVec(n + 4, MEMF_ANY);
    if (!p) return 0;
    *p = n;
    return p + 1;
}

void *ftl_calloc(size_t n, size_t s)
{
    ULONG *p = AllocVec(n * s + 4, MEMF_ANY | MEMF_CLEAR);
    if (!p) return 0;
    *p = n * s;
    return p + 1;
}

void ftl_free(void *p)
{
    if (p) FreeVec((ULONG *)p - 1);
}

void *ftl_realloc(void *p, size_t n)
{
    void *q;
    ULONG old;
    if (!p) return ftl_malloc(n);
    old = ((ULONG *)p)[-1];
    if (!(q = ftl_malloc(n))) return 0;
    CopyMem(p, q, old < n ? old : n);
    ftl_free(p);
    return q;
}

__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl _ftl_setjmp\n"
    "_ftl_setjmp:\n"
    "   move.l  4(sp),a0\n"
    "   movem.l d2-d7/a2-a6,(a0)\n"
    "   move.l  sp,44(a0)\n"
    "   move.l  (sp),48(a0)\n"
    "   moveq   #0,d0\n"
    "   rts\n"
    "   .globl _ftl_longjmp\n"
    "_ftl_longjmp:\n"
    "   move.l  4(sp),a0\n"
    "   move.l  8(sp),d0\n"
    "   bne.s   1f\n"
    "   moveq   #1,d0\n"
    "1: movem.l (a0),d2-d7/a2-a6\n"
    "   move.l  44(a0),sp\n"
    "   move.l  48(a0),(sp)\n"
    "   rts\n");
