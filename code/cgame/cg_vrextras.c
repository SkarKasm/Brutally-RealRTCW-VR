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
	int         bleedKind, bleedStart;  // vr_bloodFX: how a bullet wound bleeds (VRB_*), from when
	vrbWound_t  vrb;
	int         nextSoak;       // vr_bloodFX: when it next soaks more blood into his clothes
} vrWound_t;

typedef struct {
	int         entityNum;
	int         lastSeen;
	qboolean    dead;
	int         deadAt;         // (his heart stops soon after)
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

// vr_bloodFX: blood soaked into someone's clothes where it landed on him (CG_VRX_BloodOnBody),
// or ran out of his wounds, each stain as big as the blood in it; laid on the body's own
// surface by the renderer like the bullet holes (VRX_DrawStains)
#define VRX_MAX_STAINED     24
#define VRX_MAX_STAINS      32      // soaked in (a draw of their own takes 32)
#define VRX_MAX_SPLATS      112     // marks of drops, one to each drop that landed (32 of them drawn without the atlas)
#define VRX_MAX_STAINS_ALL  144     // both together on one body
#define VRX_STAIN_PEND      48

typedef struct {
	int         bone;
	vec3_t      local, localN, localD;  // in the bone's frame: where, out of the skin, and down it
	float       amount;                 // how much blood (a drop of size s is about s*s)
	float       shown, run;             // how far it has soaked out, how far it has run down (catching up)
	float       flip;
	// a splat: a drop that flew into him, marked where and the way it hit (localD: the way it
	// was going along the skin, stretch: how drawn out, the more the shallower it came in),
	// its picture (VRD_DROPS, VRD_STREAK..) and half its width
	qboolean    splat;
	float       stretch, size;
	int         look;
	int         time;
} vrStain_t;

typedef struct {
	qboolean    used, dead;
	int         entityNum, lastSeen, lastDraw;
	int         num;
	vrStain_t   s[VRX_MAX_STAINS_ALL];
	int         numPend;                // landed this frame, put on his bones when he's next drawn
	int         pendBone[VRX_STAIN_PEND];
	vec3_t      pendP[VRX_STAIN_PEND], pendN[VRX_STAIN_PEND], pendV[VRX_STAIN_PEND];
	float       pendAmount[VRX_STAIN_PEND];
	int         pendLook[VRX_STAIN_PEND];   // 0: as it landed (pendV the way it flew), or a picture
} vrStained_t;

static vrStained_t vrStained[VRX_MAX_STAINED];
static vrStained_t *VRX_Stained( int entityNum, qboolean create );
static void VRX_StainAdd( vrStained_t *st, int bone, const vec3_t local, const vec3_t localN, const vec3_t localD, float amount );
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
	CG_VRX_WoundBy( entityNum, point, dir, -1 );
}

