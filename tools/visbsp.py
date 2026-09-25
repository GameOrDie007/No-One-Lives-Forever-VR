"""VisBSP: one displacement, before the planes, and the whole model falls out.

VisBSP is the visible world, so stage 2 cannot draw a level without it, and it
was the one model this project could not read - 103 of 103 worlds.

THE DIAGNOSIS WAS WRONG FOR DAYS, and the wrong diagnosis was the expensive
part. The note said "VisBSP's surface array is not after its planes". The
surfaces ARE after its planes. **Its PLANES are not where the counts say** - 0
of M01S02's 1688 normals are unit length at the computed offset, against 100%
for every other model in the file - and the surfaces, the points and the
polygons are all measured from the planes, so one displacement made four arrays
wrong and each looked like its own problem.

Nobody checked the planes because nothing downstream had ever needed checking.
The lesson generalises: when several arrays in a record are wrong at once, suspect
the one they are all measured from, not each of them in turn.

LOCATING IT. A plane is {normal, dist} and the normal is UNIT LENGTH, so
n_planes of them in a row at stride 16 is not something other data does by
accident - the same criterion, and the same strength, as find_points. On
M01S02's VisBSP exactly one offset in the entire model satisfies it, 273505
bytes past the computed position.

AND IT CHECKS ITSELF, four times over, against things the scan never looked at:

    the surfaces sit EXACTLY at the located planes end - 273505 again, the
      same number, which is what says one block was inserted rather than two
      things being independently wrong
    surface_check() passes: texture indices exactly {0..166}, planes in range
    the flags collapse from 1177 distinct values to 14, the same small
      vocabulary every other model uses, with Sky.dtx alone on 110 and
      Invisible.dtx and occluder.dtx alone on 202
    ALL 5957 POLYGONS parse with the ordinary 10-byte header and their own
      Newell normals match the planes their surfaces name

Nothing was fitted to make any of that happen.

NO FORMULA WILL GIVE THE GAP, so do not go looking for one. An earlier note fitted
`10*nodes + c7 + 4*c8 + 4*c10` and called the leftover "one small term
unidentified". Solved exactly over all twelve counts plus a constant on 12
worlds, it fails out of sample on 78 of the other 90, and the leftover is not a
multiple of any count. There is variable-length data in that block.

    python tools/visbsp.py                    M01S02: the before and after
    python tools/visbsp.py sweep              every world
    python tools/visbsp.py WORLDS/M13S02.DAT  one named world
"""

import collections
import sys

import dat

GAME = "<repo>/game"


def vocabulary(m, plane_base):
    """The flag values read with the planes at a given offset.

    The check that had no part in the search: the scan looked only at plane
    NORMALS, and this reads a field three arrays away.
    """
    keep = m.plane_start
    m.plane_start = plane_base
    fl = collections.Counter()
    tex = collections.defaultdict(set)
    try:
        for s in m.surfaces():
            fl[s["flags"]] += 1
            if s["texture"] < len(m.textures):
                tex[s["flags"]].add(m.textures[s["texture"]].split(chr(92))[-1])
    except Exception:
        pass
    m.plane_start = keep
    return fl, tex


def report(path, quiet=False):
    w = dat.load(GAME, path)
    try:
        m = w["VisBSP"]
    except KeyError:
        return None
    delta = m.plane_start - m.plane_assumed
    ok = m.surface_check()
    polys = m.polygons() if ok else None
    if not quiet:
        state = "OK" if polys and len(polys) == m.n_polygons else "surfaces only" \
            if ok else "NOT LOCATED"
        print(f"{path:26s} planes {delta:+9d}   surfaces {'ok' if ok else 'no':3s}"
              f"   polygons {len(polys) if polys else 0:6d}/{m.n_polygons:<6d} {state}")
    return dict(model=m, delta=delta, ok=ok,
                polys=len(polys) if polys else 0)


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "WORLDS/M01S02.DAT"
    if arg == "sweep":
        full = part = none = 0
        for p in sorted(dat.mounted(GAME)):
            try:
                r = report(p)
            except Exception as exc:
                print(f"{p:26s} FAILED: {exc}")
                none += 1
                continue
            if r is None:
                continue
            if r["polys"] == r["model"].n_polygons:
                full += 1
            elif r["ok"]:
                part += 1
            else:
                none += 1
        print()
        print(f"  {full} VisBSPs fully parse, {part} surfaces only, {none} not located")
        return

    r = report(arg)
    if not r:
        return
    m = r["model"]
    for label, base in (("computed, straight after the polygon sizes",
                         m.plane_assumed),
                        ("located", m.plane_start)):
        fl, tex = vocabulary(m, base)
        print()
        print(f"  planes {label}: {len(fl)} distinct flag values over "
              f"{m.n_surfaces} surfaces")
        for v, c in fl.most_common(8):
            names = sorted(tex[v])
            shown = ", ".join(names[:4]) + (f" (+{len(names) - 4} more)"
                                            if len(names) > 4 else "")
            print(f"    flags {v:6d}  {c:6d} surfaces   {shown}")


if __name__ == "__main__":
    main()
