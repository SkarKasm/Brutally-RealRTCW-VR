"""Bloody stumps from Soldier of Fortune 2 (Raven Software): the cap surfaces its characters show
where a limb or the jaw came off (models/characters/average_sleeves/average_sleeves.glm, the
"cap_*_off" surfaces, texture models/characters/caps/caps.jpg) and its bone gib
(models/characters/gore/bone_small, bone_long.png), for cg_vrextras.c's dismemberment.

  python3 sof2_stumps.py <SoF2 average_sleeves.glm> <SoF2 caps.jpg> <SoF2 bone_small.glm>
                         <SoF2 bone_long.png> <RTCW head.mdc (infantryss)> <out dir>

writes, under <out dir>/models/vr_gore:
  sof2_cap_shoulder.md3, sof2_cap_elbow.md3, sof2_cap_hip.md3, sof2_cap_knee.md3
        a limb's cap: x out of the stump (towards where the limb was), the origin in its
        middle, 2.3 from the middle to the rim, 4.6 across (shader vr_sof2cap)
  sof2_cap_neck.md3   the shoulder's again, for necks (a little rounder)
  sof2_bone.md3       the bone, from the origin out along x, 5 long (shader vr_sof2bone)
  sof2_jawcap.md3     the jaw's caps (cap_head_frnt_lwr_l/r_off) moved into RTCW's head frame
                      (x the way the face looks, z up), filling what cg_vrextras.c cuts away
                      as the jaw (VRX_JAW_*), flattened up against the cut
  sof2_caps.tga, sof2_bone.tga, sof2_meat.tga (caps.jpg's meat, mirrored so it tiles: the
  inside of a cut body, shader vr_stumpinside)
"""
import sys, os, math, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glm, md3

# the jaw cut (cg_vrextras.c VRX_JAW_*), head frame: in front of x0, below the line through
# the mouth (z top at the front, falling slope per unit back), above the neck
JAW_X0, JAW_TOP, JAW_FRONT, JAW_SLOPE, JAW_BOTTOM = 0.5, 1.1, 4.4, 0.35, -2.1
CAP_RIM, BONE_LEN = 2.3, 5.0     # (md3 keeps 1/64 of a unit: real sizes, scaled in the game)
JAW_FLATTEN = 0.6       # how much of its depth below the cut the jaw's cap keeps

def jaw_top(x):
    return JAW_TOP + JAW_SLOPE * (x - JAW_FRONT)

def in_jaw(p):
    return p[0] > JAW_X0 and p[2] < jaw_top(p[0]) and p[2] > JAW_BOTTOM

def unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)

def read_mdc0(path):
    """frame 0 of an MDC / MD3: [(name, verts)]"""
    import struct
    d = open(path, 'rb').read()
    out = []
    if d[:4] == b'IDPC':
        h = struct.unpack('<4si64s10i', d[:112]); nS, o = h[6], h[11]
        for _ in range(nS):
            s = struct.unpack('<4s64s15i', d[o:o + 128])
            nv, oXyz, oEnd = s[6], s[11], s[15]
            vs = [tuple(c / 64.0 for c in struct.unpack('<3h', d[o + oXyz + 8 * i:o + oXyz + 8 * i + 6])) for i in range(nv)]
            out.append((s[1].split(b'\0')[0].decode(), vs)); o += oEnd
    else:
        h = struct.unpack('<4si64s9i', d[:108]); nS, o = h[6], h[10]
        for _ in range(nS):
            s = struct.unpack('<4s64s10i', d[o:o + 108])
            nv, oXyz, oEnd = s[5], s[10], s[11]
            vs = [tuple(c / 64.0 for c in struct.unpack('<3h', d[o + oXyz + 8 * i:o + oXyz + 8 * i + 6])) for i in range(nv)]
            out.append((s[1].split(b'\0')[0].decode(), vs)); o += oEnd
    return out

def surf_points(S, name):
    return np.array([v['p'] for v in S[name]['verts']])

def limb_cap(S, cap, removed, flatten=1.0):
    """the cap surface in its own frame: x out towards the removed part, CAP_RIM to the rim"""
    s = S[cap]
    P = surf_points(S, cap)
    c = P.mean(0)
    w, vec = np.linalg.eigh(np.cov((P - c).T))
    x = vec[:, 0]                                   # it's thinnest along the limb
    if np.dot(x, surf_points(S, removed).mean(0) - c) < 0:
        x = -x
    y = unit(np.cross([0, 0, 1.0], x) if abs(x[2]) < 0.9 else np.cross([1.0, 0, 0], x))
    z = np.cross(x, y)
    R = np.array([x, y, z])
    Q = (P - c) @ R.T
    rim = np.percentile(np.hypot(Q[:, 1], Q[:, 2]), 90)
    Q *= CAP_RIM / rim
    Q[:, 0] *= flatten
    N = np.array([v['n'] for v in s['verts']]) @ R.T
    verts = [(Q[i], s['verts'][i]['st'], N[i]) for i in range(len(Q))]
    return [('cap', 'vr_sof2cap', verts, [tuple(t) for t in s['tris']])]

