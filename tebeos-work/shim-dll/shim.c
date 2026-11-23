/* shim.c -- COM server for CLSID_HTMLDocument: a drop-in for Wine's Gecko-backed mshtml in
 * which every document is a headless Chromium tab (page.c).
 *
 * This file: the document's lifetime and interfaces, and loading (IPersistMoniker for a
 * navigation urlmon started, IPersistStreamInit for WebBrowser.DocumentText).
 *
 * Build: build.sh. The generated stubs.c provides all vtables + per-interface
 * QueryInterface/AddRef/Release wrappers that call html_doc_qi/addref/release here.
 */

#include "html_doc.h"
#include "stubs.h"
#include "page.h"
#include "log.h"
#include "util.h"
#include "dispmap.h"
#include <stdlib.h>
#include <string.h>

HINSTANCE shim_hinst = NULL;

/* ------------------------------------------------------------------ shared helpers */

HRESULT html_doc_qi(HTMLDocument *d, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &IID_IHTMLDocument) ||
        IsEqualIID(riid, &IID_IHTMLDocument2))
        *ppv = &d->IHTMLDocument2_iface;
    else if (IsEqualIID(riid, &IID_IHTMLDocument3))
        *ppv = &d->IHTMLDocument3_iface;
    else if (IsEqualIID(riid, &IID_IHTMLDocument4))
        *ppv = &d->IHTMLDocument4_iface;
    else if (IsEqualIID(riid, &IID_IHTMLDocument5))
        *ppv = &d->IHTMLDocument5_iface;
    else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistMoniker))
        *ppv = &d->IPersistMoniker_iface;
    else if (IsEqualIID(riid, &IID_IPersistStreamInit))
        *ppv = &d->IPersistStreamInit_iface;
    else if (IsEqualIID(riid, &IID_IOleObject))
        *ppv = &d->IOleObject_iface;
    else if (IsEqualIID(riid, &IID_IOleDocument))
        *ppv = &d->IOleDocument_iface;
    else if (IsEqualIID(riid, &IID_IOleDocumentView))
        *ppv = &d->IOleDocumentView_iface;
    else if (IsEqualIID(riid, &IID_IOleInPlaceObject))
        *ppv = &d->IOleInPlaceObject_iface;
    else if (IsEqualIID(riid, &IID_IOleWindow) || IsEqualIID(riid, &IID_IOleInPlaceActiveObject))
        *ppv = &d->IOleInPlaceActiveObject_iface;
    else if (IsEqualIID(riid, &IID_IOleCommandTarget))
        *ppv = &d->IOleCommandTarget_iface;
    else if (IsEqualIID(riid, &IID_IProvideClassInfo))
        *ppv = &d->IProvideClassInfo_iface;
    else if (IsEqualIID(riid, &IID_IConnectionPointContainer))
        *ppv = &d->IConnectionPointContainer_iface;
    else if (SUCCEEDED(dispwrap_query(DISP_DOC, (IDispatch *)&d->IHTMLDocument2_iface, riid, ppv)))
        return S_OK;    /* DispHTMLDocument: `DomDocument as HTMLDocument` (Form1_Model) */
    else {
        *ppv = NULL;
        shim_qi_miss("document", riid);
        return E_NOINTERFACE;
    }
    html_doc_addref(d);
    return S_OK;
}

ULONG html_doc_addref(HTMLDocument *d)
{
    return InterlockedIncrement(&d->ref);
}

ULONG html_doc_release(HTMLDocument *d)
{
    LONG ref = InterlockedDecrement(&d->ref);
    if (ref == 0) {
        int i;
        /* keep the document alive while it tears down (sinks may call back) */
        d->ref = 1;
        html_doc_render_stop(d);
        if (d->hwnd)
            DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        if (d->page) {
            page_close(d->page);
            page_release(d->page);
            d->page = NULL;
        }
        html_doc_release_site(d);
        for (i = 0; i < DOC_MAX_PROP_SINKS; i++)
            if (d->prop_sinks[i])
                IPropertyNotifySink_Release(d->prop_sinks[i]);
        if (d->ipsite)
            IOleInPlaceSite_Release(d->ipsite);
        if (d->inplace_frame)
            IOleInPlaceFrame_Release(d->inplace_frame);
        if (d->inplace_ui)
            IOleInPlaceUIWindow_Release(d->inplace_ui);
        SysFreeString(d->url);
        SysFreeString(d->title);
        SysFreeString(d->ready_state);
        shim_log("DOC %p released", (void *)d);
        HeapFree(GetProcessHeap(), 0, d);
    }
    return ref;
}

