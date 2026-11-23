#!/bin/bash
# shimctl.sh -- switch the Wine prefix's HTMLDocument between Wine's mshtml and the CDP shim.
#   shimctl.sh install   copy shim32/64.dll in as mshtml_cdp.dll and point CLSID_HTMLDocument at it
#   shimctl.sh revert    point CLSID_HTMLDocument back at Wine's mshtml.dll
#   shimctl.sh status
# Only the HTMLDocument class moves; about:/res: protocols etc. stay in Wine's mshtml.
set -euo pipefail
WINE="$HOME/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wine"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-tebeos}" WINEDEBUG=-all
SHIM="$(cd "$(dirname "$0")/../shim-dll" && pwd)"
CLSID='{25336920-03F9-11CF-8FD0-00AA00686F13}'
K32="HKLM\\Software\\Classes\\Wow6432Node\\CLSID\\$CLSID\\InprocServer32"
K64="HKLM\\Software\\Classes\\CLSID\\$CLSID\\InprocServer32"
C="$WINEPREFIX/drive_c/windows"

status () {
  "$WINE" reg query "$K32" /ve 2>/dev/null | tr -d '\r' | grep REG_SZ
  "$WINE" reg query "$K64" /ve 2>/dev/null | tr -d '\r' | grep REG_SZ
}
case "${1:-status}" in
  install)
    cp "$SHIM/shim32.dll" "$C/syswow64/mshtml_cdp.dll"
    cp "$SHIM/shim64.dll" "$C/system32/mshtml_cdp.dll"
    "$WINE" reg add "$K32" /ve /t REG_SZ /d 'C:\windows\syswow64\mshtml_cdp.dll' /f >/dev/null
    "$WINE" reg add "$K64" /ve /t REG_SZ /d 'C:\windows\system32\mshtml_cdp.dll' /f >/dev/null
    status ;;
  revert)
    "$WINE" reg add "$K32" /ve /t REG_SZ /d 'C:\windows\syswow64\mshtml.dll' /f >/dev/null
    "$WINE" reg add "$K64" /ve /t REG_SZ /d 'C:\windows\system32\mshtml.dll' /f >/dev/null
    status ;;
  status) status ;;
  *) echo "usage: $0 install|revert|status"; exit 2 ;;
esac
