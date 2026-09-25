"""Diff two sweeps of the whole game, level by level.

Every sweep this project had run measured 103 levels as the FIRST world of
their process, because `+runworld` gives each level a process to itself. That
is a structural blind spot rather than thin coverage: the renderer caches on
the engine's world POINTER, the allocator hands the same address back for the
next level, and a fault of that shape cannot appear until a second load. One
of them left every level after the first unlit and came through a clean
103-world sweep without a mark.

`sweep-levels.ps1 -Anchor` loads an anchor world first and the level under test
second. This reads that sweep against a first-world control taken with the same
binary, so the only thing that differs between the arms is which load the level
was.

    python tools/compare-sweeps.py logs/sweep-first-today logs/sweep-second

Both arguments are sweep-levels.ps1 -OutDir directories, holding <TAG>/renstub.log.

THE INTEGRITY CHECK COMES FIRST AND IS NOT OPTIONAL. In anchor mode a run that
stops too early records the ANCHOR's numbers under the name of the level under
test - plausible, well-formed, and wrong. The polygon total is read from the
level file and is a fingerprint of which world was actually measured, so if the
two arms disagree on it the row is thrown out rather than reported.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module

audit = import_module('audit-sweep')


def grid(txt):
    """Model light grid fill - the metric today's bug actually moved."""
    m = re.findall(r'MODEL LIGHT GRID: (\d+) of (\d+) cells carry light', txt)
    if not m:
        return None, None
    return int(m[-1][0]), int(m[-1][1])


def read(root):
    """<TAG>/renstub.log -> {level: parsed}."""
    out = {}
    if not os.path.isdir(root):
        sys.exit('no such sweep directory: %s' % root)
    for tag in sorted(os.listdir(root)):
        log = os.path.join(root, tag, 'renstub.log')
        if not os.path.exists(log):
            continue
        with open(log, 'r', encoding='utf-8', errors='replace') as fh:
            txt = fh.read()
        r = audit.one(txt)
        r['gridFill'], r['gridCells'] = grid(txt)
        out[tag] = r
    return out


# name, key, "higher is worse"
#
# ONLY METRICS THAT A FAULT MOVES REGARDLESS OF WHEN THE LOG WAS CUT.
#
# Most of these numbers are printed periodically as running totals, so their
# value depends on how many frames had elapsed when the sweep killed the level.
# That is fine for the ones above - a level with no untextured pieces reports 0
# at every sample, and a level with some reports some - but it is fatal for a
# raw instance count, which simply grows.
METRICS = [
    ('model pieces drawn untextured', 'white', True),
    ('sprites in view resolving nothing', 'sprNone', True),
    ('polygons dropped for no texture', 'notex', True),
    ('polygons UNACCOUNTED', 'unacct', True),
    ('models at or under 0.12 light', 'dark', True),
    ('model light grid cells filled', 'gridFill', False),
    ('polygons drawn', 'drawn', False),
]

# NOT COMPARED, AND THE REASON MATTERS.
#
# 'instances flagged invisible' is a running total sampled wherever the log
# stops, so it measures ELAPSED FRAMES rather than anything about the level.
# Compared across two arms whose runs are different lengths it produced "93
# levels better, 2 worse" and a headline 1 -> 1350 on M14S01, all of which was
# the clock. This project has been misled by a counter sampled once at least
# four times; naming it here is cheaper than being misled a fifth.
NOT_COMPARABLE = [
    ('instances flagged invisible', 'invis',
     'a running total - measures elapsed frames, not the level'),
]


def num(r, k):
    v = r.get(k)
    return v if isinstance(v, (int, float)) else None


