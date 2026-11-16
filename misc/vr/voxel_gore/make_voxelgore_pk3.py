# make_voxelgore_pk3.py <z_vr_assets.pk3> <out.pk3>
# The voxel gore's shader and its detail texture: neck_meat.tga from z_vr_assets.pk3 (the
# crypt's own meat, see crypt_gore) turned grey and centred on mid grey, so drawn over the
# flesh's colours (blendFunc GL_DST_COLOR GL_SRC_COLOR, twice the product) it adds grain
# without changing them.
import math, os, struct, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
d = zipfile.ZipFile(src).read("models/vr_gore/neck_meat.tga")
idl, kind = d[0], d[2]
w, h = struct.unpack("<HH", d[12:16])
bpp = d[16] // 8
assert kind == 2 and bpp in (3, 4), "uncompressed tga expected"
px = d[18 + idl:18 + idl + w * h * bpp]
lum = [0.114 * px[i * bpp] + 0.587 * px[i * bpp + 1] + 0.299 * px[i * bpp + 2] for i in range(w * h)]
mean = sum(lum) / len(lum)
lo, hi = min(lum), max(lum)
span = max(hi - lo, 1.0)
grey = bytearray()
for v in lum:
    g = 128 + (v - mean) * (90.0 / span)        # +-45 round mid grey
    g = int(max(0, min(255, g)))
    grey += bytes((g, g, g))
tga = bytes(18) [:2] + b"\x02" + bytes(9) + struct.pack("<HHBB", w, h, 24, d[17] & 0x20) + bytes(grey)
# bloodsoak.tga and bloodrun.tga: RTCW-VR's bodyblood.tga (blood soaked into cloth) cut into a
# stain with a ragged round edge, and into blood running down in a few streams, each ending in
# a drop. The colour and grain are the texture's own; only the shapes are made here.
b = zipfile.ZipFile(src).read("models/vr_gore/bodyblood.tga")
bid, bkind = b[0], b[2]
bw, bh = struct.unpack("<HH", b[12:16])
bb = b[16] // 8
assert bkind == 2 and bb in (3, 4), "uncompressed tga expected"
bp = b[18 + bid:18 + bid + bw * bh * bb]
def texel(x, y):
    i = ((y % bh) * bw + (x % bw)) * bb
    return bp[i], bp[i + 1], bp[i + 2]          # b g r
def dark(x, y):
    bl, g, r = texel(x, y)
    return 1.0 - (0.114 * bl + 0.587 * g + 0.299 * r) / 255.0
def tga32(w, h, px):                             # rows top first
    return bytes(2) + b"" + bytes(9) + struct.pack("<HHBB", w, h, 32, 0x28) + bytes(px)
def clear_border(px, w, h):                     # clampmap smears the edge texels: keep them empty
    for y in range(h):
        for x in range(w):
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                px[(y * w + x) * 4 + 3] = 0
    return px
def clamp(v):
    return 0.0 if v < 0 else (1.0 if v > 1 else v)

# the blotch's outline: the texture's grain blurred and read slowly round a circle, so the edge
# swells and pinches in a few big lobes like blood wicking through cloth
def blurred(x, y, k=6):
    t = 0.0
    for j in range(-k, k + 1, 2):
        for i in range(-k, k + 1, 2):
            t += dark(int(x) + i, int(y) + j)
    return t / ((k + 1) ** 2)
ring = [blurred(128 + 28 * math.cos(i * 2 * math.pi / 64), 128 + 28 * math.sin(i * 2 * math.pi / 64)) for i in range(64)]
lo, hi = min(ring), max(ring)
ring = [(v - lo) / max(hi - lo, 1e-3) for v in ring]
px = bytearray()
for y in range(128):
    for x in range(128):
        dx, dy = (x - 63.5) / 64, (y - 63.5) / 64
        r = (dx * dx + dy * dy) ** 0.5
        f = (math.atan2(dy, dx) / (2 * math.pi) % 1.0) * 64
        i0 = int(f) % 64
        t = f - int(f)
        edge = 0.42 + 0.52 * ( ring[i0] * (1 - t) + ring[(i0 + 1) % 64] * t )
        a = clamp((edge - r) * 9.0 + (dark(x + 40, y + 40) - 0.5) * 1.2)
        # wet: brighter red where it's thin, near black where it's soaked through
        core = clamp(1.0 - r / max(edge, 0.05))
        bl, g, rr = texel(x + 40, y + 40)
        red = 0.55 + 0.45 * (1.0 - core)
        px += bytes((int(bl * 0.35), int(g * 0.3), min(255, int(rr * (0.75 + 0.6 * red))), int(a * 245)))
bloodsoak = tga32(128, 128, clear_border(px, 128, 128))

