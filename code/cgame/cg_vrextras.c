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

void CG_AddBloodTrails( vec3_t origin, vec3_t dir, int speed, int duration, int count, float randScale );     // cg_effects.c

#ifndef BONEFLAG_TAG
#define BONEFLAG_TAG        1       // qfiles.h: this bone is actually a tag
#endif

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

/*
==============================================================================

Bullet wounds (g_woundDecals, with the gore switch)

A hit leaves a wound on the limb it struck, alive or dead: kept in that bone's frame, so
it moves with the animation and with the ragdoll. The bullet's impact point is on the
hit box, not the body, so the wound goes on the nearest limb (bone to bone, each with
a rough thickness), on the side the shot came from.

==============================================================================
*/

#define VRX_MAX_WOUNDED     24
#define VRX_MAX_WOUNDS      12
#define VRX_MAX_BONES       128
#define VRX_MAX_SKELS       8

typedef enum {
	VRW_BULLET,
	VRW_THROAT,         // a cut across the front of the neck, bleeding
	VRW_BELLY           // opened, the guts hanging out, bleeding
} vrWoundKind_t;

typedef struct {
	int         kind;
	int         bone;           // -1: not on a bone yet (placed when the body is next drawn), -2: missed
	vec3_t      local, localN;  // in that bone's frame
	vec3_t      localT;         // and which way the wound runs (cuts)
	vec3_t      hit, dir;       // the shot, while not on a bone
	float       size, spin, aspect;
	int         shader;
	int         bleedEnd, nextSpurt;
} vrWound_t;

typedef struct {
	int         entityNum;
	int         lastSeen;
	qboolean    dead;
	int         num, next;      // num 0: free
	vrWound_t   w[VRX_MAX_WOUNDS];
} vrWounded_t;

// a model's bones: parents, which are tags, how thick the limb to each bone from its parent is
typedef struct {
	qhandle_t   model;
	int         n;
	int         parents[VRX_MAX_BONES];
	int         flags[VRX_MAX_BONES];
	float       radius[VRX_MAX_BONES];
	char        names[VRX_MAX_BONES][32];
} vrSkel_t;

static vrWounded_t vrWounded[VRX_MAX_WOUNDED];
static vrSkel_t vrSkels[VRX_MAX_SKELS];
static int vrSkelNext;
static qhandle_t vrWoundShaders[5];
static qhandle_t vrGutsModel;

// how thick the limb from this bone to its child is
static float VRX_LimbRadius( const char *name ) {
	static const struct { const char *part; float r; } limbs[] = {
		{ "Head", 4.2f }, { "Neck", 2.8f }, { "Spine", 6.5f }, { "Pelvis", 6.5f }, { "Clavicle", 3.5f },
		{ "UpperArm", 2.6f }, { "Forearm", 2.1f }, { "Hand", 1.4f }, { "Thigh", 3.8f },
		{ "Calf", 2.8f }, { "Foot", 2.0f },
	};
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( limbs ); i++ ) {
		if ( strstr( name, limbs[i].part ) ) {
			return limbs[i].r;
		}
	}
	return 0;
}

// the model's bones, and its animated pose into pose (when asked for)
static vrSkel_t *VRX_Skel( refEntity_t *body, float *pose ) {
	static int parents[VRX_MAX_BONES], flags[VRX_MAX_BONES];
	static char names[VRX_MAX_BONES * 32];
	static float scratch[VRX_MAX_BONES * 12];
	vrSkel_t *sk = NULL;
	int i, n;

	for ( i = 0; i < VRX_MAX_SKELS; i++ ) {
		if ( vrSkels[i].n > 0 && vrSkels[i].model == body->hModel ) {
			sk = &vrSkels[i];
			break;
		}
	}
	if ( sk && !pose ) {
		return sk;
	}
	n = trap_R_GetSkeleton( body, VRX_MAX_BONES, parents, flags, names, pose ? pose : scratch );
	if ( n <= 0 ) {
		return NULL;
	}
	if ( sk ) {
		return sk;
	}
	sk = &vrSkels[vrSkelNext];
	vrSkelNext = ( vrSkelNext + 1 ) % VRX_MAX_SKELS;
	sk->model = body->hModel;
	sk->n = n;
	for ( i = 0; i < n; i++ ) {
		sk->parents[i] = parents[i];
		sk->flags[i] = flags[i];
		sk->radius[i] = ( flags[i] & BONEFLAG_TAG ) || parents[i] < 0 ? 0 :
						VRX_LimbRadius( names + parents[i] * 32 );
		Q_strncpyz( sk->names[i], names + i * 32, sizeof( sk->names[i] ) );
	}
	return sk;
}

