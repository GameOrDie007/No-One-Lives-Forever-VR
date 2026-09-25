"""Which SKIN belongs to which MODEL, read from the game's own attribute files.

The reference reading for host/renstub/butes.cpp, and the verifier that
established it - the same role rez.py plays for the archives, dtx.py for the
textures and dat.py for the worlds. The file is the answer key; this is how to
ask it a question without launching the game.

  python tools/butes.py                 the map, with its counts
  python tools/butes.py --check         every declared skin must be on disk
  python tools/butes.py --model "Guns\\Models_HH\\Delisle_hh.abc"
  python tools/butes.py --transform     score the OBVIOUS rule instead

WHY THIS EXISTS. The engine binds a model's skin only when it draws the model
through the path the VR renderer replaced, so an object nobody has walked past
has both skin slots reading 00000000 - no texture object, no name, nothing for
the file route to look up. A De Lisle carbine lying on the floor of T10S01 draws
WHITE for exactly that reason, with its skin sitting on disk the whole time.

The game declares the pair. ATTRIBUTES/WEAPONS.TXT:

    HHModel = "Guns\\Models_HH\\delisle_hh.abc"
    HHSkin  = "Guns\\Skins_HH\\delisle_hh.dtx"

AND THE OBVIOUS TRANSFORM IS NOT GOOD ENOUGH. Turning MODELS_x into SKINS_x and
.abc into .dtx holds on 49% of the game's 701 models - `--transform` prints the
score. Half is not a rule, it is a coincidence with a good week, and fitting one
is what this project has a standing note against. Read the declaration instead.

PAIRED BY PREFIX, NEVER BY POSITION. PVModel goes with PVSkin and HHModel with
HHSkin. Pairing in file order would put a weapon's player-view skin onto its
hand-held model, which is a wrong picture that looks entirely plausible.

PATHS ARE NORMALISED BEFORE COMPARISON, because the game spells one file three
ways inside a single record - Guns\\Skins_HH, guns\\skins_hh and Guns\\skins_hh -
and read literally those are three different skins, which gets the model refused
as ambiguous. Normalising takes the ambiguous count from 9 to 3.

THE THREE THAT REMAIN ARE REFUSED ON PURPOSE. lipstick, perfume and the
helicopter window each declare several skins, and they are genuine variants -
impact, proximity and timed bombs share one model. Drawing the wrong one is a
worse outcome than drawing the white stand-in, because nobody would notice it.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rez import RezFile                                   # noqa: E402

# The mount order, as dtx.py has it: later archives override earlier ones.
MOUNT = ["NOLF.rez", "NOLF2.rez", "NOLFdll.rez", "NOLFl.rez",
         "Nolfu003.rez", "Nolfcres003.rez", "NolfGoty.rez", "Modernizer.rez"]

RE_MODEL = re.compile(r'\s*(\w*?)Model\s*=\s*"([^"]+\.abc)"', re.I)
RE_SKIN = re.compile(r'\s*(\w*?)Skin\s*=\s*"([^"]+\.dtx)"', re.I)


def norm(p):
    return p.replace("\\", "/").lower().lstrip("/")


def archives(game):
    have = {f.lower(): f for f in os.listdir(game)}
    for m in MOUNT:
        real = have.get(m.lower())
        if real:
            yield os.path.join(game, real)


def build(game):
    """model -> set of skins declared for it, over every mounted archive."""
    out = {}
    for path in archives(game):
        r = RezFile(path)
        for name in r.files:
            u = name.upper()
            if not (u.startswith("ATTRIBUTES/") and u.endswith(".TXT")):
                continue
            blob = r.read(name)
            if not blob:
                continue
            # prefix -> the model most recently declared under it, cleared at
            # every section header so a skin cannot pair across records.
            cur = {}
            for line in blob.decode("latin-1").splitlines():
                if line.lstrip().startswith("["):
                    cur = {}
                    continue
                m = RE_MODEL.match(line)
                if m:
                    cur[m.group(1).lower()] = norm(m.group(2))
                    continue
                m = RE_SKIN.match(line)
                if m:
                    k = m.group(1).lower()
                    if k in cur:
                        out.setdefault(cur[k], set()).add(norm(m.group(2)))
    return out


def mounted_files(game, suffix):
    seen = set()
    for path in archives(game):
        for n in RezFile(path).files:
            if n.upper().endswith(suffix):
                seen.add(norm(n))
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="game")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--model", default="")
    ap.add_argument("--transform", action="store_true")
    a = ap.parse_args()

    if a.transform:
        # THE RULE THAT WAS NOT USED, scored so the decision stays checkable.
        abcs = mounted_files(a.game, ".ABC")
        dtxs = mounted_files(a.game, ".DTX")
        hit = miss = 0
        for m in abcs:
            cand = (m.replace("/models_", "/skins_")
                     .replace("/models/", "/skins/")
                     .replace(".abc", ".dtx"))
            if cand == m:
                continue
            if cand in dtxs:
                hit += 1
            else:
                miss += 1
        tot = hit + miss
        print("MODELS->SKINS, .abc->.dtx over %d models: %d hold, %d do not"
              " (%.0f%%)" % (tot, hit, miss, 100.0 * hit / max(tot, 1)))
        print("Not a rule. This is why butes.cpp reads the declaration.")
        return 0

    m = build(a.game)
    one = {k: sorted(v)[0] for k, v in m.items() if len(v) == 1}
    amb = {k: sorted(v) for k, v in m.items() if len(v) > 1}

    if a.model:
        key = norm(a.model)
        if key in one:
            print("%s -> %s" % (key, one[key]))
        elif key in amb:
            print("%s declares %d skins and is REFUSED: %s"
                  % (key, len(amb[key]), ", ".join(amb[key])))
        else:
            print("%s is not declared in any attribute file" % key)
        return 0

    print("%d models declare a skin, %d usable, %d refused for declaring"
          " several" % (len(m), len(one), len(amb)))
    for k, v in sorted(amb.items()):
        print("   refused: %-46s %s" % (k, ", ".join(v)))

    if a.check:
        dtxs = mounted_files(a.game, ".DTX")
        missing = [(k, v) for k, v in sorted(one.items()) if v not in dtxs]
        print("declared skins NOT on disk: %d" % len(missing))
        for k, v in missing[:20]:
            print("   %s -> %s" % (k, v))
        # The renderer prints these same three numbers at startup. If they
        # disagree, the parse changed - not the game.
        return 1 if missing else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
