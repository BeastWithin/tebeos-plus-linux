# TEBEOS Plus on Linux

Run **TEBEOS Plus**, the pharmacy management software of the Turkish Pharmacists' Association
(TEB), on Linux with Wine, including its **Medula** module (SGK's e-prescription system).

This is an independent community project. It is not affiliated with or supported by TEB or SGK.
It does not change or redistribute TEBEOS files: you install TEBEOS from the vendor yourself,
and this project adapts the Wine side around it.

## Status (2026-11)

In daily use in one pharmacy as a **client/terminal** against a Windows main computer:
- the main program, terminal connection and the cash register program (TEBEOS Ödeme Noktası);
- Medula: login (captcha OCR included), prescription query and entry, Karekod / İTS, printing.

The original author has stopped active work. This repo is a snapshot meant to let others take
it further. **Open problems** are listed in [Where to continue](#where-to-continue).

## How it works

TEBEOS's Medula window hosts Internet Explorer's `WebBrowser` control. Wine's stand-in for IE is
based on an old Gecko (Firefox 47), which can't run SGK's current Medula pages. So:

- **`tebeos-work/shim-dll/`**: a replacement `mshtml.dll` (C, built with llvm-mingw). It answers
  the IE COM DOM interfaces that TEBEOS calls and forwards each call to a real **Chromium** over the
  DevTools Protocol (CDP). In "embed mode" (the default) a visible Chromium window is placed inside
  the Medula window, while keyboard and mouse still go through the shim so TEBEOS's event handlers
  see them in IE's order. `iecompat.js` covers IE-only behaviour that TEBEOS relies on.
- **`tebeos-work/embed-spike/embed_run.py`**: an X11 helper that keeps Chromium's window placed
  inside Medula's.
- **`tebeos-work/ocr-service/`** + **`tebeos-work/tessnet2-shim/`**: TEBEOS reads the Medula
  captcha with the `tessnet2` OCR library. A drop-in stand-in for that library hands the image
  to a Tesseract service running on Linux.
- **`wine-patch/`**: a small fix to Wine's own mshtml (keypress `keyCode`), needed for the captcha
  field when not using the shim.
- **`install-tebeos-client.sh`**, **`image/`**: install a client PC, or build a ready-made
  installer image for new PCs.
- **`medula-api/`**: a small Python client for SGK's documented pharmacy stock web service
  (SOAP). It is separate from the TEBEOS work.

## Where to start reading

1. **`AI-HANDOFF.md`**: the whole story — installing TEBEOS under Wine, the database, the
   configuration traps, and how the Medula automation works. Its sections are in order;
   the "LATEST" blocks at the top are the most recent.
2. **`HANDOFF-PATH-A.md`**: the shim in depth. The status block at the top has how embed mode
   works, the lessons learned, the known gaps and the test workflow.
3. **`BUILDING.md`**: what is not in the repo and how to get it, how to build the shim and the
   client image.
4. **`TEBEOS-CLIENT-README.md`**: installing a client PC (end-user guide).
5. Background: `captcha-root-cause.md` (the login/captcha investigation),
   `TEBEOS-SERVER-NOTES.md` (running the main computer under Wine), `VENDOR-NOTES.md`
   (findings for the vendor), `tebeos-work/*/README.md`, `tebeos-facts.json` (machine-readable
   summary).

The docs were written as working notes during development (partly as handoffs between AI coding
sessions), so they are detailed and date-stamped rather than polished.

## Where to continue

From `HANDOFF-PATH-A.md`:
- **Server role:** install the main computer (PostgreSQL on 5454) on Linux too, in the same
  `.run` image (`--role server`). The risky part is moving a live database; plan it with a
  full dump-and-restore test.
- **Embed mode gaps:** TEBEOS controls that Wine draws over the web area are hidden behind
  Chromium's window. Untested: other resolutions, HiDPI, several monitors, desktops other than KDE.
  The helper should become a small native program the shim starts itself.
- Automate the TEBEOS installer's key sequence in the image build (`app` phase).
- Medula screens nobody has used on Linux yet; cookie sync between wininet and Chromium.

## Privacy

Medula and TEBEOS screens show patient data. Never commit screenshots, logs from real sessions,
captured responses or credentials. See "Keep the repo shareable" in `BUILDING.md`.

## License

GPL-3.0, see `LICENSE`. Third-party pieces keep their own licenses (`stb_image.h`: public
domain / MIT; Wine patches: LGPL, as Wine).
