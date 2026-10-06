"""
RealRTCW-VR asset builder.

Builds main/z_vr_assets.pk3 from the player's own RealRTCW install (RealRTCW's art can't
be redistributed, so it is generated locally by deploy.sh):

  models/vr/hand_r.md3     right hand + sleeve (MP40 grip hand)
  models/vr/hand_l.md3     left hand + sleeve (MP40 support hand)
  vr/weapon_grips.cfg      for every .weap: where the trigger hand holds the first person
                           model (the game hides the model's own hands and draws these)

The first person models are built around the camera with +x forward. Each one is posed at
its idle frame, every piece is put on its tag, and the gripping right hand is found among
the hand surfaces. In game the palm goes on the controller, so every gun sits in the hand
without per-weapon tuning.

usage: python make_vr_assets.py <RealRTCW Main dir> <output pk3>
"""

import math
import os
import re
import struct
import sys
import zipfile

MODEL_PAKS = ("z_realrtcw_models_weapons.pk3",)
SCRIPT_PAKS = ("z_zzrealrtcw_scripts.pk3",)
HAND_WORDS = ("hand", "glove")
SKIN_WORDS = ("hand", "arm", "sleeve", "glove")     # must match R_VR_IsHandSurface

MDC_TAG_ANGLE_SCALE = 360.0 / 32700.0


class Paks:
    """Looks files up in the pk3s the way the game does (later paks win)."""

    def __init__(self, main_dir):
        self.zips = []
        for name in sorted(os.listdir(main_dir)):
            if name.lower().endswith(".pk3"):
                try:
                    self.zips.append(zipfile.ZipFile(os.path.join(main_dir, name)))
                except zipfile.BadZipFile:
                    pass
        self.index = {}
        for z in self.zips:
            for n in z.namelist():
                self.index[n.lower()] = (z, n)

    def read(self, path):
        hit = self.index.get(path.lower().replace("\\", "/"))
        if not hit:
            # .md3 / .mdc are interchangeable in the game's loader
            base, ext = os.path.splitext(path.lower())
            for alt in (".md3", ".mdc"):
                hit = self.index.get(base + alt)
                if hit:
                    break
        return hit[0].read(hit[1]) if hit else None

    def names(self):
        return list(self.index.keys())


# ---------------------------------------------------------------- math

def mat_identity():
    return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]


def angles_to_axis(pitch, yaw, roll):
    # Quake AngleVectors: axis[0] forward, axis[1] left, axis[2] up
    p, y, r = (math.radians(a) for a in (pitch, yaw, roll))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    fwd = [cp * cy, cp * sy, -sp]
    right = [-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp]
    up = [cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp]
    return [fwd, [-v for v in right], up]


def to_parent(origin, axis, p):
    # child local point -> parent space, as CG_PositionEntityOnTag does
    return [origin[k] + p[0] * axis[0][k] + p[1] * axis[1][k] + p[2] * axis[2][k] for k in range(3)]


def compose(parent_origin, parent_axis, tag_origin, tag_axis):
    origin = to_parent(parent_origin, parent_axis, tag_origin)
    axis = [[sum(tag_axis[i][j] * parent_axis[j][k] for j in range(3)) for k in range(3)] for i in range(3)]
    return origin, axis


def decode_normal(n):
    lat = ((n >> 8) & 255) * 2 * math.pi / 255
    lng = (n & 255) * 2 * math.pi / 255
    return [math.cos(lat) * math.sin(lng), math.sin(lat) * math.sin(lng), math.cos(lng)]


def encode_normal(v):
    x, y, z = v
    if x == 0 and y == 0:
        return 0 if z > 0 else (128 & 255)
    lat = int(math.atan2(y, x) * 255 / (2 * math.pi)) & 255
    lng = int(math.acos(max(-1.0, min(1.0, z))) * 255 / (2 * math.pi)) & 255
    return (lat << 8) | lng


# ---------------------------------------------------------------- models

class Surface:
    def __init__(self, name, shader, tris, st, verts):
        self.name, self.shader, self.tris, self.st, self.verts = name, shader, tris, st, verts


