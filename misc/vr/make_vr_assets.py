"""
RealRTCW-VR co-op asset builder.

Builds coopmain/z_vr_assets.pk3 from the player's own game files (the art can't be
redistributed, so it is generated locally by deploy.sh). Co-op uses the original game's
weapons, which have no .weap files: every first person weapon is the folder's
v_<name>_hand model (tags only) carrying v_<name> on tag_weapon and v_<name>_barrel[N]
on tag_barrel[N].

  models/vr/hand_r.md3     right hand + sleeve (from the Luger's grip hand)
  models/vr/hand_l.md3     the same, mirrored
  vr/weapons.cfg           grip / magazine / slide geometry per first person model
  ui/vr_options.menu       VR options page, plus a VR OPTIONS button on the main menu

usage: python make_vr_assets.py <output pk3> <pak dir> [<pak dir> ...]   (lowest priority first)
"""

import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vr_assets_common as V    # noqa: E402  (the single player builder's model code)

# co-op's original weapons: grips moved along the gun (game units, + forward)
GRIP_SHIFT = {
    "models/weapons2/mauser/v_mauser": -7.0,
    "models/weapons2/garand/v_garand": -10.0,
}
MELEE = ("models/weapons2/knife/v_knife",)

# reload recordings of the weapons reloaded by hand, keyed like cg_vr.c looks them up
# (vr/sound/w<weapon number>_<out|in|rack>.wav); weapon numbers from bg_public.h
RELOAD_SOUNDS = {
    2: "sound/weapons/luger/luger_reload.wav",          # WP_LUGER
    3: "sound/weapons/mp40/mp40_reload.wav",            # WP_MP40
    4: "sound/weapons/mauser/mauser_reload.wav",        # WP_MAUSER
    5: "sound/weapons/fg42/fg42_reload.wav",            # WP_FG42
    11: "sound/weapons/colt/colt_reload.wav",           # WP_COLT
    12: "sound/weapons/thompson/thompson_reload.wav",   # WP_THOMPSON
    13: "sound/weapons/garand/garand_reload.wav",       # WP_GARAND
    15: "sound/weapons/mauser/sniper_reload.wav",       # WP_SNIPERRIFLE
    16: "sound/weapons/garand/snooper_reload.wav",      # WP_SNOOPERSCOPE
    17: "sound/weapons/fg42/fg42_reload.wav",           # WP_FG42SCOPE
    18: "sound/weapons/sten/sten_reload.wav",           # WP_STEN
    19: "sound/weapons/luger/luger_reload.wav",         # WP_SILENCER
}


class Paks(V.Paks):
    """Several game directories, in the game's search order (later ones win)."""

    def __init__(self, dirs):
        self.zips = []
        self.index = {}
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for name in sorted(os.listdir(d), key=str.lower):
                if not name.lower().endswith(".pk3") or name.lower() == "z_vr_assets.pk3":
                    continue
                try:
                    z = zipfile.ZipFile(os.path.join(d, name))
                except zipfile.BadZipFile:
                    continue
                self.zips.append(z)
                for n in z.namelist():
                    self.index[n.lower()] = (z, n)


def weapon_infos(paks):
    """{model key: info} for every first person weapon (key: v_ model path, no extension)."""
    out = {}
    for name in sorted(paks.names()):
        m = re.match(r"^(models/weapons2/[^/]+)/v_([^/]+)_hand\.(md3|mdc)$", name)
        if not m:
            continue
        folder, base = m.group(1), m.group(2)
        key = "%s/v_%s" % (folder, base)
        if not paks.read(key + ".md3"):
            continue
        parts = []
        for j in range(8):
            suffix = "_barrel" if j == 0 else "_barrel%d" % (j + 1)
            path = key + suffix + ".md3"
            if paks.read(path):
                parts.append((j, "tag_barrel" if j == 0 else "tag_barrel%d" % (j + 1), path))
        out[key] = {"hands": key + "_hand.md3", "model": key + ".md3", "parts": parts,
                    "config": folder + "/weapon.cfg"}
    return out


def mirrored(surfaces):
    """Left hand from the right: mirror across the forward/up plane, flip the winding."""
    out = []
    for s in surfaces:
        verts = [([p[0], -p[1], p[2]], [n[0], -n[1], n[2]]) for p, n in s.verts]
        tris = [(t[0], t[2], t[1]) for t in s.tris]
        out.append(V.Surface(s.name, s.shader, tris, s.st, verts))
    return out


VR_MENU_BUTTON = """
		// VR OPTIONS BUTTON (RealRTCW-VR, added by make_vr_assets.py)
		itemDef
		{
			name vroptionsmenu
			rect 392 378 72 14
			style 0
			type 1
			text "VR OPTIONS"
			textStyle 6
			textAlign ITEM_ALIGN_CENTER
			textScale .25
			textAlignX 36
			textAlignY 14
			textFont UI_FONT_NORMAL
			foreColor .9 .9 .9 1
			visible 1
			action { close main; open vr_options }
		}
"""


