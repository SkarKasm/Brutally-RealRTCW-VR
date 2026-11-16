/*
===========================================================================

HitReact -- active ragdoll hit reactions on Jolt Physics
Copyright (C) 2026 RealRTCW-VR contributors

This file is part of HitReact, which grew out of RealRTCW-VR's ragdolls.

HitReact is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. Jolt Physics (code/JoltPhysics) is MIT licensed.

===========================================================================
*/

// hitreact.h -- the whole library, as a C API any engine can call.
//
// A character's skeleton goes in as 21 points (HR_P_*), and comes out as 15 rigid parts
// (HR_S_* segments) simulated by Jolt: a box torso and chest, capsules for the head and
// limbs, swing-twist joints and hinged elbows and knees. The joints have motors (muscles)
// that chase a pose, and a reaction brain decides the pose every frame:
//
//   shot and stunned, alive:  staggers under the bullets, steps to catch himself (a capture
//                             point balancer), clutches the wound, flails when he's going
//                             over, and comes to on his feet when he's steady again;
//   knocked down, alive:      staggers, goes over, breaks the fall, lies there hurting, and
//                             when the game stands him up, gets up as physics through a
//                             series of poses (or the game's own get-up animation);
//   killed:                   fights for his feet for a few seconds by where he was shot
//                             (doubled over a gut wound, reeling from a chest shot, hopping
//                             on a shot leg), gives out, throws his arms out to break the
//                             fall, writhes on the floor with gasps that fade, goes limp;
//   headshot or blast:        limp at once (a blast throws him first).
//
// The host (the game) feeds the level's geometry, the time, hits, and what its own logic says
// about the character (alive or dead, stunned, knocked down); it gets back each part's
// position and rotation to skin the bones from, and events for its game logic (he fell over,
// his body has staggered to here, he faces this way getting up).
//
// Spaces. Inside the library, Z is up and 1 unit is 1 inch (0.0254 m), as in Quake-engine
// games, and every tuning number is in those units. HR_SetHostSpace says how the host's
// units and up axis relate, and the host-space functions below convert. The HRP_* ragdoll
// functions and the fields of hrBody_t are in library space. Matrices are 9 floats, three
// columns (axis k is m[k*3+0..2]).
//
// Threads: none. Call everything from one thread. Results are the same on every run of the
// same build for the same calls (single threaded Jolt, fixed steps); builds for different C
// runtimes differ slightly (rand(), sinf()), so give hrHost_t.random a generator of your own
// if two platforms must agree.

#ifndef HITREACT_H
#define HITREACT_H