class Model:
    """md3 or mdc, read at one frame: surfaces with positions/normals, plus tags."""

    def __init__(self, data, frame=0):
        self.surfaces = []
        self.tags = {}
        ident = data[:4]
        if ident == b"IDP3":
            self._md3(data, frame)
        elif ident == b"IDPC":
            self._mdc(data, frame)
        else:
            raise ValueError("unknown model format")

    def _md3(self, d, frame):
        num_frames, num_tags, num_surfs = struct.unpack_from("<3i", d, 76)
        ofs_tags, ofs_surfs = struct.unpack_from("<i", d, 96)[0], struct.unpack_from("<i", d, 100)[0]
        frame = min(frame, num_frames - 1)
        for t in range(num_tags):
            o = ofs_tags + (frame * num_tags + t) * 112
            name = d[o:o + 64].split(b"\0")[0].decode().lower()
            vals = struct.unpack_from("<12f", d, o + 64)
            self.tags[name] = (list(vals[0:3]), [list(vals[3:6]), list(vals[6:9]), list(vals[9:12])])
        off = ofs_surfs
        for _ in range(num_surfs):
            name = d[off + 4:off + 68].split(b"\0")[0].decode()
            _f, sframes, _ns, nv, nt = struct.unpack_from("<5i", d, off + 68)
            o_tri, o_sh, o_st, o_xyz, o_end = struct.unpack_from("<5i", d, off + 88)
            shader = d[off + o_sh:off + o_sh + 64].split(b"\0")[0].decode()
            sf = min(frame, sframes - 1)
            tris = [struct.unpack_from("<3i", d, off + o_tri + i * 12) for i in range(nt)]
            st = [struct.unpack_from("<2f", d, off + o_st + i * 8) for i in range(nv)]
            verts = []
            for i in range(nv):
                x, y, z, n = struct.unpack_from("<3hH", d, off + o_xyz + (sf * nv + i) * 8)
                verts.append(([x / 64.0, y / 64.0, z / 64.0], decode_normal(n)))
            self.surfaces.append(Surface(name, shader, tris, st, verts))
            off += o_end

    def _mdc(self, d, frame):
        num_frames, num_tags, num_surfs = struct.unpack_from("<3i", d, 76)
        ofs_tagnames, ofs_tags, ofs_surfs = struct.unpack_from("<3i", d, 96)
        frame = min(frame, num_frames - 1)
        for t in range(num_tags):
            name = d[ofs_tagnames + t * 64:ofs_tagnames + t * 64 + 64].split(b"\0")[0].decode().lower()
            x, y, z, a0, a1, a2 = struct.unpack_from("<6h", d, ofs_tags + (frame * num_tags + t) * 12)
            axis = angles_to_axis(a0 * MDC_TAG_ANGLE_SCALE, a1 * MDC_TAG_ANGLE_SCALE, a2 * MDC_TAG_ANGLE_SCALE)
            self.tags[name] = ([x / 64.0, y / 64.0, z / 64.0], axis)
        off = ofs_surfs
        for _ in range(num_surfs):
            name = d[off + 4:off + 68].split(b"\0")[0].decode()
            v = struct.unpack_from("<14i", d, off + 68)
            _flags, _ncomp, nbase, _nsh, nv, nt = v[:6]
            o_tri, o_sh, o_st, o_xyz, _o_comp, o_fbase, _o_fcomp, o_end = v[6:14]
            shader = d[off + o_sh:off + o_sh + 64].split(b"\0")[0].decode()
            base = struct.unpack_from("<h", d, off + o_fbase + frame * 2)[0] if nbase else 0
            tris = [struct.unpack_from("<3i", d, off + o_tri + i * 12) for i in range(nt)]
            st = [struct.unpack_from("<2f", d, off + o_st + i * 8) for i in range(nv)]
            verts = []
            for i in range(nv):
                x, y, z, n = struct.unpack_from("<3hH", d, off + o_xyz + (base * nv + i) * 8)
                verts.append(([x / 64.0, y / 64.0, z / 64.0], decode_normal(n)))
            self.surfaces.append(Surface(name, shader, tris, st, verts))
            off += o_end


def transformed(surface, origin, axis):
    verts = []
    for p, n in surface.verts:
        q = to_parent(origin, axis, p)
        m = [n[0] * axis[0][k] + n[1] * axis[1][k] + n[2] * axis[2][k] for k in range(3)]
        verts.append((q, m))
    return Surface(surface.name, surface.shader, surface.tris, surface.st, verts)


def centroid(surface):
    n = len(surface.verts)
    return [sum(v[0][k] for v in surface.verts) / n for k in range(3)]


# ---------------------------------------------------------------- .weap parsing

def parse_weap(text):
    text = re.sub(r"//[^\n]*", "", text)
    info = {"parts": []}
    m = re.search(r'handsModel\s+"([^"]+)"', text)
    info["hands"] = m.group(1) if m else None
    m = re.search(r'weaponConfig\s+"([^"]+)"', text)
    info["config"] = m.group(1) if m else None
    m = re.search(r'modModel\s+0\s+"([^"]+)"', text)
    info["scope"] = m.group(1) if m else None
    fp = re.search(r"firstPerson\s*\{", text)
    if fp:
        body = text[fp.end():]
        m = re.search(r'\bmodel\s+"([^"]+)"', body)
        info["model"] = m.group(1) if m else None
        for pm in re.finditer(r'part\s+(\d+)\s*\{\s*tag\s+"([^"]+)"\s*model\s+"([^"]+)"', body):
            info["parts"].append((int(pm.group(1)), pm.group(2).lower(), pm.group(3)))
    else:
        info["model"] = None
    return info


