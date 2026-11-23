# Spike: embed a headful Chromium in the Medula HWND (alternative to the screencast + GDI path)

Status: feasibility notes only. Nothing built or run yet. Branch: `spike/embed-headful-chromium`.

## Idea

Today (`tebeos-work/shim-dll`): headless Chromium over CDP -> `Page.startScreencast` JPEG
frames -> `stb_image` decode -> `StretchDIBits` into the Medula HWND (`render.c`), with input
forwarded through CDP `Input`.

Alternative: run a headful Chromium, find its X11 top-level window, and `XReparentWindow` it
into the X window that backs the Medula HWND. Chromium then paints itself and reads input
natively; no per-frame JPEG work.

## What the code already tells us

- Wine exposes the X11 id of an HWND to the same process as a window property:
  `GetPropW(hwnd, L"__wine_x11_whole_window")` (`toolchain/wine-11.18/dlls/winex11.drv/window.c:101,2464,2682,3023`).
  The shim runs inside the process, so it can read this id. The Medula window's X id is
  therefore reachable without a new Wine patch.
- Wine itself reparents X windows (`window.c:2315,2336`), so reparenting under a Wine-managed
  X window is a pattern Wine already uses.
- Chromium's `--user-data-dir` and the CDP port (9333) remain usable with a headful instance,
  so `Runtime.evaluate` / the event bindings (`__shim_evt`) can stay as they are.

## Open questions (each must be answered by a test, not by reading)

1. Does the Medula HWND have a real X window? Wine only creates X windows for some child
   HWNDs. If `__wine_x11_whole_window` is 0 for the Medula child, this approach needs a Wine
   patch or a different parent.
2. Does Chromium's top-level window survive `XReparentWindow` (size, map state, repaint,
   and no fighting with its own window manager requests)?
3. Is the session X11 or XWayland? The desktop is KDE; winex11 must be the driver in use.
4. Focus and input: keyboard and mouse go to the reparented X window directly. Does the
   vendor's IE-era behaviour still hold: click-hold on checkboxes, Enter moving to the next
   field, WM_MOUSE capture, and the context-menu hook (`docobj.c`)? Each of these is a
   behaviour the shim currently implements itself, so each must be re-checked.
5. KWin popup rule (`tebeos-popups`) and `<select>` popups: a headful Chromium would open
   its popups as real X windows. Check they stay above the Wine windows.
6. Efficiency: measure CPU per page change for the screencast path versus the embed path
   on the same Medula screen before claiming a gain.

## Spike plan (needs a private X server)

No Xvfb or Xephyr is installed on this machine, and `:0` must not be used. Needed:
`sudo pacman -S xorg-server-xephyr` (or equivalent), then run a nested X server on a free
display (e.g. `:6`).

1. Start Xephyr on `:6`. Start a Wine prefix (`~/.wine-spike`, never `~/.wine-tebeos`) with a
   small test program that creates a parent window and a child HWND.
2. In that program, read `__wine_x11_whole_window`, and print the id.
3. Start headful Chromium on `:6` (`--user-data-dir` in a temp dir, `--remote-debugging-port`),
   with a test page.
4. Find its X window (`_NET_WM_PID` or WM_CLASS via `xprop`), `XReparentWindow` it under the
   id from step 2, and map it.
5. Check with `xwd`/a screenshot that the page shows; send a click and a key (XTEST) and check
   the page reacts.
6. Record: result of each open question above, and the CPU comparison.

## Decision rule

Continue only if (1) the Medula child has an X window, and (4) the vendor's input behaviours
work without a rewrite. Otherwise, keep the screencast path.

## Wayland (checked 2026-11-18)

- The session is Wayland (`XDG_SESSION_TYPE=wayland`, KWin). An Xwayland server runs as `:0`.
- The running TEBEOS and Medula processes load `winex11.drv` (checked in `/proc/<pid>/maps`
  for the Kron4ek `wine-11.18-staging-amd64-wow64` build). So Medula's HWND is an Xwayland
  window with a real X id, and the `__wine_x11_whole_window` lookup applies unchanged.
