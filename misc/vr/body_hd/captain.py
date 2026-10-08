# swap BJ's private chevron for captain's bars on a body texture
import sys, os, numpy as np, cv2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
def bars_on(img, poly, cx, cy, w=18, h=56, gap=28):
    img=img.astype(np.float32)
    m=np.zeros(img.shape[:2],np.uint8); cv2.fillPoly(m,[np.array(poly,np.int32)],255)
    m=cv2.dilate(m,np.ones((9,9),np.uint8))
    img=cv2.inpaint(np.clip(img,0,255).astype(np.uint8),m,12,cv2.INPAINT_TELEA).astype(np.float32)
    for bx in (cx-gap//2, cx+gap//2):
        x0,x1,y0,y1=bx-w//2,bx+w//2,cy-h//2,cy+h//2
        sh=np.zeros(img.shape[:2],np.float32); sh[y0+3:y1+3,x0+3:x1+3]=1
        sh=cv2.GaussianBlur(sh,(0,0),2); img*=(1-0.45*sh[...,None])
        for y in range(y0,y1):
            for x in range(x0,x1):
                t=(x-x0)/(w-1); v=(y-y0)/(h-1)
                edge=min(x-x0,x1-1-x,y-y0,y1-1-y)
                g=100+55*np.cos((t-0.3)*np.pi)*0.6+15*(1-v)
                if edge<2: g=45
                elif edge<4: g*=1.12
                img[y,x]=(min(255,g*1.04),g,g)   # BGR
    return np.clip(img,0,255).astype(np.uint8)
if __name__=='__main__':
    src,dst=sys.argv[1],sys.argv[2]
    im=cv2.imread(src)
    out=bars_on(im,[(1698,1293),(1630,1375),(1634,1404),(1698,1352),(1770,1402),(1770,1373)],1700,1352)
    import ranger
    out,_=ranger.apply(out,[(1625,1163),(1707,1120),(1788,1172),(1706,1217)])
    cv2.imwrite(dst,out,[cv2.IMWRITE_JPEG_QUALITY,93])
