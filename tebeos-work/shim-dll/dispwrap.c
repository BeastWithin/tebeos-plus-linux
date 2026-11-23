/* dispwrap.c -- the dispinterface wrapper (see dispmap.h). */

#include "dispmap.h"
#include "log.h"
#include <stdlib.h>

typedef struct {
    IDispatch IDispatch_iface;
    LONG ref;
    IDispatch *inner;
    const DispIface *di;
} DispWrap;

static inline DispWrap *wrap_from(IDispatch *t) { return CONTAINING_RECORD(t, DispWrap, IDispatch_iface); }

static HRESULT STDMETHODCALLTYPE wrap_qi(IDispatch *This, REFIID riid, void **ppv)
{
    DispWrap *w = wrap_from(This);

    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &w->di->diid)) {
        *ppv = This;
        IDispatch_AddRef(This);
        return S_OK;
    }
    /* IUnknown (COM identity) and every vtable interface come from the object itself */
    return IDispatch_QueryInterface(w->inner, riid, ppv);
}

static ULONG STDMETHODCALLTYPE wrap_addref(IDispatch *This)
{
    return InterlockedIncrement(&wrap_from(This)->ref);
}

static ULONG STDMETHODCALLTYPE wrap_release(IDispatch *This)
{
    DispWrap *w = wrap_from(This);
    LONG r = InterlockedDecrement(&w->ref);
    if (!r) {
        IDispatch_Release(w->inner);
        free(w);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE wrap_gettypeinfocount(IDispatch *This, UINT *n)
{
    if (n)
        *n = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wrap_gettypeinfo(IDispatch *This, UINT i, LCID l, ITypeInfo **p)
{
    if (p)
        *p = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE wrap_getidsofnames(IDispatch *This, REFIID riid, LPOLESTR *names,
                                                     UINT n, LCID lcid, DISPID *ids)
{
    return IDispatch_GetIDsOfNames(wrap_from(This)->inner, riid, names, n, lcid, ids);
}

static HRESULT STDMETHODCALLTYPE wrap_invoke(IDispatch *This, DISPID id, REFIID riid, LCID lcid,
                                             WORD flags, DISPPARAMS *dp, VARIANT *res,
                                             EXCEPINFO *ei, UINT *argerr)
{
    DispWrap *w = wrap_from(This);
    const char *name;
    WCHAR wname[128];
    LPOLESTR names[1];
    DISPID dyn;
    HRESULT hr;

    if (id == DISPID_VALUE)
        return IDispatch_Invoke(w->inner, id, riid, lcid, flags, dp, res, ei, argerr);
    name = dispmap_name(w->di, id);
    if (!name) {
        /* Not a typelib DISPID: an id our own GetIDsOfNames handed out (it forwards to the
         * object's dynamic names, small numbers). .NET's `dynamic` binder does exactly that,
         * e.g. TEBEOS's HtmlElement_Helper on table rows; refusing them failed every such call. */
        hr = IDispatch_Invoke(w->inner, id, riid, lcid, flags, dp, res, ei, argerr);
        if (hr == DISP_E_MEMBERNOTFOUND)
            shim_log("DISPWRAP %s: no member for DISPID %ld", w->di->iface_name, (long)id);
        return hr;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, ARRAYSIZE(wname)))
        return DISP_E_MEMBERNOTFOUND;
    names[0] = wname;
    hr = IDispatch_GetIDsOfNames(w->inner, &IID_NULL, names, 1, lcid, &dyn);
    if (FAILED(hr))
        return DISP_E_MEMBERNOTFOUND;
    return IDispatch_Invoke(w->inner, dyn, riid, lcid, flags, dp, res, ei, argerr);
}

static IDispatchVtbl g_wrap_vtbl = {
    wrap_qi, wrap_addref, wrap_release, wrap_gettypeinfocount,
    wrap_gettypeinfo, wrap_getidsofnames, wrap_invoke,
};

HRESULT dispwrap_query(DispKind kind, IDispatch *inner, REFIID riid, void **ppv)
{
    const DispIface *di = dispmap_find(kind, riid);
    DispWrap *w;

    if (!di)
        return E_NOINTERFACE;
    if (!(w = (DispWrap *)calloc(1, sizeof(*w))))
        return E_OUTOFMEMORY;
    w->IDispatch_iface.lpVtbl = &g_wrap_vtbl;
    w->ref = 1;
    w->inner = inner;
    IDispatch_AddRef(inner);
    w->di = di;
    *ppv = &w->IDispatch_iface;
    return S_OK;
}