# plain co-op's RealRTCW style main menu: under its QUIT button
VR_MENU_BUTTON_COOP = """
	// VR OPTIONS BUTTON (RealRTCW-VR, added by make_vr_assets.py)
	itemDef {
		name vroptionsmenu
		text "VR OPTIONS"
		group mainmenus
		type 1
		rect 10 309 178 26
		visible 1
		textfont UI_FONT_NORMAL
		textscale .3
		textalign ITEM_ALIGN_LEFT
		textalignx 6
		textaligny 19
		forecolor 1 1 1 1
		action { close main; open vr_options }
	}
"""


def vr_menus(paks):
    here = os.path.dirname(os.path.abspath(__file__))
    out = {}
    with open(os.path.join(here, "ui", "vr_options.menu"), "rb") as f:
        out["ui/vr_options.menu"] = f.read()

    menus = paks.read("ui/menus.txt")
    main = paks.read("ui/main.menu")
    if not menus or not main:
        print("warning: menus not found, no VR OPTIONS button")
        return out
    menus = menus.decode(errors="replace")
    if "vr_options.menu" not in menus:
        menus = re.sub(r'(loadMenu\s*\{\s*"ui/main\.menu"\s*\})',
                       r'\1\n\tloadMenu { "ui/vr_options.menu" }\t// RealRTCW-VR', menus, count=1)
    out["ui/menus.txt"] = menus.encode()

    main = main.decode(errors="replace")
    m = re.search(r'itemDef\s*\{\s*name\s+"?fadeBox"?', main)
    q = re.search(r'mouseExit\s*\{\s*hide\s+"realrtcw_main_quit"\s*\}\s*\}', main)
    if "vroptionsmenu" in main:
        pass
    elif m:     # Cursed Sands
        main = main[:m.start()] + VR_MENU_BUTTON.strip() + "\n\t\t" + main[m.start():]
        out["ui/main.menu"] = main.encode()
    elif q:     # plain co-op
        main = main[:q.end()] + VR_MENU_BUTTON_COOP + main[q.end():]
        out["ui/main.menu"] = main.encode()
    else:
        print("warning: couldn't place the VR OPTIONS button in ui/main.menu")
    return out


def main():
    out_pk3, dirs = sys.argv[1], sys.argv[2:]
    paks = Paks(dirs)

    grips = []
    for key, info in sorted(weapon_infos(paks).items()):
        try:
            pieces = V.pose_weapon(paks, info)
        except (ValueError, V.struct.error, IndexError) as e:
            print("skipping", key, e)
            continue
        palm = V.find_grip(pieces)
        if not palm:
            continue
        palm = [palm[0] + GRIP_SHIFT.get(key, 0.0), palm[1], palm[2]]
        if key in MELEE:
            pitch, yaw = V.blade_correction(pieces, palm)
        else:
            pitch, yaw = V.barrel_correction(pieces)
        geo = V.reload_geometry(paks, info) or [-1, [0, 0, 0], [0, 0, 0], [0, 0, -1], -1, [0, 0, 0], [-1, 0, 0], 0.0]
        mag, mpt, mloc, mdir, chg, cpt, cdir, travel = geo

        def v(x):
            return "%.2f %.2f %.2f" % tuple(x)
        grips.append("%s  %s %.1f %.1f  %d %s %s %s  %d %s %s %.1f" % (
            key, v(palm), pitch, yaw, mag, v(mpt), v(mloc), v(mdir), chg, v(cpt), v(cdir), travel))

    # free hands: the Luger's grip hand, and its mirror image for the left
    luger = weapon_infos(paks)["models/weapons2/luger/v_luger"]
    right, right_centre = V.hand_model(V.pose_weapon(paks, luger), "gun")
    left = mirrored(right)
    left_centre = [right_centre[0], -right_centre[1], right_centre[2]]

    with zipfile.ZipFile(out_pk3, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("models/vr/hand_r.md3", V.write_md3("models/vr/hand_r.md3", right, right_centre))
        out.writestr("models/vr/hand_l.md3", V.write_md3("models/vr/hand_l.md3", left, left_centre))
        out.writestr("vr/weapons.cfg", V.WEAPONS_CFG_HEADER.replace("weap file", "first person model")
                     + "\n".join(grips) + "\n")
        for path, data in vr_menus(paks).items():
            out.writestr(path, data)
        cut = 0
        for weapon, path in sorted(RELOAD_SOUNDS.items()):
            for key, wav in V.reload_sounds(paks, path, 'reloadSound "%s"' % path).items():
                out.writestr("vr/sound/w%d_%s.wav" % (weapon, key), wav)
                cut += 1
    print("cut %d reload sounds" % cut)
    print("wrote %s: %d weapon grips" % (out_pk3, len(grips)))


if __name__ == "__main__":
    main()
