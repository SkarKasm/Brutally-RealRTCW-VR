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

// cg_physworld.c -- the level as Jolt sees it (vr_physics 1).
//
// On the first frame with vr_physics on, the map's .bsp is read again (the client's collision
// map isn't reachable from here) and turned into physics shapes: every solid or player-clip
// brush of the world becomes a convex hull, every solid curve a triangle mesh. Brush entities
// (doors, lifts, crates, script movers) get kinematic bodies built from their own brushes,
// placed where the game draws them each frame and taken out when the entity is gone.
//
// The physics world is stepped here, once a frame, in the same fixed steps as the old
// ragdolls (cg_ragdoll.c), before any body is drawn.

#include "cg_local.h"
#include "../qcommon/qfiles.h"     // the .bsp layout
#include "cg_physics.h"

#define PW_MASK             ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP )   // what the old ragdolls collided with
#define PW_MAX_HULL_POINTS  256
#define PW_PATCH_STEPS      4           // each 3x3 piece of a curve is cut into 4x4 quads
#define PW_STEP_MS          16
#define PW_MAX_STEPS        6
#define PW_MAX_MODELS       MAX_MODELS

static vmCvar_t vr_physics;
static vmCvar_t vr_ragdollGravityPW;

static qboolean pwBuilt;            // tried to build this map (it may have failed)
static qboolean pwReady;            // physics running with the level in it
static int      pwSimTime;
static int      pwNumModels;
static int      pwMover[PW_MAX_MODELS];         // inline model -> mover, 0 none
static int      pwMoverSeen[PW_MAX_MODELS];     // cg.snap->serverTime it was last in a snapshot
static qboolean pwMoverPlaced[PW_MAX_MODELS];

static void PW_Print( const char *msg ) {
	CG_Printf( "%s", msg );
}

/*
==============================================================================

Reading the .bsp

==============================================================================
*/

typedef struct {
	const byte          *base;
	const dshader_t     *shaders;
	int                 numShaders;
	const dplane_t      *planes;
	int                 numPlanes;
	const dmodel_t      *models;
	int                 numModels;
	const dbrush_t      *brushes;
	int                 numBrushes;
	const dbrushside_t  *sides;
	int                 numSides;
	const drawVert_t    *verts;
	int                 numVerts;
	const dsurface_t    *surfaces;
	int                 numSurfaces;
} pwBsp_t;

static const void *PW_Lump( const byte *buf, int len, const dheader_t *h, int lump, int size, int *count ) {
	int ofs = LittleLong( h->lumps[lump].fileofs ), l = LittleLong( h->lumps[lump].filelen );

	if ( ofs < 0 || l < 0 || ofs + l > len || l % size ) {
		*count = 0;
		return NULL;
	}
	*count = l / size;
	return buf + ofs;
}

// the corners of a brush: where three of its planes meet, inside all the others
static int PW_BrushPoints( const pwBsp_t *bsp, const dbrush_t *b, float *out ) {
	int first = LittleLong( b->firstSide ), num = LittleLong( b->numSides );
	int i, j, k, s, n = 0;

	if ( first < 0 || num < 4 || first + num > bsp->numSides ) {
		return 0;
	}
	for ( i = 0; i < num; i++ ) {
		const dplane_t *pi = &bsp->planes[LittleLong( bsp->sides[first + i].planeNum )];
		for ( j = i + 1; j < num; j++ ) {
			const dplane_t *pj = &bsp->planes[LittleLong( bsp->sides[first + j].planeNum )];
			vec3_t jk, ki, ij;
			for ( k = j + 1; k < num; k++ ) {
				const dplane_t *pk = &bsp->planes[LittleLong( bsp->sides[first + k].planeNum )];
				vec3_t p;
				float det;

				CrossProduct( pj->normal, pk->normal, jk );
				det = DotProduct( pi->normal, jk );
				if ( fabs( det ) < 1e-6f ) {
					continue;
				}
				CrossProduct( pk->normal, pi->normal, ki );
				CrossProduct( pi->normal, pj->normal, ij );
				VectorScale( jk, pi->dist, p );
				VectorMA( p, pj->dist, ki, p );
				VectorMA( p, pk->dist, ij, p );
				VectorScale( p, 1.0f / det, p );

				for ( s = 0; s < num; s++ ) {
					const dplane_t *ps = &bsp->planes[LittleLong( bsp->sides[first + s].planeNum )];
					if ( DotProduct( ps->normal, p ) - ps->dist > 0.05f ) {
						break;
					}
				}
				if ( s < num ) {
					continue;
				}
				for ( s = 0; s < n; s++ ) {
					if ( Distance( out + s * 3, p ) < 0.1f ) {
						break;
					}
				}
				if ( s < n || n >= PW_MAX_HULL_POINTS ) {
					continue;
				}
				VectorCopy( p, out + n * 3 );
				n++;
			}
		}
	}
	return n;
}

