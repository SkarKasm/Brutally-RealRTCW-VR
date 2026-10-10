RealRTCW-VR Co-op
=================

Co-op Return to Castle Wolfenstein (RealRTCW co-op mode, including the Cursed Sands mod)
with full VR support: 6DOF head and hands, guns held in your hands, manual reloading,
a physical knife, ragdolls you can pick up and throw, a world-space menu with a laser
pointer. Ragdolls can be turned off in VR OPTIONS.

Every player needs this build. The normal RTCWCoop.exe can't join a game hosted with
it, and the other way round. Players without a headset can still use it: the
"(no VR)" launchers play on the monitor, and the VR launchers fall back to the monitor
when no headset is found.


What's new (October 10 update)
------------------------------
Everything from the single player update that works in co-op:
- The RTCWQuest button layout (see VR controls below; Y pings, grip + Y opens the notebook).
- Weapon wheel: hold the right grip, move your right hand towards a gun (or tilt the right
  stick) and let go. It has pages, and binoculars on the last one.
- Dual wielding: double press the left grip for a second pistol / SMG. Each gun has its own
  magazine; X and A drop the left and right gun's, a hand held down at your hip reloads its gun.
- A gun held with both hands: tap the right grip and the left hand holds it alone, so the right
  hand can work the bolt or slide.
- Ladders: walk into them and look up or down. Doors, buttons and levers: touch them with a hand.
- Lean out over your body (VR OPTIONS, Lean), height offset, grenade throws at the reticle,
  and Weapon size: Realistic, Larger or Smaller.
- Life size hands on every gun, with a trigger hand on rifles that never showed one, and arms
  that bend at the wrist and elbow, on RealRTCW's HD guns (used whenever RealRTCW is
  installed; console: vr_hdWeapons 0 for the original game's guns, which keep their own hands).
- The game window on your monitor shows your left eye, filling the window
  (console: vr_mirrorFit 1 for the whole eye image with bars).
- Monitor, experimental and off by default: cg_flatADS 1 aims down the sights.
Not in co-op: knife throws, and quick save / load on the weapon wheel.


What you need
-------------
- Return to Castle Wolfenstein (Steam)
- RealRTCW (Steam), with its co-op mode installed
- For Cursed Sands: the "RealRTCW Cursed Sands - Coop Mode" Workshop item
- For VR: a PC VR headset with SteamVR (or another OpenXR runtime) running.
  Quest: connect with Steam Link, Air Link, Virtual Desktop or a Link cable first.

Unzip into your RealRTCW folder (or anywhere). Nothing in the Steam folders is changed.
Settings and saves are kept in the "game" folder. Co-op's VR files for Cursed Sands are
in game\coop_EE, apart from the single player ones in game\EE.

The launchers find RealRTCW's co-op files through Steam. If they can't, edit
game\find_coop.cmd and set COOP to the folder that contains "coopmain"
(normally ...\steamapps\common\RealRTCW\coop).


Launchers
---------
  Cursed Sands VR.bat          Cursed Sands co-op, VR
  Cursed Sands (no VR).bat     Cursed Sands co-op, monitor
  Classic co-op VR.bat         the original campaign in co-op, VR
  Classic co-op (no VR).bat    the original campaign in co-op, monitor


Playing together (over Steam - nothing to set up)
-------------------------------------------------
Keep Steam running and be Steam friends. No IP addresses, port forwarding or VPN:
the game connects through Steam.

Host:  start a launcher and click HOST GAME on the main menu. Pick the level,
       difficulty, number of players, lives, friendly fire, saves and the other rules,
       then click START GAME. Your choices are remembered for next time.

Join:  start a launcher and click JOIN FRIEND. That's it.
       Or, with the game running, right-click the host in your Steam friends list
       and choose "Join Game".

Start the game from one of these launchers BEFORE using "Join Game" in Steam;
if the game isn't running, Steam starts the normal RealRTCW instead.

If the host plays Cursed Sands and you started classic co-op (or the other way
round), the game switches to the host's mod by itself.

The old ways still work too: JOIN GAME lists games on your LAN, and the console
(~ key) takes "connect <IP address>".

Each player sets their own VR options (main menu, VR OPTIONS), including manual
reloading and aim accuracy. The host also chooses the optional mechanics and gore for
everyone on HOST GAME's EXTRAS & GORE page (body pickup without a headset, grab assist,
shot reactions, wounds, throat cuts, throwing props, taking heads, scaring enemies with
them, dismemberment).

For single player, Cursed Sands SP and Enemy Territory SP, use the single player launchers.


VR controls (RTCWQuest layout)
------------------------------
Left stick           move (click: kick)
Right stick          left/right turn (snap or smooth), up jump, down crouch (hold)
Right stick click    use (doors, levers, where you look). Or touch doors, buttons and levers with a hand.
Right trigger        fire (knife: slash by swinging it)
Left trigger         sprint (with two guns: fires the left gun, and the left grip sprints)
Right grip (hold)    weapon wheel: move the right hand towards a weapon (or tilt the right
                     stick), then let go of the grip or pull the trigger
Left grip            hold the gun with both hands (close it where the flat game's support
                     hand holds it, e.g. the MP40's magazine well), take a magazine from
                     your chest and push it into the gun, grab bodies, props and heads
Left trigger         on the slide / bolt / pump handle: grab it and pull it back
Right grip (tap)     on a gun held with both hands: the right hand lets go and the left
                     hand holds it alone. Then the right trigger on the bolt / slide grabs
                     it and pulls it back, and a right grip press back on the gun takes it
Left grip (double)   a second pistol / SMG, or back to one
A                    drop the magazine (manual reloading) / reload; two guns: the right gun's
X                    drop the magazine; two guns: the left gun's
B                    jump         Y   ping (tap: mark a spot for the others, hold: talk
                     with Voice chat on)       Right grip + Y   notebook
Right grip + A       toggle HUD
Right grip + X       use item       Right grip + B   next item
Two guns             hold a hand down at your hip to reload that gun.
Ladders              walk into them and look up or down to climb, like the flat game.
Other controllers    edit game\coopmain\vr_controls.cfg to move any of the above.
Menu button          game menu; in menus point with either controller and pull the trigger
Any button during cutscenes and videos skips them.
