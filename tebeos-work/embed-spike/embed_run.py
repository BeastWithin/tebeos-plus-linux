# embed_run.py -- keep the shim's visible Chromium window embedded over the shim's window.
#
#   embed_run.py <wineprefix> <cdp_port> [seconds]
#
# Design (2026-11-19): Chromium only draws; the shim keeps all input. The shim (page.c, embed mode)
# sizes Chromium's window to its own window through CDP and writes
#   %TEMP%\mshtml-cdp-embed.txt = "<x11 window hex> <screen x> <screen y> <width> <height>"
# This helper, every 0.5 s:
#   - finds the Chromium browser window (the process listening on <cdp_port>),
#   - reparents it into that X window at the shim window's position (moves only; never resizes,
#     so Chromium's idea of its own size stays right),
#   - gives it an empty input shape, so clicks and keys fall through to Medula and the shim.
import ctypes
import ctypes.util
import os
import sys
import time

X = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11"))
Xext = ctypes.cdll.LoadLibrary(ctypes.util.find_library("Xext"))
VP, UL, I, UI = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_uint
X.XOpenDisplay.restype, X.XOpenDisplay.argtypes = VP, [ctypes.c_char_p]
X.XDefaultRootWindow.restype, X.XDefaultRootWindow.argtypes = UL, [VP]
X.XInternAtom.restype, X.XInternAtom.argtypes = UL, [VP, ctypes.c_char_p, I]
X.XQueryTree.argtypes = [VP, UL, ctypes.POINTER(UL), ctypes.POINTER(UL), ctypes.POINTER(ctypes.POINTER(UL)),
                         ctypes.POINTER(UI)]
X.XFree.argtypes = [VP]
X.XGetWindowProperty.argtypes = [VP, UL, UL, ctypes.c_long, ctypes.c_long, I, UL, ctypes.POINTER(UL),
                                 ctypes.POINTER(I), ctypes.POINTER(UL), ctypes.POINTER(UL),
                                 ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
X.XTranslateCoordinates.argtypes = [VP, UL, UL, I, I, ctypes.POINTER(I), ctypes.POINTER(I), ctypes.POINTER(UL)]
X.XReparentWindow.argtypes = [VP, UL, UL, I, I]
X.XClearArea.argtypes = [VP, UL, I, I, UI, UI, I]
X.XSetWindowBackgroundPixmap.argtypes = [VP, UL, UL]
X.XFlush.argtypes = [VP]
X.XQueryPointer.argtypes = [VP, UL, ctypes.POINTER(UL), ctypes.POINTER(UL), ctypes.POINTER(I), ctypes.POINTER(I),
                            ctypes.POINTER(I), ctypes.POINTER(I), ctypes.POINTER(UI)]
X.XKeysymToKeycode.restype = ctypes.c_ubyte
X.XKeysymToKeycode.argtypes = [VP, UL]
X.XSendEvent.argtypes = [VP, UL, I, ctypes.c_long, VP]


class WinAttrs(ctypes.Structure):
    _fields_ = [("x", I), ("y", I), ("width", I), ("height", I), ("border_width", I), ("depth", I),
                ("visual", VP), ("root", UL), ("class_", I), ("bit_gravity", I), ("win_gravity", I),
                ("backing_store", I), ("backing_planes", UL), ("backing_pixel", UL), ("save_under", I),
                ("colormap", UL), ("map_installed", I), ("map_state", I), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                ("override_redirect", I), ("screen", VP)]


X.XGetWindowAttributes.argtypes = [VP, UL, ctypes.POINTER(WinAttrs)]


class XKeyEvent(ctypes.Structure):
    _fields_ = [("type", I), ("serial", UL), ("send_event", I), ("display", VP), ("window", UL), ("root", UL),
                ("subwindow", UL), ("time", UL), ("x", I), ("y", I), ("x_root", I), ("y_root", I),
                ("state", UI), ("keycode", UI), ("same_screen", I)]


class XEventPad(ctypes.Union):           # XEvent is a 24-long union; XSendEvent reads that much
    _fields_ = [("key", XKeyEvent), ("pad", ctypes.c_long * 24)]
X.XMoveWindow.argtypes = [VP, UL, I, I]
X.XMapWindow.argtypes = [VP, UL]
X.XSync.argtypes = [VP, I]
Xext.XShapeCombineRectangles.argtypes = [VP, UL, I, I, I, VP, I, I, I]
Xext.XShapeGetRectangles.restype = VP
Xext.XShapeGetRectangles.argtypes = [VP, UL, I, ctypes.POINTER(I), ctypes.POINTER(I)]
ERRORS = []
ERR = ctypes.CFUNCTYPE(I, VP, VP)
_err_cb = ERR(lambda d, ev: ERRORS.append(1) or 0)   # a window that vanished must not kill the helper
X.XSetErrorHandler.argtypes = [ERR]
X.XSetErrorHandler(_err_cb)

SHAPE_INPUT, SHAPE_SET = 2, 0
dpy = X.XOpenDisplay(os.environ.get("DISPLAY", ":0").encode())
if not dpy:
    sys.exit("cannot open the X display")
root = X.XDefaultRootWindow(dpy)


def children(w):
    r, p, ch, n = UL(), UL(), ctypes.POINTER(UL)(), UI()
    if not X.XQueryTree(dpy, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(n)):
        return []
    out = [ch[i] for i in range(n.value)]
    if ch:
        X.XFree(ch)
    return out


def parent_of(w):
    r, p, ch, n = UL(), UL(), ctypes.POINTER(UL)(), UI()
    if not X.XQueryTree(dpy, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(n)):
        return 0
    if ch:
        X.XFree(ch)
    return p.value


def prop(w, name, typ):
    """bytes for 8-bit properties, list of ints for 32-bit ones (Xlib returns those as C longs)."""
    a = X.XInternAtom(dpy, name.encode(), 1)
    if not a:
        return None
    t, f, n, after = UL(), I(), UL(), UL()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    if X.XGetWindowProperty(dpy, w, a, 0, 4096, 0, typ, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n),
                            ctypes.byref(after), ctypes.byref(data)) != 0 or not n.value:
        return None
    addr = ctypes.cast(data, VP).value
    if f.value == 8:
        v = ctypes.string_at(addr, n.value)
    elif f.value == 32:
        arr = ctypes.cast(data, ctypes.POINTER(ctypes.c_long))
        v = [arr[i] for i in range(n.value)]
    else:
        v = None
    X.XFree(data)
    return v


