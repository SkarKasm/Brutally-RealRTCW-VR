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

// cg_ragdoll.c -- dead characters go limp (vr_ragdolls, on by default).
//
// When a skeletal character dies its pose is taken from the renderer (trap_R_GetSkeleton)
// and handed to a small Verlet simulation: about 20 points on the joints, a rigid torso,
// limbs with loose joint limits, world collision. Every frame the points are turned back
// into bone transforms, which the renderer draws instead of the death animation.
//
// The same code runs in single player and co-op. The server stays in charge of where a
// body is: the simulation runs in fixed 1/60 s steps from data every client sees alike
// (the death pose, the body's velocity, the direction from its killer), and the body is
// kept within reach of the server's position for it. Carried bodies (VR grabbing) hang
// from the hand that holds them: the server puts the grab point in origin2 and the
// holder's client number + 1 in dl_intensity of the body's entityState.

#include "cg_local.h"

#ifndef BONEFLAG_TAG
#define BONEFLAG_TAG        1       // qfiles.h: this bone is actually a tag
#endif

#define RD_MAX_RAGDOLLS     32
#define RD_MAX_BONES        128
#define RD_MAX_LINKS        64
#define RD_STEP_MS          16          // fixed step, the same on every machine
#define RD_STEP             ( RD_STEP_MS * 0.001f )
#define RD_MAX_STEPS        6           // per frame, after a hitch the rest is dropped
#define RD_ITERATIONS       8
#define RD_GRAVITY          800.0f
#define RD_ANCHOR_SLACK     20.0f       // how far the pelvis may drift from the server's body
#define RD_SLEEP_MOVE       0.04f       // a point moving less than this per step is at rest
#define RD_SLEEP_STEPS      45
#define RD_KILL_PUSH        110.0f      // speed of the fall away from the killer

typedef enum {
	P_PELVIS, P_CHEST, P_NECK, P_HEAD, P_HEADEND,
	P_LSHOULDER, P_LELBOW, P_LWRIST, P_LHANDEND,
	P_RSHOULDER, P_RELBOW, P_RWRIST, P_RHANDEND,
	P_LHIP, P_LKNEE, P_LANKLE, P_LTOE,
	P_RHIP, P_RKNEE, P_RANKLE, P_RTOE,
	P_COUNT
} rdPoint_t;

// bone name (after the "Bip01 " prefix), and a second choice
static const char *rdPointBones[P_COUNT][2] = {
	{ "Pelvis", NULL }, { "Spine2", "Spine1" }, { "Neck", NULL }, { "Head", NULL }, { "tag_head", NULL },
	{ "L UpperArm", NULL }, { "L Forearm", NULL }, { "L Hand", NULL }, { "L Finger2", "L Finger1" },
	{ "R UpperArm", NULL }, { "R Forearm", NULL }, { "R Hand", NULL }, { "R Finger2", "R Finger1" },
	{ "L Thigh", NULL }, { "L Calf", NULL }, { "L Foot", NULL }, { "L Toe0", NULL },
	{ "R Thigh", NULL }, { "R Calf", NULL }, { "R Foot", NULL }, { "R Toe0", NULL },
};

// how far each point stays from walls and floors: roughly the thickness of the body there
static const float rdPointRadius[P_COUNT] = {
	6.5f, 6.5f, 5.0f, 4.5f, 3.5f,
	4.0f, 3.0f, 2.5f, 2.0f,
	4.0f, 3.0f, 2.5f, 2.0f,
	5.0f, 3.5f, 3.0f, 2.0f,
	5.0f, 3.5f, 3.0f, 2.0f,
};

// body segments: each bone turns with one of them
typedef enum {
	S_TORSO, S_HEAD,
	S_LUPPER, S_LFORE, S_LHAND, S_RUPPER, S_RFORE, S_RHAND,
	S_LTHIGH, S_LCALF, S_LFOOT, S_RTHIGH, S_RCALF, S_RFOOT,
	S_COUNT
} rdSegment_t;

