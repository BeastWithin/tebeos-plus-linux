/* dispmap.h -- mshtml dispinterfaces (DispHTMLDocument, DispHTMLTableRow, ...) on shim objects.
 *
 * The .NET interop coclass types the vendor casts to -- `DomDocument as HTMLDocument`,
 * `(HTMLTableRow)he_row.DomElement`, `(HTMLTableCell)`, `(HTMLBody)` -- are dispinterfaces:
 * the CLR QueryInterfaces for the dispinterface IID and calls IDispatch::Invoke with mshtml's
 * fixed DISPIDs. The shim's IDispatch uses dynamic DISPIDs, so a QI for one of these IIDs
 * returns a small wrapper that maps the fixed DISPID to the member name (dispmap.c, generated
 * by gen-dispmap.py) and forwards by name to the object's own IDispatch.
 */
#ifndef SHIM_DISPMAP_H
#define SHIM_DISPMAP_H

#define COBJMACROS
#include <windows.h>
#include <oaidl.h>
#include <stddef.h>

typedef enum { DISP_DOC, DISP_ELEM } DispKind;

typedef struct { DISPID id; const char *name; } DispName;

typedef struct {
    GUID diid;
    const char *iface_name;
    DispKind kind;
    const DispName *names;   /* this dispinterface's own members, sorted by DISPID */
    size_t n_names;
    int elem_common;         /* also has the members every DispHTML*Element shares */
} DispIface;

/* The dispinterface `kind` objects answer for riid, or NULL. */
const DispIface *dispmap_find(DispKind kind, REFIID riid);

/* The member name for a fixed DISPID of that dispinterface, or NULL. */
const char *dispmap_name(const DispIface *di, DISPID id);

/* QI helper: if riid is a dispinterface of `kind`, *ppv = a wrapper over `inner` (the
 * object's own IDispatch, which resolves names via GetIDsOfNames) and S_OK; else
 * E_NOINTERFACE with *ppv untouched. */
HRESULT dispwrap_query(DispKind kind, IDispatch *inner, REFIID riid, void **ppv);

#endif /* SHIM_DISPMAP_H */
