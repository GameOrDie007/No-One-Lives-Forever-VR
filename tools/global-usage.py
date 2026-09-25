"""Every field the renderer reads through one of its own globals.

docs/WORLD-REACHED.md found the world: it arrives in the first dword of
RebindLightmaps' argument, and d3d.ren keeps it in the global at 0x1008BEAC
(written by the scene setup function at 0x1001EB90 as `world = [handle + 8]`).

Everything the renderer knows about the world it reaches from there, so the set
of `[world + N]` accesses across .text IS the world structure's field list - as
far as the renderer uses it, which is the only part we have to reimplement.

  python tools/global-usage.py 0x1008beac                 # the world
  python tools/global-usage.py 0x1008beb0                 # the scene description
  python tools/global-usage.py 0x1008d988 --exe game/d3d.ren

METHOD

Same taint tracking as tools/ren-anatomy.py: a register loaded from the global
is tainted, any write to it clears the taint, and calls clear the volatile
registers. So an offset reported here was reached through that global and not
through a register that happened to survive from somewhere else.

It prints the function each access sits in, found by walking back to the
nearest `push ebp; mov ebp, esp`, because the interesting question is usually
"what code walks this" rather than "which offsets exist".

WHAT IT DOES NOT DO

It does not follow the pointer. `[world + 0x18C]` is an array of pointers to
other structures, and those structures are reached through registers this knows
nothing about - point it at the next global, or read the function it names.
"""

import argparse
import collections
import os
import struct
import sys

import capstone


def sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
    n = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    size_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    base = struct.unpack_from("<I", data, e_lfanew + 24 + 28)[0]
    off = e_lfanew + 24 + size_opt
    out = []
    for i in range(n):
        s = data[off + i * 40: off + (i + 1) * 40]
        name = s[:8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", s, 8)
        out.append((name, va, vsize, raw, rawsize))
    return base, out


def kind(ins):
    if ins.mnemonic.startswith("f"):
        return "float"
    if ins.mnemonic in ("movsx", "movzx"):
        return "short/byte"
    if ins.mnemonic == "lea":
        return "addr"
    if ins.mnemonic == "call":
        return "CALL"
    return "dword"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", help="the global's address, e.g. 0x1008beac")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--sites", type=int, default=4)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "d3d.ren")
    with open(exe, "rb") as f:
        data = f.read()

    base, secs = sections(data)
    text = [s for s in secs if s[0] == ".text"][0]
    _, va, vsize, raw, rawsize = text
    code = data[raw:raw + max(vsize, rawsize)]
    start = base + va
    target = int(args.addr, 0)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    stream = list(md.disasm(code, start))

    # Function starts, so an access can be attributed to something callable.
    starts = []
    for i, ins in enumerate(stream):
        if ins.mnemonic == "push" and ins.op_str == "ebp" \
                and i + 1 < len(stream) \
                and stream[i + 1].mnemonic == "mov" \
                and stream[i + 1].op_str == "ebp, esp":
            starts.append(ins.address)

    def owner(addr):
        lo, hi = 0, len(starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if starts[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        return starts[lo - 1] if lo else 0

    tainted = set()
    seen = collections.OrderedDict()   # offset -> [kind, count, sites, funcs]
    nload = 0

    for ins in stream:
        ops = ins.operands

        if ins.mnemonic == "lea" and len(ops) == 2 \
                and ops[1].type == capstone.x86.X86_OP_MEM \
                and ops[1].mem.disp == 0 and ops[1].mem.index == 0 \
                and ops[1].mem.base and ops[0].type == capstone.x86.X86_OP_REG \
                and ins.reg_name(ops[1].mem.base) == ins.reg_name(ops[0].reg):
            continue                        # alignment padding

        for op in ops:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base == 0 or ins.reg_name(op.mem.base) not in tainted:
                continue
            d = op.mem.disp
            if d < 0:
                continue
            idx = "+reg*%d" % op.mem.scale if op.mem.index else ""
            e = seen.setdefault(d, [kind(ins), 0, [], set(), idx])
            e[1] += 1
            if e[0] == "dword" and kind(ins) != "dword":
                e[0] = kind(ins)
            if idx and not e[4]:
                e[4] = idx
            if len(e[2]) < args.sites:
                e[2].append((ins.address, "%s %s" % (ins.mnemonic, ins.op_str)))
            e[3].add(owner(ins.address))

        _, regs_w = ins.regs_access()
        for r in regs_w:
            tainted.discard(ins.reg_name(r))
        if ins.mnemonic == "call":
            tainted -= {"eax", "ecx", "edx"}
        if ins.mnemonic == "mov" and len(ops) == 2 \
                and ops[0].type == capstone.x86.X86_OP_REG \
                and ops[1].type == capstone.x86.X86_OP_MEM \
                and ops[1].mem.base == 0 and ops[1].mem.index == 0 \
                and (ops[1].mem.disp & 0xFFFFFFFF) == target:
            tainted.add(ins.reg_name(ops[0].reg))
            nload += 1

    print("%s  global %08X  loaded into a register at %d sites"
          % (os.path.basename(exe), target, nload))
    if not nload:
        print()
        print("  Never loaded. Either the address is wrong, or it is only")
        print("  reached as [global] directly rather than through a register.")
        return 0
    if not seen:
        print()
        print("  Loaded, but nothing is read through it here - it is passed")
        print("  somewhere else. Follow it in the functions that load it.")
        return 0

    print()
    print("=== fields reached through it ===")
    print("  offset  word  uses  as          in functions")
    for d in sorted(seen):
        k, cnt, sites, funcs, idx = seen[d]
        fs = " ".join("%08X" % f for f in sorted(funcs)[:5])
        print("  %6d  %4s  %4d  %-10s  %s%s"
              % (d, (d // 4) if d % 4 == 0 else "-", cnt, k + idx, fs,
                 " ..." if len(funcs) > 5 else ""))
        for a, txt in sites[:2]:
            print("  %38s%08X  %s" % ("", a, txt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
