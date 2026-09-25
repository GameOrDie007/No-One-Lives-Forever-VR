"""Which fields of a pointer argument does a function actually touch?

docs/D3D7-SEAM-CLOSED.md established that d3d.ren imports nothing from
lithtech.exe - only DDRAW, WINMM, KERNEL32 and USER32 - so every fact the
renderer has about the world arrives through exactly two pointers: the
RenderStruct, and the scene description handed to RenderScene. Whatever our own
renderer has to walk is reachable from those, and from nothing else.

That makes "what does the real renderer read out of the scene description"
a bounded, static question. This answers it.

  python tools/fn-args.py 0x1002d1c0                 # RenderScene, arg 0
  python tools/fn-args.py 0x1001eb90 --arg 0
  python tools/fn-args.py 0x1002d1c0 --exe game/d3d.ren --length 0x900

METHOD

The argument arrives at [ebp + 8 + 4*n] in a frame-pointer function. Any
register loaded from there is tainted, the taint propagates through register
moves, and any write to a register clears it - so a taint cannot survive into
an unrelated basic block and invent an offset. Every `[tainted + N]` is then
reported with the instruction that made it, because the instruction says the
TYPE: `fld` is a float, `mov` a dword, `movsx`/`movzx` a short or a byte.

It also reports where the pointer is handed on to another function, with the
argument slot, so the trace can be continued by hand into the callee.

WHAT IT DOES NOT DO

It does not follow calls, and it does not track the pointer through memory. A
field read only after the pointer has been stashed in a global is invisible
here. So an offset it reports is real; an offset it does not report is not
thereby proven unused.

WHERE THE FUNCTION ENDS

It stops at the next `push ebp; mov ebp, esp`, because running off the end of
the function is not a theoretical hazard: the first version of this ran 0x700
bytes from 1001EB90, straight through the function's last instruction at
1001EF2D and into the next one at 1001EF30 - which also takes a struct pointer
in [ebp+8], so the taint re-armed on a DIFFERENT structure and eleven plausible
offsets up to 1236 were reported as part of the scene description. They are
not. Every function in this binary is MSVC frame-pointer code, so the prologue
is a reliable boundary; the decoded instruction count is printed so a function
that ends some other way is visible as an implausibly short trace.
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


def va_to_off(base, secs, va):
    rva = va - base
    for _, sva, vsize, raw, rawsize in secs:
        if sva <= rva < sva + max(vsize, rawsize):
            return raw + (rva - sva)
    return None


FLOAT_OPS = ("fld", "fst", "fstp", "fadd", "fsub", "fmul", "fdiv",
             "fcom", "fcomp", "fsubr", "fdivr", "fild", "fistp")


def kind(ins):
    if ins.mnemonic.startswith("f"):
        return "float"
    if ins.mnemonic in ("movsx", "movzx"):
        return "short/byte"
    if ins.mnemonic == "lea":
        return "address taken"
    return "dword"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va", help="function's virtual address, e.g. 0x1002d1c0")
    ap.add_argument("--arg", type=int, default=0, help="which argument (0-based)")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--length", default="0xa00", help="bytes to decode")
    ap.add_argument("--sites", type=int, default=3,
                    help="how many instruction addresses to show per offset")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "d3d.ren")
    with open(exe, "rb") as f:
        data = f.read()

    base, secs = sections(data)
    va = int(args.va, 0)
    off = va_to_off(base, secs, va)
    if off is None:
        print("VA %08X is not inside any section" % va)
        return 1
    n = int(args.length, 0)
    code = data[off:off + n]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    slot_ebp = 8 + 4 * args.arg           # [ebp + 8] is the first argument
    slot_esp = 4 + 4 * args.arg           # before the prologue, [esp + 4]
    tainted = set()
    seen = collections.OrderedDict()      # offset -> [kind, count, [sites]]
    handoffs = []                         # (address, call target, arg slot)
    pending = {}                          # esp offset -> True, for a stacked arg
    ninsn = 0

    stream = list(md.disasm(code, va))
    end_at = None
    for i, ins in enumerate(stream):
        if i and ins.mnemonic == "push" and ins.op_str == "ebp" \
                and i + 1 < len(stream) \
                and stream[i + 1].mnemonic == "mov" \
                and stream[i + 1].op_str == "ebp, esp":
            end_at = ins.address
            break

    for ins in stream:
        if end_at is not None and ins.address >= end_at:
            break
        ninsn += 1
        ops = ins.operands

        # `lea edi, [edi]` and friends are alignment padding, not a field read.
        if ins.mnemonic == "lea" and len(ops) == 2 \
                and ops[1].type == capstone.x86.X86_OP_MEM \
                and ops[1].mem.disp == 0 and ops[1].mem.index == 0 \
                and ops[1].mem.base and ops[0].type == capstone.x86.X86_OP_REG \
                and ins.reg_name(ops[1].mem.base) == ins.reg_name(ops[0].reg):
            continue

        # --- uses through the tainted register, before the taint is updated --
        for op in ops:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base == 0 or op.mem.index != 0:
                continue
            if ins.reg_name(op.mem.base) not in tainted:
                continue
            d = op.mem.disp
            if d < 0:
                continue
            e = seen.setdefault(d, [kind(ins), 0, []])
            e[1] += 1
            if e[0] == "dword" and kind(ins) != "dword":
                e[0] = kind(ins)
            if len(e[2]) < args.sites:
                e[2].append((ins.address, "%s %s" % (ins.mnemonic, ins.op_str)))

        # --- the pointer being handed to another function --------------------
        if ins.mnemonic == "push" and ops and ops[0].type == capstone.x86.X86_OP_REG \
                and ins.reg_name(ops[0].reg) in tainted:
            pending["push"] = ins.address
        if ins.mnemonic == "mov" and len(ops) == 2 \
                and ops[0].type == capstone.x86.X86_OP_MEM \
                and ops[1].type == capstone.x86.X86_OP_REG \
                and ins.reg_name(ops[1].reg) in tainted \
                and ops[0].mem.base != 0 \
                and ins.reg_name(ops[0].mem.base) == "esp":
            pending[ops[0].mem.disp] = ins.address
        if ins.mnemonic == "call":
            if pending:
                tgt = ins.op_str if not ins.op_str.startswith("0x") else ins.op_str
                for k, at in sorted(pending.items(), key=lambda kv: str(kv[0])):
                    slot = "push" if k == "push" else "arg %d" % (k // 4)
                    handoffs.append((at, ins.address, tgt, slot))
                pending = {}

        # --- update the taint ------------------------------------------------
        _, regs_w = ins.regs_access()
        for r in regs_w:
            tainted.discard(ins.reg_name(r))
        if ins.mnemonic == "call":
            tainted -= {"eax", "ecx", "edx"}
        if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG:
            src = ops[1]
            if src.type == capstone.x86.X86_OP_MEM and src.mem.index == 0 and src.mem.base:
                b = ins.reg_name(src.mem.base)
                if (b == "ebp" and src.mem.disp == slot_ebp) or \
                   (b == "esp" and src.mem.disp == slot_esp and ninsn < 6):
                    tainted.add(ins.reg_name(ops[0].reg))
            elif src.type == capstone.x86.X86_OP_REG and ins.reg_name(src.reg) in tainted:
                tainted.add(ins.reg_name(ops[0].reg))

    print("%s  function %08X  argument %d" % (os.path.basename(exe), va, args.arg))
    if end_at is not None:
        print("  %d instructions, ending at %08X where the next prologue starts"
              % (ninsn, end_at))
    else:
        print("  %d instructions; NO following prologue found inside %d bytes, so"
              % (ninsn, n))
        print("  the trace may have run past the end of the function. Raise")
        print("  --length or check the disassembly by hand.")
    if not seen:
        print()
        print("  Nothing read through that argument in the first %d bytes." % n)
        print("  Either the argument index is wrong, or the function stashes")
        print("  the pointer in a global before using it - which this cannot")
        print("  follow. Check the disassembly by hand.")
        return 0

    print()
    print("=== fields touched through argument %d ===" % args.arg)
    print("  offset  word  uses  read as       first sites")
    for d in sorted(seen):
        k, cnt, sites = seen[d]
        first = sites[0][1] if sites else ""
        print("  %6d  %4s  %4d  %-12s  %08X  %s"
              % (d, (d // 4) if d % 4 == 0 else "-", cnt, k,
                 sites[0][0] if sites else 0, first))
        for a, txt in sites[1:]:
            print("  %38s%08X  %s" % ("", a, txt))

    if handoffs:
        print()
        print("=== the pointer handed on to another function ===")
        for at, callat, tgt, slot in handoffs:
            print("  %08X  as %-6s -> call %s  (at %08X)" % (at, slot, tgt, callat))
        print()
        print("  Continue the trace with:  python tools/fn-args.py <target> --arg N")
    return 0


if __name__ == "__main__":
    sys.exit(main())
