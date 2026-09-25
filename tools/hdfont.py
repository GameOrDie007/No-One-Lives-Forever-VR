"""hdfont.py - make higher-resolution copies of NOLF's bitmap menu fonts.

WHY. The menu's positions scale with resolution (every folder lays out through
GetYRatio, screen height over 480) but the fonts are BITMAPS blitted 1:1, so
the glyphs do not. At 4K the layout is 4.5x the authored size and the largest
glyph on offer is 52 px; in a headset the same defect reads as "the font is
too small". The Modernizer added exactly one higher-resolution sheet by hand
(font_large_0_hd.pcx, 1.86x). This makes the rest, at 2x, 3x and 4x, and the
client picks one by the layout ratio (InterfaceResMgr::InitFonts).

WHY NOT A SYSTEM FONT. Tried 12 September: the engine can render a TrueType
face at any size and the layout and hit-testing both follow it - but the font
library makes a temporary surface PER STRING PER FRAME (3,250 creates in 20 s)
and reads the SCREEN back to blend each one (a GPU stall per string), and the
write-back lands in one eye only. Bitmap fonts blit like every other 2D
primitive: duplicated per eye for free, no readback, and the typeface stays.

HOW THE FORMAT WORKS (lithfontdefs.h, "SPECIAL NOTE ABOUT BITMAP FONTS"):
glyphs sit in one horizontal strip and are separated by columns that are
ENTIRELY the transparent colour; the library computes each glyph's width from
those gaps. Every menu font is set up with bBlend, so the transparent colour
is BLACK and brightness is alpha. Measured on the shipped files: all six
detect at 94 glyph runs (printable ASCII minus the space) with that rule.

So: find the separator columns in the source, upscale the strip with a real
filter (soft edges are simply softer alpha, which is correct for a blended
font), then force every column that was a separator back to exact black so
the width calculation cannot be fooled by filter ringing. Output is 24-bit
RGB, which the engine already accepts (font_med_0.pcx ships that way).

    python tools/hdfont.py                 # all menu fonts, 2x 3x 4x
    python tools/hdfont.py --scales 3      # one scale
    python tools/hdfont.py --check         # verify outputs re-detect at 94
"""
import argparse
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import dat  # noqa: E402

GAME = os.path.join(ROOT, 'game')
OUT = os.path.join(ROOT, 'src', 'nolf1-modernizer', 'ASSETS', 'interface', 'fonts')

# The menu fonts, by the names layout.txt uses. HUD fonts are left alone on
# purpose: their size in the headset is StubHudScale100's business.
FONTS = ['font_small_0', 'font_med_0', 'font_large_0', 'font_title', 'font_help']
KEY = (0, 0, 0)


def load_source(name):
    """The shipped strip, from whichever rez wins, as RGB."""
    pcx = dat.mounted(GAME, '.PCX')
    key = 'INTERFACE/FONTS/%s.PCX' % name.upper()
    if key not in pcx:
        raise SystemExit('not in any rez: ' + key)
    import io
    im = Image.open(io.BytesIO(pcx[key].read(key)))
    return im.convert('RGB')


def separators(im):
    """Column indices that are entirely the transparent colour."""
    w, h = im.size
    px = im.load()
    return [x for x in range(w) if all(px[x, y] == KEY for y in range(h))]


def glyph_runs(im):
    sep = set(separators(im))
    runs, inrun = 0, False
    for x in range(im.size[0]):
        if x not in sep:
            if not inrun:
                runs += 1
                inrun = True
        else:
            inrun = False
    return runs


def upscale(im, s):
    w, h = im.size
    seps = separators(im)
    big = im.resize((w * s, h * s), Image.LANCZOS)
    px = big.load()
    # Every source separator column becomes s output columns, all forced black.
    for x in seps:
        for xx in range(x * s, x * s + s):
            for y in range(h * s):
                px[xx, y] = KEY
    # Lanczos can undershoot to negative-ish values that clamp oddly, and can
    # leave faint non-black specks in what should be clear; anything darker
    # than a threshold is noise and goes to exact black so alpha is truly 0.
    for y in range(h * s):
        for xx in range(w * s):
            r, g, b = px[xx, y]
            if r < 6 and g < 6 and b < 6:
                px[xx, y] = KEY
    return big


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scales', default='2,3,4')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    scales = [int(v) for v in a.scales.split(',')]
    os.makedirs(OUT, exist_ok=True)

    ok = True
    for name in FONTS:
        src = load_source(name)
        n0 = glyph_runs(src)
        line = '%-14s %5dx%-3d runs %3d' % (name, src.size[0], src.size[1], n0)
        for s in scales:
            out = os.path.join(OUT, '%s_%dx.pcx' % (name, s))
            if not a.check:
                big = upscale(src, s)
                big.save(out, 'PCX')
            chk = Image.open(out).convert('RGB')
            n = glyph_runs(chk)
            flag = '' if n == n0 else '   <-- MISMATCH'
            if n != n0:
                ok = False
            line += '   %dx: %5dx%-3d runs %3d%s' % (s, chk.size[0], chk.size[1], n, flag)
        print(line)
    print('outputs in', OUT)
    if not ok:
        print('AT LEAST ONE OUTPUT DOES NOT RE-DETECT LIKE ITS SOURCE - do not ship it.')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
