#!/bin/bash
# setup.sh -- install the ready-made TEBEOS-on-Linux image on this PC.
#
#   terminal:       ./tebeos-linux-<date>.run --role client --server-ip 192.168.1.101
#   main computer:  ./tebeos-linux-<date>.run --role server --restore tebeos_backup_<date>.zip
#
# (The .run file is this script with the image appended; build-client-image.sh makes it.)
#
# What it puts where (the same layout as the PC it was built on):
#   ~/.local/share/tebeos/wine/<build>   Wine 11.18 staging with the patched mshtml and fonts
#   ~/.wine-tebeos                       the Wine prefix: .NET 4.8, TEBEOS, the CDP shim
#   ~/.local/share/tebeos/ocr-service    the captcha OCR daemon (host tesseract)
#   ~/.local/share/tebeos/embed          embed_run.py: puts Chromium's window inside Medula
#   ~/.local/share/tebeos/start-tebeos.sh + menu and desktop entries "TEBEOS Plus"
# and for the server role also:
#   ~/.wine-tebeos-db                    a small Wine prefix that only runs the database:
#                                        PostgreSQL 12.4 (TEBEOS's own build) in C:\tebeos-db,
#                                        the data in C:\tebeos-db\data, port 5454
#   ~/.local/share/tebeos/tebeos-db.sh   start|stop|status|restart for the database
#   ~/.config/systemd/user/tebeos-db.service  starts the database at boot
#
# Options:
#   --role client|server  client: this PC is a terminal of a main computer.
#                         server: this PC is the main computer ("Ana Bilgisayar"): it runs
#                         the database for itself and the terminals, and TEBEOS itself.
#   --server-ip IP        (client) the TEBEOS main computer (database on TCP 5454)
#   --server-name NAME    (client) its Windows computer name, for [HOSTNAME] (default: looked
#                         up, or IP)
#   --restore FILE        (server) the pharmacy's database: a TEBEOS backup ("Yedek Al",
#                         tebeos_backup_*.zip) or a plain tebeos_db.sql
#   --allow CIDR[,CIDR]   (server) networks whose terminals may use the database (default:
#                         this PC's own networks, e.g. 192.168.1.0/24)
#   --no-service          (server) don't install the systemd service; start the database
#                         with ~/.local/share/tebeos/tebeos-db.sh start
#   --replace             an existing ~/.wine-tebeos (and ~/.wine-tebeos-db) is moved aside
#                         (.bak-DATE) instead of stopping the install
#   --check               only check this PC's requirements
set -euo pipefail

ROLE="" SERVER_IP="" SERVER_NAME="" RESTORE="" ALLOW="" SERVICE=1 REPLACE=0 CHECK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --role) ROLE="${2:-}"; shift ;;
        --server-ip) SERVER_IP="${2:-}"; shift ;;
        --server-name) SERVER_NAME="${2:-}"; shift ;;
        --restore) RESTORE="${2:-}"; shift ;;
        --allow) ALLOW="${2:-}"; shift ;;
        --no-service) SERVICE=0 ;;
        --replace) REPLACE=1 ;;
        --check) CHECK=1 ;;
        -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

SELF="$(readlink -f "$0")"
SHARE="$HOME/.local/share/tebeos"
PFX="$HOME/.wine-tebeos"
DBPFX="$HOME/.wine-tebeos-db"
STAMP="$(date +%Y%m%d-%H%M%S)"

if [ -t 1 ]; then B=$'\e[1m' G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' N=$'\e[0m'; else B= G= Y= R= N=; fi
say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()   { printf '    %s[ ok ]%s %s\n' "$G" "$N" "$*"; }
warn() { printf '    %s[warn]%s %s\n' "$Y" "$N" "$*"; }
die()  { printf '    %s[FAIL]%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

port_open() { timeout 3 bash -c "cat </dev/null >/dev/tcp/$1/$2" 2>/dev/null; }

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
    local miss=0 need=8
    [ "$ROLE" = server ] && need=10
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
    [ "$free" -ge "$need" ] && ok "disk: ${free} GB free in $HOME" || { warn "only ${free} GB free in $HOME ($need needed)"; return 1; }
}

if [ "$CHECK" = 1 ]; then check_requirements; exit $?; fi

# ------------------------------------------------------------------------ options --
case "$ROLE" in
    client)
        [ -n "$SERVER_IP" ] || { [ -t 0 ] && read -rp "    IP address of the TEBEOS main computer: " SERVER_IP; }
        case "$SERVER_IP" in ''|*[!0-9.]*) die "'$SERVER_IP' is not an IPv4 address (--server-ip)";; esac
        ;;
    server)
        [ -n "$RESTORE" ] || die "give --restore with the pharmacy's TEBEOS backup (tebeos_backup_*.zip or tebeos_db.sql)"
        [ -f "$RESTORE" ] || die "backup not found: $RESTORE"
        RESTORE="$(readlink -f "$RESTORE")"
        case "$RESTORE" in
            *.zip) python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.exit(0 if any(n.lower().endswith(".sql") for n in z.namelist()) else 1)' "$RESTORE" \
                       || die "$RESTORE holds no .sql file: is it a TEBEOS backup?" ;;
            *.sql) ;;
            *) die "--restore takes a .zip (TEBEOS backup) or a .sql file" ;;
        esac
        port_open 127.0.0.1 5454 && die "something already listens on port 5454 on this PC (another TEBEOS database?): stop it first"
        ;;
    *) die "give --role client or --role server" ;;