// a bone in the world: rotation (columns) and position, from the 12 floats of a pose
static void VRX_BoneWorld( const float *bm, qboolean isTag, vec3_t axis[3], const vec3_t origin,
						   vec3_t out[3], vec3_t pos ) {
	vec3_t m[3];
	int k;

	// tags store their axes as rows, bones as columns (see cg_ragdoll.c RD_Build)
	for ( k = 0; k < 3; k++ ) {
		if ( isTag ) {
			VectorSet( m[k], bm[k * 3 + 0], bm[k * 3 + 1], bm[k * 3 + 2] );
		} else {
			VectorSet( m[k], bm[0 * 3 + k], bm[1 * 3 + k], bm[2 * 3 + k] );
		}
	}
	for ( k = 0; k < 3; k++ ) {
		VectorScale( axis[0], m[k][0], out[k] );
		VectorMA( out[k], m[k][1], axis[1], out[k] );
		VectorMA( out[k], m[k][2], axis[2], out[k] );
	}
	VectorCopy( origin, pos );
	VectorMA( pos, bm[9], axis[0], pos );
	VectorMA( pos, bm[10], axis[1], pos );
	VectorMA( pos, bm[11], axis[2], pos );
}

/*
=================
CG_VRX_Wound

A bullet hit this character at point, travelling along dir.
=================
*/
static vrWound_t *VRX_NewWound( int entityNum, int kind, const vec3_t point, const vec3_t dir ) {
	vrWounded_t *wd = NULL, *spare = NULL;
	vrWound_t *w;
	int i;

	if ( entityNum < 0 || entityNum >= MAX_GENTITIES || !cg.snap ||
		 entityNum == cg.snap->ps.clientNum || cg_entities[entityNum].currentState.eType != ET_PLAYER ) {
		return NULL;
	}
	for ( i = 0; i < VRX_MAX_WOUNDED; i++ ) {
		if ( vrWounded[i].num && vrWounded[i].entityNum == entityNum ) {
			wd = &vrWounded[i];
			break;
		}
		// else a free slot, or the body seen longest ago
		if ( !spare || ( spare->num && ( !vrWounded[i].num || vrWounded[i].lastSeen < spare->lastSeen ) ) ) {
			spare = &vrWounded[i];
		}
	}
	if ( !wd ) {
		wd = spare;
		memset( wd, 0, sizeof( *wd ) );
		wd->entityNum = entityNum;
		wd->lastSeen = cg.time;
		wd->dead = ( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) != 0;
	}
	w = &wd->w[wd->next];
	wd->next = ( wd->next + 1 ) % VRX_MAX_WOUNDS;
	if ( wd->num < VRX_MAX_WOUNDS ) {
		wd->num++;
	}
	memset( w, 0, sizeof( *w ) );
	w->kind = kind;
	w->bone = -1;
	VectorCopy( point, w->hit );
	VectorNormalize2( dir, w->dir );
	w->size = 1.8f + random() * 1.0f;
	w->aspect = 1.0f;
	w->spin = random() * 2.0f * M_PI;
	w->shader = rand() % 5;
	return w;
}

void CG_VRX_Wound( int entityNum, const vec3_t point, const vec3_t dir ) {
	if ( CG_VRX_Gore( VRX_WOUNDS ) ) {
		VRX_NewWound( entityNum, VRW_BULLET, point, dir );
	}
}

/*
=================
CG_VRX_GoreEvent

EV_VR_GORE: a killing blow did something gory to a body (g_vrextras.c G_VRX_Kill).
=================
*/
void CG_VRX_GoreEvent( entityState_t *es ) {
	vrWound_t *w;

	if ( !CG_VRX_Gore( VRX_THROAT ) ) {
		return;
	}
	switch ( es->eventParm ) {
	case VRG_THROAT:
		w = VRX_NewWound( es->otherEntityNum, VRW_THROAT, es->pos.trBase, es->origin2 );
		if ( w ) {
			w->size = 0.9f;
			w->aspect = 3.2f;
		}
		break;
	case VRG_BELLY:
		w = VRX_NewWound( es->otherEntityNum, VRW_BELLY, es->pos.trBase, es->origin2 );
		if ( w ) {
			w->size = 3.6f;
		}
		break;
	}
}

