# HANDOFF — Path A: "IE→CEF" shim (modern engine behind the Medula WebBrowser control)

> **2026-11-19: embed mode — a visible Chromium draws the page inside the Medula window; the shim
> keeps all input.** Merged into `main` (from `spike/embed-headful-chromium`). The image
> `tebeos-linux-20261119.run` installs it, on by default. Tested on the spare PC (KDE Plasma on Wayland,
> Xwayland `:0`, 1366x768, one monitor); the pharmacy PC still runs the 2026-11-17 setup. The user's
> verdict: works, and works better than the screencast. Every run: `tebeos-work/embed-spike/README.md`.
>
> **How it works**
> - `TEBEOS_EMBED=1` (set by `tebeos/start-tebeos-client.sh`) switches the shim to embed mode. Without
>   it the same DLL is the old GDI/screencast shim, so removing the variable is the way back.
> - `page.c` boot: a leftover Chromium on the port is closed (`Browser.close`) and a fresh one launched,
>   visible, `--ozone-platform=x11 --disable-gpu --app=data:text/html,<title>Medula</title>`. A fresh one
>   each time because X destroys a window's children with it: closing Medula destroys Chromium's window.
> - `page_create` takes a page of that app window (`Target.getTargets`, `embed_app_tid`) instead of
>   `Target.createTarget`, which would open a normal window with tabs and an address bar. `page_close`
>   keeps the page. No separate browser context in embed mode.
> - `embed_place()`: Chromium's window gets the shim window's size through `Browser.setWindowBounds` (never
>   resized from outside; Chromium then knows its size), and `%TEMP%\mshtml-cdp-embed.txt` gets the
>   Medula X window (`__wine_x11_whole_window`) and the shim window's screen rectangle. Called on attach,
>   on `page_resize`, and every 0.5 s from the shim window's `EMBED_TIMER` (`page_embed_tick`); it returns at
>   once when nothing changed and resizes Chromium only on a new size.
> - `tebeos-work/embed-spike/embed_run.py` (started by the launcher, exits two minutes after the last
>   TEBEOS program): reparents Chromium's window into the Medula X window at the shim window's offset and
>   keeps its X input shape empty, checked every 0.5 s because Chromium resets it after setup and resize.
>   Clicks and keys therefore reach Medula and the shim, exactly as in GDI mode. It also: computes the
>   offset once per report (Chromium moves with Medula as its child); moves Chromium back when Chromium
>   moves its own window (seen 14 px down); asks Chromium to redraw (background None + X expose, no blink)
>   when Medula becomes the active window again, because Wine repaints Medula over it; closes Chromium's
>   menu with an Escape on a click outside it (that click goes to the shim, not to Chromium). Log:
>   `~/.cache/tebeos-embed_run.log` (image: `~/.local/share/tebeos/embed/embed_run.log`).
> - Right-click: the shim forwards it to Chromium, which shows its own menu; `doc_context_menu` skips
>   the IE-style menu in embed mode (after asking the host, as IE does).
>
> **Lessons (each one cost a test round)**
> - Input must go through the shim. With native X input, TEBEOS's handlers on the T.C. fields (keydown,
>   keypress, keyup sinks) and the right-click broke; the exact mechanism was not found.
> - Never resize or remap Chromium's window from outside; that gave stale frames and wrong menus.
> - `--use-gl=egl` fails on this APU (`gl=egl-gles2 not found`, GPU process restarts, browser died);
>   `--disable-gpu` works.
> - On Wayland, XTEST clicks/keys reach Xwayland windows only when they have Wayland focus. Automated input
>   tests on `:0` are unreliable; only the user's own mouse and keyboard count.
> - Test-prefix logins need their own OCR daemon (`TEBEOS_WINEPREFIX=<prefix> python3 ocr_service.py`);
>   otherwise every captcha is `999999` and SGK answers with `LogoutServlet`.
> - X destroys a window's children with it: a Chromium left from an earlier Medula has no window, hence a
>   fresh Chromium per Medula. Chromium's process title is one space-separated string in `/proc/<pid>/cmdline`.
> - "Blank page" had three different causes: stale frames after outside resizes, Wine repainting Medula
>   after another app was in front (page stayed `visibilityState: visible`), and a missing page target.
>
> **Known gaps**
> - TEBEOS controls that Wine draws over the web area (bottom zoom buttons, F2 hint, WinForms panels such
>   as the T.C./Ad Soyad list) are hidden behind Chromium's window; clicks on them still work. Possible fix:
>   the shim lists the visible HWNDs above its window, the helper cuts them out of Chromium's X bounding shape.
> - Untested: other resolutions, display scaling (HiDPI), more than one monitor (the helper assumes Windows
>   screen coordinates and X root coordinates share an origin), desktops other than KDE.
> - If the host's `ShowContextMenu` answers S_OK, Chromium's menu appears as well (click already forwarded).
> - Chromium's menu has items IE's did not (İncele/DevTools, Geri, Yeniden yükle); a policy can hide them.
> - One Medula at a time per CDP port (one app window). The helper is Python outside Wine; better: a small
>   native helper the shim starts and stops itself.
>
> **Inventory: what is core, what exists only because Chromium was headless**
> - Core, needed in both modes (the bulk of 16.7k lines, 7.6k of them generated `stubs.c`): the COM/IE DOM
>   bridge (`shim.c`, `stubs.c`, `dispmap.c`, `dispwrap.c`, `element.c`, `window.c`, `jsprop.c`, most of
>   `page.c`), CDP (`cdp.c`, `json.c`), the read cache `rcache.c` (a prescription opened in 28 s without it),
>   vendor event sinks and click hold (`LISTEN_FN`, `iecompat.js` §3), the input path (`docobj.c` WM_* →
>   `input.c` CDP Input), IE semantics in `iecompat.js` §1 (untyped buttons) and §5 (textarea innerText),
>   captcha `DrawToDC` through a canvas, navigation and BeforeNavigate2.
> - Headless-only, removable once embed mode is the only mode (about 800–900 lines; check each against the
>   visible Chromium before removing): `render.c` screencast and GDI blit plus `WM_PAINT` repaint and the
>   screencast ack in the pump; the IE-style menu in `doc_context_menu`; in-page `<select>` lists
>   (`iecompat.js` §2: check native drop-downs with forwarded clicks); `window.print`/Ctrl+P through
>   `printToPDF` and a RAW job (`iecompat.js` §6, `doc_print`: check Chromium's own print dialog); the
>   clipboard bridge `clip_put`/`clip_get` (check Chromium's clipboard with keys forwarded by the shim);
>   the `HeadlessChrome` user-agent rewrite.
> - Embed-only additions: `g_embed` paths in `page.c` (launch, `embed_app_tid`, `g_embed_busy`, boot close,
>   `embed_place`, `page_embed_mode`), the menu skip in `docobj.c`, `embed_run.py`, the launcher lines.
> - Keeping both modes for long is itself the overhead. Plan: use embed mode on the spare PC; if it holds,
>   make it the only mode, then remove the headless-only parts one at a time.
>
> **State on the spare PC**
> - `~/.wine-tebeos` has the shim built from `343ca96`. Backups next to `mshtml_cdp.dll` in `syswow64` and
>   `system32`: `.bak-20261119` (GDI build before this work), `.bak-20261119b` to `d` (earlier embed builds).
>   Launcher backup: `tebeos/start-tebeos-client.sh.bak-20261119`.
> - `runtests.sh` in GDI mode (throwaway `~/.wine-spike`, Xvfb `:6`): 25 passed, both arches.
> - Back to GDI mode: delete the embed lines in the launcher (image install: `TEBEOS_EMBED=0`) and restart TEBEOS.
> - Image: `image/build-client-image.sh` packs `embed_run.py`; `setup.sh` installs it to
>   `~/.local/share/tebeos/embed` and the launcher it writes sets `TEBEOS_EMBED=1` and starts the helper.
> - Test prefixes `~/.wine-embed-test` and `~/.wine-spike` were deleted on 2026-11-19. The shim tests need a
>   throwaway prefix again: see "Workflow" in the 2026-11-12 block.
> - Next steps: test the image on another PC; holes for overlapping TEBEOS controls; native helper owned by
>   the shim; try native drop-downs, print and clipboard in embed mode; then remove headless-only code.

> **2026-11-12: in daily production use. The pharmacist works in Medula through the shim on
> display :0.** The fixes below came from the pharmacist's bug reports, in shim-dll commits
> 60dc1ab..63e773d. Each one has a check in `eventtest.c`. `runtests.sh` passes 25/25 on both
> arches.
> - **Keyboard** (`docobj.c`, `input.c`):
>   - Every key goes out as keydown, keypress with text, then keyup, like a real keyboard.
>   - F3 / "Hasta Arama" work.
>   - Backspace/Delete go through `document.execCommand`, because hxclient's mask on the T.C.
>     field cancels the raw keydown.
>   - Left/Right in text fields move the caret with `setSelectionRange` instead of a keydown:
>     Shift extends the selection, Ctrl moves by word.
>   - Enter sends a keypress 13. SGK's `medula.js` and TEBEOS's `ft43focus` use it to move to
>     the next field.
>   - `page_evt_drain()` waits until the Input events already sent are acknowledged, so a
>     later `Runtime.evaluate` cannot overtake them.
> - **Mouse** (`docobj.c`): every message carries the held-buttons mask. SetCapture on press,
>   and click counts (double/triple), make scrollbar thumb drags and track clicks work.
>   WM_MOUSEWHEEL / WM_MOUSEHWHEEL are handled.
> - **`iecompat.js`** runs in every document. Edit the .js, then run `python3 gen-iecompat.py`
>   to regenerate `iecompat.h`. It handles:
>   1. An untyped `<button>` becomes type=button, as in IE7.
>   2. `<select>` lists are drawn inside the page, because headless Chromium never shows its
>      popup widget.
>   3. **Click hold:** a vendor mousedown/mouseup on a checkbox or radio makes the shim cancel
>      the native click. The click is replayed (`el.click()`) after the C side has run the
>      vendor handlers; it calls `__shim_ack(q)`. In IE, mouseup runs *before* the click flips
>      the box. Without the hold, Reçete Giriş's drug checkboxes (`f:tbl1:N:checkbox7`, with
>      TEBEOS `Medula_Recete_Parser.Ilac_Bilgisi_checked`) unticked themselves.
> - **The basic limitation behind most of these bugs:** vendor event handlers run
>   *asynchronously*, after the browser has finished the event. They arrive through the
>   `__shim_evt` binding and are dispatched on the UI thread. Anything IE lets a handler do
>   synchronously has to be emulated in the page instead: `cancelBubble`, `returnValue`, and
>   reading state before the default action. One example is `LISTEN_FN` in `element.c`: a
>   click on a checkbox or radio that has a vendor click handler calls `stopPropagation`.
>   Without it, Reçete Listesi's "kontrol" checkbox opened the prescription.
> - **Barcode scanner on Turkish F (2026-11-17):** the desktop layout is Turkish F (KDE, from
>   localectl `tr`/`f`); the scanner (USB 0581:011C) types as US/Q, so lot letters came out wrong
>   (26E811 → 26Ğ811; digits fine). Not Wine. `tebeos/scanner/70-barcode-scanner-trf.hwdb` remaps
>   only that device's keys (udev hwdb, root install). A wrong lot does not break TEBEOS's
>   `kod_27` function (checked read-only); its 22004 error that day came from the prescription
>   not yet being in TEBEOS's DB (created 2 min later by another terminal).
> - **Hidden drop-down lists under KDE (2026-11-17):** e.g. "Zayi Tipi Seçiniz" in Zayi
>   Bildirimi opened nothing. The DevExpress pop-up *is* mapped (managed, untitled,
>   `_NET_WM_USER_TIME=0`, not topmost), but KWin's focus-stealing prevention restacks a window
>   that may not take focus under the active program's lowest window -- below every TEBEOS
>   window. Menus are topmost, so they were fine. Fix: KWin rule `tebeos-popups` (untitled
>   `^tebeos_.*\.exe$` windows → keep above); both installers add it via `kde_popup_rule` when
>   kwriteconfig6 exists. Other desktops: untested.
> - **Cash register program (2026-11-17):** TEBEOS's built-in install downloads
>   `TebeosÖdemeNoktası64_Terminal.exe` and starts it while TEBEOS runs; the installer then
>   fails (it replaces files TEBEOS has open; Inno's Restart Manager can't close apps under Wine).
>   Run it with TEBEOS closed: works. It overwrites shared DLLs in `app\` (e.g.
>   `tebeos_pos_serverLib.dll` 2026-02 → 2025-04) and reuses TEBEOS's Inno AppId -- vendor's.
>   Installed on the production PC (`C:\kasa-setup.log`); every terminal uses it, so the image
>   now carries it: build phase `pos` runs the vendor's current installer silently in the
>   build prefix (registry + `PosReqFile.exe` included; its `[Run]` postinstall launch is
>   skipped in silent mode). Open: the device connection (USB/serial or network; make/model
>   not known yet).
> - **Saved-prescription page add-ons (2026-11-17):** on a prescription opened from Reçete
>   Listesi / Günlük, TEBEOS's add-ons (Hasta button `DevamRecet2`, stock `Ehover*`, the
>   Teslim Alan `f:text37` hook and its WinForms `panel2` list) were missing, and later the
>   T.C. / Teslim Alan fields would not delete. Both were gone after restarting the Medula
>   window, so not reproduced. The add-ons run from `Form1` DocumentCompleted, status 8/22
>   (form `f` action `ReceteIslem2.jsp`, `GetStatus` reads it via `GETATTR_FN`, correct), gated
>   by `Bu_Recete_Eklenmis_Mi` (was true) and several session flags (`EnjectorDurum`,
>   `DevamRecetesiFormKontrol()`, `UyariAcBtnBasildi`...): a stuck flag in a long session is the
>   likely cause. The shim log showed nothing after `NAV complete` while it failed. hxclient's
>   number assist on those fields deletes fine with the shim's `execCommand('delete')`
>   (checked in a local page with SGK's real hxclient scripts). If it recurs: create
>   `%TEMP%\mshtml-cdp.trace`, restart Medula only if needed, read what TEBEOS asks for after
>   the page loads; strip `>>`/`<<`/`FRAME` lines from the log afterwards (patient data;
>   trace mode also logs every frame). `DISPWRAP DispHTMLTableRow: no member for DISPID 4`
>   (thousands a day) is not yet explained: the id is not one our GetIDsOfNames handed out.
> - **Printing (2026-11-16):** headless Chromium has no print dialog and ignores
>   `window.print()`. `docobj.c doc_print` does IE's print: the right-click menu has
>   "Yazdır..." and "Baskı Önizleme...", plus Ctrl+P, OLECMDID_PRINT/PRINTPREVIEW
>   (DONTPROMPTUSER → default printer) and `window.print()` (`iecompat.js` §6 → `__shim_evt`
>   `{"t":"__print"}` → page.c → posted WM_DOC_PRINT). Chromium makes the PDF
>   (`Page.printToPDF`, paper/orientation/page range from the dialog's DEVMODE, IE-style margins:
>   title + "Sayfa x / y" on top, address + Turkish date below, optical zoom lifted while
>   printing). The PDF goes as a RAW job (`OpenPrinter`/`WritePrinter`), and Wine passes RAW
>   to CUPS with the format left to auto-detection, so CUPS sees `application/pdf`. Preview
>   saves `%TEMP%\medula-onizleme-*.pdf` and opens it with `winebrowser` (xdg-open). Test
>   without paper: `domtest` with `SHIM_PRINT_TEST=1` and `HOME` pointing to a dir whose
>   `.cups/lpoptions` says `Dest <queue> job-hold-until=indefinite` (Wine merges the user's
>   lpoptions through `cupsGetNamedDest`); check with `lpstat -o` / ipptool
>   `document-format-detected`, then `cancel`. The pharmacy's printer is a network
>   EPSON L3150 in CUPS.
>   **Selection only (2026-11-17)** — the pharmacist's most used print: with a page
>   selection the menu adds "Seçili Alanı Yazdır..." / "Seçili Alanı Önizle...", and the dialog
>   enables "Seçim" (as IE). `PRINT_SEL_JS` clones the selection's common ancestor (typed
>   field values copied in), prunes it to the range, wraps it in shallow ancestor copies so the
>   page CSS applies (position/height/overflow flattened), appends it as `#__shim_ps` with a
>   print-only style hiding the rest; `PRINT_SEL_UNDO` removes both after `printToPDF`.
> - **Client image (2026-11-13; rebuilt 2026-11-17 with the POS program and the KDE rule:
>   `~/.cache/tebeos-build/out/tebeos-linux-20261117.run`, not yet test-installed):**
>   `image/build-client-image.sh` (phases base, fonts, app, pos, linux,
>   pack, header) and `image/setup.sh` make a 1.1 GB `.run` that installs a ready TEBEOS client
>   (`--role client --server-ip IP`); see `BUILDING.md`. Tested by installing into a separate
>   HOME as another user: TEBEOS reached its login screen as a terminal, DB version read.
>   Traps met: winetricks lowercases font names with `tr`, which under tr_TR turns I into ı
>   (corefonts/tahoma silently skipped) — winetricks runs under C.UTF-8; and never edit a
>   build script while it runs (bash reads it as it goes).
> - **Karekod loop (2026-11-13):** Chromium's `textarea.innerText` is always "" (IE: the
>   box's text). TEBEOS reads the Karekod box that way, so "ITS Satış Bildirimi" saved no
>   karekods to the sale, and its repair routine (Form1 status 87: İptal → İşlemleri →
>   Geri Dön → Sonlandır) looped against SGK on one prescription. `iecompat.js` §5 makes a textarea's
>   innerText its value. Each vendor click or submit is now logged (`CLICK ...` lines: element
>   id/name and button label, never field values).
> - **Clipboard and right-click (2026-11-13):** headless Chromium's clipboard is cut off from
>   the desktop and it draws no context menu. `docobj.c` now does Ctrl+C/X/V/A and
>   Ctrl/Shift+Insert against the Windows clipboard (Wine shares it with Linux): the selection
>   is read from the page, paste goes in through `Input.insertText` like typing. A right click
>   asks the host's `IDocHostUIHandler::ShowContextMenu` first (WinForms answers S_FALSE
>   unless IsWebBrowserContextMenuEnabled is off), then shows IE's edit menu (Kes, Kopyala,
>   Yapıştır, Tümünü Seç), unless the page cancelled `contextmenu` (`iecompat.js` §4).
> - **Page zoom (2026-11-13):** `ReceteEventBtn` calls `ExecWB(OLECMDID_OPTICAL_ZOOM)` on
>   screens ≤ 1366 px wide, with no try/catch. The shim refused command 63 (`CMD Exec (null)
>   63 not supported` in the log), so .NET threw and the rest of TEBEOS's DocumentCompleted
>   handler was skipped on those pages; the Karekod page had no drug list. `docobj.c` now
>   does OPTICAL_ZOOM (CSS zoom on `<html>`) and OPTICAL_GETZOOMRANGE. Found with the trace:
>   create `%TEMP%\mshtml-cdp.trace`, restart Medula, and every request is logged with the
>   start and end of its text and the start of its reply (patient data: delete the file and
>   the `>>`/`<<` lines afterwards).
> - **Speed (2026-11-12):** each DOM call costs about 1 ms of Chromium CPU on this laptop.
>   TEBEOS finds elements with full collection scans (LINQ over `.All` comparing `.Id`),
>   often nested in loops. Opening a prescription made **31,265 calls, taking 28.4 s**.
>   - **`PROF` lines** (`page.c`): after every page load with 300+ calls, the shim logs its
>     busiest call shapes to `mshtml-cdp.log`. Use them to find slow vendor code.
>   - **`rcache.c`:** list items come in batches of 256, together with each element's id,
>     tagName and className. Those reads, and the list's length, are then answered locally.
>     Nothing else is cached; values, text and HTML are always read live.
>   - **A miss on id/tagName/className** fetches those labels for that element and the
>     255 stashed after it (`rc_elem_fetch`). That covers TEBEOS's lists built earlier with
>     `ToList()`, whose `.Id` it compares a dozen times per element.
>   - **Live results** (opening a prescription, `ReceteSorgu.jsp` commit → NAV complete):
>     before, 31,265 calls, 31 s; list batching alone, 16,026 calls, 16 s (14,008 `.Id`
>     misses left); with `rc_elem_fetch` (2026-11-13), **2,020 calls, 2.8 s in calls, 4.6 s
>     total**. What remains is spread out: 592 `length` reads of fresh
>     `getElementsByTagName('*')` lists (359), 223 batches, 195 `innerHTML`.
>   - **The cache is cleared** by any call not marked as a pure read (`rc_pure_enter`), any
>     vendor event dispatch, any WM_PAGE_EVENT, keyboard or mouse input, and after 1 s.
>   - **When adding a new read-only path,** wrap it in `rc_pure_enter`/`rc_pure_leave`.
>     Otherwise it clears the cache, which is safe, but slow inside a scan.
>   - `domtest` checks it: 600-element scan in 4 requests (was 3,000), and fresh values after a
>     vendor write, a vendor insert, and a page-script change once the second is over.
>   - Findings for the vendor letter, including this one: `VENDOR-NOTES.md`.
> - **Chromium:** it starts with `--optimization-guide-performance-class=1
>   --optimization-guide-disable-installer`. Otherwise it launches its on-device AI
>   service about 3 minutes after start. The port is 9333 and the profile is
>   `/tmp/tebeos-cdp-profile`. A new launch line needs a Chromium restart: close Medula, then
>   `pkill -f tebeos-cdp-profile`.
> - **Workflow:**
>   - Test in a throwaway prefix on the private display :5. Never use the user's :0 session or
>     `~/.wine-tebeos`.
>   - Set up and run the tests:
>     ```sh
>     (setsid bash run/nested.sh &)
>     mkdir -p /tmp/claude-keytest
>     WINEPREFIX=/tmp/claude-keytest/pfx WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
>     wine regsvr32 /s mshtml.dll                              # 32-bit
>     wine C:/windows/system32/regsvr32.exe /s mshtml.dll      # 64-bit
>     WINEPREFIX=/tmp/claude-keytest/pfx DISPLAY=:5 PORT=9560 bash runtests.sh [filter]
>     ```
>   - Install with an atomic copy (`cp` to `.mshtml_cdp.dll.new`, then `mv`) into
>     `~/.wine-tebeos/drive_c/windows/{syswow64←shim32,system32←shim64}/mshtml_cdp.dll`,
>     keeping a `.bak-YYYYMMDD[x]` beside it. The user then restarts Medula.
>   - Read-only checks on the live SGK tab: `python3 run/tabeval.py run 'expr'`.
>   - Shim log: `~/.wine-tebeos/drive_c/users/$USER/AppData/Local/Temp/mshtml-cdp.log`.
>     `EVENT x on H: n sink(s)` lines show which vendor handlers fired.
> - **Known, not addressed yet:**
>   - Very fast scrollbar drags can land slightly short, because Chromium coalesces moves.
>   - Chromium loads the desktop's plasma browser-integration extension (harmless).
>   - Cookie sync between wininet and Chromium; Medula screens nobody has used yet on Linux.
> - **Server role (2026-11-23):** `--role server --restore <TEBEOS backup>` in the same `.run`
>   installs the main computer: PostgreSQL 12.4 in its own prefix `~/.wine-tebeos-db`, a systemd
>   user service, the restore, `CLIENT=FALSE`. Tested with a synthetic backup, not yet with a
>   real pharmacy's. How it works and the move from Windows: `TEBEOS-SERVER-NOTES.md` §8.
> - **Next:** test the server role with a real backup on the spare PC (restore time, TEBEOS
>   login, Windows terminals against it). Encoding the installer's key sequence in the `app` phase.
> - **Vendor letter:** a Turkish letter to TEB built from `VENDOR-NOTES.md` is in
>   a separate letter (2026-11-13, not in this repo). Update `VENDOR-NOTES.md` with new findings.
> - **Repo and backups:** this repo is the public snapshot of the project (2026-11-23),
>   shim included. It is kept shareable, so it holds no credentials, pharmacy
>   identifiers or screenshots. The TEBEOS login for `run/runmedula.sh` is in the git-ignored
>   `run/login.env`. See `BUILDING.md`.

