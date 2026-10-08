import sys,os,math
sys.path.insert(0,'/tmp/claude-0/src/RealRTCW-VR/misc/vr')
import make_vr_assets as M
paks=M.Paks('/mnt/user-data/uploads/RealRTCW/Main')
def d2(a,b): return sum((a[k]-b[k])**2 for k in range(3))
def wrist(pieces,owner,hand):
    hv=[p for p,_ in hand.verts]
    arms=[s for o,s in pieces if o==owner and any(w in s.name.lower() for w in ("arm","sleeve")) and s.verts]
    best=[]
    step=max(1,len(hv)//300)
    hs=hv[::step]
    for s in arms:
        for p,_ in s.verts:
            if min(d2(p,q) for q in hs) < 1.5**2: best.append(p)
    if not best: return M.centroid(hand)
    return [sum(p[k] for p in best)/len(best) for k in range(3)]
rows=[]
for name in sorted(n for n in paks.names() if n.startswith("weapons/") and n.endswith(".weap") and not n.startswith("weapons/vanilla/")):
    info=M.parse_weap(paks.read(name).decode(errors="replace"))
    try: pieces=M.pose_weapon(paks,info)
    except Exception: continue
    palm=M.find_grip(pieces)
    if not palm: continue
    palm=[palm[0]+M.GRIP_SHIFT.get(name,0.0),palm[1],palm[2]]
    vis=lambda c: 0.0 < c[0] < 50.0 and c[2] > -25.0
    hands=[(o,s,M.centroid(s)) for o,s in pieces if any(w in s.name.lower() for w in M.HAND_WORDS) and s.verts]
    hands=[h for h in hands if vis(h[2])]
    trig=sorted([h for h in hands if d2(h[2],palm)<8**2],key=lambda h:d2(h[2],palm))
    trig=trig[0] if trig else None
    sup=None
    for h in hands:
        if trig and h[0]==trig[0]: continue
        if h[2][0] < palm[0]+5: continue
        if sup is None or h[2][0]>sup[2][0]: sup=h
    if not trig and not sup: continue
    pc=lambda o: -1 if o=="gun" else int(o)
    tw=wrist(pieces,trig[0],trig[1]) if trig else (0,0,0)
    sw=wrist(pieces,sup[0],sup[1]) if sup else (0,0,0)
    rows.append('\t{ "%s", %d, { %.1ff, %.1ff, %.1ff }, %d, { %.1ff, %.1ff, %.1ff } },' % (os.path.basename(name),
        pc(trig[0]) if trig else -2, *tw, pc(sup[0]) if sup else -2, *sw))
    print(os.path.basename(name), "palm %.1f %.1f %.1f"%tuple(palm), "trigC", trig and [round(x,1) for x in trig[2]], "supC", sup and [round(x,1) for x in sup[2]], file=sys.stderr)
open('/tmp/claude-0/adstool/hands_table.c','w').write("\n".join(rows)+"\n")
