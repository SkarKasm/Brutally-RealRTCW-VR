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
//
// vr_physics 1: the same bodies as Jolt Physics rigid bodies instead (cg_physics.cpp): a
// box for the torso and capsules for the head and limbs, joined by swing-twist joints
// (shoulders, hips, neck, wrists, ankles) and hinges (elbows, knees), colliding with the
// level, doors and lifts and with each other. The death pose, the hits, the grab and the
// tether to the server's body are the same; the bones follow the bodies (RD_PhysPose).
//
// vr_activeRagdolls 1 (with vr_physics 1): active ragdolls. The joints' motors chase a pose
// instead of hanging limp (RD_Active): the dying chase their death animation, weakening over a
// time set by where the killing shot went in (a headshot drops them at once, a gut shot keeps
// them folding over it for seconds), and someone shot down (VRX_GB_DOWNED) is a ragdoll while
// still alive, chasing CG_VRX_Downed's SoF2 writhe, so he writhes against the floor and walls
// and is knocked about by shots. A limb that's hit flops a moment before it pulls again.

// The Jolt bodies and their reactions (everything vr_physics 1 does past the bones) are the
// HitReact library's (code/hitreact), so other games can use them too: this file gives it the
// skeleton, the hits and what the server says, and draws the bones from what it gives back.

#include "cg_local.h"
#include "cg_physics.h"
#include "../game/bg_vrextras.h"

#ifndef BONEFLAG_TAG
#define BONEFLAG_TAG        1       // qfiles.h: this bone is actually a tag
#endif

#define RD_MAX_RAGDOLLS     96
#define RD_MAX_ACTIVE       12          // at most this many chase a pose at once (a driven body never sleeps)
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
#define RD_MAX_SPEED        650.0f
#define RD_CAPSULES         10          // rdCapsules
#define RD_PEN_EASE         0.02f       // a part that died inside another is eased out by this much per pass
#define RD_PRESOLVE         24          // passes over the joints before the first step      // no point moves faster (shots, blasts and links can't fling it away)

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
	S_TORSO, S_CHEST, S_HEAD,
	S_LUPPER, S_LFORE, S_LHAND, S_RUPPER, S_RFORE, S_RHAND,
	S_LTHIGH, S_LCALF, S_LFOOT, S_RTHIGH, S_RCALF, S_RFOOT,
	S_COUNT
} rdSegment_t;

static const struct {
	const char *keyBone;    // first bone of the segment
	int parent;             // segment it hangs from
	int from, to;           // points along it
} rdSegments[S_COUNT] = {
	{ "Pelvis", -1, P_PELVIS, P_NECK },         // (vr_physics 1: the belly and hips, up to the chest)
	{ "Spine2", S_TORSO, P_CHEST, P_NECK },      // the chest on a spine joint (the Jolt bodies; the points keep one rigid trunk)
	{ "Head", S_CHEST, P_HEAD, P_HEADEND },
	{ "L UpperArm", S_CHEST, P_LSHOULDER, P_LELBOW },
	{ "L Forearm", S_LUPPER, P_LELBOW, P_LWRIST },
	{ "L Hand", S_LFORE, P_LWRIST, P_LHANDEND },
	{ "R UpperArm", S_CHEST, P_RSHOULDER, P_RELBOW },
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
	hrBody_t    hb;                 // the Jolt body and its reactions (HitReact), and the points the old ragdolls use too
	int         entityNum;
	qhandle_t   model;
	int         lastSeen;           // cg.time this body was last drawn

	int         numBones;
	int         boneSeg[RD_MAX_BONES];
	int         neckBone;                   // the Neck bone (-1: none), and what hangs on it short of the head
	unsigned char boneOnNeck[RD_MAX_BONES];
	qboolean    boneIsTag[RD_MAX_BONES];
	vec3_t      bone0Axis[RD_MAX_BONES][3];     // world rotation at death (columns)
	vec3_t      bone0Pos[RD_MAX_BONES];

	vec3_t      old[P_COUNT];               // the points a step ago (the old ragdolls; hb.p is where they are)
	int         numLinks;
	rdLink_t    links[RD_MAX_LINKS];
	vec3_t      torso0[3];          // torso frame at death
	float       selfPen[RD_CAPSULES][RD_CAPSULES];  // how far parts overlapped at death (RD_SelfCollide)

	int         simTime;            // time simulated up to

	int         numTone;
	rdLink_t    tone[RD_MAX_TONE];  // posture links, fading (RD_Tone)

	int         pinned;             // point held by a hand, -1 none
	int         grabber;            // client number + 1 holding it, 0 none

	float       out[RD_MAX_BONES * 12];
	vec3_t      outOrigin;

	int         segKeyBone[S_COUNT];        // the bone each segment turns like

	// drawn part physics, part animation: coming to on his feet, the end of getting up
	qboolean    blended;                    // drawn from blend: part physics, part animation (RD_Blend)
	float       blend[RD_MAX_BONES * 12 + 12];
	// getting up as the game's own get-up animation (RD_GuAnim*)
	qboolean    guAnim;
	vec3_t      guOrigin, guAxis[3];        // where the animation is laid
	float       guLegK;                     // his legs' length against the zombie's
	int         guMap[RD_MAX_BONES];        // the zombie's bone each of his follows (-1: rides on its parent)
	int         guParent[RD_MAX_BONES];
	float       guLoc[RD_MAX_BONES * 12];   // each bone's turn and offset from its parent, his own
} ragdoll_t;

static ragdoll_t rdPool[RD_MAX_RAGDOLLS];
static ragdoll_t *RD_Find( int entityNum );
static void RD_ApplyPending( ragdoll_t *rd );
static void RD_SolveLinks( ragdoll_t *rd );
static vmCvar_t vr_ragdolls;
static vmCvar_t vr_ragdollGravity;
static vmCvar_t vr_activeRagdolls;
static vmCvar_t vr_activeStrength;
static vmCvar_t vr_realWeights;
static vmCvar_t vr_activeEuphoria;
static vmCvar_t vr_activeHits;

static void RD_GuAnimInit( void );

static void RD_Free( ragdoll_t *rd ) {
	HR_BodyFree( &rd->hb );
	rd->inUse = qfalse;
}

/*
==============================================================================

HitReact's side of the game: the cvars, the clock, traces, the server's commands

==============================================================================
*/

static centity_t *rdCent;      // the body CG_Ragdoll_Apply is on (for the get-up callbacks)
static refEntity_t *rdBody;

// the cvars and the clock, before anything is asked of HitReact
static void RD_Sync( void ) {
	hrOptions_t o;

	trap_Cvar_Update( &vr_ragdolls );
	trap_Cvar_Update( &vr_ragdollGravity );
	trap_Cvar_Update( &vr_activeRagdolls );
	trap_Cvar_Update( &vr_activeEuphoria );
	trap_Cvar_Update( &vr_activeStrength );
	o.gravity = vr_ragdollGravity.value;
	o.active = vr_activeRagdolls.integer != 0;
	o.euphoria = vr_activeEuphoria.integer != 0;
	o.strength = vr_activeStrength.value;
	o.maxActive = RD_MAX_ACTIVE;
	o.debug = ( vr_ragdolls.integer > 1 ? 1 : 0 ) | ( CG_VRLogOn() ? 2 : 0 );
	HR_SetOptions( &o );
	HR_SetTime( cg.time );
}

static void RD_HostPrint( const char *msg ) {
	CG_Printf( "%s", msg );
}

static int RD_HostTrace( void *user, const float *start, const float *end, float *fraction, float *endpos ) {
	ragdoll_t *rd = user;
	trace_t tr;

	CG_Trace( &tr, start, NULL, NULL, end, rd->entityNum, MASK_SOLID );
	*fraction = tr.fraction;
	VectorCopy( tr.endpos, endpos );
	return tr.startsolid;
}

// what HitReact tells the game, as the server's commands
static void RD_HostEvent( void *user, int event, const float *args ) {
	ragdoll_t *rd = user;

	switch ( event ) {
	case HR_EV_FELL:
		trap_SendClientCommand( va( "vrfell %d", rd->entityNum ) );
		break;
	case HR_EV_POSITION:
		trap_SendClientCommand( va( "vrdownpos %d %d %d", rd->entityNum, (int)args[0], (int)args[1] ) );
		break;
	case HR_EV_HOLD:
		trap_SendClientCommand( va( "vrstagger %d", rd->entityNum ) );
		break;
	case HR_EV_GETUP_YAW:
		trap_SendClientCommand( va( "vrgetupyaw %d %.0f", rd->entityNum, args[0] ) );
		break;
	}
}

static int RD_HostGetUpPose( void *user, int stage, float *partTargets, float *torsoPos, float *torsoAxis );
static int RD_HostGetUpAnimBegin( void *user );
static void RD_HostGetUpAnimAbort( void *user );

// once in a while: a ragdoll whose body hasn't been in view for 20 s gives its physics back (a
// Survival body that went, G_VRX_BodyGone: nothing would ever draw it again)
void CG_Ragdoll_Purge( void ) {
	static int next;
	int i;

	if ( cgs.gametype != GT_SURVIVAL || ( cg.time < next && cg.time > next - 2000 ) ) {
		return;     // (elsewhere a body left behind is still there to come back to)
	}
	next = cg.time + 1000;
	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		if ( rdPool[i].inUse && cg.time - rdPool[i].lastSeen > 20000 ) {
			RD_Free( &rdPool[i] );
		}
	}
}

void CG_Ragdoll_Init( void ) {
	memset( rdPool, 0, sizeof( rdPool ) );
	trap_Cvar_Register( &vr_ragdolls, "vr_ragdolls", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_ragdollGravity, "vr_ragdollGravity", RD_GRAVITY, CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_activeRagdolls, "vr_activeRagdolls", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_activeStrength, "vr_activeStrength", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_realWeights, "vr_realWeights", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_activeEuphoria, "vr_activeEuphoria", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_activeHits, "vr_activeHits", "1", CVAR_ARCHIVE );
	RD_GuAnimInit();
	{
		hrHost_t host;
		memset( &host, 0, sizeof( host ) );
		host.print = RD_HostPrint;
		host.trace = RD_HostTrace;
		host.event = RD_HostEvent;
		host.getUpPose = RD_HostGetUpPose;
		host.getUpAnimBegin = RD_HostGetUpAnimBegin;
		host.getUpAnimAbort = RD_HostGetUpAnimAbort;
		HR_SetHost( &host );
		HR_SetHostSpace( 1, 0 );       // game units are HitReact's own (inches, z up)
	}
}

/*
==============================================================================

Small 3x3 helpers, matrices are column bases: m[k] is the k-th column

==============================================================================
*/

// (HitReact's: the same maths the library's reactions use)
#define RD_MatMul           HR_MatMul
#define RD_MatVec           HR_MatVec
#define RD_FrameRotation    HR_FrameRotation
#define RD_Align            HR_Align
#define RD_AxisAngle        HR_AxisAngle
#define RD_TorsoFrame       HR_TorsoFrame
#define RD_RotLerp          HR_RotLerp
#define RD_SegmentDist      HR_SegmentDist

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

	if ( !rd->hb.hasPoint[root] || !rd->hb.hasPoint[tip] || tip == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->hb.p[tip], rd->hb.p[root], d );
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
	VectorMA( rd->hb.p[root], len, nd, nd );
	VectorSubtract( nd, rd->hb.p[tip], d );
	VectorMA( rd->hb.p[tip], 0.5f, d, rd->hb.p[tip] );
	if ( root != rd->pinned ) {
		VectorMA( rd->hb.p[root], -0.5f, d, rd->hb.p[root] );
	}
}

