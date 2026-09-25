#!/usr/bin/env python3
"""Did anything change between two tours?

A tour is 65 levels of numbers. Reading two of them side by side is not
something a person will do, so this does it: same levels, same measurements,
and it prints only what moved.

The point is regression, not discovery. After a night of changes the question
is "is anything WORSE", and the honest way to answer it is to run the same
instrument twice and diff the output, rather than to look at a few pictures and
feel reassured.

    python tools/tour-compare.py logs/<tour-run-a> logs/<tour-run-b>

What counts as moved is deliberately loose on the noisy measurements (the
effects counts depend on where the tour happened to be standing when the report
printed) and tight on the ones that should be identical (polygons drawn,
untextured, missing frames).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_tri = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tour-triage.py')
_ns = {}
exec(compile(open(_tri).read().split("def main")[0], _tri, 'exec'), _ns)
level = _ns['level']

# key -> (label, how much movement matters). None means "any change".
WATCH = [
    ('drawn',      'polygons drawn',   0.02),
    ('untextured', 'untextured',       None),
    ('missing',    'missing frames',   None),
    ('nopicture',  'no picture',       None),
    ('skyobj',     'sky objects',      None),
    ('complete',   'tour completed',   None),
    ('watchdog',   'stalls',           None),
    ('exitfault',  'exit fault',       None),
]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    a_root, b_root = sys.argv[1], sys.argv[2]
    a_lv = {d for d in os.listdir(a_root) if os.path.isdir(os.path.join(a_root, d))}
    b_lv = {d for d in os.listdir(b_root) if os.path.isdir(os.path.join(b_root, d))}

    only_a = sorted(a_lv - b_lv)
    only_b = sorted(b_lv - a_lv)
    if only_a:
        print('only in %s: %s' % (a_root, ', '.join(only_a)))
    if only_b:
        print('only in %s: %s' % (b_root, ', '.join(only_b)))

    moved = 0
    for name in sorted(a_lv & b_lv):
        A = level(os.path.join(a_root, name))
        B = level(os.path.join(b_root, name))
        notes = []
        for key, label, tol in WATCH:
            x, y = A[key], B[key]
            if isinstance(x, bool) or tol is None:
                if x != y:
                    notes.append('%s %s -> %s' % (label, x, y))
            else:
                if x == 0 and y == 0:
                    continue
                base = max(abs(x), abs(y), 1)
                if abs(x - y) / float(base) > tol:
                    notes.append('%s %s -> %s' % (label, x, y))
        if notes:
            moved += 1
            print('%-32s %s' % (name[:32], '; '.join(notes)))

    print()
    print('%d levels compared, %d moved' % (len(a_lv & b_lv), moved))
    if not moved:
        print('nothing moved - the two runs agree on every measurement watched.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
