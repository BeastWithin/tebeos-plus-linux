#!/usr/bin/env bash
#
# install-tebeos-client.sh
# ---------------------------------------------------------------------------
# Installs TEBEOS Plus as a *client / terminal* on Linux, using Wine.
# The client connects to an existing TEBEOS "Ana Bilgisayar" (main computer,
# normally a Windows PC) that runs PostgreSQL on TCP port 5454.
#
# Works on Debian/Ubuntu (apt), Arch (pacman), Fedora (dnf) and openSUSE (zypper).
# Wine comes from the distro when possible, otherwise a portable build is
# downloaded into your home directory (no multilib / 32-bit host libs needed).
#
# Nothing system-wide is installed except a few small packages (cabextract etc.).
# Everything else lives in:
#     ${TEBEOS_PREFIX}      Wine prefix           (default ~/.wine-tebeos)
#     ~/.local/share/tebeos Wine/tools if downloaded, plus logs
#     ~/tebeos              launcher script
#
# Usage:
#   ./install-tebeos-client.sh --server-ip 192.168.1.50
#   ./install-tebeos-client.sh                 # will ask for the server IP
#
# Run with --help for all options.
# ---------------------------------------------------------------------------

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_VERSION="1.1"
TEBEOS_URL_DEFAULT="https://eczacilar-p.mncdn.com/Setup/tebeos_plus_client_setup_v1.exe"

# ---------------------------------------------------------------- defaults --
SERVER_IP=""
SERVER_NAME=""
WINE_MODE="auto"                       # auto | system | download
WINE_URL=""                            # optional explicit wine tarball URL
TEBEOS_PREFIX="${TEBEOS_PREFIX:-$HOME/.wine-tebeos}"
STATE_DIR="$HOME/.local/share/tebeos"
WINE_HOME=""                           # resolved wine installation dir
INSTALLER=""
SKIP_DEPS=0
SKIP_DOTNET=0
SKIP_INSTALL=0
SKIP_VERIFY=0
SKIP_FONTS=0
SKIP_GECKO=0
ASSUME_YES=0
MAIN_COMPUTER=0
FORCE_PREFIX=0
DRY_RUN=0

LOG="$STATE_DIR/install.log"

# ------------------------------------------------------------------ output --
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
    C_BLU=$'\033[36m'; C_BLD=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_RED=; C_GRN=; C_YEL=; C_BLU=; C_BLD=; C_OFF=
fi

say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$C_BLU$C_BLD" "$*" "$C_OFF"; }
ok()   { printf '    %s[ ok ]%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '    %s[warn]%s %s\n' "$C_YEL" "$C_OFF" "$*"; }
err()  { printf '    %s[FAIL]%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; }
die()  { err "$*"; say ""; say "Log: $LOG"; exit 1; }

run() {   # run a command, tee its output into the log
    if [ "$DRY_RUN" = 1 ]; then
        say "    (dry-run) $*"
        return 0
    fi
    { printf '\n$ %s\n' "$*"; } >>"$LOG"
    "$@" >>"$LOG" 2>&1
}

usage() {
    cat <<EOF
install-tebeos-client.sh v$SCRIPT_VERSION - install TEBEOS Plus client (terminal) on Linux

Usage:
  $0 [options]

Options:
  --server-ip IP        IP address of the TEBEOS main computer (server).
  --server-name NAME    Host name of the main computer (optional, auto-detected).
  --main-computer       Configure this machine as the MAIN COMPUTER instead of a
                        terminal (equivalent to the vendor's server install).
  --wine MODE           auto (default) | system | download
  --wine-url URL        Explicit Wine tarball URL (used with --wine download).
  --prefix DIR          Wine prefix to use (default: ~/.wine-tebeos).
  --installer FILE      Use a local copy of tebeos_plus_client_setup_v1.exe.
  --skip-deps           Do not try to install packages (no sudo).
  --skip-dotnet         Do not install .NET Framework 4.8 (already present).
  --skip-install        Do not run the TEBEOS installer (files already there).
  --skip-verify         Do not do the post-install launch check.
  --skip-fonts          Do not install Windows fonts / patch ₺ into them.
  --skip-gecko          Do not install wine-gecko (the Medula window's engine).
  --force-prefix        Wipe and recreate the Wine prefix.
  --dry-run             Show what would be done, change nothing.
  -y, --yes             Do not ask for confirmation.
  -h, --help            This help.

Examples:
  $0 --server-ip 192.168.1.50
  $0 --server-ip 192.168.1.50 --wine download
  $0 --main-computer
EOF
}

# ------------------------------------------------------------------- parse --
while [ $# -gt 0 ]; do
    case "$1" in
        --server-ip)     SERVER_IP="${2:-}"; shift 2 ;;
        --server-name)   SERVER_NAME="${2:-}"; shift 2 ;;
        --main-computer) MAIN_COMPUTER=1; shift ;;
        --wine)          WINE_MODE="${2:-}"; shift 2 ;;
        --wine-url)      WINE_URL="${2:-}"; shift 2 ;;
        --prefix)        TEBEOS_PREFIX="${2:-}"; shift 2 ;;
        --installer)     INSTALLER="${2:-}"; shift 2 ;;
        --skip-deps)     SKIP_DEPS=1; shift ;;
        --skip-dotnet)   SKIP_DOTNET=1; shift ;;
        --skip-install)  SKIP_INSTALL=1; shift ;;
        --skip-verify)   SKIP_VERIFY=1; shift ;;
        --skip-fonts)    SKIP_FONTS=1; shift ;;
        --skip-gecko)    SKIP_GECKO=1; shift ;;
        --force-prefix)  FORCE_PREFIX=1; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        -y|--yes)        ASSUME_YES=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) die "Unknown option: $1  (try --help)" ;;
    esac
