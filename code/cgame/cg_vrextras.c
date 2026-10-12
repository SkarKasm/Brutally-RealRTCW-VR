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
void CG_LaunchGib( centity_t *cent, vec3_t origin, vec3_t angles, vec3_t velocity, qhandle_t hModel, float sizeScale, int breakCount );

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
#define VRX_MAX_WOUNDS      24
#define VRX_MAX_BONES       128
#define VRX_MAX_SKELS       8

typedef enum {
	VRW_BULLET,
	VRW_THROAT,         // a cut across the front of the neck, bleeding
	VRW_BELLY,          // opened, the guts hanging out, bleeding
	VRW_STUMP,          // the neck where the head came off, bleeding
	VRW_LIMB            // where an arm / leg came off (limb: which), bleeding, the limb flying off
} vrWoundKind_t;

typedef struct {
	int         kind;
	int         bone;           // -1: not on a bone yet (placed when the body is next drawn), -2: missed
	vec3_t      local, localN;  // in that bone's frame
	vec3_t      localT;         // and which way the wound runs (cuts)
	vec3_t      hit, dir;       // the shot, while not on a bone
	float       size, spin, aspect;
	float       flip;           // bullet holes: mirrored or not
	int         shader;
	int         bleedEnd, nextSpurt;
	int         limb, high;     // VRW_LIMB: which (VRX_GB_*), and cut at the shoulder / hip
} vrWound_t;

typedef struct {
	int         entityNum;
	int         lastSeen;
	qboolean    dead;
	int         num, next;      // num 0: free
	vrWound_t   w[VRX_MAX_WOUNDS];
	int         parts[VRW_REGION_COUNT];    // bullet holes in each part of the body (drawn as blood soaked into it)
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
static int vrxDownStyle[MAX_GENTITIES];      // how each one went down (from the gore markers)
static qboolean VRX_HeadHit( int entityNum, const vec3_t point, const vec3_t dir );
static void VRX_AddFleshCap( const vec3_t p, const vec3_t dir, float len, float width, const vec3_t light );
static qboolean VRX_AddSofStump( int which, const vec3_t p, const vec3_t dir, const vec3_t across, float width, float bone, const vec3_t light );
static qhandle_t VRX_StumpInside( void );
static vrSkel_t vrSkels[VRX_MAX_SKELS];
static int vrSkelNext;
static qhandle_t vrWoundShaders[5];
static qhandle_t vrGutsModel;
static float vrxPoses[MAX_CLIENTS][VRX_MAX_BONES * 12 + 16];     // poses we draw with: the renderer reads them at the end of the frame

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
	w->flip = ( rand() & 1 ) ? 1.0f : -1.0f;
	w->shader = rand() % 5;
	return w;
}

void CG_VRX_Wound( int entityNum, const vec3_t point, const vec3_t dir ) {
	char dbg[8];

	trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
	if ( CG_VRX_Gore( VRX_WOUNDS ) && entityNum >= 0 && entityNum < MAX_GENTITIES && cg.snap &&
		 entityNum != cg.snap->ps.clientNum && cg_entities[entityNum].currentState.eType == ET_PLAYER &&
		 VRX_HeadHit( entityNum, point, dir ) ) {
		if ( atoi( dbg ) == 3 ) {
			CG_Printf( "VR wound: %d in the head\n", entityNum );
		}
		return;     // a head wound instead
	}
	if ( atoi( dbg ) == 3 ) {
		CG_Printf( "VR wound: %d in the body\n", entityNum );
	}
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
static int vrxGoreBits[MAX_GENTITIES];      // (below: from the gore markers)

void CG_VRX_GoreEvent( entityState_t *es ) {
	vrWound_t *w;

	if ( es->eventParm == VRG_HEADPOP ) {
		// the head blown apart: skull and brains flying, the neck pumping
		centity_t *body = es->otherEntityNum >= 0 && es->otherEntityNum < MAX_CLIENTS ? &cg_entities[es->otherEntityNum] : NULL;
		vec3_t p, d;
		if ( !CG_VRX_Gore( VRX_DISMEMBER ) || !body ) {
			return;
		}
		if ( body->pe.headRefEnt.hModel ) {
			VectorMA( body->pe.headRefEnt.origin, 4.0f, body->pe.headRefEnt.axis[2], p );
		} else {
			VectorCopy( es->pos.trBase, p );
		}
		vrxGoreBits[es->otherEntityNum] |= VRX_GB_HEAD;     // gone now, not when the marker arrives
		CG_GibHead( p );
		CG_GibHead( p );        // (two lots: a skull's worth of bits and plenty of brain)
		CG_AddBloodTrails( p, tv( 0, 0, 1 ), 180, 900, 12, 0.9f );
		if ( VectorNormalize2( es->origin2, d ) > 0.1f ) {
			CG_AddBloodTrails( p, d, 260, 600, 8, 0.7f );   // out the far side
		}
		trap_S_StartSound( p, ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.gibSound );
		w = VRX_NewWound( es->otherEntityNum, VRW_STUMP, es->pos.trBase, es->origin2 );
		if ( w ) {
			w->size = 2.6f;
		}
		return;
	}

	if ( !CG_VRX_Gore( VRX_THROAT ) && !( es->eventParm == VRG_BELLY && es->density && CG_VRX_Gore( VRX_DISMEMBER ) ) ) {
		return;     // (a shotgun's disembowelling goes with dismemberment)
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
			w->size = es->density ? 4.6f : 3.6f;    // a shotgun tears a bigger hole than a knife
		}
		break;
	case VRG_LIMBS:
		{
			int b;
			for ( b = VRX_GB_LARM; b <= VRX_GB_RLEG; b <<= 1 ) {
				if ( !( es->density & b ) ) {
					continue;
				}
				w = VRX_NewWound( es->otherEntityNum, VRW_LIMB, es->pos.trBase, es->origin2 );
				if ( w ) {
					w->size = 2.4f;
					w->limb = b;
					w->high = ( es->frame & b ) != 0;
				}
			}
			CG_AddBloodTrails( es->pos.trBase, tv( 0, 0, 1 ), 140, 900, 8, 0.8f );
		}
		break;
	case VRG_DECAP:
		w = VRX_NewWound( es->otherEntityNum, VRW_STUMP, es->pos.trBase, es->origin2 );
		if ( w ) {
			w->size = 2.6f;
		}
		CG_AddBloodTrails( es->pos.trBase, tv( 0, 0, 1 ), 120, 900, 6, 0.6f );
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

// the first bone of what comes off with a limb
static const char *VRX_LimbRoot( int limb, qboolean high ) {
	switch ( limb ) {
	case VRX_GB_LARM: return high ? "L UpperArm" : "L Forearm";
	case VRX_GB_RARM: return high ? "R UpperArm" : "R Forearm";
	case VRX_GB_LLEG: return high ? "L Thigh" : "L Calf";
	default: return high ? "R Thigh" : "R Calf";
	}
}

/*
Ghoul 2 style dismemberment (after Raven's Ghoul 2 in SoF2 / Jedi Academy, which turns a
limb's surfaces off on the body and throws a copy of the model drawing only them): our
bodies are one mesh, so the renderer drops the triangles skinned to the limb's bones
(tr_animation.c R_VRX_Trailer), and the limb that came off is the same model again drawing
only those, tumbling on its own.
*/
#define VRX_CUT_MAGIC       0x56524354      // "VRCT"
#define VRX_CUT_WORDS       4               // 32 bones to a word (MDS_MAX_BONES 128)
#define VRX_CUT_FLOATS      ( 2 + VRX_CUT_WORDS )

// the bones of the limb from root down
static void VRX_CutMask( vrSkel_t *sk, int root, unsigned mask[VRX_CUT_WORDS] ) {
	int i, p;

	for ( i = 0; i < sk->n && i < VRX_CUT_WORDS * 32; i++ ) {
		for ( p = i; p >= 0 && p != root; p = sk->parents[p] ) {
		}
		if ( p == root ) {
			mask[i >> 5] |= 1u << ( i & 31 );
		}
	}
}

// after a pose: draw without the masked bones (mode 0) or only them (1)
static void VRX_WriteCut( float *t, int mode, const unsigned mask[VRX_CUT_WORDS] ) {
	union { float f; int i; } u;
	int k;

	u.i = VRX_CUT_MAGIC;
	t[0] = u.f;
	t[1] = mode;
	for ( k = 0; k < VRX_CUT_WORDS; k++ ) {
		u.i = (int)mask[k];
		t[2 + k] = u.f;
	}
}

static qboolean VRX_HasCut( const float *t ) {
	union { float f; int i; } u;

	u.f = t[0];
	return u.i == VRX_CUT_MAGIC;
}

#define VRX_MAX_LIMBS       8
#define VRX_LIMB_LIFE       30000

typedef struct {
	int start;
	refEntity_t ref;
	float pose[VRX_MAX_BONES * 12 + VRX_CUT_FLOATS + 2];
	vec3_t center;          // its middle in the model's space
	vec3_t pos, vel;        // its middle in the world
	vec3_t axis[3];
	vec3_t spinAxis;
	float spin;             // degrees a second
	qboolean resting;
	int nextDrip;
	qboolean jaw;           // a shot off jaw (the head drawn with only its jaw), not a limb
	int cap;                // the stump's cap (VRX_SOF_*), on the end that came off
	float capWidth, bone;
	vec3_t capPos, capDir, capAcross;   // where it came off, model space: the joint, down the limb
} vrLimb_t;

static vrLimb_t vrLimbs[VRX_MAX_LIMBS];
static int vrLimbNext;

// the limb from bone root of body (drawn with pose bones) flying off, thrown along along
static qboolean VRX_MeshLimb( vrSkel_t *sk, const refEntity_t *body, const float *bones, int root,
							  const vec3_t along, const vec3_t shot, int cap, float width, float bone ) {
	vrLimb_t *l;
	unsigned mask[VRX_CUT_WORDS] = { 0 };
	int i, k, count = 0;

	if ( !body || !bones || root < 0 || sk->n > VRX_MAX_BONES ) {
		return qfalse;
	}
	l = &vrLimbs[vrLimbNext];
	vrLimbNext = ( vrLimbNext + 1 ) % VRX_MAX_LIMBS;
	memset( l, 0, sizeof( *l ) );
	memcpy( l->pose, bones, sizeof( float ) * 12 * sk->n );
	VRX_CutMask( sk, root, mask );
	VRX_WriteCut( l->pose + sk->n * 12, 1, mask );
	for ( i = 0; i < sk->n; i++ ) {
		if ( ( mask[i >> 5] & ( 1u << ( i & 31 ) ) ) && !( sk->flags[i] & BONEFLAG_TAG ) ) {
			VectorAdd( l->center, bones + i * 12 + 9, l->center );
			count++;
		}
	}
	if ( !count ) {
		return qfalse;
	}
	VectorScale( l->center, 1.0f / count, l->center );
	// its stump: the joint it came off at, facing back up the limb's parent
	l->cap = cap;
	l->capWidth = width;
	l->bone = bone;
	VectorCopy( bones + root * 12 + 9, l->capPos );
	if ( sk->parents[root] >= 0 ) {
		VectorSubtract( bones + root * 12 + 9, bones + sk->parents[root] * 12 + 9, l->capDir );
	}
	if ( VectorNormalize( l->capDir ) < 0.01f ) {
		VectorSet( l->capDir, 0, 0, -1 );
	}
	PerpendicularVector( l->capAcross, l->capDir );
	l->ref = *body;
	l->ref.ragdollBones = l->pose;
	l->ref.ragdollMagic = RAGDOLL_MAGIC;
	l->ref.customShader = 0;
	l->ref.renderfx &= ~RF_THIRD_PERSON;
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( body->axis[k], l->axis[k] );
	}
	VectorCopy( body->origin, l->pos );
	for ( k = 0; k < 3; k++ ) {
		VectorMA( l->pos, l->center[k], body->axis[k], l->pos );
	}
	VectorScale( along, 140, l->vel );
	VectorMA( l->vel, 180, shot, l->vel );
	l->vel[0] += crandom() * 60;
	l->vel[1] += crandom() * 60;
	l->vel[2] += 160 + random() * 140;
	VectorSet( l->spinAxis, crandom(), crandom(), crandom() );
	if ( VectorNormalize( l->spinAxis ) < 0.1f ) {
		VectorSet( l->spinAxis, 0, 0, 1 );
	}
	l->spin = 400 + random() * 500;
	l->start = cg.time;
	return qtrue;
}

/*
A jaw shot off (Soldier of Fortune 2 blows a character's lower face away the same way it
does a limb): the head drawn again with only its jaw (tr_surface.c R_VRX_ClipMesh), flying
off like a limb.
*/
#define VRX_CLIP_MAGIC      0x5652434C      // "VRCL", as tr_surface.c
static const float *VRX_HeadClipData( int which );

static void VRX_JawGib( const refEntity_t *head, const vec3_t shot ) {
	static const vec3_t jawMiddle = { 3.0f, 0.0f, -0.4f };     // in the head's frame
	vrLimb_t *l;
	int k;

	if ( !head->hModel ) {
		return;
	}
	l = &vrLimbs[vrLimbNext];
	vrLimbNext = ( vrLimbNext + 1 ) % VRX_MAX_LIMBS;
	memset( l, 0, sizeof( *l ) );
	l->jaw = qtrue;
	l->ref = *head;
	l->ref.ragdollBones = VRX_HeadClipData( 3 );
	l->ref.ragdollMagic = VRX_CLIP_MAGIC;
	l->ref.customShader = 0;
	l->ref.renderfx &= ~( RF_THIRD_PERSON | RF_FIRST_PERSON );
	VectorCopy( jawMiddle, l->center );
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( head->axis[k], l->axis[k] );
	}
	VectorCopy( head->origin, l->pos );
	for ( k = 0; k < 3; k++ ) {
		VectorMA( l->pos, l->center[k], head->axis[k], l->pos );
	}
	VectorScale( shot, 170, l->vel );
	l->vel[0] += crandom() * 50;
	l->vel[1] += crandom() * 50;
	l->vel[2] += 90 + random() * 90;
	VectorSet( l->spinAxis, crandom(), crandom(), crandom() );
	if ( VectorNormalize( l->spinAxis ) < 0.1f ) {
		VectorSet( l->spinAxis, 0, 0, 1 );
	}
	l->spin = 500 + random() * 500;
	l->start = cg.time;
}

