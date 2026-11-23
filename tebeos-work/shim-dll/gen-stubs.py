#!/usr/bin/env python3
"""
gen-stubs.py -- emit stubs.c/stubs.h for the shim COM objects.

Parses Wine's generated interface headers (mshtml.h, docobj.h, urlmon.h, oleidl.h,
ocidl.h) and emits, for each interface on each OBJECT, the vtables + per-interface
QueryInterface/AddRef/Release wrappers. A vtable slot is filled, in order of preference:

  1. REAL_IMPLS -- a hand-written implementation in shim.c / docobj.c / element.c /
     window.c (signature copied verbatim from the header);
  2. an AUTO accessor -- on the IHTML* interfaces of an object with a `js` target, every
     plain property getter or setter (get_X/put_X over BSTR, LONG, short, float,
     VARIANT_BOOL, VARIANT, or a getter returning an element, collection, node or
     IDispatch) becomes a round-trip on the same-named JS property of that target in the
     object's page (jsprop.c), and every method without arguments a call of the same-named
     JS method. `on*` event-handler properties read as null and ignore writes;
  3. an E_NOTIMPL stub that logs its name the first time it is called (log.c), so a run
     of the real app lists exactly which methods it still needs.

Each OBJECT entry: ctype (C struct name), qi/addref/release (shared lifetime fns),
js (JS target expression; `%ld` is replaced by the object's `handle`; None = no auto
accessors), page (C expression for the object's Page, with `o` the object) and
interfaces [(header, iface), ...].
"""

import os
import re

INC = os.environ.get("WINE_INC", os.path.expanduser("~/toolchain/wine-11.18/include"))

ELEM_JS = "window.__shim_els[%ld]"

OBJECTS = [
    {
        "ctype": "HTMLDocument",
        "qi": "html_doc_qi",
        "addref": "html_doc_addref",
        "release": "html_doc_release",
        "js": "document",
        "page": "doc_page(o)",
        "interfaces": [
            ("mshtml.h", "IHTMLDocument2"),
            ("mshtml.h", "IHTMLDocument3"),
            ("mshtml.h", "IHTMLDocument4"),
            ("mshtml.h", "IHTMLDocument5"),
            ("urlmon.h", "IPersistMoniker"),
            ("ocidl.h", "IPersistStreamInit"),
            ("oleidl.h", "IOleObject"),
            ("docobj.h", "IOleDocument"),
            ("docobj.h", "IOleDocumentView"),
            ("oleidl.h", "IOleInPlaceObject"),
            ("oleidl.h", "IOleInPlaceActiveObject"),
            ("docobj.h", "IOleCommandTarget"),
            ("ocidl.h", "IProvideClassInfo"),
            ("ocidl.h", "IConnectionPointContainer"),
        ],
    },
    {
        "ctype": "HtmlElement",
        "qi": "html_elem_qi",
        "addref": "html_elem_addref",
        "release": "html_elem_release",
        "js": ELEM_JS,
        "page": "o->page",
        "interfaces": [
            ("mshtml.h", "IHTMLElement"),
            ("mshtml.h", "IHTMLElement2"),
            ("mshtml.h", "IHTMLElement3"),
            ("mshtml.h", "IHTMLElement4"),
            ("mshtml.h", "IHTMLInputElement"),
            ("mshtml.h", "IHTMLInputTextElement"),
            ("mshtml.h", "IHTMLSelectElement"),
            ("mshtml.h", "IHTMLOptionElement"),
            ("mshtml.h", "IHTMLTextAreaElement"),
            ("mshtml.h", "IHTMLScriptElement"),
            ("mshtml.h", "IHTMLFormElement"),
            ("mshtml.h", "IHTMLAnchorElement"),
            ("mshtml.h", "IHTMLImgElement"),
            ("mshtml.h", "IHTMLDOMNode"),
        ],
    },
    {
        "ctype": "HtmlStyle",
        "qi": "html_style_qi",
        "addref": "html_style_addref",
        "release": "html_style_release",
        "js": ELEM_JS + ".style",
        "page": "o->page",
        "interfaces": [
            ("mshtml.h", "IHTMLStyle"),
        ],
    },
    {
        "ctype": "HtmlElementCollection",
        "qi": "html_col_qi",
        "addref": "html_col_addref",
        "release": "html_col_release",
        "js": ELEM_JS,
        "page": "o->page",
        "interfaces": [
            ("mshtml.h", "IHTMLElementCollection"),
        ],
    },
    {
        "ctype": "HtmlWindow",
        "qi": "html_win_qi",
        "addref": "html_win_addref",
        "release": "html_win_release",
        "js": "window",
        "page": "doc_page(o->doc)",
        "interfaces": [
            ("mshtml.h", "IHTMLWindow2"),
            ("mshtml.h", "IHTMLWindow3"),
        ],
    },
    {
        "ctype": "HtmlLocation",
        "qi": "html_loc_qi",
        "addref": "html_loc_addref",
        "release": "html_loc_release",
        "js": "location",
        "page": "doc_page(o->doc)",
        "interfaces": [
            ("mshtml.h", "IHTMLLocation"),
        ],
    },
    {
        "ctype": "HtmlEventObj",
        "qi": "html_evo_qi",
        "addref": "html_evo_addref",
        "release": "html_evo_release",
        "js": None,
        "interfaces": [
            ("mshtml.h", "IHTMLEventObj"),
        ],
    },
    {
        "ctype": "HtmlTxtRange",
        "qi": "html_txr_qi",
        "addref": "html_txr_addref",
        "release": "html_txr_release",
        "js": None,
        "interfaces": [
            ("mshtml.h", "IHTMLTxtRange"),
        ],
    },
]