static const struct {
	const char *keyBone;    // first bone of the segment
	int parent;             // segment it hangs from
	int from, to;           // points along it
} rdSegments[S_COUNT] = {
	{ "Pelvis", -1, P_PELVIS, P_NECK },
	{ "Head", S_TORSO, P_HEAD, P_HEADEND },
	{ "L UpperArm", S_TORSO, P_LSHOULDER, P_LELBOW },
	{ "L Forearm", S_LUPPER, P_LELBOW, P_LWRIST },
	{ "L Hand", S_LFORE, P_LWRIST, P_LHANDEND },
	{ "R UpperArm", S_TORSO, P_RSHOULDER, P_RELBOW },
	{ "R Forearm", S_RUPPER, P_RELBOW, P_RWRIST },
	{ "R Hand", S_RFORE, P_RWRIST, P_RHANDEND },
	{ "L Thigh", S_TORSO, P_LHIP, P_LKNEE },
	{ "L Calf", S_LTHIGH, P_LKNEE, P_LANKLE },
	{ "L Foot", S_LCALF, P_LANKLE, P_LTOE },
	{ "R Thigh", S_TORSO, P_RHIP, P_RKNEE },
	{ "R Calf", S_RTHIGH, P_RKNEE, P_RANKLE },
	{ "R Foot", S_RCALF, P_RANKLE, P_RTOE },
};

typedef struct {
	int a, b;
	float min, max;
} rdLink_t;

typedef struct {
	qboolean    inUse;
	int         entityNum;
	qhandle_t   model;
	int         lastSeen;           // cg.time this body was last drawn

	int         numBones;
	int         boneSeg[RD_MAX_BONES];
	qboolean    boneIsTag[RD_MAX_BONES];
	vec3_t      bone0Axis[RD_MAX_BONES][3];     // world rotation at death (columns)
	vec3_t      bone0Pos[RD_MAX_BONES];

	qboolean    hasPoint[P_COUNT];
	vec3_t      p0[P_COUNT];        // points at death
	vec3_t      p[P_COUNT], old[P_COUNT];
	int         numLinks;
	rdLink_t    links[RD_MAX_LINKS];
	qboolean    segValid[S_COUNT];
	vec3_t      torso0[3];          // torso frame at death

	vec3_t      anchorOffset;       // pelvis relative to the server's body origin
	vec3_t      lastOrigin;         // the server's body origin at the last step
	int         simTime;            // time simulated up to
	int         stillSteps;
	qboolean    asleep;

	int         pinned;             // point held by a hand, -1 none
	int         grabber;            // client number + 1 holding it, 0 none

	float       out[RD_MAX_BONES * 12];
	vec3_t      outOrigin;
} ragdoll_t;

static ragdoll_t rdPool[RD_MAX_RAGDOLLS];
static vmCvar_t vr_ragdolls;

void CG_Ragdoll_Init( void ) {
	memset( rdPool, 0, sizeof( rdPool ) );
	trap_Cvar_Register( &vr_ragdolls, "vr_ragdolls", "1", CVAR_ARCHIVE );
}

/*
==============================================================================

Small 3x3 helpers, matrices are column bases: m[k] is the k-th column

==============================================================================
*/

