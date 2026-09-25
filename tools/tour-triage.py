#!/usr/bin/env python3
"""Read a night's auto-tour and say which levels are worth looking at.

Sixty levels x eight stops is 480 pictures and 120 logs. Nobody reads that,
so this reads it: per level it pulls the numbers the renderer and client
already print, measures every screenshot, and prints only what stands out
against the rest of the game.

WHAT IT MEASURES, and why each one is a real fault when it is odd:

  untextured   polygons the walk could not texture. A handful is normal
               (markers, sky edges); hundreds is a level drawing white.
  no-picture   effect runs whose sprite or texture would not load.
  missing      sprite-animation frames that were not on disk.
  drawn/total  world polygons drawn against the level's own count. A level
               far below the others is hiding geometry.
  effects      runs and vertices in the effects pass - a level with steam
               objects and zero effect runs is not drawing them.
  lights       dynamic lights published. Zero everywhere is suspicious;
               zero in a level with flicker lamps is a fault.
  sky          sky objects the pointers named. A level with pointers and
               no sky objects drew a flat box.
  pushed       tour stops the world shoved more than 64 units. Many means
               the sample points are inside geometry, not that the level is
               broken - it is a note about the instrument.

  THE PICTURES: each is measured for mean brightness, for how much of it is
  the clear colour (magenta - inherited pixels), near-black or near-white,
  and whether it is a duplicate of the previous stop (a teleport that did
  not move). A stop that is 90% black is either inside a wall or a level
  that did not draw.

    python tools/tour-triage.py logs/tour
    python tools/tour-triage.py logs/tour --all      every level, not just odd
"""
import os
import re
import sys
from collections import OrderedDict

try:
    from PIL import Image
except ImportError:
    Image = None


def read(path):
    try:
        with open(path, 'r', errors='ignore') as f:
            return f.read()
    except OSError:
        return ''


def last_int(text, pattern, group=1, default=0):
    m = list(re.finditer(pattern, text))
    if not m:
        return default
    try:
        return int(m[-1].group(group))
    except (ValueError, IndexError):
        return default


def shot_stats(path):
    """mean, %black, %white, %magenta of one screenshot, cheaply."""
    if Image is None:
        return None
    try:
        im = Image.open(path).convert('RGB')
    except Exception:
        return None
    im = im.resize((160, 90))
    px = list(im.getdata())
    n = float(len(px))
    black = white = magenta = 0
    tot = 0
    for r, g, b in px:
        tot += r + g + b
        if r < 18 and g < 18 and b < 18:
            black += 1
        elif r > 240 and g > 240 and b > 240:
            white += 1
        if r > 200 and b > 200 and g < 60:
            magenta += 1
    return {
        'mean': tot / (n * 3.0),
        'black': 100.0 * black / n,
        'white': 100.0 * white / n,
        'magenta': 100.0 * magenta / n,
        'sig': px[::97],           # a cheap fingerprint, for duplicate stops
    }


