# RealRTCW-VR: how big each weapon's first person hand is (its longest span, model units), so
# cg_vr.c can draw every gun at the size that makes its hand life size. Prints a C table.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_vr_assets as M
paks = M.Paks(sys.argv[1])

def span(s):
    v = [p for p, _ in s.verts]
    v = v[::max(1, len(v) // 400)]
    return math.sqrt(max(sum((a[k] - b[k]) ** 2 for k in range(3)) for a in v for b in v))

def d2(a, b):
    return sum((a[k] - b[k]) ** 2 for k in range(3))

rows = []
for name in sorted(n for n in paks.names() if n.startswith("weapons/") and n.endswith(".weap") and not n.startswith("weapons/vanilla/")):
    try:
        pieces = M.pose_weapon(paks, M.parse_weap(paks.read(name).decode(errors="replace")))
    except Exception:
        continue
    hands = [(o, s, M.centroid(s)) for o, s in pieces if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
    hands = [h for h in hands if 0.0 < h[2][0] < 50.0 and h[2][2] > -25.0]
    if not hands:
        continue
    palm = M.find_grip(pieces)
    if palm:
        hands.sort(key=lambda h: d2(h[2], palm))
    rows.append('\t{ "%s", %.1ff },' % (os.path.basename(name), span(hands[0][1])))
    print(os.path.basename(name), rows[-1], file=sys.stderr)

mp40 = M.pose_weapon(paks, M.parse_weap(paks.read("weapons/mp40.weap").decode(errors="replace")))
free = {}
for owner, key in (("gun", "r"), ("2", "l")):
    s = [s for o, s in mp40 if o == owner and any(w in s.name.lower() for w in M.HAND_WORDS)][0]
    free[key] = span(s)
print("\n".join(rows))
print("// free hands (hand_r / hand_l): %.1f %.1f" % (free["r"], free["l"]))
