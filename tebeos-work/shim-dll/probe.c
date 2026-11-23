/* probe.c -- verify the shim DLL loads and serves the full HTMLDocument surface.
 *
 * Loads htmlshim.dll, gets DllGetClassObject, creates the HTMLDocument, and QIs for every
 * interface the object should expose (IHTMLDocument2 + the OLE DocObject surface), plus a
 * couple of vtable calls. Uses lpVtbl directly (avoids the header's C-mode #define wrappers).
 *
 *   wine shim32-probe.exe shim32.dll     (or shim64-probe.exe shim64.dll)
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

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);

static const char *hex8(HRESULT h)
{
    static char b[16];
    snprintf(b, sizeof(b), "0x%08lx", (unsigned long)h);
    return b;
}

static int check_qi(IHTMLDocument2 *doc, REFIID riid, const char *name)
{
    void *p = NULL;
    HRESULT hr = doc->lpVtbl->QueryInterface(doc, riid, &p);
    printf("  QI(%-22s) = %s\n", name, hex8(hr));
    if (p)
        ((IUnknown *)p)->lpVtbl->Release((IUnknown *)p);
    return SUCCEEDED(hr) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "shim32.dll";
    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        printf("FAIL: LoadLibrary(%s) err=%lu\n", dll, (unsigned long)GetLastError());
        return 1;
    }

    DllGetClassObject_fn getco = (DllGetClassObject_fn)(void *)GetProcAddress(mod, "DllGetClassObject");
    if (!getco) {
        printf("FAIL: DllGetClassObject missing\n");
        return 1;
    }

    IClassFactory *cf = NULL;
    HRESULT hr = getco(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&cf);
    printf("DllGetClassObject(CLSID_HTMLDocument, IClassFactory) = %s\n", hex8(hr));
    if (FAILED(hr)) return 1;

    IHTMLDocument2 *doc = NULL;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IHTMLDocument2, (void **)&doc);
    printf("CreateInstance(IHTMLDocument2) = %s\n", hex8(hr));
    if (FAILED(hr)) return 1;

    printf("vtable routing (expect E_NOTIMPL=%s at this stage):\n", hex8(E_NOTIMPL));
    BSTR s = NULL;
    hr = doc->lpVtbl->get_title(doc, &s);
    printf("  get_title      = %s\n", hex8(hr));
    hr = doc->lpVtbl->get_URL(doc, &s);
    printf("  get_URL        = %s\n", hex8(hr));

    printf("DocObject surface QI:\n");
    int ok = 0;
    ok += check_qi(doc, &IID_IUnknown, "IUnknown");
    ok += check_qi(doc, &IID_IDispatch, "IDispatch");
    ok += check_qi(doc, &IID_IHTMLDocument2, "IHTMLDocument2");
    ok += check_qi(doc, &IID_IPersistMoniker, "IPersistMoniker");
    ok += check_qi(doc, &IID_IPersistStreamInit, "IPersistStreamInit");
    ok += check_qi(doc, &IID_IOleObject, "IOleObject");
    ok += check_qi(doc, &IID_IOleDocument, "IOleDocument");
    ok += check_qi(doc, &IID_IOleDocumentView, "IOleDocumentView");
    ok += check_qi(doc, &IID_IOleInPlaceObject, "IOleInPlaceObject");
    ok += check_qi(doc, &IID_IOleInPlaceActiveObject, "IOleInPlaceActiveObject");
    ok += check_qi(doc, &IID_IOleCommandTarget, "IOleCommandTarget");
    ok += check_qi(doc, &IID_IProvideClassInfo, "IProvideClassInfo");
    printf("  => %d/12 interfaces OK\n", ok);

    /* Milestone 3: IPersistMoniker::Load -> get_URL / get_readyState round-trip. */
    IPersistMoniker *pm = NULL;
    hr = doc->lpVtbl->QueryInterface(doc, &IID_IPersistMoniker, (void **)&pm);
    printf("\nIPersistMoniker QI = %s\n", hex8(hr));
    if (SUCCEEDED(hr)) {
        IMoniker *mk = NULL;
        /* a data: URL: the shim really navigates now, and tests never touch SGK */
        hr = CreateURLMoniker(NULL,
            L"data:text/html,<title>probe</title><p>probe</p>", &mk);
        printf("CreateURLMoniker     = %s\n", hex8(hr));
        if (SUCCEEDED(hr)) {
            hr = pm->lpVtbl->Load(pm, TRUE, mk, NULL, 0);
            printf("Load(moniker)        = %s\n", hex8(hr));
            mk->lpVtbl->Release(mk);
        }
        BSTR url = NULL, rs = NULL;
        doc->lpVtbl->get_URL(doc, &url);
        doc->lpVtbl->get_readyState(doc, &rs);
        printf("get_URL              = '%ls'\n", url ? url : L"(null)");
        printf("get_readyState       = '%ls'\n", rs ? rs : L"(null)");
        SysFreeString(url); SysFreeString(rs);
        pm->lpVtbl->Release(pm);
    }

    doc->lpVtbl->Release(doc);
    cf->lpVtbl->Release(cf);
    FreeLibrary(mod);
    printf("PASS\n");
    return 0;
}