done

case "$WINE_MODE" in auto|system|download) ;; *) die "--wine must be auto|system|download" ;; esac

mkdir -p "$STATE_DIR" || die "Cannot create $STATE_DIR"
: >"$LOG"

# ------------------------------------------------------------- environment --
export WINEPREFIX="$TEBEOS_PREFIX"
export WINEARCH=win64
export WINEDEBUG="${WINEDEBUG:--all}"
# mscoree must be NATIVE once real .NET is installed: an empty/disabled override
# (e.g. "mscoree,mshtml=") makes Wine report mscoree.dll as missing and any
# .NET app dies with "IL-only binary cannot be loaded".
#
# mshtml must stay ENABLED: TEBEOS' Medula window hosts a *Microsoft Web Browser*
# ActiveX control (Wine's ieframe) browsing medeczane.sgk.gov.tr, and Wine
# renders that with MSHTML + wine-gecko.  See setup_gecko/register_mshtml below.
export WINEDLLOVERRIDES="mscoree=n"
export WINEDLLPATH=""

# ------------------------------------------------- Turkish Windows locale ---
# Ayarlar.ini is written and read through the ANSI Win32 profile API
# (Get/WritePrivateProfileStringA).  Wine takes that codepage from the HOST
# locale, *not* from HKLM\...\Nls\CodePage (verified: setting ACP=1254 in the
# registry changes nothing, LC_ALL=tr_TR.UTF-8 does).  With an en_US host the
# codepage is 1252, which has no 'ğ', so Windows' best-fit mapping silently
# turns a remembered password of 'çığ' into 'çıg' -- the next login then fails
# and the user has to retype it.  Under tr_TR.UTF-8 the codepage is 1254 and
# 'çığ' round-trips byte for byte.
TEBEOS_LC=""
if command -v locale >/dev/null 2>&1; then
    case "$(locale -a 2>/dev/null | tr 'A-Z' 'a-z')" in
        *tr_tr.utf8*) TEBEOS_LC="tr_TR.UTF-8" ;;
        *tr_tr*)      TEBEOS_LC="tr_TR" ;;
    esac
fi
[ -n "$TEBEOS_LC" ] || warn "no Turkish locale on this system - Turkish characters in Ayarlar.ini would be corrupted (install tr_TR.UTF-8)"
[ -n "$TEBEOS_LC" ] && say "    Windows locale : $TEBEOS_LC (ANSI codepage 1254 - required for Turkish passwords)"

# ------------------------------------------------------------------- setup --
step "TEBEOS client installer v$SCRIPT_VERSION"
say "    Wine prefix : $TEBEOS_PREFIX"
say "    Log file    : $LOG"
[ "$DRY_RUN" = 1 ] && warn "dry-run mode: nothing will be changed"

if [ "$MAIN_COMPUTER" = 0 ]; then
    if [ -z "$SERVER_IP" ]; then
        if [ -t 0 ]; then
            printf '    IP address of the TEBEOS main computer: '
            read -r SERVER_IP
        fi
    fi
    [ -n "$SERVER_IP" ] || die "No --server-ip given. A terminal needs to know where the main computer is."

    case "$SERVER_IP" in
        *[!0-9.]*|"") die "'$SERVER_IP' does not look like an IPv4 address." ;;
    esac
    ok "main computer (server) = $SERVER_IP"
fi

# --------------------------------------------------------------- packages --
detect_pkg() {
    if   command -v pacman >/dev/null 2>&1; then PKG=pacman
    elif command -v apt-get >/dev/null 2>&1; then PKG=apt
    elif command -v dnf    >/dev/null 2>&1; then PKG=dnf
    elif command -v zypper >/dev/null 2>&1; then PKG=zypper
    else PKG=unknown
    fi
}

SUDO=""
need_sudo() { [ "$(id -u)" != 0 ] && SUDO="sudo"; }
have_sudo() { [ -z "$SUDO" ] || sudo -n true 2>/dev/null; }

pkg_install_cmd() {
    case "$PKG" in
        pacman) printf 'pacman -S --needed --noconfirm' ;;
        apt)    printf 'apt-get install -y' ;;
        dnf)    printf 'dnf install -y' ;;
        zypper) printf 'zypper --non-interactive install' ;;
    esac
}

ensure_deps() {
    step "Checking basic tools"
    local missing=""
    for t in curl tar xz unzip cabextract; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    # xz is often the binary 'xz'; tar handles .tar.xz if xz is present
    if [ -z "$missing" ]; then
        ok "all basic tools present"
        return 0
    fi
    say "    missing:$missing"

    if [ "$SKIP_DEPS" = 1 ]; then
        warn "--skip-deps: not installing them. Some steps may fail."
        return 0
    fi
    detect_pkg
    if [ "$PKG" = unknown ]; then
        warn "Could not detect a known package manager."
        warn "Please install these manually: cabextract curl tar xz-utils unzip"
        return 0
    fi
    need_sudo
    if ! have_sudo; then
        warn "sudo is not available without a password here."
        warn "Please run this once:  sudo $(pkg_install_cmd) cabextract curl unzip xz"
        return 0
    fi
    say "    installing via $PKG ..."
    case "$PKG" in
        pacman) run $SUDO pacman -S --needed --noconfirm cabextract curl unzip xz || true ;;
        apt)    run $SUDO apt-get update || true
                run $SUDO apt-get install -y cabextract curl unzip xz-utils || true ;;
        dnf)    run $SUDO dnf install -y cabextract curl unzip xz || true ;;
        zypper) run $SUDO zypper --non-interactive install cabextract curl unzip xz || true ;;
    esac
    for t in cabextract; do
        if ! command -v "$t" >/dev/null 2>&1; then
            warn "'$t' still missing - winetricks may not be able to install .NET."
        fi
    done
}