static void RD_MatMul( vec3_t a[3], vec3_t b[3], vec3_t out[3] ) {
	int i, j;
	vec3_t r[3];

	for ( i = 0; i < 3; i++ ) {         // column of the result
		for ( j = 0; j < 3; j++ ) {     // row
			r[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

static void RD_MatVec( vec3_t m[3], const vec3_t v, vec3_t out ) {
	vec3_t r;
	int j;

	for ( j = 0; j < 3; j++ ) {
		r[j] = m[0][j] * v[0] + m[1][j] * v[1] + m[2][j] * v[2];
	}
	VectorCopy( r, out );
}

// frame (columns) -> rotation from frame0 to frame: R = F * F0^T
static void RD_FrameRotation( vec3_t f[3], vec3_t f0[3], vec3_t out[3] ) {
	vec3_t t[3];
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			t[i][j] = f0[j][i];         // transpose
		}
	}
	RD_MatMul( f, t, out );
}

// smallest rotation turning unit vector u onto v
static void RD_Align( const vec3_t u, const vec3_t v, vec3_t out[3] ) {
	vec3_t axis;
	float c = DotProduct( u, v ), s, t, x, y, z;

	CrossProduct( u, v, axis );
	s = VectorNormalize( axis );
	if ( s < 1e-5f ) {
		memset( out, 0, sizeof( vec3_t ) * 3 );
		out[0][0] = out[1][1] = out[2][2] = 1;
		if ( c < 0 ) {      // opposite: half turn about any perpendicular axis
			PerpendicularVector( axis, u );
			x = axis[0]; y = axis[1]; z = axis[2];
			out[0][0] = 2 * x * x - 1; out[1][0] = 2 * x * y;     out[2][0] = 2 * x * z;
			out[0][1] = 2 * x * y;     out[1][1] = 2 * y * y - 1; out[2][1] = 2 * y * z;
			out[0][2] = 2 * x * z;     out[1][2] = 2 * y * z;     out[2][2] = 2 * z * z - 1;
		}
		return;
	}
	t = 1 - c;
	x = axis[0]; y = axis[1]; z = axis[2];
	// columns of the Rodrigues rotation
	out[0][0] = t * x * x + c;     out[0][1] = t * x * y + s * z; out[0][2] = t * x * z - s * y;
	out[1][0] = t * x * y - s * z; out[1][1] = t * y * y + c;     out[1][2] = t * y * z + s * x;
	out[2][0] = t * x * z + s * y; out[2][1] = t * y * z - s * x; out[2][2] = t * z * z + c;
}

static void RD_TorsoFrame( vec3_t p[P_COUNT], vec3_t f[3] ) {
	VectorSubtract( p[P_NECK], p[P_PELVIS], f[0] );
	VectorNormalize( f[0] );
	VectorSubtract( p[P_LSHOULDER], p[P_RSHOULDER], f[1] );
	VectorMA( f[1], -DotProduct( f[1], f[0] ), f[0], f[1] );
	VectorNormalize( f[1] );
	CrossProduct( f[0], f[1], f[2] );
}

/*
==============================================================================

Building a ragdoll from the death pose

==============================================================================
*/

// "Bip01 L Hand" -> "L Hand"
static const char *RD_BoneShortName( const char *name ) {
	if ( !Q_stricmpn( name, "Bip0", 4 ) ) {
		const char *s = strchr( name, ' ' );
		return s ? s + 1 : name;
	}
	return name;
}

static void RD_AddLink( ragdoll_t *rd, int a, int b, float minScale, float maxScale, float restOverride ) {
	rdLink_t *l;
	float rest;

	if ( !rd->hasPoint[a] || !rd->hasPoint[b] || rd->numLinks >= RD_MAX_LINKS ) {
		return;
	}
	rest = restOverride > 0 ? restOverride : Distance( rd->p0[a], rd->p0[b] );
	l = &rd->links[rd->numLinks++];
	l->a = a;
	l->b = b;
	l->min = rest * minScale;
	l->max = rest * maxScale;
}

// a limb of three points: both segments rigid, the joint between them bends within limits
static void RD_AddLimb( ragdoll_t *rd, int a, int b, int c, float minBend ) {
	float len;

	RD_AddLink( rd, a, b, 1, 1, 0 );
	RD_AddLink( rd, b, c, 1, 1, 0 );
	if ( rd->hasPoint[a] && rd->hasPoint[b] && rd->hasPoint[c] ) {
		len = Distance( rd->p0[a], rd->p0[b] ) + Distance( rd->p0[b], rd->p0[c] );
		RD_AddLink( rd, a, c, minBend, 1.0f, len );
	}
}

static qboolean RD_Build( ragdoll_t *rd, centity_t *cent, refEntity_t *body ) {
	static int parents[RD_MAX_BONES], flags[RD_MAX_BONES];
	static char names[RD_MAX_BONES * 32];
	static float bones[RD_MAX_BONES * 12];
	vec3_t w[3], f0[3];
	int n, i, k, s, b;
	const int torsoPoints[] = { P_PELVIS, P_CHEST, P_NECK, P_LSHOULDER, P_RSHOULDER, P_LHIP, P_RHIP };
	int keyBone[S_COUNT];
	vec3_t vel, push;

	n = trap_R_GetSkeleton( body, RD_MAX_BONES, parents, flags, names, bones );
	if ( n <= 0 ) {
		return qfalse;
	}

	memset( rd, 0, sizeof( *rd ) );
	rd->numBones = n;
	rd->pinned = -1;

	// model space -> world: columns are the entity's axes
	VectorCopy( body->axis[0], w[0] );
	VectorCopy( body->axis[1], w[1] );
	VectorCopy( body->axis[2], w[2] );
	for ( i = 0; i < n; i++ ) {
		const float *bm = bones + i * 12;
		vec3_t m[3];
		// The renderer skins vertices with the bone matrix as stored (out = M * offset), but
		// reads a tag's matrix rows as its axes, i.e. the transpose. Work with the rotation
		// each one really means, as columns.
		rd->boneIsTag[i] = ( flags[i] & BONEFLAG_TAG ) != 0;
		for ( k = 0; k < 3; k++ ) {
			if ( rd->boneIsTag[i] ) {
				m[k][0] = bm[k * 3 + 0];    // row k is axis k
				m[k][1] = bm[k * 3 + 1];
				m[k][2] = bm[k * 3 + 2];
			} else {
				m[k][0] = bm[0 * 3 + k];    // column k
				m[k][1] = bm[1 * 3 + k];
				m[k][2] = bm[2 * 3 + k];
			}
		}
		RD_MatMul( w, m, rd->bone0Axis[i] );
		RD_MatVec( w, bm + 9, rd->bone0Pos[i] );
		VectorAdd( rd->bone0Pos[i], body->origin, rd->bone0Pos[i] );
	}

	// the points
	for ( k = 0; k < P_COUNT; k++ ) {
		for ( s = 0; s < 2 && !rd->hasPoint[k]; s++ ) {
			const char *want = rdPointBones[k][s];
			if ( !want ) {
				continue;
			}
			for ( i = 0; i < n; i++ ) {
				if ( !Q_stricmp( RD_BoneShortName( names + i * 32 ), want ) ) {
					VectorCopy( rd->bone0Pos[i], rd->p0[k] );
					rd->hasPoint[k] = qtrue;
					break;
				}
			}
		}
	}
	if ( !rd->hasPoint[P_PELVIS] || !rd->hasPoint[P_NECK] || !rd->hasPoint[P_LSHOULDER] || !rd->hasPoint[P_RSHOULDER] ) {
		return qfalse;      // not a human skeleton (dogs, bosses...)
	}

	// segments, and which one each bone turns with
	for ( s = 0; s < S_COUNT; s++ ) {
		keyBone[s] = -1;
		for ( i = 0; i < n; i++ ) {
			if ( !Q_stricmp( RD_BoneShortName( names + i * 32 ), rdSegments[s].keyBone ) ) {
				keyBone[s] = i;
				break;
			}
		}
		rd->segValid[s] = s == S_TORSO ||
						  ( keyBone[s] >= 0 && rd->hasPoint[rdSegments[s].from] && rd->hasPoint[rdSegments[s].to] &&
							rd->segValid[rdSegments[s].parent] );
	}
	for ( i = 0; i < n; i++ ) {
		rd->boneSeg[i] = S_TORSO;
		for ( b = i; b >= 0; b = parents[b] ) {
			for ( s = 1; s < S_COUNT; s++ ) {
				if ( rd->segValid[s] && keyBone[s] == b ) {
					break;
				}
			}
			if ( s < S_COUNT ) {
				rd->boneSeg[i] = s;
				break;
			}
		}
	}

	// links: a rigid torso, a head on a neck, jointed limbs
	for ( i = 0; i < (int)ARRAY_LEN( torsoPoints ); i++ ) {
		for ( k = i + 1; k < (int)ARRAY_LEN( torsoPoints ); k++ ) {
			RD_AddLink( rd, torsoPoints[i], torsoPoints[k], 1, 1, 0 );
		}
	}
	RD_AddLink( rd, P_NECK, P_HEAD, 1, 1, 0 );
	RD_AddLink( rd, P_HEAD, P_HEADEND, 1, 1, 0 );
	RD_AddLink( rd, P_CHEST, P_HEADEND, 0.85f, 1.05f, 0 );    // the head lolls, but not far
	RD_AddLink( rd, P_PELVIS, P_HEADEND, 0.9f, 1.05f, 0 );
	RD_AddLimb( rd, P_LSHOULDER, P_LELBOW, P_LWRIST, 0.3f );
	RD_AddLimb( rd, P_RSHOULDER, P_RELBOW, P_RWRIST, 0.3f );
	RD_AddLimb( rd, P_LHIP, P_LKNEE, P_LANKLE, 0.62f );     // knees bend to about 110 degrees
	RD_AddLimb( rd, P_RHIP, P_RKNEE, P_RANKLE, 0.62f );
	// hips: a knee can't come up into the chest, the legs don't pass through each other
	RD_AddLink( rd, P_LKNEE, P_NECK, 0.72f, 1.1f, 0 );
	RD_AddLink( rd, P_RKNEE, P_NECK, 0.72f, 1.1f, 0 );
	RD_AddLink( rd, P_LKNEE, P_RKNEE, 0.45f, 4.0f, 0 );
	RD_AddLink( rd, P_LANKLE, P_RANKLE, 0.35f, 6.0f, 0 );
	RD_AddLimb( rd, P_LELBOW, P_LWRIST, P_LHANDEND, 0.6f );
	RD_AddLimb( rd, P_RELBOW, P_RWRIST, P_RHANDEND, 0.6f );
	RD_AddLimb( rd, P_LKNEE, P_LANKLE, P_LTOE, 0.72f );
	RD_AddLimb( rd, P_RKNEE, P_RANKLE, P_RTOE, 0.72f );

	RD_TorsoFrame( rd->p0, f0 );
	memcpy( rd->torso0, f0, sizeof( f0 ) );

	// starting motion: the body's own velocity, plus a fall away from whoever killed it
	VectorCopy( cent->currentState.pos.trDelta, vel );
	VectorClear( push );
	if ( cent->currentState.otherEntityNum2 >= 0 && cent->currentState.otherEntityNum2 < MAX_GENTITIES &&
		 cent->currentState.otherEntityNum2 != cent->currentState.number ) {
		centity_t *killer = &cg_entities[cent->currentState.otherEntityNum2];
		VectorSubtract( cent->lerpOrigin, killer->lerpOrigin, push );
		push[2] = 0;
		if ( VectorNormalize( push ) > 1.0f ) {
			VectorScale( push, RD_KILL_PUSH, push );
		}
	}
	for ( k = 0; k < P_COUNT; k++ ) {
		vec3_t v;
		float upper;

		if ( !rd->hasPoint[k] ) {
			continue;
		}
		// the upper body is pushed harder, so the body tips over
		upper = ( rd->p0[k][2] - rd->p0[P_PELVIS][2] ) / 40.0f;
		upper = upper < 0 ? 0.3f : ( upper > 1 ? 1.3f : 0.3f + upper );
		VectorMA( vel, upper, push, v );
		VectorCopy( rd->p0[k], rd->p[k] );
		VectorMA( rd->p[k], -RD_STEP, v, rd->old[k] );
	}

	VectorSubtract( rd->p0[P_PELVIS], cent->lerpOrigin, rd->anchorOffset );
	VectorCopy( cent->lerpOrigin, rd->lastOrigin );
	rd->entityNum = cent->currentState.number;
	rd->model = body->hModel;
	rd->simTime = cg.time;
	rd->inUse = qtrue;
	return qtrue;
}

/*
==============================================================================

Simulation

==============================================================================
*/

static void RD_Collide( ragdoll_t *rd, int k, const vec3_t from ) {
	float r = rdPointRadius[k];
	vec3_t mins = { -r, -r, -r }, maxs = { r, r, r };
	trace_t tr;
	vec3_t v, vn;

	CG_Trace( &tr, from, mins, maxs, rd->p[k], rd->entityNum, MASK_SOLID );
	if ( tr.startsolid ) {
		return;     // already embedded: leave it to the links to pull it out
	}
	if ( tr.fraction < 1.0f ) {
		VectorMA( tr.endpos, 0.1f, tr.plane.normal, rd->p[k] );
		// friction: drop the motion into the surface, slow the sliding
		VectorSubtract( rd->p[k], rd->old[k], v );
		VectorScale( tr.plane.normal, DotProduct( v, tr.plane.normal ), vn );
		VectorSubtract( v, vn, v );
		VectorMA( rd->p[k], -0.5f, v, rd->old[k] );
	}
}

// the hand holding the body (VR grabbing), in the world
static qboolean RD_GrabPoint( ragdoll_t *rd, centity_t *cent, vec3_t out ) {
	int grabber = cent->currentState.dl_intensity;

	if ( grabber <= 0 || grabber > MAX_CLIENTS ) {
		return qfalse;
	}
	// only while whoever holds it is around (left the game, out of view...)
	if ( grabber - 1 != cg.snap->ps.clientNum && !cg_entities[grabber - 1].currentValid ) {
		return qfalse;
	}
	if ( grabber - 1 == cg.snap->ps.clientNum && cgvr.active ) {
		VectorCopy( cgvr.handOrigin[!CG_VR_DominantHand()], out );     // our own hand, exactly
	} else {
		// the grab point moves with the body, which is interpolated between snapshots
		vec3_t rel;
		VectorSubtract( cent->currentState.origin2, cent->currentState.pos.trBase, rel );
		VectorAdd( cent->lerpOrigin, rel, out );
	}
	return qtrue;
}

static void RD_Step( ragdoll_t *rd, centity_t *cent, const vec3_t grabPoint, qboolean grabbed ) {
	vec3_t start[P_COUNT], v, target, err;
	float moved = 0, d;
	int k, it, i;
	qboolean originMoved;

	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hasPoint[k] ) {
			continue;
		}
		VectorCopy( rd->p[k], start[k] );
		VectorSubtract( rd->p[k], rd->old[k], v );
		VectorScale( v, 0.995f, v );
		VectorCopy( rd->p[k], rd->old[k] );
		VectorAdd( rd->p[k], v, rd->p[k] );
		rd->p[k][2] -= RD_GRAVITY * RD_STEP * RD_STEP;
		RD_Collide( rd, k, start[k] );
	}

	if ( grabbed && rd->pinned >= 0 ) {
		VectorCopy( grabPoint, rd->p[rd->pinned] );
	}

	for ( it = 0; it < RD_ITERATIONS; it++ ) {
		for ( i = 0; i < rd->numLinks; i++ ) {
			rdLink_t *l = &rd->links[i];
			float len, want, wa, wb;

			VectorSubtract( rd->p[l->b], rd->p[l->a], v );
			len = VectorLength( v );
			if ( len < 1e-4f ) {
				continue;
			}
			want = len < l->min ? l->min : ( len > l->max ? l->max : len );
			if ( want == len ) {
				continue;
			}
			wa = rd->pinned == l->a ? 0 : 0.5f;
			wb = rd->pinned == l->b ? 0 : 0.5f;
			if ( !wa && !wb ) {
				continue;
			}
			wa /= wa + wb;
			wb = 1 - wa;
			VectorScale( v, ( len - want ) / len, v );
			VectorMA( rd->p[l->a], wa, v, rd->p[l->a] );
			VectorMA( rd->p[l->b], -wb, v, rd->p[l->b] );
		}
	}

	// the links must not drag points through walls
	for ( k = 0; k < P_COUNT; k++ ) {
		if ( rd->hasPoint[k] && k != rd->pinned ) {
			float r = rdPointRadius[k];
			vec3_t mins = { -r, -r, -r }, maxs = { r, r, r };
			trace_t tr;
			CG_Trace( &tr, start[k], mins, maxs, rd->p[k], rd->entityNum, MASK_SOLID );
			if ( tr.fraction < 1.0f && !tr.startsolid ) {
				VectorMA( tr.endpos, 0.1f, tr.plane.normal, rd->p[k] );
			}
			d = Distance( rd->p[k], start[k] );
			if ( d > moved ) {
				moved = d;
			}
		}
	}

	// stay with the server's body: the same place for every player. Only while the
	// server moves the body (thrown, carried off, pushed by a mover), or if the ragdoll
	// has wandered far; otherwise it would fight the floor and never come to rest.
	originMoved = Distance( cent->lerpOrigin, rd->lastOrigin ) > 0.25f;
	VectorCopy( cent->lerpOrigin, rd->lastOrigin );
	if ( !grabbed ) {
		VectorAdd( cent->lerpOrigin, rd->anchorOffset, target );
		VectorSubtract( target, rd->p[P_PELVIS], err );
		err[2] = 0;
		d = VectorLength( err );
		if ( d > ( originMoved ? RD_ANCHOR_SLACK : RD_ANCHOR_SLACK * 3 ) ) {
			VectorScale( err, ( d - RD_ANCHOR_SLACK ) / d, err );
			// a thrown body is carried along, slack is taken up without a jolt
			VectorScale( err, 0.25f, err );
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hasPoint[k] ) {
					VectorAdd( rd->p[k], err, rd->p[k] );
					VectorAdd( rd->old[k], err, rd->old[k] );
				}
			}
			moved = 1;
		}
		// the body lifted well above the ragdoll (thrown, carried up a ledge)
		d = target[2] - rd->p[P_PELVIS][2];
		if ( d > 40.0f && originMoved ) {
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hasPoint[k] ) {
					rd->p[k][2] += 0.25f * ( d - 40.0f );
					rd->old[k][2] += 0.25f * ( d - 40.0f );
				}
			}
			moved = 1;
		}
	}

	if ( moved < RD_SLEEP_MOVE && !grabbed ) {
		if ( ++rd->stillSteps > RD_SLEEP_STEPS ) {
			rd->asleep = qtrue;
		}
	} else {
		rd->stillSteps = 0;
	}
}

