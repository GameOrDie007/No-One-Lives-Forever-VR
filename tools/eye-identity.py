"""Do the two eyes agree when they are LOOKING FROM THE SAME POINT?

With the fake host reporting an IPD of zero and the frustum symmetric, the two
halves of a side-by-side capture are two renders of one camera. Every pixel
that differs is therefore a fault in OUR pass, not stereo: something that is
sampled, seeded or clocked per PASS instead of per FRAME.

That is how the per-eye animation clock was found - each eye asked the wall
clock for the time, so an animated surface was at two different moments in the
two halves, which is what shimmer IS in a headset. This is the instrument that
says whether that is fixed, and it can fail: run it against a build with the
bug and the animated surfaces light up.

The frame marker is drawn per eye ON PURPOSE and always differs; it is a few
hundred pixels in one corner, and the --ignore-left/--ignore-top options keep
it out of the number.

    python tools/eye-identity.py logs/pool.png
    python tools/eye-identity.py logs/pool.png --out logs/pool-diff.png
    python tools/eye-identity.py logs/*.png --thresh 12

Prints, per file:
    differing pixels (over the threshold), as a percentage of one eye
    the mean absolute difference over the whole half
    where the differences are, as a coarse grid, so "the water" and "the
    marker" can be told apart without opening the picture.
"""
import argparse
import glob
import sys

import numpy as np
from PIL import Image


def measure(path, thresh, ignore_left, ignore_top, out):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    if w % 2:
        w -= 1
    a = np.asarray(im, dtype=np.int16)[:, :w]
    half = w // 2
    L = a[:, :half, :]
    R = a[:, half:, :]

    d = np.abs(L - R).max(axis=2)          # worst channel, per pixel
    mask = np.ones(d.shape, dtype=bool)
    if ignore_top:
        mask[:ignore_top, :] = False
    if ignore_left:
        mask[:, :ignore_left] = False

    over = (d > thresh) & mask
    pct = 100.0 * over.sum() / max(1, mask.sum())
    mad = float(d[mask].mean())
    print('%-46s  %6.3f%% of pixels differ by more than %d   mean |L-R| %.2f'
          % (path.split('\\')[-1], pct, thresh, mad))

    # WHERE, coarsely. A number alone cannot separate "the marker" from "the
    # whole lake", and the grid does it in one line each.
    gy, gx = 6, 8
    rows = []
    for j in range(gy):
        cells = []
        for i in range(gx):
            sl = over[j * d.shape[0] // gy:(j + 1) * d.shape[0] // gy,
                      i * d.shape[1] // gx:(i + 1) * d.shape[1] // gx]
            f = 100.0 * sl.sum() / max(1, sl.size)
            cells.append('.' if f < 0.05 else ('%d' % min(9, int(f / 2) + 1)))
        rows.append('    ' + ' '.join(cells))
    if pct > 0.02:
        print('\n'.join(rows))

    if out:
        m = np.zeros(d.shape + (3,), dtype=np.uint8)
        m[..., 0] = np.clip(d * 4, 0, 255)
        m[..., 1] = np.where(over, 255, 0)
        Image.fromarray(m).save(out)
        print('    map written to %s' % out)
    return pct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pngs', nargs='+')
    ap.add_argument('--thresh', type=int, default=8,
                    help='per-channel difference that counts as a difference; '
                         '8 of 255 is well above the renderer\'s own dither')
    ap.add_argument('--ignore-left', type=int, default=0)
    ap.add_argument('--ignore-top', type=int, default=0)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    paths = []
    for p in args.pngs:
        paths += sorted(glob.glob(p)) or [p]
    worst = 0.0
    for p in paths:
        try:
            worst = max(worst, measure(p, args.thresh, args.ignore_left,
                                       args.ignore_top, args.out))
        except Exception as e:                      # a missing file is a result
            print('%-46s  FAILED: %s' % (p, e))
    return 0 if worst < 1.0 else 1


if __name__ == '__main__':
    sys.exit(main())
