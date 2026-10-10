# RealRTCW-VR: a trigger hand for the long guns whose first person model has none on screen
# (the flat game parks it off screen; VR drew the free MP40 hand, posed for a pistol grip,
# which clipped through the stock). The BAR's trigger hand holds a rifle's wrist, so it is
# moved onto each of these guns: its palm onto the gun's wrist (measured by eye from side
# renders: just behind the trigger guard) and scaled to the gun's support hand, so both
# hands are the same size. Writes models/vr/rhand_<gun>.md3 into a folder (for the pk3) and
# prints the grip points for vr/weapons.cfg and the C rows for cg_vr.c (hand poses, sleeve
# ends).
import sys, os, math, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_vr_assets as M

paks = M.Paks(sys.argv[1])

def d2(a, b):
    return sum((a[k] - b[k]) ** 2 for k in range(3))

# (as vr_hand_size.py)
def edges(s):
    out = {}
    for t in s.tris:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            ka = (round(s.st[a][0], 4), round(s.st[a][1], 4))
            kb = (round(s.st[b][0], 4), round(s.st[b][1], 4))
            if ka != kb:
                out[tuple(sorted((ka, kb)))] = math.dist(s.verts[a][0], s.verts[b][0])
    return out

def hand_size(s, ref=[]):
    if not ref:
        mp40 = M.pose_weapon(paks, M.parse_weap(paks.read("weapons/mp40.weap").decode(errors="replace")))
        ref.append(edges([x for o, x in mp40 if o == "gun" and "hand" in x.name.lower()][0]))
    e = edges(s)
    return statistics.median(e[k] / ref[0][k] for k in set(e) & set(ref[0]) if ref[0][k] > 0.05) * 10.9