/*
==============================================================================

Points -> bones

==============================================================================
*/

static void RD_Pose( ragdoll_t *rd ) {
	vec3_t segR[S_COUNT][3], f[3], d0, d, u;
	int s, i, k, anchor;

	for ( s = 0; s < S_COUNT; s++ ) {
		if ( s == S_TORSO ) {
			RD_TorsoFrame( rd->p, f );
			RD_FrameRotation( f, rd->torso0, segR[s] );
			continue;
		}
		if ( !rd->segValid[s] ) {
			continue;
		}
		// turn the parent's rotation so the segment points where its points are
		VectorSubtract( rd->p0[rdSegments[s].to], rd->p0[rdSegments[s].from], d0 );
		VectorNormalize( d0 );
		RD_MatVec( segR[rdSegments[s].parent], d0, u );
		VectorSubtract( rd->p[rdSegments[s].to], rd->p[rdSegments[s].from], d );
		if ( VectorNormalize( d ) < 1e-4f ) {
			VectorCopy( u, d );
		}
		RD_Align( u, d, f );
		RD_MatMul( f, segR[rdSegments[s].parent], segR[s] );
	}

	VectorCopy( rd->p[P_PELVIS], rd->outOrigin );
	for ( i = 0; i < rd->numBones; i++ ) {
		float *o = rd->out + i * 12;
		vec3_t m[3], rel, pos;

		s = rd->boneSeg[i];
		anchor = s == S_TORSO ? P_PELVIS : rdSegments[s].from;

		RD_MatMul( segR[s], rd->bone0Axis[i], m );
		VectorSubtract( rd->bone0Pos[i], rd->p0[anchor], rel );
		RD_MatVec( segR[s], rel, pos );
		VectorAdd( pos, rd->p[anchor], pos );

		// renderer layout: matrix rows, then the translation (model space, axis = identity)
		for ( k = 0; k < 3; k++ ) {
			if ( rd->boneIsTag[i] ) {
				o[k * 3 + 0] = m[k][0];
				o[k * 3 + 1] = m[k][1];
				o[k * 3 + 2] = m[k][2];
			} else {
				o[0 * 3 + k] = m[k][0];
				o[1 * 3 + k] = m[k][1];
				o[2 * 3 + k] = m[k][2];
			}
		}
		VectorSubtract( pos, rd->outOrigin, o + 9 );
	}
}

