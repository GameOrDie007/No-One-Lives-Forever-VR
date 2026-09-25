"""Name every level the way the GAME names it, not the way its filename looks.

play-level.ps1 used to label a world by picking its mission number out of the
FILENAME. That is honest but it is not the game's own answer, and in a
monospace column `M16S01` (a GOTY bonus mission) and `M06S01` (campaign
mission 6) differ by one character - which is exactly the misread it produced.

The game already carries the mapping. ATTRIBUTES/MISSIONS.TXT, in NOLFGOTY.REZ,
lists every mission with the worlds that make it up:

    [Mission1]
    //  Misfortune in Morocco - Mission 1
    ...
    Level0      = "Worlds\\m01s01"
    Level1      = "Worlds\\m01s02"

so the world -> mission link is DATA, and a level's scene number is its position
in its own mission rather than a digit in its name.

The NAME is a comment. The name the game DISPLAYS is a string resource
(NameId 2501) in a CRes DLL this script does not read, so the comment is the
authors' own label for the block rather than the shipped string. It is checked
against the one place the game writes a mission name to disk - Save/Save1001.ini
records `Worlds\\m01s01|Misfortune in Morocco` - and any disagreement is
printed rather than silently resolved.

    python tools/mission-names.py            write tools/level-names.txt

Output is one `world<TAB>label` line per world, consumed by play-level.ps1.
Worlds MISSIONS.TXT does not mention - multiplayer maps, the tech demo - are
left out, and play-level.ps1 keeps deriving those from the path.
"""

import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REZ = os.path.join(ROOT, 'game', 'NOLFGOTY.REZ')
OUT = os.path.join(ROOT, 'tools', 'level-names.txt')
TMP = os.path.join(ROOT, 'tools', '.missions.tmp')

BACKSLASH = chr(92)


def extract():
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'rez.py'),
                    REZ, '--get', 'ATTRIBUTES/MISSIONS.TXT', TMP],
                   check=True, stdout=subprocess.DEVNULL)
    s = io.open(TMP, encoding='latin-1').read()
    os.remove(TMP)
    return s


def key_for(world):
    """One spelling for a world path, so the ini and the rez can be compared."""
    return world.replace('/', BACKSLASH).lower()


def parse(text):
    """[MissionN] blocks -> [(number, name, [world, ...]), ...]."""
    out = []
    parts = re.split(r'^\[Mission(\d+)\]', text, flags=re.M)
    # re.split leaves [preamble, num, body, num, body, ...]
    for i in range(1, len(parts) - 1, 2):
        num, body = int(parts[i]), parts[i + 1]
        m = re.search(r'^\s*//\s*(.+?)\s*$', body, flags=re.M)
        name = m.group(1) if m else ''
        # "Misfortune in Morocco - Mission 1" - drop the trailing restatement
        name = re.sub(r'\s*-\s*Mission\s*\d+\s*$', '', name).strip()
        levels = re.findall(r'^Level\d+\s*=\s*"([^"]+)"', body, flags=re.M)
        if levels:
            out.append((num, name, levels))
    return out


def ini_names():
    """The one place the GAME itself writes a mission name beside a world."""
    path = os.path.join(ROOT, 'game', 'Save', 'Save1001.ini')
    found = {}
    if not os.path.exists(path):
        return found
    for line in io.open(path, encoding='latin-1'):
        if '=' not in line:
            continue
        value = line.split('=', 1)[1].strip().split('|')
        if len(value) >= 2 and value[0].lower().startswith('worlds'):
            found[key_for(value[0])] = value[1]
    return found


def main():
    missions = parse(extract())
    ini = ini_names()
    rows = []
    checked = 0
    disagreed = []

    for num, name, levels in missions:
        for i, world in enumerate(levels):
            key = key_for(world)
            label = name if name else 'mission %d' % num
            if len(levels) > 1:
                label = '%s  (scene %d of %d)' % (label, i + 1, len(levels))
            rows.append((key, label))

            # ONLY CHECK AGAINST SOMETHING THAT IS ACTUALLY A MISSION NAME.
            #
            # Save1001.ini's second field is whatever named that slot, and the
            # game writes three different kinds of thing there: an auto name
            # ("The Assignment, Scene 2"), a name the PLAYER typed over a quick
            # save ("1,1"), and for Continue= a file path. Compared blind, the
            # last two read as the mission list being wrong, and a check that
            # cries wolf twice out of four is a check nobody reads again.
            seen = ini.get(key)
            if seen and name and BACKSLASH not in seen \
                    and len(re.findall(r'[A-Za-z]', seen)) >= 3:
                checked += 1
                # The ini stores a scene-level name ("The Assignment, Scene 2"),
                # so compare on the mission name it starts with.
                if not seen.lower().startswith(name.lower()[:12]):
                    disagreed.append((key, name, seen))

    rows.sort()
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# world<TAB>label. GENERATED by tools/mission-names.py from\n')
        f.write('# ATTRIBUTES/MISSIONS.TXT in NOLFGOTY.REZ - do not hand-edit.\n')
        for key, label in rows:
            f.write('%s\t%s\n' % (key, label))

    print('%d worlds named across %d missions -> %s'
          % (len(rows), len(missions), os.path.relpath(OUT, ROOT)))
    print('cross-checked %d against Save1001.ini, %d disagreed'
          % (checked, len(disagreed)))
    for key, name, seen in disagreed:
        print('  DISAGREE %s: MISSIONS.TXT says "%s", the ini says "%s"'
              % (key, name, seen))


main()
