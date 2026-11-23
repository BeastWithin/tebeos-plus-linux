/* domtest.c -- exercise the CDP-backed DOM COM surface against real Chromium + shim DLL.
 *
 * Usage:  wine shim32-domtest.exe [port]     (default 9222)
 *
 * Expects a Chromium already running with that port, e.g.:
 *   chromium --headless=new --remote-debugging-port=9222 --no-sandbox about:blank
 *
 * Drives the shim's exported shim_dom_connect()/shim_dom_navigate() to point the DLL at the
 * live browser and load a test page, then creates the HTMLDocument via the COM class
 * factory and proves IHTMLDocument3::getElementById -> IHTMLElement getAttribute /
 * get_innerText / get_tagName / get_id -> IDispatch Invoke("click") all round-trip
 * through Chrome DevTools Protocol.
 */

#include <windows.h>
#include <initguid.h>
#include <ole2.h>
#include <mshtml.h>
#include <oleidl.h>
#include <docobj.h>
#include <urlmon.h>
#include <ocidl.h>
#include <oaidl.h>
#include <stdio.h>

#ifdef _WIN64
#define SHIM_DLL "shim64.dll"
#else
#define SHIM_DLL "shim32.dll"
#endif

typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef int (__cdecl *connect_fn)(int port);
typedef int (__cdecl *navigate_fn)(const char *url);

static const char TEST_URL[] =
    "data:text/html;base64,PCFET0NUWVBFIGh0bWw+PGh0bWw+PGhlYWQ+PG1ldGEgY2hhcnNldD0i"
    "dXRmLTgiPjx0aXRsZT50ZWI8L3RpdGxlPjwvaGVhZD48Ym9keT48ZGl2"
    "IGlkPSJmb28iIGRhdGEteD0iNDIiIGRhdGEtY2xpY2tlZD0iMCI+bWVy"
    "aGFiYSDFn8Sxw6fDtjwvZGl2PjxkaXYgaWQ9ImJhciIgZGF0YS15PSI3"
    "Ij5pa2luY2k8L2Rpdj48aW5wdXQgaWQ9ImlucCIgdHlwZT0idGV4dCIg"
    "bmFtZT0idW5hbWUiIHZhbHVlPSJoaSI+PGlucHV0IGlkPSJjaGsiIHR5"
    "cGU9ImNoZWNrYm94IiBuYW1lPSJrdmtrIj48c2NyaXB0PmRvY3VtZW50"
    "LmdldEVsZW1lbnRCeUlkKCdmb28nKS5vbmNsaWNrPWZ1bmN0aW9uKCl7"
    "dGhpcy5zZXRBdHRyaWJ1dGUoJ2RhdGEtY2xpY2tlZCcsJzEnKTt9Ozwv"
    "c2NyaXB0PjwvYm9keT48L2h0bWw+";

static int failures = 0;

static const char *pass(int ok)
{
    if (!ok)
        failures++;
    return ok ? "OK" : "FAIL";
}

/* getAttribute -> UTF-8 C string (malloc'd) via the element's vtable */
static char *get_attr(IHTMLElement *e, const wchar_t *name)
{
    VARIANT v;
    char *out = NULL;
    BSTR b = SysAllocString(name);
    HRESULT hr = e->lpVtbl->getAttribute(e, b, 0, &v);
    SysFreeString(b);
    if (SUCCEEDED(hr) && v.vt == VT_BSTR) {
        int n = WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, NULL, 0, NULL, NULL);
        out = (char *)malloc((size_t)n + 1);
        if (out)
            WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, out, n, NULL, NULL);
        VariantClear(&v);
    } else {
        VariantClear(&v);
    }
    return out;
}

static char *get_bstr(IHTMLElement *e, int which)
{
    BSTR s = NULL;
    HRESULT hr;
    char *out = NULL;
    switch (which) {
    case 0: hr = e->lpVtbl->get_id(e, &s); break;
    case 1: hr = e->lpVtbl->get_tagName(e, &s); break;
    case 2: hr = e->lpVtbl->get_innerText(e, &s); break;
    default: return NULL;
    }
    if (SUCCEEDED(hr) && s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        out = (char *)malloc((size_t)n + 1);
        if (out)
            WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
        SysFreeString(s);
    }
    return out;
}