def main():
    glm_path, caps_jpg, bone_glm, bone_png, head_mdc, out = sys.argv[1:7]
    out = os.path.join(out, 'models', 'vr_gore')
    os.makedirs(out, exist_ok=True)
    S = {s['name']: s for s in glm.read(glm_path)['surfs']}

    # the limbs' caps (the right side's; each is the same mesh on both sides of the cut)
    for name, cap, removed in (('shoulder', 'cap_torso_r_arm_off', 'arm_uppr_r'),
                               ('elbow', 'cap_arm_uppr_r_lwr_off', 'arm_lwr_r'),
                               ('hip', 'cap_leg_uppr_r_hip_off', 'leg_uppr_r'),
                               ('knee', 'cap_leg_uppr_r_lwr_off', 'leg_lwr_r')):
        sf = limb_cap(S, cap, removed)
        md3.write(out + '/sof2_cap_%s.md3' % name, 'models/vr_gore/sof2_cap_%s.md3' % name, sf)
        if name == 'shoulder':
            md3.write(out + '/sof2_cap_neck.md3', 'models/vr_gore/sof2_cap_neck.md3', limb_cap(S, cap, removed, 0.8))

    # the bone: bone_small runs down -y from about its top; laid along +x from the origin, 1 long
    b = glm.read(bone_glm)['surfs'][0]
    P = np.array([v['p'] for v in b['verts']])
    top = P[:, 1].max()
    L = top - P[:, 1].min()
    R = np.array([[0, -1.0, 0], [1.0, 0, 0], [0, 0, 1.0]])     # -y to +x
    Q = (P - np.array([P[:, 0].mean(), top, P[:, 2].mean()])) @ R.T * BONE_LEN / L
    N = np.array([v['n'] for v in b['verts']]) @ R.T
    md3.write(out + '/sof2_bone.md3', 'models/vr_gore/sof2_bone.md3',
              [('bone', 'vr_sof2bone', [(Q[i], b['verts'][i]['st'], N[i]) for i in range(len(Q))], [tuple(t) for t in b['tris']])])

    # the jaw: SoF2's head front lower caps, from SoF2's frame (x the head's left, -y the way
    # the face looks) into RTCW's, fitted to what the cut takes off RTCW's head
    head = [v for n, vs in read_mdc0(head_mdc) if n in ('h_head', 'h_teethbot', 'h_mouthin') for v in vs]
    J = np.array([p for p in head if in_jaw(p)])
    jmin, jmax = J.min(0), J.max(0)
    jmin[2] = max(jmin[2], JAW_BOTTOM)
    names = ('cap_head_frnt_lwr_r_off', 'cap_head_frnt_lwr_l_off')
    toR = np.array([[0, -1.0, 0], [1.0, 0, 0], [0, 0, 1.0]])
    allP = np.concatenate([surf_points(S, n) for n in names]) @ toR.T
    smin, smax = allP.min(0), allP.max(0)
    grow = np.array([1.06, 1.08, 1.0])      # a little bigger than the hole, so no edge shows
    mid = (jmin + jmax) / 2
    surfs = []
    for n in names:
        s = S[n]
        Q = surf_points(S, n) @ toR.T
        Q = jmin + (Q - smin) / (smax - smin) * (jmax - jmin)
        Q = mid + (Q - mid) * grow
        # flattened up against the cut: the jaw's gone, only a ragged layer of meat is left
        for q in Q:
            t = jaw_top(q[0])
            q[2] = t - (t - q[2]) * JAW_FLATTEN
        N = np.array([v['n'] for v in s['verts']]) @ toR.T
        surfs.append((n, 'vr_sof2cap', [(Q[i], s['verts'][i]['st'], N[i]) for i in range(len(Q))], [tuple(t) for t in s['tris']]))
    md3.write(out + '/sof2_jawcap.md3', 'models/vr_gore/sof2_jawcap.md3', surfs)
    print('jaw cut on the head: min', jmin.round(2), 'max', jmax.round(2))

    # textures: as SoF2 has them, and the meat out of the middle of caps.jpg, mirrored to tile
    caps = Image.open(caps_jpg).convert('RGB')
    caps.save(out + '/sof2_caps.tga')
    Image.open(bone_png).convert('RGBA').save(out + '/sof2_bone.tga')
    w, h = caps.size
    m = caps.crop((w * 3 // 16, h * 3 // 16, w * 13 // 16, h * 13 // 16))
    a = np.asarray(m)
    tile = np.concatenate([np.concatenate([a, a[:, ::-1]], 1), np.concatenate([a[::-1], a[::-1, ::-1]], 1)], 0)
    Image.fromarray(tile).resize((128, 128), Image.BICUBIC).save(out + '/sof2_meat.tga')

if __name__ == '__main__':
    main()
