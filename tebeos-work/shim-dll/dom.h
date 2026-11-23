/* dom.h -- the page's DOM as seen from the shim's COM objects.
 *
 * Everything is a Runtime.evaluate on the page's dom connection (UI thread only). A DOM
 * object handed to the app is stashed in the page as `window.__shim_els[id]` and wrapped
 * in an HtmlElement / HtmlElementCollection that remembers (page, id). Ids are unique per
 * page across navigations (each document draws them from a fresh block), so an element
 * kept from a previous document finds nothing instead of some other element of the new one.
 */

#ifndef SHIM_DOM_H
#define SHIM_DOM_H

#include "html_doc.h"
#include "page.h"

/* Evaluate `expr`. *reply is the whole CDP reply (j_free it); returns the RemoteObject
 * (reply.result.result) or NULL on transport failure or a thrown exception (logged). */
JVal *dom_eval(Page *pg, const char *expr, JVal **reply);

char *dom_eval_str(Page *pg, const char *expr);              /* malloc'd; NULL for null/undefined */
int dom_eval_num(Page *pg, const char *expr, double *out);   /* 0 if a number came back */
int dom_eval_bool(Page *pg, const char *expr, int *out);     /* 0 if a value came back */
void dom_log_click(Page *pg, const char *target, const char *what);  /* a "CLICK ..." log line */
void dom_exec(Page *pg, const char *expr);                   /* run for its side effects */

/* window.__shim_stash(x, B): x's stash id (created on first use; ids start at B), -1 for
 * null/undefined/a primitive. Prefix an expression with it: "(" STASH_DEF ",...)". */
#define STASH_DEF \
    "(window.__shim_stash||(window.__shim_stash=function(x,B){" \
    "if(x===null||x===undefined)return -1;var t=typeof x;" \
    "if(t==='string'||t==='number'||t==='boolean'||t==='symbol'||t==='bigint')return -1;" \
    "var w=window;if(!w.__shim_els){w.__shim_els={};w.__shim_ids=new Map();w.__shim_n=B;}" \
    "var i=w.__shim_ids.get(x);if(i===undefined){i=w.__shim_n++;w.__shim_ids.set(x,i);w.__shim_els[i]=x;}" \
    "return i;}))"

/* Stash the object `expr` evaluates to; its id, or -1 for null/undefined/a primitive. */
long dom_stash(Page *pg, const char *expr);

/* JS expression for a stashed object. */
char *dom_ref(long id);   /* "window.__shim_els[id]" (malloc'd) */

/* Evaluate `expr` into a VARIANT the way IE would hand it back: strings VT_BSTR, integers
 * VT_I4, other numbers VT_R8, booleans VT_BOOL, null VT_NULL, undefined VT_EMPTY, objects
 * VT_DISPATCH (an HtmlElement over the stashed object). */
HRESULT dom_eval_variant(Page *pg, const char *expr, VARIANT *out);

/* JS expression for a VARIANT argument (malloc'd). Our own elements become their stash
 * reference; foreign objects become null (logged). */
char *dom_variant_js(const VARIANT *v);

/* Wrappers. */
IHTMLElement *elem_new(Page *pg, long id);
IHTMLElementCollection *col_new(Page *pg, long id);
HtmlElement *elem_unwrap(IUnknown *unk);   /* our element behind any of its interfaces, or NULL; addref'd */

/* IDispatch for any page object: names get process-wide DISPIDs (case-insensitive, 1..),
 * and Invoke calls/reads/writes the same-named JS member of `target` (resolved
 * case-insensitively, as IE does). DISPID_VALUE calls or reads `target` itself. */
HRESULT dom_disp_ids(LPOLESTR *names, UINT n, DISPID *ids);
HRESULT dom_disp_invoke(Page *pg, const char *target, DISPID id, WORD flags, DISPPARAMS *dp, VARIANT *res);

/* attachEvent/detachEvent for an element (id >= 0), the document or the window. */
#define DOM_DOC_HANDLE (-1L)
#define DOM_WIN_HANDLE (-2L)
HRESULT dom_attach_event(Page *pg, long id, BSTR name, IDispatch *sink, VARIANT_BOOL *res);
HRESULT dom_detach_event(Page *pg, long id, BSTR name, IDispatch *sink);
/* The page's document went away: drop its sinks (UI thread). */
void dom_events_reset(Page *pg);

#endif /* SHIM_DOM_H */
