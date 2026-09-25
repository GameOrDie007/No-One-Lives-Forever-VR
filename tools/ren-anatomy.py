"""The anatomy of the real d3d.ren, read out of the binary.

Two questions our own renderer has to answer, both answerable without running
anything:

  1. WHERE IS EACH SLOT?  RenderDLLSetup writes all 37 function pointers into
     the RenderStruct as immediate constants, so the slot -> address map is a
     fact about the file. Every "what does the real renderer do for slot N"
     question starts here, and until now those addresses were only obtainable
     by running the game and logging them.

  2. WHAT DOES THE RENDERER ASK THE ENGINE FOR?  d3d.ren saves the RenderStruct
     pointer in one global in RenderDLLSetup's first two instructions, and
     every later use goes through it - the 15 engine callbacks at offsets
     0..56 are `call dword ptr [reg + N]`, and the data fields are `mov reg2,
     dword ptr [reg + N]`. Disassembling .text and tracking which registers
     hold that global gives the complete interface surface, statically.

     This matters because the engine pushes no geometry: RenderScene has to
     reach the world through the engine, and this is the list of every door
     it can go through.

  python tools/ren-anatomy.py
  python tools/ren-anatomy.py --exe game/d3d.ren

SELF-CHECKS (the script refuses to print confident nonsense):

  - The global must be discovered from RenderDLLSetup's own code, not assumed.
  - Slot 0's address must be a function that stores 0xD5D through its argument.
    That is the success cookie measured in docs/PHASE1-DEVICE.md, so if the
    slot map is off by one entry this check fails instead of mislabelling all
    37.
  - Every indirect call through the tracked pointer must land on a 4-byte
    boundary at or below offset 56, and no data read may fall below offset 60.
    That is the layout Phase 0 measured from the ENGINE's side - 15 callback
    pointers, then the dimensions - arrived at here from the renderer's side by
    a completely different route. A taint that had drifted onto an unrelated
    register would scatter offsets across both regions and fail this.

    (An earlier version of this check compared the histogram against "303 calls
    to callback 6 inside Init". That was wrong and is worth recording: 303 is a
    RUNTIME count and this is a count of STATIC call sites. One site inside a
    loop over the console-variable table produces 303 calls. The check failed
    while the tool was correct.)
"""

import argparse
import collections
import os
import struct
import sys

import capstone

# Measured, not read off a header. docs/PHASE0-RENDERSTRUCT.md identifies these
# by driving each operation a distinctive number of times and counting; a blank
# means the slot has never been seen called and has no measured identity.
SLOT_NAMES = [
    "Init", "Term", "", "", "RebindLightmaps", "",
    "Clear", "Start3D", "End3D", "IsIn3D",
    "StartOptimized2D", "EndOptimized2D", "IsInOptimized2D",
    "SetOptimized2DBlend", "GetOptimized2DBlend",
    "SetOptimized2DColor", "GetOptimized2DColor",
    "RenderScene", "RenderCommand", "GetDirectDrawInterface", "SwapBuffers",
    "GetInfoFlags", "GetScreenFormat",
    "CreateSurface", "DeleteSurface", "GetSurfaceInfo",
    "LockSurface", "UnlockSurface", "OptimizeSurface", "UnoptimizeSurface",
    "", "", "BlitToScreen", "", "", "ReadConsoleVariables", "",
]

# The engine half. Offsets 0..56 are the 15 callbacks; the ones with a name
# were identified in docs/PHASE1-DEVICE.md by tracing every call the real Init
# makes and reading the arguments.
ENGINE_CALLBACKS = {
    16: "console variable, \"%s %f\"",
    20: "status print",
    24: "console variable by name",
    28: "console variable value as a string",
    32: "(called once in Init, unidentified)",
    52: "memory allocation",
}

FIRST_SLOT_OFF = 108        # RenderStruct offset of slot 0
N_SLOTS = 37
CALLBACK_MAX = 56           # offsets 0..56 are the engine's function pointers


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


