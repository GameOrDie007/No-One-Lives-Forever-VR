"""One row per world, out of a sweep's per-level renderer logs.

The sweep already writes down what the renderer made of every level. Nobody has
ever read all 103 of them side by side, so a fault that is obvious in the table
- half the batches unresolved, a light grid that is empty, an account that does
not add up - has never been visible as a PATTERN.

    python tools/sweep-triage.py logs/sweep-final
"""
import os
import re
import sys


def num(m, i=1):
    return int(m.group(i).replace(',', '')) if m else None


def read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def last(pat, txt):
    """THE LAST ACCOUNT, NOT THE FIRST.

    The world mesh is built before a single texture has been bound and then
    REBUILT once the engine has bound them. Build 1 of T03S01 drops 2582 of its
    3052 polygons for having no texture; build 2 drops three. Reading the first
    account in the log says fourteen levels of this game are missing most of
    their geometry, and it is not true - it is the state of the mesh for the
    fraction of a second before the rebuild.
    """
    ms = re.findall(pat, txt, re.M)
    return int(ms[-1].replace(',', '')) if ms else None


def one(txt):
    r = {}
    # AND ONLY INSIDE THAT BLOCK. "drawn" and "UNACCOUNTED" are column labels
    # several reports use, so a search over the whole log finds the model
    # account's numbers and files them under the world's.
    parts = txt.split('R3D ACCOUNT: the level has')
    r['builds'] = len(parts) - 1
    blk = parts[-1][:1500] if r['builds'] else ''
    m = re.match(r'\s*(\d+) polygons', blk)
    r['polys'] = num(m)
    r['drawn'] = last(r'^\s+drawn\s+(\d+)', blk)
    r['unacct'] = last(r'^\s+UNACCOUNTED\s+(\d+)', blk)
    r['notex'] = last(r'^\s+no texture, dropped\s+(\d+)', blk)
    # LAST occurrence: the batch list grows as textures arrive.
    ms = re.findall(r'resolved (\d+) of (\d+) batches to a texture', txt)
    if ms:
        r['res'], r['batches'] = int(ms[-1][0]), int(ms[-1][1])
    ms = re.findall(r'SKY DRAWN: (\d+) vertices', txt)
    r['sky'] = int(ms[-1]) if ms else 0
    m = re.search(r'LIGHT GRID: (\d+) of (\d+) cells carry light', txt)
    if m:
        r['lit'], r['cells'] = int(m.group(1)), int(m.group(2))
    r['crash'] = 'EXCEPTION' in txt or 'ACCESS_VIOLATION' in txt
    return r


def main(root):
    rows = []
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d, 'renstub.log')
        if not os.path.isfile(p):
            p2 = os.path.join(root, d + '-renstub.log')
            if os.path.isfile(p2):
                p = p2
            else:
                continue
        rows.append((d, one(read(p))))

    print('%-34s %8s %8s %7s %6s %9s %7s %s' %
          ('world', 'polys', 'drawn', 'unacct', 'notex', 'batches', 'sky',
           'lightgrid'))
    for d, r in rows:
        b = ('%d/%d' % (r['res'], r['batches'])) if 'batches' in r else '-'
        g = ('%d/%d' % (r['lit'], r['cells'])) if 'cells' in r else 'NONE'
        print('%-34s %8s %8s %7s %6s %9s %7s %s%s' % (
            d[:34], r['polys'], r['drawn'], r['unacct'], r['notex'], b,
            r['sky'] or 'NONE', g, '   CRASH' if r['crash'] else ''))

    # And the same thing as a count of levels, which is the number that says
    # whether a fault is one level's problem or the game's.
    n = len(rows)
    def cnt(f):
        return sum(1 for _, r in rows if f(r))
    print('')
    print('%d worlds' % n)
    print('  %d drew no sky' % cnt(lambda r: not r['sky']))
    print('  %d have no light grid' % cnt(lambda r: 'cells' not in r))
    print('  %d left polygons UNACCOUNTED' % cnt(lambda r: r['unacct']))
    print('  %d dropped polygons for having no texture'
          % cnt(lambda r: r['notex']))
    print('  %d failed to resolve every batch to a texture'
          % cnt(lambda r: 'batches' in r and r['res'] != r['batches']))
    print('  %d crashed' % cnt(lambda r: r['crash']))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'logs/sweep-final')
