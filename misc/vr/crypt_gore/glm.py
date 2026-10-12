"""Reads Ghoul 2 GLM (mdxm, version 6) models, as Soldier of Fortune 2 (Raven Software) ships them:
the surfaces of one LOD with their base pose verts, normals, texture coordinates, triangles
and bone weights.  python3 glm.py <file.glm> lists the surfaces."""
import struct,sys
def read(path, lod=0):
    d=open(path,'rb').read()
    ident,ver=struct.unpack('<4si',d[:8]); assert ident==b'2LGM',ident
    name=d[8:72]; anim=d[72:136]
    animIndex,numBones,numLODs,ofsLODs,numSurfaces,ofsSurfH,ofsEnd=struct.unpack('<7i',d[136:164])
    hier=[]
    offs=struct.unpack("<%di"%numSurfaces,d[164:164+4*numSurfaces])
    for i in range(numSurfaces):
        o=164+offs[i]
        sname=d[o:o+64].split(b'\0')[0].decode('latin1'); flags,=struct.unpack('<I',d[o+64:o+68])
        shader=d[o+68:o+132].split(b'\0')[0].decode('latin1')
        shIdx,parent,nch=struct.unpack('<3i',d[o+132:o+144])
        hier.append(dict(name=sname,flags=flags,shader=shader,parent=parent))
    o=ofsLODs
    for l in range(lod): o+=struct.unpack('<i',d[o:o+4])[0]
    base=o+4
    soffs=struct.unpack('<%di'%numSurfaces,d[base:base+4*numSurfaces])
    surfs=[]
    for i in range(numSurfaces):
        so=base+soffs[i]
        ident2,idx,ofsH,nv,ofsV,nt,ofsT,nb,ofsB,oE=struct.unpack('<10i',d[so:so+40])
        bonerefs=struct.unpack('<%di'%nb,d[so+ofsB:so+ofsB+4*nb])
        vs=[]
        for j in range(nv):
            vo=so+ofsV+j*32
            n=struct.unpack('<3f',d[vo:vo+12]); p=struct.unpack('<3f',d[vo+12:vo+24])
            packed,=struct.unpack('<I',d[vo+24:vo+28]); w4=d[vo+28:vo+32]
            nw=(packed>>30)+1
            ws=[]; tot=0
            for k in range(nw):
                bi=(packed>>(5*k))&31
                wv=w4[k] | (((packed>>(20+2*k))&3)<<8)
                wf=wv/1023.0
                if k==nw-1: wf=1.0-tot
                tot+=wf; ws.append((bonerefs[bi] if bi<len(bonerefs) else -1,wf))
            vs.append(dict(p=p,n=n,w=ws))
        to=so+ofsV+nv*32
        for j in range(nv):
            vs[j]['st']=struct.unpack('<2f',d[to+j*8:to+j*8+8])
        tris=[struct.unpack('<3i',d[so+ofsT+k*12:so+ofsT+k*12+12]) for k in range(nt)]
        surfs.append(dict(hier[i],verts=vs,tris=tris,index=idx))
    return dict(numBones=numBones,numLODs=numLODs,surfs=surfs,anim=anim.split(b'\0')[0].decode())
if __name__=='__main__':
    m=read(sys.argv[1])
    print(m['anim'],m['numBones'],m['numLODs'])
    for s in m['surfs']:
        print('%-40s %-45s fl=%x v=%d t=%d'%(s['name'],s['shader'],s['flags'],len(s['verts']),len(s['tris'])))
