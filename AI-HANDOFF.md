# TEBEOS Plus on Linux — engineering handoff

> ## LATEST (2026-11-19): embed mode, merged into `main`: a visible Chromium draws the Medula page
> inside the Medula window and the shim keeps all input (`TEBEOS_EMBED=1` in the launcher, helper
> `tebeos-work/embed-spike/embed_run.py`). Works per the user on the spare PC; the new image
> `tebeos-linux-20261119.run` installs it, on by default. The status block at the top of
> `HANDOFF-PATH-A.md` has how it works, the lessons, the known gaps, and an inventory of core vs.
> headless-only code.

> ## LATEST (2026-11-17): printing (incl. selection), scanner layout fix (udev hwdb), hidden
> DevExpress drop-downs under KDE (KWin rule, added by both installers), the cash register
> program (TEBEOS Ödeme Noktası) installed and packed into the image (build phase `pos`;
> new `.run` 20261117), and an intermittent loss of TEBEOS's add-ons on saved prescriptions
> (cleared by restarting Medula; how to trace it next time). Details in the status block at
> the top of `HANDOFF-PATH-A.md`.

> ## LATEST (2026-11-13): fixes for page zoom, clipboard/right-click, the Karekod loop (textarea
> `InnerText`), a ready-made client image (`image/`, see `BUILDING.md`), and notes for the vendor
> (`VENDOR-NOTES.md`). Details in the status block at the top of `HANDOFF-PATH-A.md`.

> ## LATEST (2026-11-12): Medula runs through the Path-A shim and is in daily production use.
> - The Path A shim is done and installed. It is a replacement `mshtml` (`mshtml_cdp.dll`)
>   that drives a headless Chromium.
> - Recent fixes are in the status block at the top of `HANDOFF-PATH-A.md`:
>   - keyboard, mouse, scrollbars and drop-downs;
>   - Reçete Listesi / Reçete Giriş checkboxes.
> - Build and setup steps are in `BUILDING.md`.
> - Findings to send to the vendor (performance, IE-only behaviour): `VENDOR-NOTES.md`.

> ## ⚠️ LATEST (2026-10-14): the post-login Medula blocker is the browser ENGINE, not SGK nor the app.
> Wine's IE stand-in is **wine-gecko 2.47 = Firefox 47**, which cannot run SGK's `hxclient`
> menu JS. Login now works; the menu is blank + navigation fails. **User confirmed Medula works
> fine in modern Chromium/Firefox**, and **chose Path A: build a Wine-level "IE→CEF" shim** so the
> vendor app stays untouched and inherits the vendor's weekly Medula maintenance. **Read
> `HANDOFF-PATH-A.md` first** — it is the canonical reference for this pivot; this file and
> `captcha-root-cause.md` remain canonical for login/captcha/fonts/DB.

Everything learned while getting **TEBEOS Plus** (Turkish pharmacy management software,
vendor **Türk Eczacılar Birliği / TEB** — the Turkish Pharmacists' Association) to run
on Linux under Wine.  (Note: *KolaySoft* is a **different** vendor — cash-register
software; TEBEOS ships and talks to a `yazarkasa` schema for it, but it is not part
of TEBEOS itself.) Written so that an agent or a
person can continue the work **without access to the original machine**.

Companion files in this folder:

| File | Purpose |
|---|---|
| `install-tebeos-client.sh` | distro-neutral installer for a client/terminal PC |
| `TEBEOS-CLIENT-README.md` | how to run that installer on the laptop |
| `TEBEOS-SERVER-NOTES.md` | notes for the machine that runs the database |
| `tebeos-facts.json` | the same facts, machine-readable |

---

## 0. Status summary

> **Session update — 2026-10-07 (DrawToDC finished + password verified).**
> The in-progress "canvas via injected `<script>`" approach for `DrawToDC` was abandoned:
> empirically a dynamically-appended `<script>` does **not** run within a pumped message
> loop, Wine's XPCOM exposes no decoded-pixel getter (canvas is JS-only; `imgIContainer::getFrame`
> returns an opaque moz2d `SourceSurface`), and there is no synchronous JS eval on Wine's DOM
> bridge. `DrawToDC` was re-implemented the way IE effectively renders an element: force a
> synchronous repaint via `nsIDOMWindowUtils::Redraw`, then `BitBlt` the element's exact
> bounding-box rectangle out of the browser window (fixes the stale "previous captcha" and the
> trailing-`1` off-by-one artifact). **Verified** on a local 6-bar PNG: the captcha `<img>` now
> renders correct pixels (no crash, no staleness). The **password** fix
> (`setAttribute("value",…)` mirrored to the input value property) was **independently verified** with
> a `type=password` input: `SetAttribute("value","çığ")` now sets `.value == "çığ"` (the Turkish
> characters round-trip correctly). Remaining: confirm on the live SGK server that the current
> captcha is read (not a stale frame) after a page reload.