def idle_frame(paks, config):
    data = paks.read(config) if config else None
    if not data:
        return 0
    for line in data.decode(errors="replace").splitlines():
        line = line.split("//")[0].strip()
        if re.match(r"^\d+\s+\d+", line):
            return int(line.split()[0])
    return 0


def pose_weapon(paks, info):
    """All pieces of a first person weapon in camera space: [(owner, Surface)]."""
    frame = idle_frame(paks, info["config"])
    hands_data = paks.read(info["hands"]) if info["hands"] else None
    if not hands_data:
        return []
    hands = Model(hands_data, frame)
    root_o, root_a = [0, 0, 0], mat_identity()
    out = [("hands", s) for s in hands.surfaces]

    def attach(owner, path, tag):
        data = paks.read(path) if path else None
        if not data or tag not in hands.tags:
            return
        o, a = compose(root_o, root_a, *hands.tags[tag])
        for s in Model(data, 0).surfaces:
            out.append((owner, transformed(s, o, a)))

    attach("gun", info["model"], "tag_weapon")
    for index, tag, path in info["parts"]:
        attach(str(index), path, tag)
    return out


# ---------------------------------------------------------------- scopes

LENS_WORDS = ("optic", "lens")


def scope_lens(paks, info):
    """The eyepiece (rear lens) of a scope on the gun, or None: (part, centre, radius) with
    part 0 = the gun model, 1 = the scope attachment (modModel 0), centre in that model."""
    for part, path in ((0, info.get("model")), (1, info.get("scope"))):
        data = paks.read(path) if path else None
        if not data:
            continue
        try:
            model = Model(data, 0)
        except (ValueError, struct.error, IndexError):
            continue
        glass = [p for s in model.surfaces
                 if any(w in (s.name + " " + s.shader).lower() for w in LENS_WORDS) for p, _n in s.verts]
        if len(glass) < 6:
            continue
        # models face +x: the rear lens is the glass furthest back
        back = min(p[0] for p in glass)
        length = max(p[0] for p in glass) - back
        ring = [p for p in glass if p[0] - back < max(0.6, 0.1 * length)]
        ys, zs = [p[1] for p in ring], [p[2] for p in ring]
        centre = [back - 0.05, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2]
        radius = max(max(ys) - min(ys), max(zs) - min(zs)) / 2
        if radius < 0.3:
            continue
        return part, centre, radius
    return None


def weapon_anims(paks, config):
    """[(first, length, name)] from a weapon.cfg"""
    data = paks.read(config) if config else None
    out = []
    if not data:
        return out
    for line in data.decode(errors="replace").splitlines():
        code, _, comment = line.partition("//")
        nums = code.split()
        if len(nums) >= 4 and nums[0].isdigit() and nums[1].isdigit():
            out.append((int(nums[0]), int(nums[1]), comment.strip().upper()))
    return out


def piece_centroids(paks, info, frame):
    """{owner: (camera space centroid, model local centroid)} of each piece's non-hand
    geometry, with the hands model at the given frame."""
    hands_data = paks.read(info["hands"]) if info["hands"] else None
    if not hands_data:
        return {}
    hands = Model(hands_data, frame)
    out = {}
    pieces = [("gun", "tag_weapon", info["model"])] + [(str(i), t, p) for i, t, p in info["parts"]]
    for owner, tag, path in pieces:
        data = paks.read(path) if path else None
        if not data or tag not in hands.tags:
            continue
        o, a = hands.tags[tag]
        local = [p for s in Model(data, 0).surfaces
                 if not any(w in s.name.lower() for w in SKIN_WORDS) for p, _n in s.verts]
        if not local:
            continue
        lc = [sum(p[k] for p in local) / len(local) for k in range(3)]
        out[owner] = (to_parent(o, a, lc), lc)
    return out


