"""How far the image moves vertically per degree of pitch, and therefore what
vertical field d3d.ren is actually rasterising.

Same method that settled the horizontal, turned ninety degrees:

    shift_pixels = focal * tan(angle)

so a straight line through several angles gives the focal length, and the focal
length gives the field:

    halfFovY = atan( (viewportH / 2) / focal )

Guards, all of them bought with a wrong answer somewhere in this project's
history:

  - SEVERAL angles, not one. Shift must be proportional to tan(angle); a single
    reading can be a coincidence, a straight line through four cannot. The
    angles must agree on one focal length within 15 percent or nothing is
    reported.
  - Normalised cross-correlation, not absolute difference. Mean luminance moves
    when the view pitches, and that swamped the structure last time.
  - A peak on the wall of the search is the wall, not a peak. Rejected.
  - The weapon is welded to the camera, has zero parallax, and is the highest
    contrast thing in frame - it correlates perfectly with itself at zero shift.
    The band is taken from the upper half, well clear of it.
  - The band is reported with its standard deviation. A blank wall cannot
    measure anything, and saying so is better than returning a number from one.

    python vertical-field.py logs/vfield --fovy 120.63 --height 1494
"""

import argparse
import math
import os
import re
import sys

import numpy as np
from PIL import Image


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = math.sqrt(float((a * a).sum()) * float((b * b).sum()))
    return float((a * b).sum() / d) if d > 1e-6 else -2.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--fovy", type=float, default=120.63,
                    help="full vertical fov the client asked for, degrees")
    ap.add_argument("--height", type=int, default=1494,
                    help="full eye viewport height in pixels (may exceed the capture)")
    ap.add_argument("--maxshift", type=int, default=200)
    args = ap.parse_args()

    shots = {}
    for name in os.listdir(args.dir):
        m = re.match(r"pitch-(-?[\d.]+)\.png$", name)
        if m:
            shots[float(m.group(1))] = os.path.join(args.dir, name)
    if len(shots) < 3:
        print("need at least three captures, found %d" % len(shots))
        return 1

    angles = sorted(shots)
    base_a = angles[0]
    base = np.array(Image.open(shots[base_a]).convert("L")).astype(float)
    h, w = base.shape
    print("captures %s, image %dx%d" % (angles, w, h))

    # SEVERAL patches, each measured on its own, and they must agree.
    #
    # Two reasons this cannot be one wide band. A pitch REPROJECTS the image
    # rather than translating it - across a 119-degree width the displacement
    # at the edges is nothing like the displacement at the centre - so the
    # patches stay near the middle third. And a patch can land on sky, or on a
    # vertical edge that lets the search slide freely; a single one of those
    # returns a confident wrong number, which is how this instrument's
    # horizontal ancestor produced four of them.
    xa, xb = int(w * 0.34), int(w * 0.66)
    ph = int(h * 0.10)
    patches = []
    for frac in (0.18, 0.28, 0.38, 0.48, 0.58, 0.68):
        top = int(h * frac)
        if top + ph + args.maxshift >= h:
            continue
        sd = float(base[top:top + ph, xa:xb].std())
        patches.append((top, sd))

    print("patches: cols %d..%d, %d px tall" % (xa, xb, ph))
    for top, sd in patches:
        print("  row %-5d sd %.1f%s" % (top, sd, "   (too flat, will be skipped)" if sd < 10.0 else ""))
    if not [p for p in patches if p[1] >= 10.0]:
        print("\nnothing with structure in the central band. Aim the camera at "
              "something with horizontal edges and capture again.")
        return 1
    print()

    # A faithful renderer, for comparison.
    focal_pred = (args.height / 2.0) / math.tan(math.radians(args.fovy / 2.0))
    print("a faithful renderer has focal %.1f px "
          "(viewport %d px tall at %.2f deg)\n" % (focal_pred, args.height, args.fovy))
    print("  %-8s %-7s %-10s %-12s %-10s %s"
          % ("pitch", "patch", "shift px", "predicted", "corr", "focal"))

    focals = []
    per_angle = {}
    for a in angles[1:]:
        img = np.array(Image.open(shots[a]).convert("L")).astype(float)
        if img.shape != base.shape:
            print("  %-8.1f capture size differs - skipped" % a)
            continue

        d_ang = a - base_a
        got = []

        for top, sd in patches:
            if sd < 10.0:
                continue

            # A pitch REPROJECTS; it does not translate. Content sitting y
            # pixels from the optical axis is at angle atan(y/f), and after the
            # camera turns by d_ang it sits at f*tan(atan(y/f) + d_ang). So the
            # displacement depends on WHERE in the frame the patch is, and on a
            # 121-degree image it varies by a factor of two from top to bottom.
            #
            # The first version of this compared every patch against
            # f*tan(d_ang) - the centre value - and rejected the measurement
            # because the patches "disagreed". They were agreeing with the
            # geometry; the predictor was wrong. Predicting per patch turns the
            # same five numbers into a result.
            y = (top + ph / 2.0) - (args.height / 2.0)
            alpha = math.atan(y / focal_pred)
            pred = focal_pred * (math.tan(alpha + math.radians(d_ang))
                                 - math.tan(alpha))
            band = base[top:top + ph, xa:xb]
            best = (-2.0, 0)
            for dy in range(-args.maxshift, args.maxshift + 1):
                ra, rb = top + dy, top + dy + ph
                if ra < 0 or rb > h:
                    continue
                c = ncc(band, img[ra:rb, xa:xb])
                if c > best[0]:
                    best = (c, dy)
            corr, dy = best

            why = ""
            if corr < 0.5:
                why = "  <- too weak"
            elif abs(dy) == args.maxshift:
                why = "  <- peak on the search wall"

            # The ratio of what moved to what should have moved. 1.000 means
            # the renderer's vertical scale is exactly what was asked for.
            ratio = (abs(dy) / abs(pred)) if abs(pred) > 1.0 else 0.0
            print("  %-8.1f %-7d %-10d %-12.1f %-10.3f %s%s"
                  % (a, top, dy, pred, corr, ("%.3f" % ratio) if not why else "-", why))
            if not why and ratio > 0.0:
                got.append((top, ratio))

        if len(got) >= 3:
            rs = np.array([g[1] for g in got])
            per_angle[a] = float(np.median(rs))
            focals.append(per_angle[a])
            print("           median ratio %.3f over %d patches (spread %.3f..%.3f)"
                  % (per_angle[a], len(rs), rs.min(), rs.max()))
        else:
            print("           fewer than three usable patches - angle rejected")

    print()
    if len(focals) < 2:
        print("fewer than two usable angles. This says nothing.")
        return 1

    f = np.array(focals)
    spread = f.max() - f.min()
    print("ratio from %d angles: %.3f, spread %.3f" % (len(f), f.mean(), spread))
    if spread > 0.15:
        print("  the angles DISAGREE by more than 0.15. A ratio that changes with"
              " the angle is not a field error; it is the measurement failing.")
        return 1

    ratio = float(f.mean())
    half = math.degrees(math.atan(math.tan(math.radians(args.fovy / 2.0)) * ratio))
    print("renderer produced half-fovY %.2f deg against %.2f asked  (tan ratio %.3f)"
          % (half, args.fovy / 2.0, ratio))
    print()
    if abs(ratio - 1.0) < 0.05:
        print("  FAITHFUL. The vertical field is not the fault either.")
    else:
        print("  NOT FAITHFUL. VRFovYScale should be %.3f so the asked vertical"
              " tangent comes out equal to the declared one." % (1.0 / ratio))
    return 0


if __name__ == "__main__":
    sys.exit(main())
