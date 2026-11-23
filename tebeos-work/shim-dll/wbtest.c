/* wbtest.c -- the shim behind Wine's real WebBrowser control (ieframe), as the app uses it.
 *
 * The shim's class object is registered in-process for CLSID_HTMLDocument, so every document
 * ieframe creates is ours, and a WebBrowser is hosted in an AtlAxWin window. Against the
 * pages of wbserver.py it checks what WinForms' WebBrowser relies on:
 *
 *   Navigate -> BeforeNavigate2 / NavigateComplete2 / DocumentComplete, LocationURL, title
 *   IPersistStreamInit::Save returns the bytes the server sent (windows-1254)
 *   InvokeScript (get_Script -> GetIDsOfNames -> Invoke)
 *   a page-initiated navigation (link click) is reported through BeforeNavigate2
 *   cancelling it in BeforeNavigate2 keeps the page (runtests.sh checks the server never saw it)
 *   Navigate with POST data and headers; a 302 redirect; a form POST from the page
 *   DocumentText: about:blank, then IPersistStreamInit::Load, then DocumentComplete again
 *
 * The shim starts its own Chromium (TEBEOS_CDP_PORT / TEBEOS_CDP_PROFILE).
 *
 * Usage:  wine shim32-wbtest.exe shim32.dll SERVER_PORT
 */

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <ole2.h>
#include <mshtml.h>
#include <exdisp.h>
#include <exdispid.h>
#include <ocidl.h>
#include <oaidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef BOOL (WINAPI *AtlAxWinInit_fn)(void);
typedef HRESULT (WINAPI *AtlAxGetControl_fn)(HWND, IUnknown **);

static int failures;
static int server_port;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

/* Wait while dispatching messages, until *flag reaches want or ms pass. */
static int pump_until(volatile LONG *flag, LONG want, DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        DWORD now = GetTickCount();
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (flag && *flag >= want)
            return 1;
        if ((LONG)(end - now) <= 0)
            return 0;
        MsgWaitForMultipleObjects(0, NULL, FALSE, end - now < 20 ? end - now : 20, QS_ALLINPUT);
    }
}

static void pump_sleep(DWORD ms)
{
    pump_until(NULL, 0, ms);
}

