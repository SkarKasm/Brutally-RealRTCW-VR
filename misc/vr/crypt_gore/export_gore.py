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
# torso frame: the torn torso lies face up, its head (bphead) beyond the neck
T=np.array([v[:3] for t in tris([11812]) for v in t]); Tc=(T.min(0)+T.max(0))/2
H=np.array([v[:3] for t in tris([11814]) for v in t]).mean(0)
fw=np.array([0,0,1.0]); up=H-Tc; up-=np.dot(up,fw)*fw; up/=np.linalg.norm(up); left=np.cross(up,fw)
R=np.array([fw,left,up])
for nm,ids in (('torso',[11812]),('ribs',[11819]),('guts',[11813]),('stump',[11818]),('bits',[11815,11817,11820])):
    sf=piece(ids,Tc,R,np.zeros(3)); info(nm,sf); md3.write(OUT+'/%s.md3'%nm,'models/vr_gore/%s.md3'%nm,sf)
