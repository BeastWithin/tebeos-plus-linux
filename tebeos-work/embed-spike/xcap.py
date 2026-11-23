# xcap.py -- capture an X window to PNG with Xlib XGetImage (ImageMagick's import/x: stopped
# returning pixels on this session). usage: xcap.py <win_hex> <out.png>
import ctypes, subprocess, sys, embed_helper as e
class WA(ctypes.Structure):
    _fields_=[("x",ctypes.c_int),("y",ctypes.c_int),("w",ctypes.c_int),("h",ctypes.c_int),("bw",ctypes.c_int),("depth",ctypes.c_int),("visual",ctypes.c_void_p),("root",ctypes.c_ulong),("cls",ctypes.c_int),("bit_grav",ctypes.c_int),("win_grav",ctypes.c_int),("backing",ctypes.c_int),("planes",ctypes.c_ulong),("pixel",ctypes.c_ulong),("save_under",ctypes.c_int),("colormap",ctypes.c_ulong),("map_installed",ctypes.c_int),("map_state",ctypes.c_int),("all_ev",ctypes.c_long),("dont_prop",ctypes.c_long),("override",ctypes.c_int),("screen",ctypes.c_void_p)]
e.X.XGetWindowAttributes.argtypes=[ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(WA)]
e.X.XGetImage.restype = ctypes.c_void_p
e.X.XGetImage.argtypes=[ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
win = int(sys.argv[1], 16); out = sys.argv[2]
a = WA(); e.X.XGetWindowAttributes(e.dpy, win, ctypes.byref(a))
img = e.X.XGetImage(e.dpy, win, 0, 0, a.w, a.h, ~0 & 0xFFFFFFFFFFFFFFFF, 2)   # ZPixmap
if not img:
    sys.exit("XGetImage failed")
data = ctypes.string_at(ctypes.c_void_p.from_address(img + 16).value, a.w * a.h * 4)   # XImage.data
raw = out + ".bgrx"
open(raw, "wb").write(data)
subprocess.run(["magick", "-size", f"{a.w}x{a.h}", "-depth", "8", f"BGRA:{raw}", "-alpha", "off", out], check=True)
import os; os.remove(raw)
print("captured", a.w, "x", a.h, "->", out)