> **Session update — 2026-10-01 (continued, after DrawToDC implemented).** Verified live
> end-to-end: (1) `wine-mshtml-tebeos.patch` regenerated to contain the **real**
> blit-based `DrawToDC` (was still the stub) and the rebuilt `mshtml.dll` (both arches)
> synced into `wine-patch/mshtml/`. (2) App launches, password `çığ` stored as CP1254
> `E7 FD F0` in `Ayarlar.ini`, **login succeeds** (Enter key) → dashboard
> "Terminal || Ip:192.168.1.101". (3) Medula button spawns
> `tebeos_medula.exe`; the SGK login page renders (Gecko `nsAppShell` window
> present). (4) The automation does **not** start on page load — the startup race is
> real — but clicking the toolbar reload button (~screen 32,42) triggers it: it fills
> user `<ECZANE_KODU>`, password, ticks KVKK, and **OCRs the captcha to `2284315`** (not the
> `999999` fallback) — so `DrawToDC`+`tessnet2` now produce real pixels and run.
> **Remaining blockers:** the OCR output is wrong (captcha has 6 digits, OCR returned 7),
> the form is never submitted ("Giriş Yap" not clicked), and the app stays stuck on the
> login page with no error and no retry. These two — captcha OCR accuracy + the
> startup race — are the only pieces left. See §10a.

**Working:** TEBEOS Plus runs on Linux. Server ("Ana Bilgisayar") install, database
bootstrap, login and main dashboard were all verified on CachyOS with Wine 11.2.

**Verified working:** vendor client installer + Wine, .NET 4.8, PostgreSQL 12.4 on
port 5454, 127 MB base schema load, app login, dashboard.

**Also verified (second machine, 2026-09-30):** the *terminal* role against a live
vendor server on the LAN — Wine 11.18 staging (Kron4ek wow64 build), db schema
1.4.1.2, project 1.3.1.8. Bugs #6–#10 below were found and fixed there, the UI's
fonts were made to match a Windows install, and **the Medula window now renders the
real SGK Medula login page** (§10.1). The app's own online update flow also ran and
hit a DB constraint error (§10.4).

