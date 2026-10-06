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

// cg_vrextras.c -- GORE & EXTRAS on the cgame side
//
// The settings come from the server (g_vrx in the serverinfo, see bg_vrextras.h), so
// everyone in a co-op game sees what the host chose.
//
// Shot reactions: someone alive flinches from a bullet, the torso and head knocked away
// from it and back over a moment, on top of whatever they're animating. Bodies are knocked
// harder where they're hit, and the shot that kills throws the body (cg_ragdoll.c asks
// CG_VRX_Knock how hard).

#include "cg_local.h"
#include "../game/bg_vrextras.h"

/*
=================
CG_VRX / CG_VRX_Gore
=================
*/
int CG_VRX( int which ) {
	static int cachedTime = -1;
	static char v[VRX_COUNT + 1];

	if ( which < 0 || which >= VRX_COUNT ) {
		return 0;
	}
	if ( cachedTime != cg.time ) {
		const char *s = Info_ValueForKey( CG_ConfigString( CS_SERVERINFO ), "g_vrx" );
		Q_strncpyz( v, s && s[0] ? s : VRX_DEFAULTS, sizeof( v ) );
		cachedTime = cg.time;
	}
	if ( which >= (int)strlen( v ) ) {
		return VRX_DEFAULTS[which] - '0';
	}
	return v[which] - '0';
}

qboolean CG_VRX_Gore( int which ) {
	return CG_VRX( VRX_GORE ) && CG_VRX( which );
}

/*
=================
CG_VRX_Knock

How hard a bullet knocks a body: a dead one (killing qfalse), or the one it just killed.
=================
*/
float CG_VRX_Knock( qboolean killing ) {
	switch ( CG_VRX( VRX_REACTIONS ) ) {
	case 0:
		return 1.0f;
	case 2:
		return killing ? 3.4f : 2.4f;
	default:
		return killing ? 2.3f : 1.6f;
	}
}

/*
=================
CG_VRX_Flinch / CG_VRX_FlinchAngles

A bullet hit someone alive at point, travelling along dir.
=================
*/
#define VRX_FLINCH_RISE     50

void CG_VRX_Flinch( int entityNum, const vec3_t point, const vec3_t dir ) {
	centity_t *cent;

	if ( !CG_VRX( VRX_REACTIONS ) || entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return;
	}
	cent = &cg_entities[entityNum];
	if ( cent->currentState.eType != ET_PLAYER || ( cent->currentState.eFlags & EF_DEAD ) ) {
		return;
	}
	cent->pe.vrFlinchTime = cg.time;
	VectorNormalize2( dir, cent->pe.vrFlinchDir );
	VectorCopy( point, cent->pe.vrFlinchPoint );
}

// in CG_PlayerAngles: the flinch on top of the torso and head angles (world angles)
void CG_VRX_FlinchAngles( centity_t *cent, vec3_t torsoAngles, vec3_t headAngles ) {
	int t = cg.time - cent->pe.vrFlinchTime, dur;
	float env, amp, fwdPush, sidePush, side, high;
	vec3_t fwd, right, ang, rel;

	if ( !cent->pe.vrFlinchTime || ( cent->currentState.eFlags & EF_DEAD ) ) {
		return;
	}
	if ( CG_VRX( VRX_REACTIONS ) == 2 ) {
		dur = 420;
		amp = 18.0f;
	} else {
		dur = 300;
		amp = 10.0f;
	}
	if ( !CG_VRX( VRX_REACTIONS ) || t < 0 || t >= dur ) {
		return;
	}
	if ( t < VRX_FLINCH_RISE ) {
		env = (float)t / VRX_FLINCH_RISE;
	} else {
		env = 1.0f - (float)( t - VRX_FLINCH_RISE ) / ( dur - VRX_FLINCH_RISE );
		env *= env;
	}

	VectorSet( ang, 0, torsoAngles[YAW], 0 );
	AngleVectors( ang, fwd, right, NULL );
	fwdPush = DotProduct( cent->pe.vrFlinchDir, fwd );      // + pushed forward, - back
	sidePush = DotProduct( cent->pe.vrFlinchDir, right );
	VectorSubtract( cent->pe.vrFlinchPoint, cent->lerpOrigin, rel );
	side = DotProduct( rel, right ) / 12.0f;                // which shoulder was hit
	side = side < -1 ? -1 : ( side > 1 ? 1 : side );
	high = rel[2] > 30.0f ? 1.6f : 1.0f;                    // a head shot snaps the head

	// leaning with the push, the hit side turned away
	torsoAngles[PITCH] += amp * env * fwdPush;
	torsoAngles[ROLL] += amp * 0.7f * env * sidePush;
	torsoAngles[YAW] += amp * 0.8f * env * side * ( fwdPush < 0 ? 1 : -1 );
	headAngles[PITCH] += amp * env * fwdPush * high;
	headAngles[ROLL] += amp * 0.7f * env * sidePush * high;
	headAngles[YAW] += amp * 0.8f * env * side * ( fwdPush < 0 ? 1 : -1 ) * high;
}
