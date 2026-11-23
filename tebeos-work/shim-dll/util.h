/* util.h -- string helpers shared by the shim's COM objects.
 *
 * Everything that embeds page content (HTML, values, scripts) is built with sfmt(), which
 * allocates exactly what it needs: page content has no size limit, so no fixed buffers.
 */

#ifndef SHIM_UTIL_H
#define SHIM_UTIL_H

#include <windows.h>
#include <oleauto.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* malloc'd printf. Returns NULL only when out of memory. */
char *sfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

char *dup_utf8(const char *s);
char *bstr_to_utf8(BSTR b);               /* malloc'd; NULL for a NULL BSTR */
char *wstr_to_utf8(const WCHAR *w);       /* malloc'd; NULL for NULL */
BSTR utf8_to_bstr(const char *s);         /* SysAllocString'd; NULL for NULL */
WCHAR *utf8_to_wstr(const char *s);       /* malloc'd; NULL for NULL */

/* Single-quoted JavaScript string literal for a UTF-8 string (malloc'd). NULL gives ''. */
char *js_lit(const char *s);
char *js_lit_bstr(BSTR b);

#endif /* SHIM_UTIL_H */
