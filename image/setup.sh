#!/bin/bash
# setup.sh -- install the ready-made TEBEOS-on-Linux image on this PC.
#
#   ./tebeos-linux-<date>.run --role client --server-ip 192.168.1.101
#
# (The .run file is this script with the image appended; build-client-image.sh makes it.)
#
# What it puts where (the same layout as the PC it was built on):
#   ~/.local/share/tebeos/wine/<build>   Wine 11.18 staging with the patched mshtml and fonts
#   ~/.wine-tebeos                       the Wine prefix: .NET 4.8, TEBEOS, the CDP shim
#   ~/.local/share/tebeos/ocr-service    the captcha OCR daemon (host tesseract)
#   ~/.local/share/tebeos/embed          embed_run.py: puts Chromium's window inside Medula
#   ~/.local/share/tebeos/start-tebeos.sh + menu and desktop entries "TEBEOS Plus"
#
# Options:
#   --role client         this PC is a terminal (the only role so far)
#   --server-ip IP        the TEBEOS main computer (database on TCP 5454)
#   --server-name NAME    its Windows computer name, for [HOSTNAME] (default: looked up, or IP)
#   --replace             an existing ~/.wine-tebeos is moved aside (.bak-DATE) instead of
#                         stopping the install
#   --check               only check this PC's requirements
set -euo pipefail

ROLE="" SERVER_IP="" SERVER_NAME="" REPLACE=0 CHECK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --role) ROLE="${2:-}"; shift ;;
        --server-ip) SERVER_IP="${2:-}"; shift ;;
        --server-name) SERVER_NAME="${2:-}"; shift ;;
        --replace) REPLACE=1 ;;
        --check) CHECK=1 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

SELF="$(readlink -f "$0")"
SHARE="$HOME/.local/share/tebeos"
PFX="$HOME/.wine-tebeos"
STAMP="$(date +%Y%m%d-%H%M%S)"

if [ -t 1 ]; then B=$'\e[1m' G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' N=$'\e[0m'; else B= G= Y= R= N=; fi
say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()   { printf '    %s[ ok ]%s %s\n' "$G" "$N" "$*"; }
warn() { printf '    %s[warn]%s %s\n' "$Y" "$N" "$*"; }
die()  { printf '    %s[FAIL]%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

# ------------------------------------------------------------------ requirements --
pkgmgr() {
    for m in apt-get pacman dnf zypper; do command -v $m >/dev/null 2>&1 && { echo $m; return; }; done
}
pkgs_for() {   # packages that provide: chromium tesseract python3-pil zstd locale
    case "$(pkgmgr)" in
        apt-get) echo "chromium tesseract-ocr python3-pil zstd locales" ;;
        pacman)  echo "chromium tesseract tesseract-data-eng python-pillow zstd" ;;
        dnf)     echo "chromium tesseract python3-pillow zstd glibc-langpack-tr" ;;
        zypper)  echo "chromium tesseract-ocr python3-Pillow zstd glibc-locale" ;;
        *)       echo "chromium tesseract python3-pillow zstd" ;;
    esac
}
find_chromium() {
    local c
    for c in chromium chromium-browser google-chrome-stable google-chrome; do
        command -v "$c" >/dev/null 2>&1 && { readlink -f "$(command -v "$c")"; return; }
    done
}
turkish_locale() {
    locale -a 2>/dev/null | tr 'A-Z' 'a-z' | grep -qE '^tr_tr\.utf-?8$'
}
check_requirements() {
    step "Checking this PC"
    local miss=0
    [ "$(uname -m)" = x86_64 ] && ok "64-bit PC" || { warn "needs a 64-bit (x86_64) PC"; miss=1; }
    if [ -n "$(find_chromium)" ]; then ok "Chromium: $(find_chromium)"; else warn "Chromium not found (the Medula window runs on it)"; miss=1; fi
    if command -v tesseract >/dev/null 2>&1; then ok "tesseract (reads the Medula captcha)"; else warn "tesseract not found"; miss=1; fi
    if command -v python3 >/dev/null 2>&1; then ok "python3"; else warn "python3 not found"; miss=1; fi
    if python3 -c 'import PIL' 2>/dev/null; then ok "python3 Pillow"; else warn "python3 Pillow not found (better captcha reading)"; miss=1; fi
    if command -v zstd >/dev/null 2>&1; then ok "zstd"; else warn "zstd not found (needed to unpack)"; miss=1; fi
    if turkish_locale; then ok "Turkish locale tr_TR.UTF-8"; else
        warn "locale tr_TR.UTF-8 missing: Turkish letters in TEBEOS's settings (passwords) would break"
        case "$(pkgmgr)" in
            apt-get) warn "  enable it: sudo sed -i 's/^# *tr_TR.UTF-8/tr_TR.UTF-8/' /etc/locale.gen && sudo locale-gen" ;;
            pacman)  warn "  enable it: uncomment tr_TR.UTF-8 in /etc/locale.gen, then sudo locale-gen" ;;
        esac
        miss=1
    fi
    if [ "$miss" = 1 ]; then
        say ""
        say "    Install what is missing, e.g.:"
        case "$(pkgmgr)" in
            apt-get) say "      sudo apt-get install $(pkgs_for)" ;;
            pacman)  say "      sudo pacman -S --needed $(pkgs_for)" ;;
            dnf)     say "      sudo dnf install $(pkgs_for)" ;;
            zypper)  say "      sudo zypper install $(pkgs_for)" ;;
            *)       say "      $(pkgs_for)" ;;
        esac
        return 1
    fi
    local free; free=$(df -Pk "$HOME" | awk 'NR==2{print int($4/1024/1024)}')
    [ "$free" -ge 8 ] && ok "disk: ${free} GB free in $HOME" || { warn "only ${free} GB free in $HOME (8 needed)"; return 1; }
}

