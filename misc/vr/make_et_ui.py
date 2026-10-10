#!/usr/bin/env python3
"""Enemy Territory Single-Player (RealRTCW's Workshop mod, fs_game ET) brings its own ET style
menus, which leave out the VR OPTIONS and GORE & EXTRAS buttons that main/z_vr_assets.pk3 adds.
This writes ET/z_vr_ui.pk3 with them added to its main and pause menus, as two more rows of
the menu list, in the same style. The VR pages themselves go in its main.menu (a .menu file
can hold several menuDefs), so the mod's own ui/menus.txt is left alone.

    make_et_ui.py <ET Workshop folder> <main/z_vr_assets.pk3> <out ET/z_vr_ui.pk3>
"""

import os
import re
import sys
import zipfile


def read_et(et_dir, path):
    """The file from the ET pak that wins, like the game picks it (the last one by name)."""
    data = None
    for name in sorted(n for n in os.listdir(et_dir) if n.lower().endswith(".pk3")):
        with zipfile.ZipFile(os.path.join(et_dir, name)) as z:
            if path in z.namelist():
                data = z.read(path)
    return data


def depth(text):
    """How many braces are still open at the end (strings and comments skipped)."""
    d, i, n, instr = 0, 0, len(text), False
    while i < n:
        c = text[i]
        if instr:
            instr = c != '"'
        elif c == '"':
            instr = True
        elif text.startswith("//", i) or text.startswith("/*", i):
            j = text.find("\n" if text[i + 1] == "/" else "*/", i)
            i = n if j < 0 else j + (1 if text[i + 1] == "/" else 2)
            continue
        elif c == "{":
            d += 1
        elif c == "}":
            d -= 1
        i += 1
    return d


def items(text):
    """(start, end, name) of every top level itemDef in the menuDef."""
    out = []
    for m in re.finditer(r"\bitemDef\b", text):
        i = text.index("{", m.end())
        depth, j = 0, i
        while True:
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[m.start():j + 1]
        name = re.search(r"\bname\s+\"?(\w+)\"?", body)
        out.append((m.start(), j + 1, name.group(1) if name else None))
    return out


def find(text, name):
    for s, e, n in items(text):
        if n == name:
            return s, e
    raise SystemExit("no itemDef %s" % name)


def move(text, name, dy, dh=0):
    """Shift an item down by dy (and make it dh taller)."""
    s, e = find(text, name)
    body = text[s:e]

    def fix(m):
        x, y, w, h = (float(v) for v in m.group(1).split())
        return "rect %g %g %g %g" % (x, y + dy, w, h + dh)
    body = re.sub(r"rect\s+([-\d. ]+?\d)\s*(?=\n|//)", fix, body, count=1)
    return text[:s] + body + text[e:]


def add_after(text, template, rows, y0):
    """Copies of the template button, one row (24 units) apart from y0, after it."""
    s, e = find(text, template)
    body = text[s:e]
    new = ""
    for k, (name, label, action) in enumerate(rows):
        b = re.sub(r"\bname\s+\"?%s\"?" % template, "name %s" % name, body)
        b = re.sub(r"\b%s\b" % template, name, b)        # mouseEnter / mouseExit item names
        b = re.sub(r"text\s+\"[^\"]*\"", 'text "%s"' % label, b, count=1)
        b = re.sub(r"rect\s+([-\d.]+)\s+[-\d.]+\s+([-\d.]+)\s+([-\d.]+)",
                   lambda m: "rect %s %d %s %s" % (m.group(1), y0 + 24 * k, m.group(2), m.group(3)), b, count=1)
        b = re.sub(r"action\s*\{[^}]*\}", "action { %s }" % action, b, count=1)
        new += "\n\n\t// %s (RealRTCW-VR, added by make_et_ui.py)\n\n\t" % label + b
    return text[:e] + new + text[e:]


def main():
    et_dir, vr_pk3, out_pk3 = sys.argv[1:4]
    with zipfile.ZipFile(vr_pk3) as z:
        pages = {p: z.read("ui/%s.menu" % p).decode() for p in ("vr_options", "vr_extras")}

    # the four pages' menuDefs: the main menu's, and the pause menu's going back to it
    defs = ""
    for p, text in pages.items():
        for ingame in (False, True):
            t = text.replace("\r\n", "\n")
            if ingame:
                t = re.sub(r"\b(vr_options|vr_extras)\b(?!_)", r"\1_ingame", t)
                t = t.replace("open main", "open ingame")
            body = t[t.index("{", t.index("#include")) + 1:t.rstrip().rfind("}")]
            defs += "\n// %s%s (RealRTCW-VR, added by make_et_ui.py)\n" % (p, "_ingame" if ingame else "") + body

    out = {}
    snd = 'play "sound/menu/select.wav"; '
    # main menu: after CREDITS, then QUIT and the Patreon button move down
    text = read_et(et_dir, "ui/main.menu").decode(errors="replace").replace("\r\n", "\n")
    text = add_after(text, "credits", [
        ("vroptionsmenu", "VR OPTIONS", snd + "close main; open vr_options"),
        ("vrextrasmenu", "GORE & EXTRAS", snd + "close main; open vr_extras")], 168)
    text = move(text, "quit", 48)
    text = move(text, "patreon", 48)
    text = move(text, "window", 0, 48)
    # at the file's top level, beside the main menuDef (the file leaves its outer brace open)
    if depth(text) == 1:
        text = text.rstrip() + "\n" + defs + "\n"
    else:
        end = text.rstrip().rfind("}")
        text = text[:end] + defs + text[end:]
    out["ui/main.menu"] = text.replace("\n", "\r\n")

    # pause menu: after OPTIONS
    text = read_et(et_dir, "ui/ingame.menu").decode(errors="replace").replace("\r\n", "\n")
    text = add_after(text, "options", [
        ("vroptionsmenu", "VR OPTIONS", snd + "close ingame; open vr_options_ingame"),
        ("vrextrasmenu", "GORE & EXTRAS", snd + "close ingame; open vr_extras_ingame")], 144)
    text = move(text, "exit", 48)
    text = move(text, "patreon", 48)
    text = move(text, "window", 0, 48)
    out["ui/ingame.menu"] = text.replace("\n", "\r\n")

    with zipfile.ZipFile(out_pk3, "w", zipfile.ZIP_DEFLATED) as z:
        for path, text in sorted(out.items()):
            z.writestr(path, text.encode())
    print("wrote", out_pk3)


if __name__ == "__main__":
    main()