#ifdef __cplusplus
extern "C" {
#endif

#define HR_VERSION          1

/*
==============================================================================

Host space

==============================================================================
*/

// hostUnitsPerInch: 1 for Quake-engine games (SoF2, RTCW, Jedi Academy), 0.0254 for metres
// (Renegade, most physics engines), 2.54 for centimetres. yUp: 0 Z up, 1 Y up (right-handed:
// host x, y, z = library x, z, -y).
void        HR_SetHostSpace( float hostUnitsPerInch, int yUp );
int         HR_HostIsLib( void );           // 1 when host space is library space (no conversion)
void        HR_PointToLib( const float *in, float *out );     // positions, velocities
void        HR_PointToHost( const float *in, float *out );
void        HR_DirToLib( const float *in, float *out );       // unit directions
void        HR_DirToHost( const float *in, float *out );
void        HR_AxisToLib( const float *in, float *out );      // rotations (9 floats)
void        HR_AxisToHost( const float *in, float *out );
float       HR_LenToLib( float len );
float       HR_LenToHost( float len );

/*
==============================================================================

The Jolt layer (HRP_*): the world, movers, and raw ragdolls

==============================================================================
*/

#define HRP_MAX_PARTS       16

typedef enum {
	HRP_SHAPE_CAPSULE,      // from a to b, radius
	HRP_SHAPE_BOX           // centre (a + b) / 2, axes boxAxis, half sizes boxHalf
} hrpShape_t;

typedef enum {
	HRP_JOINT_NONE,         // the root
	HRP_JOINT_SWINGTWIST,   // a cone round twist1 (the parent's), with twist about the limb
	HRP_JOINT_HINGE         // bends about hingeAxis from hingeMin to hingeMax degrees
} hrpJoint_t;

typedef struct {
	int         parent;         // part it hangs from, -1 for the root
	int         shape;          // hrpShape_t
	float       a[3], b[3];
	float       radius;
	float       boxAxis[9];
	float       boxHalf[3];
	float       mass;           // kg
	float       torqueScale;    // its joint's drive torque times this (0: 1)

	int         joint;          // hrpJoint_t
	float       pivot[3];
	float       twist1[3];      // swing twist: the middle of the allowed swing, on the parent
	float       twist2[3];      //  the limb's direction now
	float       plane[3];       //  square to twist1: swinging about it uses planeHalf
	float       planeHalf, normalHalf, twistMin, twistMax;     // degrees
	float       hingeAxis[3];
	float       hingeMin, hingeMax;     // degrees, 0 = the bend it has now

	float       vel[3];         // starting velocity, units/s
	float       toneMs;         // the joint holds the starting pose, fading over this long (0 = limp)
} hrpPart_t;

int         HRP_Init( void ( *print )( const char *msg ) );     // 0 if the physics couldn't start
void        HRP_Shutdown( void );
int         HRP_Active( void );

// the level, in host space: convex hulls (brushes) and triangles (meshes), built once per map
void        HRP_BeginWorld( void );
void        HRP_AddWorldHull( const float *points, int numPoints );
void        HRP_AddWorldTriangle( const float *a, const float *b, const float *c );
void        HRP_EndWorld( void );

// doors, lifts, crates, in host space: hulls in the model's own space, moved by the game
int         HRP_CreateMover( void );
void        HRP_MoverAddHull( int mover, const float *points, int numPoints );
int         HRP_MoverFinish( int mover );      // 0 if it has no usable hulls
void        HRP_MoverPlace( int mover, const float *origin, const float *axis, int teleport );
void        HRP_MoverHide( int mover );

// ragdolls (library space; hrBody_t below makes and drives these for a humanoid)
int         HRP_CreateRagdoll( const hrpPart_t *parts, int numParts, const unsigned char *noCollide );
void        HRP_DestroyRagdoll( int rd );
void        HRP_RagdollPose( int rd, float *pos, float *axis );    // per part: pos[3] and axis[9]
void        HRP_RagdollMove( int rd, const float *delta );         // everything moved, velocities kept
void        HRP_RagdollSetPose( int rd, const float *pos, const float *axis );     // every part put there, at rest
void        HRP_RagdollAddVelocity( int rd, int part, const float *vel );     // part -1: all of it
void        HRP_RagdollAddVelocityAt( int rd, int part, const float *vel, const float *point );
int         HRP_RagdollAwake( int rd );
void        HRP_RagdollSleep( int rd );
void        HRP_RagdollWake( int rd );
void        HRP_RagdollGrab( int rd, int part, const float *point );  // held at point by a hand
void        HRP_RagdollGrabMove( int rd, const float *target, float maxSpeed );
void        HRP_RagdollRelease( int rd );
void        HRP_RagdollDrive( int rd, const float *axis, const float *strength, float blend );  // the joints chase a pose
void        HRP_RagdollSupport( int rd, float upright, float lift, float maxDrift, const float *up );     // balance help while he fights to stay up
void        HRP_RagdollScaleVelocity( int rd, float k );
void        HRP_RagdollGravity( int rd, float k );         // gravity (and drag) on it times k
void        HRP_RagdollLegPush( int rd, int footL, int footR, float shareL, float shareR );     // planted legs carry his weight
void        HRP_RagdollPin( int rd, const float *pos, const float *axis, float omega );   // torso sprung to a pose, weight carried
void        HRP_RagdollTone( int rd, const float *hz, const float *torque );     // per part drive stiffness (0 or NULL: by depth)
void        HRP_RagdollCatch( int rd, float z, float slack, float tiltDeg );     // a net under a staggering man: free within slack/tilt

void        HRP_Step( float seconds, float gravity );      // gravity in host units/s^2
int         HRP_RayCast( const float *start, const float *end, float *fraction, float *endpos );   // the level and movers only, host space (1: started inside a solid)
void        HRP_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris );

/*
==============================================================================

The humanoid: one character's body and reactions

==============================================================================
*/

// the skeleton points the host gives (from its bones), in this order
typedef enum {
	HR_P_PELVIS, HR_P_CHEST, HR_P_NECK, HR_P_HEAD, HR_P_HEADEND,  // HEADEND: top of the skull
	HR_P_LSHOULDER, HR_P_LELBOW, HR_P_LWRIST, HR_P_LHANDEND,
	HR_P_RSHOULDER, HR_P_RELBOW, HR_P_RWRIST, HR_P_RHANDEND,
	HR_P_LHIP, HR_P_LKNEE, HR_P_LANKLE, HR_P_LTOE,
	HR_P_RHIP, HR_P_RKNEE, HR_P_RANKLE, HR_P_RTOE,
	HR_P_COUNT
} hrPoint_t;

