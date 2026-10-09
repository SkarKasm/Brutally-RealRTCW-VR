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

// bg_vr.c -- VR weapon handling shared by game and cgame: how each weapon is reloaded,
// and where its grip, magazine and slide/bolt are, so the server's hand interactions and
// cgame's visuals agree exactly.
//
// The geometry comes from vr/weapons.cfg, which misc/vr/make_vr_assets.py measures from
// RealRTCW's own first person models and reload animations.

#include "../qcommon/q_shared.h"
#include "bg_public.h"
#include "../qcommon/vr_shared.h"

int trap_FS_FOpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode );
void trap_FS_Read( void *buffer, int len, fileHandle_t f );
void trap_FS_FCloseFile( fileHandle_t f );

#define VR_MAX_WEAPON_FILES 128

typedef struct {
	char            weap[MAX_QPATH];
	vrWeaponGeom_t  geom;
} vrWeaponFile_t;

static vrWeaponFile_t vrWeaponFiles[VR_MAX_WEAPON_FILES];
static int numVRWeaponFiles;
static qboolean vrWeaponsLoaded;

/*
=================
BG_VR_ReloadClass
=================
*/
int BG_VR_ReloadClass( int weapon ) {
	switch ( weapon ) {
	case WP_LUGER:
	case WP_SILENCER:
	case WP_COLT:
	case WP_TT33:
	case WP_HDM:
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_PPSH:
	case WP_MP34:
	case WP_MP44:
	case WP_FG42:
	case WP_FG42SCOPE:
	case WP_BAR:
	case WP_G43:
	case WP_M1941:
	case WP_M1941SCOPE:
	case WP_GARAND:
	case WP_SNOOPERSCOPE:
		return VRR_MAG;
	case WP_MAUSER:
	case WP_SNIPERRIFLE:
	case WP_MOSIN:
	case WP_DELISLE:
	case WP_DELISLESCOPE:
		return VRR_BOLT;
	case WP_M1GARAND:
		return VRR_ENBLOC;
	case WP_M97:
		return VRR_PUMP;
	case WP_AUTO5:
		return VRR_SHELLS;
	case WP_REVOLVER:
		return VRR_REVOLVER;
	default:
		return VRR_NONE;    // automatic reload (machine guns, flamethrower, akimbo, ...)
	}
}

