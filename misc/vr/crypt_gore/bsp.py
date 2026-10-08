import struct,sys,collections
def load(p):
    d=open(p,'rb').read()
    magic,ver=struct.unpack_from('<4si',d,0)
    lumps=[struct.unpack_from('<ii',d,8+8*i) for i in range(17)]
    def L(i): o,l=lumps[i]; return d[o:o+l]
    sh=L(1); shaders=[sh[i:i+72][:64].split(b'\0')[0].decode() for i in range(0,len(sh),72)]
    vb=L(10); verts=[struct.unpack_from('<3f2f2f3f4B',vb,i) for i in range(0,len(vb),44)]
    ib=L(11); idx=struct.unpack('<%di'%(len(ib)//4),ib)
    sb=L(13); surfs=[struct.unpack_from('<12i12f2i',sb,i) for i in range(0,len(sb),104)]
    return ver,shaders,verts,idx,surfs,L(0)
if __name__=='__main__':
    for p in sys.argv[1:]:
        ver,shaders,verts,idx,surfs,ent=load(p)
        print(p,ver,len(shaders),len(surfs))
        c=collections.Counter()
        for s in surfs:
            n=shaders[s[0]]
            if 'bodyparts' in n or 'gibs' in n or 'corpse' in n:
                c[(n,s[2])]+=1
        for k,v in sorted(c.items()): print(' ',k,v)
