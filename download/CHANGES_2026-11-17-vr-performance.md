# VR performance patch (November 17)

A separate download that goes on top of the **November 16** update. Unzip it into your
RealRTCW folder (the one with `RealRTCW.x64.exe`) and let it replace the files. Close the
game first.

## What it does

The new gore was heavy in VR because a headset draws every scene twice, once per eye, and
every bloodied body was being worked out again from scratch for each eye.

- **Faster with no change to the look:** the renderer now reuses a body it has already
  deformed when the bones haven't moved (the second eye of every character, and the extra
  pass that draws a body's blood). It also keeps the blood laid on a body and reuses it while
  nothing has changed. Only results with exactly the same inputs are reused, so nothing on
  screen changes. `r_vrSkinCache 0` turns this off to compare.
- **New option: Gore & Extras > Gore detail (VR speed)** (`vr_goreLite`):
  - **High** (default): exactly the look of the November 16 update.
  - **Medium:** half the blood mist off a hit, half as many drops in the air at once, round 3D
    blood only up close, fewer drop marks on clothes, blood layers on bodies drawn 2/3 as far
    off, physics capped at 4 ms a frame.
  - **Low:** less again: a third of the mist, fewer heavy drops, flat blood in the air, a
    quarter of the drop marks, blood layers drawn 40% as far off, physics capped at 3 ms.
- **`vr_perfStats 1`** in the console prints, once a second, how long the physics, gore shots,
  gore meshes, blood drops and body blood took each frame. Please include that line if you
  report slowdowns.

If it still struggles on Low, `vr_realWeights 0` runs the physics at 62 Hz instead of 125.

## Files

- `game/renderer_sp_opengl1_x64.dll`
- `game/main/cgame_sp_x64.dll`
- `game/main/z_vr_zperf.pk3` (the new menu row)

To undo, reinstall those two DLLs from the November 16 zip and delete `z_vr_zperf.pk3`.
