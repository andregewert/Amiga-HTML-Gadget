/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef PROTO_HTML_H
#define PROTO_HTML_H

#include <clib/html_protos.h>

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/html.h>
#  else
#   include <inline/html.h>
#  endif
# else
#  include <pragmas/html_pragmas.h>
# endif
#endif /* _NO_INLINE */

#ifdef __amigaos4__
# include <interfaces/html.h>
# ifndef __NOGLOBALIFACE__
   extern struct HTMLIFace *IHTML;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern struct Library *
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  HTMLBase;
#endif /* !__NOLIBBASE__ */

#endif /* !PROTO_HTML_H */
