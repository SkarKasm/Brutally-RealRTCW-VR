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

// vr_gore.h -- voxel gore (cgame cg_gore.cpp / cg_vrgore.c), what the renderer draws of it
//
// A wounded body's skin has holes where its voxel body (gore/gorecore.cpp) was carved: the
// cgame works out, for each of the model's surfaces, which triangles stay whole and which
// pieces of the others (sub triangles, as barycentric corners of the triangle they come
// from) are left, and the renderer draws just those, deformed as usual (tr_animation.c
// R_VRG_Skin). The wound walls (the flesh) are a separate mesh the cgame poses itself, drawn
// for a second copy of the body with the shader "vr_goreflesh" (R_VRG_Flesh).
//
// It reaches the renderer the way the other per body extras do (tr_animation.c
// R_VRX_Trailer): after the pose in refEntity_t.ragdollBones (and a cut, if any) come
// VRG_MAGIC and the address of a vrGoreDraw_t in the cgame's memory, which stays valid until
// the frame is drawn.

#ifndef VR_GORE_H
#define VR_GORE_H

#define VRG_MAGIC           0x56524758      // "VRGX"
#define VRG_HEAD_MAGIC      0x56524748      // "VRGH": refEntity_t.ragdollMagic of a head (md3 / mdc)
#define VRG_TRAILER_FLOATS  3               // the magic and the address (two 32 bit halves)
#define VRG_MAX_SURFS       32              // MDS_MAX_SURFACES
#define VRG_SUB_FLOATS      10              // a sub triangle: its triangle, then 3 corners x 3 weights

typedef struct {
	int         numVerts, numTriangles;     // must match the surface drawn, else it's drawn whole
	int         numKeep;                    // < 0: the surface is untouched
	const int   *keep;                      // triangles drawn whole
	int         numSub;
	const float *sub;                       // VRG_SUB_FLOATS each
} vrGoreSurf_t;

typedef struct vrGoreDraw_s {
	int             numSurfaces;
	vrGoreSurf_t    surf[VRG_MAX_SURFS];

	// the wound walls, posed this frame (the model's space), coloured and lit
	int             numFleshVerts, numFleshIndexes;
	const float     *fleshXyz;              // 3 each
	const float     *fleshNormal;           // 3 each
	const unsigned char *fleshRGBA;         // 4 each
	const int       *fleshIndexes;
} vrGoreDraw_t;

#endif
