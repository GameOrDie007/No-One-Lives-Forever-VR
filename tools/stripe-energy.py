"""How much high-frequency VERTICAL striping is in a region of a capture.

The player's third symptom is streaking - triangles, Venetian-blind patterns,
lines in different patterns, constant flicker. On a wall seen at a
grazing angle that is texture aliasing: many texels fall inside one screen
pixel, and with no mip chain the sampler picks one of them, so the picture
beats against the texel grid and the pattern shifts as the camera moves.

The measurement is the energy in the column-to-column difference, divided by
the energy in the row-to-row difference. A real stone wall has structure in
both directions in similar amounts; aliasing of this kind is almost purely
vertical lines, so the ratio rises. Reporting a RATIO rather than an absolute
means a darker or brighter capture does not move the number.

    python stripe-energy.py <png> x0,y0,x1,y1 [more regions...]
"""

import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    im = np.asarray(Image.open(sys.argv[1]).convert("L"), dtype=float)
    for spec in sys.argv[2:]:
        x0, y0, x1, y1 = (int(v) for v in spec.split(","))
        r = im[y0:y1, x0:x1]
        if r.size == 0:
            print("  %-18s empty" % spec)
            continue
        # Column-to-column and row-to-row gradient energy.
        dx = np.diff(r, axis=1)
        dy = np.diff(r, axis=0)
        ex = float((dx * dx).mean())
        ey = float((dy * dy).mean())
        print("  %-18s vertical-stripe energy %7.1f, horizontal %7.1f, "
              "ratio %5.2f" % (spec, ex, ey, ex / ey if ey > 1e-6 else -1.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
