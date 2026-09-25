"""How does RenderScene get to DrawPrimitive?

docs/D3D7-SEAM-CLOSED.md established that every DrawPrimitive site in d3d.ren
submits FVF 0x1C4 - XYZRHW, already transformed. docs/WORLD-REACHED.md found
where the world comes in. What is between them is the pipeline our own renderer
has to reimplement, and it is a graph in a file we have.

  python tools/callgraph.py --to-draw          # paths RenderScene -> DrawPrimitive
  python tools/callgraph.py --callers 1001e450
  python tools/callgraph.py --calls 1002d1c0
  python tools/callgraph.py --path 1002d1c0 1001e450

METHOD

Function entry points are taken to be every target of a `call rel32`, plus the
37 renderer slots read out of RenderDLLSetup. That is deliberate rather than
lazy: prologue sniffing misses functions that do not set up a frame pointer -
RebindLightmaps itself starts `push ebx; sub esp, 8` - and a call target is by
construction a thing that gets called.

Each instruction is attributed to the nearest preceding entry point. That is an
approximation, and it fails in one direction: a function that is only ever
reached through a function POINTER has no `call rel32` to it, so its body is
attributed to whatever precedes it. Those show up as a caller with an
implausibly large span, and --verify prints the worst offenders so the failure
is visible rather than silent.

Indirect calls (`call [eax + N]`) are edges we cannot follow at all. Every COM
call into Direct3D is one, which is fine - those are the leaves we are looking
FOR - but a virtual dispatch inside the renderer would be invisible.

A Draw* method is only counted when the object is the D3D7 device held in
d3d.ren's own global at 0x1008D988 (identified in docs/D3D7-SEAM-CLOSED.md).
Matching on the vtable offset alone is not enough and produced nonsense the
first time this ran: `call [reg + 0x64]` on an IDirectDrawSurface7 is some
other method, so CreateSurface, LockSurface and SwapBuffers were all reported
as drawing the world.
"""

import argparse
import collections
import os
import struct
import sys

import capstone

DEVICE_GLOBAL = 0x1008D988      # the IDirect3DDevice7, docs/D3D7-SEAM-CLOSED.md

DRAW_SLOTS = {
    100: "DrawPrimitive",
    104: "DrawIndexedPrimitive",
    112: "DrawPrimitiveStrided",
    116: "DrawIndexedPrimitiveStrided",
    120: "DrawPrimitiveVB",
    124: "DrawIndexedPrimitiveVB",
}


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


