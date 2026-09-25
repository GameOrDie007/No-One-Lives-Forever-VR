#!/usr/bin/env python3
"""Where should an unattended tour STAND in a level?

Not the spawn point - that is one room of forty. Not a random point in the
bounding box either: most of a level's box is solid rock, and a camera inside
rock photographs black.

The level file already knows. Its object block holds the places the GAME
expects a person to stand:

    AINode        4365 across the campaign - where an AI stands, sits, guards,
                  or takes cover. Always inside a room, always on a floor.
    TeleportPoint  465 - where the game itself teleports the player.
    StartPoint          - where the player begins.

So the sample is the level's own furniture, and a stop that lands 64 units
from where it was asked for is a stop the world pushed away, which the client
logs as such.

SPREAD, not the first eight. Farthest-point sampling: take one, then
repeatedly take whichever candidate is furthest from everything taken so far.
Eight of those cover a level the way eight random ones do not.

FACING. A point on its own says nothing about which way to look, and half of
any level's AI nodes face a wall. Each stop faces the centroid of its nearest
neighbours - into the room the node belongs to, rather than out of it.

    python tools/tour-points.py WORLDS/M01S02.DAT 8        -> stdout
    python tools/tour-points.py WORLDS/M01S02.DAT 8 out.txt

Output is what the client's VRTour reads: "x y z yaw" per line.
"""
import math
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REZ = os.path.join(ROOT, 'game', 'NOLF.REZ')
DATDIR = os.path.join(ROOT, 'logs', 'dat')

# The classes worth standing at, most trustworthy first. AINode covers every
# level; the other two are sparse but are literally player positions.
WANT_PREFIX = ('AINode', 'TeleportPoint', 'StartPoint', 'PlayerLure')


# The campaign is in NOLF.REZ; the four GOTY bonus levels are NOT, and asking
# only NOLF.REZ for them came back empty - which reads as "this level has no AI
# nodes" rather than "wrong archive", and cost four levels of a night's tour.
ARCHIVES = ('NOLF.REZ', 'NolfGoty.rez', 'NOLF2.REZ', 'Nolfu003.rez',
            'Modernizer.rez')


def extract(path):
    """The level file, from whichever archive holds it."""
    os.makedirs(DATDIR, exist_ok=True)
    local = os.path.join(DATDIR, os.path.basename(path).upper())
    if os.path.exists(local) and os.path.getsize(local) > 0:
        return local
    for arch in ARCHIVES:
        full = os.path.join(ROOT, 'game', arch)
        if not os.path.exists(full):
            continue
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'rez.py'), full,
                        '--get', path.replace(chr(92), '/'), local],
                       capture_output=True, text=True)
        if os.path.exists(local) and os.path.getsize(local) > 0:
            return local
    return None


def candidates(dat):
    """Every (class, name, pos) in the object block whose class we want."""
    b = open(dat, 'rb').read()
    objpos = struct.unpack_from('<I', b, 4)[0]
    if objpos + 4 > len(b):
        return []
    nobj = struct.unpack_from('<I', b, objpos)[0]
    q = objpos + 4
    out = []
    for _ in range(nobj):
        if q + 2 > len(b):
            break
        rec = struct.unpack_from('<H', b, q)[0]
        nxt = q + 2 + rec
        r = q + 2
        cl = struct.unpack_from('<H', b, r)[0]
        r += 2
        cls = b[r:r + cl].decode('latin-1')
        r += cl
        nprop = struct.unpack_from('<I', b, r)[0]
        r += 4
        pos = None
        name = ''
        for _k in range(nprop):
            pl = struct.unpack_from('<H', b, r)[0]
            r += 2
            pname = b[r:r + pl].decode('latin-1')
            r += pl
            ptype = b[r]
            r += 1 + 4
            dl = struct.unpack_from('<H', b, r)[0]
            r += 2
            data = b[r:r + dl]
            r += dl
            if pname == 'Pos' and dl == 12:
                pos = struct.unpack_from('<3f', data, 0)
            elif pname == 'Name' and ptype == 0 and dl >= 2:
                sl = struct.unpack_from('<H', data, 0)[0]
                name = data[2:2 + sl].decode('latin-1')
        if pos and cls.startswith(WANT_PREFIX):
            out.append((cls, name, pos))
        q = nxt
    return out


def spread(points, n):
    """Farthest-point sampling. points is a list of (x, y, z)."""
    if not points:
        return []
    if len(points) <= n:
        return list(points)
    # Start from the one furthest from the centroid, so the first stop is a
    # corner rather than the middle of the busiest room.
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    cz = sum(p[2] for p in points) / len(points)

    def d2(a, b):
        return (a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2

    chosen = [max(points, key=lambda p: d2(p, (cx, cy, cz)))]
    best = [d2(p, chosen[0]) for p in points]
    while len(chosen) < n:
        i = max(range(len(points)), key=lambda k: best[k])
        if best[i] <= 0.0:
            break
        chosen.append(points[i])
        for k, p in enumerate(points):
            dd = d2(p, points[i])
            if dd < best[k]:
                best[k] = dd
    return chosen


def facing(p, others, k=6):
    """Yaw in degrees toward the centroid of the k nearest other points.

    NOLF's yaw is measured the way the engine's SetupEuler takes it: 0 looks
    down +Z, and it increases toward +X.
    """
    if not others:
        return 0.0
    near = sorted(others, key=lambda q: (q[0]-p[0])**2 + (q[1]-p[1])**2 + (q[2]-p[2])**2)[:k]
    cx = sum(q[0] for q in near) / len(near)
    cz = sum(q[2] for q in near) / len(near)
    dx, dz = cx - p[0], cz - p[2]
    if abs(dx) < 1e-3 and abs(dz) < 1e-3:
        return 0.0
    return math.degrees(math.atan2(dx, dz))


def points_for(world, n=8):
    dat = extract(world)
    if not dat:
        return [], 'no level file'
    cands = candidates(dat)
    if not cands:
        return [], 'no AI nodes or teleport points in the object block'
    pos = [c[2] for c in cands]
    picked = spread(pos, n)
    # Stand at head height above the node, not inside the floor it sits on.
    return [(p[0], p[1] + 32.0, p[2], facing(p, pos)) for p in picked], \
           '%d candidates (%s)' % (len(cands), ', '.join(sorted(set(c[0] for c in cands))[:4]))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    world = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    stops, note = points_for(world, n)
    lines = ['# %s - %s' % (world, note)]
    lines += ['%.1f %.1f %.1f %.1f' % s for s in stops]
    text = '\n'.join(lines) + '\n'
    if len(sys.argv) > 3:
        open(sys.argv[3], 'w').write(text)
        print('%s: %d stops -> %s  (%s)' % (world, len(stops), sys.argv[3], note))
    else:
        sys.stdout.write(text)
    return 0 if stops else 2


if __name__ == '__main__':
    sys.exit(main())
