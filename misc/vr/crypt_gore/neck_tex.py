# neck_flat.tga: a flat cut through the neck, for the necks of cut off heads and the bodies they
# came off (neck_flat.md3, shader vr_neckflat). The front of the neck is to the right.
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFilter
rng = np.random.default_rng(3)
S = 128; c = S / 2; R = 60
y, x = np.mgrid[0:S, 0:S] + 0.5
dx, dy = (x - c) / R, (y - c) / R
r = np.hypot(dx, dy)

def blur_noise(scale, sig):
    n = rng.random((S, S)).astype(np.float32)
    im = Image.fromarray((n * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sig))
    a = np.asarray(im, np.float32) / 255
    return (a - a.mean()) / (a.std() + 1e-6) * scale

# muscle: dark red, with blotches and a little grain
n1, n2 = blur_noise(1, 6), blur_noise(1, 1.2)
meat = np.stack([110 + 18 * n1 + 8 * n2, 16 + 5 * n1 + 3 * n2, 14 + 4 * n1 + 3 * n2], -1)
# wet blood pooled in the middle, darker
pool = np.clip(1 - r / 0.75, 0, 1) ** 1.5 * np.clip(0.6 + 0.4 * blur_noise(1, 5), 0, 1)
meat = meat * (1 - 0.45 * pool[..., None]) + np.array([60, 4, 4]) * 0.45 * pool[..., None]
rgb = meat
# fat under the skin, and the skin itself
fat = np.clip(1 - np.abs(r - 0.90) / 0.04, 0, 1)
rgb = rgb * (1 - fat[..., None] * 0.7) + np.array([180, 140, 95]) * fat[..., None] * 0.7
skin = np.clip((r - 0.93) / 0.03, 0, 1)
rgb = rgb * (1 - skin[..., None]) + np.array([150, 105, 85]) * skin[..., None]
img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).convert('RGBA')
d = ImageDraw.Draw(img)
P = lambda u, v: (c + u * R, c + v * R)
def ell(u, v, ru, rv, fill):
    (a, b), (e, f) = P(u - ru, v - rv), P(u + ru, v + rv)
    d.ellipse([a, b, e, f], fill=fill)
# the spine at the back (left): bone, the cord in it
ell(-0.62, 0, 0.13, 0.15, (150, 125, 105, 255))
ell(-0.62, 0, 0.08, 0.09, (95, 45, 40, 255))
ell(-0.36, 0, 0.18, 0.21, (150, 125, 105, 255))
ell(-0.36, 0, 0.14, 0.17, (125, 85, 70, 255))
# the windpipe at the front (right): a ring of cartilage, open
ell(0.55, 0, 0.21, 0.19, (165, 130, 115, 255))
ell(0.55, 0, 0.15, 0.13, (20, 4, 4, 255))
# the gullet between them, closed flat
ell(0.25, 0, 0.06, 0.12, (70, 10, 10, 255))
# the big vessels either side, dark and open
for s in (-1, 1):
    ell(0.30, s * 0.42, 0.08, 0.07, (40, 3, 3, 255))
    ell(0.30, s * 0.42, 0.05, 0.045, (12, 1, 1, 255))
img = img.filter(ImageFilter.GaussianBlur(0.7))
a = np.clip((1.0 - r) / 0.03, 0, 1)
arr = np.asarray(img).copy(); arr[..., 3] = (a * 255).astype(np.uint8)
Image.fromarray(arr).save(sys.argv[1])
