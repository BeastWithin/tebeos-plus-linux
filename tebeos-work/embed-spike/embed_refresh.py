# embed_refresh.py -- keep an embedded Chromium window repainted inside the Medula window.
#   embed_refresh.py <chromium_xwin_hex> [seconds] [interval_s]
# Sends an X expose (XClearArea with exposures) to the embedded window at an interval.
# Finding (2026-11-18): after Medula repaints its client area, the embedded window stops being
# redrawn until it is re-parented or exposed; one expose restores it without moving the window.
import ctypes, sys, time, embed_helper as e
e.X.XClearArea.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_int]
win = int(sys.argv[1], 16)
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 600
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
end = time.time() + seconds
while time.time() < end:
    e.X.XClearArea(e.dpy, win, 0, 0, 0, 0, 1)
    e.X.XFlush(e.dpy)
    time.sleep(interval)
