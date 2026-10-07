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


What you need
-------------
- Return to Castle Wolfenstein (Steam)
- RealRTCW (Steam), with its co-op mode installed
- For Cursed Sands: the "RealRTCW Cursed Sands - Coop Mode" Workshop item
- For VR: a PC VR headset with SteamVR (or another OpenXR runtime) running.
  Quest: connect with Steam Link, Air Link, Virtual Desktop or a Link cable first.

Unzip this folder anywhere. Nothing in the Steam folders is changed. Settings and
saves are kept in the "game" folder.

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

For single player, use the separate RealRTCW-VR package (RealRTCW-VR.zip).


VR controls (Quest / Touch layout)
----------------------------------
Left stick           move (click: kick)
Right stick          left/right turn (snap or smooth), up/down change weapon (click: use)
Right trigger        fire           Right grip + trigger   alternate fire
Left trigger         sprint
Right grip (tap)     drop the magazine (manual reloading) / reload
Left grip            hold the gun with both hands (hold in front of the gun hand),
                     take a magazine from your chest and push it into the gun,
                     grab the slide / bolt and pull it back, grab bodies
A                    crouch         B   jump
X                    toggle HUD     Y   notebook
Menu button          game menu; in menus point with either controller and pull the trigger
Any button during cutscenes and videos skips them.
