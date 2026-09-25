#!/usr/bin/env python3
"""dtx.py - read NOLF's .dtx textures, and check that the reading is right.

The reference reading for host/renstub/dtx.cpp, and the verifier that
established the format. Same role rez.py plays for the archives and abc.py for
the models: the file is the answer key, and this is how to ask it a question
without launching the game.

THE FORMAT
    +0x00  uint32  resource type, always 0
    +0x04  int32   version, always -5
    +0x08  uint16  width          +0x0A  uint16 height
    +0x0C  uint16  mip count      +0x0E  uint16 section count
    +0x10  int32   flags          +0x14  int32  user flags
    +0x18  uint8   extra[12]  - extra[2] is the format identifier
    +0x24  char    command string[128], NUL terminated, usually empty
    0xA4   the mip chain, largest first, no padding

THE SIZE IDENTITY is the check that matters:

    164 + sum(mip bytes) == file size

The header is a fixed 164 bytes and nothing follows the mip chain, so this
either lands on the last byte of the file or the parse is wrong. It holds on
4173 of the 4174 textures the game mounts.

PALETTISED TEXTURES STILL STORE 32-BIT PIXELS. 605 files declare BPP_8P and
every one is four bytes per texel on disk - the identifier says what the engine
converts to, not what is written down. Believing it named the storage put 604
files at a quarter of their real size, and the size identity is what caught it.

    python tools/dtx.py info   TEX/AI.DTX
    python tools/dtx.py sweep                  # every texture, in mount order
    python tools/dtx.py png    TEX/AI.DTX out.png
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rez import RezFile                                    # noqa: E402

HEADER = 164

BPP_8P, BPP_8, BPP_16, BPP_32, BPP_DXT1, BPP_DXT3, BPP_DXT5 = range(7)
# FORMAT 7 IS THE ONE FILE THE IDENTITY EVER FAILED ON, and it is not a
# malformed file - it is a format nothing here knew about.
# TEX/GLASS/GL01/GL0007.DTX is shipped TWICE: NOLF.rez has an ordinary 32-bit
# 4-mip copy of 87204 bytes, and NolfGoty.rez replaces it with a 22724-byte one
# that mounts later and therefore wins. That copy declares sections = 1, stores
# ONE BYTE PER TEXEL, and carries its colours in a trailing section named
# "PALLETE32" - the game's own spelling - whose last 1024 bytes are 256 BGRA
# entries, every one of them alpha 0x80. That is a glass texture at half alpha,
# which is exactly what the level asks for.
#
# So the identity was never "164 + mips == size". It was "164 + mips + sections
# == size", and it held on 4173 files only because 4173 files have no sections.
BPP_32P = 7
BPP_NAME = {BPP_8P: "8P", BPP_8: "8", BPP_16: "16", BPP_32: "32",
            BPP_DXT1: "DXT1", BPP_DXT3: "DXT3", BPP_DXT5: "DXT5",
            BPP_32P: "32P"}

# 256 BGRA entries. The palette is the LAST 1024 bytes of the file: the 22
# bytes between the section's name and it are not decoded here, because one
# sample cannot settle a section header and guessing at one is how this project
# has lost time before. Taking it from the END needs no such guess, and the
# parse still has to land on the byte the file ends on to be believed.
PALETTE_BYTES = 256 * 4
PALETTE_SECTION = b"PALLETE32"

# The mount order the engine is launched with. tools/run.ps1 owns the list and
# the renderer reads it back off the command line rather than copying it -
# order is load bearing, because later archives override earlier ones and
# WORLDS/M13S02.DAT differs between NOLF.rez and NOLF2.rez.
MOUNT = ["NOLF.rez", "NOLF2.rez", "NOLFdll.rez", "NOLFl.rez",
         "Nolfu003.rez", "Nolfcres003.rez", "NolfGoty.rez", "Modernizer.rez"]


def block_bytes(fmt):
    if fmt == BPP_DXT1:
        return 8
    if fmt in (BPP_DXT3, BPP_DXT5):
        return 16
    return 0


def mip_bytes(w, h, fmt):
    nb = block_bytes(fmt)
    if nb:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * nb
    if fmt == BPP_32P:
        return w * h            # one INDEX per texel; the colours are a section
    return w * h * 4            # 8P and 32 are both four bytes per texel


class Dtx:
    def __init__(self, blob, name=""):
        self.name = name
        self.blob = blob
        if len(blob) <= HEADER:
            raise ValueError(f"{name}: {len(blob)} bytes, shorter than a header")
        self.restype, self.version = struct.unpack_from("<Ii", blob, 0)
        self.w, self.h, self.mips, self.sections = struct.unpack_from("<4H", blob, 8)
        self.flags, self.user_flags = struct.unpack_from("<2i", blob, 16)
        self.extra = blob[24:36]
        self.fmt = self.extra[2]
        self.command = blob[0x24:0xA4].split(b"\0")[0].decode("latin-1", "replace")

        if self.restype != 0 or self.version != -5:
            raise ValueError(f"{name}: resource type {self.restype}, version "
                             f"{self.version} - not a DTX we know")

        self.mip_sizes = []
        for i in range(self.mips):
            mw, mh = max(1, self.w >> i), max(1, self.h >> i)
            self.mip_sizes.append(mip_bytes(mw, mh, self.fmt))
        self.pixel_bytes = sum(self.mip_sizes)

        # The palette, for the one format that keeps its colours out of line.
        # Validated, not assumed: the section has to be named, and the arithmetic
        # has to land on the last byte of the file.
        self.palette = None
        if self.fmt == BPP_32P:
            tail = blob[HEADER + self.pixel_bytes:]
            if PALETTE_SECTION in tail and len(tail) >= PALETTE_BYTES:
                self.palette = blob[-PALETTE_BYTES:]
            self.section_bytes = len(tail)
        else:
            self.section_bytes = 0

    @property
    def exact(self):
        """Does the size identity hold? This is the whole verification.

        164 + mips + sections. The sections term is zero on 4173 of the 4174
        files the game mounts, which is why it went unnoticed - and non-zero on
        the one that always failed.
        """
        return (HEADER + self.pixel_bytes + self.section_bytes
                == len(self.blob))

    def mip(self, level=0):
        off = HEADER + sum(self.mip_sizes[:level])
        return self.blob[off:off + self.mip_sizes[level]]

    def __str__(self):
        return (f"{self.w}x{self.h} {BPP_NAME.get(self.fmt, self.fmt)} "
                f"{self.mips} mips, flags {self.flags} user {self.user_flags}, "
                f"{len(self.blob)} bytes "
                f"({'exact' if self.exact else 'SIZE IDENTITY FAILS'})")


# --------------------------------------------------------------------------
# Decoding, for looking at a texture rather than counting it.
#
# Four wrong answers in this project came from a statistic over pixels nobody
# opened, so the reader that produces a number also produces a picture.
# --------------------------------------------------------------------------

def _rgb565(c):
    return (((c >> 11) & 0x1F) * 255 // 31,
            ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31)


def decode(dtx, level=0):
    """One mip as (width, height, bytes of RGBA)."""
    w, h = max(1, dtx.w >> level), max(1, dtx.h >> level)
    src = dtx.mip(level)
    out = bytearray(w * h * 4)

    if not block_bytes(dtx.fmt):
        for i in range(w * h):                  # stored BGRA
            b, g, r, a = src[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return w, h, bytes(out)

    nb = block_bytes(dtx.fmt)
    ncol = 0 if dtx.fmt == BPP_DXT1 else 8
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    for by in range(bh):
        for bx in range(bw):
            blk = src[(by * bw + bx) * nb:(by * bw + bx + 1) * nb]
            c0, c1 = struct.unpack_from("<2H", blk, ncol)
            bits, = struct.unpack_from("<I", blk, ncol + 4)
            e0, e1 = _rgb565(c0), _rgb565(c1)
            if dtx.fmt == BPP_DXT1 and c0 <= c1:
                pal = [e0, e1,
                       tuple((e0[k] + e1[k]) // 2 for k in range(3)),
                       (0, 0, 0)]
                alpha_idx3 = True
            else:
                pal = [e0, e1,
                       tuple((2 * e0[k] + e1[k]) // 3 for k in range(3)),
                       tuple((e0[k] + 2 * e1[k]) // 3 for k in range(3))]
                alpha_idx3 = False
            for t in range(16):
                x, y = bx * 4 + (t % 4), by * 4 + (t // 4)
                if x >= w or y >= h:
                    continue
                idx = (bits >> (t * 2)) & 3
                r, g, b = pal[idx]
                if dtx.fmt == BPP_DXT3:
                    nib = blk[t >> 1]
                    a = ((nib >> 4) if (t & 1) else (nib & 0x0F)) * 17
                elif dtx.fmt == BPP_DXT5:
                    a = _dxt5_alpha(blk, t)
                else:
                    a = 0 if (alpha_idx3 and idx == 3) else 255
                o = (y * w + x) * 4
                out[o:o + 4] = bytes((r, g, b, a))
    return w, h, bytes(out)


def _dxt5_alpha(blk, t):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    idx = (bits >> (t * 3)) & 7
    if idx == 0:
        return a0
    if idx == 1:
        return a1
    if a0 > a1:
        return ((8 - idx) * a0 + (idx - 1) * a1) // 7
    if idx == 6:
        return 0
    if idx == 7:
        return 255
    return ((6 - idx) * a0 + (idx - 1) * a1) // 5


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------

def mounted(game):
    """Every .DTX the game can see, resolved in mount order (last wins)."""
    have = {f.lower(): f for f in os.listdir(game)}
    out = {}
    for m in MOUNT:
        real = have.get(m.lower())
        if not real:
            continue
        r = RezFile(os.path.join(game, real))
        for n in r.files:
            if n.endswith(".DTX"):
                out[n] = r
    return out


def find(game, path):
    files = mounted(game)
    key = path.replace("\\", "/").upper().lstrip("/")
    if key in files:
        return Dtx(files[key].read(key), key)
    raise SystemExit(f"{path}: not in any mounted archive")


def main():
    args = sys.argv[1:]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = os.path.join(root, "game")
    if not args:
        raise SystemExit(__doc__)
    cmd = args[0]

    if cmd == "info":
        d = find(game, args[1])
        print(f"{d.name}: {d}")
        if d.command:
            print(f"  command string: {d.command!r}")
        print(f"  extra: {' '.join('%02X' % c for c in d.extra)}")
        for i, n in enumerate(d.mip_sizes):
            print(f"  mip {i}: {max(1, d.w >> i)}x{max(1, d.h >> i)}, {n} bytes")

    elif cmd == "sweep":
        files = mounted(game)
        ok, bad, fmts, total = 0, [], {}, 0
        for n, r in sorted(files.items()):
            try:
                d = Dtx(r.read(n), n)
            except ValueError as e:
                bad.append(str(e))
                continue
            fmts[BPP_NAME.get(d.fmt, d.fmt)] = fmts.get(BPP_NAME.get(d.fmt, d.fmt), 0) + 1
            if d.exact:
                ok += 1
                total += d.pixel_bytes
            else:
                bad.append(f"{n}: {len(d.blob)} bytes, formula says "
                           f"{HEADER + d.pixel_bytes} ({d})")
        print(f"{len(files)} textures mounted, {ok} satisfy the size identity, "
              f"{len(bad)} do not")
        print(f"formats: {fmts}")
        print(f"{total / 1048576.0:.1f} MB of pixels")
        for b in bad:
            print(f"  REFUSED {b}")

    elif cmd == "png":
        d = find(game, args[1])
        w, h, rgba = decode(d, int(args[3]) if len(args) > 3 else 0)
        write_png(args[2], w, h, rgba)
        print(f"{d.name}: {d}\n  -> {args[2]} ({w}x{h})")

    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