- Chromium must be forced onto X11: `--ozone-platform=x11`. Its default on Wayland is a
  `wl_surface`, which has no X id and no reparent operation, so the embed would be impossible.
  With `--ozone-platform=x11`, both windows are X clients of the same Xwayland server, and
  `XReparentWindow` works inside it.
- Test server: a private `Xvfb` (or `xwayland-run`, which starts a separate Xwayland) instead of
  Xephyr. Xephyr is an X server and runs poorly inside a Wayland session. A private server keeps
  XTEST input and screenshots away from the live `:0`.
- Input: XTEST and `xwd` only reach X clients. That is fine on a private server, but any test on
  the live session cannot use them to drive real Wayland windows.
- Focus and stacking: KWin manages these windows through Xwayland. The `tebeos-popups` KWin rule
  still applies to X windows, but focus-stealing rules on Wayland differ. Re-check popup
  behaviour on the live session, not only on the spike server.

## Results (2026-11-18, private Xvfb `:6`, not `:0`)

Setup: Xvfb `:6`; headful `chromium --ozone-platform=x11 --no-sandbox` (156.0.8078.79) with a
test page; a throwaway Wine prefix `~/.wine-spike`, not the TEBEOS prefix.

1. **Reparent works.** `XReparentWindow` of Chromium's browser window (`WM_NAME` "... - Chromium",
   `_NET_WM_PID` = Chromium's pid) into a plain X window: the parent shows the full Chromium
   window with its own tab strip and address bar. The earlier client-leader window
   (`0x400001`, 10×10 hint) is not the browser window.
2. **Wine HWNDs (open question 1).** `wintest.exe` (`scratchpad` source copied here as
   `wintest.c`) prints `__wine_x11_whole_window`: the top-level window has an X id
   (`0x1200001` / `0x1a00001`), the `WS_CHILD` window has **0** (no X window). The shim's
   `ShimHtmlServer` is a child, so it would have to embed into its top-level ancestor.
3. **Embed into the Wine top-level X window works.** Reparenting Chromium under the top-level's X id
   showed the page inside the Wine window (`embed-wine.png`, 777 colours; the stand-in parent had 1).
4. **Input works (XTEST).** `input.py`: click on the button changed its text to "CLICKED"; a click
   in the text field and typed letters appeared in the field, and the page's `keydown` handler ran.

Open after this spike:
- Chromium's own UI (tab strip, address bar, the `--no-sandbox` warning bar) is visible. Use
  `--app=<url>` or similar to hide it: untested.
- Vendor behaviour inside a real Medula page (click-hold, Enter, context menu, clipboard, popups)
  is untested; this spike used a plain page.
- The child-HWND case: the embed target must be the top-level, so Chromium covers the whole top-level
  client area. Whether that conflicts with the vendor's own child windows is untested.
- Efficiency: not measured.
- Still untested on Wayland itself (`:0`): this spike ran on Xvfb.

Scripts: `xtool.py` (list windows, reparent), `reparent2.py` (reparent with an X error handler),
`input.py` (XTEST click/key), `mkparent.py` (stand-in parent window), `wintest.c` (Wine test).

## Live run attempt (2026-11-18, option 1: run despite production session)

- Throwaway prefix `~/.wine-embed-test` (copy of the production prefix, embed-mode shim installed).
  `TEBEOS_EMBED=1`, CDP port 9555, private display `:6`. Headful Chromium on X11 started and
  `embed_helper.py` reparented its window into the Medula window (`0xa00003`). The TEBEOS
  window rendered; the embedded area showed black (not yet diagnosed).
- The shim's SGK flow ran: the test login was submitted (`faces/login.jsp`), then SGK redirected
  to `LogoutServlet` and `PostLogoutRedirectServlet`. Test stopped after that, per the
  stop-at-first-refusal rule.
- **Side effect observed:** the production `tebeos_medula.exe` (pid 3190, running since before
  this run) was no longer running afterwards. Its Chromium (pid 3205, port 9333) is still up.
  Cause not established: an SGK session conflict, or the window being closed.