// the first bone whose name has part in it
static int VRX_FindBone( vrSkel_t *sk, const char *part ) {
	int i;

	for ( i = 0; i < sk->n; i++ ) {
		if ( !( sk->flags[i] & BONEFLAG_TAG ) && strstr( sk->names[i], part ) ) {
			return i;
		}
	}
	return -1;
}

// a cut or opened belly: on the front of the neck / belly, the way the body faced
static void VRX_PlaceGore( vrWound_t *w, centity_t *cent, vrSkel_t *sk, vec3_t bAxis[][3], vec3_t bPos[] ) {
	vec3_t ang, fwd, right, c, rel;
	int b = VRX_FindBone( sk, w->kind == VRW_THROAT ? "Neck" : "Spine" );

	if ( b < 0 ) {
		w->bone = -2;
		return;
	}
	VectorSet( ang, 0, cent->lerpAngles[YAW], 0 );
	AngleVectors( ang, fwd, right, NULL );
	VectorMA( bPos[b], w->kind == VRW_THROAT ? 2.6f : 6.0f, fwd, c );
	if ( w->kind == VRW_BELLY ) {
		c[2] -= 3.0f;
	}
	VectorSubtract( c, bPos[b], rel );
	w->local[0] = DotProduct( rel, bAxis[b][0] );
	w->local[1] = DotProduct( rel, bAxis[b][1] );
	w->local[2] = DotProduct( rel, bAxis[b][2] );
	w->localN[0] = DotProduct( fwd, bAxis[b][0] );
	w->localN[1] = DotProduct( fwd, bAxis[b][1] );
	w->localN[2] = DotProduct( fwd, bAxis[b][2] );
	w->localT[0] = DotProduct( right, bAxis[b][0] );
	w->localT[1] = DotProduct( right, bAxis[b][1] );
	w->localT[2] = DotProduct( right, bAxis[b][2] );
	w->bone = b;
	w->bleedEnd = cg.time + ( w->kind == VRW_THROAT ? 3500 : 2500 );
	w->nextSpurt = cg.time;
}

// put a new wound on the limb the shot met
static void VRX_PlaceWound( vrWound_t *w, vrSkel_t *sk, vec3_t bAxis[][3], vec3_t bPos[] ) {
	float best = 1e9f, r = 0;
	int i, bi = -1;
	vec3_t c, start, d1, n, ab, rel;

	VectorMA( w->hit, -40.0f, w->dir, start );
	VectorScale( w->dir, 80.0f, d1 );
	for ( i = 0; i < sk->n; i++ ) {
		vec3_t d2, rr, p1, p2, e;
		float aa, ee, f, cc, bb, den, s, t, d;

		if ( sk->radius[i] <= 0 ) {
			continue;
		}
		// closest approach of the bullet's path and the limb
		VectorSubtract( bPos[i], bPos[sk->parents[i]], d2 );
		VectorSubtract( start, bPos[sk->parents[i]], rr );
		aa = DotProduct( d1, d1 );
		ee = DotProduct( d2, d2 );
		f = DotProduct( d2, rr );
		cc = DotProduct( d1, rr );
		bb = DotProduct( d1, d2 );
		den = aa * ee - bb * bb;
		s = den > 1e-6f ? ( bb * f - cc * ee ) / den : 0;
		s = s < 0 ? 0 : ( s > 1 ? 1 : s );
		t = ee > 1e-6f ? ( bb * s + f ) / ee : 0;
		t = t < 0 ? 0 : ( t > 1 ? 1 : t );
		VectorMA( start, s, d1, p1 );
		VectorMA( bPos[sk->parents[i]], t, d2, p2 );
		VectorSubtract( p1, p2, e );
		d = VectorLength( e ) - sk->radius[i];
		if ( d < best ) {
			best = d;
			bi = i;
			VectorCopy( p2, c );
			r = sk->radius[i];
		}
	}
	if ( bi < 0 || best > 10.0f ) {
		w->bone = -2;
		return;
	}
	// on the limb's surface, facing where the shot came from
	VectorSubtract( bPos[bi], bPos[sk->parents[bi]], ab );
	VectorNormalize( ab );
	VectorNegate( w->dir, n );
	VectorMA( n, -DotProduct( n, ab ), ab, n );
	if ( VectorNormalize( n ) < 0.1f ) {
		PerpendicularVector( n, ab );
	}
	VectorMA( c, r, n, c );
	// kept in the frame of the bone the limb hangs from
	i = sk->parents[bi];
	VectorSubtract( c, bPos[i], rel );
	w->local[0] = DotProduct( rel, bAxis[i][0] );
	w->local[1] = DotProduct( rel, bAxis[i][1] );
	w->local[2] = DotProduct( rel, bAxis[i][2] );
	w->localN[0] = DotProduct( n, bAxis[i][0] );
	w->localN[1] = DotProduct( n, bAxis[i][1] );
	w->localN[2] = DotProduct( n, bAxis[i][2] );
	w->bone = i;
}

