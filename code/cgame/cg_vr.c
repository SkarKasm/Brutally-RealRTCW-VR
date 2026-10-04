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
static vmCvar_t vr_reticle;

static qboolean CG_VR_OneHanded( int weapon );

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
void CG_VR_Init( void ) {
	vmCvar_t dummy;

	memset( &cgvr, 0, sizeof( cgvr ) );

	// Fine tuning on top of the measured grip, "forward left up pitch yaw roll" in model
	// units/degrees. vr_gunOffset_<weapon number> applies to a single weapon.
	trap_Cvar_Register( &dummy, "vr_gunOffset", "0 0 0 0 0 0", CVAR_ARCHIVE );
	trap_Cvar_Register( &dummy, "vr_laserPointerColor", "1 0.25 0.15", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_drawHands, "vr_drawHands", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_reticle, "vr_reticle", "1", CVAR_ARCHIVE );

	// built by misc/vr/make_vr_assets.py into main/z_vr_assets.pk3
	cgvr.handModel[VR_HAND_LEFT] = trap_R_RegisterModel( "models/vr/hand_l.md3" );
	cgvr.handModel[VR_HAND_RIGHT] = trap_R_RegisterModel( "models/vr/hand_r.md3" );
	cgvr.lastMagDrop = cgVR ? cgVR->magDropCount : 0;
}

// the current weapon's measured geometry (see bg_vr.c)
static void CG_VR_Geom( int weapon, vrWeaponGeom_t *g ) {
	BG_VR_WeaponGeom( weapon, cg_vanilla_guns.integer, g );
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
	cgVR->twoHandAllowed = !CG_VR_OneHanded( cg.predictedPlayerState.weapon );
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
void CG_VR_WeaponPose( int weapon, vec3_t origin, vec3_t axis[3] ) {
	char buf[MAX_CVAR_VALUE_STRING];
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	vrWeaponGeom_t geom;

	trap_Cvar_VariableStringBuffer( va( "vr_gunOffset_%d", weapon ), buf, sizeof( buf ) );
	if ( !buf[0] ) {
		trap_Cvar_VariableStringBuffer( "vr_gunOffset", buf, sizeof( buf ) );
	}
	if ( buf[0] ) {
		sscanf( buf, "%f %f %f %f %f %f", &adj[0], &adj[1], &adj[2], &adj[3], &adj[4], &adj[5] );
	}

	CG_VR_Geom( weapon, &geom );
	BG_VR_WeaponFrame( &geom, cgvr.handOrigin[CG_VR_DominantHand()], cgvr.weaponAngles, adj, origin, axis );

	// remembered for the reload visuals
	cgvr.gunWeapon = weapon;
	VectorCopy( origin, cgvr.gunOrigin );
	AxisCopy( axis, cgvr.gunAxis );
}

/*
=================
CG_VR_WeaponPart

Hand reloading on the first person weapon's linked parts: the magazine is hidden while
it's out of the gun, the slide / bolt / pump slides back while pulled or locked open.
Returns qfalse if the part shouldn't be drawn.
=================
*/
qboolean CG_VR_WeaponPart( int weapon, int part, refEntity_t *parent, refEntity_t *ent ) {
	vrWeaponGeom_t geom;
	vec3_t dir;
	float amount = 0;
	int cls;

	if ( !cgvr.active || !cgVR->manualReload ) {
		return qtrue;
	}
	CG_VR_Geom( weapon, &geom );
	cls = BG_VR_ReloadClass( weapon );

	if ( part == geom.magPart && cgVR->magOut ) {
		return qfalse;
	}

	if ( part == geom.chargePart ) {
		if ( cgVR->chargeHeld ) {
			amount = cgVR->chargePull;
		} else if ( !cgVR->chambered && ( cls == VRR_MAG || cls == VRR_BOLT ) ) {
			amount = 1.0f;      // slide locked back / bolt open
		}
		if ( amount > 0 ) {
			BG_VR_ModelDir( parent->axis, geom.chargeDir, dir );
			VectorMA( ent->origin, amount * geom.travel, dir, ent->origin );
		}
	}
	return qtrue;
}

/*
=================
CG_VR_AddReloadItems

The magazine / clip / shell in the off hand, and magazines falling out of the gun.
=================
*/
void CG_VR_AddReloadItems( void ) {
	weaponInfo_t *weapon;
	vrWeaponGeom_t geom;
	refEntity_t ent;
	vec3_t magPoint;
	qhandle_t model = 0;
	int off = !CG_VR_DominantHand();

	if ( !cgvr.active || cgvr.gunWeapon <= WP_NONE ) {
		return;
	}
	weapon = &cg_weapons[cgvr.gunWeapon];
	CG_VR_Geom( cgvr.gunWeapon, &geom );
	if ( geom.magPart >= 0 && geom.magPart < W_MAX_PARTS ) {
		model = weapon->partModels[W_FP_MODEL][geom.magPart].model;
	}

	// a magazine fell out: let it drop
	if ( cgVR->magDropCount != cgvr.lastMagDrop ) {
		cgvr.lastMagDrop = cgVR->magDropCount;
		if ( model ) {
			localEntity_t *le = CG_AllocLocalEntity();
			refEntity_t *re = &le->refEntity;

			BG_VR_ModelPoint( cgvr.gunOrigin, cgvr.gunAxis, geom.mag, magPoint );
			BG_VR_ModelDir( cgvr.gunAxis, geom.magLocal, re->origin );
			VectorSubtract( magPoint, re->origin, re->origin );
			AxisCopy( cgvr.gunAxis, re->axis );
			re->hModel = model;
			re->renderfx = RF_MINLIGHT;

			le->leType = LE_FRAGMENT;
			le->startTime = cg.time;
			le->endTime = cg.time + 10000;
			le->pos.trType = TR_GRAVITY;
			le->pos.trTime = cg.time;
			VectorCopy( re->origin, le->pos.trBase );
			BG_VR_ModelDir( cgvr.gunAxis, geom.magDir, le->pos.trDelta );
			VectorScale( le->pos.trDelta, 60, le->pos.trDelta );
			le->bounceFactor = 0.25f;
			le->angles.trType = TR_STATIONARY;
			le->leBounceSoundType = LEBS_BRASS;
			le->leMarkType = LEMT_NONE;
		}
	}

	// held in the off hand, lined up with the gun so it goes straight in
	if ( cgVR->heldItem && cgVR->handValid[off] ) {
		memset( &ent, 0, sizeof( ent ) );
		ent.hModel = model ? model : cgs.media.smallgunBrassModel;
		AxisCopy( cgvr.gunAxis, ent.axis );
		if ( model ) {
			BG_VR_ModelDir( ent.axis, geom.magLocal, ent.origin );
			VectorSubtract( cgvr.handOrigin[off], ent.origin, ent.origin );
		} else {
			VectorCopy( cgvr.handOrigin[off], ent.origin );
		}
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &ent );
	}
}

