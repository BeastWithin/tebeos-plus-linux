/* eventtest.c -- prove the AttachEventHandler/attachEvent event-sinking loop end-to-end.
 *
 *   AttachEventHandler("onclick", handler) -> IHTMLElement2::attachEvent(name, proxy) ->
 *     store the sink, inject `addEventListener` that calls the CDP binding
 *     `window.__shim_evt(...)` -> Runtime.bindingCalled (on the page's evt channel) ->
 *     page.c pump -> posted to the UI thread -> shim_dom_event_dispatch -> sink->Invoke(DISPID 0).
 *
 * This is design (a) from the handoff: it reuses hostrender.c's DocObject hosting
 * scaffolding (so the render pump — the only reader of the render CDP channel, and the
 * thing that handles Runtime.bindingCalled — is actually running), then drives the exact
 * production path the injected "Medula'da Aç" table uses:
 *
 *   getElementById("b") -> QI IHTMLElement2 -> attachEvent("onclick", mockSink) ->
 *   IHTMLElement::click() -> (page fires) -> mockSink->Invoke(DISPID 0) -> counter++.
 *
 * The mock sink is a small cross-thread-safe C IDispatch (increments on DISPID 0), so it
 * stands in for the .NET HtmlToClrEventProxy that real mshtml calls back into.
 *
 * Usage:  wine shim32-eventtest.exe shim32.dll [port]
 */

#include <windows.h>
#include <initguid.h>
#include <ole2.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <oleidl.h>
#include <docobj.h>
#include <urlmon.h>
#include <ocidl.h>
#include <oaidl.h>
#include <mshtmhst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef int     (__cdecl *shim_connect_fn)(int port);
typedef int     (__cdecl *frame_info_fn)(IHTMLDocument2 *, int *, int *, long *);
typedef void    (__cdecl *disconnect_fn)(void);

/* A bare button; no inline handler, so the only click listener is the one our
 * attachEvent injects. */
static const char PAGE_URL[] =
    "data:text/html;base64,"
    "PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHk+PGJ1dHRvbiBpZ"
    "D0iYiI+QjwvYnV0dG9uPjwvYm9keT48L2h0bWw+";

static const char *hx(HRESULT h) { static char b[16]; snprintf(b, sizeof b, "0x%08lx", (unsigned long)h); return b; }
/* the dispinterfaces behind the interop coclass types the vendor casts to */
static const GUID T_DIID_DispHTMLDocument = {0x3050f55f,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};
static const GUID T_DIID_DispHTMLTableRow = {0x3050f535,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};
static const GUID T_DIID_DispHTMLTableCell = {0x3050f536,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};

/* IDispatch::Invoke as the CLR does on a dispinterface: a fixed DISPID, no GetIDsOfNames */
static HRESULT disp_call(IDispatch *d, DISPID id, WORD flags, VARIANT *args, UINT n, VARIANT *res)
{
    DISPPARAMS dp = { args, NULL, n, 0 };
    DISPID put = DISPID_PROPERTYPUT;
    if (flags & DISPATCH_PROPERTYPUT) {
        dp.rgdispidNamedArgs = &put;
        dp.cNamedArgs = 1;
    }
    if (res)
        VariantInit(res);
    return d->lpVtbl->Invoke(d, id, &IID_NULL, 0, flags, &dp, res, NULL, NULL);
}

static BSTR input_value(IHTMLElement *el)
{
    IHTMLInputElement *in = NULL;
    BSTR v = NULL;
    if (el && SUCCEEDED(el->lpVtbl->QueryInterface(el, &IID_IHTMLInputElement, (void **)&in))) {
        in->lpVtbl->get_value(in, &v);
        in->lpVtbl->Release(in);
    }
    return v;
}

static const char *pf(int ok) { return ok ? "PASS" : "FAIL"; }

/* Wait while dispatching messages: page events (DOM events, navigation) reach the
 * document through the UI thread's message queue, as they do in the app. */
static void pump_sleep(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        DWORD now = GetTickCount();
        if ((LONG)(end - now) <= 0)
            break;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, end - now < 20 ? end - now : 20, QS_ALLINPUT);
    }
}

/* ---- Mock event sink: a minimal IDispatch that counts Invoke(DISPID 0) ---- */
typedef struct MockSink {
    IDispatch IDispatch_iface;
    LONG ref;
    volatile LONG count;
    volatile LONG key;   /* keyCode of the last event object passed to Invoke */
    void (*fn)(void *);  /* optional: run on each Invoke (a vendor handler's work) */
    void *ctx;
} MockSink;

static inline MockSink *sink_from(IDispatch *t) { return CONTAINING_RECORD(t, MockSink, IDispatch_iface); }

