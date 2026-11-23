/* window.c -- the document's window and location, ieframe's navigation seam, and the
 * document's own IDispatch / ready-state notification.
 *
 * Mirrors Wine's mshtml (htmlwindow.c, oleobj.c, persist.c) where ieframe depends on it:
 *   - ieframe reads the document's ready state with IDispatch::Invoke(DISPID_READYSTATE)
 *     and listens for changes through IConnectionPointContainer/IPropertyNotifySink;
 *   - ieframe sends WebBrowser.Navigate to IHTMLPrivateWindow::SuperNavigate on the window
 *     the document gave it (docobj.c SetClientSite), and reads the address bar URL back with
 *     GetAddressBarUrl when the document reports NavigateComplete2/DocumentComplete.
 */

#include "html_doc.h"
#include "stubs.h"
#include "dom.h"
#include "page.h"
#include "jsprop.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOC2(THIS) CONTAINING_RECORD(THIS, HTMLDocument, IHTMLDocument2_iface)
#define WIN2(THIS) CONTAINING_RECORD(THIS, HtmlWindow, IHTMLWindow2_iface)
#define WIN3(THIS) CONTAINING_RECORD(THIS, HtmlWindow, IHTMLWindow3_iface)
#define PRIV(THIS) CONTAINING_RECORD(THIS, HtmlWindow, IHTMLPrivateWindow_iface)
#define LOC(THIS)  CONTAINING_RECORD(THIS, HtmlLocation, IHTMLLocation_iface)

/* ------------------------------------------------------------------ document state */

static LONG ready_state_value(BSTR s)
{
    if (!s)
        return READYSTATE_UNINITIALIZED;
    if (!wcscmp(s, L"loading"))
        return READYSTATE_LOADING;
    if (!wcscmp(s, L"loaded"))
        return READYSTATE_LOADED;
    if (!wcscmp(s, L"interactive"))
        return READYSTATE_INTERACTIVE;
    if (!wcscmp(s, L"complete"))
        return READYSTATE_COMPLETE;
    return READYSTATE_UNINITIALIZED;
}

void html_doc_set_ready_state(HTMLDocument *d, LPCWSTR state)
{
    IPropertyNotifySink *sinks[DOC_MAX_PROP_SINKS];
    int i, n = 0;

    if (d->ready_state && !wcscmp(d->ready_state, state))
        return;
    SysFreeString(d->ready_state);
    d->ready_state = SysAllocString(state);
    /* a sink may unadvise from inside OnChanged */
    for (i = 0; i < DOC_MAX_PROP_SINKS; i++)
        if (d->prop_sinks[i]) {
            sinks[n] = d->prop_sinks[i];
            IPropertyNotifySink_AddRef(sinks[n]);
            n++;
        }
    for (i = 0; i < n; i++) {
        IPropertyNotifySink_OnChanged(sinks[i], DISPID_READYSTATE);
        IPropertyNotifySink_Release(sinks[i]);
    }
}

void html_doc_set_url(HTMLDocument *d, LPCWSTR url)
{
    SysFreeString(d->url);
    d->url = url ? SysAllocString(url) : NULL;
}

/* ------------------------------------------------------------------ document IDispatch */

