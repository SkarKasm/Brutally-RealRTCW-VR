RealRTCW-VR (single player)
===========================

RealRTCW's single player campaign with full VR support: 6DOF head and hands, guns held in
your hands, manual reloading, see-through scopes, a physical knife and grenades, a full
body, ragdolls you can pick up and throw, a world-space menu with a laser pointer.
It also plays on the monitor without a headset.

For co-op, use the separate RealRTCW-VR-Coop package.


What's new (October 8 update)
-----------------------------
- The launchers start again for everyone: they find RealRTCW from the folder you unzipped into
  (a bug in game\find_realrtcw.cmd closed them instantly when Steam wasn't in its default place).
- Ragdolls: heads no longer spin round when soldiers die, and bodies fall at real-world gravity
  instead of being slammed to the floor (console: vr_ragdollGravity, default 400, the old value 800).
- New GORE & EXTRAS option "Original gibbing" (Off by default): explosions no longer burst
  soldiers into the game's stock gibs, so the mod's dismemberment takes them apart instead.


What you need
-------------
- Return to Castle Wolfenstein (Steam)
- RealRTCW (Steam)
- For VR: a PC VR headset with SteamVR (or another OpenXR runtime) running.
  Quest: connect with Steam Link, Air Link, Virtual Desktop or a Link cable first.

Unzip into your RealRTCW folder (normally ...\steamapps\common\RealRTCW). Nothing of
RealRTCW's own is changed. Settings and saves are kept in the "game" folder.

The launchers find RealRTCW through Steam. If they can't, edit game\find_realrtcw.cmd
and set RRTCW to the RealRTCW folder (normally ...\steamapps\common\RealRTCW).


Launchers
---------
  RealRTCW VR.bat          the campaign in VR (falls back to the monitor without a headset)
  RealRTCW (no VR).bat     the campaign on the monitor


Options
-------
Main menu, VR OPTIONS: reloading, aim, turning, comfort, weapon size and the rest.

Main menu (or VR OPTIONS), GORE & EXTRAS: everything here is optional, VR or not.
  Extra gore                 master switch for all the gory ones
  Body pickup (flat)         without a headset: hold USE on a body, attack throws it
  Grab assist                how far your hand reaches for things
  Can grab (VR)              a buzz / a hand sign when your hand is on something
  Shot reactions             enemies flinch, bodies are knocked about
  Bullet wounds              wounds where shots hit
  Throat cuts and guts       knife kills from behind cut the throat, low stabs open the belly
  Pick up and throw props    chairs, bottles, lamps...; thrown ones stun / hurt / break
  Knife takes heads (dead)   cut a dead body's neck; the head can be picked up and thrown
  Thrown heads               soldiers who see one land run, or drop their guns and give up
  Dismemberment (dead)       explosions (and heavy hits) take limbs off the dead
  Original gibbing           Off: explosions leave bodies whole for the gore above


VR controls (Quest / Touch layout)
----------------------------------
Left stick           move (click: kick)
Right stick          left/right turn (snap or smooth), up/down change weapon (click: use)
Right trigger        fire           Right grip + trigger   alternate fire
Left trigger         sprint
Right grip (tap)     drop the magazine (manual reloading) / reload
Left grip            hold the gun with both hands (hold in front of the gun hand),
                     take a magazine from your chest and push it into the gun,
                     grab the slide / bolt and pull it back, grab bodies, props and heads
A                    crouch         B   jump
X                    toggle HUD     Y   notebook
Menu button          game menu; in menus point with either controller and pull the trigger
Any button during cutscenes and videos skips them.
