"""
RealRTCW-VR asset builder.

Builds main/z_vr_assets.pk3 from the player's own RealRTCW install:
  models/vr/hand_r.md3   right hand + sleeve, taken from the Luger first person model
  models/vr/hand_l.md3   the same, mirrored

The hand is re-centred so the palm sits on the model origin, which the game puts on the
controller's grip pose.

usage: python make_vr_assets.py <RealRTCW Main dir> <output pk3>
"""

import io
import math
import os
import struct
import sys
import zipfile

SOURCE_PAK = "z_realrtcw_models_weapons.pk3"
SOURCE_MODEL = "models/weapons/pistols/luger/v_luger.md3"
KEEP_SURFACES = ("hqhand", "hqarm")


def read_surfaces(data):
    """Return (name, shader, triangles, st, verts) for the kept surfaces, frame 0 only."""
    num_surfaces = struct.unpack_from("<i", data, 84)[0]
    off = struct.unpack_from("<i", data, 100)[0]
    out = []
    for _ in range(num_surfaces):
        name = data[off + 4:off + 68].split(b"\0")[0].decode()
        _flags, _frames, num_shaders, num_verts, num_tris = struct.unpack_from("<5i", data, off + 68)
        ofs_tris, ofs_shaders, ofs_st, ofs_xyz, ofs_end = struct.unpack_from("<5i", data, off + 88)
        if name in KEEP_SURFACES:
            shader = data[off + ofs_shaders:off + ofs_shaders + 64].split(b"\0")[0].decode()
            tris = [struct.unpack_from("<3i", data, off + ofs_tris + i * 12) for i in range(num_tris)]
            st = [struct.unpack_from("<2f", data, off + ofs_st + i * 8) for i in range(num_verts)]
            verts = [struct.unpack_from("<4h", data, off + ofs_xyz + i * 8) for i in range(num_verts)]
            out.append((name, shader, tris, st, verts))
        off += ofs_end
    return out


def palm_centre(surfaces):
    hand = [s for s in surfaces if s[0] == "hqhand"][0]
    xs = [v[0] / 64.0 for v in hand[4]]
    ys = [v[1] / 64.0 for v in hand[4]]
    zs = [v[2] / 64.0 for v in hand[4]]
    return (sum(xs) / len(xs), sum(ys) / len(ys), sum(zs) / len(zs))


def mirror_normal(n):
    # md3 normals are packed as lat (high byte) / lng (low byte); mirroring y negates lng
    lat = (n >> 8) & 255
    lng = n & 255
    return (lat << 8) | ((256 - lng) & 255)


def write_md3(name, surfaces, centre, mirror):
    allpts = []
    surf_blobs = []
    for sname, shader, tris, st, verts in surfaces:
        xyz = b""
        for x, y, z, n in verts:
            px = x / 64.0 - centre[0]
            py = y / 64.0 - centre[1]
            pz = z / 64.0 - centre[2]
            if mirror:
                py = -py
                n = mirror_normal(n & 0xFFFF)
            allpts.append((px, py, pz))
            xyz += struct.pack("<3hH", int(round(px * 64)), int(round(py * 64)), int(round(pz * 64)), n & 0xFFFF)
        tri_blob = b"".join(struct.pack("<3i", a, c, b) if mirror else struct.pack("<3i", a, b, c) for a, b, c in tris)
        st_blob = b"".join(struct.pack("<2f", s, t) for s, t in st)
        shader_blob = shader.encode().ljust(64, b"\0") + struct.pack("<i", 0)

        header_size = 108
        ofs_shaders = header_size
        ofs_tris = ofs_shaders + len(shader_blob)
        ofs_st = ofs_tris + len(tri_blob)
        ofs_xyz = ofs_st + len(st_blob)
        ofs_end = ofs_xyz + len(xyz)
        header = b"IDP3" + sname.encode().ljust(64, b"\0")
        header += struct.pack("<5i", 0, 1, 1, len(verts), len(tris))
        header += struct.pack("<5i", ofs_tris, ofs_shaders, ofs_st, ofs_xyz, ofs_end)
        surf_blobs.append(header + shader_blob + tri_blob + st_blob + xyz)

    mins = [min(p[i] for p in allpts) for i in range(3)]
    maxs = [max(p[i] for p in allpts) for i in range(3)]
    radius = max(math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) for p in allpts)
    frame = struct.pack("<3f3f3ff", *mins, *maxs, 0, 0, 0, radius) + b"vr_hand".ljust(16, b"\0")

    ofs_frames = 108
    ofs_tags = ofs_frames + len(frame)
    ofs_surfaces = ofs_tags
    body = b"".join(surf_blobs)
    ofs_end = ofs_surfaces + len(body)
    header = b"IDP3" + struct.pack("<i", 15) + name.encode().ljust(64, b"\0")
    header += struct.pack("<i4i4i", 0, 1, 0, len(surf_blobs), 0, ofs_frames, ofs_tags, ofs_surfaces, ofs_end)
    return header + frame + body


def main():
    main_dir, out_pk3 = sys.argv[1], sys.argv[2]
    with zipfile.ZipFile(os.path.join(main_dir, SOURCE_PAK)) as z:
        data = z.read(SOURCE_MODEL)

    surfaces = read_surfaces(data)
    centre = palm_centre(surfaces)

    with zipfile.ZipFile(out_pk3, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("models/vr/hand_r.md3", write_md3("models/vr/hand_r.md3", surfaces, centre, False))
        out.writestr("models/vr/hand_l.md3", write_md3("models/vr/hand_l.md3", surfaces, centre, True))
    print("wrote", out_pk3, "palm centre", tuple(round(c, 2) for c in centre))


if __name__ == "__main__":
    main()
