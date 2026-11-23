"""ocrvariants.py OCRDIR IMG=LABEL... -- compare OCR strategies on captchas run through the
vendor's preprocessing (bicubic 8x, any channel >= 150 -> white else black)."""
import os, sys, subprocess, tempfile, collections
sys.path.insert(0, sys.argv[1])
import ocr_service
from PIL import Image, ImageOps

TMP = tempfile.mkdtemp()

def vendor(path):
    im = Image.open(path).convert("RGB")
    big = im.resize((im.width * 8, im.height * 8), Image.BICUBIC)
    px = big.load()
    for x in range(big.width):
        for y in range(big.height):
            r, g, b = px[x, y]
            px[x, y] = (255, 255, 255) if (r >= 150 or g >= 150 or b >= 150) else (0, 0, 0)
    return big.convert("L")

def tess(im, psm, extra=()):
    p = os.path.join(TMP, "t.png"); im.save(p)
    out = subprocess.run(["tesseract", p, "stdout", "--psm", psm, "--oem", "3",
                          "-c", "tessedit_char_whitelist=0123456789", *extra],
                         capture_output=True, text=True, timeout=10).stdout
    return "".join(c for c in out if c.isdigit())

def segments(im):
    w, h = im.size; px = im.load()
    cols = [any(px[x, y] > 128 for y in range(0, h, 2)) for x in range(w)]
    runs, start = [], None
    for x, c in enumerate(cols + [False]):
        if c and start is None: start = x
        if not c and start is not None: runs.append([start, x]); start = None
    runs = [r for r in runs if r[1] - r[0] >= 12]
    while len(runs) > 6:
        gaps = [runs[i + 1][0] - runs[i][1] for i in range(len(runs) - 1)]
        i = gaps.index(min(gaps)); runs[i:i + 2] = [[runs[i][0], runs[i + 1][1]]]
    while 0 < len(runs) < 6:
        i = max(range(len(runs)), key=lambda k: runs[k][1] - runs[k][0])
        a, b = runs[i]; m = (a + b) // 2; runs[i:i + 1] = [[a, m], [m, b]]
    return runs

def glyphs(im, H=64):
    out = []
    for a, b in segments(im):
        c = im.crop((a, 0, b, im.height)); bb = c.getbbox()
        if bb: c = c.crop(bb)
        c = ImageOps.invert(c)
        c = c.resize((max(1, round(c.width * H / c.height)), H), Image.LANCZOS)
        out.append(c)
    return out

def line(gs, pad=24, gap=28):
    W = sum(g.width for g in gs) + gap * (len(gs) - 1) + 2 * pad
    L = Image.new("L", (W, gs[0].height + 2 * pad), 255); x = pad
    for g in gs: L.paste(g, (x, pad)); x += g.width + gap
    return L

def v_baseline(p, im): return ocr_service.ocr(p)
def v_invert(p, im):
    inv = ImageOps.invert(im)
    for psm in ("13", "8", "7", "6"):
        r = tess(inv, psm)
        if len(r) == 6: return r
    return r
def v_line(p, im):
    gs = glyphs(im)
    if not gs: return ""
    L = line(gs)
    for psm in ("7", "8", "13"):
        r = tess(L, psm)
        if len(r) == 6: return r
    return r
def v_chars(p, im):
    out = ""
    for g in glyphs(im):
        r = tess(ImageOps.expand(g, 20, 255), "10")
        out += r[:1] if r else "?"
    return out
import time
def vote(rs, prefer):
    six = [r for r in rs if len(r) == 6 and "?" not in r]
    if not six:
        return max(rs, key=len)
    out = ""
    for i in range(6):
        c = collections.Counter(r[i] for r in six).most_common()
        best = [k for k, n in c if n == c[0][1]]
        out += prefer[i] if len(prefer) == 6 and prefer[i] in best else best[0]
    return out

BASE = [v_baseline, v_invert, v_line, v_chars]
names = [f.__name__[2:] for f in BASE] + ["vote", "vote_line"]
score = collections.Counter(); spent = collections.Counter()
pairs = [a.split("=") for a in sys.argv[2:]]
for path, label in pairs:
    im = vendor(path)
    p = os.path.join(TMP, "v.bmp"); im.convert("RGB").save(p)
    rs = []
    for f in BASE:
        t = time.time(); rs.append(f(p, im)); spent[f.__name__[2:]] += time.time() - t
    rs += [vote(rs, ""), vote(rs, rs[2])]
    for n, r in zip(names, rs):
        score[n] += r == label
    print("%-7s %s  %s" % (os.path.basename(path), label, "  ".join("%-8s" % (r + ("" if r == label else "*")) for r in rs)), flush=True)
print("        " + " ".join("%s=%d/%d" % (n, score[n], len(pairs)) for n in names))
print("        mean s: " + " ".join("%s=%.1f" % (n, spent[n] / len(pairs)) for n in names[:4]))
