## Tooling added
- `tools/ocrprobe/` — load PNG, run the exact vendor pipeline + tessnet2, dump the Word list.
- `tools/ocrcount/` — live: capture N captchas via DrawToDC + OCR each, log result length.
- `tools/reloadprobe/` — capture the captcha twice (reload) and pixel-compare.
- `captcha-samples/` — captured live captcha PNGs (140x30) for offline repro.

---

# UPDATE — 2026-10-13 (later): the 302 rejection is "wrong captcha", NOT a POST/form bug

## Definitive finding (proven by replaying the login with a plain HTTP client)
The SGK login POST returns `302 -> LogoutServlet` **exactly when the captcha is wrong**
(or the KVKK checkbox isn't sent as `true`). It returns `200` (re-render) only for
missing `button1`/`ViewState` (less important). So the handoff's "POST rejected for an
unknown reason" is fully explained: **the app is submitting a wrong captcha**. A correct
POST (all 7 fields, correct captcha) returns `200` and lands on the logged-in page
("<ECZANE ADI> (<ECZANE_KODU>)" + ":: Reçete / :: Fatura / …" menu).

Relevant reproduction (`medula_login2.py` in /tmp):
- correct captcha + `kvkkTaahhut=true` + `button1=Giriş Yap` + `_SUBMIT=1` + ViewState → `200` ✅
- wrong captcha → `302 → LogoutServlet` ❌
- missing `kvkkTaahhut` → `302 → LogoutServlet` ❌
- missing `button1` or `ViewState` → `200` (form re-render, no crash)

## The captcha is now 150×50 (SGK changed it), squished to 140×30
- The servlet `/eczane/SayiUretenImageYeniServlet` now returns a **150×50** PNG, but the
  login page still declares `<img width="140px" height="30px">`. Gecko squishes 150×50 into
  140×30 (≈40% vertical compression), and the vendor's `Bitmap(img.width,img.height)` =
  140×30 → `Resize` ×8 → 1120×240 → grayscale/noise → tessnet2.
- Every request to the servlet returns a **different** captcha (verified 5/5 hashes differ),
  so "re-fetch to get the native image" does NOT work — the session's expected captcha is
  whatever the LAST servlet request set.

## Left-drift is NOT fixed by removing Redraw
Measured the live `DrawToDC` BMP dumps again: the squished captcha still drifts ~1px LEFT
per call (dark-pixel count falls monotonically 933→473 over blit_65→84; `leftmost_digit_col=0`
the whole way), progressively clipping the leading digit → 5-digit OCR → the app's
`text.Length<=5 → "999999"` fallback → guaranteed wrong captcha → 302 loop.

## Fix applied this session (memoise the first DrawToDC capture)
`dlls/mshtml/htmlelem.c` `HTMLElementRender_DrawToDC` now snapshots the captcha region ONCE
(on the first call for a given element) and replays that snapshot for every later call, so
the app's retry loop always OCRs the same clean pixels instead of an increasingly clipped
frame.  Details:
- added `HTMLElement.render_bmp / render_bmp_w / render_bmp_h` (cache HBITMAP), init in
  `HTMLElement_Init`, freed in `HTMLElement_destructor`;
- `capture_region()` replaces the old `dump_blit_full` instrumentation (now DIB snapshot);
- removed `dump_setattr` + `dump_blit_full` debug instrumentation (the handoff's hygiene TODO).
- Rebuilt both arches and installed to prefix + `wine-patch/mshtml/`.

## Still to verify
Run the app, trigger automation (reload button at screen (74,40)), and confirm the captcha
field now holds a stable 6-digit value that SGK accepts (login advances past `login.jsp`).
If the first capture is itself already clipped (drift present on call #1), the next step is
to decode the native 150×50 PNG deterministically instead of BitBlt the squished surface.

---

# UPDATE — 2026-10-14: robust path implemented (decode native PNG in DrawToDC)

Per the "do it" instruction, `HTMLElementRender_DrawToDC` now **decodes the captcha's
native PNG instead of BitBlt-ing the drifting squished surface**, eliminating the drift at
its root rather than papering over it:

- `get_img_src()` QIs the element to `nsIDOMHTMLImageElement` and resolves its absolute
  `src` (via `nsuri_to_url`) to detect the captcha (`wcsstr(..., L"SayiUretenImageYeniServlet")`).
- `fetch_and_decode()` re-fetches that URL with `InternetOpenUrlW` (`INTERNET_FLAG_RELOAD`,
  so it uses the browser's session cookies in wininet's cookie cache and the servlet updates
  the session's expected captcha), collects the bytes, and decodes with `OleLoadPicture`
  (WIC PNG) → `IPicture::get_Handle` → `CopyImage(... LR_CREATEDIBSECTION)` to own the
  native HBITMAP.
- `DrawToDC` StretchBlts the native (150×50) image into the app's 140×30 DC, caches the
  native DIB in `HTMLElement.render_bmp`, and replays it on later calls.
- Rebuilt both arches, installed to prefix + `wine-patch/mshtml/`.

Session-sync rationale: the captcha servlet regenerates on every request and stores the
result in the session, so re-fetching inside DrawToDC keeps the session's expected value
equal to the image the app OCRs.  The main thing to verify live is that wininet's re-fetch
sends the same `JSESSIONID` (it should - cookies are cached per-domain).

---

# UPDATE — 2026-10-13 (LIVE verification: shim OCR works, left-drift is the last blocker)

The "robust path" (`tessnet2_32.dll` drop-in shim → host Tesseract 5.5.3) is **installed and
verified LIVE end-to-end**. The remaining blocker is **confirmed and quantified**: the
**left-drift** in `HTMLElementRender_DrawToDC` (`dlls/mshtml/htmlelem.c`).

## What was verified live (2026-10-13, prog.vers 1.3.2.0 / db.vers 1.4.1.5)
- Launched `tebeos_medula.exe` under wine; SGK login page renders.
- Clicking the toolbar **reload button at screen (74,40)** fires the automation
  (note: screen (32,42) is the **back** arrow — the old `click_reload.py` is wrong).
- Automation fills `Kullanıcı Adı=<ECZANE_KODU>`, `Parola`, ticks KVKK, and OCRs the captcha.
- The shim reads clean **6-digit** values (the old "trailing 1" and the `tessnet2`-weak
  `2601291`-style garbage are **gone**). E.g. captcha `415683` → OCR `415683` ✅.

## Left-drift: quantified
The instrumented `DrawToDC` (`dump_blit_full`, still in the installed `mshtml.dll`)
writes one 140×30 BMP per call to `C:\captcha-dumps\blit_<N>.bmp`. Measured the leftmost
bright/digit column per blit — the content **slides left ~1 px per DrawToDC call**:

```
blit_4  bright-left=14
blit_5  =13  blit_6 =12  blit_7 =11  blit_8 =10  blit_9 =9  ...
blit_18 =0   blit_19 =0    <- leading digit now touching/clipping left edge
blit_20 =16                <- drift RESETS on captcha reload (fresh image)
blit_21 =15  ...  blit_35 =1
```

Consequence: within one captcha's lifetime the app calls `DrawToDC` ~16×, each call drifts
1 px, so the **leading digit is progressively clipped** and OCR degrades:
- fresh captcha `415683` → OCR `415683` (correct) early, then `475583` (wrong) mid-cycle;
- later captcha `312308` → OCR `312309` (last digit off), then **`12309` / `27058` (5 digits,
  leading digit clipped)** as drift accumulates.

Because SGK requires an exact 6-digit match, the login never succeeds and the app loops.

## Root cause of the drift
`DrawToDC` = `nsIDOMWindowUtils_Redraw()` then `GetBoundingClientRect()` then
`BitBlt` from the window surface. The captcha `<img>` is **intrinsic 150×50 squished to
140×30**, and each forced `Redraw` re-rasterises that squished image at a drifting subpixel
transform (Gecko 2.47 layout/paint state accumulates; it resets only when the image reloads).
This is a **rendering drift**, not an OCR problem.

## Fix direction (NOT yet done)
Render the captcha element **without** the drifting `Redraw`+`BitBlt`. Options, in order of
preference:
1. **Decode the loaded `<img>`'s source data directly** (get raw bytes from the element's
   image request and decode with `OleLoadPicture`) — bypasses viewport rendering entirely.
2. **Memoise the blit per image generation** — re-blit only when the img src / loaded content
   changes; return the cached (un-drifted) bitmap on repeat calls.
3. Re-fetch the captcha URL with the session cookies and decode that (works because the
   servlet overwrites the session's expected value), but changes what the app OCRs vs shows.

## Fix DONE (2026-10-13): remove the forced `Redraw`
The drift is caused by `nsIDOMWindowUtils_Redraw()` re-rasterising the squished captcha at a
drifting subpixel transform. **Removing the `Redraw` call** (blit the window's already-painted,
stable surface instead) fixes it. Verified live:
- before: `415683`→`475583`→…→`12309`/`27058` (5-digit clipped leading digit); 32 renders.
- after: `350807`×2, `228576`×11 — **13 clean 6-digit reads, 0 short reads**, and the captcha
  field holds the correct value `228576`.

What was tried and rejected:
- **Memoise by element `currentSrc`** (cache the render, keyed on the img URL) — does **not**
  cache-hit because the captcha `src` carries a cache-buster and changes on every reload,
  even when the session captcha image is unchanged. Removed.
- **Decode the img bytes directly** — wine's mshtml binding exposes no `imgIRequest`/
  `imgIContainer`, so the decoded pixels are unreachable (as concluded previously).

The `DrawToDC` change is in `dlls/mshtml/htmlelem.c` (`HTMLElementRender_DrawToDC`), rebuilt
with the llvm-mingw recipe in `wine-patch/README.md` and installed to the prefix plus
`wine-patch/mshtml/`.

## Remaining blocker (separate from OCR/drift)
Login still does not complete: the app loops retrying the **same** correct captcha (`228576`
×11) without advancing. The decompiled `Giris_YapMedEczane()` fills user + password +
captcha via `SetAttribute("Value", …)` and clicks `button1`; the live form shows user+captcha
filled but **password (`secret1`) empty**, so the submit never succeeds. This is the
"form never submitted" issue from the handoff §10a, a separate wine mshtml input/submit
problem (not OCR and not the drift).

## Hygiene notes for production
- The installed `mshtml.dll` still contains `dump_blit_full` instrumentation (writes
  `C:\captcha-dumps\blit_*.bmp` every DrawToDC). Strip it before shipping.
- `ocr_service.py` must be running whenever Medula is used (start it from the launcher).
- Only ONE `ocr_service.py` instance may run (two will race on `ocr-in/ocr-out`).

---

# UPDATE — 2026-10-14 (cache-read fix: stop re-fetching the captcha)

## Root cause of the re-fetch failure (established by inspecting the Wine prefix)
The "decode native PNG + re-fetch" path was feeding the app a captcha that did not
match the session the form-submit used, so every attempt 302'd (60+ retries, all
different OCR values). Key evidence from the live prefix:

- **Gecko (the WebBrowser control) routes its HTTP through wininet.** wininet's
  URL cache (`users/$USER/AppData/Local/Microsoft/Windows/INetCache/Content.IE5`)
  holds the login page's CSS/images AND the captcha response
  `SayiUretenImageYeniServlet` with its `Set-Cookie` headers. So cookies ARE
  shared, but the `InternetOpenUrlW` re-fetch regenerates a NEW captcha that gets
  stored under a session that diverges from the JSF form POST (the 302 loop).
- **The captcha `<img>` src is a STABLE URL, no cache-buster:**
  `<IMG width="140px" height="30px" src="/eczane/SayiUretenImageYeniServlet">`
  → resolves to `https://medeczane.sgk.gov.tr/eczane/SayiUretenImageYeniServlet`,
  which is also the exact wininet cache key. The cached body is a valid **150×50
  PNG** (native, un-squished).
- Session cookies: `JSESSIONID` (Path=/; HttpOnly), `oam.Flash.RENDERMAP.TOKEN`,
  `TS011f1c29` / `TS011eeb86` (F5 persistence), `TS4c6368c0027` — all in wininet.

## Fix applied
`dlls/mshtml/htmlelem.c` `fetch_and_decode()` now **reads the captcha bytes from
wininet's URL cache** (`RetrieveUrlCacheEntryStreamW` + `ReadUrlCacheEntryStream`)
and decodes them, instead of re-fetching. This returns the EXACT image the browser
already displayed (the one the session expects). Re-fetch remains only as a fallback
when the cache has no entry. The re-fetch body-collection now uses a growable
heap buffer; decode is shared via a new `decode_picture()` helper.

Rebuilt both arches (llvm-mingw), installed to the prefix and `wine-patch/mshtml/`.

## Open risk to confirm live
If wininet serves the captcha BODY from cache across page reloads (the response
uses `cache-control: no-cache="set-cookie, set-cookie2"`, and there are two
`SayiUretenImageYeniServlet` cache entries at different times = it IS being
re-fetched fresh), the displayed captcha stays in sync with the session and the
cache-read works. If it ever serves stale, the fix degrades back to the same
symptom — the fallback `+wininet` cookie diagnostic in the handoff is still the
tool to distinguish the two.

# UPDATE — 2026-10-14 (RESOLVED: missing submit button `button1` was the last blocker)

## Root cause of the "form never submits" failure
Wine's mshtml `IHTMLFormElement::submit()` builds the form data with a **NULL submitter**:
`nsIDOMHTMLFormElement_GetFormData(This->nsform, NULL, ...)` in `HTMLFormElement_submit`
(htmlform.c). `HTMLFormElement_handle_event` intercepts the `submit` event with
`*prevent_default = TRUE` and re-submits this way, so a clicked `<input type=submit>`
(e.g. SGK's `j_id_jsp_2072829783_2:button1`) has its name/value **dropped** from the POST.
A JSF command button must be present in the POST for the login action to fire, so every
login attempt failed (302 → LogoutServlet).

## Fix
- htmlform.c: track a `pending_form_submitter` (the clicked submit control) and pass it to
  `GetFormData` as `aOriginatingElement` instead of NULL.
- htmlelem.c `HTMLElement_click`: detect `<input type=submit|image>` via
  `nsIDOMHTMLInputElement_GetType` and set/clear the pending submitter around the native
  `nsIDOMHTMLElement_Click`. A plain `form.submit()` still passes NULL (correct per HTML).
- mshtml_private.h: declare the `mshtml_set/get_pending_form_submitter` accessors.

## Verified LIVE (2026-10-14)
- POST now contains `…%3Abutton1=Giri%FE+Yap` (`%FE` = CP1254 `ş`, matching the page's
  windows-1254 charset) and the response changes from 302 to 200.
- The app completes the login and lands on the logged-in dashboard
  `<ECZANE ADI> (<ECZANE_KODU>)` with the Reçete / Fatura / Mesaj / Çıkış menu. OCR daemon
  stops retrying (last captcha `690759`). **Medula login automation now works under Wine.**
- Captcha OCR occasionally needs a few retries (same as on Windows; the app's retry loop
  handles it). All retries in the successful run carried `kvkkTaahhut=true`.

Build: `make dlls/mshtml/{i386,x86_64}-windows/mshtml.dll`; installed to
`wine-11.18…/lib/wine/{i386,x86_64}-windows/mshtml.dll` and `wine-patch/mshtml/*/`
(built from the same source tree, so it also carries the earlier cache-read fix and
DrawToDC work). Backups: `mshtml.dll.bak.1014` next to each installed DLL.
