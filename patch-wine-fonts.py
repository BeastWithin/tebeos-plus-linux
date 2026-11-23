#!/usr/bin/env python3
"""Graft U+20BA (TURKISH LIRA SIGN) into fonts that do not have it.

    patch-wine-fonts.py [-n] FILE.ttf [FILE.ttf ...]

TEBEOS prints every money amount with the lira sign.  Neither Wine's bundled
Tahoma/Arial metric clones nor the XP-era fonts winetricks installs contain that
glyph, so the app paints a tofu box instead:  "1.234,56[]".

The glyph is copied from a donor font (Noto Sans) and rescaled to the target's
units-per-em, so the target keeps its family name, its metrics and every other
glyph - no layout shift, no substitution.  Files whose name looks bold get the
bold donor.

Requires fontTools (pip install fonttools).  Use -n to only report.

    # the fonts Wine itself ships (a portable Wine build)
    patch-wine-fonts.py <wine>/share/wine/fonts/{tahoma,tahomabd,arial}.ttf

    # the fonts winetricks/corefonts drops into the prefix
    patch-wine-fonts.py <prefix>/drive_c/windows/Fonts/{tahoma,tahomabd,arial,arialbd}.ttf
"""
import os
import stat
import sys

from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.misc.transform import Scale

CP = 0x20BA  # ₺
DONOR_REGULAR = '/usr/share/fonts/noto/NotoSans-Regular.ttf'
DONOR_BOLD = '/usr/share/fonts/noto/NotoSans-Bold.ttf'


def donor_for(path):
    """Bold-looking target files get the bold lira sign."""
    if os.path.exists(DONOR_BOLD):
        base = os.path.basename(path).lower()
        if 'bd' in base or 'bold' in base:
            return DONOR_BOLD
    return DONOR_REGULAR


def graft(target, donor, dry_run=False):
    font = TTFont(target)
    if CP in font.getBestCmap():
        print(f"  {os.path.basename(target)}: already has ₺ - skipped")
        return False
    if dry_run:
        print(f"  {os.path.basename(target)}: would add ₺ (donor {os.path.basename(donor)})")
        return False

    src = TTFont(donor)
    name = src.getBestCmap().get(CP)
    if name is None:
        raise SystemExit(f"donor {donor} has no U+20BA")

    scale = font['head'].unitsPerEm / src['head'].unitsPerEm
    pen = TTGlyphPen(None)
    src.getGlyphSet()[name].draw(TransformPen(pen, Scale(scale)))

    new = 'uni20BA'
    font.setGlyphOrder(font.getGlyphOrder() + [new])
    font['glyf'][new] = pen.glyph()
    advance, lsb = src['hmtx'][name]
    font['hmtx'][new] = (round(advance * scale), round(lsb * scale))
    for table in font['cmap'].tables:
        if table.isUnicode():
            table.cmap[CP] = new

    # winetricks installs its fonts read-only
    os.chmod(target, os.stat(target).st_mode | stat.S_IWUSR)
    font.save(target)
    print(f"  {os.path.basename(target)}: ₺ added (from {os.path.basename(donor)})")
    return True


def main(argv):
    args = [a for a in argv[1:] if a != '-n']
    dry_run = '-n' in argv[1:]
    if not args:
        raise SystemExit(__doc__)
    changed = 0
    for path in args:
        if not os.path.exists(path):
            print(f"  {path}: not found - skipped")
            continue
        if graft(path, donor_for(path), dry_run):
            changed += 1
    print(f"{changed} font(s) patched")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