// every frame: the limbs that came off, falling, bouncing and lying where they land
/*
=================
CG_VRX_ShowModel_f

"vrshowmodel <model> [seconds]": a test aid, the model held up in front of the eye (its x
turned to face it) and slowly turning, to look at gore pieces close up.
=================
*/
static qhandle_t vrShowModel;
static int vrShowStart, vrShowEnd;

void CG_VRX_ShowModel_f( void ) {
	vrShowModel = trap_R_RegisterModel( CG_Argv( 1 ) );
	vrShowStart = cg.time;
	vrShowEnd = cg.time + ( trap_Argc() > 2 ? atoi( CG_Argv( 2 ) ) : 10 ) * 1000;
	CG_Printf( "vrshowmodel %s: %s\n", CG_Argv( 1 ), vrShowModel ? "ok" : "not found" );
}

static void VRX_AddShowModel( void ) {
	refEntity_t re;
	vec3_t angles;

	if ( !vrShowModel || cg.time > vrShowEnd ) {
		return;
	}
	memset( &re, 0, sizeof( re ) );
	re.hModel = vrShowModel;
	VectorMA( cg.refdef.vieworg, 20, cg.refdef.viewaxis[0], re.origin );
	VectorSet( angles, 0, cg.refdefViewAngles[YAW] + 180 + ( cg.time - vrShowStart ) * 0.03f, 0 );
	AnglesToAxis( angles, re.axis );
	VectorCopy( re.origin, re.lightingOrigin );
	VectorCopy( re.origin, re.oldorigin );
	trap_R_AddRefEntityToScene( &re );
}

void CG_VRX_AddLimbs( void ) {
	float dt = cg.frametime * 0.001f;
	vec3_t mins = { -3, -3, -3 }, maxs = { 3, 3, 3 }, end, dir;
	trace_t tr;
	int i, k;

	VRX_AddShowModel();
	if ( dt > 0.1f ) {
		dt = 0.1f;
	}
	for ( i = 0; i < VRX_MAX_LIMBS; i++ ) {
		vrLimb_t *l = &vrLimbs[i];

		if ( !l->start ) {
			continue;
		}
		if ( cg.time - l->start > VRX_LIMB_LIFE || cg.time < l->start ) {
			l->start = 0;
			continue;
		}
		if ( !l->resting && dt > 0 ) {
			l->vel[2] -= 800 * dt;
			VectorMA( l->pos, dt, l->vel, end );
			CG_Trace( &tr, l->pos, mins, maxs, end, -1, CONTENTS_SOLID );
			VectorCopy( tr.endpos, l->pos );
			if ( tr.fraction < 1 ) {
				float into = DotProduct( l->vel, tr.plane.normal );
				VectorMA( l->vel, -1.6f * into, tr.plane.normal, l->vel );
				VectorScale( l->vel, 0.45f, l->vel );
				l->spin *= 0.4f;
				if ( into < -40 ) {
					CG_ImpactMark( cgs.media.bloodDotShaders[rand() % 5], tr.endpos, tr.plane.normal, random() * 360, 1, 1, 1, 1, qfalse, 6 + random() * 4, qfalse, -1 );
				}
				if ( tr.plane.normal[2] > 0.7f && VectorLength( l->vel ) < 40 ) {
					l->resting = qtrue;
					l->spin = 0;
				}
			}
			if ( l->spin > 0 ) {
				for ( k = 0; k < 3; k++ ) {
					RotatePointAroundVector( dir, l->spinAxis, l->axis[k], l->spin * dt );
					VectorCopy( dir, l->axis[k] );
				}
			}
		}
		// the model's origin, from where its middle is
		VectorCopy( l->pos, l->ref.origin );
		for ( k = 0; k < 3; k++ ) {
			VectorMA( l->ref.origin, -l->center[k], l->axis[k], l->ref.origin );
			VectorCopy( l->axis[k], l->ref.axis[k] );
		}
		VectorCopy( l->pos, l->ref.lightingOrigin );
		l->ref.reFlags |= REFLAG_DEAD_LOD;
		l->ref.renderfx |= RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &l->ref );
		// its open end: the inside of it meat (not hollow), and Soldier of Fortune 2's
		// bloody stump over the cut, the bone sticking out of it
		if ( VRX_StumpInside() ) {
			refEntity_t in = l->ref;
			in.customSkin = 0;
			in.customShader = VRX_StumpInside();
			trap_R_AddRefEntityToScene( &in );
		}
		if ( !l->jaw ) {
			vec3_t p, d, a;
			VectorCopy( l->ref.origin, p );
			VectorClear( d );
			VectorClear( a );
			for ( k = 0; k < 3; k++ ) {
				VectorMA( p, l->capPos[k], l->axis[k], p );
				VectorMA( d, -l->capDir[k], l->axis[k], d );     // back where it came from
				VectorMA( a, l->capAcross[k], l->axis[k], a );
			}
			VRX_AddSofStump( l->cap, p, d, a, l->capWidth, l->bone, l->pos );
			if ( cg.time - l->start < 4000 && cg.time >= l->nextDrip && cg_blood.integer ) {
				CG_AddBloodTrails( p, d, 40 + rand() % 40, 700, 2, 0.4f );
				l->nextDrip = cg.time + 150 + rand() % 200;
			}
		} else if ( cg.time - l->start < 2500 && cg.time >= l->nextDrip && cg_blood.integer ) {
			CG_AddBloodTrails( l->pos, l->axis[2], 30 + rand() % 30, 600, 1, 0.5f );
			l->nextDrip = cg.time + 120 + rand() % 150;
		}
	}
}

// Soldier of Fortune 2's stump for a limb cut off at the shoulder / hip (high) or the elbow /
// knee, how wide it is (a little wider than the limb, so the cut's edge doesn't show), and how
// far its bone sticks out
enum { VRX_SOF_SHOULDER, VRX_SOF_ELBOW, VRX_SOF_HIP, VRX_SOF_KNEE, VRX_SOF_NECK, VRX_SOF_CAPS };

static int VRX_LimbCap( int limb, int high ) {
	if ( limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) {
		return high ? VRX_SOF_HIP : VRX_SOF_KNEE;
	}
	return high ? VRX_SOF_SHOULDER : VRX_SOF_ELBOW;
}

static float VRX_LimbCapWidth( int limb, int high ) {
	if ( limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) {
		return high ? 6.8f : 5.2f;
	}
	return high ? 5.0f : 4.1f;
}