// body segments: each of the host's bones turns with one of them (HR_PointSegment says
// which segment carries each point)
typedef enum {
	HR_S_TORSO, HR_S_CHEST, HR_S_HEAD,
	HR_S_LUPPER, HR_S_LFORE, HR_S_LHAND, HR_S_RUPPER, HR_S_RFORE, HR_S_RHAND,
	HR_S_LTHIGH, HR_S_LCALF, HR_S_LFOOT, HR_S_RTHIGH, HR_S_RCALF, HR_S_RFOOT,
	HR_S_COUNT
} hrSegment_t;

// limbs (the bits of hrFrame's limbsOff: a limb cut off pulls with nothing)
#define HR_LIMB_LARM        ( 1 << 1 )
#define HR_LIMB_RARM        ( 1 << 2 )
#define HR_LIMB_LLEG        ( 1 << 3 )
#define HR_LIMB_RLEG        ( 1 << 4 )

// the reaction's phase (hrBody_t.eu)
enum { HR_EU_NONE, HR_EU_STAGGER, HR_EU_FALL, HR_EU_LYING };
// where a shot went in
enum { HR_RG_NONE, HR_RG_GUT, HR_RG_CHEST, HR_RG_ARM, HR_RG_LEG };
// physics get-up stages (hrHost_t.getUpPose); HR_GU_ANIM: standing, his own animation
enum { HR_GU_KNEEL, HR_GU_PRESS, HR_GU_TUCK, HR_GU_SIT, HR_GU_ANIM = -1 };

// events for the host's game logic (hrHost_t.event)
enum {
	HR_EV_FELL = 1,         // stunned and shoved over: knock him down (and stand him up later)
	HR_EV_POSITION,         // args[0..2]: where his body has got to (host space): move his game body there
	HR_EV_HOLD,             // the stun's over but he's still reeling: hold his game body a moment longer
	HR_EV_GETUP_YAW         // args[0]: the yaw he gets up facing (degrees, library space), args[1..3]: that direction (host space)
};

typedef struct {
	void        ( *print )( const char *msg );     // debug lines, NULL: none
	// a line through the level (and other characters, but never this one): 1 if start is in
	// something solid; else fraction along it and where it stopped. Host space. NULL: through
	// the level HitReact was given (HRP_RayCast).
	int         ( *trace )( void *user, const float *start, const float *end, float *fraction, float *endpos );
	void        ( *event )( void *user, int event, const float *args );
	// the physics get-up's poses: for get-up stage (HR_GU_*), each part's target rotation
	// (library space, b->numParts * 9 floats, see HR_PartTargetFromBone) and where that pose
	// has the torso (HR_TorsoTargetFromBone). 0: no such pose. NULL: HitReact stands him in
	// the pose his body was made in.
	int         ( *getUpPose )( void *user, int stage, float *partTargets, float *torsoPos, float *torsoAxis );
	// the host gets him up with its own animation (return 1) instead of the physics; and that
	// animation broken off (killed or knocked down on the way up). Both may be NULL.
	int         ( *getUpAnimBegin )( void *user );
	void        ( *getUpAnimAbort )( void *user );
	float       ( *random )( void );                // 0..1, NULL: from rand()
} hrHost_t;

typedef struct {
	float       gravity;        // host units/s^2 (400 in Quake units is about real gravity)
	int         active;         // the muscles work at all (0: every body is a plain ragdoll)
	int         euphoria;       // the dying fight for their feet (0: limp at death)
	float       strength;       // the muscles' strength (1)
	int         maxActive;      // at most this many fight at once (12)
	int         debug;          // 1: a line for each state change through hrHost_t.print
} hrOptions_t;

