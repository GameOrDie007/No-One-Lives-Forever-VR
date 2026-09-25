# How far apart two captures are, per pixel.
#
# The mean colour of a frame is not a comparison - two completely different
# pictures can share one. This walks the pixels and reports the mean absolute
# difference plus the fraction of pixels that differ by more than a threshold,
# which is the number that says "this arm is closer to retail than that one".
#
# Rows that are entirely black in EITHER image are skipped: window-shot captures
# a live game and the desktop compositor tears, so a run can contain black bands
# that belong to the capture and not to the renderer. They would otherwise
# dominate the result.
#
#   python tools/imgdiff.py retail.png ours-mode0.png ours-mode1.png

import sys, struct, zlib


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path + ': not a PNG'
    o, w, h, bits, ctype, idat = 8, 0, 0, 0, 0, b''
    while o < len(d):
        ln, = struct.unpack_from('>I', d, o)
        typ = d[o + 4:o + 8]
        body = d[o + 8:o + 8 + ln]
        if typ == b'IHDR':
            w, h, bits, ctype = struct.unpack_from('>IIBB', body, 0)
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        o += 12 + ln
    assert bits == 8 and ctype in (2, 6), '%s: %d-bit type %d not handled' % (path, bits, ctype)
    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, nch, out


def compare(a, b, step=4, thresh=40):
    wa, ha, na, da = a
    wb, hb, nb, db = b
    w, h = min(wa, wb), min(ha, hb)
    tot = 0.0
    n = 0
    nbig = 0
    for y in range(0, h, step):
        ra, rb = y * wa * na, y * wb * nb
        # A row black in either capture is compositor tearing, not a render.
        blk_a = blk_b = True
        for x in range(0, w, 64):
            if da[ra + x * na] or da[ra + x * na + 1] or da[ra + x * na + 2]: blk_a = False
            if db[rb + x * nb] or db[rb + x * nb + 1] or db[rb + x * nb + 2]: blk_b = False
        if blk_a or blk_b:
            continue
        for x in range(0, w, step):
            ia, ib = ra + x * na, rb + x * nb
            d = (abs(da[ia] - db[ib]) + abs(da[ia + 1] - db[ib + 1])
                 + abs(da[ia + 2] - db[ib + 2])) / 3.0
            tot += d
            n += 1
            if d > thresh:
                nbig += 1
    if not n:
        return None
    return tot / n, 100.0 * nbig / n, n


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ref = read_png(sys.argv[1])
    print('reference: %s (%dx%d)' % (sys.argv[1], ref[0], ref[1]))
    for p in sys.argv[2:]:
        try:
            r = compare(ref, read_png(p))
        except Exception as e:
            print('  %-40s FAILED %r' % (p, e)); continue
        if r is None:
            print('  %-40s no comparable rows' % p); continue
        print('  %-40s mean |diff| %6.2f   %5.1f%% of pixels differ a lot   (%d sampled)'
              % (p, r[0], r[1], r[2]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
