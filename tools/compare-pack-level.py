"""Does OUR world parser accept the pack's rewritten levels?

HD-TEX4 ships 103 .DAT files with every UV vector multiplied by the upscale
factor. This renderer parses .DAT itself - geometry, surface flags, texture
names, lightmaps and now the light objects - so the question is not whether the
engine likes them but whether WE do, and what changed.
"""
import io
import os
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace')
sys.path.insert(0, '<repo>/tools')
import dat  # noqa: E402
import rez  # noqa: E402

P = '<repo>/packs/HD-TEX4-X4.REZ'
GAME = '<repo>/game'
WANT = 'WORLDS/M01S02.DAT'

r = rez.RezFile(P)
hit = None
for p in r.files:
    path = p if isinstance(p, str) else getattr(p, 'path', str(p))
    if path.replace(chr(92), '/').upper() == WANT:
        hit = path
        break

assert hit, 'not in the archive'
blob = r.read(hit)
out = '<repo>/logs/theirs-M01S02.DAT'
open(out, 'wb').write(blob)

orig = dat.load(GAME, 'WORLDS/M01S02.DAT')
theirs = dat.World(blob, 'theirs M01S02')

print('%-12s %-12s %-12s' % ('', 'ORIGINAL', 'PACK'))
print('%-12s %-12d %-12d' % ('bytes', len(orig.b), len(blob)))
print('%-12s %-12d %-12d' % ('version', orig.version, theirs.version))
print('%-12s %-12d %-12d' % ('models', len(orig.models), len(theirs.models)))
print('%-12s %-12d %-12d' % ('objectData', orig.object_data_pos,
                             theirs.object_data_pos))
print('%-12s %-12d %-12d' % ('renderData', orig.render_data_pos,
                             theirs.render_data_pos))
print('')
print('info string, original: %r' % orig.info[:70])
print('info string, pack    : %r' % theirs.info[:70])

# The polygon totals are the thing a UV rewrite must NOT have changed.
po = sum(m.n_polygons for m in orig.models)
pt = sum(m.n_polygons for m in theirs.models)
print('')
print('polygons: %d original, %d pack   %s'
      % (po, pt, 'SAME' if po == pt else 'DIFFERENT'))
vo = sum(m.n_points for m in orig.models)
vt = sum(m.n_points for m in theirs.models)
print('points:   %d original, %d pack   %s'
      % (vo, vt, 'SAME' if vo == vt else 'DIFFERENT'))
