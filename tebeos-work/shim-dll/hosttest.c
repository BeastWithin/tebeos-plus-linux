/* hosttest.c -- minimal OLE DocObject *container* that hosts CLSID_HTMLDocument from the
 * shim DLL, mirroring exactly what Wine's ieframe (client.c + dochost.c) does:
 *
 *   GetUserClassID -> SetClientSite -> IPersistMoniker::Load -> DoVerb(SHOW)
 *     -> [document QIs client for IOleDocumentSite::ActivateMe]
 *     -> ActivateMe: IOleDocument::CreateView -> SetRect -> Show(TRUE)
 *     -> IOleDocumentView::UIActivate(TRUE)
 *     -> verify a window was created (IOleInPlaceObject::GetWindow -> valid HWND).
 *
 * Run:  wine hosttest32.exe shim32.dll   (or hosttest64.exe shim64.dll)
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

typedef struct Host Host;

struct Host {
    IOleClientSite     IOleClientSite_iface;
    IOleDocumentSite   IOleDocumentSite_iface;
    IOleInPlaceSiteEx  IOleInPlaceSiteEx_iface;   /* serves IOleInPlaceSite too */
    IOleInPlaceFrame   IOleInPlaceFrame_iface;    /* serves IOleInPlaceUIWindow too */
    LONG ref;
    HWND hwnd;
    IUnknown *document;
    IOleDocumentView *view;
    BOOL set_client_site_called;
    BOOL activate_me_called;
    BOOL on_inplace_activate_called;
    IOleInPlaceActiveObject *active_object;
};

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);

static const char *hx(HRESULT h) { static char b[16]; snprintf(b,sizeof b,"0x%08lx",(unsigned long)h); return b; }

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

