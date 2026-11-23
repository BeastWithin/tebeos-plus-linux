/* util.c -- see util.h. */

#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *sfmt(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *out;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NULL;
    out = (char *)malloc((size_t)n + 1);
    if (!out)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

char *dup_utf8(const char *s)
{
    size_t n;
    char *o;
    if (!s)
        return NULL;
    n = strlen(s);
    o = (char *)malloc(n + 1);
    if (o)
        memcpy(o, s, n + 1);
    return o;
}

char *wstr_to_utf8(const WCHAR *w)
{
    int n;
    char *out;
    if (!w)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    out = (char *)malloc((size_t)n);
    if (out)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    return out;
}

char *bstr_to_utf8(BSTR b)
{
    int n;
    char *out;
    if (!b)
        return NULL;
    /* BSTRs carry their length and may hold embedded NULs; convert all of it. */
    n = WideCharToMultiByte(CP_UTF8, 0, b, (int)SysStringLen(b), NULL, 0, NULL, NULL);
    out = (char *)malloc((size_t)n + 1);
    if (!out)
        return NULL;
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, b, (int)SysStringLen(b), out, n, NULL, NULL);
    out[n] = '\0';
    return out;
}

BSTR utf8_to_bstr(const char *s)
{
    int n;
    BSTR out;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    out = SysAllocStringLen(NULL, (UINT)(n - 1));
    if (out)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

WCHAR *utf8_to_wstr(const char *s)
{
    int n;
    WCHAR *out;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    out = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (out)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

char *js_lit(const char *s)
{
    size_t n, i, o = 0;
    char *out;
    if (!s)
        s = "";
    n = strlen(s);
    out = (char *)malloc(n * 6 + 3);
    if (!out)
        return NULL;
    out[o++] = '\'';
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' || c == '\'') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c < 0x20) {
            o += (size_t)sprintf(out + o, "\\x%02x", c);
        } else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
                   ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            /* U+2028/2029 end a line inside a JS string literal */
            o += (size_t)sprintf(out + o, "\\u%04x", (unsigned char)s[i + 2] == 0xA8 ? 0x2028 : 0x2029);
            i += 2;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '\'';
    out[o] = '\0';
    return out;
}

char *js_lit_bstr(BSTR b)
{
    char *u = bstr_to_utf8(b);
    char *l = js_lit(u);
    free(u);
    return l;
}