def browser_pid(port):
    # Chromium rewrites its process title: /proc/<pid>/cmdline is one space-separated string, not
    # NUL-separated arguments, so match substrings
    flag = f"--remote-debugging-port={port} ".encode()
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            cmd = open(f"/proc/{d}/cmdline", "rb").read().replace(b"\0", b" ") + b" "
        except OSError:
            continue
        if flag in cmd and b" --type=" not in cmd:
            return int(d)
    return None


def browser_window(pid, parent):
    """Chromium's browser window: top-level, or already inside <parent>. Not the 10x10 client leader,
    whose WM_CLASS is plain "chromium"."""
    for w in children(root) + children(parent):
        if prop(w, "_NET_WM_PID", 6) != [pid]:
            continue
        cls = prop(w, "WM_CLASS", 31)
        if isinstance(cls, bytes) and cls.split(b"\0")[0] != b"chromium":
            return w
    return None


def input_shape_empty(w):
    n, order = I(), I()
    rects = Xext.XShapeGetRectangles(dpy, w, SHAPE_INPUT, ctypes.byref(n), ctypes.byref(order))
    if rects:
        X.XFree(rects)
    return n.value == 0


def ignore_input(w):
    """Empty input shape: clicks and keys fall through to Medula and the shim. Chromium resets the
    shape when it sets up or resizes its window (seen 2026-11-19), so this is checked every pass."""
    if not input_shape_empty(w):
        Xext.XShapeCombineRectangles(dpy, w, SHAPE_INPUT, 0, 0, None, 0, SHAPE_SET, 0)
        X.XSync(dpy, 0)
        return True
    return False


def no_background(w):
    """Background None: an expose then only asks Chromium to redraw, X does not clear the window first
    (clearing first is what made the page blink when exposes were sent periodically)."""
    X.XSetWindowBackgroundPixmap(dpy, w, 0)


def expose(w):
    X.XClearArea(dpy, w, 0, 0, 0, 0, 1)
    X.XFlush(dpy)


def active_window():
    v = prop(root, "_NET_ACTIVE_WINDOW", 33)          # XA_WINDOW
    return v[0] if isinstance(v, list) and v else 0


def attrs(w):
    a = WinAttrs()
    return a if X.XGetWindowAttributes(dpy, w, ctypes.byref(a)) else None


MENU_TYPES = ("_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_MENU")


def open_menu(pid):
    """A mapped menu window of the Chromium browser process (its right-click menu), as (window, attrs)."""
    menu_atoms = {X.XInternAtom(dpy, t.encode(), 1) for t in MENU_TYPES} - {0}
    for w in children(root):
        if prop(w, "_NET_WM_PID", 6) != [pid]:
            continue
        types = prop(w, "_NET_WM_WINDOW_TYPE", 4)        # XA_ATOM
        if not isinstance(types, list) or not menu_atoms.intersection(types):
            continue
        a = attrs(w)
        if a and a.map_state == 2:
            return w, a
    return None


