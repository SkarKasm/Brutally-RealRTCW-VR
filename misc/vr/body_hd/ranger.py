# a crisp Rangers diamond (gold border and lettering on navy) warped onto a texture's patch
import numpy as np, cv2
from PIL import Image, ImageDraw, ImageFont
W,H=1200,640
def patch():
    im=Image.new('RGB',(W,H),(0,0,0)); m=Image.new('L',(W,H),0)
    d=ImageDraw.Draw(im); dm=ImageDraw.Draw(m)
    outer=[(0,H/2),(W/2,0),(W,H/2),(W/2,H)]
    dm.polygon(outer,fill=255)
    d.polygon(outer,fill=(196,150,62))                     # gold border
    k=0.86
    inner=[(W/2-W/2*k,H/2),(W/2,H/2-H/2*k),(W/2+W/2*k,H/2),(W/2,H/2+H/2*k)]
    d.polygon(inner,fill=(28,34,86))                       # navy
    f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',150)
    t='RANGERS'
    tw=d.textlength(t,font=f)
    tmp=Image.new('L',(int(tw)+20,200),0); ImageDraw.Draw(tmp).text((10,10),t,font=f,fill=255)
    tmp=tmp.crop(tmp.getbbox()).resize((int(W*0.56),int(H*0.2)),Image.LANCZOS)
    gold=Image.new('RGB',tmp.size,(222,178,80))
    im.paste(gold,((W-tmp.size[0])//2,(H-tmp.size[1])//2),tmp)
    return np.asarray(im).astype(np.float32), np.asarray(m).astype(np.float32)/255
def apply(img_bgr, corners, mirror=True):
    """corners: left, top, right, bottom of the diamond in the texture"""
    P,M=patch()
    if mirror:
        P=P[:,::-1].copy(); M=M[:,::-1].copy()
    src=np.float32([(0,H/2),(W/2,0),(W,H/2),(W/2,H)])
    T=cv2.getPerspectiveTransform(src,np.float32(corners))
    h,w=img_bgr.shape[:2]
    Pw=cv2.warpPerspective(P[...,::-1].copy(),T,(w,h),flags=cv2.INTER_AREA)
    Mw=cv2.warpPerspective(M,T,(w,h),flags=cv2.INTER_LINEAR)
    Pw=cv2.GaussianBlur(Pw,(0,0),0.5); Mw=cv2.GaussianBlur(Mw,(0,0),0.7)
    img=img_bgr.astype(np.float32)
    lum=cv2.GaussianBlur(img.mean(2),(0,0),10)
    xs=[c[0] for c in corners]; ys=[c[1] for c in corners]
    ref=lum[int(min(ys)):int(max(ys)),int(min(xs)):int(max(xs))]
    shade=np.clip(lum/max(ref.mean(),1),0.6,1.3)
    a=Mw[...,None]
    out=img*(1-a)+Pw*shade[...,None]*0.8*a
    return np.clip(out,0,255).astype(np.uint8), Mw