struct Page *doc_page(HTMLDocument *d)
{
    return d->page ? d->page : page_legacy();
}

/* ------------------------------------------------------------------ document state */

static BSTR dup_bstr_or_empty(BSTR s)
{
    return s ? SysAllocString(s) : SysAllocStringLen(L"", 0);
}

HRESULT STDMETHODCALLTYPE html_doc_get_readyState(IHTMLDocument2 *This, BSTR *p)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IHTMLDocument2_iface);
    *p = dup_bstr_or_empty(d->ready_state);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_get_URL(IHTMLDocument2 *This, BSTR *p)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IHTMLDocument2_iface);
    *p = SysAllocString(d->url ? d->url : L"about:blank");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_persist_moniker_GetClassID(IPersistMoniker *This, CLSID *pClassID)
{
    (void)This;
    *pClassID = CLSID_HTMLDocument;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_persist_moniker_IsDirty(IPersistMoniker *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistMoniker_iface);
    return d->dirty ? S_OK : S_FALSE;
}

/* The POST body and extra headers of the navigation urlmon is binding: ieframe's bind status
 * callback holds them (Wine's mshtml reads them the same way, navigate.c). */
static void bindctx_request(IBindCtx *pibc, LPCWSTR url, BYTE **post, DWORD *postlen, char **headers)
{
    IUnknown *unk = NULL;
    IBindStatusCallback *bsc = NULL;
    IHttpNegotiate *neg = NULL;
    BINDINFO bi;
    DWORD bindf = 0;

    *post = NULL;
    *postlen = 0;
    *headers = NULL;
    if (!pibc || FAILED(IBindCtx_GetObjectParam(pibc, (LPOLESTR)L"_BSCB_Holder_", &unk)) || !unk)
        return;
    if (FAILED(IUnknown_QueryInterface(unk, &IID_IBindStatusCallback, (void **)&bsc))) {
        IUnknown_Release(unk);
        return;
    }
    memset(&bi, 0, sizeof(bi));
    bi.cbSize = sizeof(bi);
    if (SUCCEEDED(IBindStatusCallback_GetBindInfo(bsc, &bindf, &bi))) {
        if (bi.dwBindVerb == BINDVERB_POST && bi.stgmedData.tymed == TYMED_HGLOBAL &&
            bi.stgmedData.hGlobal && bi.cbstgmedData) {
            BYTE *src = (BYTE *)GlobalLock(bi.stgmedData.hGlobal);
            if (src && (*post = (BYTE *)malloc(bi.cbstgmedData))) {
                memcpy(*post, src, bi.cbstgmedData);
                *postlen = bi.cbstgmedData;
            }
            if (src)
                GlobalUnlock(bi.stgmedData.hGlobal);
        }
        ReleaseBindInfo(&bi);
    }
    if (*postlen && SUCCEEDED(IBindStatusCallback_QueryInterface(bsc, &IID_IHttpNegotiate, (void **)&neg))) {
        LPWSTR add = NULL;
        if (SUCCEEDED(IHttpNegotiate_BeginningTransaction(neg, url, L"", 0, &add)) && add) {
            *headers = wstr_to_utf8(add);
            CoTaskMemFree(add);
        }
        IHttpNegotiate_Release(neg);
    }
    IBindStatusCallback_Release(bsc);
    IUnknown_Release(unk);
}

/* urlmon hands a new document its URL here (ieframe has fired BeforeNavigate2 already).
 * The document gets its own Chromium tab and navigates it; urlmon's own download of the
 * same URL is not used (the request is made twice). */
HRESULT STDMETHODCALLTYPE html_doc_persist_moniker_Load(
    IPersistMoniker *This, BOOL fFullyAvailable, IMoniker *pimkName, LPBC pibc, DWORD grfMode)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistMoniker_iface);
    LPOLESTR display = NULL;
    BYTE *post = NULL;
    DWORD postlen = 0;
    char *u8, *headers = NULL;
    HRESULT hr;

    (void)fFullyAvailable; (void)grfMode;
    if (!pimkName)
        return E_INVALIDARG;
    hr = IMoniker_GetDisplayName(pimkName, pibc, NULL, &display);
    if (FAILED(hr))
        return hr;
    u8 = wstr_to_utf8(display);
    html_doc_set_url(d, display);
    html_doc_set_ready_state(d, L"loading");
    shim_log("DOC %p Load %s", (void *)d, u8 ? u8 : "?");

    if (!d->page && page_legacy()) {
        /* test harness: its own tab, navigated synchronously */
        shim_dom_navigate(u8 ? u8 : "about:blank");
        html_doc_set_ready_state(d, L"complete");
        hr = S_OK;
    } else {
        if (!d->page)
            d->page = page_create(d);
        if (d->page) {
            bindctx_request(pibc, display, &post, &postlen, &headers);
            page_navigate(d->page, u8 ? u8 : "about:blank", post, postlen, headers);
            hr = S_OK;
        } else {
            shim_log("DOC %p: no Chromium page; the document stays empty", (void *)d);
            hr = E_FAIL;
        }
    }
    html_doc_window_ready(d);
    free(post);
    free(headers);
    free(u8);
    CoTaskMemFree(display);
    return hr;
}