static char *w2u(const WCHAR *w)
{
    int n;
    char *s;
    if (!w)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = s ? strlen(s) : 0, b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* ---- DWebBrowserEvents2 sink ---- */

typedef struct Events {
    IDispatch IDispatch_iface;
    volatile LONG before, nav_complete, doc_complete, nav_error;
    char last_before[1024];
    char last_complete[1024];
    LONG last_before_post;          /* bytes of PostData in the last BeforeNavigate2 */
    char before_title[256];         /* document.title read inside the last BeforeNavigate2 */
    LONG before_title_ms;
} Events;

static Events ev;
static char *doc_title(void);

static HRESULT STDMETHODCALLTYPE ev_qi(IDispatch *t, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &DIID_DWebBrowserEvents2)) {
        *ppv = t;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ev_addref(IDispatch *t) { return 2; }
static ULONG STDMETHODCALLTYPE ev_release(IDispatch *t) { return 1; }
static HRESULT STDMETHODCALLTYPE ev_tic(IDispatch *t, UINT *n) { *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_ti(IDispatch *t, UINT i, LCID l, ITypeInfo **ti) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ev_ids(IDispatch *t, REFIID r, LPOLESTR *n, UINT c, LCID l, DISPID *d) { return E_NOTIMPL; }

static const WCHAR *byref_bstr(VARIANT *v)
{
    if (V_VT(v) == (VT_BYREF | VT_VARIANT) && V_VARIANTREF(v))
        v = V_VARIANTREF(v);
    return V_VT(v) == VT_BSTR ? V_BSTR(v) : NULL;
}

static HRESULT STDMETHODCALLTYPE ev_invoke(IDispatch *t, DISPID id, REFIID r, LCID l, WORD f,
                                           DISPPARAMS *dp, VARIANT *res, EXCEPINFO *ei, UINT *ae)
{
    char *u;
    switch (id) {
    case DISPID_BEFORENAVIGATE2: {
        /* pDisp, URL, Flags, TargetFrameName, PostData, Headers, Cancel (reversed) */
        VARIANT *post = &dp->rgvarg[2];
        u = w2u(byref_bstr(&dp->rgvarg[5]));
        snprintf(ev.last_before, sizeof(ev.last_before), "%s", u ? u : "");
        ev.last_before_post = 0;
        /* ieframe passes it as a variant reference to a variant reference, as IE does */
        while (V_VT(post) == (VT_BYREF | VT_VARIANT) && V_VARIANTREF(post))
            post = V_VARIANTREF(post);
        if ((V_VT(post) & VT_ARRAY) && V_ARRAY(post))
            ev.last_before_post = (LONG)V_ARRAY(post)->rgsabound[0].cElements;
        {
            /* IE: the page being left can still be read (the vendor's handler does) */
            DWORD t0 = GetTickCount();
            char *title = doc_title();
            ev.before_title_ms = (LONG)(GetTickCount() - t0);
            snprintf(ev.before_title, sizeof(ev.before_title), "%s", title ? title : "");
            free(title);
        }
        printf("  BeforeNavigate2 %s (post %ld, title \"%s\" in %ld ms)\n", ev.last_before,
               ev.last_before_post, ev.before_title, ev.before_title_ms);
        if (u && strstr(u, "cancelme") && V_VT(&dp->rgvarg[0]) == (VT_BYREF | VT_BOOL))
            *V_BOOLREF(&dp->rgvarg[0]) = VARIANT_TRUE;
        free(u);
        InterlockedIncrement(&ev.before);
        break;
    }
    case DISPID_NAVIGATECOMPLETE2:
        u = w2u(byref_bstr(&dp->rgvarg[0]));
        printf("  NavigateComplete2 %s\n", u ? u : "");
        free(u);
        InterlockedIncrement(&ev.nav_complete);
        break;
    case DISPID_DOCUMENTCOMPLETE:
        u = w2u(byref_bstr(&dp->rgvarg[0]));
        snprintf(ev.last_complete, sizeof(ev.last_complete), "%s", u ? u : "");
        printf("  DocumentComplete %s\n", ev.last_complete);
        free(u);
        InterlockedIncrement(&ev.doc_complete);
        break;
    case DISPID_NAVIGATEERROR:
        u = w2u(byref_bstr(&dp->rgvarg[3]));
        printf("  NavigateError %s\n", u ? u : "");
        free(u);
        InterlockedIncrement(&ev.nav_error);
        break;
    }
    return S_OK;
}

static IDispatchVtbl ev_vtbl = { ev_qi, ev_addref, ev_release, ev_tic, ev_ti, ev_ids, ev_invoke };

/* ---- helpers over the WebBrowser and its document ---- */

static IWebBrowser2 *wb;

static void url_of(const char *path, WCHAR *out, int n)
{
    char u[256];
    snprintf(u, sizeof(u), "http://127.0.0.1:%d%s", server_port, path);
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, n);
}

/* Navigate and wait for its DocumentComplete. */
static int navigate(const WCHAR *url, VARIANT *post, VARIANT *headers)
{
    BSTR b = SysAllocString(url);
    LONG want = ev.doc_complete + 1;
    VARIANT empty;
    HRESULT hr;
    VariantInit(&empty);
    hr = IWebBrowser2_Navigate(wb, b, &empty, &empty, post ? post : &empty, headers ? headers : &empty);
    SysFreeString(b);
    if (FAILED(hr)) {
        printf("  Navigate hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    return pump_until(&ev.doc_complete, want, 20000);
}

static IHTMLDocument2 *document(void)
{
    IDispatch *disp = NULL;
    IHTMLDocument2 *doc = NULL;
    if (SUCCEEDED(IWebBrowser2_get_Document(wb, &disp)) && disp) {
        IDispatch_QueryInterface(disp, &IID_IHTMLDocument2, (void **)&doc);
        IDispatch_Release(disp);
    }
    return doc;
}

static char *doc_title(void)
{
    IHTMLDocument2 *doc = document();
    BSTR t = NULL;
    char *s;
    if (!doc)
        return NULL;
    IHTMLDocument2_get_title(doc, &t);
    IHTMLDocument2_Release(doc);
    s = w2u(t);
    SysFreeString(t);
    return s;
}

static char *location(void)
{
    BSTR u = NULL;
    char *s;
    IWebBrowser2_get_LocationURL(wb, &u);
    s = w2u(u);
    SysFreeString(u);
    return s;
}

static IHTMLElement *element(const WCHAR *id)
{
    IHTMLDocument2 *doc = document();
    IHTMLDocument3 *d3 = NULL;
    IHTMLElement *el = NULL;
    BSTR b = SysAllocString(id);
    if (doc && SUCCEEDED(IHTMLDocument2_QueryInterface(doc, &IID_IHTMLDocument3, (void **)&d3))) {
        IHTMLDocument3_getElementById(d3, b, &el);
        IHTMLDocument3_Release(d3);
    }
    if (doc)
        IHTMLDocument2_Release(doc);
    SysFreeString(b);
    return el;
}

static char *inner_text(const WCHAR *id)
{
    IHTMLElement *el = element(id);
    BSTR t = NULL;
    char *s;
    if (!el)
        return NULL;
    IHTMLElement_get_innerText(el, &t);
    IHTMLElement_Release(el);
    s = w2u(t);
    SysFreeString(t);
    return s;
}

static int click(const WCHAR *id)
{
    IHTMLElement *el = element(id);
    if (!el)
        return 0;
    IHTMLElement_click(el);
    IHTMLElement_Release(el);
    return 1;
}

/* HtmlElement.Focus() / a blur, both through IHTMLElement2 */
static void focus_el(const WCHAR *id, int on)
{
    IHTMLElement *el = element(id);
    IHTMLElement2 *e2 = NULL;
    if (el && SUCCEEDED(IHTMLElement_QueryInterface(el, &IID_IHTMLElement2, (void **)&e2))) {
        if (on)
            IHTMLElement2_focus(e2);
        else
            IHTMLElement2_blur(e2);
        IHTMLElement2_Release(e2);
    }
    if (el)
        IHTMLElement_Release(el);
}

/* HtmlElement.GetAttribute("value") */
static char *value_of(const WCHAR *id)
{
    IHTMLElement *el = element(id);
    BSTR n = SysAllocString(L"value");
    VARIANT v;
    char *s = NULL;
    VariantInit(&v);
    if (el && SUCCEEDED(IHTMLElement_getAttribute(el, n, 0, &v)) && V_VT(&v) == VT_BSTR)
        s = w2u(V_BSTR(&v));
    VariantClear(&v);
    SysFreeString(n);
    if (el)
        IHTMLElement_Release(el);
    return s;
}

/* HtmlElement.SetAttribute("value", s) */
static void set_value(const WCHAR *id, const WCHAR *s)
{
    IHTMLElement *el = element(id);
    BSTR n = SysAllocString(L"value");
    VARIANT v;
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(s);
    if (el) {
        IHTMLElement_setAttribute(el, n, v, 0);
        IHTMLElement_Release(el);
    }
    VariantClear(&v);
    SysFreeString(n);
}

static int mem_has(const char *p, size_t n, const char *needle, size_t m)
{
    size_t i;
    for (i = 0; m <= n && i <= n - m; i++)
        if (!memcmp(p + i, needle, m))
            return 1;
    return 0;
}

static void sa_post(VARIANT *v, const char *data)
{
    SAFEARRAY *sa = SafeArrayCreateVector(VT_UI1, 0, (ULONG)strlen(data));
    void *p;
    SafeArrayAccessData(sa, &p);
    memcpy(p, data, strlen(data));
    SafeArrayUnaccessData(sa);
    VariantInit(v);
    V_VT(v) = VT_ARRAY | VT_UI1;
    V_ARRAY(v) = sa;
}

/* What the app's SetCaptcha/CapthaParser do: the first image of document.images (enumerated
 * with foreach) whose href contains SayiUretenImageYeniServlet, drawn with
 * IHTMLElementRender::DrawToDC into a width x height bitmap. Returns a 32-bpp DIB section
 * (top-down) or NULL, with its size in *w and *h. */
static HBITMAP captcha_bitmap(int *w, int *h, BYTE **bits, char *href_out, size_t href_len)
{
    IHTMLDocument2 *doc = document();
    IHTMLElementCollection *images = NULL;
    IUnknown *unk = NULL;
    IEnumVARIANT *en = NULL;
    IHTMLImgElement *img = NULL;
    HBITMAP bmp = NULL;
    VARIANT v;
    ULONG got;

    *w = *h = 0;
    if (!doc || FAILED(IHTMLDocument2_get_images(doc, &images)) || !images ||
        FAILED(IHTMLElementCollection_get__newEnum(images, &unk)) || !unk ||
        FAILED(IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&en)))
        goto done;
    VariantInit(&v);
    while (!img && IEnumVARIANT_Next(en, 1, &v, &got) == S_OK && got == 1) {
        IHTMLImgElement *i = NULL;
        BSTR href = NULL;
        char *hs;
        if (V_VT(&v) == VT_DISPATCH && V_DISPATCH(&v) &&
            SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&v), &IID_IHTMLImgElement, (void **)&i))) {
            IHTMLImgElement_get_href(i, &href);
            hs = w2u(href);
            if (hs && strstr(hs, "SayiUretenImageYeniServlet")) {
                img = i;
                i = NULL;
                snprintf(href_out, href_len, "%s", hs);
            }
            free(hs);
            SysFreeString(href);
            if (i)
                IHTMLImgElement_Release(i);
        }
        VariantClear(&v);
    }
    if (img) {
        IHTMLElementRender *r = NULL;
        long lw = 0, lh = 0;
        IHTMLImgElement_get_width(img, &lw);
        IHTMLImgElement_get_height(img, &lh);
        *w = (int)lw;
        *h = (int)lh;
        if (lw > 0 && lh > 0 &&
            SUCCEEDED(IHTMLImgElement_QueryInterface(img, &IID_IHTMLElementRender, (void **)&r))) {
            BITMAPINFO bi;
            HDC dc = CreateCompatibleDC(NULL);
            HGDIOBJ old;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = lw;
            bi.bmiHeader.biHeight = -lh;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
            old = SelectObject(dc, bmp);
            memset(*bits, 0xff, (size_t)lw * lh * 4);
            if (FAILED(IHTMLElementRender_DrawToDC(r, dc))) {
                SelectObject(dc, old);
                DeleteObject(bmp);
                bmp = NULL;
            } else {
                GdiFlush();
                SelectObject(dc, old);
            }
            DeleteDC(dc);
            IHTMLElementRender_Release(r);
        }
        IHTMLImgElement_Release(img);
    }