streams = [(0.50, 0.19, 0.90), (0.24, 0.045, 0.42)]     # a thin trickle (Max Payne 2 VR), a fainter one beside it
px = bytearray()
for y in range(256):
    v = y / 255.0
    for x in range(64):
        u = x / 63.0
        a = 0.0
        for cx, wd, ln in streams:
            if v > ln:
                continue
            w = wd * (1.0 - 0.45 * v / ln)                       # thinner as it runs
            if v > ln - 0.04:                                    # a drop at the end
                w = wd * 0.9
            wob = 0.015 * math.sin(v * 23.0 + cx * 40.0)
            d = abs(u - cx - wob) / max(w, 1e-3)
            a = max(a, clamp(1.3 - d * 1.3))
        a *= clamp(0.65 + 0.5 * dark(x * 2, y))
        a *= clamp((v - 0.0625) / 0.0625)       # clear at the top: drawn small, a texture's edge smears on down the skin
        bl, g, rr = texel(x * 2, y)
        px += bytes((int(bl * 0.75), int(g * 0.75), int(rr * 0.8), int(clamp(a) * 235)))
bloodrun = tga32(64, 256, clear_border(px, 64, 256))

# bloodtrail.tga: the same trickle with no beginning or end, for the stretches of a run behind
# its tip (cg_vrgore.c VRG_Flow): they join end to end
px = bytearray()
for y in range(256):
    v = y / 255.0
    for x in range(64):
        u = x / 63.0
        wob = 0.015 * math.sin(v * 2 * math.pi * 3.0)
        w = 0.19 * (0.8 + 0.2 * math.sin(v * 2 * math.pi * 2.0 + 1.0))
        d = abs(u - 0.50 - wob) / w
        a = clamp(1.3 - d * 1.3) * clamp(0.65 + 0.5 * dark(x * 2, y + 256))
        a *= clamp((min(v, 1.0 - v) - 0.0625) / 0.0625)     # clear at both ends (as bloodrun.tga's top)
        bl, g, rr = texel(x * 2, y + 256)
        px += bytes((int(bl * 0.75), int(g * 0.75), int(rr * 0.8), int(clamp(a) * 235)))
bloodtrail = tga32(64, 256, clear_border(px, 64, 256))     # (each stretch is drawn a little into the next)

# bloodsplat.tga: where a flying drop hit someone (cg_vrextras.c VRX_SplatAdd): a blot with
# fingers thrown out the way it was going (down the texture) and a few specks beyond them
import random
rnd = random.Random(7)
# (short and thick: long thin ones read as ruled lines once a few splats overlap)
fingers = [(math.pi / 2 + rnd.uniform(-0.8, 0.8), rnd.uniform(0.22, 0.34), rnd.uniform(0.07, 0.11)) for _ in range(3)]
fingers += [(rnd.uniform(0, 2 * math.pi), rnd.uniform(0.18, 0.24), rnd.uniform(0.06, 0.09)) for _ in range(3)]
specks = [(rnd.uniform(-0.35, 0.35), rnd.uniform(0.25, 0.55), rnd.uniform(0.025, 0.06)) for _ in range(10)]
specks += [(rnd.uniform(-0.5, 0.5), rnd.uniform(-0.45, 0.1), rnd.uniform(0.015, 0.03)) for _ in range(4)]
px = bytearray()
for y in range(128):
    for x in range(128):
        dx, dy = (x - 63.5) / 64 / 1.7, ((y - 63.5) / 64 + 0.38) / 1.7     # the blot above the middle
        r = (dx * dx + dy * dy) ** 0.5
        f = (math.atan2(dy, dx) / (2 * math.pi) % 1.0) * 64
        i0 = int(f) % 64
        t = f - int(f)
        edge = 0.15 + 0.10 * (ring[i0] * (1 - t) + ring[(i0 + 1) % 64] * t)
        a = clamp((edge - r) * 14.0)
        for ang, ln, wd in fingers:                           # each a tapering streak out of the blot
            ux, uy = math.cos(ang), math.sin(ang)
            al = dx * ux + dy * uy
            if 0 < al < ln:
                off = abs(dx * uy - dy * ux)
                w = wd * (1.0 - 0.75 * al / ln)
                a = max(a, clamp((w - off) * 30.0))
            ex, ey = dx - ux * ln, dy - uy * ln               # a drop at its end
            a = max(a, clamp((wd * 0.7 - (ex * ex + ey * ey) ** 0.5) * 30.0))
        for sx, sy, sr in specks:
            a = max(a, clamp((sr - ((dx - sx) ** 2 + (dy - sy) ** 2) ** 0.5) * 30.0))
        a *= clamp(0.7 + 0.5 * dark(x + 90, y + 10))
        bl, g, rr = texel(x + 90, y + 10)
        px += bytes((int(bl * 0.6), int(g * 0.6), min(255, int(rr * 0.95)), int(clamp(a) * 240)))
bloodsplat = tga32(128, 128, clear_border(px, 128, 128))

here = os.path.dirname(os.path.abspath(__file__))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(here, "vr_voxelgore.shader"), "scripts/vr_voxelgore.shader")
    z.writestr("models/vr_gore/flesh_detail.tga", tga)
    z.writestr("models/vr_gore/bloodsoak.tga", bloodsoak)
    z.writestr("models/vr_gore/bloodrun.tga", bloodrun)
    z.writestr("models/vr_gore/bloodtrail.tga", bloodtrail)
    z.writestr("models/vr_gore/bloodsplat.tga", bloodsplat)
print("wrote", out)
