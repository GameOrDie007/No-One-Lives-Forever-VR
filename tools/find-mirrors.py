#!/usr/bin/env python3
"""find-mirrors.py - every mirror face in every level, and where to stand to see it.

A level marks a mirror with the surface effect "mirror" (see dat.py surfaces()).
For each one this prints the world model it is on, its centre, its facing
normal, its area, and a desk-harness stop in front of it: the position 160
units out along the normal at eye height, and the yaw that faces the glass
(yaw 0 looks down +z, 90 down +x, the convention of vrtour.txt).

    python tools/find-mirrors.py                  all levels
    python tools/find-mirrors.py WORLDS/M06S01.DAT
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dat


def newell(pts):
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        n[0] += (a[1] - b[1]) * (a[2] + b[2])
        n[1] += (a[2] - b[2]) * (a[0] + b[0])
        n[2] += (a[0] - b[0]) * (a[1] + b[1])
    ln = math.sqrt(sum(c * c for c in n))
    return [c / ln for c in n] if ln > 0 else None, ln / 2.0


def mirrors(w):
    for m in w.models:
        try:
            surfs = list(m.surfaces())
        except Exception:
            continue
        if not any(s["mirror"] for s in surfs):
            continue
        polys = m.polygons()
        if not polys:
            yield m.name, None
            continue
        V = [dat.struct.unpack_from("<3f", w.b, m.find_points() + i * 24) for i in range(m.n_points)]
        for p in polys:
            if not surfs[p["surface"]]["mirror"]:
                continue
            pts = [V[i] for i in p["points"]]
            n, area = newell(pts)
            c = [sum(q[k] for q in pts) / len(pts) for k in range(3)]
            yield m.name, (c, n, area)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = os.path.join(root, "game")
    files = dat.mounted(game)
    names = [a.replace("\\", "/").upper() for a in sys.argv[1:]] or sorted(files)
    for key in names:
        if not key.startswith("WORLDS/"):
            continue
        w = dat.World(files[key].read(key), key)
        rows = list(mirrors(w))
        if not rows:
            continue
        print(key)
        for name, r in rows:
            if r is None:
                print(f"  {name}: mirror surfaces, polygons did not parse")
                continue
            c, n, area = r
            if n is None:
                continue
            hx, hz = n[0], n[2]
            hl = math.hypot(hx, hz) or 1.0
            sx, sz = c[0] + hx / hl * 160.0, c[2] + hz / hl * 160.0
            yaw = math.degrees(math.atan2(-hx, -hz))
            print(f"  {name:24s} centre ({c[0]:.0f} {c[1]:.0f} {c[2]:.0f})  normal ({n[0]:+.2f} {n[1]:+.2f} {n[2]:+.2f})"
                  f"  area {area:.0f}  -> stand ({sx:.0f} {c[1]:.0f} {sz:.0f}) yaw {yaw:.0f}")


if __name__ == "__main__":
    main()
