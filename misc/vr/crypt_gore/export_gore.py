import sys,os,numpy as np
sys.path.insert(0,os.path.dirname(__file__))
from bsp import load
from headframe import frame, tris
import md3
B='/mnt/user-data/uploads/RealRTCW/game/extracted/maps/crypt1.bsp'
ver,shaders,verts,idx,surfs,ent=load(B)
OUT=sys.argv[1]; os.makedirs(OUT,exist_ok=True)
def piece(ids,C,R,off,shader_over=None):
    out=[]
    for si in ids:
        s=surfs[si]; vs=[]; ts=[]
        for k in range(s[4]):
            v=verts[s[3]+k]
            p=(np.array(v[:3])-C)@R.T+off
            n=np.array(v[7:10])@R.T
            vs.append((p,v[3:5],n))
        for k in range(0,s[6],3):
            ts.append(tuple(idx[s[5]+k+j] for j in range(3)))
        sh=shader_over.get(si,shaders[s[0]]) if shader_over else shaders[s[0]]
        out.append(('s%d'%si,sh,vs,ts))
    return out
def info(name,sf):
    P=np.array([v[0] for s in sf for v in s[2]]); print(name,'min',P.min(0).round(1),'max',P.max(0).round(1))
# heads: line up with RTCW's own head models (x forward, z up, neck at z -3.5, face at x +5)
REFMIN=np.array([-3.3,0,-3.5])
C,R,mn,mx=frame([11784,11785,11786],(110,210,290,420),140,440)
off=np.array([REFMIN[0]-mn[0],0,REFMIN[2]-mn[2]])
sf=piece([11784,11785,11786],C,R,off,{11784:'models/vr_gore/deadnazi3_shot',11785:'models/vr_gore/deadnazi3_shot',11786:'models/vr_gore/deadnazi3_shot'})
info('head_shot',sf); md3.write(OUT+'/head_shot.md3','models/vr_gore/head_shot.md3',sf)
C,R,mn,mx=frame([11814],(70,160,90,200),50,200,W=256)
off=np.array([REFMIN[0]-mn[0],0,REFMIN[2]-mn[2]])
off=np.array([REFMIN[0]-1.25*mn[0],0,REFMIN[2]-1.25*mn[2]])
sf=piece([11814],C,R*1.25,off); info('head_skinned',sf); md3.write(OUT+'/head_skinned.md3','models/vr_gore/head_skinned.md3',sf)
# torso frame: its head (bphead) beyond the neck
T=np.array([v[:3] for t in tris([11812]) for v in t]); Tc=(T.min(0)+T.max(0))/2
H=np.array([v[:3] for t in tris([11814]) for v in t]).mean(0)
fw=np.array([0,0,1.0])    # the torn torso lies face up
up=H-Tc; up-=np.dot(up,fw)*fw; up/=np.linalg.norm(up); left=np.cross(up,fw)
R=np.array([fw,left,up])
for nm,ids in (('torso',[11812]),('ribs',[11819]),('guts',[11813]),('stump',[11818]),('bits',[11815,11817,11820])):
    sf=piece(ids,Tc,R,np.zeros(3)); info(nm,sf); md3.write(OUT+'/%s.md3'%nm,'models/vr_gore/%s.md3'%nm,sf)
# just the guts out of the belly: the lower half of the torn front (the upper half is the
# torn chest), moved forward and up onto the belly (in the map it hangs through the body)
GUTS_SHIFT=np.array([3.0,0,3.0])
def lower(ids,zmax):
    out=[]
    for sname,sh,vs,ts in piece(ids,Tc,R,GUTS_SHIFT):
        keep=[t for t in ts if np.mean([vs[i][0][2] for i in t])<zmax+GUTS_SHIFT[2]]
        used=sorted({i for t in keep for i in t}); remap={o:i for i,o in enumerate(used)}
        out.append((sname,sh,[vs[i] for i in used],[tuple(remap[i] for i in t) for t in keep]))
    return out
