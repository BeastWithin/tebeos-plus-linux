/* html_doc.h -- the shim's COM objects (interface fields + lifetime helpers).
 *
 * shim.c/docobj.c/window.c/element.c implement them; the generated stubs.c builds their
 * vtables and per-interface QueryInterface/AddRef/Release wrappers, which recover the object
 * with CONTAINING_RECORD against these fields. Field names MUST follow the generator's
 * `<Iface>_iface` convention.
 *
 * Every DOM object carries the Page (page.c) it was created from: one Chromium tab per
 * document, so two WebBrowser controls in one process never talk to each other's page.
 */

#ifndef HTML_DOC_H
#define HTML_DOC_H

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <initguid.h>
#include <windows.h>
#include <ole2.h>
#include <mshtml.h>
#include <oleidl.h>
#include <docobj.h>
#include <urlmon.h>
#include <ocidl.h>
#include "privif.h"

struct CdpConn;     /* cdp.h */
struct RenderCtx;   /* render.h */
struct Page;        /* page.h: one Chromium tab */
struct HTMLDocument;

/* ------------------------------------------------------------------ HtmlWindow */
/*
 * The document's window (IHTMLWindow2/3 + Wine's private navigation interface). Embedded
 * in the HTMLDocument: its AddRef/Release forward to the document, so the window lives
 * exactly as long as the document (as in Wine's own mshtml).
 */
typedef struct HtmlWindow {
    IHTMLWindow2       IHTMLWindow2_iface;
    IHTMLWindow3       IHTMLWindow3_iface;
    IHTMLPrivateWindow IHTMLPrivateWindow_iface;
    struct HTMLDocument *doc;
} HtmlWindow;

HRESULT html_win_qi(HtmlWindow *w, REFIID riid, void **ppv);
ULONG html_win_addref(HtmlWindow *w);
ULONG html_win_release(HtmlWindow *w);

/* The document's location object, embedded in the document like the window. */
typedef struct HtmlLocation {
    IHTMLLocation IHTMLLocation_iface;
    struct HTMLDocument *doc;
} HtmlLocation;

HRESULT html_loc_qi(HtmlLocation *l, REFIID riid, void **ppv);
ULONG html_loc_addref(HtmlLocation *l);
ULONG html_loc_release(HtmlLocation *l);

/* ------------------------------------------------------------------ HTMLDocument */

#define DOC_MAX_PROP_SINKS 8

typedef struct HTMLDocument {
    IHTMLDocument2          IHTMLDocument2_iface;
    IHTMLDocument3          IHTMLDocument3_iface;
    IHTMLDocument4          IHTMLDocument4_iface;
    IHTMLDocument5          IHTMLDocument5_iface;
    IPersistMoniker         IPersistMoniker_iface;
    IPersistStreamInit      IPersistStreamInit_iface;
    IOleObject              IOleObject_iface;
    IOleDocument            IOleDocument_iface;
    IOleDocumentView        IOleDocumentView_iface;
    IOleInPlaceObject       IOleInPlaceObject_iface;
    IOleInPlaceActiveObject IOleInPlaceActiveObject_iface;
    IOleCommandTarget       IOleCommandTarget_iface;
    IProvideClassInfo       IProvideClassInfo_iface;
    IConnectionPointContainer IConnectionPointContainer_iface;
    IConnectionPoint        PropNotifyCP_iface;   /* hand vtable, window.c */
    LONG ref;
    /* document state: set by Load and by the page's navigation events */
    BSTR url;
    BSTR title;
    BSTR ready_state;   /* L"uninitialized" | L"loading" | L"interactive" | L"complete" */
    BOOL dirty;
    IPropertyNotifySink *prop_sinks[DOC_MAX_PROP_SINKS];
    /* DocObject hosting state */
    IOleClientSite      *client;
    IOleInPlaceSite     *ipsite;
    IOleInPlaceFrame    *inplace_frame;
    IOleInPlaceUIWindow *inplace_ui;
    HWND                 hwnd;
    RECT                 view_rect;
    BOOL                 window_active;
    BOOL                 ui_active;
    BOOL                 in_place_active;
    /* ieframe navigation seam (docobj.c SetClientSite) */
    IOleCommandTarget   *client_cmdtrg;
    IDocObjectService   *doc_object_service;
    BOOL                 webbrowser;    /* site is a WebBrowser (IWebBrowserApp service) */
    /* the window/location objects, embedded */
    HtmlWindow           window;
    HtmlLocation         location;
    /* Chromium backing: NULL until Load, then this document's own tab */
    struct Page         *page;
    LONG                 zoom;          /* OLECMDID_OPTICAL_ZOOM percent, 0 = never set (100) */
} HTMLDocument;

HRESULT html_doc_qi(HTMLDocument *d, REFIID riid, void **ppv);
ULONG html_doc_addref(HTMLDocument *d);
ULONG html_doc_release(HTMLDocument *d);

/* The page a document's DOM calls go to: its own tab, else the harness's legacy page. */
struct Page *doc_page(HTMLDocument *d);

/* Set the document's ready state and tell ieframe (IPropertyNotifySink DISPID_READYSTATE). */
void html_doc_set_ready_state(HTMLDocument *d, LPCWSTR state);
void html_doc_set_url(HTMLDocument *d, LPCWSTR url);

/* docobj.c: stop/start screencasting the page into the document window; release what
 * SetClientSite took from the site. */
void html_doc_render_stop(HTMLDocument *d);
#define WM_DOC_PRINT (WM_APP + 0x60)
void html_doc_print_later(HTMLDocument *d);   /* window.print(): print dialog, posted */
void html_doc_window_ready(HTMLDocument *d);
void html_doc_release_site(HTMLDocument *d);