/*
=================
BG_VR_LoadWeapons
=================
*/
static void BG_VR_LoadWeapons( void ) {
	static char buf[32768];
	fileHandle_t f;
	int len;
	char *p, *line, *next;
	int n;

	vrWeaponsLoaded = qtrue;
	numVRWeaponFiles = 0;

	len = trap_FS_FOpenFile( "vr/weapons.cfg", &f, FS_READ );
	if ( len <= 0 ) {
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	buf[len] = 0;
	trap_FS_FCloseFile( f );

	for ( p = buf; p && *p && numVRWeaponFiles < VR_MAX_WEAPON_FILES; p = next ) {
		vrWeaponFile_t *w = &vrWeaponFiles[numVRWeaponFiles];
		vrWeaponGeom_t *g = &w->geom;
		char name[MAX_QPATH];

		line = p;
		next = strchr( p, '\n' );
		if ( next ) {
			*next++ = 0;
		}
		if ( line[0] == '/' || line[0] == '\r' || !line[0] ) {
			continue;
		}

		memset( w, 0, sizeof( *w ) );
		if ( ( n = sscanf( line, "%63s %f %f %f %f %f %d %f %f %f %f %f %f %f %f %f %d %f %f %f %f %f %f %f %d %f %f %f %f",
					 name,
					 &g->grip[0], &g->grip[1], &g->grip[2], &g->pitch, &g->yaw,
					 &g->magPart, &g->mag[0], &g->mag[1], &g->mag[2],
					 &g->magLocal[0], &g->magLocal[1], &g->magLocal[2],
					 &g->magDir[0], &g->magDir[1], &g->magDir[2],
					 &g->chargePart, &g->charge[0], &g->charge[1], &g->charge[2],
					 &g->chargeDir[0], &g->chargeDir[1], &g->chargeDir[2], &g->travel,
					 &g->lensPart, &g->lens[0], &g->lens[1], &g->lens[2], &g->lensRadius ) ) < 6 ) {
			continue;
		}
		if ( n < 29 ) {
			g->lensPart = -1;       // made before scopes were measured
			g->lensRadius = 0;
		}
		Q_strncpyz( w->weap, name, sizeof( w->weap ) );
		g->valid = qtrue;
		numVRWeaponFiles++;
	}
}

/*
=================
BG_VR_WeaponGeom

Geometry for a weapon, with sensible stand-ins for anything the measurement didn't find
(the interactions are forgiving, an approximate spot is good enough).
=================
*/
void BG_VR_WeaponGeom( int weapon, qboolean vanillaGuns, vrWeaponGeom_t *out ) {
	const char *file = BG_GetWeaponFilename( weapon );
	char weap[MAX_QPATH];
	int i, cls = BG_VR_ReloadClass( weapon );

	if ( !vrWeaponsLoaded ) {
		BG_VR_LoadWeapons();
	}

	memset( out, 0, sizeof( *out ) );
	VectorSet( out->grip, 14, -6, -10 );
	out->magPart = out->chargePart = out->lensPart = -1;

	if ( file && *file ) {
		Com_sprintf( weap, sizeof( weap ), vanillaGuns ? "weapons/vanilla/%s" : "weapons/%s", file );
		for ( i = 0; i < numVRWeaponFiles; i++ ) {
			if ( !Q_stricmp( vrWeaponFiles[i].weap, weap ) ) {
				*out = vrWeaponFiles[i].geom;
				break;
			}
		}
	}

	// slide / bolt / pump handle
	if ( out->chargePart < 0 || out->travel < 1.0f ) {
		if ( cls == VRR_PUMP ) {
			VectorSet( out->charge, out->grip[0] + 18, out->grip[1] + 1, out->grip[2] + 2 );   // forestock
		} else {
			VectorSet( out->charge, out->grip[0] + 4, out->grip[1], out->grip[2] + 7 );        // back of the slide
		}
		VectorSet( out->chargeDir, -1, 0, 0 );
		out->travel = 6;
	}
	if ( out->travel < 4.0f ) {
		out->travel = 4.0f;
	}

	// where magazines / clips / shells go in
	if ( cls == VRR_BOLT ) {
		// stripper clips load from the top of the receiver, in front of the bolt
		VectorSet( out->mag, out->charge[0] + 3, out->charge[1], out->charge[2] + 3 );
		VectorSet( out->magDir, 0, 0, 1 );
	} else if ( cls == VRR_REVOLVER ) {
		// the cylinder sits above and in front of the grip
		VectorSet( out->mag, out->grip[0] + 6, out->grip[1], out->grip[2] + 4 );
		VectorSet( out->magDir, 0, 1, 0 );
	} else if ( out->magPart < 0 ) {
		VectorSet( out->mag, out->grip[0] + 6, out->grip[1], out->grip[2] - 4 );
		VectorSet( out->magDir, 0, 0, -1 );
	}
}

/*
=================
BG_VR_WeaponFrame

Pose of the first person weapon model so its grip sits in the hand: hand position,
world aim angles, plus the per weapon straightening and any fine tuning (adj: forward
left up pitch yaw roll, may be NULL). The correction is composed as a rotation in the
hand's frame, which stays stable at any angle.
=================
*/
void BG_VR_WeaponFrame( const vrWeaponGeom_t *g, const vec3_t hand, const vec3_t aimAngles, const float *adj,
						vec3_t origin, vec3_t axis[3] ) {
	vec3_t grip, corr, aim[3], local[3];
	int i, j;

	VectorCopy( g->grip, grip );
	VectorSet( corr, g->pitch, g->yaw, 0 );
	if ( adj ) {
		grip[0] -= adj[0];
		grip[1] -= adj[1];
		grip[2] -= adj[2];
		corr[PITCH] += adj[3];
		corr[YAW] += adj[4];
		corr[ROLL] += adj[5];
	}

	AnglesToAxis( aimAngles, aim );
	AnglesToAxis( corr, local );
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			axis[i][j] = local[i][0] * aim[0][j] + local[i][1] * aim[1][j] + local[i][2] * aim[2][j];
		}
	}

	// a smaller gun: the same pose, every axis shortened, so it shrinks towards the grip
	if ( g->scale > 0.0f && g->scale != 1.0f ) {
		VectorScale( axis[0], g->scale, axis[0] );
		VectorScale( axis[1], g->scale, axis[1] );
		VectorScale( axis[2], g->scale, axis[2] );
	}

	VectorCopy( hand, origin );
	VectorMA( origin, -grip[0], axis[0], origin );
	VectorMA( origin, -grip[1], axis[1], origin );
	VectorMA( origin, -grip[2], axis[2], origin );
}

