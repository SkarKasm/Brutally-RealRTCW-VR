#!/usr/bin/env python3
"""Cursed Sands (fs_game EE) brings its own console style menus, which hide the VR OPTIONS and
GORE & EXTRAS buttons that main/z_vr_assets.pk3 adds. This writes EE/z_vr_ui.pk3 with those
menus patched the same way.

    make_ee_ui.py <RealRTCW EE folder> <main/z_vr_assets.pk3> <out EE/z_vr_ui.pk3>
"""

import os
import re
import sys
import zipfile


def read_ee(ee_dir, path):
    """The file from the EE pak that wins, like the game picks it (the last one by name)."""
    data = None
    for name in sorted(n for n in os.listdir(ee_dir) if n.lower().endswith(".pk3")):
        with zipfile.ZipFile(os.path.join(ee_dir, name)) as z:
            if path in z.namelist():
                data = z.read(path)
    return data


def button(menu, name, text, x, page):
    return """
		// %s (RealRTCW-VR, added by make_ee_ui.py)
		itemDef
		{
			group "button"
			name "%s"
			type ITEM_TYPE_BUTTON
			rect %d 380 165 40
			style WINDOW_STYLE_SHADER
			background "ui/assets/button"
			foreColor 1 1 1 .5
			text "%s"
			textAlign ITEM_ALIGN_CENTER
			textAlignX 82.5
			textAlignY 25
			textFont UI_FONT_NORMAL
			textScale .32
			visible %d
			action { play "sound/menu/select.wav"; close "%s"; open "%s" }
			mouseEnter { setitemcolor "%s" foreColor 1 1 1 1 }
			mouseExit { setitemcolor "%s" foreColor 1 1 1 .5 }
		}
""" % (text, name, x, text, 0 if menu == "main" else 1, menu, page, name, name)


def add_buttons(text, menu, suffix):
    """The two buttons side by side under the last row, at the end of the menuDef."""
    items = (button(menu, "vroptionsmenu", "VR OPTIONS", 150, "vr_options" + suffix) +
             button(menu, "vrextrasmenu", "GORE & EXTRAS", 325, "vr_extras" + suffix))
    end = text.rstrip().rfind("}")
    end = text.rfind("}", 0, end)     # the menuDef's closing brace
    return text[:end] + items.replace("\n", "\r\n") + text[end:]


def main():
    ee_dir, vr_pk3, out_pk3 = sys.argv[1:4]
    with zipfile.ZipFile(vr_pk3) as z:
        pages = {p: z.read("ui/%s.menu" % p).decode() for p in ("vr_options", "vr_extras")}

    out = {}
    for p, text in pages.items():
        out["ui/%s.menu" % p] = text
        # the pause menu's copies go back to "ingame_back", like Cursed Sands' own pages
        text = re.sub(r"\b(vr_options|vr_extras)\b(?!_)", r"\1_ingame", text)
        out["ui/%s_ingame.menu" % p] = text.replace("open main", "open ingame_back")

    for path, menu, suffix in (("ui/main.menu", "main", ""), ("ui/ingame.menu", "ingame", "_ingame"),
                               ("ui/ingame_back.menu", "ingame_back", "_ingame")):
        text = read_ee(ee_dir, path).decode(errors="replace")
        out[path] = add_buttons(text, menu, suffix)

    menus = read_ee(ee_dir, "ui/menus.txt").decode(errors="replace")
    vr = "".join('\tloadMenu { "ui/%s.menu" }\t// RealRTCW-VR\r\n' % p for p in
                 ("vr_options", "vr_extras", "vr_options_ingame", "vr_extras_ingame"))
    out["ui/menus.txt"] = menus[:menus.rfind("}")] + vr + menus[menus.rfind("}"):]

    with zipfile.ZipFile(out_pk3, "w", zipfile.ZIP_DEFLATED) as z:
        for path, text in sorted(out.items()):
            z.writestr(path, text.encode())
    print("wrote", out_pk3)


if __name__ == "__main__":
    main()
