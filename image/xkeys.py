#!/usr/bin/env python3
"""xkeys.py DISPLAY KEY... -- type keys into whatever has the focus on an X display (XTEST).

Used by build-client-image.sh to click through the TEBEOS installer on the private display.
KEY is an X keysym name (Return, Tab, space, Down, ...), optionally with modifiers joined by
'+' (alt+k, shift+Tab). 'sleep:SECONDS' pauses between keys; 'click:X,Y' clicks the left button there."""
import sys, time
from Xlib import X, XK, display
from Xlib.ext import xtest

MODS = {"alt": "Alt_L", "shift": "Shift_L", "ctrl": "Control_L"}

def code(d, name):
    ks = XK.string_to_keysym(name)
    if not ks and len(name) == 1:
        ks = ord(name)
    kc = d.keysym_to_keycode(ks)
    if not kc:
        raise SystemExit("unknown key: " + name)
    return kc

def press(d, combo):
    parts = combo.split("+")
    mods = [code(d, MODS[p.lower()]) for p in parts[:-1]]
    key = code(d, parts[-1])
    for m in mods:
        xtest.fake_input(d, X.KeyPress, m)
    xtest.fake_input(d, X.KeyPress, key)
    xtest.fake_input(d, X.KeyRelease, key)
    for m in reversed(mods):
        xtest.fake_input(d, X.KeyRelease, m)
    d.sync()

def main():
    d = display.Display(sys.argv[1])
    for k in sys.argv[2:]:
        if k.startswith("click:"):
            x, y = (int(v) for v in k[6:].split(","))
            xtest.fake_input(d, X.MotionNotify, x=x, y=y)
            xtest.fake_input(d, X.ButtonPress, 1)
            xtest.fake_input(d, X.ButtonRelease, 1)
            d.sync()
            time.sleep(0.4)
            continue
        if k.startswith("sleep:"):
            time.sleep(float(k[6:]))
            continue
        press(d, k)
        time.sleep(0.4)

if __name__ == "__main__":
    main()