## Live run, second round (2026-11-18, spare PC, read-only DB use, no saves)

- Fixed in `page.c`: `--disable-gpu` added to the embed launch. A local page embedded this way
  rendered correctly (`gpu-off.png` in the scratchpad, 436 colours; button, field and key line visible).
- Medula in embed mode: Chromium's own CDP screenshot shows the SGK login page correctly
  (captcha, form, notices). The embedded area inside the Medula window is still black. So the
  black area is in how the reparented window is painted inside Medula, not in the page. Not yet diagnosed.
- SGK answered with the wrong-security-number page, and no session was established. Three
  login attempts in total across the run. Per the handoff, SGK answers a wrong captcha with
  `LogoutServlet`, so the earlier refusals were probably misread captchas, not a session conflict.
  That makes the production logout less likely to have been caused by the test, though not
  confirmed.
- Stopped after that. No saves or writes in TEBEOS.

## EGL run (2026-11-18, real session :0, spare PC, no saves)

- Embed launch changed to `--use-gl=egl` (replaces `--disable-gpu`). Verified on the real session:
  a local page embedded in a Wine top-level window renders correctly (`egl-apu.png`).
- Medula with that flag: the black box is gone. The embedded area is white instead, and
  Chromium's own screenshot shows the SGK page drawn correctly (`cdp-egl.png`). So the reparented
  window still isn't showing its content inside Medula. Not yet diagnosed.
- SGK refused the login (wrong security number page). One attempt this round. Stopped.
- Test processes stopped.

## Correction and root cause of the test refusals (2026-11-18)

- **Refusals were a test-rig fault, not SGK.** The OCR daemon watches a fixed prefix
  (`TEBEOS_WINEPREFIX`, default `~/.wine-tebeos`). The test prefix's captcha requests sat unanswered
  in `~/.wine-embed-test/drive_c/ocr-in`, so the app fell back to `999999` (the wrong-captcha path in
  `captcha-root-cause.md`), and SGK answered with `LogoutServlet`. With a second daemon on the test
  prefix the captcha was read (`829427`) and the form was submitted.
- **Correction:** the "EGL works on the APU" result above was wrong. Chromium's GPU process fails with
  `--use-gl=egl` on `:0` ("Requested GL implementation (gl=egl-gles2) not found"), and restarts
  repeatedly. The plain-page render was probably software fallback. Do not use `--use-gl=egl` as the
  fix until the GL backend works on this session.
- After the POST, Chromium's browser process died (GLib assertions) during the GPU restarts, and the
  Medula process ended. Whether the login itself succeeded is not known.
- Stopped the test daemon; production's daemon untouched.

## Observed by the user (2026-11-18, real session :0, spare PC)

- With the test OCR daemon on the test prefix, the embedded page passed the captcha, the login
  succeeded, and the main Medula page appeared with the TEBEOS HTML injections. This is the first
  successful login through the embed path.
- Then Chromium closed. Logs: GPU process restarts with `--use-gl=egl` ("gl=egl-gles2 not found"),
  and browser-process GLib assertions at 14:25:58. The leftover test processes were killed by hand
  at about 14:43, after that failure, so the kill did not cause the window to close.
- Not yet determined what closed the window. Next: reproduce with `--disable-gpu` to check whether it
  stays up and the injections still render.

## Run 7 (2026-11-18, `--disable-gpu`, real session :0, spare PC, no saves)

- Build with `--disable-gpu`. The test OCR daemon on the test prefix answered the captcha. Login
  succeeded: the SGK page `ReceteIslem2.jsp` (Medula main page with the TEBEOS injections) showed in
  Chromium's own screenshot (`run7-cdp.png`).
- `embed_helper.py` picked the wrong window first (a new-tab window). Reparenting the app window
  (`0x180000c`) into the Medula window by hand showed the logged-in page inside the Medula window
  (`run7-embed-app.png`, 3342 colours). Page content and TEBEOS injections are visible in the embed.