REAL = {"QueryInterface", "AddRef", "Release"}

# JS property names that differ from the IE COM property name.
JS_NAME = {
    "styleFloat": "cssFloat",
}
# ... on one interface only. IE's <img> answers `href` with its src: the app finds the Medula
# captcha by `img.href.Contains("SayiUretenImageYeniServlet")`.
JS_NAME_IFACE = {
    ("IHTMLImgElement", "href"): "src",
}

# Methods implemented by hand, keyed as (interface, method) -> C function name. The
# generator emits an `extern` decl for each and wires the vtable slot to it.
REAL_IMPLS = {
    # IHTMLDocument2 (shim.c: state; window.c: IDispatch, window, location, script;
    # element.c: element creation and document.write)
    ("IHTMLDocument2", "GetTypeInfoCount"): "html_doc_GetTypeInfoCount",
    ("IHTMLDocument2", "GetTypeInfo"):      "html_doc_GetTypeInfo",
    ("IHTMLDocument2", "GetIDsOfNames"):    "html_doc_GetIDsOfNames",
    ("IHTMLDocument2", "Invoke"):           "html_doc_Invoke",
    ("IHTMLDocument2", "get_readyState"):   "html_doc_get_readyState",
    ("IHTMLDocument2", "get_URL"):          "html_doc_get_URL",
    ("IHTMLDocument2", "get_parentWindow"): "html_doc_get_parentWindow",
    ("IHTMLDocument2", "get_location"):     "html_doc_get_location",
    ("IHTMLDocument2", "get_Script"):       "html_doc_get_Script",
    ("IHTMLDocument2", "createElement"):    "html_doc_createElement",
    ("IHTMLDocument2", "write"):            "html_doc_write",
    ("IHTMLDocument2", "writeln"):          "html_doc_writeln",
    ("IHTMLDocument3", "getElementById"):       "html_doc3_getElementById",
    ("IHTMLDocument3", "getElementsByTagName"): "html_doc3_getElementsByTagName",
    ("IHTMLDocument3", "getElementsByName"):    "html_doc3_getElementsByName",
    ("IHTMLDocument3", "attachEvent"):          "html_doc3_attachEvent",
    ("IHTMLDocument3", "detachEvent"):          "html_doc3_detachEvent",
    ("IHTMLDocument4", "focus"):                "html_doc4_focus",

    # persistence (shim.c)
    ("IPersistMoniker",  "GetClassID"):     "html_doc_persist_moniker_GetClassID",
    ("IPersistMoniker",  "IsDirty"):        "html_doc_persist_moniker_IsDirty",
    ("IPersistMoniker",  "Load"):           "html_doc_persist_moniker_Load",
    ("IPersistStreamInit", "GetClassID"):   "html_doc_persist_stream_GetClassID",
    ("IPersistStreamInit", "IsDirty"):      "html_doc_persist_stream_IsDirty",
    ("IPersistStreamInit", "InitNew"):      "html_doc_persist_init_new",
    ("IPersistStreamInit", "Load"):         "html_doc_persist_stream_Load",
    ("IPersistStreamInit", "Save"):         "html_doc_persist_stream_Save",

    # DocObject hosting (docobj.c)
    ("IOleObject", "SetClientSite"):   "html_doc_ole_SetClientSite",
    ("IOleObject", "GetClientSite"):   "html_doc_ole_GetClientSite",
    ("IOleObject", "GetUserClassID"):  "html_doc_ole_GetUserClassID",
    ("IOleObject", "DoVerb"):          "html_doc_ole_DoVerb",
    ("IOleObject", "Close"):           "html_doc_ole_Close",
    ("IOleDocument", "CreateView"):    "html_doc_doc_CreateView",
    ("IOleDocumentView", "SetInPlaceSite"): "html_doc_view_SetInPlaceSite",
    ("IOleDocumentView", "GetInPlaceSite"): "html_doc_view_GetInPlaceSite",
    ("IOleDocumentView", "GetDocument"):    "html_doc_view_GetDocument",
    ("IOleDocumentView", "SetRect"):        "html_doc_view_SetRect",
    ("IOleDocumentView", "GetRect"):        "html_doc_view_GetRect",
    ("IOleDocumentView", "Show"):           "html_doc_view_Show",
    ("IOleDocumentView", "UIActivate"):     "html_doc_view_UIActivate",
    ("IOleDocumentView", "CloseView"):      "html_doc_view_CloseView",
    ("IOleInPlaceObject", "GetWindow"):            "html_doc_ipo_GetWindow",
    ("IOleInPlaceObject", "ContextSensitiveHelp"): "html_doc_ipo_ContextSensitiveHelp",
    ("IOleInPlaceObject", "InPlaceDeactivate"):    "html_doc_ipo_InPlaceDeactivate",
    ("IOleInPlaceObject", "UIDeactivate"):         "html_doc_ipo_UIDeactivate",
    ("IOleInPlaceObject", "SetObjectRects"):       "html_doc_ipo_SetObjectRects",
    ("IOleInPlaceObject", "ReactivateAndUndo"):    "html_doc_ipo_ReactivateAndUndo",
    ("IOleInPlaceActiveObject", "GetWindow"):             "html_doc_ipao_GetWindow",
    ("IOleInPlaceActiveObject", "ContextSensitiveHelp"):  "html_doc_ipao_ContextSensitiveHelp",
    ("IOleInPlaceActiveObject", "TranslateAccelerator"):  "html_doc_ipao_TranslateAccelerator",
    ("IOleInPlaceActiveObject", "OnFrameWindowActivate"): "html_doc_ipao_OnFrameWindowActivate",
    ("IOleInPlaceActiveObject", "OnDocWindowActivate"):   "html_doc_ipao_OnDocWindowActivate",
    ("IOleInPlaceActiveObject", "ResizeBorder"):          "html_doc_ipao_ResizeBorder",
    ("IOleInPlaceActiveObject", "EnableModeless"):        "html_doc_ipao_EnableModeless",
    ("IOleCommandTarget", "QueryStatus"): "html_doc_cmd_QueryStatus",
    ("IOleCommandTarget", "Exec"):        "html_doc_cmd_Exec",

    # IPropertyNotifySink for ieframe's ready-state tracking (window.c)
    ("IConnectionPointContainer", "EnumConnectionPoints"): "html_doc_cpc_EnumConnectionPoints",
    ("IConnectionPointContainer", "FindConnectionPoint"):  "html_doc_cpc_FindConnectionPoint",

    # IHTMLElement and friends (element.c)
    ("IHTMLElement", "GetTypeInfoCount"):   "html_elem_GetTypeInfoCount",
    ("IHTMLElement", "GetTypeInfo"):        "html_elem_GetTypeInfo",
    ("IHTMLElement", "GetIDsOfNames"):      "html_elem_GetIDsOfNames",
    ("IHTMLElement", "Invoke"):             "html_elem_Invoke",
    ("IHTMLElement", "setAttribute"):       "html_elem_setAttribute",
    ("IHTMLElement", "getAttribute"):       "html_elem_getAttribute",
    ("IHTMLElement", "removeAttribute"):    "html_elem_removeAttribute",
    ("IHTMLElement", "get_children"):       "html_elem_get_children",
    ("IHTMLElement", "get_all"):            "html_elem_get_all",
    ("IHTMLElement", "get_document"):       "html_elem_get_document",
    ("IHTMLElement", "insertAdjacentHTML"): "html_elem_insertAdjacentHTML",
    ("IHTMLElement", "insertAdjacentText"): "html_elem_insertAdjacentText",
    ("IHTMLElement", "scrollIntoView"):     "html_elem_scrollIntoView",
    ("IHTMLElement", "contains"):           "html_elem_contains",
    ("IHTMLElement", "get_style"):          "html_elem_get_style",
    ("IHTMLElement2", "focus"):                "html_elem2_focus",
    ("IHTMLElement2", "blur"):                 "html_elem2_blur",
    ("IHTMLElement2", "getElementsByTagName"): "html_elem2_getElementsByTagName",
    ("IHTMLElement2", "insertAdjacentElement"): "html_elem2_insertAdjacentElement",
    ("IHTMLElement2", "attachEvent"):          "html_elem2_attachEvent",
    ("IHTMLElement2", "detachEvent"):          "html_elem2_detachEvent",
    ("IHTMLElement2", "get_readyState"):       "html_elem2_get_readyState",
    ("IHTMLDOMNode", "hasChildNodes"):  "html_node_hasChildNodes",
    ("IHTMLDOMNode", "get_childNodes"): "html_node_get_childNodes",
    ("IHTMLDOMNode", "appendChild"):    "html_node_appendChild",
    ("IHTMLDOMNode", "removeChild"):    "html_node_removeChild",
    ("IHTMLDOMNode", "replaceChild"):   "html_node_replaceChild",
    ("IHTMLDOMNode", "insertBefore"):   "html_node_insertBefore",
    ("IHTMLDOMNode", "cloneNode"):      "html_node_cloneNode",
    ("IHTMLInputTextElement", "createTextRange"): "html_inputtext_createTextRange",
    ("IHTMLTextAreaElement", "createTextRange"):  "html_textarea_createTextRange",

    # IHTMLElementCollection (element.c)
    ("IHTMLElementCollection", "GetTypeInfoCount"): "html_col_GetTypeInfoCount",
    ("IHTMLElementCollection", "GetTypeInfo"):      "html_col_GetTypeInfo",
    ("IHTMLElementCollection", "GetIDsOfNames"):    "html_col_GetIDsOfNames",
    ("IHTMLElementCollection", "Invoke"):           "html_col_Invoke",
    ("IHTMLElementCollection", "item"):             "html_col_item",
    ("IHTMLElementCollection", "get__newEnum"):     "html_col__newEnum",

    # IHTMLWindow2 / IHTMLWindow3 (window.c)
    ("IHTMLWindow2", "GetTypeInfoCount"): "html_win_GetTypeInfoCount",
    ("IHTMLWindow2", "GetTypeInfo"):      "html_win_GetTypeInfo",
    ("IHTMLWindow2", "GetIDsOfNames"):    "html_win_GetIDsOfNames",
    ("IHTMLWindow2", "Invoke"):           "html_win_Invoke",
    ("IHTMLWindow2", "get_document"):     "html_win_get_document",
    ("IHTMLWindow2", "get_location"):     "html_win_get_location",
    ("IHTMLWindow2", "get_event"):        "html_win_get_event",
    ("IHTMLWindow2", "get_self"):         "html_win_get_self",
    ("IHTMLWindow2", "get_window"):       "html_win_get_window",
    ("IHTMLWindow2", "get_top"):          "html_win_get_top",
    ("IHTMLWindow2", "get_parent"):       "html_win_get_parent",
    ("IHTMLWindow2", "navigate"):         "html_win_navigate",
    ("IHTMLWindow2", "execScript"):       "html_win_execScript",
    ("IHTMLWindow2", "alert"):            "html_win_alert",
    ("IHTMLWindow2", "scrollTo"):         "html_win_scrollTo",
    ("IHTMLWindow2", "scroll"):           "html_win_scroll",
    ("IHTMLWindow2", "scrollBy"):         "html_win_scrollBy",
    ("IHTMLWindow3", "attachEvent"):      "html_win3_attachEvent",
    ("IHTMLWindow3", "detachEvent"):      "html_win3_detachEvent",

    # IHTMLLocation (window.c)
    ("IHTMLLocation", "reload"):   "html_loc_reload",
    ("IHTMLLocation", "replace"):  "html_loc_replace",
    ("IHTMLLocation", "assign"):   "html_loc_assign",
    ("IHTMLLocation", "toString"): "html_loc_toString",
    ("IHTMLLocation", "get_href"): "html_loc_get_href",

    # IHTMLEventObj (element.c): a snapshot taken when the DOM event fired
    ("IHTMLEventObj", "get_srcElement"):   "html_evo_get_srcElement",
    ("IHTMLEventObj", "get_altKey"):       "html_evo_get_altKey",
    ("IHTMLEventObj", "get_ctrlKey"):      "html_evo_get_ctrlKey",
    ("IHTMLEventObj", "get_shiftKey"):     "html_evo_get_shiftKey",
    ("IHTMLEventObj", "put_returnValue"):  "html_evo_put_returnValue",
    ("IHTMLEventObj", "get_returnValue"):  "html_evo_get_returnValue",
    ("IHTMLEventObj", "put_cancelBubble"): "html_evo_put_cancelBubble",
    ("IHTMLEventObj", "get_cancelBubble"): "html_evo_get_cancelBubble",
    ("IHTMLEventObj", "put_keyCode"):      "html_evo_put_keyCode",
    ("IHTMLEventObj", "get_keyCode"):      "html_evo_get_keyCode",
    ("IHTMLEventObj", "get_button"):       "html_evo_get_button",
    ("IHTMLEventObj", "get_type"):         "html_evo_get_type",
    ("IHTMLEventObj", "get_x"):            "html_evo_get_clientX",
    ("IHTMLEventObj", "get_y"):            "html_evo_get_clientY",
    ("IHTMLEventObj", "get_clientX"):      "html_evo_get_clientX",
    ("IHTMLEventObj", "get_clientY"):      "html_evo_get_clientY",

    # IHTMLTxtRange (element.c)
    ("IHTMLTxtRange", "collapse"): "html_txr_collapse",
    ("IHTMLTxtRange", "select"):   "html_txr_select",
    ("IHTMLTxtRange", "get_text"): "html_txr_get_text",
}