def buttons_down():
    r, c, rx, ry, wx, wy, mask = UL(), UL(), I(), I(), I(), I(), UI()
    X.XQueryPointer(dpy, root, ctypes.byref(r), ctypes.byref(c), ctypes.byref(rx), ctypes.byref(ry),
                    ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
    return mask.value & (0x100 | 0x200 | 0x400), rx.value, ry.value     # Button1..3


def send_escape(w):
    """Close a Chromium menu: Escape as a synthetic key event to the menu window."""
    kc = X.XKeysymToKeycode(dpy, 0xff1b)                # XK_Escape
    for etype in (2, 3):                                 # KeyPress, KeyRelease
        ev = XEventPad()
        ev.key.type, ev.key.send_event, ev.key.display, ev.key.window = etype, 1, dpy, w
        ev.key.root, ev.key.keycode, ev.key.same_screen = root, kc, 1
        X.XSendEvent(dpy, w, 1, 1 if etype == 2 else 2, ctypes.byref(ev))   # Key{Press,Release}Mask
    X.XFlush(dpy)


def read_place(path):
    try:
        x11, sx, sy, w, h = open(path).read().split()
        return int(x11, 16), int(sx), int(sy), int(w), int(h)
    except (OSError, ValueError):
        return None


def origin(w):
    x, y, ch = I(), I(), UL()
    X.XTranslateCoordinates(dpy, w, root, 0, 0, ctypes.byref(x), ctypes.byref(y), ctypes.byref(ch))
    return x.value, y.value


def tebeos_running():
    """Any TEBEOS program (tebeos_plus.exe, tebeos_medula.exe, ...) running under Wine."""
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                if b"tebeos_" in open(f"/proc/{d}/cmdline", "rb").read().lower():
                    return True
            except OSError:
                pass
    return False


def main():
    prefix, port = sys.argv[1], int(sys.argv[2])
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 24 * 3600
    user = os.environ.get("USER", "")
    path = os.path.join(prefix, "drive_c", "users", user, "AppData", "Local", "Temp", "mshtml-cdp-embed.txt")
    end = time.time() + seconds
    placed = None
    idle_since = None
    while time.time() < end:
        # started by the TEBEOS launcher: stop once no TEBEOS program has run for two minutes
        if tebeos_running():
            idle_since = None
        elif idle_since is None:
            idle_since = time.time()
        elif time.time() - idle_since > 120:
            print("no TEBEOS program for 2 minutes; exiting", flush=True)
            return
        place = read_place(path)
        pid = browser_pid(port)
        if place and pid and place[0]:
            parent, sx, sy = place[0], place[1], place[2]
            win = browser_window(pid, parent)
            if win:
                px, py = origin(parent)
                # The offset inside Medula is worked out once per report from the shim (which rewrites
                # the file within 0.5 s of any move or resize), never from an old report and a new
                # Medula position: when Medula moves, Chromium's window moves with it as its child.
                if state.get("place") != place:
                    state["place"] = place
                    state["off"] = (sx - px, sy - py)
                ox, oy = state["off"]
                if parent_of(win) != parent:
                    X.XReparentWindow(dpy, win, parent, ox, oy)
                    X.XMapWindow(dpy, win)
                    X.XSync(dpy, 0)
                    placed = (win, parent, ox, oy)
                    print(f"embedded {win:#x} into {parent:#x} at {ox},{oy}", flush=True)
                else:
                    # Chromium moves its own window now and then (seen 14 px down after setup), and the
                    # shim maps clicks to its window's position: compare where it really is, every pass
                    cx, cy = origin(win)
                    if (cx - px, cy - py) != (ox, oy):
                        X.XMoveWindow(dpy, win, ox, oy)
                        X.XSync(dpy, 0)
                        print(f"moved {win:#x} from {cx - px},{cy - py} to {ox},{oy}", flush=True)
                    placed = (win, parent, ox, oy)
                if ignore_input(win):
                    print(f"input shape of {win:#x} cleared", flush=True)
        else:
            win = parent = None
        if win and placed and placed[0] == win and not state.get("bg") == win:
            no_background(win)
            state["bg"] = win
            redraw_at.extend(time.time() + d for d in (0.0, 0.5))
        fast_ticks(win, parent, pid)


state = {}            # "bg": window whose background is None; "active": last _NET_ACTIVE_WINDOW
redraw_at = []        # times at which to ask Chromium to redraw


def fast_ticks(win, parent, pid):
    """0.5 s in 50 ms steps:
    - When Medula becomes the active window again (another app was in front), Wine repaints the whole
      Medula window, including the area under Chromium's window, and Chromium does not notice: ask it to
      redraw now, after 0.3 s and after 1 s (Wine's repaint comes a little later).
    - While a Chromium menu is open: Chromium's window ignores input, so a click outside the menu goes
      to the shim and Chromium never closes the menu. On a button press outside it, send it Escape."""
    prev = None
    for _ in range(10):
        now = time.time()
        if win:
            act = active_window()
            if act != state.get("active"):
                state["active"] = act
                if act == parent:
                    redraw_at.extend(now + d for d in (0.0, 0.3, 1.0))
            if any(t <= now for t in redraw_at):
                redraw_at[:] = [t for t in redraw_at if t > now]
                expose(win)
        if pid:
            menu = open_menu(pid)
            if menu:
                buttons, x, y = buttons_down()
                w, a = menu
                outside = not (a.x <= x < a.x + a.width and a.y <= y < a.y + a.height)
                # only a new press (prev == 0), not the right button still held from opening the menu
                if buttons and prev == 0 and outside:
                    send_escape(w)
                    print(f"menu {w:#x} closed (click outside at {x},{y})", flush=True)
                prev = buttons
            else:
                prev = None
        time.sleep(0.05)


if __name__ == "__main__":
    main()
