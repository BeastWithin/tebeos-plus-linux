/* element.c -- the page's DOM behind the shim's element, collection, style, text-range and
 * event objects (see dom.h).
 *
 * Every call is a Runtime.evaluate on the page the object came from. Most plain properties
 * never reach this file: gen-stubs.py turns them into jsprop.c round-trips. What is here is
 * what IE does differently from a plain property: getAttribute/setAttribute (IE7 reads and
 * writes the property first), IDispatch name lookup (case-insensitive), attachEvent (the
 * listener reports through the page's __shim_evt binding; page.c brings it to the UI
 * thread), collections and DrawToDC.
 */

#include "html_doc.h"
#include "stubs.h"
#include "dom.h"
#include "rcache.h"
#include "dispmap.h"
#include "page.h"
#include "log.h"
#include "util.h"
#include "stb_image.h"   /* stbi_load_from_memory (defined in render.c) for DrawToDC */
#include <oaidl.h>
#include <cguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ page-side helpers */

/* window.__shim_stash(x, B): the object's id, stashing it on first sight. B is the first id
 * of a document that has not stashed anything yet (page.c hands out a fresh block per
 * document). Primitives, null and undefined give -1; document.all is an object here even
 * though typeof says "undefined". */
/* STASH_DEF: see dom.h */

/* A value as {k: kind, v: value}; objects are stashed. */
#define VARIANT_FN \
    "(function(v,B){var t=typeof v;if(v===undefined)return{k:'u'};if(v===null)return{k:'n'};" \
    "if(t==='string')return{k:'s',v:v};if(t==='boolean')return{k:'b',v:v};" \
    "if(t==='number')return(Number.isInteger(v)&&v>=-2147483648&&v<=2147483647)?{k:'i',v:v}:{k:'d',v:v};" \
    "if(t==='bigint')return{k:'d',v:Number(v)};if(t==='symbol')return{k:'s',v:String(v)};" \
    "if(v instanceof HTMLCollection||v instanceof NodeList)return{k:'c',v:window.__shim_stash(v,B)};" \
    "return{k:'o',v:window.__shim_stash(v,B)};})"

/* IDispatch::Invoke on a page object. n: member name or null for DISPID_VALUE; a: the
 * arguments; f: the DISPATCH_* flags. IE resolves member names case-insensitively. */
#define INVOKE_FN \
    "(function(o,n,a,f){if(n===null){if((f&1)&&typeof o==='function')return o.apply(null,a);return o;}" \
    "var k=n;if(!(k in o)){var l=n.toLowerCase();for(var p in o){if(p.toLowerCase()===l){k=p;break;}}}" \
    "if(f&12){o[k]=a[a.length-1];return undefined;}" \
    "var v=o[k];if(typeof v==='function'&&((f&1)||a.length))return v.apply(o,a);" \
    "if(a.length&&v!==null&&v!==undefined&&typeof v==='object')return v[a[0]];return v;})"

/* IE7 getAttribute: the property when it holds a plain value, else the attribute. IE's
 * form.action is the attribute as written ("/eczane/faces/login.jsp"), which the app's
 * GetStatus compares against; Chromium's is resolved to an absolute URL. */
#define GETATTR_FN \
    "(function(e,n){var l=n.toLowerCase(),k=l==='class'?'className':l==='for'?'htmlFor':n;" \
    "if(l==='action'&&e.getAttribute)return e.getAttribute('action')||'';" \
    "if(!(k in e)){for(var p in e){if(p.toLowerCase()===l){k=p;break;}}}" \
    "var v=(k in e)?e[k]:undefined;" \
    "if(v!==undefined&&v!==null&&typeof v!=='object'&&typeof v!=='function')return v;" \
    "return e.getAttribute?e.getAttribute(n):null;})"

/* IE7 setAttribute: a plain-valued property is set (converted to its type), else the attribute. */
#define SETATTR_FN \
    "(function(e,n,v){var l=n.toLowerCase(),k=l==='class'?'className':l==='for'?'htmlFor':n;" \
    "if(!(k in e)){for(var p in e){if(p.toLowerCase()===l){k=p;break;}}}" \
    "if(k in e){var c=e[k],t=typeof c;" \
    "if(t==='boolean'){e[k]=!(v===false||v==='false'||v===''||v===0);return;}" \
    "if(t==='number'){e[k]=Number(v);return;}" \
    "if(t==='string'){e[k]=String(v);return;}}" \
    "e.setAttribute(n,v);})"

/* attachEvent: one listener per (object, event) that reports to the __shim_evt binding.
 * The vendor's handlers run later (the binding is asynchronous), so an IE
 * event.cancelBubble = true they set cannot stop the event any more. Every click handler
 * TEBEOS attaches to a checkbox does that first thing -- its "kontrol edildi" box in each
 * Recete Listesi row, where the click bubbling on to the row opened the prescription -- so
 * a click on a checkbox or radio with a vendor click handler stops there, as in IE.
 * For the same reason a vendor's mousedown/mouseup on a checkbox/radio (or around one) would
 * see the box already flipped by the click; q is iecompat.js's token for holding that click
 * back until shim_dom_event_dispatch has run the handlers (and answers __shim_ack(q)). */
#define LISTEN_FN \
    "(function(o,h,t,B){var L=o.__shim_l||(o.__shim_l={});if(L[t])return;L[t]=1;" \
    "o.addEventListener(t,function(e){" \
    "if(t==='click'&&e.target===o&&o.tagName==='INPUT'&&(o.type==='checkbox'||o.type==='radio'))" \
    "e.stopPropagation();" \
    "if(!window.__shim_evt)return;var s=-1,q=0;" \
    "try{if(e.target&&e.target.nodeType===1)s=window.__shim_stash(e.target,B);}catch(x){}" \
    "try{if(window.__shim_hold)q=window.__shim_hold(e);}catch(x){}" \
    "window.__shim_evt(JSON.stringify({h:h,t:t,s:s,q:q,k:e.keyCode|0,b:e.button|0,x:e.clientX|0,y:e.clientY|0," \
    "a:!!e.altKey,c:!!e.ctrlKey,f:!!e.shiftKey}));},false);})"

#define EL "window.__shim_els[%ld]"

char *dom_ref(long id)
{
    return sfmt(EL, id);
}

JVal *dom_eval(Page *pg, const char *expr, JVal **reply)
{
    JVal *res, *exc;
    char *q, *params;
    int rc;

    *reply = NULL;
    if (!pg || !page_dom(pg) || !expr)
        return NULL;
    q = j_quote(expr);
    params = q ? sfmt("{\"expression\":%s,\"returnByValue\":true,\"userGesture\":true}", q) : NULL;
    free(q);
    if (!params)
        return NULL;
    rc = page_call(pg, "Runtime.evaluate", params, reply);
    free(params);
    if (rc != 0) {
        shim_log("EVAL transport failure: %.200s", expr);
        return NULL;
    }
    res = j_get(*reply, "result");
    if ((exc = j_get(res, "exceptionDetails"))) {
        const char *d = j_str(j_get(j_get(exc, "exception"), "description"));
        if (!d)
            d = j_str(j_get(exc, "text"));
        shim_log("EVAL threw %.300s  <- %.300s", d ? d : "?", expr);
        return NULL;
    }
    if (!res && j_get(*reply, "error"))
        shim_log("EVAL error %.200s  <- %.200s", j_str(j_get(j_get(*reply, "error"), "message")) ?
                 j_str(j_get(j_get(*reply, "error"), "message")) : "?", expr);
    return j_get(res, "result");
}

char *dom_eval_str(Page *pg, const char *expr)
{
    JVal *reply, *ro = dom_eval(pg, expr, &reply);
    JVal *v = j_get(ro, "value");
    char *out = NULL;

    if (v && v->t == J_STR)
        out = dup_utf8(v->s);
    else if (v && v->t == J_NUM)
        out = sfmt("%.17g", v->num);
    else if (v && v->t == J_BOOL)
        out = dup_utf8(v->b ? "true" : "false");
    j_free(reply);
    return out;
}

int dom_eval_num(Page *pg, const char *expr, double *out)
{
    JVal *reply, *ro = dom_eval(pg, expr, &reply);
    JVal *v = j_get(ro, "value");
    int rc = -1;

    *out = 0.0;
    if (v && v->t == J_NUM) {
        *out = v->num;
        rc = 0;
    } else if (v && v->t == J_BOOL) {
        *out = v->b;
        rc = 0;
    }
    j_free(reply);
    return rc;
}

int dom_eval_bool(Page *pg, const char *expr, int *out)
{
    JVal *reply, *ro = dom_eval(pg, expr, &reply);
    JVal *v = j_get(ro, "value");
    int rc = -1;

    *out = 0;
    if (v && (v->t == J_BOOL || v->t == J_NUM)) {
        *out = v->t == J_BOOL ? v->b : v->num != 0.0;
        rc = 0;
    }
    j_free(reply);
    return rc;
}

void dom_exec(Page *pg, const char *expr)
{
    JVal *reply;
    dom_eval(pg, expr, &reply);
    j_free(reply);
}

long dom_stash(Page *pg, const char *expr)
{
    long base = page_stash_base(pg);
    char *e = sfmt("%s(%s,%ld)", STASH_DEF, expr, base);
    double id = -1;

    if (!e || dom_eval_num(pg, e, &id) != 0)
        id = -1;
    free(e);
    if (id >= 0)
        page_stash_used(pg, (long)id);
    return id >= 0 ? (long)id : -1;
}

