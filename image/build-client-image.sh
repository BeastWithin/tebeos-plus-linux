#!/bin/bash
# build-client-image.sh -- build the ready-made TEBEOS-on-Linux image from scratch.
#
# A fresh Wine prefix gets everything the working setup has (.NET 4.8, fonts with the lira
# sign, TEBEOS itself, the mshtml CDP shim, the tessnet2 OCR hookup) and is packed together
# with the Wine build, the OCR service, the embed helper and setup.sh into one archive. The image is never run
# as TEBEOS, so it carries no login, pharmacy settings or logs; setup.sh on the target PC
# picks the role (client now, server later) and writes the settings.
#
# It contains TEBEOS's and Microsoft's files: for the pharmacy's own PCs only, not for
# sharing (the repo itself stays shareable).
#
# Phases (each can be run again on its own):
#   base   new prefix: Windows 7, .NET 4.8, VC++ 2019, then fonts
#   fonts  Windows fonts (corefonts, Tahoma) + lira sign, System -> Tahoma
#   app    the TEBEOS installer (its window is driven on the private display :5)
#   pos    the cash register program (TEBEOS Ödeme Noktası), the vendor's current installer
#   linux  shim, tessnet2 OCR hookup, mshtml registration, folders
#   pack   archive + setup.sh -> $OUT
#   header put the current setup.sh in front of the newest .run (no repacking)
#   all    everything in order
#
# Usage: image/build-client-image.sh [phase...]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${TEBEOS_BUILD:-$HOME/.cache/tebeos-build}"
PFX="$BUILD/pfx"
OUT="$BUILD/out"
WINEDIR="${TEBEOS_WINEDIR:-$HOME/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64}"
WINE="$WINEDIR/bin/wine"
WT="${TEBEOS_WINETRICKS:-$HOME/.local/share/tebeos/winetricks}"
INSTALLER="${TEBEOS_INSTALLER:-$HOME/.local/share/tebeos/tebeos_plus_client_setup_v1.exe}"
# the URL TEBEOS's own menu command downloads it from (Ana_Form_ -> PosSetupDownloader)
POS_URL="https://eczacilar-p.mncdn.com/OdemeNoktasiSetup/Tebeos%C3%96demeNoktas%C4%B164_Terminal.exe"
DISP="${TEBEOS_BUILD_DISPLAY:-:5}"
LOG="$BUILD/build.log"

export WINEPREFIX="$PFX" WINEARCH=win64 WINEDEBUG=-all DISPLAY="$DISP"
export WINEDLLOVERRIDES="mscoree=n"
export LC_ALL=tr_TR.UTF-8          # ANSI codepage 1254, as TEBEOS runs (see install-tebeos-client.sh)
export PATH="$WINEDIR/bin:$PATH"

mkdir -p "$BUILD" "$OUT"
say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
step() { say ""; say "==> $*"; }
die()  { say "FAIL: $*"; exit 1; }
run()  { "$@" >>"$LOG" 2>&1; }

need_display() {
    xdpyinfo -display "$DISP" >/dev/null 2>&1 || [ -S "/tmp/.X11-unix/X${DISP#:}" ] \
        || die "no X display $DISP: start one with tebeos-work/run/nested.sh"
}

APPDIR="$PFX/drive_c/Program Files (x86)/TebeosPlusClient"

phase_base() {
    step "base: new prefix $PFX"
    need_display
    [ -e "$PFX" ] && die "$PFX exists; remove it first (rm -rf) to rebuild from scratch"
    run env WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
    run wineserver -w
    step "base: Windows 7"
    run env LC_ALL=C.UTF-8 WINE="$WINE" "$WT" -q win7
    step "base: .NET Framework 4.8 (slow: 5-20 minutes)"
    run env LC_ALL=C.UTF-8 WINE="$WINE" "$WT" -q -f dotnet48
    step "base: VC++ 2019"
    run env LC_ALL=C.UTF-8 WINE="$WINE" "$WT" -q vcrun2019 || say "    vcrun2019 reported errors (often harmless)"
    phase_fonts
}

