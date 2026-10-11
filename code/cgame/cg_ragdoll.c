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
#define RD_MAX_LINKS        96
#define RD_STEP_MS          16          // fixed step, the same on every machine
#define RD_STEP             ( RD_STEP_MS * 0.001f )
#define RD_MAX_STEPS        6           // per frame, after a hitch the rest is dropped
#define RD_ITERATIONS       12
#define RD_MASK             ( MASK_SOLID | CONTENTS_PLAYERCLIP )    // props are often only player-clipped
#define RD_GRAVITY          "400"       // vr_ragdollGravity: about real gravity at RTCW's scale (the game's own 800 slams bodies down)
#define RD_HEAD_LEN         9.0f        // neck joint to the top of the skull
#define RD_ANCHOR_SLACK     20.0f       // how far the pelvis may drift from the server's body
#define RD_SETTLE_MS        7000        // after this long on the ground a body stops for good (no twitching)
#define RD_SLEEP_MOVE       0.04f       // a point moving less than this per step is at rest
#define RD_SLEEP_STEPS      45
#define RD_KILL_PUSH        110.0f      // speed of the fall away from the killer
#define RD_MAX_TONE         16
#define RD_GRAB_SPEED       900.0f      // a held point follows the hand this fast (no snapping)
#define RD_DAMP             0.985f      // velocity kept per step: limbs settle instead of swinging on and on
#define RD_DAMP_HELD        0.93f       // carried: the body hangs from the hand instead of whirling round it
#define RD_MAX_SPEED        650.0f      // no point moves faster (shots, blasts and links can't fling it away)

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

	int         startTime;          // when it went limp (muscle tone fades from here)
	int         settleBy;           // from then on, lying on the ground, it stops for good
	qboolean    settled;            // stopped for good: only a kick, a blast or a hand moves it again
	int         numTone;
	rdLink_t    tone[RD_MAX_TONE];  // posture links, fading (RD_Tone)

	int         pinned;             // point held by a hand, -1 none
	int         grabber;            // client number + 1 holding it, 0 none

	float       out[RD_MAX_BONES * 12];
	vec3_t      outOrigin;
} ragdoll_t;

static ragdoll_t rdPool[RD_MAX_RAGDOLLS];
static ragdoll_t *RD_Find( int entityNum );
static void RD_ApplyPending( ragdoll_t *rd );
static vmCvar_t vr_ragdolls;
static vmCvar_t vr_ragdollGravity;

