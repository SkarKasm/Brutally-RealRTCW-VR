# make_bloodmatch_pk3.py <z_vr_assets.pk3> <z_vr_voxelgore.pk3> <bpblood.tga> <out.pk3>
# The blood on the bodies (bodyblood.tga, and the stain and the run cut from it in
# z_vr_voxelgore.pk3) brought down to the colour of RTCW's own body blood: bodyparts/bpblood.tga
# from pak0. Each texture keeps its own grain, shape and alpha; only its colour is scaled, channel
# by channel, so its average (where it shows) is bpblood's. Loads after the z_vr_* pk3s
# (zz_), so these win.
import io, struct, sys, zipfile

def read_tga(d):
    idl, kind = d[0], d[2]
    w, h = struct.unpack("<HH", d[12:16])
    bpp = d[16] // 8
    assert kind == 2 and bpp in (3, 4), "uncompressed tga expected"
    return w, h, bpp, d[17], bytearray(d[18 + idl:18 + idl + w * h * bpp])

def mean(w, h, bpp, px):
    t = [0.0, 0.0, 0.0]; wsum = 0.0
    for i in range(w * h):
        a = px[i * bpp + 3] / 255.0 if bpp == 4 else 1.0
        for k in range(3):
            t[k] += px[i * bpp + k] * a          # b g r
        wsum += a
    return [x / max(wsum, 1e-6) for x in t]

src, gore, ref, out = sys.argv[1:5]
rw, rh, rb, _, rp = read_tga(open(ref, "rb").read())
target = mean(rw, rh, rb, rp)
print("bpblood mean (r g b):", [round(target[2]), round(target[1]), round(target[0])])

def match(d):
    w, h, bpp, desc, px = read_tga(d)
    m = mean(w, h, bpp, px)
    scale = [min(1.0, target[k] / m[k]) if m[k] > 0.5 else 1.0 for k in range(3)]
    for i in range(w * h):
        for k in range(3):
            px[i * bpp + k] = int(px[i * bpp + k] * scale[k] + 0.5)
    return d[:18 + d[0]] + bytes(px) + d[18 + d[0] + w * h * bpp:], m

z = zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED)
for pk3, name in ((src, "models/vr_gore/bodyblood.tga"), (gore, "models/vr_gore/bloodsoak.tga"), (gore, "models/vr_gore/bloodrun.tga"), (gore, "models/vr_gore/bloodtrail.tga"), (gore, "models/vr_gore/bloodsplat.tga")):
    d, m = match(zipfile.ZipFile(pk3).read(name))
    print(name, "was (r g b):", [round(m[2]), round(m[1]), round(m[0])])
    z.writestr(name, d)
z.close()