static qboolean PW_BrushSolid( const pwBsp_t *bsp, const dbrush_t *b ) {
	int s = LittleLong( b->shaderNum ), i;

	if ( s < 0 || s >= bsp->numShaders ) {
		return qfalse;
	}
	if ( !( LittleLong( bsp->shaders[s].contentFlags ) & PW_MASK ) ) {
		return qfalse;
	}
	// every plane must be a real one
	for ( i = 0; i < LittleLong( b->numSides ); i++ ) {
		int side = LittleLong( b->firstSide ) + i, plane;
		if ( side < 0 || side >= bsp->numSides ) {
			return qfalse;
		}
		plane = LittleLong( bsp->sides[side].planeNum );
		if ( plane < 0 || plane >= bsp->numPlanes ) {
			return qfalse;
		}
	}
	return qtrue;
}

// a point on a 3x3 biquadratic Bezier piece
static void PW_PatchPoint( const drawVert_t *v, int width, int x, int y, float s, float t, vec3_t out ) {
	float bs[3] = { ( 1 - s ) * ( 1 - s ), 2 * s * ( 1 - s ), s * s };
	float bt[3] = { ( 1 - t ) * ( 1 - t ), 2 * t * ( 1 - t ), t * t };
	int i, j;

	VectorClear( out );
	for ( j = 0; j < 3; j++ ) {
		for ( i = 0; i < 3; i++ ) {
			VectorMA( out, bs[i] * bt[j], v[( y + j ) * width + x + i].xyz, out );
		}
	}
}

static void PW_AddPatch( const pwBsp_t *bsp, const dsurface_t *surf ) {
	int width = LittleLong( surf->patchWidth ), height = LittleLong( surf->patchHeight );
	int first = LittleLong( surf->firstVert ), shader = LittleLong( surf->shaderNum );
	const drawVert_t *v;
	int x, y, i, j;

	if ( shader < 0 || shader >= bsp->numShaders || width < 3 || height < 3 || !( width & 1 ) || !( height & 1 ) ||
		 first < 0 || first + width * height > bsp->numVerts ) {
		return;
	}
	if ( !( LittleLong( bsp->shaders[shader].contentFlags ) & PW_MASK ) ||
		 ( LittleLong( bsp->shaders[shader].surfaceFlags ) & SURF_NONSOLID ) ) {
		return;
	}
	v = bsp->verts + first;
	for ( y = 0; y + 2 < height; y += 2 ) {
		for ( x = 0; x + 2 < width; x += 2 ) {
			vec3_t grid[PW_PATCH_STEPS + 1][PW_PATCH_STEPS + 1];
			for ( j = 0; j <= PW_PATCH_STEPS; j++ ) {
				for ( i = 0; i <= PW_PATCH_STEPS; i++ ) {
					PW_PatchPoint( v, width, x, y, (float)i / PW_PATCH_STEPS, (float)j / PW_PATCH_STEPS, grid[j][i] );
				}
			}
			// both windings: a curve stops bodies from either side
			for ( j = 0; j < PW_PATCH_STEPS; j++ ) {
				for ( i = 0; i < PW_PATCH_STEPS; i++ ) {
					Phys_AddWorldTriangle( grid[j][i], grid[j][i + 1], grid[j + 1][i + 1] );
					Phys_AddWorldTriangle( grid[j][i], grid[j + 1][i + 1], grid[j][i + 1] );
					Phys_AddWorldTriangle( grid[j][i], grid[j + 1][i + 1], grid[j + 1][i] );
					Phys_AddWorldTriangle( grid[j][i], grid[j + 1][i], grid[j + 1][i + 1] );
				}
			}
		}
	}
}