> **2026-10-22: acceptance test PASSED in the real app** (`run/e2e.py`, 22:46 box time).
> 1. Login on the first captcha.
> 2. The index page with the injected prescription table.
> 3. "Medula'da Aç" opens Reçete Sorgu, and the app posts Sorgula.
> 4. SGK returns the prescription (form action `ReceteIslem2.jsp`), as in a modern browser.
>
> The shim is installed; `run/shimctl.sh revert` puts Wine's mshtml back. Fixes this session,
> in shim-dll commits 90d13ef, 8d67897 and 8b934ad:
> - **CDP speed:** single-write frames and `TCP_NODELAY` (Nagle cost 43 ms per call), plus a
>   local `location.href`.
> - **Navigation:** BeforeNavigate2 aborts the paused request and re-issues it with its body,
>   Content-Type, Referer and Origin.
> - **Painting:** frames are painted in WM_PAINT on the UI thread.
> - **Focus timing:** IE fires focus/blur events from the message loop, not inside `focus()`.
>   Without that, hxclient's T.C. prompt underscores went into the POST and SGK returned the
>   empty form.
> - **Captcha:** `DrawToDC` draws the captcha `<img>` through a canvas, exact pixels. The
>   half-pixel screenshot clip caused wrong captchas, which SGK answers with `LogoutServlet`.
> - **OCR:** the daemon (`ocr-service/ocr_service.py`) votes over four readings per digit. It
>   read 24 of 24 live captchas.
>
> The runbook, traps and measurements were kept in a local Claude Code skill (`tebeos-shim`, not in this repo); the same content is spread through this file.
> - **Still open:** cookie sync between wininet and Chromium, and every Medula screen beyond
>   this flow.
> - **Before any SGK test:** one login per run, and stop at the first refusal.