- Open: the app window still has Chromium's tabs and address bar, so `--app` did not apply to the
  window that holds the SGK page. Helper should match the page title (`_NET_WM_NAME`, not `WM_NAME`).
- Left running for observation: Medula, the test Chromium (port 9555) and the test OCR daemon.

## Step 1 (2026-11-18): one browser window for the page

- Cause of the separate window: `Target.createTarget` with a new browser context opens a new window.
  Checked with a standalone app-mode Chromium: with no context, the target reuses the app window
  (same windowId). Embed mode now skips the separate context at boot (`page.c`, `g_embed`).
- Run 8: Medula logged in; the embedded window is the one with the SGK page, title "Medula Eczane",
  with the TEBEOS injections (`run8-medula.png`). Window still shows Chromium's tabs and a "Yeni Sekme"
  tab, so `--app` still does not apply. Cosmetic; not fixed.
- `embed_helper.py` now takes a separate Chromium page title (4th argument).
- Side effect of a bad kill pattern on this run: the production OCR daemon (pid 2988) was killed and
  restarted with default settings (pid 47527). Production Chromium (pid 3205) untouched.

## Step 2 start (2026-11-18): first navigation blanks the embed

- Clicking "Reçete Listesi" (navigation to `index.jsp`) left the embedded area blank. Chromium's own
  page was fine (`s2-cdp.png`). The window stayed mapped and sized (1000x700 viewable).
- Not a covering window (only the Medula window is top-level on `:0`), not a plain navigation
  (`page2.html` test in a plain parent kept painting), and not a missing repaint request
  (`Page.bringToFront` did nothing).
- Re-reparenting restored the content, but moved the window to (0,0). An X expose on the embedded
  window (`XClearArea`) restored it without moving (`s2-expose.png`). `embed_refresh.py` does this
  periodically. Cause not yet known: most likely Medula repaints its client area, which drops the
  embedded window's contents.
- Step 2 checks (click-hold, Enter, context menu, clipboard, print, popups) not run yet; blocked on
  this refresh loop being in place.

## Step 2, run 9 (2026-11-18): Medula exited when opening Reçete Giriş

- Clicked the "Reçete Giriş" menu item in the embedded Medula window (navigation only, no saves).
- The Medula window and process ended. Shim log: the page document was released with no error
  (`DOC ... released`). The Medula log shows no error for the exit. Cause not known: the click or
  TEBEOS closing itself. The following checks are blocked until this is understood.
- Test Chromium on port 9555 stopped afterwards. No SGK re-login made.

## Run 10 (2026-11-18): retry with click logging

- Login reached `index.jsp`. Embed with the helper (`0x1c00003` in `0x1a00003`) and the refresh loop
  (`embed_refresh.py 0x1c00003`). The window was blank once; one expose restored it.
- Click on the menu item at (104,231): Medula stayed up, the embedded page changed to the
  "E-Reçete Sorgu" form (form POST to `index.jsp`). Screenshot `r9-after-click.png`.
- **Click logging gap:** the menu click did not produce a `CLICK` line in the shim log. The shim logs
  clicks it sees on its own event path; embed-mode clicks reach Chromium directly. So the log does not
  show native clicks. Run 9's exit is still unexplained; this run did not reproduce it.

## Run 11 (2026-11-18): keys work; fit and restore flash

- User: F3 and other shortcuts work in the embed out of the box.
- Fit: the embedded window is a fixed 1000x700. Resized to (0,33) 1366x655 with `XMoveResizeWindow`
  (the Medula content area below the TEBEOS toolbar). Not verified on screen: the Medula window was then
  minimized (unmapped, `map_state` 0), which also made ImageMagick `import` and `magick x:` return
  nothing. `xcap.py` (Xlib `XGetImage`) was added for captures; it needs a mapped window.
- Restore flash: the shim log of this run has no `FRAME` lines and no `RENDER attach`, so the shim's
  GDI blit (`render_repaint`) does not run in embed mode. The flash is therefore not that path, unless
  it came from another shim instance. Which window flashed is not yet known.
