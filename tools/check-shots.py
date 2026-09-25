#!/usr/bin/env python3
"""Judge a directory of level captures: did each one actually DRAW?

  python tools/check-shots.py logs/shots-0920

WHY THIS IS NOT A BRIGHTNESS TEST. The obvious check is "is the frame black",
and it is wrong. M10S01 comes back at mean brightness 2.3 and is a fully drawn
night level - blue-lit stone, the view weapon at 1/19 ammo, the crosshair and
the HUD, correct in both eyes. A mean test calls that a failure, and a sweep
that reports four failures which are not failures is a sweep nobody reads, which
is exactly how a real one gets missed.

A DRAWN FRAME HAS EDGES. Neighbouring pixels differ, because there is geometry
and texture in it, and that holds at any brightness - a dark level is dim, not
flat. An undrawn one is a constant colour and its mean absolute gradient falls
to nearly nothing. That is the test.

AND THE TRUNCATION TEST NEEDS A SIZE. DPI virtualisation hands the capture a
half-size surface, so a 2560x1384 grab gets 1280x720 of real pixels and black
for the rest - always 664 dead rows, always from row 720. At 1280x692, which is
what the sweep now asks for, it cannot happen at all: the frame is shorter than
the cut. So dead rows at the bottom of a 692-tall capture are DARKNESS, not
truncation, and flagging them is the same false alarm in a different coat.
"""
import sys, os, glob
import numpy as np
from PIL import Image

# Below this mean absolute gradient a frame has no structure in it worth the
# name. Measured: the drawn levels in the 20 September sweep ran 0.70 (the
# darkest night level) to 12+; a flat frame is ~0.0.
EDGE_FLOOR = 0.40

# The DPI cut, in the only geometry where it occurs.
DPI_FULL_HEIGHT = 1384
DPI_CUT_ROW = 720


def judge(path):
    a = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    h, w = a.shape
    edge = float(np.abs(np.diff(a, axis=1)).mean() + np.abs(np.diff(a, axis=0)).mean())

    dead = 0
    dark_rows = (a < 12).mean(axis=1)
    for i in range(h - 1, -1, -1):
        if dark_rows[i] > 0.98:
            dead += 1
        else:
            break

    # Truncation is only possible in the full-size geometry, and there it cuts
    # at a known row. Anywhere else, a black band is the level being dark.
    truncated = (h >= DPI_FULL_HEIGHT * 0.9) and (h - dead) <= DPI_CUT_ROW + 8

    if truncated:
        return "TRUNCATED", edge, f"{dead} dead rows, cut at {h - dead}"
    if edge < EDGE_FLOOR:
        return "NO STRUCTURE", edge, "frame is flat - the level did not draw"
    return "ok", edge, ""


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "logs/shots"
    ps = sorted(glob.glob(os.path.join(d, "*.png")))
    if not ps:
        raise SystemExit(f"no captures in {d}")

    bad = []
    for p in ps:
        verdict, edge, why = judge(p)
        if verdict != "ok":
            bad.append((os.path.basename(p), verdict, edge, why))

    print(f"{len(ps)} captures in {d}")
    if not bad:
        print("all drew: no truncated frames, none flat")
    else:
        print(f"{len(bad)} SUSPECT:")
        for n, v, e, why in bad:
            print(f"  {n:28} {v:14} edge {e:5.2f}  {why}")

    # The five least structured, always - so a frame that is merely odd still
    # gets a human glance rather than passing silently.
    scored = sorted(((os.path.basename(p), judge(p)[1]) for p in ps), key=lambda r: r[1])
    print("\nleast structure (look at these by eye):")
    for n, e in scored[:5]:
        print(f"  {n:28} edge {e:5.2f}")

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