/*
=================
BG_VR_WeaponScale / BG_VR_ScalePreset

The weapon sizes on offer (VR OPTIONS: Realistic, Larger, Smaller), multiplying the size
that makes each gun's hand life size (cg_vr.c); co-op sends which one in two bits.
=================
*/
static const float vrWeaponScales[3] = { 1.0f, 1.15f, 0.87f };

float BG_VR_WeaponScale( int preset ) {
	return ( preset >= 0 && preset < 3 ) ? vrWeaponScales[preset] : 1.0f;
}

int BG_VR_ScalePreset( float scale ) {
	int i, best = 0;

	for ( i = 1; i < 3; i++ ) {
		if ( fabs( scale - vrWeaponScales[i] ) < fabs( scale - vrWeaponScales[best] ) ) {
			best = i;
		}
	}
	return best;
}

// a point / direction of the first person model, in the world
void BG_VR_ModelPoint( const vec3_t origin, vec3_t axis[3], const vec3_t local, vec3_t out ) {
	VectorCopy( origin, out );
	VectorMA( out, local[0], axis[0], out );
	VectorMA( out, local[1], axis[1], out );
	VectorMA( out, local[2], axis[2], out );
}

void BG_VR_ModelDir( vec3_t axis[3], const vec3_t local, vec3_t out ) {
	VectorScale( axis[0], local[0], out );
	VectorMA( out, local[1], axis[1], out );
	VectorMA( out, local[2], axis[2], out );
}

/*
=================
BG_VR_InstantFire

Guns that fire the moment the trigger is pulled when aimed with a tracked controller.
The flat game delays every shot so the first person model can come up into position
first (fireDelayTime); a gun held in the hand is already up and aimed. Weapons where the
delay is part of how they work (the Venom's spin-up, throws) keep it.
=================
*/
qboolean BG_VR_InstantFire( int weapon ) {
	switch ( weapon ) {
	case WP_LUGER:
	case WP_SILENCER:
	case WP_COLT:
	case WP_AKIMBO:
	case WP_TT33:
	case WP_DUAL_TT33:
	case WP_REVOLVER:
	case WP_HDM:
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_PPSH:
	case WP_MP34:
	case WP_MAUSER:
	case WP_GARAND:
	case WP_MOSIN:
	case WP_DELISLE:
	case WP_M1GARAND:
	case WP_G43:
	case WP_M1941:
	case WP_MP44:
	case WP_FG42:
	case WP_BAR:
	case WP_M97:
	case WP_AUTO5:
	case WP_M30:
	case WP_BROWNING:
	case WP_MG42M:
	case WP_SNIPERRIFLE:
	case WP_SNOOPERSCOPE:
	case WP_DELISLESCOPE:
	case WP_FG42SCOPE:
	case WP_M1941SCOPE:
		return qtrue;
	default:
		return qfalse;
	}
}

/*
=================
BG_VR_HandsLow

Both hands down at the waist (dual wielding: the reload button reloads the pair). Game and
cgame both read it from the shared tracking, so prediction agrees.
=================
*/
qboolean BG_VR_HandsLow( const struct vrShared_s *vr ) {
	float drop;
	int h;

	if ( !vr || !vr->active ) {
		return qfalse;
	}
	drop = 0.40f * ( vr->worldScale > 0 ? vr->worldScale : 32.0f );   // 40cm under the eyes
	for ( h = 0; h < 2; h++ ) {
		if ( !vr->handValid[h] || vr->handOrigin[h][2] > vr->hmdOrigin[2] - drop ) {
			return qfalse;
		}
	}
	return qtrue;
}
