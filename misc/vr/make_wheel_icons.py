# RealRTCW-VR: the VR weapon wheel's item icons (cg_draw.c): the binoculars, from their own
# texture, and red / blue folders with an up / down arrow for quick save / quick load. Each
# also as _select with a white outline, like the game's weapon icons.
#
#     make_wheel_icons.py <RealRTCW main folder> <out folder>   (writes gfx/vr/wheel_*.tga)
import io, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_vr_assets as M
out = sys.argv[2]
os.makedirs(os.path.join(out, "gfx/vr"), exist_ok=True)
tex = Image.open(io.BytesIO(M.Paks(sys.argv[1]).read("models/powerups/holdable/binoc.jpg"))).convert("RGB")
leather = tex.crop((10, 100, 160, 400))
metal = tex.crop((15, 5, 160, 75))
lens = tex.crop((190, 5, 325, 140))
eye = tex.crop((330, 100, 500, 490))
SS = 4

def outline(im, width=2):
    a = im.split()[3].point(lambda v: 255 if v > 40 else 0)
    big = a.filter(ImageFilter.MaxFilter(width * 2 + 1))
    ring = ImageChops.subtract(big, a)
    o = Image.new("RGBA", im.size, (255, 255, 255, 0)); o.putalpha(ring)
    res = Image.new("RGBA", im.size, (0, 0, 0, 0)); res.alpha_composite(o); res.alpha_composite(im)
    return res

def binocs():
    W, H = 128 * SS, 64 * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    def tube(x0, y0, x1, y1, src):
        t = src.resize((x1 - x0, y1 - y0)).convert("RGBA")
        m = Image.new("L", t.size, 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, t.size[0] - 1, t.size[1] - 1), radius=10 * SS, fill=255)
        t.putalpha(m); im.alpha_composite(t, (x0, y0))
    # seen from above: two barrels side by side, the eyepieces at the left, the wide
    # objectives at the right, the hinge between
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((26 * SS, H // 2 - 4 * SS, 100 * SS, H // 2 + 4 * SS), radius=2 * SS, fill=(70, 66, 60, 255))
    for dy in (-15, 15):
        cy = H // 2 + dy * SS
        tube(8 * SS, cy - 7 * SS, 30 * SS, cy + 7 * SS, eye)              # eyepiece
        tube(26 * SS, cy - 11 * SS, 84 * SS, cy + 11 * SS, leather)       # leather body
        tube(80 * SS, cy - 14 * SS, 114 * SS, cy + 14 * SS, metal)        # objective bell
        tube(110 * SS, cy - 12 * SS, 120 * SS, cy + 12 * SS, lens)        # lens rim
    d = ImageDraw.Draw(im)
    d.ellipse((54 * SS, H // 2 - 7 * SS, 68 * SS, H // 2 + 7 * SS), fill=(95, 90, 82, 255))   # focus wheel
    im = im.resize((128, 64), Image.LANCZOS)
    # darken like the weapon icons
    r, g, b, a = im.split()
    im = Image.merge("RGBA", [c.point(lambda v: int(min(255, v * 1.15))) for c in (r, g, b)] + [a])
    return im

def folder(color, up):
    W = H = 64 * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    dark = tuple(int(c * 0.6) for c in color)
    # folder (top part), tab behind
    d.rounded_rectangle((8 * SS, 6 * SS, 30 * SS, 16 * SS), radius=3 * SS, fill=dark + (255,))
    d.rounded_rectangle((6 * SS, 10 * SS, 58 * SS, 38 * SS), radius=4 * SS, fill=dark + (255,))
    d.rounded_rectangle((6 * SS, 15 * SS, 58 * SS, 38 * SS), radius=4 * SS, fill=color + (255,))
    d.line((10 * SS, 19 * SS, 54 * SS, 19 * SS), fill=tuple(min(255, c + 70) for c in color) + (255,), width=2 * SS)
    # arrow under it
    cx = 32 * SS
    if up:
        d.polygon([(cx, 40 * SS), (cx - 12 * SS, 51 * SS), (cx + 12 * SS, 51 * SS)], fill=color + (255,))
        d.rectangle((cx - 5 * SS, 50 * SS, cx + 5 * SS, 61 * SS), fill=color + (255,))
    else:
        d.rectangle((cx - 5 * SS, 40 * SS, cx + 5 * SS, 51 * SS), fill=color + (255,))
        d.polygon([(cx, 62 * SS), (cx - 12 * SS, 50 * SS), (cx + 12 * SS, 50 * SS)], fill=color + (255,))
    return im.resize((64, 64), Image.LANCZOS)

icons = {"wheel_binocs": binocs(), "wheel_qsave": folder((200, 40, 35), True), "wheel_qload": folder((40, 90, 210), False)}
for n, im in icons.items():
    im.save("%s/gfx/vr/%s.tga" % (out, n))
    outline(im).save("%s/gfx/vr/%s_select.tga" % (out, n))
