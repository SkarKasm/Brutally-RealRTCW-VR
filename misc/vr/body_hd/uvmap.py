import sys; sys.path.insert(0,'.')
from mds import load
from PIL import Image, ImageDraw
bones,surfs=load(sys.argv[1]); tex=Image.open(sys.argv[2]).convert('RGB').resize((1024,1024))
names=[b[0] for b in bones]
def cls(ws):
    bi=max(ws,key=lambda w:w[1])[0]; n=names[bi]
    if 'UpperArm' in n: return 'ua'
    if 'Forearm' in n: return 'fa'
    if 'Hand' in n or 'Finger' in n: return 'hand'
    if 'Clavicle' in n: return 'cl'
    return 'other'
col={'ua':(255,0,0),'fa':(0,255,0),'hand':(0,0,255),'cl':(255,255,0),'other':None}
ov=Image.new('RGB',(1024,1024)); dr=ImageDraw.Draw(tex)
sn,sh,verts,tris=surfs[0]
for t in tris:
    cs=[cls(verts[i][1]) for i in t]
    c=max(set(cs),key=cs.count)
    pts=[(verts[i][0][0]*1024,verts[i][0][1]*1024) for i in t]
    if col[c]: dr.polygon(pts,outline=col[c])
    else: dr.polygon(pts,outline=(90,90,90))
tex.save(sys.argv[3])