# -------------------------------------------------------------------- wine --
probe_wine() {   # $1 = wine binary ; true if it can run 32-bit Windows apps
    local wine="$1" out
    out=$(timeout 180 "$wine" 'C:\windows\syswow64\cmd.exe' /c "echo ok32" 2>&1) || true
    printf '%s' "$out" | grep -q "ok32"
}

find_system_wine() {
    local c
    for c in wine wine-stable wine-staging wine-development; do
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

install_system_wine() {
    detect_pkg
    [ "$PKG" = unknown ] && return 1
    need_sudo
    have_sudo || return 1
    say "    installing Wine via $PKG ..."
    case "$PKG" in
        pacman) run $SUDO pacman -S --needed --noconfirm wine winetricks || return 1 ;;
        apt)    run $SUDO dpkg --add-architecture i386 || true
                run $SUDO apt-get update || true
                run $SUDO apt-get install -y wine cabextract || return 1
                # 32-bit wine libs; not present on newer Debian (wow64 build)
                run $SUDO apt-get install -y wine32 || true
                run $SUDO apt-get install -y wine64 || true ;;
        dnf)    run $SUDO dnf install -y wine cabextract || return 1
                run $SUDO dnf install -y wine-core.i686 || true ;;
        zypper) run $SUDO zypper --non-interactive install wine cabextract || return 1 ;;
    esac
    return 0
}

