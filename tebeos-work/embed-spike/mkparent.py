# mkparent.py: create a plain X parent window (stand-in for Wine's whole_window) and print its id
import ctypes, os, time
from xtool import X, dpy, root
X.XCreateSimpleWindow.restype = ctypes.c_ulong
X.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
X.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XFlush.argtypes = [ctypes.c_void_p]
w = X.XCreateSimpleWindow(dpy, root, 0, 0, 800, 600, 0, 0, 0x335577)
X.XMapWindow(dpy, w); X.XFlush(dpy)
print(hex(w), flush=True)
time.sleep(600)