esac

check_requirements || die "requirements missing (see above)"

for p in "$PFX" $([ "$ROLE" = server ] && echo "$DBPFX"); do
    if [ -e "$p" ]; then
        [ "$REPLACE" = 1 ] || die "$p already exists. Run again with --replace to move it aside to $p.bak-$STAMP"
    fi
done
if [ -e "$PFX" ]; then
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
WINE="$SHARE/wine/$WINE_BUILD/bin/wine"

# ------------------------------------------------------------ the prefix's user --
# The prefix was made by $BUILT_BY; Wine keeps the profile under C:\users\<name>.
if [ "$BUILT_BY" != "$USER" ] && [ -d "$PFX/drive_c/users/$BUILT_BY" ]; then
    rm -rf "$PFX/drive_c/users/$USER"
    mv "$PFX/drive_c/users/$BUILT_BY" "$PFX/drive_c/users/$USER"
    sed -i "s|\"C:\\\\\\\\users\\\\\\\\$BUILT_BY\"|\"C:\\\\\\\\users\\\\\\\\$USER\"|" "$PFX/system.reg"
    ok "Windows user folder: C:\\users\\$USER"
fi

# --------------------------------------------------------------------- settings --
APP="$PFX/drive_c/Program Files (x86)/TebeosPlusClient"
if [ "$ROLE" = client ]; then
    step "Settings: terminal of $SERVER_IP"
    DB_HOST="$SERVER_IP"
    name="$SERVER_NAME"
    [ -n "$name" ] || name=$(getent hosts "$SERVER_IP" 2>/dev/null | awk '{print $2; exit}') || true
    [ -n "$name" ] || name="$SERVER_IP"
    IS_CLIENT=TRUE
else
    step "Settings: main computer"
    # The main computer reaches its own database on 127.0.0.1 and stays CLIENT=FALSE: with
    # TRUE, TEBEOS treats itself as a terminal and rewrites <Host> to the LAN address.
    DB_HOST=127.0.0.1
    name=$(hostname 2>/dev/null || cat /etc/hostname 2>/dev/null || echo localhost)
    IS_CLIENT=FALSE
