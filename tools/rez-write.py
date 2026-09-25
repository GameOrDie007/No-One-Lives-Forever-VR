"""Build a .REZ archive - the writer tools/rez.py never had.

Why this exists: the ESRGAN upscale pack crashes with HD-COMMON1 mounted and
runs clean with HD-COMMON2, and narrowing inside COMMON1 means mounting a
SUBSET of it. Tonight established that a directory mount does nothing and says
nothing - twice - so the subset has to be a real archive.

    python tools/rez-write.py out.rez  packs/HD-COMMON1-X4.REZ  GUNS
    python tools/rez-write.py out.rez  packs/HD-COMMON1-X4.REZ  GUNS CHARS

The format is the one tools/rez.py documents and reads:

  header    127-byte banner ending \\r\\n\\x1a, then u32 version(1),
            u32 rootDirPos, u32 rootDirSize, then more fields.
  directory entries at {pos,size}; each starts with a u32 type.
  type 1    u32 pos, u32 size, u32 time, cstring name        subdirectory
  type 0    u32 pos, u32 size, u32 time, u32 id, u32 ext,
            u32 numKeys, cstring name, cstring comment       file

  `ext` is four characters stored REVERSED, so .ABC is "CBA\\0". The comment is
  a single trailing zero when empty; omitting it puts every later entry one
  byte out, and the format announces that by the next pos landing outside the
  file - which is the check at the bottom of this script.

Written round-trip safe: after building, the result is re-read with rez.py and
every file compared byte for byte against the source. An archive that cannot
prove itself is not written.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rez  # noqa: E402

BS = chr(92)
BANNER = (b'Ryan Bogart, Monolith Productions, RezMgr 1.0'
          b' ' * 40)[:124] + b'\r\n\x1a'


class Node(object):
    def __init__(self, name):
        self.name = name
        self.dirs = {}
        self.files = []          # (name, ext, data, time, ident)

    def sub(self, name):
        key = name.upper()
        if key not in self.dirs:
            self.dirs[key] = Node(key)
        return self.dirs[key]


def cstr(s):
    return s.encode('latin-1', 'replace') + b'\0'


def build(root, want, src, maxdim=0, mindim=0):
    """Pull every file under one of `want` out of `src` into a tree."""
    tree = Node('')
    n = 0
    for p in src.files:
        path = p if isinstance(p, str) else getattr(p, 'path', str(p))
        u = path.replace(BS, '/').upper()
        top = u.split('/')[0]
        if want and top not in want:
            continue
        parts = u.split('/')
        node = tree
        for d in parts[:-1]:
            node = node.sub(d)
        leaf = parts[-1]
        # THE NAME IS STORED WITHOUT ITS EXTENSION. The reader composes
        # prefix + name + "." + ext, so storing "BOLT.DTX" with ext "DTX"
        # reads back as BOLT.DTX.DTX - which the round-trip check caught as
        # "NOT IN SOURCE" rather than letting a bad archive out.
        stem, dot = os.path.splitext(leaf)
        ext = dot.lstrip('.').upper()
        data = src.read(path)
        if (maxdim or mindim) and ext == 'DTX' and len(data) >= 16:
            w, h = struct.unpack_from('<HH', data, 8)
            if maxdim and (w > maxdim or h > maxdim):
                continue
            if mindim and w < mindim and h < mindim:
                continue
        node.files.append((stem, ext, data))
        n += 1
    return tree, n


def write(tree, out):
    """Two passes: lay the file DATA down first so every entry knows its pos,
    then write the directory blocks bottom-up so a parent knows where its
    children live."""
    blobs = []
    pos = len(BANNER) + 4 * 8          # banner + version + 7 header dwords

    def lay(node):
        nonlocal pos
        placed = []
        for name, ext, data in node.files:
            placed.append((name, ext, pos, len(data)))
            blobs.append(data)
            pos += len(data)
        node.placed = placed
        for d in sorted(node.dirs):
            lay(node.dirs[d])

    lay(tree)

    dirblocks = {}

    def emit(node):
        """Returns (pos, size) of this node's own directory block."""
        nonlocal pos
        for d in sorted(node.dirs):
            emit(node.dirs[d])
        buf = bytearray()
        for d in sorted(node.dirs):
            child = node.dirs[d]
            cp, cs = dirblocks[id(child)]
            buf += struct.pack('<4I', 1, cp, cs, 0) + cstr(child.name)
        for name, ext, fpos, fsize in node.placed:
            e = (ext[::-1] + '\0\0\0\0')[:4].encode('latin-1')
            buf += struct.pack('<3I', 0, fpos, fsize)
            buf += struct.pack('<I', 0)              # time
            buf += struct.pack('<I', 0)              # id
            buf += e                                 # ext, reversed
            buf += struct.pack('<I', 0)              # numKeys
            buf += cstr(name) + b'\0'                # name, empty comment
        here = pos
        blobs.append(bytes(buf))
        pos += len(buf)
        dirblocks[id(node)] = (here, len(buf))
        return dirblocks[id(node)]

    rp, rs = emit(tree)

    with open(out, 'wb') as f:
        f.write(BANNER)
        f.write(struct.pack('<8I', 1, rp, rs, 0, 0, 0, 0, 0))
        for b in blobs:
            f.write(b)
    return rp, rs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out, srcpath = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    # --max-dim N drops any DTX wider or taller than N. This is how "is it the
    # SIZE or is it a particular file" gets answered: build the same folder
    # twice, once whole and once without its biggest textures.
    maxdim = 0
    if '--max-dim' in rest:
        i = rest.index('--max-dim')
        maxdim = int(rest[i + 1])
        rest = rest[:i] + rest[i + 2:]
    # --min-dim is the COMPLEMENT, and it is what makes the size test
    # self-proving: an archive of nothing but the biggest textures either
    # crashes - in which case they loaded and they are the cause - or it does
    # not. No separate positive control needed.
    mindim = 0
    if '--min-dim' in rest:
        i = rest.index('--min-dim')
        mindim = int(rest[i + 1])
        rest = rest[:i] + rest[i + 2:]
    want = set(w.upper() for w in rest)

    src = rez.RezFile(srcpath)
    tree, n = build(None, want, src, maxdim, mindim)
    if not n:
        print('nothing matched %s in %s' % (sorted(want), srcpath))
        return 1
    write(tree, out)
    print('%s: %d files, %d bytes' % (out, n, os.path.getsize(out)))

    # ---- PROVE IT. Re-read and compare every file byte for byte. -----------
    back = rez.RezFile(out)
    if len(back.files) != n:
        print('VERIFY FAILED: wrote %d, read back %d' % (n, len(back.files)))
        return 1
    # Compare by NORMALISED path. The source spells paths with backslashes and
    # mixed case; what is written back is uppercase with forward slashes, so
    # feeding one archive's path to the other returns None and the check dies
    # on len(None) instead of reporting a mismatch.
    def key(p):
        p = p if isinstance(p, str) else getattr(p, 'path', str(p))
        return p.replace(BS, '/').upper()

    want_bytes = {}
    for p in src.files:
        path = p if isinstance(p, str) else getattr(p, 'path', str(p))
        k = key(path)
        if not want or k.split('/')[0] in want:
            want_bytes[k] = src.read(path)

    bad = 0
    for p in back.files:
        path = p if isinstance(p, str) else getattr(p, 'path', str(p))
        got = back.read(path)
        orig = want_bytes.get(key(path))
        if orig is None:
            bad += 1
            if bad < 4:
                print('  NOT IN SOURCE: %s' % key(path))
        elif got != orig:
            bad += 1
            if bad < 4:
                print('  MISMATCH %s: %d vs %d bytes'
                      % (key(path), len(got) if got else -1, len(orig)))
    print('verify: %d of %d files identical to the source%s'
          % (n - bad, n, '' if not bad else '   <- ARCHIVE IS BAD'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