def level(dirpath):
    ren = read(os.path.join(dirpath, 'renstub.log'))
    cli = read(os.path.join(dirpath, 'client.log'))
    d = OrderedDict()
    d['untextured'] = last_int(ren, r'of those (\d+) untextured polygons')
    d['nopicture'] = last_int(ren, r'(\d+) with no picture')
    d['missing'] = last_int(ren, r'\((\d+) frames, (\d+) missing\)', group=2)
    # THE ACCOUNT, READ THE WAY IT IS PRINTED. This used to look for "N / M"
    # anywhere after the words R3D ACCOUNT, and the account does not contain a
    # slash - so it matched whatever ratio appeared later in the log and
    # reported it as the polygon count. Two tours of the same 65 levels then
    # "differed" on levels that had not changed at all. The real shape is:
    #     R3D ACCOUNT: the level has 26904 polygons (from the level file)
    #        drawn                       18105
    # THE PAIR, TOGETHER. "drawn" on its own appears in the sky account and in
    # per-polygon dumps too, so taking the last one reported 128, or 21. The
    # only reliable anchor is the account's own header immediately above it:
    #     R3D ACCOUNT: the level has 26904 polygons (from the level file)
    #        drawn                       18105
    # The account is printed once per world build, so the LAST pair is the
    # settled one.
    acc = list(re.finditer(
        r'R3D ACCOUNT: the level has (\d+) polygons[^\n]*\n\s+drawn\s+(\d+)', ren))
    d['total'] = int(acc[-1].group(1)) if acc else 0
    d['drawn'] = int(acc[-1].group(2)) if acc else 0
    d['fxruns'] = last_int(ren, r'effects frame \d+: (\d+) runs')
    d['fxverts'] = last_int(ren, r'effects frame \d+: \d+ runs, (\d+) verts')
    d['lights'] = last_int(ren, r'dynamic lights: (\d+) this frame')
    # DISTINCT sky objects, not log lines: a world built twice in one run logs
    # each of them twice, which read as the sky changing between tours.
    d['skyobj'] = len(set(re.findall(r'R3D SKY OBJECT: (\S+)', ren)))
    # POINTERS THAT NAME SOMETHING OTHER THAN THE BOX. Most levels have one
    # pointer and it names SkyBox, which the original sky path already draws -
    # counting those as "layers that did not join" flagged half the game.
    d['skyptr'] = 0
    msky = list(re.finditer(r'WORLD SKY: \d+ sky pointers name ([^;]+);', ren))
    if msky:
        names = [n.strip() for n in msky[-1].group(1).split(',')]
        d['skyptr'] = len([n for n in names if not n.lower().startswith('skybox')])
    # Did the effects report print at all? Under about fifteen seconds it does
    # not, and "no effects" then means "no report", not "nothing drawn".
    d['fxreported'] = 'effects frame' in ren
    d['visited'] = len(re.findall(r'VRTour: stop \d+ of', cli))
    d['pushed'] = len(re.findall(r'MOVED BY THE WORLD', cli))
    d['complete'] = 'VRTour: COMPLETE' in cli
    mw = list(re.finditer(r'VRTour: WAITING - the game state is (\d+)', cli))
    d['waitstate'] = mw[-1].group(1) if mw else ''
    # A STALL WORTH THE NAME. The watchdog fires at any wait past a frame, and
    # a level's own mesh build takes 100 ms or more - flagging that flags
    # loading, which is not a fault. Half a second, and not while building.
    d['watchdog'] = 0
    for m in re.finditer(r"WATCHDOG: main thread (\d+) ms past its last present, in .([^.]*)", ren):
        if int(m.group(1)) >= 500 and 'building the world' not in m.group(2):
            d['watchdog'] += 1
    d['exitfault'] = 'did not stop in ten seconds' in ren
    # particle systems / polygrids the client saw, so "no effects" can be told
    # from "nothing to draw".
    d['psseen'] = last_int(cli, r'(\d+) particle systems seen')
    d['pgseen'] = last_int(cli, r'(\d+) polygrids \(')
    return d


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join('logs', 'tour')
    show_all = '--all' in sys.argv
    dirs = sorted(d for d in os.listdir(root)
                  if os.path.isdir(os.path.join(root, d)))
    if not dirs:
        print('no level directories under', root)
        return 1

    rows = []
    for name in dirs:
        p = os.path.join(root, name)
        d = level(p)
        shots = sorted(f for f in os.listdir(p) if f.endswith('.png'))
        stats, dupes, dark, blank = [], 0, 0, 0
        prev = None
        for f in shots:
            st = shot_stats(os.path.join(p, f))
            if not st:
                continue
            stats.append((f, st))
            if st['black'] > 85.0:
                dark += 1
            if st['white'] > 85.0 or st['magenta'] > 20.0:
                blank += 1
            if prev is not None and st['sig'] == prev:
                dupes += 1
            prev = st['sig']
        d['shots'] = len(shots)
        d['dark'] = dark
        d['blank'] = blank
        d['dupes'] = dupes
        d['meanlum'] = sum(s['mean'] for _, s in stats) / len(stats) if stats else 0.0
        d['worst'] = max(stats, key=lambda s: s[1]['black'])[0] if stats else ''
        rows.append((name, d))

    # What counts as odd, judged against the whole set rather than a guess.
    def med(key):
        vals = sorted(r[1][key] for r in rows)
        return vals[len(vals) // 2] if vals else 0
    med_untex = med('untextured')

    print('%-34s %6s %5s %5s %5s %5s %6s %5s %5s  %s' %
          ('level', 'untex', 'nopic', 'miss', 'shots', 'dark', 'meanlum',
           'fxrun', 'sky', 'flags'))
    n_flagged = 0
    for name, d in rows:
        flags = []
        if d['exitfault']:
            flags.append('EXIT-FAULT')
        if d['watchdog']:
            flags.append('STALL x%d' % d['watchdog'])
        if not d['complete']:
            # Say WHY, when the client said. A cutscene is not a fault.
            flags.append('TOUR-INCOMPLETE' + (('(state %s)' % d['waitstate']) if d['waitstate'] else ''))
        if d['untextured'] > max(50, med_untex * 8):
            flags.append('UNTEXTURED')
        if d['nopicture']:
            flags.append('NO-PICTURE x%d' % d['nopicture'])
        if d['missing']:
            flags.append('MISSING-FRAMES x%d' % d['missing'])
        if d['dark']:
            flags.append('DARK x%d' % d['dark'])
        if d['blank']:
            flags.append('BLANK x%d' % d['blank'])
        if d['dupes'] >= max(2, d['shots'] - 2) and d['shots'] > 2:
            flags.append('STOPS-DID-NOT-MOVE')
        if d['skyptr'] and not d['skyobj']:
            flags.append('SKY-FLAT')
        if d['psseen'] and d['fxreported'] and not d['fxruns']:
            flags.append('NO-EFFECTS-DRAWN')
        if d['total'] and d['drawn'] * 2 < d['total']:
            flags.append('HALF-THE-WORLD')
        if flags:
            n_flagged += 1
        if flags or show_all:
            print('%-34s %6d %5d %5d %5d %5d %6.1f %5d %5d  %s' %
                  (name[:34], d['untextured'], d['nopicture'], d['missing'],
                   d['shots'], d['dark'], d['meanlum'], d['fxruns'],
                   d['skyobj'], ' '.join(flags)))
    print()
    print('%d levels, %d flagged, median untextured %d' %
          (len(rows), n_flagged, med_untex))
    if Image is None:
        print('NOTE: Pillow is not installed, so no picture was measured.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
