# shim-engine — the "engine half" of the Path-A shim

Reference implementation + live test harness for the **CDP ↔ DOM** mapping that the
C++ `mshtml.dll` shim will use. See [`HANDOFF-PATH-A.md`](../../HANDOFF-PATH-A.md)
(§2c surface, §5a seam) for the full context.

## Files

| File | Purpose |
|---|---|
| `cdp.py` | Minimal Chrome DevTools Protocol client (stdlib only): `/json` discovery + a hand-rolled RFC 6455 WebSocket (client-masked). |
| `engine.py` | `MedulaEngine` + `ElementHandle`: the §2c operation set over CDP (`navigate`, `get_element_by_id`, `get/set_attribute`, `inner_text`, `click`, `screenshot`, `attach_event`). |
| `demo.py` | Live end-to-end demo against any Chromium/CEF debugging port. |
| `selftest.py` | In-process mock WS server that verifies the transport **without Chromium**. |

## Run the self-test (no Chromium needed)

```sh
python3 selftest.py
# PASS: handshake + accept, client masking, call() round-trip, event dispatch
```

## Run the live demo (needs a throwaway Chromium/Edge/CEF)

Do **not** point it at the app's own CEF (port 8080) — that instance renders the
TEBEOS dashboard.

```sh
# dedicated Chromium (headless or windowed) exposing CDP:
chromium --headless=new --remote-debugging-port=9222 --no-sandbox about:blank
# or:   chromium --remote-debugging-port=9222

python3 demo.py --port 9222
```

The demo round-trips the Turkish string `çığ` (the same value as the user's login
password) through `setAttribute`/`getAttribute`, proving UTF-8 correctness of the
whole CDP/DOM path, then exercises `click()` → `innerText` and a screenshot
(`/tmp/shim_demo.png`).

## Why this exists

The vendor app drives the Medula page through ~20 IE COM methods (§2c). Every one
maps 1:1 to CDP. This module is (a) the proof that mapping is real, (b) the test
harness during shim development, and (c) the reference the C++ `mshtml.dll`
implements natively (CEF exposes CDP, so the C++ code is the same operations via
CEF's C API / CdpSession).

## Next step (see HANDOFF-PATH-A.md §5a/§8)

Build the native COM skeleton: a DLL registering `CLSID_HTMLDocument`/
`CLSID_MHTMLDocument`, a CEF view drawn into `IOleDocumentView`'s rect, then the
§2c DOM/event surface in the frequency order `GetElementById →
Get/SetAttribute/InnerText → InvokeMember("click") → AttachEventHandler →
DrawToDC`.