def reload_geometry(paks, info):
    """From the reload animation: the magazine piece (drops down out of the gun) and the
    slide / bolt piece (travels backwards along the gun), as
    (magPart, magPoint, magLocal, magDir, chargePart, chargePoint, chargeDir, travel)."""
    anims = weapon_anims(paks, info["config"])
    actions = [a for a in anims if "RELOAD" in a[2] or "ATTACK" in a[2]]
    if not anims or not actions:
        return None
    idle = piece_centroids(paks, info, anims[0][0])
    idle_gun = gun_frame(paks, info, anims[0][0])
    if not idle_gun:
        return None

    # Motion of every piece relative to the gun body: during a reload the whole gun also
    # moves and tilts, which would otherwise read as the bolt moving into the gun. Reloads
    # show magazines coming out; the bolt / pump cycling is often in the fire animation.
    moves = {}      # owner -> {"reload"/"attack": (dist, d in gun space)}
    for first, length, name in actions:
        kind = "reload" if "RELOAD" in name else "attack"
        for f in range(first, first + length, max(1, length // 16)):
            cur = piece_centroids(paks, info, f)
            gun = gun_frame(paks, info, f)
            if not gun:
                continue
            for owner, (c, _l) in cur.items():
                if owner == "gun" or owner not in idle:
                    continue
                a = to_local(gun, c)
                b = to_local(idle_gun, idle[owner][0])
                d = [a[k] - b[k] for k in range(3)]
                dist = math.sqrt(sum(x * x for x in d))
                best = moves.setdefault(owner, {})
                if dist > best.get(kind, (0.0, None))[0]:
                    best[kind] = (dist, d)

    def on_screen(p):
        return 0.0 < p[0] < 60.0

    def unit(v):
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        return [x / n for x in v]

    mag = None
    for owner, m in moves.items():
        if "reload" not in m or not on_screen(idle[owner][0]):
            continue
        dist, d = m["reload"]
        # the biggest mover that isn't sliding straight back (side magazines, shells and
        # revolver rounds move every which way)
        if dist > 8.0 and -d[0] < 0.85 * dist:
            if mag is None or dist > moves[mag]["reload"][0]:
                mag = owner

    # slide / bolt / pump: slides back along the gun (straight back, a little either way)
    charge, charge_back = None, 0.0
    for owner, m in moves.items():
        if owner == mag or not on_screen(idle[owner][0]):
            continue
        for kind in ("attack", "reload"):
            if kind not in m:
                continue
            dist, d = m[kind]
            if 2.0 < dist < 20.0 and -d[0] > 0.6 * dist and -d[0] > charge_back:
                charge, charge_back = owner, -d[0]

    out = [-1, [0, 0, 0], [0, 0, 0], [0, 0, -1], -1, [0, 0, 0], [-1, 0, 0], 0.0]
    if mag is not None:
        # direction back out of gun space into camera space at the idle frame
        out[0:4] = [int(mag), idle[mag][0], idle[mag][1], unit(dir_to_camera(idle_gun, moves[mag]["reload"][1]))]
    if charge is not None:
        # always straight back along the gun body
        out[4:8] = [int(charge), idle[charge][0], unit(dir_to_camera(idle_gun, [-1.0, 0.0, 0.0])),
                    min(max(charge_back, 4.0), 10.0)]
    return out


def gun_frame(paks, info, frame):
    """(origin, axis) of the gun body (tag_weapon of the hands model) at a frame."""
    data = paks.read(info["hands"]) if info["hands"] else None
    if not data:
        return None
    tags = Model(data, frame).tags
    return tags.get("tag_weapon")


def to_local(frame, p):
    o, a = frame
    d = [p[k] - o[k] for k in range(3)]
    return [sum(d[k] * a[i][k] for k in range(3)) for i in range(3)]


def dir_to_camera(frame, v):
    _o, a = frame
    return [v[0] * a[0][k] + v[1] * a[1][k] + v[2] * a[2][k] for k in range(3)]


def barrel_correction(pieces):
    """First person guns are angled a little towards the middle of a flat screen. Returns
    the (pitch, yaw) that turns the gun body's long axis to point straight ahead."""
    pts = [p for owner, s in pieces if owner == "gun" and not any(w in s.name.lower() for w in SKIN_WORDS)
           for p, _n in s.verts]
    if len(pts) < 10:
        return 0.0, 0.0
    c = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
    cov = [[sum((p[i] - c[i]) * (p[j] - c[j]) for p in pts) / len(pts) for j in range(3)] for i in range(3)]
    v = [1.0, 0.0, 0.0]
    for _ in range(50):     # power iteration: principal axis
        w = [sum(cov[i][j] * v[j] for j in range(3)) for i in range(3)]
        n = math.sqrt(sum(x * x for x in w)) or 1.0
        v = [x / n for x in w]
    if v[0] < 0:
        v = [-x for x in v]
    yaw = math.degrees(math.atan2(v[1], v[0]))
    pitch = -math.degrees(math.atan2(v[2], math.hypot(v[0], v[1])))
    if abs(yaw) > 8.0 or abs(pitch) > 8.0:
        return 0.0, 0.0     # not a long gun body, leave it
    return -pitch, -yaw


# Grips moved along the gun (game units, + forward) where the automatic estimate is off,
# measured with the in-game inspector (vr_inspect 1, ruler ticks every 5 units).
GRIP_SHIFT = {
    "weapons/mp34.weap": -6.0,
    "weapons/ppsh.weap": 7.0,
    "weapons/mauser.weap": -7.0,
    "weapons/sniperrifle.weap": -7.0,
    "weapons/mosin.weap": -13.0,
    "weapons/g43.weap": -13.0,
    "weapons/garand.weap": -12.0,
    "weapons/snooper.weap": -12.0,
}

# on guns where both hands are on screen (MP40, Thompson) the trigger hand sits about this
# far behind the support hand
SUPPORT_TO_GRIP = (-18.0, -1.0, 0.0)
SUPPORT_HAND_MIN_X = 28.0


def find_grip(pieces):
    """Where the trigger hand holds the gun, in camera space.

    A hand on the gun model itself, in trigger position, is the grip (pistols, SMGs). Long
    guns only show the support hand on the forestock at their idle frame (the trigger hand
    is parked off screen), so then the grip is estimated behind the rearmost visible hand."""
    def visible(c):
        return 0.0 < c[0] < 50.0 and c[2] > -25.0   # anything else is parked off screen

    hands = []
    for owner, s in pieces:
        if any(w in s.name.lower() for w in HAND_WORDS) and s.verts:
            c = centroid(s)
            if visible(c):
                hands.append((owner, c))
    if not hands:
        return None

    on_gun = [c for o, c in hands if o == "gun" and c[0] <= SUPPORT_HAND_MIN_X]
    if on_gun:
        return min(on_gun, key=lambda c: c[1])      # rightmost
    c = min((c for _, c in hands), key=lambda c: c[0])
    if c[0] > SUPPORT_HAND_MIN_X:
        c = [c[k] + SUPPORT_TO_GRIP[k] for k in range(3)]
    return c


MELEE = ("knife.weap",)


def blade_correction(pieces, grip):
    """Melee weapons are held at an angle in the flat game. Returns the (pitch, yaw) that
    turns the blade (the weapon point furthest from the grip) to point straight ahead."""
    far, best = None, 0.0
    for owner, s in pieces:
        if any(w in s.name.lower() for w in SKIN_WORDS):
            continue
        for p, _n in s.verts:
            d = sum((p[k] - grip[k]) ** 2 for k in range(3))
            if d > best:
                far, best = p, d
    if far is None:
        return 0.0, 0.0
    d = [far[k] - grip[k] for k in range(3)]
    yaw = math.degrees(math.atan2(d[1], d[0]))
    pitch = -math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))   # Quake pitch: + is down
    # rotate the model by the opposite to line the blade up with the hand
    return -pitch, -yaw