# AUTO accessor type table: header param type -> jsprop.c helper suffix.
GET_TYPES = {
    "BSTR *": "bstr", "LONG *": "long", "long *": "long", "short *": "short",
    "float *": "float", "VARIANT_BOOL *": "bool", "VARIANT *": "variant",
    "IHTMLElement **": "elem", "IHTMLElementCollection **": "col",
    "IHTMLDOMNode **": "node", "IDispatch **": "disp",
}
PUT_TYPES = {
    "BSTR": "bstr", "LONG": "long", "long": "long", "short": "short",
    "float": "float", "VARIANT_BOOL": "bool", "VARIANT": "variant",
}


def parse_vtbl(header, iface):
    src = open(os.path.join(INC, header)).read()
    start = src.index("typedef struct %sVtbl {" % iface)
    end = src.index("} %sVtbl;" % iface, start)
    body = src[start:end]
    methods = []
    pat = re.compile(r'\w[\w ]*?\s*\(\s*STDMETHODCALLTYPE\s*\*\s*(\w+)\s*\)\s*\(')
    for m in pat.finditer(body):
        name = m.group(1)
        pos = m.end()
        depth = 1
        i = pos
        while i < len(body) and depth > 0:
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
            i += 1
        params = " ".join(body[pos:i - 1].split())
        methods.append((name, params))
    return methods


