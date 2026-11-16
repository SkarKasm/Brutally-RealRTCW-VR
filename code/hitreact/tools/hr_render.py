#!/usr/bin/env python3
"""Render hr_demo's frames.csv as an animated GIF of stick figures, one panel per man.

    hr_render.py frames.csv out.gif [timeline.txt]

Side view (X across, Z up), with a little of Y (from his own pelvis) mixed in so left and
right limbs separate.
His left side is drawn darker. The wall the demo puts in front of the row is drawn too.
"""
import sys, csv
from PIL import Image, ImageDraw, ImageFont

NAMES = ["stunned", "gut shot", "chest shot", "knockdown", "headshot", "blast", "leg shot"]
P = 21
(PELVIS, CHEST, NECK, HEAD, HEADEND, LSH, LEL, LWR, LHA, RSH, REL, RWR, RHA,
 LHIP, LKN, LAN, LTO, RHIP, RKN, RAN, RTO) = range(P)
SPINE = [(PELVIS, CHEST), (CHEST, NECK), (NECK, HEAD), (HEAD, HEADEND)]
LEFT = [(NECK, LSH), (LSH, LEL), (LEL, LWR), (LWR, LHA), (PELVIS, LHIP), (LHIP, LKN), (LKN, LAN), (LAN, LTO)]
RIGHT = [(NECK, RSH), (RSH, REL), (REL, RWR), (RWR, RHA), (PELVIS, RHIP), (RHIP, RKN), (RKN, RAN), (RAN, RTO)]

SCALE = 2.0                 # pixels per inch
PW, PH = 300, 250           # panel
X0, Z0 = 60, 225            # panel pixel of world x=0, z=0
COLS = 4

def load(path):
    rows = []
    with open(path) as f:
        for r in csv.reader(f):
            v = [float(x) for x in r[1:]]
            rows.append((int(r[0]), v))
    return rows

def events(path):
    out = []
    if not path:
        return out
    for line in open(path):
        parts = line.split(None, 1)
        try:
            t = float(parts[0])
        except (ValueError, IndexError):
            continue
        rest = parts[1].strip() if len(parts) > 1 else ""
        for i, n in enumerate(NAMES):
            if rest.startswith(n) and "body at" not in rest and "reeling" not in rest:
                out.append((int(t * 1000), i, rest[len(n):].strip()))
    return out

def main():
    rows = load(sys.argv[1])
    evs = events(sys.argv[3] if len(sys.argv) > 3 else None)
    men = len(rows[0][1]) // (P * 3)
    rowsN = (men + COLS - 1) // COLS
    W, H = PW * COLS, PH * rowsN + 24
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 13)
        small = ImageFont.truetype("DejaVuSans.ttf", 11)
    except OSError:
        font = small = ImageFont.load_default()
    frames = []
    label = [""] * men
    ei = 0
    for t, v in rows[::3]:
        while ei < len(evs) and evs[ei][0] <= t:
            label[evs[ei][1]] = evs[ei][2]
            ei += 1
        im = Image.new("RGB", (W, H), (246, 244, 239))
        d = ImageDraw.Draw(im)
        d.text((8, 4), "HitReact demo   t = %.2f s" % (t / 1000.0), fill=(40, 40, 40), font=font)
        for m in range(men):
            px0, py0 = (m % COLS) * PW, 24 + (m // COLS) * PH
            pim = Image.new("RGB", (PW, PH), (246, 244, 239))
            d = ImageDraw.Draw(pim)
            ox, oy = 0, 0       # (each panel drawn on its own: a man thrown out of it is clipped)
            pts = [v[(m * P + k) * 3:(m * P + k) * 3 + 3] for k in range(P)]
            home_y = pts[PELVIS][1]     # (side view: drifting sideways doesn't move him off the panel)
            def px(p):
                x, y, z = p
                return (ox + X0 + (x + 0.35 * (y - home_y)) * SCALE, oy + Z0 - (z - 0.2 * (y - home_y)) * SCALE)
            d.rectangle([ox + 1, oy + 1, ox + PW - 2, oy + PH - 2], outline=(220, 216, 208))
            d.line([ox + 4, oy + Z0, ox + PW - 4, oy + Z0], fill=(150, 140, 125), width=2)
            wx0, wx1 = ox + X0 + 90 * SCALE, ox + X0 + 110 * SCALE
            d.rectangle([wx0, oy + 8, min(wx1, ox + PW - 4), oy + Z0], fill=(214, 208, 196))
            d.text((ox + 8, oy + 6), NAMES[m] if m < len(NAMES) else str(m), fill=(30, 30, 30), font=font)
            d.text((ox + 8, oy + 22), label[m][:34], fill=(120, 80, 40), font=small)
            for bones, col, w in ((LEFT, (60, 70, 110), 3), (SPINE, (25, 25, 25), 4), (RIGHT, (190, 70, 50), 3)):
                for a, b in bones:
                    d.line([px(pts[a]), px(pts[b])], fill=col, width=w)
            hx, hy = px(pts[HEAD])
            ex, ey = px(pts[HEADEND])
            r = max(5, ((hx - ex) ** 2 + (hy - ey) ** 2) ** 0.5 * 0.75)
            cx, cy = (hx + ex) / 2, (hy + ey) / 2
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(25, 25, 25), width=3, fill=(246, 244, 239))
            im.paste(pim, (px0, py0))
        d = ImageDraw.Draw(im)
        frames.append(im.quantize(colors=32))
    frames[0].save(sys.argv[2], save_all=True, append_images=frames[1:], duration=48, loop=0, optimize=True)
    print("%d frames, %dx%d" % (len(frames), W, H))

main()
