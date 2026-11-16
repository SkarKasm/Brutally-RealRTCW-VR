"""RealRTCW VR hand editor: reading the game's weapon models, the finger rig, and baking edits.

Standard library only. Never launches the game or a VR runtime.

Spaces: everything is worked in the first person "hands model" space the game poses each
weapon in (x forward, y left, z up, game units), at the weapon's idle frame, as the VR mod
holds it (cg_weapons.c, CG_VR_WeaponPose). Each hand is the hqhand surface of the piece the
VR mod draws as that hand (cg_vr.c vrHandPoses: -1 the gun model, 0.. a part, -3 the VR rifle
hand models/vr/rhand_<gun>.md3).

Fingers: RealRTCW's first person hands have no bones, but every hand uses the same glove
texture layout (glove_default), so each vertex's texture coordinate says which finger it is
on and how far along it. From that a small rig is fitted per hand (3 joints per finger) and
the vertices are bent smoothly about them.
"""
import json, math, os, re, struct, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = json.load(open(os.path.join(HERE, 'rtcw_vr_tables.json')))
DIGITS = ['thumb', 'index', 'middle', 'ring', 'little']
OUT_PK3 = 'zz_vr_handedits.pk3'
MDC_TAG_ANGLE_SCALE = 360.0 / 32700.0

# ------------------------------------------------------------------ math (rows = axes)

def angles_to_axis(pitch, yaw, roll):
    p, y, r = (math.radians(a) for a in (pitch, yaw, roll))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    fwd = [cp * cy, cp * sy, -sp]
    right = [-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp]
    up = [cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp]
    return [fwd, [-v for v in right], up]

def to_parent(origin, axis, p):
    return [origin[k] + p[0] * axis[0][k] + p[1] * axis[1][k] + p[2] * axis[2][k] for k in range(3)]

def compose(po, pa, to, ta):
    return to_parent(po, pa, to), [[sum(ta[i][j] * pa[j][k] for j in range(3)) for k in range(3)] for i in range(3)]

def sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def scale(a, s): return [a[0] * s, a[1] * s, a[2] * s]
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def length(a): return math.sqrt(dot(a, a))
def norm(a):
    l = length(a)
    return [a[0] / l, a[1] / l, a[2] / l] if l > 1e-9 else [0.0, 0.0, 0.0]
def mean(pts):
    n = len(pts)
    return [sum(p[k] for p in pts) / n for k in range(3)] if n else None

def mat_mul(a, b):  # column-vector matrices
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def mat_vec(m, v):
    return [m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]]
def mat_t(m): return [[m[j][i] for j in range(3)] for i in range(3)]
IDENT = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]

def axis_angle(axis, deg):
    x, y, z = norm(axis)
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    t = 1 - c
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]

def euler_xyz(deg):
    """Rx(a) Ry(b) Rz(c), as three.js Euler 'XYZ' (the editor's gizmo)."""
    a, b, c = (math.radians(v) for v in deg)
    rx = [[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]]
    ry = [[math.cos(b), 0, math.sin(b)], [0, 1, 0], [-math.sin(b), 0, math.cos(b)]]
    rz = [[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]]
    return mat_mul(mat_mul(rx, ry), rz)

# affine: (m, t) with p' = m p + t
def aff_apply(f, p): return add(mat_vec(f[0], p), f[1])
def aff_mul(f, g):  # f after g
    return mat_mul(f[0], g[0]), add(mat_vec(f[0], g[1]), f[1])
def aff_about(m, pivot):
    return m, sub(pivot, mat_vec(m, pivot))
AFF_ID = (IDENT, [0.0, 0.0, 0.0])
def aff_lerp_id(f, w):
    """Blend from identity: the rotation by angle, translation by amount."""
    if w >= 0.999:
        return f
    if w <= 0.001:
        return AFF_ID
    return [[IDENT[i][j] + (f[0][i][j] - IDENT[i][j]) * w for j in range(3)] for i in range(3)], scale(f[1], w)

def smoothstep(x):
    x = 0.0 if x < 0 else (1.0 if x > 1 else x)
    return x * x * (3 - 2 * x)

# ------------------------------------------------------------------ normals

def decode_normal(n):
    lat = ((n >> 8) & 255) * 2 * math.pi / 255
    lng = (n & 255) * 2 * math.pi / 255
    return [math.cos(lat) * math.sin(lng), math.sin(lat) * math.sin(lng), math.cos(lng)]

def encode_normal(v):
    x, y, z = norm(v)
    if abs(x) < 1e-9 and abs(y) < 1e-9:
        return 0 if z > 0 else 128
    lat = int(round(math.atan2(y, x) * 255 / (2 * math.pi))) & 255
    lng = int(round(math.acos(max(-1.0, min(1.0, z))) * 255 / (2 * math.pi))) & 255
    return (lat << 8) | lng

