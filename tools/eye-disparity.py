"""Measure the disparity between the two eyes from a captured frame.

Two numbers, offline, from an image on disk:

  HORIZONTAL disparity is the stereo. It should be negative and small with the
  left eye on the left - near objects sit further left in the right eye - and
  it is what gives depth.

  VERTICAL disparity should be ZERO. Any of it is unfusable: the eyes cannot
  converge on it, and it presents as strain and doubling rather than as an
  obvious visual fault, so it is exactly the kind of thing that gets described
  as "uncomfortable" and never located. Nothing in this project had measured it.

Normalised cross-correlation, because absolute difference on packed RGB was one
of the four ways the old field instrument produced a confident wrong answer.

SEVERAL INDEPENDENT BANDS, because one band can be fooled and five agreeing
cannot. Each band is measured on its own and the answer is only reported if
they agree; a spread is printed either way so a disagreement is visible rather
than averaged away. The other lesson from that instrument was to reject a peak
sitting on the wall of the search, which is also done here.

    python eye-disparity.py <png> [--eyew 1440]
"""

import argparse
import sys

import numpy as np
from PIL import Image


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-6 else -2.0


def measure(img, top, bh, xa, xb, eyew, maxdx, maxdy, x1, y1):
    left = img[top:top + bh, xa:xb]
    sd = float(left.std())
    if sd < 10.0:
        return None, sd, "flat"

    # TWO STAGES, because the search had to get wide enough to cross the
    # constant per-eye offset and a brute-force sweep of +/-420 by +/-maxdy is
    # tens of thousands of correlations per band - it ran for minutes and got
    # killed. Coarse pass along dy=0 at a stride of 8 to find which side of the
    # frame the match is on, then a fine pass around it. Same answer, ~1% of
    # the work.
    def score(dx, dy):
        ra, rb = top + dy, top + dy + bh
        ca, cb = eyew + xa + dx, eyew + xb + dx
        if ra < 0 or rb > y1 or ca < 0 or cb > x1:
            return None
        return ncc(left, img[ra:rb, ca:cb])

    coarse = (-2.0, 0)
    for dx in range(-maxdx, maxdx + 1, 8):
        c = score(dx, 0)
        if c is not None and c > coarse[0]:
            coarse = (c, dx)

    best = (-2.0, 0, 0)
    for dy in range(-maxdy, maxdy + 1):
        for dx in range(coarse[1] - 10, coarse[1] + 11):
            c = score(dx, dy)
            if c is not None and c > best[0]:
                best = (c, dx, dy)

    corr, dx, dy = best
    if corr < 0.6:
        return None, sd, "corr %.2f" % corr
    if abs(dx) == maxdx or abs(dy) == maxdy:
        return None, sd, "peak on the search wall"
    return (dx, dy, corr), sd, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--eyew", type=int, default=1440,
                    help="width of one eye in the captured image")
    # 420, NOT 80, AND THAT IS WHY THIS TOOL HAS BEEN USELESS ON OUR CAPTURES.
    #
    # Each eye is projected about its OWN optical centre, so the two halves of
    # a frame carry a large CONSTANT horizontal offset that has nothing to do
    # with stereo - measured at -314 px on a 1280-wide eye, with the sky
    # matching at 0.994 correlation. The old default searched +/-80, so the true
    # peak was four times outside the window and every band was rejected with
    # "peak on the search wall" or a garbage correlation. The tool reported
    # "this says nothing" on frames it could have measured perfectly.
    #
    # THE NUMBER THAT MATTERS IS DISPARITY RELATIVE TO INFINITY, not the raw
    # offset. Distant content gives the constant; an object's depth is how far
    # its own dx sits from that. On the Morocco quick save the sky is -314 and
    # the weapon is -388, so the weapon is -74 px from infinity - about 5
    # degrees, which puts it around 0.7 m away and is a plausible arm's length
    # rather than a defect.
    ap.add_argument("--maxdx", type=int, default=420)
    ap.add_argument("--maxdy", type=int, default=24)
    # A COLUMN RANGE, IN LEFT-EYE PIXELS, so two PARTS of one frame can be
    # compared against each other instead of the frame against itself.
    #
    # A full-width band over the pause menu measures the menu and the world
    # TOGETHER and returns whichever has the stronger features - which is how
    # an eyeballed reading and a correlation came to disagree about the sign.
    # The question that matters there is whether the MENU and the WORLD BEHIND
    # IT sit at the same disparity: if they do not, the two cannot be fused at
    # once and the result is the thing testers call double vision. Run it twice
    # with different --cols and compare.
    ap.add_argument("--cols", default="",
                    help="a,b - restrict to these left-eye columns")
    args = ap.parse_args()

    img = np.array(Image.open(args.image).convert("L")).astype(float)
    h, w = img.shape

    lit_cols = np.where((img > 8).any(0))[0]
    lit_rows = np.where((img > 8).any(1))[0]
    if len(lit_cols) == 0 or len(lit_rows) == 0:
        print("the image is entirely black - nothing to measure")
        return 1
    x1, y1 = int(lit_cols.max()) + 1, int(lit_rows.max()) + 1
    print("image %dx%d, lit region %dx%d, eye width %d" % (w, h, x1, y1, args.eyew))

    # The band is clipped so its match in the RIGHT eye stays on screen. The
    # game window can be wider than the desktop - 2880 px on a 2560 px desktop
    # with the headset disconnected - so a band running to the end of the left
    # eye would have nothing to compare against.
    xa = args.maxdx + 8
    xb = min(args.eyew - args.maxdx - 8, x1 - args.eyew - args.maxdx - 8)
    if args.cols:
        # Deliberately NOT clamped into the safe overlap: a caller asking for a
        # specific region is asking about that region, and a silently moved
        # window would answer about somewhere else. Shifts that fall off the
        # image are skipped by score() instead.
        ca, cb = (int(v) for v in args.cols.split(","))
        xa, xb = ca, cb
        print("columns restricted to %d..%d by --cols" % (xa, xb))
    if xb - xa < 128:
        print("only %d px of the right eye are on screen - not enough overlap."
              % (x1 - args.eyew))
        return 1

    bh = max(24, int(y1 * 0.10))
    print("bands %d px tall, cols %d..%d\n" % (bh, xa, xb))
    print("  %-8s %-7s %-8s %-8s %s" % ("band top", "sd", "dx", "dy", "corr"))

    good = []
    for frac in (0.12, 0.22, 0.32, 0.42, 0.52, 0.62, 0.72):
        top = int(y1 * frac)
        if top + bh + args.maxdy >= y1:
            continue
        res, sd, why = measure(img, top, bh, xa, xb, args.eyew,
                               args.maxdx, args.maxdy, x1, y1)
        if res is None:
            print("  %-8d %-7.1f %s" % (top, sd, "rejected (%s)" % why))
            continue
        dx, dy, corr = res
        good.append((dx, dy, corr))
        print("  %-8d %-7.1f %+-8d %+-8d %.3f" % (top, sd, dx, dy, corr))

    print()
    if len(good) < 3:
        print("only %d usable band(s). This says nothing - point the camera at"
              " something with structure and capture again." % len(good))
        return 1

    dxs = np.array([g[0] for g in good])
    dys = np.array([g[1] for g in good])
    print("horizontal: median %+d px, spread %d..%d over %d bands"
          % (np.median(dxs), dxs.min(), dxs.max(), len(good)))
    print("vertical:   median %+d px, spread %d..%d over %d bands"
          % (np.median(dys), dys.min(), dys.max(), len(good)))

    if dys.max() - dys.min() > 3:
        print("\n  the bands DISAGREE on the vertical. That is not a vertical"
              " offset, it is the measurement failing - a different band should"
              " not give a different answer for a rigid pair of images.")
        return 1

    dy = int(np.median(dys))
    print()
    if abs(dy) <= 1:
        print("  VERTICAL DISPARITY %+d px - zero to within quantisation. Correct." % dy)
    else:
        print("  VERTICAL DISPARITY %+d px - NOT ZERO, and every band agrees."
              " The eyes are misaligned vertically and cannot be fused." % dy)
    return 0


if __name__ == "__main__":
    sys.exit(main())