find_portable_wine() {   # an already-extracted portable Wine, if there is one
    local d
    for d in "$STATE_DIR"/wine/bin/wine "$STATE_DIR"/wine/*/bin/wine; do
        [ -x "$d" ] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

download_wine() {
    local url="$WINE_URL" base tmp
    if [ -z "$url" ]; then
        say "    looking up the latest portable Wine build ..."
        url=$(curl -sS --max-time 40 https://api.github.com/repos/Kron4ek/Wine-Builds/releases/latest 2>/dev/null \
              | grep -o 'https://[^"]*wine-[0-9.]*-staging-amd64-wow64\.tar\.xz' | head -1) || true
        # known-good fallback
        [ -z "$url" ] && url="https://github.com/Kron4ek/Wine-Builds/releases/download/11.18/wine-11.18-staging-amd64-wow64.tar.xz"
    fi
    say "    $url"
    mkdir -p "$WINE_HOME" || return 1
    tmp="$STATE_DIR/wine-dl.tar.xz"
    run curl -L --fail --retry 3 -o "$tmp" "$url" || return 1
    run tar -xJf "$tmp" -C "$WINE_HOME" || return 1
    rm -f "$tmp"
    # the tarball may contain bin/ at the top level or inside one directory
    if [ ! -x "$WINE_HOME/bin/wine" ]; then
        local d
        d=$(find "$WINE_HOME" -maxdepth 3 -type f -name wine -path '*/bin/*' 2>/dev/null | head -1)
        if [ -n "$d" ]; then
            WINE_HOME=$(dirname "$(dirname "$d")")
        else
            return 1
        fi
    fi
    return 0
}

setup_wine() {
    step "Setting up Wine"

    case "$WINE_MODE" in
        system)
            local sys
            sys=$(find_system_wine) || die "No system Wine found (and --wine system was requested)."
            WINE_HOME=$(dirname "$(dirname "$sys")")
            WINE_BIN="$sys"
            ;;
        download)
            if sys=$(find_portable_wine); then
                WINE_BIN="$sys"
                WINE_HOME=$(dirname "$(dirname "$sys")")
                ok "using previously downloaded Wine"
            else
                WINE_HOME="$STATE_DIR/wine"
                download_wine || die "Could not download a portable Wine build."
                WINE_BIN="$WINE_HOME/bin/wine"
            fi
            ;;
        auto)
            local sys=""
            sys=$(find_system_wine) || true
            if [ -n "$sys" ]; then
                WINE_BIN="$sys"
                WINE_HOME=$(dirname "$(dirname "$sys")")
                ok "system Wine: $WINE_BIN ($("$WINE_BIN" --version 2>/dev/null))"
            else
                say "    no system Wine found"
                if install_system_wine && sys=$(find_system_wine); then
                    WINE_BIN="$sys"
                    WINE_HOME=$(dirname "$(dirname "$sys")")
                    ok "installed system Wine: $WINE_BIN"
                else
                    warn "could not get Wine from the package manager"
                    say "    falling back to a portable Wine build (no root needed)"
                    if sys=$(find_portable_wine); then
                        WINE_BIN="$sys"
                        WINE_HOME=$(dirname "$(dirname "$sys")")
                    else
                        WINE_HOME="$STATE_DIR/wine"
                        download_wine || die "Could not obtain Wine. See --help."
                        WINE_BIN="$WINE_HOME/bin/wine"
                    fi
                    ok "portable Wine: $WINE_BIN"
                fi
            fi
            ;;
    esac

    export WINE="$WINE_BIN"
    say "    $("$WINE_BIN" --version 2>/dev/null || echo '?')"

    # ---- prefix ------------------------------------------------------------
    if [ "$FORCE_PREFIX" = 1 ] && [ -d "$WINEPREFIX" ]; then
        warn "removing existing prefix: $WINEPREFIX"
        rm -rf "$WINEPREFIX"
    fi

    if [ ! -d "$WINEPREFIX" ]; then
        say "    creating prefix (this takes a moment) ..."
        run "$WINE_BIN" wineboot -u || die "wineboot failed"
        sleep 3
    else
        ok "prefix already exists"
    fi

    # ---- 32-bit capability (TEBEOS is a 32-bit program) --------------------
    if probe_wine "$WINE_BIN"; then
        ok "Wine can run 32-bit Windows programs"
    else
        warn "this Wine cannot run 32-bit programs (TEBEOS needs that)"
        if [ "$WINE_MODE" != download ]; then
            say "    trying a portable Wine build instead ..."
            if ! sys=$(find_portable_wine); then
                WINE_HOME="$STATE_DIR/wine"
                download_wine || true
                sys=$(find_portable_wine) || sys=""
            fi
            if [ -n "$sys" ] && probe_wine "$sys"; then
                WINE_BIN="$sys"
                WINE_HOME=$(dirname "$(dirname "$sys")")
                export WINE="$WINE_BIN"
                ok "portable Wine works: $WINE_BIN"
            else
                die "No Wine build able to run 32-bit programs. On Debian try: sudo dpkg --add-architecture i386; sudo apt install wine32"
            fi
        else
            die "The downloaded Wine cannot run 32-bit programs - please report this."
        fi
    fi
}

# --------------------------------------------------------------- winetricks --
prefix_busy() {   # a TEBEOS process still running in the prefix
    # anchored on argv[0]: a plain `ps | grep TebeosPlusClient` also matches the
    # shell that happens to have that string in its own command line
    pgrep -f '^C:\\Program Files \(x86\)\\TebeosPlusClient' >/dev/null 2>&1
}

busy_guard() {    # winetricks runs `wineserver -w`, which waits for every wine
                  # process to exit - so it would hang until TEBEOS is closed
    if prefix_busy; then
        warn "TEBEOS is running in $WINEPREFIX - close it and re-run:"
        warn "  $0 --skip-deps --skip-dotnet --skip-install"
        return 1
    fi
    return 0
}

ensure_winetricks() {   # download only - safe to call more than once
    WT="$STATE_DIR/winetricks"
    if [ ! -s "$WT" ]; then
        say "    downloading winetricks ..."
        run curl -L --fail --retry 3 -o "$WT" \
            https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks \
            || die "Could not download winetricks."
    fi
    chmod +x "$WT"
}

setup_winetricks() {
    step "Setting up winetricks"
    ensure_winetricks
    ok "winetricks ready"

    say "    setting Windows version to Windows 7"
    busy_guard || return 0
    run env WINE="$WINE_BIN" "$WT" -q win7 || warn "could not set Windows version"
}

# ------------------------------------------------------------------- dotnet --
install_dotnet() {
    if [ "$SKIP_DOTNET" = 1 ]; then
        warn "--skip-dotnet: assuming .NET Framework 4.8 is already installed"
        return 0
    fi
    step "Installing .NET Framework 4.8 and VC++ runtime (this is slow)"
    say "    TEBEOS is a .NET 4.5.2 WinForms application. Expect 5-20 minutes."
    say "    Progress is written to: $LOG"
    if run env WINE="$WINE_BIN" "$WT" -q -f dotnet48; then
        ok ".NET Framework 4.8 installed"
    else
        warn "dotnet48 reported errors - see the log"
        warn "If the app later fails to start, re-run:  WINEPREFIX=$WINEPREFIX $WT -q dotnet48"
    fi
    run env WINE="$WINE_BIN" "$WT" -q vcrun2019 || warn "vcrun2019 had errors (often harmless)"
}

# --------------------------------------------------------------------- app --
app_dir()  { printf '%s/drive_c/Program Files (x86)/TebeosPlusClient' "$WINEPREFIX"; }

get_installer() {
    step "Getting the TEBEOS client installer"
    if [ -n "$INSTALLER" ]; then
        [ -f "$INSTALLER" ] || die "Installer not found: $INSTALLER"
        ok "using $INSTALLER"
        return 0
    fi
    local local_copy=""
    for c in "$HOME/İndirilenler/tebeos_plus_client_setup_v1.exe" \
             "$HOME/Downloads/tebeos_plus_client_setup_v1.exe" \
             "$STATE_DIR/tebeos_plus_client_setup_v1.exe"; do
        [ -f "$c" ] && { local_copy="$c"; break; }
    done
    if [ -n "$local_copy" ]; then
        INSTALLER="$local_copy"
        ok "found local copy: $INSTALLER"
        return 0
    fi

    INSTALLER="$STATE_DIR/tebeos_plus_client_setup_v1.exe"
    say "    downloading ~266 MB from the vendor ..."
    run curl -L --fail --retry 3 -o "$INSTALLER" "$TEBEOS_URL_DEFAULT" \
        || die "Download failed. Download it manually and use --installer FILE."
    ok "downloaded"
}

install_app() {
    local dir; dir=$(app_dir)

    if [ "$SKIP_INSTALL" = 1 ]; then
        warn "--skip-install: not running the installer"
        return 0
    fi
    if [ -f "$dir/app/tebeos_plus.exe" ]; then
        ok "TEBEOS is already installed in this prefix"
        return 0
    fi

    get_installer

    step "Installing TEBEOS (silent attempt)"
    run env WINEPREFIX="$WINEPREFIX" WINE="$WINE_BIN" "$WINE_BIN" "$INSTALLER" \
        /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/LOG=C:\\tebeos_setup.log" || true

    if [ -f "$dir/app/tebeos_plus.exe" ]; then
        ok "installed silently"
        return 0
    fi

    # The vendor's installer has a wizard page it will not skip in silent mode,
    # so fall back to the normal (visible) installer.
    step "Silent install not possible - running the installer window"
    say "    ${C_BLD}Please click through the installer${C_OFF} (a few 'İleri/Next' clicks)."
    say "    Accept the licence (SOZLESME) and keep the default folder."
    if [ "$DRY_RUN" = 0 ]; then
        env WINEPREFIX="$WINEPREFIX" WINE="$WINE_BIN" "$WINE_BIN" "$INSTALLER" \
            "/LOG=C:\\tebeos_setup.log" || true
    fi

    if [ ! -f "$dir/app/tebeos_plus.exe" ]; then
        if [ "$DRY_RUN" = 1 ]; then return 0; fi
        die "TEBEOS does not seem to be installed. Check the window for errors."
    fi
    ok "installed"
}

# ---------------------------------------------------------------- configure --
# TEBEOS finds its database server through "tebeos_plus.xml" -> <Host>, and
# keeps terminal settings in "Ayarlar.ini".  Notes:
#   * [BAGLANTI] CLIENT=TRUE   -> this machine is a terminal (client)
#     [BAGLANTI] CLIENT=FALSE  -> this machine is the main computer
#   * the section name in the code is "CONNECTİON" with a Turkish dotted İ.
#     Depending on the Wine build that is stored either as UTF-8 or as CP1254,
#     so we write the section in *both* encodings with the same value.
configure_client() {
    step "Configuring TEBEOS"
    local dir xml ini tmp name
    dir=$(app_dir); xml="$dir/tebeos_plus.xml"; ini="$dir/app/Ayarlar.ini"

    [ -f "$xml" ] || die "tebeos_plus.xml not found - is TEBEOS installed in $dir ?"

    if [ "$MAIN_COMPUTER" = 1 ]; then
        SERVER_IP="127.0.0.1"
        BAGLANTI_VALUE="FALSE"
        say "    mode: MAIN COMPUTER (server)"
    else
        BAGLANTI_VALUE="TRUE"
        say "    mode: TERMINAL (client of $SERVER_IP)"
    fi

    # --- server IP into tebeos_plus.xml (this is the DB host) ---------------
    if grep -q '<Host>' "$xml" 2>/dev/null; then
        run sed -i "s|<Host>[^<]*</Host>|<Host>${SERVER_IP}</Host>|" "$xml"
        ok "tebeos_plus.xml: <Host>${SERVER_IP}</Host>"
    else
        run sed -i "s|</Settings>|  <Host>${SERVER_IP}</Host>\n</Settings>|" "$xml"
        ok "tebeos_plus.xml: added <Host>${SERVER_IP}</Host>"
    fi

    # --- terminal settings in Ayarlar.ini ----------------------------------
    # work out the server's name for [HOSTNAME] ANABILGISAYAR
    name="$SERVER_NAME"
    if [ -z "$name" ] && [ "$MAIN_COMPUTER" = 0 ]; then
        if command -v getent >/dev/null 2>&1; then
            name=$(getent hosts "$SERVER_IP" 2>/dev/null | awk '{print $2; exit}')
        fi
        [ -z "$name" ] && name="$SERVER_IP"
    fi
    [ -z "$name" ] && name="$(hostname 2>/dev/null || echo localhost)"

    if [ -f "$ini" ]; then
        tmp="$ini.new"
        # drop the sections we manage (both İ encodings), keep everything else
        LC_ALL=C awk '
            /^\[/ { h=$0; sub(/\r$/,"",h);
                    skip = (h ~ /^\[CONNECT.*ON\]$/ || h == "[BAGLANTI]" || h == "[HOSTNAME]") }
            { if (!skip) print }
        ' "$ini" >"$tmp"

        # İ in UTF-8 (0xC4 0xB0) and in CP1254 (0xDD)
        {
            printf '[CONNECT\304\260ON]\r\nCLIENT=%s\r\n' "$SERVER_IP"
            printf '[CONNECT\335ON]\r\nCLIENT=%s\r\n'      "$SERVER_IP"
            printf '[BAGLANTI]\r\nCLIENT=%s\r\n'           "$BAGLANTI_VALUE"
            printf '[HOSTNAME]\r\nANABILGISAYAR=%s\r\n'    "$name"
        } >>"$tmp"

        run mv "$tmp" "$ini"
        ok "Ayarlar.ini: server=$SERVER_IP  terminal=$([ "$BAGLANTI_VALUE" = TRUE ] && echo yes || echo no)"
    else
        warn "Ayarlar.ini not found yet - it is created when TEBEOS first runs."
        warn "Re-run this script with --skip-deps --skip-dotnet --skip-install afterwards"
        warn "to apply the configuration, or set it in the app (Ayarlar > Terminal)."
    fi
}

# ---------------------------------------------------------------- fonts --
# Two separate font problems:
#   1. TEBEOS asks for "System" ~9,000 times a session; Wine resolves that
#      through fontconfig, so an unknown family came back as Noto Sans and the
#      UI mixed two typefaces.  Real Windows fonts + a System->Tahoma
#      substitution make it look like it does on Windows.
#   2. Neither Wine's Tahoma/Arial clones nor the XP-era fonts that
#      winetricks/corefonts installs contain U+20BA (TURKISH LIRA SIGN), so
#      every money amount was painted as "1.234,56□".
# ./fonts/ holds Wine's own three files with that single glyph grafted in;
# patch-wine-fonts.py does the same to the real fonts in the prefix (family
# names, metrics and all other glyphs are untouched).

fonttools_python() {   # print a python3 that can import fontTools, or nothing
    if python3 -c 'import fontTools' 2>/dev/null; then printf 'python3'; return 0; fi
    local lib="$STATE_DIR/pylibs"
    if [ -d "$lib/fontTools" ]; then printf 'python3:%s' "$lib"; return 0; fi
    # to stderr: this function's stdout is captured as its return value
    say "    fetching fontTools (needed to add ₺ to the installed Windows fonts) ..." >&2
    if python3 -m pip install --quiet --target "$lib" --break-system-packages fonttools >>"$LOG" 2>&1 \
       && [ -d "$lib/fontTools" ]; then
        printf 'python3:%s' "$lib"
    fi
}

graft_lira_in_prefix() {   # $1 = prefix Fonts dir, $2 = "python3[:libdir]"
    local dir="$1" spec="$2" py extra="" f files=""
    case "$spec" in *:*) py="${spec%%:*}"; extra="${spec#*:}" ;; *) py="$spec" ;; esac
    for f in tahoma.ttf tahomabd.ttf arial.ttf arialbd.ttf; do
        [ -f "$dir/$f" ] && files="$files $dir/$f"
    done
    [ -n "$files" ] || return 0
    if ( cd "$SCRIPT_DIR" && PYTHONPATH="$extra" "$py" ./patch-wine-fonts.py $files ) >>"$LOG" 2>&1; then
        ok "₺ present in the prefix's Tahoma/Arial (details in $LOG)"
    else
        warn "could not graft ₺ into the prefix fonts - see $LOG"
    fi
}

install_fonts() {
    if [ "$SKIP_FONTS" = 1 ]; then
        warn "--skip-fonts: leaving fonts alone"
        return 0
    fi
    step "Windows fonts and the ₺ (lira) sign"
    busy_guard || return 0

    ensure_winetricks
    say "    installing corefonts + tahoma (real Windows fonts) ..."
    run env WINE="$WINE_BIN" "$WT" -q corefonts tahoma \
        || warn "corefonts/tahoma reported errors - Wine's own fonts are still patched below"

    # Wine's own copies: used when no real Tahoma was installed, or alongside it
    local src="$SCRIPT_DIR/fonts" wroot dest f
    wroot="$(cd "$(dirname "$WINE_BIN")/.." 2>/dev/null && pwd)" || wroot=""
    dest="$wroot/share/wine/fonts"
    if [ -n "$wroot" ] && [ -d "$dest" ] && [ -w "$dest" ] && [ -d "$src" ]; then
        mkdir -p "$dest/orig"
        for f in tahoma.ttf tahomabd.ttf arial.ttf; do
            [ -f "$src/$f" ] || continue
            [ -f "$dest/$f" ] && [ ! -f "$dest/orig/$f" ] && cp "$dest/$f" "$dest/orig/$f"
            run cp "$src/$f" "$dest/$f" && ok "Wine's $f: ₺ added"
        done
    else
        warn "Wine's font directory not writable - skipped"
    fi

    # the real Windows fonts winetricks just dropped into the prefix
    local spec; spec="$(fonttools_python)"
    if [ -n "$spec" ]; then
        graft_lira_in_prefix "$WINEPREFIX/drive_c/windows/Fonts" "$spec"
    else
        warn "fontTools unavailable - installed Tahoma/Arial would show a box instead of ₺"
    fi

    # stop the UI mixing Noto Sans with Tahoma
    run "$WINE_BIN" reg add \
        'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' \
        /v 'System' /t REG_SZ /d 'Tahoma' /f || warn "could not add the System→Tahoma substitution"
    ok "font substitution: System → Tahoma"
}

# ----------------------------------------------------- mshtml keyCode --
# Bug #11: Wine's MSHTML returns keyCode = 0 for keypress events, so IE-era input
# filters cancel every keystroke.  SGK Medula's captcha field is exactly such a
# filter, which makes the Medula login impossible.  wine-patch/mshtml/ holds
# mshtml.dll built from matching Wine source with that one change.
install_mshtml_patch() {
    step "MSHTML keyCode patch (for the Medula captcha)"
    local src="$SCRIPT_DIR/wine-patch/mshtml" wroot arch f want have
    if [ ! -d "$src" ]; then
        warn "wine-patch/mshtml not found - Medula's captcha will stay untypable"
        warn "see wine-patch/README.md"
        return 0
    fi
    busy_guard || return 0
    want="$(cat "$src/WINE-VERSION" 2>/dev/null)"
    have="$("$WINE_BIN" --version 2>/dev/null | sed 's/^wine-//; s/[^0-9.].*$//')"
    if [ -n "$want" ] && [ "$have" != "$want" ]; then
        warn "patch was built for Wine $want, this is Wine $have - skipping"
        warn "rebuild it with the recipe in wine-patch/README.md"
        return 0
    fi
    wroot="$(cd "$(dirname "$WINE_BIN")/.." 2>/dev/null && pwd)" || return 0
    for arch in i386-windows x86_64-windows; do
        f="$src/$arch/mshtml.dll"
        [ -f "$f" ] || continue
        if [ -f "$wroot/lib/wine/$arch/mshtml.dll" ] && [ ! -f "$wroot/lib/wine/$arch/mshtml.dll.orig" ]; then
            cp "$wroot/lib/wine/$arch/mshtml.dll" "$wroot/lib/wine/$arch/mshtml.dll.orig"
        fi
        run cp "$f" "$wroot/lib/wine/$arch/mshtml.dll" && ok "$arch/mshtml.dll patched"
    done
}

# ----------------------------------------------------------------- medula --
# The Medula window hosts a legacy *Microsoft Web Browser* ActiveX control
# (Wine's ieframe) that browses medeczane.sgk.gov.tr.  Wine renders it with
# MSHTML + wine-gecko, i.e. Firefox's engine, so two things are needed:
#   * wine-gecko, matching the version this Wine build asks for;
#   * MSHTML's classes registered for 32-BIT processes.  In the tested prefix
#     they were missing, so ieframe could not create an HTML document, mshtml
#     was never loaded and the pane stayed blank.  Registering it fixed it.
gecko_version() {
    local root f
    root="$(cd "$(dirname "$WINE_BIN")/.." 2>/dev/null && pwd)" || return 1
    for f in "$root"/lib/wine/*/mshtml.dll; do
        [ -f "$f" ] || continue
        grep -ao '2\.[0-9][0-9]*\.[0-9][0-9]*' "$f" | sort -u | tail -1 && return 0
    done
    return 1
}

