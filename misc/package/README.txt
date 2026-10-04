RealRTCW-VR Co-op
=================

Co-op Return to Castle Wolfenstein (RealRTCW co-op mode, including the Cursed Sands mod)
with full VR support: 6DOF head and hands, guns held in your hands, manual reloading,
a physical knife, grabbing bodies, a world-space menu with a laser pointer.

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


Playing together
----------------
Both players must use the same mod: both Cursed Sands, or both classic.

Host:  start a launcher, choose CREATE SERVER, pick the map and start it.
       The others connect to your IP address on UDP port 27960. On the internet, forward
       UDP 27960 on your router to your PC, or use a virtual LAN such as Tailscale,
       ZeroTier or Radmin VPN and give your friend that address.

Join:  start the same launcher and choose JOIN GAME, or join directly from a
       command prompt in this folder:
           "Cursed Sands VR.bat" +connect 192.168.1.20
       (the host's address), or open the console (~ key) and type
           connect 192.168.1.20

Each player sets their own VR options (main menu, VR OPTIONS), including manual
reloading and aim accuracy.


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