// a throat or belly wound pumps blood for a while after the blow
static void VRX_Bleed( vrWound_t *w, const vec3_t p, const vec3_t n ) {
	vec3_t dir;

	if ( cg.time >= w->bleedEnd || cg.time < w->nextSpurt || !cg_blood.integer ) {
		return;
	}
	VectorCopy( n, dir );
	dir[2] -= 0.4f;
	VectorNormalize( dir );
	CG_AddBloodTrails( (float *)p, dir, 60 + rand() % 50, 700, 2, 0.35f );
	w->nextSpurt = cg.time + 130 + rand() % 150;
}

// the guts out of an opened belly: a loop of three lengths sagging from it (the gib is
// about 8 units long, along its x)
static void VRX_AddGuts( const vec3_t p, const vec3_t n, const vec3_t across ) {
	static const float bend[3][3] = {     // out, across, down of each length
		{ 0.6f, 0.0f, 0.8f }, { 0.2f, -0.5f, 0.9f }, { -0.1f, 0.6f, 0.7f }
	};
	refEntity_t re;
	vec3_t at, d;
	int i;

	if ( !vrGutsModel ) {
		vrGutsModel = trap_R_RegisterModel( "models/gibs/intestine.mdc" );
		if ( !vrGutsModel ) {
			vrGutsModel = -1;
		}
	}
	if ( vrGutsModel <= 0 ) {
		return;
	}
	VectorMA( p, 1.0f, n, at );
	for ( i = 0; i < 3; i++ ) {
		VectorScale( n, bend[i][0], d );
		VectorMA( d, bend[i][1], across, d );
		d[2] -= bend[i][2];
		VectorNormalize( d );

		memset( &re, 0, sizeof( re ) );
		re.hModel = vrGutsModel;
		VectorMA( at, 4.4f, d, re.origin );
		VectorCopy( re.origin, re.oldorigin );
		VectorCopy( re.origin, re.lightingOrigin );
		VectorCopy( d, re.axis[0] );
		VectorMA( across, -DotProduct( across, d ), d, re.axis[1] );
		VectorNormalize( re.axis[1] );
		CrossProduct( re.axis[0], re.axis[1], re.axis[2] );
		VectorScale( re.axis[0], 1.1f, re.axis[0] );     // a little longer, and thicker
		VectorScale( re.axis[1], 1.8f, re.axis[1] );
		VectorScale( re.axis[2], 1.8f, re.axis[2] );
		re.nonNormalizedAxes = qtrue;
		re.renderfx = RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &re );

		VectorMA( at, 8.2f, d, at );
	}
}