static char *utf8_from_bstr(BSTR s)
{
    int n;
    char *out;
    if (!s)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    out = (char *)malloc((size_t)n + 1);
    if (out)
        WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}

static IHTMLElement *col_item_elem(IHTMLElementCollection *col, long index)
{
    VARIANT vn, vi;
    IDispatch *d = NULL;
    VariantInit(&vn);
    vi.vt = VT_I4;
    vi.lVal = index;
    col->lpVtbl->item(col, vn, vi, &d);
    return (IHTMLElement *)d;
}

static char *input_value(IHTMLInputElement *inp)
{
    BSTR s = NULL;
    char *out = NULL;
    if (SUCCEEDED(inp->lpVtbl->get_value(inp, &s)) && s) {
        out = utf8_from_bstr(s);
        SysFreeString(s);
    }
    return out;
}

static void set_attr_str(IHTMLElement *e, const wchar_t *name, const wchar_t *val)
{
    BSTR nb = SysAllocString(name);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(val);
    e->lpVtbl->setAttribute(e, nb, v, 0);
    SysFreeString(nb);
    VariantClear(&v);
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    HMODULE mod = LoadLibraryA(SHIM_DLL);
    DllGetClassObject_fn getco;
    connect_fn pconnect;
    navigate_fn pnavigate;
    IClassFactory *cf = NULL;
    IHTMLDocument2 *doc = NULL;
    IHTMLDocument3 *doc3 = NULL;
    IHTMLElement *e = NULL;
    LCID lcid = 0;

    setbuf(stdout, NULL);

    if (!mod) {
        printf("FAIL: LoadLibrary(%s) err=%lu\n", SHIM_DLL, (unsigned long)GetLastError());
        return 1;
    }
    getco = (DllGetClassObject_fn)(void *)GetProcAddress(mod, "DllGetClassObject");
    pconnect = (connect_fn)(void *)GetProcAddress(mod, "shim_dom_connect");
    pnavigate = (navigate_fn)(void *)GetProcAddress(mod, "shim_dom_navigate");
    if (!getco || !pconnect || !pnavigate) {
        printf("FAIL: missing exports\n");
        return 1;
    }

    printf("shim_dom_connect(%d) = %s\n", port, pass(pconnect(port) == 0) );
    printf("shim_dom_navigate(test) = %s\n", pass(pnavigate(TEST_URL) == 0));

    if (FAILED(getco(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&cf))) {
        printf("FAIL: DllGetClassObject\n");
        return 1;
    }
    if (FAILED(cf->lpVtbl->CreateInstance(cf, NULL, &IID_IHTMLDocument2, (void **)&doc))) {
        printf("FAIL: CreateInstance\n");
        return 1;
    }
    if (FAILED(doc->lpVtbl->QueryInterface(doc, &IID_IHTMLDocument3, (void **)&doc3))) {
        printf("FAIL: QI IHTMLDocument3\n");
        return 1;
    }

    /* getElementById -> IHTMLElement */
    {
        BSTR id = SysAllocString(L"foo");
        HRESULT hr = doc3->lpVtbl->getElementById(doc3, id, &e);
        SysFreeString(id);
        printf("getElementById(foo) hr=0x%08lx elem=%s\n",
               (unsigned long)hr, pass(SUCCEEDED(hr) && e != NULL));
    }

    if (!e) return 1;

    {
        char *v = get_attr(e, L"data-x");
        printf("getAttribute(data-x) = '%s'  %s\n", v ? v : "(null)",
               pass(v && strcmp(v, "42") == 0));
        free(v);
    }
    {
        char *v = get_bstr(e, 0);
        printf("get_id            = '%s'  %s\n", v ? v : "(null)",
               pass(v && strcmp(v, "foo") == 0));
        free(v);
    }
    {
        char *v = get_bstr(e, 1);
        printf("get_tagName       = '%s'  %s\n", v ? v : "(null)",
               pass(v && strcmp(v, "DIV") == 0));
        free(v);
    }
    {
        char *v = get_bstr(e, 2);
        /* Turkish chars in UTF-8: ş = C5 9F, ı = C4 B1, ç = C3 A7, ö = C3 B6 */
        printf("get_innerText (utf-8) = '%s'  %s\n", v ? v : "(null)",
               pass(v && strcmp(v, "merhaba \xC5\x9F\xC4\xB1\xC3\xA7\xC3\xB6") == 0));
        free(v);
    }

    /* IDispatch: GetIDsOfNames("click") -> Invoke -> element should set data-clicked=1 */
    {
        LPOLESTR names[1];
        DISPID dispid = 0;
        DISPPARAMS dp;
        names[0] = (LPOLESTR)L"click";
        HRESULT hr = e->lpVtbl->GetIDsOfNames(e, &IID_NULL, names, 1, lcid, &dispid);
        printf("GetIDsOfNames(click) hr=0x%08lx dispid=%ld  %s\n",
               (unsigned long)hr, (long)dispid, pass(SUCCEEDED(hr) && dispid == 1));

        ZeroMemory(&dp, sizeof(dp));
        hr = e->lpVtbl->Invoke(e, dispid, &IID_NULL, lcid, DISPATCH_METHOD,
                               &dp, NULL, NULL, NULL);
        printf("Invoke(click) hr=0x%08lx  %s\n", (unsigned long)hr, pass(SUCCEEDED(hr)));
    }
    {
        char *v = get_attr(e, L"data-clicked");
        printf("getAttribute(data-clicked) = '%s'  %s\n", v ? v : "(null)",
               pass(v && strcmp(v, "1") == 0));
        free(v);
    }

    /* getElementsByTagName -> collection -> length / item */
    {
        IHTMLElementCollection *col = NULL;
        LONG len = 0;
        BSTR t = SysAllocString(L"div");
        HRESULT hr = doc3->lpVtbl->getElementsByTagName(doc3, t, &col);
        SysFreeString(t);
        printf("getElementsByTagName(div) hr=0x%08lx  %s\n", (unsigned long)hr,
               pass(SUCCEEDED(hr) && col != NULL));
        if (col) {
            col->lpVtbl->get_length(col, &len);
            printf("collection.length = %ld  %s\n", (long)len, pass(len == 2));
            {
                IHTMLElement *i0 = col_item_elem(col, 0);
                IHTMLElement *i1 = col_item_elem(col, 1);
                char *a0 = i0 ? get_attr(i0, L"data-x") : NULL;
                char *a1 = i1 ? get_attr(i1, L"data-y") : NULL;
                printf("item(0).data-x = '%s'  %s\n", a0 ? a0 : "(null)",
                       pass(a0 && strcmp(a0, "42") == 0));
                printf("item(1).data-y = '%s'  %s\n", a1 ? a1 : "(null)",
                       pass(a1 && strcmp(a1, "7") == 0));
                free(a0); free(a1);
                if (i0) i0->lpVtbl->Release(i0);
                if (i1) i1->lpVtbl->Release(i1);
            }
            col->lpVtbl->Release(col);
        }
    }

    /* IHTMLInputElement: value / type / name / checked + Turkish put_value */
    {
        IHTMLElement *ie = NULL;
        IHTMLInputElement *inp = NULL;
        BSTR id = SysAllocString(L"inp");
        doc3->lpVtbl->getElementById(doc3, id, &ie);
        SysFreeString(id);
        if (ie)
            ie->lpVtbl->QueryInterface(ie, &IID_IHTMLInputElement, (void **)&inp);
        printf("QI IHTMLInputElement = %s\n", pass(inp != NULL));
        if (inp) {
            char *v = input_value(inp);
            printf("input.value = '%s'  %s\n", v ? v : "(null)",
                   pass(v && strcmp(v, "hi") == 0));
            free(v);
            {
                BSTR s = NULL;
                char *x;
                inp->lpVtbl->get_type(inp, &s); x = utf8_from_bstr(s); SysFreeString(s);
                printf("input.type  = '%s'  %s\n", x ? x : "(null)",
                       pass(x && strcmp(x, "text") == 0));
                free(x);
            }
            {
                BSTR s = NULL;
                char *x;
                inp->lpVtbl->get_name(inp, &s); x = utf8_from_bstr(s); SysFreeString(s);
                printf("input.name  = '%s'  %s\n", x ? x : "(null)",
                       pass(x && strcmp(x, "uname") == 0));
                free(x);
            }
            {
                BSTR nv = SysAllocString(L"\u015f\u0131\u00e7\u00f6");
                char *v2;
                inp->lpVtbl->put_value(inp, nv);
                SysFreeString(nv);
                v2 = input_value(inp);
                printf("put_value(şıçö) -> value = '%s'  %s\n", v2 ? v2 : "(null)",
                       pass(v2 && strcmp(v2, "\xC5\x9F\xC4\xB1\xC3\xA7\xC3\xB6") == 0));
                free(v2);
            }
            inp->lpVtbl->Release(inp);
        }
        if (ie) ie->lpVtbl->Release(ie);

        /* checkbox checked */
        {
            IHTMLElement *ce = NULL;
            IHTMLInputElement *chk = NULL;
            VARIANT_BOOL c = VARIANT_TRUE;
            BSTR cid = SysAllocString(L"chk");
            doc3->lpVtbl->getElementById(doc3, cid, &ce);
            SysFreeString(cid);
            if (ce)
                ce->lpVtbl->QueryInterface(ce, &IID_IHTMLInputElement, (void **)&chk);
            if (chk) {
                chk->lpVtbl->get_checked(chk, &c);
                printf("checkbox.checked (initial) = %s\n", pass(c == VARIANT_FALSE));
                chk->lpVtbl->put_checked(chk, VARIANT_TRUE);
                chk->lpVtbl->get_checked(chk, &c);
                printf("checkbox.checked (after)   = %s\n", pass(c == VARIANT_TRUE));
                chk->lpVtbl->Release(chk);
            }
            if (ce) ce->lpVtbl->Release(ce);
        }
    }

    /* createElement + get_body */
    {
        IHTMLElement *ne = NULL;
        BSTR tag = SysAllocString(L"p");
        HRESULT hr = doc->lpVtbl->createElement(doc, tag, &ne);
        SysFreeString(tag);
        printf("createElement(p) hr=0x%08lx  %s\n", (unsigned long)hr,
               pass(SUCCEEDED(hr) && ne != NULL));
        if (ne) {
            char *k;
            set_attr_str(ne, L"data-k", L"9");
            k = get_attr(ne, L"data-k");
            printf("created element data-k = '%s'  %s\n", k ? k : "(null)",
                   pass(k && strcmp(k, "9") == 0));
            free(k);
            ne->lpVtbl->Release(ne);
        }
        {
            IHTMLElement *body = NULL;
            char *bt;
            doc->lpVtbl->get_body(doc, &body);
            bt = body ? get_bstr(body, 1) : NULL;
            printf("get_body tagName = '%s'  %s\n", bt ? bt : "(null)",
                   pass(bt && strcmp(bt, "BODY") == 0));
            free(bt);
            if (body) body->lpVtbl->Release(body);
        }
    }

    /* Milestone 7: get_style -> IHTMLStyle cssText */
    {
        IHTMLStyle *st = NULL;
        HRESULT hr = e->lpVtbl->get_style(e, &st);
        printf("get_style hr=0x%08lx  %s\n", (unsigned long)hr, pass(SUCCEEDED(hr) && st != NULL));
        if (st) {
            BSTR css = SysAllocString(L"background-color: rgb(1, 2, 3); color: red");
            st->lpVtbl->put_cssText(st, css);
            SysFreeString(css);
            {
                BSTR out = NULL;
                char *x;
                st->lpVtbl->get_cssText(st, &out);
                x = utf8_from_bstr(out); SysFreeString(out);
                printf("style.cssText contains background-color = %s\n",
                       pass(x && strstr(x, "background-color") != NULL));
                free(x);
            }
            st->lpVtbl->Release(st);
        }
    }

    /* Milestone 7: IHTMLDOMNode QI + appendChild + childNodes */
    {
        IHTMLDOMNode *enode = NULL, *child_node = NULL;
        IHTMLElement *span = NULL;
        HRESULT hr = e->lpVtbl->QueryInterface(e, &IID_IHTMLDOMNode, (void **)&enode);
        printf("QI IHTMLDOMNode = %s\n", pass(SUCCEEDED(hr) && enode != NULL));
        if (enode) {
            LONG nt = -1;
            BSTR nn = NULL;
            char *x;
            enode->lpVtbl->get_nodeType(enode, &nt);
            printf("nodeType = %ld, %s\n", (long)nt, pass(nt == 1));
            enode->lpVtbl->get_nodeName(enode, &nn);
            x = utf8_from_bstr(nn); SysFreeString(nn);
            printf("nodeName = '%s'  %s\n", x ? x : "(null)",
                   pass(x && strcmp(x, "DIV") == 0));
            free(x);

            /* create a <span> and append it to the div */
            {
                BSTR tag = SysAllocString(L"span");
                doc->lpVtbl->createElement(doc, tag, &span);
                SysFreeString(tag);
                if (span && SUCCEEDED(span->lpVtbl->QueryInterface(span, &IID_IHTMLDOMNode,
                                                                   (void **)&child_node))) {
                    IHTMLDOMNode *out = NULL;
                    IDispatch *child_col = NULL;
                    LONG n = 0;
                    hr = enode->lpVtbl->appendChild(enode, child_node, &out);
                    printf("appendChild(span) hr=0x%08lx  %s\n", (unsigned long)hr,
                           pass(SUCCEEDED(hr) && out != NULL));
                    if (out) out->lpVtbl->Release(out);
                    enode->lpVtbl->get_childNodes(enode, &child_col);
                    if (child_col) {
                        IHTMLElementCollection *cc = (IHTMLElementCollection *)child_col;
                        cc->lpVtbl->get_length(cc, &n);
                        printf("childNodes.length after append = %ld  %s\n", (long)n,
                               pass(n >= 2));
                        cc->lpVtbl->Release(cc);
                    }
                }
            }
            enode->lpVtbl->Release(enode);
        }
        if (child_node) child_node->lpVtbl->Release(child_node);
        if (span) span->lpVtbl->Release(span);
    }

    /* Milestone 7: outerHTML get/put */
    {
        BSTR out = NULL;
        char *x;
        e->lpVtbl->get_outerHTML(e, &out);
        x = utf8_from_bstr(out); SysFreeString(out);
        printf("get_outerHTML contains 'foo' = %s\n",
               pass(x && strstr(x, "foo") != NULL));
        free(x);
    }

    /* Milestone 7: get__newEnum -> IEnumVARIANT (foreach over a collection) */
    {
        IHTMLElementCollection *col = NULL;
        BSTR t = SysAllocString(L"div");
        doc3->lpVtbl->getElementsByTagName(doc3, t, &col);
        SysFreeString(t);
        if (col) {
            IUnknown *unk = NULL;
            IEnumVARIANT *en = NULL;
            HRESULT hr = col->lpVtbl->get__newEnum(col, &unk);
            printf("get__newEnum hr=0x%08lx  %s\n", (unsigned long)hr,
                   pass(SUCCEEDED(hr) && unk != NULL));
            if (unk) {
                hr = unk->lpVtbl->QueryInterface(unk, &IID_IEnumVARIANT, (void **)&en);
                printf("QI IEnumVARIANT = %s\n", pass(SUCCEEDED(hr) && en != NULL));
                if (en) {
                    VARIANT v[2];
                    ULONG fetched = 0;
                    int i;
                    ZeroMemory(v, sizeof(v));
                    hr = en->lpVtbl->Next(en, 2, v, &fetched);
                    printf("IEnumVARIANT.Next(2) fetched=%lu  %s\n", (unsigned long)fetched,
                           pass(fetched == 2));
                    for (i = 0; i < (int)fetched; i++) {
                        char *a = NULL;
                        if (V_VT(&v[i]) == VT_DISPATCH) {
                            IHTMLElement *ie = (IHTMLElement *)V_DISPATCH(&v[i]);
                            a = get_attr(ie, i == 0 ? L"data-x" : L"data-y");
                        }
                        printf("enum[%d].%s = '%s'  %s\n", i, i == 0 ? "data-x" : "data-y",
                               a ? a : "(null)",
                               pass(a && (i == 0 ? strcmp(a, "42") == 0 : strcmp(a, "7") == 0)));
                        free(a);
                        VariantClear(&v[i]);
                    }
                    en->lpVtbl->Release(en);
                }
                unk->lpVtbl->Release(unk);
            }
            col->lpVtbl->Release(col);
        }
    }

    /* rcache.c: a scan over a list (TEBEOS's `from HtmlElement k in col where k.Id == ...`)
     * comes back in a few requests, and nothing it caches outlives a write or a second */
    {
        typedef LONG (__cdecl *calls_fn)(void);
        calls_fn pcalls = (calls_fn)(void *)GetProcAddress(mod, "shim_dom_calls");
        IHTMLElement *body = NULL, *s5 = NULL, *s7 = NULL, *kept[600];
        IHTMLElementCollection *col = NULL;
        BSTR where, h, t;
        WCHAR *buf = (WCHAR *)malloc(600 * 64 * sizeof(WCHAR)), *q;
        LONG len = 0, c0, c1, k;
        int bad = 0, n;

        ZeroMemory(kept, sizeof(kept));
        doc->lpVtbl->get_body(doc, &body);
        for (q = buf, n = 0; n < 600; n++)
            q += swprintf(q, 64, L"<kbd id='s%d' class='c'>x</kbd>", n);
        where = SysAllocString(L"beforeEnd");
        h = SysAllocString(buf);
        if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
        SysFreeString(h);
        t = SysAllocString(L"kbd");
        doc3->lpVtbl->getElementsByTagName(doc3, t, &col);
        SysFreeString(t);
        c0 = pcalls ? pcalls() : 0;
        for (k = 0; col && (col->lpVtbl->get_length(col, &len), k < len); k++) {
            VARIANT vi, v0;
            IDispatch *d = NULL;
            IHTMLElement *el = NULL;
            char want[16];
            VariantInit(&vi); VariantInit(&v0);
            V_VT(&vi) = VT_I4; V_I4(&vi) = k;
            V_VT(&v0) = VT_I4; V_I4(&v0) = 0;
            col->lpVtbl->item(col, vi, v0, &d);
            if (d) { d->lpVtbl->QueryInterface(d, &IID_IHTMLElement, (void **)&el); d->lpVtbl->Release(d); }
            snprintf(want, sizeof(want), "s%ld", (long)k);
            if (!el) { bad++; continue; }
            {
                char *id = get_bstr(el, 0), *ga = get_attr(el, L"id"), *cn = get_attr(el, L"className");
                if (!id || strcmp(id, want) || !ga || strcmp(ga, want) || !cn || strcmp(cn, "c"))
                    bad++;
                free(id); free(ga); free(cn);
            }
            if (k < 600) { kept[k] = el; el->lpVtbl->AddRef(el); }
            if (k == 5) { s5 = el; el->lpVtbl->AddRef(el); }
            if (k == 7) { s7 = el; el->lpVtbl->AddRef(el); }
            el->lpVtbl->Release(el);
        }
        c1 = pcalls ? pcalls() : 0;
        printf("scan of %ld elements (length, item, id, getAttribute id/className each): %ld wrong, %ld requests  %s\n",
               (long)len, (long)bad, (long)(c1 - c0), pass(len == 600 && bad == 0 && pcalls && c1 - c0 < 40));

        /* TEBEOS's other pattern: a list materialised earlier (ToList), whose ids it compares
         * later, after the cache's second -- read in batches of the elements stashed together */
        Sleep(1100);
        c0 = pcalls ? pcalls() : 0;
        bad = 0;
        for (k = 0; k < 600; k++) {
            char want[16], *id;
            snprintf(want, sizeof(want), "s%ld", (long)k);
            id = kept[k] ? get_bstr(kept[k], 0) : NULL;
            if (!id || strcmp(id, want))
                bad++;
            free(id);
        }
        c1 = pcalls ? pcalls() : 0;
        printf("ids of 600 kept elements, read after the cache expired: %ld wrong, %ld requests  %s\n",
               (long)bad, (long)(c1 - c0), pass(bad == 0 && pcalls && c1 - c0 < 10));
        for (k = 0; k < 600; k++)
            if (kept[k]) kept[k]->lpVtbl->Release(kept[k]);

        /* a write is seen at once */
        if (s5) {
            VARIANT nv;
            char *id;
            BSTR a = SysAllocString(L"id");
            V_VT(&nv) = VT_BSTR; V_BSTR(&nv) = SysAllocString(L"changed");
            s5->lpVtbl->setAttribute(s5, a, nv, 0);
            VariantClear(&nv); SysFreeString(a);
            id = get_bstr(s5, 0);
            printf("id after the vendor's setAttribute = '%s'  %s\n", id ? id : "(null)", pass(id && !strcmp(id, "changed")));
            free(id);
        }
        h = SysAllocString(L"<kbd id='s600'>y</kbd>");
        if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
        SysFreeString(h);
        len = 0;
        if (col) col->lpVtbl->get_length(col, &len);
        printf("list length after the vendor adds an element = %ld  %s\n", (long)len, pass(len == 601));

        /* the page's own script: seen once the cache's second is over */
        h = SysAllocString(L"<img src='data:image/gif;base64,R0lGODlhAQABAAAAACw=' "
                           L"onerror=\"setTimeout(function(){document.getElementById('s7').id='late';},300)\" "
                           L"onload=\"setTimeout(function(){document.getElementById('s7').id='late';},300)\">");
        if (body) body->lpVtbl->insertAdjacentHTML(body, where, h);
        SysFreeString(h);
        if (col) {   /* refill the cache */
            VARIANT vi, v0;
            IDispatch *d = NULL;
            VariantInit(&vi); VariantInit(&v0);
            V_VT(&vi) = VT_I4; V_I4(&vi) = 0; V_VT(&v0) = VT_I4; V_I4(&v0) = 0;
            col->lpVtbl->item(col, vi, v0, &d);
            if (d) d->lpVtbl->Release(d);
        }
        Sleep(1300);
        if (s7) {
            char *id = get_bstr(s7, 0);
            printf("id changed by page script, read after 1.3 s = '%s'  %s\n", id ? id : "(null)", pass(id && !strcmp(id, "late")));
            free(id);
        }
        SysFreeString(where);
        free(buf);
        if (s5) s5->lpVtbl->Release(s5);
        if (s7) s7->lpVtbl->Release(s7);
        if (col) col->lpVtbl->Release(col);
        if (body) body->lpVtbl->Release(body);
    }

    /* IE page zoom (OLECMDID_OPTICAL_ZOOM): TEBEOS sets it on every prescription page and a
     * refusal ends its page handler (.NET throws) */
    {
        IOleCommandTarget *ct = NULL;
        IHTMLWindow2 *win = NULL;
        VARIANT in, out, r;
        BSTR code = SysAllocString(L"document.title='z'+document.documentElement.style.zoom"), lang = SysAllocString(L"JScript"), title = NULL;
        HRESULT hz = E_FAIL, hr2 = E_FAIL;
        OLECMD q[2] = { { OLECMDID_OPTICAL_ZOOM, 0 }, { OLECMDID_OPTICAL_GETZOOMRANGE, 0 } };
        char *t;

        doc->lpVtbl->QueryInterface(doc, &IID_IOleCommandTarget, (void **)&ct);
        doc->lpVtbl->get_parentWindow(doc, &win);
        VariantInit(&in); VariantInit(&out); VariantInit(&r);
        V_VT(&in) = VT_I4; V_I4(&in) = 70;
        if (ct) {
            ct->lpVtbl->QueryStatus(ct, NULL, 2, q, NULL);
            hz = ct->lpVtbl->Exec(ct, NULL, OLECMDID_OPTICAL_ZOOM, OLECMDEXECOPT_DONTPROMPTUSER, &in, &out);
        }
        printf("optical zoom 70: hr=0x%08lx out=%ld supported=%d  %s\n", (unsigned long)hz,
               V_VT(&out) == VT_I4 ? (long)V_I4(&out) : -1L, !!(q[0].cmdf & OLECMDF_SUPPORTED),
               pass(hz == S_OK && V_VT(&out) == VT_I4 && V_I4(&out) == 70 && (q[0].cmdf & OLECMDF_SUPPORTED)));
        if (win) win->lpVtbl->execScript(win, code, lang, &r);
        doc->lpVtbl->get_title(doc, &title);
        t = utf8_from_bstr(title);
        printf("page zoom after 70 = '%s'  %s\n", t ? t : "(null)", pass(t && !strcmp(t, "z70%")));
        free(t); SysFreeString(title); title = NULL;
        V_I4(&in) = 100;
        VariantClear(&out);
        if (ct) hr2 = ct->lpVtbl->Exec(ct, NULL, OLECMDID_OPTICAL_ZOOM, OLECMDEXECOPT_DODEFAULT, &in, NULL);
        if (win) win->lpVtbl->execScript(win, code, lang, &r);
        doc->lpVtbl->get_title(doc, &title);
        t = utf8_from_bstr(title);
        printf("page zoom after 100 = '%s' hr=0x%08lx  %s\n", t ? t : "(null)", (unsigned long)hr2, pass(hr2 == S_OK && t && !strcmp(t, "z")));
        free(t); SysFreeString(title);
        SysFreeString(code); SysFreeString(lang);
        if (win) win->lpVtbl->Release(win);
        if (ct) ct->lpVtbl->Release(ct);
    }

    /* Printing (OLECMDID_PRINT): advertised always; sent to the default printer only when
     * SHIM_PRINT_TEST is set (a real print job: run it with the queue holding jobs). */
    {
        IOleCommandTarget *ct = NULL;
        OLECMD q[2] = { { OLECMDID_PRINT, 0 }, { OLECMDID_PRINTPREVIEW, 0 } };
        doc->lpVtbl->QueryInterface(doc, &IID_IOleCommandTarget, (void **)&ct);
        if (ct)
            ct->lpVtbl->QueryStatus(ct, NULL, 2, q, NULL);
        printf("print commands supported: %d %d  %s\n", !!(q[0].cmdf & OLECMDF_ENABLED),
               !!(q[1].cmdf & OLECMDF_ENABLED), pass((q[0].cmdf & OLECMDF_ENABLED) && (q[1].cmdf & OLECMDF_ENABLED)));
        if (ct && getenv("SHIM_PRINT_TEST")) {
            HRESULT hp = ct->lpVtbl->Exec(ct, NULL, OLECMDID_PRINT, OLECMDEXECOPT_DONTPROMPTUSER, NULL, NULL);
            printf("print to the default printer: hr=0x%08lx (see PRINT in the shim log)\n", (unsigned long)hp);
        }
        if (ct && getenv("SHIM_PREVIEW_TEST")) {
            HRESULT hp = ct->lpVtbl->Exec(ct, NULL, OLECMDID_PRINTPREVIEW, OLECMDEXECOPT_DONTPROMPTUSER, NULL, NULL);
            printf("print preview: hr=0x%08lx (see PRINT in the shim log)\n", (unsigned long)hp);
        }
        if (ct) ct->lpVtbl->Release(ct);
    }

    e->lpVtbl->Release(e);
    doc3->lpVtbl->Release(doc3);
    doc->lpVtbl->Release(doc);
    cf->lpVtbl->Release(cf);
    FreeLibrary(mod);
    printf("%s\n", failures ? "FAIL" : "PASS (CDP-backed DOM surface)");
    return failures ? 1 : 0;
}
