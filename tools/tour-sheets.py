#!/usr/bin/env python3
"""Turn a night's tour into pages a person can actually look at.

auto-tour.ps1 leaves about a thousand side-by-side captures in
logs/tour/<LEVEL>/stopNNx.png. Nobody opens a thousand files, so this makes:

  <LEVEL>.png     one page per level - every stop, left eye only, labelled
  _overview_N.png one stop per level, twelve to a page, for a first pass

The left eye alone, because a side-by-side pair at thumbnail size is two
unreadable smears; anything that is wrong in one eye and right in the other is
a stereo fault, and a contact sheet is the wrong instrument for those anyway.

    python tools/tour-sheets.py                     logs/tour -> logs/tour-sheets
    python tools/tour-sheets.py logs/tour out/
"""
import os
import sys

from PIL import Image, ImageDraw

CELL_W, CELL_H = 440, 238
PAD, LABEL = 5, 14


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, img.size[1] - LABEL, img.size[0], img.size[1]], fill=(0, 0, 0))
    d.text((3, img.size[1] - LABEL + 2), text[:64], fill=(235, 235, 90))
    return img


def left_eye(path, w=CELL_W, h=CELL_H):
    im = Image.open(path).convert('RGB')
    return im.crop((0, 0, im.size[0] // 2, im.size[1])).resize((w, h))


def grid(cells, cols):
    if not cells:
        return None
    rows = (len(cells) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (CELL_W + PAD) + PAD,
                              rows * (CELL_H + PAD) + PAD), (24, 24, 26))
    for i, c in enumerate(cells):
        x = PAD + (i % cols) * (CELL_W + PAD)
        y = PAD + (i // cols) * (CELL_H + PAD)
        sheet.paste(c, (x, y))
    return sheet


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join('logs', 'tour')
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join('logs', 'tour-sheets')
    os.makedirs(dst, exist_ok=True)
    levels = sorted(d for d in os.listdir(src)
                    if os.path.isdir(os.path.join(src, d)))
    firsts = []
    made = 0
    for lv in levels:
        p = os.path.join(src, lv)
        shots = sorted(f for f in os.listdir(p) if f.endswith('.png'))
        if not shots:
            continue
        cells = []
        for f in shots:
            try:
                cells.append(label(left_eye(os.path.join(p, f)),
                                   f.replace('.png', '')))
            except Exception as e:
                print('  skipped %s/%s: %s' % (lv, f, e))
        sheet = grid(cells, 4)
        if sheet:
            out = os.path.join(dst, lv + '.png')
            sheet.save(out)
            made += 1
        # The middle stop stands for the level on the overview - the first is
        # often the spawn corner and the last is often a wall.
        mid = shots[len(shots) // 2]
        try:
            firsts.append(label(left_eye(os.path.join(p, mid)),
                                lv.replace('WORLDS_', '')))
        except Exception:
            pass

    for i in range(0, len(firsts), 12):
        sheet = grid(firsts[i:i + 12], 4)
        if sheet:
            sheet.save(os.path.join(dst, '_overview_%d.png' % (i // 12 + 1)))

    print('%d level sheets and %d overview pages -> %s'
          % (made, (len(firsts) + 11) // 12, dst))
    return 0


if __name__ == '__main__':
    sys.exit(main())
