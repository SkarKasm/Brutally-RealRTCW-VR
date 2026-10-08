# RealRTCW-VR: models/vr_dual/left_arm.md3, the flat game's left arm for dual wielding: the
# akimbo Colts' left arm and hand (models/weapons/pistols/colt2/v_colt2_barrel3.md3) without
# its Colt, so any pistol / SMG can be put in that hand (cg_weapons.c, CG_AddDualLeft).
#   python3 left_arm.py v_colt2_barrel3.md3 left_arm.md3
import struct, sys

d = bytearray(open(sys.argv[1], 'rb').read())
h = list(struct.unpack('<4si64s9i', d[:108]))
ns, ofss, ofse = h[6], h[10], h[11]
out = d[:ofss]
o, kept = ofss, 0
for i in range(ns):
    s = struct.unpack('<4s64s10i', d[o:o + 108])
    name = s[1].split(b'\0')[0].decode()
    if name != 'wpn':
        out += d[o:o + s[11]]
        kept += 1
    o += s[11]
h[6] = kept
h[11] = len(out)
out[:108] = struct.pack('<4si64s9i', *h)
open(sys.argv[2], 'wb').write(out)
print('surfaces kept', kept, 'bytes', len(out))