typedef struct hrBody_s {
	// the host's
	void        *user;          // passed back to every callback
	int         id;             // the host's number for him (seeds his own writhe and flail)

	// the skeleton: set by HR_BodySetSkeleton (or the host, in library space) before HR_BodyBuild
	unsigned char hasPoint[HR_P_COUNT];
	float       p0[HR_P_COUNT][3];      // the points as the body was made
	float       p[HR_P_COUNT][3];       // and now (HR_BodyReadPose)
	unsigned char segValid[HR_S_COUNT];
	float       startVel[3], killPush[3];   // how he started moving, and a push away from his killer
	float       anchorOffset[3];        // pelvis relative to the host's origin for him
	float       lastOrigin[3];          // the host's origin at the last frame

	// coming to rest (HR_Settle)
	int         stillSteps;
	int         asleep;
	int         startTime;
	int         settleBy;
	int         settled;

	// the Jolt body
	int         phys;                   // HRP ragdoll, 0: none
	int         realW;
	int         numParts;
	int         segPart[HR_S_COUNT];    // the part each segment turns with
	float       part0Pos[HRP_MAX_PARTS][3];     // the parts as made
	float       part0Axis[HRP_MAX_PARTS][3][3];
	float       partPos[HRP_MAX_PARTS][3];      // and now
	float       partAxis[HRP_MAX_PARTS][3][3];
	float       partRot[HRP_MAX_PARTS][3][3];   // each part's turn from how it was made (skin the bones with it)
	int         partSeg[HRP_MAX_PARTS];
	float       segHinge0[HR_S_COUNT][3];       // an elbow or knee's hinge axis as made

	// the muscles
	int         active;
	int         downed;                 // alive and knocked down
	int         blasted;                // died in a blast: limp at once
	int         activeStart;
	int         partHitTime[HRP_MAX_PARTS];
	int         lastHitPart, lastHitTime;
	float       lastHitPoint[3];
	float       lastHitDir[3];
	float       target[HRP_MAX_PARTS * 9];

	// the reaction
	int         eu;                     // HR_EU_*
	int         euStart, euPhaseStart;
	float       euDur;                  // ms on his feet
	int         euRegion;               // HR_RG_*
	int         euHurtLimb;             // 1/2 left/right arm, 3/4 left/right leg
	int         euWoundPart;
	float       euWoundLocal[3];
	float       euPush[3];
	float       euFall[3];
	float       euStartPelvis[3];
	float       stunPos[3], stunAxis[3][3];
	float       euGait;
	float       euLyMs;
	float       armFlail;
	// the balancer
	int         balInit;
	int         balSwing;
	int         balSwingStart, balSwingDur, balLastStep;
	float       balFrom[3], balTo[3], balPlant[2][3], balFoot[3];
	float       balCom[3], balComVel[3];
	int         balComTime;
	float       balComH, balCpOut;
	int         balTiltSince, balOutSince;

	// alive
	int         living;
	int         stun;
	int         fellSent;
	int         downedAt;
	int         recoverStart;
	int         lastPosSent;
	int         stunOverAt;
	int         steadySince, lastStaggerSent;
	int         upAt;
	int         restSince;
	float       restPos[3];
	int         getUpStart;
	int         getUpPhase;
	int         getUpProne;
	int         getUpAnimated;          // the host's animation is getting him up
	float       getUpYaw, getUpSitYaw;
	float       getUpPos[3], getUpAxis[3][3];
	float       gravK;
	int         resetDrawn;             // set when the host should stop blending how it draws him (back to plain physics)
	int         registered;
	// fighting back while staggering (HR_Aim, HR_Recoil)
	int         aimOn;
	float       aimPoint[3];            // library space
	int         recoilTime;
} hrBody_t;

void        HR_SetHost( const hrHost_t *host );
void        HR_SetOptions( const hrOptions_t *options );
void        HR_SetTime( int ms );           // the game's clock, before each frame's calls
int         HR_NumActive( void );           // bodies whose muscles are working

// An hrBody_t must be all zeros before its first use (memset, or static storage): the calls
// below read it to free whatever it held before. Its memory must stay where it is, and be
// given back with HR_BodyFree before it is freed or reused: HitReact keeps a pointer to every
// built body. HRP_BeginWorld and HRP_Shutdown destroy every ragdoll: HR_BodyFree the bodies first.
//
// The skeleton, in host space: points[HR_P_COUNT][3] (hasPoint 0 for any it hasn't got;
// the pelvis, neck and shoulders are needed), vel: how he's moving, push: a shove away
// from whoever shot him (0s for none). segValid NULL: every segment whose points are there.
void        HR_BodySetSkeleton( hrBody_t *b, void *user, int id, const float *points, const unsigned char *hasPoint,
								const unsigned char *segValid, const float *vel, const float *push, const float *origin );
int         HR_BodyBuild( hrBody_t *b, int realWeights );   // the Jolt body: 0 if it couldn't be made
void        HR_BodyFree( hrBody_t *b );
void        HR_BodyReadPose( hrBody_t *b );                 // p, partPos, partAxis, partRot from Jolt
int         HR_PointSegment( int point );
int         HR_SegmentParent( int seg );

