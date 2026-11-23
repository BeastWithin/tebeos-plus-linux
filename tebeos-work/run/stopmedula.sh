#!/bin/bash
# stopmedula.sh -- stop the app, its wineserver and the shim's Chromium (the OCR daemon stays)
export WINEPREFIX="$HOME/.wine-tebeos"
"$HOME/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wineserver" -k 2>/dev/null
sleep 1
pkill -f "[u]ser-data-dir=/tmp/tebeos-cdp-profile" 2>/dev/null
echo stopped
