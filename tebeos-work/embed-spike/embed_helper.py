# embed_helper.py -- reparent the headful Chromium browser window into the TEBEOS Medula window.
#   embed_helper.py <chromium_pid> <wine_title_substring> [timeout_s]
# Finds the X window of the Chromium pid (_NET_WM_PID, class "chromium", not the 10x10 leader),
# and the top-level X window whose WM_NAME contains the substring, then XReparentWindow.
import ctypes, ctypes.util, os, sys, time
X = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11"))
X.XOpenDisplay.restype = ctypes.c_void_p; X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XDefaultRootWindow.restype = ctypes.c_ulong; X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
X.XInternAtom.restype = ctypes.c_ulong; X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
X.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
X.XReparentWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int]
X.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
X.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
X.XSetErrorHandler.argtypes = [ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)]
ERRS = []
X.XSetErrorHandler(ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(lambda d, e: ERRS.append(1) or 0))
dpy = X.XOpenDisplay(os.environ["DISPLAY"].encode())
root = X.XDefaultRootWindow(dpy)

def prop_values(w, name, typ):
    """Return the list of values of a property (format 8 -> bytes, format 32 -> ints)."""
    a = X.XInternAtom(dpy, name.encode(), 1)
    t = ctypes.c_ulong(); f = ctypes.c_int(); n = ctypes.c_ulong(); after = ctypes.c_ulong()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    X.XGetWindowProperty(dpy, w, a, 0, 4096, 0, typ, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n), ctypes.byref(after), ctypes.byref(data))
    if not n.value:
        return None
    if f.value == 8:
        return ctypes.string_at(ctypes.cast(data, ctypes.c_void_p).value, n.value)
    if f.value == 32:   # Xlib returns 32-bit items as C longs
        arr = ctypes.cast(data, ctypes.POINTER(ctypes.c_long))
        return [arr[i] for i in range(n.value)]
    return None

def children(w):
    r = ctypes.c_ulong(); p = ctypes.c_ulong(); ch = ctypes.POINTER(ctypes.c_ulong)(); nc = ctypes.c_uint()
    X.XQueryTree(dpy, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(nc))
    return [ch[i] for i in range(nc.value)]

def utf8_title(w):
    v = prop_values(w, "_NET_WM_NAME", X.XInternAtom(dpy, b"UTF8_STRING", 0))
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else ""

def wm_name(w):
    name = ctypes.c_char_p()
    X.XFetchName(dpy, w, ctypes.byref(name))
    return name.value.decode("utf-8", "replace") if name.value else ""

def find(pid, title_sub, ctitle=None):
    ctitle = ctitle or title_sub
    chromium = medula = None
    for w in children(root):
        pidv = prop_values(w, "_NET_WM_PID", 6)
        cls = prop_values(w, "WM_CLASS", 31)
        if pidv and pidv[0] == pid and ctitle and ctitle in utf8_title(w):
            # the window showing the page with this title (Chromium sets _NET_WM_NAME in UTF-8)
            chromium = w
        elif pidv and pidv[0] == pid and chromium is None:
            # the browser window's WM_CLASS is "chromium (profile)" or the page URL; the plain
            # "chromium" window is the 10x10 client leader and must not be embedded
            if isinstance(cls, bytes) and cls.split(b"\x00")[0] != b"chromium" and chromium is None:
                chromium = w
        if title_sub and title_sub in wm_name(w):
            medula = w
    return chromium, medula

if __name__ == "__main__":
    pid, title_sub = int(sys.argv[1]), sys.argv[2]
    ctitle = sys.argv[4] if len(sys.argv) > 4 else None   # Chromium page title, e.g. "Medula Eczane"
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 120
    end = time.time() + timeout
    while time.time() < end:
        ch, me = find(pid, title_sub, ctitle)
        if ch and me:
            X.XReparentWindow(dpy, ch, me, 0, 0)
            X.XMapWindow(dpy, ch)
            X.XSync(dpy, 0)
            print("embedded chromium %#x into %#x (%s)" % (ch, me, wm_name(me)), flush=True)
            sys.exit(0)
        time.sleep(0.5)
    print("timeout: chromium=%s medula=%s" % (ch, me), flush=True)
    sys.exit(1)