# ------------------------------------------------------------------ paks

class Paks:
    """The game's files the way the mod finds them: RealRTCW's Main, then the mod's game/main
    on top (later wins), leaving out the editor's own output."""

    def __init__(self, dirs):
        self.index = {}
        self.zips = []
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for name in sorted(os.listdir(d), key=str.lower):
                if not name.lower().endswith('.pk3') or name.lower() == OUT_PK3:
                    continue
                try:
                    z = zipfile.ZipFile(os.path.join(d, name))
                except (zipfile.BadZipFile, OSError):
                    continue
                self.zips.append(z)
                for n in z.namelist():
                    self.index[n.lower()] = (z, n)

    def find(self, path):
        path = path.lower().replace('\\', '/')
        if path in self.index:
            return path
        base, _ = os.path.splitext(path)
        for alt in ('.md3', '.mdc'):
            if base + alt in self.index:
                return base + alt
        return None

    def read(self, path):
        key = self.find(path) if path else None
        if not key:
            return None
        z, n = self.index[key]
        return z.read(n)

    def names(self):
        return list(self.index)

# ------------------------------------------------------------------ models

class Surface:
    def __init__(self, name, tris, st, verts, nverts, offsets):
        self.name, self.tris, self.st, self.verts, self.nverts, self.offsets = name, tris, st, verts, nverts, offsets

class Model:
    """md3 / mdc at one frame: surfaces (positions, normals), tags."""

    def __init__(self, data, frame=0):
        self.data = data
        self.surfaces, self.tags = [], {}
        if data[:4] == b'IDP3':
            self._md3(data, frame)
        elif data[:4] == b'IDPC':
            self._mdc(data, frame)
        else:
            raise ValueError('unknown model format')

    def _md3(self, d, frame):
        self.num_frames, num_tags, num_surfs = struct.unpack_from('<3i', d, 76)
        self.ofs_frames, ofs_tags, ofs_surfs = struct.unpack_from('<3i', d, 92)
        frame = min(frame, self.num_frames - 1)
        for t in range(num_tags):
            o = ofs_tags + (frame * num_tags + t) * 112
            name = d[o:o + 64].split(b'\0')[0].decode(errors='replace').lower()
            v = struct.unpack_from('<12f', d, o + 64)
            self.tags[name] = (list(v[0:3]), [list(v[3:6]), list(v[6:9]), list(v[9:12])])
        off = ofs_surfs
        for _ in range(num_surfs):
            name = d[off + 4:off + 68].split(b'\0')[0].decode(errors='replace')
            _f, sframes, _ns, nv, nt = struct.unpack_from('<5i', d, off + 68)
            o_tri, o_sh, o_st, o_xyz, o_end = struct.unpack_from('<5i', d, off + 88)
            sf = min(frame, sframes - 1)
            tris = [struct.unpack_from('<3i', d, off + o_tri + i * 12) for i in range(nt)]
            st = [struct.unpack_from('<2f', d, off + o_st + i * 8) for i in range(nv)]
            verts = []
            for i in range(nv):
                x, y, z, n = struct.unpack_from('<3hH', d, off + o_xyz + (sf * nv + i) * 8)
                verts.append(([x / 64.0, y / 64.0, z / 64.0], decode_normal(n)))
            self.surfaces.append(Surface(name, tris, st, verts, nv, dict(off=off, xyz=o_xyz, frames=sframes)))
            off += o_end

    def _mdc(self, d, frame):
        self.num_frames, num_tags, num_surfs = struct.unpack_from('<3i', d, 76)
        ofs_tagnames, ofs_tags, ofs_surfs = struct.unpack_from('<3i', d, 96)
        frame = min(frame, self.num_frames - 1)
        for t in range(num_tags):
            name = d[ofs_tagnames + t * 64:ofs_tagnames + t * 64 + 64].split(b'\0')[0].decode(errors='replace').lower()
            x, y, z, a0, a1, a2 = struct.unpack_from('<6h', d, ofs_tags + (frame * num_tags + t) * 12)
            axis = angles_to_axis(a0 * MDC_TAG_ANGLE_SCALE, a1 * MDC_TAG_ANGLE_SCALE, a2 * MDC_TAG_ANGLE_SCALE)
            self.tags[name] = ([x / 64.0, y / 64.0, z / 64.0], axis)
        off = ofs_surfs
        for _ in range(num_surfs):
            name = d[off + 4:off + 68].split(b'\0')[0].decode(errors='replace')
            v = struct.unpack_from('<14i', d, off + 68)
            nbase, nv, nt = v[2], v[4], v[5]
            o_tri, o_st, o_xyz, o_fbase, o_end = v[6], v[8], v[9], v[11], v[13]
            base = struct.unpack_from('<h', d, off + o_fbase + frame * 2)[0] if nbase else 0
            tris = [struct.unpack_from('<3i', d, off + o_tri + i * 12) for i in range(nt)]
            st = [struct.unpack_from('<2f', d, off + o_st + i * 8) for i in range(nv)]
            verts = []
            for i in range(nv):
                x, y, z, n = struct.unpack_from('<3hH', d, off + o_xyz + (base * nv + i) * 8)
                verts.append(([x / 64.0, y / 64.0, z / 64.0], decode_normal(n)))
            self.surfaces.append(Surface(name, tris, st, verts, nv, None))
            off += o_end