phase_fonts() {
    step "fonts: corefonts + tahoma, lira sign, System -> Tahoma"
    local fonts="$PFX/drive_c/windows/Fonts" files="" f py="python3" extra=""
    # winetricks lowercases font file names with tr; under the Turkish locale (which the rest of
    # the build uses, as TEBEOS does) "I" becomes a dotless "ı", impact.ttf is never found and
    # the rest of corefonts and tahoma is skipped. So: a neutral locale for this step.
    rm -f "$fonts"/*mpact.ttf "$fonts"/Impact.ttf
    run env LC_ALL=C.UTF-8 WINE="$WINE" "$WT" -q -f corefonts tahoma
    for f in tahoma.ttf verdana.ttf times.ttf impact.ttf; do
        [ -f "$fonts/$f" ] || die "font $f missing after winetricks"
    done
    for f in tahoma.ttf tahomabd.ttf arial.ttf arialbd.ttf; do
        [ -f "$fonts/$f" ] && files="$files $fonts/$f"
    done
    python3 -c 'import fontTools' 2>/dev/null || extra="$HOME/.local/share/tebeos/pylibs"
    ( cd "$REPO" && PYTHONPATH="$extra" $py ./patch-wine-fonts.py $files ) >>"$LOG" 2>&1 \
        || die "could not add the lira sign to the prefix fonts"
    run wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' \
        /v System /t REG_SZ /d Tahoma /f
    run wineserver -w
    say "    fonts done"
}

phase_app() {
    step "app: TEBEOS installer"
    need_display
    [ -f "$INSTALLER" ] || die "installer not found: $INSTALLER (set TEBEOS_INSTALLER)"
    if [ -f "$APPDIR/app/tebeos_plus.exe" ]; then
        say "    already installed"
        return 0
    fi
    run wine "$INSTALLER" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART '/LOG=C:\tebeos_setup.log' || true
    run wineserver -w
    if [ ! -f "$APPDIR/app/tebeos_plus.exe" ]; then
        # The vendor's wizard aborts in silent mode ("Failed to proceed to next wizard page"):
        # run its window on the private display and press through it.
        say "    silent mode refused; driving the installer window on $DISP"
        wine "$INSTALLER" '/LOG=C:\tebeos_setup.log' >>"$LOG" 2>&1 &
        local pid=$! i
        for i in $(seq 60); do sleep 1; [ -n "${TEBEOS_KEYS:-}" ] && break; done
        if [ -n "${TEBEOS_KEYS:-}" ]; then
            # shellcheck disable=SC2086
            python3 "$REPO/image/xkeys.py" "$DISP" $TEBEOS_KEYS
        else
            say "    (no TEBEOS_KEYS given: press through the window on $DISP by hand)"
        fi
        wait "$pid" || true
        run wineserver -w
    fi
    [ -f "$APPDIR/app/tebeos_plus.exe" ] || die "TEBEOS was not installed; see $PFX/drive_c/tebeos_setup.log"
    say "    TEBEOS installed"
}

# Every terminal runs the vendor's cash register program. TEBEOS's menu command starts its
# installer while TEBEOS runs, which fails under Wine (it replaces files TEBEOS has open; Inno's
# Restart Manager cannot close apps here), so it is installed into the image instead. Run by
# its own installer, not copied: it also writes registry entries and runs PosReqFile.exe. It
# shares TEBEOS's app folder and replaces some of TEBEOS's DLLs, as it does on Windows.
phase_pos() {
    step "pos: cash register program (TEBEOS Ödeme Noktası)"
    need_display
    [ -f "$APPDIR/app/tebeos_plus.exe" ] || die "TEBEOS is not installed yet (phase app)"
    if [ -f "$APPDIR/app/tebeos_pos.exe" ]; then
        say "    already installed"
        return 0
    fi
    local exe="$BUILD/TebeosOdemeNoktasi64_Terminal.exe"
    curl -fsSL --retry 2 -o "$exe.part" "$POS_URL" || die "could not download $POS_URL"
    mv "$exe.part" "$exe"
    mkdir -p "$PFX/drive_c/pos-setup"
    cp "$exe" "$PFX/drive_c/pos-setup/setup.exe"
    run wine 'C:\pos-setup\setup.exe' /VERYSILENT /SUPPRESSMSGBOXES /NORESTART '/LOG=C:\pos-setup\setup.log' || true
    run wineserver -k || true      # it may leave PosReqFile / the program running
    grep -aq "Installation process succeeded" "$PFX/drive_c/pos-setup/setup.log" 2>/dev/null \
        && [ -f "$APPDIR/app/tebeos_pos.exe" ] \
        || die "the cash register installer failed; see $PFX/drive_c/pos-setup/setup.log"
    rm -rf "$PFX/drive_c/pos-setup"
    say "    installed"
}

phase_linux() {
    step "linux: CDP shim (mshtml_cdp.dll) for the Medula window"
    local shim="$REPO/tebeos-work/shim-dll" C="$PFX/drive_c/windows"
    local clsid='{25336920-03F9-11CF-8FD0-00AA00686F13}'
    [ -f "$shim/shim32.dll" ] && [ -f "$shim/shim64.dll" ] || die "build the shim first: bash $shim/build.sh"
    cp "$shim/shim32.dll" "$C/syswow64/mshtml_cdp.dll"
    cp "$shim/shim64.dll" "$C/system32/mshtml_cdp.dll"
    # Wine's mshtml must be registered for both arches (without it ieframe never creates an
    # HTML document); then only the HTMLDocument class is pointed at the shim.
    run wine regsvr32 /s mshtml.dll
    run wine 'C:\windows\syswow64\regsvr32.exe' /s 'C:\windows\syswow64\mshtml.dll'
    run wine reg add "HKLM\\Software\\Classes\\Wow6432Node\\CLSID\\$clsid\\InprocServer32" /ve /t REG_SZ \
        /d 'C:\windows\syswow64\mshtml_cdp.dll' /f
    run wine reg add "HKLM\\Software\\Classes\\CLSID\\$clsid\\InprocServer32" /ve /t REG_SZ \
        /d 'C:\windows\system32\mshtml_cdp.dll' /f

    step "linux: tessnet2 OCR hookup (captcha -> C:\\ocr-in -> host tesseract)"
    local t="$REPO/tebeos-work/tessnet2-shim/bin/Release/net48/tessnet2_32.dll"
    [ -f "$t" ] || die "tessnet2 shim not built: $t"
    [ -f "$APPDIR/app/tessnet2_32.dll.orig" ] || cp "$APPDIR/app/tessnet2_32.dll" "$APPDIR/app/tessnet2_32.dll.orig"
    cp "$t" "$APPDIR/app/tessnet2_32.dll"
    mkdir -p "$PFX/drive_c/ocr-in" "$PFX/drive_c/ocr-out"
    run wineserver -w
    say "    linux done"
}

phase_pack() {
    step "pack: cleaning the prefix"
    run wineserver -k || true
    rm -f "$PFX/dosdevices/y:" "$PFX/drive_c/tebeos_setup.log"
    rm -rf "$PFX"/drive_c/users/*/AppData/Local/Temp/* "$PFX"/drive_c/windows/temp/*
    # the installer ships Ayarlar.ini with defaults; a login in it means TEBEOS was run here
    grep -qa 'LOGIN_KULLANICI\|SIFRE=' "$APPDIR/app/Ayarlar.ini" 2>/dev/null \
        && die "Ayarlar.ini holds a login: TEBEOS was run in the build prefix; rebuild"
    local stage="$BUILD/stage" ver date name run
    rm -rf "$stage"; mkdir -p "$stage/ocr-service" "$stage/embed"
    cp "$REPO/tebeos-work/ocr-service/ocr_service.py" "$stage/ocr-service/"
    cp "$REPO/tebeos-work/embed-spike/embed_run.py" "$stage/embed/"
    ver=$(sed -n 's|.*<CurrentVersion>\([^<]*\)</CurrentVersion>.*|\1|p' "$APPDIR/tebeos_plus.xml")
    date=$(date +%Y%m%d)
    cat >"$stage/BUILD-INFO" <<EOF
BUILT_AT='$(date '+%Y-%m-%d %H:%M')'
BUILT_BY='$USER'
TEBEOS_VERSION='$ver'
WINE_BUILD='$(basename "$WINEDIR")'
SHIM_COMMIT='$(git -C "$REPO" rev-parse --short HEAD)'
EOF
    name="tebeos-linux-$date"
    run="$OUT/$name.run"
    step "pack: $run (zstd; several minutes)"
    # the Wine build without the debugging backups next to its patched mshtml / fonts
    tar -c --exclude='*.bak*' --exclude='*.orig' --exclude='*.before-*' --exclude='fonts/orig' \
        -C "$(dirname "$WINEDIR")" --transform "s|^$(basename "$WINEDIR")|wine|" "$(basename "$WINEDIR")" \
        -C "$BUILD" --transform 's|^pfx|prefix|' pfx \
        -C "$stage" ocr-service embed BUILD-INFO \
        | zstd -T0 -12 -q >"$OUT/$name.tar.zst"
    cat "$REPO/image/setup.sh" "$OUT/$name.tar.zst" >"$run"
    chmod +x "$run"
    rm -f "$OUT/$name.tar.zst"
    say "    $(du -h "$run" | cut -f1)  $run"
}

# replace setup.sh at the front of the newest .run (the image after it stays as it is)
phase_header() {
    local run line
    run=$(ls -t "$OUT"/tebeos-linux-*.run 2>/dev/null | head -1)
    [ -n "$run" ] || die "no .run in $OUT"
    line=$(awk '/^__IMAGE_BELOW__$/{print NR + 1; exit 0}' "$run")
    [ -n "$line" ] || die "$run has no image marker"
    { cat "$REPO/image/setup.sh"; tail -n +"$line" "$run"; } >"$run.new"
    mv "$run.new" "$run"
    chmod +x "$run"
    say "    setup.sh in $run updated"
}

for p in "${@:-}"; do
    case "$p" in
        base) phase_base ;;
        fonts) phase_fonts ;;
        app) phase_app ;;
        pos) phase_pos ;;
        linux) phase_linux ;;
        pack) phase_pack ;;
        header) phase_header ;;
        all) phase_base; phase_app; phase_pos; phase_linux; phase_pack ;;
        *) echo "usage: $0 base|app|pos|linux|pack|all ..." >&2; exit 2 ;;
    esac
done
