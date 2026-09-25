"""Measure how the eye-to-eye shift varies with DEPTH in a captured frame.

A stereo pair has ONE property that matters and it is not the size of the
shift: it is that the shift CHANGES with distance. A constant shift at every
depth is not stereo, it is a fixed aim difference, and the eyes cannot fuse it.

So this reports the shift for several named patches and, as the single number,
the SPREAD between the nearest and the furthest. Doubling the eye separation
must double that spread; if it does not, the separation is not reaching the
picture whatever the log says it is.

Patches are given on the command line as name:x0,y0,x1,y1 so the same script
serves any capture.  Cross-correlation, and a patch whose correlation is weak
is reported as unusable rather than quietly averaged in - the lesson from
docs/ZFIGHTING-REVERSED-Z.md is that a number which looks like a measurement
but matches the wrong two things is worse than no number.
"""

import argparse
import sys

import numpy as np
from PIL import Image

DEFAULT = [
    "far-roofline:470,150,700,210",
    "far-sunlit-wall:480,200,660,330",
    "mid-doorway:760,300,900,430",
    "near-ground:430,470,640,590",
]


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-6 else -2.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--eyew", type=int, default=928)
    ap.add_argument("--bottom", type=int, default=640,
                    help="ignore rows below this - a capture wider than the "
                         "desktop picks up the taskbar")
    ap.add_argument("--range", type=int, default=380)
    ap.add_argument("--patch", action="append", default=None)
    a = ap.parse_args()

    im = np.asarray(Image.open(a.png).convert("L"), dtype=float)
    L, R = im[:, :a.eyew], im[:, a.eyew:2 * a.eyew]

    rows = []
    for spec in (a.patch or DEFAULT):
        name, box = spec.split(":")
        x0, y0, x1, y1 = (int(v) for v in box.split(","))
        pat = L[y0:y1, x0:x1]
        if pat.std() < 6.0:
            print("  %-22s flat, unusable" % name)
            continue
        best = (-2.0, 0, 0)
        for dy in range(-14, 15):
            for dx in range(-a.range, a.range // 3 + 1):
                ca, cb, ra, rb = x0 + dx, x1 + dx, y0 + dy, y1 + dy
                if ca < 0 or cb > a.eyew or ra < 0 or rb > a.bottom:
                    continue
                c = ncc(pat, R[ra:rb, ca:cb])
                if c > best[0]:
                    best = (c, dx, dy)
        c, dx, dy = best
        ok = c >= 0.70
        print("  %-22s dx %+5d  dy %+3d  corr %.2f%s"
              % (name, dx, dy, c, "" if ok else "   UNUSABLE"))
        if ok:
            rows.append((name, dx))

    if len(rows) < 2:
        print("\nfewer than two usable patches - this says nothing.")
        return 1
    lo = min(r[1] for r in rows)
    hi = max(r[1] for r in rows)
    print("\n  depth spread: %d px  (%s .. %s)"
          % (hi - lo, min(rows, key=lambda r: r[1])[0],
             max(rows, key=lambda r: r[1])[0]))
    print("  constant part: %d px  - the asymmetric frustum, not stereo" % hi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
