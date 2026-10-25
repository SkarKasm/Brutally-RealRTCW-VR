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

// cg_physics.h -- the C side of the Jolt Physics wrapper (cg_physics.cpp).
//
// Everything here is in game units (about an inch) and seconds; the wrapper turns that into
// metres for Jolt. Matrices are 9 floats, three columns (the same layout as vec3_t m[3] in
// cg_ragdoll.c). Handles are small integers, 0 means none.
//
// Without USE_JOLT the functions are stubs that report "no physics", and vr_physics 1 falls
// back to the old ragdolls.

#ifndef CG_PHYSICS_H
#define CG_PHYSICS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PHYS_MAX_PARTS      16

typedef enum {
	PHYS_SHAPE_CAPSULE,     // from a to b, radius
	PHYS_SHAPE_BOX          // centre (a + b) / 2, axes boxAxis, half sizes boxHalf
} physShape_t;

typedef enum {
	PHYS_JOINT_NONE,        // the root
	PHYS_JOINT_SWINGTWIST,  // a cone round twist1 (the parent's), with twist about the limb
	PHYS_JOINT_HINGE        // bends about hingeAxis from hingeMin to hingeMax degrees
} physJoint_t;

typedef struct {
	int         parent;         // part it hangs from, -1 for the root
	int         shape;          // physShape_t
	float       a[3], b[3];
	float       radius;
	float       boxAxis[9];
	float       boxHalf[3];
	float       mass;           // kg

	int         joint;          // physJoint_t
	float       pivot[3];
	float       twist1[3];      // swing twist: the middle of the allowed swing, on the parent
	float       twist2[3];      //  the limb's direction now
	float       plane[3];       //  square to twist1: swinging about it uses planeHalf
	float       planeHalf, normalHalf, twistMin, twistMax;     // degrees
	float       hingeAxis[3];
	float       hingeMin, hingeMax;     // degrees, 0 = the bend it has now

	float       vel[3];         // starting velocity, units/s
	float       toneMs;         // the joint holds the starting pose, fading over this long (0 = limp)
} physPart_t;

int         Phys_Init( void ( *print )( const char *msg ) );     // 0 if the physics couldn't start
void        Phys_Shutdown( void );
int         Phys_Active( void );

// the level: convex brushes and triangles (curves), in world space
void        Phys_BeginWorld( void );
void        Phys_AddWorldHull( const float *points, int numPoints );
void        Phys_AddWorldTriangle( const float *a, const float *b, const float *c );
void        Phys_EndWorld( void );

// doors, lifts, crates: a body made of hulls in the model's own space, moved by the game
int         Phys_CreateMover( void );
void        Phys_MoverAddHull( int mover, const float *points, int numPoints );
int         Phys_MoverFinish( int mover );      // 0 if it has no usable hulls
void        Phys_MoverPlace( int mover, const float *origin, const float *axis, int teleport );
void        Phys_MoverHide( int mover );

// ragdolls
int         Phys_CreateRagdoll( const physPart_t *parts, int numParts, const unsigned char *noCollide );
void        Phys_DestroyRagdoll( int rd );
void        Phys_RagdollPose( int rd, float *pos, float *axis );    // per part: pos[3] and axis[9]
void        Phys_RagdollMove( int rd, const float *delta );         // everything moved, velocities kept
void        Phys_RagdollAddVelocity( int rd, int part, const float *vel );     // part -1: all of it
void        Phys_RagdollAddVelocityAt( int rd, int part, const float *vel, const float *point );
int         Phys_RagdollAwake( int rd );
void        Phys_RagdollSleep( int rd );
void        Phys_RagdollWake( int rd );
void        Phys_RagdollGrab( int rd, int part, const float *point );  // held at point by a hand
void        Phys_RagdollGrabMove( int rd, const float *target, float maxSpeed );
void        Phys_RagdollRelease( int rd );

void        Phys_Step( float seconds, float gravity );      // gravity in units/s^2
void        Phys_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris );

#ifdef __cplusplus
}
#endif

#endif