static float VRX_LimbBone( int limb ) {
	return ( limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ? 3.4f : 2.8f;
}

// the limb flying off (RTCW's own gibs)
static void VRX_LimbGib( vrWound_t *w, centity_t *cent, const vec3_t at, const vec3_t along ) {
	static qhandle_t arm, leg;
	vec3_t vel, ang;

	if ( !arm ) {
		arm = trap_R_RegisterModel( "models/gibs/forearm.mdc" );
		leg = trap_R_RegisterModel( "models/gibs/leg.mdc" );
	}
	VectorScale( along, 120, vel );
	VectorMA( vel, 160, w->dir, vel );
	vel[0] += crandom() * 80;
	vel[1] += crandom() * 80;
	vel[2] += 180 + random() * 120;
	VectorSet( ang, random() * 360, random() * 360, random() * 360 );
	CG_LaunchGib( cent, (float *)at, ang, vel, ( w->limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ? leg : arm, 1.0f, 0 );
}

// a cut or opened belly: on the front of the neck / belly, the way the body faced
static void VRX_PlaceGore( vrWound_t *w, centity_t *cent, vrSkel_t *sk, vec3_t bAxis[][3], vec3_t bPos[],
						   const refEntity_t *body, const float *bones ) {
	vec3_t ang, fwd, right, c, rel;
	int b = VRX_FindBone( sk, w->kind == VRW_THROAT ? "Neck" : "Spine" );

	if ( b < 0 ) {
		w->bone = -2;
		return;
	}
	VectorSet( ang, 0, cent->lerpAngles[YAW], 0 );
	AngleVectors( ang, fwd, right, NULL );
	if ( w->kind == VRW_LIMB ) {
		// on the stump of the limb, facing down the limb; the rest of it flies off
		int root = VRX_FindBone( sk, VRX_LimbRoot( w->limb, w->high ) ), p;
		if ( root < 0 || sk->parents[root] < 0 ) {
			w->bone = -2;
			return;
		}
		p = sk->parents[root];
		VectorSubtract( bPos[root], bPos[p], fwd );
		VectorNormalize( fwd );
		VectorCopy( bPos[root], c );
		PerpendicularVector( right, fwd );
		if ( !VRX_MeshLimb( sk, body, bones, root, fwd, w->dir, VRX_LimbCap( w->limb, w->high ), VRX_LimbCapWidth( w->limb, w->high ), VRX_LimbBone( w->limb ) ) ) {
			VRX_LimbGib( w, cent, bPos[root], fwd );      // (no pose to cut it from)
		}
		b = p;
	} else if ( w->kind == VRW_STUMP ) {
		int h = VRX_FindBone( sk, "Head" );
		if ( h < 0 ) {
			w->bone = -2;
			return;
		}
		VectorSubtract( bPos[h], bPos[b], fwd );        // up the neck
		VectorNormalize( fwd );
		VectorCopy( bPos[h], c );
		PerpendicularVector( right, fwd );
	} else {
		VectorMA( bPos[b], w->kind == VRW_THROAT ? 2.6f : 6.0f, fwd, c );
	}
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
	w->bleedEnd = cg.time + ( w->kind == VRW_BELLY ? 2500 : w->kind == VRW_THROAT ? 3500 : 9000 );    // stumps bleed longest
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
	// which way the blood runs from it: down, along the skin (as he stood when hit)
	{
		vec3_t down = { 0, 0, -1 };
		VectorMA( down, -DotProduct( down, n ), n, down );
		if ( VectorNormalize( down ) < 0.1f ) {
			VectorNegate( ab, down );
		}
		w->localT[0] = DotProduct( down, bAxis[i][0] );
		w->localT[1] = DotProduct( down, bAxis[i][1] );
		w->localT[2] = DotProduct( down, bAxis[i][2] );
	}
	w->size = 3.0f + random() * 1.2f;
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
	if ( w->kind == VRW_LIMB || w->kind == VRW_STUMP ) {
		// a stump pumps harder and longer, fading as it goes
		float left = ( w->bleedEnd - cg.time ) / 9000.0f;
		CG_AddBloodTrails( (float *)p, dir, 70 + (int)( 90 * left ) + rand() % 40, 900, 3, 0.5f );
		w->nextSpurt = cg.time + 90 + rand() % 120 + (int)( 200 * ( 1.0f - left ) );
		return;
	}
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
==============================================================================

The crypt's torn soldier

crypt1.bsp has a soldier torn open, built into the map: misc/vr/make_vr_assets.py cuts its
pieces out into models (z_vr_assets.pk3, models/vr_gore/). A head shot kill gets the dead
nazi head with a hole in the forehead; a blast close by tears the body open, with the half
skinned head, ribs through the chest and the guts out; an opened belly spills the guts; a
cut off head leaves the torn neck. The torso pieces are in the torso's frame (x the way the
chest faces, z up the spine, the origin VRX_CRYPT_NECK below the neck).

==============================================================================
*/

#define VRX_CRYPT_NECK      11.4f

typedef struct {
	qboolean    registered;
	qhandle_t   headSkinned, ribs, guts, stump, cap, neck, hole, insideHead, insideBody;
	qhandle_t   sofCap[VRX_SOF_CAPS], sofBone, sofJaw, stumpInside;    // Soldier of Fortune 2's stumps
} vrxCrypt_t;

static vrxCrypt_t *VRX_CryptModels( void ) {
	static vrxCrypt_t m;

	if ( !m.registered ) {
		m.registered = qtrue;
		m.cap = trap_R_RegisterModel( "models/vr_gore/flesh_cap.md3" );
		m.hole = trap_R_RegisterModel( "models/vr_gore/headhole.md3" );
		m.headSkinned = trap_R_RegisterModel( "models/vr_gore/head_skinned.md3" );
		m.ribs = trap_R_RegisterModel( "models/vr_gore/ribs.md3" );
		m.guts = trap_R_RegisterModel( "models/vr_gore/guts.md3" );
		m.stump = trap_R_RegisterModel( "models/vr_gore/stump.md3" );
		m.neck = trap_R_RegisterModel( "models/vr_gore/neck_flat.md3" );
		m.insideHead = trap_R_RegisterShader( "vr_neckinside_head" );
		m.insideBody = trap_R_RegisterShader( "vr_neckinside_body" );
		// Soldier of Fortune 2's bloody stumps (z_vr_stumps.pk3, sof2_stumps.py)
		m.sofCap[VRX_SOF_SHOULDER] = trap_R_RegisterModel( "models/vr_gore/sof2_cap_shoulder.md3" );
		m.sofCap[VRX_SOF_ELBOW] = trap_R_RegisterModel( "models/vr_gore/sof2_cap_elbow.md3" );
		m.sofCap[VRX_SOF_HIP] = trap_R_RegisterModel( "models/vr_gore/sof2_cap_hip.md3" );
		m.sofCap[VRX_SOF_KNEE] = trap_R_RegisterModel( "models/vr_gore/sof2_cap_knee.md3" );
		m.sofCap[VRX_SOF_NECK] = trap_R_RegisterModel( "models/vr_gore/sof2_cap_neck.md3" );
		m.sofBone = trap_R_RegisterModel( "models/vr_gore/sof2_bone.md3" );
		m.sofJaw = trap_R_RegisterModel( "models/vr_gore/sof2_jawcap.md3" );
		m.stumpInside = trap_R_RegisterShader( "vr_stumpinside" );
	}
	return &m;
}

static qhandle_t VRX_StumpInside( void ) {
	return VRX_CryptModels()->stumpInside;
}

/*
=================
CG_VRX_GoreHead

The head about to be drawn on a body (already on its tag_head): the crypt's skinned one
instead, for a blast. qtrue when it was swapped (no hat then).
=================
*/
qboolean CG_VRX_GoreHead( centity_t *cent, refEntity_t *head ) {
	qhandle_t m = 0;

	if ( CG_VRX_GoreBits( cent ) & VRX_GB_BLOWN ) {
		m = VRX_CryptModels()->headSkinned;
	}
	if ( !m ) {
		return qfalse;
	}
	head->hModel = m;
	head->customSkin = 0;
	return qtrue;
}

qboolean CG_VRX_HeadSwapped( centity_t *cent ) {
	return ( CG_VRX_GoreBits( cent ) & VRX_GB_BLOWN ) != 0;
}

// torn flesh closing off a stump: out along dir from p, len long and width across
static void VRX_AddFleshCap( const vec3_t p, const vec3_t dir, float len, float width, const vec3_t light ) {
	refEntity_t re;
	vec3_t n;

	if ( !VRX_CryptModels()->cap ) {
		return;
	}
	memset( &re, 0, sizeof( re ) );
	re.hModel = VRX_CryptModels()->cap;
	VectorNormalize2( dir, n );
	VectorCopy( n, re.axis[0] );
	PerpendicularVector( re.axis[1], n );
	CrossProduct( re.axis[0], re.axis[1], re.axis[2] );
	VectorScale( re.axis[0], len / 5.8f, re.axis[0] );     // the cap is 5.8 long, 4.6 across
	VectorScale( re.axis[1], width / 4.6f, re.axis[1] );
	VectorScale( re.axis[2], width / 4.6f, re.axis[2] );
	re.nonNormalizedAxes = qtrue;
	VectorCopy( p, re.origin );
	VectorCopy( p, re.oldorigin );
	VectorCopy( light, re.lightingOrigin );
	re.renderfx = RF_LIGHTING_ORIGIN;
	trap_R_AddRefEntityToScene( &re );
}

/*
Soldier of Fortune 2's bloody stumps (Raven Software): the torn flesh caps its characters
show where a limb comes off (the "cap_*_off" surfaces of models/characters/average_sleeves,
texture models/characters/caps/caps), with its bone gib (models/characters/gore/bone_small)
sticking out of the middle, cut out into md3s by misc/vr/crypt_gore/sof2_stumps.py
(z_vr_stumps.pk3). A cap is 4.6 across, x out of the stump; the bone 5 long along x. At p,
facing dir, across the way its y goes; width across, the bone sticking out bone.
*/
static qboolean VRX_AddSofStump( int which, const vec3_t p, const vec3_t dir, const vec3_t across, float width, float bone, const vec3_t light ) {
	vrxCrypt_t *m = VRX_CryptModels();
	refEntity_t re;
	vec3_t n, a, b;
	float sc = width / 4.6f;
	int k;

	if ( which < 0 || which >= VRX_SOF_CAPS || !m->sofCap[which] ) {
		return qfalse;
	}
	VectorNormalize2( dir, n );
	VectorMA( across, -DotProduct( across, n ), n, a );
	if ( VectorNormalize( a ) < 0.01f ) {
		PerpendicularVector( a, n );
	}
	CrossProduct( n, a, b );

	memset( &re, 0, sizeof( re ) );
	re.hModel = m->sofCap[which];
	VectorScale( n, sc, re.axis[0] );
	VectorScale( a, sc, re.axis[1] );
	VectorScale( b, sc, re.axis[2] );
	re.nonNormalizedAxes = qtrue;
	VectorCopy( p, re.origin );
	VectorCopy( p, re.oldorigin );
	VectorCopy( light, re.lightingOrigin );
	re.renderfx = RF_LIGHTING_ORIGIN;
	trap_R_AddRefEntityToScene( &re );

	if ( bone > 0 && m->sofBone ) {
		// the bone, from inside the stump out past the meat
		re.hModel = m->sofBone;
		for ( k = 0; k < 3; k++ ) {
			re.axis[0][k] = n[k] * bone * 1.4f / 5.0f;
			re.axis[1][k] = a[k] * sc;
			re.axis[2][k] = b[k] * sc;
		}
		VectorMA( p, -0.4f * bone, n, re.origin );
		VectorCopy( re.origin, re.oldorigin );
		trap_R_AddRefEntityToScene( &re );
	}
	return qtrue;
}

// a flat cut through the neck at p, facing n, the front of the neck towards front
static void VRX_AddNeckCut( const vec3_t p, const vec3_t n, const vec3_t front, float width, const vec3_t light ) {
	refEntity_t re;

	if ( !VRX_CryptModels()->neck ) {
		return;
	}
	memset( &re, 0, sizeof( re ) );
	re.hModel = VRX_CryptModels()->neck;
	VectorNormalize2( n, re.axis[0] );
	VectorMA( front, -DotProduct( front, re.axis[0] ), re.axis[0], re.axis[1] );
	if ( VectorNormalize( re.axis[1] ) < 0.01f ) {
		PerpendicularVector( re.axis[1], re.axis[0] );
	}
	CrossProduct( re.axis[0], re.axis[1], re.axis[2] );
	VectorScale( re.axis[1], width / 4.6f, re.axis[1] );      // the disc is 4.6 across
	VectorScale( re.axis[2], width / 4.6f, re.axis[2] );
	re.nonNormalizedAxes = qtrue;
	VectorCopy( p, re.origin );
	VectorCopy( p, re.oldorigin );
	VectorCopy( light, re.lightingOrigin );
	re.renderfx = RF_LIGHTING_ORIGIN;
	trap_R_AddRefEntityToScene( &re );
}

/*
==============================================================================

Head wounds

A bullet that meets a head leaves a hole with the blood running from it (down, whichever
way the head lies), or, low on the face, blows the jaw off the way Soldier of Fortune 2
does: the jaw's triangles left out of the head (tr_surface.c R_VRX_ClipMesh) and flying off
on their own (VRX_JawGib), SoF2's torn flesh cap where it was, bleeding. Kept in the head's
own frame, so they work on every head and skin, and on a head that was cut off too. The
head is taken as an egg about its centre (RTCW's heads all have their neck at the origin,
the face along x).

==============================================================================
*/

#define VRX_MAX_HEADS       16
#define VRX_MAX_HOLES       4

typedef struct {
	int         entityNum;      // the body, -1 free
	int         lastSeen;
	qboolean    dead;
	int         numHoles;
	vec3_t      hole[VRX_MAX_HOLES], holeN[VRX_MAX_HOLES];  // on the head, in its frame
	int         jaw;            // the jaw's been shot off
	int         jawTime, nextSpurt;
} vrHeadWounds_t;

static vrHeadWounds_t vrHeads[VRX_MAX_HEADS];
static const vec3_t vrHeadCentre = { 0.9f, 0.0f, 3.4f };
static const vec3_t vrHeadRadii = { 4.3f, 3.4f, 4.8f };

static vrHeadWounds_t *VRX_HeadWounds( int entityNum, qboolean create ) {
	vrHeadWounds_t *spare = NULL;
	int i;

	for ( i = 0; i < VRX_MAX_HEADS; i++ ) {
		if ( vrHeads[i].lastSeen && vrHeads[i].entityNum == entityNum ) {
			return &vrHeads[i];
		}
		if ( !spare || ( spare->lastSeen && ( !vrHeads[i].lastSeen || vrHeads[i].lastSeen < spare->lastSeen ) ) ) {
			spare = &vrHeads[i];
		}
	}
	if ( !create ) {
		return NULL;
	}
	memset( spare, 0, sizeof( *spare ) );
	spare->entityNum = entityNum;
	spare->lastSeen = cg.time;
	spare->dead = ( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) != 0;
	return spare;
}

/*
The clips the renderer cuts heads with (tr_surface.c R_VRX_ClipMesh), in the head's frame:
the jaw (sof2_stumps.py JAW_*: in front of x 0.5, below the line through the mouth falling
towards the back, above the neck, and the lower teeth), the neck of a cut off head (below
z -2.3, where its stump goes), both, and the jaw alone (flying off).
*/
#define VRX_JAW_X0          0.5f
#define VRX_JAW_TOP         1.1f
#define VRX_JAW_FRONT       4.4f
#define VRX_JAW_SLOPE       0.35f
#define VRX_JAW_BOTTOM      -2.1f
#define VRX_NECK_CUT        -2.3f

static const float *VRX_HeadClipData( int which ) {
	static float clips[4][2 + 2 * ( 2 + 3 * 4 )];
	static qboolean built;

	if ( !built ) {
		static const float jaw[2 + 3 * 4] = {
			3, 1,
			1, 0, 0, VRX_JAW_X0,
			VRX_JAW_SLOPE, 0, -1, VRX_JAW_SLOPE * VRX_JAW_FRONT - VRX_JAW_TOP,
			0, 0, 1, VRX_JAW_BOTTOM
		};
		static const float neck[2 + 4] = { 1, 0, 0, 0, -1, -VRX_NECK_CUT };
		float *c;

		built = qtrue;
		c = clips[0]; c[0] = 0; c[1] = 1; memcpy( c + 2, jaw, sizeof( jaw ) );
		c = clips[1]; c[0] = 0; c[1] = 1; memcpy( c + 2, neck, sizeof( neck ) );
		c = clips[2]; c[0] = 0; c[1] = 2; memcpy( c + 2, jaw, sizeof( jaw ) ); memcpy( c + 2 + ARRAY_LEN( jaw ), neck, sizeof( neck ) );
		c = clips[3]; c[0] = 1; c[1] = 1; memcpy( c + 2, jaw, sizeof( jaw ) );
	}
	return clips[which];
}

// the jaw blown off (a bullet along dir): it flies off, blood bursting from where it was
static void VRX_ShootJaw( vrHeadWounds_t *hw, const refEntity_t *head, const vec3_t dir ) {
	vec3_t p, d;
	int k;

	hw->jaw = 1;
	hw->jawTime = cg.time;
	hw->nextSpurt = cg.time;
	if ( !CG_VRX_Gore( VRX_DISMEMBER ) && !CG_VRX_Gore( VRX_WOUNDS ) ) {
		return;
	}
	VRX_JawGib( head, dir );
	if ( cg_blood.integer ) {
		VectorCopy( head->origin, p );
		for ( k = 0; k < 3; k++ ) {
			static const float at[3] = { 3.2f, 0.0f, -0.2f };
			VectorMA( p, at[k], head->axis[k], p );
		}
		VectorNormalize2( dir, d );
		CG_AddBloodTrails( p, d, 150, 900, 8, 0.7f );
		VectorNegate( d, d );
		CG_AddBloodTrails( p, d, 80, 700, 4, 0.8f );
	}
}

/*
=================
CG_VRX_HeadClip

A head about to be drawn (the body entityNum's, severed when it was cut off): the pieces shot
off it left out.
=================
*/
void CG_VRX_HeadClip( int entityNum, refEntity_t *head, qboolean severed ) {
	vrHeadWounds_t *hw = VRX_HeadWounds( entityNum, qfalse );
	qboolean jaw = hw && hw->jaw && CG_VRX_Gore( VRX_WOUNDS );
	qboolean dead = ( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) != 0;

	if ( jaw && hw->dead && !dead && cg_entities[entityNum].currentState.eType == ET_PLAYER ) {
		jaw = qfalse;       // alive again (CG_VRX_AddHeadWounds starts a new face)
	}
	if ( severed && !VRX_CryptModels()->sofCap[VRX_SOF_NECK] ) {
		severed = qfalse;   // (no stump to close it with)
	}
	if ( !jaw && !severed ) {
		return;
	}
	head->ragdollBones = VRX_HeadClipData( severed ? ( jaw ? 2 : 1 ) : 0 );
	head->ragdollMagic = VRX_CLIP_MAGIC;
}

// a bullet hit this character at point (where it met the hit box), travelling along dir: did
// its path go through the head? (qtrue when it did) The hit box is much bigger than the body,
// so it is the path that counts, not where it met the box.
#define VRX_HEAD_SLACK      1.35f   // how far off the head (in head radii) a shot still counts
static qboolean VRX_HeadHit( int entityNum, const vec3_t point, const vec3_t dir ) {
	centity_t *cent = &cg_entities[entityNum];
	refEntity_t *head = &cent->pe.headRefEnt;
	vrHeadWounds_t *hw;
	vec3_t rel, d, e, n, ed, nd;
	float len, t, dd;
	int k;

	if ( !head->hModel || CG_VRX_Headless( cent ) ) {
		return qfalse;
	}
	VectorNormalize2( dir, nd );
	VectorMA( point, -40.0f, nd, rel );
	VectorSubtract( rel, head->origin, rel );
	// the path in the head's egg, where the head is the unit sphere
	for ( k = 0; k < 3; k++ ) {
		float axisLen = VectorLength( head->axis[k] );
		float sc = axisLen > 0.01f ? axisLen * axisLen : 1.0f;
		e[k] = ( DotProduct( rel, head->axis[k] ) / sc - vrHeadCentre[k] ) / vrHeadRadii[k];
		ed[k] = DotProduct( nd, head->axis[k] ) / sc / vrHeadRadii[k];
	}
	dd = DotProduct( ed, ed );
	if ( dd < 1e-6f ) {
		return qfalse;
	}
	t = -DotProduct( e, ed ) / dd;
	if ( t < 0 || t * VectorLength( nd ) > 120.0f ) {
		return qfalse;
	}
	VectorMA( e, t, ed, d );        // closest to the head's centre
	len = VectorLength( d );
	if ( len > VRX_HEAD_SLACK ) {
		return qfalse;
	}
	if ( len < 1.0f ) {
		// where it went in: back along the path to the surface
		VectorMA( d, -sqrt( ( 1.0f - len * len ) / dd ), ed, e );
	} else if ( len > 0.01f ) {
		VectorScale( d, 1.0f / len, e );    // a graze: the nearest point of the head
	} else {
		return qfalse;
	}
	for ( k = 0; k < 3; k++ ) {
		d[k] = e[k] * vrHeadRadii[k];       // (from the centre)
	}
	len = 1.0f;
	hw = VRX_HeadWounds( entityNum, qtrue );
	hw->lastSeen = cg.time;
	// on the egg's surface, facing out of it
	for ( k = 0; k < 3; k++ ) {
		d[k] = vrHeadCentre[k] + d[k] / len;
		n[k] = e[k] / len / vrHeadRadii[k];
	}
	VectorNormalize( n );
	// low on the face: often the jaw goes; anywhere now and then
	{
		char dbg[8];
		trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
		if ( atoi( dbg ) == 4 ) {
			CG_Printf( "VR head hit: %d at %.1f %.1f %.1f, jaw %s\n", entityNum, d[0], d[1], d[2], hw->jaw ? "already gone" : "shot off" );
		}
		if ( atoi( dbg ) == 4 && !hw->jaw ) {
			VRX_ShootJaw( hw, head, nd );   // (test aid: every head shot takes the jaw)
			return qtrue;
		}
	}
	// the lower face, from the front or the side: the jaw goes (now and then anywhere else)
	if ( !hw->jaw && ( ( d[2] < vrHeadCentre[2] - 1.2f && n[0] > 0.0f && random() < 0.85f ) || random() < 0.08f ) ) {
		VRX_ShootJaw( hw, head, nd );
		return qtrue;
	}
	if ( hw->numHoles < VRX_MAX_HOLES ) {
		VectorCopy( d, hw->hole[hw->numHoles] );
		VectorCopy( n, hw->holeN[hw->numHoles] );
		hw->numHoles++;
	}
	return qtrue;
}

/*
=================
CG_VRX_AddHeadWounds

A head just drawn (head: where, its model's frame), the body entityNum it belongs to.
=================
*/
void CG_VRX_AddHeadWounds( int entityNum, refEntity_t *head ) {
	vrHeadWounds_t *hw = VRX_HeadWounds( entityNum, qfalse );
	vrxCrypt_t *m = VRX_CryptModels();
	qboolean dead = ( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) != 0;
	refEntity_t re;
	vec3_t n, down, p;
	int i, k;

	if ( !hw ) {
		return;
	}
	if ( hw->dead && !dead && cg_entities[entityNum].currentState.eType == ET_PLAYER ) {
		hw->lastSeen = 0;       // alive again: a new face
		return;
	}
	hw->dead = dead;
	hw->lastSeen = cg.time;
	if ( !CG_VRX_Gore( VRX_WOUNDS ) ) {
		return;
	}
	if ( hw->jaw ) {
		// the jaw's gone (CG_VRX_HeadClip): Soldier of Fortune 2's torn flesh where it was, in
		// the head's own frame, and the inside of the head meat where the cut leaves it open
		if ( m->sofJaw ) {
			memset( &re, 0, sizeof( re ) );
			re.hModel = m->sofJaw;
			VectorCopy( head->origin, re.origin );
			VectorCopy( head->origin, re.oldorigin );
			AxisCopy( head->axis, re.axis );
			re.nonNormalizedAxes = head->nonNormalizedAxes;
			VectorCopy( head->lightingOrigin, re.lightingOrigin );
			re.renderfx = RF_LIGHTING_ORIGIN | ( head->renderfx & ( RF_THIRD_PERSON | RF_FIRST_PERSON ) );
			trap_R_AddRefEntityToScene( &re );
		}
		if ( m->stumpInside ) {
			re = *head;
			re.customSkin = 0;
			re.customShader = m->stumpInside;
			re.renderfx |= RF_LIGHTING_ORIGIN;
			trap_R_AddRefEntityToScene( &re );
		}
		// bleeding from it, hard at first
		if ( cg_blood.integer && cg.time >= hw->nextSpurt && cg.time < hw->jawTime + 7000 && cg.time >= hw->jawTime ) {
			static const float at[3] = { 2.8f, 0.0f, -0.3f };
			float left = 1.0f - ( cg.time - hw->jawTime ) / 7000.0f;
			VectorCopy( head->origin, p );
			VectorClear( n );
			for ( k = 0; k < 3; k++ ) {
				VectorMA( p, at[k], head->axis[k], p );
			}
			VectorScale( head->axis[0], 0.6f, n );
			VectorMA( n, -0.8f, head->axis[2], n );
			VectorNormalize( n );
			CG_AddBloodTrails( p, n, 40 + (int)( 70 * left ) + rand() % 30, 800, 2, 0.5f );
			hw->nextSpurt = cg.time + 110 + rand() % 120 + (int)( 250 * ( 1.0f - left ) );
		}
	}
	for ( i = 0; i < hw->numHoles && m->hole; i++ ) {
		VectorCopy( head->origin, p );
		VectorClear( n );
		for ( k = 0; k < 3; k++ ) {
			VectorMA( p, hw->hole[i][k], head->axis[k], p );
			VectorMA( n, hw->holeN[i][k], head->axis[k], n );
		}
		VectorNormalize( n );
		VectorMA( p, 0.15f, n, p );
		// the blood runs down: on a face looking up it runs off to the side it lies on
		VectorSet( down, 0, 0, -1 );
		VectorMA( down, -DotProduct( down, n ), n, down );
		if ( VectorNormalize( down ) < 0.2f ) {
			VectorNegate( head->axis[2], down );
			VectorMA( down, -DotProduct( down, n ), n, down );
			VectorNormalize( down );
		}
		memset( &re, 0, sizeof( re ) );
		re.hModel = m->hole;
		VectorCopy( n, re.axis[0] );
		VectorNegate( down, re.axis[2] );
		CrossProduct( re.axis[2], re.axis[0], re.axis[1] );
		VectorScale( re.axis[1], 1.2f, re.axis[1] );
		VectorScale( re.axis[2], 1.2f, re.axis[2] );
		re.nonNormalizedAxes = qtrue;
		VectorCopy( p, re.origin );
		VectorCopy( p, re.oldorigin );
		VectorCopy( head->lightingOrigin, re.lightingOrigin );
		re.renderfx = RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &re );
	}
}

static void VRX_AddCryptPiece( qhandle_t model, const vec3_t origin, vec3_t axis[3], const vec3_t light ) {
	refEntity_t re;

	if ( !model ) {
		return;
	}
	memset( &re, 0, sizeof( re ) );
	re.hModel = model;
	VectorCopy( origin, re.origin );
	VectorCopy( origin, re.oldorigin );
	VectorCopy( light, re.lightingOrigin );
	AxisCopy( axis, re.axis );
	re.renderfx = RF_LIGHTING_ORIGIN;
	trap_R_AddRefEntityToScene( &re );
}

/*
=================
CG_VRX_AddCryptGore

A dead body ready to draw (after the ragdoll and dismemberment): the torn soldier's pieces
on its torso.
=================
*/
void CG_VRX_AddCryptGore( centity_t *cent, refEntity_t *body ) {
	static float pose[VRX_MAX_BONES * 12];
	vrxCrypt_t *m;
	vrSkel_t *sk;
	const float *bones;
	vec3_t axis[3], bAxis[3], neckPos, pelvisPos, lPos, rPos, up, left, center, frame[3];
	int bits, neck, pelvis, la, ra, i, k;
	qboolean belly = qfalse;

	if ( !( cent->currentState.eFlags & EF_DEAD ) ) {
		return;
	}
	bits = CG_VRX_GoreBits( cent );
	for ( i = 0; i < VRX_MAX_WOUNDED; i++ ) {
		if ( vrWounded[i].num && vrWounded[i].entityNum == cent->currentState.number ) {
			for ( k = 0; k < vrWounded[i].num; k++ ) {
				belly |= vrWounded[i].w[k].kind == VRW_BELLY && vrWounded[i].w[k].bone >= 0;
			}
			break;
		}
	}
	if ( !( bits & ( VRX_GB_BLOWN | VRX_GB_HEAD ) ) && !belly ) {
		return;
	}
	m = VRX_CryptModels();

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
	neck = VRX_FindBone( sk, "Neck" );
	pelvis = VRX_FindBone( sk, "Pelvis" );
	la = VRX_FindBone( sk, "L UpperArm" );
	ra = VRX_FindBone( sk, "R UpperArm" );
	if ( neck < 0 || pelvis < 0 || la < 0 || ra < 0 ) {
		return;
	}
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( body->axis[k], axis[k] );
	}
	VRX_BoneWorld( bones + neck * 12, qfalse, axis, body->origin, bAxis, neckPos );
	VRX_BoneWorld( bones + pelvis * 12, qfalse, axis, body->origin, bAxis, pelvisPos );
	VRX_BoneWorld( bones + la * 12, qfalse, axis, body->origin, bAxis, lPos );
	VRX_BoneWorld( bones + ra * 12, qfalse, axis, body->origin, bAxis, rPos );

	// the torso's frame: up the spine, across the shoulders, the way the chest faces
	VectorSubtract( neckPos, pelvisPos, up );
	VectorSubtract( lPos, rPos, left );
	if ( VectorNormalize( up ) < 1.0f ) {
		return;
	}
	VectorMA( left, -DotProduct( left, up ), up, left );
	if ( VectorNormalize( left ) < 0.01f ) {
		return;
	}
	CrossProduct( left, up, frame[0] );
	VectorCopy( left, frame[1] );
	VectorCopy( up, frame[2] );
	VectorMA( neckPos, -VRX_CRYPT_NECK, up, center );

	if ( bits & VRX_GB_BLOWN ) {
		VRX_AddCryptPiece( m->ribs, center, frame, center );
	}
	if ( ( bits & VRX_GB_BLOWN ) || belly ) {
		VRX_AddCryptPiece( m->guts, center, frame, center );
	}
	if ( bits & VRX_GB_HEAD ) {
		// the cut through the neck, where the head came off: the body again, only its inside
		// faces, so the opening fills with meat right up to its edge, and Soldier of Fortune
		// 2's bloody stump over it, the spine sticking out
		int h = VRX_FindBone( sk, "Head" );
		vec3_t hPos, n;
		qboolean sof = qfalse;
		if ( h >= 0 ) {
			VRX_BoneWorld( bones + h * 12, qfalse, axis, body->origin, bAxis, hPos );
			VectorSubtract( hPos, neckPos, n );
			if ( VectorNormalize( n ) < 0.1f ) {
				VectorCopy( up, n );
			}
			VectorMA( hPos, 0.3f, n, hPos );
			sof = VRX_AddSofStump( VRX_SOF_NECK, hPos, n, frame[1], 4.6f, 2.0f, center );
		}
		if ( m->insideBody ) {
			refEntity_t re = *body;
			re.customSkin = 0;
			re.customShader = m->insideBody;
			trap_R_AddRefEntityToScene( &re );
		} else if ( sof ) {
		} else if ( h >= 0 && m->neck ) {
			VRX_AddNeckCut( hPos, n, frame[0], 4.6f, center );
		} else {
			VRX_AddCryptPiece( m->stump, center, frame, center );
		}
	}
}

