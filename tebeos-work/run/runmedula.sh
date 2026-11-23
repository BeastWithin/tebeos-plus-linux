#!/bin/bash
# runmedula.sh -- start the OCR daemon (if not running) and tebeos_medula.exe on the private
# display :5 (nested.sh). Logs in run/. Stop with: run/stopmedula.sh
cd "$(dirname "$0")"
RUN=$(pwd)
if ! pgrep -f "[o]cr_service.py" >/dev/null; then
  (cd ../ocr-service && setsid nohup python3 ocr_service.py >> service.log 2>&1 < /dev/null &)
fi
export WINEPREFIX="$HOME/.wine-tebeos" WINEARCH=win64 WINEDEBUG="-all" WINEDLLOVERRIDES="mscoree=n" LC_ALL="tr_TR.UTF-8"
export DISPLAY=:5
unset WAYLAND_DISPLAY
WINE="$HOME/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wine"
: > "$WINEPREFIX/drive_c/users/$USER/AppData/Local/Temp/mshtml-cdp.log"
cd "$WINEPREFIX/drive_c/Program Files (x86)/TebeosPlusClient/app" || exit 1
# the TEBEOS login lives in run/login.env (git-ignored): TEBEOS_USER=... TEBEOS_PASS='...'
. "$RUN/login.env" || { echo "run/login.env missing"; exit 1; }
setsid nohup "$WINE" tebeos_medula.exe "$TEBEOS_USER" "$TEBEOS_PASS" > "$RUN/medula.log" 2>&1 < /dev/null &
echo $! > "$RUN/medula.pid"
echo started