HRESULT STDMETHODCALLTYPE html_doc_GetTypeInfoCount(IHTMLDocument2 *This, UINT *pctinfo)
{
    if (!pctinfo)
        return E_INVALIDARG;
    *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_GetTypeInfo(IHTMLDocument2 *This, UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
{
    if (ppTInfo)
        *ppTInfo = NULL;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE html_doc_GetIDsOfNames(IHTMLDocument2 *This, REFIID riid, LPOLESTR *rgszNames,
                                                UINT cNames, LCID lcid, DISPID *rgDispId)
{
    return dom_disp_ids(rgszNames, cNames, rgDispId);
}

HRESULT STDMETHODCALLTYPE html_doc_Invoke(IHTMLDocument2 *This, DISPID dispIdMember, REFIID riid, LCID lcid,
                                         WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                         EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLDocument *d = DOC2(This);

    if (dispIdMember == DISPID_READYSTATE) {
        /* ieframe's get_doc_ready_state: a VT_I4 READYSTATE_* */
        if (!(wFlags & DISPATCH_PROPERTYGET) || !pVarResult)
            return DISP_E_MEMBERNOTFOUND;
        V_VT(pVarResult) = VT_I4;
        V_I4(pVarResult) = ready_state_value(d->ready_state);
        return S_OK;
    }
    return dom_disp_invoke(doc_page(d), "document", dispIdMember, wFlags, pDispParams, pVarResult);
}

HRESULT STDMETHODCALLTYPE html_doc_get_parentWindow(IHTMLDocument2 *This, IHTMLWindow2 **p)
{
    HTMLDocument *d = DOC2(This);
    if (!p)
        return E_POINTER;
    *p = &d->window.IHTMLWindow2_iface;
    html_doc_addref(d);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_get_location(IHTMLDocument2 *This, IHTMLLocation **p)
{
    HTMLDocument *d = DOC2(This);
    if (!p)
        return E_POINTER;
    *p = &d->location.IHTMLLocation_iface;
    html_doc_addref(d);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_get_Script(IHTMLDocument2 *This, IDispatch **p)
{
    HTMLDocument *d = DOC2(This);
    if (!p)
        return E_POINTER;
    *p = (IDispatch *)&d->window.IHTMLWindow2_iface;
    html_doc_addref(d);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc4_focus(IHTMLDocument4 *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IHTMLDocument4_iface);
    dom_exec(doc_page(d), "window.focus()");
    return S_OK;
}

/* ------------------------------------------------------------------ IPropertyNotifySink point */

#define PNCP(THIS) CONTAINING_RECORD(THIS, HTMLDocument, PropNotifyCP_iface)

static HRESULT STDMETHODCALLTYPE pncp_QueryInterface(IConnectionPoint *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IConnectionPoint)) {
        *ppv = This;
        html_doc_addref(PNCP(This));
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE pncp_AddRef(IConnectionPoint *This)
{
    return html_doc_addref(PNCP(This));
}

static ULONG STDMETHODCALLTYPE pncp_Release(IConnectionPoint *This)
{
    return html_doc_release(PNCP(This));
}

static HRESULT STDMETHODCALLTYPE pncp_GetConnectionInterface(IConnectionPoint *This, IID *pIID)
{
    if (!pIID)
        return E_POINTER;
    *pIID = IID_IPropertyNotifySink;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pncp_GetConnectionPointContainer(IConnectionPoint *This,
                                                                 IConnectionPointContainer **ppCPC)
{
    HTMLDocument *d = PNCP(This);
    if (!ppCPC)
        return E_POINTER;
    *ppCPC = &d->IConnectionPointContainer_iface;
    html_doc_addref(d);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pncp_Advise(IConnectionPoint *This, IUnknown *pUnkSink, DWORD *pdwCookie)
{
    HTMLDocument *d = PNCP(This);
    IPropertyNotifySink *sink = NULL;
    int i;

    if (!pUnkSink || !pdwCookie)
        return E_POINTER;
    if (FAILED(IUnknown_QueryInterface(pUnkSink, &IID_IPropertyNotifySink, (void **)&sink)))
        return CONNECT_E_CANNOTCONNECT;
    for (i = 0; i < DOC_MAX_PROP_SINKS; i++)
        if (!d->prop_sinks[i]) {
            d->prop_sinks[i] = sink;
            *pdwCookie = (DWORD)i + 1;
            return S_OK;
        }
    IPropertyNotifySink_Release(sink);
    return CONNECT_E_ADVISELIMIT;
}

static HRESULT STDMETHODCALLTYPE pncp_Unadvise(IConnectionPoint *This, DWORD dwCookie)
{
    HTMLDocument *d = PNCP(This);
    IPropertyNotifySink *sink;

    if (dwCookie < 1 || dwCookie > DOC_MAX_PROP_SINKS || !d->prop_sinks[dwCookie - 1])
        return CONNECT_E_NOCONNECTION;
    sink = d->prop_sinks[dwCookie - 1];
    d->prop_sinks[dwCookie - 1] = NULL;
    IPropertyNotifySink_Release(sink);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pncp_EnumConnections(IConnectionPoint *This, IEnumConnections **ppEnum)
{
    if (ppEnum)
        *ppEnum = NULL;
    return E_NOTIMPL;
}

static IConnectionPointVtbl pncp_vtbl = {
    pncp_QueryInterface, pncp_AddRef, pncp_Release, pncp_GetConnectionInterface,
    pncp_GetConnectionPointContainer, pncp_Advise, pncp_Unadvise, pncp_EnumConnections,
};

HRESULT STDMETHODCALLTYPE html_doc_cpc_EnumConnectionPoints(IConnectionPointContainer *This,
                                                            IEnumConnectionPoints **ppEnum)
{
    if (ppEnum)
        *ppEnum = NULL;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE html_doc_cpc_FindConnectionPoint(IConnectionPointContainer *This, REFIID riid,
                                                           IConnectionPoint **ppCP)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IConnectionPointContainer_iface);

    if (!ppCP)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IPropertyNotifySink)) {
        *ppCP = &d->PropNotifyCP_iface;
        html_doc_addref(d);
        return S_OK;
    }
    *ppCP = NULL;
    shim_qi_miss("document connection point", riid);
    return CONNECT_E_NOCONNECTION;
}

/* ------------------------------------------------------------------ HtmlWindow */

HRESULT html_win_qi(HtmlWindow *w, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &IID_IHTMLFramesCollection2) || IsEqualIID(riid, &IID_IHTMLWindow2))
        *ppv = &w->IHTMLWindow2_iface;
    else if (IsEqualIID(riid, &IID_IHTMLWindow3))
        *ppv = &w->IHTMLWindow3_iface;
    else if (IsEqualIID(riid, &IID_IHTMLPrivateWindow))
        *ppv = &w->IHTMLPrivateWindow_iface;
    else {
        *ppv = NULL;
        shim_qi_miss("window", riid);
        return E_NOINTERFACE;
    }
    html_doc_addref(w->doc);
    return S_OK;
}

ULONG html_win_addref(HtmlWindow *w)
{
    return html_doc_addref(w->doc);
}

ULONG html_win_release(HtmlWindow *w)
{
    return html_doc_release(w->doc);
}

HRESULT STDMETHODCALLTYPE html_win_GetTypeInfoCount(IHTMLWindow2 *This, UINT *pctinfo)
{
    if (!pctinfo)
        return E_INVALIDARG;
    *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_GetTypeInfo(IHTMLWindow2 *This, UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
{
    if (ppTInfo)
        *ppTInfo = NULL;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE html_win_GetIDsOfNames(IHTMLWindow2 *This, REFIID riid, LPOLESTR *rgszNames,
                                                UINT cNames, LCID lcid, DISPID *rgDispId)
{
    return dom_disp_ids(rgszNames, cNames, rgDispId);
}

/* HtmlDocument.InvokeScript lands here (get_Script is the window). */
HRESULT STDMETHODCALLTYPE html_win_Invoke(IHTMLWindow2 *This, DISPID dispIdMember, REFIID riid, LCID lcid,
                                         WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                         EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    return dom_disp_invoke(doc_page(WIN2(This)->doc), "window", dispIdMember, wFlags, pDispParams, pVarResult);
}

HRESULT STDMETHODCALLTYPE html_win_get_document(IHTMLWindow2 *This, IHTMLDocument2 **p)
{
    HTMLDocument *d = WIN2(This)->doc;
    if (!p)
        return E_POINTER;
    *p = &d->IHTMLDocument2_iface;
    html_doc_addref(d);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_get_location(IHTMLWindow2 *This, IHTMLLocation **p)
{
    return html_doc_get_location(&WIN2(This)->doc->IHTMLDocument2_iface, p);
}

HRESULT STDMETHODCALLTYPE html_win_get_event(IHTMLWindow2 *This, IHTMLEventObj **p)
{
    HtmlEventObj *e = page_current_event(doc_page(WIN2(This)->doc));
    if (!p)
        return E_POINTER;
    *p = e ? &e->IHTMLEventObj_iface : NULL;
    if (e)
        html_evo_addref(e);
    return S_OK;
}

static HRESULT win_self(IHTMLWindow2 *This, IHTMLWindow2 **p)
{
    if (!p)
        return E_POINTER;
    *p = This;
    html_win_addref(WIN2(This));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_get_self(IHTMLWindow2 *This, IHTMLWindow2 **p)   { return win_self(This, p); }
HRESULT STDMETHODCALLTYPE html_win_get_window(IHTMLWindow2 *This, IHTMLWindow2 **p) { return win_self(This, p); }
HRESULT STDMETHODCALLTYPE html_win_get_top(IHTMLWindow2 *This, IHTMLWindow2 **p)    { return win_self(This, p); }
HRESULT STDMETHODCALLTYPE html_win_get_parent(IHTMLWindow2 *This, IHTMLWindow2 **p) { return win_self(This, p); }

/* window.navigate from the app: a page-initiated navigation, so BeforeNavigate2 is fired
 * for it (page.c), as IE does. */
HRESULT STDMETHODCALLTYPE html_win_navigate(IHTMLWindow2 *This, BSTR url)
{
    char *l = js_lit_bstr(url), *x = l ? sfmt("location.href=%s", l) : NULL;
    if (x)
        dom_exec(doc_page(WIN2(This)->doc), x);
    free(x);
    free(l);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_execScript(IHTMLWindow2 *This, BSTR scode, BSTR language, VARIANT *pvarRet)
{
    char *l = js_lit_bstr(scode), *x = l ? sfmt("(0,eval)(%s)", l) : NULL;
    if (pvarRet)
        VariantInit(pvarRet);
    if (x)
        dom_exec(doc_page(WIN2(This)->doc), x);
    free(x);
    free(l);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_alert(IHTMLWindow2 *This, BSTR message)
{
    MessageBoxW(NULL, message ? message : L"", L"Medula", MB_OK | MB_ICONINFORMATION | MB_TASKMODAL);
    return S_OK;
}

static HRESULT win_scroll(IHTMLWindow2 *This, const char *how, LONG x, LONG y)
{
    char *e = sfmt("window.%s(%ld,%ld)", how, (long)x, (long)y);
    if (e)
        dom_exec(doc_page(WIN2(This)->doc), e);
    free(e);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_win_scrollTo(IHTMLWindow2 *This, LONG x, LONG y) { return win_scroll(This, "scrollTo", x, y); }
HRESULT STDMETHODCALLTYPE html_win_scroll(IHTMLWindow2 *This, LONG x, LONG y)   { return win_scroll(This, "scroll", x, y); }
HRESULT STDMETHODCALLTYPE html_win_scrollBy(IHTMLWindow2 *This, LONG x, LONG y) { return win_scroll(This, "scrollBy", x, y); }

HRESULT STDMETHODCALLTYPE html_win3_attachEvent(IHTMLWindow3 *This, BSTR event, IDispatch *pDisp,
                                               VARIANT_BOOL *pfResult)
{
    return dom_attach_event(doc_page(WIN3(This)->doc), DOM_WIN_HANDLE, event, pDisp, pfResult);
}

HRESULT STDMETHODCALLTYPE html_win3_detachEvent(IHTMLWindow3 *This, BSTR event, IDispatch *pDisp)
{
    return dom_detach_event(doc_page(WIN3(This)->doc), DOM_WIN_HANDLE, event, pDisp);
}

/* ------------------------------------------------------------------ IHTMLPrivateWindow */

static HRESULT STDMETHODCALLTYPE priv_QueryInterface(IHTMLPrivateWindow *This, REFIID riid, void **ppv)
{
    return html_win_qi(PRIV(This), riid, ppv);
}

static ULONG STDMETHODCALLTYPE priv_AddRef(IHTMLPrivateWindow *This)
{
    return html_win_addref(PRIV(This));
}

static ULONG STDMETHODCALLTYPE priv_Release(IHTMLPrivateWindow *This)
{
    return html_win_release(PRIV(This));
}

/* ieframe's navigation of this document (WebBrowser.Navigate, with POST data and headers).
 * ieframe has already fired BeforeNavigate2 for it. */
static HRESULT STDMETHODCALLTYPE priv_SuperNavigate(IHTMLPrivateWindow *This, BSTR url, BSTR arg2, BSTR arg3,
                                                   BSTR arg4, VARIANT *post_data, VARIANT *headers, ULONG flags)
{
    HTMLDocument *d = PRIV(This)->doc;
    Page *pg = doc_page(d);
    char *u = bstr_to_utf8(url), *h = NULL;
    BYTE *post = NULL;
    DWORD postlen = 0;
    SAFEARRAY *sa = NULL;

    if (!u)
        return E_INVALIDARG;
    if (!pg && (d->page = page_create(d)))
        pg = d->page;
    if (post_data && V_VT(post_data) == (VT_ARRAY | VT_UI1))
        sa = V_ARRAY(post_data);
    else if (post_data && V_VT(post_data) == (VT_BYREF | VT_VARIANT) && V_VARIANTREF(post_data) &&
             V_VT(V_VARIANTREF(post_data)) == (VT_ARRAY | VT_UI1))
        sa = V_ARRAY(V_VARIANTREF(post_data));
    if (sa && SUCCEEDED(SafeArrayAccessData(sa, (void **)&post))) {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        postlen = hi >= lo ? (DWORD)(hi - lo + 1) : 0;
    }
    if (headers && V_VT(headers) == VT_BSTR)
        h = bstr_to_utf8(V_BSTR(headers));
    shim_log("SuperNavigate %s%s", postlen ? "POST " : "", u);
    if (pg)
        page_navigate(pg, u, post, postlen, h);
    else
        shim_log("SuperNavigate: no page for %s", u);
    if (post)
        SafeArrayUnaccessData(sa);
    free(h);
    free(u);
    return pg ? S_OK : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE priv_GetPendingUrl(IHTMLPrivateWindow *This, BSTR *url)
{
    if (url)
        *url = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE priv_SetPICSTarget(IHTMLPrivateWindow *This, IOleCommandTarget *cmdtrg)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE priv_PICSComplete(IHTMLPrivateWindow *This, int arg)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE priv_FindWindowByName(IHTMLPrivateWindow *This, LPCWSTR name, IHTMLWindow2 **ret)
{
    if (ret)
        *ret = NULL;
    shim_log("FindWindowByName not implemented");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE priv_GetAddressBarUrl(IHTMLPrivateWindow *This, BSTR *url)
{
    HTMLDocument *d = PRIV(This)->doc;
    if (!url)
        return E_POINTER;
    *url = SysAllocString(d->url ? d->url : L"about:blank");
    return *url ? S_OK : E_OUTOFMEMORY;
}

static IHTMLPrivateWindowVtbl priv_vtbl = {
    priv_QueryInterface, priv_AddRef, priv_Release, priv_SuperNavigate, priv_GetPendingUrl,
    priv_SetPICSTarget, priv_PICSComplete, priv_FindWindowByName, priv_GetAddressBarUrl,
};

/* ------------------------------------------------------------------ HtmlLocation */

HRESULT html_loc_qi(HtmlLocation *l, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IHTMLLocation)) {
        *ppv = &l->IHTMLLocation_iface;
        html_doc_addref(l->doc);
        return S_OK;
    }
    *ppv = NULL;
    shim_qi_miss("location", riid);
    return E_NOINTERFACE;
}

ULONG html_loc_addref(HtmlLocation *l)
{
    return html_doc_addref(l->doc);
}

ULONG html_loc_release(HtmlLocation *l)
{
    return html_doc_release(l->doc);
}

HRESULT STDMETHODCALLTYPE html_loc_reload(IHTMLLocation *This, VARIANT_BOOL flag)
{
    dom_exec(doc_page(LOC(This)->doc), "location.reload()");
    return S_OK;
}

static HRESULT loc_go(IHTMLLocation *This, const char *how, BSTR url)
{
    char *l = js_lit_bstr(url), *x = l ? sfmt("location.%s(%s)", how, l) : NULL;
    if (x)
        dom_exec(doc_page(LOC(This)->doc), x);
    free(x);
    free(l);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_loc_replace(IHTMLLocation *This, BSTR bstr) { return loc_go(This, "replace", bstr); }
HRESULT STDMETHODCALLTYPE html_loc_assign(IHTMLLocation *This, BSTR bstr)  { return loc_go(This, "assign", bstr); }

/* WinForms reads location.href on every WebBrowser.Document access, ReadyState included, and
 * the vendor's waitTillLoad() polls ReadyState up to 100000 times. Answer from the URL the
 * navigation events keep. A harness tab (no page of its own) gets no events, so it asks. */
HRESULT STDMETHODCALLTYPE html_loc_get_href(IHTMLLocation *This, BSTR *p)
{
    HTMLDocument *d = LOC(This)->doc;
    if (!p)
        return E_POINTER;
    if (!d->page || !d->url)
        return shim_js_get_bstr(doc_page(d), "location", "href", p);
    *p = SysAllocString(d->url);
    return *p ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE html_loc_toString(IHTMLLocation *This, BSTR *String)
{
    return html_loc_get_href(This, String);
}

/* ------------------------------------------------------------------ wiring */

void html_doc_init_window(HTMLDocument *d)
{
    d->window.IHTMLWindow2_iface.lpVtbl = &g_IHTMLWindow2_vtbl;
    d->window.IHTMLWindow3_iface.lpVtbl = &g_IHTMLWindow3_vtbl;
    d->window.IHTMLPrivateWindow_iface.lpVtbl = &priv_vtbl;
    d->window.doc = d;
    d->location.IHTMLLocation_iface.lpVtbl = &g_IHTMLLocation_vtbl;
    d->location.doc = d;
    d->PropNotifyCP_iface.lpVtbl = &pncp_vtbl;
}
