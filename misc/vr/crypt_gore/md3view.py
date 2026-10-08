import sys,struct,math,numpy as np,os
sys.path.insert(0,os.path.dirname(__file__))
from render import render, tex
from PIL import Image
def readmd3(p,texdirs):
    d=open(p,'rb').read()
    h=struct.unpack_from('<4si64s9i',d,0); ns=h[6]; o=h[10]; tris=[]
    for _ in range(ns):
        sh=struct.unpack_from('<4s64s10i',d,o); nv,nt,ot,osh,ost,oxyz,end=sh[5],sh[6],sh[7],sh[8],sh[9],sh[10],sh[11]
        shader=struct.unpack_from('<64s',d,o+osh)[0].split(b'\0')[0].decode()
        t=None
        for td in texdirs:
            for ext in ('.jpg','.tga'):
                f=os.path.join(td,shader.split('/')[-1]+ext)
                if os.path.exists(f): t=np.asarray(Image.open(f).convert('RGB')).astype(np.float32); break
            if t is not None: break
        st=[struct.unpack_from('<2f',d,o+ost+8*i) for i in range(nv)]
        xyz=[np.array(struct.unpack_from('<3h',d,o+oxyz+8*i))/64 for i in range(nv)]
        for i in range(nt):
            a=struct.unpack_from('<3i',d,o+ot+12*i)
            tris.append(([xyz[j] for j in a],[st[j] for j in a],None,t))
        o+=end
    return tris
if __name__=='__main__':
    texdirs=sys.argv[1].split(','); outp=sys.argv[2]; specs=sys.argv[3:]
    tiles=[]
    for sp in specs:
        files,yaw,pitch=sp.split(':')
        tr=[t for f in files.split('+') for t in readmd3(f,texdirs)]
        tiles.append(render(tr,math.radians(float(yaw)),math.radians(float(pitch)),360,360))
    sheet=Image.new('RGB',(360*min(4,len(tiles)),360*((len(tiles)+3)//4)))
    for i,t in enumerate(tiles): sheet.paste(t,((i%4)*360,(i//4)*360))
    sheet.save(outp)
