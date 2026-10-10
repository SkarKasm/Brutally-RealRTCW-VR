/*
===========================================================================

RealRTCW-VR
Copyright (C) 2026 RealRTCW-VR contributors

This file is part of RealRTCW-VR, a VR port of RealRTCW.

RealRTCW-VR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// vr_shared.h -- VR state shared between the engine (cl_vr.c) and the game/cgame modules.
//
// The engine owns a single vrShared_t (com_vr in common.c) and hands its address to the
// cgame DLL through CG_SET_VR_SHARED. In co-op the server may be on another machine, so the
// game module never reads it: each player's hands travel in their usercmds (vr* fields) and
// their hand reloading state comes back in playerState_t.vrState / vrGrab.

#ifndef __VR_SHARED_H__
#define __VR_SHARED_H__

#define VR_PASS_FLAT    0       // no headset: normal monitor rendering
#define VR_PASS_EYE     1       // render the 3D world only, for vr->eye
#define VR_PASS_2D      2       // render 2D only (HUD, menus, cinematics) into the UI layer

#define VR_HAND_LEFT    0
#define VR_HAND_RIGHT   1

// refdef flag: this scene is a first person world view that should get per-eye
// projection and eye offsets from the renderer
#define RDF_VRVIEW      ( 1 << 8 )

// refdef flag: the view through a weapon's scope, rendered into the "*vrScope" texture
// between trap_R_VR_BeginScope and trap_R_VR_EndScope (first person models are left out)
#define RDF_VRSCOPE     ( 1 << 9 )

// usercmd_t.vrFlags (co-op): dual pistols, the off hand's gun fires
#define VRF_AKIMBO_LEFT 4096
// usercmd_t.vrFlags (co-op): 2 bits, which weapon size (BG_VR_WeaponScale)
#define VRF_SCALE_SHIFT 13
#define VRF_SCALE_MASK  ( 3 << VRF_SCALE_SHIFT )

// usercmd_t.vrFlags
#define VRF_ACTIVE      1       // the player is in VR: vr* fields are valid
#define VRF_TWOHANDED   2
#define VRF_GRIP_LEFT   4
#define VRF_GRIP_RIGHT  8
#define VRF_RIGHTHANDED 16
#define VRF_MUZZLE      32      // vrMuzzle is valid
#define VRF_HAND_LEFT   64      // vrHand[VR_HAND_LEFT] is tracked
#define VRF_HAND_RIGHT  128
#define VRF_MANUAL      256     // the player wants to reload by hand
#define VRF_SPREAD_SHIFT 9      // 2 bits: aim spread setting, see BG_VR_SpreadScale
#define VRF_SPREAD_MASK ( 3 << VRF_SPREAD_SHIFT )
#define VRF_SIGHTS      2048    // aiming down the sights: steadier
// usercmd_t vrFlags2
#define VRF2_AKIMBO_BOTH 1      // dual wielding, both triggers held: the guns take turns
#define VRF2_GUN_IN_OFF  2      // the off hand holds the gun alone (vrGun is where)
#define VRF2_TRIG_LEFT   4      // triggers held, by VR_HAND_* (the free hand's works the action)
#define VRF2_TRIG_RIGHT  8
#define VRF2_THROWAIM    16     // grenades and knives land on the reticle (vr_throwAim)

// playerState_t.vrState
#define VRS_MANUAL      0x0001  // current weapon is reloaded by hand
#define VRS_CHAMBERED   0x0002
#define VRS_MAGOUT      0x0004
#define VRS_HELD_SHIFT  3       // 2 bits: item in the off hand
#define VRS_CHARGEHELD  0x0020
#define VRS_CANGRAB     0x0040  // the off hand is on something it can take hold of
#define VRS_PULL_SHIFT  8       // 8 bits: slide / bolt pull 0..255
#define VRS_DROP_SHIFT  16      // 8 bits: magazine drop counter
#define VRS_PULSE_L_SHIFT 24    // 4 bits: bumped for a buzz in the left controller
#define VRS_PULSE_R_SHIFT 28    // 4 bits: bumped for a buzz in the right controller

// entityState_t.vrFlags (co-op): a VR player's hands, for everyone else's view of them
#define VRE_ACTIVE      1
#define VRE_HAND_LEFT   2       // vrHand[VR_HAND_LEFT] is tracked
#define VRE_HAND_RIGHT  4
#define VRE_RIGHTHANDED 8
#define VR_NET_HANDS    1       // this build sends them

// refEntity renderfx: don't draw hand / arm / sleeve / glove surfaces (first person weapon in VR)
#define RF_VR_NOHANDS   ( 1 << 12 )
// refEntity renderfx: don't draw arm / sleeve surfaces, keep the hand (VR hands on a full body)
#define RF_VR_NOSLEEVES ( 1 << 13 )
// refEntity renderfx: draw only the hand / arm surfaces (a mirrored copy of a gun's hand)
#define RF_VR_ONLYHANDS ( 1 << 14 )
// refEntity renderfx: the axes are mirrored (one negated), cull the other faces
#define RF_VR_MIRRORED  ( 1 << 15 )
// refEntity renderfx: don't draw the hand itself (palm, fingers), keep the sleeve
#define RF_VR_NOPALMS   ( 1 << 16 )

// refEntity_t.vrBendMagic: vrBend holds an elbow bend for the arm surfaces
#define VR_BEND_MAGIC   0x56524231

typedef struct vrShared_s {
	//
	// written by the engine every frame
	//
	int     active;             // a headset frame is being produced
	int     pass;               // VR_PASS_*
	int     eye;                // 0 = left, 1 = right during VR_PASS_EYE
	float   worldScale;         // game units per metre
	int     rightHanded;        // dominant hand is the right hand

	// Poses are in "play space": Quake axes (x forward, y left, z up), game units, origin
	// on the floor, already rotated by the artificial (thumbstick) turn.  The world yaw of
	// anything in play space is its play space yaw + SHORT2ANGLE( ps->delta_angles[YAW] ),
	// because the engine sends weaponAngles as the usercmd angles.
	vec3_t  hmdOrigin;
	vec3_t  hmdAngles;
	vec3_t  handOrigin[2];      // grip pose, indexed by VR_HAND_*
	vec3_t  handAngles[2];
	int     handValid[2];

	vec3_t  weaponAngles;       // aim sent as the usercmd angles (play space)
	int     twoHanded;          // off hand is steadying the weapon
	int     gunInOffHand;       // the gun hand let go (grip tap): the off hand holds the gun alone
	vec3_t  gunOrigin;          // play space, where the gun is held (the gun hand, or carried by the off hand)

	int     menuPointer;        // a menu is up: hide the gun, show the laser pointer
	int     pointerVisible;     // pointerStart/End are valid
	vec3_t  pointerStart;       // laser pointer, play space
	vec3_t  pointerEnd;

	//
	// written by cgame every frame
	//
	int     cutscene;           // scripted camera active, head look is free
	int     flatCutscene;       // ...and shown on a flat screen in front of the player (cl_vr.c)
	float   eyeHeight;          // eye height above ps->origin used for the view (game units)
	float   lean[2];            // world: how far the head is out over the body (client: wanted, cl_vr.c)
	float   leanUsed[2];        // world: that, stopped at walls, as the view used it (cgame)
	int     zoomed;             // scope / binocular view active
	int     twoHandAllowed;     // current weapon can be steadied with the off hand (not pistols)
	int     muzzleValid;        // muzzleOffset is set
	vec3_t  muzzleOffset;       // gun muzzle relative to the weapon hand, in the aim frame
	                            // (forward, left, up; game units), so shots leave the barrel

	//
	// manual reloading and grabbing: game runs it, cgame draws it, pmove (both sides)
	// reads manualReload/chambered so prediction matches the server
	//
	int     manualReload;       // current weapon is reloaded by hand: no automatic reload
	int     chambered;          // slide/bolt/pump worked: the gun can fire
	int     magOut;             // magazine / clip / cylinder out of the gun
	int     heldItem;           // off hand holds: 0 nothing, 1 magazine / clip, 2 shell
	int     chargeHeld;         // off hand is on the slide / bolt / pump
	float   chargePull;         // 0..1, how far it is pulled
	int     magDropCount;       // bumped whenever a magazine falls out (cgame drops one)
	int     grabbedEntity;      // body held by the off hand, -1 none

	//
	// controller buttons, written by the engine
	//
	int     ejectCount;         // bumped by the "drop magazine" gesture (the weapon hand's gun)
	int     ejectOffCount;      // dual wielding: the off hand's gun drops its magazine
	int     gripDown[2];        // grip buttons held, indexed by VR_HAND_*
	int     triggerDown[2];     // triggers held (the off hand's grabs the bolt / slide)

	//
	// haptics: cgame/game request a pulse, the engine plays and clears it
	//
	float   hapticAmplitude[2]; // 0..1, indexed by VR_HAND_*
	int     hapticDuration[2];  // msec

	//
	// written by the engine for every VR_PASS_EYE
	//
	vec3_t  eyeOffset;          // this eye from the head centre (forward, left, up; game units)

	//
	// written by cgame
	//
	int     ironSights;         // an eye is lined up behind the gun: steadier aim
	float   recoilPitch;        // the gun's kick, added to the aim (degrees, negative is up)
	float   recoilBack;         // and how far it jolts back in the hand (game units)
	int     dualWield;          // a pistol in each hand: the off hand's trigger fires its own
	int     binocsOut;          // binoculars picked from the weapon wheel: in the gun hand, the gun put away (cgame sets it)
	float   weaponScale;        // guns and hands drawn this size (1 = the flat game's models)
	int     supportValid;       // the held gun has a support hand: the off hand takes hold there
	float   supportRel[3];      // where, from the gun hand: forward, left, up along the gun (game units)

	//
	// written by the engine
	//
	int     akimboLeft;         // with dualWield: this shot is the off hand's gun
	int     akimboBoth;         // with dualWield: both triggers held, the guns take turns
	int     teleportAiming;     // the teleport stick is held: cgame shows the arc
	int     pingCount;          // bumped by the ping button: cgame marks where the gun points

	//
	// written by cgame
	//
	int     onLadder;           // hands on a ladder climb it (cl_vr.c)
	int     teleportValid;      // where the arc lands is somewhere to stand
	vec3_t  teleportDelta;      // from the player's origin to there (world, game units)
	float   yawOffset;          // play space yaw -> world yaw
} vrShared_t;

#endif // __VR_SHARED_H__
