import sys,numpy as np
sys.path.insert(0,'tools')
from bsp import load
ver,shaders,verts,idx,surfs,ent=load('/mnt/user-data/uploads/RealRTCW/game/extracted/maps/crypt1.bsp')
def tris(ids):
    T=[]
    for si in ids:
        s=surfs[si]
        for k in range(0,s[6],3):
            T.append([verts[s[3]+idx[s[5]+k+j]] for j in range(3)])
    return T
def frame(ids,facebox,topv,chinv,W=512):
    T=tris(ids)
    P=np.array([v[:3] for t in T for v in t]); UV=np.array([v[3:5] for t in T for v in t])*W
    fw=np.zeros(3)
    for t in T:
        uv=np.mean([v[3:5] for v in t],0)*W
        if facebox[0]<uv[0]<facebox[1] and facebox[2]<uv[1]<facebox[3]:
            a,b,c=[np.array(v[:3]) for v in t]; fw+=np.cross(b-a,c-a)
    # winding: Q3 front faces are clockwise -> normal may be inward; check against centroid
    C=P.mean(0); fw/=np.linalg.norm(fw)
    facepts=P[(UV[:,0]>facebox[0])&(UV[:,0]<facebox[1])&(UV[:,1]>facebox[2])&(UV[:,1]<facebox[3])]
    if np.dot(facepts.mean(0)-C,fw)<0: fw=-fw
    top=P[UV[:,1]<topv].mean(0); chin=P[UV[:,1]>chinv].mean(0)
    up=top-chin; up-=np.dot(up,fw)*fw; up/=np.linalg.norm(up)
    left=np.cross(up,fw)
    R=np.array([fw,left,up])
    L=(P-C)@R.T
    return C,R,L.min(0),L.max(0)
if __name__=='__main__':
    C,R,mn,mx=frame([11784,11785,11786],(110,210,290,420),140,440)
    print('deadnazi3',C,R,mn,mx)
