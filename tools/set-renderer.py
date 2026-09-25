"""Set RenderDll in the staged game's autoexec.cfg.

The engine writes its console variables back to autoexec.cfg when it exits, so
any run that selected our renderer leaves the staged game defaulting to it -
and a launcher that does not name a renderer then silently runs the stub, which
looks like the game being broken.

This puts it back, deterministically, without launching anything.

  python tools/set-renderer.py            # back to the stock d3d.ren
  python tools/set-renderer.py d3dstub.ren

Edits in place, preserving the file's own line endings, and prints the before
and after. Refuses to touch anything outside game\\.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "game", "autoexec.cfg")


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else "d3d.ren"

    if not os.path.isfile(CFG):
        print("no autoexec.cfg at %s" % CFG)
        return 1

    with open(CFG, "rb") as f:
        raw = f.read()

    crlf = raw.count(b"\r\n")
    text = raw.decode("latin-1")

    pat = re.compile(r'("RenderDll"\s+")([^"]*)(")')
    m = pat.search(text)
    if not m:
        print('no "RenderDll" line in autoexec.cfg - the engine has never '
              "persisted one, so nothing is being inherited")
        return 0

    print("was: RenderDll = %s" % m.group(2))
    if m.group(2) == want:
        print("already %s - nothing to do" % want)
        return 0

    text = text[:m.start()] + m.group(1) + want + m.group(3) + text[m.end():]
    with open(CFG, "wb") as f:
        f.write(text.encode("latin-1"))

    print("now: RenderDll = %s   (%d CRLF line endings preserved)" % (want, crlf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
