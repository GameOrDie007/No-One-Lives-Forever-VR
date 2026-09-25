"""Does a surface flag value separate marker geometry from the visible world?

The same question, asked of both sources, so the two answers can be put side by
side:

  the HEAP  - the renderer's +StubFlagHist, reading the engine's own surface
              record at +0x38 while the level is running
  the FILE  - this script, reading .dat's 53-byte surface record at +51

The heap answered NO on 5 September, and it answered with a denominator: over
m01s02, flag value 00001081 covers 14751 marker polygons AND 5898 ordinary
ones, so no mask over that field can tell them apart. That closes a search four
earlier attempts had already failed at, this time with a number.

This is the control. If the FILE's flags separate cleanly on the same level,
then the separation exists, it is simply not in the field the heap exposes -
which is the whole argument for wiring stage 2 in rather than looking harder at
memory.

    python tools/flagsplit.py [WORLDS/M01S02.DAT]

Marker means what the renderer's SkippedModel means, so the two sides are
counting the same class: PhysicsBSP, AIVolume*, blocker*.
"""

import collections
import sys

import dat


def klass(name):
    if (name == "PhysicsBSP" or name.startswith("AIVolume")
            or name.startswith("blocker")):
        return "marker"
    if name.startswith("TranslucentWorldModel"):
        return "trans"
    return "other"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    path = args[0] if args else "WORLDS/M01S02.DAT"
    game = "<repo>/game"
    w = dat.load(game, path)

    # VISBSP IS INCLUDED NOW, and that is the change. Its surface array is
    # not after its planes in any world, so this file used to read the wrong
    # bytes for it: 4349 surfaces with flags >= 0x2000, 887 distinct junk
    # values, 17 of them 0xFFFF with a single bit cleared. dat.py now LOCATES
    # the array instead of computing it, and the junk becomes 14 values from
    # the same small vocabulary every other model uses.
    #
    # Pass -assumed to read it at the old planes-adjacent offset and see the
    # garbage this replaced.
    if "-assumed" in sys.argv:
        for m in w.models:
            m.surface_start = m.surface_assumed

    rows = collections.defaultdict(collections.Counter)
    tex = collections.defaultdict(set)
    skipped = 0
    for m in w.models:
        if not m.n_surfaces:
            continue
        k = klass(m.name)
        try:
            surfs = list(m.surfaces())
        except Exception:
            skipped += 1
            continue
        for s in surfs:
            rows[s["flags"]][k] += 1
            if s["texture"] < len(m.textures):
                tex[s["flags"]].add(
                    m.textures[s["texture"]].rsplit("\\", 1)[-1])

    tot = collections.Counter()
    for c in rows.values():
        tot.update(c)

    print(f"{path}: {len(w.models)} models, {sum(tot.values())} surfaces read"
          f"  ({skipped} models' surface arrays not locatable)")
    print(f"  marker {tot['marker']}   translucent {tot['trans']}"
          f"   other {tot['other']}")
    print()
    print("  flags   marker  transl    other   marker-only?  textures")
    for fl in sorted(rows, key=lambda f: -sum(rows[f].values())):
        c = rows[fl]
        names = sorted(tex[fl])
        shown = ", ".join(names[:4]) + (f" (+{len(names) - 4} more)"
                                        if len(names) > 4 else "")
        only = "<= MARKER ONLY" if c["marker"] and not c["other"] \
                                   and not c["trans"] else ""
        print(f"  {fl:5d}  {c['marker']:7d} {c['trans']:7d} {c['other']:8d}"
              f"   {only:14s} {shown}")

    # THE NUMBER THAT DECIDES IT. A rule is only usable if the values it fires
    # on are marker-only, so the question is what fraction of marker surfaces
    # those values actually cover - and what they cost elsewhere, which is
    # zero by construction.
    clean = [f for f, c in rows.items()
             if c["marker"] and not c["other"] and not c["trans"]]
    got = sum(rows[f]["marker"] for f in clean)
    print()
    print(f"  marker-only values: {sorted(clean)}")
    if tot["marker"]:
        print(f"  they cover {got} of {tot['marker']} marker surfaces"
              f"  ({100.0 * got / tot['marker']:.1f}%)"
              f", and 0 of {tot['other'] + tot['trans']} others")


if __name__ == "__main__":
    main()
