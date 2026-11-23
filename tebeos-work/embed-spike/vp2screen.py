# vp2screen.py -- screen coordinates of a viewport point in the embedded Chromium window.
#   vp2screen.py <chromium_hex> <vx> <vy> <outerW> <outerH> <innerW> <innerH>
import ctypes, sys, embed_helper as e
X = e.X
X.XTranslateCoordinates.restype = ctypes.c_int
X.XTranslateCoordinates.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong)]
win = int(sys.argv[1], 16); vx, vy, ow, oh, iw, ih = map(int, sys.argv[2:8])
rx, ry, ch = ctypes.c_int(), ctypes.c_int(), ctypes.c_ulong()
X.XTranslateCoordinates(e.dpy, win, e.root, 0, 0, ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(ch))
side = (ow - iw) // 2              # Chromium's own window border
top = oh - ih - side               # tab strip + toolbar + info bar
print(rx.value + side + vx, ry.value + top + vy)
