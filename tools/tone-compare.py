# Whole-frame tone comparison that needs NO viewpoint alignment.
#
# The repo's own rule says hand-matched region pairs between retail shots and
# our captures produce nonsense because the viewpoints differ. These three
# numbers do not care where the camera is pointing: they describe the
# DISTRIBUTION of light in the frame, which is what "retail has contrast and
# ours is flat" actually claims.
import sys, os
from PIL import Image

def tone(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    # Drop letterbox bars: ignore rows that are essentially black edge to edge.
    px = im.load()
    lum = []
    for y in range(0, h, 2):
        row = [0.299*px[x,y][0] + 0.587*px[x,y][1] + 0.114*px[x,y][2]
               for x in range(0, w, 2)]
        if sum(row)/len(row) < 4.0:      # a letterbox bar
            continue
        lum.extend(row)
    n = len(lum)
    mean = sum(lum)/n
    std = (sum((v-mean)**2 for v in lum)/n) ** 0.5
    hi = sum(1 for v in lum if v >= 250) / n
    lo = sum(1 for v in lum if v <= 60) / n
    return mean, std, hi, lo, n

print(f"{'image':<34} {'mean':>6} {'contrast':>9} {'>=250':>7} {'<=60':>7}")
print("-" * 68)
for p in sys.argv[1:]:
    mean, std, hi, lo, n = tone(p)
    print(f"{os.path.basename(p)[:34]:<34} {mean:>6.1f} {std:>9.1f} {hi*100:>6.1f}% {lo*100:>6.1f}%")
