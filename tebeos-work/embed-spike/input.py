import ctypes, ctypes.util, time, os
from xtool import X, dpy
T = ctypes.cdll.LoadLibrary(ctypes.util.find_library("Xtst"))
T.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XStringToKeysym.argtypes = [ctypes.c_char_p]; X.XStringToKeysym.restype = ctypes.c_ulong
X.XFlush.argtypes = [ctypes.c_void_p]
def click(x, y):
    T.XTestFakeMotionEvent(dpy, -1, x, y, 0); X.XFlush(dpy); time.sleep(0.2)
    T.XTestFakeButtonEvent(dpy, 1, 1, 0); X.XFlush(dpy); time.sleep(0.05)
    T.XTestFakeButtonEvent(dpy, 1, 0, 0); X.XFlush(dpy); time.sleep(0.3)
def key(name):
    kc = X.XKeysymToKeycode(dpy, X.XStringToKeysym(name.encode()))
    T.XTestFakeKeyEvent(dpy, kc, 1, 0); X.XFlush(dpy); time.sleep(0.05)
    T.XTestFakeKeyEvent(dpy, kc, 0, 0); X.XFlush(dpy); time.sleep(0.2)
def rclick(x, y, button=3):
    T.XTestFakeMotionEvent(dpy, -1, x, y, 0); X.XFlush(dpy); time.sleep(0.3)
    T.XTestFakeButtonEvent(dpy, button, 1, 0); X.XFlush(dpy); time.sleep(0.08)
    T.XTestFakeButtonEvent(dpy, button, 0, 0); X.XFlush(dpy); time.sleep(1.0)
import sys
if sys.argv[1] == "click": click(int(sys.argv[2]), int(sys.argv[3]))
if sys.argv[1] == "rclick": rclick(int(sys.argv[2]), int(sys.argv[3]))
if sys.argv[1] == "keys":
    for k in sys.argv[2:]: key(k)
