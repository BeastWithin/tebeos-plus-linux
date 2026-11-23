/* json.c -- minimal JSON parser (see json.h). */

#include "json.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static JVal *j_new(jtype t)
{
    JVal *v = (JVal *)calloc(1, sizeof(*v));
    if (v)
        v->t = t;
    return v;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* parse a string starting at the opening quote (which c->p points to). */
static JVal *parse_string(cur_t *c)
{
    size_t cap = 32, n = 0;
    char *buf = (char *)malloc(cap);
    JVal *v;

    if (!buf)
        return NULL;
    c->p++; /* opening quote */
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') {
            c->p++;
            if (n + 1 > cap) { cap = n + 1; buf = (char *)realloc(buf, cap); }
            buf[n] = '\0';
            v = j_new(J_STR);
            if (!v) { free(buf); return NULL; }
            v->s = buf;
            return v;
        }
        if (ch == '\\') {
            int u;
            c->p++;
            if (c->p >= c->end) break;
            switch (*c->p) {
            case '"':  ch = '"';  c->p++; break;
            case '\\': ch = '\\'; c->p++; break;
            case '/':  ch = '/';  c->p++; break;
            case 'b':  ch = '\b'; c->p++; break;
            case 'f':  ch = '\f'; c->p++; break;
            case 'n':  ch = '\n'; c->p++; break;
            case 'r':  ch = '\r'; c->p++; break;
            case 't':  ch = '\t'; c->p++; break;
            case 'u': {
                /* \uXXXX -> UTF-8 (surrogate pairs not supported; CDP strings are ASCII) */
                unsigned short cp = 0;
                int i;
                c->p++;
                for (i = 0; i < 4; i++) {
                    int h;
                    if (c->p >= c->end || (h = hexval(*c->p)) < 0) { free(buf); return NULL; }
                    cp = (unsigned short)(cp * 16 + h);
                    c->p++;
                }
                if (cp < 0x80) {
                    if (n + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                    buf[n++] = (char)cp;
                } else if (cp < 0x800) {
                    if (n + 2 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                    buf[n++] = (char)(0xC0 | (cp >> 6));
                    buf[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    if (n + 3 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                    buf[n++] = (char)(0xE0 | (cp >> 12));
                    buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[n++] = (char)(0x80 | (cp & 0x3F));
                }
                (void)u;
                continue;
            }
            default: free(buf); return NULL; /* invalid escape */
            }
        } else {
            c->p++;
        }
        if (n + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        buf[n++] = (char)ch;
    }
    free(buf);
    return NULL;
}

static JVal *parse_value(cur_t *c);

static JVal *parse_array(cur_t *c)
{
    JVal *v = j_new(J_ARR);
    size_t cap = 8;
    if (!v) return NULL;
    v->items = (JVal *)malloc(cap * sizeof(JVal));
    if (!v->items) { j_free(v); return NULL; }
    c->p++; /* '[' */
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') { c->p++; return v; }
    while (c->p < c->end) {
        JVal *e;
        if (v->n == cap) { cap *= 2; v->items = (JVal *)realloc(v->items, cap * sizeof(JVal)); }
        e = parse_value(c);
        if (!e) { j_free(v); return NULL; }
        v->items[v->n++] = *e; free(e);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') { c->p++; skip_ws(c); continue; }
        if (c->p < c->end && *c->p == ']') { c->p++; return v; }
        j_free(v); return NULL;
    }
    j_free(v); return NULL;
}

static JVal *parse_object(cur_t *c)
{
    JVal *v = j_new(J_OBJ);
    size_t cap = 8;
    if (!v) return NULL;
    v->keys = (char **)malloc(cap * sizeof(char *));
    v->vals = (JVal *)malloc(cap * sizeof(JVal));
    if (!v->keys || !v->vals) { j_free(v); return NULL; }
    c->p++; /* '{' */
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') { c->p++; return v; }
    while (c->p < c->end) {
        JVal *k, *e;
        if (c->p >= c->end || *c->p != '"') { j_free(v); return NULL; }
        k = parse_string(c);
        if (!k) { j_free(v); return NULL; }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') { j_free(k); j_free(v); return NULL; }
        c->p++;
        skip_ws(c);
        e = parse_value(c);
        if (!e) { j_free(k); j_free(v); return NULL; }
        if (v->n == cap) {
            cap *= 2;
            v->keys = (char **)realloc(v->keys, cap * sizeof(char *));
            v->vals = (JVal *)realloc(v->vals, cap * sizeof(JVal));
        }
        v->keys[v->n] = k->s;      /* steal the string */
        free(k);
        v->vals[v->n] = *e; free(e);
        v->n++;
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') { c->p++; skip_ws(c); continue; }
        if (c->p < c->end && *c->p == '}') { c->p++; return v; }
        j_free(v); return NULL;
    }
    j_free(v); return NULL;
}

static JVal *parse_value(cur_t *c)
{
    skip_ws(c);
    if (c->p >= c->end) return NULL;
    if (*c->p == '"') return parse_string(c);
    if (*c->p == '{') return parse_object(c);
    if (*c->p == '[') return parse_array(c);
    if (strncmp(c->p, "true", 4) == 0) { JVal *v = j_new(J_BOOL); if (v) v->b = 1; c->p += 4; return v; }
    if (strncmp(c->p, "false", 5) == 0) { JVal *v = j_new(J_BOOL); c->p += 5; return v; }
    if (strncmp(c->p, "null", 4) == 0) { JVal *v = j_new(J_NULL); c->p += 4; return v; }
    if (*c->p == '-' || (*c->p >= '0' && *c->p <= '9')) {
        char *end;
        JVal *v = j_new(J_NUM);
        if (!v) return NULL;
        v->num = strtod(c->p, &end);
        if (end == c->p) { j_free(v); return NULL; }
        c->p = end;
        return v;
    }
    return NULL;
}

JVal *j_parse(const char *s, size_t *off)
{
    cur_t c;
    JVal *v;
    c.p = s;
    c.end = s + strlen(s);
    v = parse_value(&c);
    if (!v) { if (off) *off = (size_t)(c.p - s); return NULL; }
    return v;
}

/* Free the *contents* of a JVal (children, strings, arrays) but NOT the struct
 * itself. Children in J_ARR/J_OBJ are stored *by value* inside the items/vals
 * arrays, so recursively freeing them must not free() an interior pointer. */
static void j_free_contents(JVal *v)
{
    size_t i;
    if (!v) return;
    if (v->t == J_STR) { free(v->s); }
    else if (v->t == J_ARR) { for (i = 0; i < v->n; i++) j_free_contents(&v->items[i]); free(v->items); }
    else if (v->t == J_OBJ) {
        for (i = 0; i < v->n; i++) { free(v->keys[i]); j_free_contents(&v->vals[i]); }
        free(v->keys); free(v->vals);
    }
}

void j_free(JVal *v)
{
    if (!v) return;
    j_free_contents(v);
    free(v); /* the struct itself: callers pass standalone roots (j_new allocates it) */
}

JVal *j_get(JVal *obj, const char *key)
{
    size_t i;
    if (!obj || obj->t != J_OBJ) return NULL;
    for (i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return &obj->vals[i];
    return NULL;
}

const char *j_str(const JVal *v) { return (v && v->t == J_STR) ? v->s : NULL; }
double      j_num(const JVal *v) { return (v && v->t == J_NUM) ? v->num : 0.0; }
int         j_is_true(const JVal *v) { return (v && v->t == J_BOOL) ? v->b : 0; }

char *j_quote(const char *s)
{
    size_t cap = strlen(s) + 3, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[n++] = '"';
    for (; *s; s++) {
        const char *esc = NULL;
        char hexbuf[8];
        switch (*s) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if ((unsigned char)*s < 0x20) {
                sprintf(hexbuf, "\\u%04x", (unsigned char)*s);
                esc = hexbuf;
            }
            break;
        }
        if (esc) {
            size_t l = strlen(esc);
            while (n + l + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
            memcpy(out + n, esc, l); n += l;
        } else {
            if (n + 3 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
            out[n++] = *s;
        }
    }
    out[n++] = '"';
    out[n] = '\0';
    return out;
}
