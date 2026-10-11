"""The guts out of an opened belly, from Soldier of Fortune 2 (Raven Software):
models/characters/gore/guts.md3 and rib_cage.png from SoF2's skins.pk3, moved out in front of
the belly in the torso's frame (README.txt) like the crypt's guts they replace.

  python3 sof2_guts.py <SoF2 guts.md3> <SoF2 rib_cage.png> <out dir>
writes guts.md3 and sof2_organs.tga (shader vr_sof2organs, vr_gore.shader)."""
import sys, struct, math
from PIL import Image
sys.path.insert(0, __import__('os').path.dirname(__file__))
import md3

SHIFT = (4.5, 1.5, -8.0)    # out of the belly, a little to the left, hanging below the navel

def read(path):
    d = open(path, 'rb').read()
    hdr = struct.unpack('<4si64s9i', d[:108])
    ns, ofsS = hdr[6], hdr[10]
    out, o = [], ofsS
    for _ in range(ns):
        _, sname, _, _, _, nv, nt, oT, oSh, oST, oXYZ, oEnd = struct.unpack('<4s64s10i', d[o:o + 108])
        tris = [struct.unpack('<3i', d[o + oT + i * 12:o + oT + i * 12 + 12]) for i in range(nt)]
        vs = []
        for j in range(nv):
            x, y, z, la, lo = struct.unpack('<3h2B', d[o + oXYZ + j * 8:o + oXYZ + j * 8 + 8])
            lat, lng = la * 2 * math.pi / 255, lo * 2 * math.pi / 255
            n = (math.cos(lng) * math.sin(lat), math.sin(lng) * math.sin(lat), math.cos(lat))
            st = struct.unpack('<2f', d[o + oST + j * 8:o + oST + j * 8 + 8])
            vs.append(((x / 64.0, y / 64.0, z / 64.0), st, n))
        out.append((sname.split(b'\0')[0].decode(), vs, tris))
        o += oEnd
    return out

src, tex, dst = sys.argv[1:4]
surfs = []
for name, vs, tris in read(src):
    vs = [(tuple(p[k] + SHIFT[k] for k in range(3)), st, n) for p, st, n in vs]
    surfs.append((name, 'vr_sof2organs', vs, tris))
md3.write(dst + '/guts.md3', 'guts', surfs)
Image.open(tex).convert('RGBA').save(dst + '/sof2_organs.tga')
