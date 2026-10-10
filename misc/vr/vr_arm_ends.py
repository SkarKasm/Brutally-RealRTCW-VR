# RealRTCW-VR: where each first person arm's sleeve ends towards the elbow (model units, the
# weapon's posed first person model space), for the trigger and the support hand of every
# weapon (same choice of hands as vr_hand_table.py), and for the free hands (hand_r / hand_l,
# relative to their palm). cg_vr.c joins BJ's own arm to these points. Prints C rows.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_vr_assets as M
paks = M.Paks(sys.argv[1])

def d2(a, b):
    return sum((a[k] - b[k]) ** 2 for k in range(3))

def arm_end(pieces, owner, hand, hands):
    """Mean of the far end of the sleeve / arm belonging to this hand."""
    hc = M.centroid(hand)
    others = [M.centroid(s) for o, s in hands if s is not hand]
    pts = []
    for o, s in pieces:
        if o != owner or not any(w in s.name.lower() for w in ("arm", "sleeve")):
            continue
        for p, _ in s.verts:
            if all(d2(p, hc) <= d2(p, c) for c in others):
                pts.append(p)
    if not pts:
        return None
    far = max(math.sqrt(d2(p, hc)) for p in pts)
    end = [p for p in pts if math.sqrt(d2(p, hc)) > far * 0.85]
    return [sum(p[k] for p in end) / len(end) for k in range(3)]

rows = []
for name in sorted(n for n in paks.names() if n.startswith("weapons/") and n.endswith(".weap") and not n.startswith("weapons/vanilla/")):
    info = M.parse_weap(paks.read(name).decode(errors="replace"))
    try:
        pieces = M.pose_weapon(paks, info)
    except Exception:
        continue
    palm = M.find_grip(pieces)
    if not palm:
        continue
    palm = [palm[0] + M.GRIP_SHIFT.get(name, 0.0), palm[1], palm[2]]
    vis = lambda c: 0.0 < c[0] < 50.0 and c[2] > -25.0
    allhands = [(o, s) for o, s in pieces if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
    hands = [(o, s, M.centroid(s)) for o, s in allhands]
    hands = [h for h in hands if vis(h[2])]
    trig = sorted([h for h in hands if d2(h[2], palm) < 8 ** 2], key=lambda h: d2(h[2], palm))
    trig = trig[0] if trig else None
    sup = None
    for h in hands:
        if trig and h[0] == trig[0]:
            continue
        if h[2][0] < palm[0] + 5:
            continue
        if sup is None or h[2][0] > sup[2][0]:
            sup = h
    if not trig and not sup:
        continue
    te = arm_end(pieces, trig[0], trig[1], allhands) if trig else None
    se = arm_end(pieces, sup[0], sup[1], allhands) if sup else None
    f = lambda e: "{ %.1ff, %.1ff, %.1ff }" % tuple(e) if e else "{ 0, 0, 0 }"
    rows.append('\t{ "%s", %d, %s, %d, %s },' % (os.path.basename(name), 1 if te else 0, f(te), 1 if se else 0, f(se)))

mp40 = M.pose_weapon(paks, M.parse_weap(paks.read("weapons/mp40.weap").decode(errors="replace")))
allhands = [(o, s) for o, s in mp40 if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
print("\n".join(rows))
for owner, key in (("gun", "R"), ("2", "L")):
    surfs, centre = M.hand_model(mp40, owner)
    hand = [s for o, s in allhands if o == owner][0]
    e = arm_end(mp40, owner, hand, allhands)
    print("// free hand %s: { %.1ff, %.1ff, %.1ff }" % (key, e[0] - centre[0], e[1] - centre[1], e[2] - centre[2]))