void CG_Ragdoll_Init( void ) {
	memset( rdPool, 0, sizeof( rdPool ) );
	trap_Cvar_Register( &vr_ragdolls, "vr_ragdolls", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_ragdollGravity, "vr_ragdollGravity", RD_GRAVITY, CVAR_ARCHIVE );
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

// rotation by angle (radians) about a unit axis
static void RD_AxisAngle( const vec3_t axis, float angle, vec3_t out[3] ) {
	float s = sin( angle ), c = cos( angle ), t = 1 - c;
	float x = axis[0], y = axis[1], z = axis[2];

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

Joints and the body against itself

==============================================================================
*/

// a direction in the torso's frame: f[0] up, f[1] left, f[2] back
static void RD_LimitCone( ragdoll_t *rd, int root, int tip, vec3_t f[3], float side,
						  float backMax, float fwdMax, float inMax, float outMax, qboolean hangsDown ) {
	vec3_t d, fwd, out, down, nd;
	float len, a, b, fc, dc, oc, ca, cb;

	if ( !rd->hasPoint[root] || !rd->hasPoint[tip] || tip == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->p[tip], rd->p[root], d );
	len = VectorNormalize( d );
	if ( len < 1e-3f ) {
		return;
	}
	VectorNegate( f[2], fwd );
	VectorScale( f[1], side, out );     // away from the body's middle
	if ( hangsDown ) {
		VectorNegate( f[0], down );     // legs rest pointing down
	} else {
		VectorCopy( f[0], down );       // the head rests pointing up
	}

	fc = DotProduct( d, fwd );
	dc = DotProduct( d, down );
	oc = DotProduct( d, out );
	a = RAD2DEG( atan2( fc, dc ) );                     // swing forwards (+) / back (-)
	b = RAD2DEG( atan2( oc, sqrt( fc * fc + dc * dc ) ) );  // out (+) / across (-)
	if ( a >= -backMax && a <= fwdMax && b >= -inMax && b <= outMax ) {
		return;
	}
	a = a < -backMax ? -backMax : ( a > fwdMax ? fwdMax : a );
	b = b < -inMax ? -inMax : ( b > outMax ? outMax : b );
	ca = DEG2RAD( a );
	cb = DEG2RAD( b );
	VectorScale( down, cos( cb ) * cos( ca ), nd );
	VectorMA( nd, cos( cb ) * sin( ca ), fwd, nd );
	VectorMA( nd, sin( cb ), out, nd );
	// half to the limb, half to the body
	VectorMA( rd->p[root], len, nd, nd );
	VectorSubtract( nd, rd->p[tip], d );
	VectorMA( rd->p[tip], 0.5f, d, rd->p[tip] );
	if ( root != rd->pinned ) {
		VectorMA( rd->p[root], -0.5f, d, rd->p[root] );
	}
}

// the bend at b between a-b and b-c stays within maxDev degrees of how it was at death
static void RD_LimitBend( ragdoll_t *rd, int a, int b, int c, float maxDev ) {
	vec3_t u, v, u0, v0, axis, nd;
	float ang, ang0, want, len, s, co;

	if ( !rd->hasPoint[a] || !rd->hasPoint[b] || !rd->hasPoint[c] || c == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->p[b], rd->p[a], u );
	VectorSubtract( rd->p[c], rd->p[b], v );
	VectorSubtract( rd->p0[b], rd->p0[a], u0 );
	VectorSubtract( rd->p0[c], rd->p0[b], v0 );
	if ( VectorNormalize( u ) < 1e-3f || VectorNormalize( u0 ) < 1e-3f || VectorNormalize( v0 ) < 1e-3f ) {
		return;
	}
	len = VectorNormalize( v );
	if ( len < 1e-3f ) {
		return;
	}
	ang = RAD2DEG( acos( Com_Clamp( -1, 1, DotProduct( u, v ) ) ) );
	ang0 = RAD2DEG( acos( Com_Clamp( -1, 1, DotProduct( u0, v0 ) ) ) );
	if ( ang <= ang0 + maxDev && ang >= ang0 - maxDev ) {
		return;
	}
	want = ang > ang0 + maxDev ? ang0 + maxDev : ang0 - maxDev;
	if ( want < 0 ) {
		want = 0;
	}
	// turn v in the plane of u and v to the allowed bend
	CrossProduct( u, v, axis );
	if ( VectorNormalize( axis ) < 1e-4f ) {
		return;
	}
	CrossProduct( axis, u, nd );        // perpendicular to u, towards v
	s = sin( DEG2RAD( want ) );
	co = cos( DEG2RAD( want ) );
	VectorScale( u, co, v );
	VectorMA( v, s, nd, v );
	VectorMA( rd->p[b], len, v, rd->p[c] );
}

// knees only bend backwards: the calf stays off the thigh's front and mostly out of its sides
static void RD_LimitKnee( ragdoll_t *rd, int hip, int knee, int ankle, vec3_t f[3] ) {
	vec3_t t, h, fwd, c, nd;
	float len, cf, ch;

	if ( !rd->hasPoint[hip] || !rd->hasPoint[knee] || !rd->hasPoint[ankle] || ankle == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->p[knee], rd->p[hip], t );
	if ( VectorNormalize( t ) < 1e-3f ) {
		return;
	}
	VectorMA( f[1], -DotProduct( f[1], t ), t, h );     // the hinge axis: the body's left, square to the thigh
	if ( VectorNormalize( h ) < 0.3f ) {
		return;     // leg flung out sideways: no clear hinge
	}
	CrossProduct( t, h, fwd );      // the way a knee can't go
	VectorSubtract( rd->p[ankle], rd->p[knee], c );
	len = VectorNormalize( c );
	if ( len < 1e-3f ) {
		return;
	}
	cf = DotProduct( c, fwd );
	ch = DotProduct( c, h );
	if ( cf <= 0.1f && ch <= 0.35f && ch >= -0.35f ) {
		return;
	}
	VectorCopy( c, nd );
	if ( cf > 0.1f ) {
		VectorMA( nd, 0.1f - cf, fwd, nd );
	}
	if ( ch > 0.35f || ch < -0.35f ) {
		VectorMA( nd, ( ch > 0 ? 0.35f : -0.35f ) - ch, h, nd );
	}
	if ( VectorNormalize( nd ) < 1e-3f ) {
		return;
	}
	VectorMA( rd->p[knee], len, nd, rd->p[ankle] );
}

static void RD_Joints( ragdoll_t *rd ) {
	vec3_t f[3];

	RD_TorsoFrame( rd->p, f );
	// hips: well forward, a little back, a little across, further out
	RD_LimitCone( rd, P_LHIP, P_LKNEE, f, 1, 25, 115, 20, 45, qtrue );
	RD_LimitCone( rd, P_RHIP, P_RKNEE, f, -1, 25, 115, 20, 45, qtrue );
	// the head on the neck, and the top of the head (which way the face can turn)
	RD_LimitCone( rd, P_NECK, P_HEAD, f, 1, 50, 60, 45, 45, qfalse );
	RD_LimitCone( rd, P_HEAD, P_HEADEND, f, 1, 45, 60, 40, 40, qfalse );
	RD_LimitBend( rd, P_NECK, P_HEAD, P_HEADEND, 50 );
	// hands on the wrists, feet on the ankles
	RD_LimitBend( rd, P_LELBOW, P_LWRIST, P_LHANDEND, 70 );
	RD_LimitBend( rd, P_RELBOW, P_RWRIST, P_RHANDEND, 70 );
	RD_LimitBend( rd, P_LKNEE, P_LANKLE, P_LTOE, 40 );
	RD_LimitBend( rd, P_RKNEE, P_RANKLE, P_RTOE, 40 );
	RD_LimitKnee( rd, P_LHIP, P_LKNEE, P_LANKLE, f );
	RD_LimitKnee( rd, P_RHIP, P_RKNEE, P_RANKLE, f );
	// shoulders: arms go almost anywhere, but not far round behind the back
	RD_LimitCone( rd, P_LSHOULDER, P_LELBOW, f, 1, 60, 180, 60, 180, qtrue );
	RD_LimitCone( rd, P_RSHOULDER, P_RELBOW, f, -1, 60, 180, 60, 180, qtrue );
}

// body parts as capsules: two points and a radius
typedef struct {
	int a, b;
	float r;
} rdCapsule_t;

static const rdCapsule_t rdCapsules[] = {
	{ P_PELVIS, P_NECK, 6.5f },         // 0 torso
	{ P_HEAD, P_HEADEND, 4.5f },        // 1 head
	{ P_LSHOULDER, P_LELBOW, 3.0f },    // 2
	{ P_LELBOW, P_LWRIST, 2.6f },       // 3
	{ P_RSHOULDER, P_RELBOW, 3.0f },    // 4
	{ P_RELBOW, P_RWRIST, 2.6f },       // 5
	{ P_LHIP, P_LKNEE, 4.5f },          // 6
	{ P_LKNEE, P_LANKLE, 3.5f },        // 7
	{ P_RHIP, P_RKNEE, 4.5f },          // 8
	{ P_RKNEE, P_RANKLE, 3.5f },        // 9
};
#define RD_NUM_CAPSULES ( (int)ARRAY_LEN( rdCapsules ) )

static void RD_Lerp( const vec3_t a, const vec3_t b, float t, vec3_t out ) {
	out[0] = a[0] + t * ( b[0] - a[0] );
	out[1] = a[1] + t * ( b[1] - a[1] );
	out[2] = a[2] + t * ( b[2] - a[2] );
}

// closest points between segments p1-q1 and p2-q2: parameters s, t
static float RD_SegmentDist( const vec3_t p1, const vec3_t q1, const vec3_t p2, const vec3_t q2, float *s, float *t ) {
	vec3_t d1, d2, r, c1, c2;
	float a, e, f, b, c, denom;

	VectorSubtract( q1, p1, d1 );
	VectorSubtract( q2, p2, d2 );
	VectorSubtract( p1, p2, r );
	a = DotProduct( d1, d1 );
	e = DotProduct( d2, d2 );
	f = DotProduct( d2, r );
	if ( a < 1e-6f && e < 1e-6f ) {
		*s = *t = 0;
	} else if ( a < 1e-6f ) {
		*s = 0;
		*t = Com_Clamp( 0, 1, f / e );
	} else {
		c = DotProduct( d1, r );
		if ( e < 1e-6f ) {
			*t = 0;
			*s = Com_Clamp( 0, 1, -c / a );
		} else {
			b = DotProduct( d1, d2 );
			denom = a * e - b * b;
			*s = denom > 1e-6f ? Com_Clamp( 0, 1, ( b * f - c * e ) / denom ) : 0;
			*t = ( b * *s + f ) / e;
			if ( *t < 0 ) {
				*t = 0;
				*s = Com_Clamp( 0, 1, -c / a );
			} else if ( *t > 1 ) {
				*t = 1;
				*s = Com_Clamp( 0, 1, ( b - c ) / a );
			}
		}
	}
	VectorMA( p1, *s, d1, c1 );
	VectorMA( p2, *t, d2, c2 );
	return Distance( c1, c2 );
}

// push a capsule's two points along n, shared by where the contact is (pinned points don't move)
static void RD_MoveCapsule( ragdoll_t *rd, const rdCapsule_t *c, float s, const vec3_t n, float amount ) {
	float wa = c->a == rd->pinned ? 0 : 1 - s, wb = c->b == rd->pinned ? 0 : s, sum = wa * wa + wb * wb;

	if ( sum < 1e-4f ) {
		return;
	}
	VectorMA( rd->p[c->a], amount * wa / sum, n, rd->p[c->a] );
	VectorMA( rd->p[c->b], amount * wb / sum, n, rd->p[c->b] );
}

static void RD_SelfCollide( ragdoll_t *rd ) {
	int i, j;

	for ( i = 0; i < RD_NUM_CAPSULES; i++ ) {
		const rdCapsule_t *ci = &rdCapsules[i];
		if ( !rd->hasPoint[ci->a] || !rd->hasPoint[ci->b] ) {
			continue;
		}
		for ( j = i + 1; j < RD_NUM_CAPSULES; j++ ) {
			const rdCapsule_t *cj = &rdCapsules[j];
			vec3_t pi, pj, n;
			float s, t, d, pen;

			if ( !rd->hasPoint[cj->a] || !rd->hasPoint[cj->b] ) {
				continue;
			}
			// joined parts (sharing a point, or a limb's root on the torso) are the joints' business
			if ( ci->a == cj->a || ci->a == cj->b || ci->b == cj->a || ci->b == cj->b ) {
				continue;
			}
			if ( i == 0 && ( j == 1 || j == 2 || j == 4 || j == 6 || j == 8 ) ) {
				continue;
			}
			d = RD_SegmentDist( rd->p[ci->a], rd->p[ci->b], rd->p[cj->a], rd->p[cj->b], &s, &t );
			pen = ci->r + cj->r - d;
			if ( pen <= 0 ) {
				continue;
			}
			RD_Lerp( rd->p[ci->a], rd->p[ci->b], s, pi );
			RD_Lerp( rd->p[cj->a], rd->p[cj->b], t, pj );
			VectorSubtract( pi, pj, n );
			if ( VectorNormalize( n ) < 1e-4f ) {
				continue;
			}
			RD_MoveCapsule( rd, ci, s, n, pen * 0.5f );
			RD_MoveCapsule( rd, cj, t, n, -pen * 0.5f );
		}
	}
}

/*
=================
RD_Tone

Muscle tone just after death: the posture is held, fading, the legs giving out first and
the body after, so it buckles and folds instead of toppling over stiff.
=================
*/
#define RD_TONE_LEGS_MS     250
#define RD_TONE_BODY_MS     700

static void RD_Tone( ragdoll_t *rd ) {
	float age = (float)( rd->simTime - rd->startTime ), k;
	int i;

	if ( age >= RD_TONE_BODY_MS || age < 0 ) {
		return;
	}
	for ( i = 0; i < rd->numTone; i++ ) {
		rdLink_t *l = &rd->tone[i];
		vec3_t v;
		float len, life = l->max;       // (max holds how long this one lasts)

		if ( age >= life ) {
			continue;
		}
		k = 1.0f - age / life;
		k = 0.35f * k * k;
		VectorSubtract( rd->p[l->b], rd->p[l->a], v );
		len = VectorLength( v );
		if ( len < 1e-4f ) {
			continue;
		}
		VectorScale( v, k * ( len - l->min ) / len * 0.5f, v );
		if ( l->a != rd->pinned ) {
			VectorAdd( rd->p[l->a], v, rd->p[l->a] );
		}
		if ( l->b != rd->pinned ) {
			VectorSubtract( rd->p[l->b], v, rd->p[l->b] );
		}
	}
}

static void RD_AddTone( ragdoll_t *rd, int a, int b, float lifeMs ) {
	rdLink_t *l;

	if ( !rd->hasPoint[a] || !rd->hasPoint[b] || rd->numTone >= RD_MAX_TONE ) {
		return;
	}
	l = &rd->tone[rd->numTone++];
	l->a = a;
	l->b = b;
	l->min = Distance( rd->p0[a], rd->p0[b] );     // the posture at death
	l->max = lifeMs;
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

// two points that must stay apart (a limb can't sink into the body)
static void RD_AddSeparation( ragdoll_t *rd, int a, int b, float dist ) {
	float d0;

	if ( !rd->hasPoint[a] || !rd->hasPoint[b] ) {
		return;
	}
	d0 = Distance( rd->p0[a], rd->p0[b] );
	RD_AddLink( rd, a, b, ( dist < d0 * 0.9f ? dist : d0 * 0.9f ), 1e6f, 1.0f );
}

// knees only bend forwards
static void RD_KneeHinge( ragdoll_t *rd, int hip, int knee, int ankle, const vec3_t forward ) {
	vec3_t mid, off;
	float ahead;

	if ( !rd->hasPoint[hip] || !rd->hasPoint[knee] || !rd->hasPoint[ankle] || knee == rd->pinned ) {
		return;
	}
	VectorAdd( rd->p[hip], rd->p[ankle], mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( rd->p[knee], mid, off );
	ahead = DotProduct( off, forward );
	if ( ahead < 0.5f ) {
		VectorMA( rd->p[knee], 0.5f - ahead, forward, rd->p[knee] );
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

	// tag_head (where the head model goes on) sits on, or right next to, the Head bone: the head
	// was a segment of almost no length, its direction (and with it the head's whole turn) set by
	// whichever way the two points were last nudged, and the head spun. The top of the skull
	// instead: along the Head bone's axis that points away from the neck, as it was at death.
	if ( rd->hasPoint[P_HEAD] ) {
		vec3_t up;
		float best = 0;

		VectorSubtract( rd->p0[P_HEAD], rd->p0[P_NECK], up );
		if ( VectorNormalize( up ) < 1e-3f ) {
			VectorCopy( body->axis[2], up );
		}
		for ( i = 0; i < n; i++ ) {
			if ( !Q_stricmp( RD_BoneShortName( names + i * 32 ), "Head" ) ) {
				vec3_t along = { 0, 0, 0 };
				for ( k = 0; k < 3; k++ ) {
					float c = DotProduct( rd->bone0Axis[i][k], up );
					if ( fabs( c ) > fabs( best ) ) {
						best = c;
						VectorScale( rd->bone0Axis[i][k], c < 0 ? -1 : 1, along );
					}
				}
				if ( fabs( best ) > 0.5f ) {
					VectorCopy( along, up );
				}
				break;
			}
		}
		VectorMA( rd->p0[P_HEAD], RD_HEAD_LEN, up, rd->p0[P_HEADEND] );
		rd->hasPoint[P_HEADEND] = qtrue;
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

	// self collision: capsules (RD_SelfCollide); the hands and feet ends are kept off the body here
	{
		const int ends[] = { P_LHANDEND, P_RHANDEND };
		for ( i = 0; i < (int)ARRAY_LEN( ends ); i++ ) {
			RD_AddSeparation( rd, ends[i], P_PELVIS, 8.0f );
			RD_AddSeparation( rd, ends[i], P_CHEST, 8.0f );
		}
	}
	RD_AddSeparation( rd, P_LTOE, P_PELVIS, 10.0f );
	RD_AddSeparation( rd, P_RTOE, P_PELVIS, 10.0f );

	// muscle tone: legs give first, then the body and arms
	RD_AddTone( rd, P_LHIP, P_LANKLE, RD_TONE_LEGS_MS );
	RD_AddTone( rd, P_RHIP, P_RANKLE, RD_TONE_LEGS_MS );
	RD_AddTone( rd, P_CHEST, P_LKNEE, RD_TONE_LEGS_MS * 2 );
	RD_AddTone( rd, P_CHEST, P_RKNEE, RD_TONE_LEGS_MS * 2 );
	RD_AddTone( rd, P_PELVIS, P_HEADEND, RD_TONE_BODY_MS );
	RD_AddTone( rd, P_LSHOULDER, P_LWRIST, RD_TONE_BODY_MS );
	RD_AddTone( rd, P_RSHOULDER, P_RWRIST, RD_TONE_BODY_MS );
	RD_AddTone( rd, P_CHEST, P_LWRIST, RD_TONE_BODY_MS * 0.7f );
	RD_AddTone( rd, P_CHEST, P_RWRIST, RD_TONE_BODY_MS * 0.7f );
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
	rd->startTime = cg.time;
	rd->settleBy = cg.time + RD_SETTLE_MS;
	rd->settled = qfalse;
	rd->inUse = qtrue;
	RD_ApplyPending( rd );
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

	CG_Trace( &tr, from, mins, maxs, rd->p[k], rd->entityNum, RD_MASK );
	if ( tr.startsolid ) {
		return;     // already embedded: leave it to the links to pull it out
	}
	if ( tr.fraction < 1.0f ) {
		VectorMA( tr.endpos, 0.1f, tr.plane.normal, rd->p[k] );
		// friction: drop the motion into the surface, slow the sliding
		VectorSubtract( rd->p[k], rd->old[k], v );
		VectorScale( tr.plane.normal, DotProduct( v, tr.plane.normal ), vn );
		VectorSubtract( v, vn, v );
		VectorMA( rd->p[k], -0.7f, v, rd->old[k] );
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
		VectorScale( v, grabbed ? RD_DAMP_HELD : RD_DAMP, v );
		d = VectorLength( v );
		if ( d > RD_MAX_SPEED * RD_STEP ) {
			VectorScale( v, RD_MAX_SPEED * RD_STEP / d, v );
		}
		VectorCopy( rd->p[k], rd->old[k] );
		VectorAdd( rd->p[k], v, rd->p[k] );
		rd->p[k][2] -= vr_ragdollGravity.value * RD_STEP * RD_STEP;
		RD_Collide( rd, k, start[k] );
	}

	if ( grabbed && rd->pinned >= 0 ) {
		// towards the hand, quickly but not in one jump (a snap stretches the whole body)
		vec3_t to;
		float dist;
		VectorSubtract( grabPoint, rd->p[rd->pinned], to );
		dist = VectorLength( to );
		if ( dist > RD_GRAB_SPEED * RD_STEP ) {
			VectorScale( to, RD_GRAB_SPEED * RD_STEP / dist, to );
		}
		VectorAdd( rd->p[rd->pinned], to, rd->p[rd->pinned] );
		VectorCopy( rd->p[rd->pinned], rd->old[rd->pinned] );
	}

	for ( it = 0; it < ( grabbed ? RD_ITERATIONS * 2 : RD_ITERATIONS ); it++ ) {
		if ( it == 0 ) {
			RD_Tone( rd );
		}
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

		{
			vec3_t f[3], forward;
			RD_TorsoFrame( rd->p, f );
			VectorNegate( f[2], forward );      // up x left points backwards
			RD_KneeHinge( rd, P_LHIP, P_LKNEE, P_LANKLE, forward );
			RD_KneeHinge( rd, P_RHIP, P_RKNEE, P_RANKLE, forward );
		}
		RD_Joints( rd );
		if ( it & 1 ) {
			RD_SelfCollide( rd );
		}
	}

	// the links must not drag points through walls
	for ( k = 0; k < P_COUNT; k++ ) {
		if ( rd->hasPoint[k] && k != rd->pinned ) {
			float r = rdPointRadius[k];
			vec3_t mins = { -r, -r, -r }, maxs = { r, r, r };
			trace_t tr;
			CG_Trace( &tr, start[k], mins, maxs, rd->p[k], rd->entityNum, RD_MASK );
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

/*
The smallest turn from the parent leaves an upper arm or thigh free to roll: swung right round
(an arm from hanging to overhead) it flips, and the forearm and hand spin with it. The bend
at the elbow or knee says which way the limb faces, so the roll is taken from that, as much
as the joint is bent (a straight limb keeps the smallest turn).
*/
static void RD_LimbRoll( ragdoll_t *rd, int s, const vec3_t d0, const vec3_t d, vec3_t r[3] ) {
	vec3_t w0, w, rw0, cr, m[3], out[3];
	float l0, l, n0, n, weight, ang;
	int to, child;

	switch ( s ) {
	case S_LUPPER: child = P_LWRIST; break;
	case S_RUPPER: child = P_RWRIST; break;
	case S_LTHIGH: child = P_LANKLE; break;
	case S_RTHIGH: child = P_RANKLE; break;
	default: return;
	}
	to = rdSegments[s].to;
	if ( !rd->hasPoint[child] ) {
		return;
	}
	VectorSubtract( rd->p0[child], rd->p0[to], w0 );
	VectorSubtract( rd->p[child], rd->p[to], w );
	l0 = VectorLength( w0 );
	l = VectorLength( w );
	if ( l0 < 1e-3f || l < 1e-3f ) {
		return;
	}
	VectorMA( w0, -DotProduct( w0, d0 ), d0, w0 );     // the bend, square to the limb
	VectorMA( w, -DotProduct( w, d ), d, w );
	n0 = VectorNormalize( w0 ) / l0;                    // sine of the bend
	n = VectorNormalize( w ) / l;
	weight = ( ( n0 < n ? n0 : n ) - 0.15f ) / 0.25f;
	if ( weight <= 0 ) {
		return;
	}
	if ( weight > 1 ) {
		weight = 1;
	}
	RD_MatVec( r, w0, rw0 );        // where the bend points now, by the smallest turn
	CrossProduct( rw0, w, cr );
	ang = atan2( DotProduct( cr, d ), DotProduct( rw0, w ) ) * weight;
	RD_AxisAngle( d, ang, m );
	RD_MatMul( m, r, out );
	memcpy( r, out, sizeof( out ) );
}

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
		RD_LimbRoll( rd, s, d0, d, segR[s] );
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
==============================================================================

Hits: bullets push the part of a body they hit, explosions throw bodies. A body killed
by the hit isn't a ragdoll yet when the hit arrives, so recent ones are kept a moment and
given to it when it becomes one.

==============================================================================
*/

#define RD_MAX_PENDING      16
#define RD_PENDING_MS       500
#define RD_HIT_RADIUS       24.0f       // a bullet moves the points this close to where it hit

typedef struct {
	int     time;
	int     entityNum;      // a bullet hit on this body, -1 for an explosion
	vec3_t  point;          // where it hit, or the explosion
	vec3_t  vel;            // the bullet's push
	float   radius, strength;   // the explosion's
} rdPending_t;

static rdPending_t rdPending[RD_MAX_PENDING];

static void RD_Wake( ragdoll_t *rd ) {
	rd->asleep = qfalse;
	rd->stillSteps = 0;
}

// a kick or a blast: moving again, and it settles again a little after
static void RD_Unsettle( ragdoll_t *rd ) {
	rd->settled = qfalse;
	if ( rd->settleBy < cg.time + 2500 ) {
		rd->settleBy = cg.time + 2500;
	}
	RD_Wake( rd );
}

static void RD_PushPoints( ragdoll_t *rd, const vec3_t point, const vec3_t vel ) {
	float w;
	int k;

	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hasPoint[k] ) {
			continue;
		}
		w = 1.0f - Distance( rd->p[k], point ) / RD_HIT_RADIUS;
		if ( w > 0 ) {
			VectorMA( rd->old[k], -RD_STEP * w, vel, rd->old[k] );     // verlet: velocity is p - old
		}
	}
	RD_Wake( rd );
}

static void RD_Blast( ragdoll_t *rd, const vec3_t origin, float radius, float strength ) {
	vec3_t dir;
	float dist, w;
	int k;

	if ( Distance( rd->p[P_PELVIS], origin ) > radius ) {
		return;
	}
	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hasPoint[k] ) {
			continue;
		}
		VectorSubtract( rd->p[k], origin, dir );
		dist = VectorNormalize( dir );
		w = 1.0f - dist / radius;
		if ( w <= 0 ) {
			continue;
		}
		dir[2] += 0.6f;         // thrown up and away
		VectorNormalize( dir );
		VectorMA( rd->old[k], -RD_STEP * w * strength, dir, rd->old[k] );
	}
	RD_Unsettle( rd );
}

static rdPending_t *RD_PendingSlot( void ) {
	rdPending_t *oldest = &rdPending[0];
	int i;

	for ( i = 0; i < RD_MAX_PENDING; i++ ) {
		if ( rdPending[i].time < oldest->time ) {
			oldest = &rdPending[i];
		}
	}
	return oldest;
}

// a new ragdoll gets whatever hit it just before it became one
static void RD_ApplyPending( ragdoll_t *rd ) {
	int i;

	for ( i = 0; i < RD_MAX_PENDING; i++ ) {
		rdPending_t *pd = &rdPending[i];
		if ( !pd->time || cg.time - pd->time > RD_PENDING_MS || cg.time < pd->time ) {
			continue;
		}
		if ( pd->entityNum == rd->entityNum ) {
			// the shot that killed: thrown by it (shot reactions)
			vec3_t v;
			VectorScale( pd->vel, CG_VRX_Knock( qtrue ), v );
			if ( CG_VRX_Knock( qtrue ) > 1.0f ) {
				v[2] += 30.0f * CG_VRX_Knock( qtrue );
			}
			RD_PushPoints( rd, pd->point, v );
			pd->time = 0;
		} else if ( pd->entityNum < 0 ) {
			RD_Blast( rd, pd->point, pd->radius, pd->strength );
		}
	}
}

/*
=================
CG_Ragdoll_Hit

A bullet hit this body at point, travelling along dir.
=================
*/
void CG_Ragdoll_Hit( int entityNum, const vec3_t point, const vec3_t dir, float speed ) {
	ragdoll_t *rd;
	rdPending_t *pd;
	vec3_t vel;

	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return;
	}
	VectorNormalize2( dir, vel );
	VectorScale( vel, speed, vel );

	rd = RD_Find( entityNum );
	if ( rd ) {
		if ( rd->settled && speed < 400.0f ) {
			return;     // stopped for good: a bullet doesn't move it, a kick does
		}
		VectorScale( vel, CG_VRX_Knock( qfalse ), vel );    // shot reactions: bodies knocked harder
		RD_PushPoints( rd, point, vel );
		if ( speed >= 400.0f ) {
			RD_Unsettle( rd );
		}
		return;
	}
	CG_VRX_Flinch( entityNum, point, dir );     // someone alive flinches
	pd = RD_PendingSlot();
	pd->time = cg.time;
	pd->entityNum = entityNum;
	VectorCopy( point, pd->point );
	VectorCopy( vel, pd->vel );
}

/*
=================
CG_Ragdoll_Explosion
=================
*/
void CG_Ragdoll_Explosion( const vec3_t origin, float radius, float strength ) {
	rdPending_t *pd;
	int i;

	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		if ( rdPool[i].inUse ) {
			RD_Blast( &rdPool[i], origin, radius, strength );
		}
	}
	pd = RD_PendingSlot();
	pd->time = cg.time;
	pd->entityNum = -1;
	VectorCopy( origin, pd->point );
	pd->radius = radius;
	pd->strength = strength;
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
	trap_Cvar_Update( &vr_ragdollGravity );
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
		// picked up: a VR hand holds it by the point nearest the hand. Without a headset the
		// "hand" is just in front of the view and swings round with every turn of the mouse:
		// held by a foot or a hand the body whirls round it, so it is taken by the collar.
		// Co-op: any player without a headset, us or someone else (their VRE_ACTIVE).
		float best = 1e9f, dist;
		int k, holder = cent->currentState.dl_intensity - 1;
		qboolean flatHolder = holder == cg.snap->ps.clientNum ? !cgvr.active :
							  !( cg_entities[holder].currentState.vrFlags & VRE_ACTIVE );
		rd->pinned = -1;
		if ( flatHolder && rd->hasPoint[P_NECK] ) {
			rd->pinned = P_NECK;
		} else {
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hasPoint[k] && ( dist = Distance( rd->p[k], grabPoint ) ) < best ) {
					best = dist;
					rd->pinned = k;
				}
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
		rd->settled = qfalse;
		rd->settleBy = cg.time + 2500;
	} else if ( !rd->settled && cg.time >= rd->settleBy ) {
		// long enough: if it's lying on something, it stops for good (no helicopters, no
		// creeping about; its blood pool stays put under it)
		trace_t tr;
		vec3_t down;
		VectorCopy( rd->p[P_PELVIS], down );
		down[2] -= 16;
		CG_Trace( &tr, rd->p[P_PELVIS], NULL, NULL, down, rd->entityNum, MASK_SOLID );
		if ( tr.fraction < 1.0f ) {
			rd->settled = qtrue;
			rd->asleep = qtrue;
		} else {
			rd->settleBy = cg.time + 500;
		}
	} else if ( rd->settled ) {
		rd->asleep = qtrue;
		VectorCopy( cent->lerpOrigin, rd->lastOrigin );
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