def is_hand(name): return any(w in name.lower() for w in ('hand', 'glove'))
def is_arm(name): return any(w in name.lower() for w in ('arm', 'sleeve')) and not is_hand(name)

# ------------------------------------------------------------------ weapons

def parse_weap(text):
    text = re.sub(r'//[^\n]*', '', text)
    info = {'parts': []}
    m = re.search(r'handsModel\s+"([^"]+)"', text)
    info['hands'] = m.group(1) if m else None
    m = re.search(r'weaponConfig\s+"([^"]+)"', text)
    info['config'] = m.group(1) if m else None
    fp = re.search(r'firstPerson\s*\{', text)
    info['model'] = None
    if fp:
        body = text[fp.end():]
        m = re.search(r'\bmodel\s+"([^"]+)"', body)
        info['model'] = m.group(1) if m else None
        for pm in re.finditer(r'part\s+(\d+)\s*\{\s*tag\s+"([^"]+)"\s*model\s+"([^"]+)"', body):
            info['parts'].append((int(pm.group(1)), pm.group(2).lower(), pm.group(3)))
    return info

def idle_frame(paks, config):
    data = paks.read(config) if config else None
    if not data:
        return 0
    for line in data.decode(errors='replace').splitlines():
        line = line.split('//')[0].strip()
        if re.match(r'^\d+\s+\d+', line):
            return int(line.split()[0])
    return 0

def vr_scale(weap):
    """How big the VR mod draws this gun (cg_vr.c CG_VR_ScaleFor), for showing centimetres."""
    sp = TABLES['spans'].get(weap)
    s = 0.45 * TABLES['handBig']
    if sp:
        s = TABLES['handSpan'] / max(sp['trig'], sp['sup'], 1e-3) * TABLES['gunSizes'].get(weap, TABLES['handBig'])
    return min(1.75, max(0.3, s))

CM_PER_UNIT_WORLD = 2.5     # cg_vr.c: VR_HAND_SPAN 8 units is about 20 cm

class Piece:
    def __init__(self, owner, path, origin, axis, model):
        self.owner, self.path, self.origin, self.axis, self.model = owner, path, origin, axis, model

def pose_weapon(paks, weap):
    """The pieces of a weapon in hands model space: [Piece] (owner 'gun', '0'.., 'rhand')."""
    text = paks.read('weapons/' + weap)
    if not text:
        return []
    info = parse_weap(text.decode(errors='replace'))
    hands_data = paks.read(info['hands']) if info['hands'] else None
    if not hands_data:
        return []
    hands = Model(hands_data, idle_frame(paks, info['config']))
    out = []

    def attach(owner, path, tag):
        key = paks.find(path) if path else None
        if not key or tag not in hands.tags or not key.endswith('.md3'):
            return
        o, a = compose([0, 0, 0], IDENT, *hands.tags[tag])
        out.append(Piece(owner, key, o, a, Model(paks.read(key), 0)))

    attach('gun', info['model'], 'tag_weapon')
    for index, tag, path in info['parts']:
        attach(str(index), path, tag)
    pose = TABLES['poses'].get(weap)
    if pose and pose['trigPiece'] == -3:
        key = paks.find('models/vr/rhand_%s.md3' % os.path.splitext(weap)[0])
        if key:
            out.append(Piece('rhand', key, [0, 0, 0], IDENT, Model(paks.read(key), 0)))
    return out

def owner_of(piece_index):
    return {-1: 'gun', -3: 'rhand'}.get(piece_index, str(piece_index))

def weapon_hands(weap):
    """{'trigger': owner, 'support': owner} for the hands the VR mod draws on this gun."""
    pose = TABLES['poses'].get(weap)
    out = {}
    if pose and pose['trigPiece'] != -2:
        out['trigger'] = owner_of(pose['trigPiece'])
    if pose and pose['supPiece'] != -2:
        out['support'] = owner_of(pose['supPiece'])
    return out

