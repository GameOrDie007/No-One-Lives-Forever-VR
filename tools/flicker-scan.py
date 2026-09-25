#!/usr/bin/env python3
"""Find pixels that ALTERNATE between two values - the signature of z-fighting.

A surface that flickers is not a surface that changes. Steam drifts, a flame
licks, a character walks: those pixels move through a range of values. Two
coplanar polygons fighting for the same depth do something different - they
swap, A B A B, frame after frame, because which one wins is decided by
floating-point noise that flips with the tiniest camera movement.

So this does not ask "did the picture change" (everything changes) but "did
this pixel take turns between exactly two values". Run it over a series of
frames dumped with +StubFrameDumpEvery N while standing still.

    python tools/flicker-scan.py game/logs            # all frame-*.bmp
    python tools/flicker-scan.py game/logs --out f.png

Prints the fraction of the image that alternates and writes a map of where,
so a picture of the fight can go straight into a bug note.
"""
import glob
import os
import re
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    root = args[0] if args else 'game/logs'
    out = None
    if '--out' in sys.argv:
        out = sys.argv[sys.argv.index('--out') + 1]

    try:
        from PIL import Image
    except ImportError:
        print('needs Pillow')
        return 1

    # BMPs from the renderer's own frame dump, PNGs from a window capture -
    # tools/canopy-burst.ps1 writes the second kind while somebody is playing,
    # and this scanner silently found zero of them for a while because it only
    # looked for the first. Both are lossless; the source does not matter.
    fs = []
    for pat in ('frame-*.bmp', '*.bmp', '*.png'):
        fs = glob.glob(os.path.join(root, pat))
        fs = [f for f in fs if 'flicker' not in os.path.basename(f).lower()]
        if fs:
            break

    def _seq(p):
        m = re.findall(r'(\d+)', os.path.basename(p))
        return int(m[-1]) if m else 0
    fs = sorted(fs, key=_seq)
    if len(fs) < 6:
        print('need at least 6 frames, found %d' % len(fs))
        return 1
    # The settled half: the first frames include the teleport and the fade.
    fs = fs[len(fs) // 2:][:24]
    print('%d frames, %s .. %s' % (len(fs), os.path.basename(fs[0]), os.path.basename(fs[-1])))

    ims = []
    for p in fs:
        im = Image.open(p).convert('L')
        # Half size: a fight is never one pixel wide, and this is 4x faster.
        im = im.resize((im.size[0] // 2, im.size[1] // 2))
        ims.append(list(im.getdata()))
    w, h = Image.open(fs[0]).size
    w //= 2
    h //= 2

    n = len(ims)
    npx = len(ims[0])
    alt = bytearray(npx)
    nAlt = 0
    nMoved = 0
    for i in range(npx):
        vals = [ims[k][i] for k in range(n)]
        lo, hi = min(vals), max(vals)
        if hi - lo < 12:
            continue            # steady enough to be nobody's problem
        nMoved += 1
        # Two clusters only, and it swaps between them at least three times.
        mid = (lo + hi) / 2.0
        near = sum(1 for v in vals if abs(v - lo) < (hi - lo) * 0.25)
        far = sum(1 for v in vals if abs(v - hi) < (hi - lo) * 0.25)
        if near + far < n * 0.9:
            continue            # spends time in between: a gradient, not a fight
        swaps = 0
        cur = vals[0] > mid
        for v in vals[1:]:
            b = v > mid
            if b != cur:
                swaps += 1
                cur = b
        if swaps >= 3:
            alt[i] = 255
            nAlt += 1

    print('pixels that changed at all : %7d  (%.2f%% of the image)' % (nMoved, 100.0 * nMoved / npx))
    print('pixels that ALTERNATE      : %7d  (%.2f%% of the image)' % (nAlt, 100.0 * nAlt / npx))
    if nAlt > npx * 0.002:
        print('  <- FLICKER. Two surfaces are fighting for the same depth.')
    else:
        print('  no flicker worth the name.')

    if out:
        m = Image.new('L', (w, h))
        m.putdata(list(alt))
        m.save(out)
        print('map of where: %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
