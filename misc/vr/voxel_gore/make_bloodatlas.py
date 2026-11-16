# make_bloodatlas.py <z_vr_assets.pk3> <zz_vr_bloodmatch.pk3> <z_vr_voxelgore.pk3>
# Run after make_voxelgore_pk3.py and make_bloodmatch_pk3.py: puts the blood atlas in
# z_vr_voxelgore.pk3 (the shader vr_gorebloodatlas, vr_voxelgore.shader), so the renderer can
# lay every kind of blood on a body in one draw (tr_animation.c R_VRX_Decals, vrxDecalCell).
#
# Two textures of the same layout, 512 by 256, drawn as two stages of the one shader; each
# picture is in one of them and the other is clear there:
#   bloodatlas.tga (mipmapped, as the rim and the soak always were):
#     soak   x   0..128  y   0..128   bloodsoak.tga (the colour matched one)
#     splat  x   0..128  y 128..256   bloodsplat.tga
#     rim    x 256..384  y   0..128   left clear (no painted hole; also written as sof2_wound1..4.tga)
#     drops  x 256..384  y 128..256   made here: one drop (each drop that lands is its own mark)
#   bloodlines.tga (no mipmaps, as the run and the trail always were: thin lines):
#     run    x 128..192  bloodrun.tga
#     trail  x 192..256  bloodtrail.tga
#     streak x 384..448, 448..512     made here: blood drawn out down the cloth, two of them
# The pictures taken over are copied texel for texel, as the engine draws them. The new ones
# (drops, streaks) are made here to look like the trails: the matched bloodtrail.tga's colour
# and grain, and its soft edge (thick in the middle, thinning out to nothing at the rim).
#
# Which way up: the engine reads a .tga's rows from the bottom of the picture up whatever its
# header says (tr_image_tga.c: "declares top-down image, ignoring"), so a file written top row
# first with the top-down flag is drawn upside down. The atlas was written that way at first:
# its two rows of pictures changed places in the game, and the marks where blood landed (drops)
# came out as the bullet hole's rim (sof2_wound1) and the other way round. So: read_tga gives
# every picture as the engine draws it, and tga32 writes the plain bottom-up file every reader
# agrees on. Here y = 0 is the top of the atlas, v = 0 in the game, the up end of a mark.
import math, os, random, shutil, struct, sys, zipfile

assets, match, gore = sys.argv[1:4]