if [ "$CHECK" = 1 ]; then check_requirements; exit $?; fi

# ------------------------------------------------------------------------ options --
[ "$ROLE" = client ] || die "give --role client (the server role is not in this image yet)"
[ -n "$SERVER_IP" ] || { [ -t 0 ] && read -rp "    IP address of the TEBEOS main computer: " SERVER_IP; }
case "$SERVER_IP" in ''|*[!0-9.]*) die "'$SERVER_IP' is not an IPv4 address (--server-ip)";; esac

check_requirements || die "requirements missing (see above)"

if [ -e "$PFX" ]; then
    [ "$REPLACE" = 1 ] || die "$PFX already exists. Run again with --replace to move it aside to $PFX.bak-$STAMP"
    pgrep -f -i 'tebeos_plus.exe|tebeos_medula.exe' >/dev/null && die "TEBEOS is running: close it first"
fi

# ------------------------------------------------------------------------- unpack --
step "Unpacking (about 5 GB; a few minutes)"
STAGE="$SHARE/.unpack-$STAMP"
mkdir -p "$STAGE"
line=$(awk '/^__IMAGE_BELOW__$/{print NR + 1; exit 0}' "$SELF")
[ -n "$line" ] || die "no image in $SELF (run the .run file, not setup.sh)"
tail -n +"$line" "$SELF" | zstd -dc | tar -x -C "$STAGE" || die "unpacking failed (disk full?)"
. "$STAGE/BUILD-INFO"
ok "image built $BUILT_AT by $BUILT_BY (TEBEOS $TEBEOS_VERSION)"

if [ -e "$PFX" ]; then
    mv "$PFX" "$PFX.bak-$STAMP"
    ok "old prefix kept as $PFX.bak-$STAMP"
fi
mkdir -p "$SHARE/wine"
if [ -e "$SHARE/wine/$WINE_BUILD" ]; then
    mv "$SHARE/wine/$WINE_BUILD" "$SHARE/wine/$WINE_BUILD.bak-$STAMP"
    ok "old Wine kept as $SHARE/wine/$WINE_BUILD.bak-$STAMP"