fi
sed -i "s|<Host>[^<]*</Host>|<Host>$DB_HOST</Host>|" "$APP/tebeos_plus.xml"
grep -q "<Host>$DB_HOST</Host>" "$APP/tebeos_plus.xml" || die "could not set <Host> in tebeos_plus.xml"
ok "tebeos_plus.xml: database at $DB_HOST:5454"
# The installer's Ayarlar.ini holds the vendor's defaults; only the connection sections are
# replaced. The section is "CONNECTİON" (dotted İ); Wine may store it as UTF-8 or CP1254
# (the installer's copy is UTF-8), so any of those goes and both are written.
# [BAGLANTI] CLIENT=TRUE: this PC is a terminal; FALSE: it is the main computer.
INI="$APP/app/Ayarlar.ini"
touch "$INI"
LC_ALL=C awk '
    /^\[/ { h=$0; sub(/\r$/,"",h); skip = (h ~ /^\[CONNECT.*ON\]$/ || h == "[BAGLANTI]" || h == "[HOSTNAME]") }
    { if (!skip) print }
' "$INI" >"$INI.new"
{
    printf '[CONNECT\304\260ON]\r\nCLIENT=%s\r\n' "$DB_HOST"
    printf '[CONNECT\335ON]\r\nCLIENT=%s\r\n' "$DB_HOST"
    printf '[BAGLANTI]\r\nCLIENT=%s\r\n' "$IS_CLIENT"
    printf '[HOSTNAME]\r\nANABILGISAYAR=%s\r\n' "$name"
} >>"$INI.new"
mv "$INI.new" "$INI"
if [ "$ROLE" = client ]; then ok "Ayarlar.ini: terminal, main computer $name"; else ok "Ayarlar.ini: main computer ($name)"; fi

# --------------------------------------------------------------------- database --
# Server role only. PostgreSQL runs in a prefix of its own, so TEBEOS (or a wineserver -k)
# on this PC can't take the terminals' database down with it. It is TEBEOS's own build
# (12.4, from the app folder), because TEBEOS backs up with its pg_dump.exe, which only
# accepts a server of its own version. Windows PostgreSQL refuses to run as an administrator
# (which every Wine user is) unless started through pg_ctl, so pg_ctl starts and stops it.
db_tool() {   # db_tool <exe> [args...]: one of PostgreSQL's tools in the database prefix
    local exe="$1"; shift
    ( cd "$DBPFX/drive_c/tebeos-db" && \
      env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX="$DBPFX" WINEARCH=win64 WINEDEBUG=-all \
          WINEDLLOVERRIDES="mscoree,mshtml=" LC_ALL=tr_TR.UTF-8 \
          "$WINE" "pgsql/bin/$exe.exe" "$@" </dev/null )
}
db_log() {   # PostgreSQL's messages are Turkish: UTF-8 or CP1254, depending on who wrote them
    if iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1; then cat "$1"; else iconv -f CP1254 -t UTF-8 "$1"; fi
}

local_networks() {   # this PC's IPv4 networks, as CIDRs
    ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | python3 -c '
import ipaddress, sys
print(",".join(sorted({str(ipaddress.ip_interface(l.strip()).network) for l in sys.stdin if l.strip()})))'
}

setup_database() {
    local D="$DBPFX/drive_c/tebeos-db" log="$DBPFX/drive_c/tebeos-db/setup.log" sql n cidr tz

    step "Database: a Wine prefix of its own ($DBPFX)"
    if [ -e "$DBPFX" ]; then
        mv "$DBPFX" "$DBPFX.bak-$STAMP"
        ok "old database prefix (and its data) kept as $DBPFX.bak-$STAMP"
    fi
    env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX="$DBPFX" WINEARCH=win64 WINEDEBUG=-all \
        WINEDLLOVERRIDES="mscoree,mshtml=" "$WINE" wineboot -i >/dev/null 2>&1 </dev/null \
        || die "could not create the Wine prefix $DBPFX"
    WINEPREFIX="$DBPFX" "${WINE}server" -w
    mkdir -p "$D"
    cp -a "$APP/pgsql" "$D/pgsql"
    rm -rf "$D/pgsql/include" "$D/pgsql/StackBuilder" "$D/pgsql/symbols"
    [ -f "$D/pgsql/bin/postgres.exe" ] || die "PostgreSQL is missing from TEBEOS's folder ($APP/pgsql)"
    ok "PostgreSQL copied from TEBEOS's folder"

    step "Database: new cluster (the vendor's parameters: UTF-8, user tebeos)"
    db_tool initdb --encoding=UTF-8 --username=tebeos --pgdata 'C:\tebeos-db\data' >"$log" 2>&1 \
        || { db_log "$log" | tail -5; die "initdb failed (log: $log)"; }
    # Wine gives PostgreSQL no usable time zone (it picks GMT); TEBEOS's times are Turkish.
    tz=$(timedatectl show -p Timezone --value 2>/dev/null || true)
    case "$tz" in ''|UTC|Etc/*) tz=Europe/Istanbul ;; esac
    cat >>"$D/data/postgresql.conf" <<EOF

# --- TEBEOS on Linux (setup.sh) ---
port = 5454
listen_addresses = '*'
timezone = '$tz'
log_timezone = '$tz'
EOF
    # initdb's pg_hba.conf lets this PC in; add the terminals' networks.
    cidr="${ALLOW:-$(local_networks)}"
    {
        printf '\n# TEBEOS terminals (setup.sh --allow)\n'
        for n in ${cidr//,/ }; do
            python3 -c 'import ipaddress, sys; ipaddress.ip_network(sys.argv[1])' "$n" 2>/dev/null \
                || die "'$n' is not a network (--allow 192.168.1.0/24)"
            printf 'host    all    tebeos    %s    trust\n' "$n"
        done
    } >>"$D/data/pg_hba.conf"
    ok "port 5454, time zone $tz, terminals from: ${cidr:-(none: only this PC)}"

    step "Database: restoring $(basename "$RESTORE")"
    mkdir -p "$D/restore"
    if [[ "$RESTORE" == *.zip ]]; then
        python3 - "$RESTORE" "$D/restore/tebeos_db.sql" <<'PY' || die "could not unpack $RESTORE"
import shutil, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = [n for n in z.namelist() if n.lower().endswith(".sql")]
name = next((n for n in names if n.lower().endswith("tebeos_db.sql")), names[0])
with z.open(name) as src, open(sys.argv[2], "wb") as dst:
    shutil.copyfileobj(src, dst, 1 << 20)
PY
    else
        cp "$RESTORE" "$D/restore/tebeos_db.sql"
    fi
    ok "$(du -h "$D/restore/tebeos_db.sql" | cut -f1) of SQL"
    db_tool pg_ctl -D 'C:\tebeos-db\data' -l 'C:\tebeos-db\postgres.log' -w -t 120 start >>"$log" 2>&1 \
        || { db_log "$D/postgres.log" | tail -5; die "the database did not start (log: $D/postgres.log)"; }
    # The same steps as TEBEOS's own restore (Geri Yükle), but stopping at the first error.
    db_tool createdb -h 127.0.0.1 -p 5454 -E UTF8 -U tebeos tebeos_db >>"$log" 2>&1 \
        || { db_tool pg_ctl -D 'C:\tebeos-db\data' -m fast -w stop >/dev/null 2>&1; die "createdb failed (log: $log)"; }
    say "    loading (large backups take several minutes) ..."
    if ! db_tool psql -h 127.0.0.1 -p 5454 -U tebeos -d tebeos_db -q -v ON_ERROR_STOP=1 \
            --single-transaction -o 'C:\tebeos-db\restore\psql.out' -f 'C:\tebeos-db\restore\tebeos_db.sql' >>"$log" 2>&1; then
        db_log "$log" | grep -iE 'error|hata' | tail -5
        db_tool pg_ctl -D 'C:\tebeos-db\data' -m fast -w stop >/dev/null 2>&1
        die "the backup could not be loaded (the backup file itself is untouched; log: $log)"
    fi
    n=$(db_tool psql -h 127.0.0.1 -p 5454 -U tebeos -d tebeos_db -At -c \
        "select count(*) from information_schema.tables where table_schema not in ('pg_catalog','information_schema')" 2>/dev/null | tr -dc 0-9) || true
    db_tool pg_ctl -D 'C:\tebeos-db\data' -m fast -w stop >>"$log" 2>&1 || true
    WINEPREFIX="$DBPFX" "${WINE}server" -w
    rm -rf "$D/restore"
    [ "${n:-0}" -gt 0 ] || die "the backup loaded, but tebeos_db has no tables: wrong file?"
    ok "tebeos_db: $n tables"

    step "Database: start/stop script and service"
    cat >"$SHARE/tebeos-db.sh" <<EOF
#!/bin/bash
# tebeos-db.sh start|stop|restart|status -- the TEBEOS database (PostgreSQL 12.4 under Wine,
# port 5454) of this main computer. Installed by the TEBEOS-on-Linux image (setup.sh).
# Data: $D/data   Log: $D/postgres.log
export WINEPREFIX="$DBPFX" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export LC_ALL=tr_TR.UTF-8
unset DISPLAY WAYLAND_DISPLAY
WINE="\$(ls -d "$SHARE"/wine/wine-*/bin/wine 2>/dev/null | grep -v '\.bak-' | head -1)"
[ -x "\$WINE" ] || { echo "Wine not found under $SHARE/wine" >&2; exit 1; }
cd "$D" || exit 1
up() { timeout 3 bash -c 'cat </dev/null >/dev/tcp/127.0.0.1/5454' 2>/dev/null; }
pgctl() { "\$WINE" pgsql/bin/pg_ctl.exe -D 'C:\\tebeos-db\\data' "\$@" </dev/null; }
case "\${1:-status}" in
    start)   up && { echo "running"; exit 0; }
             pgctl -l 'C:\\tebeos-db\\postgres.log' -w -t 120 start >/dev/null 2>&1 && up && echo "started" ;;
    stop)    pgctl -m fast -w -t 120 stop >/dev/null 2>&1; "\${WINE}server" -w; up && exit 1; echo "stopped" ;;
    restart) "\$0" stop; "\$0" start ;;
    status)  if up; then echo "running (port 5454)"; else echo "stopped"; exit 3; fi ;;
    *)       echo "usage: \$0 start|stop|restart|status" >&2; exit 2 ;;
