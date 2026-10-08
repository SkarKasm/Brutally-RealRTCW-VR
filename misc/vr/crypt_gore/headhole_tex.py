import sys,random
from PIL import Image, ImageDraw, ImageFilter
random.seed(7)
W,H=64,128; cx,cy=32,32
im=Image.new('RGBA',(W,H),(0,0,0,0))
blood=Image.new('RGBA',(W,H),(0,0,0,0)); d=ImageDraw.Draw(blood)
d.ellipse([cx-13,cy-12,cx+13,cy+12],fill=(40,4,3,110))
d.ellipse([cx-9,cy-8,cx+9,cy+8],fill=(48,5,4,190))
blood=blood.filter(ImageFilter.GaussianBlur(1.6))
im=Image.alpha_composite(im,blood); d=ImageDraw.Draw(im)
d.ellipse([cx-6,cy-6,cx+6,cy+6],fill=(32,4,3,255))
d.ellipse([cx-4,cy-4,cx+4,cy+4],fill=(8,1,1,255))
im.save(sys.argv[1])