def catalog(paks):
    rows = []
    for weap in sorted(TABLES['poses']):
        if not paks.find('weapons/' + weap):
            continue
        hands = weapon_hands(weap)
        if hands:
            rows.append(dict(id=weap, name=os.path.splitext(weap)[0], hands=sorted(hands)))
    return rows

# ------------------------------------------------------------------ the finger rig

# glove_default's layout: the back of the hand on the top half (fingers to the right, little
# finger at the top), the palm side on the bottom half (fingers to the right, index at the top),
# the thumb's two halves as islands between them. Per finger: (side, v range, u where the
# knuckle (MCP) is, u of the tip).
FINGER_BANDS = {
    'little': [('back', 0.000, 0.075, 0.43, 0.640), ('palm', 0.905, 1.000, 0.70, 0.885)],
    'ring':   [('back', 0.075, 0.170, 0.43, 0.755), ('palm', 0.845, 0.905, 0.70, 0.965)],
    'middle': [('back', 0.170, 0.270, 0.43, 0.770), ('palm', 0.765, 0.845, 0.70, 0.990)],
    'index':  [('back', 0.270, 0.425, 0.43, 0.725), ('palm', 0.665, 0.765, 0.70, 0.985)],
}
# the thumb islands: base point and tip point in texture space
THUMB_ISLANDS = [('back', (0.175, 0.425, 0.415, 0.675), (0.205, 0.445), (0.375, 0.650)),
                 ('palm', (0.530, 0.455, 0.700, 0.665), (0.590, 0.655), (0.650, 0.470))]
# joints along each finger (0 knuckle, 1 tip) and how far a bend spreads either side
JOINTS = {'thumb': (0.0, 0.42, 0.70), 'index': (0.0, 0.47, 0.76), 'middle': (0.0, 0.47, 0.76),
          'ring': (0.0, 0.47, 0.76), 'little': (0.0, 0.47, 0.76)}
BLEND = 0.07

def classify(u, v, thumb=False):
    """(finger, t along it, side) for a texture coordinate, finger None for the palm."""
    u, v = u % 1.0, v % 1.0
    for side, (u0, v0, u1, v1), b, tip in THUMB_ISLANDS:
        if thumb and ((side == 'back') == (v < 0.45 or u < 0.45)):
            dx, dy = tip[0] - b[0], tip[1] - b[1]
            t = ((u - b[0]) * dx + (v - b[1]) * dy) / (dx * dx + dy * dy)
            return 'thumb', t, side
    if thumb:
        return None, 0.0, 'back' if u < 0.45 else 'palm'
    for finger, bands in FINGER_BANDS.items():
        for side, va, vb, ku, tu in bands:
            if va <= v < vb and (side == 'back' and v < 0.43 or side == 'palm' and v >= 0.665 and u > 0.25):
                return finger, (u - ku) / (tu - ku), side
    return None, 0.0, 'back' if v < 0.43 else 'palm'

def classify_mesh(st, tris, points):
    """classify() per vertex, by texture island (the mesh is split at texture seams, so its
    connected pieces are the islands), then welded: points at the same place agree."""
    n = len(st)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, b, c in tris:
        for x, y in ((a, b), (b, c)):
            rx, ry = find(x), find(y)
            if rx != ry:
                parent[rx] = ry
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    island = [None] * n
    for members in groups.values():
        u = sum(st[i][0] % 1.0 for i in members) / len(members)
        v = sum(st[i][1] % 1.0 for i in members) / len(members)
        kind = 'main'
        for side, (u0, v0, u1, v1), _b, _t in THUMB_ISLANDS:
            if u0 <= u <= u1 and v0 <= v <= v1 and len(members) < n // 4:
                kind = 'thumb'
        if kind == 'main' and u < 0.22 and v > 0.68:
            kind = 'cap'
        for i in members:
            island[i] = kind
    cls = []
    for i, (u, v) in enumerate(st):
        if island[i] == 'cap':
            cls.append((None, 0.0, 'palm'))
            continue
        c = classify(u, v, island[i] == 'thumb')
        cls.append(c)
    # weld: the same point on both sides of a seam takes the same finger and place along it
    weld = {}
    for i, p in enumerate(points):
        weld.setdefault((round(p[0] * 64), round(p[1] * 64), round(p[2] * 64)), []).append(i)
    for members in weld.values():
        if len(members) < 2:
            continue
        fingers = [cls[i][0] for i in members if cls[i][0]]
        if not fingers:
            continue
        f = max(set(fingers), key=fingers.count)
        ts = [cls[i][1] if cls[i][0] == f else 0.0 for i in members]
        t = sum(ts) / len(ts)
        for i in members:
            cls[i] = (f, t, cls[i][2])
    return cls

