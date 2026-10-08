import sys,math,numpy as np
from PIL import Image
sys.path.insert(0,sys.argv[0].rsplit('/',1)[0])
from bsp import load
TEX=sys.argv[2] if len(sys.argv)>2 else "."
def tex(name):
    for ext in ('.jpg','.tga'):
        try: return np.asarray(Image.open(TEX+'/'+name.split('/')[-1]+ext).convert('RGB')).astype(np.float32)
        except FileNotFoundError: pass
def tris_for(bspfile,surfids):
    ver,shaders,verts,idx,surfs,ent=load(bspfile)
    out=[]
    for si in surfids:
        s=surfs[si]; t=tex(shaders[s[0]])
        for k in range(0,s[6],3):
            vv=[verts[s[3]+idx[s[5]+k+j]] for j in range(3)]
            out.append(([v[:3] for v in vv],[v[3:5] for v in vv],[v[7:10] for v in vv],t))
    return out
def render(tris,yaw,pitch,W=520,H=520):
    P=np.array([p for t in tris for p in t[0]]); c=(P.min(0)+P.max(0))/2; r=np.linalg.norm(P-c,axis=1).max()
    cy,sy,cp,sp=math.cos(yaw),math.sin(yaw),math.cos(pitch),math.sin(pitch)
    def xf(p):
        x,y,z=np.array(p)-c
        x,y=x*cy-y*sy,x*sy+y*cy
        y,z=y*cp-z*sp,y*sp+z*cp   # y depth
        return np.array([W/2+x/r*W*0.45,H/2-z/r*H*0.45,y])
    img=np.full((H,W,3),40,np.float32); zb=np.full((H,W),1e9)
    L=np.array([0.3,-0.5,0.8]);L/=np.linalg.norm(L)
    for ps,st,ns,t in tris:
        q=[xf(p) for p in ps]
        n=np.cross(np.array(ps[1])-ps[0],np.array(ps[2])-ps[0]); nl=np.linalg.norm(n)
        if nl==0: continue
        shade=0.45+0.55*abs(np.dot(n/nl,L))
        xs=[a[0] for a in q]; ys=[a[1] for a in q]
        x0,x1=max(int(min(xs)),0),min(int(max(xs))+1,W-1); y0,y1=max(int(min(ys)),0),min(int(max(ys))+1,H-1)
        if x0>x1 or y0>y1: continue
        X,Y=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
        (ax,ay,az),(bx,by,bz),(cx_,cy_,cz)=q
        den=(by-cy_)*(ax-cx_)+(cx_-bx)*(ay-cy_)
        if abs(den)<1e-9: continue
        w0=((by-cy_)*(X-cx_)+(cx_-bx)*(Y-cy_))/den; w1=((cy_-ay)*(X-cx_)+(ax-cx_)*(Y-cy_))/den; w2=1-w0-w1
        m=(w0>=0)&(w1>=0)&(w2>=0)
        if not m.any(): continue
        z=w0*az+w1*bz+w2*cz
        sub=zb[y0:y1+1,x0:x1+1]; m&=z<sub
        u=w0*st[0][0]+w1*st[1][0]+w2*st[2][0]; v=w0*st[0][1]+w1*st[1][1]+w2*st[2][1]
        th,tw=t.shape[:2]
        col=t[(np.mod(v,1)*th).astype(int)%th,(np.mod(u,1)*tw).astype(int)%tw]*shade
        sub[m]=z[m]; img[y0:y1+1,x0:x1+1][m]=col[m]
    return Image.fromarray(np.clip(img,0,255).astype(np.uint8))
if __name__=='__main__':
    bspf=sys.argv[1]; groups=eval(sys.argv[3]); outp=sys.argv[4]
    tiles=[]
    for g in groups:
        tr=tris_for(bspf,g)
        for yaw,pitch in ((0.6,-1.0),(2.4,-0.6)):
            tiles.append(render(tr,yaw,pitch))
    W=520; sheet=Image.new('RGB',(W*2,W*len(groups)))
    for i,t in enumerate(tiles): sheet.paste(t,((i%2)*W,(i//2)*W))
    sheet.save(outp)
