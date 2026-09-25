"""One row per world for the things the 9 September night sweep was run for.

The older triage (sweep-triage.py) reads the polygon account, which the sweep
prints within two seconds of a load. This one reads the lines that only exist
once the client has published its objects - the world-model bridge, the door
baseline, the model account, the skin audit - which is why the night sweep
stops on 'WORLD MODELS MOVED' rather than on the account.

    python tools/night-triage.py logs/sweep-night
    python tools/night-triage.py logs/sweep-night --names     the unmatched/hidden names too
"""
import os
import re
import sys


def read(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def last(pat, txt, group=1, cast=int):
    ms = re.findall(pat, txt, re.M)
    if not ms:
        return None
    m = ms[-1]
    if isinstance(m, tuple):
        m = m[group - 1]
    try:
        return cast(m.replace(',', '')) if cast is int else cast(m)
    except (ValueError, AttributeError):
        return m


def one(d):
    r = {'world': os.path.basename(d.rstrip('\\/'))}
    ren = read(os.path.join(d, 'renstub.log'))
    cli = read(os.path.join(d, 'client.log'))
    r['polys'] = last(r'R3D ACCOUNT: the level has (\d+) polygons', ren)
    r['drawn'] = last(r'^\s+drawn\s+(\d+)', ren)
    r['unacct'] = last(r'^\s+UNACCOUNTED\s+(\d+)', ren)
    m = re.findall(r'WORLD MODELS MOVED: (\d+) of (\d+)', ren)
    r['wmMoved'], r['wmTracked'] = (int(m[-1][0]), int(m[-1][1])) if m else (None, None)
    r['seededAway'] = last(r'FIRST SEEN AWAY FROM THEIR AUTHORED PLACE: (\d+)', ren) or 0
    m = re.findall(r'WORLD MODELS WITH NO ENGINE OBJECT: (\d+)[^|]*\|\s*(.*)$', ren, re.M)
    r['unmatched'], r['unmatchedNames'] = (int(m[-1][0]), m[-1][1].strip()) if m else (0, '')
    m = re.findall(r'WORLD MODELS THE ENGINE HAS HIDDEN(?: AND WE STILL DRAW)?: (\d+) of (\d+)[^|]*\|\s*(.*)$', ren, re.M)
    r['hidden'], r['hiddenNames'] = (int(m[-1][0]), m[-1][2].strip()) if m else (0, '')
    m = re.findall(r'R3D MODEL ACCOUNT: (\d+) published, (\d+) dropped at the cap', ren)
    r['published'], r['dropped'] = (int(m[-1][0]), int(m[-1][1])) if m else (None, None)
    m = re.findall(r'SKINAUDIT: (\d+) of (\d+) drawn pieces have NO texture', ren)
    r['noSkin'], r['pieces'] = (int(m[-1][0]), int(m[-1][1])) if m else (None, None)
    m = re.findall(r'skipped: at-camera/player (\d+), dims (\d+), nodes (\d+), skin (\d+)', ren)
    r['skips'] = tuple(int(x) for x in m[-1]) if m else None
    r['census'] = last(r'VRCensus: the engine has (\d+) objects', cli)
    r['truncated'] = 'TRUNCATED' in cli
    r['crash'] = ('EXCEPTION' in ren or 'ACCESS_VIOLATION' in ren
                  or 'EXCEPTION' in cli or 'ACCESS_VIOLATION' in cli)
    r['noReport'] = r['wmTracked'] is None
    return r


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join('logs', 'sweep-night')
    names = '--names' in sys.argv
    rows = []
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if os.path.isdir(p):
            rows.append(one(p))
    print('%-32s %6s %6s %5s %5s %5s %6s %6s %6s %5s %s' % (
        'world', 'polys', 'drawn', 'wm', 'away', 'unmat', 'hidden', 'models', 'drop', 'noskn', 'flags'))
    for r in rows:
        flags = []
        if r['crash']: flags.append('CRASH')
        if r['noReport']: flags.append('NO-REPORT')
        if r['truncated']: flags.append('TRUNCATED')
        if r['unacct']: flags.append('UNACCT')
        if r['dropped']: flags.append('CAP')
        if r['skips'] and any(r['skips'][1:]): flags.append('SKIPS%s' % (r['skips'],))
        print('%-32s %6s %6s %5s %5s %5s %6s %6s %6s %5s %s' % (
            r['world'][:32], r['polys'], r['drawn'], r['wmTracked'], r['seededAway'],
            r['unmatched'], r['hidden'], r['published'], r['dropped'], r['noSkin'],
            ' '.join(flags)))
        if names and (r['unmatchedNames'] or r['hiddenNames']):
            if r['unmatchedNames']: print('      unmatched: ' + r['unmatchedNames'][:300])
            if r['hiddenNames']: print('      hidden:    ' + r['hiddenNames'][:300])
    n = len(rows)
    def cnt(f): return sum(1 for r in rows if f(r))
    print('')
    print('%d worlds' % n)
    print('  %d crashed' % cnt(lambda r: r['crash']))
    print('  %d never printed the world-model report' % cnt(lambda r: r['noReport']))
    print('  %d truncated the object scan' % cnt(lambda r: r['truncated']))
    print('  %d had world models first seen away from their authored place' % cnt(lambda r: r['seededAway']))
    print('  %d had world models with no engine object' % cnt(lambda r: r['unmatched']))
    print('  %d had hidden world models' % cnt(lambda r: r['hidden']))
    print('  %d dropped models at the cap' % cnt(lambda r: r['dropped']))
    print('  %d drew pieces with no skin' % cnt(lambda r: r['noSkin']))
    print('  %d left polygons unaccounted' % cnt(lambda r: r['unacct']))


if __name__ == '__main__':
    main()