/*
=================
VRX_AddBodyWounds

Bullet holes the way Ghoul 2 does skin gore (Soldier of Fortune 2, Jedi Academy): the body
drawn again with "vr_bodywound" (one draw for each of its textures), the renderer keeping only its triangles round each hole and
laying the hole's texture over them (tr_animation.c R_VRX_BodyWounds). It needs the pose, so
that goes first in the buffer, then the holes (model space, 12 floats each).
=================
*/
#define VRX_WOUND_MAGIC     0x56525744      // "VRWD", as tr_animation.c
#define VRX_BODYWOUND_SETS  4               // vr_bodywound, vr_bodywound2..4: the Soldier of Fortune 2 holes

static void VRX_AddBodyWounds( centity_t *cent, refEntity_t *body, vrSkel_t *sk, const float *bones,
							   float holes[][12], int numHoles ) {
	static float buf[MAX_CLIENTS][VRX_BODYWOUND_SETS][VRX_MAX_BONES * 12 + VRX_CUT_FLOATS + 2 + VRX_MAX_WOUNDS * 12];
	static qhandle_t shaders[VRX_BODYWOUND_SETS];
	union { float f; int i; } magic;
	refEntity_t ov;
	float *b;
	int n = cent->currentState.number, o, set, i, count;

	if ( !numHoles || n < 0 || n >= MAX_CLIENTS || sk->n > VRX_MAX_BONES ) {
		return;
	}
	if ( !shaders[0] ) {
		for ( set = 0; set < VRX_BODYWOUND_SETS; set++ ) {
			shaders[set] = trap_R_RegisterShader( set ? va( "vr_bodywound%i", set + 1 ) : "vr_bodywound" );
		}
	}
	// one texture to a draw: the body drawn again for each kind of hole it has
	for ( set = 0; set < VRX_BODYWOUND_SETS; set++ ) {
		b = buf[n][set];
		memcpy( b, bones, sizeof( float ) * 12 * sk->n );
		o = sk->n * 12;
		if ( VRX_HasCut( bones + o ) ) {     // no holes on a limb that's gone
			memcpy( b + o, bones + o, sizeof( float ) * VRX_CUT_FLOATS );
			o += VRX_CUT_FLOATS;
		}
		count = 0;
		for ( i = 0; i < numHoles; i++ ) {
			if ( (int)holes[i][11] == set ) {
				memcpy( b + o + 2 + count * 12, holes[i], sizeof( float ) * 12 );
				count++;
			}
		}
		if ( !count ) {
			continue;
		}
		magic.i = VRX_WOUND_MAGIC;
		b[o] = magic.f;
		b[o + 1] = count;
		ov = *body;
		ov.customShader = shaders[set];
		ov.customSkin = 0;
		ov.ragdollBones = b;
		ov.ragdollMagic = RAGDOLL_MAGIC;
		trap_R_AddRefEntityToScene( &ov );
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
	float holes[VRX_MAX_WOUNDS][12];
	int i, k, numHoles = 0;

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
				VRX_PlaceGore( w, cent, sk, bAxis, bPos, body, bones );
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
		if ( w->kind == VRW_BULLET ) {
			// a bullet hole: laid on the body's own skin by the renderer (VRX_AddBodyWounds)
			if ( numHoles < VRX_MAX_WOUNDS ) {
				float *h = holes[numHoles++];
				vec3_t down, rel;
				VectorScale( bAxis[w->bone][0], w->localT[0], down );
				VectorMA( down, w->localT[1], bAxis[w->bone][1], down );
				VectorMA( down, w->localT[2], bAxis[w->bone][2], down );
				VectorSubtract( p, body->origin, rel );
				for ( k = 0; k < 3; k++ ) {     // into the model's own space
					h[k] = DotProduct( rel, body->axis[k] );
					h[3 + k] = DotProduct( n, body->axis[k] );
					h[6 + k] = DotProduct( down, body->axis[k] );
				}
				h[9] = w->size * 1.3f;     // (the Soldier of Fortune 2 holes splash out round them)
				h[10] = w->flip;
				h[11] = w->shader % VRX_BODYWOUND_SETS;     // which hole
			}
			continue;
		}
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
			if ( w->kind == VRW_BELLY && !VRX_CryptModels()->guts ) {
				VRX_AddGuts( p, n, u );     // (with the crypt's guts, CG_VRX_AddCryptGore spills those)
			}
			if ( w->kind == VRW_LIMB ) {
				// Soldier of Fortune 2's bloody stump closing it (thighs are thicker than arms)
				if ( !VRX_AddSofStump( VRX_LimbCap( w->limb, w->high ), p, n, u, VRX_LimbCapWidth( w->limb, w->high ), VRX_LimbBone( w->limb ), p ) ) {
					VRX_AddFleshCap( p, n, 2.4f, ( w->limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ? ( w->high ? 6.0f : 4.6f ) : ( w->high ? 4.4f : 3.6f ), p );
				}
			} else if ( w->kind == VRW_STUMP && dead && ( CG_VRX_GoreBits( cent ) & VRX_GB_HEAD ) ) {
				continue;   // the neck's stump is drawn with the crypt gore (CG_VRX_AddCryptGore)
			} else if ( w->kind == VRW_STUMP ) {
				if ( !VRX_AddSofStump( VRX_SOF_NECK, p, n, u, 4.6f, 2.0f, p ) ) {
					VRX_AddFleshCap( p, n, 2.2f, 4.6f, p );
				}
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
	VRX_AddBodyWounds( cent, body, sk, bones, holes, numHoles );
}

// vrtesthit larm|rarm|lleg|rleg|gut|chest (test aid): the nearest living character shot there
static qboolean VRX_TestPoint( int c, int k, vec3_t out ) {
	orientation_t or;

	if ( !CG_VRX_BodyPoints( c, k / 4, &or ) ) {
		return qfalse;
	}
	VectorCopy( k % 4 ? or.axis[k % 4 - 1] : or.origin, out );
	return qtrue;
}

void CG_VRX_TestHit_f( void ) {
	const char *part = CG_Argv( 1 );
	int i, best = -1, a, b;
	float d, bestD = 1e9f;
	vec3_t p, q, dir;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		centity_t *c = &cg_entities[i];
		if ( i == cg.snap->ps.clientNum || !c->currentValid || c->currentState.eType != ET_PLAYER ||
			 ( c->currentState.eFlags & EF_DEAD ) ) {
			continue;
		}
		d = Distance( c->lerpOrigin, cg.refdef.vieworg );
		if ( d < bestD ) {
			bestD = d;
			best = i;
		}
	}
	if ( best < 0 ) {
		return;
	}
	if ( !Q_stricmp( part, "head" ) ) {     // vrtesthit head: a shot to the face from where we stand
		refEntity_t *hd = &cg_entities[best].pe.headRefEnt;
		if ( !hd->hModel ) {
			return;
		}
		VectorCopy( hd->origin, p );
		VectorSubtract( p, cg.refdef.vieworg, dir );
		VectorNormalize( dir );
		CG_VRX_Wound( best, p, dir );
		CG_Printf( "hit %d in the head\n", best );
		return;
	}
	a = !Q_stricmp( part, "larm" ) ? VRB_LUA : !Q_stricmp( part, "rarm" ) ? VRB_RUA :
		!Q_stricmp( part, "lleg" ) ? VRB_LTHIGH : !Q_stricmp( part, "rleg" ) ? VRB_RTHIGH :
		!Q_stricmp( part, "gut" ) ? VRB_PELVIS : VRB_SPINE2;
	b = a == VRB_LUA ? VRB_LFA : a == VRB_RUA ? VRB_RFA : a == VRB_LTHIGH ? VRB_LCALF :
		a == VRB_RTHIGH ? VRB_RCALF : a == VRB_PELVIS ? VRB_SPINE2 : VRB_NECK;
	if ( !VRX_TestPoint( best, a, p ) || !VRX_TestPoint( best, b, q ) ) {
		return;
	}
	VectorAdd( p, q, p );
	VectorScale( p, 0.5f, p );
	VectorSubtract( p, cg.refdef.vieworg, dir );
	VectorNormalize( dir );
	CG_VRX_Flinch( best, p, dir );
	CG_VRX_Wound( best, p, dir );
	CG_Printf( "hit %d in the %s\n", best, part );
}

// vrposedump (test aid): the bones of the character nearest the view, as drawn last frame
// (model space: x forward, y left, z up), for checking poses offline
void CG_VRX_PoseDump_f( void ) {
	static float scratch[VRX_MAX_BONES * 12];
	centity_t *best = NULL;
	float bestD = 1e9f, d;
	const float *bones;
	vrSkel_t *sk;
	refEntity_t *body;
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		centity_t *c = &cg_entities[i];
		if ( !c->currentValid || i == cg.snap->ps.clientNum || !c->pe.legsRefEnt.hModel ) {
			continue;
		}
		d = Distance( c->lerpOrigin, cg.refdef.vieworg );
		if ( d < bestD ) {
			bestD = d;
			best = c;
		}
	}
	if ( !best ) {
		return;
	}
	body = &best->pe.legsRefEnt;
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		sk = VRX_Skel( body, NULL );
		bones = body->ragdollBones;
	} else {
		sk = VRX_Skel( body, scratch );
		bones = scratch;
	}
	if ( !sk ) {
		return;
	}
	CG_Printf( "VRPOSE %d %d bits %d style %d\n", best->currentState.number, sk->n, CG_VRX_GoreBits( best ),
			   vrxDownStyle[best->currentState.number] );
	for ( i = 0; i < sk->n; i++ ) {
		const float *b = bones + i * 12;
		CG_Printf( "VRB %d %d %d %.1f %.1f %.1f %s\n", i, sk->parents[i], sk->flags[i] & BONEFLAG_TAG ? 1 : 0,
				   b[9], b[10], b[11], sk->names[i] );
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

/*
==============================================================================

Cut off heads (g_decapitate)

A body's gore marker entity (g_vrextras.c) carries what has been cut off it; a head that
came off is its own entity, drawn here with the dead man's own head and helmet. The knife
going through a dead body's neck is seen here, where the ragdoll is, and asked of the
server ("vrdecap"): a VR blade swept through it, or a knife swing at it without a headset.

==============================================================================
*/

static int vrxGoreBits[MAX_GENTITIES];      // from the gore markers, by body
static int vrxGoreHigh[MAX_GENTITIES];      // and which limbs were cut high
static int vrxDecapAsked[MAX_CLIENTS];      // when we last asked for a body

// a gore marker entity: what's been done to its body (drawn as nothing)
static int vrxGroinTime[MAX_GENTITIES];

static void VRX_GoreMarker( centity_t *cent ) {
	int body = cent->currentState.otherEntityNum2;

	if ( body >= 0 && body < MAX_GENTITIES ) {
		vrxGoreBits[body] = cent->currentState.density;
		vrxGoreHigh[body] = cent->currentState.frame;
		vrxDownStyle[body] = cent->currentState.time2;
		vrxGroinTime[body] = cent->currentState.time;
	}
}

int CG_VRX_GoreBits( centity_t *cent ) {
	int n = cent->currentState.number;

	if ( !( cent->currentState.eFlags & EF_DEAD ) ) {
		// alive (a new character in the slot): only being scared or giving up shows
		int keep = VRX_GB_SCARED | VRX_GB_SURRENDER | VRX_GB_CRIPPLED | VRX_GB_DOWNED;
		if ( vrxGoreBits[n] & VRX_GB_CRIPPLED ) {
			keep |= VRX_GB_LARM | VRX_GB_RARM;      // an arm shot off someone still running
		}
		vrxGoreBits[n] &= keep;
	}
	return vrxGoreBits[n];
}

qboolean CG_VRX_Headless( centity_t *cent ) {
	return ( CG_VRX_GoreBits( cent ) & VRX_GB_HEAD ) != 0;
}

// a cut off head: the body's own head model and skin, and its hat
static void VRX_DrawHead( centity_t *cent ) {
	int body = cent->currentState.otherEntityNum2;
	clientInfo_t *ci = body >= 0 && body < MAX_CLIENTS ? &cgs.clientinfo[body] : NULL;
	refEntity_t head, acc;
	int i;

	memset( &head, 0, sizeof( head ) );
	VectorCopy( cent->lerpOrigin, head.origin );
	VectorCopy( cent->lerpOrigin, head.oldorigin );
	VectorCopy( cent->lerpOrigin, head.lightingOrigin );
	AnglesToAxis( cent->lerpAngles, head.axis );
	head.renderfx = RF_LIGHTING_ORIGIN;
	if ( ci && ci->infoValid && ci->headModel ) {
		head.hModel = ci->headModel;
		head.customSkin = ci->headSkin;
		CG_VRX_HeadClip( body, &head, qtrue );     // the neck below its stump, and a shot off jaw, left out
		trap_R_AddRefEntityToScene( &head );
		// the torn neck under it, and whatever the head took before it came off
		{
			vec3_t p, down;
			VectorMA( head.origin, 0.6f, head.axis[0], p );
			VectorMA( p, -2.4f, head.axis[2], p );
			VectorNegate( head.axis[2], down );
			// Soldier of Fortune 2's bloody stump where the neck was cut (the head's own skin
			// closes it off below, so it goes over the cut the clip makes), the spine in it
			{
				vec3_t at;
				VectorMA( head.origin, 0.25f, head.axis[0], at );
				VectorMA( at, VRX_NECK_CUT, head.axis[2], at );
				VRX_AddSofStump( VRX_SOF_NECK, at, down, head.axis[1], 4.0f, 1.8f, head.lightingOrigin );
			}
			if ( VRX_CryptModels()->insideHead ) {
				// the head again, only its inside faces: the cut fills the open neck exactly
				refEntity_t re = head;
				re.customSkin = 0;
				re.customShader = VRX_CryptModels()->insideHead;
				trap_R_AddRefEntityToScene( &re );
			} else if ( VRX_CryptModels()->neck ) {
				VRX_AddNeckCut( p, down, head.axis[0], 4.8f, head.lightingOrigin );
			} else {
				VRX_AddFleshCap( p, down, 2.2f, 4.8f, head.lightingOrigin );
			}
		}
		CG_VRX_AddHeadWounds( body, &head );
		// the helmet / cap stays on
		for ( i = 0; i < 3; i++ ) {
			static const int hats[3] = { ACC_HAT, ACC_MOUTH2, ACC_MOUTH3 };
			if ( !ci->accModels[hats[i]] ) {
				continue;
			}
			memset( &acc, 0, sizeof( acc ) );
			acc.hModel = ci->accModels[hats[i]];
			acc.customSkin = ci->accSkins[hats[i]];
			VectorCopy( head.lightingOrigin, acc.lightingOrigin );
			acc.renderfx = RF_LIGHTING_ORIGIN;
			CG_PositionEntityOnTag( &acc, &head, "tag_mouth", 0, NULL );
			trap_R_AddRefEntityToScene( &acc );
		}
		return;
	}
	head.hModel = cgs.gameModels[cent->currentState.modelindex];    // no head to draw: a skull
	trap_R_AddRefEntityToScene( &head );
}

// CG_General: our entities; qtrue when it was one
qboolean CG_VRX_General( centity_t *cent ) {
	switch ( cent->currentState.dmgFlags ) {
	case VRX_ENT_GORE:
		VRX_GoreMarker( cent );
		return qtrue;
	case VRX_ENT_HEAD:
		VRX_DrawHead( cent );
		return qtrue;
	}
	return qfalse;
}

// vr_debug 7: why a head did or didn't come off
static qboolean cg_vrxDebug( void ) {
	char dbg[8];

	trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
	return atoi( dbg ) == 7;
}

// the blade went from a to b: through a dead body's neck?
static void VRX_TryDecap( const vec3_t a, const vec3_t b, float radius ) {
	centity_t *cent;
	vec3_t ab, ap, c, neck;
	float t, len2;
	int i;

	if ( !CG_VRX_Gore( VRX_DECAP ) || !cg.snap ) {
		return;
	}
	VectorSubtract( b, a, ab );
	len2 = DotProduct( ab, ab );
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		cent = &cg_entities[i];
		if ( !cent->currentValid || i == cg.snap->ps.clientNum || cent->currentState.eType != ET_PLAYER ||
			 !( cent->currentState.eFlags & EF_DEAD ) || CG_VRX_Headless( cent ) || !cent->pe.headRefEnt.hModel ||
			 cg.time - vrxDecapAsked[i] < 1000 ) {
			continue;
		}
		VectorCopy( cent->pe.headRefEnt.origin, neck );     // tag_head: the top of the neck
		VectorSubtract( neck, a, ap );
		t = len2 > 0.001f ? DotProduct( ap, ab ) / len2 : 0;
		t = t < 0 ? 0 : ( t > 1 ? 1 : t );
		VectorMA( a, t, ab, c );
		if ( Distance( c, neck ) <= radius ) {
			if ( cg_vrxDebug() ) {
				CG_Printf( "VR decap: asking for %i\n", i );
			}
			trap_SendClientCommand( va( "vrdecap %i %.0f %.0f %.0f", i, neck[0], neck[1], neck[2] ) );
			vrxDecapAsked[i] = cg.time;
			return;
		}
	}
}

// the server says a knife hit body at pos: on the neck (or near enough), the head comes off
void CG_VRX_KnifeHit( int body, const vec3_t pos ) {
	centity_t *cent;

	if ( body < 0 || body >= MAX_CLIENTS || !cg.snap || cg.snap->ps.weapon != WP_KNIFE ) {
		return;
	}
	cent = &cg_entities[body];
	if ( !cent->pe.headRefEnt.hModel ) {
		return;
	}
	if ( cg_vrxDebug() ) {
		CG_Printf( "VR decap: knife hit %i at %.0f from the neck\n", body, Distance( pos, cent->pe.headRefEnt.origin ) );
	}
	if ( Distance( pos, cent->pe.headRefEnt.origin ) <= 14.0f ) {
		VRX_TryDecap( pos, pos, 14.0f );
	}
}

// without a headset: a knife swing at a body's neck
void CG_VRX_KnifeSwing( void ) {
	vec3_t end;

	VectorMA( cg.refdef.vieworg, 72, cg.refdef.viewaxis[0], end );
	VRX_TryDecap( cg.refdef.vieworg, end, 8.0f );
}

// with one: the blade swept through it, fast enough to cut
void CG_VRX_KnifeFrame( void ) {
	static vec3_t lastTip;
	static int lastTime;
	vec3_t fwd, tip;
	int dom;

	if ( !cgvr.active || cg.predictedPlayerState.weapon != WP_KNIFE ||
		 cg.predictedPlayerState.stats[STAT_HEALTH] <= 0 ) {
		lastTime = 0;
		return;
	}
	dom = CG_VR_DominantHand();
	AngleVectors( cgvr.weaponAngles, fwd, NULL, NULL );
	VectorMA( cgvr.handOrigin[dom], 18.0f, fwd, tip );     // as the server's G_VR_KnifeStab
	if ( lastTime && cg.time > lastTime && cg.time - lastTime < 200 &&
		 Distance( tip, lastTip ) / ( ( cg.time - lastTime ) * 0.001f ) > 40.0f ) {
		VRX_TryDecap( lastTip, tip, 7.0f );
	}
	VectorCopy( tip, lastTip );
	lastTime = cg.time;
}

/*
=================
CG_VRX_Surrender

Someone who gave up (VRX_GB_SURRENDER): over the animation the arms go up, upper arms
out to the sides and the forearms straight up, eased in. The pose goes to the renderer
the way a ragdoll's does (ragdollBones), in the model's own space.
=================
*/
static int vrxGaveUp[MAX_CLIENTS];      // when we first saw it

// rotate bone b and everything under it about pivot (model space)
static void VRX_RotateSubtree( float *pose, vrSkel_t *sk, int top, vec3_t r[3], const vec3_t pivot ) {
	int i, k, j, p;

	for ( i = 0; i < sk->n; i++ ) {
		for ( p = i; p >= 0 && p != top; p = sk->parents[p] ) {
		}
		if ( p != top ) {
			continue;
		}
		{
			float *bm = pose + i * 12, m[9], t[3];
			memcpy( m, bm, sizeof( m ) );
			for ( k = 0; k < 3; k++ ) {
				for ( j = 0; j < 3; j++ ) {
					if ( sk->flags[i] & BONEFLAG_TAG ) {
						// rows are the axes: each axis turned
						bm[k * 3 + j] = m[k * 3 + 0] * r[0][j] + m[k * 3 + 1] * r[1][j] + m[k * 3 + 2] * r[2][j];
					} else {
						// columns are the axes: R * M
						bm[k * 3 + j] = r[0][k] * m[0 * 3 + j] + r[1][k] * m[1 * 3 + j] + r[2][k] * m[2 * 3 + j];
					}
				}
			}
			VectorSubtract( bm + 9, pivot, t );
			for ( k = 0; k < 3; k++ ) {
				bm[9 + k] = pivot[k] + r[0][k] * t[0] + r[1][k] * t[1] + r[2][k] * t[2];
			}
		}
	}
}

// the rotation (columns) taking unit u part of the way (f) onto unit v
static void VRX_Turn( const vec3_t u, const vec3_t v, float f, vec3_t r[3] ) {
	vec3_t axis;
	float ang, s, c, t, x, y, z;

	CrossProduct( u, v, axis );
	s = VectorNormalize( axis );
	ang = atan2( s, DotProduct( u, v ) ) * f;
	if ( s < 1e-4f ) {
		AxisClear( r );
		return;
	}
	s = sin( ang );
	c = cos( ang );
	t = 1 - c;
	x = axis[0]; y = axis[1]; z = axis[2];
	r[0][0] = t * x * x + c;     r[0][1] = t * x * y + s * z; r[0][2] = t * x * z - s * y;
	r[1][0] = t * x * y - s * z; r[1][1] = t * y * y + c;     r[1][2] = t * y * z + s * x;
	r[2][0] = t * x * z + s * y; r[2][1] = t * y * z - s * x; r[2][2] = t * z * z + c;
}

void CG_VRX_Surrender( centity_t *cent, refEntity_t *body ) {
	float *pose;
	static const char *arms[2][3] = { { "L UpperArm", "L Forearm", "L Hand" }, { "R UpperArm", "R Forearm", "R Hand" } };
	int n = cent->currentState.number, side, b[3], k;
	vrSkel_t *sk;
	vec3_t r[3], d, want;
	float f;

	if ( n >= MAX_CLIENTS || ( cent->currentState.eFlags & EF_DEAD ) || body->ragdollBones ||
		 !( CG_VRX_GoreBits( cent ) & VRX_GB_SURRENDER ) ) {
		if ( n < MAX_CLIENTS ) {
			vrxGaveUp[n] = 0;
		}
		return;
	}
	if ( !vrxGaveUp[n] ) {
		vrxGaveUp[n] = cg.time;
	}
	f = ( cg.time - vrxGaveUp[n] ) / 500.0f;
	f = f > 1 ? 1 : f;
	pose = vrxPoses[n];
	sk = VRX_Skel( body, pose );
	if ( !sk ) {
		return;
	}
	for ( side = 0; side < 2; side++ ) {
		for ( k = 0; k < 3; k++ ) {
			b[k] = VRX_FindBone( sk, arms[side][k] );
		}
		if ( b[0] < 0 || b[1] < 0 || b[2] < 0 ) {
			continue;
		}
		// upper arm out to the side (model space: x forward, y left, z up), a little up
		VectorSubtract( pose + b[1] * 12 + 9, pose + b[0] * 12 + 9, d );
		VectorNormalize( d );
		VectorSet( want, 0.15f, side ? -0.9f : 0.9f, 0.35f );
		VectorNormalize( want );
		VRX_Turn( d, want, f, r );
		VRX_RotateSubtree( pose, sk, b[0], r, pose + b[0] * 12 + 9 );
		// forearm straight up
		VectorSubtract( pose + b[2] * 12 + 9, pose + b[1] * 12 + 9, d );
		VectorNormalize( d );
		VectorSet( want, 0.1f, 0, 1 );
		VectorNormalize( want );
		VRX_Turn( d, want, f, r );
		VRX_RotateSubtree( pose, sk, b[1], r, pose + b[1] * 12 + 9 );
	}
	pose[sk->n * 12] = 0;      // nothing after the pose (no cut, CG_VRX_Dismember)
	body->ragdollBones = pose;
	body->ragdollMagic = RAGDOLL_MAGIC;
}

/*
=================
CG_VRX_Downed

Someone shot down (VRX_GB_DOWNED) plays one of Soldier of Fortune II's agony animations,
picked by the server from where the shot went in (G_VRX_KnockDown): falling, then writhing on
the floor until someone finishes him (VRX_Sof2Downed). A shot in the balls doubles him over
for a while instead.

Like CG_VRX_Surrender, a pose handed to the renderer as ragdollBones, in the model's own
space: x forward, y left, z up.
=================
*/
static int vrxWentDown[MAX_CLIENTS];

// bone b's position in the pose
#define VRX_BP( pose, b ) ( ( pose ) + ( b ) * 12 + 9 )

// turn bone a (and all under it) so the direction from a to its child c points along want
static void VRX_Aim( float *pose, vrSkel_t *sk, int a, int c, const vec3_t want, float f ) {
	vec3_t d, w, r[3];

	if ( a < 0 || c < 0 ) {
		return;
	}
	VectorSubtract( VRX_BP( pose, c ), VRX_BP( pose, a ), d );
	if ( VectorNormalize( d ) < 0.01f ) {
		return;
	}
	VectorNormalize2( want, w );
	VRX_Turn( d, w, f, r );
	VRX_RotateSubtree( pose, sk, a, r, VRX_BP( pose, a ) );
}

// is bone i bone top or under it
static qboolean VRX_Under( vrSkel_t *sk, int i, int top ) {
	for ( ; i >= 0; i = sk->parents[i] ) {
		if ( i == top ) {
			return qtrue;
		}
	}
	return qfalse;
}

// VRX_Aim for the trunk: in these skeletons the thighs hang off the spine, so turning the
// spine swings the legs with it; they're turned back (about their own joints) after
static void VRX_AimTrunk( float *pose, vrSkel_t *sk, int a, int c, const vec3_t want, float f, int lth, int rth ) {
	vec3_t d, w, r[3], back[3];
	int k, j;

	if ( a < 0 || c < 0 ) {
		return;
	}
	VectorSubtract( VRX_BP( pose, c ), VRX_BP( pose, a ), d );
	if ( VectorNormalize( d ) < 0.01f ) {
		return;
	}
	VectorNormalize2( want, w );
	VRX_Turn( d, w, f, r );
	VRX_RotateSubtree( pose, sk, a, r, VRX_BP( pose, a ) );
	for ( k = 0; k < 3; k++ ) {
		for ( j = 0; j < 3; j++ ) {
			back[k][j] = r[j][k];
		}
	}
	if ( lth >= 0 && lth != a && VRX_Under( sk, lth, a ) ) {
		VRX_RotateSubtree( pose, sk, lth, back, VRX_BP( pose, lth ) );
	}
	if ( rth >= 0 && rth != a && VRX_Under( sk, rth, a ) ) {
		VRX_RotateSubtree( pose, sk, rth, back, VRX_BP( pose, rth ) );
	}
}

// turn bone a so its child c points at the point at
static void VRX_AimAt( float *pose, vrSkel_t *sk, int a, int c, const vec3_t at, float f ) {
	vec3_t want;

	if ( a < 0 || c < 0 ) {
		return;
	}
	VectorSubtract( at, VRX_BP( pose, a ), want );
	VRX_Aim( pose, sk, a, c, want, f );
}

// everything up or down so the lowest bone is height above the floor
static void VRX_Lower( float *pose, vrSkel_t *sk, float floor, float height ) {
	float low = 1e9f;
	int i;

	for ( i = 0; i < sk->n; i++ ) {
		if ( !( sk->flags[i] & BONEFLAG_TAG ) && VRX_BP( pose, i )[2] < low ) {
			low = VRX_BP( pose, i )[2];
		}
	}
	if ( low > 1e8f ) {
		return;
	}
	for ( i = 0; i < sk->n; i++ ) {
		pose[i * 12 + 11] += floor + height - low;
	}
}

/*
SoF2's agony animations (cg_vrsof2anims.h, joint positions taken from its skeleton) put on
this skeleton: the body turned to the animation's (the line up the back, the line across the
hips), then each bone turned to point where the animation's does, then the pelvis put at the
animation's height and drift, scaled by how long the legs are. Past the end the last stretch
plays back and forth, slower: he keeps writhing.
*/
#include "cg_vrsof2anims.h"

// CG_VRX_Downed's bones (LUA ... HEAD) in the animation data
static const int vrxSof2Of[17] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 1, 2, 3, 4 };