/*
==============================================================================

Per body, from CG_Player

==============================================================================
*/

static ragdoll_t *RD_Find( int entityNum ) {
	int i;

	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		if ( rdPool[i].inUse && rdPool[i].entityNum == entityNum ) {
			return &rdPool[i];
		}
	}
	return NULL;
}

static ragdoll_t *RD_Alloc( void ) {
	ragdoll_t *oldest = &rdPool[0];
	int i;

	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		if ( !rdPool[i].inUse ) {
			return &rdPool[i];
		}
		if ( rdPool[i].lastSeen < oldest->lastSeen ) {
			oldest = &rdPool[i];
		}
	}
	return oldest;      // the body seen longest ago goes back to its death animation
}

/*
=================
CG_Ragdoll_Apply

Called with a skeletal character's body just before it is drawn. A dead one is drawn
as a ragdoll instead of its animation.
=================
*/
void CG_Ragdoll_Apply( centity_t *cent, refEntity_t *body ) {
	ragdoll_t *rd = RD_Find( cent->currentState.number );
	vec3_t grabPoint;
	qboolean grabbed;
	int steps = 0;

	trap_Cvar_Update( &vr_ragdolls );
	if ( !( cent->currentState.eFlags & EF_DEAD ) || !vr_ragdolls.integer || !body->hModel ||
		 cent->currentState.eType != ET_PLAYER ) {
		if ( rd ) {
			rd->inUse = qfalse;     // alive again (respawned, revived) or ragdolls off
		}
		return;
	}

	if ( rd && rd->model != body->hModel ) {
		rd->inUse = qfalse;
		rd = NULL;
	}
	if ( !rd && cent->currentState.clientNum != cent->currentState.number &&
		 cent->currentState.clientNum >= 0 && cent->currentState.clientNum < MAX_CLIENTS ) {
		// a dead player's body left behind as a corpse (co-op respawns): the corpse
		// carries on with the player's ragdoll instead of starting over
		ragdoll_t *player = RD_Find( cent->currentState.clientNum );
		if ( player && player->model == body->hModel && Distance( player->p[P_PELVIS], cent->lerpOrigin ) < 96.0f ) {
			rd = player;
			rd->entityNum = cent->currentState.number;
			VectorSubtract( rd->p[P_PELVIS], cent->lerpOrigin, rd->anchorOffset );
			VectorCopy( cent->lerpOrigin, rd->lastOrigin );
		}
	}
	if ( !rd ) {
		rd = RD_Alloc();
		if ( !RD_Build( rd, cent, body ) ) {
			rd->inUse = qfalse;
			return;
		}
		if ( vr_ragdolls.integer > 1 ) {    // 2: report each one (testing)
			int k, points = 0;
			for ( k = 0; k < P_COUNT; k++ ) {
				points += rd->hasPoint[k];
			}
			CG_Printf( "ragdoll: entity %d, %d bones, %d points, %d links\n", rd->entityNum, rd->numBones, points, rd->numLinks );
		}
		RD_Pose( rd );
	}
	rd->lastSeen = cg.time;

	grabbed = RD_GrabPoint( rd, cent, grabPoint );
	if ( grabbed && rd->grabber != cent->currentState.dl_intensity ) {
		// picked up: hold it by the point nearest the hand
		float best = 1e9f, dist;
		int k;
		rd->pinned = -1;
		for ( k = 0; k < P_COUNT; k++ ) {
			if ( rd->hasPoint[k] && ( dist = Distance( rd->p[k], grabPoint ) ) < best ) {
				best = dist;
				rd->pinned = k;
			}
		}
	} else if ( !grabbed && rd->grabber ) {
		// let go: the body keeps the hand's motion, and stays with the server's body
		rd->pinned = -1;
		VectorSubtract( rd->p[P_PELVIS], cent->lerpOrigin, rd->anchorOffset );
	}
	rd->grabber = grabbed ? cent->currentState.dl_intensity : 0;
	if ( grabbed ) {
		rd->asleep = qfalse;
	} else if ( rd->asleep ) {
		// woken when the server moves the body (thrown, pushed by a mover...)
		if ( Distance( cent->lerpOrigin, rd->lastOrigin ) > 0.5f ) {
			rd->asleep = qfalse;
			rd->stillSteps = 0;
		}
	}

	// fixed steps (the same on every machine); one simulation per frame however many
	// views draw it (both eyes and the HUD layer in VR)
	if ( rd->simTime > cg.time || cg.time - rd->simTime > 1000 ) {
		rd->simTime = cg.time;      // time went backwards (map restart) or a long pause
	}
	while ( rd->simTime + RD_STEP_MS <= cg.time ) {
		rd->simTime += RD_STEP_MS;
		if ( !rd->asleep && steps < RD_MAX_STEPS ) {
			RD_Step( rd, cent, grabPoint, grabbed );
			steps++;
		}
	}
	if ( steps ) {
		RD_Pose( rd );
	}

	body->ragdollBones = rd->out;
	body->ragdollMagic = RAGDOLL_MAGIC;
	VectorCopy( rd->outOrigin, body->origin );
	VectorCopy( rd->outOrigin, body->oldorigin );
	VectorCopy( rd->outOrigin, body->lightingOrigin );
	AxisClear( body->axis );
	AxisClear( body->torsoAxis );
}

/*
=================
CG_Ragdoll_DebugHead

vr_ragdolls 3: how far the head model ended up from the ragdoll's head (testing)
=================
*/
void CG_Ragdoll_DebugHead( centity_t *cent, const vec3_t headOrigin ) {
	static int next;
	ragdoll_t *rd;

	if ( vr_ragdolls.integer < 3 || cg.time < next || !( rd = RD_Find( cent->currentState.number ) ) ) {
		return;
	}
	next = cg.time + 500;
	CG_Printf( "ragdoll %d: head model %.1f from the head bone, pelvis %.1f %.1f %.1f, head %.1f %.1f %.1f%s\n",
			   rd->entityNum, Distance( headOrigin, rd->p[P_HEAD] ), rd->p[P_PELVIS][0], rd->p[P_PELVIS][1],
			   rd->p[P_PELVIS][2], rd->p[P_HEAD][0], rd->p[P_HEAD][1], rd->p[P_HEAD][2], rd->asleep ? " (at rest)" : "" );
}