def main(a_dir, b_dir):
    A, B = read(a_dir), read(b_dir)
    both = sorted(set(A) & set(B))

    print('FIRST world : %s   %d levels' % (a_dir, len(A)))
    print('SECOND world: %s   %d levels' % (b_dir, len(B)))
    print('%d levels in both\n' % len(both))

    only_a = sorted(set(A) - set(B))
    only_b = sorted(set(B) - set(A))
    for label, lst in (('only in the first-world arm', only_a),
                       ('only in the second-world arm', only_b)):
        if lst:
            print('%-46s %3d   %s' % (label, len(lst), ', '.join(lst[:6])))
    if only_a or only_b:
        print('')

    # ---- integrity: did each arm measure the same world? ---------------
    bad = []
    for lv in both:
        pa, pb = num(A[lv], 'polys'), num(B[lv], 'polys')
        if pa is not None and pb is not None and pa != pb:
            bad.append((lv, pa, pb))
    print('INTEGRITY  polygon total agrees on %d of %d levels'
          % (len(both) - len(bad), len(both)))
    for lv, pa, pb in bad[:12]:
        print('    MISMATCH %-32s first %s, second %s  <- different WORLD,'
              ' row discarded' % (lv, pa, pb))
    print('')

    discarded = set(lv for lv, _, _ in bad)
    good = [lv for lv in both if lv not in discarded]

    # ---- crashes -------------------------------------------------------
    ca = [lv for lv in good if A[lv].get('crash')]
    cb = [lv for lv in good if B[lv].get('crash')]
    print('CRASHES    first world %d, second world %d' % (len(ca), len(cb)))
    for lv in cb:
        if lv not in ca:
            print('    ONLY AS A SECOND WORLD: %s' % lv)
    print('')

    # ---- per metric ----------------------------------------------------
    for name, key, higher_worse in METRICS:
        # COVERAGE BEFORE COMPARISON. A metric absent from one arm compares
        # equal on every level and prints as "unchanged", which reads as an
        # all-clear and is the opposite of one.
        #
        # It is not hypothetical. The sweep kills a level the moment the
        # polygon account prints, which is BEFORE the client has published a
        # model or a sprite - so the first-world arm carried no white-piece,
        # sprite, invisible-instance or model-light numbers at all, while the
        # anchored arm did, because its process had been alive ten seconds
        # longer. Four of eight metrics reported "unchanged on every level"
        # on the strength of no data whatsoever.
        na = sum(1 for lv in good if num(A[lv], key) is not None)
        nb = sum(1 for lv in good if num(B[lv], key) is not None)
        if na == 0 or nb == 0:
            print('%-40s NOT MEASURED - present on %d/%d first, %d/%d second'
                  % (name, na, len(good), nb, len(good)))
            continue

        rows = []
        for lv in good:
            va, vb = num(A[lv], key), num(B[lv], key)
            if va is None or vb is None or va == vb:
                continue
            worse = (vb > va) if higher_worse else (vb < va)
            rows.append((lv, va, vb, worse))

        both_have = sum(1 for lv in good
                        if num(A[lv], key) is not None
                        and num(B[lv], key) is not None)
        if both_have < len(good):
            print('%-40s (comparable on %d of %d levels only)'
                  % ('  partial coverage:', both_have, len(good)))
        if not rows:
            print('%-40s unchanged on all %d compared levels'
                  % (name, both_have))
            continue
        w = [r for r in rows if r[3]]
        b_ = [r for r in rows if not r[3]]
        print('%-40s %d worse, %d better, of %d levels'
              % (name, len(w), len(b_), len(good)))
        for lv, va, vb, _ in sorted(w, key=lambda r: -abs(r[2] - r[1]))[:8]:
            print('    WORSE  %-32s %s -> %s' % (lv, va, vb))
        for lv, va, vb, _ in sorted(b_, key=lambda r: -abs(r[2] - r[1]))[:3]:
            print('    better %-32s %s -> %s' % (lv, va, vb))
    print('')

    for name, key, why in NOT_COMPARABLE:
        print('%-40s NOT COMPARED - %s' % (name, why))

    # ---- what is wrong in BOTH arms -----------------------------------
    #
    # The diff answers "does a second load make anything worse". It says
    # nothing about a fault that is equally present in both, which is exactly
    # what a level-order comparison hides: it cancels.
    print('\nSTANDING FAULTS - present in BOTH arms, nothing to do with order')
    for name, key, _ in METRICS[:4]:
        hits = [(lv, num(A[lv], key)) for lv in good
                if isinstance(num(A[lv], key), (int, float))
                and num(A[lv], key) > 0]
        if not hits:
            print('  %-38s none of %d levels' % (name, len(good)))
            continue
        print('  %-38s %d of %d levels' % (name, len(hits), len(good)))
        for lv, v in sorted(hits, key=lambda r: -r[1])[:6]:
            print('        %-40s %s' % (lv, v))
    print('')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
