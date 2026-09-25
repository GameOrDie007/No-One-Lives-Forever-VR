"""The level's own lights, read out of the file - correct property layout.

A property is: u16 nameLen, name, u8 type, u32 flags, u16 dataLen, data.
The u32 flags is what the first attempt missed, which ran every record
together.
"""
import collections
import io
import struct
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace')
sys.path.insert(0, '<repo>/tools')
import dat  # noqa: E402

GAME = '<repo>/game'
TYPES = {0: 'string', 1: 'vector', 2: 'colour', 3: 'real', 4: 'flags',
         5: 'bool', 6: 'long', 7: 'rotation'}


def walk(world):
    b = world.b
    p = world.object_data_pos
    count, = struct.unpack_from('<I', b, p)
    p += 4
    for _ in range(count):
        rec_len, = struct.unpack_from('<H', b, p)
        q = p + 2
        nlen, = struct.unpack_from('<H', b, q)
        q += 2
        name = b[q:q + nlen].decode('latin-1', 'replace')
        q += nlen
        nprop, = struct.unpack_from('<I', b, q)
        q += 4
        props = {}
        for _ in range(nprop):
            pl, = struct.unpack_from('<H', b, q)
            q += 2
            pn = b[q:q + pl].decode('latin-1', 'replace')
            q += pl
            ptype = b[q]
            q += 1 + 4                     # type, then the property flags
            dl, = struct.unpack_from('<H', b, q)
            q += 2
            raw = b[q:q + dl]
            q += dl
            if ptype in (1, 2) and dl == 12:
                props[pn] = tuple(round(x, 1) for x in struct.unpack('<3f', raw))
            elif ptype == 3 and dl == 4:
                props[pn] = round(struct.unpack('<f', raw)[0], 2)
            elif ptype == 6 and dl == 4:
                props[pn] = struct.unpack('<i', raw)[0]
            elif ptype == 5 and dl == 1:
                props[pn] = bool(raw[0])
            elif ptype == 0:
                props[pn] = raw.split(b'\0')[0].decode('latin-1', 'replace')
            else:
                props[pn] = '<%s %d bytes>' % (TYPES.get(ptype, ptype), dl)
        yield name, props
        p = p + 2 + rec_len


for w in ['WORLDS/M01S02.DAT', 'WORLDS/M11S01.DAT', 'WORLDS/M16S04.DAT',
          'WORLDS/M07S03.DAT', 'WORLDS/T03S01.DAT']:
    world = dat.load(GAME, w)
    kinds = collections.Counter()
    shown = set()
    samples = []
    for name, props in walk(world):
        kinds[name] += 1
        if 'light' in name.lower() and name not in shown:
            shown.add(name)
            samples.append((name, props))
    if w == 'WORLDS/M01S02.DAT':
        for name, props in samples:
            print('--- %s ---' % name)
            for k in sorted(props):
                print('    %-20s %s' % (k, props[k]))
            print('')
    lights = {k: v for k, v in kinds.items() if 'light' in k.lower()}
    print('%-14s %5d objects   lights: %s'
          % (w.replace('WORLDS/', '').replace('.DAT', ''),
             sum(kinds.values()), lights))
