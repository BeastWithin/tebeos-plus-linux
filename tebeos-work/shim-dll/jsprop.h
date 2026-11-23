/* jsprop.h -- the AUTO accessors gen-stubs.py wires into the vtables.
 *
 * shim_js_get_<type>(page, target, prop, out) reads `(target)[prop]` from the page and
 * shim_js_put_<type>(page, target, prop, value) writes it; shim_js_call0 calls a method with
 * no arguments. `target` is a JS expression (document, window, location,
 * window.__shim_els[id], ...). Without a page, reads give empty values and writes do nothing,
 * as for a document that has not loaded yet; every call returns S_OK so a missing member
 * never becomes an exception in the app (the page-side error is logged).
 */

#ifndef SHIM_JSPROP_H
#define SHIM_JSPROP_H

#include "html_doc.h"
#include "page.h"

HRESULT shim_js_get_bstr(Page *pg, const char *t, const char *prop, BSTR *p);
HRESULT shim_js_get_long(Page *pg, const char *t, const char *prop, LONG *p);
HRESULT shim_js_get_short(Page *pg, const char *t, const char *prop, short *p);
HRESULT shim_js_get_float(Page *pg, const char *t, const char *prop, float *p);
HRESULT shim_js_get_bool(Page *pg, const char *t, const char *prop, VARIANT_BOOL *p);
HRESULT shim_js_get_variant(Page *pg, const char *t, const char *prop, VARIANT *p);
HRESULT shim_js_get_elem(Page *pg, const char *t, const char *prop, IHTMLElement **p);
HRESULT shim_js_get_col(Page *pg, const char *t, const char *prop, IHTMLElementCollection **p);
HRESULT shim_js_get_node(Page *pg, const char *t, const char *prop, IHTMLDOMNode **p);
HRESULT shim_js_get_disp(Page *pg, const char *t, const char *prop, IDispatch **p);

HRESULT shim_js_put_bstr(Page *pg, const char *t, const char *prop, BSTR v);
HRESULT shim_js_put_long(Page *pg, const char *t, const char *prop, LONG v);
HRESULT shim_js_put_short(Page *pg, const char *t, const char *prop, short v);
HRESULT shim_js_put_float(Page *pg, const char *t, const char *prop, float v);
HRESULT shim_js_put_bool(Page *pg, const char *t, const char *prop, VARIANT_BOOL v);
HRESULT shim_js_put_variant(Page *pg, const char *t, const char *prop, VARIANT v);

HRESULT shim_js_call0(Page *pg, const char *t, const char *method);

#endif /* SHIM_JSPROP_H */