/* window.c: wire the embedded window/location vtables (called by the class factory). */
void html_doc_init_window(HTMLDocument *d);

/* element.c CDP/DOM setup exports (the test harnesses' legacy single-tab mode). */
int shim_dom_connect(int port);
int shim_render_connect(int port);
struct CdpConn *shim_dom_cdp(void);
struct CdpConn *shim_render_cdp(void);
int shim_dom_navigate(const char *url);
void shim_dom_disconnect(void);

/* Dispatch a DOM event forwarded from `page` via Runtime.bindingCalled(__shim_evt). Runs on
 * the UI thread (page.c marshals it). The payload is {h: handle, t: "click", ...}. */
void shim_dom_event_dispatch(struct Page *page, const char *payload_json);

extern HINSTANCE shim_hinst;

/* ------------------------------------------------------------------ HtmlElement */
/*
 * A DOM element. `handle` indexes the page's `window.__shim_els[]` stash. Tag-specific
 * interfaces (input, select, form, ...) answer QueryInterface only for matching tags; the
 * tag is read once, on the first such QI.
 */
typedef struct HtmlElement {
    IHTMLElement          IHTMLElement_iface;
    IHTMLElement2         IHTMLElement2_iface;
    IHTMLElement3         IHTMLElement3_iface;
    IHTMLElement4         IHTMLElement4_iface;
    IHTMLInputElement     IHTMLInputElement_iface;
    IHTMLInputTextElement IHTMLInputTextElement_iface;
    IHTMLSelectElement    IHTMLSelectElement_iface;
    IHTMLOptionElement    IHTMLOptionElement_iface;
    IHTMLTextAreaElement  IHTMLTextAreaElement_iface;
    IHTMLScriptElement    IHTMLScriptElement_iface;
    IHTMLFormElement      IHTMLFormElement_iface;
    IHTMLAnchorElement    IHTMLAnchorElement_iface;
    IHTMLImgElement       IHTMLImgElement_iface;
    IHTMLDOMNode          IHTMLDOMNode_iface;
    IHTMLElementRender    IHTMLElementRender_iface;
    LONG ref;
    long handle;
    struct Page *page;
    char tag[16];       /* upper-case tagName, "" until first needed */
} HtmlElement;

HRESULT html_elem_qi(HtmlElement *e, REFIID riid, void **ppv);
ULONG html_elem_addref(HtmlElement *e);
ULONG html_elem_release(HtmlElement *e);

/* ------------------------------------------------------------------ HtmlStyle */
typedef struct HtmlStyle {
    IHTMLStyle IHTMLStyle_iface;
    LONG ref;
    long handle;        /* the element's handle; the style is `__shim_els[handle].style` */
    struct Page *page;
} HtmlStyle;

HRESULT html_style_qi(HtmlStyle *s, REFIID riid, void **ppv);
ULONG html_style_addref(HtmlStyle *s);
ULONG html_style_release(HtmlStyle *s);

/* ------------------------------------------------------------------ HtmlElementCollection */
typedef struct HtmlElementCollection {
    IHTMLElementCollection IHTMLElementCollection_iface;
    LONG ref;
    long handle;        /* the stashed collection, `window.__shim_els[handle]` */
    struct Page *page;
} HtmlElementCollection;

HRESULT html_col_qi(HtmlElementCollection *c, REFIID riid, void **ppv);
ULONG html_col_addref(HtmlElementCollection *c);
ULONG html_col_release(HtmlElementCollection *c);

/* ------------------------------------------------------------------ HtmlEventObj */
/*
 * window.event: a snapshot of the DOM event that is being dispatched (page.c records it
 * from the __shim_evt payload). Reads come from the snapshot; returnValue/cancelBubble/
 * keyCode writes are stored on it (and returnValue=false is applied by the listener).
 */
typedef struct HtmlEventObj {
    IHTMLEventObj IHTMLEventObj_iface;
    LONG ref;
    struct Page *page;
    char type[32];
    long src_handle;    /* srcElement stash index, -1 if none */
    long key_code, button, client_x, client_y;
    BOOL alt_key, ctrl_key, shift_key;
    VARIANT return_value;
    VARIANT_BOOL cancel_bubble;
} HtmlEventObj;

HRESULT html_evo_qi(HtmlEventObj *e, REFIID riid, void **ppv);
ULONG html_evo_addref(HtmlEventObj *e);
ULONG html_evo_release(HtmlEventObj *e);

/* ------------------------------------------------------------------ HtmlTxtRange */
/*
 * createTextRange() on an input/textarea: just enough of IE's text range for the
 * "caret to the end" idiom (createTextRange -> collapse(false) -> select()).
 */
typedef struct HtmlTxtRange {
    IHTMLTxtRange IHTMLTxtRange_iface;
    LONG ref;
    long handle;        /* the element */
    struct Page *page;
    int collapse;       /* -1 whole text, 0 collapsed to the start, 1 to the end */
} HtmlTxtRange;

HRESULT html_txr_qi(HtmlTxtRange *r, REFIID riid, void **ppv);
ULONG html_txr_addref(HtmlTxtRange *r);
ULONG html_txr_release(HtmlTxtRange *r);

/* Private IID: QueryInterface on one of our elements answers it with the HtmlElement itself
 * (addref'd), so an element handed back to us (appendChild, contains, script arguments)
 * can be recognised. */
DEFINE_GUID(IID_IShimElement, 0x5f1a3c20, 0x7b2d, 0x4e61, 0x9a, 0x0c, 0x51, 0x6d, 0x2e, 0x88, 0x40, 0x13);

#endif /* HTML_DOC_H */