fi
mv "$STAGE/wine" "$SHARE/wine/$WINE_BUILD"
mv "$STAGE/prefix" "$PFX"
rm -rf "$SHARE/ocr-service"
mv "$STAGE/ocr-service" "$SHARE/ocr-service"
rm -rf "$SHARE/embed"
mv "$STAGE/embed" "$SHARE/embed"
rm -rf "$STAGE"
ok "Wine, prefix, OCR service and embed helper in place"

# ------------------------------------------------------------ the prefix's user --
# The prefix was made by $BUILT_BY; Wine keeps the profile under C:\users\<name>.
if [ "$BUILT_BY" != "$USER" ] && [ -d "$PFX/drive_c/users/$BUILT_BY" ]; then
    rm -rf "$PFX/drive_c/users/$USER"
    mv "$PFX/drive_c/users/$BUILT_BY" "$PFX/drive_c/users/$USER"
    sed -i "s|\"C:\\\\\\\\users\\\\\\\\$BUILT_BY\"|\"C:\\\\\\\\users\\\\\\\\$USER\"|" "$PFX/system.reg"
    ok "Windows user folder: C:\\users\\$USER"
fi

# --------------------------------------------------------------------- settings --
step "Settings: terminal of $SERVER_IP"
APP="$PFX/drive_c/Program Files (x86)/TebeosPlusClient"
sed -i "s|<Host>[^<]*</Host>|<Host>$SERVER_IP</Host>|" "$APP/tebeos_plus.xml"
grep -q "<Host>$SERVER_IP</Host>" "$APP/tebeos_plus.xml" || die "could not set <Host> in tebeos_plus.xml"
ok "tebeos_plus.xml: database at $SERVER_IP:5454"
name="$SERVER_NAME"
[ -n "$name" ] || name=$(getent hosts "$SERVER_IP" 2>/dev/null | awk '{print $2; exit}') || true
[ -n "$name" ] || name="$SERVER_IP"
# The installer's Ayarlar.ini holds the vendor's defaults; only the connection sections are
# replaced. The section is "CONNECTİON" (dotted İ); Wine may store it as UTF-8 or CP1254
# (the installer's copy is UTF-8), so any of those goes and both are written.
# [BAGLANTI] CLIENT=TRUE: this PC is a terminal.
INI="$APP/app/Ayarlar.ini"
touch "$INI"
LC_ALL=C awk '
    /^\[/ { h=$0; sub(/\r$/,"",h); skip = (h ~ /^\[CONNECT.*ON\]$/ || h == "[BAGLANTI]" || h == "[HOSTNAME]") }
    { if (!skip) print }
' "$INI" >"$INI.new"
{
    printf '[CONNECT\304\260ON]\r\nCLIENT=%s\r\n' "$SERVER_IP"
    printf '[CONNECT\335ON]\r\nCLIENT=%s\r\n' "$SERVER_IP"
    printf '[BAGLANTI]\r\nCLIENT=TRUE\r\n'
    printf '[HOSTNAME]\r\nANABILGISAYAR=%s\r\n' "$name"
} >>"$INI.new"
mv "$INI.new" "$INI"
ok "Ayarlar.ini: terminal, main computer $name"

# --------------------------------------------------------------------- launcher --
step "Launcher and menu entries"
cat >"$SHARE/start-tebeos.sh" <<'EOF'
#!/bin/bash
# start-tebeos.sh -- start TEBEOS Plus (installed by the TEBEOS-on-Linux image)
SHARE="$HOME/.local/share/tebeos"
export WINEPREFIX="$HOME/.wine-tebeos" WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree=n"     # .NET's mscoree must be the native one
export LC_ALL=tr_TR.UTF-8               # ANSI codepage 1254: Turkish letters in Ayarlar.ini
WINE="$(ls -d "$SHARE"/wine/wine-*/bin/wine 2>/dev/null | grep -v '\.bak-' | head -1)"
[ -x "$WINE" ] || { echo "Wine not found under $SHARE/wine" >&2; exit 1; }
# the Medula window runs on Chromium; tell the shim where it is
for c in chromium chromium-browser google-chrome-stable google-chrome; do
    p="$(command -v "$c" 2>/dev/null)" && { p="$(readlink -f "$p")"; export TEBEOS_CHROMIUM="Z:${p//\//\\}"; break; }
done
# the captcha OCR daemon (tessnet2 shim -> C:\ocr-in -> tesseract), one per prefix: the
# prefix is on its command line (the daemon reads TEBEOS_WINEPREFIX) so pgrep can tell them apart
if ! pgrep -f "[o]cr_service.py $WINEPREFIX\$" >/dev/null; then
    (cd "$SHARE/ocr-service" && TEBEOS_WINEPREFIX="$WINEPREFIX" setsid nohup python3 ocr_service.py "$WINEPREFIX" \
        >>service.log 2>&1 </dev/null &)
fi
# Medula in embed mode: a visible Chromium draws the page inside the Medula window and the shim
# keeps all input. embed_run.py puts Chromium's window in place (X11 or Xwayland); one per prefix,
# it exits two minutes after the last TEBEOS program. TEBEOS_EMBED=0 brings back the screencast.
export TEBEOS_EMBED="${TEBEOS_EMBED:-1}"
if [ "$TEBEOS_EMBED" = 1 ] && ! pgrep -f "[e]mbed_run.py $WINEPREFIX " >/dev/null; then
    (setsid nohup python3 "$SHARE/embed/embed_run.py" "$WINEPREFIX" "${TEBEOS_CDP_PORT:-9333}" \
        >>"$SHARE/embed/embed_run.log" 2>&1 </dev/null &)
fi
cd "$WINEPREFIX/drive_c/Program Files (x86)/TebeosPlusClient/app" || exit 1
exec "$WINE" "${1:-tebeos_plus}.exe"
EOF
chmod +x "$SHARE/start-tebeos.sh"
ok "$SHARE/start-tebeos.sh"
entry="[Desktop Entry]
Type=Application
Name=TEBEOS Plus
Comment=TEBEOS eczane programı (ana bilgisayar: $SERVER_IP)
Exec=$SHARE/start-tebeos.sh tebeos_plus
Terminal=false
Categories=Office;MedicalSoftware;
StartupNotify=true
StartupWMClass=tebeos_plus.exe
"
mkdir -p "$HOME/.local/share/applications"
printf '%s' "$entry" >"$HOME/.local/share/applications/tebeos-client.desktop"
ok "menu entry: TEBEOS Plus"
desk=$(xdg-user-dir DESKTOP 2>/dev/null || true)
[ -d "$desk" ] || for d in "$HOME/Masaüstü" "$HOME/Desktop"; do [ -d "$d" ] && { desk="$d"; break; }; done
if [ -d "$desk" ]; then
    printf '%s' "$entry" >"$desk/TEBEOS-client.desktop"
    chmod +x "$desk/TEBEOS-client.desktop"
    ok "desktop icon: $desk/TEBEOS-client.desktop"
fi

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
kde_popup_rule

# ------------------------------------------------------------------------ check --
step "Checking the main computer"
if timeout 5 bash -c "cat </dev/null >/dev/tcp/$SERVER_IP/5454" 2>/dev/null; then
    ok "database reachable at $SERVER_IP:5454"
else
    warn "cannot reach $SERVER_IP:5454: is the main computer on, and TCP 5454 allowed in its firewall?"
fi

step "Done"
say "    Start TEBEOS from the menu or the desktop icon \"TEBEOS Plus\","
say "    or: $SHARE/start-tebeos.sh"
say "    The first start logs in as usual; TEBEOS may update itself then."
exit 0
__IMAGE_BELOW__
