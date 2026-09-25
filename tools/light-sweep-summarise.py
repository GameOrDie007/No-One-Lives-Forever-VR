# Read a finished lighting sweep and say which levels are at risk.
#
# Model lighting was proved on two levels out of 103. The failure mode it can
# have on the other 101 is specific and gameplay-affecting rather than merely
# ugly: a world with few lightmapped surfaces AND no declared AmbientLight
# gives every model a light of nearly nothing, and characters go black.
#
# So the sweep is not read by eye. Each level's renderer log carries one
# MODEL LIGHT line - the darkest instance in the level, how many sit at or
# under the threshold, the grid's fill, and the ambient floor - and this walks
# them all and sorts the worst to the top.
#
#   python tools/light-sweep-summarise.py [logs/sweep-light]
#
# A level that never reported the line at all is listed separately and is NOT
# counted as passing: "no evidence" and "evidence of health" are different
# answers, and a sweep that quietly drops the levels it could not measure is
# the kind of instrument this project has been bitten by before.

import os
import re
import sys

DIR = sys.argv[1] if len(sys.argv) > 1 else 'logs/sweep-light'

RE_LIGHT = re.compile(
    r'MODEL LIGHT: darkest instance ([-\d.]+), (\d+) of (\d+) at or under '
    r'([\d.]+)\s*\|\s*grid (\d+) of (\d+) cells, ambient floor ([\d.]+)')
RE_WORLD = re.compile(r'WORLD FILE: (\S+)')


def main():
    if not os.path.isdir(DIR):
        print('no such directory: ' + DIR)
        return 1

    rows, silent = [], []
    # The sweep writes ONE DIRECTORY PER WORLD, each holding renstub.log.
    for name in sorted(os.listdir(DIR)):
        sub = os.path.join(DIR, name)
        if not os.path.isdir(sub):
            continue
        path = os.path.join(sub, 'renstub.log')
        if not os.path.isfile(path):
            silent.append((name + '.log', '(no renstub.log)'))
            continue
        try:
            text = open(path, 'r', errors='replace').read()
        except OSError:
            continue

        world = ''
        mw = RE_WORLD.search(text)
        if mw:
            world = mw.group(1)

        # The LAST report in the run, not the first: a level is built twice and
        # the early one is a state nobody plays.
        hits = RE_LIGHT.findall(text)
        if not hits:
            silent.append((name + '.log', world))
            continue
        darkest, ndark, ntot, thr, filled, cells, floor = hits[-1]
        rows.append(dict(level=name, world=world,
                         darkest=float(darkest), ndark=int(ndark),
                         ntot=int(ntot), filled=int(filled),
                         cells=int(cells), floor=float(floor)))

    if not rows and not silent:
        print('no logs found in ' + DIR)
        return 1

    rows.sort(key=lambda r: (r['darkest'], -r['ndark']))

    print('%d levels reported, %d silent\n' % (len(rows), len(silent)))
    print('%-28s %8s %10s %12s %8s' %
          ('level', 'darkest', 'under thr', 'grid cells', 'floor'))
    for r in rows:
        flag = ''
        if r['ntot'] and r['ndark']:
            flag = '  <- DARK MODELS'
            if r['floor'] <= 0.01:
                flag += ' AND NO FLOOR'
        if r['ntot'] == 0:
            flag = '  (no models published here)'
        print('%-28s %8.3f %5d/%-5d %6d/%-6d %8.3f%s' %
              (r['level'][:28], r['darkest'], r['ndark'], r['ntot'],
               r['filled'], r['cells'], r['floor'], flag))

    worst = [r for r in rows if r['ndark']]
    nofloor = [r for r in rows if r['floor'] <= 0.01]
    empty = [r for r in rows if r['ntot'] and r['filled'] == 0]

    print('')
    print('  levels with any model at or under the threshold : %d' % len(worst))
    print('  levels declaring NO ambient light               : %d' % len(nofloor))
    print('  levels whose light grid came out EMPTY          : %d' % len(empty))
    if silent:
        print('')
        print('  NOT MEASURED (%d) - these are not passes:' % len(silent))
        for name, world in silent[:20]:
            print('    %-30s %s' % (name[:-4][:30], world or '(no world line)'))
        if len(silent) > 20:
            print('    ... and %d more' % (len(silent) - 20))
    return 0


if __name__ == '__main__':
    sys.exit(main())
