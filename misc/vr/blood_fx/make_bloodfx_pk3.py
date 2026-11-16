# make_bloodfx_pk3.py <z_vr_assets.pk3> <out.pk3> [bpblood.tga]
# vr_bloodFX's two textures (cgame cg_vrblood.c), cut from RTCW-VR's own bodyblood.tga: the
# colour and grain are the texture's own, taken from its darkest, most soaked patch; only the
# shapes (and a wet sheen down the middle) are made here.
#   bloodstream.tga: a thin stream of blood, the length of it tiling, soft at both sides
#   blooddrop.tga:   one drop, rounded at both ends (stretched along the way it flies)
import math, struct, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
ref = sys.argv[3] if len(sys.argv) > 3 else None
b = zipfile.ZipFile(src).read("models/vr_gore/bodyblood.tga")
bid, bkind = b[0], b[2]
bw, bh = struct.unpack("<HH", b[12:16])
bb = b[16] // 8
assert bkind == 2 and bb in (3, 4), "uncompressed tga expected"
top = (b[17] & 0x20) != 0
bp = b[18 + bid:18 + bid + bw * bh * bb]

def texel(x, y):
    x %= bw; y %= bh
    if not top:
        y = bh - 1 - y
    i = (y * bw + x) * bb
    return bp[i + 2], bp[i + 1], bp[i]          # r g b

def lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]

# the darkest 32x32 patch: soaked through, the colour of the blood itself
best = None
for py in range(0, bh, 8):
    for px in range(0, bw, 8):
        s = sum(lum(texel(px + i, py + j)) for j in range(0, 32, 4) for i in range(0, 32, 4))
        if best is None or s < best[0]:
            best = (s, px, py)
_, ox, oy = best
patch = [texel(ox + i, oy + j) for j in range(32) for i in range(32)]
mean = [sum(c[k] for c in patch) / len(patch) for k in range(3)]
bright = 1.6        # (a little brighter than the soaked cloth: wet)
if ref:
    # matched to RTCW's own body blood (pak0's bodyparts/bpblood.tga): its average colour where it shows
    rd = open(ref, "rb").read()
    rw, rh = struct.unpack("<HH", rd[12:16]); rb = rd[16] // 8
    rp = rd[18 + rd[0]:18 + rd[0] + rw * rh * rb]
    t = [0.0, 0.0, 0.0]; ws = 0.0
    for i in range(rw * rh):
        a = rp[i * rb + 3] / 255.0 if rb == 4 else 1.0
        for k in range(3):
            t[2 - k] += rp[i * rb + k] * a      # (stored b g r)
        ws += a
    target = [x / ws for x in t]
    bright = 1.0
    mean = target

pmean = [sum(c[k] for c in patch) / len(patch) for k in range(3)]
def colour(x, y, sheen):
    # the patch's own texel, pulled most of the way to its mean (a liquid, not cloth), then the sheen
    c = texel(ox + x % 32, oy + y % 32)
    out = []
    for k in range(3):
        base = mean[k] * 0.75 + c[k] * 0.25 if not ref else mean[k] * ( 0.75 + 0.25 * ( c[k] / max( pmean[k], 1 ) ) )
        v = base * bright
        v = v * (1.0 + 1.2 * sheen) + ( 40 if not ref else 14 ) * sheen
        out.append(int(max(0, min(255, v))))
    return out

def tga32(w, h, px):        # rows top first
    return bytes(2) + b"\x02" + bytes(9) + struct.pack("<HHBB", w, h, 32, 0x28) + bytes(px)

def stream():
    w, h = 16, 64
    px = bytearray()
    for y in range(h):
        for x in range(w):
            u = (x + 0.5) / w * 2 - 1                    # -1..1 across
            a = max(0.0, 1 - u * u) ** 0.6               # round, soft at the sides
            sheen = max(0.0, 1 - abs(u + 0.25) / 0.22) * 0.55   # a highlight a little off centre
            r, g, bl = colour(x, y, sheen)
            px += bytes((bl, g, r, int(255 * a)))
    return tga32(w, h, px)

def drop():
    w, h = 32, 64
    px = bytearray()
    for y in range(h):
        for x in range(w):
            u = (x + 0.5) / w * 2 - 1
            v = (y + 0.5) / h * 2 - 1
            d = math.sqrt(u * u + v * v)
            a = 0.0 if d >= 1 else min(1.0, (1 - d) * 4.0)
            if x in (0, w - 1) or y in (0, h - 1):
                a = 0.0                                  # (clampmap smears the edge texels)
            sheen = max(0.0, 1 - math.hypot(u + 0.3, v + 0.35) / 0.3) * 0.7
            r, g, bl = colour(x, y, sheen)
            px += bytes((bl, g, r, int(255 * a)))
    return tga32(w, h, px)

shader = open(__file__.replace("make_bloodfx_pk3.py", "vr_bloodfx.shader")).read()
z = zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED)
z.writestr("scripts/vr_bloodfx.shader", shader)
z.writestr("models/vr_gore/bloodstream.tga", stream())
z.writestr("models/vr_gore/blooddrop.tga", drop())
z.close()
print("patch", ox, oy, "mean", [int(m) for m in mean])
