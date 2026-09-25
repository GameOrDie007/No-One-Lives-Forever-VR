"""Re-read a finished sweep's logs and rebuild summary.csv, without re-running it.

A sweep costs about 25 minutes and 103 game launches. Its LOGS are kept, per
level, under logs\\sweep\\<WORLD>\\ - so every question the summary answers can be
asked again offline, and a new question can be added without paying for the
sweep a second time. That is the whole point of this file.

    python tools/sweep-summarise.py                     logs\\sweep -> summary.csv
    python tools/sweep-summarise.py --dir logs\\sweep-b  another run
    python tools/sweep-summarise.py --show whitePieces  print one column, sorted

THE LAST MATCH, NEVER THE FIRST. A level is built TWICE - once as the world
arrives, before the engine has bound a single texture, and again once it has.
The first build drops most of the level for "no texture": M04S01 reads 3317
dropped and 68.2% of its polygons missing a texture object, and its second build
reads 5. PowerShell's -match returns the first match, so every world-side number
sweep-levels.ps1 reported before 6 September was the pre-texture build - a state
that exists for a fraction of a second and that nobody plays. Everything here
takes the last occurrence, deliberately and uniformly.
"""

import argparse
import csv
import os
import re
import sys

# name -> regex with one capturing group. Every one of these is read as the
# LAST occurrence in the file; see the note above.
RENSTUB_FIELDS = [
    ("total",        r"R3D ACCOUNT: the level has (\d+) polygons"),
    ("drawn",        r"drawn\s+(\d+)"),
    ("collision",    r"collision hull \+ AI volumes\s+(\d+)"),
    ("markerTex",    r"editor marker textures\s+(\d+)"),
    ("markerFlags",  r"marker surface FLAGS\s+(\d+)"),
    ("noTex",        r"no texture, dropped\s+(\d+)"),
    ("noTexTrans",   r"translucent, no texture\s+(\d+)"),
    ("occluder",     r"occluder texture\s+(\d+)"),
    ("malformed",    r"malformed in the heap\s+(\d+)"),
    ("unaccounted",  r"UNACCOUNTED\s+(\d+)"),
    ("worldFile",    r"WORLD FILE: (\S+)"),
    ("missingTex",   r"(\d+) distinct textures are referenced but missing"),
    ("meshTris",     r"R3D MESH: (\d+) triangles from"),
    ("meshInst",     r"R3D MESH: \d+ triangles from \d+ pieces of (\d+) instances"),
    ("bridgeHeld",   r"bridge held on (\d+) models"),
    ("bridgeFailed", r"bridge held on \d+ models and failed on (\d+)"),
    ("pieces",       r"R3D SKINAUDIT: \d+ of (\d+) drawn pieces"),
    ("whitePieces",  r"R3D SKINAUDIT: (\d+) of \d+ drawn pieces"),
    ("skinTried",    r"skin slots: (\d+) tried"),
    ("skinNamed",    r"skin slots: \d+ tried, (\d+) named"),
    ("skinLoaded",   r"skin slots: \d+ tried, \d+ named, (\d+) loaded"),
    ("skinRecycled", r"(\d+) rebuilt after the engine recycled"),
]

# These live INSIDE the R3D ACCOUNT block and are read from it alone. Every one
# of them is a word generic enough to appear elsewhere in a 4000-line log.
ACCOUNT_FIELDS = {"total", "drawn", "collision", "markerTex", "markerFlags",
                  "noTex", "noTexTrans", "occluder", "malformed", "unaccounted"}

CLIENT_FIELDS = [
    ("published", r"VRModels: (\d+) instances,"),
    ("dropped",   r"VRModels: \d+ instances, \d+ nodes, (\d+) dropped"),
    ("objects",   r"VRModels: \d+ instances, \d+ nodes, \d+ dropped this frame \(of (\d+) objects"),
]

# The columns worth leading with: a non-zero in any of these is a level with a
# defect in it, and the point of a 103-level sweep is to find those without a
# person in a headset.
ALARM = ("whitePieces", "noTex", "noTexTrans", "malformed", "unaccounted",
         "bridgeFailed")


def last(text, pattern):
    hits = re.findall(pattern, text)
    return hits[-1] if hits else ""


