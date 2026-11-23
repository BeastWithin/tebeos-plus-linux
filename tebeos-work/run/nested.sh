#!/bin/bash
# nested.sh -- a private headless display for running TEBEOS without touching the user's desktop:
# a virtual KWin (own D-Bus) with a full-root Xwayland on :5. Stop with: kill $(cat nested.pid)
cd "$(dirname "$0")"
exec dbus-run-session -- bash -c '
  kwin_wayland --virtual --width 1600 --height 1000 --socket wayland-tebeos \
      --no-lockscreen --no-global-shortcuts > kwin.log 2>&1 &
  for i in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/wayland-tebeos" ] && break; sleep 0.2; done
  WAYLAND_DISPLAY=wayland-tebeos exec Xwayland :5 -geometry 1600x1000 -nolisten tcp > xwayland.log 2>&1'