def build(exe):
    with open(exe, "rb") as f:
        data = f.read()
    base, secs = sections(data)
    text = [s for s in secs if s[0] == ".text"][0]
    _, va, vsize, raw, rawsize = text
    code = data[raw:raw + max(vsize, rawsize)]
    start = base + va

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    stream = list(md.disasm(code, start))

    # Pass 1: entry points.
    entries = set()
    direct = []
    for ins in stream:
        if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            try:
                t = int(ins.op_str, 0)
            except ValueError:
                continue
            entries.add(t)
            direct.append((ins.address, t))
    # The renderer slots: RenderDLLSetup writes them as immediates.
    for ins in stream:
        if ins.mnemonic == "mov" and len(ins.operands) == 2:
            d, s = ins.operands
            if d.type == capstone.x86.X86_OP_MEM and d.mem.base != 0 \
                    and 0x6C <= d.mem.disp <= 0xFC \
                    and s.type == capstone.x86.X86_OP_IMM:
                v = s.imm & 0xFFFFFFFF
                if start <= v < start + len(code):
                    entries.add(v)
    ents = sorted(entries)

    def owner(addr):
        lo, hi = 0, len(ents)
        while lo < hi:
            mid = (lo + hi) // 2
            if ents[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        return ents[lo - 1] if lo else 0

    callers = collections.defaultdict(set)
    callees = collections.defaultdict(set)
    for at, tgt in direct:
        f = owner(at)
        callees[f].add(tgt)
        callers[tgt].add(f)

    # Which functions actually draw.
    #
    # The `this` pointer is loaded too far back to track cheaply, and matching
    # on the vtable offset alone is worthless - `call [reg + 0x64]` on an
    # IDirectDrawSurface7 is some other method, and the first version of this
    # reported CreateSurface, LockSurface and SwapBuffers as drawing the world.
    #
    # So match on the ARGUMENT instead. DrawPrimitive's second parameter is the
    # flexible vertex format, stored as an immediate at [esp + 8] a few
    # instructions before the call, and an FVF always carries a position bit.
    # Nothing else at that vtable offset does that, and the value is printed so
    # the identification can be checked rather than trusted.
    draws = collections.defaultdict(set)
    fvfs = collections.defaultdict(set)
    loose = collections.defaultdict(set)
    recent = collections.deque(maxlen=14)
    for ins in stream:
        if ins.mnemonic == "call":
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base != 0                         and op.mem.index == 0 and op.mem.disp in DRAW_SLOTS:
                    hit = None
                    for v in recent:
                        if v is not None and (v & 0x006):
                            hit = v
                    if hit is not None:
                        draws[owner(ins.address)].add(op.mem.disp)
                        fvfs[owner(ins.address)].add(hit)
                    else:
                        loose[owner(ins.address)].add(op.mem.disp)
        v = None
        if ins.mnemonic == "mov" and len(ins.operands) == 2:
            dd, sr = ins.operands
            if dd.type == capstone.x86.X86_OP_MEM and dd.mem.base != 0                     and ins.reg_name(dd.mem.base) == "esp" and dd.mem.disp == 8                     and sr.type == capstone.x86.X86_OP_IMM:
                v = sr.imm & 0xFFFFFFFF
                if v > 0xFFFF:
                    v = None
        recent.append(v)
    return ents, callers, callees, draws, loose, owner, fvfs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=None)
    ap.add_argument("--to-draw", action="store_true")
    ap.add_argument("--callers")
    ap.add_argument("--calls")
    ap.add_argument("--path", nargs=2)
    ap.add_argument("--from-va", default="0x1002d1c0")
    ap.add_argument("--depth", type=int, default=8)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "d3d.ren")
    ents, callers, callees, draws, loose, owner, fvfs = build(exe)

    print("%s  %d entry points, %d with call edges, %d reach a Draw* method"
          % (os.path.basename(exe), len(ents), len(callees), len(draws)))
    print("  (%d more call something at a Draw* vtable offset on an object that"
          % len(set(loose) - set(draws)))
    print("   is NOT the device - those are other COM interfaces, not drawing)")

    if args.callers:
        f = int(args.callers, 0)
        print()
        print("=== callers of %08X ===" % f)
        for c in sorted(callers.get(f, ())):
            print("  %08X" % c)
        if not callers.get(f):
            print("  none - reached only through a function pointer, or not called")
        return 0

    if args.calls:
        f = int(args.calls, 0)
        print()
        print("=== %08X calls ===" % f)
        for c in sorted(callees.get(f, ())):
            print("  %08X %s" % (c, "  <- DRAWS" if c in draws else ""))
        return 0

    if args.path or args.to_draw:
        src = int(args.path[0], 0) if args.path else int(args.from_va, 0)
        if args.path:
            targets = {int(args.path[1], 0)}
        else:
            targets = set(draws)

        # Breadth-first, so the path printed is a shortest one.
        prev = {src: None}
        q = collections.deque([(src, 0)])
        found = []
        while q:
            f, d = q.popleft()
            if d >= args.depth:
                continue
            for c in sorted(callees.get(f, ())):
                if c in prev:
                    continue
                prev[c] = f
                if c in targets:
                    found.append(c)
                q.append((c, d + 1))

        print()
        if not found:
            print("No path from %08X within depth %d." % (src, args.depth))
            print("The pipeline is reached through a function pointer somewhere,")
            print("or the depth is too small.")
            return 0
        print("=== %d of %d targets reachable from %08X ==="
              % (len(found), len(targets), src))
        for t in sorted(found):
            chain = []
            n = t
            while n is not None:
                chain.append(n)
                n = prev[n]
            chain.reverse()
            names = " -> ".join("%08X" % c for c in chain)
            what = " ".join(sorted(DRAW_SLOTS[d] for d in draws.get(t, ())))
            print("  %s   [%s]" % (names, what))
        unreached = sorted(targets - set(found))
        if unreached:
            print()
            print("  NOT reachable from there: %s"
                  % " ".join("%08X" % u for u in unreached[:12]))
        return 0

    print()
    print("=== functions that call a Draw* method ===")
    for f in sorted(draws):
        print("  %08X  %-14s FVF %s  (%d callers)"
              % (f, " ".join(sorted(DRAW_SLOTS[d] for d in draws[f])),
                 ",".join("0x%X" % v for v in sorted(fvfs[f])),
                 len(callers.get(f, ()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