setup_gecko() {
    if [ "$SKIP_GECKO" = 1 ]; then
        warn "--skip-gecko: the Medula window would stay blank"
        return 0
    fi
    step "wine-gecko (the engine behind the Medula window)"
    busy_guard || return 0
    local ver; ver="$(gecko_version)"
    if [ -z "$ver" ]; then
        warn "could not tell which gecko version this Wine wants - skipping"
        return 0
    fi
    local cache="${XDG_CACHE_HOME:-$HOME/.cache}/wine" arch f
    mkdir -p "$cache"
    for arch in x86 x86_64; do
        f="$cache/wine-gecko-$ver-$arch.msi"
        if [ ! -s "$f" ]; then
            run curl -L --fail --retry 3 -o "$f" \
                "https://dl.winehq.org/wine/wine-gecko/$ver/wine-gecko-$ver-$arch.msi" \
                || { warn "gecko download failed ($arch) - Medula may stay blank"; return 0; }
        fi
        run "$WINE_BIN" msiexec /i "$f" /qn || warn "gecko install failed ($arch)"
    done
    ok "wine-gecko $ver installed (x86 + x86_64)"
}

register_mshtml() {
    [ "$SKIP_GECKO" = 1 ] && return 0
    step "Registering MSHTML for 32-bit programs"
    busy_guard || return 0
    if run "$WINE_BIN" 'C:\windows\syswow64\regsvr32.exe' /s 'C:\windows\syswow64\mshtml.dll'; then
        ok "mshtml registered (without this the Medula pane is blank)"
    else
        warn "mshtml registration failed - the Medula pane will stay blank"
    fi
}

