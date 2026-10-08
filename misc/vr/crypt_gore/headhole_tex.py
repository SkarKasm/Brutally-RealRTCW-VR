import sys,random
from PIL import Image, ImageDraw, ImageFilter
random.seed(7)
W,H=64,128; cx,cy=32,32
im=Image.new('RGBA',(W,H),(0,0,0,0))
blood=Image.new('RGBA',(W,H),(0,0,0,0)); d=ImageDraw.Draw(blood)
for x0,l,w,a in ((cx-1,88,5,210),(cx+5,46,3,170),(cx-6,30,2,150)):
    x,y=x0,cy
    for i in range(l):
        d.ellipse([x-w/2,y-w/2,x+w/2,y+w/2],fill=(85,5,4,a)); y+=1; x+=random.choice((-0.4,0,0,0.4))
        if i>l*0.7: w=max(1.2,w*0.985)
d.ellipse([cx-15,cy-14,cx+15,cy+14],fill=(90,10,8,120))
d.ellipse([cx-10,cy-9,cx+10,cy+9],fill=(105,12,10,190))
blood=blood.filter(ImageFilter.GaussianBlur(1.6))
im=Image.alpha_composite(im,blood); d=ImageDraw.Draw(im)
d.ellipse([cx-6,cy-6,cx+6,cy+6],fill=(55,6,5,255))
d.ellipse([cx-4,cy-4,cx+4,cy+4],fill=(8,1,1,255))
im.save(sys.argv[1])
