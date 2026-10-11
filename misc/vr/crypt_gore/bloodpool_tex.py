# bloodpool.tga for vr_bloodpool (vr_gore.shader): a pool of blood seen from above, multiplied
# onto the floor (white is no change), an uneven edge and a few drops round it.
# python3 bloodpool_tex.py out.tga
import math, random, struct, sys

N = 128
random.seed(7)
waves = [(k, random.uniform(0.02, 0.07) / k ** 0.5, random.uniform(0, 6.283)) for k in range(2, 9)]
drops = [(random.uniform(0, 6.283), random.uniform(0.8, 0.95), random.uniform(0.03, 0.06)) for _ in range(7)]

def coverage(x, y):
    dx, dy = x - 0.5, y - 0.5
    r = math.hypot(dx, dy) * 2
    a = math.atan2(dy, dx)
    edge = 0.78 + sum(amp * math.sin(k * a + ph) for k, amp, ph in waves)
    c = min(1.0, max(0.0, (edge - r) / 0.03))
    for da, dr, ds in drops:
        px, py = 0.5 + math.cos(da) * dr * 0.5, 0.5 + math.sin(da) * dr * 0.5
        d = math.hypot(x - px, y - py)
        c = max(c, min(1.0, max(0.0, (ds - d) / 0.012)))
    return c, r

pix = bytearray()
for j in range(N):
    for i in range(N):
        c, r = coverage((i + 0.5) / N, (j + 0.5) / N)
        # thick and dark in the middle, a little thinner (redder) toward the edge
        dark = 0.10 + 0.12 * min(1.0, r)
        rgb = (0.26 + 0.14 * min(1.0, r), dark * 0.12, dark * 0.1)     # dark: the engine overbrights it
        out = [int(255 * (1 - c + c * v)) for v in rgb]
        pix += bytes((out[2], out[1], out[0], 255))
if N:
    # keep the very border white so the clamped edge doesn't smear
    pass
hdr = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, N, N, 32, 8)
open(sys.argv[1], 'wb').write(hdr + bytes(pix))