> **Progress update (2026-10-20, `_newEnum` + `AttachEventHandler`/`attachEvent` DONE & verified).**
> Implemented and verified both arches vs Chromium 153: **`get__newEnum`→`IEnumVARIANT`**
> (the `foreach (element in collection)` loop the injected TEBEOS table uses) and, the last
> major blocker for "Medula'da Aç", **the full event-sinking loop**:
> `IHTMLElement2::attachEvent(name, sink)` stores the sink + injects an `addEventListener`
> that calls a CDP binding `window.__shim_evt(...)` (added lazily via `Runtime.addBinding`
> on the render channel); the render pump's `Runtime.bindingCalled` handler calls
> `shim_dom_event_dispatch` which invokes each matching sink's `IDispatch::Invoke(DISPID 0)`.
> **New `eventtest.c`** (design (a): full DocObject hosting → render pump running, then
> `getElementById → QI IHTMLElement2 → attachEvent("onclick", mockSink) →
> IHTMLElement::click()`) proves click→binding→dispatch→`Invoke(0)`: sink counter 1 then 2,
> and `detachEvent` stops it — **PASS both arches**.
>
> **Also done (same session, 2026-10-20):** `IHTMLElement2::get_readyState` (VT_BSTR
> "complete") and **`IHTMLDocument3::attachEvent`/`detachEvent`** (document-level event
> sinking — `HtmlDocument.AttachEventHandler`; the listener is registered on `document`
> and dispatched under a `DOC_HANDLE`(-1)/`WIN_HANDLE`(-2) sentinel shared with the element
> registry). `eventtest.c` verifies both: element sink + document sink fire and detach cleanly.
> **Two bugs found & fixed:** (1) the test harnesses' `shim_connect_fn`/`frame_info_fn`/
> `disconnect_fn` typedefs used `WINAPI` (stdcall) but the DLL's custom exports are `cdecl` —
> a 32-bit-only ABI mismatch causing a page fault at `shim_render_connect`; fixed in both
> `eventtest.c` and `hostrender.c` (domtest.c already used `__cdecl`). (2) `shim_dom_event_dispatch`
> guarded on `handle >= 0`, silently dropping the document/window sentinel handles; changed to
> `if (dom)` (the matching loop already matches the handle exactly).
>
> Remaining: `IHTMLWindow2`/`InvokeScript` + `IHTMLWindow3::attachEvent` (window-level events),
> **`DrawToDC`** (captcha), **bootstrap** (Chromium launch + CDP port discovery in the real app),
> **marshal the sink to the UI thread**, and the final `mshtml.dll` override install.

