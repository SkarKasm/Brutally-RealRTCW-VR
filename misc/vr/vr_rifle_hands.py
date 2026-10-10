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
}

def posed(name):
    return M.pose_weapon(paks, M.parse_weap(paks.read("weapons/%s.weap" % name).decode(errors="replace")))

bar = posed("bar")
bhand = [s for o, s in bar if o == "gun" and "hand" in s.name.lower()][0]
barm = [s for o, s in bar if o == "gun" and "arm" in s.name.lower()][0]
bpalm = M.centroid(bhand)
bsize = hand_size(bhand)
ballhands = [(o, s) for o, s in bar if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
bwrist = wrist(bar, "gun", bhand)
bend = arm_end(bar, "gun", bhand, ballhands)

cfg = {}
for l in open(sys.argv[3]):
    f = l.split()
    if f and f[0].startswith("weapons/"):
        cfg[f[0]] = f

os.makedirs(os.path.join(outdir, "models/vr"), exist_ok=True)
for name, (px, pz) in sorted(RIFLE_PALMS.items()):
    pieces = posed(name)
    sup = [s for o, s in pieces if "hand" in s.name.lower() and s.verts and 0 < M.centroid(s)[0] < 50 and M.centroid(s)[2] > -25]
    sup.sort(key=lambda s: -M.centroid(s)[0])
    k = hand_size(sup[0]) / bsize
    line = cfg["weapons/%s.weap" % name]
    palm = [px, float(line[2]), pz]
    xf = lambda p: [palm[i] + k * (p[i] - bpalm[i]) for i in range(3)]
    surfs = []
    for s in (bhand, barm):
        surfs.append(M.Surface(s.name, s.shader, s.tris, s.st, [(xf(p), n) for p, n in s.verts]))
    path = "models/vr/rhand_%s.md3" % name
    open(os.path.join(outdir, path), "wb").write(M.write_md3(path, surfs, [0, 0, 0]))
    line[1:4] = ["%.2f" % palm[0], "%.2f" % palm[1], "%.2f" % palm[2]]
    print("CFG", "  ".join([line[0], " ".join(line[1:4]), " ".join(line[4:6])]) + "  " + " ".join(line[6:]))
    w, e = xf(bwrist), xf(bend)
    print('POSE\t{ "%s.weap", -3, { %.1ff, %.1ff, %.1ff } },' % (name, *w))
    print('END\t{ "%s.weap", { %.1ff, %.1ff, %.1ff } },' % (name, *e))
