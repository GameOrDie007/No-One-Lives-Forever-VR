#!/usr/bin/env python3
"""Rank levels by how big their sprites actually are on screen.

Reads the `SPRITE FOOTPRINT` line out of every renstub log a sweep produced
and sorts by the largest single sprite, which is the one that can settle a
question about the sprite size formula.

    python tools\\sprfoot.py logs\\sprsweep

"117 published, 117 resolved" is not the same question. Doubling every sprite
on the quick save moved 0.01 of a mean pixel because all 117 of them are
behind the camera or a few pixels across; a level where the largest subtends
a tangent of 0.3 is one where a factor of two is unmissable.
"""
import os
import re
import sys

PAT = re.compile(
    r"SPRITE FOOTPRINT:\s+(\d+) in front of the camera,"
    r"\s+summed half-angle tangent ([\d.]+),\s+LARGEST ([\d.]+) '([^']*)'")


def main(root):
    rows = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            if not f.endswith('.log'):
                continue
            path = os.path.join(dirpath, f)
            best = None
            try:
                with open(path, 'r', encoding='utf-8', errors='replace') as fh:
                    for line in fh:
                        m = PAT.search(line)
                        if not m:
                            continue
                        # The LAST report, not the first: the first is emitted
                        # before the level has settled, and an instrument that
                        # reads the startup has misled this project twice.
                        best = (int(m.group(1)), float(m.group(2)),
                                float(m.group(3)), m.group(4))
            except OSError:
                continue
            if best:
                rows.append((os.path.basename(dirpath), best))

    if not rows:
        print('no SPRITE FOOTPRINT lines under', root)
        print('(is the sweep still running, or was the renderer not ours?)')
        return 1

    rows.sort(key=lambda r: r[1][2], reverse=True)
    print('%-28s %6s %8s %8s  %s' % ('world', 'front', 'sum', 'largest', 'texture'))
    for name, (n, tot, mx, tex) in rows:
        print('%-28s %6d %8.3f %8.3f  %s' % (name, n, tot, mx, tex))
    print()
    print('%d levels reported; %d have a sprite over 0.05 across'
          % (len(rows), sum(1 for _, b in rows if b[2] > 0.05)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'logs/sprsweep'))
