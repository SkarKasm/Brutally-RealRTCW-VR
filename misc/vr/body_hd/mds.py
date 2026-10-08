import struct,sys
def load(path):
    d=open(path,'rb').read()
    h=struct.unpack_from('<ii64sffiiiiiiiiii',d,0)
    ident,ver,name,ls,lb,nF,nB,oF,oB,tp,nS,oS,nT,oT,oE=h
    bones=[]
    for i in range(nB):
        nm,par,tw,pd,fl=struct.unpack_from('<64siffi',d,oB+i*80)
        bones.append((nm.split(b'\0')[0].decode(),par))
    surfs=[];o=oS
    for s in range(nS):
        sh=struct.unpack_from('<i64s64siiiiiiiiiii',d,o)
        _,sn,shd,si,ml,oh,nv,ov,nt,ot,ocm,nbr,obr,oe=sh
        verts=[];p=o+ov
        for v in range(nv):
            nx,ny,nz,u,t,nw,fp,fd=struct.unpack_from('<5fiif',d,p);p+=32
            ws=[]
            for w in range(nw):
                bi,bw,ox,oy,oz=struct.unpack_from('<if3f',d,p);p+=20;ws.append((bi,bw,(ox,oy,oz)))
            verts.append(((u,t),ws))
        tris=[struct.unpack_from('<3i',d,o+ot+12*k) for k in range(nt)]
        surfs.append((sn.split(b'\0')[0].decode(),shd.split(b'\0')[0].decode(),verts,tris))
        o+=oe
    return bones,surfs
if __name__=='__main__':
    b,s=load(sys.argv[1])
    print([x[0] for x in b])
    for sn,sh,v,t in s: print(sn,sh,len(v),len(t))