> **Progress update (2026-10-19, Milestone 7 DOM surface DONE).** On top of the Milestone 6
> render+input integration, the element DOM surface the injected TEBEOS table depends on is
> now implemented over CDP and verified both arches vs Chromium 153: **`IHTMLStyle`**
> (`HtmlStyle` object via `get_style`; `get/put_cssText` = the `.Style = "css"` path),
> **`IHTMLDOMNode`** on `HtmlElement` (`appendChild`/`removeChild`/`cloneNode`/`firstChild`/
> `lastChild`/`parentNode`/`hasChildNodes`/`childNodes`/`nodeType`/`nodeName`), and
> **`get/put_outerHTML` + `outerText`** (the `element.OuterHtml += "<button…/>"` injection
> path). `domtest.c` has Milestone-7 assertions — all PASS. Remaining: `_newEnum`/`foreach`,
> `IHTMLWindow2`/`InvokeScript`, **`AttachEventHandler`→`Runtime.addBinding`** (hardest),
> **`DrawToDC`** (captcha), **bootstrap** (Chromium launch + CDP port discovery in the real
> app), and the final `mshtml.dll` override install. Details in `tebeos-work/shim-dll/README.md`.

> **Progress update (2026-10-19).** The "render target" is fully unblocked, both arches,
> vs real Chromium 153: `rendertest.c` (screencast session + frame pump, valid **JPEG**
> frames 780×437) PASS; **decode+blit** (`blittest.c`) PASS — `stb_image.h` (vendored,
> public-domain) decodes the JPEG and `StretchDIBits` into a 32bpp BGRA DIB,
> byte-identical on read-back. Real bugs fixed en route (see
> `tebeos-work/shim-dll/README.md` "Milestone 5b" + "Decode + blit"): a
> heap-use-after-free/double-free in `json.c::j_free` (the actual blocker), an `acc`
> overflow + buffer-full bug in `cdp.c::b64_decode`, and a missing `SO_RCVTIMEO`.
> Remaining is *plumbing* (Milestone 5c): a worker-thread frame pump + GDI blit into the
> `docobj.c` `ShimHtmlServer` HWND, plus input forwarding via CDP `Input`.
>
> **5c-i done (2026-10-19):** `render.c` (thread-safe continuous screencast pump: worker
> thread decode→back-buffer→`StretchDIBits` into an HWND, fire-and-forget acks via new
> `cdp_command()`), verified by `renderlive.c` — the animated page's centre pixel flips
> RED↔BLUE across frames, both arches. Remaining: input forwarding + wiring `render.c`
> into `docobj.c`.

> **Read this first.** Status as of **2026-10-14 (late session)**. This supersedes the
> engine discussions in `AI-HANDOFF.md` §10/§10a and `captcha-root-cause.md`.
> Those two files remain correct for everything about *login/captcha/fonts/DB* and are
> the canonical reference for those topics; **this file is the canonical reference for
> the post-login "modern engine" problem and the chosen solution (Path A).**

---

## 0. TL;DR — where we are, what we decided

- **Medula login automation now WORKS under Wine.** (Missing submit `button1` fix is
  installed; see `captcha-root-cause.md` "RESOLVED".) Dashboard "<ECZANE ADI>
  (<ECZANE_KODU>)" renders, TEBEOS-injected "Karekodu Sonlandırılmamış Reçete Listesi" table
  renders.
- **Blocked at the next step:** SGK's left menu renders top-level items but **sub-items
  are blank**, and menu-driven navigation ("Medula'da Aç" → "E-Reçete Giriş") does not
  complete. Root cause = SGK's `hxclient` JS framework does not run on
  **wine-gecko 2.47 = Firefox 47 (2016)**.
- **User confirmed:** SGK's Medula works fine in **modern Chromium and Firefox**. So the
  problem is 100% the engine, not SGK and not the vendor's automation code.