esac
EOF
    chmod +x "$SHARE/tebeos-db.sh"
    ok "$SHARE/tebeos-db.sh"

    if [ "$SERVICE" = 1 ] && systemctl --user show-environment >/dev/null 2>&1; then
        mkdir -p "$HOME/.config/systemd/user"
        cat >"$HOME/.config/systemd/user/tebeos-db.service" <<EOF
[Unit]
Description=TEBEOS database (PostgreSQL 12.4 under Wine, port 5454)

[Service]
# pg_ctl starts the server and returns; the server keeps running in this unit.
Type=oneshot
RemainAfterExit=yes
ExecStart=$SHARE/tebeos-db.sh start
ExecStop=$SHARE/tebeos-db.sh stop
TimeoutStartSec=180
TimeoutStopSec=180

[Install]
WantedBy=default.target
EOF
        systemctl --user daemon-reload
        systemctl --user enable --now tebeos-db.service >/dev/null 2>&1 \
            || die "the service did not start: systemctl --user status tebeos-db; log: $D/postgres.log"
        ok "systemd user service tebeos-db: enabled and started"
        # Without lingering the service only runs while this user is logged in.
        if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" = yes ] \
            || loginctl enable-linger "$USER" 2>/dev/null; then
            ok "the database starts at boot, before anyone logs in"
        else
            warn "the database only runs while $USER is logged in. To start it at boot:"
            warn "  sudo loginctl enable-linger $USER"
        fi
    else
        "$SHARE/tebeos-db.sh" start >/dev/null || die "the database did not start (log: $D/postgres.log)"
        warn "no systemd service: start the database after each boot with $SHARE/tebeos-db.sh start"
        warn "(start-tebeos.sh also starts it when TEBEOS is opened on this PC)"
    fi
}
[ "$ROLE" = server ] && setup_database

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
# main computer: its database must be up first (normally the tebeos-db service already runs it)
if [ -x "$SHARE/tebeos-db.sh" ] && ! "$SHARE/tebeos-db.sh" status >/dev/null 2>&1; then
    systemctl --user start tebeos-db.service 2>/dev/null || "$SHARE/tebeos-db.sh" start