HRESULT STDMETHODCALLTYPE html_doc_persist_stream_GetClassID(IPersistStreamInit *This, CLSID *pClassID)
{
    (void)This;
    *pClassID = CLSID_HTMLDocument;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_persist_stream_IsDirty(IPersistStreamInit *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistStreamInit_iface);
    return d->dirty ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE html_doc_persist_init_new(IPersistStreamInit *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistStreamInit_iface);
    d->dirty = FALSE;
    return S_OK;
}

/* The whole stream as UTF-8 text: a UTF-16 or UTF-8 BOM decides, else valid UTF-8 is taken
 * as is and anything else is read in the ANSI code page. */
static char *stream_text(IStream *stm)
{
    size_t cap = 65536, len = 0;
    BYTE *buf = (BYTE *)malloc(cap + 2);
    char *out = NULL;
    ULONG got;

    if (!buf)
        return NULL;
    for (;;) {
        if (len == cap) {
            BYTE *nb = (BYTE *)realloc(buf, cap * 2 + 2);
            if (!nb)
                break;
            buf = nb;
            cap *= 2;
        }
        got = 0;
        if (FAILED(IStream_Read(stm, buf + len, (ULONG)(cap - len), &got)) || !got)
            break;
        len += got;
    }
    buf[len] = buf[len + 1] = 0;
    if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        out = wstr_to_utf8((WCHAR *)(buf + 2));
    } else if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        out = dup_utf8((char *)buf + 3);
    } else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)buf, (int)len, NULL, 0) > 0 || !len) {
        out = dup_utf8((char *)buf);
    } else {
        int n = MultiByteToWideChar(CP_ACP, 0, (char *)buf, (int)len, NULL, 0);
        WCHAR *w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
        if (w) {
            MultiByteToWideChar(CP_ACP, 0, (char *)buf, (int)len, w, n);
            w[n] = 0;
            out = wstr_to_utf8(w);
            free(w);
        }
    }
    free(buf);
    return out;
}

/* WebBrowser.DocumentText = ...: WinForms navigates to about:blank, then loads the HTML from
 * a stream when that DocumentComplete arrives, and waits for another DocumentComplete. */
HRESULT STDMETHODCALLTYPE html_doc_persist_stream_Load(IPersistStreamInit *This, LPSTREAM pStm)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistStreamInit_iface);
    char *html;
    Page *pg;

    if (!pStm)
        return E_INVALIDARG;
    html = stream_text(pStm);
    if (!html)
        return E_OUTOFMEMORY;
    if (!(pg = doc_page(d)) && (d->page = page_create(d)))
        pg = d->page;
    shim_log("DOC %p stream Load (%u bytes)", (void *)d, (unsigned)strlen(html));
    html_doc_set_ready_state(d, L"loading");
    if (!pg || page_set_content(pg, html) != 0 || !d->page)
        html_doc_set_ready_state(d, L"complete");   /* no page, or the harness page: done now */
    html_doc_window_ready(d);
    free(html);
    return S_OK;
}

/* WebBrowser.DocumentText (get): the page as the server sent it, in its own charset. */
HRESULT STDMETHODCALLTYPE html_doc_persist_stream_Save(IPersistStreamInit *This, LPSTREAM pStm, BOOL fClearDirty)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IPersistStreamInit_iface);
    Page *pg = doc_page(d);

    if (!pStm)
        return E_INVALIDARG;
    if (fClearDirty)
        d->dirty = FALSE;
    return pg ? page_save(pg, pStm) : S_OK;
}

/* ------------------------------------------------------------------ ClassFactory */

typedef struct ClassFactory {
    IClassFactory IClassFactory_iface;
    LONG ref;
} ClassFactory;

static inline ClassFactory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, ClassFactory, IClassFactory_iface);
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface);
static ULONG WINAPI ClassFactory_Release(IClassFactory *iface);

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = iface;
        ClassFactory_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    ClassFactory *cf = impl_from_IClassFactory(iface);
    return InterlockedIncrement(&cf->ref);
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    ClassFactory *cf = impl_from_IClassFactory(iface);
    LONG ref = InterlockedDecrement(&cf->ref);
    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, cf);
    return ref;
}