static qboolean PW_Build( void ) {
	static float points[PW_MAX_HULL_POINTS * 3];
	fileHandle_t f;
	const dheader_t *h;
	pwBsp_t bsp;
	byte *buf;
	int len, i, m, start = trap_Milliseconds(), worldBrushes = 0, movers = 0;

	len = trap_FS_FOpenFile( cgs.mapname, &f, FS_READ );
	if ( len <= (int)sizeof( dheader_t ) ) {
		if ( len >= 0 ) {
			trap_FS_FCloseFile( f );
		}
		CG_Printf( "physics: can't read %s\n", cgs.mapname );
		return qfalse;
	}
	buf = malloc( len );
	if ( !buf ) {
		trap_FS_FCloseFile( f );
		return qfalse;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );

	h = (const dheader_t *)buf;
	if ( LittleLong( h->version ) != BSP_VERSION ) {
		CG_Printf( "physics: %s is not a version %d map\n", cgs.mapname, BSP_VERSION );
		free( buf );
		return qfalse;
	}
	memset( &bsp, 0, sizeof( bsp ) );
	bsp.base = buf;
	bsp.shaders = PW_Lump( buf, len, h, LUMP_SHADERS, sizeof( dshader_t ), &bsp.numShaders );
	bsp.planes = PW_Lump( buf, len, h, LUMP_PLANES, sizeof( dplane_t ), &bsp.numPlanes );
	bsp.models = PW_Lump( buf, len, h, LUMP_MODELS, sizeof( dmodel_t ), &bsp.numModels );
	bsp.brushes = PW_Lump( buf, len, h, LUMP_BRUSHES, sizeof( dbrush_t ), &bsp.numBrushes );
	bsp.sides = PW_Lump( buf, len, h, LUMP_BRUSHSIDES, sizeof( dbrushside_t ), &bsp.numSides );
	bsp.verts = PW_Lump( buf, len, h, LUMP_DRAWVERTS, sizeof( drawVert_t ), &bsp.numVerts );
	bsp.surfaces = PW_Lump( buf, len, h, LUMP_SURFACES, sizeof( dsurface_t ), &bsp.numSurfaces );
	if ( !bsp.numModels || !bsp.numBrushes || !bsp.numPlanes ) {
		free( buf );
		return qfalse;
	}

	Phys_BeginWorld();
	memset( pwMover, 0, sizeof( pwMover ) );
	memset( pwMoverSeen, 0, sizeof( pwMoverSeen ) );
	memset( pwMoverPlaced, 0, sizeof( pwMoverPlaced ) );
	pwNumModels = bsp.numModels < PW_MAX_MODELS ? bsp.numModels : PW_MAX_MODELS;

	for ( m = 0; m < pwNumModels; m++ ) {
		const dmodel_t *model = &bsp.models[m];
		int first = LittleLong( model->firstBrush ), num = LittleLong( model->numBrushes );
		int mover = 0;

		if ( first < 0 || num < 0 || first + num > bsp.numBrushes ) {
			continue;
		}
		if ( m > 0 ) {
			mover = Phys_CreateMover();
			if ( !mover ) {
				continue;
			}
		}
		for ( i = first; i < first + num; i++ ) {
			int n;
			if ( !PW_BrushSolid( &bsp, &bsp.brushes[i] ) ) {
				continue;
			}
			n = PW_BrushPoints( &bsp, &bsp.brushes[i], points );
			if ( n < 4 ) {
				continue;
			}
			if ( m == 0 ) {
				Phys_AddWorldHull( points, n );
				worldBrushes++;
			} else {
				Phys_MoverAddHull( mover, points, n );
			}
		}
		if ( m == 0 ) {
			int s0 = LittleLong( model->firstSurface ), ns = LittleLong( model->numSurfaces );
			for ( i = s0; i >= 0 && i < s0 + ns && i < bsp.numSurfaces; i++ ) {
				if ( LittleLong( bsp.surfaces[i].surfaceType ) == MST_PATCH ) {
					PW_AddPatch( &bsp, &bsp.surfaces[i] );
				}
			}
		} else if ( Phys_MoverFinish( mover ) ) {
			pwMover[m] = mover;
			movers++;
		}
	}
	Phys_EndWorld();
	free( buf );

	{
		int bodies, active, hulls, tris;
		Phys_Stats( &bodies, &active, &hulls, &tris );
		CG_Printf( "physics: %s: %d of %d world brushes as hulls, %d curve triangles, %d brush entities, %d ms\n",
				   cgs.mapname, hulls, worldBrushes, tris, movers, trap_Milliseconds() - start );
	}
	return qtrue;
}