/* ---- IOleClientSite ---- */
static HRESULT STDMETHODCALLTYPE CS_QI(IOleClientSite *t, REFIID r, void **p) { return host_qi(host_from_client(t), r, p); }
static ULONG STDMETHODCALLTYPE CS_AddRef(IOleClientSite *t) { return host_addref(host_from_client(t)); }
static ULONG STDMETHODCALLTYPE CS_Release(IOleClientSite *t) { return host_release(host_from_client(t)); }
static HRESULT STDMETHODCALLTYPE CS_SaveObject(IOleClientSite *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_GetMoniker(IOleClientSite *t, DWORD a, DWORD b, IMoniker **m) { (void)t;(void)a;(void)b; if(m)*m=NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE CS_GetContainer(IOleClientSite *t, IOleContainer **c) { (void)t; if(c)*c=NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE CS_ShowObject(IOleClientSite *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_OnShowWindow(IOleClientSite *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_RequestNewObjectLayout(IOleClientSite *t) { (void)t; return E_NOTIMPL; }

static IOleClientSiteVtbl CS_vtbl = {
    CS_QI, CS_AddRef, CS_Release, CS_SaveObject, CS_GetMoniker, CS_GetContainer,
    CS_ShowObject, CS_OnShowWindow, CS_RequestNewObjectLayout,
};

/* ---- IOleDocumentSite ---- */
static HRESULT STDMETHODCALLTYPE DS_QI(IOleDocumentSite *t, REFIID r, void **p) { return host_qi(host_from_docsite(t), r, p); }
static ULONG STDMETHODCALLTYPE DS_AddRef(IOleDocumentSite *t) { return host_addref(host_from_docsite(t)); }
static ULONG STDMETHODCALLTYPE DS_Release(IOleDocumentSite *t) { return host_release(host_from_docsite(t)); }
static HRESULT STDMETHODCALLTYPE DS_ActivateMe(IOleDocumentSite *t, IOleDocumentView *pView)
{
    Host *h = host_from_docsite(t);
    IOleDocument *oledoc;
    RECT rc;
    HRESULT hr;

    printf("  [host] ActivateMe called (view=%p)\n", (void *)pView);
    h->activate_me_called = TRUE;

    hr = h->document->lpVtbl->QueryInterface(h->document, &IID_IOleDocument, (void **)&oledoc);
    if (FAILED(hr)) { printf("  [host] Can't get IOleDocument: %s\n", hx(hr)); return hr; }
    hr = oledoc->lpVtbl->CreateView(oledoc, (IOleInPlaceSite *)&h->IOleInPlaceSiteEx_iface,
                                    NULL, 0, &h->view);
    oledoc->lpVtbl->Release(oledoc);
    if (FAILED(hr)) { printf("  [host] CreateView failed: %s\n", hx(hr)); return hr; }
    printf("  [host] CreateView -> view %p\n", (void *)h->view);

    GetClientRect(h->hwnd, &rc);
    h->view->lpVtbl->SetRect(h->view, &rc);
    hr = h->view->lpVtbl->Show(h->view, TRUE);
    printf("  [host] Show(TRUE) = %s\n", hx(hr));
    return hr;
}
static IOleDocumentSiteVtbl DS_vtbl = { DS_QI, DS_AddRef, DS_Release, DS_ActivateMe };

/* ---- IOleInPlaceSiteEx (also IOleInPlaceSite) ---- */
static HRESULT STDMETHODCALLTYPE IPS_QI(IOleInPlaceSiteEx *t, REFIID r, void **p) { return host_qi(host_from_ips(t), r, p); }
static ULONG STDMETHODCALLTYPE IPS_AddRef(IOleInPlaceSiteEx *t) { return host_addref(host_from_ips(t)); }
static ULONG STDMETHODCALLTYPE IPS_Release(IOleInPlaceSiteEx *t) { return host_release(host_from_ips(t)); }
static HRESULT STDMETHODCALLTYPE IPS_GetWindow(IOleInPlaceSiteEx *t, HWND *w) { if(w)*w=host_from_ips(t)->hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_ContextSensitiveHelp(IOleInPlaceSiteEx *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_CanInPlaceActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceActivate(IOleInPlaceSiteEx *t) { host_from_ips(t)->on_inplace_activate_called = TRUE; return S_OK; }
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
static HRESULT STDMETHODCALLTYPE IPS_Scroll(IOleInPlaceSiteEx *t, SIZE s) { (void)t;(void)s; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_OnUIDeactivate(IOleInPlaceSiteEx *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceDeactivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_DiscardUndoState(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_DeactivateAndUndo(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnPosRectChange(IOleInPlaceSiteEx *t, LPCRECT r) { (void)t;(void)r; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceActivateEx(IOleInPlaceSiteEx *t, BOOL *pnr, DWORD f) { (void)t;(void)f; if(pnr)*pnr=FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnInPlaceDeactivateEx(IOleInPlaceSiteEx *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_RequestUIActivate(IOleInPlaceSiteEx *t) { (void)t; return S_OK; }

static IOleInPlaceSiteExVtbl IPS_vtbl = {
    IPS_QI, IPS_AddRef, IPS_Release, IPS_GetWindow, IPS_ContextSensitiveHelp,
    IPS_CanInPlaceActivate, IPS_OnInPlaceActivate, IPS_OnUIActivate, IPS_GetWindowContext,
    IPS_Scroll, IPS_OnUIDeactivate, IPS_OnInPlaceDeactivate, IPS_DiscardUndoState,
    IPS_DeactivateAndUndo, IPS_OnPosRectChange,
    IPS_OnInPlaceActivateEx, IPS_OnInPlaceDeactivateEx, IPS_RequestUIActivate,
};

/* ---- IOleInPlaceFrame (also IOleInPlaceUIWindow) ---- */
static HRESULT STDMETHODCALLTYPE FR_QI(IOleInPlaceFrame *t, REFIID r, void **p) { return host_qi(host_from_frame(t), r, p); }
static ULONG STDMETHODCALLTYPE FR_AddRef(IOleInPlaceFrame *t) { return host_addref(host_from_frame(t)); }
static ULONG STDMETHODCALLTYPE FR_Release(IOleInPlaceFrame *t) { return host_release(host_from_frame(t)); }
static HRESULT STDMETHODCALLTYPE FR_GetWindow(IOleInPlaceFrame *t, HWND *w) { if(w)*w=host_from_frame(t)->hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_ContextSensitiveHelp(IOleInPlaceFrame *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_GetBorder(IOleInPlaceFrame *t, LPRECT r) { (void)t; if(r)SetRectEmpty(r); return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_RequestBorderSpace(IOleInPlaceFrame *t, LPCBORDERWIDTHS b) { (void)t;(void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetBorderSpace(IOleInPlaceFrame *t, LPCBORDERWIDTHS b) { (void)t;(void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetActiveObject(IOleInPlaceFrame *t, IOleInPlaceActiveObject *a, LPCOLESTR n)
{ (void)n; host_from_frame(t)->active_object = a; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_InsertMenus(IOleInPlaceFrame *t, HMENU m, LPOLEMENUGROUPWIDTHS w) { (void)t;(void)m;(void)w; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetMenu(IOleInPlaceFrame *t, HMENU m, HOLEMENU o, HWND w) { (void)t;(void)m;(void)o;(void)w; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_RemoveMenus(IOleInPlaceFrame *t, HMENU m) { (void)t;(void)m; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_SetStatusText(IOleInPlaceFrame *t, LPCOLESTR s) { (void)t;(void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_EnableModeless(IOleInPlaceFrame *t, BOOL f) { (void)t;(void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE FR_TranslateAccelerator(IOleInPlaceFrame *t, LPMSG m, WORD w) { (void)t;(void)m;(void)w; return S_FALSE; }

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

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "shim32.dll";
    WNDCLASSEXW wc; HWND hwnd; Host host; HINSTANCE hinst = GetModuleHandleW(NULL);
    HMODULE mod; DllGetClassObject_fn getco; IClassFactory *cf = NULL;
    IHTMLDocument2 *doc = NULL; IOleObject *ole = NULL; IPersistMoniker *pm = NULL;
    IMoniker *mk = NULL; HRESULT hr; int failures = 0;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndproc; wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"HostTestWnd";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, L"HostTestWnd", L"host", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, hinst, NULL);

    CoInitialize(NULL);
    host_init(&host, hwnd);

    mod = LoadLibraryA(dll);
    if (!mod) { printf("FAIL: LoadLibrary(%s)\n", dll); return 1; }
    getco = (DllGetClassObject_fn)(void *)GetProcAddress(mod, "DllGetClassObject");
    hr = getco(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&cf);
    printf("DllGetClassObject = %s\n", hx(hr));
    if (FAILED(hr)) return 1;

    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IHTMLDocument2, (void **)&doc);
    printf("CreateInstance(IHTMLDocument2) = %s\n", hx(hr));
    if (FAILED(hr)) return 1;
    host.document = (IUnknown *)doc;

    hr = doc->lpVtbl->QueryInterface(doc, &IID_IOleObject, (void **)&ole);
    printf("QI(IOleObject) = %s\n", hx(hr));

    { CLSID clsid; hr = ole->lpVtbl->GetUserClassID(ole, &clsid);
      printf("GetUserClassID = %s (%s)\n", hx(hr),
             IsEqualGUID(&clsid, &CLSID_HTMLDocument) ? "CLSID_HTMLDocument" : "???");
      if (!IsEqualGUID(&clsid, &CLSID_HTMLDocument)) failures++; }

    hr = ole->lpVtbl->SetClientSite(ole, &host.IOleClientSite_iface);
    printf("SetClientSite = %s\n", hx(hr));

    /* Load a URL (so get_URL round-trips, as in ieframe's navigation). */
    hr = doc->lpVtbl->QueryInterface(doc, &IID_IPersistMoniker, (void **)&pm);
    if (SUCCEEDED(hr)) {
        CreateURLMoniker(NULL, L"about:blank", &mk);
        hr = pm->lpVtbl->Load(pm, TRUE, mk, NULL, 0);
        printf("Load(about:blank) = %s\n", hx(hr));
    }

    hr = ole->lpVtbl->DoVerb(ole, OLEIVERB_SHOW, NULL, NULL, 0, 0, NULL);
    printf("DoVerb(SHOW) = %s\n", hx(hr));
    if (FAILED(hr)) failures++;
    if (!host.activate_me_called) { printf("FAIL: ActivateMe was not called\n"); failures++; }

    if (host.view) {
        hr = host.view->lpVtbl->UIActivate(host.view, TRUE);
        printf("UIActivate(TRUE) = %s\n", hx(hr));
    }

    /* Verify a render window exists. */
    {
        IOleInPlaceObject *ipo = NULL;
        HWND w = NULL;
        hr = doc->lpVtbl->QueryInterface(doc, &IID_IOleInPlaceObject, (void **)&ipo);
        if (SUCCEEDED(hr)) {
            hr = ipo->lpVtbl->GetWindow(ipo, &w);
            printf("IOleInPlaceObject::GetWindow = %s, hwnd=%p, IsWindow=%d\n",
                   hx(hr), (void *)w, w ? IsWindow(w) : 0);
            if (!w || !IsWindow(w)) failures++;
            ipo->lpVtbl->Release(ipo);
        }
    }

    printf("%s\n", failures ? "FAIL" : "PASS (DocObject hosting)");
    CoUninitialize();
    return failures ? 1 : 0;
}
