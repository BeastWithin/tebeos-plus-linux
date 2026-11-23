import ctypes, sys, os
from xtool import X, dpy
ERR = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
def handler(d, ev):
    # XErrorEvent: type(int) display(ptr) resourceid(ulong) serial(ulong) error_code(uchar) request_code(uchar) minor_code(uchar)
    e = ctypes.cast(ev, ctypes.POINTER(ctypes.c_int * 2)); code = ctypes.cast(ev, ctypes.POINTER(ctypes.c_ubyte))
    base = 32 + 2*8 + 8 + 0
    print("XERROR code=%d req=%d" % (ctypes.c_ubyte.from_address(ev + 32 + 8 + 8 + 8 + 8 - 8 + 0).value if False else 0, 0), flush=True)
    return 0
cb = ERR(handler)
X.XSetErrorHandler.argtypes = [ERR]
X.XSetErrorHandler(cb)
child, parent = int(sys.argv[1], 16), int(sys.argv[2], 16)
X.XReparentWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int]
X.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
X.XReparentWindow(dpy, child, parent, 0, 0)
X.XMapWindow(dpy, child)
X.XSync(dpy, 0)
print("done", flush=True)