sf=lower([11813],-3.0); info('guts (lower)',sf); md3.write(OUT+'/guts.md3','models/vr_gore/guts.md3',sf)
# the skinned jaw (lower front of the skinned head), worn over a normal head: a little bigger
# so the head's own jaw doesn't show through
def jaw(zmax,xmin,scale):
    import struct as _s
    sf=[]
    C2,R2,mn2,mx2=frame([11814],(70,160,90,200),50,200,W=256)
    off=np.array([REFMIN[0]-1.25*mn2[0],0,REFMIN[2]-1.25*mn2[2]])
    for sname,sh,vs,ts in piece([11814],C2,R2*1.25,off):
        keep=[t for t in ts if max(vs[i][0][2] for i in t)<zmax and np.mean([vs[i][0][0] for i in t])>xmin]
        used=sorted({i for t in keep for i in t}); remap={o:i for i,o in enumerate(used)}
        vv=[(np.array(vs[i][0])*np.array([scale,scale,1.0])+np.array([0.3,0,0]),vs[i][1],vs[i][2]) for i in used]
        sf.append((sname,sh,vv,[tuple(remap[i] for i in t) for t in keep]))
    return sf
sf=jaw(2.2,0.0,1.08); info('jaw',sf); md3.write(OUT+'/jaw_skinned.md3','models/vr_gore/jaw_skinned.md3',sf)
# a flesh cap for stumps and the neck of a cut off head: the torn neck, its torn end along +x
S=[v for t in tris([11818]) for v in t]
P=np.array([v[:3] for v in S]); 
Rs=np.array([up,left,np.cross(up,left)])         # x out of the neck (up the spine), z across
c=P.mean(0); base=((P-c)@Rs.T)[:,0].min()
sf=piece([11818],c+base*up,Rs,np.zeros(3)); info('cap',sf); md3.write(OUT+'/flesh_cap.md3','models/vr_gore/flesh_cap.md3',sf)
# the jaw the other way round (the skinned side on the left)
sf=[(n,sh,[(np.array([v[0][0],-v[0][1],v[0][2]]),v[1],np.array([v[2][0],-v[2][1],v[2][2]])) for v in vs],[(t[0],t[2],t[1]) for t in ts]) for n,sh,vs,ts in jaw(2.2,0.0,1.08)]
md3.write(OUT+'/jaw_skinned2.md3','models/vr_gore/jaw_skinned2.md3',sf)
# a bullet hole: a quad facing +x, the hole at the origin and the blood running down -z
# (headhole.tga is 64x128, the hole a quarter of the way down)
# (just the hole: no blood running from it, which looked wrong on a head lying the other way up)
q=[(np.array([0,-1.0,1.0]),(0,0),np.array([1.0,0,0])),(np.array([0,1.0,1.0]),(1,0),np.array([1.0,0,0])),
   (np.array([0,1.0,-1.0]),(1,0.5),np.array([1.0,0,0])),(np.array([0,-1.0,-1.0]),(0,0.5),np.array([1.0,0,0]))]
md3.write(OUT+'/headhole.md3','models/vr_gore/headhole.md3',[('hole','vr_headhole',q,[(0,2,1),(0,3,2)])])

# a flat cut through the neck (neck_flat.tga from neck_tex.py): a disc 4.6 across facing +x,
# the front of the neck along +y
import math
N=24; R=2.3
disc=[((0,0,0),(0.5,0.5),(1,0,0))]+[((0,R*math.cos(2*math.pi*i/N),R*math.sin(2*math.pi*i/N)),(0.5+0.5*math.cos(2*math.pi*i/N),0.5-0.5*math.sin(2*math.pi*i/N)),(1,0,0)) for i in range(N)]
md3.write(OUT+'/neck_flat.md3','models/vr_gore/neck_flat.md3',[('neck','vr_neckflat',disc,[(0,1+(i+1)%N,1+i) for i in range(N)])])
