# xtool.py: list top-level X windows; reparent one window under another (Xlib via ctypes)
import ctypes, ctypes.util, sys, os
X = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11"))
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XInternAtom.restype = ctypes.c_ulong
X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
X.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
X.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
dpy = X.XOpenDisplay(os.environ["DISPLAY"].encode())
root = X.XDefaultRootWindow(ctypes.c_void_p(dpy)) if hasattr(X, "XDefaultRootWindow") else None
X.XDefaultRootWindow.restype = ctypes.c_ulong
X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
root = X.XDefaultRootWindow(dpy)
def prop(w, name, typ=0):
    a = X.XInternAtom(dpy, name.encode(), 1)
    t = ctypes.c_ulong(); f = ctypes.c_int(); n = ctypes.c_ulong(); after = ctypes.c_ulong(); data = ctypes.POINTER(ctypes.c_ubyte)()
    X.XGetWindowProperty(dpy, w, a, 0, 1024, 0, typ, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n), ctypes.byref(after), ctypes.byref(data))
    if not n.value: return None
    raw = ctypes.cast(data, ctypes.c_void_p).value
    return ctypes.string_at(raw, n.value * (f.value // 8)) if f.value == 8 else ctypes.cast(data, ctypes.POINTER(ctypes.c_uint32))[0] if n.value else None
def children(w):
    r = ctypes.c_ulong(); p = ctypes.c_ulong(); ch = ctypes.POINTER(ctypes.c_ulong)(); nc = ctypes.c_uint()
    X.XQueryTree(dpy, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(nc))
    return [ch[i] for i in range(nc.value)]
if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "list":
        for w in children(root):
            wm = prop(w, "WM_CLASS", 31)
            name = ctypes.c_char_p(); X.XFetchName(dpy, w, ctypes.byref(name))
            print(hex(w), "class=", wm, "name=", name.value)
    elif cmd == "reparent":
        child, parent = int(sys.argv[2], 16), int(sys.argv[3], 16)
        X.XReparentWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int]
        X.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        X.XReparentWindow(dpy, child, parent, 0, 0); X.XMapWindow(dpy, child); X.XFlush(dpy)
        print("reparented", hex(child), "under", hex(parent))
