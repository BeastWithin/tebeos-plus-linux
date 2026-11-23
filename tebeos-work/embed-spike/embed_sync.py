# embed_sync.py -- keep the embedded Chromium window sized to the Medula content area and repainted.
#   embed_sync.py <medula_hex> <chromium_hex> [seconds] [top_offset] [bottom_offset]
# Every 0.5 s: read the Medula window size (XGetGeometry); when it changes, resize the Chromium child to
# (0, top) .. (w, h - bottom) and sends one expose after each change. No periodic expose (it blinks).
import ctypes, sys, time, embed_helper as e
X = e.X
X.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
X.XMoveResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
X.XFlush.argtypes = [ctypes.c_void_p]
X.XClearArea.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_int]
medula, chromium = int(sys.argv[1], 16), int(sys.argv[2], 16)
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 3600
top = int(sys.argv[4]) if len(sys.argv) > 4 else 33
bottom = int(sys.argv[5]) if len(sys.argv) > 5 else 55
end = time.time() + seconds
last = None
while time.time() < end:
    r, x, y = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_int()
    w, h, bw, dep = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
    if X.XGetGeometry(e.dpy, medula, ctypes.byref(r), ctypes.byref(x), ctypes.byref(y), ctypes.byref(w), ctypes.byref(h), ctypes.byref(bw), ctypes.byref(dep)):
        size = (w.value, h.value)
        if size != last and size[0] > 0 and size[1] > top + bottom:
            X.XMoveResizeWindow(e.dpy, chromium, 0, top, size[0], size[1] - top - bottom)
            X.XClearArea(e.dpy, chromium, 0, 0, 0, 0, 1)   # repaint only after a resize: a periodic
            X.XFlush(e.dpy)                                # expose made the page blink every 0.5 s
            last = size
    time.sleep(0.5)