# -------------------------------------------------------------- KDE window rule --
# Under KWin, TEBEOS's drop-down lists (DevExpress pop-ups: untitled, "don't focus" windows)
# open hidden under the TEBEOS windows -- KWin's focus-stealing prevention puts a window that
# may not take focus below the active program. A rule keeps untitled tebeos_*.exe windows in
# the "above" layer. KDE only; other desktops ignore it. Merged into the user's other rules.
kde_popup_rule() {
    command -v kwriteconfig6 >/dev/null && command -v kreadconfig6 >/dev/null || return 0
    local rc=kwinrulesrc g=tebeos-popups rules
    kw() { kwriteconfig6 --file "$rc" --group "$g" --key "$1" "$2"; }
    kw Description "TEBEOS (Wine): untitled pop-up lists stay above their window"
    kw above true;  kw aboverule 2
    kw title '^$';  kw titlematch 3
    kw wmclass '^tebeos_.*\.exe$';  kw wmclassmatch 3
    rules=$(kreadconfig6 --file "$rc" --group General --key rules)
    case ",$rules," in *",$g,"*) ;; *) rules=${rules:+$rules,}$g ;; esac
    kwriteconfig6 --file "$rc" --group General --key rules "$rules"
    kwriteconfig6 --file "$rc" --group General --key count "$(tr ',' '\n' <<<"$rules" | grep -c .)"
    dbus-send --session --type=method_call --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure \
        >/dev/null 2>&1 || true
    ok "KDE: TEBEOS drop-down lists kept on top (window rule \"$g\")"
}

