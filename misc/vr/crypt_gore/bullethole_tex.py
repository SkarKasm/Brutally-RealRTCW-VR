# bullethole.tga for vr_bodywound (vr_gore.shader): a bullet hole in skin or cloth with the
# blood running down from it, Soldier of Fortune style. The renderer lays it on the body's own
# triangles (tr_animation.c R_VRX_BodyWounds): the hole at (0.5, 0.3), "down" toward the
# bottom of the image, transparent at the edges (it's clamped).
# python3 bullethole_tex.py out.tga
import struct, sys
import numpy as np

W, H = 128, 128
rng = np.random.default_rng(5)
y, x = np.mgrid[0:H, 0:W]
u = (x + 0.5) / W
v = (y + 0.5) / H
hx, hy = 0.5, 0.3

def smooth(e0, e1, t):
    t = np.clip((t - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)

ang = np.arctan2(v - hy, u - hx)
r = np.hypot(u - hx, v - hy)
ragged = 1 + 0.18 * np.sin(5 * ang + 1.3) + 0.1 * np.sin(11 * ang + 0.4)
hole = 1 - smooth(0.06, 0.085, r / ragged)            # the black hole itself
ring = 1 - smooth(0.11, 0.17, r / ragged)              # bruised, bloody flesh round it
# a few drops round the hole
spk = np.zeros_like(u)
for _ in range(14):
    a = rng.uniform(0, 2 * np.pi); d = rng.uniform(0.1, 0.22); s = rng.uniform(0.008, 0.02)
    spk = np.maximum(spk, 1 - smooth(s * 0.6, s, np.hypot(u - hx - np.cos(a) * d, v - hy - np.sin(a) * d)))
# the run of blood: a wavering streak from the hole down, thinning, a drop at the end
length = 0.45
t = np.clip((v - hy) / length, 0, 1)
centre = hx + 0.006 * np.sin(v * 17.0) + 0.003 * np.sin(v * 41.0 + 1)
width = 0.034 * (1 - 0.6 * t) + 0.006
run = (1 - smooth(width * 0.7, width, np.abs(u - centre))) * (v > hy) * (1 - smooth(0.92, 1.0, t))
drop = 1 - smooth(0.018, 0.026, np.hypot(u - (hx + 0.006 * np.sin((hy + length) * 17.0) + 0.003 * np.sin((hy + length) * 41 + 1)), v - (hy + length)))
blood = np.clip(np.maximum.reduce([ring, spk, run, drop]), 0, 1)
alpha = np.clip(np.maximum(blood * 0.92, hole), 0, 1)
# colour: near-black in the hole, dark red flesh, wetter red running down
grain = rng.normal(0, 0.03, (H, W))
red = np.clip(0.32 - 0.12 * ring + grain, 0, 1)     # dark: the engine overbrights it
red = red * (1 - hole) + 0.06 * hole
g = np.clip(0.03 + grain * 0.2, 0, 1) * (1 - hole) + 0.01 * hole
b = g.copy()
# clamp: the outermost pixels fully clear
edge = (x == 0) | (y == 0) | (x == W - 1) | (y == H - 1)
alpha[edge] = 0
pix = np.dstack([b, g, red, alpha]) * 255
out = sys.argv[1] if len(sys.argv) > 1 else 'bullethole.tga'
with open(out, 'wb') as fh:
    fh.write(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, W, H, 32, 8))
    fh.write(pix.astype(np.uint8)[::-1].tobytes())