static void VRX_Sof2Joints( int anim, int ms, vec3_t out[VRX_SOF2_JOINTS] ) {
	int n = vrxSof2Frames[anim], tail = n * 35 / 100, f0, f1, j, k;
	const short *p0, *p1;
	float t = ms * 0.02f, fr;

	if ( tail < 2 ) {
		tail = 2;
	}
	if ( t > n - 1 ) {
		float u = ( t - ( n - 1 ) ) * 0.6f, period = 2.0f * ( tail - 1 );
		u = fmod( u, period );
		t = n - 1 - ( u < tail - 1 ? u : period - u );
	}
	if ( t < 0 ) {
		t = 0;
	}
	f0 = (int)t;
	f1 = f0 + 1 < n ? f0 + 1 : f0;
	fr = t - f0;
	p0 = vrxSof2Pose[vrxSof2Start[anim] + f0];
	p1 = vrxSof2Pose[vrxSof2Start[anim] + f1];
	for ( j = 0; j < VRX_SOF2_JOINTS; j++ ) {
		for ( k = 0; k < 3; k++ ) {
			out[j][k] = ( ( 1 - fr ) * p0[j * 3 + k] + fr * p1[j * 3 + k] ) * ( 1.0f / 32 );
		}
	}
}

// turn every tree in the model by r about pivot
static void VRX_TurnAll( float *pose, vrSkel_t *sk, vec3_t r[3], const vec3_t pivot ) {
	vec3_t at;
	int k;

	VectorCopy( pivot, at );        // (pivot may be one of the bones)
	for ( k = 0; k < sk->n; k++ ) {
		if ( sk->parents[k] < 0 ) {
			VRX_RotateSubtree( pose, sk, k, r, at );
		}
	}
}