fi
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
if [ "$ROLE" = client ]; then comment="TEBEOS eczane programı (ana bilgisayar: $SERVER_IP)"
else comment="TEBEOS eczane programı (ana bilgisayar)"; fi
entry="[Desktop Entry]
Type=Application
Name=TEBEOS Plus
Comment=$comment
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
if [ "$ROLE" = client ]; then
    step "Checking the main computer"
    if port_open "$SERVER_IP" 5454; then
        ok "database reachable at $SERVER_IP:5454"
    else
        warn "cannot reach $SERVER_IP:5454: is the main computer on, and TCP 5454 allowed in its firewall?"
    fi
else
    step "Checking the database"
    port_open 127.0.0.1 5454 && ok "database running on port 5454" || warn "the database is not running: $SHARE/tebeos-db.sh start"
    if systemctl is-active -q firewalld 2>/dev/null; then
        warn "firewalld is on: let the terminals in with"
        warn "  sudo firewall-cmd --permanent --add-port=5454/tcp && sudo firewall-cmd --reload"
    elif command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
        warn "ufw is on: let the terminals in with  sudo ufw allow from <network> to any port 5454 proto tcp"
    fi
    ips=$(ip -4 -o addr show scope global 2>/dev/null | awk '{sub(/\/.*/, "", $4); print $4}' | paste -sd' ')
    say ""
    say "    Terminals connect to this PC at: ${ips:-<IP of this PC>} (port 5454)."
    say "    Give it a fixed IP (DHCP reservation in the router), then on each Linux terminal:"
    say "      ./tebeos-linux-<date>.run --role client --server-ip ${ips%% *}"
    say "    Windows terminals: set the main computer's IP in TEBEOS's terminal settings."
fi

step "Done"
say "    Start TEBEOS from the menu or the desktop icon \"TEBEOS Plus\","
say "    or: $SHARE/start-tebeos.sh"
say "    The first start logs in as usual; TEBEOS may update itself then."
exit 0
__IMAGE_BELOW__
