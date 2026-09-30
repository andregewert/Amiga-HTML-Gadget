/*
 * ftlibc.h - C library replacement for FreeType inside htmlttf.gadget
 *
 * The gadget is a shared library without C runtime; FreeType gets these
 * minimal functions (ttf/ftlibc.c) instead of the ANSI C library.
 */
#ifndef __FTSTDLIB_H__
#define __FTSTDLIB_H__

#include <stddef.h>
#include <limits.h>
#include <stdarg.h>

#define ft_ptrdiff_t  ptrdiff_t

#define FT_CHAR_BIT   CHAR_BIT
#define FT_INT_MAX    INT_MAX
#define FT_UINT_MAX   UINT_MAX
#define FT_ULONG_MAX  ULONG_MAX

void  *ftl_memchr(const void *s, int c, size_t n);
int    ftl_memcmp(const void *a, const void *b, size_t n);
void  *ftl_memcpy(void *d, const void *s, size_t n);
void  *ftl_memmove(void *d, const void *s, size_t n);
void  *ftl_memset(void *d, int c, size_t n);
char  *ftl_strcat(char *d, const char *s);
int    ftl_strcmp(const char *a, const char *b);
char  *ftl_strcpy(char *d, const char *s);
size_t ftl_strlen(const char *s);
int    ftl_strncmp(const char *a, const char *b, size_t n);
char  *ftl_strncpy(char *d, const char *s, size_t n);
char  *ftl_strrchr(const char *s, int c);
char  *ftl_strstr(const char *h, const char *n);
void   ftl_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
long   ftl_atol(const char *s);
long   ftl_labs(long v);
void  *ftl_malloc(size_t n);
void  *ftl_calloc(size_t n, size_t s);
void  *ftl_realloc(void *p, size_t n);
void   ftl_free(void *p);

#define ft_memchr   ftl_memchr
#define ft_memcmp   ftl_memcmp
#define ft_memcpy   ftl_memcpy
#define ft_memmove  ftl_memmove
#define ft_memset   ftl_memset
#define ft_strcat   ftl_strcat
#define ft_strcmp   ftl_strcmp
#define ft_strcpy   ftl_strcpy
#define ft_strlen   ftl_strlen
#define ft_strncmp  ftl_strncmp
#define ft_strncpy  ftl_strncpy
#define ft_strrchr  ftl_strrchr
#define ft_strstr   ftl_strstr
#define ft_qsort    ftl_qsort
#define ft_atol     ftl_atol
#define ft_labs     ftl_labs
#define ft_scalloc  ftl_calloc
#define ft_sfree    ftl_free
#define ft_smalloc  ftl_malloc
#define ft_srealloc ftl_realloc

/* no stdio: faces are always opened from memory (FT_New_Memory_Face) */
#define FT_FILE     void
#define ft_fclose(f)          (0)
#define ft_fopen(n, m)        ((void *)0)
#define ft_fread(b, s, n, f)  (0)
#define ft_fseek(f, o, w)     (-1)
#define ft_ftell(f)           (0)
#define ft_sprintf            ftl_sprintf_unused

/* d2-d7, a2-a7, return address */
typedef long ftl_jmp_buf[14];
int  ftl_setjmp(ftl_jmp_buf b);
void ftl_longjmp(ftl_jmp_buf b, int v) __attribute__((noreturn));

#define ft_jmp_buf     ftl_jmp_buf
#define ft_longjmp     ftl_longjmp
#define ft_setjmp( b ) ftl_setjmp( *(ftl_jmp_buf *) &(b) )

#endif
