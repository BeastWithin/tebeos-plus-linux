# Building and setting up

This repo has the source and the docs. Everything else can be re-created, as listed below.

## Keep the repo shareable

It contains **no credentials, pharmacy identifiers, patient data or screenshots**. Keep it that way:
- The TEBEOS login used by `tebeos-work/run/runmedula.sh` lives in `tebeos-work/run/login.env`,
  which git ignores: `TEBEOS_USER=...` and `TEBEOS_PASS='...'`.
- Images are ignored, because screens show pharmacy and patient data.
- The docs use placeholders: `<ECZANE_KODU>`, `<MEDULA_PAROLA>`, `<ECZANE ADI>`, `<user>`.
- The shim's trace mode logs page content. Strip `>>`/`<<`/`FRAME` lines from a log before
  sharing it.

## Not in the repo

| What | How to get it |
|---|---|
| `tebeos_plus_setup.exe`, `tebeos_plus_client_setup_v1.exe`, `YeniTebeosIndirV1002.exe` | vendor downloads, see `AI-HANDOFF.md` §13 |
| `fonts/` (Arial, Tahoma) | copy from a Windows install (Microsoft fonts can't be redistributed) |
| `toolchain/llvm-mingw-*`, `toolchain/dotnet*`, `toolchain/ilspy*` | `AI-HANDOFF.md` §12. `shim-dll/build.sh` looks for llvm-mingw in `~/toolchain` or `toolchain/` |
| `toolchain/wine-11.18*` | Wine 11.18 source tarball from winehq.org |
| decompiled vendor code (`medula_decomp.cs`, `winforms_decomp`, `tessnet2_decomp`) | run ilspycmd on the vendor's binaries yourself (`AI-HANDOFF.md` §12); it is not redistributed |
| `wine-patch/mshtml/*/mshtml.dll` (patched Wine mshtml) | rebuild: `wine-patch/README.md` + `wine-mshtml-tebeos.patch` |
| `tebeos-work/run/hxjs/` (SGK's hxclient JS, for reading) | fetched from the SGK site |
| SGK's web service PDFs | SGK's Medula documentation pages (see `medula-api/NOTES.md`) |
| `~/.local/share/tebeos/` (Wine build, winetricks) | `install-tebeos-client.sh` downloads them |
| `~/.wine-tebeos/` (the Wine prefix with TEBEOS installed) | `install-tebeos-client.sh`, then the steps below |

## Ready-made image (a new client PC in a few minutes)

`image/build-client-image.sh all` builds `~/.cache/tebeos-build/out/tebeos-linux-<date>.run`
(about 1.1 GB): a fresh Wine prefix with .NET 4.8, the fonts with ₺, TEBEOS, the CDP shim and
the OCR hookup, plus the Wine build and the OCR service. It is never run as TEBEOS, so it holds
no login or pharmacy settings. It does contain TEBEOS's and Microsoft's files: **keep it on
the pharmacy's own PCs/drives, don't publish it.**

On the new PC (needs chromium, tesseract, python3-pillow, zstd, the tr_TR.UTF-8 locale;
`--check` lists what is missing):

    ./tebeos-linux-<date>.run --role client --server-ip 192.168.1.101

It unpacks to the same places as on the build PC (`~/.wine-tebeos`, `~/.local/share/tebeos`),
renames the Windows user folder to the new user, writes the terminal settings (`<Host>`,
`Ayarlar.ini` connection sections; the vendor's defaults are kept) and adds the menu entry,
desktop icon and `~/.local/share/tebeos/start-tebeos.sh`. TEBEOS updates itself on first
start. The server role is still to come.

Building needs a private display (`tebeos-work/run/nested.sh`): TEBEOS's installer refuses
silent mode, so its window is pressed through on :5 (licence, folder, untick the desktop icon,
Kur, close the two VC++ setups — the newer runtime is already in — Bitti). `image/xkeys.py`
sends the keys/clicks; the sequence still has to be done by hand or scripted (TEBEOS_KEYS).

## Set up from source

1. Clone the repo. For `tebeos-work/run/runmedula.sh`, create `tebeos-work/run/login.env`.
2. Install TEBEOS with `install-tebeos-client.sh` (`TEBEOS-CLIENT-README.md`).
3. Rebuild the patched Wine mshtml (`wine-patch/README.md`). Copy it to
   `~/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/lib/wine/{i386,x86_64}-windows/`.
4. Get llvm-mingw, then build the shim: `bash tebeos-work/shim-dll/build.sh`.
5. Install it: `bash tebeos-work/run/shimctl.sh install`.
6. Install Chromium (`chromium` on PATH) and run the OCR service
   (`tebeos-work/ocr-service/`).

Once it works, back up `~/.wine-tebeos` and `~/.local/share/tebeos` (about 5 GB together).
They contain the installed TEBEOS, the patched mshtml and the installed shim, so restoring
them skips steps 2–5.
