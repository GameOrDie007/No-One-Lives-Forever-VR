"""Where does firing ADD light, per weapon? Located, not eyeballed.

WHY DIFFERENCING AND NOT LOOKING. Twice today I read a frame wrong and said so
confidently: once calling a travelling tracer a wrong muzzle origin, once calling
the HUD ammo counter a muzzle light sitting a metre off the barrel. A tracer
moves between the shot and the shutter, and a still cannot tell that from a bad
origin. A difference against the SAME WEAPON not firing can: whatever firing adds
is exactly the flash, the tracer, the casings and the light, and nothing else.

TWO TRAPS, BOTH PAID FOR:
  - Difference against a DIFFERENT weapon and the HUD differs too. The ammo
    counter is green, sits at a fixed place, and a Lighter has no ammo to count,
    so it showed up as a fat blob at (540,358) for all seven weapons tested -
    identical whether the muzzle was at 24.95 units or 56.95. Hence HUD_MASK.
  - The gun RECOILS, so its own pixels move a little between the two frames. The
    flash is far brighter than that, so take the brightest blob rather than every
    changed pixel.

The marker is drawn on the NON-FIRING frame, because that is the one where the
barrel is not hidden behind its own flash.
"""
import sys, glob, os
import numpy as np
from PIL import Image, ImageDraw

# The ammo counter, left eye. Measured, not guessed: see the docstring.
HUD_MASK = (470, 330, 660, 385)
# THE IMPACT IS BRIGHTER THAN THE MUZZLE, AND IT IS NOT THE MUZZLE.
# Every weapon shoots the same wall, so the first pass returned (344,243) peak
# 235 for fifteen of seventeen - including the LIGHTER, which does not shoot.
# That is the impact sparkle on the statue alcove. So do not take one blob:
# take several, keep them apart, and let the caller see both.
BLOB_SEP = 80
# Left eye only. The right eye is the same scene and doubles every blob.
EYE = (0, 0, 640, 692)


def locate(fire_png, ref_png, want=3):
    f = np.asarray(Image.open(fire_png).convert("RGB"), dtype=np.float32)
    r = np.asarray(Image.open(ref_png).convert("RGB"), dtype=np.float32)
    if f.shape != r.shape:
        return []
    d = (f - r).sum(axis=2) / 3.0          # luminance ADDED by firing
    x0, y0, x1, y1 = EYE
    m = np.zeros(d.shape, dtype=bool)
    m[y0:y1, x0:x1] = True
    hx0, hy0, hx1, hy1 = HUD_MASK
    m[hy0:hy1, hx0:hx1] = False
    d = np.where(m, d, 0.0)

    yy, xx = np.mgrid[0:d.shape[0], 0:d.shape[1]]
    out = []
    work = d.copy()
    for _ in range(want):
        peak = float(work.max())
        if peak < 12.0:
            break
        py, px = np.unravel_index(int(np.argmax(work)), work.shape)
        near = ((yy - py) ** 2 + (xx - px) ** 2) < 70 ** 2
        sel = near & (work > peak * 0.35)
        w = work[sel]
        cy = float((yy[sel] * w).sum() / w.sum())
        cx = float((xx[sel] * w).sum() / w.sum())
        out.append(dict(peak=peak, cx=cx, cy=cy, n=int(sel.sum())))
        # blank this blob out so the next pass finds a DIFFERENT one
        work = np.where(((yy - py) ** 2 + (xx - px) ** 2) < BLOB_SEP ** 2, 0.0, work)
    return out


def mark(ref_png, out_png, res, label):
    im = Image.open(ref_png).convert("RGB").crop((0, 0, 640, 692))
    d = ImageDraw.Draw(im)
    if res and res.get("found"):
        cx, cy = res["cx"], res["cy"]
        for rr in (16, 17, 26, 27):
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=(255, 40, 40))
        d.line([cx - 34, cy, cx - 20, cy], fill=(255, 40, 40), width=2)
        d.line([cx + 20, cy, cx + 34, cy], fill=(255, 40, 40), width=2)
        txt = f"{label}  flash at ({cx:.0f},{cy:.0f})  peak {res['peak']:.0f}"
    else:
        txt = f"{label}  NO ADDED LIGHT (peak {res['peak']:.0f} if any)" if res else f"{label}  size mismatch"
    d.rectangle([0, 0, 639, 20], fill=(0, 0, 0))
    d.text((5, 5), txt, fill=(255, 255, 0))
    im.save(out_png)
    return txt


if __name__ == "__main__":
    names = {}
    tsv = "logs/muzzle-table.tsv"
    if os.path.exists(tsv):
        for line in open(tsv, encoding="utf-8-sig").read().splitlines()[1:]:
            p = line.split("	")
            if len(p) > 4 and p[0].isdigit():
                names[int(p[0])] = f"{p[1]} {p[4]}"
    for i in range(1, 18):
        fire = f"logs/wsweep/v{i:02d}.png"
        ref = f"logs/wsweep/n{i:02d}.png"
        if not (os.path.exists(fire) and os.path.exists(ref)):
            print(f"  {i:2}  missing"); continue
        blobs = locate(fire, ref)
        lab = names.get(i, f"weapon {i}")
        desc = "  ".join(f"({b['cx']:.0f},{b['cy']:.0f}) pk{b['peak']:.0f}" for b in blobs)
        print(f"  {i:2}. {lab:28} {desc}")