static HRESULT STDMETHODCALLTYPE sink_qi(IDispatch *t, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
        *ppv = t;
        t->lpVtbl->AddRef(t);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sink_addref(IDispatch *t) { return InterlockedIncrement(&sink_from(t)->ref); }
static ULONG STDMETHODCALLTYPE sink_release(IDispatch *t) { return InterlockedDecrement(&sink_from(t)->ref); }
static HRESULT STDMETHODCALLTYPE sink_gettypeinfocount(IDispatch *t, UINT *n) { (void)t; if (n) *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE sink_gettypeinfo(IDispatch *t, UINT i, LCID l, ITypeInfo **p) { (void)t; (void)i; (void)l; if (p) *p = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sink_getidsofnames(IDispatch *t, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *d)
{ (void)t; (void)r; (void)names; (void)n; (void)l; if (d) *d = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE sink_invoke(IDispatch *t, DISPID d, REFIID r, LCID l, WORD w,
                                             DISPPARAMS *dp, VARIANT *res, EXCEPINFO *ex, UINT *ae)
{
    (void)r; (void)l; (void)w; (void)res; (void)ex; (void)ae;
    if (d == 0) {
        if (dp && dp->cArgs == 1 && V_VT(&dp->rgvarg[0]) == VT_DISPATCH && V_DISPATCH(&dp->rgvarg[0])) {
            IHTMLEventObj *ev = NULL;
            LONG k = 0;
            if (SUCCEEDED(V_DISPATCH(&dp->rgvarg[0])->lpVtbl->QueryInterface(V_DISPATCH(&dp->rgvarg[0]), &IID_IHTMLEventObj, (void **)&ev))) {
                ev->lpVtbl->get_keyCode(ev, &k);
                ev->lpVtbl->Release(ev);
            }
            sink_from(t)->key = k;
        }
        InterlockedIncrement(&sink_from(t)->count);
        if (sink_from(t)->fn)
            sink_from(t)->fn(sink_from(t)->ctx);
    }
    return S_OK;
}
static IDispatchVtbl sink_vtbl = {
    sink_qi, sink_addref, sink_release, sink_gettypeinfocount,
    sink_gettypeinfo, sink_getidsofnames, sink_invoke,
};
static void sink_init(MockSink *s) { ZeroMemory(s, sizeof(*s)); s->IDispatch_iface.lpVtbl = &sink_vtbl; s->ref = 1; }

/* Recete Giris's TEBEOS mouseup on each drug checkbox (Medula_Recete_Parser.Ilac_Bilgisi_checked):
 * when exactly one box is ticked, clear them all with SetAttribute("checked", "") */
static void clear_if_one(void *ctx)
{
    IHTMLElement **b = (IHTMLElement **)ctx;
    IHTMLInputElement *in;
    VARIANT_BOOL on;
    int i, n = 0;
    for (i = 0; i < 2; i++)
        if (b[i] && SUCCEEDED(b[i]->lpVtbl->QueryInterface(b[i], &IID_IHTMLInputElement, (void **)&in))) {
            on = VARIANT_FALSE;
            in->lpVtbl->get_checked(in, &on);
            n += on != VARIANT_FALSE;
            in->lpVtbl->Release(in);
        }
    if (n != 1)
        return;
    for (i = 0; i < 2; i++)
        if (b[i]) {
            BSTR a = SysAllocString(L"checked");
            VARIANT v;
            V_VT(&v) = VT_BSTR;
            V_BSTR(&v) = SysAllocString(L"");
            b[i]->lpVtbl->setAttribute(b[i], a, v, 0);
            VariantClear(&v);
            SysFreeString(a);
        }
}

/* ---- minimal OLE DocObject container (identical to hosttest.c / hostrender.c) ---- */
typedef struct Host Host;
struct Host {
    IOleClientSite     IOleClientSite_iface;
    IOleDocumentSite   IOleDocumentSite_iface;
    IOleInPlaceSiteEx  IOleInPlaceSiteEx_iface;
    IOleInPlaceFrame   IOleInPlaceFrame_iface;
    LONG ref;
    HWND hwnd;
    IUnknown *document;
    IOleDocumentView *view;
    BOOL activate_me_called;
    IOleInPlaceActiveObject *active_object;
    IDocHostUIHandler  IDocHostUIHandler_iface;
    LONG ctx_calls;         /* ShowContextMenu calls (this host shows its own menu: S_OK) */
    DWORD ctx_id;
};

static Host *host_from_client(IOleClientSite *i) { return CONTAINING_RECORD(i, Host, IOleClientSite_iface); }
static Host *host_from_docsite(IOleDocumentSite *i) { return CONTAINING_RECORD(i, Host, IOleDocumentSite_iface); }
static Host *host_from_ips(IOleInPlaceSiteEx *i) { return CONTAINING_RECORD(i, Host, IOleInPlaceSiteEx_iface); }
static Host *host_from_frame(IOleInPlaceFrame *i) { return CONTAINING_RECORD(i, Host, IOleInPlaceFrame_iface); }

static HRESULT host_qi(Host *h, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IOleClientSite))
        *ppv = &h->IOleClientSite_iface;
    else if (IsEqualIID(riid, &IID_IOleDocumentSite))
        *ppv = &h->IOleDocumentSite_iface;
    else if (IsEqualIID(riid, &IID_IOleInPlaceSite) || IsEqualIID(riid, &IID_IOleInPlaceSiteEx))
        *ppv = &h->IOleInPlaceSiteEx_iface;
    else if (IsEqualIID(riid, &IID_IOleInPlaceFrame) || IsEqualIID(riid, &IID_IOleInPlaceUIWindow))
        *ppv = &h->IOleInPlaceFrame_iface;
    else if (IsEqualIID(riid, &IID_IDocHostUIHandler))
        *ppv = &h->IDocHostUIHandler_iface;
    else { *ppv = NULL; return E_NOINTERFACE; }
    ((IUnknown *)*ppv)->lpVtbl->AddRef((IUnknown *)*ppv);
    return S_OK;
}
static ULONG host_addref(Host *h) { return InterlockedIncrement(&h->ref); }
static ULONG host_release(Host *h) { return InterlockedDecrement(&h->ref); }

static HRESULT STDMETHODCALLTYPE DS_ActivateMe(IOleDocumentSite *t, IOleDocumentView *pView)
{
    Host *h = host_from_docsite(t);
    IOleDocument *oledoc; RECT rc; HRESULT hr;
    h->activate_me_called = TRUE;
    hr = h->document->lpVtbl->QueryInterface(h->document, &IID_IOleDocument, (void **)&oledoc);
    if (FAILED(hr)) return hr;
    hr = oledoc->lpVtbl->CreateView(oledoc, (IOleInPlaceSite *)&h->IOleInPlaceSiteEx_iface, NULL, 0, &h->view);
    oledoc->lpVtbl->Release(oledoc);
    if (FAILED(hr)) return hr;
    GetClientRect(h->hwnd, &rc);
    h->view->lpVtbl->SetRect(h->view, &rc);
    return h->view->lpVtbl->Show(h->view, TRUE);
}

static HRESULT STDMETHODCALLTYPE CS_QI(IOleClientSite *t, REFIID r, void **p) { return host_qi(host_from_client(t), r, p); }
static ULONG    STDMETHODCALLTYPE CS_AddRef(IOleClientSite *t) { return host_addref(host_from_client(t)); }
static ULONG    STDMETHODCALLTYPE CS_Release(IOleClientSite *t) { return host_release(host_from_client(t)); }
static HRESULT STDMETHODCALLTYPE CS_SaveObject(IOleClientSite *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_GetMoniker(IOleClientSite *t, DWORD a, DWORD b, IMoniker **m) { (void)t; (void)a; (void)b; if (m) *m = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE CS_GetContainer(IOleClientSite *t, IOleContainer **c) { (void)t; if (c) *c = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE CS_ShowObject(IOleClientSite *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_OnShowWindow(IOleClientSite *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_RequestNewObjectLayout(IOleClientSite *t) { (void)t; return E_NOTIMPL; }

static IOleClientSiteVtbl CS_vtbl = {
    CS_QI, CS_AddRef, CS_Release, CS_SaveObject, CS_GetMoniker, CS_GetContainer,
    CS_ShowObject, CS_OnShowWindow, CS_RequestNewObjectLayout,
};

static HRESULT STDMETHODCALLTYPE DS_QI(IOleDocumentSite *t, REFIID r, void **p) { return host_qi(host_from_docsite(t), r, p); }
static ULONG    STDMETHODCALLTYPE DS_AddRef(IOleDocumentSite *t) { return host_addref(host_from_docsite(t)); }
static ULONG    STDMETHODCALLTYPE DS_Release(IOleDocumentSite *t) { return host_release(host_from_docsite(t)); }
static IOleDocumentSiteVtbl DS_vtbl = { DS_QI, DS_AddRef, DS_Release, DS_ActivateMe };

static HRESULT STDMETHODCALLTYPE IPS_QI(IOleInPlaceSiteEx *t, REFIID r, void **p) { return host_qi(host_from_ips(t), r, p); }
static ULONG    STDMETHODCALLTYPE IPS_AddRef(IOleInPlaceSiteEx *t) { return host_addref(host_from_ips(t)); }
static ULONG    STDMETHODCALLTYPE IPS_Release(IOleInPlaceSiteEx *t) { return host_release(host_from_ips(t)); }
static HRESULT STDMETHODCALLTYPE IPS_GetWindow(IOleInPlaceSiteEx *t, HWND *w) { if (w) *w = host_from_ips(t)->hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_ContextSensitiveHelp(IOleInPlaceSiteEx *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_CanInPlaceActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnUIActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_GetWindowContext(IOleInPlaceSiteEx *t, IOleInPlaceFrame **pf, IOleInPlaceUIWindow **pd, LPRECT pr, LPRECT pc, LPOLEINPLACEFRAMEINFO lf)
{
    Host *h = host_from_ips(t);
    (&h->IOleInPlaceFrame_iface)->lpVtbl->AddRef(&h->IOleInPlaceFrame_iface);
    *pf = &h->IOleInPlaceFrame_iface;
    if (pd) *pd = NULL;
    GetClientRect(h->hwnd, pr);
    if (pc) *pc = *pr;
    lf->cb = sizeof(*lf); lf->fMDIApp = FALSE; lf->hwndFrame = h->hwnd; lf->haccel = NULL; lf->cAccelEntries = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE IPS_Scroll(IOleInPlaceSiteEx *t, SIZE s) { (void)t; (void)s; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_OnUIDeactivate(IOleInPlaceSiteEx *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceDeactivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_DiscardUndoState(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_DeactivateAndUndo(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnPosRectChange(IOleInPlaceSiteEx *t, LPCRECT r) { (void)t; (void)r; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceActivateEx(IOleInPlaceSiteEx *t, BOOL *pnr, DWORD f) { (void)t; (void)f; if (pnr) *pnr = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceDeactivateEx(IOleInPlaceSiteEx *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_RequestUIActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }

static IOleInPlaceSiteExVtbl IPS_vtbl = {
    IPS_QI, IPS_AddRef, IPS_Release, IPS_GetWindow, IPS_ContextSensitiveHelp,
    IPS_CanInPlaceActivate, IPS_OnInPlaceActivate, IPS_OnUIActivate, IPS_GetWindowContext,
    IPS_Scroll, IPS_OnUIDeactivate, IPS_OnInPlaceDeactivate, IPS_DiscardUndoState,
    IPS_DeactivateAndUndo, IPS_OnPosRectChange,
    IPS_OnInPlaceActivateEx, IPS_OnInPlaceDeactivateEx, IPS_RequestUIActivate,
};

static HRESULT STDMETHODCALLTYPE FR_QI(IOleInPlaceFrame *t, REFIID r, void **p) { return host_qi(host_from_frame(t), r, p); }
static ULONG    STDMETHODCALLTYPE FR_AddRef(IOleInPlaceFrame *t) { return host_addref(host_from_frame(t)); }
static ULONG    STDMETHODCALLTYPE FR_Release(IOleInPlaceFrame *t) { return host_release(host_from_frame(t)); }
static HRESULT STDMETHODCALLTYPE FR_GetWindow(IOleInPlaceFrame *t, HWND *w) { if (w) *w = host_from_frame(t)->hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_ContextSensitiveHelp(IOleInPlaceFrame *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_GetBorder(IOleInPlaceFrame *t, LPRECT r) { (void)t; if (r) SetRectEmpty(r); return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_RequestBorderSpace(IOleInPlaceFrame *t, LPCBORDERWIDTHS b) { (void)t; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetBorderSpace(IOleInPlaceFrame *t, LPCBORDERWIDTHS b) { (void)t; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetActiveObject(IOleInPlaceFrame *t, IOleInPlaceActiveObject *a, LPCOLESTR n)
{ (void)n; host_from_frame(t)->active_object = a; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_InsertMenus(IOleInPlaceFrame *t, HMENU m, LPOLEMENUGROUPWIDTHS w) { (void)t; (void)m; (void)w; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetMenu(IOleInPlaceFrame *t, HMENU m, HOLEMENU o, HWND w) { (void)t; (void)m; (void)o; (void)w; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_RemoveMenus(IOleInPlaceFrame *t, HMENU m) { (void)t; (void)m; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetStatusText(IOleInPlaceFrame *t, LPCOLESTR s) { (void)t; (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_EnableModeless(IOleInPlaceFrame *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_TranslateAccelerator(IOleInPlaceFrame *t, LPMSG m, WORD w) { (void)t; (void)m; (void)w; return S_FALSE; }

static IOleInPlaceFrameVtbl FR_vtbl = {
    FR_QI, FR_AddRef, FR_Release, FR_GetWindow, FR_ContextSensitiveHelp,
    FR_GetBorder, FR_RequestBorderSpace, FR_SetBorderSpace, FR_SetActiveObject,
    FR_InsertMenus, FR_SetMenu, FR_RemoveMenus, FR_SetStatusText, FR_EnableModeless,
    FR_TranslateAccelerator,
};

/* IDocHostUIHandler: only ShowContextMenu matters (a host with its own menu, like WinForms
 * with IsWebBrowserContextMenuEnabled off); the rest say "default". */
static Host *host_from_ui(IDocHostUIHandler *i) { return CONTAINING_RECORD(i, Host, IDocHostUIHandler_iface); }
static HRESULT STDMETHODCALLTYPE UI_QI(IDocHostUIHandler *t, REFIID r, void **p) { return host_qi(host_from_ui(t), r, p); }
static ULONG STDMETHODCALLTYPE UI_AddRef(IDocHostUIHandler *t) { return host_addref(host_from_ui(t)); }
static ULONG STDMETHODCALLTYPE UI_Release(IDocHostUIHandler *t) { return host_release(host_from_ui(t)); }
static HRESULT STDMETHODCALLTYPE UI_ShowContextMenu(IDocHostUIHandler *t, DWORD id, POINT *pt, IUnknown *ct, IDispatch *d)
{ (void)pt; (void)ct; (void)d; host_from_ui(t)->ctx_calls++; host_from_ui(t)->ctx_id = id; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_GetHostInfo(IDocHostUIHandler *t, DOCHOSTUIINFO *i) { (void)t; (void)i; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE UI_ShowUI(IDocHostUIHandler *t, DWORD a, IOleInPlaceActiveObject *b, IOleCommandTarget *c, IOleInPlaceFrame *d, IOleInPlaceUIWindow *e)
{ (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE UI_HideUI(IDocHostUIHandler *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_UpdateUI(IDocHostUIHandler *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_EnableModeless(IDocHostUIHandler *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_OnDocWindowActivate(IDocHostUIHandler *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_OnFrameWindowActivate(IDocHostUIHandler *t, BOOL f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_ResizeBorder(IDocHostUIHandler *t, LPCRECT r, IOleInPlaceUIWindow *w, BOOL f) { (void)t; (void)r; (void)w; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE UI_TranslateAccelerator(IDocHostUIHandler *t, LPMSG m, const GUID *g, DWORD c) { (void)t; (void)m; (void)g; (void)c; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE UI_GetOptionKeyPath(IDocHostUIHandler *t, LPOLESTR *k, DWORD r) { (void)t; (void)r; if (k) *k = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE UI_GetDropTarget(IDocHostUIHandler *t, IDropTarget *a, IDropTarget **b) { (void)t; (void)a; if (b) *b = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE UI_GetExternal(IDocHostUIHandler *t, IDispatch **d) { (void)t; if (d) *d = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE UI_TranslateUrl(IDocHostUIHandler *t, DWORD a, LPWSTR b, LPWSTR *c) { (void)t; (void)a; (void)b; if (c) *c = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE UI_FilterDataObject(IDocHostUIHandler *t, IDataObject *a, IDataObject **b) { (void)t; (void)a; if (b) *b = NULL; return S_FALSE; }
static IDocHostUIHandlerVtbl UI_vtbl = {
    UI_QI, UI_AddRef, UI_Release, UI_ShowContextMenu, UI_GetHostInfo, UI_ShowUI, UI_HideUI,
    UI_UpdateUI, UI_EnableModeless, UI_OnDocWindowActivate, UI_OnFrameWindowActivate,
    UI_ResizeBorder, UI_TranslateAccelerator, UI_GetOptionKeyPath, UI_GetDropTarget,
    UI_GetExternal, UI_TranslateUrl, UI_FilterDataObject,
};

/* test clipboard helpers */
static void clip_set_w(const WCHAR *w)
{
    size_t n = (wcslen(w) + 1) * sizeof(WCHAR);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), w, n);
    GlobalUnlock(g);
    if (OpenClipboard(NULL)) { EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); CloseClipboard(); }
    else GlobalFree(g);
}
static void clip_get_w(WCHAR *out, int cap)
{
    HANDLE h; WCHAR *p;
    out[0] = 0;
    if (!OpenClipboard(NULL)) return;
    if ((h = GetClipboardData(CF_UNICODETEXT)) && (p = GlobalLock(h))) { lstrcpynW(out, p, cap); GlobalUnlock(h); }
    CloseClipboard();
}
/* a key with Ctrl and/or Shift held, as the app's thread sees it (GetKeyState) */
static void post_mod_key(HWND dw, UINT vk, int ctrl, int shift)
{
    BYTE ks[256];
    GetKeyboardState(ks);
    ks[VK_CONTROL] = ctrl ? 0x80 : 0; ks[VK_SHIFT] = shift ? 0x80 : 0;
    SetKeyboardState(ks);
    SendMessageW(dw, WM_KEYDOWN, vk, 1);
    SendMessageW(dw, WM_KEYUP, vk, 0xC0000001);
    ks[VK_CONTROL] = 0; ks[VK_SHIFT] = 0;
    SetKeyboardState(ks);
}

static void host_init(Host *h, HWND hwnd)
{
    ZeroMemory(h, sizeof(*h));
    h->IOleClientSite_iface.lpVtbl = &CS_vtbl;
    h->IOleDocumentSite_iface.lpVtbl = &DS_vtbl;
    h->IOleInPlaceSiteEx_iface.lpVtbl = &IPS_vtbl;
    h->IOleInPlaceFrame_iface.lpVtbl = &FR_vtbl;
    h->IDocHostUIHandler_iface.lpVtbl = &UI_vtbl;
    h->ref = 1;
    h->hwnd = hwnd;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { (void)m; (void)w; (void)l; return DefWindowProcW(h, m, w, l); }

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "shim32.dll";
    int port = (argc > 2) ? atoi(argv[2]) : 9222;
    WNDCLASSEXW wc; HWND hwnd; Host host; HINSTANCE hinst = GetModuleHandleW(NULL);
    HMODULE mod; DllGetClassObject_fn getco; shim_connect_fn domc, rendc; frame_info_fn finfo; disconnect_fn disco;
    IClassFactory *cf = NULL; IHTMLDocument2 *doc = NULL; IHTMLDocument3 *doc3 = NULL;
    IOleObject *ole = NULL; IPersistMoniker *pm = NULL; IMoniker *mk = NULL;
    HRESULT hr; int failures = 0; int i;

    setbuf(stdout, NULL);

    mod = LoadLibraryA(dll);
    if (!mod) { printf("FAIL: LoadLibrary(%s)\n", dll); return 1; }
    getco = (DllGetClassObject_fn)(void *)GetProcAddress(mod, "DllGetClassObject");
    domc  = (shim_connect_fn)(void *)GetProcAddress(mod, "shim_dom_connect");
    rendc = (shim_connect_fn)(void *)GetProcAddress(mod, "shim_render_connect");
    finfo = (frame_info_fn)(void *)GetProcAddress(mod, "shim_doc_render_frame_info");
    disco = (disconnect_fn)(void *)GetProcAddress(mod, "shim_dom_disconnect");
    if (!getco || !domc || !rendc || !finfo) { printf("FAIL: missing exports\n"); return 1; }

    printf("shim_dom_connect(%d)    = %s\n", port, pf(domc(port) == 0));
    printf("shim_render_connect(%d) = %s\n", port, pf(rendc(port) == 0));
    if (domc(port) != 0 || rendc(port) != 0) { failures++; }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndproc; wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"EventTestWnd";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, L"EventTestWnd", L"event", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, hinst, NULL);

    CoInitialize(NULL);
    host_init(&host, hwnd);

    hr = getco(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) { printf("FAIL: DllGetClassObject %s\n", hx(hr)); return 1; }
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IHTMLDocument2, (void **)&doc);
    if (FAILED(hr)) { printf("FAIL: CreateInstance %s\n", hx(hr)); return 1; }
    host.document = (IUnknown *)doc;
    doc->lpVtbl->QueryInterface(doc, &IID_IHTMLDocument3, (void **)&doc3);

    hr = doc->lpVtbl->QueryInterface(doc, &IID_IOleObject, (void **)&ole);
    ole->lpVtbl->SetClientSite(ole, &host.IOleClientSite_iface);

    hr = doc->lpVtbl->QueryInterface(doc, &IID_IPersistMoniker, (void **)&pm);
    {
        OLECHAR wurl[2048];
        MultiByteToWideChar(CP_UTF8, 0, PAGE_URL, -1, wurl, 2048);
        CreateURLMoniker(NULL, wurl, &mk);
        hr = pm->lpVtbl->Load(pm, TRUE, mk, NULL, 0);
        printf("Load(data URL) = %s\n", hx(hr));
    }

    hr = ole->lpVtbl->DoVerb(ole, OLEIVERB_SHOW, NULL, NULL, 0, 0, NULL);
    printf("DoVerb(SHOW) = %s\n", hx(hr));
    if (host.view)
        host.view->lpVtbl->UIActivate(host.view, TRUE);

    /* wait for the render pump to decode its first frame (proves the pump — the
     * bindingCalled reader — is alive before we attach + click). */
    {
        long seq = -1; int fw = 0, fh = 0; int got_frame = 0;
        for (i = 0; i < 50; i++) {
            if (finfo(doc, &fw, &fh, &seq) == 0 && seq > 0) { got_frame = 1; break; }
            pump_sleep(120);
        }
        printf("render frame seq=%ld %dx%d %s\n", seq, fw, fh, pf(got_frame));
        if (!got_frame) { printf("FAIL: no rendered frame decoded\n"); failures++; }
    }

    /* ---- the event loop under test ---- */
    {
        BSTR id = SysAllocString(L"b");
        IHTMLElement *el = NULL;
        IHTMLElement2 *e2 = NULL;
        MockSink sink;
        sink_init(&sink);
        VARIANT_BOOL ok = VARIANT_FALSE;
        long c1, c2;

        hr = doc3->lpVtbl->getElementById(doc3, id, &el);
        printf("getElementById(b) = %s elem=%s\n", hx(hr), pf(SUCCEEDED(hr) && el));
        if (!el) { failures++; SysFreeString(id); goto no_elem; }

        if (SUCCEEDED(el->lpVtbl->QueryInterface(el, &IID_IHTMLElement2, (void **)&e2)))
            printf("QI IHTMLElement2 = PASS\n");
        else { printf("QI IHTMLElement2 = FAIL\n"); failures++; }

        if (e2) {
            BSTR on = SysAllocString(L"onclick");
            hr = e2->lpVtbl->attachEvent(e2, on, &sink.IDispatch_iface, &ok);
            SysFreeString(on);
            printf("attachEvent(onclick) hr=%s pfResult=%s %s\n", hx(hr),
                   ok == VARIANT_TRUE ? "VARIANT_TRUE" : "VARIANT_FALSE",
                   pf(SUCCEEDED(hr) && ok == VARIANT_TRUE));
            if (FAILED(hr) || ok != VARIANT_TRUE) failures++;
        }

        /* give Runtime.addBinding time to land in the page before the click, so the
         * injected listener's `if(window.__shim_evt)` guard doesn't drop it. */
        pump_sleep(1000);

        /* trigger the DOM click (production parity with HtmlElement.InvokeMember). */
        hr = el->lpVtbl->click(el);
        printf("IHTMLElement::click() hr=%s\n", hx(hr));

        /* allow click -> binding -> bindingCalled -> dispatch -> Invoke(0) to complete. */
        for (i = 0; i < 50 && sink.count == 0; i++)
            pump_sleep(120);

        c1 = sink.count;
        printf("sink count after 1 click = %ld  %s\n", c1, pf(c1 == 1));
        if (c1 != 1) failures++;

        /* a second click must fire again (the listener is persistent). */
        el->lpVtbl->click(el);
        for (i = 0; i < 50 && sink.count < 2; i++)
            pump_sleep(120);
        c2 = sink.count;
        printf("sink count after 2 clicks = %ld  %s\n", c2, pf(c2 == 2));
        if (c2 != 2) failures++;

        /* detachEvent removes the sink: a 3rd click must not increment it. */
        if (e2) {
            BSTR on = SysAllocString(L"onclick");
            e2->lpVtbl->detachEvent(e2, on, &sink.IDispatch_iface);
            SysFreeString(on);
        }
        el->lpVtbl->click(el);
        pump_sleep(800);
        printf("sink count after detachEvent + click = %ld  %s\n",
               sink.count, pf(sink.count == 2));
        if (sink.count != 2) failures++;

        if (e2) e2->lpVtbl->Release(e2);
        el->lpVtbl->Release(el);
        SysFreeString(id);
    no_elem:
        ;
    }

    /* ---- document-level attachEvent (IHTMLDocument3) + get_readyState ---- */
    {
        BSTR id = SysAllocString(L"b");
        IHTMLElement *el = NULL;
        IHTMLElement2 *e2 = NULL;
        MockSink dsink;
        VARIANT_BOOL ok = VARIANT_FALSE;
        VARIANT rs;
        long d1, d2;

        sink_init(&dsink);

        /* 1. IHTMLElement2::get_readyState -> VT_BSTR "complete" */
        if (SUCCEEDED(doc3->lpVtbl->getElementById(doc3, id, &el)) && el &&
            SUCCEEDED(el->lpVtbl->QueryInterface(el, &IID_IHTMLElement2, (void **)&e2))) {
            VariantInit(&rs);
            hr = e2->lpVtbl->get_readyState(e2, &rs);
            {
                int okrs = (hr == S_OK && rs.vt == VT_BSTR &&
                            rs.bstrVal && wcscmp(rs.bstrVal, L"complete") == 0);
                printf("get_readyState hr=%s vt=%d  %s\n", hx(hr), rs.vt, pf(okrs));
                if (!okrs) failures++;
                VariantClear(&rs);
            }
        } else {
            printf("FAIL: re-fetch element for readyState/doc-event\n");
            failures++;
        }

        /* 2. IHTMLDocument3::attachEvent("onclick", docSink) */
        {
            BSTR on = SysAllocString(L"onclick");
            hr = doc3->lpVtbl->attachEvent(doc3, on, &dsink.IDispatch_iface, &ok);
            SysFreeString(on);
            printf("doc attachEvent(onclick) hr=%s pfResult=%s  %s\n", hx(hr),
                   ok == VARIANT_TRUE ? "VARIANT_TRUE" : "VARIANT_FALSE",
                   pf(SUCCEEDED(hr) && ok == VARIANT_TRUE));
            if (FAILED(hr) || ok != VARIANT_TRUE) failures++;
        }
        pump_sleep(1000); /* let the document listener + binding settle */

        /* 3. clicking the button bubbles the event to document -> doc sink fires */
        if (el) {
            int i2;
            el->lpVtbl->click(el);
            for (i2 = 0; i2 < 50 && dsink.count == 0; i2++) pump_sleep(120);
            d1 = dsink.count;
            printf("doc sink after 1 click = %ld  %s\n", d1, pf(d1 == 1));
            if (d1 != 1) failures++;

            el->lpVtbl->click(el);
            for (i2 = 0; i2 < 50 && dsink.count < 2; i2++) pump_sleep(120);
            d2 = dsink.count;
            printf("doc sink after 2 clicks = %ld  %s\n", d2, pf(d2 == 2));
            if (d2 != 2) failures++;

            /* detach: a 3rd click must not fire the doc sink */
            {
                BSTR on = SysAllocString(L"onclick");
                doc3->lpVtbl->detachEvent(doc3, on, &dsink.IDispatch_iface);
                SysFreeString(on);
            }
            el->lpVtbl->click(el);
            pump_sleep(800);
            printf("doc sink after detachEvent + click = %ld  %s\n",
                   dsink.count, pf(dsink.count == 2));
            if (dsink.count != 2) failures++;
        }

        if (e2) e2->lpVtbl->Release(e2);
        if (el) el->lpVtbl->Release(el);
        SysFreeString(id);
    }

    /* ---- the "Hasta Arama (F3)" bugs on E-Recete Sorgula ----
     * The vendor injects an untyped <button> by OuterHtml += on the T.C. field and binds F3 with
     * attachEvent("onkeydown") on that field. In IE7/quirks the button is type=button (no
     * submit); a key typed into the shim's window must reach the page as keydown/keyup. */
    {
        IHTMLElement *body = NULL, *t = NULL, *kb = NULL, *kf = NULL, *ks = NULL;
        IHTMLElement2 *t2 = NULL, *f2 = NULL;
        IOleInPlaceObject *ipo = NULL;
        HWND dw = NULL;
        MockSink ksink, usink, ssink;
        VARIANT_BOOL ok = VARIANT_FALSE;
        VARIANT tv;
        int i2;
        BSTR s1, s2;
        sink_init(&ksink); sink_init(&usink); sink_init(&ssink);
        VariantInit(&tv);

        doc->lpVtbl->get_body(doc, &body);
        s1 = SysAllocString(L"beforeEnd");
        s2 = SysAllocString(L"<form id='kf' action='javascript:void 0'><input id='kt'>"
                            L"<input type='submit' id='ks' value='S'></form>");
        if (body) body->lpVtbl->insertAdjacentHTML(body, s1, s2);
        SysFreeString(s1); SysFreeString(s2);
        /* the vendor's injection: elementById.OuterHtml += "&nbsp;&nbsp;<button id=...>" */
        s1 = SysAllocString(L"kt");
        doc3->lpVtbl->getElementById(doc3, s1, &t);
        if (t) {
            BSTR oh = NULL, add = SysAllocString(L"&nbsp;&nbsp;<button id='kb'>Hasta Arama(F3)</button>"), nw;
            t->lpVtbl->get_outerHTML(t, &oh);
            nw = SysAllocStringLen(NULL, SysStringLen(oh) + SysStringLen(add));
            memcpy(nw, oh, SysStringLen(oh) * sizeof(WCHAR));
            memcpy(nw + SysStringLen(oh), add, (SysStringLen(add) + 1) * sizeof(WCHAR));
            t->lpVtbl->put_outerHTML(t, nw);
            SysFreeString(oh); SysFreeString(add); SysFreeString(nw);
            t->lpVtbl->Release(t); t = NULL;
        }
        doc3->lpVtbl->getElementById(doc3, s1, &t);   /* the field, re-fetched as the vendor does */
        SysFreeString(s1);
        s1 = SysAllocString(L"kb"); doc3->lpVtbl->getElementById(doc3, s1, &kb); SysFreeString(s1);
        s1 = SysAllocString(L"kf"); doc3->lpVtbl->getElementById(doc3, s1, &kf); SysFreeString(s1);
        s1 = SysAllocString(L"ks"); doc3->lpVtbl->getElementById(doc3, s1, &ks); SysFreeString(s1);
        printf("injected form/field/button = %s\n", pf(t && kb && kf && ks));
        if (!(t && kb && kf && ks)) failures++;

        if (kb) {
            s1 = SysAllocString(L"type");
            kb->lpVtbl->getAttribute(kb, s1, 0, &tv);
            SysFreeString(s1);
            printf("untyped <button> type = %ls  %s\n", V_VT(&tv) == VT_BSTR ? V_BSTR(&tv) : L"?",
                   pf(V_VT(&tv) == VT_BSTR && !lstrcmpW(V_BSTR(&tv), L"button")));
            if (!(V_VT(&tv) == VT_BSTR && !lstrcmpW(V_BSTR(&tv), L"button"))) failures++;
            VariantClear(&tv);
        }
        if (kf && SUCCEEDED(kf->lpVtbl->QueryInterface(kf, &IID_IHTMLElement2, (void **)&f2))) {
            s1 = SysAllocString(L"onsubmit");
            f2->lpVtbl->attachEvent(f2, s1, &ssink.IDispatch_iface, &ok);
            SysFreeString(s1);
            pump_sleep(500);
            kb->lpVtbl->click(kb);
            pump_sleep(1500);
            printf("click untyped <button>: form submits = %ld  %s\n", ssink.count, pf(ssink.count == 0));
            if (ssink.count != 0) failures++;
            ks->lpVtbl->click(ks);   /* control: a real submit is seen by the same sink */
            for (i2 = 0; i2 < 25 && ssink.count == 0; i2++) pump_sleep(120);
            printf("click type=submit (control): form submits = %ld  %s\n", ssink.count, pf(ssink.count == 1));
            if (ssink.count != 1) failures++;
        }

        if (t && SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IHTMLElement2, (void **)&t2))) {
            s1 = SysAllocString(L"onkeydown");
            t2->lpVtbl->attachEvent(t2, s1, &ksink.IDispatch_iface, &ok);
            SysFreeString(s1);
            s1 = SysAllocString(L"onkeyup");
            t2->lpVtbl->attachEvent(t2, s1, &usink.IDispatch_iface, &ok);
            SysFreeString(s1);
            t2->lpVtbl->focus(t2);
            pump_sleep(500);
        }
        if (SUCCEEDED(doc->lpVtbl->QueryInterface(doc, &IID_IOleInPlaceObject, (void **)&ipo))) {
            ipo->lpVtbl->GetWindow(ipo, &dw);
            ipo->lpVtbl->Release(ipo);
        }
        printf("document window = %s\n", pf(dw && IsWindow(dw)));
        if (dw) {
            SendMessageW(dw, WM_KEYDOWN, VK_F3, 0x003D0001);
            SendMessageW(dw, WM_KEYUP, VK_F3, 0xC03D0001);
            for (i2 = 0; i2 < 25 && (ksink.count == 0 || usink.count == 0); i2++) pump_sleep(120);
            printf("F3 -> onkeydown sinks = %ld keyCode = %ld  %s\n", ksink.count, ksink.key,
                   pf(ksink.count == 1 && ksink.key == 114));
            printf("F3 -> onkeyup   sinks = %ld keyCode = %ld  %s\n", usink.count, usink.key,
                   pf(usink.count == 1 && usink.key == 114));
            if (!(ksink.count == 1 && ksink.key == 114 && usink.count == 1 && usink.key == 114)) failures++;
            /* Escape (vendor closes its panels on 27) arrives; F5 stays out of Chromium */
            SendMessageW(dw, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
            for (i2 = 0; i2 < 25 && ksink.count < 2; i2++) pump_sleep(120);
            printf("Escape -> onkeydown keyCode = %ld  %s\n", ksink.key, pf(ksink.count == 2 && ksink.key == 27));
            if (!(ksink.count == 2 && ksink.key == 27)) failures++;
            SendMessageW(dw, WM_KEYDOWN, VK_F5, 0x003F0001);
            pump_sleep(1500);
            printf("F5 not forwarded (no Chromium reload): sinks = %ld  %s\n", ksink.count, pf(ksink.count == 2));
            if (ksink.count != 2) failures++;

            /* Backspace/Delete as the app gets them: posted WM_KEYDOWN, so TranslateMessage
             * also makes the WM_CHAR 0x08 that used to be inserted as text */
            {
                IHTMLInputElement *in = NULL;
                BSTR v;
                if (SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IHTMLInputElement, (void **)&in))) {
                    BSTR s5 = SysAllocString(L"12345");
                    in->lpVtbl->put_value(in, s5);    /* the caret goes to the end */
                    SysFreeString(s5);
                    in->lpVtbl->Release(in);
                }
                pump_sleep(300);
                PostMessageW(dw, WM_KEYDOWN, VK_BACK, 0x000E0001);
                PostMessageW(dw, WM_KEYUP, VK_BACK, 0xC00E0001);
                pump_sleep(1000);
                v = input_value(t);
                printf("Backspace at end of 12345 -> \"%ls\" (len %u)  %s\n", v ? v : L"?",
                       v ? SysStringLen(v) : 0, pf(v && !lstrcmpW(v, L"1234")));
                if (!(v && !lstrcmpW(v, L"1234"))) failures++;
                SysFreeString(v);
                PostMessageW(dw, WM_KEYDOWN, VK_HOME, 0x01470001);
                PostMessageW(dw, WM_KEYUP, VK_HOME, 0xC1470001);
                PostMessageW(dw, WM_KEYDOWN, VK_DELETE, 0x01530001);
                PostMessageW(dw, WM_KEYUP, VK_DELETE, 0xC1530001);
                pump_sleep(1000);
                v = input_value(t);
                printf("Home, Delete -> \"%ls\"  %s\n", v ? v : L"?", pf(v && !lstrcmpW(v, L"234")));
                if (!(v && !lstrcmpW(v, L"234"))) failures++;
                SysFreeString(v);
            }

            /* SGK's hxclient masks (the T.C. field) cancel the Backspace/Delete keydown and redo
             * the edit on their own copy of the value, which TEBEOS's writes leave out of step:
             * typing goes in as text and Backspace/Delete through the browser's delete command,
             * so the field edits even with such a handler on it. */
            {
                IHTMLInputElement *in = NULL;
                BSTR v, on;
                UINT k;
                static const UINT keys[] = { '1', '2', '3' };
                LONG ku0 = usink.count;
                {
                    BSTR where = SysAllocString(L"beforeEnd"), js = SysAllocString(
                        L"<img src='x' style='display:none' onerror=\"var t=document.getElementById('kt');"
                        L"t.addEventListener('keydown',function(e){var c=e.keyCode;if(c==8||c==46||c==37||c==39)e.preventDefault();});"
                        L"t.addEventListener('keypress',function(e){if(e.keyCode==13)t.title='kp13';e.preventDefault();});\">");
                    if (body) body->lpVtbl->insertAdjacentHTML(body, where, js);
                    SysFreeString(where); SysFreeString(js);
                }
                if (SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IHTMLInputElement, (void **)&in))) {
                    BSTR e0 = SysAllocString(L"");
                    in->lpVtbl->put_value(in, e0);
                    SysFreeString(e0);
                    in->lpVtbl->Release(in);
                }
                pump_sleep(800);
                for (k = 0; k < ARRAYSIZE(keys); k++) {
                    UINT sc = MapVirtualKeyW(keys[k], MAPVK_VK_TO_VSC);
                    PostMessageW(dw, WM_KEYDOWN, keys[k], 1 | (sc << 16));
                    PostMessageW(dw, WM_KEYUP, keys[k], 0xC0000001 | (sc << 16));
                }
                pump_sleep(1000);
                v = input_value(t);
                printf("hxclient-like field: typed 1,2,3 -> \"%ls\"  %s\n", v ? v : L"?", pf(v && !lstrcmpW(v, L"123")));
                if (!(v && !lstrcmpW(v, L"123"))) failures++;
                SysFreeString(v);
                PostMessageW(dw, WM_KEYDOWN, VK_BACK, 0x000E0001);
                PostMessageW(dw, WM_KEYUP, VK_BACK, 0xC00E0001);
                pump_sleep(1000);
                v = input_value(t);
                printf("  Backspace (keydown cancelled by the page) -> \"%ls\"  %s\n", v ? v : L"?", pf(v && !lstrcmpW(v, L"12")));
                if (!(v && !lstrcmpW(v, L"12"))) failures++;
                SysFreeString(v);
                PostMessageW(dw, WM_KEYDOWN, VK_HOME, 0x01470001);
                PostMessageW(dw, WM_KEYUP, VK_HOME, 0xC1470001);
                PostMessageW(dw, WM_KEYDOWN, VK_DELETE, 0x01530001);
                PostMessageW(dw, WM_KEYUP, VK_DELETE, 0xC1530001);
                pump_sleep(1000);
                v = input_value(t);
                printf("  Home, Delete -> \"%ls\"  %s\n", v ? v : L"?", pf(v && !lstrcmpW(v, L"2")));
                if (!(v && !lstrcmpW(v, L"2"))) failures++;
                SysFreeString(v);
                printf("  keyup sinks for Backspace/Home/Delete = +%ld (last keyCode %ld)  %s\n", usink.count - ku0, usink.key,
                       pf(usink.count - ku0 == 3 && usink.key == 46));
                if (!(usink.count - ku0 == 3 && usink.key == 46)) failures++;
                /* the arrows: hxclient cancels their keydown too. One key at a time, as typed:
                 * TranslateMessage posts each WM_CHAR behind whatever is already queued. */
                {
                    static const UINT seq[] = { VK_RIGHT, '5', VK_LEFT, VK_LEFT, '7' };
                    for (k = 0; k < ARRAYSIZE(seq); k++) {
                        UINT sc = MapVirtualKeyW(seq[k], MAPVK_VK_TO_VSC);
                        LPARAM ext = (seq[k] == VK_LEFT || seq[k] == VK_RIGHT) ? 0x01000000 : 0;
                        PostMessageW(dw, WM_KEYDOWN, seq[k], 1 | (sc << 16) | ext);
                        PostMessageW(dw, WM_KEYUP, seq[k], 0xC0000001 | (sc << 16) | ext);
                        pump_sleep(150);
                    }
                }
                pump_sleep(1000);
                v = input_value(t);
                printf("  Right, 5, Left, Left, 7 -> \"%ls\"  %s\n", v ? v : L"?", pf(v && !lstrcmpW(v, L"725")));
                if (!(v && !lstrcmpW(v, L"725"))) failures++;
                SysFreeString(v);
                /* Enter is a keypress 13 (SGK's medula.js moves the focus on it and cancels it) */
                {
                    LONG sub0 = ssink.count;
                    on = NULL;
                    PostMessageW(dw, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                    PostMessageW(dw, WM_KEYUP, VK_RETURN, 0xC01C0001);
                    pump_sleep(1500);
                    t->lpVtbl->get_title(t, &on);
                    printf("  Enter -> keypress 13 seen = %s, cancelled: form submits +%ld  %s\n",
                           on && !lstrcmpW(on, L"kp13") ? "yes" : "no", ssink.count - sub0,
                           pf(on && !lstrcmpW(on, L"kp13") && ssink.count == sub0));
                    if (!(on && !lstrcmpW(on, L"kp13") && ssink.count == sub0)) failures++;
                    SysFreeString(on);
                }
            }

            /* Clipboard keys against the Windows clipboard (headless Chromium's own is cut off
             * from the desktop): Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V, Shift+Insert. */
            {
                IHTMLInputElement *in = NULL;
                WCHAR cb[64];
                BSTR v;
                if (SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IHTMLInputElement, (void **)&in))) {
                    BSTR s5 = SysAllocString(L"merhaba");
                    in->lpVtbl->put_value(in, s5);
                    SysFreeString(s5);
                    in->lpVtbl->Release(in);
                }
                clip_set_w(L"-");
                pump_sleep(300);
                post_mod_key(dw, 'A', 1, 0);
                post_mod_key(dw, 'C', 1, 0);
                pump_sleep(300);
                clip_get_w(cb, 64);
                printf("Ctrl+A, Ctrl+C -> clipboard \"%ls\"  %s\n", cb, pf(!lstrcmpW(cb, L"merhaba")));
                if (lstrcmpW(cb, L"merhaba")) failures++;
                clip_set_w(L"-");
                post_mod_key(dw, 'X', 1, 0);
                pump_sleep(800);
                clip_get_w(cb, 64);
                v = input_value(t);
                printf("Ctrl+X -> clipboard \"%ls\", field \"%ls\"  %s\n", cb, v ? v : L"?",
                       pf(!lstrcmpW(cb, L"merhaba") && v && !v[0]));
                if (!(!lstrcmpW(cb, L"merhaba") && v && !v[0])) failures++;
                SysFreeString(v);
                clip_set_w(L"A\x011f\x0131 12");
                post_mod_key(dw, 'V', 1, 0);
                pump_sleep(300);
                post_mod_key(dw, VK_INSERT, 0, 1);
                pump_sleep(1000);
                v = input_value(t);
                printf("Ctrl+V, Shift+Insert -> field \"%ls\"  %s\n", v ? v : L"?",
                       pf(v && !lstrcmpW(v, L"A\x011f\x0131 12A\x011f\x0131 12")));
                if (!(v && !lstrcmpW(v, L"A\x011f\x0131 12A\x011f\x0131 12"))) failures++;
                SysFreeString(v);
                /* a long, multi-line paste (a list of karekods): a 512-byte buffer once cut it */
                {
                    static WCHAR big[1200];
                    BSTR e0 = SysAllocString(L"");
                    IHTMLInputElement *in2 = NULL;
                    int k2;
                    for (k2 = 0; k2 < 1000; k2++)
                        big[k2] = (k2 % 50 == 49 && k2 < 999) ? L'\n' : (WCHAR)(L'0' + k2 % 10);
                    big[1000] = 0;
                    if (SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IHTMLInputElement, (void **)&in2))) {
                        in2->lpVtbl->put_value(in2, e0);
                        in2->lpVtbl->Release(in2);
                    }
                    SysFreeString(e0);
                    clip_set_w(big);
                    pump_sleep(300);
                    post_mod_key(dw, 'V', 1, 0);
                    pump_sleep(1000);
                    v = input_value(t);
                    printf("Ctrl+V of 1000 chars in 20 lines -> field length %u  %s\n", v ? SysStringLen(v) : 0,
                           pf(v && SysStringLen(v) == 1000));
                    if (!(v && SysStringLen(v) == 1000)) failures++;
                    SysFreeString(v);
                }
            }

            /* TEXTAREA innerText is the box's text (its value), as in IE (iecompat.js 5): TEBEOS
             * reads the Karekod box that way to save the scanned karekods */
            {
                BSTR where = SysAllocString(L"beforeEnd"), h = SysAllocString(L"<textarea id='kta'>srv</textarea>"), id = SysAllocString(L"kta"), v = NULL;
                IHTMLElement *ta = NULL;
                IHTMLTextAreaElement *tae = NULL;
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
                doc3->lpVtbl->getElementById(doc3, id, &ta);
                if (ta) ta->lpVtbl->get_innerText(ta, &v);
                printf("textarea innerText (server text) = '%ls'  %s\n", v ? v : L"(null)", pf(v && !lstrcmpW(v, L"srv")));
                if (!(v && !lstrcmpW(v, L"srv"))) failures++;
                SysFreeString(v); v = NULL;
                if (ta && SUCCEEDED(ta->lpVtbl->QueryInterface(ta, &IID_IHTMLTextAreaElement, (void **)&tae))) {
                    BSTR nv = SysAllocString(L"k1\nk2");
                    tae->lpVtbl->put_value(tae, nv);
                    SysFreeString(nv);
                    tae->lpVtbl->Release(tae);
                }
                if (ta) ta->lpVtbl->get_innerText(ta, &v);
                printf("textarea innerText after a 2-line value = %u chars  %s\n", v ? SysStringLen(v) : 0,
                       pf(v && v[0] == L'k' && wcsstr(v, L"k2")));
                if (!(v && v[0] == L'k' && wcsstr(v, L"k2"))) failures++;
                SysFreeString(v);
                SysFreeString(where); SysFreeString(h); SysFreeString(id);
                if (ta) ta->lpVtbl->Release(ta);
            }

            /* Right click: the host is asked for its menu (this one shows its own), unless the
             * page cancels the contextmenu event */
            {
                LONG c0 = host.ctx_calls;
                BSTR where = SysAllocString(L"beforeEnd"), js;
                PostMessageW(dw, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(400, 300));
                PostMessageW(dw, WM_RBUTTONUP, 0, MAKELPARAM(400, 300));
                pump_sleep(1200);
                printf("right click -> host ShowContextMenu calls %ld (id %lu)  %s\n", host.ctx_calls - c0,
                       (unsigned long)host.ctx_id, pf(host.ctx_calls - c0 == 1));
                if (host.ctx_calls - c0 != 1) failures++;
                js = SysAllocString(L"<img src='x' style='display:none' onerror=\"document.oncontextmenu=function(){return false;};\">");
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, js);
                SysFreeString(where); SysFreeString(js);
                pump_sleep(500);
                c0 = host.ctx_calls;
                PostMessageW(dw, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(400, 300));
                PostMessageW(dw, WM_RBUTTONUP, 0, MAKELPARAM(400, 300));
                pump_sleep(1200);
                printf("right click, page cancels contextmenu -> host calls %ld  %s\n", host.ctx_calls - c0,
                       pf(host.ctx_calls == c0));
                if (host.ctx_calls != c0) failures++;
            }

            /* <select> drop-downs: headless Chromium never shows its own list, so the shim
             * draws one in the page. Click the drop-down, then the second option in that list. */
            {
                IHTMLElement *sel = NULL;
                IHTMLElement2 *sel2 = NULL;
                MockSink csink;
                VARIANT si;
                BSTR where = SysAllocString(L"beforeEnd"), h = SysAllocString(
                    L"<select id='ksel' style='position:fixed;left:5px;top:5px;height:20px;font:16px Arial;z-index:9999'>"
                    L"<option>a</option><option>b</option><option>c</option></select>");
                sink_init(&csink);
                VariantInit(&si);
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
                SysFreeString(where); SysFreeString(h);
                s1 = SysAllocString(L"ksel"); doc3->lpVtbl->getElementById(doc3, s1, &sel); SysFreeString(s1);
                if (sel && SUCCEEDED(sel->lpVtbl->QueryInterface(sel, &IID_IHTMLElement2, (void **)&sel2))) {
                    s1 = SysAllocString(L"onchange");
                    sel2->lpVtbl->attachEvent(sel2, s1, &csink.IDispatch_iface, &ok);
                    SysFreeString(s1);
                }
                pump_sleep(500);
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 15));
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(15, 15));
                pump_sleep(800);
                /* the list opens under the drop-down (bottom 25), one ~20px row per option */
                PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(15, 55));
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 55));
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(15, 55));
                for (i2 = 0; i2 < 25 && csink.count == 0; i2++) pump_sleep(120);
                if (sel) {
                    s1 = SysAllocString(L"selectedIndex");
                    sel->lpVtbl->getAttribute(sel, s1, 0, &si);
                    SysFreeString(s1);
                }
                VariantChangeType(&si, &si, 0, VT_I4);
                printf("drop-down: click it, click the 2nd option -> selectedIndex %ld, onchange sinks %ld  %s\n",
                       V_VT(&si) == VT_I4 ? (long)V_I4(&si) : -1L, csink.count,
                       pf(V_VT(&si) == VT_I4 && V_I4(&si) == 1 && csink.count == 1));
                if (!(V_VT(&si) == VT_I4 && V_I4(&si) == 1 && csink.count == 1)) failures++;
                VariantClear(&si);
                if (sel) { BSTR e0 = SysAllocString(L""); sel->lpVtbl->put_outerHTML(sel, e0); SysFreeString(e0); }
                if (sel2) {   /* csink lives on this stack frame: the shim must not keep it */
                    s1 = SysAllocString(L"onchange");
                    sel2->lpVtbl->detachEvent(sel2, s1, &csink.IDispatch_iface);
                    SysFreeString(s1);
                    sel2->lpVtbl->Release(sel2);
                }
                if (sel) sel->lpVtbl->Release(sel);
            }

            /* Recete Listesi: TEBEOS's "kontrol edildi" checkbox in a row whose click opens the
             * prescription; its handler sets event.cancelBubble, which IE honours at once */
            {
                IHTMLElement *chk = NULL, *row = NULL, *chk2 = NULL;
                IHTMLElement2 *c2 = NULL;
                IHTMLInputElement *ci = NULL;
                MockSink vsink;
                VARIANT_BOOL on1 = VARIANT_FALSE;
                BSTR rt = NULL, rt2 = NULL;
                BSTR where = SysAllocString(L"beforeEnd"), h = SysAllocString(
                    L"<table><tr id='krow' onclick=\"this.title=(+this.title||0)+1\"><td>"
                    L"<input type='checkbox' class='selectBooleanCheckbox' id='kchk' style='position:fixed;left:5px;top:5px;margin:0;width:16px;height:16px'>"
                    L"<input type='checkbox' id='kchk2' style='position:fixed;left:40px;top:5px;margin:0;width:16px;height:16px'>"
                    L"</td></tr></table>");
                sink_init(&vsink);
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
                SysFreeString(where); SysFreeString(h);
                s1 = SysAllocString(L"kchk"); doc3->lpVtbl->getElementById(doc3, s1, &chk); SysFreeString(s1);
                s1 = SysAllocString(L"kchk2"); doc3->lpVtbl->getElementById(doc3, s1, &chk2); SysFreeString(s1);
                s1 = SysAllocString(L"krow"); doc3->lpVtbl->getElementById(doc3, s1, &row); SysFreeString(s1);
                if (chk && SUCCEEDED(chk->lpVtbl->QueryInterface(chk, &IID_IHTMLElement2, (void **)&c2))) {
                    s1 = SysAllocString(L"onclick");
                    c2->lpVtbl->attachEvent(c2, s1, &vsink.IDispatch_iface, &ok);
                    SysFreeString(s1);
                }
                pump_sleep(500);
                PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(12, 12));
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 12));
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(12, 12));
                for (i2 = 0; i2 < 25 && vsink.count == 0; i2++) pump_sleep(120);
                pump_sleep(300);
                if (row) row->lpVtbl->get_title(row, &rt);
                if (chk && SUCCEEDED(chk->lpVtbl->QueryInterface(chk, &IID_IHTMLInputElement, (void **)&ci))) {
                    ci->lpVtbl->get_checked(ci, &on1);
                    ci->lpVtbl->Release(ci);
                }
                printf("vendor checkbox in a clickable row: checked %d, vendor sink %ld, row clicks \"%ls\"  %s\n",
                       on1 != VARIANT_FALSE, vsink.count, rt ? rt : L"",
                       pf(on1 != VARIANT_FALSE && vsink.count == 1 && (!rt || !*rt)));
                if (!(on1 != VARIANT_FALSE && vsink.count == 1 && (!rt || !*rt))) failures++;
                /* control: a checkbox without a vendor handler still bubbles (SGK's own) */
                PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(47, 12));
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(47, 12));
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(47, 12));
                pump_sleep(800);
                if (row) row->lpVtbl->get_title(row, &rt2);
                printf("  plain checkbox in the row: row clicks \"%ls\"  %s\n", rt2 ? rt2 : L"",
                       pf(rt2 && !lstrcmpW(rt2, L"1")));
                if (!(rt2 && !lstrcmpW(rt2, L"1"))) failures++;
                SysFreeString(rt); SysFreeString(rt2);
                if (c2) {
                    s1 = SysAllocString(L"onclick");
                    c2->lpVtbl->detachEvent(c2, s1, &vsink.IDispatch_iface);
                    SysFreeString(s1);
                    c2->lpVtbl->Release(c2);
                }
                if (row) {   /* remove the whole table */
                    IHTMLElement *tb = NULL, *p1 = NULL;
                    row->lpVtbl->get_parentElement(row, &p1);           /* tbody */
                    if (p1) { p1->lpVtbl->get_parentElement(p1, &tb); p1->lpVtbl->Release(p1); }
                    if (tb) { BSTR e0 = SysAllocString(L""); tb->lpVtbl->put_outerHTML(tb, e0); SysFreeString(e0); tb->lpVtbl->Release(tb); }
                    row->lpVtbl->Release(row);
                }
                if (chk) chk->lpVtbl->Release(chk);
                if (chk2) chk2->lpVtbl->Release(chk2);
            }

            /* Recete Giris: SGK's drug checkboxes (onclick checkBox(this): tick only this one)
             * with TEBEOS's mouseup that clears them when exactly one is ticked -- in IE it runs
             * before the click flips the box, so a click moves the tick; it must not untick */
            {
                IHTMLElement *bx[2] = { NULL, NULL };
                IHTMLElement2 *b2x[2] = { NULL, NULL };
                MockSink msink[2];
                VARIANT_BOOL st[2][2];
                int c, j;
                BSTR where = SysAllocString(L"beforeEnd"), h = SysAllocString(
                    L"<div id='rgbox'>"
                    L"<input type='checkbox' class='rgc' id='rg0' onclick=\"var a=document.querySelectorAll('.rgc');for(var i=0;i<a.length;i++)a[i].checked=false;this.checked=true;\" style='position:fixed;left:5px;top:5px;margin:0;width:16px;height:16px'>"
                    L"<input type='checkbox' class='rgc' id='rg1' onclick=\"var a=document.querySelectorAll('.rgc');for(var i=0;i<a.length;i++)a[i].checked=false;this.checked=true;\" style='position:fixed;left:40px;top:5px;margin:0;width:16px;height:16px'>"
                    L"</div>");
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
                SysFreeString(where); SysFreeString(h);
                for (j = 0; j < 2; j++) {
                    s1 = SysAllocString(j ? L"rg1" : L"rg0"); doc3->lpVtbl->getElementById(doc3, s1, &bx[j]); SysFreeString(s1);
                    sink_init(&msink[j]);
                    msink[j].fn = clear_if_one;
                    msink[j].ctx = bx;
                    if (bx[j] && SUCCEEDED(bx[j]->lpVtbl->QueryInterface(bx[j], &IID_IHTMLElement2, (void **)&b2x[j]))) {
                        s1 = SysAllocString(L"onmouseup");
                        b2x[j]->lpVtbl->attachEvent(b2x[j], s1, &msink[j].IDispatch_iface, &ok);
                        SysFreeString(s1);
                    }
                }
                pump_sleep(500);
                for (c = 0; c < 2; c++) {   /* tick the first, then the second */
                    int x = c ? 47 : 12;
                    LONG before = msink[c].count;
                    PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(x, 12));
                    PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 12));
                    PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(x, 12));
                    for (i2 = 0; i2 < 25 && msink[c].count == before; i2++) pump_sleep(120);
                    pump_sleep(500);
                    for (j = 0; j < 2; j++) {
                        IHTMLInputElement *in = NULL;
                        st[c][j] = VARIANT_FALSE;
                        if (bx[j] && SUCCEEDED(bx[j]->lpVtbl->QueryInterface(bx[j], &IID_IHTMLInputElement, (void **)&in))) {
                            in->lpVtbl->get_checked(in, &st[c][j]);
                            in->lpVtbl->Release(in);
                        }
                    }
                }
                {
                    int good = st[0][0] && !st[0][1] && !st[1][0] && st[1][1] && msink[0].count == 1 && msink[1].count == 1;
                    printf("vendor mouseup before the click: first box %d%d, second box %d%d, mouseups %ld/%ld  %s\n",
                           st[0][0] != 0, st[0][1] != 0, st[1][0] != 0, st[1][1] != 0,
                           msink[0].count, msink[1].count, pf(good));
                    if (!good) failures++;
                }
                for (j = 0; j < 2; j++) {   /* msink lives on this stack frame */
                    if (b2x[j]) {
                        s1 = SysAllocString(L"onmouseup");
                        b2x[j]->lpVtbl->detachEvent(b2x[j], s1, &msink[j].IDispatch_iface);
                        SysFreeString(s1);
                        b2x[j]->lpVtbl->Release(b2x[j]);
                    }
                }
                {
                    IHTMLElement *box = NULL;
                    s1 = SysAllocString(L"rgbox"); doc3->lpVtbl->getElementById(doc3, s1, &box); SysFreeString(s1);
                    if (box) { BSTR e0 = SysAllocString(L""); box->lpVtbl->put_outerHTML(box, e0); SysFreeString(e0); box->lpVtbl->Release(box); }
                }
                for (j = 0; j < 2; j++) if (bx[j]) bx[j]->lpVtbl->Release(bx[j]);
            }

            /* the scrollbar: drag its thumb (the left button held through the moves) */
            {
                IHTMLElement *tall = NULL;
                IHTMLElement2 *b2 = NULL;
                RECT rc;
                LONG top = 0, k2;
                BSTR where = SysAllocString(L"afterBegin"), h = SysAllocString(L"<div id='ktall' style='height:6000px'></div>");
                if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
                SysFreeString(where); SysFreeString(h);
                s1 = SysAllocString(L"ktall"); doc3->lpVtbl->getElementById(doc3, s1, &tall); SysFreeString(s1);
                pump_sleep(800);
                {   /* the scrollbar sits right of the root element's client area (the page's
                     * viewport, which need not be the window's width) */
                    IHTMLElement *de = NULL;
                    LONG cw = 0;
                    if (SUCCEEDED(doc3->lpVtbl->get_documentElement(doc3, &de)) && de &&
                        SUCCEEDED(de->lpVtbl->QueryInterface(de, &IID_IHTMLElement2, (void **)&b2))) {
                        b2->lpVtbl->get_clientWidth(b2, &cw);
                        b2->lpVtbl->Release(b2);
                    }
                    if (de) de->lpVtbl->Release(de);
                    GetClientRect(dw, &rc);
                    rc.right = cw > 0 ? cw + 7 : rc.right - 6;
                }
                PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(rc.right, 20));
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(rc.right, 20));
                pump_sleep(100);
                for (k2 = 30; k2 <= 150; k2 += 15) {   /* at a hand's pace */
                    PostMessageW(dw, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rc.right, k2));
                    pump_sleep(30);
                }
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(rc.right, 150));
                pump_sleep(1000);
                if (body && SUCCEEDED(body->lpVtbl->QueryInterface(body, &IID_IHTMLElement2, (void **)&b2))) {
                    b2->lpVtbl->get_scrollTop(b2, &top);
                    b2->lpVtbl->put_scrollTop(b2, 0);
                    b2->lpVtbl->Release(b2);
                }
                {   /* standards mode scrolls the root element, quirks mode the body */
                    IHTMLElement *de = NULL;
                    LONG t2v = 0;
                    if (SUCCEEDED(doc3->lpVtbl->get_documentElement(doc3, &de)) && de &&
                        SUCCEEDED(de->lpVtbl->QueryInterface(de, &IID_IHTMLElement2, (void **)&b2))) {
                        b2->lpVtbl->get_scrollTop(b2, &t2v);
                        b2->lpVtbl->put_scrollTop(b2, 0);
                        b2->lpVtbl->Release(b2);
                    }
                    if (de) de->lpVtbl->Release(de);
                    if (t2v > top) top = t2v;
                }
                printf("scrollbar thumb dragged 20 -> 150 px: scrollTop %ld  %s\n", (long)top, pf(top > 200));
                if (!(top > 200)) failures++;
                /* a click on the track below the thumb pages once; it auto-repeats only while held */
                pump_sleep(500);
                PostMessageW(dw, WM_MOUSEMOVE, 0, MAKELPARAM(rc.right, 400));
                PostMessageW(dw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(rc.right, 400));
                pump_sleep(80);
                PostMessageW(dw, WM_LBUTTONUP, 0, MAKELPARAM(rc.right, 400));
                pump_sleep(2000);
                top = 0;
                {
                    IHTMLElement *de = NULL;
                    LONG t2v = 0;
                    if (body && SUCCEEDED(body->lpVtbl->QueryInterface(body, &IID_IHTMLElement2, (void **)&b2))) {
                        b2->lpVtbl->get_scrollTop(b2, &top);
                        b2->lpVtbl->put_scrollTop(b2, 0);
                        b2->lpVtbl->Release(b2);
                    }
                    if (SUCCEEDED(doc3->lpVtbl->get_documentElement(doc3, &de)) && de &&
                        SUCCEEDED(de->lpVtbl->QueryInterface(de, &IID_IHTMLElement2, (void **)&b2))) {
                        b2->lpVtbl->get_scrollTop(b2, &t2v);
                        b2->lpVtbl->put_scrollTop(b2, 0);
                        b2->lpVtbl->Release(b2);
                    }
                    if (de) de->lpVtbl->Release(de);
                    if (t2v > top) top = t2v;
                }
                printf("scrollbar track clicked once: scrollTop %ld (a page, not the end)  %s\n", (long)top,
                       pf(top > 100 && top < 1500));
                if (!(top > 100 && top < 1500)) failures++;
                if (tall) { BSTR e0 = SysAllocString(L""); tall->lpVtbl->put_outerHTML(tall, e0); SysFreeString(e0); tall->lpVtbl->Release(tall); }
                pump_sleep(300);
            }
        } else failures++;

        /* the vendor's dispinterface casts.
         * Form1_Model.Remove_Element_By_ID: (DomDocument as HTMLDocument).getElementById(id)
         *   is IHTMLDOMNode n -> n.parentNode.removeChild(n)
         * HtmlElement_Helper.AppendCellToRow: (HTMLTableRow)row.DomElement -> .cells;
         *   (HTMLTableCell)cell -> .id = ... */
        {
            IDispatch *dd = NULL, *rowd = NULL, *celld = NULL;
            IHTMLElement *row = NULL, *gone = NULL;
            VARIANT a, r, r2;
            BSTR s;

            hr = doc->lpVtbl->QueryInterface(doc, &T_DIID_DispHTMLDocument, (void **)&dd);
            printf("QI DispHTMLDocument (DomDocument as HTMLDocument) = %s  %s\n", hx(hr), pf(SUCCEEDED(hr) && dd));
            if (FAILED(hr) || !dd) failures++;
            if (dd) {
                V_VT(&a) = VT_BSTR; V_BSTR(&a) = SysAllocString(L"kt");
                hr = disp_call(dd, DISPID_IHTMLDOCUMENT3_GETELEMENTBYID, DISPATCH_METHOD, &a, 1, &r);
                VariantClear(&a);
                printf("  getElementById(kt) via DISPID %d = %s vt=%d  %s\n", DISPID_IHTMLDOCUMENT3_GETELEMENTBYID,
                       hx(hr), V_VT(&r), pf(SUCCEEDED(hr) && V_VT(&r) == VT_DISPATCH && V_DISPATCH(&r)));
                if (SUCCEEDED(hr) && V_VT(&r) == VT_DISPATCH && V_DISPATCH(&r)) {
                    IHTMLDOMNode *node = NULL, *parent = NULL, *removed = NULL;
                    if (SUCCEEDED(V_DISPATCH(&r)->lpVtbl->QueryInterface(V_DISPATCH(&r), &IID_IHTMLDOMNode, (void **)&node))
                        && SUCCEEDED(node->lpVtbl->get_parentNode(node, &parent)) && parent) {
                        hr = parent->lpVtbl->removeChild(parent, node, &removed);
                        printf("  parentNode.removeChild = %s\n", hx(hr));
                    }
                    if (removed) removed->lpVtbl->Release(removed);
                    if (parent) parent->lpVtbl->Release(parent);
                    if (node) node->lpVtbl->Release(node);
                } else failures++;
                VariantClear(&r);
                s = SysAllocString(L"kt");
                doc3->lpVtbl->getElementById(doc3, s, &gone);
                SysFreeString(s);
                printf("Remove_Element_By_ID(kt): element gone = %s\n", pf(!gone));
                if (gone) { failures++; gone->lpVtbl->Release(gone); }
                dd->lpVtbl->Release(dd);
            }

            s = SysAllocString(L"beforeEnd");
            {
                BSTR html = SysAllocString(L"<table><tr id='kr'><td>a</td><td>b</td></tr></table>");
                if (body) body->lpVtbl->insertAdjacentHTML(body, s, html);
                SysFreeString(html);
            }
            SysFreeString(s);
            s = SysAllocString(L"kr"); doc3->lpVtbl->getElementById(doc3, s, &row); SysFreeString(s);
            if (row && SUCCEEDED(row->lpVtbl->QueryInterface(row, &T_DIID_DispHTMLTableRow, (void **)&rowd))) {
                hr = disp_call(rowd, DISPID_IHTMLTABLEROW_CELLS, DISPATCH_PROPERTYGET, NULL, 0, &r);
                {
                    IHTMLElementCollection *cells = NULL;
                    long len = -1;
                    if (SUCCEEDED(hr) && V_VT(&r) == VT_DISPATCH && V_DISPATCH(&r) &&
                        SUCCEEDED(V_DISPATCH(&r)->lpVtbl->QueryInterface(V_DISPATCH(&r), &IID_IHTMLElementCollection, (void **)&cells))) {
                        VARIANT idx, none;
                        IDispatch *c1 = NULL;
                        cells->lpVtbl->get_length(cells, &len);
                        V_VT(&idx) = VT_I4; V_I4(&idx) = 1; V_VT(&none) = VT_ERROR; V_ERROR(&none) = DISP_E_PARAMNOTFOUND;
                        cells->lpVtbl->item(cells, idx, none, &c1);
                        if (c1 && SUCCEEDED(c1->lpVtbl->QueryInterface(c1, &T_DIID_DispHTMLTableCell, (void **)&celld))) {
                            V_VT(&a) = VT_BSTR; V_BSTR(&a) = SysAllocString(L"kcell");
                            hr = disp_call(celld, DISPID_IHTMLELEMENT_ID, DISPATCH_PROPERTYPUT, &a, 1, NULL);
                            VariantClear(&a);
                            hr = disp_call(celld, DISPID_IHTMLELEMENT_ID, DISPATCH_PROPERTYGET, NULL, 0, &r2);
                            printf("(HTMLTableCell).id = \"kcell\" -> \"%ls\"  %s\n",
                                   V_VT(&r2) == VT_BSTR ? V_BSTR(&r2) : L"?",
                                   pf(V_VT(&r2) == VT_BSTR && !lstrcmpW(V_BSTR(&r2), L"kcell")));
                            if (!(V_VT(&r2) == VT_BSTR && !lstrcmpW(V_BSTR(&r2), L"kcell"))) failures++;
                            VariantClear(&r2);
                            celld->lpVtbl->Release(celld);
                        } else { printf("QI DispHTMLTableCell = FAIL\n"); failures++; }
                        if (c1) c1->lpVtbl->Release(c1);
                        cells->lpVtbl->Release(cells);
                    }
                    printf("(HTMLTableRow).cells.length = %ld  %s\n", len, pf(len == 2));
                    if (len != 2) failures++;
                }
                VariantClear(&r);
                rowd->lpVtbl->Release(rowd);
            } else { printf("QI DispHTMLTableRow = FAIL\n"); failures++; }
            if (row) row->lpVtbl->Release(row);
        }

        if (t2) t2->lpVtbl->Release(t2);
        if (f2) f2->lpVtbl->Release(f2);
        if (t) t->lpVtbl->Release(t);
        if (kb) kb->lpVtbl->Release(kb);
        if (kf) kf->lpVtbl->Release(kf);
        if (ks) ks->lpVtbl->Release(ks);
        if (body) body->lpVtbl->Release(body);
    }

    printf("%s\n", failures == 0 ? "PASS (attachEvent -> DOM event -> CDP binding -> sink Invoke(0))" : "FAIL");

    fprintf(stderr, "cleanup: Close...\n");
    if (ole) ole->lpVtbl->Close(ole, OLECLOSE_NOSAVE);
    fprintf(stderr, "cleanup: Close done, releases...\n");
    if (pm)   pm->lpVtbl->Release(pm);
    if (mk)   mk->lpVtbl->Release(mk);
    if (doc3) doc3->lpVtbl->Release(doc3);
    if (ole)  ole->lpVtbl->Release(ole);
    if (doc)  doc->lpVtbl->Release(doc);
    if (cf)   cf->lpVtbl->Release(cf);
    fprintf(stderr, "cleanup: releases done, CoUninit...\n");
    if (disco) disco();
    CoUninitialize();
    fprintf(stderr, "cleanup: done\n");
    fflush(NULL);
    ExitProcess(failures == 0 ? 0 : 1);
}