// b: CG_VRX_Downed's bones; floor: where his feet stood
static void VRX_Sof2Downed( float *pose, vrSkel_t *sk, const int *b, int anim, int ms, float floor ) {
	enum { LUA, LFA, LHAND, RUA, RFA, RHAND, LTH, LCALF, LFOOT, RTH, RCALF, RFOOT, PELVIS, SPINE, SPINE2, NECK, HEAD };
	static const int chain[][2] = {
		{ NECK, HEAD }, { LUA, LFA }, { LFA, LHAND }, { RUA, RFA }, { RFA, RHAND },
		{ LTH, LCALF }, { LCALF, LFOOT }, { RTH, RCALF }, { RCALF, RFOOT }
	};
	vec3_t J[VRX_SOF2_JOINTS], J0[VRX_SOF2_JOINTS], want, cur, up, r[3], d;
	float f, s, leg, ankle0;
	int k;

	if ( anim < 0 || anim >= VRX_SOF2_ANIMS ) {
		anim = 0;
	}
	f = ms / 150.0f;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	VRX_Sof2Joints( anim, ms, J );
	VRX_Sof2Joints( anim, 0, J0 );
#define VRX_J( a, i ) ( a )[vrxSof2Of[i]]
	leg = Distance( VRX_BP( pose, b[LTH] ), VRX_BP( pose, b[LCALF] ) ) + Distance( VRX_BP( pose, b[LCALF] ), VRX_BP( pose, b[LFOOT] ) );
	s = leg > 1 ? leg / VRX_SOF2_LEG : 1;

	// the body: up the back, then across the hips
	VectorSubtract( VRX_J( J, SPINE2 ), VRX_J( J, PELVIS ), up );
	VectorNormalize( up );
	VectorSubtract( VRX_BP( pose, b[SPINE2] ), VRX_BP( pose, b[PELVIS] ), cur );
	VectorNormalize( cur );
	VRX_Turn( cur, up, f, r );
	VRX_TurnAll( pose, sk, r, VRX_BP( pose, b[PELVIS] ) );
	VectorSubtract( VRX_BP( pose, b[SPINE2] ), VRX_BP( pose, b[PELVIS] ), up );
	VectorNormalize( up );
	VectorSubtract( VRX_J( J, LTH ), VRX_J( J, RTH ), want );
	VectorMA( want, -DotProduct( want, up ), up, want );
	VectorSubtract( VRX_BP( pose, b[LTH] ), VRX_BP( pose, b[RTH] ), cur );
	VectorMA( cur, -DotProduct( cur, up ), up, cur );
	if ( VectorNormalize( want ) > 0.01f && VectorNormalize( cur ) > 0.01f ) {
		VRX_Turn( cur, want, f, r );
		VRX_TurnAll( pose, sk, r, VRX_BP( pose, b[PELVIS] ) );
	}

	// the back (the thighs hang off it here), then the head, arms and legs
	VectorSubtract( VRX_J( J, NECK ), VRX_J( J, SPINE ), want );
	VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], want, f, b[LTH], b[RTH] );
	for ( k = 0; k < (int)ARRAY_LEN( chain ); k++ ) {
		VectorSubtract( VRX_J( J, chain[k][1] ), VRX_J( J, chain[k][0] ), want );
		VRX_Aim( pose, sk, b[chain[k][0]], b[chain[k][1]], want, f );
	}

	// the pelvis where the animation has it: its height over the ankles it stood on, its drift
	ankle0 = min( VRX_J( J0, LFOOT )[2], VRX_J( J0, RFOOT )[2] );
	want[0] = ( VRX_J( J, PELVIS )[0] - VRX_J( J0, PELVIS )[0] ) * s;
	want[1] = ( VRX_J( J, PELVIS )[1] - VRX_J( J0, PELVIS )[1] ) * s;
	want[2] = floor + 3.0f + ( VRX_J( J, PELVIS )[2] - ankle0 ) * s;
	VectorSubtract( want, VRX_BP( pose, b[PELVIS] ), d );
	VectorScale( d, f, d );
	for ( k = 0; k < sk->n; k++ ) {
		VectorAdd( VRX_BP( pose, k ), d, VRX_BP( pose, k ) );
	}
