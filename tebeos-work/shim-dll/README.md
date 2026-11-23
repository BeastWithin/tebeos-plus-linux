# shim-dll — Path-A "fresh mshtml.dll" (native COM server + CEF, drop-in for Wine's Gecko mshtml)

The C++/native half of the Path-A shim (see `HANDOFF-PATH-A.md` §5a): a DLL that
registers `CLSID_HTMLDocument` and will render the Medula page in CEF while answering
the IE `mshtml` COM DOM surface the vendor app drives.

## Status — 2026-11-12: in production
The shim is in daily use and every milestone below is done. The newest notes are in the
status block at the top of `../../HANDOFF-PATH-A.md`. They cover keyboard/mouse emulation,
`iecompat.js` (`gen-iecompat.py` regenerates `iecompat.h`), the click hold for vendor
mouseup handlers, the test and install workflow, and known gaps. The "Next steps" section at
the end of this file is historical.

## Status — Milestone 2 (done, verified under Wine)

A native COM server (`shim32.dll` / `shim64.dll`) that:

- exports `DllGetClassObject / DllCanUnloadNow / DllRegisterServer / DllUnregisterServer`,
- serves `CLSID_HTMLDocument` (`25336920-03F9-11CF-8FD0-00AA00686F13`),
- creates an `HTMLDocument` object implementing **10 interfaces** with correct,
  compiler-guaranteed vtables:

  - DOM: `IHTMLDocument2` (116-method vtable: IUnknown + IDispatch + IHTMLDocument +
    IHTMLDocument2 all inlined)
  - OLE DocObject document-side surface (what `ieframe`/`urlmon` need to host it):
    `IPersistMoniker`, `IPersistStreamInit`, `IOleObject`, `IOleDocument`,
    `IOleDocumentView`, `IOleInPlaceObject`, `IOleInPlaceActiveObject`,
    `IOleCommandTarget`, `IProvideClassInfo`

- `QueryInterface/AddRef/Release` are real (shared 1 refcount on the object); every
  other method is an `E_NOTIMPL` stub.

Verified end-to-end under Wine (both arches): `DllGetClassObject` → `CreateInstance` →
`QI` of all 12 interface IDs returns `S_OK` (12/12), and deep-vtable calls (`get_title`,
`get_URL`) route to `E_NOTIMPL` without crashing.

```
DllGetClassObject(CLSID_HTMLDocument, IClassFactory) = 0x00000000
CreateInstance(IHTMLDocument2) = 0x00000000
get_title  = 0x80004001   # E_NOTIMPL, routed through the correct slot
get_URL    = 0x80004001
DocObject surface QI: IUnknown/IDispatch/IHTMLDocument2/IPersistMoniker/IPersistStreamInit/
  IOleObject/IOleDocument/IOleDocumentView/IOleInPlaceObject/IOleInPlaceActiveObject/
  IOleCommandTarget/IProvideClassInfo => 12/12 interfaces OK
```

### ABI note added at Milestone 2

`DEFINE_GUID` in mingw only *declares* (not defines) the GUIDs unless `INITGUID` is set,
so `html_doc.h` now `#include <initguid.h>` **before** `mshtml.h`/`oleidl.h`/etc. Without
it, `CLSID_HTMLDocument` is an undefined symbol at link time (`undefined symbol:
_CLSID_HTMLDocument`, referenced via i686 `.refptr`).

## Files

| File | Purpose |
|---|---|
| `gen-stubs.py` | Parses Wine headers (`mshtml.h`/`docobj.h`/`urlmon.h`/`oleidl.h`/`ocidl.h`) and emits `stubs.c`/`stubs.h`: for each of the 10 interfaces, E_NOTIMPL stubs + a `g_<Iface>_vtbl` (field-ordered ⇒ compiler-guaranteed layout) + `shim_<Iface>_QueryInterface/AddRef/Release` wrappers routing to `html_doc_*`. |
| `html_doc.h` | `HTMLDocument` struct (10 `<Iface>_iface` fields + `ref`) and `html_doc_qi/addref/release` decls. |
| `shim.c` | COM server: `Dll*` entry points, `IClassFactory`, `HTMLDocument` creation + `html_doc_qi/addref/release`, registry registration. |
| `probe.c` | Loads the DLL, QIs all 12 interface IDs, exercises class factory + vtable, prints results. |
| `shim.def` | Undecorated COM entry-point exports. |
| `build.sh` | llvm-mingw build (i686 + x86_64). |
| `json.{h,c}` | Minimal JSON parser + `j_quote` (CDP request/response, §Milestone 4a). |
| `cdp.{h,c}` | CDP client: WebSocket (RFC 6455) + JSON over it, mirroring `shim-engine/cdp.py`. |
| `cdptest.c` | Self-test: mock WS server (SHA-1 + base64 accept) over 127.0.0.1. |
| `livetest.c` | Exercises the CDP client against a **real Chromium** debugging port. |
| `hosttest.c` | Minimal DocObject *container* harness (proves ieframe-style hosting). |

