# jaw_wound.tga: a shot-away jaw, projected onto the front of any head by shader vr_jawwound
# (tcGen vector: s across the face, t down it; the head's own surface, so no extra mesh).
# The art is the crypt's skinned head (bphead.jpg, from RTCW's pak0): its torn mouth and teeth,
# in a ragged soft-edged patch. 128x128, the mouth about
# the middle; everything near the edges clear.
# usage: jaw_tex.py bphead.jpg out.tga
import sys, numpy as np
from PIL import Image, ImageFilter, ImageOps
rng = np.random.default_rng(11)
S = 128
src = Image.open(sys.argv[1]).convert('RGB')
both = src.crop((80, 166, 190, 240))                        # the mouth: teeth, flayed cheek, the torn skin edge
both = both.resize((104, 70), Image.LANCZOS)
img = Image.new('RGB', (S, S), (30, 3, 3))
cx, cy = 64, 74
img.paste(both, (cx - 52, cy - 35))
rgb = np.asarray(img, np.float32) * 0.9                    # a little darker than the crypt's
y, x = np.mgrid[0:S, 0:S] + 0.5

def noise(sig):
    im = Image.fromarray((rng.random((S, S)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sig))
    a = np.asarray(im, np.float32) / 255
    return (a - a.mean()) / (a.std() + 1e-6)

r = np.hypot((x - cx) / 46, (y - cy) / 30)
edge = 1 + 0.07 * noise(5)
a = np.clip((edge - r) / 0.2, 0, 1)
arr = np.dstack([np.clip(rgb, 0, 255), a * 255]).astype(np.uint8)
Image.fromarray(arr, 'RGBA').save(sys.argv[2])
