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
MELEE = ("models/weapons2/knife/v_knife", "models/multiplayer/knife/v_knife")

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

    def read(self, path):
        # the renderer loads a model's .mdc before its .md3 (tr_model.c), whichever pak
        base, ext = os.path.splitext(path.lower().replace("\\", "/"))
        if ext in (".md3", ".mdc"):
            for alt in (".mdc", ".md3"):
                hit = self.index.get(base + alt)
                if hit:
                    return hit[0].read(hit[1])
            return None
        return V.Paks.read(self, path)


def weapon_infos(paks):
    """{model key: info} for every first person weapon (key: v_ model path, no extension)."""
    out = {}
    for name in sorted(paks.names()):
        m = re.match(r"^(models/(?:weapons2|multiplayer)/[^/]+)/v_([^/]+)_hand\.(md3|mdc)$", name)
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
                    "config": folder + "/weapon.cfg",
                    "scope": key + "_scope.md3" if paks.read(key + "_scope.md3") else None}
    return out


def mirrored(surfaces):
    """Left hand from the right: mirror across the forward/up plane, flip the winding."""
    out = []
    for s in surfaces:
        verts = [([p[0], -p[1], p[2]], [n[0], -n[1], n[2]]) for p, n in s.verts]
        tris = [(t[0], t[2], t[1]) for t in s.tris]
        out.append(V.Surface(s.name, s.shader, tris, s.st, verts))
    return out


# (name, text, action, highlighted). HOST GAME starts a game friends can join over Steam,
# JOIN FRIEND joins a Steam friend's (net_steam.c).
MENU_BUTTONS = (
    ("vrhostgame", "HOST GAME", "close main; open vr_host", True),
    ("vrjoinfriend", "JOIN FRIEND", "exec \"steamjoin\"", True),
    ("vroptionsmenu", "VR OPTIONS", "close main; open vr_options", False),
)


