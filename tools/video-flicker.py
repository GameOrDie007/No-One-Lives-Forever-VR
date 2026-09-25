"""video-flicker.py - find a surface that FLASHES, from a screen capture.

The Morocco canopy flashes in the headset and cannot be photographed there: the
tester did not think it would come through in a single screenshot. That is
right, and the reason is the diagnosis. A z-fight is stable while the camera is
still and flips the instant it moves, so every still frame looks correct and
the fault lives only in the difference between frames.

A video has those differences in it. This pulls the frames out and asks, per
pixel, whether it ALTERNATES between two levels rather than travelling through
a range - the same question tools/flicker-scan.py asks of the renderer's own
BMP dumps, with two changes that a screen capture forces:

  TOLERANCE. flicker-scan compares bytes exactly, which is right for a
  lossless dump and useless for h264: compression moves every pixel by a
  little every frame. Here a pixel counts as "the same level" within --tol.

  A MOVING CAMERA IS EXPECTED. In a headset the view never sits perfectly
  still, so a pixel that merely changes proves nothing. What still means
  something is a pixel that keeps RETURNING to two particular levels, which
  is what the run-length test below measures.

    python tools/video-flicker.py clip.mp4
    python tools/video-flicker.py clip.mp4 --crop 0.3,0.1,0.7,0.5   # x0,y0,x1,y1
    python tools/video-flicker.py clip.mp4 --eye left               # side-by-side capture

Prints the alternating fraction and writes <clip>-flicker.png, a map of where.
A few tenths of a percent is nothing. A flashing canopy is percent-scale and
lands in one connected patch, which is what the map is for.
"""
import argparse
import os
import sys

import numpy as np

try:
    import cv2
except ImportError:
    print('needs opencv (python -m pip install opencv-python)')
    sys.exit(1)


def load_frames(path, limit, crop, eye, scale):
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        print('could not open %s' % path)
        sys.exit(1)
    out = []
    while len(out) < limit:
        ok, fr = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(fr, cv2.COLOR_BGR2GRAY)
        h, w = g.shape
        if eye in ('left', 'right'):
            half = w // 2
            g = g[:, :half] if eye == 'left' else g[:, half:]
            h, w = g.shape
        if crop:
            x0, y0, x1, y1 = crop
            g = g[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
        if scale != 1.0:
            g = cv2.resize(g, (max(1, int(g.shape[1] * scale)),
                               max(1, int(g.shape[0] * scale))))
        out.append(g.astype(np.int16))
    cap.release()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--frames', type=int, default=120)
    ap.add_argument('--tol', type=int, default=10,
                    help='how far two samples may differ and still count as the '
                         'same level; compression noise is a few units')
    ap.add_argument('--swings', type=int, default=4,
                    help='how many times a pixel must come back to a level '
                         'before it counts as alternating rather than moving')
    ap.add_argument('--crop', default=None, help='x0,y0,x1,y1 as fractions')
    ap.add_argument('--eye', default='whole', choices=['whole', 'left', 'right'])
    ap.add_argument('--scale', type=float, default=0.5)
    a = ap.parse_args()

    crop = tuple(float(v) for v in a.crop.split(',')) if a.crop else None
    frames = load_frames(a.video, a.frames, crop, a.eye, a.scale)
    if len(frames) < 8:
        print('only %d frames - need at least 8' % len(frames))
        return 1
    st = np.stack(frames)                      # t, h, w
    print('%d frames of %dx%d (scaled %.2f)' % (st.shape[0], st.shape[2], st.shape[1], a.scale))

    # A PIXEL THAT ALTERNATES returns to two levels again and again. Take the
    # per-pixel median as the divide, count how often the signal crosses it by
    # more than the tolerance, and call a pixel alternating when it crosses
    # often. A pixel that is merely moving crosses once or twice; one that is
    # flashing crosses on almost every frame.
    med = np.median(st, axis=0)
    above = st > (med + a.tol)
    below = st < (med - a.tol)
    # a crossing is a frame where the side changes from the last decided side
    side = np.zeros(st.shape, dtype=np.int8)
    side[above] = 1
    side[below] = -1
    # carry the last non-zero side forward so noise around the median does not count
    last = np.zeros(st.shape[1:], dtype=np.int8)
    crossings = np.zeros(st.shape[1:], dtype=np.int16)
    for t in range(st.shape[0]):
        s = side[t]
        changed = (s != 0) & (last != 0) & (s != last)
        crossings += changed.astype(np.int16)
        last = np.where(s != 0, s, last)

    alt = crossings >= a.swings
    span = (st.max(axis=0) - st.min(axis=0))
    alt &= (span > a.tol * 2)                  # and it actually swings, not just noise

    npx = alt.size
    changed_at_all = int((span > a.tol).sum())
    print('pixels that changed at all : %8d  (%.2f%%)' % (changed_at_all, 100.0 * changed_at_all / npx))
    print('pixels that ALTERNATE      : %8d  (%.2f%%)  [>= %d crossings]'
          % (int(alt.sum()), 100.0 * alt.sum() / npx, a.swings))

    frac = 100.0 * alt.sum() / npx
    if frac < 0.05:
        print('  no flicker worth the name.')
    else:
        ys, xs = np.nonzero(alt)
        print('  centred around x %d%%, y %d%% of the frame'
              % (100 * int(xs.mean()) // alt.shape[1], 100 * int(ys.mean()) // alt.shape[0]))

    out = os.path.splitext(a.video)[0] + '-flicker.png'
    m = np.zeros(alt.shape + (3,), dtype=np.uint8)
    m[..., 1] = np.clip(span * 2, 0, 255).astype(np.uint8)   # green: anything moving
    m[..., 2] = (alt * 255).astype(np.uint8)                 # red: alternating
    cv2.imwrite(out, m)
    print('map written to %s  (red = alternating, green = merely moving)' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
