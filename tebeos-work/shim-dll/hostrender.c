/* hostrender.c -- Milestone 6 integration test: the document now hosts its own CDP
 * renderer + input forwarding. Hosts CLSID_HTMLDocument (exactly as hosttest.c), but
 * also connects the DOM + render CDP channels to a real Chromium, then verifies:
 *
 *   1. IPersistMoniker::Load navigates the engine (page loads).
 *   2. Continuous render started during in-place activation -> at least one frame
 *      decoded (render_frame_info seq advances).
 *   3. Input forwarding: a WM_LBUTTONDOWN/UP on the render window is dispatched to the
 *      page, and the page's onclick fires (read back via the document's own DOM surface).
 *
 * Usage:  wine hostrender32.exe shim32.dll [port]
 */

#include <windows.h>
#include <initguid.h>
#include <ole2.h>
#include <mshtml.h>
#include <oleidl.h>
#include <docobj.h>
#include <urlmon.h>
#include <ocidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef int     (__cdecl *shim_connect_fn)(int port);
typedef int     (__cdecl *frame_info_fn)(IHTMLDocument2 *, int *, int *, long *);
typedef void    (__cdecl *disconnect_fn)(void);

/* Test page: a fixed-position button whose onclick mutates #out, which we read back. */
static const char PAGE_URL[] =
    "data:text/html;base64,"
    "PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHk+PGJ1dHRvbiBpZD0iYiIgc3R5bGU9InBvc2l0aW9uOmZpeGVkO2xlZnQ6MTBweDt0b3A6NjBweDt3aWR0aDoxMjBweDtoZWlnaHQ6NDBweCIgb25jbGljaz0iZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ291dCcpLmlubmVyVGV4dD0nY2xpY2tlZCciPkI8L2J1dHRvbj48ZGl2IGlkPSJvdXQiPm5vbmU8L2Rpdj48L2JvZHk+PC9odG1sPg==";

static const char *hx(HRESULT h) { static char b[16]; snprintf(b, sizeof b, "0x%08lx", (unsigned long)h); return b; }
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

/* ---- minimal OLE DocObject container (identical to hosttest.c) ---- */
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

static void host_init(Host *h, HWND hwnd)
{
    ZeroMemory(h, sizeof(*h));
    h->IOleClientSite_iface.lpVtbl = &CS_vtbl;
    h->IOleDocumentSite_iface.lpVtbl = &DS_vtbl;
    h->IOleInPlaceSiteEx_iface.lpVtbl = &IPS_vtbl;
    h->IOleInPlaceFrame_iface.lpVtbl = &FR_vtbl;
    h->ref = 1;
    h->hwnd = hwnd;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { (void)m; (void)w; (void)l; return DefWindowProcW(h, m, w, l); }

/* BSTR -> UTF-8 (malloc) */
static char *bstr_c(BSTR b)
{
    int n; char *o;
    if (!b) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, b, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    o = (char *)malloc((size_t)n + 1);
    if (!o) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, b, -1, o, n, NULL, NULL);
    return o;
}

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

    /* 1. connect both CDP channels to the real Chromium target. */
    printf("shim_dom_connect(%d)    = %s\n", port, pf(domc(port) == 0));
    printf("shim_render_connect(%d) = %s\n", port, pf(rendc(port) == 0));
    if (domc(port) != 0 || rendc(port) != 0) { failures++; }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndproc; wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"HostRenderWnd";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, L"HostRenderWnd", L"host", WS_OVERLAPPEDWINDOW,
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

    /* 2. Load navigates the engine (via the DOM channel). */
    hr = doc->lpVtbl->QueryInterface(doc, &IID_IPersistMoniker, (void **)&pm);
    {
        OLECHAR wurl[2048];
        MultiByteToWideChar(CP_UTF8, 0, PAGE_URL, -1, wurl, 2048);
        CreateURLMoniker(NULL, wurl, &mk);
        hr = pm->lpVtbl->Load(pm, TRUE, mk, NULL, 0);
        printf("Load(data URL) = %s\n", hx(hr));
    }

    /* 3. in-place activation -> renderer starts. */
    hr = ole->lpVtbl->DoVerb(ole, OLEIVERB_SHOW, NULL, NULL, 0, 0, NULL);
    printf("DoVerb(SHOW) = %s\n", hx(hr));
    if (host.view)
        host.view->lpVtbl->UIActivate(host.view, TRUE);

    /* 4. verify a render window exists and the pump decoded at least one frame. */
    {
        IOleInPlaceObject *ipo = NULL; HWND w = NULL;
        long seq = -1; int fw = 0, fh = 0; int got_frame = 0;
        hr = doc->lpVtbl->QueryInterface(doc, &IID_IOleInPlaceObject, (void **)&ipo);
        if (SUCCEEDED(hr)) { ipo->lpVtbl->GetWindow(ipo, &w); ipo->lpVtbl->Release(ipo); }

        for (i = 0; i < 50; i++) {
            if (finfo(doc, &fw, &fh, &seq) == 0 && seq > 0) { got_frame = 1; break; }
            pump_sleep(120);
        }
        printf("render frame: seq=%ld %dx%d (window=%p IsWindow=%d)\n",
               seq, fw, fh, (void *)w, w ? IsWindow(w) : 0);
        if (!got_frame) { printf("FAIL: no rendered frame decoded\n"); failures++; }
        if (!w || !IsWindow(w)) { printf("FAIL: no valid render window\n"); failures++; }

        /* 5. input forwarding: click the button at (70,80) through the render window. */
        if (w && IsWindow(w)) {
            LPARAM pos = MAKELPARAM(70, 80);
            SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, pos);
            SendMessageW(w, WM_LBUTTONUP, 0, pos);
            pump_sleep(500);

            /* 6. read #out back via the document's own DOM surface. */
            {
                BSTR id = SysAllocString(L"out");
                IHTMLElement *e = NULL;
                if (doc3 && SUCCEEDED(doc3->lpVtbl->getElementById(doc3, id, &e)) && e) {
                    BSTR t = NULL;
                    char *s;
                    e->lpVtbl->get_innerText(e, &t);
                    s = bstr_c(t);
                    printf("after click, #out innerText = '%s'  %s\n",
                           s ? s : "(null)", pf(s && strcmp(s, "clicked") == 0));
                    if (!s || strcmp(s, "clicked") != 0) failures++;
                    if (s) free(s);
                    if (t) SysFreeString(t);
                    e->lpVtbl->Release(e);
                } else {
                    printf("FAIL: getElementById(out) failed\n");
                    failures++;
                }
                SysFreeString(id);
            }
        }
    }

    printf("%s\n", failures == 0 ? "PASS (host + render + input integration)" : "FAIL");

    /* Clean shutdown: Close() deactivates the document, which stops + joins the render
     * pump; without this the worker thread outlives process teardown and page-faults. */
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
    /* Note: a normal `return` from main() hangs under Wine *after* the CDP sockets were
     * used (the CRT exit path blocks). ExitProcess() skips CRT/DllMain teardown and lets
     * the harness terminate cleanly. This is a test-harness quirk, not a shim defect; the
     * production mshtml install owns its exit via tebeos_medula.exe's WinForms loop. */
    ExitProcess(failures == 0 ? 0 : 1);
}