/*
=================
CG_VRX_AddWounds

A skeletal body ready to draw (after the ragdoll): its wounds.
=================
*/
void CG_VRX_AddWounds( centity_t *cent, refEntity_t *body ) {
	static float pose[VRX_MAX_BONES * 12];
	static vec3_t bAxis[VRX_MAX_BONES][3], bPos[VRX_MAX_BONES];
	vrWounded_t *wd = NULL;
	vrSkel_t *sk;
	const float *bones;
	vec3_t axis[3];
	qboolean dead = ( cent->currentState.eFlags & EF_DEAD ) != 0;
	int i, k;

	for ( i = 0; i < VRX_MAX_WOUNDED; i++ ) {
		if ( vrWounded[i].num && vrWounded[i].entityNum == cent->currentState.number ) {
			wd = &vrWounded[i];
			break;
		}
	}
	if ( !wd ) {
		return;
	}
	// back alive (a respawn, someone new in the slot): a clean body
	if ( wd->dead && !dead ) {
		wd->num = 0;
		return;
	}
	wd->dead = dead;
	wd->lastSeen = cg.time;
	if ( !CG_VRX_Gore( VRX_WOUNDS ) ) {
		return;
	}

	// the pose being drawn: the ragdoll's, or the animation's
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		sk = VRX_Skel( body, NULL );
		bones = body->ragdollBones;
	} else {
		sk = VRX_Skel( body, pose );
		bones = pose;
	}
	if ( !sk ) {
		return;
	}
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( body->axis[k], axis[k] );
	}
	for ( i = 0; i < sk->n; i++ ) {
		VRX_BoneWorld( bones + i * 12, ( sk->flags[i] & BONEFLAG_TAG ) != 0, axis, body->origin, bAxis[i], bPos[i] );
	}

	if ( !vrWoundShaders[0] ) {
		for ( k = 0; k < 5; k++ ) {
			vrWoundShaders[k] = trap_R_RegisterShader( va( "blood_dot%i", k + 1 ) );
		}
	}

	for ( i = 0; i < wd->num; i++ ) {
		vrWound_t *w = &wd->w[i];
		polyVert_t v[4];
		vec3_t p, n, u, s, t;

		if ( w->bone == -1 ) {
			if ( w->kind == VRW_BULLET ) {
				VRX_PlaceWound( w, sk, bAxis, bPos );
			} else {
				VRX_PlaceGore( w, cent, sk, bAxis, bPos );
			}
		}
		if ( w->bone < 0 || w->bone >= sk->n ) {
			continue;
		}
		VectorCopy( bPos[w->bone], p );
		for ( k = 0; k < 3; k++ ) {
			VectorMA( p, w->local[k], bAxis[w->bone][k], p );
		}
		VectorScale( bAxis[w->bone][0], w->localN[0], n );
		VectorMA( n, w->localN[1], bAxis[w->bone][1], n );
		VectorMA( n, w->localN[2], bAxis[w->bone][2], n );
		VectorNormalize( n );
		VectorMA( p, 0.3f, n, p );      // just off the skin

		if ( w->kind == VRW_BULLET ) {
			// a small turned square, flat on the skin
			PerpendicularVector( u, n );
			CrossProduct( n, u, s );
			VectorScale( u, cos( w->spin ) * w->size, t );
			VectorMA( t, sin( w->spin ) * w->size, s, t );
			CrossProduct( n, t, s );
		} else {
			// across the neck / belly
			VectorScale( bAxis[w->bone][0], w->localT[0], u );
			VectorMA( u, w->localT[1], bAxis[w->bone][1], u );
			VectorMA( u, w->localT[2], bAxis[w->bone][2], u );
			VectorMA( u, -DotProduct( u, n ), n, u );
			VectorNormalize( u );
			VectorScale( u, w->size * w->aspect, t );
			CrossProduct( n, u, s );
			VectorScale( s, w->size, s );
			VRX_Bleed( w, p, n );
			if ( w->kind == VRW_BELLY ) {
				VRX_AddGuts( p, n, u );
			}
		}
		for ( k = 0; k < 4; k++ ) {
			float a = ( k == 0 || k == 3 ) ? 1 : -1, b = k < 2 ? 1 : -1;
			v[k].xyz[0] = p[0] + t[0] * a + s[0] * b;
			v[k].xyz[1] = p[1] + t[1] * a + s[1] * b;
			v[k].xyz[2] = p[2] + t[2] * a + s[2] * b;
			v[k].st[0] = a > 0 ? 1 : 0;
			v[k].st[1] = b > 0 ? 1 : 0;
			v[k].modulate[0] = 150;
			v[k].modulate[1] = 20;
			v[k].modulate[2] = 20;
			v[k].modulate[3] = 255;
		}
		trap_R_AddPolyToScene( vrWoundShaders[w->shader], 4, v );
		// and wound the other way round: the shader only shows one side
		{
			polyVert_t r[4];
			for ( k = 0; k < 4; k++ ) {
				r[k] = v[3 - k];
			}
			trap_R_AddPolyToScene( vrWoundShaders[w->shader], 4, r );
		}
	}
}

// vrtestwound (test aid): a wound where the view meets a character, as a bullet would make
void CG_VRX_TestWound_f( void ) {
	trace_t tr;
	vec3_t end;

	VectorMA( cg.refdef.vieworg, 4096, cg.refdef.viewaxis[0], end );
	CG_Trace( &tr, cg.refdef.vieworg, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
	if ( tr.entityNum < MAX_GENTITIES && cg_entities[tr.entityNum].currentState.eType == ET_PLAYER ) {
		CG_VRX_Wound( tr.entityNum, tr.endpos, cg.refdef.viewaxis[0] );
		CG_Printf( "wound on %d\n", tr.entityNum );
	}
}
