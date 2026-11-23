/* json.h -- minimal JSON value + parser for the CDP transport (cdp.c).
 *
 * Supports exactly what Chrome DevTools Protocol messages need: objects, arrays,
 * strings (full escape set), numbers (stored as double; integers also readable),
 * true/false/null, with whitespace. Not a generic serializer; responses are parsed,
 * requests are built with json_escape + snprintf in cdp.c.
 */

#ifndef SHIM_JSON_H
#define SHIM_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct JVal {
    jtype t;
    int   b;                       /* J_BOOL */
    double num;                    /* J_NUM */
    char *s;                       /* J_STR (owned) */
    struct JVal *items;            /* J_ARR */
    size_t n;                      /* J_ARR count / J_OBJ count */
    char **keys;                   /* J_OBJ keys (owned) */
    struct JVal *vals;             /* J_OBJ values */
} JVal;

/* Parse a NUL-terminated JSON document. Returns owned root (call j_free), or NULL on
 * syntax error. On error *off is set to the 0-based byte offset of the failure. */
JVal *j_parse(const char *s, size_t *off);

void j_free(JVal *v);

/* Object lookup. Returns NULL if v is not an object or the key is absent. */
JVal *j_get(JVal *obj, const char *key);

/* Convenience accessors (NULL/0.0 on wrong type). */
const char *j_str(const JVal *v);
double      j_num(const JVal *v);
int         j_is_true(const JVal *v);

/* Escape a string for embedding in a JSON document. Returns a malloc'd NUL-terminated
 * string (with surrounding quotes). Caller frees with free(). */
char *j_quote(const char *s);

#endif /* SHIM_JSON_H */
