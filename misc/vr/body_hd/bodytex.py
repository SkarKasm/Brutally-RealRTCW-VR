import sys; sys.path.insert(0,'.')
import numpy as np, cv2
from mds import load
from PIL import Image, ImageDraw
BJ='/tmp/claude-0/bj/'; N=2048
base=cv2.cvtColor(cv2.imread(BJ+'body_default_x4.png'),cv2.COLOR_BGR2RGB).astype(np.float32)
bones,surfs=load(BJ+'models/players/player/body.mds')
sn,sh,verts,tris=surfs[0]
names=[b[0] for b in bones]
m=Image.new('L',(N,N)); d=ImageDraw.Draw(m)
for t in tris:
    uv=[verts[i][0] for i in t]
    cu=sum(p[0] for p in uv)/3; cv=sum(p[1] for p in uv)/3
    cls=[names[max(verts[i][1],key=lambda w:w[1])[0]] for i in t]
    if cu>0.63 and cv<0.667 and not any('Hand' in c or 'Finger' in c for c in cls):
        d.polygon([(p[0]*N,p[1]*N) for p in uv],fill=255)
arm=np.asarray(m).astype(np.float32)/255
arm=cv2.dilate(arm,np.ones((7,7),np.uint8)); arm=cv2.GaussianBlur(arm,(0,0),2)
arm[int(0.667*N):]=0
# patches to keep from BJ's own sleeves (1024 space boxes)
pm=Image.new('L',(N,N)); pd=ImageDraw.Draw(pm)
s=N/1024
pd.rectangle([803*s,146*s,890*s,212*s],fill=255)                     # flag
pd.polygon([(845*s,381*s),(880*s,410*s),(845*s,438*s),(810*s,410*s)],fill=255)  # rangers diamond
patch=np.asarray(pm).astype(np.float32)/255
lum=base.mean(2)
cb=np.zeros_like(lum); cb[int(465*s):int(535*s),int(820*s):int(875*s)]=1    # chevron lines
loc=cv2.GaussianBlur(lum,(0,0),6)
patch=np.maximum(patch,cb*np.clip((loc-lum-4)/10,0,1))
patch=cv2.GaussianBlur(patch,(0,0),1.2)
blo=cv2.GaussianBlur(lum,(0,0),18)          # body folds and shading, low frequency
panels=[(650,8,992,292),(658,296,990,684)]
def sleeve(prof):
    S=cv2.cvtColor(cv2.imread(BJ+f'models/weapons/hands/sleeve_{prof}.jpg'),cv2.COLOR_BGR2RGB).astype(np.float32)
    S=S[70:980,10:1014]
    return S
def make(prof,ref):
    S=sleeve(prof); out=base.copy()
    k=58.0/36.0*(0.8 if prof=='snow' else 0.9)                                   # FP sleeves are lit darker than world models
    for (x0,y0,x1,y1) in panels:
        X0,Y0,X1,Y1=[int(v*s) for v in (x0,y0,x1,y1)]
        T=cv2.resize(S,(X1-X0,Y1-Y0),interpolation=cv2.INTER_CUBIC)
        Tl=T.mean(2)+1
        big=cv2.GaussianBlur(Tl,(0,0),60)         # baked first person lighting
        alb=cv2.GaussianBlur(T,(0,0),1.0)/big[...,None]*k*Tl.mean()/1.0
        det=np.clip(Tl/cv2.GaussianBlur(Tl,(0,0),4),0.6,1.5)
        shade=blo[Y0:Y1,X0:X1]/blo[Y0:Y1,X0:X1][arm[Y0:Y1,X0:X1]>0.5].mean()
        new=alb*det[...,None]*np.clip(shade,0.35,1.4)[...,None]
        a=arm[Y0:Y1,X0:X1,None]*(1-patch[Y0:Y1,X0:X1,None])
        out[Y0:Y1,X0:X1]=out[Y0:Y1,X0:X1]*(1-a)+new*a
    # the leather cuff above the hand in the glove's colour
    if prof!='default':
        Gl=cv2.cvtColor(cv2.imread(BJ+f'models/weapons/hands/glove_{prof}.jpg'),cv2.COLOR_BGR2RGB).astype(np.float32)
        Gd=cv2.cvtColor(cv2.imread(BJ+'models/weapons/hands/glove_default.jpg'),cv2.COLOR_BGR2RGB).astype(np.float32)
        mk=Gd.mean(2)>6
        ratio=Gl[mk].mean(0)/Gd[mk].mean(0)
        Y0,Y1,X0,X1=int(683*s),int(832*s),int(655*s),int(1005*s)
        c=out[Y0:Y1,X0:X1]
        l=c.mean(2,keepdims=True)
        tint=Gl[mk].mean(0); tint=tint/tint.mean(); tint=0.6*tint+0.4
        out[Y0:Y1,X0:X1]=l*tint*min(ratio.mean(),2.0)
    # fine fabric grain on the rest of the uniform
    G=cv2.cvtColor(cv2.imread(BJ+'models/weapons/hands/sleeve_default.jpg'),cv2.COLOR_BGR2RGB).astype(np.float32).mean(2)[70:980,10:1014]+1
    g=np.clip(G/cv2.GaussianBlur(G,(0,0),4),0.7,1.35)
    g=cv2.resize(g,(N//2,N//2)); g=np.tile(g,(2,2))
    rest=(1-arm)[...,None]*0.5
    out=out*(1+(g[...,None]-1)*rest)
    return np.clip(out,0,255).astype(np.uint8)
for prof in ['default','green','snow']:
    o=make(prof,None)
    Image.fromarray(o).save(BJ+f'body_vr_{prof}.jpg',quality=92)
b=cv2.imread(BJ+'boot_default_x4.png'); cv2.imwrite(BJ+'boot_vr.jpg',b,[cv2.IMWRITE_JPEG_QUALITY,92])
Image.fromarray((arm*255).astype(np.uint8)).save(BJ+'armmask.png')
