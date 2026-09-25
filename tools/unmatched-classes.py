"""Which CLASS is every world model the client has no engine object for?

The night sweep names, per level, the world models the renderer could not
match to a client object. Some of those are visible in retail (Terrain, the
water volumes) and some are objects ObjectRemover deleted at level start
(charges, barrels, switches). The level file's object block says which class
each name is, so this joins the two: extract each level from NOLF.REZ, walk
its object block for Name -> class, and tally the unmatched names by class.

    python tools/unmatched-classes.py logs/sweep-night
"""
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REZ = os.path.join(ROOT, 'game', 'NOLF.REZ')
DATDIR = os.path.join(ROOT, 'logs', 'dat')


def rez_list():
    """The sweep's own level list: WORLDS/MULTI/ASSAULTMAP/07HYDRO_AM.DAT per line."""
    with open(os.path.join(ROOT, 'tools', 'worlds.txt'), 'r', encoding='utf-8-sig') as f:
        return [ln.strip() for ln in f if ln.strip()]


def extract(path):
    os.makedirs(DATDIR, exist_ok=True)
    local = os.path.join(DATDIR, os.path.basename(path).upper())
    if not os.path.exists(local):
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'rez.py'), REZ,
                        '--get', path.replace('\\', '/'), local],
                       capture_output=True, text=True)
    return local if os.path.exists(local) else None


def classes(dat):
    b = open(dat, 'rb').read()
    objpos = struct.unpack_from('<I', b, 4)[0]
    if objpos + 4 > len(b):
        return {}
    nobj = struct.unpack_from('<I', b, objpos)[0]
    q = objpos + 4
    out = {}
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
        name = None
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
            if pname == 'Name' and ptype == 0 and dl >= 2:
                sl = struct.unpack_from('<H', data, 0)[0]
                name = data[2:2 + sl].decode('latin-1')
        if name:
            out[name] = cls
        q = nxt
    return out


def main():
    sweep = sys.argv[1] if len(sys.argv) > 1 else os.path.join('logs', 'sweep-night')
    # sweep folder = the path without .DAT, '/' -> '_'
    paths = {os.path.splitext(p)[0].replace('/', '_').replace(chr(92), '_').upper(): p for p in rez_list()}
    tally = Counter()
    examples = defaultdict(set)
    perlevel = []
    for d in sorted(os.listdir(sweep)):
        log = os.path.join(sweep, d, 'renstub.log')
        if not os.path.exists(log):
            continue
        txt = open(log, 'r', encoding='utf-8', errors='replace').read()
        m = re.findall(r'WORLD MODELS WITH NO ENGINE OBJECT: \d+[^|]*\|\s*(.*)$', txt, re.M)
        if not m:
            continue
        names = m[-1].split()
        stem = d.upper() if d.upper() in paths else None
        if not stem:
            perlevel.append((d, 'no DAT match', names))
            continue
        dat = extract(paths[stem])
        if not dat:
            perlevel.append((d, 'extract failed', names))
            continue
        cmap = classes(dat)
        row = []
        for n in names:
            c = cmap.get(n, '?')
            tally[c] += 1
            examples[c].add(n)
            row.append('%s:%s' % (n, c))
        perlevel.append((d, '', row))
    for d, err, row in perlevel:
        print('%-40s %s %s' % (d[:40], err, ' '.join(row)[:200]))
    print('')
    print('unmatched world models by class, all levels:')
    for c, n in tally.most_common():
        ex = sorted(examples[c])[:6]
        print('  %-24s %4d   e.g. %s' % (c, n, ' '.join(ex)))


if __name__ == '__main__':
    main()
