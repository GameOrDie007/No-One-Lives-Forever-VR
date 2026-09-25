"""What does d3d.ren actually ask Direct3D 7 to do?

The question this exists to answer is worth weeks: is the world drawn with a
PROJECTION MATRIX the device holds, or with vertices d3d.ren has already
transformed on the CPU?

  - matrix:  the asymmetric per-eye frustum is reachable by replacing one
             matrix, and the renderer rewrite is an optimisation rather than
             the only way to fix the bending.
  - CPU:     there is no matrix to replace, and owning the renderer is the way.

This reads the binary rather than running it. host/ddrawproxy/ddraw_proxy.cpp
records that patching DirectDraw vtables at runtime KILLED THE GAME,
reproducibly, so the runtime route is closed and re-running it is exactly what
this project has a rule against.

METHOD

COM calls compile to `call dword ptr [reg + N]`, where N is the method's byte
offset in the vtable. Disassembling .text and building a histogram of N says
which methods are used and how heavily, without executing anything.

The vtable layout below is the DirectX 7 SDK's declaration order for
IDirect3DDevice7. That is a READING, not a measurement, so the script checks
itself: in any D3D7 renderer SetRenderState and SetTextureStageState are called
from far more sites than anything else. If the histogram does not peak there,
the mapping is wrong and every label in the output is worthless - so the script
says so instead of printing confident nonsense.

  python tools/d3d7-usage.py
  python tools/d3d7-usage.py --exe game/d3d.ren
"""

import argparse
import collections
import os
import struct
import sys

import capstone

# IDirect3DDevice7, in the order d3d.h declares it. Offset = index * 4.
DEVICE7 = [
    "QueryInterface", "AddRef", "Release",
    "GetCaps", "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
    "SetRenderTarget", "GetRenderTarget", "Clear",
    "SetTransform", "GetTransform", "SetViewport", "MultiplyTransform",
    "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock",
    "PreLoad", "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus",
    "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided",
    "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState",
    "ValidateDevice", "ApplyStateBlock", "CaptureStateBlock",
    "DeleteStateBlock", "CreateStateBlock", "Load", "LightEnable",
    "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo",
]

# Flexible vertex format bits that decide the question outright.
FVF_BITS = [
    (0x002, "XYZ (untransformed - the device transforms it)"),
    (0x004, "XYZRHW (ALREADY TRANSFORMED on the CPU)"),
    (0x010, "NORMAL"),
    (0x040, "DIFFUSE"),
    (0x080, "SPECULAR"),
    (0x100, "TEX1"),
    (0x200, "TEX2"),
]


def sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
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