## Build & test

```sh
python3 gen-stubs.py           # regenerate stubs.c/.h (only if Wine headers changed)
bash build.sh                  # -> shim32.dll/shim32-probe.exe, shim64.*
WINEPREFIX=~/.wine-tebeos \
  ~/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wine \
  shim32-probe.exe shim32.dll
```

## ABI notes learned (keep in mind)

- In **C mode**, mingw/Wine `mshtml.h` exposes `IHTMLDocument2_QueryInterface` etc. only
  as `#define` macros (**not** functions), so the real implementations must use distinct
  names (`shim_doc_*`); the generator wires `.QueryInterface/.AddRef/.Release` to them.
- `CLSID_HTMLDocument` is defined by mingw's `mshtml.h`; `CLSID_MHTMLDocument` is **not**
  (dropped — it's only for `.mht` archives).
- The `lpVtbl` member is non-const in C mode (`CONST_VTBL` is empty), so `g_IHTMLDocument2_vtbl`
  is non-const.
- **`DEFINE_GUID` needs `INITGUID`** (via `#include <initguid.h>` before the GUID headers)
  or the GUIDs are declarations only and the DLL fails to link.

## Status — Milestone 3a (done, verified under Wine)

Document state + persistence are real now (not `E_NOTIMPL`):

- `IPersistMoniker::Load` reads the URL from the moniker's display name and stores it;
  `IPersistStreamInit::Load`/`InitNew` set the ready state; `GetClassID` returns
  `CLSID_HTMLDocument`; `IsDirty` reports the dirty flag.
- `IHTMLDocument2::get_URL` / `get_title` / `get_readyState` return the stored state.
- Probe round-trip verified both arches: `CreateURLMoniker(login.jsp)` → `Load` →
  `get_URL` returns the URL and `get_readyState` returns `"complete"`.

Implemented via a new **generator hook**: `gen-stubs.py` `REAL_IMPLS` maps
`(interface, method) → C function`; the generator emits an `extern` decl (signature parsed
from the Wine header) and wires the vtable slot to it instead of an `E_NOTIMPL` stub. The
real implementations live in `shim.c`. `stubs.c` now `#include`s `stubs.h` so it sees those
`extern` decls.

## Status — Milestone 3b (done, verified under Wine)

The document is now a **hostable DocObject**. New file `docobj.c` implements the
container-facing surface (all wired via the `REAL_IMPLS` hook), mirroring Wine's own
mshtml `oleobj.c`/`view.c`:

- `IOleObject::{SetClientSite,GetClientSite,GetUserClassID,DoVerb,Close}` — `DoVerb` QIs the
  client for `IOleDocumentSite` and calls `ActivateMe` (else activates directly).
- `IOleDocument::CreateView` — the document is its own view; returns `IOleDocumentView`.
- `IOleDocumentView::{SetInPlaceSite,GetInPlaceSite,GetDocument,SetRect,GetRect,Show,UIActivate,CloseView}`.
- `IOleInPlaceObject::*` and `IOleInPlaceActiveObject::*` — window + deactivation + frame
  integration (`SetActiveObject` is issued on the frame's `IOleInPlaceUIWindow` part, since
  ieframe returns a NULL `*ppDoc`).
- The render target is a placeholder child window (class `ShimHtmlServer`, system-window
  colour); it is created/re-parented/sized during in-place activation and will be swapped
  for a CEF view later.

Verified with a new **`hosttest.c`** — a minimal OLE DocObject *container* that mirrors
ieframe's exact sequence (`GetUserClassID → SetClientSite → IPersistMoniker::Load →
DoVerb(SHOW) → ActivateMe → CreateView → SetRect → Show → UIActivate → GetWindow`).
Both arches report `PASS (DocObject hosting)` with a valid window (`IsWindow=1`).

## Status — Milestone 4a (done, verified under Wine): CDP transport

A self-contained native `cdp.{c,h}` + `json.{c,h}` implementing the Chrome DevTools
Protocol transport the shim needs to reach a Chromium-family engine (the app's in-process
CEF on `:8080` today, or a self-hosted CEF later). It mirrors `shim-engine/cdp.py`:

- `json.c` — minimal JSON parser (objects/arrays/strings/numbers/bool/null, `\u` escapes) +
  `j_quote`.
- `cdp.c` — Winsock connect + HTTP upgrade handshake (correct `Sec-WebSocket-Key`),
  client-masked text frames, server-frame parse with continuation reassembly and
  ping/pong, and a thin `cdp_call`/`cdp_recv` (id matching) layer.

Verified by **`cdptest.c`** — a mock WebSocket server (correct `Sec-WebSocket-Accept` via a
hand-rolled SHA-1 + base64) in a thread, exercised over 127.0.0.1. Proves the handshake,
client masking, server-frame parsing, SHA-1 against the known `"abc"` vector, and the
`cdp_call`→reply / `cdp_recv`→event JSON round-trip. Both arches `PASS (CDP transport)`.

**Also verified against real Chromium** (`chromium --headless=new --remote-debugging-port=9222`)
via **`livetest.c`**: `/json` discovery (`cdp_first_page_wsurl`), the WS handshake against a
genuine server, `Runtime.evaluate("1+2") → 3`, `Page.navigate` to a data URL, and reading
`document.title` — all `PASS (live CDP vs Chromium)` on both arches. This de-risks the whole
"drive a Chromium-family engine over CDP" approach. (Fix made along the way: `cdp_http_get`
now honours `Content-Length` since Chromium HTTP-keeps-alive rather than closing.)

## Status — Milestone 4b/5 (done, verified): CDP-backed DOM COM surface

The document now exposes `IHTMLDocument3` and `getElementById` returns a real
`IHTMLElement` (a second `HtmlElement` object) whose DOM methods round-trip through
Chrome DevTools Protocol. New file `element.c` + a `domtest.c` live harness.

- `gen-stubs.py` refactored from a flat `IFACES` list to an **`OBJECTS`** list (each:
  `ctype` struct + `qi`/`addref`/`release` fns + interface list), so it emits vtables for
  `HtmlElement` (IHTMLElement + IHTMLElement2) in addition to `HTMLDocument` (now 11
  interfaces incl. `IHTMLDocument3`).
- The CDP connection is process-global (`g_cdp` in `element.c`), set by the exported
  `shim_dom_connect(port)`; `shim_dom_navigate(url)` drives `Page.navigate`.
- `getElementById` stashes the element in the page's `window.__shim_els[]` and returns its
  index as a `handle`; element methods evaluate `window.__shim_els[handle].<prop>`.
- Implemented over CDP: `getAttribute`/`setAttribute`, `get/put_innerText`,
  `get/put_innerHTML`, `get_id`, `get_tagName`, `get_parentElement`, `click`, and the
  `IDispatch` head (`GetIDsOfNames`/`Invoke`) mapping `"click"`/`"focus"`/`"blur"` — the
  `HtmlElement.InvokeMember("click")` path the app actually uses.
- `domtest.c` drives it against **real Chromium**: `getElementById → getAttribute/
  get_innerText(UTF-8 Turkish)/get_tagName/get_id → Invoke("click")` — `PASS` both arches.

## Status — Milestone 5a (done, verified): collections + element/input surface

Added a third object `HtmlElementCollection` and broadened the surfaces the app drives
most. All over CDP `Runtime.evaluate` with a second page stash `window.__shim_cols[]`.

- **`IHTMLElementCollection`**: `get_length`, `item(i)` (returns a stashed `IHTMLElement`).
  `_newEnum` (for `foreach`) is still a stub.
- **Document**: `IHTMLDocument3::{getElementsByTagName,getElementsByName}`,
  `IHTMLDocument2::{get_all,createElement,get_body,get_activeElement}`.
- **Element**: `get_children`, `get_all` (=`getElementsByTagName('*')`), `insertAdjacentHTML`/
  `-Text`, `scrollIntoView`, `contains`; `IHTMLElement2::{focus,blur,getElementsByTagName}`.
- **`IHTMLInputElement`** (on `HtmlElement`, QI'd for inputs): `get/put_value`, `get/put_type`,
  `get_name`, `get/put_checked` (KVKK checkbox).
- Added a `cdp_eval_bool` JSON reader (fix: `.checked`/`.contains()` return a JSON **boolean**,
  not a number).
- `domtest.c` now proves collections (`length`/`item`), value/type/name/checked, a Turkish
  `put_value("şıçö")` Turkish round-trip (the login-password path), `createElement`+`setAttribute`, and
  `get_body` — `PASS` both arches (23 assertions).

## Status — Milestone 5b (done, verified): screencast render target over CDP

Proved the **render path** for a CDP-driven engine: `Page.startScreencast` → an async
frame pump that receives `Page.screencastFrame` events (valid **JPEG** payloads,
`ff d8` magic, correct `deviceWidth`/`deviceHeight`), per-frame ack, and clean
`Page.stopScreencast`. `rendertest.c` streams a base64 `data:`-URL animated page (repaints
every 200 ms) and captures multiple frames. `PASS` on both arches vs real Chromium 153.

Three real bugs fixed en route (all would have blocked the GDI-blit milestone):

- **`json.c` `j_free` heap-use-after-free / double-free.** Children in J_ARR/J_OBJ are
  stored **by value** inside `items`/`vals` arrays, but the old `j_free` ended with
  `free(v)` regardless — so recursively freeing an array slot did `free(&vals[i])` (an
  interior pointer) and then `free(vals)` again. Latent on small CDP messages, it caused
  a page fault on the first large `screencastFrame`. Fixed by splitting out a
  `j_free_contents` (no struct free) and keeping `free(v)` only for standalone roots.
  (Captured & confirmed via ASan on a native build of `json.c`.)
- **`cdp.c` `b64_decode`** had an accumulator that was never reset (corrupts output past
  ~4 bytes on 32-bit) and treated "output buffer full" as `-1` error. Rewrote it to mask
  off emitted bits and return the partial count instead.
- **Added `SO_RCVTIMEO` (3 s) to the CDP socket** so a dead/quiet target (e.g. the
  screencast stream going silent) can't block `cdp_recv`/`cdp_call` forever.

**Decision ratify (still open):** render target option (b) drive Chromium over CDP +
GDI blit is what `rendertest.c` exercises; option (a) self-host CEF is not yet committed.

### Decode + blit (done, verified) — `blittest.c`

The "no libjpeg/libpng import lib in llvm-mingw msvcrt" blocker is **solved** by vendoring
**`stb_image.h`** (single-file, public domain, decodes JPEG+PNG/BMP/GIF with only the C
stdlib). `blittest.c` proves the full path, both arches vs real Chromium 153:

1. `Page.startScreencast` → one `screencastFrame` → base64-decode the whole JPEG;
2. `stbi_load_from_memory` → RGBA 780×437 (centre pixel correctly RED on the red/blue
   animated page);
3. RGBA→BGRA, then `StretchDIBits` the 32bpp top-down `BI_RGB` DIB into a DIB-section DC;
4. read-back is **byte-identical** to the source (centre pixel `00 00 FE FF`);
5. writes `blittest_out.bmp` (verified red via Pillow).

`PASS (decode + blit)` both arches. So the render target is unblocked end-to-end: Chromium
frame → JPEG → pixels → BGRA → GDI. What remains is *plumbing*, not feasibility.

### Continuous render worker (done, verified) — `render.c` / `renderlive.c`

A reusable, thread-safe render pump proves the "keep streaming forever" half:

- `render_start(c, hwnd, &ctx)` sends `Page.startScreencast` and spawns a **worker
  thread** that owns the CDP read loop. Each `Page.screencastFrame` is base64+JPEG
  decoded (`stb_image`), converted to BGRA, stored in a `CRITICAL_SECTION`-guarded
  back-buffer, and `StretchDIBits`-ed into the target HWND; acks go out fire-and-forget
  via new `cdp_command()`. `render_last_frame()` reads the back-buffer race-free.
- `renderlive.c` (both arches, vs Chromium 153) navigates the animated red/blue page and
  confirms the pump decodes a *changing* image: `seq` advances and the centre pixel
  flips RED↔BLUE. `PASS (continuous render)`.

Note: headless Chromium throttles the test page's `setInterval` after ~3 ticks (the
`seq` stalls at 3), which is why only 3 *distinct* frames appear — not a pump issue;
the real Medula page repaints on interaction, not timers.

## Status — Milestone 6 (done, verified): render + input wired into docobj.c

The renderer and input forwarding are now integrated into the actual document object
(previously they were only proven by standalone `renderlive`/`inputtest` harnesses).

- **Two CDP channels** to the same page target (element.c): the DOM channel (`g_cdp`,
  synchronous `cdp_call` on the UI thread) and a new render/input channel
  (`g_render_cdp`, set by exported `shim_render_connect(port)`, exposed via
  `shim_render_cdp()`). Two sockets keep the render worker's read loop from racing the
  DOM thread's synchronous reads; `Input.*` rides the render channel as fire-and-forget
  `cdp_command` alongside the screencast acks.
- **`docobj.c` now hosts the renderer**: `IPersistMoniker::Load` navigates via
  `shim_dom_navigate()`; `doc_activate_window`/`Show`/`SetRect` call `doc_ensure_render()`
  (idempotent `render_start` once the window + channel exist); the window proc handles
  `WM_PAINT` (`render_repaint`), mouse (`L…/R…DOWN/UP`, `mousemove`), and keys
  (`WM_CHAR` → `input_insert_text`, `WM_KEYDOWN` for Enter/Backspace/Tab/arrows).
- **`html_doc_render_stop()`** stops + joins the pump from `InPlaceDeactivate` and
  `html_doc_release`. `render_stop` now forces a stuck worker down with `TerminateThread`
  if it doesn't join within 5 s (a dead socket must never hang shutdown). The pump also
  survives idle gaps (SO_RCVTIMEO + bounded retry) instead of dying after 3 s of silence.
- New **`hostrender.c`** proves the whole path vs real Chromium 153, both arches:
  connect → host CLSID_HTMLDocument → `Load(data URL)` navigates → in-place activation
  starts the renderer (`render_frame_info` seq advances, 780×437) → a `WM_LBUTTONDOWN`/`UP`
  on the render window dispatches to the page and its `onclick` fires (read back through
  the document's own DOM surface). `PASS (host + render + input integration)`.

### Gotcha (test-only)

A normal `return` from `hostrender` main() **hangs under Wine after the CDP sockets were
used** — the CRT exit path blocks. `ExitProcess()` skips CRT/DllMain teardown and lets the
harness finish. This is a harness quirk, not a shim defect; the production `mshtml.dll`
install exits through `tebeos_medula.exe`'s own WinForms loop. Worth re-verifying on the
real app.

## Status — Milestone 7 (done, verified): style + outerHTML + IHTMLDOMNode

Grew the element surface the injected TEBEOS table depends on, and `HtmlElement` gained a
4th interface (`IHTMLDOMNode`) plus a new `HtmlStyle` object (`IHTMLStyle`). All over CDP:

- **`IHTMLStyle`**: new `HtmlStyle` object + `IHTMLElement::get_style` → `get_style` returns
  it; `get/put_cssText` round-trip `el.style.cssText` (the `.Style = "css string"` setter the
  app uses ~129+ times). The ~150 other IHTMLStyle property methods remain `E_NOTIMPL` stubs
  (generator auto-stubs them).
- **`IHTMLDOMNode`** (now QI'd on `HtmlElement`): `appendChild` (the injected-table workhorse,
  ~90 sites), `removeChild`, `cloneNode`, `get_firstChild`/`get_lastChild`/`get_parentNode`,
  `hasChildNodes`, `get_childNodes` (returns a collection), `get_nodeType`/`get_nodeName`.
  `newChild`/`oldChild` args are the same `HtmlElement` object QI'd as `IHTMLDOMNode`, so their
  handles are recovered via `CONTAINING_RECORD`.
- **`IHTMLElement`: `get/put_outerHTML` + `get/put_outerText`.** `outerHTML` is the critical
  one (`element.OuterHtml += "<button .../>"` → get + put, ~60 sites, injects the TEBEOS
  shortcut buttons); `outerText` falls back to `innerText` (Chromium has no `outerText`).

`domtest.c` grown (Milestone-7 assertions: get_style/cssText, QI IHTMLDOMNode, nodeType/
nodeName, appendChild→childNodes.length, get_outerHTML). **PASS both arches** vs Chromium 153.

## Status — Milestone 7b (done, verified): `_newEnum` + `attachEvent` event sinking

Proved the two last DOM blockers for the injected TEBEOS table's buttons:

- **`get__newEnum` → `IEnumVARIANT`** (the `foreach (HtmlElement e in collection)` loop).
  `domtest.c` `Next(2)` returns two `VT_DISPATCH` elements with correct attributes — PASS.
- **`attachEvent`/`detachEvent` + `shim_dom_event_dispatch` + `render.c` `bindingCalled`.**
  `IHTMLElement2::attachEvent(name, sink)` stores the sink, injects an `addEventListener`
  that calls the CDP binding `window.__shim_evt(...)` (added lazily via `Runtime.addBinding`
  on the render channel), and the render pump's `Runtime.bindingCalled` handler invokes each
  matching sink's `IDispatch::Invoke(DISPID 0)` — the exact .NET `HtmlToClrEventProxy`
  contract. **New `eventtest.c`** (full DocObject hosting + live render pump) drives
  `attachEvent("onclick", mockSink)` → `IHTMLElement::click()` → sink counter 1→2, and
  `detachEvent` stops it — **PASS both arches** vs Chromium 153.

## Status — Milestone 7c (done, verified): document-level events + `get_readyState`

- **`IHTMLDocument3::attachEvent`/`detachEvent`** (document-level event sinking —
  `HtmlDocument.AttachEventHandler`). Same mechanism as the element path, but the listener
  is registered on `document` and dispatched under a `DOC_HANDLE`(-1)/`WIN_HANDLE`(-2)
  sentinel shared with the element registry.
- **`IHTMLElement2::get_readyState`** → `VT_BSTR "complete"`.
- `eventtest.c` grown to cover both (element sink + document sink fire on click; detach
  stops each) — **PASS both arches**.

Two bugs fixed en route:

- **Test-harness calling-convention mismatch.** `shim_connect_fn`/`frame_info_fn`/
  `disconnect_fn` typedefs used `WINAPI` (stdcall) while the DLL's custom exports are
  `cdecl` — a 32-bit-only ABI bug that page-faulted on `shim_render_connect`. Fixed in
  `eventtest.c` and `hostrender.c` (`DllGetClassObject` stays `WINAPI`; `domtest.c` was
  already `__cdecl`).
- **Dispatch sentinel guard.** `shim_dom_event_dispatch` guarded on `handle >= 0`, dropping
  the negative document/window handles; now guards on `if (dom)` and matches the handle
  exactly.

## Next steps (Milestone 7 → up)

1. Remaining DOM: `IHTMLWindow2`/`InvokeScript` and `IHTMLWindow3::attachEvent`
   (window-level events — `Form1` also sinks `load`/`keypress` at the window level).
2. `IHTMLElementRender::DrawToDC` (captcha) → CDP `Page.captureScreenshot` clipped to the
   element geometry, wired to the existing `tessnet2` shim + `ocr_service.py`.
3. **Bootstrap**: who launches Chromium and how does the shim learn the CDP port in the
   real app (vs the harness passing it explicitly)? Also widen `cdp_send_request`'s 2048-byte
   message buffer before the large injected TEBEOS table, and re-check the render/input
   coordinate mapping when the screencast frame size differs from the HWND client size.
4. **Marshal the sink to the UI thread.** `shim_dom_event_dispatch` currently invokes the
   sink from the render pump worker thread; the .NET sink is an STA CCW and its delegate
   touches WinForms UI, so production must `PostMessage` to the doc HWND and invoke on the
   UI thread (see thread-safety note in HANDOFF-PATH-A.md).
5. Wire the DLL into Wine as a real `mshtml.dll` override (both arches) and hit the §3
   acceptance test (`Medula'da Aç` → `E-Reçete Giriş` menu sub-items + navigation).

Note: a `data:` URL with no charset defaults to Windows-1252 in Chromium — always set
`<meta charset>` (or `;charset=utf-8`) in test pages or Turkish text gets mojibake'd.