#undef VRX_J
}

void CG_VRX_Downed( centity_t *cent, refEntity_t *body ) {
	enum { LUA, LFA, LHAND, RUA, RFA, RHAND, LTH, LCALF, LFOOT, RTH, RCALF, RFOOT, PELVIS, SPINE, SPINE2, NECK, HEAD, NB };
	static const char *names[NB] = {
		"L UpperArm", "L Forearm", "L Hand", "R UpperArm", "R Forearm", "R Hand",
		"L Thigh", "L Calf", "L Foot", "R Thigh", "R Calf", "R Foot",
		"Pelvis", "Spine", "Spine2", "Neck", "Head"
	};
	int n = cent->currentState.number, b[NB], k, root = -1, style;
	float *pose, f, floor;
	vrSkel_t *sk;
	vec3_t v, pivot;
	qboolean groin = qfalse;

	if ( n >= MAX_CLIENTS || ( cent->currentState.eFlags & EF_DEAD ) ) {
		if ( n < MAX_CLIENTS ) {
			vrxWentDown[n] = 0;
		}
		return;
	}
	if ( !( CG_VRX_GoreBits( cent ) & VRX_GB_DOWNED ) ) {
		vrxWentDown[n] = 0;
		k = cg.time - vrxGroinTime[n];
		if ( !vrxGroinTime[n] || k < 0 || k >= VRX_GROIN_MS ) {
			return;
		}
		// shot in the balls: doubled over, eased in quickly and out at the end
		groin = qtrue;
		f = k / 200.0f;
		if ( k > VRX_GROIN_MS - 500 ) {
			f = ( VRX_GROIN_MS - k ) / 500.0f;
		}
		f = f > 1 ? 1 : f;
		style = 0;
	} else {
		static int wentStyle[MAX_CLIENTS];

		style = vrxDownStyle[n] >= 1 && vrxDownStyle[n] <= VRX_DOWN_STYLES ? vrxDownStyle[n] : 1 + n % VRX_DOWN_STYLES;
		if ( !vrxWentDown[n] || wentStyle[n] != style ) {     // (put down again another way: vrtestbody)
			vrxWentDown[n] = cg.time;
			wentStyle[n] = style;
		}
		f = ( cg.time - vrxWentDown[n] ) / 400.0f;
		f = f > 1 ? 1 : f;
	}
	pose = vrxPoses[n];
	// the pose is built on one still frame of the animation, the one he was in when he went
	// down: the idle playing underneath would make the downed sway and twitch
	{
		static int stillFor[MAX_CLIENTS], stillFrames[MAX_CLIENTS][4];
		static vec3_t stillTorso[MAX_CLIENTS][3];
		int since = groin ? vrxGroinTime[n] : vrxWentDown[n];
		refEntity_t still = *body;

		if ( stillFor[n] != since ) {
			stillFor[n] = since;
			stillFrames[n][0] = body->backlerp < 0.5f ? body->frame : body->oldframe;
			stillFrames[n][1] = body->torsoBacklerp < 0.5f ? body->torsoFrame : body->oldTorsoFrame;
			memcpy( stillTorso[n], body->torsoAxis, sizeof( stillTorso[n] ) );
		}
		memcpy( still.torsoAxis, stillTorso[n], sizeof( still.torsoAxis ) );
		still.frame = still.oldframe = stillFrames[n][0];
		still.torsoFrame = still.oldTorsoFrame = stillFrames[n][1];
		still.backlerp = still.torsoBacklerp = 0;
		if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC && body->ragdollBones == pose ) {
			sk = VRX_Skel( body, NULL );
		} else {
			still.ragdollBones = NULL;
			sk = VRX_Skel( &still, pose );
		}
	}
	if ( !sk ) {
		return;
	}
	for ( k = 0; k < NB; k++ ) {
		b[k] = VRX_FindBone( sk, names[k] );
		if ( b[k] < 0 && k != SPINE2 ) {
			return;
		}
	}
	if ( b[SPINE2] < 0 ) {
		b[SPINE2] = b[SPINE];
	}
	for ( k = 0; k < sk->n; k++ ) {
		if ( sk->parents[k] < 0 ) {
			root = k;
			break;
		}
	}
	if ( root < 0 ) {
		return;
	}
	floor = min( VRX_BP( pose, b[LFOOT] )[2], VRX_BP( pose, b[RFOOT] )[2] ) - 3.0f;     // (the foot bone is at the ankle)
	VectorAdd( VRX_BP( pose, b[LFOOT] ), VRX_BP( pose, b[RFOOT] ), pivot );
	VectorScale( pivot, 0.5f, pivot );
	pivot[2] = floor;

	if ( groin ) {
		float low = 1e9f;
		vec3_t crotch;

		for ( k = 0; k < sk->n; k++ ) {
			if ( !( sk->flags[k] & BONEFLAG_TAG ) && VRX_BP( pose, k )[2] < low ) {
				low = VRX_BP( pose, k )[2];
			}
		}
		// knees together and bent, bent right over
		VectorSet( v, 0.4f, -0.18f, -1 );
		VRX_Aim( pose, sk, b[LTH], b[LCALF], v, f );
		VectorSet( v, 0.4f, 0.18f, -1 );
		VRX_Aim( pose, sk, b[RTH], b[RCALF], v, f );
		VectorSet( v, -0.25f, 0.1f, -1 );
		VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, f );
		VectorSet( v, -0.25f, -0.1f, -1 );
		VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, f );
		VectorSet( v, 0.7f, 0, 0.7f );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, f, b[LTH], b[RTH] );
		VectorSet( v, 0.3f, 0, 1 );
		VRX_Aim( pose, sk, b[NECK], b[HEAD], v, f * 0.5f );
		// both hands to the crotch
		VectorAdd( VRX_BP( pose, b[LTH] ), VRX_BP( pose, b[RTH] ), crotch );
		VectorScale( crotch, 0.5f, crotch );
		crotch[0] += 4.0f;
		crotch[2] -= 4.0f;
		VectorSet( v, 0.35f, 0.1f, -1 );
		VRX_Aim( pose, sk, b[LUA], b[LFA], v, f );
		VectorSet( v, 0.35f, -0.1f, -1 );
		VRX_Aim( pose, sk, b[RUA], b[RFA], v, f );
		crotch[1] += 1.5f;
		VRX_AimAt( pose, sk, b[LFA], b[LHAND], crotch, f );
		crotch[1] -= 3.0f;
		VRX_AimAt( pose, sk, b[RFA], b[RHAND], crotch, f );
		VRX_Lower( pose, sk, low, 0 );
	} else {
		VRX_Sof2Downed( pose, sk, b, style - 1, cg.time - vrxWentDown[n], floor );
	}
	pose[sk->n * 12] = 0;      // nothing after the pose (no cut, CG_VRX_Dismember)
	body->ragdollBones = pose;
	body->ragdollMagic = RAGDOLL_MAGIC;
}