# ---------------------------------------------------------------- md3 writer

def write_md3(name, surfaces, centre):
    allpts = []
    blobs = []
    for s in surfaces:
        xyz = b""
        for p, n in s.verts:
            q = [p[k] - centre[k] for k in range(3)]
            allpts.append(q)
            xyz += struct.pack("<3hH", *(int(round(c * 64)) for c in q), encode_normal(n))
        tri_blob = b"".join(struct.pack("<3i", *t) for t in s.tris)
        st_blob = b"".join(struct.pack("<2f", *t) for t in s.st)
        shader_blob = s.shader.encode().ljust(64, b"\0") + struct.pack("<i", 0)
        o_sh = 108
        o_tri = o_sh + len(shader_blob)
        o_st = o_tri + len(tri_blob)
        o_xyz = o_st + len(st_blob)
        o_end = o_xyz + len(xyz)
        head = b"IDP3" + s.name.encode().ljust(64, b"\0")
        head += struct.pack("<5i", 0, 1, 1, len(s.verts), len(s.tris))
        head += struct.pack("<5i", o_tri, o_sh, o_st, o_xyz, o_end)
        blobs.append(head + shader_blob + tri_blob + st_blob + xyz)

    mins = [min(p[i] for p in allpts) for i in range(3)]
    maxs = [max(p[i] for p in allpts) for i in range(3)]
    radius = max(math.sqrt(sum(c * c for c in p)) for p in allpts)
    frame = struct.pack("<3f3f3ff", *mins, *maxs, 0, 0, 0, radius) + b"vr_hand".ljust(16, b"\0")
    o_frames = 108
    o_surfs = o_frames + len(frame)
    body = b"".join(blobs)
    head = b"IDP3" + struct.pack("<i", 15) + name.encode().ljust(64, b"\0")
    head += struct.pack("<i4i4i", 0, 1, 0, len(blobs), 0, o_frames, o_surfs, o_surfs, o_surfs + len(body))
    return head + frame + body


def hand_model(pieces, owner):
    """Hand + sleeve surfaces of one piece, centred on the palm."""
    surfs = [s for o, s in pieces if o == owner and any(w in s.name.lower() for w in SKIN_WORDS)]
    hands = [s for s in surfs if any(w in s.name.lower() for w in HAND_WORDS)]
    return surfs, centroid(hands[0])


# ---------------------------------------------------------------- main

# ---------------------------------------------------------------- sounds

