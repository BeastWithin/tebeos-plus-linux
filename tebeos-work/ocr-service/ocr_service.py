#!/usr/bin/env python3
"""Host-side OCR service for the TEBEOS Medula captcha (drop-in tessnet2_32 shim)."""
import os, sys, time, subprocess, glob, collections
from concurrent.futures import ThreadPoolExecutor

try:
    from PIL import Image, ImageOps
except Exception:
    Image = None

PREFIX = os.path.expanduser(os.environ.get('TEBEOS_WINEPREFIX', '~/.wine-tebeos'))
IN_DIR  = os.path.join(PREFIX, 'drive_c', 'ocr-in')
OUT_DIR = os.path.join(PREFIX, 'drive_c', 'ocr-out')
TESSERACT = os.environ.get('TESSERACT', '/usr/bin/tesseract')

def _run_tess(img_path, psm):
    out_base = os.path.join(OUT_DIR, 'tmp_' + os.path.basename(img_path) + '_' + psm)
    cmd = [TESSERACT, img_path, out_base, '--psm', psm, '--oem', '3',
           '-c', 'tessedit_char_whitelist=0123456789']
    # tesseract's OpenMP threads sometimes hang for good on a small image (seen with --psm 10)
    env = dict(os.environ, OMP_THREAD_LIMIT='1')
    try:
        subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=10, env=env)
    except Exception:
        return ''
    txt = out_base + '.txt'
    if not os.path.exists(txt):
        return ''
    try:
        with open(txt, 'r', encoding='utf-8', errors='ignore') as f:
            s = f.read()
    except Exception:
        return ''
    finally:
        try: os.remove(txt)
        except Exception: pass
    return ''.join(ch for ch in s if ch.isdigit())

def _ocr_image(im, psm):
    """tesseract on a PIL image."""
    fd, p = tempfile_png()
    try:
        im.save(p)
        return _run_tess(p, psm)
    finally:
        try: os.remove(p)
        except Exception: pass

def tempfile_png():
    import tempfile
    fd, p = tempfile.mkstemp(suffix='.png', dir=OUT_DIR)
    os.close(fd)
    return fd, p