// hits (host space): a bullet at point with velocity vel (returns the part hit, -1 none),
// and a blast (strength: the speed it throws at its centre)
int         HR_BodyShot( hrBody_t *b, const float *point, const float *vel );
void        HR_BodyBlast( hrBody_t *b, const float *origin, float radius, float strength );

// what the game says happened (each starts a reaction)
// HR_Stun and HR_KnockDown make him a living body (b->living); for a blast death set
// b->blasted = 1 before HR_Die (limp at once, after the throw)
void        HR_Die( hrBody_t *b, int hitPart, const float *hitPoint );     // killed (hitPart -1: no shot, a fall or blast; hitPoint NULL: the middle of that part)
void        HR_Stun( hrBody_t *b );                 // shot and stunned (uses the last HR_BodyShot)
void        HR_KnockDown( hrBody_t *b );            // knocked down
void        HR_Rehit( hrBody_t *b );                // shot again while stunned
void        HR_Killed( hrBody_t *b );               // killed while alive in physics (stunned, down, getting up)
// staggering, he fights back: holds his gun on point (host space; NULL: stops, his arms go back
// to clutching and balancing), and each shot he fires kicks it back and up (HR_Recoil)
void        HR_Aim( hrBody_t *b, const float *point );
void        HR_Recoil( hrBody_t *b );

// Each frame, in this order (origin: the host's position for him, host space):
// HR_Living  while he's alive: the stun, the knockdown and the get-up, from what the game
//            says (hit: stunned now, downed: knocked down now). 1: he's himself again,
//            give him back his animation (HR_BodyFree).
// HR_Settle  coming to rest for good when he's dead and still
// HR_Frame   the muscles, the hand holding him, the tether to the host's origin; 1 if the
//            pose was read back (skin his bones from partRot / partPos)
int         HR_Living( hrBody_t *b, int hit, int downed );
void        HR_Settle( hrBody_t *b, int grabbed, const float *origin );
int         HR_Frame( hrBody_t *b, const float *origin, int grabbed, const float *grabPoint, int limbsOff, int headOff );     // (grabPoint: only read when grabbed)

// a hand picks him up by a point (HR_P_*) and lets go (host space)
void        HR_Grab( hrBody_t *b, int point );
void        HR_Release( hrBody_t *b, const float *origin );

// reading him in host space
void        HR_GetPoint( const hrBody_t *b, int point, float *out );
void        HR_GetPart( const hrBody_t *b, int part, float *pos, float *axis );
void        HR_GetPartRotation( const hrBody_t *b, int part, float *rot );     // from how he was made

// get-up poses from the host's bones (hrHost_t.getUpPose), host space: a bone's world axes in
// that pose and as the body was made
void        HR_PartTargetFromBone( const hrBody_t *b, int part, const float *boneAxis, const float *boneAxis0, float *out );
void        HR_TorsoTargetFromBone( const hrBody_t *b, const float *boneAxis, const float *bonePos,
									const float *boneAxis0, const float *bonePos0, float *torsoPos, float *torsoAxis );

// the steps of HR_Living and HR_Frame, for hosts that drive the states themselves
void        HR_ActiveFrame( hrBody_t *b, const float *origin, int grabbed, int limbsOff, int headOff );
void        HR_GetUpBegin( hrBody_t *b );
void        HR_GetUpFrame( hrBody_t *b, int limbsOff );
void        HR_BodyAnchor( hrBody_t *b, const float *origin );
int         HR_Steady( hrBody_t *b );
int         HR_AtRest( hrBody_t *b );

// 3x3 helpers (columns), shared with hosts that skin bones the same way
void        HR_MatMul( float a[3][3], float b[3][3], float out[3][3] );
void        HR_MatVec( float m[3][3], const float v[3], float out[3] );
void        HR_FrameRotation( float f[3][3], float f0[3][3], float out[3][3] );
void        HR_Align( const float u[3], const float v[3], float out[3][3] );
void        HR_AxisAngle( const float axis[3], float angle, float out[3][3] );
void        HR_RotLerp( float a[3][3], float b[3][3], float f, float out[3][3] );       // part of the way (f) from a to b
void        HR_TorsoFrame( float p[HR_P_COUNT][3], float f[3][3] );     // up, left, back
float       HR_SegmentDist( const float p1[3], const float q1[3], const float p2[3], const float q2[3], float *s, float *t );

#ifdef __cplusplus
}
#endif

#endif
