"""Every level in the game on a handful of pages.

103 screenshots is not a thing anyone - a person or a model - will look at one
at a time, so nobody ever has. This crops the LEFT EYE out of each side-by-side
capture, shrinks it, labels it and lays them out in a grid, which turns "the
whole game" into about nine pages you can actually scan for missing sky, black
boxes and untextured walls.

    python tools/contact-sheet.py logs/shots logs/contact
"""
import io
import os
import sys

from PIL import Image, ImageDraw

COLS, ROWS = 4, 3
CELL_W, CELL_H = 520, 285
PAD, LABEL = 6, 16


def level_names(list_path='<repo>/tools/worlds.txt'):
    """The world list decides which shots are levels.

    logs/shots also holds a hundred one-off captures from earlier work, and no
    naming convention separates them - so the level list is the filter, taken
    from the same file the sweep itself walks.
    """
    out = []
    for line in io.open(list_path, encoding='utf-8'):
        w = line.strip()
        if not w:
            continue
        leaf = w.replace(chr(92), '/').split('/')[-1]
        out.append(leaf[:-4] if leaf.upper().endswith('.DAT') else leaf)
    return out


def sheets(src, dst, only_dat=True):
    os.makedirs(dst, exist_ok=True)
    have = {f[:-4]: f for f in os.listdir(src) if f.lower().endswith('.png')}
    names = [have[n] for n in level_names() if n in have]
    if not names:
        print('no level shots in ' + src)
        return

    per = COLS * ROWS
    made = []
    for page in range((len(names) + per - 1) // per):
        sheet = Image.new('RGB',
                          (COLS * (CELL_W + PAD) + PAD,
                           ROWS * (CELL_H + LABEL + PAD) + PAD),
                          (24, 24, 28))
        d = ImageDraw.Draw(sheet)
        for i, name in enumerate(names[page * per:(page + 1) * per]):
            try:
                im = Image.open(os.path.join(src, name)).convert('RGB')
            except Exception as e:
                print('  %s: %s' % (name, e))
                continue
            # THE LEFT EYE ONLY. A side-by-side pair shrunk to thumbnail size
            # is two pictures too small to read; one of them is legible.
            im = im.crop((0, 0, im.width // 2, im.height))
            im = im.resize((CELL_W, CELL_H), Image.LANCZOS)

            c, r = i % COLS, i // COLS
            x = PAD + c * (CELL_W + PAD)
            y = PAD + r * (CELL_H + LABEL + PAD)
            sheet.paste(im, (x, y + LABEL))
            d.text((x + 2, y + 2),
                   name.replace('.DAT.png', '').replace('.png', ''),
                   fill=(235, 235, 235))

        out = os.path.join(dst, 'sheet-%02d.png' % (page + 1))
        sheet.save(out)
        made.append(out)
        print(out)
    print('%d levels on %d sheets' % (len(names), len(made)))


if __name__ == '__main__':
    sheets(sys.argv[1] if len(sys.argv) > 1 else 'logs/shots',
           sys.argv[2] if len(sys.argv) > 2 else 'logs/contact')