/*
=================
CG_VRX_HitReact

Soldier of Fortune style hit reactions (SoF2 picks a pain animation by where the shot went in;
here the living character's own pose is bent instead), on top of the flinch
(CG_VRX_FlinchAngles), for a moment after a bullet hits:

  an arm    flung back along the shot, the forearm flopping
  a leg     the knee buckling, the body dropping onto it (a stumble)
  the gut   doubled over, the near hand clutching the wound
  the chest knocked back, the near hand clutching the wound
=================
*/
void CG_VRX_HitReact( centity_t *cent, refEntity_t *body ) {
	enum { LUA, LFA, LHAND, RUA, RFA, RHAND, LTH, LCALF, LFOOT, RTH, RCALF, RFOOT, PELVIS, SPINE, SPINE2, NECK, HEAD, NB };
	static const char *names[NB] = {
		"L UpperArm", "L Forearm", "L Hand", "R UpperArm", "R Forearm", "R Hand",
		"L Thigh", "L Calf", "L Foot", "R Thigh", "R Calf", "R Foot",
		"Pelvis", "Spine", "Spine2", "Neck", "Head"
	};
	int n = cent->currentState.number, b[NB], k, best = -1, t, dur, rise = 60;
	float *pose, env, d, bestD = 1e9f, floor;
	vrSkel_t *sk;
	vec3_t hit, dir, rel, v, w;

	if ( n >= MAX_CLIENTS || !CG_VRX( VRX_REACTIONS ) || !cent->pe.vrFlinchTime || body->ragdollBones ||
		 ( cent->currentState.eFlags & EF_DEAD ) || CG_VR_FullBody( cent ) ) {
		return;
	}
	t = cg.time - cent->pe.vrFlinchTime;
	dur = CG_VRX( VRX_REACTIONS ) == 2 ? 1100 : 800;
	if ( t < 0 || t >= dur ) {
		return;
	}
	pose = vrxPoses[n];
	sk = VRX_Skel( body, pose );
	if ( !sk ) {
		return;
	}
	for ( k = 0; k < NB; k++ ) {
		b[k] = VRX_FindBone( sk, names[k] );
		if ( b[k] < 0 && k != SPINE2 ) {
			return;
		}
	}
	if ( b[SPINE2] < 0 ) {
		b[SPINE2] = b[SPINE];
	}
	// the shot in the model's own space
	VectorSubtract( cent->pe.vrFlinchPoint, body->origin, rel );
	for ( k = 0; k < 3; k++ ) {
		hit[k] = DotProduct( rel, body->axis[k] );
		dir[k] = DotProduct( cent->pe.vrFlinchDir, body->axis[k] );
	}
	// where it went in: the nearest joint, or the middle of a limb
	for ( k = 0; k < NB; k++ ) {
		int c = k == LUA ? b[LFA] : k == LFA ? b[LHAND] : k == RUA ? b[RFA] : k == RFA ? b[RHAND] :
				k == LTH ? b[LCALF] : k == LCALF ? b[LFOOT] : k == RTH ? b[RCALF] : k == RCALF ? b[RFOOT] : -1;
		VectorCopy( VRX_BP( pose, b[k] ), v );
		if ( c >= 0 ) {
			VectorAdd( v, VRX_BP( pose, c ), v );
			VectorScale( v, 0.5f, v );
		}
		VectorSubtract( v, hit, w );
		d = VectorLength( w );
		if ( d < bestD ) {
			bestD = d;
			best = k;
		}
	}
	if ( best < 0 || best == HEAD || best == NECK ) {
		return;     // the head: the flinch snaps it
	}
	// fast in, slow out; a clutched wound is held a while
	env = t < rise ? (float)t / rise : 1.0f - (float)( t - rise ) / ( dur - rise );
	env = env < 0 ? 0 : env;
	env = env * env * ( 3 - 2 * env );
	floor = min( VRX_BP( pose, b[LFOOT] )[2], VRX_BP( pose, b[RFOOT] )[2] );

	if ( best == LUA || best == LFA || best == LHAND || best == RUA || best == RFA || best == RHAND ) {
		int ua = best <= LHAND ? LUA : RUA;
		// the arm thrown back along the shot
		VectorSubtract( VRX_BP( pose, b[ua + 1] ), VRX_BP( pose, b[ua] ), v );
		VectorNormalize( v );
		VectorMA( v, 1.4f, dir, w );
		w[1] += ua == LUA ? 0.9f : -0.9f;       // and out to the side, so it shows from the front
		w[2] += 0.3f;
		VRX_Aim( pose, sk, b[ua], b[ua + 1], w, env * 0.85f );
		VectorSubtract( VRX_BP( pose, b[ua + 2] ), VRX_BP( pose, b[ua + 1] ), v );
		VectorNormalize( v );
		VectorMA( v, 1.2f, dir, w );
		w[2] -= 0.6f;
		VRX_Aim( pose, sk, b[ua + 1], b[ua + 2], w, env * 0.8f );
		VectorSet( v, -dir[0] * 0.15f, -dir[1] * 0.15f, 1 );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, env * 0.25f, b[LTH], b[RTH] );
	} else if ( best == LTH || best == LCALF || best == LFOOT || best == RTH || best == RCALF || best == RFOOT ) {
		int th = best <= LFOOT ? LTH : RTH, other = th == LTH ? RTH : LTH;
		float side = th == LTH ? 0.08f : -0.08f;
		// the knee gives: thigh forward, shin back, the other knee bending a little too
		VectorSet( v, 0.75f, side, -1 );
		VRX_Aim( pose, sk, b[th], b[th + 1], v, env );
		VectorSet( v, -0.55f, side, -1 );
		VRX_Aim( pose, sk, b[th + 1], b[th + 2], v, env );
		VectorSet( v, 0.25f, -side, -1 );
		VRX_Aim( pose, sk, b[other], b[other + 1], v, env * 0.6f );
		VectorSet( v, -0.15f, -side, -1 );
		VRX_Aim( pose, sk, b[other + 1], b[other + 2], v, env * 0.6f );
		// and over the hurt leg, reaching down for it
		VectorSet( v, 0.35f, side * 3, 1 );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, env * 0.7f, b[LTH], b[RTH] );
		VRX_Lower( pose, sk, floor, 0 );
	} else {
		qboolean gut = hit[2] < VRX_BP( pose, b[SPINE2] )[2] - 2.0f;
		int ua = hit[1] > VRX_BP( pose, b[PELVIS] )[1] ? LUA : RUA;
		vec3_t at, was, now, r[3];

		VectorSubtract( VRX_BP( pose, b[NECK] ), VRX_BP( pose, b[SPINE] ), was );
		VectorSubtract( hit, VRX_BP( pose, b[SPINE] ), rel );

		if ( gut ) {
			VectorSet( v, 0.7f, 0, 0.7f );      // doubled over
		} else {
			VectorSet( v, dir[0] * 0.6f, dir[1] * 0.6f, 1 );       // knocked back
		}
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, env * 0.8f, b[LTH], b[RTH] );
		// the hand to the wound (where it is now, turned with the trunk)
		VectorSubtract( VRX_BP( pose, b[NECK] ), VRX_BP( pose, b[SPINE] ), now );
		VectorNormalize( was );
		VectorNormalize( now );
		VRX_Turn( was, now, 1, r );
		for ( k = 0; k < 3; k++ ) {
			at[k] = VRX_BP( pose, b[SPINE] )[k] + r[0][k] * rel[0] + r[1][k] * rel[1] + r[2][k] * rel[2];
		}
		VectorMA( at, -2.0f, dir, at );         // on the skin, not in it
		VectorSubtract( at, VRX_BP( pose, b[ua] ), v );
		v[2] -= 6.0f;
		VRX_Aim( pose, sk, b[ua], b[ua + 1], v, env * 0.9f );
		VRX_AimAt( pose, sk, b[ua + 1], b[ua + 2], at, env );
		if ( gut ) {
			VectorSet( v, 0.3f, 0.06f, -1 );
			VRX_Aim( pose, sk, b[LTH], b[LCALF], v, env * 0.5f );
			VectorSet( v, 0.3f, -0.06f, -1 );
			VRX_Aim( pose, sk, b[RTH], b[RCALF], v, env * 0.5f );
			VectorSet( v, -0.2f, 0.06f, -1 );
			VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, env * 0.5f );
			VectorSet( v, -0.2f, -0.06f, -1 );
			VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, env * 0.5f );
			VRX_Lower( pose, sk, floor, 0 );
		}
	}
	pose[sk->n * 12] = 0;      // nothing after the pose (no cut, CG_VRX_Dismember)
	body->ragdollBones = pose;
	body->ragdollMagic = RAGDOLL_MAGIC;
}

/*
=================
CG_VRX_BodyPoints

The server's tight hit boxes ("vrcaps:N" tags): where this character's bones were last
drawn, four points (VRB_*) per tag, the first as the origin and the rest as the axis.
=================
*/
qboolean CG_VRX_BodyPoints( int clientNum, int group, orientation_t *or ) {
	static const char *names[VRB_COUNT] = {
		"Pelvis", "Spine2", "Neck", "Head",
		"L UpperArm", "L Forearm", "L Hand", "R UpperArm", "R Forearm", "R Hand",
		"L Thigh", "L Calf", "L Foot", "L Toe0", "R Thigh", "R Calf", "R Foot", "R Toe0"
	};
	static float scratch[VRX_MAX_BONES * 12];
	static vec3_t pts[MAX_CLIENTS][VRB_GROUPS * 4];
	static int ptsTime[MAX_CLIENTS];
	centity_t *cent;
	refEntity_t *body;
	vrSkel_t *sk;
	const float *bones;
	vec3_t axis[3], bAxis[3];
	int k, b;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS || group < 0 || group >= VRB_GROUPS ) {
		return qfalse;
	}
	cent = &cg_entities[clientNum];
	body = &cent->pe.legsRefEnt;
	if ( !cent->currentValid || !body->hModel ) {
		return qfalse;
	}
	if ( ptsTime[clientNum] != cg.time ) {
		if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
			sk = VRX_Skel( body, NULL );
			bones = body->ragdollBones;
		} else {
			sk = VRX_Skel( body, scratch );
			bones = scratch;
		}
		if ( !sk ) {
			return qfalse;
		}
		for ( k = 0; k < 3; k++ ) {
			VectorCopy( body->axis[k], axis[k] );
		}
		for ( k = 0; k < VRB_COUNT; k++ ) {
			b = VRX_FindBone( sk, names[k] );
			if ( b < 0 && k == VRB_SPINE2 ) {
				b = VRX_FindBone( sk, "Spine1" );
			}
			if ( b < 0 && ( k == VRB_LTOE || k == VRB_RTOE ) ) {
				b = VRX_FindBone( sk, k == VRB_LTOE ? "L Foot" : "R Foot" );
			}
			if ( b < 0 ) {
				return qfalse;
			}
			VRX_BoneWorld( bones + b * 12, ( sk->flags[b] & BONEFLAG_TAG ) != 0, axis, body->origin, bAxis, pts[clientNum][k] );
		}
		for ( ; k < VRB_GROUPS * 4; k++ ) {
			VectorCopy( pts[clientNum][0], pts[clientNum][k] );
		}
		ptsTime[clientNum] = cg.time;
	}
	VectorCopy( pts[clientNum][group * 4], or->origin );
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( pts[clientNum][group * 4 + 1 + k], or->axis[k] );
	}
	return qtrue;
}

/*
=================
CG_VRX_Dismember

A body with limbs off: the pose being drawn (the ragdoll's, or the animation's), with a cut
after it telling the renderer to leave out the triangles skinned to each lost limb (Ghoul 2
style; the limb itself flies off as VRX_MeshLimb).
=================
*/

void CG_VRX_Dismember( centity_t *cent, refEntity_t *body ) {
	int n = cent->currentState.number, bits, limb, root;
	unsigned mask[VRX_CUT_WORDS] = { 0 };
	float *pose;
	vrSkel_t *sk;

	if ( n >= MAX_CLIENTS || !CG_VRX_Gore( VRX_DISMEMBER ) ) {
		return;
	}
	bits = CG_VRX_GoreBits( cent ) & ( VRX_GB_LARM | VRX_GB_RARM | VRX_GB_LLEG | VRX_GB_RLEG );
	if ( !( cent->currentState.eFlags & EF_DEAD ) ) {
		bits &= VRX_GB_LARM | VRX_GB_RARM;      // the living only ever lose an arm (crippled)
	}
	if ( !bits ) {
		return;
	}
	pose = vrxPoses[n];
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		sk = VRX_Skel( body, NULL );
		if ( !sk ) {
			return;
		}
		if ( body->ragdollBones != pose ) {     // (a pose of ours already: hands up, or down on the knees)
			memcpy( pose, body->ragdollBones, sizeof( float ) * 12 * sk->n );
		}
	} else {
		sk = VRX_Skel( body, pose );
		if ( !sk ) {
			return;
		}
	}
	for ( limb = VRX_GB_LARM; limb <= VRX_GB_RLEG; limb <<= 1 ) {
		if ( !( bits & limb ) ) {
			continue;
		}
		root = VRX_FindBone( sk, VRX_LimbRoot( limb, ( vrxGoreHigh[n] & limb ) != 0 ) );
		if ( root < 0 ) {
			continue;
		}
		VRX_CutMask( sk, root, mask );
	}
	VRX_WriteCut( pose + sk->n * 12, 0, mask );     // the renderer leaves those triangles out
	body->ragdollBones = pose;
	body->ragdollMagic = RAGDOLL_MAGIC;
	// the inside of the body meat where it's open (Soldier of Fortune 2's stump goes over the
	// cut, CG_VRX_AddWounds): the body again, only the faces turned away from the eye. (A
	// headless body already has its inside drawn, CG_VRX_AddCryptGore.)
	if ( VRX_StumpInside() && !( ( cent->currentState.eFlags & EF_DEAD ) && ( CG_VRX_GoreBits( cent ) & VRX_GB_HEAD ) && VRX_CryptModels()->insideBody ) ) {
		refEntity_t in = *body;
		in.customSkin = 0;
		in.customShader = VRX_StumpInside();
		trap_R_AddRefEntityToScene( &in );
	}
}

/*
=================
CG_VRX_BloodPool

A dead body ready to draw: the blood spreading out under it, from the chest (from the
neck, bigger, when the head is gone; bigger for the torn open and cut up), flat on the
floor and following the body if it slides. Drawn fresh each frame (a temporary mark), so
it grows and goes with the body.
=================
*/
static int vrxPoolStart[MAX_CLIENTS];
static float vrxPoolSpin[MAX_CLIENTS];

void CG_VRX_BloodPool( centity_t *cent, refEntity_t *body ) {
	static qhandle_t shader;
	static float pose[VRX_MAX_BONES * 12];
	int n = cent->currentState.number, bits, b;
	vrSkel_t *sk;
	const float *bones;
	vec3_t axis[3], bAxis[3], at, end;
	trace_t tr;
	float size, grow, t;
	int k;

	if ( n >= MAX_CLIENTS ) {
		return;
	}
	if ( !( cent->currentState.eFlags & EF_DEAD ) || !CG_VRX_Gore( VRX_WOUNDS ) || !cg_blood.integer ) {
		vrxPoolStart[n] = 0;
		return;
	}
	if ( !vrxPoolStart[n] ) {
		vrxPoolStart[n] = cg.time + 500;     // starts once he's down
		vrxPoolSpin[n] = random() * 360;
	}
	if ( cg.time < vrxPoolStart[n] ) {
		return;
	}
	if ( !shader ) {
		shader = trap_R_RegisterShader( "vr_bloodpool" );
		if ( !shader ) {
			shader = -1;
		}
	}
	if ( shader < 0 ) {
		return;
	}
	bits = CG_VRX_GoreBits( cent );
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
	b = VRX_FindBone( sk, ( bits & VRX_GB_HEAD ) ? "Neck" : "Spine" );
	if ( b < 0 ) {
		return;
	}
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( body->axis[k], axis[k] );
	}
	VRX_BoneWorld( bones + b * 12, qfalse, axis, body->origin, bAxis, at );
	VectorCopy( at, end );
	end[2] -= 48;
	at[2] += 4;
	CG_Trace( &tr, at, NULL, NULL, end, n, MASK_SOLID );
	if ( tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.6f ) {
		return;
	}
	size = 20.0f;
	if ( bits & ( VRX_GB_HEAD | VRX_GB_BLOWN ) ) {
		size += 10.0f;
	}
	if ( bits & ( VRX_GB_LARM | VRX_GB_RARM | VRX_GB_LLEG | VRX_GB_RLEG ) ) {
		size += 6.0f;
	}
	// spreads quickly at first, then slowly, over about ten seconds
	t = ( cg.time - vrxPoolStart[n] ) / 10000.0f;
	grow = t >= 1.0f ? 1.0f : 1.0f - ( 1.0f - t ) * ( 1.0f - t );
	CG_ImpactMark( shader, tr.endpos, tr.plane.normal, vrxPoolSpin[n], 1, 1, 1, 1, qfalse,
				   3.0f + size * grow, qtrue, 0 );
}
