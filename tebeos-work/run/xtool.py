#!/usr/bin/env python3
"""xtool.py -- screenshot / click / type on a private X display (the nested KWin), never on
the user's own session.  DISPLAY must be set.

  xtool.py shot OUT.png
  xtool.py click X Y [hold_seconds]
  xtool.py key KEYSYM_NAME
  xtool.py windows            (top-level windows with geometry)
"""
import os, sys, time
from Xlib import X, display, XK
from Xlib.ext import xtest

d = display.Display()
root = d.screen().root
cmd = sys.argv[1]
if cmd == "shot":
    from PIL import Image
    g = root.get_geometry()
    raw = root.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    Image.frombytes("RGB", (g.width, g.height), raw.data, "raw", "BGRX").save(sys.argv[2])
    print("shot %dx%d" % (g.width, g.height))
elif cmd == "click":
    x, y = int(sys.argv[2]), int(sys.argv[3])
    hold = float(sys.argv[4]) if len(sys.argv) > 4 else 0.08
    xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync(); time.sleep(0.1)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(hold)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
    print("clicked", x, y)
elif cmd == "key":
    kc = d.keysym_to_keycode(XK.string_to_keysym(sys.argv[2]))
    xtest.fake_input(d, X.KeyPress, kc); d.sync(); time.sleep(0.05)
    xtest.fake_input(d, X.KeyRelease, kc); d.sync()
    print("key", sys.argv[2])
elif cmd == "windows":
    def walk(w, depth=0):
        for c in w.query_tree().children:
            try:
                a = c.get_attributes()
                if a.map_state != X.IsViewable:
                    continue
                g = c.get_geometry()
                name = c.get_wm_name() or ""
                if name or depth == 0:
                    print("%s0x%x %dx%d+%d+%d %r" % ("  " * depth, c.id, g.width, g.height, g.x, g.y, name))
                if depth < 1:
                    walk(c, depth + 1)
            except Exception:
                pass
    walk(root)