/*
=================
CG_VR_SetMuzzle

Called with the first person gun's muzzle (tag_flash) every frame. The server fires from
the same spot relative to the weapon hand, so bullets leave the barrel.
=================
*/
void CG_VR_SetMuzzle( const vec3_t muzzle ) {
	vec3_t d, aim[3];
	int dom = CG_VR_DominantHand();

	VectorSubtract( muzzle, cgvr.handOrigin[dom], d );
	if ( VectorLength( d ) > 80.0f ) {
		cgVR->muzzleValid = qfalse;     // tag missing or the gun is mid animation somewhere odd
		return;
	}
	AnglesToAxis( cgvr.weaponAngles, aim );
	cgVR->muzzleOffset[0] = DotProduct( d, aim[0] );
	cgVR->muzzleOffset[1] = DotProduct( d, aim[1] );
	cgVR->muzzleOffset[2] = DotProduct( d, aim[2] );
	cgVR->muzzleValid = qtrue;
	VectorCopy( muzzle, cgvr.muzzle );
}

// weapons held in one hand: the off hand grip doesn't turn them into a rifle hold
static qboolean CG_VR_OneHanded( int weapon ) {
	switch ( weapon ) {
	case WP_NONE:
	case WP_KNIFE:
	case WP_LUGER:
	case WP_SILENCER:
	case WP_COLT:
	case WP_TT33:
	case WP_REVOLVER:
	case WP_HDM:
	case WP_AKIMBO:
	case WP_DUAL_TT33:
	case WP_GRENADE_LAUNCHER:
	case WP_GRENADE_PINEAPPLE:
	case WP_DYNAMITE:
	case WP_DYNAMITE_ENG:
	case WP_AIRSTRIKE:
	case WP_ARTY:
	case WP_POISONGAS:
	case WP_SMOKETRAIL:
	case WP_SMOKE_BOMB:
	case WP_HOLYCROSS:
		return qtrue;
	default:
		return qfalse;
	}
}

// weapons that fire bullets or projectiles along the barrel get an aim reticle
static qboolean CG_VR_HasReticle( int weapon ) {
	return !CG_VR_OneHanded( weapon ) || weapon == WP_LUGER || weapon == WP_SILENCER || weapon == WP_COLT ||
		   weapon == WP_TT33 || weapon == WP_REVOLVER || weapon == WP_HDM || weapon == WP_AKIMBO ||
		   weapon == WP_DUAL_TT33;
}

/*
=================
CG_VR_AddReticle

A dot where the shot would land: traced from the muzzle along the aim, like the server
fires it. Kept at the same apparent size at any distance.
=================
*/
void CG_VR_AddReticle( void ) {
	refEntity_t ent;
	trace_t tr;
	vec3_t start, end, aim[3], toEye;
	int weapon = cg.predictedPlayerState.weapon;
	float dist;

	trap_Cvar_Update( &vr_reticle );
	if ( !cgvr.active || !vr_reticle.integer || cgVR->menuPointer || cg.cameraMode ||
		 !CG_VR_HasReticle( weapon ) || cg.predictedPlayerState.stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	AnglesToAxis( cgvr.weaponAngles, aim );
	if ( cgVR->muzzleValid ) {
		VectorCopy( cgvr.muzzle, start );
	} else {
		VectorCopy( cgvr.handOrigin[CG_VR_DominantHand()], start );
	}
	VectorMA( start, 8192, aim[0], end );
	CG_Trace( &tr, start, NULL, NULL, end, cg.predictedPlayerState.clientNum, MASK_SHOT );
	if ( tr.startsolid ) {
		return;
	}

	// pull it a little towards the viewer so it doesn't sink into the surface
	VectorSubtract( cg.refdef.vieworg, tr.endpos, toEye );
	dist = VectorNormalize( toEye );
	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_SPRITE;
	VectorMA( tr.endpos, 2.0f, toEye, ent.origin );
	ent.radius = dist * 0.006f;     // about 0.7 degrees across
	if ( ent.radius < 0.4f ) {
		ent.radius = 0.4f;
	}
	ent.customShader = cgs.media.crosshairShader[0];
	ent.shaderRGBA[0] = 255;
	ent.shaderRGBA[1] = 255;
	ent.shaderRGBA[2] = 255;
	ent.shaderRGBA[3] = 255;
	ent.renderfx = RF_DEPTHHACK | RF_FIRST_PERSON;
	trap_R_AddRefEntityToScene( &ent );
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
