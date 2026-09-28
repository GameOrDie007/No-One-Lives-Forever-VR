#!/usr/bin/env python3
"""sheet.py DIR [OUT] [--cols N] - a contact sheet of a folder's frame-*.bmp dumps,
right eye of each, labelled with its frame number. Desk-review aid."""
import os, re, sys
from PIL import Image, ImageDraw, ImageFont

args = [a for a in sys.argv[1:] if not a.startswith('--')]
cols = 3
if '--cols' in sys.argv:
    cols = int(sys.argv[sys.argv.index('--cols') + 1])
d = args[0]
out = args[1] if len(args) > 1 else os.path.join(d, 'sheet.png')
fs = sorted((f for f in os.listdir(d) if re.match(r'frame-\d+\.bmp$', f)), key=lambda f: int(re.findall(r'\d+', f)[0]))
if not fs:
    raise SystemExit('no frames in ' + d)
ims = []
for f in fs:
    im = Image.open(os.path.join(d, f)).convert('RGB')
    w, h = im.size
    ims.append((f, im.crop((w // 2, 0, w, h))))
tw, th = ims[0][1].size[0] // 2, ims[0][1].size[1] // 2
rows = (len(ims) + cols - 1) // cols
s = Image.new('RGB', (tw * cols, th * rows), (0, 0, 0))
dr = ImageDraw.Draw(s)
try:
    font = ImageFont.truetype('arial.ttf', 20)
except Exception:
    font = None
for i, (f, im) in enumerate(ims):
    x, y = (i % cols) * tw, (i // cols) * th
    s.paste(im.resize((tw, th)), (x, y))
    dr.text((x + 6, y + 4), f, fill=(255, 255, 0), font=font)
s.save(out)
print(out, s.size, len(ims), 'frames')
