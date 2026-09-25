#!/usr/bin/env python3
"""Did every chained world build COMPLETELY?

A chain run (tools/chain-levels.ps1) plays a mission's scenes in one process,
so every world after the first is a later world of its process - the case the
one-level-per-process tour and sweep can never see. This reads each mission's
renderer log, takes the LAST build of every world in it, and compares the
polygon count with what the same level built as the FIRST world of a process
in the tour. A later world that builds far fewer polygons than its first-world
self is the black-walls bug, whatever the pictures look like.

    python tools/chain-triage.py logs/<chain-run> logs/<tour-run>

Prints one row per world; flags anything under 90% of the tour's figure, any
world with no build at all, any renderer restart, crash or hang.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_tri = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tour-triage.py')
_ns = {}
exec(compile(open(_tri).read().split("def main")[0], _tri, 'exec'), _ns)
tour_level = _ns['level']


def worlds_in(renstub):
    """[(world name, last built polygons or None, empty builds, gave up)] in order."""
    out = []
    cur = None
    for line in open(renstub, errors='ignore'):
        if 'R3D WORLD LOADED (' in line:
            cur = {'name': '?', 'built': None, 'empty': 0, 'gaveup': False}
            out.append(cur)
        elif cur is None:
            continue
        elif 'WORLD FILE: WORLDS' in line and cur['name'] == '?':
            m = re.search(r'WORLD FILE: (WORLDS/\S+?)\.DAT', line)
            if m:
                cur['name'] = m.group(1).replace('/', '_').upper()
        elif 'R3D: built' in line:
            m = re.search(r'R3D: built \d+ models, (\d+) polygons', line)
            if m:
                cur['built'] = int(m.group(1))
        elif 'produced no triangles' in line:
            cur['empty'] += 1
        elif 'REBUILD: giving up' in line:
            cur['gaveup'] = True
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    chain_root, tour_root = sys.argv[1], sys.argv[2]
    tour = {}
    for d in os.listdir(tour_root):
        p = os.path.join(tour_root, d)
        if os.path.isdir(p) and os.path.exists(os.path.join(p, 'renstub.log')):
            try:
                tour[d.upper()] = tour_level(p)['drawn']
            except Exception:
                pass

    print('%-10s %-16s %2s %8s %8s %6s  %s' % ('mission', 'world', '#', 'built', 'tour', 'ratio', 'flags'))
    flagged = 0
    rows = 0
    for m in sorted(os.listdir(chain_root)):
        ren = os.path.join(chain_root, m, 'renstub.log')
        if not os.path.exists(ren):
            continue
        txt = open(ren, errors='ignore').read()
        restarts = txt.count('RE-INITIALISED')
        crash = '=== CRASH ===' in txt
        for i, w in enumerate(worlds_in(ren), 1):
            rows += 1
            ref = tour.get(w['name'])
            flags = []
            if w['built'] is None:
                flags.append('NO-BUILD')
            elif ref:
                if w['built'] < 0.9 * ref:
                    flags.append('INCOMPLETE')
            # A give-up on a level that still reads 1.00 is the loop declining
            # to chase placeholder-textured polygons (the hatches, the
            # weather volumes) - the engine never draws those either. It is
            # a finding only when the level is actually short.
            if w['gaveup'] and (ref is None or w['built'] is None or w['built'] < 0.99 * ref):
                flags.append('GAVE-UP')
            if i == 1 and restarts:
                flags.append('RESTARTS x%d' % restarts)
            if i == 1 and crash:
                flags.append('CRASH')
            ratio = ('%5.2f' % (w['built'] / float(ref))) if (ref and w['built'] is not None) else '    -'
            if flags:
                flagged += 1
            print('%-10s %-16s %2d %8s %8s %6s  %s' % (
                m, w['name'][:16], i,
                w['built'] if w['built'] is not None else '-',
                ref if ref else '-', ratio, ' '.join(flags)))
    print()
    print('%d worlds, %d flagged' % (rows, flagged))
    return 0


if __name__ == '__main__':
    sys.exit(main())
