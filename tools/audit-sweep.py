"""What is WRONG in each level, read out of the sweep's own logs.

sweep-triage.py answers "did this level build". This one answers "is anything
in this level not being drawn": untextured model pieces, sprites that resolve
nothing, world batches whose texture never arrived, geometry the renderer chose
to drop, and objects the engine flagged invisible.

Every one of these numbers is already printed per level. Nobody has ever read
them ACROSS levels, which is the only way a fault that affects forty worlds
looks different from a fault that affects one.

    python tools/audit-sweep.py logs/shots
"""
import os
import re
import sys


def last_int(pat, txt, group=1):
    ms = re.findall(pat, txt, re.M)
    if not ms:
        return None
    m = ms[-1]
    if isinstance(m, tuple):
        m = m[group - 1]
    return int(m.replace(',', ''))


def one(txt):
    r = {}

    # --- models drawing the white stand-in ------------------------------
    m = re.findall(r'SKINAUDIT: (\d+) of (\d+) drawn pieces have NO texture',
                   txt)
    if m:
        r['white'], r['pieces'] = int(m[-1][0]), int(m[-1][1])

    # --- sprites in front of the camera that resolved nothing -----------
    m = re.findall(r'SPRITES: (\d+) published, (\d+) resolved a texture,'
                   r' (\d+) vertices drawn\s+\| IN FRONT of the camera: (\d+),'
                   r' of which (\d+) RESOLVED NOTHING', txt)
    if m:
        r['spr'], r['sprRes'] = int(m[-1][0]), int(m[-1][1])
        r['sprFront'], r['sprNone'] = int(m[-1][3]), int(m[-1][4])

    # --- world batches whose texture never arrived ----------------------
    m = re.findall(r'resolved (\d+) of (\d+) batches to a texture', txt)
    if m:
        r['batRes'], r['batAll'] = int(m[-1][0]), int(m[-1][1])

    # --- polygons the renderer dropped, from the LAST account -----------
    parts = txt.split('R3D ACCOUNT: the level has')
    if len(parts) > 1:
        blk = parts[-1][:1600]
        mm = re.match(r'\s*(\d+) polygons', blk)
        r['polys'] = int(mm.group(1)) if mm else None
        for key, pat in (('drawn', r'^\s+drawn\s+(\d+)'),
                         ('marker', r'^\s+editor marker textures\s+(\d+)'),
                         ('notex', r'^\s+no texture, dropped\s+(\d+)'),
                         ('unacct', r'^\s+UNACCOUNTED\s+(\d+)')):
            v = last_int(pat, blk)
            r[key] = v

    # --- objects the engine says are invisible --------------------------
    m = re.findall(r'INVISIBLE: (\d+) drawn instances have FLAG_VISIBLE CLEAR',
                   txt)
    if m:
        r['invis'] = int(m[-1])
    m = re.findall(r'INVISIBLE: (\d+) instances SKIPPED', txt)
    if m:
        r['invisSkip'] = int(m[-1])

    # --- the model light grid -------------------------------------------
    m = re.findall(r'MODEL LIGHT: darkest instance ([\d.]+), (\d+) of (\d+)'
                   r' at or under', txt)
    if m:
        r['darkest'] = float(m[-1][0])
        r['dark'], r['insts'] = int(m[-1][1]), int(m[-1][2])

    r['crash'] = '=== CRASH ===' in txt
    return r


def main(root):
    rows = []
    for f in sorted(os.listdir(root)):
        if not f.endswith('-renstub.log'):
            continue
        with open(os.path.join(root, f), 'r', encoding='utf-8',
                  errors='replace') as fh:
            rows.append((f[:-12], one(fh.read())))

    print('%d levels\n' % len(rows))

    def flag(name, test, fmt):
        hits = [(n, r) for n, r in rows if test(r)]
        print('%-46s %3d levels' % (name, len(hits)))
        for n, r in sorted(hits, key=lambda x: -fmt(x[1]))[:10]:
            print('      %-30s %s' % (n, fmt(r)))
        if hits:
            print('')

    flag('MODEL PIECES DRAWN UNTEXTURED (white stand-in)',
         lambda r: r.get('white'), lambda r: r.get('white', 0))
    flag('SPRITES IN VIEW THAT RESOLVED NOTHING',
         lambda r: r.get('sprNone'), lambda r: r.get('sprNone', 0))
    flag('WORLD BATCHES WITH NO TEXTURE',
         lambda r: r.get('batAll') and r['batRes'] != r['batAll'],
         lambda r: r.get('batAll', 0) - r.get('batRes', 0))
    flag('POLYGONS DROPPED FOR HAVING NO TEXTURE',
         lambda r: r.get('notex'), lambda r: r.get('notex', 0))
    flag('POLYGONS UNACCOUNTED FOR',
         lambda r: r.get('unacct'), lambda r: r.get('unacct', 0))
    flag('INSTANCES THE ENGINE FLAGS INVISIBLE',
         lambda r: r.get('invis'), lambda r: r.get('invis', 0))
    flag('MODELS AT OR UNDER 0.12 LIGHT (near black)',
         lambda r: r.get('dark'), lambda r: r.get('dark', 0))
    flag('CRASHED', lambda r: r['crash'], lambda r: 1)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'logs/shots')
