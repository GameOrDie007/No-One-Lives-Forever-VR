"""Read a LithTech .REZ archive - the containers NOLF ships its art in.

This is stage 0 of docs/PLAN-FILES-NOT-HEAP.md: before the renderer can load a
texture or a model from the game's own files, it has to be able to open a file
by name. This is the reference implementation and the specification for the C++
one in host/renstub/rezfs.cpp.

  python tools/rez.py game/NOLF.REZ                 list it
  python tools/rez.py game/NOLF.REZ --verify <dir>  check against an extraction
  python tools/rez.py game/NOLF.REZ --get CHARS/MODELS/HERO_ACTION.ABC out.abc

The format, derived from the bytes and checked against lithrez's own extraction
of all 4477 files:

  header    127-byte text banner ending \\r\\n\\x1a, then
            u32 version (1), u32 rootDirPos, u32 rootDirSize, and more fields
            that are not needed to read one.
  directory a block of entries at {pos, size}, each beginning with a u32 type.
  type 1    u32 pos, u32 size, u32 time, cstring name    - a subdirectory,
            whose own block is at that {pos, size}.
  type 0    u32 pos, u32 size, u32 time, u32 id, u32 ext, u32 numKeys,
            cstring name, cstring comment                - a file.

  `ext` is four characters stored in reverse, so an .ABC model reads "CBA".
  The comment is almost always empty, which is the single trailing zero byte
  after the name - miss it and every entry after the first is one byte out,
  which is how this format announces a wrong reading: the next `pos` lands
  outside the file.

Names are stored uppercase with no path; a full path is the directory names
joined. Lookups here are case-insensitive with '/' and '\\' equivalent, which
is what the engine does.
"""

import os
import struct
import sys


class RezEntry:
    __slots__ = ("path", "pos", "size", "ext", "id")

    def __init__(self, path, pos, size, ext, ident):
        self.path = path
        self.pos = pos
        self.size = size
        self.ext = ext
        self.id = ident

    def __repr__(self):
        return f"<{self.path} {self.size}B @{self.pos:#x}>"


class RezFile:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.length = self.f.tell()

        self.f.seek(0x7F)
        ver, root_pos, root_size = struct.unpack("<III", self.f.read(12))
        if ver != 1:
            raise ValueError(f"{path}: rez version {ver}, expected 1")
        if root_pos + root_size > self.length:
            raise ValueError(f"{path}: root directory runs past the end")

        self.files = {}          # normalised path -> RezEntry
        self._read_dir(root_pos, root_size, "")

    # A directory block is a flat run of entries. Recursion is on the
    # subdirectories it names, not on the bytes.
    def _read_dir(self, pos, size, prefix):
        self.f.seek(pos)
        blk = self.f.read(size)
        if len(blk) != size:
            raise ValueError(f"{self.path}: short directory block at {pos:#x}")
        o = 0
        subdirs = []
        while o < size:
            (kind,) = struct.unpack_from("<I", blk, o)
            o += 4
            if kind == 1:
                dpos, dsize, _time = struct.unpack_from("<III", blk, o)
                o += 12
                name, o = self._cstr(blk, o)
                subdirs.append((dpos, dsize, prefix + name + "/"))
            elif kind == 0:
                fpos, fsize, _time, ident, ext, nkeys = struct.unpack_from("<IIIIII", blk, o)
                o += 24
                name, o = self._cstr(blk, o)
                _comment, o = self._cstr(blk, o)
                if nkeys:
                    raise ValueError(f"{self.path}: {name} has {nkeys} keys; "
                                     "the key block is not decoded")
                if fpos + fsize > self.length:
                    raise ValueError(f"{self.path}: {name} runs past the end "
                                     f"({fpos:#x}+{fsize}) - the reading is wrong")
                # Four characters, stored reversed: an .ABC model reads "CBA"
                # in file order. Strip the padding BEFORE reversing - reverse
                # first and the NUL ends up at the front where rstrip cannot
                # see it, which silently names every file ".\0ABC" and makes
                # all 4754 of them miss.
                raw = struct.pack("<I", ext).rstrip(b"\0")
                e = raw[::-1].decode("latin-1")
                full = prefix + name
                if e:
                    full += "." + e
                self.files[norm(full)] = RezEntry(full, fpos, fsize, e, ident)
            else:
                raise ValueError(f"{self.path}: entry type {kind} at {pos + o:#x}")
        if o != size:
            raise ValueError(f"{self.path}: directory block overran by {o - size}")
        for dpos, dsize, pfx in subdirs:
            self._read_dir(dpos, dsize, pfx)

    @staticmethod
    def _cstr(blk, o):
        end = blk.index(b"\0", o)
        return blk[o:end].decode("latin-1"), end + 1

    def read(self, path):
        e = self.files.get(norm(path))
        if e is None:
            return None
        self.f.seek(e.pos)
        return self.f.read(e.size)


def norm(p):
    return p.replace("\\", "/").upper().lstrip("/")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    rez = RezFile(args[0])
    print(f"{args[0]}: {len(rez.files)} files")

    if "--verify" in args:
        root = args[args.index("--verify") + 1]
        # Every file lithrez extracted must be present here, at the same size,
        # with the same bytes. That is the whole check: the extraction was made
        # by Monolith's own tool and this reading had no part in it.
        missing = extra = mismatch = 0
        on_disk = {}
        for dirpath, _dirs, names in os.walk(root):
            for n in names:
                fp = os.path.join(dirpath, n)
                rel = norm(os.path.relpath(fp, root))
                on_disk[rel] = fp
        for rel, fp in sorted(on_disk.items()):
            e = rez.files.get(rel)
            if e is None:
                missing += 1
                if missing <= 5:
                    print(f"  MISSING from the rez reading: {rel}")
                continue
            want = os.path.getsize(fp)
            if want != e.size:
                mismatch += 1
                if mismatch <= 5:
                    print(f"  SIZE {rel}: rez {e.size}, extracted {want}")
        for rel in rez.files:
            if rel not in on_disk:
                extra += 1
                if extra <= 5:
                    print(f"  EXTRA in the rez reading: {rel}")
        # Content, on a sample spread across the archive rather than the first
        # few - a wrong `pos` on entry 4000 is exactly what a first-few check
        # cannot see.
        keys = sorted(rez.files)
        step = max(1, len(keys) // 200)
        bad = checked = 0
        for rel in keys[::step]:
            fp = on_disk.get(rel)
            if not fp:
                continue
            checked += 1
            if rez.read(rel) != open(fp, "rb").read():
                bad += 1
                if bad <= 5:
                    print(f"  CONTENT differs: {rel}")
        print(f"  on disk {len(on_disk)}, in rez {len(rez.files)}")
        print(f"  missing {missing}, extra {extra}, size mismatches {mismatch}")
        print(f"  content checked {checked}, differing {bad}")
        ok = not (missing or extra or mismatch or bad)
        print("  VERIFIED" if ok else "  FAILED")
        return 0 if ok else 1

    if "--get" in args:
        i = args.index("--get")
        data = rez.read(args[i + 1])
        if data is None:
            print(f"not found: {args[i + 1]}")
            return 1
        open(args[i + 2], "wb").write(data)
        print(f"wrote {len(data)} bytes to {args[i + 2]}")
        return 0

    # --all lists every file; the default is a 20-line taste.
    for k in sorted(rez.files)[: (None if '--all' in sys.argv else 20)]:
        print("  ", rez.files[k])
    if len(rez.files) > 20:
        print(f"   ... and {len(rez.files) - 20} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