# (as vr_hand_table.py) where the hand meets its sleeve
def wrist(pieces, owner, hand):
    hs = [p for p, _ in hand.verts][::max(1, len(hand.verts) // 300)]
    best = [p for o, s in pieces if o == owner and "arm" in s.name.lower() for p, _ in s.verts
            if min(d2(p, q) for q in hs) < 1.5 ** 2]
    return [sum(p[k] for p in best) / len(best) for k in range(3)]

# (as vr_arm_ends.py) the far end of the sleeve
def arm_end(pieces, owner, hand, hands):
    hc = M.centroid(hand)
    others = [M.centroid(s) for o, s in hands if s is not hand]
    pts = [p for o, s in pieces if o == owner and "arm" in s.name.lower() for p, _ in s.verts
           if all(d2(p, hc) <= d2(p, c) for c in others)]
    far = max(math.sqrt(d2(p, hc)) for p in pts)
    end = [p for p in pts if math.sqrt(d2(p, hc)) > far * 0.85]
    return [sum(p[k] for p in end) / len(end) for k in range(3)]
outdir = sys.argv[2]

# the palm on each gun's wrist (first person model space, x forward, z up); y stays the
# measured grip's
RIFLE_PALMS = {
    "mauser": (6.0, -12.5),
    "sniperrifle": (6.0, -12.5),
    "mosin": (4.5, -7.5),
    "g43": (3.0, -5.5),
    "m1garand": (5.0, -10.5),
    "m7": (5.0, -10.5),
    "mp44": (15.0, -13.8),
    "sten": (12.0, -12.5),
    "mp34": (10.0, -16.0),
    "ithaca": (7.5, -10.5),
    # only the left (support) hand is on these in the flat game, so VR drew that one on the
    # right controller: (x, y, z), on the pistol grip / back of the receiver, y on the gun
    "venom": (4.0, -9.1, -15.5),
    "flamethrower": (-3.0, -9.5, -12.5),
    "tesla": (-2.6, -8.4, -11.5),
    "browning": (5.0, -6.5, -13.0),
    "mg42m": (-0.5, -6.0, -22.5),
    # drawn bigger than their own hands make them (cg_vr.c vrGunSizes): the stock's wrist
    "ppsh": (0.0, -9.0),
    "garand": (2.0, -7.0),
    "snooper": (2.0, -7.0),
}

# cg_vr.c vrGunSizes: these guns are drawn this much bigger, so their trigger hand is made
# this much smaller here to come out the size of every other hand (1.15x life size)
RIFLE_SHRINK = {
    "ppsh": 1.33 / 1.15,
    "garand": 1.3 / 1.15,
    "snooper": 1.3 / 1.15,
    "venom": 1.5 / 1.15,
    "tesla": 1.5 / 1.15,
    "mp34": 1.0 / 1.15,
}

# where the index finger rests on each gun's trigger (x, z: the front of the trigger blade,
# halfway down, read off close side renders). The donor hand is moved so its own finger,
# which rests on its own gun's trigger, lands there; guns not listed keep RIFLE_PALMS
# (the Kar98 and its sniper version as they are, the guns without a trigger)
TRIGGERS = {
    "mosin": (7.2, -8.5),
    "g43": (2.6, -7.3),
    "m1garand": (13.1, -13.5),
    "m7": (13.1, -13.5),
    "mp44": (19.3, -13.8),
    "sten": (11.3, -15.8),
    "mp34": (14.8, -18.3),
    "ithaca": (11.0, -10.7),
    "ppsh": (8.1, -8.3),
    "garand": (8.6, -4.85),
    "snooper": (8.6, -4.85),
    "venom": (8.6, -12.7),
    "tesla": (-0.1, -8.4),
    "mg42m": (8.2, -18.0),
}
# the donor hands and their own triggers: the BAR's trigger hand holds a rifle's wrist, the
# MP40's a pistol grip
DONORS = {
    "bar": (14.4, -12.9),
    "mp40": (10.0, -8.9),
}
PISTOL_GRIPS = {    # gun: degrees its grip leans further back than the MP40's (turned about the trigger)
    "mp44": 0.0,
    "venom": 10.0,
    "tesla": 6.0,
    "mg42m": -3.0,
}

def posed(name):
    return M.pose_weapon(paks, M.parse_weap(paks.read("weapons/%s.weap" % name).decode(errors="replace")))

def trigger_y(pieces, x, z):
    """The middle of the gun across, at its trigger."""
    ys = [p[1] for o, s in pieces if not any(w in s.name.lower() for w in M.HAND_WORDS + ("arm",))
          for p, _ in s.verts if abs(p[0] - x) < 0.6 and abs(p[2] - z) < 0.6]
    return sum(ys) / len(ys) if ys else None

donors = {}
for dname, (dx, dz) in DONORS.items():
    d = posed(dname)
    dhand = [s for o, s in d if o == "gun" and "hand" in s.name.lower()][0]
    darm = [s for o, s in d if o == "gun" and "arm" in s.name.lower()][0]
    dall = [(o, s) for o, s in d if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
    donors[dname] = dict(hand=dhand, arm=darm, palm=M.centroid(dhand), size=hand_size(dhand),
                         wrist=wrist(d, "gun", dhand), end=arm_end(d, "gun", dhand, dall),
                         trig=[dx, trigger_y(d, dx, dz), dz])

cfg = {}
for l in open(sys.argv[3]):
    f = l.split()
    if f and f[0].startswith("weapons/"):
        cfg[f[0]] = f
only = set(sys.argv[4:])

os.makedirs(os.path.join(outdir, "models/vr"), exist_ok=True)
for name in sorted(set(RIFLE_PALMS) | set(TRIGGERS)):
    if only and name not in only:
        continue
    pieces = posed(name)
    sup = [s for o, s in pieces if "hand" in s.name.lower() and s.verts and 0 < M.centroid(s)[0] < 50 and M.centroid(s)[2] > -25]
    sup.sort(key=lambda s: -M.centroid(s)[0])
    line = cfg["weapons/%s.weap" % name]
    if name in TRIGGERS:
        D = donors["mp40" if name in PISTOL_GRIPS else "bar"]
        tx, tz = TRIGGERS[name]
        trig = [tx, trigger_y(pieces, tx, tz), tz]
        a = math.radians(PISTOL_GRIPS.get(name, 0.0))
        rot = lambda v: [v[0] * math.cos(a) - v[2] * math.sin(a), v[1], v[0] * math.sin(a) + v[2] * math.cos(a)]
    else:
        D = donors["bar"]
        pt = RIFLE_PALMS[name]
        trig = [pt[0], float(line[2]), pt[1]] if len(pt) == 2 else list(pt)     # (the palm, here)
        rot = lambda v: v
    k = hand_size(sup[0]) / D["size"] / RIFLE_SHRINK.get(name, 1.0)
    ref = D["trig"] if name in TRIGGERS else D["palm"]
    xf = lambda p: [trig[i] + k * rot([p[j] - ref[j] for j in range(3)])[i] for i in range(3)]
    palm = xf(D["palm"])
    surfs = []
    for s in (D["hand"], D["arm"]):
        surfs.append(M.Surface(s.name, s.shader, s.tris, s.st, [(xf(p), rot(n)) for p, n in s.verts]))
    path = "models/vr/rhand_%s.md3" % name
    open(os.path.join(outdir, path), "wb").write(M.write_md3(path, surfs, [0, 0, 0]))
    line[1:4] = ["%.2f" % palm[0], "%.2f" % palm[1], "%.2f" % palm[2]]
    print("CFG", "  ".join([line[0], " ".join(line[1:4]), " ".join(line[4:6])]) + "  " + " ".join(line[6:]))
    w, e = xf(D["wrist"]), xf(D["end"])
    print('POSE\t{ "%s.weap", -3, { %.1ff, %.1ff, %.1ff } },' % (name, *w))
    print('SUP\t{ "%s.weap", %.1ff, { %.1ff, %.1ff, %.1ff } },' % (name, hand_size(sup[0]), *M.centroid(sup[0])))
    print('END\t{ "%s.weap", { %.1ff, %.1ff, %.1ff } },' % (name, *e))
