"""ocrbench.py OCRDIR IMG=LABEL... -- run captcha images through TEBEOS's CapthaParser
preprocessing (bicubic 8x, any channel >= 150 -> white else black) and ocr_service.ocr()."""
import os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import ocr_service
from PIL import Image
ok = 0
pairs = [a.split("=") for a in sys.argv[2:]]
for path, label in pairs:
    im = Image.open(path).convert("RGB")
    big = im.resize((im.width * 8, im.height * 8), Image.BICUBIC)
    px = big.load()
    for x in range(big.width):
        for y in range(big.height):
            r, g, b = px[x, y]
            px[x, y] = (255, 255, 255) if (r >= 150 or g >= 150 or b >= 150) else (0, 0, 0)
    fd, tmp = tempfile.mkstemp(suffix=".bmp"); os.close(fd)
    big.save(tmp)
    got = ocr_service.ocr(tmp)
    os.remove(tmp)
    ok += got == label
    print("%-8s want %s got %-8s %s" % (os.path.basename(path), label, got, "OK" if got == label else "MISS"))
print("%d/%d" % (ok, len(pairs)))