def describe_fvf(v):
    parts = [name for bit, name in FVF_BITS if v & bit]
    ntex = (v >> 8) & 0xF
    if not parts:
        return None
    return "  |  ".join(parts) + ("  (%d tex)" % ntex if ntex else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=None)
    ap.add_argument("--context", default=None,
                    help="vtable byte offset; print the code before every call "
                         "site at that offset, so the pushed arguments can be "
                         "read. SetTransform is 44.")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = args.exe or os.path.join(root, "game", "d3d.ren")
    with open(exe, "rb") as f:
        data = f.read()

    base, secs = sections(data)
    text = [s for s in secs if s[0] == ".text"]
    if not text:
        print("no .text section")
        return 1
    _, va, vsize, raw, rawsize = text[0]
    code = data[raw:raw + max(vsize, rawsize)]
    start = base + va

    print("%s  image base %08X  .text %08X..%08X (%d bytes)"
          % (os.path.basename(exe), base, start, start + len(code), len(code)))

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    slots = collections.Counter()
    pushes = collections.Counter()

    # A COM call compiles to the arguments pushed in reverse, then `this`, then
    # the indirect call. So the state constant for SetTransform(state, matrix)
    # is the second-to-last push before it, and it can simply be read.
    if args.context is not None:
        want_off = int(args.context, 0)
        recent = collections.deque(maxlen=8)
        nfound = 0
        print()
        print("=== code before every call at vtable offset %d ===" % want_off)
        for ins in md.disasm(code, start):
            if ins.mnemonic == "call" and ins.op_str.startswith("dword ptr ["):
                body = ins.op_str[len("dword ptr ["):-1]
                if "+" in body and "*" not in body:
                    reg, _, disp = body.partition("+")
                    disp = disp.strip()
                    if disp.startswith("0x") and reg.strip().isalpha():
                        try:
                            off = int(disp, 16)
                        except ValueError:
                            off = -1
                        if off == want_off:
                            nfound += 1
                            print()
                            print("  --- site %d at %08X ---" % (nfound, ins.address))
                            for r in recent:
                                print("      %08X  %s %s" % r)
                            print("   -> %08X  %s %s"
                                  % (ins.address, ins.mnemonic, ins.op_str))
            recent.append((ins.address, ins.mnemonic, ins.op_str))
        print()
        print("  %d call sites at offset %d" % (nfound, want_off))
        return 0

    for ins in md.disasm(code, start):
        if ins.mnemonic == "call" and ins.op_str.startswith("dword ptr ["):
            body = ins.op_str[len("dword ptr ["):-1]
            if "+" in body:
                reg, _, disp = body.partition("+")
                disp = disp.strip()
                reg = reg.strip()
                # [reg + 0xNN] only - [reg + reg*n] is not a vtable call, and
                # an absolute [0x...] is an import thunk, not a COM method.
                if disp.startswith("0x") and "*" not in body and reg.isalpha():
                    try:
                        slots[int(disp, 16)] += 1
                    except ValueError:
                        pass
        elif ins.mnemonic == "push" and ins.op_str.startswith("0x"):
            try:
                pushes[int(ins.op_str, 16)] += 1
            except ValueError:
                pass

    # ---- the self-check -------------------------------------------------
    top = slots.most_common(6)
    hot = {off for off, _ in top}
    want = {DEVICE7.index("SetRenderState") * 4,
            DEVICE7.index("SetTextureStageState") * 4}
    ok = bool(hot & want)

    print()
    print("=== indirect call sites by vtable offset (top 20) ===")
    print("  offset  slot  sites  IDirect3DDevice7 name at that index")
    for off, n in slots.most_common(20):
        idx = off // 4
        name = DEVICE7[idx] if off % 4 == 0 and idx < len(DEVICE7) else "-"
        print("  %6d  %4d  %5d  %s" % (off, idx, n, name))

    print()
    if ok:
        print("SELF-CHECK PASSED: the histogram peaks at SetRenderState and/or")
        print("SetTextureStageState, which is what a D3D7 renderer looks like.")
        print("The names above can be trusted as far as the SDK's order is.")
    else:
        print("SELF-CHECK FAILED: the busiest offsets are not the render-state")
        print("setters. These call sites are probably not IDirect3DDevice7 at")
        print("all - IGNORE the name column, it is meaningless here.")

    print()
    print("=== the question ===")
    for name in ("SetTransform", "SetViewport", "DrawPrimitive",
                 "DrawIndexedPrimitive", "DrawPrimitiveVB",
                 "DrawIndexedPrimitiveVB", "DrawPrimitiveStrided",
                 "DrawIndexedPrimitiveStrided", "SetLight", "LightEnable"):
        off = DEVICE7.index(name) * 4
        print("  %-30s offset %3d : %d call sites" % (name, off, slots.get(off, 0)))

    print()
    print("=== pushed constants that look like a vertex format ===")
    print("  (an FVF is pushed as an immediate at the DrawPrimitive call site)")
    found = False
    for v, n in sorted(pushes.items()):
        if v == 0 or v > 0xFFF:
            continue
        if not (v & 0x006):            # needs XYZ or XYZRHW to be an FVF
            continue
        d = describe_fvf(v)
        if d:
            found = True
            print("  0x%03X  pushed at %3d sites   %s" % (v, n, d))
    if not found:
        print("  none - the format may be held in a variable rather than pushed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
