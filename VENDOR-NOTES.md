# Notes for TEB (TEBEOS vendor)

Findings from running TEBEOS Plus on Linux. Its Medula module runs through `tebeos-work/shim-dll`,
a stand-in for IE's `mshtml` that drives Chromium. Since 2026-11-19 (embed mode) Chromium draws
the page itself, in its own window placed inside the Medula window; keyboard and mouse still pass
through the shim, which delivers them in IE's order. Each finding gives the measurement, where
it is in TEBEOS (names from the decompiled `tebeos_medula.exe`), and a suggested fix. They are
the raw material for a letter to the vendor.

Several of these also matter on Windows. The vendor is moving Medula screens to CefSharp
(`ChromiumWebBrowser`), and there every DOM access is an IPC round trip, just as in the shim.

## 1. Performance: element search by full scans

**Measured (2026-11-12):**
- **Setup:** a 2012 laptop (AMD A10-4600M), opening a recorded prescription in E-Reçete Giriş
  (`ReceteSorgu.jsp` → prescription view).
- **Result:** after SGK's page had loaded, TEBEOS needed **31 s** to finish its injections.
  Its handler made **31,265 DOM calls**, taking 28.4 s:

| Calls | Time | What |
|---|---|---|
| 14,110 | 11.6 s | `HtmlElement.Id` |
| 6,046 | 5.4 s | `HtmlElementCollection.Count` |
| 5,656 | 5.4 s | `collection[i]` |
| 2,680 | 2.8 s | `GetAttribute("id")` |
| ~1,600 | 1.6 s | `TagName`, `GetAttribute("className")` |
| the rest | < 1.5 s | the real work: values, text, injected HTML |

The shim logs this breakdown after every page load (`PROF` lines in `mshtml-cdp.log`).

**Cause:** elements are found by walking a whole collection and comparing ids, often once per
table row and per column. For example, `Medula_Recete_Tag.ReceteIlacKurumRecetesi()`:

```csharp
for (i = 0; i < num; i++) {
    HtmlElement t1 = (from HtmlElement b in htmlElement.All where b.Id == "f:tbl1:" + i + ":t1" select b).FirstOrDefault();
    HtmlElement t2 = (from HtmlElement b in htmlElement.All where b.Id == "f:tbl1:" + i + ":t2" select b).FirstOrDefault();
    ...
```

Every line scans all elements of the table, and enumerating `HtmlElementCollection` reads
`Count` again at every step. Similar code appears in about 700 places (`from HtmlElement`),
for example:
- `Recete_Giris_Tag_Ekle.SiparisEkle()` and `TebRpMiniIconEkle()`:
  `.All where k.GetAttribute("id").Contains("checkbox7")`, to count rows;
- `Form1`'s Reçete Rapor block: one scan of every `SPAN` per row of `form1:tableEx1`.

**Why it matters for the vendor:**
- In IE each access is an in-process call, so the cost stays hidden.
- On slow pharmacy PCs it still adds up.
- Under CefSharp/Chromium each access costs about 1 ms, so these screens become many seconds
  slower.

**Suggested fix:**
- Use `Document.GetElementById("f:tbl1:" + i + ":t1")`: one call instead of a scan.
- Where a scan is unavoidable, materialise the collection once (`.Cast<HtmlElement>().ToList()`)
  and reuse it, instead of re-enumerating `.All` inside loops.
- Count rows with one query (e.g. `getElementsByTagName("tr").Count`).

**What the Linux shim does about it:** a read cache for list scans. The list's elements come
back in batches of 256, together with each element's id, tag name and class name. Writes,
events, input and a one-second limit clear it; see `tebeos-work/shim-dll/rcache.c`. In the
test, a 600-element scan went from 3,000 requests to 4. In the real app, the same prescription
screen now makes **2,020 calls and is ready 4.6 s after SGK's page arrives (was 31 s)**. This is
a workaround: with `GetElementById` the vendor code would need a few hundred calls, on Windows
and under CefSharp alike.

## 2. TEBEOS relies on IE-only event behaviour (matters for the CefSharp migration)

Each of these broke when the same page ran in Chromium. Each needed a workaround in the shim.

1. **`event.cancelBubble` in click handlers, set from .NET.**
   - **Where:** `Recete_Listesi_Tag_Ekle` and `Recete_Listesi_Gunluk_Tag_Islemleri` (the
     "kontrol edildi" checkbox, `fcheck:<i>:<receteno>`), and `Recete_Rapor_Kontrol_Form2`.
   - **Problem:** this only works if the handler runs synchronously inside the event. With an
     out-of-process browser the click has already reached the row (`rowActionReceteSec`) and
     opened the prescription.
   - **Fix:** stop propagation in page JS, or have the row's handler ignore clicks that come
     from the checkbox.