// the same, the weapon known (-1 if not): the voxel gore (cg_vrgore.c) takes body shots
void CG_VRX_WoundBy( int entityNum, const vec3_t point, const vec3_t dir, int weapon ) {
	char dbg[8];

	if ( CG_VRX_Gore( VRX_VOXEL ) ) {
		CG_VRGore_Shot( entityNum, point, dir, weapon );     // heads too
		return;
	}

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

	if ( es->eventParm == VRG_LIMBS && CG_VRX_Gore( VRX_VOXEL ) ) {
		// limbs off: cut through the body itself (cg_vrgore.c)
		CG_VRGore_Limbs( es->otherEntityNum, es->density, es->frame, es->pos.trBase, es->origin2 );
		CG_AddBloodTrails( es->pos.trBase, tv( 0, 0, 1 ), 160, 1000, 10, 0.8f );
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
	vec3_t c, start, d1, n, ab, rel, onPath;

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
			VectorCopy( p1, onPath );
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
	{
		// where the path went into the limb: back along it from its nearest point to the bone,
		// to the limb's surface, so the hole is where the shot met him (a graze: the nearest side)
		vec3_t off, dperp, entry;
		float dist, across, back;
		VectorSubtract( onPath, c, off );
		VectorMA( off, -DotProduct( off, ab ), ab, off );
		dist = VectorLength( off );
		VectorMA( w->dir, -DotProduct( w->dir, ab ), ab, dperp );
		across = VectorLength( dperp );
		if ( dist < r && across > 0.2f ) {
			back = sqrt( r * r - dist * dist ) / across;
			VectorMA( onPath, -back, w->dir, entry );
			VectorSubtract( entry, c, n );
			VectorMA( n, -DotProduct( n, ab ), ab, n );
			VectorMA( c, DotProduct( entry, ab ) - DotProduct( c, ab ), ab, c );
		} else {
			VectorCopy( off, n );
		}
		if ( DotProduct( n, w->dir ) > 0 || VectorNormalize( n ) < 0.1f ) {
			VectorNegate( w->dir, n );     // (the side facing the shot)
			VectorMA( n, -DotProduct( n, ab ), ab, n );
			if ( VectorNormalize( n ) < 0.1f ) {
				PerpendicularVector( n, ab );
			}
		}
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
	w->size = 1.4f + random() * 0.5f;     // a bullet's hole, not half the limb (was 3 to 4.2)
	w->bone = i;
	// how it bleeds (vr_bloodFX): the throat and the femoral pump, the trunk runs, the rest drips
	w->bleedStart = cg.time;
	if ( i == VRX_FindBone( sk, "Neck" ) || i == VRX_FindBone( sk, "Head" ) ) {
		w->bleedKind = VRB_ARTERY;
		w->bleedEnd = cg.time + 25000;
	} else if ( ( i == VRX_FindBone( sk, "L Thigh" ) || i == VRX_FindBone( sk, "R Thigh" ) ) && random() < 0.6f ) {
		w->bleedKind = VRB_ARTERY;
		w->bleedEnd = cg.time + 25000;
	} else if ( i == VRX_FindBone( sk, "Spine" ) || i == VRX_FindBone( sk, "Pelvis" ) ||
				i == VRX_FindBone( sk, "Spine1" ) || i == VRX_FindBone( sk, "Spine2" ) ) {
		w->bleedKind = VRB_FLOW;
		w->bleedEnd = cg.time + 20000;
	} else {
		w->bleedKind = VRB_SEEP;
		w->bleedEnd = cg.time + 10000;
	}
}

// vr_bloodFX: a wound bleeding, until bleedEnd; dead, the heart stops in a few seconds and
// then it only drips a while
static void VRX_WoundBleed( vrWounded_t *wd, vrWound_t *w, int kind, const vec3_t p, const vec3_t n, const vec3_t down ) {
	float strength;
	int end = w->bleedEnd;

	if ( !kind || !CG_VRB_On() ) {
		return;
	}
	if ( wd->dead && wd->deadAt ) {
		int since = cg.time - wd->deadAt;
		if ( end > wd->deadAt + 15000 ) {
			end = wd->deadAt + 15000;
		}
		if ( since > 4000 ) {
			kind = VRB_SEEP;
		}
	}
	if ( cg.time >= end ) {
		return;
	}
	strength = ( end - cg.time ) / 15000.0f;
	strength = strength > 1 ? 1 : strength < 0.15f ? 0.15f : strength;
	if ( wd->dead && wd->deadAt && kind != VRB_SEEP ) {
		float left = 1.0f - ( cg.time - wd->deadAt ) / 4000.0f;
		strength *= left < 0 ? 0 : left;
	}
	w->vrb.owner = wd->entityNum;
	CG_VRB_Wound( &w->vrb, p, n, down, kind, strength );
	// and it soaks into his clothes round the wound and runs down them, as much as he bleeds
	if ( w->bone >= 0 && cg.time >= w->nextSoak ) {
		vrStained_t *st = VRX_Stained( wd->entityNum, qtrue );
		float amount = !w->nextSoak ? 0.5f : 0.1f * strength * ( kind == VRB_ARTERY ? 1.6f : kind == VRB_FLOW ? 1.2f : 0.6f );
		w->nextSoak = cg.time + 300;
		if ( st ) {
			vec3_t none = { 0, 0, 0 };      // (a cut's localT runs across it: down as he lies now)
			VRX_StainAdd( st, w->bone, w->local, w->localN, w->kind == VRW_BULLET ? w->localT : none, amount );
		}
	}
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
		VectorScale( re.axis[1], 0.8f, re.axis[1] );     // (was 1.2: as wide as an eye socket)
		VectorScale( re.axis[2], 0.8f, re.axis[2] );
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

/*
The overlays below (bullet holes, blood stains) are the body drawn again, and go into the
scene before the voxel gore (cg_vrgore.c) has cut this frame's body. Without its cut in their
buffers the renderer drew them on the whole body: over a limb that's gone, a red see-through
shape of it. So each is noted here, and CG_VRX_OverlayTrailer puts the cut in once it's known.
*/
#define VRX_OV_ROOM         4       // floats kept free in each overlay's buffer for it
static struct { float *b; int o, len, ent, frame; } vrxOv[MAX_CLIENTS * 8];    // (every overlay of every body in view: one left out is drawn uncut)

static void VRX_OverlayNote( float *b, int o, int len, int ent ) {
	int i, slot = -1;

	for ( i = 0; i < (int)ARRAY_LEN( vrxOv ); i++ ) {
		if ( vrxOv[i].b == b ) {
			slot = i;
			break;
		}
		if ( slot < 0 && ( !vrxOv[i].b || vrxOv[i].frame != cg.clientFrame ) ) {
			slot = i;
		}
	}
	if ( slot < 0 ) {
		return;
	}
	vrxOv[slot].b = b;
	vrxOv[slot].o = o;
	vrxOv[slot].len = len;
	vrxOv[slot].ent = ent;
	vrxOv[slot].frame = cg.clientFrame;
}

void CG_VRX_OverlayTrailer( int entityNum, const float *trailer, int n ) {
	int i;

	if ( n <= 0 || n > VRX_OV_ROOM ) {
		return;
	}
	for ( i = 0; i < (int)ARRAY_LEN( vrxOv ); i++ ) {
		if ( !vrxOv[i].b || vrxOv[i].ent != entityNum || vrxOv[i].frame != cg.clientFrame ) {
			continue;
		}
		memmove( vrxOv[i].b + vrxOv[i].o + n, vrxOv[i].b + vrxOv[i].o, sizeof( float ) * vrxOv[i].len );
		memcpy( vrxOv[i].b + vrxOv[i].o, trailer, sizeof( float ) * n );
		vrxOv[i].b = NULL;      // (done: not twice)
	}
}

#define VRX_BODYWOUND_SETS  4               // vr_bodywound, vr_bodywound2..4: the Soldier of Fortune 2 holes

static void VRX_AddBodyWounds( centity_t *cent, refEntity_t *body, vrSkel_t *sk, const float *bones,
							   float holes[][12], int numHoles ) {
	static float buf[MAX_CLIENTS][VRX_BODYWOUND_SETS][VRX_MAX_BONES * 12 + VRX_CUT_FLOATS + 2 + VRX_MAX_WOUNDS * 12 + VRX_OV_ROOM];
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
		VRX_OverlayNote( b, o, 2 + count * 12, n );
		ov = *body;
		ov.reFlags |= REFLAG_FULL_LOD;      // (as the voxel gore draws him: its cut is for the whole model)
		ov.customShader = shaders[set];
		ov.customSkin = 0;
		ov.ragdollBones = b;
		ov.ragdollMagic = RAGDOLL_MAGIC;
		trap_R_AddRefEntityToScene( &ov );
	}
}

/*
=================
VRX_Stained, CG_VRX_BloodOnBody, VRX_StainAdd, VRX_DrawStains

vr_bloodFX: blood on someone's clothes. A drop that flies into a body (cg_vrblood.c) lands
where it hit, and a wound soaks the clothes round it as it bleeds; a stain grows with the blood
in it, soaking out over a moment, and once there's enough it runs down the cloth. Drawn like
the bullet holes, the body again with only its triangles under each stain kept
(tr_animation.c R_VRX_Decals): vr_gorebloodsoak and vr_gorebloodrun (z_vr_voxelgore.pk3, cut
from bodyblood.tga).
=================
*/
static vrStained_t *VRX_Stained( int entityNum, qboolean create ) {
	vrStained_t *spare = NULL;
	int i;

	if ( entityNum < 0 || entityNum >= MAX_CLIENTS ) {
		return NULL;
	}
	for ( i = 0; i < VRX_MAX_STAINED; i++ ) {
		vrStained_t *st = &vrStained[i];
		if ( st->used && st->entityNum == entityNum ) {
			return st;
		}
		if ( !spare || ( spare->used && ( !st->used || st->lastSeen < spare->lastSeen ) ) ) {
			spare = st;
		}
	}
	if ( !create || !spare ) {
		return NULL;
	}
	memset( spare, 0, sizeof( *spare ) );
	spare->used = qtrue;
	spare->entityNum = entityNum;
	spare->lastSeen = cg.time;
	spare->dead = ( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) != 0;
	return spare;
}

static void VRX_BloodLanded( int entityNum, int bone, const vec3_t point, const vec3_t normal, const vec3_t dir, float amount, int look ) {
	vrStained_t *st = VRX_Stained( entityNum, qtrue );
	int k;

	if ( !st || bone < 0 ) {
		return;
	}
	if ( st->numPend >= VRX_STAIN_PEND ) {
		return;     // (a burst: enough of it has landed this frame)
	}
	k = st->numPend++;
	st->pendBone[k] = bone;
	VectorCopy( point, st->pendP[k] );
	VectorCopy( normal, st->pendN[k] );
	VectorCopy( dir, st->pendV[k] );
	st->pendAmount[k] = amount;
	st->pendLook[k] = look;
}

// blood flying along dir landed on him at point (the world's), on the limb of bone
void CG_VRX_BloodOnBody( int entityNum, int bone, const vec3_t point, const vec3_t normal, const vec3_t dir, float amount ) {
	VRX_BloodLanded( entityNum, bone, point, normal, dir, amount, 0 );
}

// his own blood on his clothes under a wound (cg_vrgore.c VRG_StreamMark): a streak down the
// skin along dir, or a scatter of drops
void CG_VRX_BloodMark( int entityNum, int bone, const vec3_t point, const vec3_t normal, const vec3_t dir, float amount, qboolean streak ) {
	if ( CG_VRB_On() ) {
		VRX_BloodLanded( entityNum, bone, point, normal, dir, amount, streak ? VRD_STREAK : VRD_DROPS );
	}
}

static float VRX_StainSize( float amount ) {
	float s = 0.5f + 1.5f * sqrt( amount );
	return s > 8.0f ? 8.0f : s;
}

static void VRX_StainAdd( vrStained_t *st, int bone, const vec3_t local, const vec3_t localN, const vec3_t localD, float amount ) {
	vrStain_t *s, *least = NULL;
	int i;

	// into one already there (on the same bone, inside its edge)
	for ( i = 0; i < st->num; i++ ) {
		s = &st->s[i];
		if ( s->splat ) {
			continue;
		}
		if ( s->bone == bone && Distance( s->local, local ) < VRX_StainSize( s->amount ) * 0.7f + 0.6f ) {
			s->amount += amount;
			return;
		}
		if ( !least || s->amount < least->amount ) {
			least = s;
		}
	}
	if ( st->num < VRX_MAX_STAINS_ALL ) {
		s = &st->s[st->num++];
	} else if ( least && least->amount < amount * 4.0f ) {
		s = least;      // (full: the smallest speck makes way)
	} else {
		return;
	}
	memset( s, 0, sizeof( *s ) );
	s->bone = bone;
	VectorCopy( local, s->local );
	VectorCopy( localN, s->localN );
	VectorCopy( localD, s->localD );
	s->amount = amount;
	s->shown = 0.3f;
	s->flip = ( rand() & 1 ) ? 1.0f : -1.0f;
}

// a drop that flew into him: one droplet where it landed, as big as the blood in it soaks out
// to on cloth, and drawn out along the way it was going as much as the angle it came in at
// makes it (a drop hitting at angle a from the cloth leaves a mark 1 / sin a times as long as
// it is wide), its tail the way it went. Shallower than about 25 degrees it skids into a
// streak. A fast heavy one throws a speck or two on ahead of it. Each stays where it landed
// (they don't run together); the oldest makes way when he's covered. look: VRD_STREAK or
// VRD_DROPS to say, 0 to tell by graze (0 to 1: sin of the angle from straight in).
static vrStain_t *VRX_SplatSlot( vrStained_t *st, int bone, const vec3_t local, float size ) {
	vrStain_t *oldest = NULL;
	int i, splats = 0;

	for ( i = 0; i < st->num; i++ ) {
		vrStain_t *o = &st->s[i];
		if ( !o->splat ) {
			continue;
		}
		// (landing all but exactly on one already there: a stream on one spot builds up drop
		// on drop where it falls, not a pile of marks on one point)
		if ( o->bone == bone && DistanceSquared( o->local, local ) < Square( 0.3f * ( size < o->size ? size : o->size ) ) ) {
			o->size += 0.08f * size * ( o->size < 1.6f );
			return NULL;
		}
		splats++;
		if ( !oldest || o->time < oldest->time ) {
			oldest = o;
		}
	}
	if ( splats >= ( CG_VR_GoreQuality() >= 2 ? VRX_MAX_SPLATS : ( CG_VR_GoreQuality() == 1 ? VRX_MAX_SPLATS / 2 : VRX_MAX_SPLATS / 4 ) ) ||
		 st->num >= VRX_MAX_STAINS_ALL ) {
		return oldest;
	}
	return &st->s[st->num++];
}

static void VRX_SplatAdd( vrStained_t *st, int bone, const vec3_t local, const vec3_t localN, const vec3_t localD,
						  float amount, float graze, int look, float speed ) {
	vrStain_t *s;
	float root = sqrt( amount ), size, stretch, sinA;
	int k, n;

	if ( !look ) {
		look = graze >= 0.9f ? VRD_STREAK : VRD_DROPS;
	}
	if ( look == VRD_STREAK ) {
		size = 0.25f + 0.35f * root;
		size = size > 0.7f ? 0.7f : size;
		stretch = 2.2f + 2.3f * graze + random();
	} else {
		size = 0.06f + 1.25f * root;        // (the drop's own size, soaked out about twice that on cloth)
		size = size > 1.5f ? 1.5f : size;
		sinA = 1.0f - graze * graze;
		sinA = sinA < 0.18f ? 0.42f : sqrt( sinA );
		stretch = 1.0f / sinA;
		stretch = stretch > 2.4f ? 2.4f : stretch;
	}
	s = VRX_SplatSlot( st, bone, local, size );
	if ( !s ) {
		return;
	}
	memset( s, 0, sizeof( *s ) );
	s->splat = qtrue;
	s->bone = bone;
	VectorCopy( local, s->local );
	VectorCopy( localN, s->localN );
	VectorCopy( localD, s->localD );
	s->amount = amount;
	s->look = look == VRD_STREAK ? ( ( rand() & 1 ) ? VRD_STREAK : VRD_STREAK2 ) : VRD_DROPS;
	s->size = size;
	s->stretch = stretch;
	s->shown = 1.0f;
	s->time = cg.time;
	s->flip = ( rand() & 1 ) ? 1.0f : -1.0f;
	// a fast one splashes: a speck or two thrown on ahead, the way it was going
	if ( look != VRD_STREAK && speed > 160.0f && root > 0.25f && graze > 0.05f ) {
		n = 1 + ( rand() % 2 ) + ( speed > 300.0f );
		for ( k = 0; k < n; k++ ) {
			vec3_t at;
			vrStain_t *t;
			float off = size * stretch * ( 1.1f + 0.9f * random() + 0.5f * k ), ts = size * ( 0.18f + 0.14f * random() );
			VectorMA( local, off, localD, at );
			t = VRX_SplatSlot( st, bone, at, ts );
			if ( !t ) {
				continue;
			}
			memset( t, 0, sizeof( *t ) );
			t->splat = qtrue;
			t->bone = bone;
			VectorCopy( at, t->local );
			VectorCopy( localN, t->localN );
			VectorCopy( localD, t->localD );
			t->amount = amount * 0.05f;
			t->look = VRD_DROPS;
			t->size = ts;
			t->stretch = 1.0f + 0.3f * stretch;
			t->shown = 1.0f;
			t->time = cg.time;
			t->flip = ( rand() & 1 ) ? 1.0f : -1.0f;
		}
	}
}

// what landed on him since he was last drawn: onto his bones as they are now
static void VRX_StainsLand( vrStained_t *st, vrSkel_t *sk, vec3_t bAxis[][3], vec3_t bPos[] ) {
	int i, k;

	for ( i = 0; i < st->numPend; i++ ) {
		int bn = st->pendBone[i];
		vec3_t rel, lp, ln, ld, along, down = { 0, 0, -1 };
		const float *n = st->pendN[i];
		float graze = 0;
		qboolean splat = st->pendLook[i] || VectorLengthSquared( st->pendV[i] ) > 0.01f;

		if ( bn < 0 || bn >= sk->n ) {
			continue;
		}
		if ( splat ) {
			// the way it was going along the skin, and how shallow it came in
			VectorNormalize2( st->pendV[i], along );
			VectorMA( along, -DotProduct( along, n ), n, along );
			graze = VectorNormalize( along );
			if ( graze < 0.15f ) {      // straight in: round, turned any way
				vec3_t a, b2;
				float ang = random() * 2.0f * M_PI;
				PerpendicularVector( a, n );
				CrossProduct( n, a, b2 );
				VectorScale( a, cos( ang ), along );
				VectorMA( along, sin( ang ), b2, along );
				graze = 0;
			}
		} else {
			// soaking in: it runs the way down is
			VectorMA( down, -DotProduct( down, n ), n, along );
			if ( VectorNormalize( along ) < 0.2f ) {
				VectorClear( along );       // (on top of him: no way down, it just spreads)
			}
		}
		VectorSubtract( st->pendP[i], bPos[bn], rel );
		for ( k = 0; k < 3; k++ ) {
			lp[k] = DotProduct( rel, bAxis[bn][k] );
			ln[k] = DotProduct( n, bAxis[bn][k] );
			ld[k] = DotProduct( along, bAxis[bn][k] );
		}
		if ( splat ) {
			VRX_SplatAdd( st, bn, lp, ln, ld, st->pendAmount[i], graze, st->pendLook[i], VectorLength( st->pendV[i] ) );
		} else {
			VRX_StainAdd( st, bn, lp, ln, ld, st->pendAmount[i] );
		}
	}
	st->numPend = 0;
}

// the body drawn again with marks laid on its skin (12 floats each, after its pose and any cut)
static void VRX_StainOverlay( float *b, const float *bones, vrSkel_t *sk, const refEntity_t *body, int ent,
							  qhandle_t shader, const float *marks, int count ) {
	union { float f; int i; } magic;
	refEntity_t ov;
	int o = sk->n * 12;

	memcpy( b, bones, sizeof( float ) * 12 * sk->n );
	if ( VRX_HasCut( bones + o ) ) {
		memcpy( b + o, bones + o, sizeof( float ) * VRX_CUT_FLOATS );
		o += VRX_CUT_FLOATS;
	}
	memcpy( b + o + 2, marks, sizeof( float ) * 12 * count );
	magic.i = VRX_WOUND_MAGIC;
	b[o] = magic.f;
	b[o + 1] = count;
	VRX_OverlayNote( b, o, 2 + count * 12, ent );
	ov = *body;
	ov.reFlags |= REFLAG_FULL_LOD;
	ov.customShader = shader;
	ov.customSkin = 0;
	ov.ragdollBones = b;
	ov.ragdollMagic = RAGDOLL_MAGIC;
	trap_R_AddRefEntityToScene( &ov );
}

#define VRX_STAIN_SET_FLOATS    ( VRX_MAX_BONES * 12 + VRX_CUT_FLOATS + 2 + VRX_MAX_STAINS * 12 + VRX_OV_ROOM )
#define VRX_STAIN_SETS          3       // soaked in, running down from that, marks of drops
typedef char vrxStainRoom_t[VRX_STAIN_SETS * VRX_STAIN_SET_FLOATS >= VRX_MAX_BONES * 12 + VRX_CUT_FLOATS + 2 + VRX_STAIN_MARKS * 12 + VRX_OV_ROOM &&
							VRX_STAIN_MARKS >= 2 * VRX_MAX_STAINS + VRX_MAX_SPLATS ? 1 : -1];

static void VRX_DrawStains( vrStained_t *st, centity_t *cent, refEntity_t *body, vrSkel_t *sk, const float *bones,
							vec3_t bAxis[][3], vec3_t bPos[] ) {
	static float buf[MAX_CLIENTS][VRX_STAIN_SETS * VRX_STAIN_SET_FLOATS];
	static qhandle_t shaders[VRX_STAIN_SETS];
	static const int picture[VRX_STAIN_SETS] = { VRD_SOAK, VRD_RUN, 0 };
	float marks[VRX_STAIN_SETS][VRX_MAX_SPLATS][12], dt;
	int count[VRX_STAIN_SETS] = { 0, 0, 0 }, room[VRX_STAIN_SETS], i, k, set, n = cent->currentState.number;
	qhandle_t atlas = CG_VRGore_BloodAtlas();

	VRX_StainsLand( st, sk, bAxis, bPos );
	if ( n < 0 || n >= MAX_CLIENTS || sk->n > VRX_MAX_BONES || !st->num ) {
		return;
	}
	if ( !shaders[0] ) {
		shaders[0] = trap_R_RegisterShader( "vr_gorebloodsoak" );
		shaders[1] = trap_R_RegisterShader( "vr_gorebloodrun" );
		// (its name has to start vr_goreblood, as the others': that is what tells the renderer
		// to lay it on the skin. Under another name the whole body was drawn in it, the same
		// red shapes on everyone)
		shaders[2] = trap_R_RegisterShader( "vr_gorebloodsplat" );
		if ( !shaders[2] ) {
			shaders[2] = shaders[0];        // (an older z_vr_voxelgore.pk3)
		}
	}
	room[0] = room[1] = VRX_MAX_STAINS;
	room[2] = atlas ? VRX_MAX_SPLATS : VRX_MAX_STAINS;
	dt = st->lastDraw ? ( cg.time - st->lastDraw ) * 0.001f : 0;
	dt = dt < 0 ? 0 : dt > 0.2f ? 0.2f : dt;
	st->lastDraw = cg.time;

	for ( i = 0; i < st->num; i++ ) {
		vrStain_t *s = &st->s[i];
		vec3_t p, nn, down, rel;
		float size = VRX_StainSize( s->amount ), runTo;

		if ( s->bone < 0 || s->bone >= sk->n ) {
			continue;
		}
		if ( !s->splat ) {
			// soaking out (about a second), and running down once there's enough (a few seconds)
			s->shown += ( 1.0f - s->shown ) * ( dt * 2.0f > 1 ? 1 : dt * 2.0f );
			runTo = s->amount > 0.9f ? ( s->amount - 0.9f ) * 1.6f : 0;
			runTo = runTo > 7.0f ? 7.0f : runTo;
			s->run += ( runTo - s->run ) * ( dt * 0.5f > 1 ? 1 : dt * 0.5f );
		}

		// where it is now, with its bone
		VectorCopy( bPos[s->bone], p );
		VectorClear( nn );
		VectorClear( down );
		for ( k = 0; k < 3; k++ ) {
			VectorMA( p, s->local[k], bAxis[s->bone][k], p );
			VectorMA( nn, s->localN[k], bAxis[s->bone][k], nn );
			VectorMA( down, s->localD[k], bAxis[s->bone][k], down );
		}
		if ( VectorNormalize( nn ) < 0.1f ) {
			continue;
		}
		if ( VectorLength( down ) < 0.1f ) {
			VectorSet( down, 0, 0, -1 );    // as he lies now
		}
		VectorMA( down, -DotProduct( down, nn ), nn, down );
		if ( VectorNormalize( down ) < 0.1f ) {
			PerpendicularVector( down, nn );
		}
		for ( set = s->splat ? 2 : 0; set < ( s->splat ? 3 : 2 ); set++ ) {
			float *h, sz, stretch, look = picture[set];
			vec3_t q;

			VectorCopy( p, q );
			if ( set == 2 ) {
				sz = s->size;
				stretch = 10.0f + s->stretch;
				look = s->look;
				if ( !atlas && stretch > 11.7f ) {
					stretch = 11.7f;        // (its own texture is a splash: not drawn out into a streak)
				}
			} else if ( set == 1 ) {
				if ( s->run < 0.3f ) {
					continue;
				}
				sz = size * 0.45f;
				stretch = 11.0f + s->run;       // (10 and up: stretched down the skin)
				VectorMA( p, sz * ( s->run + 0.2f ), down, q );     // its top at the stain
			} else {
				sz = size * s->shown;
				stretch = 1.0f;
			}
			if ( count[set] >= room[set] ) {
				continue;
			}
			h = marks[set][count[set]++];
			VectorSubtract( q, body->origin, rel );
			for ( k = 0; k < 3; k++ ) {     // into the model's own space
				h[k] = DotProduct( rel, body->axis[k] );
				h[3 + k] = DotProduct( nn, body->axis[k] );
				h[6 + k] = DotProduct( down, body->axis[k] );
			}
			h[9] = sz;
			h[10] = atlas ? s->flip * ( look + 1 ) : s->flip;
			h[11] = stretch;
		}
	}

	if ( atlas ) {
		// every kind in one draw: the voxel gore's, with the blood of his wounds, or one of its own
		float all[VRX_STAIN_MARKS][12];
		int total = 0;
		for ( set = 0; set < VRX_STAIN_SETS; set++ ) {
			memcpy( all[total], marks[set], sizeof( float ) * 12 * count[set] );
			total += count[set];
		}
		if ( total && !CG_VRGore_TakeBlood( n, body, ( cent->currentState.eFlags & EF_DEAD ) != 0, all[0], total ) ) {
			VRX_StainOverlay( buf[n], bones, sk, body, n, atlas, all[0], total );
		}
		return;
	}
	for ( set = 0; set < VRX_STAIN_SETS; set++ ) {
		if ( count[set] && shaders[set] ) {
			VRX_StainOverlay( buf[n] + set * VRX_STAIN_SET_FLOATS, bones, sk, body, n, shaders[set], marks[set][0], count[set] );
		}
	}
}

// the bodies near blood in the air, as capsules round their bones (cg_vrblood.c: it lands on them)
static void VRX_BloodBody( centity_t *cent, vrSkel_t *sk, vec3_t bPos[] ) {
	static vec3_t a[48], b[48];
	static float r[48];
	static int bone[48];
	int i, num = 0;

	for ( i = 0; i < sk->n && num < 48; i++ ) {
		if ( sk->radius[i] <= 0 || sk->parents[i] < 0 ) {
			continue;
		}
		VectorCopy( bPos[sk->parents[i]], a[num] );
		VectorCopy( bPos[i], b[num] );
		r[num] = sk->radius[i] * 1.15f + 0.5f;      // (his clothes stand off him a little)
		bone[num] = sk->parents[i];                 // (the limb moves with the bone it hangs from)
		num++;
	}
	CG_VRB_Body( cent->currentState.number, num, a, b, r, bone );
}

/*
=================
CG_VRX_AddWounds

A skeletal body ready to draw (after the ragdoll): its wounds.
=================
*/
/*
=================
CG_VRX_NewLife

Someone new in an entity (Survival brings the dead back in the same one, somewhere else): nothing
of the last one is left on him. Told by the server's respawn event, by his being alive after
he was last seen dead, or by his turning up far from where he was last drawn.
=================
*/
static struct { int lastSeen; vec3_t origin; qboolean dead; } vrLife[MAX_CLIENTS];

void CG_VRX_NewLife( int entityNum ) {
	vrStained_t *st;
	vrHeadWounds_t *hw;
	int i;

	if ( entityNum < 0 || entityNum >= MAX_CLIENTS ) {
		return;
	}
	for ( i = 0; i < VRX_MAX_WOUNDED; i++ ) {
		if ( vrWounded[i].num && vrWounded[i].entityNum == entityNum ) {
			vrWounded[i].num = 0;
		}
	}
	st = VRX_Stained( entityNum, qfalse );
	if ( st ) {
		st->used = qfalse;
	}
	hw = VRX_HeadWounds( entityNum, qfalse );
	if ( hw ) {
		hw->lastSeen = 0;
	}
	CG_VRGore_Clear( entityNum );
	vrLife[entityNum].lastSeen = 0;
}

void CG_VRX_AddWounds( centity_t *cent, refEntity_t *body ) {
	static float pose[VRX_MAX_BONES * 12];
	static vec3_t bAxis[VRX_MAX_BONES][3], bPos[VRX_MAX_BONES];
	vrWounded_t *wd = NULL;
	vrStained_t *st;
	vrSkel_t *sk;
	const float *bones;
	vec3_t axis[3];
	qboolean dead = ( cent->currentState.eFlags & EF_DEAD ) != 0, nearBlood;
	float holes[VRX_MAX_WOUNDS][12];
	int i, k, numHoles = 0;

	if ( cent->currentState.number >= 0 && cent->currentState.number < MAX_CLIENTS ) {
		int ln = cent->currentState.number;
		if ( vrLife[ln].lastSeen && !dead &&
			 ( vrLife[ln].dead || Distance( vrLife[ln].origin, cent->lerpOrigin ) > ( cg.time - vrLife[ln].lastSeen > 1000 ? 300.0f : 500.0f ) ) ) {
			CG_VRX_NewLife( ln );
		}
		vrLife[ln].lastSeen = cg.time ? cg.time : 1;
		vrLife[ln].dead = dead;
		VectorCopy( cent->lerpOrigin, vrLife[ln].origin );
	}
	// (far off, none of it could be made out: the body isn't drawn again for it.
	// vr_goreLite: nearer)
	if ( Distance( cg.refdef.vieworg, body->origin ) > ( CG_VR_GoreQuality() >= 2 ? 1500.0f : ( CG_VR_GoreQuality() == 1 ? 1000.0f : 600.0f ) ) ) {
		return;
	}

	for ( i = 0; i < VRX_MAX_WOUNDED; i++ ) {
		if ( vrWounded[i].num && vrWounded[i].entityNum == cent->currentState.number ) {
			wd = &vrWounded[i];
			break;
		}
	}
	if ( wd ) {
		if ( wd->dead && !dead ) {
			// back alive (a respawn, someone new in the slot): a clean body
			wd->num = 0;
			wd = NULL;
		} else {
			if ( dead && !wd->dead ) {
				wd->deadAt = cg.time;
			}
			wd->dead = dead;
			wd->lastSeen = cg.time;
			if ( !CG_VRX_Gore( VRX_WOUNDS ) ) {
				wd = NULL;
			}
		}
	}
	// vr_bloodFX: the blood on his clothes, and blood flying near him that may land on him
	st = VRX_Stained( cent->currentState.number, qfalse );
	if ( st ) {
		if ( st->dead && !dead ) {
			st->used = qfalse;
			st = NULL;
		} else {
			st->dead = dead;
			st->lastSeen = cg.time;
		}
	}
	if ( st && !CG_VRB_On() ) {
		st = NULL;
	}
	nearBlood = CG_VRB_On() && cent->currentState.number < MAX_CLIENTS && CG_VRB_Near( body->origin, 110.0f );
	if ( !wd && !st && !nearBlood ) {
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
	if ( nearBlood ) {
		VRX_BloodBody( cent, sk, bPos );
	}
	if ( st ) {
		VRX_DrawStains( st, cent, body, sk, bones, bAxis, bPos );
	}
	if ( !wd ) {
		return;
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
			{
				vec3_t down;
				VectorScale( bAxis[w->bone][0], w->localT[0], down );
				VectorMA( down, w->localT[1], bAxis[w->bone][1], down );
				VectorMA( down, w->localT[2], bAxis[w->bone][2], down );
				VectorNormalize( down );
				VRX_WoundBleed( wd, w, w->bleedKind, p, n, down );
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
			if ( CG_VRB_On() ) {
				vec3_t down = { 0, 0, -1 };
				VectorMA( down, -DotProduct( down, n ), n, down );
				if ( VectorNormalize( down ) < 0.1f ) {
					VectorCopy( u, down );
				}
				if ( !w->bleedStart ) {     // a cut throat, an opened belly, a stump
					w->bleedStart = cg.time;
					w->bleedEnd = cg.time + ( w->kind == VRW_BELLY ? 20000 : 25000 );
				}
				VRX_WoundBleed( wd, w, w->kind == VRW_BELLY ? VRB_FLOW : VRB_ARTERY, p, n, down );
			} else {
				VRX_Bleed( w, p, n );
			}
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
static int vrxMarkerSeen[MAX_GENTITIES];    // when the body's marker was last in a snapshot

static void VRX_GoreMarker( centity_t *cent ) {
	int body = cent->currentState.otherEntityNum2;

	if ( body >= 0 && body < MAX_GENTITIES ) {
		vrxMarkerSeen[body] = cg.time;
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
static qboolean VRX_TryDecap( const vec3_t a, const vec3_t b, float radius ) {
	centity_t *cent;
	vec3_t ab, ap, c, neck;
	float t, len2;
	int i;

	if ( !CG_VRX_Gore( VRX_DECAP ) || !cg.snap ) {
		return qfalse;
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
			return qtrue;
		}
	}
	return qfalse;
}

static int vrxKnifeCut[MAX_CLIENTS];        // when our knife last cut each

// where the blade is: from the eye out without a headset, the hand to the tip with one
static void VRX_Blade( vec3_t a, vec3_t b, vec3_t dir ) {
	if ( cgvr.active ) {
		AngleVectors( cgvr.weaponAngles, dir, NULL, NULL );
		VectorCopy( cgvr.handOrigin[CG_VR_DominantHand()], a );
		VectorMA( a, 18.0f, dir, b );
	} else {
		VectorCopy( cg.refdef.viewaxis[0], dir );
		VectorCopy( cg.refdef.vieworg, a );
		VectorMA( a, 72.0f, dir, b );
	}
}

// the blade (a to b) into a dead body lying in its way: cut where it lands, an arm or a leg
// further through with each blow until it comes off
static void VRX_KnifeDead( const vec3_t a, const vec3_t b ) {
	vec3_t point, dir;
	int n;

	if ( !CG_VRX_Gore( VRX_VOXEL ) ) {
		return;
	}
	n = CG_Ragdoll_Trace( a, b, -1, point );
	if ( n < 0 || n >= MAX_CLIENTS || cg.time - vrxKnifeCut[n] < 250 ) {
		return;
	}
	VectorSubtract( b, a, dir );
	VectorNormalize( dir );
	vrxKnifeCut[n] = cg.time;
	if ( CG_VRGore_Knife( n, point, dir, qtrue ) ) {
		CG_Ragdoll_Hit( n, point, dir, 50.0f );
		CG_Bleed( point, n );
		if ( cgs.media.sfx_knifehit[0] ) {
			trap_S_StartSound( point, n, CHAN_AUTO, cgs.media.sfx_knifehit[rand() % 4] ? cgs.media.sfx_knifehit[rand() % 4] : cgs.media.sfx_knifehit[0] );
		}
	}
}

// the server says a knife hit body at pos. Someone on his feet: a slit where it went in, nothing
// comes off. Someone dead: at the neck the head comes off at once. Down or dead: on an arm or a
// leg the blade cuts into it (through, after enough blows), anywhere else a slit.
void CG_VRX_KnifeHit( int body, const vec3_t pos ) {
	centity_t *cent;
	vec3_t a, b, dir, ab, ap, c, point;
	qboolean down;
	float t, len2;

	if ( body < 0 || body >= MAX_CLIENTS || !cg.snap || cg.snap->ps.weapon != WP_KNIFE || body == cg.snap->ps.clientNum ) {
		return;
	}
	cent = &cg_entities[body];
	if ( cent->currentState.eType != ET_PLAYER ) {
		return;
	}
	VRX_Blade( a, b, dir );
	down = ( cent->currentState.eFlags & EF_DEAD ) || ( CG_VRX_GoreBits( cent ) & VRX_GB_DOWNED );
	if ( ( cent->currentState.eFlags & EF_DEAD ) && cent->pe.headRefEnt.hModel && !CG_VRX_Headless( cent ) && CG_VRX_Gore( VRX_DECAP ) ) {     // (the dead only: never someone still alive, down or not)
		// how near the blade passes the neck as it's drawn (the server's own idea of where a body
		// on the floor lies is rough)
		VectorSubtract( b, a, ab );
		len2 = DotProduct( ab, ab );
		VectorSubtract( cent->pe.headRefEnt.origin, a, ap );
		t = len2 > 0.001f ? DotProduct( ap, ab ) / len2 : 0;
		t = t < 0 ? 0 : ( t > 1 ? 1 : t );
		VectorMA( a, t, ab, c );
		if ( cg_vrxDebug() ) {
			CG_Printf( "VR decap: knife hit %i, the blade %.0f from the neck\n", body, Distance( c, cent->pe.headRefEnt.origin ) );
		}
		if ( Distance( c, cent->pe.headRefEnt.origin ) <= 9.0f || Distance( pos, cent->pe.headRefEnt.origin ) <= 9.0f ) {
			if ( cg.time - vrxDecapAsked[body] >= 1000 ) {
				// (dead by the time the server reads this: the blow that got here finished him)
				trap_SendClientCommand( va( "vrdecap %i %.0f %.0f %.0f", body, cent->pe.headRefEnt.origin[0],
											cent->pe.headRefEnt.origin[1], cent->pe.headRefEnt.origin[2] ) );
				vrxDecapAsked[body] = cg.time;
			}
			return;
		}
	}
	if ( !CG_VRX_Gore( VRX_VOXEL ) || cg.time - vrxKnifeCut[body] < 250 ) {
		return;
	}
	vrxKnifeCut[body] = cg.time;
	if ( down ) {
		// on the body as it lies, if the blade meets it
		if ( CG_Ragdoll_Trace( a, b, body, point ) == body && CG_VRGore_Knife( body, point, dir, qtrue ) ) {
			return;
		}
		if ( CG_VRGore_Knife( body, pos, dir, qtrue ) ) {
			return;
		}
	}
	CG_VRGore_Knife( body, pos, dir, qfalse );
}

// without a headset: a knife swing at a body's neck, or into it
void CG_VRX_KnifeSwing( void ) {
	vec3_t end;

	VectorMA( cg.refdef.vieworg, 72, cg.refdef.viewaxis[0], end );
	if ( !VRX_TryDecap( cg.refdef.vieworg, end, 8.0f ) ) {
		VectorMA( cg.refdef.vieworg, 64, cg.refdef.viewaxis[0], end );
		VRX_KnifeDead( cg.refdef.vieworg, end );
	}
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
		if ( !VRX_TryDecap( lastTip, tip, 7.0f ) ) {
			VRX_KnifeDead( cgvr.handOrigin[dom], tip );
		}
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

A shot in the balls doubles him over for a while. (Someone knocked down, VRX_GB_DOWNED, is
the physics' now: cg_ragdoll.c staggers him, drops him and stands him back up.)

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
=================
CG_VRX_DownedStyle

knocked down, alive (cg_ragdoll.c plays it as an active ragdoll): 1, or 0
=================
*/
int CG_VRX_DownedStyle( centity_t *cent ) {
	int n = cent->currentState.number;

	if ( n >= MAX_CLIENTS || ( cent->currentState.eFlags & EF_DEAD ) || !( CG_VRX_GoreBits( cent ) & VRX_GB_DOWNED ) ||
		 cg.time - vrxMarkerSeen[n] > 300 || cg.time < vrxMarkerSeen[n] ) {
		return 0;       // (a marker gone from the snapshots: whatever it last said is over)
	}
	return 1;
}

// shot and stunned (the server's G_VRX_Stun): shoved about by the bullets
qboolean CG_VRX_Stunned( centity_t *cent ) {
	int n = cent->currentState.number;

	return n < MAX_CLIENTS && !( cent->currentState.eFlags & EF_DEAD ) && vrxDownStyle[n] == VRX_STUN_MARK &&
		   cg.time - vrxMarkerSeen[n] <= 300 && cg.time >= vrxMarkerSeen[n];
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

		return;     // knocked down: the physics (cg_ragdoll.c)
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
		return;
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

	if ( n >= MAX_CLIENTS || !CG_VRX_Gore( VRX_DISMEMBER ) || CG_VRX_Gore( VRX_VOXEL ) ) {
		return;     // (the voxel gore cuts limbs off the body itself)
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

// everything up or down so the lowest of his legs and pelvis is height above the floor (his
// arms, not placed yet, don't count)
static void VRX_LowerLegs( float *pose, vrSkel_t *sk, const int *b, float floor, float height ) {
	static const int legs[7] = { 6, 7, 8, 9, 10, 11, 12 };     // LTH .. RFOOT, PELVIS in CG_VRX_GetUpPose's order
	float low = 1e9f;
	int i;

	for ( i = 0; i < 7; i++ ) {
		float z = VRX_BP( pose, b[legs[i]] )[2];
		low = z < low ? z : low;
	}
	for ( i = 0; i < sk->n; i++ ) {
		pose[i * 12 + 11] += floor + height - low;
	}
}

// the get-up's poses on the floor (CG_VRX_GetUpPose): b the bones in its order
static void VRX_GetUpLying( float *pose, vrSkel_t *sk, const int *b, int which, float floor ) {
	enum { LUA, LFA, LHAND, RUA, RFA, RHAND, LTH, LCALF, LFOOT, RTH, RCALF, RFOOT, PELVIS, SPINE, NECK, HEAD, NB };
	vec3_t v, at, elbow;
	int side;

	if ( which == VRX_GU_PRESS ) {
		// face down, chest pushed up off the floor, legs out straight behind
		VectorSet( v, 1, 0, 0.7f );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, 1, b[LTH], b[RTH] );
		VectorSet( v, 1, 0, 0.45f );
		VRX_Aim( pose, sk, b[NECK], b[HEAD], v, 1 );
		VectorSet( v, -1, 0.07f, -0.12f );
		VRX_Aim( pose, sk, b[LTH], b[LCALF], v, 1 );
		VectorSet( v, -1, -0.07f, -0.12f );
		VRX_Aim( pose, sk, b[RTH], b[RCALF], v, 1 );
		VectorSet( v, -1, 0, 0.05f );
		VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, 1 );
		VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, 1 );
		VRX_LowerLegs( pose, sk, b, floor, 3.0f );
	} else if ( which == VRX_GU_TUCK ) {
		// hips drawn back: the right knee under him, the left foot brought up ahead, leaning on his hands
		VectorSet( v, 1, 0, 0.45f );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, 1, b[LTH], b[RTH] );
		VectorSet( v, 1, 0, 0.6f );
		VRX_Aim( pose, sk, b[NECK], b[HEAD], v, 1 );
		VectorSet( v, 0.15f, -0.12f, -1 );
		VRX_Aim( pose, sk, b[RTH], b[RCALF], v, 1 );
		VectorSet( v, -1, -0.05f, -0.12f );
		VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, 1 );
		VectorSet( v, 0.8f, 0.2f, -0.55f );
		VRX_Aim( pose, sk, b[LTH], b[LCALF], v, 1 );
		VectorSet( v, -0.15f, 0.05f, -1 );
		VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, 1 );
		VRX_LowerLegs( pose, sk, b, floor, 1.5f );
	} else {
		// sat up, leaning back, knees up (the left more), feet flat
		VectorSet( v, -0.5f, 0, 0.87f );
		VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, 1, b[LTH], b[RTH] );
		VectorSet( v, 0.25f, 0, 1 );
		VRX_Aim( pose, sk, b[NECK], b[HEAD], v, 1 );
		VectorSet( v, 0.7f, 0.1f, 0.7f );
		VRX_Aim( pose, sk, b[LTH], b[LCALF], v, 1 );
		VectorSet( v, 0.5f, 0, -0.85f );
		VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, 1 );
		VectorSet( v, 0.9f, -0.15f, 0.3f );
		VRX_Aim( pose, sk, b[RTH], b[RCALF], v, 1 );
		VectorSet( v, 0.65f, 0, -0.75f );
		VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, 1 );
		VRX_LowerLegs( pose, sk, b, floor, 3.0f );
	}

	// the hands on the floor: under his shoulders (on his front), behind his hips (sat up)
	for ( side = 0; side < 2; side++ ) {
		int ua = side ? b[RUA] : b[LUA], fa = side ? b[RFA] : b[LFA], hand = side ? b[RHAND] : b[LHAND];
		float out = side ? -1.0f : 1.0f;
		if ( which == VRX_GU_SIT ) {
			VectorCopy( VRX_BP( pose, b[PELVIS] ), at );
			at[0] -= 10.0f;
			at[1] += out * 9.0f;
		} else {
			VectorCopy( VRX_BP( pose, ua ), at );
			at[0] += which == VRX_GU_TUCK ? 6.0f : 3.0f;
			at[1] += out * 3.0f;
		}
		at[2] = floor + 1.5f;
		// the elbow out to the side and back, as far as the arm needs to bend
		VectorAdd( VRX_BP( pose, ua ), at, elbow );
		VectorScale( elbow, 0.5f, elbow );
		elbow[0] -= which == VRX_GU_SIT ? 1.0f : 3.0f;
		elbow[1] += out * 3.0f;
		VRX_AimAt( pose, sk, ua, fa, elbow, 1 );
		VRX_AimAt( pose, sk, fa, hand, at, 1 );
	}
}

/*
=================
CG_VRX_KneelPose

Getting up off the floor (cg_ragdoll.c eases a knocked-down body through this into his
animation): down on the right knee, the left foot planted ahead, leaning forward with the
right hand pushing off the floor and the left forearm on the left knee. A pose in the
model's own space (x forward, y left, z up), or NULL.
=================
*/
const float *CG_VRX_KneelPose( centity_t *cent, refEntity_t *body ) {
	return CG_VRX_GetUpPose( cent, body, VRX_GU_KNEEL );
}

/*
=================
CG_VRX_GetUpPose

The poses a man goes through getting up off the floor (cg_ragdoll.c RD_GetUpFrame drives his
muscles through them, one into the next), in the model's own space (x the way he faces, y to
his left, z up), built from his animation's pose:

  VRX_GU_PRESS  off his front: pushing his chest up off the floor on his hands, legs out behind
  VRX_GU_TUCK   then his hips drawn back, the right knee under him and the left foot brought
                up ahead, still leaning on his hands
  VRX_GU_SIT    off his back: sat up, leaning back on his hands, knees up
  VRX_GU_KNEEL  up on the right knee, the left foot planted ahead, the left hand on that knee
                to push himself up, upright, looking ahead
=================
*/
const float *CG_VRX_GetUpPose( centity_t *cent, refEntity_t *body, int which ) {
	enum { LUA, LFA, LHAND, RUA, RFA, RHAND, LTH, LCALF, LFOOT, RTH, RCALF, RFOOT, PELVIS, SPINE, NECK, HEAD, NB };
	static const char *names[NB] = {
		"L UpperArm", "L Forearm", "L Hand", "R UpperArm", "R Forearm", "R Hand",
		"L Thigh", "L Calf", "L Foot", "R Thigh", "R Calf", "R Foot",
		"Pelvis", "Spine", "Neck", "Head"
	};
	static float poses[MAX_CLIENTS][VRX_GU_POSES][VRX_MAX_BONES * 12 + 16];
	int n = cent->currentState.number, b[NB], k;
	float *pose, floor;
	vrSkel_t *sk;
	refEntity_t still = *body;
	vec3_t v, at;

	if ( n < 0 || n >= MAX_CLIENTS || which < 0 || which >= VRX_GU_POSES ) {
		return NULL;
	}
	pose = poses[n][which];
	still.ragdollBones = NULL;
	sk = VRX_Skel( &still, pose );
	if ( !sk ) {
		return NULL;
	}
	for ( k = 0; k < NB; k++ ) {
		b[k] = VRX_FindBone( sk, names[k] );
		if ( b[k] < 0 ) {
			return NULL;
		}
	}
	floor = min( VRX_BP( pose, b[LFOOT] )[2], VRX_BP( pose, b[RFOOT] )[2] ) - 3.0f;     // (the foot bone is at the ankle)

	if ( which != VRX_GU_KNEEL ) {
		VRX_GetUpLying( pose, sk, b, which, floor );
		pose[sk->n * 12] = 0;
		return pose;
	}

	// upright over the front knee, a little forward, looking ahead
	VectorSet( v, 0.3f, 0, 1 );
	VRX_AimTrunk( pose, sk, b[SPINE], b[NECK], v, 1, b[LTH], b[RTH] );
	VectorSet( v, 0.15f, 0, 1 );
	VRX_Aim( pose, sk, b[NECK], b[HEAD], v, 1 );
	// right knee down, the shin flat on the floor behind; left foot planted ahead
	VectorSet( v, 0.15f, -0.12f, -1 );
	VRX_Aim( pose, sk, b[RTH], b[RCALF], v, 1 );
	VectorSet( v, -1, -0.05f, -0.12f );
	VRX_Aim( pose, sk, b[RCALF], b[RFOOT], v, 1 );
	VectorSet( v, 1, 0.12f, -0.35f );
	VRX_Aim( pose, sk, b[LTH], b[LCALF], v, 1 );
	VectorSet( v, 0.08f, 0.04f, -1 );
	VRX_Aim( pose, sk, b[LCALF], b[LFOOT], v, 1 );
	VRX_Lower( pose, sk, floor, 0 );
	// right arm hanging, a little forward and out (for balance as he rises)
	VectorSet( v, 0.25f, -0.25f, -1 );
	VRX_Aim( pose, sk, b[RUA], b[RFA], v, 1 );
	VectorSet( v, 0.6f, -0.1f, -0.7f );
	VRX_Aim( pose, sk, b[RFA], b[RHAND], v, 1 );
	// left hand on the left knee, to push himself up
	VectorCopy( VRX_BP( pose, b[LCALF] ), at );
	at[2] += 5.0f;
	at[1] += 2.0f;
	VRX_AimAt( pose, sk, b[LUA], b[LFA], at, 1 );
	at[0] += 6.0f;
	at[1] -= 3.0f;
	VRX_AimAt( pose, sk, b[LFA], b[LHAND], at, 1 );
	pose[sk->n * 12] = 0;      // nothing after the pose (no cut, CG_VRX_Dismember)
	return pose;
}