def read_wav(data):
    """(params, mono-ish sample list for analysis, raw frames) of a 16 bit wav, or None."""
    import io
    import wave
    try:
        w = wave.open(io.BytesIO(data))
    except (wave.Error, EOFError):
        return None
    if w.getsampwidth() != 2:
        return None
    params = w.getparams()
    frames = w.readframes(w.getnframes())
    samples = struct.unpack("<%dh" % (len(frames) // 2), frames)[::params.nchannels]
    return params, samples, frames


def sound_events(params, samples):
    """Times (s) of the sharp mechanical clicks in a recording."""
    rate = params.framerate
    win = max(1, int(rate * 0.01))
    env = [max(abs(x) for x in samples[i:i + win]) for i in range(0, len(samples) - win, win)]
    if not env:
        return []
    peak = max(env) or 1
    events, last = [], -100
    for i, e in enumerate(env):
        prev = max(env[max(0, i - 5):i] or [0])
        if e > 0.25 * peak and e > 2.5 * prev + 200 and i - last > 8:
            events.append(i * 0.01)
            last = i
    return events


def cut_wav(params, frames, start, end):
    """A slice of a recording as a new wav, with a short fade at the end."""
    import io
    import wave
    bpf = params.nchannels * 2
    total = len(frames) // bpf
    a = max(0, int(start * params.framerate))
    b = min(total, int(end * params.framerate))
    if b <= a:
        return None
    chunk = list(struct.unpack("<%dh" % ((b - a) * params.nchannels), frames[a * bpf:b * bpf]))
    fade = min(len(chunk) // params.nchannels, int(0.02 * params.framerate))
    for i in range(fade):
        f = i / float(fade)
        for c in range(params.nchannels):
            k = len(chunk) - (i + 1) * params.nchannels + c
            chunk[k] = int(chunk[k] * f)
    out = io.BytesIO()
    w = wave.open(out, "wb")
    w.setnchannels(params.nchannels)
    w.setsampwidth(2)
    w.setframerate(params.framerate)
    w.writeframes(struct.pack("<%dh" % len(chunk), *chunk))
    w.close()
    return out.getvalue()


# shotguns have separate recordings for the pump and for a shell going in
SHOTGUN_SOUNDS = {
    "ithaca.weap": ("sound/weapons/ithaca/ithaca_reload.wav", "sound/weapons/ithaca/ithaca_pump_reload_fast.wav"),
}


def reload_sounds(paks, weap_name, weap_text):
    """{"out"/"in"/"rack": wav bytes} cut from the weapon's own reload recording."""
    base = os.path.basename(weap_name)
    out = {}
    if base in SHOTGUN_SOUNDS:
        shell_path, pump_path = SHOTGUN_SOUNDS[base]
        for key, path in (("in", shell_path), ("rack", pump_path)):
            data = paks.read(path)
            wav = read_wav(data) if data else None
            if wav:
                params, samples, frames = wav
                ev = sound_events(params, samples)
                start = max(0.0, (ev[-1] if key == "rack" and ev else 0.0) - 0.08)
                clip = cut_wav(params, frames, start, start + 0.9)
                if clip:
                    out[key] = clip
        return out

    m = re.search(r'^\s*reloadSound\s+"([^"]+)"', weap_text, re.M)
    data = paks.read(m.group(1)) if m else None
    wav = read_wav(data) if data else None
    if not wav:
        return out
    params, samples, frames = wav
    length = len(samples) / float(params.framerate)
    ev = sound_events(params, samples)
    if not ev:
        return out

    # first click: the magazine coming out; middle: going in; last pair: slide / bolt
    rack_at = ev[-2] if len(ev) >= 3 and ev[-1] - ev[-2] < 0.3 else ev[-1]
    in_at = ev[len(ev) // 2] if len(ev) >= 3 else ev[-1]
    picks = {
        "out": (ev[0], min(ev[1] if len(ev) > 1 else length, ev[0] + 0.5)),
        "in": (in_at, min(in_at + 0.45, rack_at if rack_at > in_at else length)),
        "rack": (rack_at, min(length, rack_at + 0.8)),
    }
    for key, (a, b) in picks.items():
        clip = cut_wav(params, frames, max(0.0, a - 0.03), b)
        if clip:
            out[key] = clip
    return out


# ---------------------------------------------------------------- menus

def menu_multi(cvar, label, choices, x, y, alignx=120):
    if all(isinstance(v, str) for _t, v in choices):
        kind = "cvarStrList"
        items = " ".join('"%s" "%s"' % (t, v) for t, v in choices)
    else:
        kind = "cvarFloatList"
        items = " ".join('"%s" %s' % (t, v) for t, v in choices)
    return """
		itemDef {
			name vr_opt
			type ITEM_TYPE_MULTI
			text "%s"
			cvar "%s"
			%s { %s }
			rect %d %d 290 14
			textalign ITEM_ALIGN_RIGHT
			textalignx %d
			textaligny 11
			textscale .22
			textfont UI_FONT_BIG
			style WINDOW_STYLE_FILLED
			backcolor 1 1 1 .07
			forecolor 1 1 1 1
			visible 1
		}
""" % (label, cvar, kind, items, x, y, alignx)


def menu_text(name, text, x, y, w, scale, color, align="ITEM_ALIGN_LEFT", alignx=4):
    return """
		itemDef {
			name %s
			text "%s"
			rect %d %d %d 16
			textalign %s
			textalignx %d
			textaligny 13
			textscale %s
			textfont UI_FONT_BIG
			forecolor %s
			visible 1
			decoration
		}
""" % (name, text, x, y, w, align, alignx, scale, color)


def menu_button(name, text, x, w, action):
    return """
		itemDef {
			name %s
			type ITEM_TYPE_BUTTON
			text "%s"
			rect %d 425 %d 24
			textalign ITEM_ALIGN_CENTER
			textalignx %d
			textaligny 19
			textscale .3
			textfont UI_FONT_BIG
			style WINDOW_STYLE_FILLED
			backcolor 1 1 1 .07
			forecolor 1 1 1 1
			visible 1
			action { %s }
		}
""" % (name, text, x, w, w // 2, action)


# GORE & EXTRAS (main menu, and a button on VR OPTIONS): the optional mechanics, flat or VR.
# (cvar, label, choices) as in the co-op host page.
EXTRAS = (
    ("g_gore", "Extra gore:", (("Off", 0), ("On", 1))),
    ("g_shotReactions", "Shot reactions:", (("Off", 0), ("Normal", 1), ("Strong", 2))),
    ("g_woundDecals", "Bullet wounds:", (("Off", 0), ("On", 1))),
    ("g_flatGrab", "Body pickup (flat):", (("Off", 0), ("On", 1))),
    ("g_vrGrabAssist", "Grab assist:", (("Off", 0), ("Normal", 1), ("Strong", 2))),
    ("vr_grabHint", "Can grab (VR):", (("Nothing", 0), ("Buzz", 1), ("Buzz and sign", 2))),
)


def vr_extras_menu():
    """GORE & EXTRAS: the optional mechanics, two columns."""
    items = [menu_text("vr_ex_title", "GORE & EXTRAS", 0, 28, 640, ".4", "1 .85 .5 1", "ITEM_ALIGN_CENTER", 320)]
    half = (len(EXTRAS) + 1) // 2
    for i, (cvar, label, choices) in enumerate(EXTRAS):
        items.append(menu_multi(cvar, label, choices, 20 if i < half else 330, 84 + 20 * (i % half), 150))
    items.append(menu_text("vr_ex_note", "Everything here is optional, with or without a headset. "
                           "Settings apply immediately.", 0, 395, 640, ".18", ".8 .8 .8 1", "ITEM_ALIGN_CENTER", 320))
    items.append(menu_button("vr_ex_back", "BACK", 270, 100, "close vr_extras; open main"))
    return ("""// RealRTCW-VR: GORE & EXTRAS, generated by misc/vr/make_vr_assets.py.
#include "ui/menudef.h"

{
	menuDef {
		name "vr_extras"
		rect 0 0 640 480
		fullScreen MENU_TRUE
		visible MENU_FALSE
		focusColor 1 .75 0 1
		style WINDOW_STYLE_FILLED
		backcolor 0 0 0 .85
		onESC { close vr_extras; open main }
""" + "".join(items) + """	}
}
""").encode()


VR_MENU_BUTTON = """
	// VR OPTIONS BUTTON (RealRTCW-VR, added by make_vr_assets.py)
	itemDef {
		name vroptionsmenu
		text "VR OPTIONS"
		group mainmenus
		type 1
		rect 10 342 178 26
		visible 1
		textfont UI_FONT_BIG
		textscale .36
		textalign ITEM_ALIGN_LEFT
		textalignx 0
		textaligny 21
		forecolor 1 1 1 1
		action { close main; open vr_options }
	}

	// GORE & EXTRAS BUTTON (RealRTCW-VR, added by make_vr_assets.py)
	itemDef {
		name vrextrasmenu
		text "GORE & EXTRAS"
		group mainmenus
		type 1
		rect 10 375 178 26
		visible 1
		textfont UI_FONT_BIG
		textscale .36
		textalign ITEM_ALIGN_LEFT
		textalignx 0
		textaligny 21
		forecolor 1 1 1 1
		action { close main; open vr_extras }
	}
"""


def vr_menus(paks):
    """{path: bytes}: RealRTCW's main menu with a VR OPTIONS button, its menu list with the
    VR options page added, and the page itself."""
    here = os.path.dirname(os.path.abspath(__file__))
    out = {}
    with open(os.path.join(here, "ui", "vr_options.menu"), "rb") as f:
        out["ui/vr_options.menu"] = f.read()
    out["ui/vr_extras.menu"] = vr_extras_menu()

    menus = paks.read("ui/menus.txt")
    main = paks.read("ui/main.menu")
    if not menus or not main:
        print("warning: RealRTCW menus not found, no VR OPTIONS button")
        return out

    menus = menus.decode(errors="replace")
    if "vr_options.menu" not in menus:
        menus = menus.replace('loadMenu { "ui/main.menu" }',
                              'loadMenu { "ui/main.menu" }\n\tloadMenu { "ui/vr_options.menu" }\t// RealRTCW-VR', 1)
    if "vr_extras.menu" not in menus:
        menus = menus.replace('loadMenu { "ui/vr_options.menu" }',
                              'loadMenu { "ui/vr_options.menu" }\n\tloadMenu { "ui/vr_extras.menu" }', 1)
    out["ui/menus.txt"] = menus.encode()

    # after the QUIT button, the last of the main menu's buttons
    main = main.decode(errors="replace")
    m = re.search(r'mouseExit\s*\{\s*hide\s+"realrtcw_main_quit"\s*\}\s*\}', main)
    if m and "vroptionsmenu" not in main:
        main = main[:m.end()] + VR_MENU_BUTTON + main[m.end():]
        out["ui/main.menu"] = main.encode()
    else:
        print("warning: couldn't place the VR OPTIONS button in ui/main.menu")
    return out


WEAPONS_CFG_HEADER = """// RealRTCW-VR weapon geometry, generated by misc/vr/make_vr_assets.py, read by bg_vr.c.
// Points are in first person model space (forward left up, game units) at the idle frame.
//
// weap file
//   grip x y z            where the trigger hand holds the gun
//   pitch yaw             rotation that straightens the gun body / blade
//   magPart               linked part that is the magazine / clip / shell (-1: none)
//   mag x y z             where it sits in the gun
//   magLocal x y z        its centre in its own model
//   magDir x y z          the way it comes out
//   chargePart            linked part that is the slide / bolt / pump (-1: none)
//   charge x y z          where it sits
//   chargeDir x y z       the way it is pulled
//   travel                how far it is pulled
//   lensPart              scope eyepiece on the gun (0) or on the scope attachment (1), -1: none
//   lens x y z            its centre in that model
//   lensRadius
"""


def main():
    main_dir, out_pk3 = sys.argv[1], sys.argv[2]
    paks = Paks(main_dir)

    grips = []
    sounds = {}
    for name in sorted(n for n in paks.names() if n.startswith("weapons/") and n.endswith(".weap")):
        text = paks.read(name).decode(errors="replace")
        info = parse_weap(text)
        if not name.startswith("weapons/vanilla/"):
            base = os.path.splitext(os.path.basename(name))[0]
            for key, wav in reload_sounds(paks, name, re.sub(r"//[^\n]*", "", text)).items():
                sounds["vr/sound/%s_%s.wav" % (base, key)] = wav
        try:
            pieces = pose_weapon(paks, info)
        except (ValueError, struct.error, IndexError) as e:
            print("skipping", name, e)
            continue
        palm = find_grip(pieces)
        if not palm:
            continue
        palm = [palm[0] + GRIP_SHIFT.get(name, 0.0), palm[1], palm[2]]
        if os.path.basename(name) in MELEE:
            pitch, yaw = blade_correction(pieces, palm)
        else:
            pitch, yaw = barrel_correction(pieces)
        geo = reload_geometry(paks, info) or [-1, [0, 0, 0], [0, 0, 0], [0, 0, -1], -1, [0, 0, 0], [-1, 0, 0], 0.0]
        mag, mpt, mloc, mdir, chg, cpt, cdir, travel = geo

        def v(x):
            return "%.2f %.2f %.2f" % tuple(x)
        lens = scope_lens(paks, info)
        lens = "  %d %s %.2f" % (lens[0], v(lens[1]), lens[2]) if lens else "  -1 0 0 0 0"
        grips.append("%s  %s %.1f %.1f  %d %s %s %s  %d %s %s %.1f%s" % (
            name, v(palm), pitch, yaw, mag, v(mpt), v(mloc), v(mdir), chg, v(cpt), v(cdir), travel, lens))

    # free hands come from the MP40: right grip hand on the gun, left hand on the magazine
    mp40 = pose_weapon(paks, parse_weap(paks.read("weapons/mp40.weap").decode(errors="replace")))
    right, right_centre = hand_model(mp40, "gun")
    left, left_centre = hand_model(mp40, "2")

    with zipfile.ZipFile(out_pk3, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("models/vr/hand_r.md3", write_md3("models/vr/hand_r.md3", right, right_centre))
        out.writestr("models/vr/hand_l.md3", write_md3("models/vr/hand_l.md3", left, left_centre))
        out.writestr("vr/weapons.cfg", WEAPONS_CFG_HEADER + "\n".join(grips) + "\n")
        for path, wav in sorted(sounds.items()):
            out.writestr(path, wav)
        for path, data in vr_menus(paks).items():
            out.writestr(path, data)
    print("cut %d reload sounds" % len(sounds))
    print("wrote %s: %d weapon grips" % (out_pk3, len(grips)))


if __name__ == "__main__":
    main()
