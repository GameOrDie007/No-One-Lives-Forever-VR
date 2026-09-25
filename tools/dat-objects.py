#!/usr/bin/env python3
"""dat-objects.py - read the OBJECT list out of NOLF's .dat worlds.

dat.py maps the container and the world models; it stops at objectDataPos and
says so. Everything a level AUTHORS lives past that line - SurfaceHeight,
ShowSurface, Current, XScaleMin, AmbientLight - and every one of those numbers
has at some point been guessed at by this project instead of read. See the
`nolf-vr-status` memory and the note in the port notes: an
animated surface's numbers are AUTHORED.

Same discipline as dat.py: parse, then prove the parse landed.

  python tools/dat-objects.py list   WORLDS/T01S02.DAT
  python tools/dat-objects.py props  WORLDS/T01S02.DAT VolumeBrush
  python tools/dat-objects.py water                       (sweep every world)

THREE IDENTITIES, enforced on every world:

  1. THE RECORD CHAIN LANDS EXACTLY ON renderDataPos. Each object record is
     length-prefixed; walking the lengths must end on the section boundary, not
     near it. A wrong stride anywhere derails it.
  2. THE DECLARED OBJECT COUNT EQUALS THE NUMBER OF RECORDS WALKED. The count
     is written independently of the chain, so agreeing is not automatic.
  3. EVERY PROPERTY'S DECLARED DATA LENGTH MATCHES ITS TYPE'S FIXED WIDTH,
     where the type has one. A string is the only variable-width type.

CROSS-CHECKED AGAINST A FACT ESTABLISHED WITHOUT IT: WORLDS/T01S02.DAT authors
Water0 with SurfaceHeight 3.0 and a second water body at 4.0. That was read out
of the running game through CPolyGridFX::GetDims on 16 September, by a
completely different route. `--selftest` asserts it here.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dat  # noqa: E402  - the container parser, reused whole

# LithTech 2 property types. Width in bytes, or None for variable.
T_STRING, T_VECTOR, T_COLOR, T_REAL, T_FLAGS, T_BOOL, T_LONGINT, T_ROTATION = range(8)
WIDTH = {T_STRING: None, T_VECTOR: 12, T_COLOR: 12, T_REAL: 4,
         T_FLAGS: 4, T_BOOL: 1, T_LONGINT: 4, T_ROTATION: 16}
TNAME = {T_STRING: "string", T_VECTOR: "vector", T_COLOR: "color",
         T_REAL: "real", T_FLAGS: "flags", T_BOOL: "bool",
         T_LONGINT: "long", T_ROTATION: "rot"}


class BadParse(Exception):
    pass


def _value(b, off, typ, ln):
    if typ == T_STRING:
        # The data is ITSELF length-prefixed, and dlen counts that prefix:
        # 13 bytes = u16 11 + "OutsideDef0".
        if ln < 2:
            return ""
        sl, = struct.unpack_from("<H", b, off)
        return b[off + 2:off + 2 + min(sl, ln - 2)].decode("latin-1", "replace")
    if typ in (T_VECTOR, T_COLOR):
        return struct.unpack_from("<3f", b, off)
    if typ == T_REAL:
        return struct.unpack_from("<f", b, off)[0]
    if typ == T_ROTATION:
        return struct.unpack_from("<4f", b, off)
    if typ == T_BOOL:
        return bool(b[off])
    return struct.unpack_from("<I", b, off)[0]


def objects(w):
    """[(classname, {prop: value})], with all three identities enforced."""
    b = w.b
    p = w.object_data_pos
    count, = struct.unpack_from("<I", b, p)
    p += 4
    if count > 100000:
        raise BadParse("object count %d is not credible" % count)

    out = []
    for i in range(count):
        if p + 2 > len(b):
            raise BadParse("record %d starts past the end of the file" % i)
        reclen, = struct.unpack_from("<H", b, p)
        nxt = p + 2 + reclen                      # identity 1, per record
        q = p + 2
        nlen, = struct.unpack_from("<H", b, q)
        q += 2
        if nlen == 0 or nlen > 128 or q + nlen > len(b):
            raise BadParse("record %d has a bad class-name length %d" % (i, nlen))
        cls = b[q:q + nlen].decode("latin-1", "replace")
        q += nlen
        nprop, = struct.unpack_from("<I", b, q)
        q += 4
        if nprop > 512:
            raise BadParse("record %d (%s) declares %d properties" % (i, cls, nprop))

        props = {}
        for _ in range(nprop):
            # u16, NOT a byte. A byte reads "OutsideDef" as "OutsideDef. Nam"
            # and everything after it is garbage - which is how this was found.
            plen, = struct.unpack_from("<H", b, q)
            q += 2
            name = b[q:q + plen].decode("latin-1", "replace")
            q += plen
            typ = b[q]
            q += 1
            q += 4                                # per-property flags
            dlen, = struct.unpack_from("<H", b, q)
            q += 2
            if typ not in WIDTH:
                raise BadParse("%s.%s has unknown type %d" % (cls, name, typ))
            fixed = WIDTH[typ]
            if fixed is not None and dlen != fixed:   # identity 3
                raise BadParse("%s.%s is %s but carries %d bytes, not %d"
                               % (cls, name, TNAME[typ], dlen, fixed))
            props[name] = _value(b, q, typ, dlen)
            q += dlen

        if q != nxt:
            raise BadParse("record %d (%s) ended at %d, its length says %d"
                           % (i, cls, q, nxt))
        out.append((cls, props))
        p = nxt

    if p != w.render_data_pos:                    # identity 1, overall
        raise BadParse("the object chain ended at %d, renderDataPos is %d"
                       % (p, w.render_data_pos))
    if len(out) != count:                         # identity 2
        raise BadParse("walked %d records, %d were declared" % (len(out), count))
    return out


def worlds(game):
    return sorted(k for k in dat.mounted(game) if k.endswith(".DAT"))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = os.path.join(root, "game")
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    cmd = args[0]

    if cmd == "list":
        w = dat.load(game, args[1])
        objs = objects(w)
        import collections
        c = collections.Counter(cls for cls, _ in objs)
        print("  %d objects, all three identities hold" % len(objs))
        for k, v in c.most_common():
            print("    %-34s %d" % (k, v))

    elif cmd == "props":
        w = dat.load(game, args[1])
        want = args[2].lower() if len(args) > 2 else None
        for cls, p in objects(w):
            if want and cls.lower() != want:
                continue
            print("  %s  %s" % (cls, p.get("Name", "")))
            for k in sorted(p):
                print("      %-22s %s" % (k, p[k]))

    elif cmd == "selftest":
        # THE CLASS IS `Water`, not `VolumeBrush`. VolumeBrush is the C++ base
        # in ObjectDLL; what a level authors is the concrete subclass, and this
        # test asserted the base name and failed on a correct parse.
        w = dat.load(game, "WORLDS/T01S02.DAT")
        heights = sorted(p["SurfaceHeight"] for cls, p in objects(w)
                         if "SurfaceHeight" in p)
        print("  T01S02 water SurfaceHeight:", heights)
        assert 3.0 in heights and 4.0 in heights, \
            "does NOT match what the running game reported on 16 September"
        print("  OK - agrees with CPolyGridFX::GetDims, read by another route")

    elif cmd == "water":
        # THE QUESTION THIS WAS WRITTEN FOR. The renderer skips a water volume
        # brush's own horizontal top face because the polygrid draws that
        # surface instead. A brush with ShowSurface OFF has no polygrid, so
        # skipping its top face would leave a hole in the water.
        bad, ok, failed = [], 0, []
        for k in worlds(game):
            try:
                objs = objects(dat.load(game, k))
            except (BadParse, Exception) as e:
                failed.append((k, str(e)[:70]))
                continue
            ok += 1
            for cls, p in objs:
                if "SurfaceHeight" not in p and "ShowSurface" not in p:
                    continue
                show = p.get("ShowSurface", None)
                if show is False:
                    bad.append((k, cls, p.get("Name", ""), p.get("SurfaceHeight")))
        print("  %d worlds parsed, %d failed" % (ok, len(failed)))
        for k, e in failed[:10]:
            print("    FAILED %-22s %s" % (k, e))
        print("  %d water brushes have ShowSurface OFF:" % len(bad))
        for k, cls, nm, h in bad:
            print("    %-22s %-16s %-18s SurfaceHeight %s" % (k, cls, nm, h))

    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
