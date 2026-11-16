# Hand alignment tool (handtweak)

A mode built into the game (cgame_sp_x64.dll) for lining the hands up on each gun yourself,
in the headset, while holding the gun. Changes are saved per weapon and load automatically.

## What it can move

The hands on the guns are RealRTCW's own first person hands, modelled into each gun, so the
fingers can't be bent one by one. What you can line up, per weapon:

| Part | What moves |
|---|---|
| **WHOLE GUN IN HAND** | The gun with both its hands, inside your controller. Use this to put the virtual fingers where your real fingers are. |
| **TRIGGER HAND** | The trigger hand's palm and fingers on the gun (turns about the wrist). Use this to put the finger on the trigger. |
| **SUPPORT HAND** | The off hand's palm and fingers on the gun (turns about the wrist). The two handed grab spot follows it. |

Each part has forward / left / up, pitch / yaw / roll and a size. Pistols have no support hand,
so that part is skipped for them.

## Using it in VR

1. Open the console (`~`) and type `handtweak` (type it again to turn it off). Or bind a key once:
   `bind F8 handtweak`.
2. A readout appears on the HUD panel: the weapon, the selected part in [brackets], and its values.
3. Hold the gun normally. Keep the **off hand away from the gun** (about a foot), so it doesn't
   grab the gun for two handed aim.
4. **Off hand grip, held:** move and turn the off hand. The selected part follows your motion (at
   half speed, for fine work). Let go to keep it. It saves on release.
5. **Off hand trigger, tap:** next part. **Hold it 1.5 seconds:** that part goes back to zero.
6. Switch weapons and do the next one. Each weapon remembers its own settings.

If the readout turns red ("Off hand is on the gun"), move the off hand away; it froze the drag
because the hand had taken the gun, a bolt or a body.

## Console commands (also for keyboard binds)

| Command | Does |
|---|---|
| `handtweak` | Turn the mode on or off (`handtweak 1` / `handtweak 0`) |
| `handtweak_part gun` / `trigger` / `support` / `next` | Pick the part |
| `handtweak_nudge <f l u p y r size> <amount>` | Step one value, for example `handtweak_nudge f 0.1` or `handtweak_nudge r -1` |
| `handtweak_undo` | Undo the last change (run it again to redo) |
| `handtweak_reset` | This part back to zero (`handtweak_reset all`: every part of this weapon) |
| `handtweak_print` | Print this weapon's values (`handtweak_print all`: every weapon) |
| `handtweak_save` / `handtweak_load` | Write or re-read the file by hand |
| `vr_handTweakSpeed 0.5` | Drag speed: 1 follows your hand exactly, 0.25 for very fine work |

Example keypad binds for fine steps (optional, type them yourself if you want them):

    bind KP_UPARROW "handtweak_nudge f 0.1"
    bind KP_DOWNARROW "handtweak_nudge f -0.1"
    bind KP_LEFTARROW "handtweak_nudge l 0.1"
    bind KP_RIGHTARROW "handtweak_nudge l -0.1"
    bind KP_PGUP "handtweak_nudge u 0.1"
    bind KP_PGDN "handtweak_nudge u -0.1"

## Where it's saved

`vr_handalign.txt`, in the game's main folder where it writes its configs (next to
`realrtcw_cvars.cfg`). One line per weapon and part:

    mp40.weap trigger 0.120 -0.300 0.050 0.00 4.50 -2.00 1.000

Delete a line to put that part back.

## Notes

- Turning the **whole gun** (pitch / yaw) also turns the barrel away from where bullets go,
  because shots follow the controller. Prefer moving the gun (forward / left / up) and turning
  the hands. Small turns are fine.
- A trigger or support hand moved far from its sleeve can show a gap at the wrist.
- Dual wielded pistols: the off hand's mirrored gun doesn't take the gun offset.
- It only changes how things are drawn (and where the off hand grabs the gun). Saves, configs
  and key bindings aren't touched.

The source is `code/cgame/cg_vrhandtweak.c`.