# ------------------------------------------------------------------ launcher --
make_launcher() {
    step "Creating launcher"
    local launcher="$HOME/tebeos/start-tebeos-client.sh"
    mkdir -p "$HOME/tebeos" || return 0
    local lcline=""
    [ -n "$TEBEOS_LC" ] && lcline="export LC_ALL=\"$TEBEOS_LC\""
    cat >"$launcher" <<EOF
#!/bin/bash
# TEBEOS client launcher - generated by install-tebeos-client.sh
export WINEPREFIX="$WINEPREFIX"
export WINEARCH=win64
export WINEDEBUG="-all"
# mscoree must be native; mshtml must stay ENABLED (the Medula window's browser)
export WINEDLLOVERRIDES="mscoree=n"
# Turkish locale: without it the ANSI codepage is 1252 and TEBEOS corrupts
# Turkish characters (notably the remembered password) when it writes Ayarlar.ini.
$lcline
WINE="$WINE_BIN"
APP="\${1:-tebeos_plus}"
# The Medula captcha is read by the OCR daemon (tessnet2 shim -> C:\\ocr-in -> tesseract).
# Without it every automatic Medula login posts a wrong captcha and SGK refuses it in a loop.
OCR="\$HOME/Projects/TebeosOnLinux/tebeos-work/ocr-service"
if [ -f "\$OCR/ocr_service.py" ] && ! pgrep -f "[o]cr_service.py" >/dev/null; then
  (cd "\$OCR" && setsid nohup python3 ocr_service.py >> service.log 2>&1 < /dev/null &)
fi
cd "\$WINEPREFIX/drive_c/Program Files (x86)/TebeosPlusClient/app" || exit 1
exec "\$WINE" "\$APP.exe"
EOF
    chmod +x "$launcher"
    ok "$launcher"

    # desktop entry
    local apps="$HOME/.local/share/applications" desk=""
    mkdir -p "$apps"
    if command -v xdg-user-dir >/dev/null 2>&1; then
        desk=$(xdg-user-dir DESKTOP 2>/dev/null) || desk=""
    fi
    [ -z "$desk" ] && for d in "$HOME/Desktop" "$HOME/Masaüstü"; do
        [ -d "$d" ] && { desk="$d"; break; }
    done

    local entry
    entry="[Desktop Entry]
Type=Application
Name=TEBEOS Plus (client)
Comment=TEBEOS pharmacy management - terminal connecting to $SERVER_IP
Exec=$launcher tebeos_plus
Terminal=false
Categories=Office;MedicalSoftware;
StartupNotify=true
StartupWMClass=tebeos_plus.exe
"
    printf '%s' "$entry" >"$apps/tebeos-client.desktop"
    chmod +x "$apps/tebeos-client.desktop"
    ok "menu entry: TEBEOS Plus (client)"
    if [ -n "$desk" ]; then
        printf '%s' "$entry" >"$desk/TEBEOS-client.desktop"
        chmod +x "$desk/TEBEOS-client.desktop"
        ok "desktop shortcut: $desk/TEBEOS-client.desktop"
    fi
}