done:
    if (en) IEnumVARIANT_Release(en);
    if (unk) IUnknown_Release(unk);
    if (images) IHTMLElementCollection_Release(images);
    if (doc) IHTMLDocument2_Release(doc);
    return bmp;
}

static void save_bmp(const char *path, const BYTE *bits, int w, int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits, 4, (size_t)w * h, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    HMODULE shim, atl;
    DllGetClassObject_fn gco;
    AtlAxWinInit_fn axinit;
    AtlAxGetControl_fn axget;
    IClassFactory *cf = NULL;
    IUnknown *ctl = NULL;
    IConnectionPointContainer *cpc = NULL;
    IConnectionPoint *cp = NULL;
    DWORD reg = 0, cookie = 0;
    HWND host;
    HRESULT hr;
    WCHAR url[256];
    char *s;

    if (argc < 3) {
        printf("usage: wbtest SHIM.dll SERVER_PORT\n"
               "       wbtest SHIM.dll --captcha URL OUT.bmp   (one page load, saves the captcha)\n");
        return 2;
    }
    server_port = atoi(argv[2]);
    setvbuf(stdout, NULL, _IONBF, 0);
    OleInitialize(NULL);

    shim = LoadLibraryA(argv[1]);
    gco = shim ? (DllGetClassObject_fn)GetProcAddress(shim, "DllGetClassObject") : NULL;
    if (!gco || FAILED(gco(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&cf))) {
        printf("FAIL: shim class object\n");
        return 1;
    }
    hr = CoRegisterClassObject(&CLSID_HTMLDocument, (IUnknown *)cf, CLSCTX_INPROC_SERVER,
                               REGCLS_MULTIPLEUSE, &reg);
    check(SUCCEEDED(hr), "CoRegisterClassObject(CLSID_HTMLDocument)");

    atl = LoadLibraryA("atl.dll");
    axinit = atl ? (AtlAxWinInit_fn)GetProcAddress(atl, "AtlAxWinInit") : NULL;
    axget = atl ? (AtlAxGetControl_fn)GetProcAddress(atl, "AtlAxGetControl") : NULL;
    if (!axinit || !axget || !axinit()) {
        printf("FAIL: atl.dll AtlAxWin\n");
        return 1;
    }
    host = CreateWindowExW(0, L"AtlAxWin", L"Shell.Explorer.2", WS_OVERLAPPEDWINDOW,
                           0, 0, 900, 700, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!host || FAILED(axget(host, &ctl)) || !ctl ||
        FAILED(IUnknown_QueryInterface(ctl, &IID_IWebBrowser2, (void **)&wb))) {
        printf("FAIL: WebBrowser in AtlAxWin\n");
        return 1;
    }
    ev.IDispatch_iface.lpVtbl = &ev_vtbl;
    if (SUCCEEDED(IWebBrowser2_QueryInterface(wb, &IID_IConnectionPointContainer, (void **)&cpc)) &&
        SUCCEEDED(IConnectionPointContainer_FindConnectionPoint(cpc, &DIID_DWebBrowserEvents2, &cp)))
        IConnectionPoint_Advise(cp, (IUnknown *)&ev.IDispatch_iface, &cookie);
    check(cookie != 0, "advise DWebBrowserEvents2");
    IWebBrowser2_put_Silent(wb, VARIANT_TRUE);

    if (!strcmp(argv[2], "--captcha") && argc >= 5) {
        /* one load of a live login page: what the app's OCR would get */
        WCHAR wurl[1024];
        char href[1024] = "";
        BYTE *bits = NULL;
        int w, h;
        HBITMAP bmp;
        MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, wurl, 1024);
        check(navigate(wurl, NULL, NULL), "Navigate(login page) -> DocumentComplete");
        pump_sleep(1500);
        bmp = captcha_bitmap(&w, &h, &bits, href, sizeof(href));
        printf("captcha img href=%s size=%dx%d\n", href, w, h);
        check(bmp != NULL, "DrawToDC(captcha)");
        if (bmp) {
            save_bmp(argv[4], bits, w, h);
            DeleteObject(bmp);
        }
        goto out;
    }

    /* 1. a plain navigation */
    url_of("/index", url, 256);
    check(navigate(url, NULL, NULL), "Navigate(/index) -> DocumentComplete");
    check(ev.before >= 1 && ev.nav_complete >= 1, "  BeforeNavigate2 + NavigateComplete2 fired");
    s = location();
    check(ends_with(s, "/index"), "  LocationURL ends /index");
    free(s);
    s = doc_title();
    check(s && !strcmp(s, "Index"), "  document.title == Index");
    free(s);
    s = inner_text(L"t");
    check(s && !strcmp(s, "Sistem hatası: çğış"), "  windows-1254 text decoded");
    free(s);

    /* 2. Save returns the server's bytes (WinForms DocumentText reads this) */
    {
        IHTMLDocument2 *doc = document();
        IPersistStreamInit *ps = NULL;
        IStream *stm = NULL;
        HGLOBAL hg;
        int ok = 0;
        if (doc && SUCCEEDED(IHTMLDocument2_QueryInterface(doc, &IID_IPersistStreamInit, (void **)&ps)) &&
            SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stm)) &&
            SUCCEEDED(IPersistStreamInit_Save(ps, stm, FALSE)) &&
            SUCCEEDED(GetHGlobalFromStream(stm, &hg))) {
            SIZE_T n = GlobalSize(hg);
            const char *p = (const char *)GlobalLock(hg);
            /* "hatas\xfd" is windows-1254 for "hatası" */
            ok = p && n > 20 && mem_has(p, n, "hatas\xfd", 6) && !mem_has(p, n, "\xc4\xb1", 2);
            GlobalUnlock(hg);
        }
        check(ok, "IPersistStreamInit::Save gives the windows-1254 bytes");
        if (stm) IStream_Release(stm);
        if (ps) IPersistStreamInit_Release(ps);
        if (doc) IHTMLDocument2_Release(doc);
    }

    /* 3. InvokeScript */
    {
        IHTMLDocument2 *doc = document();
        IDispatch *script = NULL;
        LPOLESTR name = L"add";
        DISPID id = -1;
        VARIANT args[2], res;
        DISPPARAMS dp;
        int ok = 0;
        VariantInit(&res);
        if (doc && SUCCEEDED(IHTMLDocument2_get_Script(doc, &script)) && script &&
            SUCCEEDED(IDispatch_GetIDsOfNames(script, &IID_NULL, &name, 1, 0, &id))) {
            V_VT(&args[0]) = VT_I4; V_I4(&args[0]) = 3;   /* reversed: add(2, 3) */
            V_VT(&args[1]) = VT_I4; V_I4(&args[1]) = 2;
            dp.rgvarg = args; dp.cArgs = 2; dp.rgdispidNamedArgs = NULL; dp.cNamedArgs = 0;
            if (SUCCEEDED(IDispatch_Invoke(script, id, &IID_NULL, 0, DISPATCH_METHOD, &dp, &res, NULL, NULL)))
                ok = (V_VT(&res) == VT_I4 && V_I4(&res) == 5) || (V_VT(&res) == VT_R8 && V_R8(&res) == 5.0);
        }
        check(ok, "InvokeScript(add, 2, 3) == 5");
        VariantClear(&res);
        if (script) IDispatch_Release(script);
        if (doc) IHTMLDocument2_Release(doc);
    }

    /* 4. a page-initiated navigation is reported */
    {
        LONG before = ev.before, want = ev.doc_complete + 1;
        check(click(L"l"), "click #l (link to /second)");
        check(pump_until(&ev.doc_complete, want, 20000), "  DocumentComplete after the click");
        check(ev.before > before && ends_with(ev.last_before, "/second"), "  BeforeNavigate2 for /second");
        check(!strcmp(ev.before_title, "Index") && ev.before_title_ms < 2000,
              "  BeforeNavigate2 handler read the title of the page it leaves");
        s = doc_title();
        check(s && !strcmp(s, "Second"), "  document.title == Second");
        free(s);
    }

    /* 5. cancel in BeforeNavigate2 */
    url_of("/index", url, 256);
    navigate(url, NULL, NULL);
    {
        LONG before = ev.before;
        click(L"c");
        pump_sleep(2500);
        check(ev.before > before && strstr(ev.last_before, "cancelme"), "BeforeNavigate2 for /cancelme");
        s = doc_title();
        check(s && !strcmp(s, "Index"), "  cancelled: still on Index");
        free(s);
    }

    /* 6. Navigate with POST data and headers */
    {
        VARIANT post, hdr;
        sa_post(&post, "a=1&b=%C3%A7");
        VariantInit(&hdr);
        V_VT(&hdr) = VT_BSTR;
        V_BSTR(&hdr) = SysAllocString(L"Content-Type: application/x-www-form-urlencoded\r\n");
        url_of("/post", url, 256);
        check(navigate(url, &post, &hdr), "Navigate(/post, POST data) -> DocumentComplete");
        s = inner_text(L"m");
        check(s && !strcmp(s, "POST"), "  server saw POST");
        free(s);
        s = inner_text(L"b");
        check(s && !strcmp(s, "a=1&b=%C3%A7"), "  server saw the body");
        free(s);
        s = inner_text(L"ct");
        check(s && strstr(s, "application/x-www-form-urlencoded"), "  server saw the Content-Type");
        free(s);
        VariantClear(&post);
        VariantClear(&hdr);
    }

    /* 7. a redirect */
    url_of("/redirect", url, 256);
    check(navigate(url, NULL, NULL), "Navigate(/redirect) -> DocumentComplete");
    s = location();
    check(ends_with(s, "/second"), "  LocationURL follows the redirect");
    free(s);

    /* 8. a form POST from the page */
    url_of("/index", url, 256);
    navigate(url, NULL, NULL);
    {
        /* GetStatus compares form.GetAttribute("action") with the path as written */
        IHTMLElement *f = element(L"f");
        BSTR an = SysAllocString(L"action");
        VARIANT av;
        char *a = NULL;
        VariantInit(&av);
        if (f && SUCCEEDED(IHTMLElement_getAttribute(f, an, 0, &av)) && V_VT(&av) == VT_BSTR)
            a = w2u(V_BSTR(&av));
        check(a && !strcmp(a, "/post"), "getAttribute(\"action\") is the attribute as written");
        free(a);
        VariantClear(&av);
        SysFreeString(an);
        if (f) IHTMLElement_Release(f);
    }
    {
        IHTMLElement *f = element(L"f");
        IHTMLFormElement *form = NULL;
        LONG want = ev.doc_complete + 1;
        if (f && SUCCEEDED(IHTMLElement_QueryInterface(f, &IID_IHTMLFormElement, (void **)&form))) {
            IHTMLFormElement_submit(form);
            IHTMLFormElement_Release(form);
        }
        if (f) IHTMLElement_Release(f);
        check(pump_until(&ev.doc_complete, want, 20000), "form.submit() -> DocumentComplete");
        check(ev.last_before_post > 0, "  BeforeNavigate2 carried the POST data");
        check(!strcmp(ev.before_title, "Index") && ev.before_title_ms < 2000,
              "  BeforeNavigate2 handler read the form's page");
        s = inner_text(L"b");
        check(s && !strcmp(s, "q=%E7%F0%FD%FE"), "  server got q in windows-1254");
        free(s);
        s = inner_text(L"ct");
        check(s && strstr(s, "application/x-www-form-urlencoded"), "  server got the form Content-Type");
        free(s);
        s = inner_text(L"r");
        check(ends_with(s, "/index"), "  server got the Referer of the form's page");
        free(s);
        s = inner_text(L"o");
        {
            char want[64];
            snprintf(want, sizeof(want), "http://127.0.0.1:%d", server_port);
            check(s && !strcmp(s, want), "  server got the Origin (SGK ignores a POST without it)");
        }
        free(s);
    }

    /* 9. DocumentText: about:blank, then IPersistStreamInit::Load */
    {
        LONG want;
        check(navigate(L"about:blank", NULL, NULL), "Navigate(about:blank) -> DocumentComplete");
        want = ev.doc_complete + 1;
        {
            static const char html[] = "\xef\xbb\xbf<html><body><p id=x>merhaba \xc5\x9f</p></body></html>";
            IHTMLDocument2 *doc = document();
            IPersistStreamInit *ps = NULL;
            IStream *stm = NULL;
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(html) - 1);
            memcpy(GlobalLock(hg), html, sizeof(html) - 1);
            GlobalUnlock(hg);
            CreateStreamOnHGlobal(hg, TRUE, &stm);
            if (doc && SUCCEEDED(IHTMLDocument2_QueryInterface(doc, &IID_IPersistStreamInit, (void **)&ps))) {
                IPersistStreamInit_InitNew(ps);
                check(SUCCEEDED(IPersistStreamInit_Load(ps, stm)), "IPersistStreamInit::Load(html)");
                IPersistStreamInit_Release(ps);
            }
            if (stm) IStream_Release(stm);
            if (doc) IHTMLDocument2_Release(doc);
        }
        check(pump_until(&ev.doc_complete, want, 10000), "  DocumentComplete for the loaded text");
        s = inner_text(L"x");
        check(s && !strcmp(s, "merhaba ş"), "  #x reads back");
        free(s);
    }

    /* 10. the captcha path of SetCaptcha/CapthaParser */
    url_of("/captcha", url, 256);
    check(navigate(url, NULL, NULL), "Navigate(/captcha) -> DocumentComplete");
    pump_sleep(300);
    {
        char href[1024] = "";
        BYTE *bits = NULL;
        int w, h;
        HBITMAP bmp = captcha_bitmap(&w, &h, &bits, href, sizeof(href));
        check(strstr(href, "SayiUretenImageYeniServlet") != NULL, "  images foreach finds the img by href");
        check(w == 80 && h == 30, "  img width/height 80x30");
        check(bmp != NULL, "  IHTMLElementRender::DrawToDC");
        if (bmp) {
            /* BGRA; the img sits at x .5 on the page, as the Medula captcha does */
            const BYTE *l = bits + (15 * w + 10) * 4, *g = bits + (15 * w + 11) * 4, *r = bits + (15 * w + 70) * 4;
            printf("  pixels x10 %02x%02x%02x x11 %02x%02x%02x x70 %02x%02x%02x\n",
                   l[2], l[1], l[0], g[2], g[1], g[0], r[2], r[1], r[0]);
            check(l[2] > 200 && l[1] < 60 && l[0] < 60 && g[1] > 200 && g[2] < 60 && g[0] < 60,
                  "  1px red/green columns drawn exactly (no resampling)");
            check(r[0] > 200 && r[1] < 60 && r[2] < 60, "  right half drawn blue");
            DeleteObject(bmp);
        }
    }

    /* 11. focus events come from the message loop, as in IE. TEBEOS focuses Medula's T.C.
     * field (onfocus writes prompt underscores, onblur strips them), fills the prescription
     * number and clicks Sorgula without pumping in between; IE posts the field empty. */
    url_of("/prompt", url, 256);
    check(navigate(url, NULL, NULL), "Navigate(/prompt) -> DocumentComplete");
    {
        char *v;
        focus_el(L"t4", 1);
        v = value_of(L"t4");
        check(v && !strcmp(v, ""), "  focus(): onfocus has not run yet");
        free(v);
        pump_sleep(300);
        v = value_of(L"t4");
        check(v && !strcmp(v, "___"), "  onfocus ran from the message loop");
        free(v);
        focus_el(L"t4", 0);
        pump_sleep(300);
        v = value_of(L"t4");
        check(v && !strcmp(v, ""), "  blur(): onblur ran from the message loop");
        free(v);
    }
    {
        LONG want = ev.doc_complete + 1;
        focus_el(L"t4", 1);
        set_value(L"t2", L"123");
        click(L"ps");
        check(pump_until(&ev.doc_complete, want, 20000), "focus + fill + click Sorgula -> DocumentComplete");
        s = inner_text(L"b");
        check(s && !strcmp(s, "t4=&t2=123&ps=Sorgula"), "  server got t4 empty, as IE posts it");
        if (s && strcmp(s, "t4=&t2=123&ps=Sorgula"))
            printf("  body: %s\n", s);
        free(s);
    }

out:
    printf("events: before=%ld navcomplete=%ld doccomplete=%ld error=%ld\n",
           ev.before, ev.nav_complete, ev.doc_complete, ev.nav_error);

    if (cp) {
        IConnectionPoint_Unadvise(cp, cookie);
        IConnectionPoint_Release(cp);
    }
    if (cpc) IConnectionPointContainer_Release(cpc);
    IWebBrowser2_Release(wb);
    IUnknown_Release(ctl);
    DestroyWindow(host);
    pump_sleep(200);
    CoRevokeClassObject(reg);
    IClassFactory_Release(cf);
    OleUninitialize();
    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
