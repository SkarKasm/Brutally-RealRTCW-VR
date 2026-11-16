# RealRTCW VR Hand Editor

A program on your PC for posing the hands and fingers on each VR weapon: a small local server
plus a 3D editor page in your browser. It never starts the game.

In the release zip it is already in place. From this repository, copy this folder to
`game\tools\hand-editor\` in your RealRTCW folder and `Start Hand Editor.cmd` into the RealRTCW
folder itself.

## Start it

Double-click **`Start Hand Editor.cmd`** in your RealRTCW folder (normally `...\steamapps\common\RealRTCW`).
A console window opens and your browser shows the editor. Close the console window (or press
**Stop editor**) to quit.

It needs Python 3 on the PC.

## What you see

The game's own weapon and hand models, posed the way VR holds them:
the **orange** trigger hand, the **blue** support hand, the grey gun, and the green sleeves.
The white lines are the finger joints; the selected finger is yellow.

## Editing

- **Weapon**: pick a gun. Weapons with only a trigger hand (pistols, grenades) have no support tab.
- **Size**: weapon size %, for the whole gun with its hands, in VR.
- **Hand**: choose Trigger or Support. Use **Move (G)** / **Rotate (R)** to drag the hand with the
  gizmo, or the sliders: forward/back, left/right, up/down (cm at VR size), roll, pitch, yaw and
  hand size. "Sleeve follows the hand" bends the sleeve with it so the wrist doesn't open up.
- **Fingers**: choose a finger, then base / middle / tip joint bend, base splay and roll.
  Positive bend curls towards the palm, negative opens.
- **Undo / Redo** (Ctrl+Z / Ctrl+Shift+Z), **Save poses** (Ctrl+S), **Export / Import JSON**,
  **Reset** hand / finger / weapon.
- **View**: camera buttons (3D, Your view, Left, Right, Muzzle, Rear, Top, Under), wireframe,
  see-through weapon, joints, other hand, weapon, sleeves, and PNG screenshots.

## Getting it into the game

1. **Save poses**.
2. **Build saved poses**: makes `zz_vr_handedits.pk3` with the edited hand models.
3. Close the game, then **Install into game**: copies the pk3 into `game\main` and writes the
   weapon sizes into `game\main\vr_handalign.txt`. The previous copies of both are backed up to
   `game\main\backup-<date>-hand-editor` first. It won't install while RealRTCW is running.
4. Start the game as usual.

**Remove from game** takes the installed pk3 out again (moved into a backup folder). Your saved
poses stay in `game\tools\hand-editor\data\hand_edits.json`.

## How it works

- RealRTCW's first person hands have no bones, so the editor fits a finger rig to each hand
  itself: every hand uses the same glove texture layout, which tells it which finger each part
  of the mesh belongs to and how far along it, and it puts 3 joints on each finger from that.
- Edits are baked into copies of the gun model files (every animation frame), in a pk3 that
  loads after the game's own. The originals in RealRTCW's Main folder are never changed.
- Some weapons share model files (for example the Mauser and the sniper rifle). The editor tells
  you when that's the case: edits on one change the other too.

## Files

- `Start Hand Editor.cmd`: goes in the RealRTCW folder.
- Everything else here goes in `game\tools\hand-editor\` (Python server, page, three.js 0.180).
- The cgame that reads the weapon size the editor sets is built from `code/cgame/cg_vrhandtweak.c`.
