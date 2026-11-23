/* jsprop.c -- see jsprop.h. */

#include "jsprop.h"
#include "dom.h"
#include "util.h"
#include "rcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* t is "window.__shim_els[N]": N (the stashed object), else -1 */
static long stashed(const char *t)
{
    long h;
    int n = 0;
    if (sscanf(t, "window.__shim_els[%ld]%n", &h, &n) == 1 && n > 0 && !t[n])
        return h;
    return -1;
}

/* "(t)['prop']" (malloc'd) */
static char *member(const char *t, const char *prop)
{
    return sfmt("(%s)['%s']", t, prop);
}

HRESULT shim_js_get_bstr(Page *pg, const char *t, const char *prop, BSTR *p)
{
    char *m, *x, *s;
    if (!p)
        return E_POINTER;
    *p = NULL;
    if (!pg)
        return S_OK;
    if (stashed(t) >= 0 &&
        (rc_elem_prop(pg, stashed(t), prop, &s) ||
         ((!strcmp(prop, "id") || !strcmp(prop, "tagName") || !strcmp(prop, "className")) &&
          rc_elem_fetch(pg, stashed(t)) && rc_elem_prop(pg, stashed(t), prop, &s)))) {   /* rcache.c */
        if (s)
            *p = utf8_to_bstr(s);
        free(s);
        return S_OK;
    }
    m = member(t, prop);
    x = m ? sfmt("(function(v){return v===null||v===undefined?null:String(v);})(%s)", m) : NULL;
    rc_pure_enter();
    s = x ? dom_eval_str(pg, x) : NULL;
    rc_pure_leave();
    if (s)
        *p = utf8_to_bstr(s);
    free(s);
    free(x);
    free(m);
    return S_OK;
}

static double get_num(Page *pg, const char *t, const char *prop)
{
    char *m, *x;
    double d = 0;
    long len;
    if (!strcmp(prop, "length") && stashed(t) >= 0 && rc_col_length(pg, stashed(t), &len))
        return (double)len;     /* rcache.c */
    m = member(t, prop);
    x = m ? sfmt("Number(%s)||0", m) : NULL;
    rc_pure_enter();
    if (x)
        dom_eval_num(pg, x, &d);
    rc_pure_leave();
    free(x);
    free(m);
    return d;
}

HRESULT shim_js_get_long(Page *pg, const char *t, const char *prop, LONG *p)
{
    if (!p)
        return E_POINTER;
    *p = pg ? (LONG)get_num(pg, t, prop) : 0;
    return S_OK;
}

HRESULT shim_js_get_short(Page *pg, const char *t, const char *prop, short *p)
{
    if (!p)
        return E_POINTER;
    *p = pg ? (short)get_num(pg, t, prop) : 0;
    return S_OK;
}

HRESULT shim_js_get_float(Page *pg, const char *t, const char *prop, float *p)
{
    if (!p)
        return E_POINTER;
    *p = pg ? (float)get_num(pg, t, prop) : 0.0f;
    return S_OK;
}

HRESULT shim_js_get_bool(Page *pg, const char *t, const char *prop, VARIANT_BOOL *p)
{
    char *m, *x;
    int b = 0;
    if (!p)
        return E_POINTER;
    *p = VARIANT_FALSE;
    if (!pg)
        return S_OK;
    m = member(t, prop);
    x = m ? sfmt("!!%s", m) : NULL;
    rc_pure_enter();
    if (x)
        dom_eval_bool(pg, x, &b);
    rc_pure_leave();
    *p = b ? VARIANT_TRUE : VARIANT_FALSE;
    free(x);
    free(m);
    return S_OK;
}

HRESULT shim_js_get_variant(Page *pg, const char *t, const char *prop, VARIANT *p)
{
    char *m;
    if (!p)
        return E_POINTER;
    VariantInit(p);
    if (!pg)
        return S_OK;
    m = member(t, prop);
    rc_pure_enter();
    if (m)
        dom_eval_variant(pg, m, p);
    rc_pure_leave();
    free(m);
    return S_OK;
}

static long get_obj(Page *pg, const char *t, const char *prop)
{
    char *m = member(t, prop);
    long id;
    rc_pure_enter();
    id = m ? dom_stash(pg, m) : -1;
    rc_pure_leave();
    free(m);
    return id;
}

HRESULT shim_js_get_elem(Page *pg, const char *t, const char *prop, IHTMLElement **p)
{
    long id;
    if (!p)
        return E_POINTER;
    *p = NULL;
    if (pg && (id = get_obj(pg, t, prop)) >= 0)
        *p = elem_new(pg, id);
    return S_OK;
}

HRESULT shim_js_get_col(Page *pg, const char *t, const char *prop, IHTMLElementCollection **p)
{
    long id;
    if (!p)
        return E_POINTER;
    *p = NULL;
    if (pg && (id = get_obj(pg, t, prop)) >= 0)
        *p = col_new(pg, id);
    return S_OK;
}

HRESULT shim_js_get_node(Page *pg, const char *t, const char *prop, IHTMLDOMNode **p)
{
    IHTMLElement *el = NULL;
    if (!p)
        return E_POINTER;
    *p = NULL;
    shim_js_get_elem(pg, t, prop, &el);
    if (el) {
        IHTMLElement_QueryInterface(el, &IID_IHTMLDOMNode, (void **)p);
        IHTMLElement_Release(el);
    }
    return S_OK;
}

HRESULT shim_js_get_disp(Page *pg, const char *t, const char *prop, IDispatch **p)
{
    return shim_js_get_elem(pg, t, prop, (IHTMLElement **)p);
}

/* (t)['prop'] = value; `value` is freed */
static HRESULT put_js(Page *pg, const char *t, const char *prop, char *value)
{
    char *m, *x;
    if (!pg || !value) {
        free(value);
        return S_OK;
    }
    m = member(t, prop);
    x = m ? sfmt("%s=%s", m, value) : NULL;
    if (x)
        dom_exec(pg, x);
    free(x);
    free(m);
    free(value);
    return S_OK;
}

HRESULT shim_js_put_bstr(Page *pg, const char *t, const char *prop, BSTR v)
{
    return put_js(pg, t, prop, js_lit_bstr(v));
}

HRESULT shim_js_put_long(Page *pg, const char *t, const char *prop, LONG v)
{
    return put_js(pg, t, prop, sfmt("%ld", (long)v));
}

HRESULT shim_js_put_short(Page *pg, const char *t, const char *prop, short v)
{
    return put_js(pg, t, prop, sfmt("%d", (int)v));
}

HRESULT shim_js_put_float(Page *pg, const char *t, const char *prop, float v)
{
    return put_js(pg, t, prop, sfmt("%.9g", (double)v));
}

HRESULT shim_js_put_bool(Page *pg, const char *t, const char *prop, VARIANT_BOOL v)
{
    return put_js(pg, t, prop, dup_utf8(v ? "true" : "false"));
}

HRESULT shim_js_put_variant(Page *pg, const char *t, const char *prop, VARIANT v)
{
    return put_js(pg, t, prop, dom_variant_js(&v));
}

HRESULT shim_js_call0(Page *pg, const char *t, const char *method)
{
    char *x;
    if (!pg)
        return S_OK;
    if (!strcmp(method, "click") || !strcmp(method, "submit"))
        dom_log_click(pg, t, method);
    x = sfmt("(%s)['%s']()", t, method);
    if (x)
        dom_exec(pg, x);
    free(x);
    return S_OK;
}