static HRESULT WINAPI ClassFactory_CreateInstance(
    IClassFactory *iface, IUnknown *pUnkOuter, REFIID riid, void **ppv)
{
    HTMLDocument *d;
    HRESULT hr;

    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d)
        return E_OUTOFMEMORY;
    d->IHTMLDocument2_iface.lpVtbl = &g_IHTMLDocument2_vtbl;
    d->IHTMLDocument3_iface.lpVtbl = &g_IHTMLDocument3_vtbl;
    d->IHTMLDocument4_iface.lpVtbl = &g_IHTMLDocument4_vtbl;
    d->IHTMLDocument5_iface.lpVtbl = &g_IHTMLDocument5_vtbl;
    d->IConnectionPointContainer_iface.lpVtbl = &g_IConnectionPointContainer_vtbl;
    d->IPersistMoniker_iface.lpVtbl = &g_IPersistMoniker_vtbl;
    d->IPersistStreamInit_iface.lpVtbl = &g_IPersistStreamInit_vtbl;
    d->IOleObject_iface.lpVtbl = &g_IOleObject_vtbl;
    d->IOleDocument_iface.lpVtbl = &g_IOleDocument_vtbl;
    d->IOleDocumentView_iface.lpVtbl = &g_IOleDocumentView_vtbl;
    d->IOleInPlaceObject_iface.lpVtbl = &g_IOleInPlaceObject_vtbl;
    d->IOleInPlaceActiveObject_iface.lpVtbl = &g_IOleInPlaceActiveObject_vtbl;
    d->IOleCommandTarget_iface.lpVtbl = &g_IOleCommandTarget_vtbl;
    d->IProvideClassInfo_iface.lpVtbl = &g_IProvideClassInfo_vtbl;
    d->ref = 1;
    d->url = NULL;
    d->title = NULL;
    d->ready_state = SysAllocString(L"uninitialized");
    d->dirty = FALSE;
    html_doc_init_window(d);
    shim_log("DOC %p created", (void *)d);

    hr = html_doc_qi(d, riid, ppv);
    html_doc_release(d);
    return hr;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    (void)iface; (void)fLock;
    return S_OK;
}

static IClassFactoryVtbl ClassFactoryVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    ClassFactory_CreateInstance,
    ClassFactory_LockServer,
};

/* ------------------------------------------------------------------ Dll exports */

static ClassFactory class_factory = { { &ClassFactoryVtbl }, 1 };

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    /* CLSID_MHTMLDocument (.mht) is not needed for the Medula flow; add later if required. */
    if (!IsEqualIID(rclsid, &CLSID_HTMLDocument))
        return CLASS_E_CLASSNOTAVAILABLE;

    return ClassFactory_QueryInterface(&class_factory.IClassFactory_iface, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

static HRESULT do_register(void)
{
    HMODULE self = NULL;
    OLECHAR clsid_str[64];
    WCHAR key[256];
    WCHAR path[MAX_PATH];
    DWORD len;
    HKEY hkey;
    LONG r;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&do_register, &self);
    len = GetModuleFileNameW(self, path, MAX_PATH);

    StringFromGUID2(&CLSID_HTMLDocument, clsid_str, 64);
    wcscpy(key, L"Software\\Classes\\CLSID\\");
    wcscat(key, clsid_str);
    r = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL);
    if (r == ERROR_SUCCESS) {
        RegSetValueExW(hkey, NULL, 0, REG_SZ, (const BYTE *)L"HTMLDocument (shim)",
                       (lstrlenW(L"HTMLDocument (shim)") + 1) * sizeof(WCHAR));
        RegSetValueExW(hkey, L"InprocServer32", 0, REG_SZ, (const BYTE *)path,
                       (len + 1) * sizeof(WCHAR));
        RegCloseKey(hkey);
    } else {
        return HRESULT_FROM_WIN32(r);
    }
    return S_OK;
}

static HRESULT do_unregister(REFCLSID clsid)
{
    OLECHAR clsid_str[64];
    WCHAR key[256];
    StringFromGUID2(clsid, clsid_str, 64);
    wcscpy(key, L"Software\\Classes\\CLSID\\");
    wcscat(key, clsid_str);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    return S_OK;
}

HRESULT WINAPI DllRegisterServer(void)
{
    return do_register();
}

HRESULT WINAPI DllUnregisterServer(void)
{
    do_unregister(&CLSID_HTMLDocument);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        shim_hinst = hinst;
    return TRUE;
}