/*
==============================================================================

Each frame

==============================================================================
*/

// brush entities: where the game draws them now
static void PW_Movers( void ) {
	int i;

	if ( !cg.snap ) {
		return;
	}
	for ( i = 0; i < cg.snap->numEntities; i++ ) {
		const entityState_t *es = &cg.snap->entities[i];
		centity_t *cent = &cg_entities[es->number];
		int model = es->modelindex;
		vec3_t axis[3];
		qboolean teleport;

		if ( es->solid != SOLID_BMODEL || model <= 0 || model >= pwNumModels || !pwMover[model] ) {
			continue;
		}
		AnglesToAxis( cent->lerpAngles, axis );
		teleport = !pwMoverPlaced[model] || pwMoverSeen[model] < cg.snap->serverTime - 1000;
		Phys_MoverPlace( pwMover[model], cent->lerpOrigin, axis[0], teleport );
		pwMoverPlaced[model] = qtrue;
		pwMoverSeen[model] = cg.snap->serverTime;
	}
	// gone from the game (a crate blown apart, a door removed by a script)
	for ( i = 1; i < pwNumModels; i++ ) {
		if ( pwMoverPlaced[i] && pwMoverSeen[i] != cg.snap->serverTime ) {
			Phys_MoverHide( pwMover[i] );
			pwMoverPlaced[i] = qfalse;
		}
	}
}

/*
=================
CG_Physics_Ready

The ragdolls use Jolt when this is true.
=================
*/
qboolean CG_Physics_Ready( void ) {
	return pwReady;
}

/*
=================
CG_Physics_Frame

Called once each time the scene is drawn, before the entities are added.
=================
*/
void CG_Physics_Frame( void ) {
	int steps = 0;

	trap_Cvar_Update( &vr_physics );
	trap_Cvar_Update( &vr_ragdollGravityPW );

	if ( !vr_physics.integer ) {
		if ( pwBuilt ) {
			CG_Ragdoll_PhysicsOff();    // back to the old ragdolls
			Phys_Shutdown();
			pwBuilt = qfalse;
			pwReady = qfalse;
		}
		return;
	}
	if ( !pwBuilt ) {
		pwBuilt = qtrue;
		pwReady = Phys_Init( PW_Print ) && PW_Build();
		if ( !pwReady ) {
			CG_Printf( "physics: Jolt isn't available, the old ragdolls are used\n" );
		}
		pwSimTime = cg.time;
	}
	if ( !pwReady ) {
		return;
	}

	PW_Movers();
	if ( pwSimTime > cg.time || cg.time - pwSimTime > 1000 ) {
		pwSimTime = cg.time;    // time went backwards (map restart) or a long pause
	}
	while ( pwSimTime + PW_STEP_MS <= cg.time ) {
		pwSimTime += PW_STEP_MS;
		if ( steps < PW_MAX_STEPS ) {
			Phys_Step( PW_STEP_MS * 0.001f, vr_ragdollGravityPW.value );
			steps++;
		}
	}
	if ( vr_physics.integer > 1 && steps ) {      // 2: a line a second (testing)
		static int next;
		if ( cg.time >= next ) {
			int bodies, active, hulls, tris;
			next = cg.time + 1000;
			Phys_Stats( &bodies, &active, &hulls, &tris );
			CG_Printf( "physics: %d bodies, %d moving\n", bodies, active );
		}
	}
}

void CG_Physics_Init( void ) {
	trap_Cvar_Register( &vr_physics, "vr_physics", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_ragdollGravityPW, "vr_ragdollGravity", "400", CVAR_ARCHIVE );
	pwBuilt = qfalse;
	pwReady = qfalse;
}

void CG_Physics_Shutdown( void ) {
	Phys_Shutdown();
	pwBuilt = qfalse;
	pwReady = qfalse;
}
