import struct, re, numpy as np
G='/tmp/claude-0/sof2skel/skeletons/average_sleeves'
d=open(G+'.gla','rb').read()
name=d[8:72].split(b'\0')[0]
scale,nF,ofsF,nB,ofsPool,ofsSkel,ofsEnd=struct.unpack('<f6i',d[72:100])
offs=struct.unpack('<%di'%nB,d[100:100+4*nB])
bones=[]
for o in offs:
    p=100+o
    nm=d[p:p+64].split(b'\0')[0].decode(); flags,parent=struct.unpack('<2i',d[p+64:p+72])
    base=np.array(struct.unpack('<12f',d[p+72:p+120])).reshape(3,4)
    bones.append(dict(name=nm,parent=parent,base=base))
def mat4(m):
    M=np.eye(4); M[:3,:]=m; return M
def uncomp(c):
    w,x,y,z,tx,ty,tz=struct.unpack('<7H',c)
    w,x,y,z=[v/16383.0-2 for v in (w,x,y,z)]
    X,Y,Z=2*x,2*y,2*z
    m=np.array([[1-(Y*y+Z*z), X*y-Z*w, X*z+Y*w, tx/64-512],
                [X*y+Z*w, 1-(X*x+Z*z), Y*z-X*w, ty/64-512],
                [X*z-Y*w, Y*z+X*w, 1-(X*x+Y*y), tz/64-512]])
    return m
def frame(f):
    L=[]
    for b in range(nB):
        p=ofsF+(f*nB+b)*3
        i=d[p]|(d[p+1]<<8)|(d[p+2]<<16)
        L.append(mat4(uncomp(d[ofsPool+i*14:ofsPool+i*14+14])))
    W=[None]*nB
    def get(b):
        if W[b] is None:
            par=bones[b]['parent']
            W[b]= L[b] if par<0 else get(par)@L[b]
        return W[b]
    for b in range(nB): get(b)
    return [W[b]@mat4(bones[b]['base']) for b in range(nB)]
anims={}
fr=open(G+'.frames').read()
for m in re.finditer(r'(\S+\.xsi)\s*\{\s*"startframe"\s*"(\d+)"\s*"duration"\s*"(\d+)"\s*"fps"\s*"(\d+)"',fr):
    anims[m.group(1).split('/')[-1][:-4]]=(int(m.group(2)),int(m.group(3)),int(m.group(4)))