- Tabs and omnibox still visible (`--app` not applied).

## Run 12 (2026-11-18): blank after restore, then maximize

- After the restore flash the embedded area was blank. The Chromium window was still a child of
  Medula, on `KartsizEreceteSorgu.jsp`. The earlier refresh loop had stopped.
- Added `embed_sync.py`: every 0.5 s it reads the Medula size (`XGetGeometry`), sets the Chromium child to
  (0,33) with the Medula width and the height minus 33 (top) and 55 (bottom: status bar), and sends an
  expose. Started as `embed_sync.py 0x1a00003 0x1c00003`. Ids change per run; pass the current ones.
- Not yet checked on screen after maximize.

## Run 16 (2026-11-18): app window, no tabs

- Cause of the tab strip: `--app=about:blank` opens the new-tab page, not an app window, and the
  shim's `Target.createTarget` opened a normal window. Standalone checks: `--app=data:text/html,...`
  gives an app window; navigating its own page target (no new target) keeps it an app window.
- Change (`page.c`, embed mode only): launch with `--app=data:text/html,<title>Medula</title>`;
  `page_create` takes the page target whose URL starts with `data:` (`Target.getTargets`) instead of
  creating one.
- Result: logged-in SGK main page fills the Medula content area, no tabs, no address bar
  (`run16-medula.png`). Only the `--no-sandbox` warning bar remains.
- `embed_sync.py` running (`0x1600003`, `0x1800003`). Medula left running for observation.
- Not checked yet: whether the warning bar goes away without `--no-sandbox` (needs a working sandbox).

## Run 17 (2026-11-18): no-tabs build breaks typing and right-click

- Reported: right-click does nothing; Reçete Giriş T.C. fields cannot be edited; "feels like GDI again".
- CDP on the SGK page: `document.hasFocus()` is false, active element is BODY. Keys do not reach the page.
- The same failure in standalone tests on `:0`: an app window and a normal window, both embedded in a plain
  X parent window, with a real click: `hasFocus` false and typed keys do not reach the input.
  So the focus loss is not specific to Medula or to app mode; it is how this session gives focus to
  an embedded child window.
- Medula's `WM_HINTS` say `input: False` (`xprop`). Setting `input=True` from the helper did not stick.
- Right-click: not investigated yet (may follow from the focus loss).
- Open choice: revert to the tab-window variant (keys worked there per the user), or keep investigating focus.

## Run 18 (2026-11-18): reverted to the tab-window build

- `page.c` restored from `bfce91a` (`--app=about:blank`, `Target.createTarget` in embed mode).
  Medula logged in; helper embedded the window (`0x1800003` in `0x1600003`); `embed_sync.py` running.
- With this build, a click on the page gives `document.hasFocus() === true` (`run18-revert.png`).
  So the focus loss in run 17 came from the no-tabs change (app window with a data URL, page reuse),
  not from the Medula window or the session. The standalone tests in run 17 did not reproduce the
  focus loss in the tab configuration, so they did not isolate the cause.
- Tabs are back (expected in this build). Typing and right-click still to be confirmed by the user.
- The no-tabs change stays in git history (`f49c3e0`) for later work.

## Run 19 (2026-11-18): T.C. field and right-click on the tab build

- The current SGK page is `RaporListe.jsp`; the table on screen is the SGK page itself, so the embed
  paints correctly here. The earlier "blank" was an almost empty page.
- Medula has one child window (the Chromium embed). The TEBEOS grid seen on screen is not a Medula child.
- Right-click: a `contextmenu` listener in the page recorded nothing after one real right-click at a
  blank spot (XTEST button 3). Left clicks do reach the page (focus test in run 18). So the right button
  is lost before Chromium, or the click lands on a TEBEOS window above Medula. Not yet resolved.
- T.C. field: not identified on this page. Need its location in Reçete Giriş from the user.

## Run 20 (2026-11-18): Reçete Giriş, T.C. field, right-click