def split_params(params):
    """'IHTMLElement *This, BSTR *p' -> [('IHTMLElement *', 'This'), ('BSTR *', 'p')]"""
    out = []
    for p in params.split(","):
        p = p.strip()
        m = re.match(r"^(.*?)(\w+)$", p)
        if not m:
            return None
        out.append((" ".join(m.group(1).replace("*", " * ").split()).replace("* *", "**"), m.group(2)))
    return out


def norm_type(t):
    """'IHTMLElement**' / 'IHTMLElement * *' -> 'IHTMLElement **'; 'BSTR*' -> 'BSTR *'."""
    base = t.replace("*", " ").strip()
    stars = t.count("*")
    return base + (" " + "*" * stars if stars else "")


def auto_kind(name, params, iface=None):
    """Return ('get'|'put', helper_suffix, js_prop), ('call0', None, method) or None."""
    ps = split_params(params)
    if ps and len(ps) == 1 and not name.startswith(("get_", "put_")) and name not in REAL \
            and name not in ("GetTypeInfoCount",):
        return ("call0", None, name)
    if not ps or len(ps) != 2:
        return None
    t = norm_type(ps[1][0])
    if name.startswith("get_") and t in GET_TYPES:
        prop = name[4:]
        return ("get", GET_TYPES[t], JS_NAME_IFACE.get((iface, prop), JS_NAME.get(prop, prop)))
    if name.startswith("put_") and t in PUT_TYPES:
        prop = name[4:]
        return ("put", PUT_TYPES[t], JS_NAME_IFACE.get((iface, prop), JS_NAME.get(prop, prop)))
    return None


