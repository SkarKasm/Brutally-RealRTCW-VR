"""Darken the Soldier of Fortune 2 wound decals (sof2_wound*.tga, from SoF2's gil_bullet_hole*
png) toward RTCW's darker, browner blood: 0.62x brightness, a little less saturated."""
import sys
from PIL import Image
import colorsys
SRC = '/tmp/claude-0/sof2x/models/characters/gore/'
names = ['gil_bullet_holef', 'gil_bullet_holee_128', 'gil_bullet_holeg', 'gil_bullet_holeh']
for i, n in enumerate(names):
    im = Image.open(SRC + n + '.png').convert('RGBA')
    if im.width > 128:
        im = im.resize((128, 128), Image.LANCZOS)
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            r2, g2, b2 = colorsys.hsv_to_rgb(h, s * 0.85, v * 0.62)
            px[x, y] = (int(r2 * 255), int(g2 * 255), int(b2 * 255), a)
    im.save('sof2_wound%d.tga' % (i + 1))