# THE LAST WORLD ACCOUNT, AS A BLOCK, AND NOT A LOOSE SEARCH OVER THE FILE.
#
# Taking the last match of `drawn\s+(\d+)` was the fix for reading the
# PRE-TEXTURE build, and it was directionally right and wrong in detail: there
# is a SECOND account in the log, `R3D MODEL ACCOUNT`, and it has a `drawn` line
# of its own. So the last match was the model count - "drawn 2" against a level
# that draws 6485 - and the summary reported that every level renders 0.3% of
# its polygons. A number that absurd is a gift; a plausible wrong one would have
# been believed.
#
# Fields that live inside the account are read from the account's own text.
def account_block(text):
    """The last R3D ACCOUNT block: from its header to the next R3D heading."""
    starts = [m.start() for m in
              re.finditer(r"R3D ACCOUNT: the level has", text)]
    if not starts:
        return ""
    i = starts[-1]
    m = re.search(r"[\r\n]\s*R3D (?!ACCOUNT)", text[i + 20:])
    return text[i:i + 20 + m.start()] if m else text[i:]


def read_level(d):
    row = {"name": os.path.basename(d).replace("WORLDS_", "")}
    rl = os.path.join(d, "renstub.log")
    if os.path.exists(rl):
        with open(rl, "r", encoding="utf-8", errors="replace") as f:
            t = f.read()
        blk = account_block(t)
        for name, pat in RENSTUB_FIELDS:
            row[name] = last(blk if name in ACCOUNT_FIELDS else t, pat)
        # How many times the level was built. Reported, not judged: most levels
        # build exactly once and that build is already textured, so a count of
        # 1 says nothing on its own. The fraction of the level dropped for want
        # of a texture is what separates a premature build from a healthy one.
        row["builds"] = str(len(re.findall(r"R3D ACCOUNT", t)))
        # The texture names a level asks for and does not get. These are the
        # actionable ones: a NAME can be looked up on disk, a null pointer
        # cannot.
        names = re.findall(r"^\s+(\S+\.DTX)\s+(\d+) polygons", t, re.M)
        if names:
            row["missingNames"] = "; ".join(
                "%s x%s" % (n, c) for n, c in dict(
                    (n, c) for n, c in names).items())
    cl = os.path.join(d, "client.log")
    if os.path.exists(cl):
        with open(cl, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        for name, pat in CLIENT_FIELDS:
            row[name] = last(c, pat)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join("logs", "sweep"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--show", default=None,
                    help="print just this column, worst first")
    a = ap.parse_args()

    if not os.path.isdir(a.dir):
        sys.exit("no such directory: %s" % a.dir)

    dirs = sorted(os.path.join(a.dir, d) for d in os.listdir(a.dir)
                  if os.path.isdir(os.path.join(a.dir, d)))
    rows = [read_level(d) for d in dirs]
    rows = [r for r in rows if len(r) > 1]
    if not rows:
        sys.exit("no level logs under %s" % a.dir)

    cols = ["name", "builds"] + [n for n, _ in RENSTUB_FIELDS] \
         + [n for n, _ in CLIENT_FIELDS] + ["missingNames"]
    out = a.out or os.path.join(a.dir, "summary-reparsed.csv")
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    print("%d levels -> %s" % (len(rows), out))

    if a.show:
        def key(r):
            v = r.get(a.show, "")
            try:
                return -int(v)
            except (TypeError, ValueError):
                return 0
        for r in sorted(rows, key=key):
            if r.get(a.show):
                print("  %-24s %s" % (r["name"], r[a.show]))
        return

    # THE ALARM COLUMNS, and nothing else. A sweep that prints 103 healthy rows
    # buries the two that matter; this prints the exceptions and the denominator.
    print("\nlevels with something to answer for:")
    any_bad = False
    for r in rows:
        bad = [(k, r.get(k)) for k in ALARM
               if r.get(k) not in ("", "0", None)]
        # THE FRACTION, NOT THE BUILD COUNT. "built once" looked like a clean
        # test for "never reached its textured state" and it is not: most
        # levels build exactly once and that single build is already textured.
        # What actually separates the two is how much of the level was dropped
        # - a pre-texture build loses most of it (M04S01: 3317 of 4856, 68%)
        # and a healthy one loses single digits.
        try:
            tot = int(r.get("total") or 0)
            nt = int(r.get("noTex") or 0)
            if tot and nt * 20 > tot:
                bad.append(("untextured",
                            "%d of %d polygons - %.0f%%, so this is the "
                            "PRE-TEXTURE build" % (nt, tot, 100.0 * nt / tot)))
        except (TypeError, ValueError):
            pass
        if bad:
            any_bad = True
            print("  %-22s %s" % (r["name"],
                  "  ".join("%s=%s" % (k, v) for k, v in bad)))
            if r.get("missingNames"):
                print("      missing: %s" % r["missingNames"])
    if not any_bad:
        print("  none - %d levels clean" % len(rows))


if __name__ == "__main__":
    main()
