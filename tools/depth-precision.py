"""How far apart must two surfaces be before the depth buffer can tell them apart?

This is the question behind the flickering, and it is arithmetic. No game run,
no screenshot, nothing to misread.

Three rendered experiments were tried first and all three were bad instruments,
which is why this exists:

  raising the near plane to build a "high precision reference" - at 50 units it
  clips the alleyway walls beside the camera, so the image change measured was
  clipping, not depth.

  nudging the head 0.05 degrees - the disputed pixels are window grilles and
  wall edges, which change under half a pixel of motion whatever the depth
  buffer does.

  nudging the far plane 0.1 % - shifts every depth by the same ABSOLUTE amount
  (zn/zf, about 4.4e-9). Conventional Z's values sit near 1.0 where a float ULP
  is 6e-8, so that shift is below the ULP and invisible; reversed Z's sit near
  1e-3 where the ULP is 1e-10, so the same shift is tens of ULPs. The
  perturbation is not the same size in the two arms.

The mappings, for a left-handed projection with the near plane at zn and the
far at zf:

  conventional   ndc = zf/(zf-zn) * (1 - zn/z)   -> near 0, far 1
  reversed       ndc = zn/(zn-zf) * (1 - zf/z)   -> near 1, far 0

Resolvable separation at view depth z is one ULP of the ndc value divided by
the rate at which ndc changes with z. Both mappings have the same rate,
zn/z**2 to first order; they differ entirely in WHERE the ndc value sits, and
therefore in how large an ULP is there.
"""

import math
import struct
import sys


def ulp(x):
    """The gap to the next representable float32 above x."""
    if x == 0.0:
        return struct.unpack("<f", struct.pack("<I", 1))[0]
    b = struct.unpack("<I", struct.pack("<f", abs(x)))[0]
    return abs(struct.unpack("<f", struct.pack("<I", b + 1))[0]) - abs(x)


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def resolvable(z, zn, zf, reversed_z):
    """Smallest depth difference at z that survives float32, in world units."""
    if reversed_z:
        ndc = f32(zn / (zn - zf) * (1.0 - zf / z))
    else:
        ndc = f32(zf / (zf - zn) * (1.0 - zn / z))
    # d(ndc)/dz is zn*zf/((zf-zn)*z**2) for both, up to sign.
    rate = abs(zn * zf / ((zf - zn) * z * z))
    if rate == 0.0:
        return float("inf")
    return ulp(ndc) / rate


def main():
    zn = float(sys.argv[1]) if len(sys.argv) > 1 else 0.44
    zf = float(sys.argv[2]) if len(sys.argv) > 2 else 100000.0

    # 1 unit is about 17 mm - VRUnitMM 17.02, measured by raycast against the
    # owner's own eye height.
    MM = 17.02

    print("near %.3f  far %.0f  (1 unit = %.2f mm, so near is %.1f mm)"
          % (zn, zf, MM, zn * MM))
    print()
    print("  depth       distance    conventional      reversed      reversed is")
    print("  (units)        (m)      resolves (mm)   resolves (mm)      better by")
    for z in (10, 50, 100, 250, 500, 1000, 2000, 5000):
        c = resolvable(z, zn, zf, False) * MM
        r = resolvable(z, zn, zf, True) * MM
        print("  %7d     %7.1f     %12.4f   %13.6f      %8.0fx"
              % (z, z * MM / 1000.0, c, r, c / r if r else 0))

    print()
    print("Read the conventional column as: two surfaces closer together than")
    print("this cannot be ordered, so whichever wins is decided by rounding and")
    print("flips with the smallest movement. That is Z-fighting, and it is what")
    print("streaks, triangles and Venetian-blind patterns are.")


if __name__ == "__main__":
    main()