// the bend at b between a-b and b-c stays within maxDev degrees of how it was at death
static void RD_LimitBend( ragdoll_t *rd, int a, int b, int c, float maxDev ) {
	vec3_t u, v, u0, v0, axis, nd;
	float ang, ang0, want, len, s, co;

	if ( !rd->hb.hasPoint[a] || !rd->hb.hasPoint[b] || !rd->hb.hasPoint[c] || c == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->hb.p[b], rd->hb.p[a], u );
	VectorSubtract( rd->hb.p[c], rd->hb.p[b], v );
	VectorSubtract( rd->hb.p0[b], rd->hb.p0[a], u0 );
	VectorSubtract( rd->hb.p0[c], rd->hb.p0[b], v0 );
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
	VectorMA( rd->hb.p[b], len, v, rd->hb.p[c] );
}

// knees only bend backwards: the calf stays off the thigh's front and mostly out of its sides
static void RD_LimitKnee( ragdoll_t *rd, int hip, int knee, int ankle, vec3_t f[3] ) {
	vec3_t t, h, fwd, c, nd;
	float len, cf, ch;

	if ( !rd->hb.hasPoint[hip] || !rd->hb.hasPoint[knee] || !rd->hb.hasPoint[ankle] || ankle == rd->pinned ) {
		return;
	}
	VectorSubtract( rd->hb.p[knee], rd->hb.p[hip], t );
	if ( VectorNormalize( t ) < 1e-3f ) {
		return;
	}
	VectorMA( f[1], -DotProduct( f[1], t ), t, h );     // the hinge axis: the body's left, square to the thigh
	if ( VectorNormalize( h ) < 0.3f ) {
		return;     // leg flung out sideways: no clear hinge
	}
	CrossProduct( t, h, fwd );      // the way a knee can't go
	VectorSubtract( rd->hb.p[ankle], rd->hb.p[knee], c );
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
	VectorMA( rd->hb.p[knee], len, nd, rd->hb.p[ankle] );
}