- **Stale frames (the "blank" and "GDI" look):** Chromium's own screenshot showed the Reçete Giriş form with
  the T.C. field focused, while the X capture showed an old grid. Chromium stopped presenting frames to its
  X window. Unmapping and remapping the child (XUnmapWindow/XMapWindow) fixed it at once (`run20-remap.png`).
  An expose alone did not. Still to find: why Chromium stops presenting, and when it happens.
- **T.C. field (cause found):** the page has focus and the field is focused, but typed digits do not
  arrive. The shim used to send keys as keydown + keypress with text through CDP, and Backspace via
  `execCommand`, because hxclient's mask on the T.C. field cancels the raw keydown (handoff, see
  `HANDOFF-PATH-A.md`). Embed mode sends native X keys, so that translation is gone.
  Live test (not in the shim yet): a capture-phase keydown handler on the page that inserts printable
  keys with `execCommand('insertText')` and deletes on Backspace/Delete. With it, `1` went into `f:t18`
  and Backspace removed it.
- **Right-click (cause partly found):** `input.py rclick` reaches the page: `mousedown` button 2 and
  `contextmenu` fire, not prevented. So the click gets through; whether Chromium's native menu shows is
  unknown (a popup window; capture of the root window crashed in the helper).
- Not done: the key handler is not in `iecompat.js` yet. It must be embed-only, or the GDI path would
  insert each key twice.

## Runs 21-22 (2026-11-18): what changed since run 7

- Run 21: current build (`bfce91a` page.c) with embed only (no sync loop, no resize, no remap). User:
  right-click and T.C. fields still broken. So the external resize/remap was not the cause.
- Only code difference from run 7 (`a1614f0`): run 7 created a separate browser context (page in its
  own window); later builds use the default context in embed mode. Run 22 = run 7 build (rebuilt from
  `a1614f0` page.c, installed in the test prefix only; repo page.c unchanged). Logged in, SGK window embedded.
- **Test method caveat:** on `:0` (Wayland), XTEST clicks/keys reach Xwayland windows only when the
  Xwayland surface has Wayland focus. `_NET_ACTIVE_WINDOW` was `0x200000` (not Medula, likely a Wayland
  window). In run 22 a real-mouse-style XTEST click on the menu produced no `mousedown` in the page.
  Several earlier automated focus/typing results on `:0` (runs 17-20) are therefore not reliable.
  Only the user's own mouse/keyboard tests count on this session.

## Run 22 result and next idea (2026-11-18)

- User: the run 7 build also has broken right-click and T.C. fields. The production Medula was not active
  during run 7 (its shim log was last written at 13:30). So no code or setup difference explains the run 7
  report; it cannot be reproduced.
- What fits all evidence: right-click and the T.C. field depend on the shim's own input handling
  (IE-style menu in `docobj.c`; keys sent to the page the way vendor handlers expect; the T.C. field has
  TEBEOS keydown/keypress/keyup sinks in the shim log). In embed mode input goes straight to Chromium.
- Idea: Chromium only draws; input falls through to the Medula window and the shim. `input_passthrough.py`
  gives the Chromium window an empty X input shape. Applied to `0x180000c`; waiting for the user's test.
  Known gap: the shim maps its window to the page viewport, so with Chromium's tab strip and border the
  click positions are offset. A real version needs Chromium's page area exactly over the shim's window.

## Pass-through result (2026-11-18)