**Not yet verified:** backup/restore round-trips, and Medula beyond the login screen
(whether the vendor's IE-style automation drives the site correctly on Gecko 2.47).

---

## 1. What TEBEOS actually is

* .NET Framework **4.5.2** WinForms application, built as **x86 (32-bit)**.
  `tebeos_plus.exe.config` → `supportedRuntime v4.0 sku=".NETFramework,Version=v4.5.2"`.
* UI toolkit **DevExpress 15.2** (bundled DLLs).
* **CefSharp / CEF** (bundled Chromium, `libcef.dll` ~166 MB) — this is the
  "Selenium-like" engine used to drive the SGK **Medulla** web application.
  The app also ships **WebView2** files and Tesseract OCR data (`tessdata/`, `ocr/`).
* Data access: **Npgsql** → local (or LAN) **PostgreSQL**, and the app drives
  PostgreSQL's own client tools (`pgsql\bin\*.exe`) for backup/restore/upgrade.
* Two roles, chosen in the installer or at the login screen:
  * **Ana Bilgisayar** (main computer) = server: runs the PostgreSQL database.
  * **Terminal** (client) = connects to the main computer over the LAN.
* The vendor ships **two different installers**; you only get one by default.
  The vendor's downloader tool (`YeniTebeosIndirV1002.exe`) offers:
  * Terminal → `tebeos_plus_client_setup_v1.exe` (~266 MB, **no** database schema)
  * Ana Bilgisayar → `tebeos_plus_setup.exe` (~382 MB, **includes the base schema**)

---

## 2. The proven environment

| Component | Value |
|---|---|
| OS tested | CachyOS (Arch family), kernel 7.2.x |
| Wine | **11.2 Staging** (from Lutris' bundled runner) |
| Wine prefix | win64 (`system32` + `syswow64`), Windows version **7 SP1 (6.1.7601)** |
| .NET | **4.8** installed via winetricks (`NDP\v4\Full Release = 0x80eb1`) |
| Winetricks verbs used | `remove_mono`, `winxp`, `dotnet40`, `dotnet48`, `ucrtbase2019`, `vcrun2019`, `quartz` |
| App version verified | project 1.2.9.3, DB schema 1.3.8.0 |

Notes:
* No system-wide packages were required on the original machine — Wine, winetricks
  and the .NET installer all lived in the user's home directory.
* 32-bit support matters (the app is x86). A Wine built with the *new WoW64*
  (`i386-windows` PE modules + `x86_64-unix`) works without any 32-bit host libs.
  Capability probe used throughout:
  `wine 'C:\windows\syswow64\cmd.exe' /c "echo ok32"` → prints `ok32`.
* `.NET 4.8 in a win64 prefix` is proven to work; don't force a win32 prefix.

---

## 3. Database contract (extracted from the binaries)

Decompiled from `tebeos_plus.exe` (`Client_db_Control_Model.getConnectionString`):

```
Host     = <Host> element of <install>\tebeos_plus.xml      <-- NOT from Ayarlar.ini
Port     = 5454                                             (hardcoded)
Database = tebeos_db                                        (hardcoded)
Username = tebeos                                           (hardcoded)
Password = (set by the vendor; not reproduced here)                                       (hardcoded)
Encoding = UTF8,  ApplicationName = TEBEOS_PLUS
```

* Data directory expected at `<install>\data`, configs at `<install>\conf\*.dist`.
* The app temporarily rewrites `pg_hba.conf` around backup/restore (`start_pga_hba_config` / `stop_pga_hba_config`).
* The app's own tools: `pgsql\bin\{createdb,createuser,initdb,pg_ctl,pg_dump,pg_dumpall,pg_restore,psql}.exe`
  — all **PG 12.4, x64 Windows builds**, and all of them run fine under Wine.

---

## 4. How the app finds its configuration (critical for clients)

Two files, both in the install root:

### `tebeos_plus.xml` — where the database is
```xml
<?xml version="1.0" encoding="UTF-8"?>
<Settings>
  <UpdateUrl>http://tebeosplus.teb.org.tr:8001/version/main.xml</UpdateUrl>
  <CurrentVersion>1.0.3.5</CurrentVersion>
  <RequiredSql>0.0.3.3</RequiredSql>
  <Host>192.168.1.50</Host>          <!-- <<< DB host: 127.0.0.1 on the server,
                                              the LAN IP on a terminal -->
  <Updateidentity>1.0.3.0</Updateidentity>
</Settings>
```
`SaveXmlTerminal(host)` rewrites only `<Host>`, preserving the other elements —
so the updater can safely rewrite this file.

### `Ayarlar.ini` — role and terminal info
```ini
[CONNECTİON]            ; server address used by the terminal logic
CLIENT=192.168.1.50
[BAGLANTI]              ; TRUE = this PC is a terminal, FALSE = main computer
CLIENT=TRUE
[HOSTNAME]
ANABILGISAYAR=TEBEOS-PC ; name of the main computer
[LOGIN_KULLANICI]       ; written by "Beni Hatırla" (remember me)
HATIRLA=TRUE
KULLANICI=<user>
SIFRE=<password>
```

Read by `TerminalIni()`:
`connection ← [BAGLANTI] CLIENT`, `ip ← [CONNECTİON] CLIENT`,
`hostname ← [HOSTNAME] ANABILGISAYAR`.

### ⚠️ The Turkish `İ` trap (this is what broke everything at first)

The C# code reads the section **`"CONNECTİON"`** — with a Turkish dotted capital
**İ (U+0130)** — while the vendor ships the file with `[CONNECTION]` (plain ASCII `I`).
The Win32 INI API matches the name exactly, so the lookup misses and `ip` comes back
**empty**. Emptiness is then fatal because of a second bug (see §5), producing:

```
System.FormatException: An invalid IP address was specified.
  at System.Net.IPAddress.Parse
  at System.Net.Dns.GetHostByAddress(String)
  at tebeos_plus.Controller.Client.Client_Model.TerminalKontrol(ClientBilgi)
  at tebeos_plus.Ana_Form_..ctor()
```

**Fix:** the section must contain the Turkish `İ`. Depending on the Wine build this
byte sequence is either UTF-8 (`C4 B0`) or CP1254 (`DD`). On the tested system the
app itself wrote **UTF-8**. Because it is not worth betting on which encoding a
given Wine uses, `install-tebeos-client.sh` writes **both** variants of the section
with the same value; a lookup can then only match one of them, but it always matches.

---

## 5. Bugs found (and their fixes)

| # | Bug | Fix |
|---|---|---|
| 1 | INI section `CONNECTİON` (İ) vs shipped `[CONNECTION]` (I) → empty IP → crash | write the section with the Turkish İ (both encodings) |
| 2 | `Scanner.VeritabanıPortAcikmi(ip, port)` uses `BeginConnect` + `AsyncWaitHandle.WaitOne(500)` and treats *completion* as success — so it returns **true** even for a *refused* connection | can't patch the vendor binary; avoid the empty-IP state (fix 1) so the call is never reached with a bogus value |
| 3 | Vendor `conf/postgresql.conf.dist` sets `lc_messages/lc_monetary/lc_numeric/lc_time = 'tr_TR.UTF-8'` — Linux-style locale names that Windows PostgreSQL rejects → server refuses to start | set them to `'C'` |
| 4 | The installer aborts in silent mode (`/VERYSILENT`) with "Failed to proceed to next wizard page; aborting" | run it in GUI mode and click through; the installer script falls back automatically |
| 5 | `Wizard.exe` (vendor's server wizard) exits immediately under Wine (exit 0, ~0.3 s, does nothing) | reproduce its steps manually — see §6 |
| 6 | `install-tebeos-client.sh` exported `WINEDLLOVERRIDES="mscoree,mshtml="` — an **empty** load order means *disabled*, so Wine reported `mscoree.dll not found` and the app died instantly: `err:module:fixup_imports_ilonly mscoree.dll not found, IL-only binary tebeos_plus.exe cannot be loaded` (exit 53) | use `mscoree=n`, and leave `mshtml` **enabled** (see #9). Real .NET must be native: the env var beats winetricks' registry override `*mscoree=native`. Disabling `mscoree` is only useful *before* .NET exists, and this script always installs .NET 4.8 |
| 7 | "Beni Hatırla" saves the password into `Ayarlar.ini` through the **ANSI** profile API, and Wine takes that codepage from the **host locale** — `HKLM\...\Nls\CodePage\ACP` is ignored (measured: setting ACP=1254 changes nothing, `LC_ALL=tr_TR.UTF-8` does). With an `en_US` host the codepage is 1252, which has no `ğ`; Windows' best-fit mapping then stores `çığ` as `gü`, and the next login is refused until the user retypes it | run the app under `LC_ALL=tr_TR.UTF-8` (codepage 1254). The installer detects a Turkish locale and writes it into the launcher. Verified: the remembered password now logs in with no typing |
| 8 | Wine's bundled `tahoma.ttf` / `tahomabd.ttf` / `arial.ttf` have no **U+20BA (₺)**, so every money amount in TEBEOS is painted as `1.234,56□`. The fonts winetricks installs (`corefonts`, `tahoma`) are XP-era too and also lack it | Wine's three files get the glyph grafted in (shipped in `fonts/`, originals kept in `<wine>/share/wine/fonts/orig/`); the same is done in place to the prefix's real Tahoma/Arial with `patch-wine-fonts.py` (donor Noto Sans, rescaled to the target's units-per-em, so metrics and family names are untouched) |
| 9 | The Medula window showed its toolbar but a **blank** content pane | the MSHTML classes were missing from the **32-bit** registry view, so Wine's `ieframe` (the IE Web Browser control) never created an HTML document and never even loaded `mshtml.dll`. Fix: install **wine-gecko 2.47.4** (x86 + x86_64 — the version this Wine build asks for) and register MSHTML for 32-bit: `regsvr32 /s C:\windows\syswow64\mshtml.dll`; keep `mshtml` enabled. Verified: the SGK Medula login page renders |
| 10 | The UI mixed two typefaces — the "fonts look off" impression: TEBEOS asks for the `System` font ~9,000 times a session and Wine resolves unknown families through **fontconfig**, so it came back as Noto Sans while `Tahoma` was Wine's own clone | install real Windows fonts (`winetricks corefonts tahoma`) and add the font substitution `System` → `Tahoma`; the installer does both |
| 11 | **The Medula login cannot be completed: the captcha field swallows every keystroke.** The page has `onkeypress="onlyNumbers();"` and `/eczane/scripts/onlyNumbers.js` is `if (event.keyCode < 48 || event.keyCode > 57) event.returnValue = false;`. Wine's MSHTML *does* provide the IE-style global `event`, but returns **keyCode = 0 for keypress** (Gecko fills keyCode only for keydown/keyup; IE puts the character code in keyCode on keypress, and Firefox/Chrome only started mirroring charCode→keyCode in 2018 — Wine's engine is wine-gecko **2.47.4 = Firefox 47**). So `0 < 48` is true and the handler cancels every keystroke | **Fixed** by a rebuilt `mshtml.dll`: `wine-patch/` holds it and `install-tebeos-client.sh` installs it. The change is 5 lines in `dlls/mshtml/htmlevent.c` — `DOMKeyboardEvent_get_keyCode()` uses the character code for keypress events when Gecko reports 0, which is what IE does (and what Firefox/Chrome have done since 2018). Verified with a minimal page in `wine iexplore`: before → the field swallowed input and the handler saw `typeof event=object  keyCode=0`; after → the field accepts input and the handler sees `keyCode=51`. Build recipe in `wine-patch/README.md` (llvm-mingw, no root needed). The app itself was always fine: it fills user, password **and** ticks the KVKK box through the DOM. Pasting is no escape — Wine's MSHTML has no paste into inputs |

---

### Note on bug #6 — how it hid

The client script's own 30 s "test launch" inherited the same `WINEDLLOVERRIDES`, so it
always failed too and left `testlaunch.log` **empty** (the app died before writing
anything). The server role on the original machine was verified with a *different*
prefix (`~/.wine`, Lutris' Wine), so the bug was never exercised end-to-end until the
client was installed on a second machine.

Diagnosis shortcut: run the app once with Wine's messages enabled (no `WINEDEBUG=-all`)
— `err:module:import_dll Library mscoree.dll ... not found` is the tell.

## 6. Server install procedure (Linux) — vendor `server.bat`, adapted

The server installer ships `server.bat`, which documents the intended order.
Adapted for Wine (use the vendor's bundled PG 12.4, run under the same prefix):

```sh
P="$WINEPREFIX/drive_c/Program Files (x86)/TebeosPlusClient"
WINE=<wine binary>

# 1. cluster (vendor parameters)
"$WINE" "$P/pgsql/bin/initdb.exe" --encoding=UTF-8 --username=tebeos --pgdata data
#    (run from $P; a plain `--locale` worked, no need to force one)

# 2. vendor configs
cp "$P/conf/pg_hba.conf.dist"     "$P/data/pg_hba.conf"
cp "$P/conf/postgresql.conf.dist" "$P/data/postgresql.conf"
#    >>> then fix lc_* -> 'C'  (bug #3)

# 3. start (vendor uses `pg_ctl register` + `net start` — the Windows service
#    route; starting it directly is simpler and works)
"$WINE" "$P/pgsql/bin/pg_ctl.exe" -D "$WROOT\\data" -l "$WROOT\\pgsql.log" start

# 4. database + base schema
"$WINE" "$P/pgsql/bin/createdb.exe" --echo -p 5454 -E UTF8 -U tebeos tebeos_db
"$WINE" "$P/pgsql/bin/psql.exe" -U tebeos -d tebeos_db --port=5454 --single-transaction \
        --echo-errors -v ON_ERROR_STOP=1 --file="sql\\tebeos_db.sql"
```

The 127 MB `sql/tebeos_db.sql` shipped inside `tebeos_plus_setup.exe` is the
**base schema** — it was the single missing piece for a fresh server. Result after
loading: **44 schemas, 317 tables, 2018 functions**, `select public.sql_version_prc(null)` → `1.3.8.0`.

Schema families present: `cari, stok, satis, kasa, fatura, efatura_entegrator, medula,
recete, kurum_recetesi, hasta, uts, its, tevzi, siparis, emanet, zayi, oda, log,
kullanici, raporlama, guncelleme, …`

---

## 7. Client (terminal) install on Linux

Use `install-tebeos-client.sh`. What it does, in order:

1. Installs a few small packages (`cabextract`, `curl`, `unzip`, `xz`) via
   apt / pacman / dnf / zypper — best effort, skippable with `--skip-deps`.
2. Obtains Wine: system Wine first; if absent or **unable to run 32-bit programs**,
   it downloads a portable **WoW64** build into `~/.local/share/tebeos/wine`.
3. Creates prefix `~/.wine-tebeos` (win64), sets Windows 7.
4. Downloads winetricks to `~/.local/share/tebeos/winetricks`.
5. Installs **.NET Framework 4.8** + `vcrun2019` (slow: 5–20 min).
6. Downloads/uses `tebeos_plus_client_setup_v1.exe`, tries a silent install,
   and if that fails (bug #4) runs the normal window for the user to click through.
7. Configures the terminal:
   * `<Host>` = server IP in `tebeos_plus.xml`
   * `[CONNECTİON]` ×2 encodings → server IP, `[BAGLANTI] CLIENT=TRUE`,
     `[HOSTNAME] ANABILGISAYAR=<server name>`
8. Creates `~/tebeos/start-tebeos-client.sh` + menu/desktop entries.
9. Checks that `server_ip:5454` is reachable, does a 30 s test launch, and reports
   the known "invalid IP" failure with the exact re-run command if it appears.

Flags worth knowing: `--main-computer`, `--wine system|download`, `--wine-url`,
`--prefix`, `--installer`, `--skip-deps|--skip-dotnet|--skip-install|--skip-verify`,
`--force-prefix`, `--dry-run`.

---

## 8. Windows main-computer requirements

The Linux client talks straight to PostgreSQL over TCP, so on the Windows PC:

* PostgreSQL must be reachable from the terminal on port 5454 (the vendor's config
  already accepts terminal connections).
* **The firewall must allow TCP 5454** (often the missing piece):
  ```powershell
  New-NetFirewallRule -DisplayName "TEBEOS DB" -Direction Inbound `
      -Protocol TCP -LocalPort 5454 -Action Allow
  ```
  and, if the network is classified "Public", set it to Private or add the rule
  for all profiles.
* Find the PC's LAN IP with `ipconfig`; that IP goes into the client's `<Host>`.
* Give the PC a **static IP / DHCP reservation**, otherwise the client breaks when
  the IP changes.

---

## 9. Patient data

The database holds customer and prescription data (`cari`, `hasta`, `recete`,
`kurum_recetesi`): personal health data under KVKK. Never share database dumps, logs from
real sessions or screenshots.

---

## 10. Open items / where to continue

1. **Medula — solves to a working login page: it is Internet Explorer, rendered by
   Gecko.** The blank window was never a CefSharp/CEF problem, and it is *not* a
   missing browser engine either — it was a missing class registration. Pressing Medula
   starts `tebeos_medula.exe`, and that window hosts a legacy
   **Microsoft Web Browser ActiveX control** (Wine's `ieframe`), which navigates to
   `https://medeczane.sgk.gov.tr/eczane/faces/login.jsp`.

   Evidence (all reproducible with `WINEDEBUG=+ieframe,+wininet,+loaddll`):

   ```
   trace:ieframe:WebBrowser_Navigate2 -> {VT_BSTR: L"https://medeczane.sgk.gov.tr/eczane/faces/login.jsp"}
   trace:ieframe:navigate_url navigating to L"https://medeczane.sgk.gov.tr/eczane/faces/login.jsp"
   trace:wininet:InternetConnectW (1, L"medeczane.sgk.gov.tr", 443, ...)
   trace:wininet:HTTP_GetResponseHeaders ... status code [L"302"]      # -> http://.../LogoutServlet
   ... but mshtml.dll is never loaded into the process  ->  nothing is ever painted
   ```

   What was wrong, in order of discovery:

   * Wine loaded `ieframe.dll`, `urlmon.dll`, `wininet.dll` — and **no `mshtml.dll`**,
     in the app *and* in Wine's own `iexplore.exe`. The document was never created, so
     the pane stayed white.
   * Installing **wine-gecko 2.47.4** (both `x86` and `x86_64`) alone changed nothing.
   * The actual cause: MSHTML's classes were **missing from the 32-bit registry view**
     (`Software\\Classes\\Wow6432Node\\CLSID`), even though the 64-bit view had them —
     so a 32-bit process could not `CoCreateInstance` the HTML document. Running
     `regsvr32 /s C:\windows\syswow64\mshtml.dll` fixed it, and with gecko installed
     the process then loaded `mshtml.dll` **+ `xul.dll` + `mozglue.dll`** (i.e. Gecko)
     and painted the real Medula page. Both steps are in the installer now; `mshtml`
     must stay **enabled**.
   * The engine you get is therefore **Gecko (Firefox)** — Wine's stand-in for IE.
     Chrome's engine (CEF) is already inside TEBEOS for other screens, but the IE
     WebBrowser control cannot be pointed at it without a shim that reimplements the
     IE interfaces over CEF (`IECEF`-style); Gecko is the supported path and it works.
   * `tebeos_medula.exe` imports **both** `CefSharp` and `SHDocVw`, so parts of the
     Medula UI may use CEF where the page is not an IE control.
   * Pharmacists do **not** need the e-imza middleware — that is the physicians' part
     of Medula (the app probes for it: `ArkSignerBEs (Chrome) is not installed`), so
     it is not a blocker for the pharmacy workflows.

   **Conclusion: the Medula login page renders.** What is still untested is how far
   the app's automation gets on Gecko 2.47 (Firefox 47, 2016) instead of Trident — the
   page itself advises using a current browser. If a screen misbehaves, compare it in
   a normal browser; the page is plain JSF and loaded fine here.
2. **Backup / restore.** `Yedek_Al` form → `pg_dump` into `<install>/backup/tebeos_db.sql`
   then zips it; `Geri_Yukle` does the reverse via `psql --single-transaction`.
   Note `Yedek_Al_Load` only runs when `[BAGLANTI] CLIENT == "FALSE"` (main computer).
3. **`TebeosBackupIndirme.exe`** ("download TEBEOS backup") probably pulls the
   pharmacy's data from the vendor's servers. Worth investigating for a real migration.
4. **Online updates.** `UpdateUrl` → `http://tebeosplus.teb.org.tr:8001/version/main.xml`.
   The manifest lists incremental SQL patches (`sql` entries 0.0.2.3 → 0.0.3.8) and an
   app package; note the app compares its `RequiredSql` (0.0.3.3) with the DB's
   reported version (1.3.8.0) — two different versioning schemes, worth watching.
5. **Multi-terminal setup.** Server config already listens on the LAN; the client
   script covers the terminal side. The app also has its own LAN discovery
   ("Ana Bilgisayar Bulunamadı" + a client-manager dialog that writes the same files).

---

## 10a. How the Medula automation works (decompiled) and where it stops on Wine

The Medula window is not just a viewer - it is the whole prescription integration, and it
is **built on the IE WebBrowser control**, not CefSharp. Decompiled with `ilspycmd`
(recipe in section 12; the module is one 21 MB C# file):

* `tebeos_medula.View.Medula.*` / class `RaporClassHtmlElement` holds `wbMedula`
  (a `System.Windows.Forms.WebBrowser`) and a **page-state machine keyed on the URL**
  (`/eczane/faces/login.jsp` -> wait, `/eczane/faces/index.jsp` -> logged in,
  `/eczane/faces/pages/recete/ReceteIslem2.jsp` -> parse the prescription ...).
  All data extraction is `wbMedula.Document.GetElementById("f:t43")` style DOM reads -
  that is what writes patient/compensation/payment rows into the database.
* The captcha is handled by `tebeos_medula.View.Login.OcrLogin` (a hidden `Form`):
  it finds the `<img>` whose `href` contains `SayiUretenImageYeniServlet`, then
  **renders that element into a bitmap with `IHTMLElementRender::DrawToDC`**
  (IID `3050F669-98B5-11CF-BB82-00AA00BDCE0B`), and OCRs it with `tessnet2_32.dll`
  (Tesseract 3.05 + `ocr/`, digits only). A result shorter than 6 chars becomes
  `"999999"`. The app also reacts to SGK's error texts ("Gizvenlik Numarasini Yanlis
  Girdiniz", "Kullanici Adi veya Sifre Yanlis") by clearing cookies and retrying.
* `Giris_YapMedEczane()` / `Giris_YapGss()` drive the login; cookies go through
  `wininet` (`InternetSetCookie` / `InternetGetCookieEx`).
* CefSharp exists in the module too, but for **other** screens - `TebRpMedulaSutForm`
  ("Sut Kontrol", `chromeBrowserMedula` + `InitializeChromiumMedula`), invoice popups,
  `tebeos_medula2.CefReq.*`. Its login helper `LoginCaptchaParser` grabs the captcha
  through a **canvas -> base64 data URL** (no `DrawToDC`) and would work under Wine.

**Where it stops under Wine (measured):**

1. The page renders and the DOM automation works - the app does fill
   `Kullanici Adi`, `Parola` and ticks `kvkkTaahhut` via `HTMLInputElement_put_value`.
   The app is otherwise fine.
2. It then writes an **empty string** into the captcha field and stops; the login is
   never submitted and the state machine never leaves the login URL. In a `+loaddll`
   trace neither `tessnet2_32.dll` nor `libtesseract3052.dll` is ever loaded, and the
   process never even queries Wine for `IHTMLElementRender`, so the flow dies before
   the OCR step.
3. Root cause candidate A (structural): **`IHTMLElementRender::DrawToDC` does not
   exist in Wine's mshtml at all** - so the captcha snapshot can never work. Wine's
   gecko binding only exposes whole-document rendering (`nsIDOMWindowUtils::
   RenderDocument`).
4. Root cause candidate B (trigger): the state machine expects exact URLs such as
   `.../eczane/faces/login.jsp`, while an unauthenticated request is redirected to
   `.../eczane/PostLogoutRedirectServlet` (which is what actually serves the login
   form). In our runs the app never started its login sequence on page load - it only
   filled the fields when a field was clicked - so the trigger step is suspect too.

**Consequence:** on Wine the automatic Medula login cannot complete, so none of the
prescription/compensation automation runs. Fixing it means implementing the IE element
render API in Wine (and checking the URL expectations), or moving that flow to the
CEF-based implementation the vendor already has.

### Work in progress: IHTMLElementRender in Wine's mshtml (2026-10-01)

Implemented and installed (patch: `wine-mshtml-tebeos.patch`, which also carries the
keypress keyCode fix; both build with the llvm-mingw recipe in `wine-patch/README.md`):

* `mshtml_private_iface.idl` - declares `IHTMLElementRender` (IID
  `3050f669-98b5-11cf-bb82-00aa00bdce0b`) with `DrawToDC`/`SetDocumentPrinter`.
* `mshtml_private.h` - the interface is added to `struct HTMLElement`, so every element
  type (including `<img>`) answers it, exactly like IE.
* `htmlelem.c` - vtable, QI wiring and `HTMLElement_Init` assignment. `DrawToDC` is
  currently a **stub** (logs a FIXME, returns S_OK) - the rendering itself is the next
  step; the caller no longer fails at its cast.

Verified against the vendor's weekly update (2026-10-01, project 1.3.1.9 / db 1.4.1.3):
`OcrLogin` is unchanged in the new binaries - still `DrawToDC`, still the
`SayiUretenImageYeniServlet` search, still Tesseract - so the patch remains the right
target.  Note: string literals live in the .NET UTF-16 heap, so `grep` for them in the
exe gives false negatives; decompile instead.

**Open blocker found while testing (comes before DrawToDC):** the app's own login
sequence does not start by itself. In runs where nothing was clicked the Medula form
stays empty, and an `+mshtml` trace shows no image enumeration at all (no
`HTMLDocument2_get_images`, no `IMGElement_get_href`), so `Giris_YapMedEczane()` ->
`SetCaptcha()` is never reached. Filling happens only when a field is clicked, i.e. a
different code path. `GetStatus()`/`timerGirisYap` (the URL-keyed state machine) is the
place to look - see the "trigger" note above.

### IE8 does not rescue this (tested 2026-09-30)

`winetricks -q ie8` in a **clone** of the prefix installs fine - native `mshtml.dll`
(5.9 MB), `ieframe`, `shdocvw`, `urlmon`, `jscript`, all with the expected
`native,builtin` overrides - and the app even starts and logs in with it. But IE8's own
`iexplore.exe` renders a **blank page**, and the log shows why:

```
err:ole:marshal_object Failed to create an IRpcStubBuffer ... {d30c1661-cdaf-11d0-8a3e-00c04fc9e26e}  (IWebBrowser2)
err:ole:proxy_manager_create_ifproxy Could not create proxy for {85cb6900-4d95-11cf-960c-0080c7f4ee85}  (IShellWindows)
```

Wine 11 cannot host the native IE stack any more, so "give it real Trident" is a dead end.

### What would have to be built (the only lever left)

The app calls exactly one missing API: **`IHTMLElementRender::DrawToDC`** on the captcha
`<img>`. Its own alternative (`SetCaptcha2()`, dead code) is blocked too -
`HTMLElement2_createControlRange()` is a `FIXME` returning `E_NOTIMPL`, so the clipboard
route through `Body.CreateControlRange()` + `execCommand("Copy")` cannot work either.

Implementing `IHTMLElementRender` for image elements in Wine's mshtml is self-contained:
the element geometry is already available (`img.width/height`, `offsetLeft/Top`), the
image bytes can be fetched with the page's own session and decoded with oleaut32's
`OleLoadPicture` (PNG/GIF/JPEG), then `StretchBlt` into the HDC the app passes in.
Success is measurable: `tessnet2_32.dll`/`libtesseract3052.dll` load, the captcha field
receives a 6-digit value, and the state machine advances to `/eczane/faces/index.jsp`.

## 11. Recipes

```sh
# start/stop everything on a Wine-based server
~/tebeos/start-tebeos.sh                    # starts PostgreSQL + the app
~/.local/share/lutris/runners/wine/wine-staging-11.2-x86_64/bin/wineserver -k   # stop all

# is the database up?
ss -tln | grep 5454
tail -f "<install>/pgsql.log"

# psql shell (server)
cd "<install>/pgsql/bin"
WINEPREFIX=~/.wine ~/.local/share/lutris/runners/wine/wine-staging-11.2-x86_64/bin/wine \
    psql.exe -U tebeos -d tebeos_db -p 5454

# quick schema sanity check
psql.exe -U tebeos -d tebeos_db -p 5454 -tAc \
   "select count(*) from information_schema.tables where table_schema not in ('pg_catalog','information_schema')"

# what is the app doing? (CEF + app logs)
tail -f "<install>/app/log/"*.log "<install>/app/debug.log"
```

---

## 12. Tooling that made this possible

* **Decompilation on Linux, no Windows needed** — `ilspycmd` as a .NET global tool
  package, run with the .NET runtime you already have:

  ```sh
  curl -sSL -o ilspy.nupkg \
     https://api.nuget.org/v3-flatcontainer/ilspycmd/8.2.0.7535/ilspycmd.8.2.0.7535.nupkg
  7z x -y ilspy.nupkg -oilspy
  DOTNET_ROLL_FORWARD=Major dotnet ilspy/tools/net6.0/any/ilspycmd.dll -o out app/tebeos_plus.exe
  # one big .cs file (~776k lines) — grep it:
  #   getConnectionString, Get_Server_Ip, TerminalIni, TerminalKontrol,
  #   SaveXmlTerminal, ReadAndAyarlarIniTerminal, Yedek_Al, Geri_Yukle
  ```
  (An older ilspycmd is used on purpose: the current one needs .NET 10.)
* **Screenshots without a screenshot tool**: `spectacle -b -n -o file.png`
  (`import -window root` from ImageMagick misbehaves when argv[0] isn't `import`;
  if you need it, call `magick import ...`).
* **Windows GUI automation** is possible with the pure-python `python-xlib`
  (download the wheel + `six`, unzip both, set `PYTHONPATH`) and XTEST
  `fake_input` — useful when an installer needs clicking.
* **Inno Setup** installers: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/LOG=C:\x.log"`
  gives a complete per-file log — the fastest way to see what an installer did.
  Both TEBEOS installers are Inno Setup 6.x with a large payload overlay;
  `7z` and `innoextract` cannot open them, so the log is the practical route.

---

## 13. Vendor files and URLs

```
client installer : https://eczacilar-p.mncdn.com/Setup/tebeos_plus_client_setup_v1.exe   (~266 MB)
server installer : https://eczacilar-p.mncdn.com/Setup/tebeos_plus_setup.exe             (~382 MB)
server installer (x86 build) : https://eczacilar-p.mncdn.com/Setup/tebeos_plus_setup_32.exe
update manifest  : http://tebeosplus.teb.org.tr:8001/version/main.xml
vendor site      : http://tebeos.teb.org.tr
downloader tool  : YeniTebeosIndirV1002.exe   (offers Terminal / Ana Bilgisayar)
```

Install layout after a server install:

```
TebeosPlusClient/
├── app/            tebeos_plus.exe, tebeos_medula.exe, CefSharp/CEF, DevExpress, WebView2, OCR
├── pgsql/          PostgreSQL 12.4 (bin, lib, share)
├── data/           the cluster (created by initdb)
├── conf/           pg_hba.conf.dist, postgresql.conf.dist
├── trust/          a second set of the same configs (used by backup/restore)
├── sql/            tebeos_db.sql  (127 MB base schema — server installer only)
├── backup/         where Yedek Al writes dumps
├── extra/          NDP452 installer, VC_redist x86/x64
├── server.bat      vendor's setup procedure (see §6)
├── kill.bat        vendor's teardown (stop service, unregister, delete data)
└── Wizard.exe      vendor's server wizard (no-op under Wine)
```
