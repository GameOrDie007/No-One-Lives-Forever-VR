"""Set LARGE_ADDRESS_AWARE on our staging copy of lithtech.exe.

The upscale pack is 2.4 GB of 32-bit textures and the exe is capped at 2 GB of
user address space, which is where it died - in the NVIDIA driver, reading
null, after 482 world passes.

This is one bit in the PE characteristics field. It raises the cap to 4 GB on
64-bit Windows and changes nothing else about the binary.

WHY THIS IS ALLOWED HERE. the project rules says to prefer client-shell APIs over
binary patching, and to patch only where source genuinely does not reach. There
is no engine source, and this is a build attribute rather than game logic.
It touches ONLY our staging copy at game\\lithtech.exe - never the retail
install at <your NOLF install> - and the original is kept beside it.

THE RISK, STATED. A 2000-era binary has never run with pointers above 0x8000
0000. Any code that treats a pointer as a SIGNED int misbehaves the moment one
appears. If the game becomes unstable, restore the backup - that is the whole
undo.
"""
import os
import shutil
import struct

P = '<repo>/game/lithtech.exe'
BAK = P + '.no-laa'

if not os.path.exists(BAK):
    shutil.copy2(P, BAK)
    print('backup -> %s' % BAK)
else:
    print('backup already present: %s' % BAK)

d = bytearray(open(P, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3C)[0]
assert d[pe:pe + 4] == b'PE\0\0', 'not a PE'
off = pe + 22
chars, = struct.unpack_from('<H', d, off)
print('characteristics before: 0x%04X  (LAA %s)'
      % (chars, bool(chars & 0x0020)))

if chars & 0x0020:
    print('already large address aware - nothing to do')
else:
    struct.pack_into('<H', d, off, chars | 0x0020)
    open(P, 'wb').write(d)
    chars2, = struct.unpack_from('<H', bytes(d), off)
    print('characteristics after:  0x%04X  (LAA %s)'
          % (chars2, bool(chars2 & 0x0020)))
