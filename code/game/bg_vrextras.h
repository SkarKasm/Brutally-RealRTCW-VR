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

#endif