# -------------------------------------------------------------------- verify --
verify() {
    step "Checking the main computer is reachable"
    if timeout 5 bash -c "cat < /dev/null > /dev/tcp/${SERVER_IP}/5454" 2>/dev/null; then
        ok "port 5454 on $SERVER_IP is open - database reachable"
    else
        warn "cannot reach $SERVER_IP:5454"
        warn "On the Windows main computer, allow TCP 5454:"
        warn '  New-NetFirewallRule -DisplayName "TEBEOS DB" -Direction Inbound -Protocol TCP -LocalPort 5454 -Action Allow'
    fi

    [ "$SKIP_VERIFY" = 1 ] && return 0
    [ "$DRY_RUN" = 1 ] && return 0
    [ -f "$(app_dir)/app/tebeos_plus.exe" ] || return 0

    step "Test launch (30 s)"
    local out="$STATE_DIR/testlaunch.log"
    [ -n "$TEBEOS_LC" ] && export LC_ALL="$TEBEOS_LC"
    ( cd "$(app_dir)/app" && "$WINE_BIN" tebeos_plus.exe ) >"$out" 2>&1 &
    local pid=$!
    sleep 30
    if kill -0 "$pid" 2>/dev/null; then
        ok "TEBEOS is running - a window should be on your desktop"
        kill "$pid" 2>/dev/null || true
        sleep 2
    else
        warn "TEBEOS exited during the test launch"
        if grep -qi "invalid IP address\|FormatException" "$out" 2>/dev/null; then
            err "known 'invalid IP' problem: the server address was not read."
            err "Fix: run  $0 --skip-deps --skip-dotnet --skip-install --server-ip $SERVER_IP"
            err "or set the address inside TEBEOS: login screen -> Terminal."
        else
            warn "see $out (and $LOG)"
        fi
    fi
}

# ------------------------------------------------------------------- summary --
summary() {
    step "Done"
    cat <<EOF
    Wine prefix : $TEBEOS_PREFIX
    Wine        : $WINE_BIN

    Start TEBEOS :  $HOME/tebeos/start-tebeos-client.sh
                    or the "TEBEOS Plus (client)" menu / desktop entry

    Log files   :  $LOG
                   $STATE_DIR/testlaunch.log

    The database server is expected at $SERVER_IP:5454
    (change it in $(app_dir)/tebeos_plus.xml -> <Host>, or in the app's
     Ayarlar / terminal settings)
EOF
    [ "$MAIN_COMPUTER" = 1 ] && say "    Mode: MAIN COMPUTER (this machine runs the database)"
}

# ---------------------------------------------------------------------- main --
ensure_deps
setup_wine
if [ "$SKIP_INSTALL" = 0 ] || [ "$SKIP_DOTNET" = 0 ]; then
    setup_winetricks
    install_dotnet
fi
install_fonts
install_mshtml_patch
setup_gecko
register_mshtml
install_app
configure_client
make_launcher
kde_popup_rule
verify
summary