def exports(data, base):
    """name -> VA, from the export directory."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    size_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    ddir = opt + (96 if magic == 0x10B else 112)
    exp_rva, exp_size = struct.unpack_from("<II", data, ddir)
    if not exp_rva:
        return {}
    _, secs = base, sections(data)[1]
    off = va_to_off(base, secs, base + exp_rva)
    n_names, = struct.unpack_from("<I", data, off + 24)
    funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", data, off + 28)
    f_off = va_to_off(base, secs, base + funcs_rva)
    n_off = va_to_off(base, secs, base + names_rva)
    o_off = va_to_off(base, secs, base + ords_rva)
    out = {}
    for i in range(n_names):
        name_rva, = struct.unpack_from("<I", data, n_off + i * 4)
        s_off = va_to_off(base, secs, base + name_rva)
        end = data.index(b"\0", s_off)
        name = data[s_off:end].decode("latin-1")
        idx, = struct.unpack_from("<H", data, o_off + i * 2)
        fn_rva, = struct.unpack_from("<I", data, f_off + idx * 4)
        out[name] = base + fn_rva
    return out


def find_slots(md, data, base, secs, setup_va):
    """Decode RenderDLLSetup and collect its `mov [eax + N], imm32` writes.

    Also returns the global the RenderStruct pointer is saved in, taken from
    the same function's own code rather than assumed.
    """
    off = va_to_off(base, secs, setup_va)
    code = data[off:off + 0x400]
    slots = {}
    global_va = None
    md.detail = True
    for ins in md.disasm(code, setup_va):
        if ins.mnemonic == "ret":
            break
        if ins.mnemonic != "mov" or len(ins.operands) != 2:
            continue
        dst, src = ins.operands
        if dst.type == capstone.x86.X86_OP_MEM and src.type == capstone.x86.X86_OP_REG:
            # mov dword ptr [0xGLOBAL], eax  -- the saved RenderStruct pointer
            if dst.mem.base == 0 and dst.mem.index == 0 and global_va is None:
                global_va = dst.mem.disp & 0xFFFFFFFF
        if dst.type == capstone.x86.X86_OP_MEM and src.type == capstone.x86.X86_OP_IMM:
            if dst.mem.base != 0 and dst.mem.index == 0:
                slots[dst.mem.disp] = src.imm & 0xFFFFFFFF
    return slots, global_va


def stores_cookie(md, data, base, secs, va):
    """Does the function at va write 0xD5D through a pointer? (the Init check)"""
    off = va_to_off(base, secs, va)
    if off is None:
        return False
    md.detail = False
    for ins in md.disasm(data[off:off + 0x40], va):
        if ins.mnemonic == "mov" and "0xd5d" in ins.op_str:
            return True
    return False


def scan_usage(md, code, start, global_va):
    """Every access through the saved RenderStruct pointer, by offset.

    A register is tainted when it is loaded from the global and untainted the
    moment anything else writes it, so a stale taint cannot survive into an
    unrelated basic block and invent an offset. Calls untaint the volatile
    registers.
    """
    md.detail = True
    calls = collections.Counter()
    reads = collections.Counter()
    writes = collections.Counter()
    lea = collections.Counter()
    sites = collections.defaultdict(list)
    tainted = set()

    for ins in md.disasm(code, start):
        used = False

        # A use through a tainted register, before the taint is updated.
        for op in ins.operands:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base == 0 or op.mem.index != 0:
                continue
            if ins.reg_name(op.mem.base) not in tainted:
                continue
            n = op.mem.disp
            if n < 0:
                continue
            used = True
            if ins.mnemonic == "call":
                calls[n] += 1
            elif ins.mnemonic == "lea":
                lea[n] += 1
            elif op.access & capstone.CS_AC_WRITE:
                writes[n] += 1
            else:
                reads[n] += 1
            if len(sites[n]) < 4:
                sites[n].append(ins.address)

        # Update the taint.
        regs_r, regs_w = ins.regs_access()
        for r in regs_w:
            tainted.discard(ins.reg_name(r))
        if ins.mnemonic == "call":
            tainted -= {"eax", "ecx", "edx"}
        if ins.mnemonic == "mov" and len(ins.operands) == 2:
            dst, src = ins.operands
            if (dst.type == capstone.x86.X86_OP_REG
                    and src.type == capstone.x86.X86_OP_MEM
                    and src.mem.base == 0 and src.mem.index == 0
                    and (src.mem.disp & 0xFFFFFFFF) == global_va):
                tainted.add(ins.reg_name(dst.reg))
        _ = used
    return calls, reads, writes, lea, sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=None)
    ap.add_argument("--slot", default=None,
                    help="print just this slot's address, for feeding disasm.py")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "d3d.ren")
    with open(exe, "rb") as f:
        data = f.read()

    base, secs = sections(data)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    exp = exports(data, base)
    if "RenderDLLSetup" not in exp:
        print("no RenderDLLSetup export - this is not a LithTech renderer")
        return 1
    setup = exp["RenderDLLSetup"]

    slots, global_va = find_slots(md, data, base, secs, setup)
    if global_va is None:
        print("RenderDLLSetup does not save its argument to a global; the")
        print("usage scan below would have nothing to track. Stopping.")
        return 1

    table = {}
    for off, addr in slots.items():
        if off < FIRST_SLOT_OFF:
            continue
        idx = (off - FIRST_SLOT_OFF) // 4
        if 0 <= idx < N_SLOTS and (off - FIRST_SLOT_OFF) % 4 == 0:
            table[idx] = addr

    if args.slot is not None:
        i = int(args.slot, 0)
        if i in table:
            print("%08X" % table[i])
            return 0
        print("slot %d is not filled by RenderDLLSetup" % i)
        return 1

    print("%s  image base %08X" % (os.path.basename(exe), base))
    print("RenderDLLSetup at %08X  saves the RenderStruct in [%08X]"
          % (setup, global_va))
    print()

    # ---- self-check: slot 0 must be the function that stamps the cookie ----
    ok_cookie = 0 in table and stores_cookie(md, data, base, secs, table[0])
    print("=== the 37 renderer slots, read out of RenderDLLSetup ===")
    print("  slot  offset  address   identified as")
    for i in range(N_SLOTS):
        off = FIRST_SLOT_OFF + i * 4
        addr = table.get(i)
        print("  %4d  %6d  %s  %s"
              % (i, off, ("%08X" % addr) if addr else "   --   ",
                 SLOT_NAMES[i] if i < len(SLOT_NAMES) else ""))
    print()
    print("  %d of %d slots filled" % (len(table), N_SLOTS))
    if ok_cookie:
        print("  SELF-CHECK PASSED: slot 0 stores 0xD5D through its argument,")
        print("  which is the Init success cookie measured in PHASE1-DEVICE.")
    else:
        print("  SELF-CHECK FAILED: slot 0 does not stamp 0xD5D, so this table")
        print("  is not the renderer function table. Every name above is wrong.")

    # ---- the engine interface ---------------------------------------------
    text = [s for s in secs if s[0] == ".text"][0]
    _, va, vsize, raw, rawsize = text
    code = data[raw:raw + max(vsize, rawsize)]
    calls, reads, writes, lea, sites = scan_usage(
        md, code, base + va, global_va)

    print()
    print("=== what d3d.ren asks the ENGINE for, through the RenderStruct ===")
    print("  offsets 0..%d are the engine's callbacks; anything higher is a"
          % CALLBACK_MAX)
    print("  data field the renderer reads out of the struct.")
    print()
    print("  offset  call  read  write  lea   what")
    allofs = sorted(set(calls) | set(reads) | set(writes) | set(lea))
    for n in allofs:
        note = ENGINE_CALLBACKS.get(n, "")
        if not note:
            note = ("callback %d, unidentified" % (n // 4)) if n <= CALLBACK_MAX \
                   else "data field"
        print("  %6d  %4d  %4d  %5d  %3d   %s  [%s]"
              % (n, calls[n], reads[n], writes[n], lea[n], note,
                 " ".join("%08X" % a for a in sites[n])))

    bad_call = [n for n in calls if n > CALLBACK_MAX or n % 4]
    bad_read = [n for n in set(reads) | set(writes) if n < 60]
    print()
    if not bad_call and not bad_read:
        print("  SELF-CHECK PASSED: every indirect call lands on a 4-byte")
        print("  boundary at or below offset %d, and no data access falls below"
              % CALLBACK_MAX)
        print("  offset 60. That is Phase 0's layout - 15 engine callbacks,")
        print("  then the dimensions - reached from the renderer's side.")
        print("  %d of the 15 callbacks are used." % len(calls))
    else:
        print("  SELF-CHECK FAILED: calls at %s, data below 60 at %s."
              % (bad_call or "none", bad_read or "none"))
        print("  The taint has drifted onto an unrelated register and the")
        print("  offsets above cannot be trusted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