- User: with `input_passthrough.py`, right-click works (the shim's IE-style menu), clicks are misaligned,
  and Chromium's own context menu appears next to the shim's: the shim forwards the right-click to
  Chromium through CDP, and a headful Chromium draws its native menu for it (headless never did).
- Screencast: not running in embed mode. Only `page_attach_window` starts it, guarded by `!g_embed`;
  `render_start` is used only by `renderlive.c`. The earlier "no FRAME lines" argument was invalid
  (FRAME is logged only in trace mode), but the code settles it. The flash on restore is most likely
  the compositor showing Medula's last buffer before Chromium repaints, not the shim's GDI path.
- Design that follows: Chromium draws, the shim keeps all input handling. Needed:
  1. iecompat.js: cancel the native context menu after recording `__shim_ctx` (harmless in headless mode).
  2. Chromium's page area exactly over the shim's window: app window without tabs (`f49c3e0`), no
     `--no-sandbox` (removes the warning bar), size via `Browser.setWindowBounds` from the shim,
     position from the helper using the shim window's rect.
  3. Helper: reparent, position, empty input shape.

## Run 23 (2026-11-19): Chromium draws, the shim keeps input

- `page.c` (embed mode): app window from `data:` URL, no `--no-sandbox`, page target reused,
  `embed_place()` sizes Chromium's window to the shim window through `Browser.setWindowBounds` and writes
  `%TEMP%\mshtml-cdp-embed.txt` (Medula X window, shim window screen rect) on attach and every resize.
- `iecompat.js`: a second `contextmenu` listener cancels Chromium's own menu after `__shim_ctx` is recorded.
- `embed_run.py`: finds the browser by CDP port, reparents its window at the shim window's offset, moves
  only (never resizes), empty input shape. Chromium rewrites its process title, so `/proc/<pid>/cmdline`
  is one space-separated string.
- Result: page viewport 1363x680 = outer size = shim window size; placed at (1,24) in Medula; no tabs,
  no warning bar, logged in (`run23-pass.png`).
- Known gap: Wine draws TEBEOS controls that overlap the shim window (bottom zoom buttons, F2 hint, WinForms
  panels such as the T.C./Ad Soyad list) inside Medula's own X window, so the Chromium child hides them.
  Possible fix: the shim lists the visible HWNDs above its window and the helper cuts them out of
  Chromium's X bounding shape.
- Log wording: `BOOT createBrowserContext failed` is printed when embed mode skips the context on purpose.

## Run 24 (2026-11-19): Chromium's own context menu

- User (run 23): T.C. fields editable, clicks land where clicked, no blank screen. Right-click: wanted
  is the browser's own menu; the shim's IE-style menu only existed because headless Chromium has none.
- `docobj.c` `doc_context_menu`: in embed mode, after the host's `ShowContextMenu` (TEBEOS may still take
  over, as in IE), return without the shim's menu; the right click was forwarded to Chromium, which
  shows its own. `page_embed_mode()` added. GDI mode unchanged.
- `iecompat.js`: removed the listener from `6997c6e` that cancelled Chromium's menu.
- `page.c`: embed mode logs "BOOT embed mode: default context" instead of "createBrowserContext failed".
- Edge case: if the host shows its own menu (S_OK), Chromium's menu also appears, because the click was
  already forwarded.

## Normal TEBEOS on this spare PC switched to embed mode (2026-11-19)

- `runtests.sh` on the private display (`~/.wine-spike`, `:6`, GDI mode): 25 passed, 0 failed, both arches.
- `~/.wine-tebeos`: shim from `8195c39` installed (atomic copy); previous DLLs kept as
  `mshtml_cdp.dll.bak-20261119` in `syswow64` and `system32`. Without `TEBEOS_EMBED` it behaves as before.
- `tebeos/start-tebeos-client.sh` (backup `.bak-20261119`): `export TEBEOS_EMBED=1` and starts
  `embed_run.py <prefix> <port>` (one instance); the helper exits two minutes after the last TEBEOS program.
  Log: `~/.cache/tebeos-embed_run.log`. TEBEOS main does not load the shim, so only Medula is affected.
- The old headless Chromium on 9333 (from the GDI session) was stopped; otherwise the shim would attach to it.
- Back to GDI mode: remove the embed lines from the launcher (the DLL can stay), or restore the backups.

## Second Medula blank (2026-11-19)

- Second Medula from TEBEOS showed only the toolbar; shim log `PAGE createTarget failed`. Chromium on 9333
  was still running from the first session, its app window still on the SGK page. `embed_app_tid` only
  accepted a `data:` page, so it found none.
- Fix (`page.c`, embed mode): take the start page if present, else any other web page of the app window
  not held by this process (`g_embed_busy`); `page_close` keeps the page instead of `Target.closeTarget`
  (closing the last page would close the app window). If no page is free, fall back to
  `Target.createTarget` (a normal window) and log `EMBED no free page`.
