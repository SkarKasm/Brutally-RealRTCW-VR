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

// cg_vrrecoil.c -- recoil for guns held in tracked hands (RealRTCW-VR)
//
// Each shot kicks the gun's muzzle up on a stiff spring that brings it back. The kick goes
// into the aim the engine sends (cgVR->recoilPitch, cl_vr.c), so it's what the bullets do
// too: a burst climbs, a slow rifle settles before the next shot. The gun also jolts back
// in the hand a little. Steadying it with the other hand halves it. The flat game's canned
// firing animations, which threw the gun around the hand, aren't used in VR.

#include "cg_local.h"

vmCvar_t vr_recoil;

static struct {
	float   angle;      // degrees the muzzle is kicked up
	float   vel;
	int     time;
} rc;

void CG_VR_RecoilInit( void ) {
	memset( &rc, 0, sizeof( rc ) );
	trap_Cvar_Register( &vr_recoil, "vr_recoil", "1", CVAR_ARCHIVE );
}

/*
==============
CG_VR_RecoilKick

The local player fired. The kick follows how slowly the gun fires: automatic weapons get
a small one each shot, pistols a firm one, rifles and shotguns a big one.
==============
*/
void CG_VR_RecoilKick( int weapon ) {
	float kick;

	switch ( weapon ) {
	case WP_KNIFE:
	case WP_GRENADE_LAUNCHER:
	case WP_GRENADE_PINEAPPLE:
	case WP_DYNAMITE:
	case WP_FLAMETHROWER:
	case WP_TESLA:
		return;
	default:
		break;
	}
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS || !cgVR ) {
		return;
	}
	trap_Cvar_Update( &vr_recoil );
	if ( vr_recoil.value <= 0 ) {
		return;
	}

	kick = ammoTable[weapon].nextShotTime / 90.0f;
	if ( kick < 1.0f ) {
		kick = 1.0f;
	} else if ( kick > 8.0f ) {
		kick = 8.0f;
	}
	if ( cgVR->twoHanded ) {
		kick *= 0.5f;
	}
	kick *= vr_recoil.value;

	// a critically damped spring given this push peaks close to 'kick' degrees
	rc.vel += kick * 47.0f;
}

/*
==============
CG_VR_RecoilUpdate

Every frame: run the spring, tell the engine (aim) and the gun pose (jolt back).
==============
*/
void CG_VR_RecoilUpdate( void ) {
	float dt, h, acc;

	if ( !cgVR ) {
		return;
	}
	dt = ( cg.time - rc.time ) * 0.001f;
	rc.time = cg.time;
	if ( dt < 0.0f || dt > 0.1f ) {
		rc.angle = rc.vel = 0;      // a pause, a restart, a new level
	} else {
		while ( dt > 0.0f ) {
			h = dt > 0.004f ? 0.004f : dt;
			acc = -300.0f * rc.angle - 35.0f * rc.vel;
			rc.vel += acc * h;
			rc.angle += rc.vel * h;
			dt -= h;
		}
	}
	if ( rc.angle > 30.0f ) {
		rc.angle = 30.0f;
	}
	{
		static int next;
		char dbg[8];
		trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
		if ( atoi( dbg ) == 6 && rc.angle > 0.05f && cg.time >= next ) {
			next = cg.time + 30;
			CG_Printf( "VR recoil: %.2f degrees\n", rc.angle );
		}
	}
	cgVR->recoilPitch = -rc.angle;      // Quake pitch: negative is up
	cgVR->recoilBack = rc.angle * 0.15f;
}
