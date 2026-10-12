The crypt's torn soldier (models/vr_gore in z_vr_assets.pk3)

The torn soldier and the dead nazis in the crypt levels are misc_models built into
crypt1.bsp, not separate model files. export_gore.py cuts their surfaces out of the map
(extract maps/crypt1.bsp from RTCW's pak0.pk3 and point B at it) into md3s:

  head_skinned.md3  the half skinned head (bphead), a little bigger than in the map (blasts)
  jaw_skinned.md3   its lower front, worn over any head when a shot takes the jaw off
  jaw_skinned2.md3  the same the other way round
  headhole.md3      a bullet hole quad (shader vr_headhole, vr_gore.shader; headhole.tga
                    from headhole_tex.py)
  flesh_cap.md3     the torn neck, closing off limb stumps and cut off heads
  ribs.md3          the ribs through the left of the chest (bp2)
  guts.md3          the guts out of the belly: Soldier of Fortune 2's own now (sof2_guts.py,
                    texture sof2_organs.tga, shader vr_sof2organs; credit Raven Software)
  stump.md3         the torn neck (bp2)

Heads line up with RTCW's own head models (x forward, z up, the neck at z -3.5). The torso
pieces are in the torso's frame (x the way the chest faces, z up the spine, the origin 11.4
below the neck), as cg_vrextras.c CG_VRX_AddCryptGore places them.

Soldier of Fortune 2's bloody stumps (z_vr_stumps.pk3, credit Raven Software)

sof2_stumps.py cuts SoF2's limb and jaw caps (the "cap_*_off" surfaces of
models/characters/average_sleeves/average_sleeves.glm in SoF2's skins.pk3, texture
models/characters/caps/caps.jpg) and its bone gib (models/characters/gore/bone_small,
bone_long.png) out into md3s (glm.py reads SoF2's Ghoul 2 models). Its docstring has the
arguments; the jaw cap is fitted to RTCW's standard soldier head (infantryss/head.mdc).
Then: z_vr_stumps.pk3 = scripts/vr_stumps.shader + the models/vr_gore/sof2_* it wrote (a copy
of them is in sof2_stumps/).
cg_vrextras.c puts the caps over every cut (VRX_AddSofStump), fills the inside of cut bodies,
limbs and heads with SoF2's cap meat (vr_stumpinside), and cuts the jaw off heads (the
renderer's R_VRX_ClipMesh, tr_surface.c).