static void RD_Joints( ragdoll_t *rd ) {
	vec3_t f[3];

	RD_TorsoFrame( rd->hb.p, f );
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
typedef char rdCapsulesFit[RD_NUM_CAPSULES == RD_CAPSULES ? 1 : -1];

static void RD_Lerp( const vec3_t a, const vec3_t b, float t, vec3_t out ) {
	out[0] = a[0] + t * ( b[0] - a[0] );
	out[1] = a[1] + t * ( b[1] - a[1] );
	out[2] = a[2] + t * ( b[2] - a[2] );
}



// push a capsule's two points along n, shared by where the contact is (pinned points don't move)
static void RD_MoveCapsule( ragdoll_t *rd, const rdCapsule_t *c, float s, const vec3_t n, float amount ) {
	float wa = c->a == rd->pinned ? 0 : 1 - s, wb = c->b == rd->pinned ? 0 : s, sum = wa * wa + wb * wb;

	if ( sum < 1e-4f ) {
		return;
	}
	VectorMA( rd->hb.p[c->a], amount * wa / sum, n, rd->hb.p[c->a] );
	VectorMA( rd->hb.p[c->b], amount * wb / sum, n, rd->hb.p[c->b] );
}

static void RD_SelfCollide( ragdoll_t *rd ) {
	int i, j;

	for ( i = 0; i < RD_NUM_CAPSULES; i++ ) {
		const rdCapsule_t *ci = &rdCapsules[i];
		if ( !rd->hb.hasPoint[ci->a] || !rd->hb.hasPoint[ci->b] ) {
			continue;
		}
		for ( j = i + 1; j < RD_NUM_CAPSULES; j++ ) {
			const rdCapsule_t *cj = &rdCapsules[j];
			vec3_t pi, pj, n;
			float s, t, d, pen;

			if ( !rd->hb.hasPoint[cj->a] || !rd->hb.hasPoint[cj->b] ) {
				continue;
			}
			// joined parts (sharing a point, or a limb's root on the torso) are the joints' business
			if ( ci->a == cj->a || ci->a == cj->b || ci->b == cj->a || ci->b == cj->b ) {
				continue;
			}
			if ( i == 0 && ( j == 1 || j == 2 || j == 4 || j == 6 || j == 8 ) ) {
				continue;
			}
			d = RD_SegmentDist( rd->hb.p[ci->a], rd->hb.p[ci->b], rd->hb.p[cj->a], rd->hb.p[cj->b], &s, &t );
			pen = ci->r + cj->r - d;
			// parts that were already inside each other at death (an arm through the chest in
			// the death animation) are not shot apart, which sent bodies spinning off like
			// helicopters: only overlap beyond that is pushed out, and that allowance shrinks
			if ( rd->selfPen[i][j] > 0 ) {
				if ( pen < rd->selfPen[i][j] ) {
					rd->selfPen[i][j] = pen > 0 ? pen : 0;
				}
				pen -= rd->selfPen[i][j];
				rd->selfPen[i][j] -= RD_PEN_EASE;
				if ( rd->selfPen[i][j] < 0 ) {
					rd->selfPen[i][j] = 0;
				}
			}
			if ( pen <= 0 ) {
				continue;
			}
			RD_Lerp( rd->hb.p[ci->a], rd->hb.p[ci->b], s, pi );
			RD_Lerp( rd->hb.p[cj->a], rd->hb.p[cj->b], t, pj );
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
	float age = (float)( rd->simTime - rd->hb.startTime ), k;
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
		VectorSubtract( rd->hb.p[l->b], rd->hb.p[l->a], v );
		len = VectorLength( v );
		if ( len < 1e-4f ) {
			continue;
		}
		VectorScale( v, k * ( len - l->min ) / len * 0.5f, v );
		if ( l->a != rd->pinned ) {
			VectorAdd( rd->hb.p[l->a], v, rd->hb.p[l->a] );
		}
		if ( l->b != rd->pinned ) {
			VectorSubtract( rd->hb.p[l->b], v, rd->hb.p[l->b] );
		}
	}
}

static void RD_AddTone( ragdoll_t *rd, int a, int b, float lifeMs ) {
	rdLink_t *l;

	if ( !rd->hb.hasPoint[a] || !rd->hb.hasPoint[b] || rd->numTone >= RD_MAX_TONE ) {
		return;
	}
	l = &rd->tone[rd->numTone++];
	l->a = a;
	l->b = b;
	l->min = Distance( rd->hb.p0[a], rd->hb.p0[b] );     // the posture at death
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

	if ( !rd->hb.hasPoint[a] || !rd->hb.hasPoint[b] || rd->numLinks >= RD_MAX_LINKS ) {
		return;
	}
	rest = restOverride > 0 ? restOverride : Distance( rd->hb.p0[a], rd->hb.p0[b] );
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
	if ( rd->hb.hasPoint[a] && rd->hb.hasPoint[b] && rd->hb.hasPoint[c] ) {
		len = Distance( rd->hb.p0[a], rd->hb.p0[b] ) + Distance( rd->hb.p0[b], rd->hb.p0[c] );
		RD_AddLink( rd, a, c, minBend, 1.0f, len );
	}
}

// two points that must stay apart (a limb can't sink into the body)
static void RD_AddSeparation( ragdoll_t *rd, int a, int b, float dist ) {
	float d0;

	if ( !rd->hb.hasPoint[a] || !rd->hb.hasPoint[b] ) {
		return;
	}
	d0 = Distance( rd->hb.p0[a], rd->hb.p0[b] );
	RD_AddLink( rd, a, b, ( dist < d0 * 0.9f ? dist : d0 * 0.9f ), 1e6f, 1.0f );
}

// knees only bend forwards
static void RD_KneeHinge( ragdoll_t *rd, int hip, int knee, int ankle, const vec3_t forward ) {
	vec3_t mid, off;
	float ahead;

	if ( !rd->hb.hasPoint[hip] || !rd->hb.hasPoint[knee] || !rd->hb.hasPoint[ankle] || knee == rd->pinned ) {
		return;
	}
	VectorAdd( rd->hb.p[hip], rd->hb.p[ankle], mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( rd->hb.p[knee], mid, off );
	ahead = DotProduct( off, forward );
	if ( ahead < 0.5f ) {
		VectorMA( rd->hb.p[knee], 0.5f - ahead, forward, rd->hb.p[knee] );
	}
}

/*
=================
CG_Ragdoll_Remember

Called with each living skeletal character's body as it is drawn: the ragdoll starts from
exactly the pose he was last drawn in (an agony writhe, hands up, a hit reaction, or just his
animation), not from the first frame of his death animation, which stands him back up.
=================
*/
typedef struct {
	int         time;
	qhandle_t   model;
	int         numBones;       // > 0: a pose drawn over the animation (ragdollBones), kept here
	int         frame, oldframe, torsoFrame, oldTorsoFrame;
	float       backlerp, torsoBacklerp;
	vec3_t      origin, axis[3], torsoAxis[3];
} rdLast_t;

static rdLast_t rdLast[MAX_CLIENTS];
static float rdLastPose[MAX_CLIENTS][RD_MAX_BONES * 12];
static const float *rdStartPose;

void CG_Ragdoll_Remember( centity_t *cent, refEntity_t *body ) {
	int n = cent->currentState.number;
	rdLast_t *l;

	if ( n < 0 || n >= MAX_CLIENTS || ( cent->currentState.eFlags & EF_DEAD ) || !body->hModel ) {
		return;
	}
	l = &rdLast[n];
	l->time = cg.time;
	l->model = body->hModel;
	l->frame = body->frame;
	l->oldframe = body->oldframe;
	l->backlerp = body->backlerp;
	l->torsoFrame = body->torsoFrame;
	l->oldTorsoFrame = body->oldTorsoFrame;
	l->torsoBacklerp = body->torsoBacklerp;
	VectorCopy( body->origin, l->origin );
	memcpy( l->axis, body->axis, sizeof( l->axis ) );
	memcpy( l->torsoAxis, body->torsoAxis, sizeof( l->torsoAxis ) );
	l->numBones = 0;
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		static int parents[RD_MAX_BONES], flags[RD_MAX_BONES];
		static char names[RD_MAX_BONES * 32];
		static float scratch[RD_MAX_BONES * 12];
		static qhandle_t countedModel[8];
		static int counted[8], next;
		int k, c;

		for ( c = 0; c < 8 && countedModel[c] != body->hModel; c++ ) {
		}
		if ( c < 8 ) {
			k = counted[c];
		} else {
			k = trap_R_GetSkeleton( body, RD_MAX_BONES, parents, flags, names, scratch );     // (just the count, once a model)
			countedModel[next] = body->hModel;
			counted[next] = k;
			next = ( next + 1 ) % 8;
		}
		if ( k > 0 ) {
			memcpy( rdLastPose[n], body->ragdollBones, sizeof( float ) * 12 * k );
			l->numBones = k;
		}
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
	if ( rdStartPose ) {
		memcpy( bones, rdStartPose, sizeof( float ) * 12 * n );     // the pose he was last drawn in
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
		for ( s = 0; s < 2 && !rd->hb.hasPoint[k]; s++ ) {
			const char *want = rdPointBones[k][s];
			if ( !want ) {
				continue;
			}
			for ( i = 0; i < n; i++ ) {
				if ( !Q_stricmp( RD_BoneShortName( names + i * 32 ), want ) ) {
					VectorCopy( rd->bone0Pos[i], rd->hb.p0[k] );
					rd->hb.hasPoint[k] = qtrue;
					break;
				}
			}
		}
	}
	if ( !rd->hb.hasPoint[P_PELVIS] || !rd->hb.hasPoint[P_NECK] || !rd->hb.hasPoint[P_LSHOULDER] || !rd->hb.hasPoint[P_RSHOULDER] ) {
		return qfalse;      // not a human skeleton (dogs, bosses...)
	}

	// tag_head (where the head model goes on) sits on, or right next to, the Head bone: the head
	// was a segment of almost no length, its direction (and with it the head's whole turn) set by
	// whichever way the two points were last nudged, and the head spun. The top of the skull
	// instead: along the Head bone's axis that points away from the neck, as it was at death.
	if ( rd->hb.hasPoint[P_HEAD] ) {
		vec3_t up;
		float best = 0;

		VectorSubtract( rd->hb.p0[P_HEAD], rd->hb.p0[P_NECK], up );
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
		VectorMA( rd->hb.p0[P_HEAD], RD_HEAD_LEN, up, rd->hb.p0[P_HEADEND] );
		rd->hb.hasPoint[P_HEADEND] = qtrue;
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
		rd->hb.segValid[s] = s == S_TORSO ||
						  ( keyBone[s] >= 0 && rd->hb.hasPoint[rdSegments[s].from] && rd->hb.hasPoint[rdSegments[s].to] &&
							rd->hb.segValid[rdSegments[s].parent] );
		rd->segKeyBone[s] = keyBone[s];
	}
	for ( i = 0; i < n; i++ ) {
		rd->boneSeg[i] = S_TORSO;
		for ( b = i; b >= 0; b = parents[b] ) {
			for ( s = 1; s < S_COUNT; s++ ) {
				if ( rd->hb.segValid[s] && keyBone[s] == b ) {
					break;
				}
			}
			if ( s < S_COUNT ) {
				rd->boneSeg[i] = s;
				break;
			}
		}
	}

	// the neck: its bone, and the bones that turn with it (RD_NeckBones)
	rd->neckBone = -1;
	memset( rd->boneOnNeck, 0, sizeof( rd->boneOnNeck ) );
	for ( i = 0; i < n; i++ ) {
		if ( !Q_stricmp( RD_BoneShortName( names + i * 32 ), "Neck" ) ) {
			rd->neckBone = i;
			break;
		}
	}
	// (the collar bones, and the arms on them, hang off the Neck bone in this skeleton: they stay
	// the chest's and the arms' own)
	for ( i = 0; rd->neckBone >= 0 && i < n; i++ ) {
		if ( rd->boneSeg[i] != rd->boneSeg[rd->neckBone] ) {
			continue;
		}
		for ( b = i; b >= 0; b = parents[b] ) {
			if ( strstr( names + b * 32, "Clavicle" ) ) {
				break;
			}
			if ( b == rd->neckBone ) {
				rd->boneOnNeck[i] = 1;
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

	RD_TorsoFrame( rd->hb.p0, f0 );
	memcpy( rd->torso0, f0, sizeof( f0 ) );

	// the death pose often breaks the joint limits a little (a leg kicked back, the head
	// turned far round): settle that before the first step, so the correction isn't turned
	// into speed. And what overlaps now may stay overlapping (RD_SelfCollide).
	for ( k = 0; k < P_COUNT; k++ ) {
		VectorCopy( rd->hb.p0[k], rd->hb.p[k] );
	}
	for ( i = 0; i < RD_PRESOLVE; i++ ) {
		RD_SolveLinks( rd );
		RD_Joints( rd );
	}
	memset( rd->selfPen, 0, sizeof( rd->selfPen ) );
	for ( i = 0; i < RD_NUM_CAPSULES; i++ ) {
		int j;
		for ( j = i + 1; j < RD_NUM_CAPSULES; j++ ) {
			const rdCapsule_t *ci = &rdCapsules[i], *cj = &rdCapsules[j];
			float st, tt;
			if ( rd->hb.hasPoint[ci->a] && rd->hb.hasPoint[ci->b] && rd->hb.hasPoint[cj->a] && rd->hb.hasPoint[cj->b] ) {
				rd->selfPen[i][j] = ci->r + cj->r -
									RD_SegmentDist( rd->hb.p[ci->a], rd->hb.p[ci->b], rd->hb.p[cj->a], rd->hb.p[cj->b], &st, &tt );
			}
		}
	}

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
	VectorCopy( vel, rd->hb.startVel );
	VectorCopy( push, rd->hb.killPush );
	for ( k = 0; k < P_COUNT; k++ ) {
		vec3_t v;
		float upper;

		if ( !rd->hb.hasPoint[k] ) {
			continue;
		}
		// the upper body is pushed harder, so the body tips over
		upper = ( rd->hb.p0[k][2] - rd->hb.p0[P_PELVIS][2] ) / 40.0f;
		upper = upper < 0 ? 0.3f : ( upper > 1 ? 1.3f : 0.3f + upper );
		VectorMA( vel, upper, push, v );
		VectorMA( rd->hb.p[k], -RD_STEP, v, rd->old[k] );
	}

	VectorSubtract( rd->hb.p0[P_PELVIS], cent->lerpOrigin, rd->hb.anchorOffset );
	VectorCopy( cent->lerpOrigin, rd->hb.lastOrigin );
	rd->entityNum = cent->currentState.number;
	rd->hb.user = rd;
	rd->hb.id = rd->entityNum;
	rd->model = body->hModel;
	rd->simTime = cg.time;
	rd->hb.startTime = cg.time;
	rd->hb.settleBy = cg.time + RD_SETTLE_MS;
	rd->hb.settled = qfalse;
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

	CG_Trace( &tr, from, mins, maxs, rd->hb.p[k], rd->entityNum, RD_MASK );
	if ( tr.startsolid ) {
		return;     // already embedded: leave it to the links to pull it out
	}
	if ( tr.fraction < 1.0f ) {
		VectorMA( tr.endpos, 0.1f, tr.plane.normal, rd->hb.p[k] );
		// friction: drop the motion into the surface, slow the sliding
		VectorSubtract( rd->hb.p[k], rd->old[k], v );
		VectorScale( tr.plane.normal, DotProduct( v, tr.plane.normal ), vn );
		VectorSubtract( v, vn, v );
		VectorMA( rd->hb.p[k], -0.7f, v, rd->old[k] );
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

static void RD_SolveLinks( ragdoll_t *rd ) {
	vec3_t v;
	int i;

	for ( i = 0; i < rd->numLinks; i++ ) {
		rdLink_t *l = &rd->links[i];
		float len, want, wa, wb;

		VectorSubtract( rd->hb.p[l->b], rd->hb.p[l->a], v );
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
		VectorMA( rd->hb.p[l->a], wa, v, rd->hb.p[l->a] );
		VectorMA( rd->hb.p[l->b], -wb, v, rd->hb.p[l->b] );
	}
}

static void RD_Step( ragdoll_t *rd, centity_t *cent, const vec3_t grabPoint, qboolean grabbed ) {
	vec3_t start[P_COUNT], v, target, err;
	float moved = 0, d;
	int k, it;
	qboolean originMoved;

	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hb.hasPoint[k] ) {
			continue;
		}
		VectorCopy( rd->hb.p[k], start[k] );
		VectorSubtract( rd->hb.p[k], rd->old[k], v );
		VectorScale( v, grabbed ? RD_DAMP_HELD : RD_DAMP, v );
		d = VectorLength( v );
		if ( d > RD_MAX_SPEED * RD_STEP ) {
			VectorScale( v, RD_MAX_SPEED * RD_STEP / d, v );
		}
		VectorCopy( rd->hb.p[k], rd->old[k] );
		VectorAdd( rd->hb.p[k], v, rd->hb.p[k] );
		rd->hb.p[k][2] -= vr_ragdollGravity.value * RD_STEP * RD_STEP;
		RD_Collide( rd, k, start[k] );
	}

	if ( grabbed && rd->pinned >= 0 ) {
		// towards the hand, quickly but not in one jump (a snap stretches the whole body)
		vec3_t to;
		float dist;
		VectorSubtract( grabPoint, rd->hb.p[rd->pinned], to );
		dist = VectorLength( to );
		if ( dist > RD_GRAB_SPEED * RD_STEP ) {
			VectorScale( to, RD_GRAB_SPEED * RD_STEP / dist, to );
		}
		VectorAdd( rd->hb.p[rd->pinned], to, rd->hb.p[rd->pinned] );
		VectorCopy( rd->hb.p[rd->pinned], rd->old[rd->pinned] );
	}

	for ( it = 0; it < ( grabbed ? RD_ITERATIONS * 2 : RD_ITERATIONS ); it++ ) {
		if ( it == 0 ) {
			RD_Tone( rd );
		}
		RD_SolveLinks( rd );

		{
			vec3_t f[3], forward;
			RD_TorsoFrame( rd->hb.p, f );
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
		if ( rd->hb.hasPoint[k] && k != rd->pinned ) {
			float r = rdPointRadius[k];
			vec3_t mins = { -r, -r, -r }, maxs = { r, r, r };
			trace_t tr;
			CG_Trace( &tr, start[k], mins, maxs, rd->hb.p[k], rd->entityNum, RD_MASK );
			if ( tr.fraction < 1.0f && !tr.startsolid ) {
				VectorMA( tr.endpos, 0.1f, tr.plane.normal, rd->hb.p[k] );
			}
			d = Distance( rd->hb.p[k], start[k] );
			if ( d > moved ) {
				moved = d;
			}
		}
	}

	// stay with the server's body: the same place for every player. Only while the
	// server moves the body (thrown, carried off, pushed by a mover), or if the ragdoll
	// has wandered far; otherwise it would fight the floor and never come to rest.
	originMoved = Distance( cent->lerpOrigin, rd->hb.lastOrigin ) > 0.25f;
	VectorCopy( cent->lerpOrigin, rd->hb.lastOrigin );
	if ( !grabbed ) {
		VectorAdd( cent->lerpOrigin, rd->hb.anchorOffset, target );
		VectorSubtract( target, rd->hb.p[P_PELVIS], err );
		err[2] = 0;
		d = VectorLength( err );
		if ( d > ( originMoved ? RD_ANCHOR_SLACK : RD_ANCHOR_SLACK * 3 ) ) {
			VectorScale( err, ( d - RD_ANCHOR_SLACK ) / d, err );
			// a thrown body is carried along, slack is taken up without a jolt
			VectorScale( err, 0.25f, err );
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hb.hasPoint[k] ) {
					VectorAdd( rd->hb.p[k], err, rd->hb.p[k] );
					VectorAdd( rd->old[k], err, rd->old[k] );
				}
			}
			moved = 1;
		}
		// the body lifted well above the ragdoll (thrown, carried up a ledge)
		d = target[2] - rd->hb.p[P_PELVIS][2];
		if ( d > 40.0f && originMoved ) {
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hb.hasPoint[k] ) {
					rd->hb.p[k][2] += 0.25f * ( d - 40.0f );
					rd->old[k][2] += 0.25f * ( d - 40.0f );
				}
			}
			moved = 1;
		}
	}

	if ( moved < RD_SLEEP_MOVE && !grabbed ) {
		if ( ++rd->hb.stillSteps > RD_SLEEP_STEPS ) {
			rd->hb.asleep = qtrue;
		}
	} else {
		rd->hb.stillSteps = 0;
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
	if ( !rd->hb.hasPoint[child] ) {
		return;
	}
	VectorSubtract( rd->hb.p0[child], rd->hb.p0[to], w0 );
	VectorSubtract( rd->hb.p[child], rd->hb.p[to], w );
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

// renderer layout: matrix rows, then the translation (model space, axis = identity)
static void RD_WriteBone( ragdoll_t *rd, int i, vec3_t m[3], const vec3_t pos ) {
	float *o = rd->out + i * 12;
	int k;

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



/*
=================
RD_NeckBones

The neck as a joint of its own. The head model is the head and the neck in one stiff piece, set on
at the top of the neck, and the body's neck stayed stiff with the chest: all of the head's turn
was up there, so the foot of the head's neck swung out of the collar (plain to see on an open
neckline). Most of the turn is now taken where the neck leaves the shoulders: the Neck bone, and
the collar skinned to it, lean after the head, and the head sits on top of the leaning neck, so
the foot of its neck stays in the collar. rc and rh: how the chest and the head have turned.
=================
*/
#define RD_NECK_SHARE       0.75f       // of the head's turn against the chest, taken at the foot of the neck

static void RD_NeckBones( ragdoll_t *rd, vec3_t rc[3], vec3_t rh[3], const vec3_t neckPos ) {
	vec3_t rn[3], m[3], rel, top, pos;
	int i, hb = rd->segKeyBone[S_HEAD];

	if ( rd->neckBone < 0 || hb < 0 || !rd->hb.segValid[S_HEAD] ) {
		return;
	}
	RD_RotLerp( rc, rh, RD_NECK_SHARE, rn );
	VectorSubtract( rd->bone0Pos[hb], rd->bone0Pos[rd->neckBone], rel );
	RD_MatVec( rn, rel, top );
	VectorAdd( top, neckPos, top );     // where the head sits
	for ( i = 0; i < rd->numBones; i++ ) {
		if ( rd->boneSeg[i] == S_HEAD ) {
			RD_MatMul( rh, rd->bone0Axis[i], m );
			VectorSubtract( rd->bone0Pos[i], rd->bone0Pos[hb], rel );
			RD_MatVec( rh, rel, pos );
			VectorAdd( pos, top, pos );
		} else if ( rd->boneOnNeck[i] ) {
			RD_MatMul( rn, rd->bone0Axis[i], m );
			VectorSubtract( rd->bone0Pos[i], rd->bone0Pos[rd->neckBone], rel );
			RD_MatVec( rn, rel, pos );
			VectorAdd( pos, neckPos, pos );
		} else {
			continue;
		}
		RD_WriteBone( rd, i, m, pos );
	}
}

static void RD_Pose( ragdoll_t *rd ) {
	vec3_t segR[S_COUNT][3], f[3], d0, d, u, neckPos;
	int s, i, anchor;

	for ( s = 0; s < S_COUNT; s++ ) {
		if ( s == S_TORSO ) {
			RD_TorsoFrame( rd->hb.p, f );
			RD_FrameRotation( f, rd->torso0, segR[s] );
			continue;
		}
		if ( !rd->hb.segValid[s] ) {
			continue;
		}
		// turn the parent's rotation so the segment points where its points are
		VectorSubtract( rd->hb.p0[rdSegments[s].to], rd->hb.p0[rdSegments[s].from], d0 );
		VectorNormalize( d0 );
		RD_MatVec( segR[rdSegments[s].parent], d0, u );
		VectorSubtract( rd->hb.p[rdSegments[s].to], rd->hb.p[rdSegments[s].from], d );
		if ( VectorNormalize( d ) < 1e-4f ) {
			VectorCopy( u, d );
		}
		RD_Align( u, d, f );
		RD_MatMul( f, segR[rdSegments[s].parent], segR[s] );
		RD_LimbRoll( rd, s, d0, d, segR[s] );
	}

	VectorCopy( rd->hb.p[P_PELVIS], rd->outOrigin );
	for ( i = 0; i < rd->numBones; i++ ) {
		vec3_t m[3], rel, pos;

		s = rd->boneSeg[i];
		anchor = s == S_TORSO ? P_PELVIS : rdSegments[s].from;

		RD_MatMul( segR[s], rd->bone0Axis[i], m );
		VectorSubtract( rd->bone0Pos[i], rd->hb.p0[anchor], rel );
		RD_MatVec( segR[s], rel, pos );
		VectorAdd( pos, rd->hb.p[anchor], pos );
		RD_WriteBone( rd, i, m, pos );
		if ( i == rd->neckBone ) {
			VectorCopy( pos, neckPos );
		}
	}
	if ( rd->neckBone >= 0 && rd->hb.segValid[S_HEAD] ) {
		RD_NeckBones( rd, segR[rd->boneSeg[rd->neckBone]], segR[S_HEAD], neckPos );
	}
}

/*
==============================================================================

vr_physics 1: the segments as Jolt bodies

==============================================================================
*/



qboolean CG_Ragdoll_RealWeights( void ) {
	trap_Cvar_Update( &vr_realWeights );
	return vr_realWeights.integer != 0;
}

// vr_physics 1: the Jolt body (HitReact's HR_BodyBuild), when the physics is up
static qboolean RD_PhysBuild( ragdoll_t *rd ) {
	if ( !CG_Physics_Ready() ) {
		return qfalse;
	}
	return HR_BodyBuild( &rd->hb, CG_Ragdoll_RealWeights() );
}

// where the bodies are now -> the points (HR_BodyReadPose) and the bones
static void RD_PhysPose( ragdoll_t *rd ) {
	vec3_t neckPos;
	int i, j, k;

	HR_BodyReadPose( &rd->hb );
	for ( k = 0; k < P_COUNT; k++ ) {
		if ( rd->hb.hasPoint[k] ) {
			VectorCopy( rd->hb.p[k], rd->old[k] );
		}
	}
	VectorCopy( rd->hb.p[P_PELVIS], rd->outOrigin );
	for ( i = 0; i < rd->numBones; i++ ) {
		vec3_t m[3], rel, pos;

		j = rd->hb.segPart[rd->boneSeg[i]];
		RD_MatMul( rd->hb.partRot[j], rd->bone0Axis[i], m );
		VectorSubtract( rd->bone0Pos[i], rd->hb.part0Pos[j], rel );
		RD_MatVec( rd->hb.partRot[j], rel, pos );
		VectorAdd( pos, rd->hb.partPos[j], pos );
		RD_WriteBone( rd, i, m, pos );
		if ( i == rd->neckBone ) {
			VectorCopy( pos, neckPos );
		}
	}
	if ( rd->neckBone >= 0 && rd->hb.segValid[S_HEAD] ) {
		RD_NeckBones( rd, rd->hb.partRot[rd->hb.segPart[rd->boneSeg[rd->neckBone]]], rd->hb.partRot[rd->hb.segPart[S_HEAD]], neckPos );
	}
}


/*
=================
CG_Ragdoll_PhysicsOff

vr_physics turned off: the bodies start over as the old ragdolls
=================
*/
void CG_Ragdoll_PhysicsOff( void ) {
	int i;

	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		if ( rdPool[i].inUse && rdPool[i].hb.phys ) {
			RD_Free( &rdPool[i] );
		}
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
	// the body seen longest ago goes back to its death animation, but never one still in
	// view: bodies are drawn one after another, so "the oldest" this frame can be one about
	// to be drawn, which would then take another's, and so on every frame (bodies standing
	// up and falling again, playing their death animations over and over)
	if ( cg.time - oldest->lastSeen < 250 ) {
		return NULL;
	}
	RD_Free( oldest );
	return oldest;
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
	rd->hb.asleep = qfalse;
	rd->hb.stillSteps = 0;
	if ( rd->hb.phys ) {
		Phys_RagdollWake( rd->hb.phys );
	}
}

// a kick or a blast: moving again, and it settles again a little after
static void RD_Unsettle( ragdoll_t *rd ) {
	rd->hb.settled = qfalse;
	if ( rd->hb.settleBy < cg.time + 2500 ) {
		rd->hb.settleBy = cg.time + 2500;
	}
	RD_Wake( rd );
}

// a shot moves the whole body a little and the part it hit more. Pushing only the points by
// the hit (an arm, the head) at full speed set bodies spinning like helicopters.
#define RD_HIT_BODY         0.35f       // share of the push the whole body gets

static void RD_PushPoints( ragdoll_t *rd, const vec3_t point, const vec3_t vel ) {
	float w;
	int k;

	if ( rd->hb.phys ) {
		HR_BodyShot( &rd->hb, point, vel );     // the whole body a little, and the part hit, where it was hit
		RD_Wake( rd );
		return;
	}

	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hb.hasPoint[k] ) {
			continue;
		}
		w = 1.0f - Distance( rd->hb.p[k], point ) / RD_HIT_RADIUS;
		w = RD_HIT_BODY + ( w > 0 ? w * ( 1.0f - RD_HIT_BODY ) : 0 );
		VectorMA( rd->old[k], -RD_STEP * w, vel, rd->old[k] );     // verlet: velocity is p - old
	}
	RD_Wake( rd );
}

static void RD_Blast( ragdoll_t *rd, const vec3_t origin, float radius, float strength ) {
	vec3_t dir;
	float dist, w;
	int k;

	if ( Distance( rd->hb.p[P_PELVIS], origin ) > radius ) {
		return;
	}
	if ( rd->hb.phys ) {
		HR_BodyBlast( &rd->hb, origin, radius, strength );
		RD_Unsettle( rd );
		return;
	}
	for ( k = 0; k < P_COUNT; k++ ) {
		if ( !rd->hb.hasPoint[k] ) {
			continue;
		}
		VectorSubtract( rd->hb.p[k], origin, dir );
		dist = VectorNormalize( dir );
		// mostly by how far the body is, a little by how far the part is: thrown, not spun
		w = 1.0f - ( 0.7f * Distance( rd->hb.p[P_PELVIS], origin ) + 0.3f * dist ) / radius;
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
	rdPending_t *last = NULL;
	int i;

	for ( i = 0; i < RD_MAX_PENDING; i++ ) {
		rdPending_t *pd = &rdPending[i];
		if ( !pd->time || cg.time - pd->time > RD_PENDING_MS || cg.time < pd->time ) {
			continue;
		}
		if ( pd->entityNum == rd->entityNum ) {
			// only the shot that killed: a whole burst added up flung the body round
			if ( !last || pd->time >= last->time ) {
				last = pd;
			}
		} else if ( pd->entityNum < 0 ) {
			RD_Blast( rd, pd->point, pd->radius, pd->strength );
			rd->hb.blasted = Distance( rd->hb.p[P_PELVIS], pd->point ) < pd->radius;
		}
	}
	for ( i = 0; i < RD_MAX_PENDING; i++ ) {
		if ( rdPending[i].entityNum == rd->entityNum && &rdPending[i] != last ) {
			rdPending[i].time = 0;
		}
	}
	if ( last ) {
		// thrown by it (shot reactions)
		vec3_t v;
		VectorScale( last->vel, CG_VRX_Knock( !rd->hb.living ), v );
		if ( rd->hb.living && VectorLength( last->vel ) < 400.0f ) {
			// the first shot into a man on his feet is the jolt every later one is (CG_Ragdoll_Hit):
			// at its full push it threw him over before he'd begun to stagger
			VectorScale( v, 0.25f, v );
		}
		if ( CG_VRX_Knock( qtrue ) > 1.0f && !rd->hb.living ) {
			v[2] += 30.0f * CG_VRX_Knock( qtrue );
		}
		RD_PushPoints( rd, last->point, v );
		last->time = 0;
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
	RD_Sync();
	VectorNormalize2( dir, vel );
	VectorScale( vel, speed, vel );
	// a shot down into a body on the floor can't push it through the floor: the solver would
	// throw the points back out, and a burst from above sent bodies cartwheeling into the air
	if ( vel[2] < 0 ) {
		vel[2] *= 0.3f;
	}

	rd = RD_Find( entityNum );
	if ( rd ) {
		if ( rd->hb.settled && speed < 400.0f ) {
			return;     // stopped for good: a bullet doesn't move it, a kick does
		}
		VectorScale( vel, CG_VRX_Knock( qfalse ), vel );    // shot reactions: bodies knocked harder
		if ( rd->hb.living && speed < 400.0f ) {
			// someone alive braces against it: a jolt he rocks with, not a throw (a bullet
			// has a punch's momentum at most; 0.55 here knocked a man over through any limp part)
			VectorScale( vel, 0.25f, vel );
		} else if ( speed < 400.0f && rd->hb.active && rd->hb.eu == 1 ) {      // (HR_EU_STAGGER, defined below)
			VectorScale( vel, 0.4f, vel );      // dying on his feet: shoved, still fighting for them
		}
		RD_PushPoints( rd, point, vel );
		if ( rd->hb.living && rd->hb.stun ) {
			HR_Rehit( &rd->hb );    // staggering: shoved the new way, clutching the new wound
		}
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
CG_Ragdoll_ShootDead

Our own shot, from start to where it ended (the wall, or whoever it hit): the first dead body
lying in its way takes it. The server only knows a corpse as a low box where the man died, not
where his ragdoll fell or was dragged to, so shots at the body as drawn went through it. Found
here against the ragdoll itself (its parts as capsules): it's knocked (woken if it had come to
rest), holed and bleeds where it was hit. Returns the body's entity, or -1.
=================
*/
// where the line from start to end first meets a ragdoll as it lies (its parts as capsules):
// any dead body's (only under 0), or only's own, alive or dead. Its entity, or -1.
static ragdoll_t *RD_TraceBodies( const vec3_t start, const vec3_t end, int only, vec3_t point ) {
	ragdoll_t *best = NULL;
	vec3_t dir;
	float bestS = 2.0f, len;
	int i, c, self = cg.snap ? cg.snap->ps.clientNum : -1;

	VectorSubtract( end, start, dir );
	len = VectorNormalize( dir );
	if ( len < 1.0f ) {
		return NULL;
	}
	for ( i = 0; i < RD_MAX_RAGDOLLS; i++ ) {
		ragdoll_t *rd = &rdPool[i];
		if ( !rd->inUse || rd->entityNum == self || cg.time - rd->lastSeen > 500 || !rd->hb.hasPoint[P_PELVIS] ) {
			continue;
		}
		if ( only >= 0 ? rd->entityNum != only :
			 ( rd->hb.living || !( cg_entities[rd->entityNum].currentState.eFlags & EF_DEAD ) ) ) {
			continue;
		}
		for ( c = 0; c < RD_NUM_CAPSULES; c++ ) {
			const rdCapsule_t *cp = &rdCapsules[c];
			float s, t, d, in;
			if ( !rd->hb.hasPoint[cp->a] || !rd->hb.hasPoint[cp->b] ) {
				continue;
			}
			d = RD_SegmentDist( start, end, rd->hb.p[cp->a], rd->hb.p[cp->b], &s, &t );
			if ( d >= cp->r ) {
				continue;
			}
			in = sqrt( cp->r * cp->r - d * d ) / len;       // (where it goes in, not where it passes nearest)
			s -= in;
			s = s < 0 ? 0 : s;
			if ( s < bestS ) {
				bestS = s;
				best = rd;
			}
		}
	}
	if ( best ) {
		VectorMA( start, bestS * len, dir, point );
	}
	return best;
}

int CG_Ragdoll_Trace( const vec3_t start, const vec3_t end, int only, vec3_t point ) {
	ragdoll_t *rd = RD_TraceBodies( start, end, only, point );

	if ( !rd ) {
		return -1;
	}
	if ( rd->hb.settled ) {
		RD_Unsettle( rd );
	}
	return rd->entityNum;
}

int CG_Ragdoll_ShootDead( const vec3_t start, const vec3_t end, int weapon ) {
	ragdoll_t *best;
	vec3_t dir, point, back;
	int self = cg.snap ? cg.snap->ps.clientNum : -1;

	best = RD_TraceBodies( start, end, -1, point );
	if ( !best ) {
		return -1;
	}
	VectorSubtract( end, start, dir );
	VectorNormalize( dir );
	if ( best->hb.settled ) {
		RD_Unsettle( best );    // (it settles again a little after)
	}
	CG_Ragdoll_Hit( best->entityNum, point, dir, 160.0f );
	CG_VRX_WoundBy( best->entityNum, point, dir, weapon );
	VectorNegate( dir, back );
	CG_Bullet( point, self, back, qtrue, best->entityNum, qfalse, ENTITYNUM_NONE );
	return best->entityNum;
}

/*
=================
CG_Ragdoll_Explosion
=================
*/
void CG_Ragdoll_Explosion( const vec3_t origin, float radius, float strength ) {
	rdPending_t *pd;
	int i;

	RD_Sync();
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
==============================================================================

Active ragdolls (vr_activeRagdolls, Jolt only)

==============================================================================
*/


static qboolean RD_ActiveOn( void ) {
	trap_Cvar_Update( &vr_activeRagdolls );
	return vr_activeRagdolls.integer != 0;
}


/*
==============================================================================

The living (vr_activeHits): a bullet knocks someone on his feet about for a moment, his
joints pulling back into his animation as he's shoved, and he's drawn part physics, part
animation (RD_Blend) until he's himself again. Knocked down (the server's VRX_GB_DOWNED) he
staggers, goes over and lies there (RD_Eu*, held while down); when the server stands him up
he's eased from where he lies back into his animation.

==============================================================================
*/

#define RD_GETUP_MS         3200        // from the floor back into his animation (within the server's VRX_GETUP_MS)
#define RD_GETUP_BLEND      0.74f       // from here eased into his animation
#define RD_RECOVER_MS       450         // came to on his feet: back into his animation

// a model-space pose: each part's target (rd->hb.target), and where it has the torso in the world
static qboolean RD_PoseTargets( ragdoll_t *rd, refEntity_t *body, const float *pose, int n, vec3_t tPos, vec3_t tAxis[3] ) {
	int j, k, b;
	vec3_t m[3], delta[3], out[3], w[3], mw[3], rel;
	const float *bm;

	if ( n != rd->numBones ) {
		return qfalse;
	}
	for ( j = 0; j < rd->hb.numParts; j++ ) {
		b = rd->segKeyBone[rd->hb.partSeg[j]];
		if ( b < 0 || b >= n ) {
			memcpy( rd->hb.target + j * 9, rd->hb.partAxis[j][0], sizeof( float ) * 9 );
			continue;
		}
		bm = pose + b * 12;
		for ( k = 0; k < 3; k++ ) {
			m[k][0] = bm[0 * 3 + k];
			m[k][1] = bm[1 * 3 + k];
			m[k][2] = bm[2 * 3 + k];
		}
		RD_FrameRotation( m, rd->bone0Axis[b], delta );
		RD_MatMul( delta, rd->hb.part0Axis[j], out );
		memcpy( rd->hb.target + j * 9, out[0], sizeof( float ) * 9 );
	}
	b = rd->segKeyBone[S_TORSO];
	if ( b < 0 || b >= n ) {
		return qfalse;
	}
	bm = pose + b * 12;
	for ( k = 0; k < 3; k++ ) {
		m[k][0] = bm[0 * 3 + k];
		m[k][1] = bm[1 * 3 + k];
		m[k][2] = bm[2 * 3 + k];
		VectorCopy( body->axis[k], w[k] );
	}
	RD_MatMul( w, m, mw );
	RD_FrameRotation( mw, rd->bone0Axis[b], delta );
	RD_MatMul( delta, rd->hb.part0Axis[0], tAxis );
	RD_MatVec( w, bm + 9, tPos );
	VectorAdd( tPos, body->origin, tPos );
	VectorSubtract( rd->hb.part0Pos[0], rd->bone0Pos[b], rel );
	RD_MatVec( delta, rel, rel );
	VectorAdd( tPos, rel, tPos );
	return qtrue;
}


#define RD_GU_ANIM      -1

// a get-up stage's pose (or the animation's), and the frame it's placed in
static const float *RD_GetUpPose( ragdoll_t *rd, centity_t *cent, refEntity_t *body, int which, refEntity_t *frame, int *n ) {
	static int parents[RD_MAX_BONES], flags[RD_MAX_BONES];
	static char names[RD_MAX_BONES * 32];
	static float bones[RD_MAX_BONES * 12];
	const float *pose;

	*frame = *body;
	if ( which != RD_GU_ANIM ) {
		vec3_t ang;
		pose = CG_VRX_GetUpPose( cent, body, which );
		if ( pose ) {
			VectorSet( ang, 0, which == VRX_GU_SIT ? rd->hb.getUpSitYaw : rd->hb.getUpYaw, 0 );
			AnglesToAxis( ang, frame->axis );
			*n = rd->numBones;
			return pose;
		}
	}
	{
		refEntity_t anim = *body;
		anim.ragdollBones = NULL;
		*n = trap_R_GetSkeleton( &anim, RD_MAX_BONES, parents, flags, names, bones );
	}
	if ( which != RD_GU_ANIM ) {
		vec3_t ang;     // (no pose for this model: his animation, but facing the way he gets up)
		VectorSet( ang, 0, rd->hb.getUpYaw, 0 );
		AnglesToAxis( ang, frame->axis );
	}
	return bones;
}


// HitReact's physics get-up asks for each stage's pose (HR_GU_* are VRX_GU_*)
static int RD_HostGetUpPose( void *user, int stage, float *partTargets, float *torsoPos, float *torsoAxis ) {
	ragdoll_t *rd = user;
	refEntity_t frame;
	const float *pose;
	vec3_t tAxis[3];
	int n;

	if ( !rdCent || !rdBody ) {
		return 0;
	}
	(void)partTargets;      // (RD_PoseTargets fills rd->hb.target, which it is)
	pose = RD_GetUpPose( rd, rdCent, rdBody, stage, &frame, &n );
	if ( !RD_PoseTargets( rd, &frame, pose, n, torsoPos, tAxis ) ) {
		return 0;
	}
	memcpy( torsoAxis, tAxis, sizeof( tAxis ) );
	return 1;
}

qboolean CG_Ragdoll_PhysHits( void ) {
	trap_Cvar_Update( &vr_activeHits );
	trap_Cvar_Update( &vr_ragdolls );
	return vr_activeHits.integer && vr_ragdolls.integer && RD_ActiveOn() && CG_Physics_Ready();
}


/*
=================
RD_Blend

what's drawn: from (a pose in the model's space; NULL the ragdoll, rd->out) eased by a (0..1)
towards to (likewise; NULL the animation the body would play), into rd->blend
=================
*/
static void RD_Blend( ragdoll_t *rd, refEntity_t *body, float a, const float *from, const float *to ) {
	static int parents[RD_MAX_BONES], flags[RD_MAX_BONES];
	static char names[RD_MAX_BONES * 32];
	static float bones[RD_MAX_BONES * 12];
	refEntity_t anim = *body;
	vec3_t w[3];
	int n, i, j, k;

	rd->blended = qfalse;
	if ( a <= 0.001f ) {
		return;
	}
	a = a > 1 ? 1 : a;
	anim.ragdollBones = NULL;
	if ( to ) {
		n = rd->numBones;
		memcpy( bones, to, sizeof( float ) * 12 * n );
	} else {
		n = trap_R_GetSkeleton( &anim, RD_MAX_BONES, parents, flags, names, bones );
	}
	if ( n != rd->numBones ) {
		return;
	}
	VectorCopy( body->axis[0], w[0] );
	VectorCopy( body->axis[1], w[1] );
	VectorCopy( body->axis[2], w[2] );
	for ( i = 0; i < n; i++ ) {
		const float *bm = bones + i * 12, *o = from ? from + i * 12 : rd->out + i * 12;
		float *q = rd->blend + i * 12, len[3];
		vec3_t ma[3], mw[3], mr[3], m[3], pa, pr, cr;

		for ( k = 0; k < 3; k++ ) {     // as RD_Build and RD_WriteBone: the rotation each means, as columns
			for ( j = 0; j < 3; j++ ) {
				ma[k][j] = rd->boneIsTag[i] ? bm[k * 3 + j] : bm[j * 3 + k];
				mr[k][j] = rd->boneIsTag[i] ? o[k * 3 + j] : o[j * 3 + k];
			}
		}
		RD_MatMul( w, ma, mw );
		RD_MatVec( w, bm + 9, pa );
		VectorAdd( pa, body->origin, pa );
		if ( from ) {       // a model-space pose: into the world as the animation is
			RD_MatMul( w, mr, m );
			memcpy( mr, m, sizeof( m ) );
			RD_MatVec( w, o + 9, pr );
			VectorAdd( pr, body->origin, pr );
		} else {
			VectorAdd( o + 9, rd->outOrigin, pr );
		}
		for ( k = 0; k < 3; k++ ) {
			len[k] = VectorLength( mr[k] ) * ( 1 - a ) + VectorLength( mw[k] ) * a;
			for ( j = 0; j < 3; j++ ) {
				m[k][j] = mr[k][j] * ( 1 - a ) + mw[k][j] * a;
			}
		}
		// back to a rotation (the same handedness)
		CrossProduct( m[0], m[1], cr );
		VectorNormalize( m[0] );
		VectorMA( m[1], -DotProduct( m[1], m[0] ), m[0], m[1] );
		VectorNormalize( m[1] );
		{
			float hand = DotProduct( cr, m[2] ) < 0 ? -1.0f : 1.0f;
			CrossProduct( m[0], m[1], m[2] );
			VectorScale( m[2], hand, m[2] );
		}
		for ( k = 0; k < 3; k++ ) {
			VectorScale( m[k], len[k], m[k] );
			for ( j = 0; j < 3; j++ ) {
				if ( rd->boneIsTag[i] ) {
					q[k * 3 + j] = m[k][j];
				} else {
					q[j * 3 + k] = m[k][j];
				}
			}
			q[9 + k] = pr[k] * ( 1 - a ) + pa[k] * a - rd->outOrigin[k];
		}
	}
	rd->blended = qtrue;
}

/*
=================
RD_GuAnim*

Getting up off the floor as an animation: the game's own get-ups, played on his skeleton.
The zombie model has two, off his back (getup_back: up on his elbows, sits up, rolls onto a
knee and stands) and off his front (revive: pushes up onto his hands and knees, then up), on
the same biped skeleton as the soldiers. Each of his bones takes its turn from the zombie's,
its length from his own (so his proportions stay his), and the tags (head, weapon) ride on
their bones as in his animation. The animation is laid where he lies, turned so its body
lines up with his, and eased into from the pose he lies in; his server body is turned and
moved to where the animation leaves him standing ("vrgetupyaw"), and at the end he's eased
into his own animation from there. Physics sleeps meanwhile; killed or knocked down again on
the way up, the physics takes over from the pose he's in (RD_GuAnimAbort).
=================
*/
#define RD_GUANIM_IN        350         // eased from how he lies into the animation's first frame
#define RD_GUANIM_PLAY      2850        // the animation, start to end (then held as he's eased into his own)

static qhandle_t rdGuModel;
static int rdGuFirst[2], rdGuNum[2];     // [0] off his back (getup_back), [1] off his front (revive)

static void RD_GuAnimInit( void ) {
	static char buf[8192];
	fileHandle_t f;
	char *p, *tok;
	int len, k;

	rdGuModel = 0;
	rdGuNum[0] = rdGuNum[1] = 0;
	len = trap_FS_FOpenFile( "models/players/flesh/wolfanim.cfg", &f, FS_READ );
	if ( len <= 0 || !f ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;
	p = buf;
	while ( 1 ) {
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		k = !Q_stricmp( tok, "getup_back" ) ? 0 : !Q_stricmp( tok, "revive" ) ? 1 : -1;
		if ( k >= 0 ) {
			rdGuFirst[k] = atoi( COM_Parse( &p ) );
			rdGuNum[k] = atoi( COM_Parse( &p ) );
		}
	}
	if ( rdGuNum[0] > 10 && rdGuNum[1] > 10 ) {
		rdGuModel = trap_R_RegisterModel( "models/players/flesh/body.mds" );
	}
}

// a bone's 12 floats (as the skeleton gives them) to its axes as columns, and back
static void RD_BoneAxes( const float *bm, qboolean tag, vec3_t ax[3] ) {
	int k, j;
	for ( k = 0; k < 3; k++ ) {
		for ( j = 0; j < 3; j++ ) {
			ax[k][j] = tag ? bm[k * 3 + j] : bm[j * 3 + k];
		}
	}
}

static void RD_BoneStore( float *bm, qboolean tag, vec3_t ax[3], const vec3_t pos ) {
	int k, j;
	for ( k = 0; k < 3; k++ ) {
		for ( j = 0; j < 3; j++ ) {
			if ( tag ) {
				bm[k * 3 + j] = ax[k][j];
			} else {
				bm[j * 3 + k] = ax[k][j];
			}
		}
	}
	VectorCopy( pos, bm + 9 );
}

// the zombie's skeleton at a frame of the animation (fractional), model space
static int RD_GuSkeleton( float frame, int last, float *bones, int *parents, int *flags, char *names ) {
	refEntity_t e;
	int f0 = (int)frame, f1;
	float frac = frame - f0;

	if ( f0 >= last ) {
		f0 = last;
		frac = 0;
	}
	f1 = f0 + 1 > last ? last : f0 + 1;
	memset( &e, 0, sizeof( e ) );
	e.hModel = rdGuModel;
	e.frame = e.torsoFrame = f1;
	e.oldframe = e.oldTorsoFrame = f0;
	e.backlerp = e.torsoBacklerp = 1.0f - frac;
	AxisClear( e.axis );
	AxisClear( e.torsoAxis );
	return trap_R_GetSkeleton( &e, RD_MAX_BONES, parents, flags, names, bones );
}

// his skeleton, model space, at t ms into the animation: each bone turned as the zombie's,
// laid out with his own bone lengths
static qboolean RD_GuAnimPose( ragdoll_t *rd, int t, float *out ) {
	static int parents[RD_MAX_BONES], flags[RD_MAX_BONES];
	static char names[RD_MAX_BONES * 32];
	static float src[RD_MAX_BONES * 12];
	vec3_t ax[RD_MAX_BONES][3], pos[RD_MAX_BONES];
	int k = rd->hb.getUpProne ? 1 : 0, i, j, p, n;
	float frame;

	t = t < 0 ? 0 : t > RD_GUANIM_PLAY ? RD_GUANIM_PLAY : t;
	frame = rdGuFirst[k] + ( rdGuNum[k] - 1 ) * ( t / (float)RD_GUANIM_PLAY );
	n = RD_GuSkeleton( frame, rdGuFirst[k] + rdGuNum[k] - 1, src, parents, flags, names );
	if ( n <= 0 ) {
		return qfalse;
	}
	for ( i = 0; i < rd->numBones; i++ ) {
		const float *loc = rd->guLoc + i * 12;
		p = rd->guParent[i];
		j = rd->guMap[i];
		if ( j >= 0 && j < n ) {
			RD_BoneAxes( src + j * 12, ( flags[j] & BONEFLAG_TAG ) != 0, ax[i] );
		} else if ( p >= 0 ) {
			vec3_t rel[3];
			memcpy( rel, loc, sizeof( rel ) );
			RD_MatMul( ax[p], rel, ax[i] );
		} else {
			AxisClear( ax[i] );
		}
		if ( p < 0 ) {
			// the root where the zombie's is, its height off the floor (24 below the origin)
			// scaled to his legs
			if ( j >= 0 && j < n ) {
				VectorCopy( src + j * 12 + 9, pos[i] );
				pos[i][2] = -24.0f + ( pos[i][2] + 24.0f ) * rd->guLegK;
			} else {
				VectorClear( pos[i] );
			}
		} else {
			RD_MatVec( ax[p], loc + 9, pos[i] );
			VectorAdd( pos[i], pos[p], pos[i] );
		}
		RD_BoneStore( out + i * 12, rd->boneIsTag[i], ax[i], pos[i] );
	}
	return qtrue;
}

// a model-space pose into the world where the animation is laid (as rd->out has its bones)
static void RD_GuAnimWorld( ragdoll_t *rd, const float *pose, float *out ) {
	int i;
	for ( i = 0; i < rd->numBones; i++ ) {
		vec3_t ma[3], mw[3], pw;
		RD_BoneAxes( pose + i * 12, rd->boneIsTag[i], ma );
		RD_MatMul( rd->guAxis, ma, mw );
		RD_MatVec( rd->guAxis, pose + i * 12 + 9, pw );
		VectorAdd( pw, rd->guOrigin, pw );
		VectorSubtract( pw, rd->outOrigin, pw );
		RD_BoneStore( out + i * 12, rd->boneIsTag[i], mw, pw );
	}
}

// part of the way (a) from one pose to another, both as rd->out has its bones
static void RD_LerpBones( ragdoll_t *rd, const float *from, const float *to, float a, float *out ) {
	int i, j, k;
	for ( i = 0; i < rd->numBones; i++ ) {
		vec3_t ma[3], mb[3], m[3], cr, pos;
		float len[3], hand;
		RD_BoneAxes( from + i * 12, rd->boneIsTag[i], ma );
		RD_BoneAxes( to + i * 12, rd->boneIsTag[i], mb );
		for ( k = 0; k < 3; k++ ) {
			len[k] = VectorLength( ma[k] ) * ( 1 - a ) + VectorLength( mb[k] ) * a;
			for ( j = 0; j < 3; j++ ) {
				m[k][j] = ma[k][j] * ( 1 - a ) + mb[k][j] * a;
			}
			pos[k] = from[i * 12 + 9 + k] * ( 1 - a ) + to[i * 12 + 9 + k] * a;
		}
		CrossProduct( m[0], m[1], cr );
		VectorNormalize( m[0] );
		VectorMA( m[1], -DotProduct( m[1], m[0] ), m[0], m[1] );
		VectorNormalize( m[1] );
		hand = DotProduct( cr, m[2] ) < 0 ? -1.0f : 1.0f;
		CrossProduct( m[0], m[1], m[2] );
		VectorScale( m[2], hand, m[2] );
		for ( k = 0; k < 3; k++ ) {
			VectorScale( m[k], len[k], m[k] );
		}
		RD_BoneStore( out + i * 12, rd->boneIsTag[i], m, pos );
	}
}

static int RD_BoneByName( const char *names, int n, const char *name ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		if ( !Q_stricmp( names + i * 32, name ) ) {
			return i;
		}
	}
	return -1;
}

static float RD_Yaw( const vec3_t from, const vec3_t to ) {
	return RAD2DEG( atan2( to[1] - from[1], to[0] - from[0] ) );
}

// set up getting up as the animation (qfalse: this body can't, the physics get-up instead)
static qboolean RD_GuAnimBegin( ragdoll_t *rd, centity_t *cent, refEntity_t *body ) {
	static int sParents[RD_MAX_BONES], sFlags[RD_MAX_BONES], fParents[RD_MAX_BONES], fFlags[RD_MAX_BONES];
	static char sNames[RD_MAX_BONES * 32], fNames[RD_MAX_BONES * 32];
	static float ref[RD_MAX_BONES * 12], fl[RD_MAX_BONES * 12], pose[RD_MAX_BONES * 12];
	static const char *need[] = { "Bip01 Pelvis", "Bip01 Spine", "Bip01 Head", "Bip01 L Thigh", "Bip01 R Thigh",
								  "Bip01 L Calf", "Bip01 R Calf", "Bip01 L Foot", "Bip01 R Foot", "Bip01 L UpperArm", "Bip01 R UpperArm" };
	refEntity_t anim = *body;
	vec3_t tf[3], ax[3], axp[3], d, rp, rh, ap, ah, start, end, ang;
	trace_t tr;
	int n, nf, i, j, p, root = -1, head, mouth, calf, foot, fcalf, ffoot, fthigh, thigh, k;
	float legS, legF, yaw, endYaw;

	if ( !rdGuModel || !rd->hb.phys ) {
		return qfalse;
	}
	anim.ragdollBones = NULL;
	n = trap_R_GetSkeleton( &anim, RD_MAX_BONES, sParents, sFlags, sNames, ref );
	if ( n != rd->numBones ) {
		return qfalse;
	}
	RD_TorsoFrame( rd->hb.p, tf );
	rd->hb.getUpProne = tf[2][2] > 0;     // his back to the sky: on his front
	k = rd->hb.getUpProne ? 1 : 0;
	nf = RD_GuSkeleton( rdGuFirst[k], rdGuFirst[k], fl, fParents, fFlags, fNames );
	if ( nf <= 0 ) {
		return qfalse;
	}
	for ( i = 0; i < (int)ARRAY_LEN( need ); i++ ) {
		if ( RD_BoneByName( sNames, n, need[i] ) < 0 || RD_BoneByName( fNames, nf, need[i] ) < 0 ) {
			return qfalse;      // not a man's skeleton (a dog, a loper)
		}
	}
	// his bones relative to their parents, from how his animation has them now; which of the
	// zombie's each follows
	for ( i = 0; i < n; i++ ) {
		float *loc = rd->guLoc + i * 12;
		p = sParents[i];
		if ( p >= i ) {
			return qfalse;      // (parents first, or it can't be laid out in one pass)
		}
		rd->guParent[i] = p;
		rd->guMap[i] = -1;
		if ( !( sFlags[i] & BONEFLAG_TAG ) && !Q_stricmpn( sNames + i * 32, "Bip01", 5 ) ) {
			j = RD_BoneByName( fNames, nf, sNames + i * 32 );
			if ( j >= 0 && !( fFlags[j] & BONEFLAG_TAG ) ) {
				rd->guMap[i] = j;
			}
		}
		if ( p < 0 ) {
			if ( root >= 0 || rd->guMap[i] < 0 ) {
				return qfalse;
			}
			root = i;
			memset( loc, 0, sizeof( float ) * 12 );
			continue;
		}
		RD_BoneAxes( ref + p * 12, ( sFlags[p] & BONEFLAG_TAG ) != 0, axp );
		RD_BoneAxes( ref + i * 12, ( sFlags[i] & BONEFLAG_TAG ) != 0, ax );
		for ( j = 0; j < 3; j++ ) {     // his turn from the parent's: the parent's axes into his
			loc[j * 3 + 0] = DotProduct( ax[j], axp[0] );
			loc[j * 3 + 1] = DotProduct( ax[j], axp[1] );
			loc[j * 3 + 2] = DotProduct( ax[j], axp[2] );
		}
		VectorSubtract( ref + i * 12 + 9, ref + p * 12 + 9, d );
		loc[9] = DotProduct( d, axp[0] );
		loc[10] = DotProduct( d, axp[1] );
		loc[11] = DotProduct( d, axp[2] );
	}
	if ( root < 0 ) {
		return qfalse;
	}
	// his legs against the zombie's, for how high the hips ride
	thigh = RD_BoneByName( sNames, n, "Bip01 L Thigh" );
	calf = RD_BoneByName( sNames, n, "Bip01 L Calf" );
	foot = RD_BoneByName( sNames, n, "Bip01 L Foot" );
	fthigh = RD_BoneByName( fNames, nf, "Bip01 L Thigh" );
	fcalf = RD_BoneByName( fNames, nf, "Bip01 L Calf" );
	ffoot = RD_BoneByName( fNames, nf, "Bip01 L Foot" );
	legS = Distance( ref + thigh * 12 + 9, ref + calf * 12 + 9 ) + Distance( ref + calf * 12 + 9, ref + foot * 12 + 9 );
	legF = Distance( fl + fthigh * 12 + 9, fl + fcalf * 12 + 9 ) + Distance( fl + fcalf * 12 + 9, fl + ffoot * 12 + 9 );
	rd->guLegK = legF > 1 ? legS / legF : 1;
	rd->guLegK = rd->guLegK < 0.7f ? 0.7f : rd->guLegK > 1.4f ? 1.4f : rd->guLegK;

	// laid where he lies: its hips on his, its body along his
	head = RD_BoneByName( sNames, n, "Bip01 Head" );
	if ( !RD_GuAnimPose( rd, 0, pose ) ) {
		return qfalse;
	}
	VectorAdd( rd->out + root * 12 + 9, rd->outOrigin, rp );
	VectorAdd( rd->out + head * 12 + 9, rd->outOrigin, rh );
	VectorCopy( pose + root * 12 + 9, ap );
	VectorCopy( pose + head * 12 + 9, ah );
	yaw = RD_Yaw( rp, rh ) - RD_Yaw( ap, ah );
	VectorSet( ang, 0, yaw, 0 );
	AnglesToAxis( ang, rd->guAxis );
	RD_MatVec( rd->guAxis, ap, d );
	rd->guOrigin[0] = rp[0] - d[0];
	rd->guOrigin[1] = rp[1] - d[1];
	// on the floor under him
	VectorSet( start, rp[0], rp[1], rp[2] + 8 );
	VectorSet( end, rp[0], rp[1], rp[2] - 48 );
	CG_Trace( &tr, start, NULL, NULL, end, rd->entityNum, MASK_SOLID );
	rd->guOrigin[2] = ( tr.fraction < 1 && !tr.startsolid ? tr.endpos[2] : rp[2] - 4 ) + 24.0f;

	// where it leaves him standing, and facing: his body goes there
	if ( !RD_GuAnimPose( rd, RD_GUANIM_PLAY, pose ) ) {
		return qfalse;
	}
	mouth = RD_BoneByName( sNames, n, "tag_mouth" );
	endYaw = yaw;
	if ( mouth >= 0 ) {
		vec3_t fh, fm;
		VectorCopy( pose + head * 12 + 9, fh );
		VectorCopy( pose + mouth * 12 + 9, fm );
		if ( fabs( fm[0] - fh[0] ) + fabs( fm[1] - fh[1] ) > 1.0f ) {
			endYaw = yaw + RD_Yaw( fh, fm );
		}
	}
	rd->hb.getUpYaw = rd->hb.getUpSitYaw = AngleNormalize360( endYaw );
	RD_MatVec( rd->guAxis, pose + root * 12 + 9, d );
	trap_SendClientCommand( va( "vrgetupyaw %d %.0f %.0f %.0f", rd->entityNum, rd->hb.getUpYaw,
								rd->guOrigin[0] + d[0], rd->guOrigin[1] + d[1] ) );
	rd->guAnim = qtrue;
	Phys_RagdollDrive( rd->hb.phys, NULL, NULL, 0 );
	Phys_RagdollPin( rd->hb.phys, NULL, NULL, 0 );
	Phys_RagdollCatch( rd->hb.phys, 0, 0, 0 );
	Phys_RagdollSleep( rd->hb.phys );
	return qtrue;
}

// the animation's frame for now, eased in from how he lay and out into his own animation
static void RD_GuAnimFrame( ragdoll_t *rd, refEntity_t *body ) {
	static float pose[RD_MAX_BONES * 12], world[RD_MAX_BONES * 12];
	int t = cg.time - rd->hb.getUpStart;
	float a;

	if ( !RD_GuAnimPose( rd, t, pose ) ) {
		return;     // (the model's gone: he stays as he was drawn)
	}
	RD_GuAnimWorld( rd, pose, world );
	a = t / (float)RD_GUANIM_IN;
	if ( a < 1 ) {
		a = a < 0 ? 0 : a;
		RD_LerpBones( rd, rd->out, world, a * a * ( 3 - 2 * a ), rd->blend );
	} else {
		memcpy( rd->blend, world, sizeof( float ) * 12 * rd->numBones );
	}
	rd->blended = qtrue;
	if ( t > RD_GUANIM_PLAY ) {
		// up: from where the animation left him into his own (the physics pose is done with)
		memcpy( rd->out, rd->blend, sizeof( float ) * 12 * rd->numBones );
		a = ( t - RD_GUANIM_PLAY ) / (float)( RD_GETUP_MS - RD_GUANIM_PLAY );
		a = a > 1 ? 1 : a;
		RD_Blend( rd, body, a * a * ( 3 - 2 * a ), NULL, NULL );     // (none yet: drawn from out)
	}
}

// the get-up animation broken off (killed, knocked down again): the physics bodies put where
// he's drawn, and off they go from there
static void RD_GuAnimAbort( ragdoll_t *rd ) {
	static float pos[PHYS_MAX_PARTS * 3], axis[PHYS_MAX_PARTS * 9];
	const float *drawn = rd->blended ? rd->blend : rd->out;
	int j, b;

	if ( !rd->guAnim ) {
		return;
	}
	rd->guAnim = qfalse;
	if ( !rd->hb.phys ) {
		return;
	}
	Phys_RagdollPose( rd->hb.phys, pos, axis );
	for ( j = 0; j < rd->hb.numParts; j++ ) {
		vec3_t m[3], delta[3], out[3], rel, bp;
		b = rd->segKeyBone[rd->hb.partSeg[j]];
		if ( b < 0 || b >= rd->numBones ) {
			continue;
		}
		RD_BoneAxes( drawn + b * 12, rd->boneIsTag[b], m );
		RD_FrameRotation( m, rd->bone0Axis[b], delta );
		RD_MatMul( delta, rd->hb.part0Axis[j], out );
		memcpy( axis + j * 9, out[0], sizeof( float ) * 9 );
		VectorAdd( drawn + b * 12 + 9, rd->outOrigin, bp );
		VectorSubtract( rd->hb.part0Pos[j], rd->bone0Pos[b], rel );
		RD_MatVec( delta, rel, rel );
		VectorAdd( bp, rel, pos + j * 3 );
	}
	memcpy( rd->out, drawn, sizeof( float ) * 12 * rd->numBones );
	rd->blended = qfalse;
	Phys_RagdollSetPose( rd->hb.phys, pos, axis );
	Phys_RagdollWake( rd->hb.phys );
	RD_PhysPose( rd );
}

// HitReact's get-up as the game's animation (RD_GuAnim*), and broken off
static int RD_HostGetUpAnimBegin( void *user ) {
	ragdoll_t *rd = user;

	rd->guAnim = qfalse;
	return rdCent && rdBody && RD_GuAnimBegin( rd, rdCent, rdBody );
}

static void RD_HostGetUpAnimAbort( void *user ) {
	RD_GuAnimAbort( (ragdoll_t *)user );
}

// HitReact put him back to plain physics: drawn from the ragdoll alone again
static void RD_ResetDrawn( ragdoll_t *rd ) {
	if ( rd->hb.resetDrawn ) {
		rd->hb.resetDrawn = qfalse;
		rd->blended = qfalse;
	}
}

/*
=================
CG_Ragdoll_Has

this entity is drawn as a ragdoll (a pose layer leaves it alone)
=================
*/
qboolean CG_Ragdoll_Has( int entityNum ) {
	return RD_Find( entityNum ) != NULL;
}


/*
=================
CG_Ragdoll_Apply

Called with a skeletal character's body just before it is drawn. A dead one is drawn
as a ragdoll instead of its animation.
=================
*/
static int rdFiredAt[MAX_GENTITIES];

// someone fired (cg_event.c EV_FIRE_WEAPON): staggering in the physics, the shot kicks his gun
// arm back (HR_Recoil)
void CG_Ragdoll_Fired( int entityNum ) {
	ragdoll_t *rd;

	if ( entityNum < 0 || entityNum >= MAX_CLIENTS ) {
		return;
	}
	rdFiredAt[entityNum] = cg.time;
	rd = RD_Find( entityNum );
	if ( rd && rd->hb.living && rd->hb.phys ) {
		HR_Recoil( &rd->hb );
	}
}

// the Uebersoldaten and the crypt monster (Olaric, AICHAR_HELGA): no physics at all, alive or
// dead; they play their own animations, death included
static qboolean RD_NoPhysics( const centity_t *cent ) {
	switch ( cent->currentState.aiChar ) {
	case AICHAR_SUPERSOLDIER: case AICHAR_SUPERSOLDIER_LAB: case AICHAR_PROTOSOLDIER: case AICHAR_HELGA:
		return qtrue;
	default:
		return qfalse;
	}
}

void CG_Ragdoll_Apply( centity_t *cent, refEntity_t *body ) {
	ragdoll_t *rd = RD_Find( cent->currentState.number );

	if ( RD_NoPhysics( cent ) ) {
		if ( rd ) {
			RD_Free( rd );
		}
		return;
	}
	{
	vec3_t grabPoint;
	qboolean grabbed;
	int steps = 0;

	qboolean dead = ( cent->currentState.eFlags & EF_DEAD ) != 0, downed, hit;
	int gore;

	RD_Sync();
	rdCent = cent;
	rdBody = body;
	// alive: knocked down (an active ragdoll until the server stands him up), or a fresh bullet
	// on his feet (knocked about a moment); Jolt only, never your own body or a pose layer's
	downed = !dead && CG_VRX_DownedStyle( cent ) > 0 && RD_ActiveOn() && CG_Physics_Ready() && ( !rd || rd->hb.phys );
	hit = !dead && !downed && CG_VRX_Stunned( cent ) && CG_Ragdoll_PhysHits() && !CG_VR_FullBody( cent ) &&
		  !body->ragdollBones && ( !rd || rd->hb.phys );     // shot and stunned: staggering under the bullets
	if ( !rd && ( downed || hit ) && HR_NumActive() >= RD_MAX_ACTIVE ) {
		downed = hit = qfalse;
	}
	if ( rd && rd->hb.living && !dead ) {
		// stunned, knocked down, getting up (HitReact's HR_Living)
		qboolean himself = HR_Living( &rd->hb, hit, downed );
		RD_ResetDrawn( rd );
		if ( himself ) {
			RD_Free( rd );          // himself again
			return;
		}
		// shot in the chest (or shooting anyway), staggering with a gun in his hands: he holds it
		// on you and fires back as he reels (the server lets him, G_VRX_Stun)
		if ( !downed && cent->currentState.weapon != WP_NONE && !( CG_VRX_GoreBits( cent ) & VRX_GB_DOWNED ) &&
			 ( rd->hb.euRegion == HR_RG_CHEST || cg.time - rdFiredAt[cent->currentState.number % MAX_GENTITIES] < 800 ) ) {
			HR_Aim( &rd->hb, cg.refdef.vieworg );
		} else {
			HR_Aim( &rd->hb, NULL );
		}
	} else if ( rd && !dead && !downed ) {
		RD_Free( rd );              // alive again (respawned, revived)
		return;
	}
	if ( ( !dead && !downed && !hit && !rd ) || !vr_ragdolls.integer || !body->hModel ||
		 cent->currentState.eType != ET_PLAYER ) {
		if ( rd ) {
			RD_Free( rd );          // ragdolls off
		}
		return;
	}

	if ( rd && rd->model != body->hModel ) {
		RD_Free( rd );
		rd = NULL;
	}
	if ( !rd && cent->currentState.clientNum != cent->currentState.number &&
		 cent->currentState.clientNum >= 0 && cent->currentState.clientNum < MAX_CLIENTS ) {
		// a dead player's body left behind as a corpse (co-op respawns): the corpse
		// carries on with the player's ragdoll instead of starting over
		ragdoll_t *player = RD_Find( cent->currentState.clientNum );
		if ( player && player->model == body->hModel && Distance( player->hb.p[P_PELVIS], cent->lerpOrigin ) < 96.0f ) {
			rd = player;
			rd->entityNum = cent->currentState.number;
			rd->hb.id = rd->entityNum;
			VectorSubtract( rd->hb.p[P_PELVIS], cent->lerpOrigin, rd->hb.anchorOffset );
			VectorCopy( cent->lerpOrigin, rd->hb.lastOrigin );
		}
	}
	if ( !rd ) {
		int n = cent->currentState.number;
		refEntity_t start = *body;
		qboolean ok;

		// from the pose he was in a moment ago, alive
		rdStartPose = NULL;
		if ( dead && n >= 0 && n < MAX_CLIENTS && rdLast[n].model == body->hModel && cg.time - rdLast[n].time >= 0 &&
			 cg.time - rdLast[n].time < 300 && Distance( rdLast[n].origin, body->origin ) < 48.0f ) {
			rdLast_t *l = &rdLast[n];
			start.frame = l->frame;
			start.oldframe = l->oldframe;
			start.backlerp = l->backlerp;
			start.torsoFrame = l->torsoFrame;
			start.oldTorsoFrame = l->oldTorsoFrame;
			start.torsoBacklerp = l->torsoBacklerp;
			VectorCopy( l->origin, start.origin );
			memcpy( start.axis, l->axis, sizeof( start.axis ) );
			memcpy( start.torsoAxis, l->torsoAxis, sizeof( start.torsoAxis ) );
			start.ragdollBones = NULL;
			if ( l->numBones > 0 ) {
				rdStartPose = rdLastPose[n];
			}
			l->time = 0;
		}
		rd = RD_Alloc();
		if ( !rd ) {
			return;
		}
		ok = RD_Build( rd, cent, &start );
		rdStartPose = NULL;
		if ( !ok ) {
			RD_Free( rd );
			return;
		}
		if ( !dead ) {
			rd->hb.living = qtrue;     // (not thrown: he's on his feet, or going down under his own weight)
			VectorClear( rd->hb.startVel );
			VectorClear( rd->hb.killPush );
		}
		RD_PhysBuild( rd );     // vr_physics 1: Jolt bodies (otherwise, or if that fails, the points)
		if ( !dead && !rd->hb.phys ) {
			RD_Free( rd );      // no physics: he keeps his animation
			return;
		}
		RD_ApplyPending( rd );
		if ( rd->hb.phys && downed ) {
			HR_KnockDown( &rd->hb );
		} else if ( rd->hb.phys && !dead ) {
			HR_Stun( &rd->hb );
		} else if ( rd->hb.phys ) {
			HR_Die( &rd->hb, cg.time - rd->hb.lastHitTime < 100 && rd->hb.lastHitTime ? rd->hb.lastHitPart : -1, rd->hb.lastHitPoint );
		}
		RD_ResetDrawn( rd );
		if ( vr_ragdolls.integer > 1 ) {    // 2: report each one (testing)
			int k, points = 0;
			for ( k = 0; k < P_COUNT; k++ ) {
				points += rd->hb.hasPoint[k];
			}
			if ( rd->hb.phys ) {
				CG_Printf( "ragdoll: entity %d, %d bones, %d Jolt bodies\n", rd->entityNum, rd->numBones, rd->hb.numParts );
			} else {
				CG_Printf( "ragdoll: entity %d, %d bones, %d points, %d links\n", rd->entityNum, rd->numBones, points, rd->numLinks );
			}
		}
		if ( rd->hb.phys ) {
			RD_PhysPose( rd );
		} else {
			RD_Pose( rd );
		}
	}
	rd->lastSeen = cg.time;
	if ( rd->hb.living && dead ) {
		HR_Killed( &rd->hb );       // killed while knocked about, down or getting up: the same body, now dying (no pop)
		RD_ResetDrawn( rd );
	}

	grabbed = RD_GrabPoint( rd, cent, grabPoint );
	if ( grabbed && rd->grabber != cent->currentState.dl_intensity ) {
		// picked up: a VR hand holds it by the point nearest the hand. Without a headset the
		// "hand" is just in front of the view and swings round with every turn of the mouse:
		// held by a foot or a hand the body whirls round it, so it is taken by the collar.
		float best = 1e9f, dist;
		int k;
		rd->pinned = -1;
		if ( cent->currentState.dl_intensity - 1 == cg.snap->ps.clientNum && !cgvr.active &&
			 rd->hb.hasPoint[P_NECK] ) {
			rd->pinned = P_NECK;
		} else {
			for ( k = 0; k < P_COUNT; k++ ) {
				if ( rd->hb.hasPoint[k] && ( dist = Distance( rd->hb.p[k], grabPoint ) ) < best ) {
					best = dist;
					rd->pinned = k;
				}
			}
		}
		if ( rd->hb.phys && rd->pinned >= 0 ) {
			HR_Grab( &rd->hb, rd->pinned );
		}
	} else if ( !grabbed && rd->grabber ) {
		// let go: the body keeps the hand's motion, and stays with the server's body
		rd->pinned = -1;
		HR_Release( &rd->hb, cent->lerpOrigin );
	}
	rd->grabber = grabbed ? cent->currentState.dl_intensity : 0;
	HR_Settle( &rd->hb, grabbed, cent->lerpOrigin );     // (alive, or still fighting: never settled)

	if ( rd->hb.phys ) {
		// Jolt steps every body at once (CG_Physics_Frame); here only the hand and the tether,
		// once a frame however many views draw it
		if ( rd->simTime != cg.time ) {
			rd->simTime = cg.time;
			gore = CG_VRX_GoreBits( cent );
			if ( HR_Frame( &rd->hb, cent->lerpOrigin, grabbed, grabPoint,
						   ( gore & VRX_GB_LARM ? HR_LIMB_LARM : 0 ) | ( gore & VRX_GB_RARM ? HR_LIMB_RARM : 0 ) |
						   ( gore & VRX_GB_LLEG ? HR_LIMB_LLEG : 0 ) | ( gore & VRX_GB_RLEG ? HR_LIMB_RLEG : 0 ),
						   ( gore & VRX_GB_HEAD ) != 0 ) ) {
				RD_PhysPose( rd );      // (the bones from where HitReact has the parts)
			}
			if ( rd->hb.getUpStart && rd->guAnim ) {
				RD_GuAnimFrame( rd, body );     // (drawn from the game's get-up animation)
			}
			if ( rd->hb.recoverStart ) {
				float a = ( cg.time - rd->hb.recoverStart ) / (float)RD_RECOVER_MS;
				a = a > 1 ? 1 : a;
				RD_Blend( rd, body, a * a * ( 3 - 2 * a ), NULL, NULL );
			} else if ( rd->hb.getUpStart && rd->guAnim ) {
				// (RD_GuAnimFrame drew him)
			} else if ( rd->hb.getUpStart ) {
				// the physics gets him up (RD_GetUpFrame); only the end is eased into his animation
				float a = ( cg.time - rd->hb.getUpStart ) / (float)RD_GETUP_MS;
				a = ( a - RD_GETUP_BLEND ) / ( 1.0f - RD_GETUP_BLEND );
				a = a > 1 ? 1 : a < 0 ? 0 : a;
				RD_Blend( rd, body, a * a * ( 3 - 2 * a ), NULL, NULL );
			}
		}
	} else {
		// fixed steps (the same on every machine); one simulation per frame however many
		// views draw it (both eyes and the HUD layer in VR)
		if ( rd->simTime > cg.time || cg.time - rd->simTime > 1000 ) {
			rd->simTime = cg.time;      // time went backwards (map restart) or a long pause
		}
		while ( rd->simTime + RD_STEP_MS <= cg.time ) {
			rd->simTime += RD_STEP_MS;
			if ( !rd->hb.asleep && steps < RD_MAX_STEPS ) {
				RD_Step( rd, cent, grabPoint, grabbed );
				steps++;
			}
		}
		if ( steps ) {
			RD_Pose( rd );
		}
	}

	rdCent = NULL;
	rdBody = NULL;
	body->ragdollBones = rd->blended ? rd->blend : rd->out;
	body->ragdollMagic = RAGDOLL_MAGIC;
	VectorCopy( rd->outOrigin, body->origin );
	VectorCopy( rd->outOrigin, body->oldorigin );
	VectorCopy( rd->outOrigin, body->lightingOrigin );
	AxisClear( body->axis );
	AxisClear( body->torsoAxis );
	}
}

/*
=================
CG_Ragdoll_On

the dead go limp instead of playing their death animations (vr_ragdolls)
=================
*/
qboolean CG_Ragdoll_On( void ) {
	trap_Cvar_Update( &vr_ragdolls );
	return vr_ragdolls.integer != 0;
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
			   rd->entityNum, Distance( headOrigin, rd->hb.p[P_HEAD] ), rd->hb.p[P_PELVIS][0], rd->hb.p[P_PELVIS][1],
			   rd->hb.p[P_PELVIS][2], rd->hb.p[P_HEAD][0], rd->hb.p[P_HEAD][1], rd->hb.p[P_HEAD][2], rd->hb.asleep ? " (at rest)" : "" );
}