def _glyphs(im, height=64):
    """The six digits of the vendor's binarised 8x image (white on black): split on empty
    columns, cropped, made black on white and scaled to one height. Raised, small and bold
    digits then look alike to tesseract."""
    w, h = im.size
    px = im.load()
    cols = [any(px[x, y] > 128 for y in range(0, h, 2)) for x in range(w)]
    runs, start = [], None
    for x, c in enumerate(cols + [False]):
        if c and start is None:
            start = x
        if not c and start is not None:
            runs.append([start, x])
            start = None
    runs = [r for r in runs if r[1] - r[0] >= 12]          # specks narrower than 1.5 source px
    while len(runs) > 6:                                     # a digit split in two: join the closest
        gaps = [runs[i + 1][0] - runs[i][1] for i in range(len(runs) - 1)]
        i = gaps.index(min(gaps))
        runs[i:i + 2] = [[runs[i][0], runs[i + 1][1]]]
    while 0 < len(runs) < 6:                                 # two digits touching: halve the widest
        i = max(range(len(runs)), key=lambda k: runs[k][1] - runs[k][0])
        a, b = runs[i]
        runs[i:i + 1] = [[a, (a + b) // 2], [(a + b) // 2, b]]
    out = []
    for a, b in runs:
        g = im.crop((a, 0, b, h))
        bb = g.getbbox()
        if bb:
            g = g.crop(bb)
        g = ImageOps.invert(g)
        out.append(g.resize((max(1, round(g.width * height / g.height)), height), Image.LANCZOS))
    return out

def _read_line(im):
    gs = _glyphs(im)
    if not gs:
        return ''
    pad, gap = 24, 28
    line = Image.new('L', (sum(g.width for g in gs) + gap * (len(gs) - 1) + 2 * pad, gs[0].height + 2 * pad), 255)
    x = pad
    for g in gs:
        line.paste(g, (x, pad))
        x += g.width + gap
    r = ''
    for psm in ('7', '8', '13'):
        r = _ocr_image(line, psm)
        if len(r) == 6:
            break
    return r

def _read_chars(im):
    out = ''
    for g in _glyphs(im):
        r = _ocr_image(ImageOps.expand(g, 20, 255), '10')
        out += r[:1] if r else '?'
    return out

def _read_inverted(im):
    inv = ImageOps.invert(im)
    r = ''
    for psm in ('13', '8', '7', '6'):
        r = _ocr_image(inv, psm)
        if len(r) == 6:
            break
    return r

def ocr(path):
    """OCR a captcha image -> digit string. Captcha is always 6 digits.

    Four readings of the vendor's image, voted per digit, ties going to the segmented line.
    Measured 2026-10-22 on 24 live captchas: the whole image alone 20/24, the segmented line
    22/24, the vote 24/24 (run/ocrvariants.py on the box)."""
    if Image is None:
        return _ocr_whole(path)
    try:
        im = Image.open(path).convert('L')
    except Exception:
        return _ocr_whole(path)
    with ThreadPoolExecutor(4) as ex:
        jobs = [ex.submit(_ocr_whole, path), ex.submit(_read_inverted, im),
                ex.submit(_read_line, im), ex.submit(_read_chars, im)]
        reads = []
        for j in jobs:
            try:
                reads.append(j.result())
            except Exception:
                reads.append('')
    six = [r for r in reads if len(r) == 6 and r.isdigit()]
    if not six:
        return max(reads, key=len)
    line = reads[2] if len(reads[2]) == 6 and reads[2].isdigit() else ''
    out = ''
    for i in range(6):
        counts = collections.Counter(r[i] for r in six).most_common()
        best = [c for c, n in counts if n == counts[0][1]]
        out += line[i] if line and line[i] in best else best[0]
    return out

def _ocr_whole(path):
    """The whole image as the vendor hands it over."""
    results = []

    def consider(img_path, psm):
        r = _run_tess(img_path, psm)
        if r:
            results.append(r)
        return r

    # The vendor's pipeline upscales the 140x30 captcha 8x and binarises it into a
    # ~1120x240 image.  OCRing that full-size image directly (rather than downscaling
    # it back to 140x30) is what makes tesseract read the digits correctly: the
    # LANCZOS downscale blurred the thin digit strokes, turning '6' into '5'/'9' and
    # dropping the leading/trailing digits.  Sparse-text (13) and single-word (8)
    # modes read the isolated digit sequence reliably at full size.
    for psm in ('13', '8', '7', '6'):
        r = consider(path, psm)
        if len(r) == 6:
            return r

    # Fallback: if the image was 8x upscaled, downscale to native and retry.
    tmp_small = None
    if Image is not None:
        try:
            im = Image.open(path).convert('L')
            if im.width >= 700:
                im2 = im.resize((im.width // 8, im.height // 8), Image.LANCZOS)
                tmp_small = os.path.join(OUT_DIR, 'tmp_small_' + os.path.basename(path) + '.png')
                im2.save(tmp_small)
        except Exception:
            tmp_small = None

    if tmp_small:
        for psm in ('7', '6', '13', '8'):
            r = consider(tmp_small, psm)
            if len(r) == 6:
                try: os.remove(tmp_small)
                except Exception: pass
                return r
        try: os.remove(tmp_small)
        except Exception: pass

    # pick longest result as last resort
    if results:
        return max(results, key=len)
    return ''

def main():
    os.makedirs(IN_DIR, exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    seen = set()
    print(f'[ocr-service] watching {IN_DIR} -> {OUT_DIR}  (tesseract {TESSERACT})', flush=True)
    while True:
        try:
            for p in sorted(glob.glob(os.path.join(IN_DIR, '*.png'))):
                if p in seen:
                    continue
                seen.add(p)
                digits = ocr(p)
                name = os.path.basename(p)[:-4]
                outp = os.path.join(OUT_DIR, name + '.txt')
                try:
                    with open(outp, 'w', encoding='utf-8') as f:
                        f.write(digits)
                except Exception:
                    pass
                try:
                    os.remove(p)
                except Exception:
                    pass
                print(f'[ocr-service] {name} -> "{digits}"', flush=True)
            seen = set(s for s in seen if os.path.exists(s))
        except Exception as e:
            print(f'[ocr-service] error: {e}', flush=True)
        time.sleep(0.05)

if __name__ == '__main__':
    main()
