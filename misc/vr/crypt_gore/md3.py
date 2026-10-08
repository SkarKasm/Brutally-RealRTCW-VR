import struct,math,numpy as np
def enc_normal(n):
    x,y,z=n
    if x==0 and y==0:
        return (0,0) if z>0 else (128,0)
    lng=int(math.atan2(y,x)*255/(2*math.pi))&0xff
    lat=int(math.acos(max(-1,min(1,z)))*255/(2*math.pi))&0xff
    return (lat,lng)
def write(path,name,surfaces,tags=()):
    """surfaces: list of (surfname, shader, verts[(xyz,st,normal)], tris[(a,b,c)])"""
    allp=np.array([v[0] for s in surfaces for v in s[2]])
    mn,mx=allp.min(0),allp.max(0); rad=float(np.linalg.norm(allp,axis=1).max())
    frame=struct.pack('<10f16s',*mn,*mx,0,0,0,rad,b'frame0')
    tagb=b''.join(struct.pack('<64s12f',t[0].encode(),*t[1],*np.array(t[2]).ravel()) for t in tags)
    surfb=b''
    for sname,shader,vs,ts in surfaces:
        h=108; ofsTri=h; ofsSh=ofsTri+12*len(ts); ofsSt=ofsSh+68; ofsXyz=ofsSt+8*len(vs); end=ofsXyz+8*len(vs)
        b=struct.pack('<4s64s10i',b'IDP3',sname.encode(),0,1,1,len(vs),len(ts),ofsTri,ofsSh,ofsSt,ofsXyz,end)
        b+=b''.join(struct.pack('<3i',*t) for t in ts)
        b+=struct.pack('<64si',shader.encode(),0)
        b+=b''.join(struct.pack('<2f',*v[1]) for v in vs)
        for v in vs:
            la,lo=enc_normal(v[2])
            b+=struct.pack('<3h2B',*[int(round(c*64)) for c in v[0]],la,lo)
        surfb+=b
    ofsFrames=108; ofsTags=ofsFrames+56; ofsSurf=ofsTags+len(tagb); end=ofsSurf+len(surfb)
    hdr=struct.pack('<4si64s9i',b'IDP3',15,name.encode(),0,1,len(tags),len(surfaces),0,ofsFrames,ofsTags,ofsSurf,end)
    open(path,'wb').write(hdr+frame+tagb+surfb)
