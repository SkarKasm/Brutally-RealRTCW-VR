# jaw_wound.tga: a shot-away jaw, projected onto the front of any head by shader vr_jawwound
# (tcGen vector: s across the face, t down it; the head's own surface, so no extra mesh).
# 128x128, the wound about the middle; everything near the edges clear.
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFilter
rng = np.random.default_rng(11)
S = 128
y, x = np.mgrid[0:S, 0:S] + 0.5

def noise(sig, scale=1):
    im = Image.fromarray((rng.random((S, S)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sig))
    a = np.asarray(im, np.float32) / 255
    return (a - a.mean()) / (a.std() + 1e-6) * scale

cx, cy = 64, 74
# a ragged blob for the torn area
ang = np.arctan2(y - cy, x - cx)
r = np.hypot((x - cx) / 34, (y - cy) / 24)
edge = 1 + 0.07 * np.sin(5 * ang + 1) + 0.05 * np.sin(13 * ang + 2) + 0.06 * noise(1.5)
wound = np.clip((edge - r) / 0.12, 0, 1)
# blood smeared around it and running down
smear = np.clip((1.45 + 0.2 * noise(4) - np.hypot((x - cx) / 36, (y - cy) / 30)) / 0.4, 0, 1) * 0.75
for x0, l, w in ((50, 40, 4), (66, 50, 5), (80, 34, 3), (58, 28, 3)):
    m = (np.abs(x - x0 - 1.5 * np.sin(y / 9)) < w * np.clip(1 - (y - cy) / (24 + l), 0.3, 1)) & (y > cy) & (y < cy + 24 + l)
    smear = np.maximum(smear, m * 0.85)
smear = np.asarray(Image.fromarray((smear * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255
n1 = noise(3)
meat = np.stack([105 + 20 * n1, 14 + 6 * n1, 12 + 5 * n1], -1)
blood = np.stack([70 + 8 * n1, 6 + 2 * n1, 5 + 2 * n1], -1)
rgb = blood * (1 - wound[..., None]) + meat * wound[..., None]
# the open mouth: dark, with the upper teeth along its top
img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).convert('RGBA')
d = ImageDraw.Draw(img)
d.ellipse([cx - 20, cy - 10, cx + 20, cy + 12], fill=(22, 3, 3, 255))
for i in range(-5, 6):
    if rng.random() < 0.2:
        continue    # a tooth gone
    tx = cx + i * 3.4
    h = 6 + rng.random() * 2 - abs(i) * 0.4
    d.rounded_rectangle([tx - 1.4, cy - 9, tx + 1.4, cy - 9 + h], 0.8, fill=(176, 160, 128, 255))
# a broken bit of the lower jaw bone
d.polygon([(cx - 14, cy + 15), (cx - 4, cy + 12), (cx + 2, cy + 17), (cx - 10, cy + 19)], fill=(185, 165, 135, 255))
img = img.filter(ImageFilter.GaussianBlur(0.6))
a = np.maximum(wound, smear)
# nothing near the edges (the projection clamps to them)
fade = np.clip(np.minimum.reduce([x - 4, S - 4 - x, y - 4, S - 4 - y]) / 10, 0, 1)
arr = np.asarray(img).copy()
arr[..., 3] = (np.clip(a * fade, 0, 1) * 255).astype(np.uint8)
Image.fromarray(arr).save(sys.argv[1])
