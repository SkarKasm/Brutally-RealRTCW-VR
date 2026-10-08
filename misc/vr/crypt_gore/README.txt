The crypt's torn soldier (models/vr_gore in z_vr_assets.pk3)

The torn soldier and the dead nazis in the crypt levels are misc_models built into
crypt1.bsp, not separate model files. export_gore.py cuts their surfaces out of the map
(extract maps/crypt1.bsp from RTCW's pak0.pk3 and point B at it) into md3s:

  head_shot.md3     dead nazi head (deadnazi3), shader models/vr_gore/deadnazi3_shot
                    (deadnazi3.jpg from RealRTCW's z_realrtcw_models_hd.pk3 with a bullet
                    hole painted on the forehead)
  head_skinned.md3  the half skinned head (bphead), a little bigger than in the map
  ribs.md3          the ribs through the left of the chest (bp2)
  guts.md3          the guts out of the belly (bp2)
  stump.md3         the torn neck (bp2)

Heads line up with RTCW's own head models (x forward, z up, the neck at z -3.5). The torso
pieces are in the torso's frame (x the way the chest faces, z up the spine, the origin 11.4
below the neck), as cg_vrextras.c CG_VRX_AddCryptGore places them.
