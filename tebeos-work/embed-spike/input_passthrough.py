# input_passthrough.py -- make the embedded Chromium window ignore input (empty X input shape), so
# clicks and keys fall through to the Medula window, where the shim handles them as in GDI mode.
#   input_passthrough.py <chromium_hex> [off]     ("off" restores normal input)
import ctypes, ctypes.util, sys, embed_helper as e
Xext = ctypes.cdll.LoadLibrary(ctypes.util.find_library("Xext"))
Xext.XShapeCombineRectangles.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                         ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
Xext.XShapeCombineMask.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_ulong, ctypes.c_int]
e.X.XFlush.argtypes = [ctypes.c_void_p]
SHAPE_INPUT, SHAPE_SET = 2, 0
win = int(sys.argv[1], 16)
if len(sys.argv) > 2 and sys.argv[2] == "off":
    Xext.XShapeCombineMask(e.dpy, win, SHAPE_INPUT, 0, 0, 0, SHAPE_SET)   # None mask = default shape
    print("input restored")
else:
    Xext.XShapeCombineRectangles(e.dpy, win, SHAPE_INPUT, 0, 0, None, 0, SHAPE_SET, 0)
    print("input passes through")
e.X.XFlush(e.dpy)