/* Stash `expr` (freed) and wrap it. */
static HRESULT stash_elem(Page *pg, char *expr, IHTMLElement **out)
{
    long id;
    *out = NULL;
    if (!expr)
        return E_OUTOFMEMORY;
    id = dom_stash(pg, expr);
    free(expr);
    if (id < 0)
        return S_OK;    /* not found: NULL, as IE */
    *out = elem_new(pg, id);
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* stash_elem for an expression that only looks something up (keeps rcache.c's cache) */
static HRESULT stash_elem_pure(Page *pg, char *expr, IHTMLElement **out)
{
    HRESULT hr;
    rc_pure_enter();
    hr = stash_elem(pg, expr, out);
    rc_pure_leave();
    return hr;
}

/* every caller passes a lookup (children, getElementsByTagName, ...): a pure read */
static HRESULT stash_col(Page *pg, char *expr, IHTMLElementCollection **out)
{
    long id;
    *out = NULL;
    if (!expr)
        return E_OUTOFMEMORY;
    rc_pure_enter();
    id = dom_stash(pg, expr);
    rc_pure_leave();
    free(expr);
    if (id < 0)
        return S_OK;
    *out = col_new(pg, id);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT stash_node(Page *pg, char *expr, IHTMLDOMNode **out)
{
    IHTMLElement *el = NULL;
    HRESULT hr = stash_elem(pg, expr, &el);
    *out = NULL;
    if (el) {
        hr = el->lpVtbl->QueryInterface(el, &IID_IHTMLDOMNode, (void **)out);
        el->lpVtbl->Release(el);
    }
    return hr;
}

HRESULT dom_eval_variant(Page *pg, const char *expr, VARIANT *out)
{
    long base = page_stash_base(pg);
    char *e = sfmt("(%s,%s(%s,%ld))", STASH_DEF, VARIANT_FN, expr, base);
    JVal *reply, *ro, *v, *val;
    const char *k;
    HRESULT hr = S_OK;

    VariantInit(out);
    if (!e)
        return E_OUTOFMEMORY;
    ro = dom_eval(pg, e, &reply);
    free(e);
    v = j_get(ro, "value");
    k = j_str(j_get(v, "k"));
    val = j_get(v, "v");
    if (!k) {
        hr = E_FAIL;
    } else if (*k == 'n') {
        V_VT(out) = VT_NULL;
    } else if (*k == 's') {
        V_VT(out) = VT_BSTR;
        V_BSTR(out) = utf8_to_bstr(j_str(val) ? j_str(val) : "");
    } else if (*k == 'b') {
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = j_is_true(val) ? VARIANT_TRUE : VARIANT_FALSE;
    } else if (*k == 'i') {
        V_VT(out) = VT_I4;
        V_I4(out) = (LONG)j_num(val);
    } else if (*k == 'd') {
        V_VT(out) = VT_R8;
        V_R8(out) = j_num(val);
    } else if (*k == 'c') {
        /* a collection (row.cells, form.elements, ...): an IHTMLElementCollection, as IE */
        long id = (long)j_num(val);
        if (id < 0) {
            V_VT(out) = VT_NULL;
        } else {
            page_stash_used(pg, id);
            V_VT(out) = VT_DISPATCH;
            V_DISPATCH(out) = (IDispatch *)col_new(pg, id);
        }
    } else if (*k == 'o') {
        long id = (long)j_num(val);
        if (id < 0) {
            V_VT(out) = VT_NULL;
        } else {
            page_stash_used(pg, id);
            V_VT(out) = VT_DISPATCH;
            V_DISPATCH(out) = (IDispatch *)elem_new(pg, id);
        }
    }   /* 'u': VT_EMPTY */
    j_free(reply);
    return hr;
}

char *dom_variant_js(const VARIANT *in)
{
    VARIANT v, n;
    char *out = NULL;

    VariantInit(&v);
    VariantInit(&n);
    if (FAILED(VariantCopyInd(&v, (VARIANT *)in)))
        return dup_utf8("undefined");
    switch (V_VT(&v)) {
    case VT_EMPTY:
        out = dup_utf8("undefined");
        break;
    case VT_NULL:
        out = dup_utf8("null");
        break;
    case VT_BSTR:
        out = js_lit_bstr(V_BSTR(&v));
        break;
    case VT_BOOL:
        out = dup_utf8(V_BOOL(&v) ? "true" : "false");
        break;
    case VT_DISPATCH:
    case VT_UNKNOWN: {
        HtmlElement *e = V_UNKNOWN(&v) ? elem_unwrap(V_UNKNOWN(&v)) : NULL;
        if (e) {
            out = dom_ref(e->handle);
            html_elem_release(e);
        } else {
            if (V_UNKNOWN(&v))
                shim_log("ARG a foreign object cannot reach the page; passing null");
            out = dup_utf8("null");
        }
        break;
    }
    default:
        if (SUCCEEDED(VariantChangeType(&n, &v, 0, VT_R8)))
            out = sfmt("%.17g", V_R8(&n));
        else if (SUCCEEDED(VariantChangeType(&n, &v, 0, VT_BSTR)))
            out = js_lit_bstr(V_BSTR(&n));
        else {
            shim_log("ARG variant type %d unsupported; passing null", V_VT(&v));
            out = dup_utf8("null");
        }
        break;
    }
    VariantClear(&n);
    VariantClear(&v);
    return out;
}

/* ------------------------------------------------------------------ IDispatch (generic) */

static SRWLOCK g_names_lock = SRWLOCK_INIT;
static char **g_names;
static UINT g_names_n, g_names_cap;

/* DISPID for `name`: 1-based index into the process-wide name table. The first three are
 * fixed (click=1, focus=2, blur=3) to stay stable for the test harnesses. */
static DISPID name_dispid(const char *name)
{
    static const char *seed[] = { "click", "focus", "blur" };
    UINT i;
    DISPID id = DISPID_UNKNOWN;

    AcquireSRWLockExclusive(&g_names_lock);
    if (!g_names_n) {
        g_names_cap = 64;
        g_names = (char **)calloc(g_names_cap, sizeof(char *));
        for (i = 0; g_names && i < ARRAY_SIZE(seed); i++)
            g_names[g_names_n++] = dup_utf8(seed[i]);
    }
    for (i = 0; i < g_names_n; i++)
        if (!_stricmp(g_names[i], name)) {
            id = (DISPID)(i + 1);
            break;
        }
    if (id == DISPID_UNKNOWN && g_names) {
        if (g_names_n == g_names_cap) {
            char **nn = (char **)realloc(g_names, g_names_cap * 2 * sizeof(char *));
            if (nn) {
                g_names = nn;
                g_names_cap *= 2;
            }
        }
        if (g_names_n < g_names_cap) {
            g_names[g_names_n] = dup_utf8(name);
            id = (DISPID)(++g_names_n);
        }
    }
    ReleaseSRWLockExclusive(&g_names_lock);
    return id;
}

static char *dispid_name(DISPID id)
{
    char *n = NULL;
    AcquireSRWLockShared(&g_names_lock);
    if (id >= 1 && (UINT)id <= g_names_n)
        n = dup_utf8(g_names[id - 1]);
    ReleaseSRWLockShared(&g_names_lock);
    return n;
}

HRESULT dom_disp_ids(LPOLESTR *names, UINT n, DISPID *ids)
{
    UINT i;
    HRESULT hr = S_OK;

    if (!names || !ids)
        return E_INVALIDARG;
    for (i = 0; i < n; i++) {
        char *u = wstr_to_utf8(names[i]);
        ids[i] = u ? name_dispid(u) : DISPID_UNKNOWN;
        if (ids[i] == DISPID_UNKNOWN)
            hr = DISP_E_UNKNOWNNAME;
        free(u);
    }
    return hr;
}

/* One log line per click or submit the vendor's code makes, for tracing its automation (it
 * presses real Medula buttons). Names the element by id/name and, for buttons and links, the
 * label; never a field's value. */
void dom_log_click(Page *pg, const char *target, const char *what)
{
    char *x = sfmt("(function(e){if(!e||!e.tagName)return '?';var t=e.tagName,y=(e.type||'').toLowerCase(),l='';"
                   "if(t==='BUTTON'||t==='A'||(t==='INPUT'&&/^(button|submit|reset|image)$/.test(y)))"
                   "l=String(t==='INPUT'?(e.value||e.alt||''):(e.innerText||'')).replace(/\\s+/g,' ').slice(0,40);"
                   "return t+(y?'['+y+']':'')+' id='+(e.id||'-')+(e.name&&e.name!==e.id?' name='+e.name:'')+(l?' \\''+l+'\\'':'')"
                   "+' on '+location.pathname.replace(/^.*\\//,'').slice(0,60);})(%s)", target);
    char *d = x ? dom_eval_str(pg, x) : NULL;
    shim_log("CLICK %s %s", what, d ? d : "?");
    free(d);
    free(x);
}

HRESULT dom_disp_invoke(Page *pg, const char *target, DISPID id, WORD flags, DISPPARAMS *dp, VARIANT *res)
{
    char *name = NULL, *nlit, *args, *expr;
    size_t cap = 64, len = 0;
    UINT i, n;
    VARIANT tmp;
    HRESULT hr;

    if (res)
        VariantInit(res);
    if (id != DISPID_VALUE && !(name = dispid_name(id)))
        return DISP_E_MEMBERNOTFOUND;
    if (!pg) {
        free(name);
        return E_FAIL;
    }
    /* rgvarg is in reverse order; a property put carries its value as the named argument */
    args = (char *)malloc(cap);
    if (args)
        args[0] = '\0';
    n = dp ? dp->cArgs : 0;
    for (i = 0; args && i < n; i++) {
        char *a = dom_variant_js(&dp->rgvarg[n - 1 - i]);
        size_t al = a ? strlen(a) : 4;
        if (len + al + 2 > cap) {
            while (len + al + 2 > cap)
                cap *= 2;
            args = (char *)realloc(args, cap);
        }
        if (args) {
            len += (size_t)sprintf(args + len, "%s%s", i ? "," : "", a ? a : "null");
        }
        free(a);
    }
    if (name && (!_stricmp(name, "click") || !_stricmp(name, "submit")) && strcmp(target, "window") != 0)
        dom_log_click(pg, target, name);
    nlit = name ? js_lit(name) : dup_utf8("null");
    expr = args && nlit ? sfmt("%s(%s,%s,[%s],%u)", INVOKE_FN, target, nlit, args, (unsigned)flags) : NULL;
    free(args);
    free(nlit);
    if (!expr) {
        free(name);
        return E_OUTOFMEMORY;
    }
    hr = dom_eval_variant(pg, expr, res ? res : &tmp);
    if (!res)
        VariantClear(&tmp);
    if (FAILED(hr))
        shim_log("INVOKE %s.%s failed", target, name ? name : "(value)");
    free(expr);
    free(name);
    return FAILED(hr) ? DISP_E_EXCEPTION : S_OK;
}

/* ------------------------------------------------------------------ HtmlElement */

static IHTMLElementRenderVtbl g_IHTMLElementRender_vtbl;

IHTMLElement *elem_new(Page *pg, long id)
{
    HtmlElement *e;
    if (!pg || id < 0)
        return NULL;
    e = (HtmlElement *)calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->IHTMLElement_iface.lpVtbl = &g_IHTMLElement_vtbl;
    e->IHTMLElement2_iface.lpVtbl = &g_IHTMLElement2_vtbl;
    e->IHTMLElement3_iface.lpVtbl = &g_IHTMLElement3_vtbl;
    e->IHTMLElement4_iface.lpVtbl = &g_IHTMLElement4_vtbl;
    e->IHTMLInputElement_iface.lpVtbl = &g_IHTMLInputElement_vtbl;
    e->IHTMLInputTextElement_iface.lpVtbl = &g_IHTMLInputTextElement_vtbl;
    e->IHTMLSelectElement_iface.lpVtbl = &g_IHTMLSelectElement_vtbl;
    e->IHTMLOptionElement_iface.lpVtbl = &g_IHTMLOptionElement_vtbl;
    e->IHTMLTextAreaElement_iface.lpVtbl = &g_IHTMLTextAreaElement_vtbl;
    e->IHTMLScriptElement_iface.lpVtbl = &g_IHTMLScriptElement_vtbl;
    e->IHTMLFormElement_iface.lpVtbl = &g_IHTMLFormElement_vtbl;
    e->IHTMLAnchorElement_iface.lpVtbl = &g_IHTMLAnchorElement_vtbl;
    e->IHTMLImgElement_iface.lpVtbl = &g_IHTMLImgElement_vtbl;
    e->IHTMLDOMNode_iface.lpVtbl = &g_IHTMLDOMNode_vtbl;
    e->IHTMLElementRender_iface.lpVtbl = &g_IHTMLElementRender_vtbl;
    e->ref = 1;
    e->handle = id;
    e->page = pg;
    page_addref(pg);
    return &e->IHTMLElement_iface;
}

ULONG html_elem_addref(HtmlElement *e)
{
    return InterlockedIncrement(&e->ref);
}

ULONG html_elem_release(HtmlElement *e)
{
    LONG ref = InterlockedDecrement(&e->ref);
    if (ref == 0) {
        page_release(e->page);
        free(e);
    }
    return ref;
}

static const char *elem_tag(HtmlElement *e)
{
    if (!e->tag[0]) {
        char *x = sfmt("(function(e){return e.tagName?String(e.tagName).toUpperCase():'#';})(" EL ")", e->handle);
        char *t = x ? dom_eval_str(e->page, x) : NULL;
        snprintf(e->tag, sizeof(e->tag), "%s", t ? t : "#");
        free(t);
        free(x);
    }
    return e->tag;
}

HRESULT html_elem_qi(HtmlElement *e, REFIID riid, void **ppv)
{
    const char *need = NULL;

    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &IID_IHTMLElement))
        *ppv = &e->IHTMLElement_iface;
    else if (IsEqualIID(riid, &IID_IHTMLElement2))
        *ppv = &e->IHTMLElement2_iface;
    else if (IsEqualIID(riid, &IID_IHTMLElement3))
        *ppv = &e->IHTMLElement3_iface;
    else if (IsEqualIID(riid, &IID_IHTMLElement4))
        *ppv = &e->IHTMLElement4_iface;
    else if (IsEqualIID(riid, &IID_IHTMLDOMNode))
        *ppv = &e->IHTMLDOMNode_iface;
    else if (IsEqualIID(riid, &IID_IHTMLElementRender))
        *ppv = &e->IHTMLElementRender_iface;
    else if (IsEqualIID(riid, &IID_IShimElement))
        *ppv = e;
    else if (IsEqualIID(riid, &IID_IHTMLInputElement)) {
        need = "INPUT"; *ppv = &e->IHTMLInputElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLInputTextElement)) {
        need = "INPUT"; *ppv = &e->IHTMLInputTextElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLSelectElement)) {
        need = "SELECT"; *ppv = &e->IHTMLSelectElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLOptionElement)) {
        need = "OPTION"; *ppv = &e->IHTMLOptionElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLTextAreaElement)) {
        need = "TEXTAREA"; *ppv = &e->IHTMLTextAreaElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLScriptElement)) {
        need = "SCRIPT"; *ppv = &e->IHTMLScriptElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLFormElement)) {
        need = "FORM"; *ppv = &e->IHTMLFormElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLAnchorElement)) {
        need = "A"; *ppv = &e->IHTMLAnchorElement_iface;
    } else if (IsEqualIID(riid, &IID_IHTMLImgElement)) {
        need = "IMG"; *ppv = &e->IHTMLImgElement_iface;
    }
    if (*ppv && need && strcmp(elem_tag(e), need) != 0)
        *ppv = NULL;    /* a tag-specific interface on another tag: E_NOINTERFACE, as IE */
    if (!*ppv && !need &&
        SUCCEEDED(dispwrap_query(DISP_ELEM, (IDispatch *)&e->IHTMLElement_iface, riid, ppv)))
        return S_OK;    /* DispHTMLTableRow/TableCell/Body...: the vendor's (HTMLTableRow) casts */
    if (!*ppv) {
        if (!need)
            shim_qi_miss("element", riid);
        return E_NOINTERFACE;
    }
    html_elem_addref(e);
    return S_OK;
}