- **Decision (user chose "A"):** stop polyfilling Firefox 47 (dead end) and instead build
  a **Wine-level shim that re-hosts the IE `WebBrowser` control onto a modern Chromium
  (CEF) engine**, exposing the IE COM DOM surface the app already uses. Vendor `tebeos_*.exe`
  stay **untouched** → survives the weekly vendor updates.
- **Why A over a rewrite:** TEBEOS's Medula business logic (login → navigate → parse
  prescription → write rows to Postgres) is vendor-maintained and updated weekly against
  SGK's changes. A shim lets the vendor *keep* maintaining that for us; a rewrite
  ("API engine") would make the user own that fragile screen-scraping forever. A also keeps
  the app working *identically* to Windows (pharmacist's daily driver).

---

## 1. Decisive facts (do not re-investigate)

1. **A modern Chromium (CEF) already runs inside TEBEOS under Wine, right now.**
   `app/cef.log` shows the bundled `libcef.dll` fully initialised and rendering: TLS to
   `fonts.gstatic.com`, a live YouTube embed on the dashboard (`https://local.app/`),
   GPU/GLES (`GL_OES_packed_depth_stencil`), V8 snapshot contexts. So "we need a modern
   engine" is **half-solved in-process**.
2. **The vendor is already migrating Medula to CEF.** ~30 classes use
   `ChromiumWebBrowser` (`CefSharp.WinForms`); several navigate to
   `rulesMedulaAdres.GetMedulaAdres() + "/login.jsp"` (e.g. `ReceteSiralamaFrm`,
   `TebRpMedulaSutForm`, `IlacBilgiForm`, `HastaninKullandigiIlaclar`, `Mustahaklik`,
   `EndikasyonMedulaForm`, `Hasta_Ilac_Bilgi_Browser`, `Recete_Rapor_Gorme`,
   `DevamReceteSiralamaForm`, `TebRpAiAktarim`). But the **core prescription flow is
   still IE**:
   - `tebeos_medula/Form1.cs` — `private System.Windows.Forms.WebBrowser wbMedula;`
     (line ~607), Url set to `https://medeczane.sgk.gov.tr/eczane/faces/login.jsp` (~12002).
   - `tebeos_medula.View.Medula.MReceteParser/Medula_Recete_Parser.cs` — the parser that
     writes patient/compensation/payment rows into the DB is **entirely
     `System.Windows.Forms.WebBrowser` + mshtml** (`static WebBrowser browser`, ~line 172).
3. **CEF already exposes CDP:** `RenkliRecete.cs` sets
   `CefBrowserSettings.RemoteDebuggingPort = 8080;` before `Cef.Initialize(...)`. So a
   Chrome DevTools Protocol handle into the *in-app* Chromium is already configured.
4. Host has **Firefox** only (no Chromium, no `node`, no `playwright`/`selenium` pip
   packages) as of this session — relevant only if a host-browser PoC is wanted (it is not
   required for Path A).

---

## 2. Exact architecture the shim must replace

The app talks to the page through two layers, both of which the shim must satisfy:

### 2a. The `WebBrowser` ActiveX (`IWebBrowser2`, SHDocVw)
`Form1.cs:2581`:
```csharp
SHDocVw.WebBrowser target = (SHDocVw.WebBrowser)wbMedula.ActiveXInstance;
new ComAwareEventInfo(typeof(DWebBrowserEvents2_Event), "BeforeNavigate2")
    .AddEventHandler(target, new DWebBrowserEvents2_BeforeNavigate2EventHandler(wb_BeforeNavigate2));
```
- `System.Windows.Forms.WebBrowser` → `.ActiveXInstance` → casts to the `SHDocVw.WebBrowser`
  coclass = **`IWebBrowser2`** (`Guid D30C1661-CDAF-11D0-8A3E-00C04FC9E26E`).
- Under Wine this is realised by `ieframe.dll` + `shdocvw.dll`, and the document by
  `mshtml.dll` **backed by wine-gecko**. That Gecko backing is the thing to replace.
- Events used (from `Form1.cs`): `BeforeNavigate2` (via `ComAwareEventInfo`) + standard
  WinForms `wbMedula.DocumentCompleted / Navigated / Navigating / NewWindow /
  ProgressChanged / FileDownload / PreviewKeyDown`.
- CLSID note: `System.Windows.Forms.WebBrowser` instantiates `CLSID_WebBrowser =
  8856F961-340A-11D0-A96B-00C04FD705A2`. The interop coclass GUID was stripped by ilspy
  (`[CoClass(typeof(object))]`), but Wine's own IDL has both.

### 2b. The DOM (`mshtml` COM interfaces, via `HtmlDocument`/`HtmlElement`)
All data extraction/automation is `wbMedula.Document.GetElementById(...)` etc. Concrete,
known calls the shim must cover (preliminary list — **enumerate exhaustively as task #1**):

- `Document.GetElementById(id)` → `IHTMLElement`
- `Document.GetElementsByTagName/TagName`, `GetElementsByName`, `get_images` (used by the
  captcha flow `HTMLDocument2_get_images` + `IMGElement_get_href`)
- `Document.All`, `Document.ActiveElement`, `readyState`, `Url`, `Title`, `Window`
  (`IHTMLWindow2`)
- `HtmlElement.GetAttribute` / `SetAttribute` / `InnerText` / `InnerHtml` / `Id` /
  `TagName` / `Style` / `Focus` / `All` / `Children`
- `HtmlElement.InvokeMember("click")` → `IHTMLElement_Click`  (IDispatch late bind)
- Concretely: `MedulaReceteClick` (see §3) does
  `webBrowser.Document.GetElementById("form1:menuHtmlCommandExButton51_MOUSE").InvokeMember("click")`.
- `IHTMLInputElement::get/put_value`, `get_type` (login + captcha fields)
- `IHTMLFormElement::submit` (login; already had to patch Wine for the missing submitter)
- `IHTMLElementRender::DrawToDC` (`IID 3050F669-98B5-11CF-BB82-00AA00BDCE0B`) — captcha
  snapshot; already implemented in Wine's mshtml (see `captcha-root-cause.md`), would be
  re-implemented via CDP `Page.captureScreenshot` in the shim
- `IHTMLElement2::createControlRange` + `execCommand("Copy")` (dead code path in the app)
- **DOM event sinking:** the app attaches .NET handlers to DOM events on *injected* elements
  (the green "Tebeos'ta Aç"/"Medula'da Aç" buttons use `onclick="return(hX_6.runEvent(this,event))"`),
  via `HtmlElement.AttachEventHandler` → `IHTMLDocument*`/`IHTMLElement*` event registration.
  The handoff observed these **do** fire; the failure is inside SGK's `hxclient` after the
  click, not in event delivery.

> Nearly every one of these maps 1:1 to a Chrome DevTools Protocol call
> (`Runtime.evaluate` for `getElementById/get/setAttribute/click/…`,
> `Page.captureScreenshot` for the captcha, `Page.navigate` for `Navigate2`,
> `Runtime.evaluate("location.href")` for the URL-keyed state machine). This is the core
> feasibility argument for the shim.

### 2c. ENUMERATED surface (task #1 done 2026-10-14) — this is the whole shim scope

Measured by grepping `~/toolchain/medula_decomp.cs/` (1317 files). The app talks to the
page almost entirely through the **managed** `System.Windows.Forms.WebBrowser` /
`HtmlDocument` / `HtmlElement` wrappers, which themselves bind to the COM interfaces below.
Direct raw-`mshtml` usage is rare and listed explicitly.

**`IWebBrowser2` (SHDocVw) — via `WebBrowser` control + `.ActiveXInstance`:**
- `Navigate`/`Navigate2` (`.Url` setter, `.Navigate`, `.Stop`), `get_Document`, `DocumentText`,
  `ReadyState`.
- Managed events: `DocumentCompleted`, `Navigating`, `Navigated`, `NewWindow`,
  `ProgressChanged`, `FileDownload`, `PreviewKeyDown`.
- Raw event: `BeforeNavigate2` attached via `ComAwareEventInfo` on the ActiveXInstance
  (`Form1.cs:2581`).

**`HtmlDocument` (→ IHTMLDocument2/3/4/5/6/7/8 + IHTMLWindow2):**
`GetElementById` **3186**, `GetElementsByTagName` 395, `CreateElement` 96, `Body` 58,
`Window` 42, `Url` 22, `ActiveElement` 10, `InvokeScript` 6, `Forms` 6, `Cookie` 2, `All` 2,
`Images` 1, `GetElementsByName` 1, raw `DomDocument` cast 11 (see raw list).

**`HtmlElement` (→ IHTMLElement + IHTMLElement2/3/4/5/6/7):**
`GetAttribute` **1210**, `SetAttribute` 370, `InnerText` **1259**, `InnerHtml` 315,
`OuterHtml` 38, `Style` 131, `Focus` 181, `Children` 42, `Parent` 214, `OffsetRectangle` 24,
`OffsetParent` 14, `ScrollRectangle` 4, `TagName` 125, `Id` 833.
- `InvokeMember("click")` 199 + `"Click"` 42 + `"onclick"` 4 = **245**.
- `AttachEventHandler` **300**, `DetachEventHandler` 3 (event sinking — the hardest piece).
- `getElementsByTagName` on elements, `createControlRange` 1, `execCommand` 2.

**Raw `mshtml` COM (direct casts, must exist as real COM objects):**
- `IHTMLDocument2` — `OcrLogin.cs`, `Ilac_Bilgisi_Form.cs`, `Form1.cs` (×4),
  `Form1_Model.cs` (`HTMLDocument`).
- `IHTMLElementRender::DrawToDC` (`IID 3050F669-98B5-11CF-BB82-00AA00BDCE0B`) — captcha;
  `OcrLogin.cs`, `Form1.cs` (already implemented in current mshtml over Gecko).
- Event sink interfaces: `HTMLDocumentEvents_Event`, `HTMLDocumentEvents2_Event`,
  `HTMLControlElementEvents_Event`, `HTMLTextContainerEvents_Event` (used via
  `AttachEventHandler` → `ComAwareEventInfo`).

**1:1 CDP mapping (the shim's implementation basis):**

| IE surface | Chrome DevTools Protocol |
|---|---|
| `Navigate2(url)` | `Page.navigate` |
| `get_Document` / `ReadyState` / `Url` | `Runtime.evaluate` (`document.readyState`, `location.href`) |
| `GetElementById` / `GetElementsByTagName` / `GetElementsByName` | `Runtime.evaluate` → RemoteObject |
| `Get/SetAttribute` / `InnerText` / `InnerHtml` / `OuterHtml` / `Style` | `Runtime.evaluate` / `Runtime.callFunctionOn` |
| `Focus` / `Click` / `InvokeMember("click")` | `el.focus()` / `el.click()` |
| `InvokeScript` | `Runtime.evaluate` |
| `AttachEventHandler` | `Runtime.addBinding` → page `el.addEventListener` → bound fn → `Runtime.bindingCalled` event → marshal to .NET handler on UI thread |
| `DrawToDC` (captcha) | `Page.captureScreenshot` clipped to element geometry |

> **Conclusion:** the shim surface is ~20 methods, dominated by `GetElementById`,
> `Get/SetAttribute`, `InnerText/InnerHtml`, `InvokeMember("click")`, and `AttachEventHandler`.
> Everything maps 1:1 to CDP. This is a bounded, tractable component — not a reimplementation
> of IE.

---

## 3. The concrete broken path to use as the acceptance test

`tebeos_medula.Controller.Index/IndexPageSonlandirilmamisReceteIslemleri.cs`:

- **"Tebeos'ta Aç" (left, works)** → `ReceteClick` → reads `elemrec` attr → opens local
  `SonlandirilmamisReceteFrm` (pure .NET/DB, no web).
- **"Medula'da Aç" (right, broken)** → `MedulaReceteClick`:
  ```csharp
  string text = htmlElement.GetAttribute("medularec");
  if (text.Length > 0) {
      reqReceteIslemReceteNo.SetAddReceteNo(text);
      webBrowser.Document.GetElementById("form1:menuHtmlCommandExButton51_MOUSE").InvokeMember("click");
  }
  ```
- The injected table is built by `SonlandirilmamisReceteTablosu()/SonlandirilmamisReceteYukle()`
  (document.createElement + `_tstok` append), and the buttons run
  `onclick="return(hX_6.runEvent(this, event))"`.

**Acceptance criterion for the shim:** after login, the left menu sub-items render, and
clicking "Medula'da Aç" actually navigates to "E-Reçete Giriş" (the SGK index.jsp menu
drives it) so the prescription flow can proceed. Same result a modern browser gives.

---

## 4. The plan (concrete tasks, in order)

0. **(this session — done)** Confirm facts, choose Path A, record this handoff.
1. **Enumerate the exact COM surface.** Write a small script that greps
   `~/toolchain/medula_decomp.cs/` for every `SHDocVw.*`, `mshtml.*`, `HtmlDocument.*`/
   `HtmlElement.*`, `.InvokeMember(`, `GetElementById/Name/TagName`, `get_/set_` call the
   app makes. Output = the definitive interface list (names + GUIDs from Wine's
   `dlls/mshtml/*.idl`). This scopes the shim precisely.
2. **Decide shim architecture** (two sub-options, see §5). Recommended: (B) below.
3. **Minimal vertical slice:** a COM DLL (registered to answer `CLSID_WebBrowser`) that
   implements `IWebBrowser2::Navigate2` by driving the **in-app CEF via its CDP port 8080**,
   plus `get_Document` → a stub `IHTMLDocument*` whose `getElementById→click` round-trips
   through `Runtime.evaluate`. Prove: app's `wbMedula.Url = login.jsp` renders in the in-app
   Chromium and a `GetElementById(...).InvokeMember("click")` executes.
4. **Grow the surface** until the §3 acceptance test passes.
5. **Reimplement `DrawToDC`** (captcha) via CDP screenshot; wire the existing
   `tessnet2` shim/OCR (already working; `ocr_service.py`).
6. **Hygiene:** strip debug `dump_*` from the installed mshtml (see `captcha-root-cause.md`
   hygiene note), revert the temporary wininet `TRACE` (`dlls/wininet/http.c` ~5738,
   `wininet.dll.orig` backup exists), keep everything in shim/Wine (never vendor EXEs).

---

## 5. Shim architecture — two sub-options

> **DECISION (2026-10-14): go with Form 1 (fresh `mshtml.dll` over CEF/CDP, keep `ieframe`).**
> Verified that Wine's `ieframe` talks to `mshtml` **only through public COM interfaces**
> (`IHTMLDocument2`, `IHTMLWindow2`, … in `dlls/mshtml/mshtml.h`), no internal symbol coupling
> (grep of `dlls/ieframe/*.c` shows only `#include "mshtml.h"` + COM calls). So a drop-in
> `mshtml.dll` (override) that renders via CEF and answers the §2c surface is feasible.
> First task (#2) = verify the exact `ieframe`→`mshtml` activation path (CoCreateInstance of
> `CLSID_HTMLDocument` / who owns the OLE in-place HWND) so the new mshtml slots in cleanly.

**Form 1 (chosen) — fresh `mshtml.dll` over CEF/CDP.** Keep Wine's `ieframe.dll` (OLE host,
   `IWebBrowser2`, window embedding, DocumentCompleted/BeforeNavigate2 events — all already
   working). Replace only `mshtml.dll` with a new DLL (builtin override, both arches like the
   existing patch) implementing exactly the §2c COM surface, rendering the page in a CEF/Chromium
   view and answering DOM calls via CDP. *Pros:* reuses ieframe's hard OLE/event plumbing; bounded
   surface (§2c); incremental. *Cons:* must nail the ieframe↔mshtml activation seam + get a CEF
   view drawn into the HWND ieframe provides (CefSharp.WinForms already proves CEF-in-WinForms
   works under Wine).

**Form 2 (fallback) — standalone COM server for `CLSID_WebBrowser`.** New DLL implementing
   `IWebBrowser2` + `IOleObject`/`IOleInPlaceActiveObject` + event connection points + the §2c
   DOM, all over CEF; re-register `CLSID_WebBrowser`. *Pros:* fully self-contained. *Cons:* must
   reimplement OLE in-place activation + event marshaling from scratch (what ieframe already
   does), which is a lot of boilerplate and easy to get subtly wrong.

**A (rejected) — Swap Gecko→CEF inside Wine's `mshtml.dll`.** Replace the Gecko (xul) backing of Wine's
   existing `mshtml` COM server with a CEF/CdpSession. *Pros:* reuses Wine's complete
   `IHTMLDocument*/IHTMLElement*` vtables, event plumbing, and OLE host (ieframe). *Cons:*
   Wine's mshtml is ~50k lines tightly coupled to Gecko's XPCOM model; there is no clean
   seam; huge and brittle against Wine upgrades.

**CEF instance choice:** reuse the app's already-initialised CEF on CDP **port 8080**
   (already set in `RenkliRecete.cs`) vs. self-host a dedicated CEF in the new mshtml. Leaning
   self-host (a) to keep the Medula document lifetime independent of the app's incidental CefSharp
   screens, and (b) so the shim owns the render HWND. Decide concretely in task #2.

## 5a. The exact seam (VERIFIED 2026-10-14) — what a fresh `mshtml.dll` must be

`ieframe` hosts the document as a standard OLE **DocObject** (Active Document). No Gecko
coupling anywhere in `ieframe`:

- `mshtml.dll` `DllGetClassObject` (`main.c:567`) serves **`CLSID_HTMLDocument`**
  (+ `CLSID_MHTMLDocument`). ieframe obtains the document by COM activation of that CLSID.
- `dochost_object_available` (`ieframe/dochost.c:339`) then QIs the document for **only**:
  `IOleObject` (`GetUserClassID` → expects `CLSID_HTMLDocument`, `SetClientSite`),
  `IHTMLDocument2` (`get_title`, ready state), and uses **`IOleDocumentView::SetRect`** to
  resize (`resize_document`, `dochost.c:394`).

So the new `mshtml.dll` (drop-in override, both arches) must implement the **document-side**
DocObject protocol + the §2c DOM surface — it does NOT need to touch `IWebBrowser2`/frame logic
(ieframe keeps that). Document-side interfaces to implement (minimum):

| Interface | Why (from source) |
|---|---|
| `IOleObject` | `SetClientSite`, `DoVerb` (in-place activate), `GetUserClassID`, `Close` |
| `IOleDocument` / `IOleDocumentView` | `CreateView`+`SetRect`/`Show`/`UIActivate` (ieframe resizes the view) |
| `IOleInPlaceObject` / `IOleInPlaceActiveObject` | the view's in-place activation |
| `IPersistStreamInit` / `IPersistMoniker` | load URL/content (mshtml's `persist.c`) |
| `IOleCommandTarget` | `CGID_MSHTML` commands (ieframe tests exercise these) |
| `IProvideClassInfo` | typeinfo (mshtml `get_class_typeinfo`) |
| `IHTMLDocument2` (+3/…/8 as needed) | the DOM consumed by the app (§2c) |
| `IHTMLElement` (+2…7) / `IHTMLWindow2` / `IHTMLElementCollection` | DOM |
| `IHTMLElementRender` | `DrawToDC` (captcha) |
| event sinks (`IHTMLDocumentEvents2`, `IHTMLControlElementEvents`, `IHTMLTextContainerEvents`) | `AttachEventHandler` → IConnectionPoint |

Plus whatever `get_doc_ready_state` uses (IHTMLDocument2 `readyState` or a prop-notify sink).
The rendering/JS is CEF; every DOM/event call above maps to CDP per the §2c table.

> **This is the concrete build target.** It is a bounded, incremental Cocoa-style project:
> (1) native COM DLL registering `CLSID_HTMLDocument`/`CLSID_MHTMLDocument`; (2) a CEF view
> drawn into ieframe's `IOleDocumentView` rect; (3) the §2c DOM/event surface over
> CDP/Runtime-binding. Next agent starts with the DocObject+`IOleDocumentView` skeleton and a
> CEF render target, then grows the DOM surface to the §2c list ordered by call frequency
> (`GetElementById` → `Get/SetAttribute`/`InnerText` → `InvokeMember("click")` →
> `AttachEventHandler` → `DrawToDC`).

---

## 6. Environment / recipes (unchanged, recap)

- Wine: Kron4ek wow64 **staging 11.18** at
  `~/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wine`; prefix `~/.wine-tebeos`
  (win64, Windows 7).
- Run Medula:
  ```sh
  cd ~/.wine-tebeos/drive_c/"Program Files (x86)"/TebeosPlusClient/app
  WINEPREFIX=~/.wine-tebeos WINEDLLOVERRIDES="mscoree=n" LC_ALL=tr_TR.UTF-8 \
    <wine> tebeos_medula.exe <user> '<password>'
  ```
- Login automation trigger: toolbar **reload button at screen (74,40)** (⚠ (32,42) = back).
- No xdotool; use `python-xlib` XTEST; screenshots via `spectacle -b -n -o FILE.png`.
- OCR daemon `~/Projects/TebeosOnLinux/tebeos-work/ocr-service/ocr_service.py` must run
  while Medula is used (captcha).
- Wine source + llvm-mingw build: `~/toolchain/wine-11.18/`; mshtml build:
  `make -j dlls/mshtml/{i386-windows,x86_64-windows}/mshtml.dll`; install to
  `<wine>/lib/wine/{i386,x86_64}-windows/` + `wine-patch/mshtml/*/`. Backups
  `mshtml.dll.bak.1014` next to installed DLLs. Recipe in `wine-patch/README.md`.
- Decompiled vendor source: `~/toolchain/medula_decomp.cs/` (1317 files). ilspy recipe in
  `AI-HANDOFF.md` §12.
- App logs: `app/{debug.log,cef.log,Log/KOLAY_SOFT_HATA.log}` (cef.log = Chromium console,
  **not** the Medula window — Medula is IE/Gecko).

---

## 7. Hard constraints (re-read before touching anything)

- **Never patch vendor binaries** (`tebeos_plus.exe`, `tebeos_medula.exe`, `*.dll` the vendor
  ships). Vendor updates executables + DB **weekly**; all fixes must live in Wine/shim.
  (Exception already agreed & shipped: `tessnet2_32.dll` host-Tesseract shim.)
- **DB read-only** when inspecting the real production DB. Medula creds and the TEBEOS login: ask the pharmacist (never commit them). Captcha is always **6 digits**.
- e-imza / ArkSigner: **not needed** (physicians only).
- Vendor is **Türk Eczacılar Birliği (TEB)**; KolaySoft only makes the cash-register software.
- Stay in permissioned dirs (project + `~/toolchain`); avoid unbounded logs (a full
  `+wininet` once hit 647 MB); OCR daemon single-instance.

---

## 8. Immediate next step for the next agent

1. Read `AI-HANDOFF.md` + `captcha-root-cause.md` + this file (in that order) for full context.
2. ~~task #1 (enumerate surface)~~ **DONE** — §2c. ~~task #2 (architecture + seam)~~ **DONE**
   — §5/§5a (Form 1: fresh `mshtml.dll` over CEF/CDP, keep `ieframe`).
3. **Engine half BUILT + self-tested (2026-10-19):** `tebeos-work/shim-engine/` holds a
   dependency-free CDP client (`cdp.py`, hand-rolled RFC 6455 WebSocket) + `MedulaEngine`
   (`engine.py`) implementing the exact §2c operations, a `demo.py` live driver, and a
   `selftest.py` (mock CDP server, runs with no Chromium: `python3 selftest.py` → PASS).
   This is the reference + test harness for the C++ shim.
4. **Milestone 1 of the native COM build is DONE (2026-10-19):** `tebeos-work/shim-dll/`
   builds (llvm-mingw, i686 + x86_64) a COM server for `CLSID_HTMLDocument` with a correct
   **116-method `IHTMLDocument2` vtable** (generated by `gen-stubs.py` from Wine's `mshtml.h`;
   only QI/AddRef/Release are real, rest are `E_NOTIMPL`). Verified under Wine both arches:
   `DllGetClassObject → CreateInstance → QI(IHTMLDocument2/IUnknown) → get_URL` all route
   without crashing. Key ABI notes are in `shim-dll/README.md` (C-mode `#define` wrappers →
   `shim_doc_*` names; `CLSID_MHTMLDocument` absent in mingw; non-const `lpVtbl`; `.def` exports).

   **Milestone 2 is DONE + verified (2026-10-19).** `gen-stubs.py` now emits a single
   `stubs.c`/`stubs.h` for **10 interfaces** (`IHTMLDocument2` + the full OLE DocObject
   document-side surface: `IPersistMoniker`, `IPersistStreamInit`, `IOleObject`,
   `IOleDocument`, `IOleDocumentView`, `IOleInPlaceObject`, `IOleInPlaceActiveObject`,
   `IOleCommandTarget`, `IProvideClassInfo`), each with a field-ordered `g_<Iface>_vtbl` and
   per-interface `shim_<Iface>_QueryInterface/AddRef/Release` wrappers routing to shared
   `html_doc_qi/addref/release` in `shim.c`. The `HTMLDocument` struct (10 `<Iface>_iface` +
   `ref`) is in `html_doc.h`. `probe.c` QIs all 12 IDs. **Build fix applied:** `html_doc.h`
   must `#include <initguid.h>` before the GUID headers (mingw `DEFINE_GUID` only declares
   unless `INITGUID` is set → otherwise `CLSID_HTMLDocument` is an undefined symbol).
   **Result:** 12/12 interfaces OK on both i686 and x86_64 under Wine. The now-stale
   `html_IHTMLDocument2_stubs.{c,h}` were deleted; `build.sh` uses `COMMON="shim.c stubs.c"`.

5. **Next (Milestone 4b):** replace the placeholder render window (currently a plain
   system-coloured child `ShimHtmlServer` window) with a real CEF browser drawn into the
   same HWND. Two options are under discussion: (a) self-host CEF in the shim — needs the
   CEF C-API SDK + import lib for the app's bundled CEF version, which are **not** in the
   toolchain; or (b) drive the app's in-process CEF via its `:8080` debug port using the
   new native CDP client (Milestone 4a), rendering via `Page.captureScreenshot`/screencast
   + GDI blit and forwarding input over the CDP `Input` domain. Then replace the
   `IHTMLDocument2` stubs with real CEF/CDP-backed DOM (frequency order as in §2c), then
   `DrawToDC`. Validate against the §3 acceptance test.

   **Milestone 4a (CDP transport) is DONE + verified (2026-10-19).** New `cdp.{c,h}` +
   `json.{c,h}` implement the CDP transport (Winsock + WS upgrade + client-masked frames +
   server frame reassembly + a `cdp_call`/`cdp_recv` layer) mirroring `shim-engine/cdp.py`.
   `cdptest.c` self-tests it against an in-thread mock WS server (hand-rolled SHA-1 + base64
   for `Sec-WebSocket-Accept`), proving the handshake, masking, SHA-1 "abc" vector, and the
   call/event JSON round-trip. **Also verified against real Chromium 153** (user installed
   it; `chromium --headless=new --remote-debugging-port=9222`): `livetest.c` does `/json`
   discovery → WS handshake against the real server → `Runtime.evaluate("1+2")=3` →
   `Page.navigate` → `document.title` — `PASS (live CDP vs Chromium)` on both arches. This
   de-risks driving a Chromium-family engine entirely over CDP. (Fix: `cdp_http_get` now
   honours `Content-Length`, since Chromium HTTP-keeps-alive.)

   **Milestone 3b is DONE + verified (2026-10-19).** New `docobj.c` implements the DocObject
   container-facing surface (`IOleObject::{SetClientSite,GetClientSite,GetUserClassID,
   DoVerb,Close}`, `IOleDocument::CreateView`, `IOleDocumentView::{SetInPlaceSite,
   GetInPlaceSite,GetDocument,SetRect,GetRect,Show,UIActivate,CloseView}`, `IOleInPlaceObject::*`,
   `IOleInPlaceActiveObject::*`), mirroring Wine's mshtml `oleobj.c`/`view.c`. `DoVerb` drives
   `IOleDocumentSite::ActivateMe` (as ieframe expects); during in-place activation a real child
   window is created into ieframe's window and `SetActiveObject` is issued on the frame's
   `IOleInPlaceUIWindow` part. New **`hosttest.c`** is a minimal DocObject *container* that
   replays ieframe's exact sequence (`GetUserClassID → SetClientSite → Load → DoVerb(SHOW) →
   ActivateMe → CreateView → SetRect → Show → UIActivate → GetWindow`) and asserts a valid
   `IsWindow=1` HWND. Both arches `PASS (DocObject hosting)`.

   **Milestone 3a is DONE + verified (2026-10-19).** `IPersistMoniker::Load` stores the URL
   (from `IMoniker::GetDisplayName`), `IPersistStreamInit::Load`/`InitNew` set ready state,
   `GetClassID` returns `CLSID_HTMLDocument`, `IsDirty` reports the dirty flag, and
   `IHTMLDocument2::get_URL`/`get_title`/`get_readyState` return real stored state. Built via
   a new generator hook: `gen-stubs.py` `REAL_IMPLS` maps `(interface,method)->C function`,
   emitting an `extern` decl (signature parsed from the Wine header) and wiring the vtable
   slot to it; the real implementations live in `shim.c`, and `stubs.c` now includes
   `stubs.h`. Probe round-trip (`CreateURLMoniker(login.jsp) -> Load -> get_URL == that URL,
   get_readyState == "complete"`) passes on both i686 and x86_64 under Wine.

   **Milestone 4b/5 is DONE + verified (2026-10-19): CDP-backed DOM COM surface.**
   `gen-stubs.py` refactored to an `OBJECTS` list, so it now also emits vtables for a second
   `HtmlElement` object (IHTMLElement + IHTMLElement2); `HTMLDocument` gained
   `IHTMLDocument3`. New `element.c` implements `IHTMLDocument3::getElementById` (stashes the
   element in the page's `window.__shim_els[]` and returns its index as an integer `handle`)
   plus `IHTMLElement::{getAttribute,setAttribute,get/put_innerText,get/put_innerHTML,get_id,
   get_tagName,get_parentElement,click}` and the `IDispatch` head (`GetIDsOfNames`/`Invoke`
   mapping `"click"/"focus"/"blur"`, which is the `HtmlElement.InvokeMember("click")` path the
   app uses), all over CDP `Runtime.evaluate`. The CDP connection is process-global
   (`g_cdp` in `element.c`), set by exported `shim_dom_connect(port)`; `cdp.c`/`json.c` are
   now linked into the DLL. New **`domtest.c`** drives it against real Chromium:
   `getElementById → getAttribute/get_innerText(UTF-8 Turkish)/get_tagName/get_id →
   Invoke("click")` → assert the click mutated a DOM attribute — `PASS` both arches.
   (Gotcha: a `data:` URL with no charset defaults to Windows-1252 in Chromium, mojibake'ing
   Turkish text — set `<meta charset="utf-8">` in test pages.)

   **Milestone 5a is DONE + verified (2026-10-19): collections + element/input surface.**
   Added a third object `HtmlElementCollection` (QI/AddRef/Release + `get_length` + `item`,
   backed by a second page stash `window.__shim_cols[]`); `_newEnum` (foreach) still stub.
   Implemented over CDP: document `IHTMLDocument3::{getElementsByTagName,getElementsByName}`
   and `IHTMLDocument2::{get_all,createElement,get_body,get_activeElement}`; element
   `get_children`/`get_all`/`insertAdjacentHTML`/`insertAdjacentText`/`scrollIntoView`/`contains`;
   `IHTMLElement2::{focus,blur,getElementsByTagName}`; and `IHTMLInputElement`
   `value/type/name/checked` (get+put) QI'd on the `HtmlElement` for `<input>`s. Fixed a
   `cdp_eval_bool` bug (`.checked`/`.contains` return a JSON boolean, not a number). `domtest.c`
   grown to 23 assertions (collections length/item, input value/type/name/checked, a Turkish
   `put_value("çığ")` round-trip on the login-password path, `createElement`+`setAttribute`,
   `get_body`) — `PASS` both arches.
