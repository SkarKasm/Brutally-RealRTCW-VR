#!/usr/bin/env python3
"""Brings the single player build's newer VR assets into co-op's z_vr_assets.pk3.

The single player pak (main/z_vr_assets.pk3, made by RealRTCW-VR's make_vr_assets.py) has the
hands, rifle hands and wheel icons the shared VR code now expects, and grips that were fitted
by hand. Co-op's weapons.cfg is keyed by first person model, the single player one by .weap
file, so the RealRTCW (HD) weapons' lines are carried across through the tables below (the
same ones as bg_vr.c's vrHDWeapons and cg_vr.c's vrWeapFiles). The original game's weapons
keep co-op's own lines.

    merge_sp_assets.py <co-op z_vr_assets.pk3> <single player z_vr_assets.pk3> <out pk3>
"""

import os
import sys
import zipfile

HD = [  # weap file, co-op first person model
    ("knife", "models/weapons/melee/knife/v_knife"),
    ("luger", "models/weapons/pistols/luger/v_luger"),
    ("luger_silenced", "models/weapons/pistols/luger_silenced/v_lugers"),
    ("colt", "models/weapons/pistols/colt/v_colt"),
    ("akimbo", "models/weapons/pistols/colt2/v_colt2"),
    ("mp40", "models/weapons/smgs/mp40/v_mp40"),
    ("thompson", "models/weapons/smgs/thompson/v_thompson"),
    ("sten", "models/weapons/smgs/sten/v_sten"),
    ("mauser", "models/weapons/rifles/mauser/v_mauser"),
    ("garand", "models/weapons/rifles/snooper/v_snooper"),
    ("fg42", "models/weapons/assault_rifles/fg42/v_fg42"),
    ("panzerfaust", "models/weapons/heavy/pf/v_pf"),
    ("venom", "models/weapons/secret/venom/v_venom"),
    ("flamethrower", "models/weapons/heavy/flame/v_flame"),
    ("tesla", "models/weapons/secret/tesla/v_tesla"),
    ("grenade", "models/weapons/explosives/grenade/v_grenade"),
    ("pineapple", "models/weapons/explosives/pineapple/v_pineapple"),
    ("dynamite", "models/weapons/explosives/dynamite/v_dynamite"),
]
TAKE = ("models/vr/hand_r.md3", "models/vr/hand_l.md3")
TAKE_PREFIX = ("models/vr/rhand_", "gfx/vr/wheel_binocs")


def main():
    coop, sp, out = sys.argv[1:4]
    here = os.path.dirname(os.path.abspath(__file__))
    with zipfile.ZipFile(coop) as c, zipfile.ZipFile(sp) as s:
        files = {n: c.read(n) for n in c.namelist() if not n.endswith("/")}
        for n in s.namelist():
            if n in TAKE or n.startswith(TAKE_PREFIX):
                files[n] = s.read(n)

        spw = {}
        for line in s.read("vr/weapons.cfg").decode().splitlines():
            if line.startswith("weapons/") and ".weap" in line:
                name, rest = line.split(None, 1)
                spw[name[len("weapons/"):-len(".weap")]] = rest
        lines = files["vr/weapons.cfg"].decode().splitlines()
        keys = {line.split(None, 1)[0]: i for i, line in enumerate(lines) if line and not line.startswith("/")}
        for weap, model in HD:
            if weap not in spw:
                continue
            line = "%s  %s" % (model, spw[weap])
            if model in keys:
                lines[keys[model]] = line
            else:
                lines.append(line)
        files["vr/weapons.cfg"] = ("\n".join(lines) + "\n").encode()

    with open(os.path.join(here, "ui", "vr_options.menu"), "rb") as f:
        files["ui/vr_options.menu"] = f.read()

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for n in sorted(files):
            z.writestr(n, files[n])
    print("wrote", out, len(files), "files")


if __name__ == "__main__":
    main()