def ee_button(name, text, action, hot, x, y, w):
    """Cursed Sands main menu style: small centred text over the bottom bar."""
    color = ".95 .75 .25 1" if hot else ".9 .9 .9 1"
    return """
		// %s (RealRTCW-VR, added by make_vr_assets.py)
		itemDef
		{
			name %s
			rect %d %d %d 14
			style 0
			type 1
			text "%s"
			textStyle 6
			textAlign ITEM_ALIGN_CENTER
			textScale .25
			textAlignX %d
			textAlignY 14
			textFont UI_FONT_NORMAL
			foreColor %s
			visible 1
			action { %s }
		}
""" % (text, name, x, y, w, text, w // 2, color, action)


def coop_button(name, text, action, hot, y):
    """Plain co-op's RealRTCW style main menu: a row under its QUIT button."""
    color = ".95 .75 .25 1" if hot else "1 1 1 1"
    return """
	// %s (RealRTCW-VR, added by make_vr_assets.py)
	itemDef {
		name %s
		text "%s"
		group mainmenus
		type 1
		rect 10 %d 178 26
		visible 1
		textfont UI_FONT_NORMAL
		textscale .3
		textalign ITEM_ALIGN_LEFT
		textalignx 6
		textaligny 19
		forecolor %s
		action { %s }
	}
""" % (text, name, text, y, color, action)


EE_PLACES = ((8, 378, 72), (119, 378, 64), (392, 378, 72))     # above CREATE SERVER, JOIN GAME, OPTIONS
COOP_ROWS = (309, 342, 375)


def arena_maps(paks):
    """[(map, long name)] of the campaign, from the game's scripts/arenas.txt."""
    data = paks.read("scripts/arenas.txt")
    if not data:
        return []
    text = data.decode(errors="replace")
    out = []
    for block in re.findall(r"\{(.*?)\}", text, re.S):
        m = re.search(r'\bmap\s+"([^"]+)"', block)
        n = re.search(r'\blongname\s+"([^"]+)"', block)
        if m:
            out.append((m.group(1), n.group(1) if n else m.group(1)))
    return out


# the host page: (cvar, label, choices). Choices are (text, value) pairs; numbers become
# cvarFloatList entries, strings cvarStrList.
HOST_GAME = (
    ("g_gametype", "Mode:", (("Co-op", 2), ("Speedrun", 1), ("Battle (1 vs 1)", 0))),
    ("g_gameskill", "Difficulty:", (("Can I play, Daddy?", 0), ("Don't hurt me.", 1),
                                    ("Bring 'em on!", 2), ("I am Death Incarnate!", 3))),
    ("sv_maxcoopclients", "Players:", (("2", 2), ("3", 3), ("4", 4), ("6", 6), ("8", 8))),
    ("g_skipcutscenes", "Cutscenes:", (("Watch", 0), ("Skip", 1))),
    ("g_reinforce", "Enemies:", (("Normal", 0), ("More", 1), ("Even more", 2))),
    ("g_airespawn", "Enemies respawn:", (("No", 0), ("Yes", 1))),
)
HOST_RULES = (
    ("g_friendlyFire", "Friendly fire:", (("Off", 0), ("On", 1), ("On (health penalty)", 2))),
    ("g_maxlives", "Lives:", (("Unlimited", 0), ("1", 1), ("3", 3), ("5", 5), ("10", 10))),
    ("g_sharedlives", "Shared lives:", (("No", 0), ("Yes", 1))),
    ("g_freeze", "After dying:", (("Respawn", 0), ("Frozen till knifed free", 1))),
    ("g_spawnpoints", "Saves:", (("Autosaves", 0), ("Manual saves", 1), ("Checkpoints", 2))),
    ("g_maxspawnpoints", "Max saves:", (("Unlimited", 0), ("1", 1), ("3", 3), ("5", 5), ("10", 10))),
    ("g_limbotime", "Respawn delay:", (("1 sec", 1000), ("3 sec", 3000), ("5 sec", 5000),
                                       ("10 sec", 10000), ("20 sec", 20000), ("30 sec", 30000))),
    ("g_spawnInvul", "Spawn protection:", (("Off", 0), ("3 sec", 3), ("5 sec", 5), ("10 sec", 10))),
    ("g_throwKnives", "Throwing knives:", (("Off", 0), ("On", 1))),
    ("g_shove", "Shove teammates:", (("Off", 0), ("On", 1))),
)


def menu_multi(cvar, label, choices, x, y):
    if all(isinstance(v, str) for _t, v in choices):
        kind = "cvarStrList"
        items = " ".join('"%s" "%s"' % (t, v) for t, v in choices)
    else:
        kind = "cvarFloatList"
        items = " ".join('"%s" %s' % (t, v) for t, v in choices)
    return """
		itemDef {
			name vr_host_opt
			type ITEM_TYPE_MULTI
			text "%s"
			cvar "%s"
			%s { %s }
			rect %d %d 290 14
			textalign ITEM_ALIGN_RIGHT
			textalignx 120
			textaligny 11
			textscale .22
			textfont UI_FONT_BIG
			style WINDOW_STYLE_FILLED
			backcolor 1 1 1 .07
			forecolor 1 1 1 1
			visible 1
		}
""" % (label, cvar, kind, items, x, y)


def menu_text(name, text, x, y, w, scale, color, align="ITEM_ALIGN_LEFT", alignx=4):
    return """
		itemDef {
			name %s
			text "%s"
			rect %d %d %d 16
			textalign %s
			textalignx %d
			textaligny 13
			textscale %s
			textfont UI_FONT_BIG
			forecolor %s
			visible 1
			decoration
		}
""" % (name, text, x, y, w, align, alignx, scale, color)


def menu_button(name, text, x, w, action):
    return """
		itemDef {
			name %s
			type ITEM_TYPE_BUTTON
			text "%s"
			rect %d 425 %d 24
			textalign ITEM_ALIGN_CENTER
			textalignx %d
			textaligny 19
			textscale .3
			textfont UI_FONT_BIG
			style WINDOW_STYLE_FILLED
			backcolor 1 1 1 .07
			forecolor 1 1 1 1
			visible 1
			action { %s }
		}
""" % (name, text, x, w, w // 2, action)


def vr_host_menu(maps):
    """The HOST GAME page: level and every co-op server option, then START GAME."""
    items = [menu_text("vr_host_title", "HOST GAME", 0, 28, 640, ".4", "1 .85 .5 1", "ITEM_ALIGN_CENTER", 320),
             menu_text("vr_host_game", "GAME", 20, 78, 290, ".27", "1 .75 0 1"),
             menu_text("vr_host_rules", "RULES", 330, 78, 290, ".27", "1 .75 0 1")]
    y = 104
    if maps:
        # by position in arenas.txt: co-op's menus can't show text-valued choices
        items.append(menu_multi("vr_hostLevel", "Level:", [(n, i) for i, (m, n) in enumerate(maps)], 20, y))
        y += 20
    for cvar, label, choices in HOST_GAME:
        items.append(menu_multi(cvar, label, choices, 20, y))
        y += 20
    y = 104
    for cvar, label, choices in HOST_RULES:
        items.append(menu_multi(cvar, label, choices, 330, y))
        y += 20
    items.append(menu_text("vr_host_note", "Friends join with JOIN FRIEND on their main menu, or Join Game in Steam. "
                           "Nothing else to set up.", 0, 395, 640, ".18", ".8 .8 .8 1", "ITEM_ALIGN_CENTER", 320))
    items.append(menu_button("vr_host_start", "START GAME", 190, 140, 'close vr_host; exec "hostcoop"'))
    items.append(menu_button("vr_host_back", "BACK", 350, 100, "close vr_host; open main"))
    return ("""// RealRTCW-VR: HOST GAME page, generated by misc/vr/make_vr_assets.py.
#include "ui/menudef.h"

{
	menuDef {
		name "vr_host"
		rect 0 0 640 480
		fullScreen MENU_TRUE
		visible MENU_FALSE
		focusColor 1 .75 0 1
		style WINDOW_STYLE_FILLED
		backcolor 0 0 0 .85
		onESC { close vr_host; open main }
""" + "".join(items) + """	}
}
""").encode()


def vr_menus(paks):
    here = os.path.dirname(os.path.abspath(__file__))
    out = {}
    with open(os.path.join(here, "ui", "vr_options.menu"), "rb") as f:
        out["ui/vr_options.menu"] = f.read()
    out["ui/vr_host.menu"] = vr_host_menu(arena_maps(paks))

    menus = paks.read("ui/menus.txt")
    main = paks.read("ui/main.menu")
    if not menus or not main:
        print("warning: menus not found, no VR OPTIONS button")
        return out
    menus = menus.decode(errors="replace")
    if "vr_options.menu" not in menus:
        menus = re.sub(r'(loadMenu\s*\{\s*"ui/main\.menu"\s*\})',
                       r'\1\n\tloadMenu { "ui/vr_options.menu" }\t// RealRTCW-VR'
                       r'\n\tloadMenu { "ui/vr_host.menu" }\t// RealRTCW-VR', menus, count=1)
    out["ui/menus.txt"] = menus.encode()

    main = main.decode(errors="replace")
    m = re.search(r'itemDef\s*\{\s*name\s+"?fadeBox"?', main)
    q = re.search(r'mouseExit\s*\{\s*hide\s+"realrtcw_main_quit"\s*\}\s*\}', main)
    if "vroptionsmenu" in main:
        pass
    elif m:     # Cursed Sands
        items = "".join(ee_button(*b, *p) for b, p in zip(MENU_BUTTONS, EE_PLACES))
        main = main[:m.start()] + items.strip() + "\n\t\t" + main[m.start():]
        out["ui/main.menu"] = main.encode()
    elif q:     # plain co-op
        items = "".join(coop_button(*b, y) for b, y in zip(MENU_BUTTONS, COOP_ROWS))
        main = main[:q.end()] + items + main[q.end():]
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
        lens = V.scope_lens(paks, info)
        lens = "  %d %s %.2f" % (lens[0], v(lens[1]), lens[2]) if lens else "  -1 0 0 0 0"
        grips.append("%s  %s %.1f %.1f  %d %s %s %s  %d %s %s %.1f%s" % (
            key, v(palm), pitch, yaw, mag, v(mpt), v(mloc), v(mdir), chg, v(cpt), v(cdir), travel, lens))

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