def read_tga(d):
    idl, kind = d[0], d[2]
    w, h = struct.unpack("<HH", d[12:16])
    bpp = d[16] // 8
    assert kind == 2 and bpp == 4, "uncompressed 32 bit tga expected"
    px = d[18 + idl:18 + idl + w * h * 4]
    rows = [px[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    rows.reverse()                              # (as the engine: the file's first row is the bottom, flagged or not)
    return w, h, rows

def tga32(w, h, px):                             # px: rows top first; written bottom row first, as a .tga is
    rows = [bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h)]
    return bytes(2) + b"\x02" + bytes(9) + struct.pack("<HHBB", w, h, 32, 0x08) + b"".join(reversed(rows))

W, H = 512, 256
zz = zipfile.ZipFile(match)
sw, sh, splat = read_tga(zz.read("models/vr_gore/bloodtrail.tga"))     # (the evenest of them: grain, no shape in its colour)

def colour(x, y):                               # the matched blood, tiled (b g r)
    r = splat[y % sh]
    i = (x % sw) * 4
    return r[i], r[i + 1], r[i + 2]

def grain(x, y):                                # 0 light .. 1 dark, the texture's own
    b, g, r = colour(x, y)
    return r / 255.0

# mean brightness of the grain, so the new pictures' alpha varies round it
gm = sum(grain(x, y) for y in range(sh) for x in range(sw)) / (sw * sh)

def blank():
    px = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            b, g, r = colour(x, y)
            i = (y * W + x) * 4
            px[i], px[i + 1], px[i + 2] = b, g, r       # (clear, but blood coloured: no dark fringe when filtered)
    return px

def paste(px, rows, w, h, x0, y0):
    for y in range(h):
        for x in range(w):
            i = ((y0 + y) * W + x0 + x) * 4
            a = rows[y][x * 4 + 3]
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                a = 0                           # a clear rim round every picture: nothing of its neighbour, or of it, past its edge
            px[i:i + 3] = rows[y][x * 4:x * 4 + 3]
            px[i + 3] = a

def clamp(v):
    return 0.0 if v < 0 else (1.0 if v > 1 else v)

def draw(px, w, h, x0, y0, alpha, seed, dark=None):
    for y in range(h):
        for x in range(w):
            a = alpha(x + 0.5, y + 0.5)
            if x < 2 or y < 2 or x >= w - 2 or y >= h - 2:
                a = 0.0
            if a <= 0:
                continue
            k = dark(x + 0.5, y + 0.5) if dark else 0.0     # (0 the trail's blood .. 1 the hole itself, near black)
            a = max(a * clamp(1.0 + 1.5 * (grain(x + seed, y + 3 * seed) - gm)), k)      # (solid in the middle, as the trail is)
            b, g, r = colour(x + seed, y + 3 * seed)
            if k > 0:
                b, g, r = int(b * (1 - 0.8 * k)), int(g * (1 - 0.8 * k)), int(r * (1 - 0.75 * k))
            i = ((y0 + y) * W + x0 + x) * 4
            px[i], px[i + 1], px[i + 2], px[i + 3] = b, g, r, int(clamp(a) * 235)     # (235: the trail's own)

# the trail's edge (make_voxelgore_pk3.py: clamp(1.3 - 1.3 d / w)): solid in the middle and
# thinning all the way out, d from the middle of something w across. Drawn SOFT times wider,
# so it reads as big as it did with a hard edge. A drop is round, so the same edge would leave
# it solid over a twentieth of itself where a trail is over a quarter: its edge is steeper
# (2 - 2 d / w), solid over a quarter of the drop as the trail is of its width.
SOFT = 1.35
def soft(d, w, k=1.3):
    return clamp(k - k * d / (w * SOFT))

# a drop: roundish, never a true circle (its edge swells and pinches a little, its own way),
# a little longer down the way it ran (tall), soft edged as the trails are
def drop(px_, py_, cx, cy, r, tall=1.0):
    dx, dy = px_ - cx, (py_ - cy) / tall
    if dy > 0:
        dy /= 1.0 + 0.25 * (tall - 1.0)         # (heavier at the bottom)
    ang, ph = math.atan2(dy, dx), cx * 1.7 + cy * 0.9
    r *= 1.0 + 0.11 * math.sin(2 * ang + ph) + 0.08 * math.sin(3 * ang + 2.3 * ph) + 0.05 * math.sin(5 * ang + 0.7 * ph)
    return soft(math.hypot(dx, dy), r, 2.0)

# --- drops: a scatter of them where blood fell on cloth, none touching, a few with a short run
rnd = random.Random(11)
drops = []
def place(n, rlo, rhi, spread):
    tries = 0
    while n > 0 and tries < 4000:
        tries += 1
        r = rnd.uniform(rlo, rhi)
        cx, cy = rnd.gauss(64, spread), rnd.gauss(60, spread)
        tall = rnd.uniform(1.0, 1.5)
        if not (r + 6 < cx < 122 - r and r + 6 < cy < 110 - r * tall):
            continue
        if any(math.hypot(cx - ox, cy - oy) < r + orr + 4 for ox, oy, orr, _, _ in drops):
            continue
        run = rnd.uniform(6, 16) if r > 5 and rnd.random() < 0.45 else 0
        drops.append((cx, cy, r, tall, run))
        n -= 1
place(2, 7.5, 10.0, 18)
place(5, 4.2, 6.5, 25)
place(11, 2.2, 3.6, 30)
place(12, 1.2, 1.9, 34)
def drops_alpha(x, y):
    a = 0.0
    for cx, cy, r, tall, run in drops:
        if abs(x - cx) > r * SOFT * 1.25 + 2 or y < cy - r * tall * SOFT * 1.25 - 2 or y > cy + r * tall * 1.3 * SOFT * 1.25 + run + 6:
            continue
        a = max(a, drop(x, y, cx, cy, r, tall))
        if run and y > cy:                       # it ran a little way down, thinning, to a bead
            t = (y - cy) / (r * tall + run)
            if t < 1:
                w = r * (0.42 - 0.2 * t)
                a = max(a, soft(abs(x - cx), w))
            a = max(a, drop(x, y, cx, cy + r * tall + run, r * 0.3, 1.3))
    return a

# --- streaks: a drop that hit and was drawn out down the cloth: a head, a tail thinning away
# from it and breaking up, a bead or two beyond its end
def streak(seed, head, r, w0, w1, end, wob, beads, side):
    rr = random.Random(seed)
    ph = rr.uniform(0, 6.28)
    gaps = [(rr.uniform(0.62, 0.9) * end * 256, rr.uniform(3, 7)) for _ in range(3)]
    def alpha(x, y):
        cx = 32 + wob * math.sin(y * 0.035 + ph) + 0.6 * wob * math.sin(y * 0.11 + 2 * ph)
        a = drop(x, y, cx, head, r, 1.35)
        if head < y < end * 256:
            t = (y - head) / (end * 256 - head)
            w = w0 + (w1 - w0) * t ** 0.7
            w *= 1.0 + 0.16 * math.sin(y * 0.21 + 3 * ph) + 0.1 * math.sin(y * 0.47 + ph)    # (never ruled)
            line = soft(abs(x - cx), w) * (1.0 - 0.45 * clamp((t - 0.55) / 0.45))
            for gy, gh in gaps:                 # it breaks up towards its end
                line *= clamp(abs(y - gy) / gh - 0.3)
            a = max(a, line)
        for by, br in beads:
            a = max(a, drop(x, y, cx + 0.5, by, br, 1.4))
        if side:                                # a thinner one beside it, begun lower, ended sooner
            sx, s0, s1, sw_ = side
            cx2 = 32 + sx + 0.7 * wob * math.sin(y * 0.05 + ph + 1.3)
            a = max(a, drop(x, y, cx2, s0, sw_ * 1.7, 1.3))
            if s0 < y < s1:
                t = (y - s0) / (s1 - s0)
                a = max(a, soft(abs(x - cx2), sw_ * (1.0 - 0.6 * t)) * (1.0 - 0.4 * clamp((t - 0.5) / 0.5)))
        return a
    return alpha

# --- no painted bullet holes: the voxel gore cuts real ones, and round each the soak (set 1,
# cg_vrgore.c VRG_BloodHoles) spreads out over 15 seconds. The rim cell and the four
# vr_bodywound pictures (sof2_wound1..4.tga, over z_vr_assets.pk3's Soldier of Fortune 2 ones)
# are left clear, so nothing is drawn for them.
def clear_tga():
    px = blank()
    out = bytearray(128 * 128 * 4)
    for y in range(128):
        out[y * 512:(y + 1) * 512] = px[y * W * 4:y * W * 4 + 512]     # (blood coloured, alpha 0)
    return tga32(128, 128, out)

# --- one drop where it landed on cloth (the cgame lays one of these for each drop that hits a
# body, as big as the drop and drawn out the way it came in, cg_vrextras.c VRX_SplatAdd): round,
# its edge uneven where the cloth wicked it, a little heavier at its leading end (down the cell,
# the way the drop was going), soft edged as the trails are. In the middle of the cell, as the
# renderer centres a mark, and filling most of it (the mark's size is the stain's half width).
def droplet_alpha(x, y):
    cx, cy, r = 64.0, 60.0, 40.0
    dx, dy = x - cx, y - cy
    if dy > 0:
        dy /= 1.12                              # (heavier where it was going)
    ang = math.atan2(dy, dx)
    rr = r * (1.0 + 0.05 * math.sin(3 * ang + 0.7) + 0.04 * math.sin(5 * ang + 2.1) + 0.03 * math.sin(9 * ang + 4.0) + 0.02 * math.sin(14 * ang + 1.3))
    return soft(math.hypot(dx, dy), rr, 4.0)    # (a firmer edge than a trail: a drop sits where it fell)

def droplet_rim(x, y):                          # its edge a little darker, where the blood dried first
    d = math.hypot(x - 64.0, (y - 60.0) / (1.12 if y > 60.0 else 1.0)) / 40.0
    return 0.3 * clamp(1.0 - abs(d - 0.92) / 0.12)

atlas, lines = blank(), blank()
for name, x0, y0 in (("bloodsoak", 0, 0), ("bloodsplat", 0, 128)):
    w, h, rows = read_tga(zz.read("models/vr_gore/%s.tga" % name))
    assert (w, h) == (128, 128)
    paste(atlas, rows, w, h, x0, y0)
draw(atlas, 128, 128, 256, 128, droplet_alpha, 37, droplet_rim)      # (the drops cell: one drop, each landed drop its own mark)
for name, x0 in (("bloodrun", 128), ("bloodtrail", 192)):
    w, h, rows = read_tga(zz.read("models/vr_gore/%s.tga" % name))
    assert (w, h) == (64, 256)
    paste(lines, rows, w, h, x0, 0)
draw(lines, 64, 256, 384, 0, streak(5, 30, 11.0, 7.0, 2.2, 0.86, 2.5, [(236, 3.2)], None), 11)
draw(lines, 64, 256, 448, 0, streak(9, 26, 8.5, 5.0, 1.8, 0.70, 3.5, [(196, 2.6), (214, 2.0)], (-13, 70, 150, 3.0)), 53)

OURS = ["models/vr_gore/bloodatlas.tga", "models/vr_gore/bloodlines.tga"] + ["models/vr_gore/sof2_wound%d.tga" % (i + 1) for i in range(4)]
tmp = gore + ".tmp"
with zipfile.ZipFile(gore) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        if item.filename not in OURS:
            zout.writestr(item, zin.read(item.filename))
    zout.writestr("models/vr_gore/bloodatlas.tga", tga32(W, H, atlas))
    zout.writestr("models/vr_gore/bloodlines.tga", tga32(W, H, lines))
    for i in range(4):      # (over z_vr_assets.pk3's Soldier of Fortune 2 holes of the same name: vr_bodywound..4)
        zout.writestr("models/vr_gore/sof2_wound%d.tga" % (i + 1), clear_tga())
shutil.move(tmp, gore)
print("added the blood atlas to", gore)