class HandRig:
    def __init__(self, hand, arm):
        self.points = [p for p, _ in hand.verts]
        self.normals = [n for _, n in hand.verts]
        self.tris = hand.tris
        self.cls = classify_mesh(hand.st, hand.tris, self.points)
        back = [p for p, c in zip(self.points, self.cls) if c[2] == 'back']
        palm = [p for p, c in zip(self.points, self.cls) if c[2] == 'palm']
        self.hand_n = norm(sub(mean(back), mean(palm))) if back and palm else [0, 0, 1]
        knuckles = []
        self.joints = {}
        for f in DIGITS:
            idx = [i for i, c in enumerate(self.cls) if c[0] == f]
            self.joints[f] = self._fit(f, idx)
            if f != 'thumb' and self.joints[f]:
                knuckles.append(self.joints[f][0]['p'])
        # the root pivot: the middle of the knuckles and the wrist side of the palm
        palm_pts = [p for p, c in zip(self.points, self.cls) if c[0] is None]
        self.palm = mean(palm_pts + knuckles * 20) if palm_pts else mean(self.points)
        # the sleeve: points near the wrist move with the hand, fading up the arm
        self.arm_points = [p for p, _ in arm.verts] if arm else []
        self.arm_normals = [n for _, n in arm.verts] if arm else []
        self.arm_tris = arm.tris if arm else []
        self.arm_w = self._arm_weights()

    def _fit(self, f, idx):
        if len(idx) < 12:
            return None
        ts = [self.cls[i][1] for i in idx]
        joints = []
        for j, tj in enumerate(JOINTS[f]):
            near, width = [], 0.05
            while len(near) < 6 and width < 0.3:
                near = [i for i in idx if abs(self.cls[i][1] - tj) < width]
                width += 0.03
            if not near:
                return None
            p = mean([self.points[i] for i in near])
            ahead_hi = JOINTS[f][j + 1] if j + 1 < 3 else 1.01
            ahead = [self.points[i] for i in idx if tj + 0.04 < self.cls[i][1] <= max(ahead_hi, tj + 0.2)]
            if len(ahead) < 3:
                ahead = [self.points[i] for i in idx if self.cls[i][1] > tj]
            d = norm(sub(mean(ahead), p)) if ahead else [1, 0, 0]
            b = [self.points[i] for i in near if self.cls[i][2] == 'back']
            pl = [self.points[i] for i in near if self.cls[i][2] == 'palm']
            n = sub(mean(b), mean(pl)) if b and pl else self.hand_n
            n = norm(sub(n, scale(d, dot(n, d))))
            if length(n) < 0.5:
                n = norm(sub(self.hand_n, scale(d, dot(self.hand_n, d))))
            # bending curls the finger towards the palm: d turns towards -n
            joints.append(dict(t=tj, p=p, d=d, n=n, bend=norm(cross(n, d))))
        # each bend is about the joint's pivot: on the back/palm middle line
        return joints

    def _arm_weights(self):
        if not self.arm_points:
            return []
        cell = 2.0
        grid = {}
        for p in self.points:
            grid.setdefault((int(p[0] // cell), int(p[1] // cell), int(p[2] // cell)), []).append(p)
        ring = []
        for p in self.arm_points:
            c = (int(p[0] // cell), int(p[1] // cell), int(p[2] // cell))
            best = 9e9
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for q in grid.get((c[0] + dx, c[1] + dy, c[2] + dz), ()):
                            best = min(best, dot(sub(p, q), sub(p, q)))
            if best < 1.5 * 1.5:
                ring.append(p)
        self.wrist = mean(ring) if ring else mean(self.points)
        return [1.0 - smoothstep(length(sub(p, self.wrist)) / 9.0) for p in self.arm_points]

    def vertex_transforms(self, edit):
        """Per hand vertex affine transform for an edit, and the root transform."""
        root = root_transform(edit, self.palm)
        fingers = edit.get('fingers', {})
        finger_tf = {}
        for f in DIGITS:
            j = self.joints.get(f)
            vals = fingers.get(f, [0, 0, 0, 0, 0])
            if not j or not any(abs(v) > 1e-6 for v in vals):
                continue
            # the knuckle: splay (about n), bend, roll (about d); then the two finger joints
            rot0 = mat_mul(axis_angle(j[0]['n'], vals[3]), mat_mul(axis_angle(j[0]['bend'], vals[0]), axis_angle(j[0]['d'], vals[4])))
            finger_tf[f] = [aff_about(rot0, j[0]['p']), aff_about(axis_angle(j[1]['bend'], vals[1]), j[1]['p']),
                            aff_about(axis_angle(j[2]['bend'], vals[2]), j[2]['p'])]
        out = []
        for c in self.cls:
            f, t = c[0], c[1]
            tf = AFF_ID
            if f in finger_tf:
                js = self.joints[f]
                # distal first, in the rest pose: each joint's turn carries everything past it
                for k in (2, 1, 0):
                    lo = js[k]['t'] - (0.0 if (f == 'thumb' and k == 0) else BLEND)
                    w = smoothstep((t - lo) / (2 * BLEND))
                    if w > 0.001:
                        tf = aff_mul(aff_lerp_id(finger_tf[f][k], w), tf)
            out.append(aff_mul(root, tf))
        return out, root

    def arm_transforms(self, edit, root):
        if not edit.get('sleeveFollows', True):
            return [AFF_ID] * len(self.arm_points)
        return [aff_lerp_id(root, w) for w in self.arm_w]

    def preview(self, edit):
        tfs, root = self.vertex_transforms(edit)
        pts = [aff_apply(f, p) for f, p in zip(tfs, self.points)]
        arm = [aff_apply(f, p) for f, p in zip(self.arm_transforms(edit, root), self.arm_points)]
        joints = []
        for f in DIGITS:
            js = self.joints.get(f)
            if not js:
                continue
            # the joint chain as posed: pivots and the tip, through the nearest vertex transforms
            chain = []
            for k in range(3):
                i = min(range(len(self.points)), key=lambda i: (self.cls[i][0] != f) * 99 + abs(self.cls[i][1] - js[k]['t'] - 0.02))
                chain.append(aff_apply(tfs[i], js[k]['p']))
            tip = [i for i, c in enumerate(self.cls) if c[0] == f and c[1] > 0.9]
            if tip:
                chain.append(mean([pts[i] for i in tip]))
            joints.append(dict(finger=f, points=[[round(x, 3) for x in p] for p in chain]))
        return dict(points=[[round(x, 3) for x in p] for p in pts], arm=[[round(x, 3) for x in p] for p in arm],
                    joints=joints, palm=[round(x, 3) for x in aff_apply(root, self.palm)])

def root_transform(edit, pivot):
    m = euler_xyz(edit.get('rotationDeg', [0, 0, 0]))
    s = float(edit.get('scale', 1.0))
    m = [[m[i][j] * s for j in range(3)] for i in range(3)]
    t = edit.get('translation', [0, 0, 0])
    return m, add(sub(pivot, mat_vec(m, pivot)), t)

# ------------------------------------------------------------------ edits

def fresh_hand():
    return dict(translation=[0.0, 0.0, 0.0], rotationDeg=[0.0, 0.0, 0.0], scale=1.0, sleeveFollows=True,
                fingers={d: [0.0] * 5 for d in DIGITS})

def _num(v, lo, hi):
    v = float(v)
    if not math.isfinite(v):
        raise ValueError('Bad number')
    return max(lo, min(hi, v))

def validate_hand(h):
    out = fresh_hand()
    out['translation'] = [_num(v, -60, 60) for v in h.get('translation', [0, 0, 0])][:3]
    out['rotationDeg'] = [_num(v, -180, 180) for v in h.get('rotationDeg', [0, 0, 0])][:3]
    out['scale'] = _num(h.get('scale', 1.0), 0.4, 2.0)
    out['sleeveFollows'] = bool(h.get('sleeveFollows', True))
    for d in DIGITS:
        vals = h.get('fingers', {}).get(d, [0] * 5)
        out['fingers'][d] = [_num(v, -150, 150) for v in list(vals)[:5]] + [0.0] * (5 - len(list(vals)[:5]))
    if len(out['translation']) != 3 or len(out['rotationDeg']) != 3:
        raise ValueError('Bad hand transform')
    return out

def validate(doc):
    if not isinstance(doc, dict) or doc.get('version') != 1 or not isinstance(doc.get('weapons'), dict):
        raise ValueError('Expected a version 1 hand edit file')
    out = dict(version=1, weapons={})
    for weap, w in doc['weapons'].items():
        if weap not in TABLES['poses']:
            continue
        ew = dict(weaponScale=_num(w.get('weaponScale', 1.0), 0.25, 3.0), hands={})
        for side, h in (w.get('hands') or {}).items():
            if side in ('trigger', 'support'):
                ew['hands'][side] = validate_hand(h)
        out['weapons'][weap] = ew
    return out

def hand_is_identity(h):
    f = fresh_hand()
    return (all(abs(a - b) < 1e-4 for a, b in zip(h['translation'] + h['rotationDeg'], f['translation'] + f['rotationDeg']))
            and abs(h['scale'] - 1) < 1e-4 and all(abs(v) < 1e-4 for d in DIGITS for v in h['fingers'][d]))

# ------------------------------------------------------------------ the editor's view of a weapon

class WeaponCache:
    def __init__(self, paks):
        self.paks = paks
        self.weapons = {}

    def get(self, weap):
        if weap not in self.weapons:
            self.weapons[weap] = self._load(weap)
        return self.weapons[weap]

    def _load(self, weap):
        pieces = pose_weapon(self.paks, weap)
        hands = weapon_hands(weap)
        owners = set(hands.values())
        gun_pts, gun_tris, rigs, where = [], [], {}, {}
        for pc in pieces:
            for s in pc.model.surfaces:
                if is_hand(s.name) or is_arm(s.name):
                    continue
                if pc.owner == 'rhand':
                    continue
                base = len(gun_pts)
                gun_pts += [to_parent(pc.origin, pc.axis, p) for p, _ in s.verts]
                gun_tris += [[a + base, b + base, c + base] for a, b, c in s.tris]
        for side, owner in hands.items():
            pc = next((p for p in pieces if p.owner == owner), None)
            if not pc:
                continue
            hs = [s for s in pc.model.surfaces if is_hand(s.name)]
            if not hs:
                continue
            arms = [s for s in pc.model.surfaces if is_arm(s.name)]

            def cam(s):
                return Surface(s.name, s.tris, s.st, [(to_parent(pc.origin, pc.axis, p), mat_vec(mat_t(pc.axis), n)) for p, n in s.verts], s.nverts, s.offsets)
            rigs[side] = HandRig(cam(hs[0]), cam(arms[0]) if arms else None)
            where[side] = dict(piece=pc, hand=hs[0].name, arm=arms[0].name if arms else None)
        shared = sorted(w for w in TABLES['poses'] if w != weap and self._shares(weap, w, where))
        return dict(weap=weap, gun=dict(points=gun_pts, tris=gun_tris), rigs=rigs, where=where, shared=shared)

    def _shares(self, weap, other, where):
        mine = {v['piece'].path for v in where.values()}
        if not mine:
            return False
        text = self.paks.read('weapons/' + other)
        if not text:
            return False
        info = parse_weap(text.decode(errors='replace'))
        hands = weapon_hands(other)
        theirs = set()
        for owner in hands.values():
            if owner == 'gun' and info['model']:
                theirs.add(self.paks.find(info['model']))
            for i, _tag, path in info['parts']:
                if str(i) == owner:
                    theirs.add(self.paks.find(path))
        return bool(mine & theirs)

    def view(self, weap):
        w = self.get(weap)
        r3 = lambda p: [round(x, 3) for x in p]
        hands = {}
        for side, rig in w['rigs'].items():
            hands[side] = dict(points=[r3(p) for p in rig.points], triangles=[list(t) for t in rig.tris],
                               arm=[r3(p) for p in rig.arm_points], armTriangles=[list(t) for t in rig.arm_tris],
                               palm=r3(rig.palm))
        return dict(id=weap, name=os.path.splitext(weap)[0], weapon=dict(points=[r3(p) for p in w['gun']['points']], triangles=w['gun']['tris']),
                    hands=hands, cmPerUnit=vr_scale(weap) * CM_PER_UNIT_WORLD, shared=w['shared'])

# ------------------------------------------------------------------ baking

def bake(paks, cache, edits, log=print):
    """The edited models: {pk3 path: bytes}. Hands are moved in the model files themselves,
    every frame by the same per vertex transform, so the game draws them as edited."""
    files = {}       # path -> bytearray
    owners = {}      # path -> weapon that edited it
    for weap, w in sorted(edits['weapons'].items()):
        hands = {s: h for s, h in w['hands'].items() if not hand_is_identity(h)}
        if not hands:
            continue
        info = cache.get(weap)
        for side, h in hands.items():
            if side not in info['rigs']:
                continue
            rig, where = info['rigs'][side], info['where'][side]
            pc = where['piece']
            if pc.path in owners and owners[pc.path] != weap:
                log('Note: %s shares %s with %s; %s\'s %s hand wins there.' % (weap, pc.path, owners[pc.path], owners[pc.path], side))
                continue
            owners[pc.path] = weap
            data = files.setdefault(pc.path, bytearray(paks.read(pc.path)))
            tfs, root = rig.vertex_transforms(h)
            _patch(data, pc, where['hand'], tfs)
            if where['arm'] and h.get('sleeveFollows', True):
                _patch(data, pc, where['arm'], rig.arm_transforms(h, root))
            log('%s: %s hand edited in %s' % (weap, side, pc.path))
    for path, data in files.items():
        _fix_bounds(data)
    return {k: bytes(v) for k, v in files.items()}

def _patch(data, pc, surf_name, cam_tfs):
    """Apply hands-model-space transforms to one surface of a piece, in its own space."""
    model = Model(bytes(data), 0)
    s = next(x for x in model.surfaces if x.name == surf_name)
    a = pc.axis                     # rows: piece axes in hands space; local -> cam: o + a^T p
    at = mat_t(a)
    o = pc.origin
    off, oxyz, nf, nv = s.offsets['off'], s.offsets['xyz'], s.offsets['frames'], s.nverts
    local = []
    for m, t in cam_tfs:
        lm = mat_mul(mat_mul(a, m), at)
        lt = mat_vec(a, sub(add(mat_vec(m, o), t), o))
        local.append((lm, lt))
    for f in range(nf):
        for i in range(nv):
            at_ = off + oxyz + (f * nv + i) * 8
            x, y, z, n = struct.unpack_from('<3hH', data, at_)
            lm, lt = local[i]
            p = add(mat_vec(lm, [x / 64.0, y / 64.0, z / 64.0]), lt)
            nn = mat_vec(lm, decode_normal(n))
            q = [max(-32768, min(32767, int(round(c * 64)))) for c in p]
            struct.pack_into('<3hH', data, at_, q[0], q[1], q[2], encode_normal(nn))

def _fix_bounds(data):
    """Each frame's bounds and radius, for culling, from its vertices."""
    nframes, _nt, nsurfs = struct.unpack_from('<3i', data, 76)
    ofs_frames, _ofs_tags, ofs_surfs = struct.unpack_from('<3i', data, 92)
    lo = [[9e9] * 3 for _ in range(nframes)]
    hi = [[-9e9] * 3 for _ in range(nframes)]
    off = ofs_surfs
    for _ in range(nsurfs):
        _f, sframes, _ns, nv, _nt2 = struct.unpack_from('<5i', data, off + 68)
        o_xyz, o_end = struct.unpack_from('<i', data, off + 100)[0], struct.unpack_from('<i', data, off + 104)[0]
        for f in range(min(sframes, nframes)):
            for i in range(nv):
                x, y, z = struct.unpack_from('<3h', data, off + o_xyz + (f * nv + i) * 8)
                for k, c in enumerate((x / 64.0, y / 64.0, z / 64.0)):
                    lo[f][k] = min(lo[f][k], c)
                    hi[f][k] = max(hi[f][k], c)
        off += o_end
    for f in range(nframes):
        if lo[f][0] > hi[f][0]:
            continue
        o = ofs_frames + f * 56
        origin = struct.unpack_from('<3f', data, o + 24)
        r = max(length(sub([a if abs(a - origin[k]) > abs(b - origin[k]) else b for k, (a, b) in enumerate(zip(lo[f], hi[f]))], list(origin))), 0.1)
        struct.pack_into('<3f3f', data, o, *lo[f], *hi[f])
        struct.pack_into('<f', data, o + 36, r)

def write_pk3(path, files, edits):
    tmp = path + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(files.items()):
            z.writestr(name, data)
        z.writestr('vr_handedits.json', json.dumps(edits, indent=1))
        z.writestr('vr_handedits_readme.txt', 'Hand and finger edits made with the RealRTCW VR Hand Editor.\n'
                   'Delete this file to put the hands back as they were.\n')
    os.replace(tmp, path)

def update_align_file(path, edits):
    """vr_handalign.txt (read by the mod's cgame): each weapon's size as the gun line's size,
    keeping everything else in it (the in-game handtweak lines)."""
    lines, seen = [], set()
    header = ('// Brutally RealRTCW VR hand alignment. One line per weapon and part:\n'
              '// <weapon file> <gun|trigger|support> <forward> <left> <up> <pitch> <yaw> <roll> <size>\n')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8', errors='replace').read().splitlines():
            parts = line.split()
            if len(parts) >= 9 and parts[1] == 'gun' and parts[0] in edits['weapons']:
                parts[8] = '%.3f' % edits['weapons'][parts[0]]['weaponScale']
                seen.add(parts[0])
                line = ' '.join(parts)
            lines.append(line)
    else:
        lines = header.rstrip('\n').split('\n')
    for weap, w in sorted(edits['weapons'].items()):
        if weap not in seen and abs(w['weaponScale'] - 1.0) > 1e-4:
            lines.append('%s gun 0.000 0.000 0.000 0.00 0.00 0.00 %.3f' % (weap, w['weaponScale']))
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)
