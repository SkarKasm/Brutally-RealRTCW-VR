# RealRTCW-VR: how big each weapon's first person hands are, so cg_vr.c can draw every gun at
# the size that makes its hand life size, and scale a support hand that was modelled bigger
# or smaller than the trigger hand. All the hands are the same glove mesh in different poses:
# matching edges by their texture coordinates against the MP40's right hand and taking the
# median length ratio gives a size that doesn't change with the pose (a fist and an open hand
# measure the same, unlike the longest span). Sizes are in the MP40 right hand's span units
# (10.9). Same choice of trigger / support hand as vr_arm_ends.py. Prints a C table.
import sys, os, math, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_vr_assets as M
paks = M.Paks(sys.argv[1])
REF_SPAN = 10.9

def d2(a, b):
    return sum((a[k] - b[k]) ** 2 for k in range(3))

def edges(s):
    out = {}
    for t in s.tris:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            ka = (round(s.st[a][0], 4), round(s.st[a][1], 4))
            kb = (round(s.st[b][0], 4), round(s.st[b][1], 4))
            if ka != kb:
                out[tuple(sorted((ka, kb)))] = math.dist(s.verts[a][0], s.verts[b][0])
    return out

def posed(name):
    return M.pose_weapon(paks, M.parse_weap(paks.read(name).decode(errors="replace")))

ref = edges([s for o, s in posed("weapons/mp40.weap") if o == "gun" and "hand" in s.name.lower()][0])

def size(s):
    e = edges(s)
    r = [e[k] / ref[k] for k in set(e) & set(ref) if ref[k] > 0.05]
    return statistics.median(r) * REF_SPAN if len(r) > 200 else None

rows = []
for name in sorted(n for n in paks.names() if n.startswith("weapons/") and n.endswith(".weap") and not n.startswith("weapons/vanilla/")):
    try:
        pieces = posed(name)
    except Exception:
        continue
    palm = M.find_grip(pieces)
    if not palm:
        continue
    palm = [palm[0] + M.GRIP_SHIFT.get(name, 0.0), palm[1], palm[2]]
    vis = lambda c: 0.0 < c[0] < 50.0 and c[2] > -25.0
    hands = [(o, s, M.centroid(s)) for o, s in pieces if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
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
    ts = size(trig[1]) if trig else None
    ss = size(sup[1]) if sup else None
    if not ts and not ss:
        continue
    sc = sup[2] if sup else (0, 0, 0)     # the support hand's palm: where the off hand takes hold
    rows.append('\t{ "%s", %.1ff, %.1ff, { %.1ff, %.1ff, %.1ff } },' % (os.path.basename(name), ts or 0, ss or 0, *sc))

mp40 = posed("weapons/mp40.weap")
left = [s for o, s in mp40 if o == "2" and "hand" in s.name.lower()][0]
print("\n".join(rows))
print("// free hands (hand_r / hand_l): %.1f %.1f" % (REF_SPAN, size(left)))