HtmlElement *elem_unwrap(IUnknown *unk)
{
    HtmlElement *e = NULL;
    if (!unk || FAILED(unk->lpVtbl->QueryInterface(unk, &IID_IShimElement, (void **)&e)))
        return NULL;
    return e;
}

#define ELEM_OF(IFACE, THIS) CONTAINING_RECORD(THIS, HtmlElement, IFACE##_iface)

/* ------------------------------------------------------------------ IHTMLElement */

HRESULT STDMETHODCALLTYPE html_elem_GetTypeInfoCount(IHTMLElement *This, UINT *pctinfo)
{
    if (!pctinfo)
        return E_INVALIDARG;
    *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_elem_GetTypeInfo(IHTMLElement *This, UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
{
    if (ppTInfo)
        *ppTInfo = NULL;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE html_elem_GetIDsOfNames(IHTMLElement *This, REFIID riid, LPOLESTR *rgszNames,
                                                 UINT cNames, LCID lcid, DISPID *rgDispId)
{
    return dom_disp_ids(rgszNames, cNames, rgDispId);
}

HRESULT STDMETHODCALLTYPE html_elem_Invoke(IHTMLElement *This, DISPID dispIdMember, REFIID riid, LCID lcid,
                                          WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                          EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    char *t = dom_ref(e->handle);
    HRESULT hr = t ? dom_disp_invoke(e->page, t, dispIdMember, wFlags, pDispParams, pVarResult) : E_OUTOFMEMORY;
    free(t);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_elem_getAttribute(IHTMLElement *This, BSTR strAttributeName, LONG lFlags,
                                                VARIANT *AttributeValue)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    char *n = js_lit_bstr(strAttributeName), *x;
    HRESULT hr;

    if (!AttributeValue)
        return E_POINTER;
    VariantInit(AttributeValue);
    if (!(lFlags & 2) && strAttributeName) {  /* IE7's getAttribute: the id / className property */
        const char *prop = !lstrcmpiW(strAttributeName, L"id") ? "id" :
                           (!lstrcmpiW(strAttributeName, L"className") || !lstrcmpiW(strAttributeName, L"class"))
                           ? "className" : NULL;
        char *v = NULL;
        if (prop && (rc_elem_prop(e->page, e->handle, prop, &v) ||
                     (rc_elem_fetch(e->page, e->handle) && rc_elem_prop(e->page, e->handle, prop, &v)))) {   /* rcache.c */
            V_VT(AttributeValue) = VT_BSTR;
            V_BSTR(AttributeValue) = utf8_to_bstr(v ? v : "");
            free(v);
            free(n);
            return S_OK;
        }
    }
    x = n ? ((lFlags & 2) ? sfmt(EL ".getAttribute(%s)", e->handle, n)
                          : sfmt("%s(" EL ",%s)", GETATTR_FN, e->handle, n)) : NULL;
    free(n);
    if (!x)
        return E_OUTOFMEMORY;
    rc_pure_enter();
    hr = dom_eval_variant(e->page, x, AttributeValue);
    rc_pure_leave();
    free(x);
    if (FAILED(hr) || V_VT(AttributeValue) == VT_EMPTY) {
        VariantClear(AttributeValue);
        V_VT(AttributeValue) = VT_NULL;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_elem_setAttribute(IHTMLElement *This, BSTR strAttributeName,
                                                VARIANT AttributeValue, LONG lFlags)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    char *n = js_lit_bstr(strAttributeName), *v = dom_variant_js(&AttributeValue);
    char *x = n && v ? sfmt("%s(" EL ",%s,%s)", SETATTR_FN, e->handle, n, v) : NULL;

    if (x)
        dom_exec(e->page, x);
    free(x);
    free(n);
    free(v);
    return x ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE html_elem_removeAttribute(IHTMLElement *This, BSTR strAttributeName, LONG lFlags,
                                                   VARIANT_BOOL *pfSuccess)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    char *n = js_lit_bstr(strAttributeName);
    char *x = n ? sfmt("(function(e,n){var h=e.hasAttribute(n);e.removeAttribute(n);return h;})(" EL ",%s)",
                       e->handle, n) : NULL;
    int had = 0;

    if (x)
        dom_eval_bool(e->page, x, &had);
    if (pfSuccess)
        *pfSuccess = had ? VARIANT_TRUE : VARIANT_FALSE;
    free(x);
    free(n);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_elem_get_children(IHTMLElement *This, IDispatch **p)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    return stash_col(e->page, sfmt(EL ".children", e->handle), (IHTMLElementCollection **)p);
}

HRESULT STDMETHODCALLTYPE html_elem_get_all(IHTMLElement *This, IDispatch **p)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    return stash_col(e->page, sfmt(EL ".getElementsByTagName('*')", e->handle), (IHTMLElementCollection **)p);
}

HRESULT STDMETHODCALLTYPE html_elem_get_document(IHTMLElement *This, IDispatch **p)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    HTMLDocument *d = page_doc(e->page);

    if (!p)
        return E_POINTER;
    *p = d ? (IDispatch *)&d->IHTMLDocument2_iface : NULL;
    if (*p)
        IDispatch_AddRef(*p);
    return S_OK;
}

static HRESULT elem_call2(HtmlElement *e, const char *method, BSTR a, BSTR b)
{
    char *la = js_lit_bstr(a), *lb = js_lit_bstr(b);
    char *x = la && lb ? sfmt(EL ".%s(%s,%s)", e->handle, method, la, lb) : NULL;
    if (x)
        dom_exec(e->page, x);
    free(x);
    free(la);
    free(lb);
    return x ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE html_elem_insertAdjacentHTML(IHTMLElement *This, BSTR where, BSTR html)
{
    return elem_call2(ELEM_OF(IHTMLElement, This), "insertAdjacentHTML", where, html);
}

HRESULT STDMETHODCALLTYPE html_elem_insertAdjacentText(IHTMLElement *This, BSTR where, BSTR text)
{
    return elem_call2(ELEM_OF(IHTMLElement, This), "insertAdjacentText", where, text);
}

/* IE's where is case-insensitive ("BeforeBegin" from WinForms); the DOM takes it the same way */
HRESULT STDMETHODCALLTYPE html_elem2_insertAdjacentElement(IHTMLElement2 *This, BSTR where,
                                                           IHTMLElement *insertedElement, IHTMLElement **inserted)
{
    HtmlElement *e = ELEM_OF(IHTMLElement2, This), *n = elem_unwrap((IUnknown *)insertedElement);
    char *w = js_lit_bstr(where);
    HRESULT hr;

    if (inserted)
        *inserted = NULL;
    if (!n || !w) {
        if (n) html_elem_release(n);
        free(w);
        return E_INVALIDARG;
    }
    {
        IHTMLElement *r = NULL;
        hr = stash_elem(e->page, sfmt(EL ".insertAdjacentElement(%s," EL ")", e->handle, w, n->handle), &r);
        if (inserted)
            *inserted = r;
        else if (r)
            IHTMLElement_Release(r);
    }
    html_elem_release(n);
    free(w);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_elem_scrollIntoView(IHTMLElement *This, VARIANT varargStart)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    VARIANT b;
    char *x;

    VariantInit(&b);
    if (V_VT(&varargStart) == VT_EMPTY || V_VT(&varargStart) == VT_ERROR ||
        FAILED(VariantChangeType(&b, &varargStart, 0, VT_BOOL)))
        V_BOOL(&b) = VARIANT_TRUE;
    x = sfmt(EL ".scrollIntoView(%s)", e->handle, V_BOOL(&b) ? "true" : "false");
    if (x)
        dom_exec(e->page, x);
    free(x);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_elem_contains(IHTMLElement *This, IHTMLElement *pChild, VARIANT_BOOL *pfResult)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    HtmlElement *c = elem_unwrap((IUnknown *)pChild);
    int r = 0;

    if (!pfResult)
        return E_POINTER;
    if (c) {
        char *x = sfmt(EL ".contains(" EL ")", e->handle, c->handle);
        if (x)
            dom_eval_bool(e->page, x, &r);
        free(x);
        html_elem_release(c);
    }
    *pfResult = r ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

/* ------------------------------------------------------------------ IHTMLElement2 */

HRESULT STDMETHODCALLTYPE html_elem2_getElementsByTagName(IHTMLElement2 *This, BSTR v,
                                                          IHTMLElementCollection **pelColl)
{
    HtmlElement *e = ELEM_OF(IHTMLElement2, This);
    char *l = js_lit_bstr(v);
    HRESULT hr = stash_col(e->page, l ? sfmt(EL ".getElementsByTagName(%s)", e->handle, l) : NULL, pelColl);
    free(l);
    return hr;
}

/* IE moves the focus inside focus()/blur() but fires focus, blur, focusin and focusout
 * later, from the message loop (jQuery: on IE <= 11 "focus() and blur() are asynchronous").
 * Hosts lean on that: TEBEOS focuses Medula's empty T.C. field, fills the prescription number
 * and clicks Sorgula in one go, and the field's hxclient onfocus writes "___________" prompt
 * characters that only its onblur takes out. Chromium fires the events inside focus(), so the
 * underscores went into the POST and SGK answered with the empty form. So the events are
 * caught while the focus moves and replayed from the message loop (page_replay_focus). */
static const char hold_focus_js[] =
    "(function(e,m){var w=window,q=w.__shim_fq||(w.__shim_fq=[]),k=q.length,"
    "t=['focus','blur','focusin','focusout'];"
    "function h(v){if(v.target===w||v.target===document)return;v.stopImmediatePropagation();"
    "q.push([v.target,v.type,v.relatedTarget]);}"
    "t.forEach(function(y){w.addEventListener(y,h,true);});"
    "try{e[m]();}catch(x){}"
    "t.forEach(function(y){w.removeEventListener(y,h,true);});"
    "return q.length-k;})(" EL ",'%s')";

static HRESULT elem_focus_later(HtmlElement *e, const char *method)
{
    char *x;
    double n = 0;

    if (!e->page)
        return S_OK;
    x = sfmt(hold_focus_js, e->handle, method);
    if (x && dom_eval_num(e->page, x, &n) == 0 && n > 0)
        page_replay_focus(e->page);
    free(x);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_elem2_focus(IHTMLElement2 *This)
{
    return elem_focus_later(ELEM_OF(IHTMLElement2, This), "focus");
}

HRESULT STDMETHODCALLTYPE html_elem2_blur(IHTMLElement2 *This)
{
    return elem_focus_later(ELEM_OF(IHTMLElement2, This), "blur");
}

HRESULT STDMETHODCALLTYPE html_elem2_attachEvent(IHTMLElement2 *This, BSTR event, IDispatch *pDisp,
                                                 VARIANT_BOOL *pfResult)
{
    HtmlElement *e = ELEM_OF(IHTMLElement2, This);
    return dom_attach_event(e->page, e->handle, event, pDisp, pfResult);
}

HRESULT STDMETHODCALLTYPE html_elem2_detachEvent(IHTMLElement2 *This, BSTR event, IDispatch *pDisp)
{
    HtmlElement *e = ELEM_OF(IHTMLElement2, This);
    return dom_detach_event(e->page, e->handle, event, pDisp);
}

HRESULT STDMETHODCALLTYPE html_elem2_get_readyState(IHTMLElement2 *This, VARIANT *p)
{
    if (!p)
        return E_POINTER;
    /* an element that can be reached is in the DOM: IE answers "complete" */
    V_VT(p) = VT_BSTR;
    V_BSTR(p) = SysAllocString(L"complete");
    return V_BSTR(p) ? S_OK : E_OUTOFMEMORY;
}

/* ------------------------------------------------------------------ IHTMLDOMNode */

HRESULT STDMETHODCALLTYPE html_node_hasChildNodes(IHTMLDOMNode *This, VARIANT_BOOL *fChildren)
{
    HtmlElement *e = ELEM_OF(IHTMLDOMNode, This);
    char *x = sfmt(EL ".hasChildNodes()", e->handle);
    int r = 0;
    if (x)
        dom_eval_bool(e->page, x, &r);
    free(x);
    if (fChildren)
        *fChildren = r ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_node_get_childNodes(IHTMLDOMNode *This, IDispatch **p)
{
    HtmlElement *e = ELEM_OF(IHTMLDOMNode, This);
    return stash_col(e->page, sfmt(EL ".childNodes", e->handle), (IHTMLElementCollection **)p);
}

/* node method taking other nodes of ours: `fmt` gets this node's id then the others' */
static HRESULT node_op(HtmlElement *e, IHTMLDOMNode *a, IHTMLDOMNode *b, const char *fmt, IHTMLDOMNode **out)
{
    HtmlElement *ea = elem_unwrap((IUnknown *)a), *eb = b ? elem_unwrap((IUnknown *)b) : NULL;
    HRESULT hr;

    if (out)
        *out = NULL;
    if (!ea || (b && !eb)) {
        if (ea) html_elem_release(ea);
        if (eb) html_elem_release(eb);
        return E_INVALIDARG;
    }
    hr = stash_node(e->page, sfmt(fmt, e->handle, ea->handle, eb ? eb->handle : 0L), out);
    html_elem_release(ea);
    if (eb)
        html_elem_release(eb);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_node_appendChild(IHTMLDOMNode *This, IHTMLDOMNode *newChild, IHTMLDOMNode **node)
{
    return node_op(ELEM_OF(IHTMLDOMNode, This), newChild, NULL, EL ".appendChild(" EL ")", node);
}

HRESULT STDMETHODCALLTYPE html_node_removeChild(IHTMLDOMNode *This, IHTMLDOMNode *oldChild, IHTMLDOMNode **node)
{
    return node_op(ELEM_OF(IHTMLDOMNode, This), oldChild, NULL, EL ".removeChild(" EL ")", node);
}

HRESULT STDMETHODCALLTYPE html_node_replaceChild(IHTMLDOMNode *This, IHTMLDOMNode *newChild,
                                                 IHTMLDOMNode *oldChild, IHTMLDOMNode **node)
{
    return node_op(ELEM_OF(IHTMLDOMNode, This), newChild, oldChild, EL ".replaceChild(" EL "," EL ")", node);
}

HRESULT STDMETHODCALLTYPE html_node_insertBefore(IHTMLDOMNode *This, IHTMLDOMNode *newChild, VARIANT refChild,
                                                 IHTMLDOMNode **node)
{
    HtmlElement *e = ELEM_OF(IHTMLDOMNode, This);
    IUnknown *ref = (V_VT(&refChild) == VT_DISPATCH || V_VT(&refChild) == VT_UNKNOWN) ? V_UNKNOWN(&refChild) : NULL;

    if (ref)
        return node_op(e, newChild, (IHTMLDOMNode *)ref, EL ".insertBefore(" EL "," EL ")", node);
    return node_op(e, newChild, NULL, EL ".insertBefore(" EL ",null)", node);
}

HRESULT STDMETHODCALLTYPE html_node_cloneNode(IHTMLDOMNode *This, VARIANT_BOOL fDeep, IHTMLDOMNode **clonedNode)
{
    HtmlElement *e = ELEM_OF(IHTMLDOMNode, This);
    return stash_node(e->page, sfmt(EL ".cloneNode(%s)", e->handle, fDeep ? "true" : "false"), clonedNode);
}

/* ------------------------------------------------------------------ IHTMLDocument2/3 element lookups */

#define DOC2(THIS) CONTAINING_RECORD(THIS, HTMLDocument, IHTMLDocument2_iface)
#define DOC3(THIS) CONTAINING_RECORD(THIS, HTMLDocument, IHTMLDocument3_iface)

HRESULT STDMETHODCALLTYPE html_doc3_getElementById(IHTMLDocument3 *This, BSTR v, IHTMLElement **pel)
{
    char *l = js_lit_bstr(v);
    HRESULT hr;
    if (!pel)
        return E_POINTER;
    hr = stash_elem_pure(doc_page(DOC3(This)), l ? sfmt("document.getElementById(%s)", l) : NULL, pel);
    free(l);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_doc3_getElementsByTagName(IHTMLDocument3 *This, BSTR v,
                                                         IHTMLElementCollection **pelColl)
{
    char *l = js_lit_bstr(v);
    HRESULT hr;
    if (!pelColl)
        return E_POINTER;
    hr = stash_col(doc_page(DOC3(This)), l ? sfmt("document.getElementsByTagName(%s)", l) : NULL, pelColl);
    free(l);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_doc3_getElementsByName(IHTMLDocument3 *This, BSTR v,
                                                      IHTMLElementCollection **pelColl)
{
    char *l = js_lit_bstr(v);
    HRESULT hr;
    if (!pelColl)
        return E_POINTER;
    hr = stash_col(doc_page(DOC3(This)), l ? sfmt("document.getElementsByName(%s)", l) : NULL, pelColl);
    free(l);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_doc3_attachEvent(IHTMLDocument3 *This, BSTR event, IDispatch *pDisp,
                                                VARIANT_BOOL *pfResult)
{
    return dom_attach_event(doc_page(DOC3(This)), DOM_DOC_HANDLE, event, pDisp, pfResult);
}

HRESULT STDMETHODCALLTYPE html_doc3_detachEvent(IHTMLDocument3 *This, BSTR event, IDispatch *pDisp)
{
    return dom_detach_event(doc_page(DOC3(This)), DOM_DOC_HANDLE, event, pDisp);
}

HRESULT STDMETHODCALLTYPE html_doc_createElement(IHTMLDocument2 *This, BSTR eTag, IHTMLElement **newElem)
{
    char *l = js_lit_bstr(eTag);
    HRESULT hr;
    if (!newElem)
        return E_POINTER;
    /* IE also takes markup: createElement("<input name=x>") */
    hr = stash_elem(doc_page(DOC2(This)),
                    l ? sfmt("(function(t){if(t.charAt(0)==='<'){var d=document.createElement('div');"
                             "d.innerHTML=t;return d.firstChild;}return document.createElement(t);})(%s)", l) : NULL,
                    newElem);
    free(l);
    return hr;
}

static HRESULT doc_write(IHTMLDocument2 *This, SAFEARRAY *psarray, BOOL ln)
{
    Page *pg = doc_page(DOC2(This));
    VARIANT *vars;
    LONG lo, hi, i;
    size_t cap = 256, len = 0;
    char *buf, *lit, *x;

    if (!psarray || SafeArrayGetDim(psarray) != 1)
        return E_INVALIDARG;
    if (FAILED(SafeArrayGetLBound(psarray, 1, &lo)) || FAILED(SafeArrayGetUBound(psarray, 1, &hi)) ||
        FAILED(SafeArrayAccessData(psarray, (void **)&vars)))
        return E_INVALIDARG;
    buf = (char *)malloc(cap);
    if (buf)
        buf[0] = '\0';
    for (i = 0; buf && i <= hi - lo; i++) {
        VARIANT s;
        char *u;
        size_t ul;
        VariantInit(&s);
        if (FAILED(VariantChangeType(&s, &vars[i], 0, VT_BSTR)))
            continue;
        u = bstr_to_utf8(V_BSTR(&s));
        ul = u ? strlen(u) : 0;
        if (len + ul + 1 > cap) {
            while (len + ul + 1 > cap)
                cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        if (buf && u) {
            memcpy(buf + len, u, ul + 1);
            len += ul;
        }
        free(u);
        VariantClear(&s);
    }
    SafeArrayUnaccessData(psarray);
    if (!buf)
        return E_OUTOFMEMORY;
    lit = js_lit(buf);
    x = lit ? sfmt("document.%s(%s)", ln ? "writeln" : "write", lit) : NULL;
    if (x)
        dom_exec(pg, x);
    free(x);
    free(lit);
    free(buf);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_write(IHTMLDocument2 *This, SAFEARRAY *psarray)
{
    return doc_write(This, psarray, FALSE);
}

HRESULT STDMETHODCALLTYPE html_doc_writeln(IHTMLDocument2 *This, SAFEARRAY *psarray)
{
    return doc_write(This, psarray, TRUE);
}

/* ------------------------------------------------------------------ HtmlElementCollection */

IHTMLElementCollection *col_new(Page *pg, long id)
{
    HtmlElementCollection *c;
    if (!pg || id < 0)
        return NULL;
    c = (HtmlElementCollection *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->IHTMLElementCollection_iface.lpVtbl = &g_IHTMLElementCollection_vtbl;
    c->ref = 1;
    c->handle = id;
    c->page = pg;
    page_addref(pg);
    return &c->IHTMLElementCollection_iface;
}

ULONG html_col_addref(HtmlElementCollection *c)
{
    return InterlockedIncrement(&c->ref);
}

ULONG html_col_release(HtmlElementCollection *c)
{
    LONG ref = InterlockedDecrement(&c->ref);
    if (ref == 0) {
        page_release(c->page);
        free(c);
    }
    return ref;
}

HRESULT html_col_qi(HtmlElementCollection *c, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &IID_IHTMLElementCollection)) {
        *ppv = &c->IHTMLElementCollection_iface;
        html_col_addref(c);
        return S_OK;
    }
    *ppv = NULL;
    shim_qi_miss("collection", riid);
    return E_NOINTERFACE;
}

#define COL(THIS) CONTAINING_RECORD(THIS, HtmlElementCollection, IHTMLElementCollection_iface)

HRESULT STDMETHODCALLTYPE html_col_GetTypeInfoCount(IHTMLElementCollection *This, UINT *pctinfo)
{
    if (!pctinfo)
        return E_INVALIDARG;
    *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_col_GetTypeInfo(IHTMLElementCollection *This, UINT iTInfo, LCID lcid,
                                               ITypeInfo **ppTInfo)
{
    if (ppTInfo)
        *ppTInfo = NULL;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE html_col_GetIDsOfNames(IHTMLElementCollection *This, REFIID riid, LPOLESTR *rgszNames,
                                                 UINT cNames, LCID lcid, DISPID *rgDispId)
{
    return dom_disp_ids(rgszNames, cNames, rgDispId);
}

HRESULT STDMETHODCALLTYPE html_col_Invoke(IHTMLElementCollection *This, DISPID dispIdMember, REFIID riid,
                                          LCID lcid, WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                          EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HtmlElementCollection *c = COL(This);
    char *t;
    HRESULT hr;

    /* late-bound foreach and col(i) / col("name"): IE answers both on the collection itself */
    if (dispIdMember == DISPID_NEWENUM) {
        IUnknown *e = NULL;
        hr = html_col__newEnum(This, &e);
        if (SUCCEEDED(hr) && pVarResult) {
            V_VT(pVarResult) = VT_UNKNOWN;
            V_UNKNOWN(pVarResult) = e;
        } else if (e) {
            IUnknown_Release(e);
        }
        return hr;
    }
    if (dispIdMember == DISPID_VALUE && pDispParams && pDispParams->cArgs >= 1 &&
        (wFlags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET))) {
        VARIANT idx;
        IDispatch *d = NULL;
        VariantInit(&idx);
        V_VT(&idx) = VT_I4;
        V_I4(&idx) = 0;
        hr = html_col_item(This, pDispParams->rgvarg[pDispParams->cArgs - 1],
                           pDispParams->cArgs >= 2 ? pDispParams->rgvarg[pDispParams->cArgs - 2] : idx, &d);
        if (pVarResult) {
            VariantInit(pVarResult);
            if (d) {
                V_VT(pVarResult) = VT_DISPATCH;
                V_DISPATCH(pVarResult) = d;
            } else {
                V_VT(pVarResult) = VT_NULL;
            }
        } else if (d) {
            IDispatch_Release(d);
        }
        return hr;
    }
    t = dom_ref(c->handle);
    hr = t ? dom_disp_invoke(c->page, t, dispIdMember, wFlags, pDispParams, pVarResult) : E_OUTOFMEMORY;
    free(t);
    return hr;
}

/* item(name, index): a number picks by position; a string picks by id or name, the
 * index-th match (WinForms passes index 0). */
HRESULT STDMETHODCALLTYPE html_col_item(IHTMLElementCollection *This, VARIANT name, VARIANT index, IDispatch **pdisp)
{
    HtmlElementCollection *c = COL(This);
    VARIANT n, i;
    LONG idx = 0;
    char *x = NULL;
    HRESULT hr;

    if (!pdisp)
        return E_POINTER;
    *pdisp = NULL;
    VariantInit(&n);
    VariantInit(&i);
    VariantCopyInd(&n, &name);
    if (SUCCEEDED(VariantChangeType(&i, &index, 0, VT_I4)))
        idx = V_I4(&i);
    if (V_VT(&n) == VT_BSTR) {
        char *l = js_lit_bstr(V_BSTR(&n));
        x = l ? sfmt("(function(c,n,i){var m=0;for(var j=0;j<c.length;j++){var e=c[j];"
                     "if(e&&(e.id===n||e.name===n)){if(m===i)return e;m++;}}return null;})(" EL ",%s,%ld)",
                     c->handle, l, (long)idx) : NULL;
        free(l);
    } else {
        VARIANT k;
        VariantInit(&k);
        if (V_VT(&n) != VT_EMPTY && V_VT(&n) != VT_ERROR && SUCCEEDED(VariantChangeType(&k, &n, 0, VT_I4)))
            idx = V_I4(&k);
        x = NULL;
    }
    VariantClear(&n);
    VariantClear(&i);
    if (!x) {   /* by position: rcache.c fetches it with the elements after it */
        long h;
        if (rc_col_item(c->page, c->handle, (long)idx, &h)) {
            if (h >= 0 && !(*pdisp = (IDispatch *)elem_new(c->page, h)))
                return E_OUTOFMEMORY;
            return S_OK;
        }
        x = sfmt(EL "[%ld]", c->handle, (long)idx);
    }
    hr = stash_elem_pure(c->page, x, (IHTMLElement **)pdisp);
    return hr;
}

/* get__newEnum: IEnumVARIANT for `foreach (HtmlElement e in collection)`. */
typedef struct EnumVARIANT {
    IEnumVARIANT IEnumVARIANT_iface;
    LONG ref;
    Page *page;
    long col;
    long pos;
    long length;
} EnumVARIANT;

#define ENUMV(THIS) CONTAINING_RECORD(THIS, EnumVARIANT, IEnumVARIANT_iface)

static HRESULT STDMETHODCALLTYPE enum_qi(IEnumVARIANT *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumVARIANT)) {
        *ppv = This;
        InterlockedIncrement(&ENUMV(This)->ref);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE enum_addref(IEnumVARIANT *This)
{
    return InterlockedIncrement(&ENUMV(This)->ref);
}

static ULONG STDMETHODCALLTYPE enum_release(IEnumVARIANT *This)
{
    EnumVARIANT *e = ENUMV(This);
    LONG ref = InterlockedDecrement(&e->ref);
    if (ref == 0) {
        page_release(e->page);
        free(e);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE enum_next(IEnumVARIANT *This, ULONG celt, VARIANT *rgVar, ULONG *pCeltFetched)
{
    EnumVARIANT *e = ENUMV(This);
    ULONG fetched = 0;

    while (fetched < celt && e->pos < e->length) {
        IHTMLElement *el = NULL;
        long h;
        if (rc_col_item(e->page, e->col, e->pos, &h))   /* rcache.c */
            el = h >= 0 ? elem_new(e->page, h) : NULL;
        else
            stash_elem_pure(e->page, sfmt(EL "[%ld]", e->col, e->pos), &el);
        e->pos++;
        if (!el)
            continue;
        VariantInit(&rgVar[fetched]);
        V_VT(&rgVar[fetched]) = VT_DISPATCH;
        V_DISPATCH(&rgVar[fetched]) = (IDispatch *)el;
        fetched++;
    }
    if (pCeltFetched)
        *pCeltFetched = fetched;
    return fetched == celt ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE enum_skip(IEnumVARIANT *This, ULONG celt)
{
    EnumVARIANT *e = ENUMV(This);
    e->pos += (long)celt;
    if (e->pos > e->length) {
        e->pos = e->length;
        return S_FALSE;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enum_reset(IEnumVARIANT *This)
{
    ENUMV(This)->pos = 0;
    return S_OK;
}

static IEnumVARIANTVtbl enum_variant_vtbl;

static EnumVARIANT *enum_new(Page *pg, long col, long pos, long length)
{
    EnumVARIANT *e = (EnumVARIANT *)calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->IEnumVARIANT_iface.lpVtbl = &enum_variant_vtbl;
    e->ref = 1;
    e->page = pg;
    page_addref(pg);
    e->col = col;
    e->pos = pos;
    e->length = length;
    return e;
}

static HRESULT STDMETHODCALLTYPE enum_clone(IEnumVARIANT *This, IEnumVARIANT **ppEnum)
{
    EnumVARIANT *e = ENUMV(This), *c;
    if (!ppEnum)
        return E_POINTER;
    c = enum_new(e->page, e->col, e->pos, e->length);
    *ppEnum = c ? &c->IEnumVARIANT_iface : NULL;
    return c ? S_OK : E_OUTOFMEMORY;
}

static IEnumVARIANTVtbl enum_variant_vtbl = {
    enum_qi, enum_addref, enum_release, enum_next, enum_skip, enum_reset, enum_clone,
};

HRESULT STDMETHODCALLTYPE html_col__newEnum(IHTMLElementCollection *This, IUnknown **p)
{
    HtmlElementCollection *c = COL(This);
    char *x = sfmt(EL ".length", c->handle);
    double len = 0;
    EnumVARIANT *e;

    long cl;

    if (!p)
        return E_POINTER;
    if (rc_col_length(c->page, c->handle, &cl)) {
        len = (double)cl;
    } else if (x) {
        rc_pure_enter();
        dom_eval_num(c->page, x, &len);
        rc_pure_leave();
    }
    free(x);
    e = enum_new(c->page, c->handle, 0, (long)len);
    *p = e ? (IUnknown *)&e->IEnumVARIANT_iface : NULL;
    return e ? S_OK : E_OUTOFMEMORY;
}

/* ------------------------------------------------------------------ HtmlStyle */

HRESULT STDMETHODCALLTYPE html_elem_get_style(IHTMLElement *This, IHTMLStyle **p)
{
    HtmlElement *e = ELEM_OF(IHTMLElement, This);
    HtmlStyle *s;

    if (!p)
        return E_POINTER;
    s = (HtmlStyle *)calloc(1, sizeof(*s));
    *p = NULL;
    if (!s)
        return E_OUTOFMEMORY;
    s->IHTMLStyle_iface.lpVtbl = &g_IHTMLStyle_vtbl;
    s->ref = 1;
    s->handle = e->handle;
    s->page = e->page;
    page_addref(e->page);
    *p = &s->IHTMLStyle_iface;
    return S_OK;
}

ULONG html_style_addref(HtmlStyle *s)
{
    return InterlockedIncrement(&s->ref);
}

ULONG html_style_release(HtmlStyle *s)
{
    LONG ref = InterlockedDecrement(&s->ref);
    if (ref == 0) {
        page_release(s->page);
        free(s);
    }
    return ref;
}

HRESULT html_style_qi(HtmlStyle *s, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IHTMLStyle)) {
        *ppv = &s->IHTMLStyle_iface;
        html_style_addref(s);
        return S_OK;
    }
    *ppv = NULL;
    shim_qi_miss("style", riid);
    return E_NOINTERFACE;
}

/* ------------------------------------------------------------------ HtmlTxtRange */

static HRESULT txr_new(HtmlElement *e, IHTMLTxtRange **range)
{
    HtmlTxtRange *r;
    if (!range)
        return E_POINTER;
    r = (HtmlTxtRange *)calloc(1, sizeof(*r));
    *range = NULL;
    if (!r)
        return E_OUTOFMEMORY;
    r->IHTMLTxtRange_iface.lpVtbl = &g_IHTMLTxtRange_vtbl;
    r->ref = 1;
    r->handle = e->handle;
    r->page = e->page;
    r->collapse = -1;
    page_addref(e->page);
    *range = &r->IHTMLTxtRange_iface;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_inputtext_createTextRange(IHTMLInputTextElement *This, IHTMLTxtRange **range)
{
    return txr_new(ELEM_OF(IHTMLInputTextElement, This), range);
}

HRESULT STDMETHODCALLTYPE html_textarea_createTextRange(IHTMLTextAreaElement *This, IHTMLTxtRange **range)
{
    return txr_new(ELEM_OF(IHTMLTextAreaElement, This), range);
}

ULONG html_txr_addref(HtmlTxtRange *r)
{
    return InterlockedIncrement(&r->ref);
}

ULONG html_txr_release(HtmlTxtRange *r)
{
    LONG ref = InterlockedDecrement(&r->ref);
    if (ref == 0) {
        page_release(r->page);
        free(r);
    }
    return ref;
}

HRESULT html_txr_qi(HtmlTxtRange *r, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &IID_IHTMLTxtRange)) {
        *ppv = &r->IHTMLTxtRange_iface;
        html_txr_addref(r);
        return S_OK;
    }
    *ppv = NULL;
    shim_qi_miss("textrange", riid);
    return E_NOINTERFACE;
}

#define TXR(THIS) CONTAINING_RECORD(THIS, HtmlTxtRange, IHTMLTxtRange_iface)

HRESULT STDMETHODCALLTYPE html_txr_collapse(IHTMLTxtRange *This, VARIANT_BOOL Start)
{
    TXR(This)->collapse = Start ? 0 : 1;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_txr_select(IHTMLTxtRange *This)
{
    HtmlTxtRange *r = TXR(This);
    char *x = sfmt("(function(e,c){try{e.focus();}catch(x){}if(typeof e.setSelectionRange!=='function')return;"
                   "var n=String(e.value||'').length;if(c===1)e.setSelectionRange(n,n);"
                   "else if(c===0)e.setSelectionRange(0,0);else e.select();})(" EL ",%d)",
                   r->handle, r->collapse);
    if (x)
        dom_exec(r->page, x);
    free(x);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_txr_get_text(IHTMLTxtRange *This, BSTR *p)
{
    HtmlTxtRange *r = TXR(This);
    char *x, *s;
    if (!p)
        return E_POINTER;
    x = sfmt("(function(e){return e.value!==undefined?String(e.value):e.innerText;})(" EL ")", r->handle);
    s = x ? dom_eval_str(r->page, x) : NULL;
    *p = r->collapse >= 0 ? SysAllocString(L"") : utf8_to_bstr(s ? s : "");
    free(s);
    free(x);
    return S_OK;
}

/* ------------------------------------------------------------------ events */

typedef struct EvtEntry {
    Page *page;
    long handle;        /* element id, DOM_DOC_HANDLE or DOM_WIN_HANDLE */
    char *type;         /* "click", "keyup", ... */
    IDispatch *sink;    /* invoked with DISPID_VALUE */
} EvtEntry;

static EvtEntry *g_evts;
static int g_evt_n, g_evt_cap;

/* "onclick" -> "click" (malloc'd) */
static char *event_type(BSTR name)
{
    char *u = bstr_to_utf8(name), *t;
    if (!u)
        return NULL;
    t = (_strnicmp(u, "on", 2) == 0 && u[2]) ? dup_utf8(u + 2) : dup_utf8(u);
    free(u);
    return t;
}

HRESULT dom_attach_event(Page *pg, long id, BSTR name, IDispatch *sink, VARIANT_BOOL *res)
{
    char *type = event_type(name), *target, *tl, *x;

    if (res)
        *res = VARIANT_FALSE;
    if (!type || !sink) {
        free(type);
        return E_INVALIDARG;
    }
    if (!pg) {
        free(type);
        return S_OK;
    }
    if (g_evt_n == g_evt_cap) {
        int ncap = g_evt_cap ? g_evt_cap * 2 : 32;
        EvtEntry *n = (EvtEntry *)realloc(g_evts, (size_t)ncap * sizeof(*n));
        if (!n) {
            free(type);
            return E_OUTOFMEMORY;
        }
        g_evts = n;
        g_evt_cap = ncap;
    }
    target = id == DOM_DOC_HANDLE ? dup_utf8("document") : id == DOM_WIN_HANDLE ? dup_utf8("window") : dom_ref(id);
    tl = js_lit(type);
    x = target && tl ? sfmt("(%s,%s(%s,%ld,%s,%ld))", STASH_DEF, LISTEN_FN, target, id, tl, page_stash_base(pg)) : NULL;
    if (x)
        dom_exec(pg, x);
    free(x);
    free(tl);
    free(target);

    g_evts[g_evt_n].page = pg;
    g_evts[g_evt_n].handle = id;
    g_evts[g_evt_n].type = type;
    g_evts[g_evt_n].sink = sink;
    IDispatch_AddRef(sink);
    page_addref(pg);
    g_evt_n++;
    shim_log("EVENT attach %s on %ld", type, id);
    if (res)
        *res = VARIANT_TRUE;
    return S_OK;
}

static void evt_drop(int i)
{
    IDispatch_Release(g_evts[i].sink);
    page_release(g_evts[i].page);
    free(g_evts[i].type);
    g_evts[i] = g_evts[--g_evt_n];
}

HRESULT dom_detach_event(Page *pg, long id, BSTR name, IDispatch *sink)
{
    char *type = event_type(name);
    int i;

    /* the page listener stays; with no sink left it reports to nobody */
    for (i = 0; type && i < g_evt_n; i++)
        if (g_evts[i].page == pg && g_evts[i].handle == id && g_evts[i].sink == sink &&
            !strcmp(g_evts[i].type, type)) {
            evt_drop(i);
            break;
        }
    free(type);
    return S_OK;
}

void dom_events_reset(Page *pg)
{
    int i;
    for (i = g_evt_n - 1; i >= 0; i--)
        if (g_evts[i].page == pg)
            evt_drop(i);
}

/* ------------------------------------------------------------------ window.event */

ULONG html_evo_addref(HtmlEventObj *e)
{
    return InterlockedIncrement(&e->ref);
}

ULONG html_evo_release(HtmlEventObj *e)
{
    LONG ref = InterlockedDecrement(&e->ref);
    if (ref == 0) {
        VariantClear(&e->return_value);
        page_release(e->page);
        free(e);
    }
    return ref;
}

HRESULT html_evo_qi(HtmlEventObj *e, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IHTMLEventObj)) {
        *ppv = &e->IHTMLEventObj_iface;
        html_evo_addref(e);
        return S_OK;
    }
    *ppv = NULL;
    shim_qi_miss("event", riid);
    return E_NOINTERFACE;
}

#define EVO(THIS) CONTAINING_RECORD(THIS, HtmlEventObj, IHTMLEventObj_iface)

HRESULT STDMETHODCALLTYPE html_evo_get_srcElement(IHTMLEventObj *This, IHTMLElement **p)
{
    HtmlEventObj *e = EVO(This);
    if (!p)
        return E_POINTER;
    *p = e->src_handle >= 0 ? elem_new(e->page, e->src_handle) : NULL;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_altKey(IHTMLEventObj *This, VARIANT_BOOL *p)
{
    *p = EVO(This)->alt_key ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_ctrlKey(IHTMLEventObj *This, VARIANT_BOOL *p)
{
    *p = EVO(This)->ctrl_key ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_shiftKey(IHTMLEventObj *This, VARIANT_BOOL *p)
{
    *p = EVO(This)->shift_key ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_put_returnValue(IHTMLEventObj *This, VARIANT v)
{
    HtmlEventObj *e = EVO(This);
    VariantClear(&e->return_value);
    VariantCopy(&e->return_value, &v);
    /* the DOM event has already run its default action by the time the app sees it */
    shim_log("EVENT %s returnValue set (too late to cancel the default action)", e->type);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_returnValue(IHTMLEventObj *This, VARIANT *p)
{
    VariantInit(p);
    return VariantCopy(p, &EVO(This)->return_value);
}

HRESULT STDMETHODCALLTYPE html_evo_put_cancelBubble(IHTMLEventObj *This, VARIANT_BOOL v)
{
    EVO(This)->cancel_bubble = v;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_cancelBubble(IHTMLEventObj *This, VARIANT_BOOL *p)
{
    *p = EVO(This)->cancel_bubble;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_put_keyCode(IHTMLEventObj *This, LONG v)
{
    EVO(This)->key_code = v;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_keyCode(IHTMLEventObj *This, LONG *p)
{
    *p = EVO(This)->key_code;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_button(IHTMLEventObj *This, LONG *p)
{
    *p = EVO(This)->button;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_type(IHTMLEventObj *This, BSTR *p)
{
    *p = utf8_to_bstr(EVO(This)->type);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_clientX(IHTMLEventObj *This, LONG *p)
{
    *p = EVO(This)->client_x;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_evo_get_clientY(IHTMLEventObj *This, LONG *p)
{
    *p = EVO(This)->client_y;
    return S_OK;
}

static HtmlEventObj *evo_from_payload(Page *pg, JVal *v)
{
    HtmlEventObj *e = (HtmlEventObj *)calloc(1, sizeof(*e));
    const char *t = j_str(j_get(v, "t"));
    long b = (long)j_num(j_get(v, "b"));

    if (!e)
        return NULL;
    e->IHTMLEventObj_iface.lpVtbl = &g_IHTMLEventObj_vtbl;
    e->ref = 1;
    e->page = pg;
    page_addref(pg);
    snprintf(e->type, sizeof(e->type), "%s", t ? t : "");
    e->src_handle = j_get(v, "s") ? (long)j_num(j_get(v, "s")) : -1;
    e->key_code = (long)j_num(j_get(v, "k"));
    /* DOM buttons 0/1/2 (left/middle/right) are IE's 1/4/2, on mouse buttons only */
    e->button = strncmp(e->type, "mouse", 5) ? 0 : b == 0 ? 1 : b == 1 ? 4 : 2;
    e->client_x = (long)j_num(j_get(v, "x"));
    e->client_y = (long)j_num(j_get(v, "y"));
    e->alt_key = j_is_true(j_get(v, "a"));
    e->ctrl_key = j_is_true(j_get(v, "c"));
    e->shift_key = j_is_true(j_get(v, "f"));
    VariantInit(&e->return_value);
    return e;
}

void shim_dom_event_dispatch(Page *pg, const char *payload_json)
{
    size_t off = 0;
    JVal *v = payload_json ? j_parse(payload_json, &off) : NULL;
    const char *type = j_str(j_get(v, "t"));
    long handle = (long)j_num(j_get(v, "h"));
    IDispatch **sinks = NULL;
    HtmlEventObj *evo, *prev;
    int i, n = 0;

    rc_bump();  /* the page has moved on since the last request */
    if (!type) {
        j_free(v);
        return;
    }
    if (j_get(v, "s"))
        page_stash_used(pg, (long)j_num(j_get(v, "s")));
    /* copy the matching sinks first: a handler may attach or detach */
    for (i = 0; i < g_evt_n; i++)
        if (g_evts[i].page == pg && g_evts[i].handle == handle && !strcmp(g_evts[i].type, type))
            n++;
    if (n && (sinks = (IDispatch **)malloc((size_t)n * sizeof(*sinks)))) {
        n = 0;
        for (i = 0; i < g_evt_n; i++)
            if (g_evts[i].page == pg && g_evts[i].handle == handle && !strcmp(g_evts[i].type, type)) {
                sinks[n] = g_evts[i].sink;
                IDispatch_AddRef(sinks[n]);
                n++;
            }
    } else {
        n = 0;
    }
    if (n)
        shim_log("EVENT %s on %ld: %d sink(s)", type, handle, n);
    evo = n ? evo_from_payload(pg, v) : NULL;
    prev = page_current_event(pg);
    if (evo)
        page_set_current_event(pg, evo);
    for (i = 0; i < n; i++) {
        /* IE passes the event object as the one argument (HtmlToClrEventProxy ignores it) */
        DISPPARAMS dp;
        VARIANT arg, result;
        memset(&dp, 0, sizeof(dp));
        VariantInit(&arg);
        VariantInit(&result);
        if (evo) {
            V_VT(&arg) = VT_DISPATCH;
            V_DISPATCH(&arg) = (IDispatch *)&evo->IHTMLEventObj_iface;
            dp.rgvarg = &arg;
            dp.cArgs = 1;
        }
        IDispatch_Invoke(sinks[i], DISPID_VALUE, &IID_NULL, 0, DISPATCH_METHOD, &dp, &result, NULL, NULL);
        VariantClear(&result);
        IDispatch_Release(sinks[i]);
    }
    if (evo) {
        page_set_current_event(pg, prev);
        html_evo_release(evo);
    }
    free(sinks);
    if (j_num(j_get(v, "q")) > 0) {   /* release the click iecompat.js held for these */
        char *x = sfmt("window.__shim_ack&&window.__shim_ack(%ld)", (long)j_num(j_get(v, "q")));
        if (x)
            dom_exec(pg, x);
        free(x);
    }
    j_free(v);
}

/* ------------------------------------------------------------------ IHTMLElementRender */

#define RENDER(THIS) CONTAINING_RECORD(THIS, HtmlElement, IHTMLElementRender_iface)

static HRESULT STDMETHODCALLTYPE render_qi(IHTMLElementRender *This, REFIID riid, void **ppv)
{
    return html_elem_qi(RENDER(This), riid, ppv);
}

static ULONG STDMETHODCALLTYPE render_addref(IHTMLElementRender *This)
{
    return html_elem_addref(RENDER(This));
}

static ULONG STDMETHODCALLTYPE render_release(IHTMLElementRender *This)
{
    return html_elem_release(RENDER(This));
}

/* DrawToDC: the element's current pixels into hdc at (0,0). A loaded same-origin <img> (the
 * captcha on the Medula login) is drawn through a canvas, which gives the image's own pixels as
 * IE does. A Page.captureScreenshot clip starts at the layout position instead -- x 12.5 on the
 * centred login page -- so Chromium resampled every glyph edge and the vendor's OCR dropped
 * digits. Anything else is a fresh Page.captureScreenshot clipped to the element, at whole
 * pixels. */
static HRESULT STDMETHODCALLTYPE render_DrawToDC(IHTMLElementRender *This, HDC hdc)
{
    HtmlElement *e = RENDER(This);
    char *x, *r, *params;
    double bx = 0, by = 0, bw = 0, bh = 0;
    JVal *reply = NULL, *v;
    const char *b64 = NULL;
    size_t off = 0;

    if (!hdc)
        return E_INVALIDARG;
    x = sfmt("JSON.stringify((function(e){var r=e.getBoundingClientRect(),w=Math.round(r.width),"
             "h=Math.round(r.height),o={x:Math.round(r.left+window.scrollX),y:Math.round(r.top+window.scrollY),"
             "w:w,h:h};if(e.tagName==='IMG'&&e.complete&&e.naturalWidth&&w>0&&h>0){try{"
             "var c=document.createElement('canvas');c.width=w;c.height=h;c.getContext('2d').drawImage(e,0,0,w,h);"
             "o.png=c.toDataURL('image/png').split(',')[1];}catch(x){}}return o;})(" EL "))", e->handle);
    r = x ? dom_eval_str(e->page, x) : NULL;
    free(x);
    v = r ? j_parse(r, &off) : NULL;
    free(r);
    if (v) {
        bx = j_num(j_get(v, "x"));
        by = j_num(j_get(v, "y"));
        bw = j_num(j_get(v, "w"));
        bh = j_num(j_get(v, "h"));
        b64 = j_str(j_get(v, "png"));
    }
    if (bw < 1 || bh < 1) {
        j_free(v);
        return E_FAIL;
    }
    if (!b64 || !*b64) {
        params = sfmt("{\"format\":\"png\",\"captureBeyondViewport\":true,"
                      "\"clip\":{\"x\":%.0f,\"y\":%.0f,\"width\":%.0f,\"height\":%.0f,\"scale\":1}}", bx, by, bw, bh);
        if (!params || page_call(e->page, "Page.captureScreenshot", params, &reply) != 0) {
            free(params);
            j_free(reply);
            j_free(v);
            return E_FAIL;
        }
        free(params);
        b64 = j_str(j_get(j_get(reply, "result"), "data"));
    }
    if (b64 && *b64) {
        size_t cap = strlen(b64) / 4 * 3 + 16;
        unsigned char *png = (unsigned char *)malloc(cap);
        int pnglen = png ? b64_decode(b64, png, cap) : -1;
        int pw = 0, ph = 0, comp = 0;
        unsigned char *rgba = pnglen > 0 ? stbi_load_from_memory(png, pnglen, &pw, &ph, &comp, 4) : NULL;
        if (rgba && pw > 0 && ph > 0) {
            int i, n = pw * ph;
            BITMAPINFO bi;
            for (i = 0; i < n; i++) {   /* RGBA -> BGRA in place */
                unsigned char t = rgba[i * 4];
                rgba[i * 4] = rgba[i * 4 + 2];
                rgba[i * 4 + 2] = t;
            }
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = pw;
            bi.bmiHeader.biHeight = -ph;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, (int)bw, (int)bh, 0, 0, pw, ph, rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        if (rgba)
            stbi_image_free(rgba);
        free(png);
    }
    j_free(reply);
    j_free(v);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE render_SetDocumentPrinter(IHTMLElementRender *This, BSTR printer, HDC hdc)
{
    return E_NOTIMPL;
}

static IHTMLElementRenderVtbl g_IHTMLElementRender_vtbl = {
    render_qi, render_addref, render_release, render_DrawToDC, render_SetDocumentPrinter,
};

/* ------------------------------------------------------------------ test-harness exports */
/*
 * The harnesses drive one tab of a Chromium they started themselves (the "legacy" page);
 * a document that was never given its own page (no IPersistMoniker::Load through page.c)
 * uses it.
 */

int shim_dom_connect(int port)
{
    return page_legacy_connect_dom(port);
}

int shim_render_connect(int port)
{
    return page_legacy_connect_evt(port);
}

struct CdpConn *shim_dom_cdp(void)
{
    return page_dom(page_legacy());
}

struct CdpConn *shim_render_cdp(void)
{
    return page_evt(page_legacy());
}

int shim_dom_navigate(const char *url)
{
    Page *pg = page_legacy();
    JVal *reply = NULL;
    char *q = j_quote(url), *p = q ? sfmt("{\"url\":%s}", q) : NULL;
    int rc = p ? page_call(pg, "Page.navigate", p, &reply) : -1;

    j_free(reply);
    free(p);
    free(q);
    if (rc != 0)
        return -1;
    Sleep(1200);
    return 0;
}

void shim_dom_disconnect(void)
{
    Page *pg = page_legacy();
    if (pg)
        dom_events_reset(pg);
    page_legacy_disconnect();
}