2. **Reading state in `onmouseup` before the click changes it.**
   - **Where:** `Medula_Recete_Parser.Ilac_Bilgisi_checked`, attached to every
     `f:tbl1:N:checkbox7` in Reçete Giriş.
   - **Problem:** it counts ticked boxes on mouseup and clears them all when exactly one is
     ticked. It relies on IE running mouseup before the click flips the box. When handlers
     run later, the new tick is cleared at once and the box "unchecks itself". The row
     handlers `Esdeger_Bilgisi_checked` and `Yazilan_Esdeger_Bilgisi_checked` have the same
     ordering assumption.
   - **Fix:** do the radio-style logic in the checkbox's `onclick`/`onchange`. SGK's own
     `checkBox(this)` already makes the boxes exclusive.
3. **`<button>` without `type`.**
   - **Problem:** the injected "Hasta Arama (F3)" / "E-Reçete Giriş (F2/F4)" buttons are
     `type=button` in IE7 mode but `type=submit` in modern engines. Every click then submitted
     SGK's form, and SGK answered with `LogoutServlet`.
   - **Fix:** always write `type="button"`.

4. **`ExecWB(OLECMDID_OPTICAL_ZOOM)` without a try/catch.**
   - **Where:** `Medula_Recete_Parser.ReceteEventBtn`, which runs on most prescription
     pages. When the screen is at most 1366 px wide it sets IE's zoom (100, or 70 when the
     warning panel `f:tableExUyariSecim` is open).
   - **Problem:** if the browser refuses the command, .NET throws, and the exception ends
     the whole `wbMedula_DocumentCompleted` handler. Everything after `ReceteEventBtn` on
     that page is skipped without a message. Under the shim (before it supported the
     command) the Karekod İşlemleri page lost its drug list
     (`Its_Karekod_Islemleri_Tag_Islemleri.Recete_Tablosunu_Getir`) on 1366 px laptops.
     CefSharp has no `ExecWB` at all.
   - **Fix:** wrap the zoom call in try/catch, and in the CefSharp version use
     `SetZoomLevel`. More generally, one failing step in the page handler should not
     silently cancel the rest.

5. **`TEXTAREA.InnerText` used to read the Karekod box.**
   - **Where:** `Form1` status 11 ("ITS Satış Bildirimi") and the repair routines 86/87
     (Karekod İptal → Karekod İşlemleri → Geri Dön → Sonlandır) read the scanned karekods
     with `GetElementById("form1:textarea1").InnerText`.
   - **Problem:** IE returns a textarea's current text there; Chromium (so CefSharp too)
     always returns `""`. The karekods were then never saved to the sale, and routine 87,
     which starts when the sale has no karekods, repeated its four SGK button presses without
     end (seen live on Linux, ~9 s per round).
   - **Fix:** read `.GetAttribute("value")` (or `.value` through script). The repair routine
     should also stop after one failed attempt instead of looping against SGK.

## 3. TEBEOS Ödeme Noktası (the cash register program)

The program itself works under Wine (user, 2026-11-19; installed on every terminal). The device
connection (USB/serial or network) has not been looked at. Findings from installing it (2026-11-17,
`HANDOFF-PATH-A.md`):

1. **TEBEOS starts the installer while TEBEOS is running.** Its built-in install downloads
   `TebeosÖdemeNoktası64_Terminal.exe` and runs it at once. The installer has to replace files that
   TEBEOS has open. On Windows, Inno Setup's Restart Manager closes TEBEOS first; under Wine it cannot,
   and the installation fails. Run with TEBEOS closed, it works.
   - **Fix:** have TEBEOS exit before it starts the installer.
2. **The installer overwrites shared DLLs in TEBEOS's `app\` folder with older versions**, e.g.
   `tebeos_pos_serverLib.dll` from 2026-02 is replaced by one from 2025-04. This happens on Windows too:
   TEBEOS then runs with an older library than the one it shipped with.
   - **Fix:** install only newer files (Inno's default version check, without `ignoreversion`), or keep
     the cash register program's libraries in its own folder.
3. **The installer reuses TEBEOS's Inno Setup `AppId`.** In Inno Setup the `AppId` identifies the
   installation (uninstall entry and log), so the two programs can get mixed up on update or removal.
   - **Fix:** a separate `AppId` for the cash register program.

## 4. Open observations (not in the letter: cause not isolated)

1. **T.C. fields with native keyboard input (2026-11-19, embed mode).**
   - **Seen:** when keys went straight from the desktop to the visible Chromium, the T.C. fields in
     Reçete Giriş (e.g. `f:t18`) could not be edited; other fields could. Delivered through the shim
     (keydown, keypress with text, keyup in IE's order; Backspace/Delete via `execCommand`), they work.
   - **Two candidates:** TEBEOS attaches `keydown`/`keypress`/`keyup` handlers to these fields (one
     sink each in the shim log), and SGK's own `hxclient` input mask on the T.C. field cancels raw
     keydowns (`HANDOFF-PATH-A.md`, 2026-11-12 block). Which of the two blocks native input was not
     found, so this is not yet a finding about TEBEOS.
   - **Why it may matter:** in CefSharp, keys go straight to Chromium, as in this test.
