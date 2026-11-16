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

// cg_gore.h -- voxel gore: the C side's view of cg_gore.cpp (gore/gorecore.cpp underneath)
//
// Everything is in a model's own space and units. Poses are 12 floats a bone, the way the
// renderer takes them (refEntity_t.ragdollBones): a 3x3 matrix by rows, then the translation.
// Handles are small integers, 0 means none.

#ifndef CG_GORE_H
#define CG_GORE_H

#include "../qcommon/vr_gore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int     cells;          // voxels it took out (0: it missed the body)
	int     boneCells;      // of them bone
	int     exited;         // it came out the other side
	float   length;         // how far it went through the body
	float   entry[3], exit[3];      // where it went in and stopped / came out (the pose it was shot in)
	int     bone;                   // the bone it was taken in through
	float   restEntry[3], restExit[3], restDir[3];     // the same in the rest pose (Gore_RestToPose)
} goreShot_t;

void        Gore_Shutdown( void );      // frees everything (a new level)
void        Gore_NewFrame( void );      // once a frame: wounds are worked out again one body a frame

// a skeletal model: its .mds file, the pose its voxel body is made in, its bones' parents,
// flags (1: a tag) and names (32 chars each). cell: voxel size in model units.
int         Gore_LoadModel( const void *mds, int size, const float *restPose, int numBones,
							const int *parents, const int *flags, const char *names, float cell );

// a head (.md3 / .mdc): rigid, one bone (pose: 12 floats of the identity), a skull in it
int         Gore_LoadHead( const void *data, int size, float cell );
void        Gore_ModelCentre( int model, float out[3] );

int         Gore_NewBody( int model );      // a fresh copy of the model's voxel body
void        Gore_FreeBody( int body );
int         Gore_BodyModel( int body );
int         Gore_BodiesInUse( void );

// a bullet through body at point along dir (both in the pose it was drawn in)
int         Gore_Shot( int body, const float *pose, const float point[3], const float dir[3],
					   float radiusIn, float radiusOut, float energy, goreShot_t *out );

// the parts shot clean off the body: each becomes a body of its own (written to pieces, with
// its middle and its main bone in the rest pose), and is taken off this one. Returns how many.
int         Gore_Sever( int body, int *pieces, float centres[][3], int *bones, int max );

// a limb cut clean through between two bones' joints (t: how far from the first), all of it
// within radius of that line (units): a blast or a heavy round taking it off. Cells cut.
int         Gore_Chop( int body, int from, int to, float t, float radius );

// the bone a point of the pose is nearest (the one Gore_Shot / Gore_Slice would take it in through)
int         Gore_NearBone( int body, const float *pose, const float point[3] );

// a blade into body at point along dir (the pose it was drawn in). limbRadius 0: a slit where it
// went in, reach its half length (units), turned by roll. limbRadius over 0: a cut across the limb
// there, as far as reach from where the blade met it; blows close by carry on in the same cut, and
// *through is set once nothing holds the limb on (Gore_Sever then takes it off). Cells cut.
int         Gore_Slice( int body, const float *pose, const float point[3], const float dir[3],
						float limbRadius, float reach, float roll, goreShot_t *out, int *through );

// where a point of the rest pose is in pose (the bone it moves with)
void        Gore_RestToPose( int body, const float *pose, int bone, const float in[3], float out[3] );

// the bone a point of the rest pose moves with (the limb it's nearest)
int         Gore_RestBone( int body, const float in[3] );

// the skin nearest a point of the rest pose, of what faces hint's way (their dot over minDot):
// where it is and which way it faces there. How far off it was (units), under 0 if there's none
// (anchor, if given: that spot of the skin itself, to follow it as it bends, Gore_AnchorPose)
typedef struct {
	int     surf, tri;      // surf under 0: none
	float   bary[3];
} goreAnchor_t;
float       Gore_RestSurface( int body, const float in[3], const float hint[3], float minDot,
							  float out[3], float normal[3], goreAnchor_t *anchor );

// where an anchored spot of the skin is in pose, which way the skin faces there, and where a
// direction along the skin in the rest pose (restDir) points now. 0 if it can't be told
// (a rigid head, no anchor): place it by its bone then.
int         Gore_AnchorPose( int body, const float *pose, const goreAnchor_t *anchor, const float restDir[3],
							 float at[3], float normal[3], float dir[3] );

// the body's holes and flesh to draw it in pose; light: ambient and directed colour (0..255)
// and the direction it comes from, in the model's space. Valid until the next call for it.
const vrGoreDraw_t *Gore_Draw( int body, const float *pose, const float ambient[3],
							   const float directed[3], const float lightDir[3] );

#ifdef __cplusplus
}
#endif

#endif
