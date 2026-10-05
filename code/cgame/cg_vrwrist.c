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

// cg_vrwrist.c -- the wrist display (RealRTCW-VR)
//
// Turn the back of the off hand's wrist towards the face, like checking a watch, and it
// shows health (top row), armour (middle) and the gun's rounds loaded and spare (bottom).
// vr_wristHud 2 shows it straight ahead all the time, for checking.

#include "cg_local.h"

vmCvar_t vr_wristHud;

static qhandle_t whDigits[11];

void CG_VR_WristInit( void ) {
	static const char *names[11] = {
		"gfx/2d/numbers/zero_32b", "gfx/2d/numbers/one_32b", "gfx/2d/numbers/two_32b",
		"gfx/2d/numbers/three_32b", "gfx/2d/numbers/four_32b", "gfx/2d/numbers/five_32b",
		"gfx/2d/numbers/six_32b", "gfx/2d/numbers/seven_32b", "gfx/2d/numbers/eight_32b",
		"gfx/2d/numbers/nine_32b", "gfx/2d/numbers/minus_32b"
	};
	int i;

	trap_Cvar_Register( &vr_wristHud, "vr_wristHud", "1", CVAR_ARCHIVE );
	// flat 2D shaders: drawn with vertex colours, on top of everything
	for ( i = 0; i < 11; i++ ) {
		whDigits[i] = trap_R_RegisterShaderNoMip( names[i] );
	}
}

// a quad on the panel: centre (x, y) in panel units, size w x h
static void WH_Quad( qhandle_t shader, const vec3_t origin, vec3_t axis[2], float x, float y, float w, float h,
					 const byte color[4] ) {
	polyVert_t v[4];
	static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };    // facing the viewer
	int i;

	for ( i = 0; i < 4; i++ ) {
		VectorCopy( origin, v[i].xyz );
		VectorMA( v[i].xyz, x + corner[i][0] * w * 0.5f, axis[0], v[i].xyz );
		VectorMA( v[i].xyz, y + corner[i][1] * h * 0.5f, axis[1], v[i].xyz );
		v[i].st[0] = corner[i][0] < 0 ? 0 : 1;
		v[i].st[1] = corner[i][1] < 0 ? 1 : 0;
		v[i].modulate[0] = color[0];
		v[i].modulate[1] = color[1];
		v[i].modulate[2] = color[2];
		v[i].modulate[3] = color[3];
	}
	trap_R_AddPolyToScene( shader, 4, v );

	// and wound the other way, whichever side these shaders cull
	{
		polyVert_t r[4];
		for ( i = 0; i < 4; i++ ) {
			r[i] = v[3 - i];
		}
		trap_R_AddPolyToScene( shader, 4, r );
	}
}

// a number, right aligned at x
static void WH_Number( const vec3_t origin, vec3_t axis[2], float x, float y, int value, const byte color[4] ) {
	char buf[16];
	int i, len;
	const float w = 0.75f, h = 1.1f;

	if ( value < 0 ) {
		value = 0;
	}
	Com_sprintf( buf, sizeof( buf ), "%d", value );
	len = strlen( buf );
	for ( i = 0; i < len; i++ ) {
		WH_Quad( whDigits[buf[i] - '0'], origin, axis, x - ( len - 1 - i ) * w * 0.9f, y, w, h, color );
	}
}

/*
==============
CG_VR_WristHUD
==============
*/
void CG_VR_WristHUD( void ) {
	static const byte red[4] = { 255, 80, 60, 255 };
	static const byte blue[4] = { 110, 170, 255, 255 };
	static const byte yellow[4] = { 255, 220, 90, 255 };
	playerState_t *ps = &cg.predictedPlayerState;
	vec3_t fwd, right, up, face, pos, toEye, axis[2];
	int off, weapon;

	if ( !cgvr.active || !CG_VR_EyePass() || !cgVR || cgVR->menuPointer || cg.cameraMode ) {
		return;
	}
	trap_Cvar_Update( &vr_wristHud );
	off = !CG_VR_DominantHand();
	if ( !vr_wristHud.integer || !cgVR->handValid[off] || ps->stats[STAT_HEALTH] <= 0 || !whDigits[0] ) {
		return;
	}

	// the back of the wrist: outward from the hand's side, behind the grip
	AngleVectors( cgvr.handAngles[off], fwd, right, up );
	if ( off == VR_HAND_LEFT ) {
		VectorScale( right, -1, face );
	} else {
		VectorCopy( right, face );
	}
	VectorMA( cgvr.handOrigin[off], -4.0f, fwd, pos );
	VectorMA( pos, 2.0f, face, pos );

	VectorSubtract( cg.refdef.vieworg, pos, toEye );
	if ( vr_wristHud.integer == 2 ) {
		VectorMA( cg.refdef.vieworg, 12.0f, cg.refdef.viewaxis[0], pos );     // testing: straight ahead
	} else if ( VectorNormalize( toEye ) > 36.0f || DotProduct( face, toEye ) < 0.6f ) {
		return;     // not being looked at
	}

	// readable: square to the view
	VectorScale( cg.refdef.viewaxis[1], -1, axis[0] );      // panel right
	VectorCopy( cg.refdef.viewaxis[2], axis[1] );           // panel up

	WH_Number( pos, axis, 1.9f, 1.4f, ps->stats[STAT_HEALTH], red );
	WH_Number( pos, axis, 1.9f, 0.0f, ps->stats[STAT_ARMOR], blue );

	weapon = ps->weapon;
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && weapon != WP_KNIFE ) {
		WH_Number( pos, axis, -0.6f, -1.4f, ps->ammoclip[BG_FindClipForWeapon( weapon )], yellow );
		WH_Number( pos, axis, 2.4f, -1.4f, ps->ammo[BG_FindAmmoForWeapon( weapon )], yellow );
	}
}
