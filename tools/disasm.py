"""Disassemble a range of lithtech.exe (or any 32-bit PE) by virtual address.

The engine's decision about whether to keep using a renderer is made in the
instructions after it calls Init. Those instructions are in a retail binary we
already have on disk, so reading them is cheaper - and far more reliable - than
inferring the decision from the outside.

  python tools/disasm.py 0x462500 0x120
  python tools/disasm.py 0x462310 0x60 --exe game/lithtech.exe

Prints VA, bytes and mnemonic. Resolves nothing; this is a reading tool, not a
decompiler.
"""

import argparse
import os
import struct
import sys

import capstone


def sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
    n_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    size_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    base = struct.unpack_from("<I", data, e_lfanew + 24 + 28)[0]
    off = e_lfanew + 24 + size_opt
    out = []
    for i in range(n_sections):
        s = data[off + i * 40: off + (i + 1) * 40]
        name = s[:8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", s, 8)
        out.append((name, va, vsize, raw, rawsize))
    return base, out


def va_to_off(base, secs, va):
    rva = va - base
    for name, sva, vsize, raw, rawsize in secs:
        if sva <= rva < sva + max(vsize, rawsize):
            return raw + (rva - sva), name
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va", help="virtual address, e.g. 0x462500")
    ap.add_argument("length", nargs="?", default="0x80", help="bytes to decode")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--back", default="0", help="start this many bytes earlier")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "lithtech.exe")
    with open(exe, "rb") as f:
        data = f.read()

    base, secs = sections(data)
    va = int(args.va, 0) - int(args.back, 0)
    n = int(args.length, 0)
    off, name = va_to_off(base, secs, va)
    if off is None:
        print("VA %08X is not inside any section" % va)
        return 1

    print("%s  image base %08X  VA %08X -> file offset %08X in %s"
          % (os.path.basename(exe), base, va, off, name))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for ins in md.disasm(data[off:off + n], va):
        print("  %08X  %-20s %s %s"
              % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