- Installed in `~/.wine-tebeos`; previous embed build kept as `mshtml_cdp.dll.bak-20261119b`.

## Medula still blank after reopening (2026-11-19)

- With `bccaabd` the second Medula reused the page (`PAGE ... tab DE73...`) and wrote the new Medula window to
  the hand-off file, but nothing was embedded: X destroys a window's children with it, so closing the first
  Medula destroyed Chromium's embedded window. Chromium kept running without a window.
- Fix (`page.c` boot, embed mode): if a Chromium already answers on the port, send `Browser.close`, wait up
  to 5 s for the port to go quiet, then launch a fresh one. Every Medula gets its own Chromium window.
- `runtests.sh` for `bccaabd`: 25 passed. Installed with backup `mshtml_cdp.dll.bak-20261119c`.

## Input reached Chromium again (2026-11-19)

- With a fresh Chromium per Medula, the helper embeds the window right after Chromium creates it. The
  embedded window's input shape was found back at full size (1363x680): Chromium resets it while it sets
  up or resizes its window, after the helper had cleared it. So T.C. fields and right-click broke again.
- `embed_run.py` now checks the input shape on every pass (`XShapeGetRectangles`) and clears it when it is
  not empty, logging `input shape of ... cleared`.

## Blank after another app covered Medula; menu would not close (2026-11-19)

- User: with another app in front and Medula brought back, the page was blank; hovering made single
  elements reappear. Recorded `document.visibilityState` every 0.5 s while the user did this: it stayed
  `visible` (focus blinked once). So Chromium did not consider itself hidden; the pixels were lost on the
  X side. Likely: Wine repaints the whole Medula window when it comes back, including the area under
  Chromium's window, and Chromium only redraws what changes (the hovered element).
- `embed_run.py`: Chromium's window background set to None (an expose then does not clear the window first:
  that is what made periodic exposes blink); when `_NET_ACTIVE_WINDOW` becomes the Medula window, expose
  now, after 0.3 s and after 1 s.
- Menu: user could not close Chromium's right-click menu except by choosing a command. Chromium's window
  ignores input, so a click outside the menu goes to the shim and Chromium never sees it. While a Chromium
  menu window (`_NET_WM_WINDOW_TYPE_*MENU`, mapped) is open, the helper watches the pointer buttons every
  50 ms and sends the menu a synthetic Escape on a new press outside it. Untested whether Chromium accepts
  a synthetic key event; fallback would be XTEST.

## Clicks landed below the pointer (2026-11-19)

- User: clicking just above a button clicked the button. Medula had not moved (origin (0,28)); the helper's
  intended offset was (1,24) but Chromium's window was at (1,38): Chromium had moved its own window 14 px
  down after the helper placed it, and the helper only moved it when its own numbers changed.
- `embed_run.py` now compares the window's real offset in Medula every pass and moves it back when it
  differs (logged `moved ... from x,y to x,y`). Corrected at once on the running Medula.
- Also confirmed by the user: the redraw after another app and the menu close on an outside click work.

## Position gap closed (2026-11-19)

- Gap: the shim reported its window only on attach and on a new size. A layout change inside Medula
  without a new size, or a moved Medula window, left the report stale; since `8defd05` the helper would
  then actively move Chromium to a wrong offset (old screen position minus new Medula position).
- Shim (embed mode): a 0.5 s timer on the shim window (`EMBED_TIMER`, `docobj.c`) calls
  `page_embed_tick` -> `embed_place`, which returns at once when nothing changed, only resizes Chromium
  (CDP) when the size changed, and rewrites the hand-off file on any change. `page_resize` reports moves too.
- Helper: the offset inside Medula is computed once per report (when the file changes), not from an old
  report and Medula's new position. When Medula moves, Chromium's window moves with it as its child.
- Installed in `~/.wine-tebeos`, backup `mshtml_cdp.dll.bak-20261119d`.
