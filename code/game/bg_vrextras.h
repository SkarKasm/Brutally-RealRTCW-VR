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

// bg_vrextras.h -- the optional mechanics of GORE & EXTRAS, shared by game and cgame
//
// The game (g_vrextras.c) reads one cvar per setting and publishes them all in the
// serverinfo cvar g_vrx, a digit each, so every player's cgame does what the host chose.

#ifndef BG_VREXTRAS_H
#define BG_VREXTRAS_H

typedef enum {
	VRX_GORE,           // master switch for everything gory (0 off, 1 on)
	VRX_REACTIONS,      // shot reactions: 0 off, 1 normal, 2 strong
	VRX_WOUNDS,         // bullet wound decals on bodies and the living
	VRX_THROAT,         // throat slitting and disembowelment (killing blows)
	VRX_PROPGRAB,       // grab and throw props
	VRX_PROPHIT,        // thrown props: 0 nothing, 1 stun, 2 stun and damage
	VRX_DISMEMBER,      // 0 off, 1 explosions, 2 explosions and heavy weapons
	VRX_DECAP,          // knife decapitation of the dead
	VRX_HEADSCARE,      // thrown heads: 0 nothing, 1 scare, 2 scare and surrender
	VRX_COUNT
} vrExtra_t;

#define VRX_DEFAULTS    "111112212"     // a digit per setting, in that order

// EV_VR_GORE (eventParm): what happened to the body otherEntityNum, the blow along origin2
typedef enum {
	VRG_NONE,
	VRG_THROAT,         // throat cut
	VRG_BELLY,          // disembowelled
	VRG_DECAP,          // the head cut off a dead body
	VRG_COUNT
} vrGore_t;

// our own entities, told apart by s.dmgFlags (ET_GENERAL)
#define VRX_ENT_GORE    0x56524752      // a body's gore marker: s.otherEntityNum2 the body, s.density VRX_GB_*
#define VRX_ENT_HEAD    0x56524844      // a cut off head: s.otherEntityNum2 the body it came off

// gore marker bits
#define VRX_GB_HEAD         1       // head off
#define VRX_GB_LARM         2       // limbs off (dismemberment)
#define VRX_GB_RARM         4
#define VRX_GB_LLEG         8
#define VRX_GB_RLEG         16
#define VRX_GB_SCARED       32      // alive, running from a thrown head
#define VRX_GB_SURRENDER    64      // alive, weapon dropped, hands up

#endif
