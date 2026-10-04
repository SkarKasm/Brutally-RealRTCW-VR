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

// usercmd_t.vrFlags
#define VRF_ACTIVE      1       // the player is in VR: vr* fields are valid
#define VRF_TWOHANDED   2
#define VRF_GRIP_LEFT   4
#define VRF_GRIP_RIGHT  8
#define VRF_RIGHTHANDED 16
#define VRF_MUZZLE      32      // vrMuzzle is valid
#define VRF_HAND_LEFT   64      // vrHand[VR_HAND_LEFT] is tracked
#define VRF_HAND_RIGHT  128

// playerState_t.vrState
#define VRS_MANUAL      0x0001  // current weapon is reloaded by hand
#define VRS_CHAMBERED   0x0002
#define VRS_MAGOUT      0x0004
#define VRS_HELD_SHIFT  3       // 2 bits: item in the off hand
#define VRS_CHARGEHELD  0x0020
#define VRS_PULL_SHIFT  8       // 8 bits: slide / bolt pull 0..255
#define VRS_DROP_SHIFT  16      // 8 bits: magazine drop counter

// refEntity renderfx: don't draw hand / arm / sleeve / glove surfaces (first person weapon in VR)
#define RF_VR_NOHANDS   ( 1 << 12 )

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

	int     menuPointer;        // a menu is up: hide the gun, show the laser pointer
	int     pointerVisible;     // pointerStart/End are valid
	vec3_t  pointerStart;       // laser pointer, play space
	vec3_t  pointerEnd;

	//
	// written by cgame every frame
	//
	int     cutscene;           // scripted camera active, head look is free
	float   eyeHeight;          // eye height above ps->origin used for the view (game units)
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
	int     ejectCount;         // bumped by the "drop magazine" gesture
	int     gripDown[2];        // grip buttons held, indexed by VR_HAND_*

	//
	// haptics: cgame/game request a pulse, the engine plays and clears it
	//
	float   hapticAmplitude[2]; // 0..1, indexed by VR_HAND_*
	int     hapticDuration[2];  // msec
} vrShared_t;

#endif // __VR_SHARED_H__
