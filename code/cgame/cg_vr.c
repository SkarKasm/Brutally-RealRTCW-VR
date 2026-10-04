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

// cg_vr.c -- headset view, hands and weapon placement for cgame.
//
// The engine tracks the head and hands in "play space" (see vr_shared.h). cgame turns
// that into the world: the head becomes the view, the hands get world positions from
// their offset to the head, and the gun is drawn where the weapon hand is.

#include "cg_local.h"

cgVRState_t cgvr;

static vmCvar_t vr_drawHands;

/*
=================
CG_VR_Active
=================
*/
qboolean CG_VR_Active( void ) {
	return cgVR && cgVR->active;
}

qboolean CG_VR_EyePass( void ) {
	return cgVR && cgVR->active && cgVR->pass == VR_PASS_EYE;
}

qboolean CG_VR_2DPass( void ) {
	return cgVR && cgVR->active && cgVR->pass == VR_PASS_2D;
}

int CG_VR_DominantHand( void ) {
	return ( !cgVR || cgVR->rightHanded ) ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

/*
=================
CG_VR_Init
=================
*/
/*
=================
CG_VR_LoadGrips

vr/weapon_grips.cfg (built by misc/vr/make_vr_assets.py): for each .weap file, the point
of the first person model where the trigger hand holds it, "forward left up".
=================
*/
#define VR_MAX_GRIPS 128

static struct {
	char    weap[MAX_QPATH];
	vec3_t  grip;
	float   pitch, yaw;     // straightens melee weapons that are held at an angle
} vrGrips[VR_MAX_GRIPS];
static int numVRGrips;

static void CG_VR_LoadGrips( void ) {
	static char buf[16384];
	fileHandle_t f;
	int len;
	char *p, *tok;

	numVRGrips = 0;
	len = trap_FS_FOpenFile( "vr/weapon_grips.cfg", &f, FS_READ );
	if ( len <= 0 ) {
		CG_Printf( S_COLOR_YELLOW "VR: vr/weapon_grips.cfg missing, guns use a default grip\n" );
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	buf[len] = 0;
	trap_FS_FCloseFile( f );

	p = buf;
	while ( numVRGrips < VR_MAX_GRIPS ) {
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		Q_strncpyz( vrGrips[numVRGrips].weap, tok, sizeof( vrGrips[0].weap ) );
		vrGrips[numVRGrips].grip[0] = atof( COM_Parse( &p ) );
		vrGrips[numVRGrips].grip[1] = atof( COM_Parse( &p ) );
		vrGrips[numVRGrips].grip[2] = atof( COM_Parse( &p ) );
		vrGrips[numVRGrips].pitch = atof( COM_ParseExt( &p, qfalse ) );
		vrGrips[numVRGrips].yaw = atof( COM_ParseExt( &p, qfalse ) );
		numVRGrips++;
	}
}

// grip point for the first person model of a weapon, qfalse if unknown
static qboolean CG_VR_WeaponGrip( int weapon, vec3_t grip, float *pitch, float *yaw ) {
	const char *file = BG_GetWeaponFilename( weapon );
	const char *weap;
	int i;

	if ( !file || !*file ) {
		return qfalse;
	}
	// the same choice CG_RegisterWeapon makes
	weap = cg_vanilla_guns.integer ? va( "weapons/vanilla/%s", file ) : va( "weapons/%s", file );
	for ( i = 0; i < numVRGrips; i++ ) {
		if ( !Q_stricmp( vrGrips[i].weap, weap ) ) {
			VectorCopy( vrGrips[i].grip, grip );
			*pitch = vrGrips[i].pitch;
			*yaw = vrGrips[i].yaw;
			return qtrue;
		}
	}
	return qfalse;
}

void CG_VR_Init( void ) {
	vmCvar_t dummy;

	memset( &cgvr, 0, sizeof( cgvr ) );

	// Fine tuning on top of the measured grip, "forward left up pitch yaw roll" in model
	// units/degrees. vr_gunOffset_<weapon number> applies to a single weapon.
	trap_Cvar_Register( &dummy, "vr_gunOffset", "0 0 0 0 0 0", CVAR_ARCHIVE );
	trap_Cvar_Register( &dummy, "vr_laserPointerColor", "1 0.25 0.15", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_drawHands, "vr_drawHands", "1", CVAR_ARCHIVE );

	// built by misc/vr/make_vr_assets.py into main/z_vr_assets.pk3
	cgvr.handModel[VR_HAND_LEFT] = trap_R_RegisterModel( "models/vr/hand_l.md3" );
	cgvr.handModel[VR_HAND_RIGHT] = trap_R_RegisterModel( "models/vr/hand_r.md3" );
	CG_VR_LoadGrips();
}

// tracked hand models are available and wanted
qboolean CG_VR_DrawHands( void ) {
	trap_Cvar_Update( &vr_drawHands );
	return vr_drawHands.integer && cgvr.handModel[VR_HAND_LEFT] && cgvr.handModel[VR_HAND_RIGHT];
}

/*
=================
CG_VR_AddHands

Both hands, each on its own controller.
=================
*/
void CG_VR_AddHands( void ) {
	refEntity_t ent;
	int h;

	if ( !cgvr.active || cgVR->menuPointer || cg.cameraMode || !CG_VR_DrawHands() ) {
		return;
	}

	for ( h = 0; h < 2; h++ ) {
		if ( !cgVR->handValid[h] || !cgvr.handModel[h] ) {
			continue;
		}
		memset( &ent, 0, sizeof( ent ) );
		ent.hModel = cgvr.handModel[h];
		VectorCopy( cgvr.handOrigin[h], ent.origin );
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		AnglesToAxis( cgvr.handAngles[h], ent.axis );
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &ent );
	}
}

/*
=================
CG_VR_BeginFrame
=================
*/
void CG_VR_BeginFrame( void ) {
	cgvr.active = CG_VR_Active();

	if ( !cgvr.active ) {
		return;
	}

	cgVR->cutscene = cg.cameraMode;
	if ( !cg.cameraMode ) {
		cgvr.cutsceneBaseValid = qfalse;
	}
}

// play space offset -> world offset
static void CG_VR_PlayToWorld( const vec3_t playOffset, vec3_t out ) {
	float s = sin( DEG2RAD( cgvr.yawOffset ) ), c = cos( DEG2RAD( cgvr.yawOffset ) );
	float x = playOffset[0], y = playOffset[1];

	out[0] = x * c - y * s;
	out[1] = x * s + y * c;
	out[2] = playOffset[2];
}

/*
=================
CG_VR_SetHead

Fixes the view for this frame: the head is at headOrigin, play space yaw is turned into
world yaw by yawOffset. Writes the head's world angles to viewAngles and places the hands.
=================
*/
void CG_VR_SetHead( const vec3_t headOrigin, float yawOffset, vec3_t viewAngles ) {
	vec3_t offset;
	int h;

	cgvr.yawOffset = yawOffset;
	VectorCopy( headOrigin, cgvr.headOrigin );

	viewAngles[PITCH] = cgVR->hmdAngles[PITCH];
	viewAngles[YAW] = AngleNormalize360( cgVR->hmdAngles[YAW] + yawOffset );
	viewAngles[ROLL] = cgVR->hmdAngles[ROLL];

	for ( h = 0; h < 2; h++ ) {
		VectorSubtract( cgVR->handOrigin[h], cgVR->hmdOrigin, offset );
		CG_VR_PlayToWorld( offset, offset );
		VectorAdd( headOrigin, offset, cgvr.handOrigin[h] );

		VectorCopy( cgVR->handAngles[h], cgvr.handAngles[h] );
		cgvr.handAngles[h][YAW] += yawOffset;
	}

	VectorCopy( cgVR->weaponAngles, cgvr.weaponAngles );
	cgvr.weaponAngles[YAW] += yawOffset;
}

/*
=================
CG_VR_CutsceneView

Scripted camera shots: the camera sets the position and direction, the player's head
looks around freely from there (and can lean a little).
=================
*/
void CG_VR_CutsceneView( vec3_t origin, vec3_t angles ) {
	vec3_t offset;
	float cameraYaw = angles[YAW];

	if ( !cgvr.active ) {
		return;
	}

	if ( !cgvr.cutsceneBaseValid ) {
		VectorCopy( cgVR->hmdOrigin, cgvr.cutsceneHmdBase );
		cgvr.cutsceneYawBase = cgVR->hmdAngles[YAW];
		cgvr.cutsceneBaseValid = qtrue;
	}

	// head movement since the shot started, relative to the camera's heading
	cgvr.yawOffset = cameraYaw - cgvr.cutsceneYawBase;
	VectorSubtract( cgVR->hmdOrigin, cgvr.cutsceneHmdBase, offset );
	CG_VR_PlayToWorld( offset, offset );
	VectorAdd( origin, offset, origin );

	CG_VR_SetHead( origin, cgvr.yawOffset, angles );
}

/*
=================
CG_VR_WeaponPose

Where to draw the first person hands+gun model so its grip sits in the player's hand.
=================
*/
void CG_VR_WeaponPose( int weapon, vec3_t origin, vec3_t angles ) {
	char buf[MAX_CVAR_VALUE_STRING];
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	vec3_t grip = { 14, -6, -10 }, fwd, right, up;
	float gripPitch = 0, gripYaw = 0;
	int dom = CG_VR_DominantHand();

	CG_VR_WeaponGrip( weapon, grip, &gripPitch, &gripYaw );

	trap_Cvar_VariableStringBuffer( va( "vr_gunOffset_%d", weapon ), buf, sizeof( buf ) );
	if ( !buf[0] ) {
		trap_Cvar_VariableStringBuffer( "vr_gunOffset", buf, sizeof( buf ) );
	}
	if ( buf[0] ) {
		sscanf( buf, "%f %f %f %f %f %f", &adj[0], &adj[1], &adj[2], &adj[3], &adj[4], &adj[5] );
	}
	grip[0] -= adj[0];
	grip[1] -= adj[1];
	grip[2] -= adj[2];

	VectorCopy( cgvr.weaponAngles, angles );
	angles[PITCH] += adj[3] + gripPitch;
	angles[YAW] += adj[4] + gripYaw;
	angles[ROLL] += adj[5];

	// move the model so its grip point lands on the palm
	AngleVectors( angles, fwd, right, up );
	VectorCopy( cgvr.handOrigin[dom], origin );
	VectorMA( origin, -grip[0], fwd, origin );
	VectorMA( origin, grip[1], right, origin );     // +left is -right
	VectorMA( origin, -grip[2], up, origin );
}

/*
=================
CG_VR_AddPointer

Laser pointer for menus opened over the game world.
=================
*/
void CG_VR_AddPointer( void ) {
	polyVert_t verts[4];
	vec3_t start, end, dir, toView, side;
	char buf[64];
	float col[3] = { 1, 0.25f, 0.15f };
	float width;
	int i;

	if ( !cgvr.active || !cgVR->menuPointer || !cgVR->pointerVisible ) {
		return;
	}

	VectorSubtract( cgVR->pointerStart, cgVR->hmdOrigin, start );
	CG_VR_PlayToWorld( start, start );
	VectorAdd( cgvr.headOrigin, start, start );
	VectorSubtract( cgVR->pointerEnd, cgVR->hmdOrigin, end );
	CG_VR_PlayToWorld( end, end );
	VectorAdd( cgvr.headOrigin, end, end );

	trap_Cvar_VariableStringBuffer( "vr_laserPointerColor", buf, sizeof( buf ) );
	sscanf( buf, "%f %f %f", &col[0], &col[1], &col[2] );

	width = 0.006f * cgVR->worldScale;
	VectorSubtract( end, start, dir );
	VectorSubtract( cg.refdef.vieworg, start, toView );
	CrossProduct( dir, toView, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		return;
	}
	VectorScale( side, width * 0.5f, side );

	VectorAdd( start, side, verts[0].xyz );
	VectorSubtract( start, side, verts[1].xyz );
	VectorSubtract( end, side, verts[2].xyz );
	VectorAdd( end, side, verts[3].xyz );
	for ( i = 0; i < 4; i++ ) {
		verts[i].st[0] = ( i == 1 || i == 2 ) ? 1 : 0;
		verts[i].st[1] = ( i >= 2 ) ? 1 : 0;
		verts[i].modulate[0] = (byte)( 255 * col[0] );
		verts[i].modulate[1] = (byte)( 255 * col[1] );
		verts[i].modulate[2] = (byte)( 255 * col[2] );
		verts[i].modulate[3] = 255;
	}
	trap_R_AddPolyToScene( cgs.media.whiteShader, 4, verts );
}

/*
=================
CG_VR_Haptic
=================
*/
void CG_VR_Haptic( int hand, float amplitude, int msec ) {
	if ( !CG_VR_Active() || hand < 0 || hand > 1 ) {
		return;
	}
	if ( amplitude > cgVR->hapticAmplitude[hand] ) {
		cgVR->hapticAmplitude[hand] = amplitude;
		cgVR->hapticDuration[hand] = msec;
	}
}