def emit():
    c = ['/* auto-generated by gen-stubs.py -- do not edit */',
         '#include "html_doc.h"',
         '#include "stubs.h"',
         '#include "jsprop.h"',
         '#include "log.h"',
         '#include <stdio.h>', '']
    h = ['/* auto-generated by gen-stubs.py -- do not edit */',
         '#ifndef SHIM_STUBS_H',
         '#define SHIM_STUBS_H',
         '#include "html_doc.h"',
         '']

    all_ifaces = [iface for o in OBJECTS for (_h, iface) in o["interfaces"]]
    n_auto = n_stub = n_real = 0

    for obj in OBJECTS:
        for header, iface in obj["interfaces"]:
            h.append("HRESULT STDMETHODCALLTYPE shim_%s_QueryInterface(%s *This, REFIID riid, void **ppv);" % (iface, iface))
            h.append("ULONG STDMETHODCALLTYPE shim_%s_AddRef(%s *This);" % (iface, iface))
            h.append("ULONG STDMETHODCALLTYPE shim_%s_Release(%s *This);" % (iface, iface))
            h.append("extern %sVtbl g_%s_vtbl;" % (iface, iface))
            methods = parse_vtbl(header, iface)
            pmap = dict(methods)
            for (ii, mm), impl in REAL_IMPLS.items():
                if ii != iface:
                    continue
                if mm not in pmap:
                    raise SystemExit("REAL_IMPLS: %s has no method %s" % (iface, mm))
                h.append("extern HRESULT STDMETHODCALLTYPE %s(%s);" % (impl, pmap[mm]))
            h.append("")
    h.append('#endif')
    h.append("")

    for obj in OBJECTS:
        ctype = obj["ctype"]
        js = obj["js"]
        for header, iface in obj["interfaces"]:
            methods = parse_vtbl(header, iface)
            real = {m: impl for (i2, m), impl in REAL_IMPLS.items() if i2 == iface}
            slot = {}
            c.append("/* ---- %s (%s) ---- */" % (iface, ctype))
            for name, params in methods:
                if name in REAL:
                    continue
                if name in real:
                    slot[name] = real[name]
                    n_real += 1
                    continue
                ps = split_params(params)
                # AUTO only on the DOM interfaces, never on the OLE plumbing of the document
                kind = auto_kind(name, params, iface) if js and iface.startswith("IHTML") else None
                if kind:
                    op, typ, prop = kind
                    fn = "auto_%s_%s" % (iface, name)
                    c.append("static HRESULT STDMETHODCALLTYPE %s(%s) {" % (fn, params))
                    c.append("    %s *o = CONTAINING_RECORD(This, %s, %s_iface);" % (ctype, ctype, iface))
                    if "%ld" in js:
                        c.append("    char t[96];")
                        c.append('    snprintf(t, sizeof(t), "%s", o->handle);' % js)
                    else:
                        c.append('    const char *t = "%s";' % js)
                    if op == "call0":
                        c.append('    return shim_js_call0(%s, t, "%s");' % (obj["page"], prop))
                    elif prop.startswith("on") and typ == "variant":
                        arg = ps[1][1]
                        c.append("    (void)o; (void)t;")
                        if op == "get":
                            c.append("    if (!%s) return E_POINTER; VariantInit(%s); V_VT(%s) = VT_NULL; return S_OK;" % (arg, arg, arg))
                        else:
                            c.append('    (void)%s; shim_log("IGNORED %s::%s"); return S_OK;' % (arg, iface, name))
                    else:
                        arg = ps[1][1]
                        c.append('    return shim_js_%s_%s(%s, t, "%s", %s);' % (op, typ, obj["page"], prop, arg))
                    c.append("}")
                    slot[name] = fn
                    n_auto += 1
                    continue
                c.append("static volatile long hit_%s_%s;" % (iface, name))
                c.append("static HRESULT STDMETHODCALLTYPE stub_%s_%s(%s) { shim_stub_hit(&hit_%s_%s, \"%s::%s\"); return E_NOTIMPL; }"
                         % (iface, name, params, iface, name, iface, name))
                slot[name] = "stub_%s_%s" % (iface, name)
                n_stub += 1
            c.append("")
            c.append("HRESULT STDMETHODCALLTYPE shim_%s_QueryInterface(%s *This, REFIID riid, void **ppv) {" % (iface, iface))
            c.append("    %s *o = CONTAINING_RECORD(This, %s, %s_iface);" % (ctype, ctype, iface))
            c.append("    return %s(o, riid, ppv);" % obj["qi"])
            c.append("}")
            c.append("ULONG STDMETHODCALLTYPE shim_%s_AddRef(%s *This) {" % (iface, iface))
            c.append("    %s *o = CONTAINING_RECORD(This, %s, %s_iface);" % (ctype, ctype, iface))
            c.append("    return %s(o);" % obj["addref"])
            c.append("}")
            c.append("ULONG STDMETHODCALLTYPE shim_%s_Release(%s *This) {" % (iface, iface))
            c.append("    %s *o = CONTAINING_RECORD(This, %s, %s_iface);" % (ctype, ctype, iface))
            c.append("    return %s(o);" % obj["release"])
            c.append("}")
            c.append("")
            c.append("%sVtbl g_%s_vtbl = {" % (iface, iface))
            for name, _p in methods:
                if name in REAL:
                    c.append("    .%s = shim_%s_%s," % (name, iface, name))
                else:
                    c.append("    .%s = %s," % (name, slot[name]))
            c.append("};")
            c.append("")

    open("stubs.c", "w").write("\n".join(c) + "\n")
    open("stubs.h", "w").write("\n".join(h) + "\n")
    print("wrote stubs.c / stubs.h: %d interfaces, %d real, %d auto, %d logged stubs"
          % (len(all_ifaces), n_real, n_auto, n_stub))


if __name__ == "__main__":
    emit()
