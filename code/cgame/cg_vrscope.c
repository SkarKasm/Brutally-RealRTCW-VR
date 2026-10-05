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

// cg_vrscope.c -- see-through scopes (RealRTCW-VR)
//
// A scope's eyepiece shows the world through it. Once a frame, on the first eye, the view
// from the scope, narrowed by its magnification, is rendered into the renderer's "*vrScope"
// texture (tr_vrscope.c). Both eyes then see that texture on a disc over the rear lens, with
// a reticle and the dark rim a real scope shows when the eye isn't right behind it.
// The lens itself is measured from the model by misc/vr/make_vr_assets.py (weapons.cfg).

#include "cg_local.h"

vmCvar_t vr_scopes;
vmCvar_t vr_scopeZoom;     // magnification multiplier

#define VS_RINGS    3
#define VS_SEGS     24

static struct {
	qhandle_t   shader;
	int         frame;          // cg.clientFrame the eyepiece was placed in
	int         weapon;
	vec3_t      centre;         // world, middle of the eyepiece
	vec3_t      axis[3];        // the scope's forward, left, up
	float       radius;
} vs;

void CG_VR_ScopeInit( void ) {
	memset( &vs, 0, sizeof( vs ) );
	vs.frame = -1;
	trap_Cvar_Register( &vr_scopes, "vr_scopes", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_scopeZoom, "vr_scopeZoom", "1", CVAR_ARCHIVE );
	vs.shader = trap_R_RegisterShaderNoMip( "*vrScope" );
}

static qboolean CG_VR_Snooper( int weapon ) {
	return weapon == WP_SNOOPERSCOPE || weapon == WP_GARAND;
}

// how much bigger the world looks through it
static float CG_VR_ScopePower( int weapon ) {
	trap_Cvar_Update( &vr_scopeZoom );
	return ( CG_VR_Snooper( weapon ) ? 3.0f : 4.0f ) * Com_Clamp( 0.1f, 4.0f, vr_scopeZoom.value );
}

/*
==============
CG_VR_ScopePiece

The first person gun (part 0) or its scope attachment (part 1) was just placed: remember
where the eyepiece is if it's on this piece.
==============
*/
void CG_VR_ScopePiece( int weapon, int part, const refEntity_t *ent ) {
	vrWeaponGeom_t g;
	int i;

	if ( !CG_VR_EyePass() ) {
		return;
	}
	trap_Cvar_Update( &vr_scopes );
	if ( !vr_scopes.integer || !vs.shader ) {
		return;
	}
	CG_VR_Geom( weapon, &g );
	if ( g.lensPart != part || g.lensRadius <= 0 ) {
		return;
	}

	VectorCopy( ent->origin, vs.centre );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( vs.centre, g.lens[i], ent->axis[i], vs.centre );
		VectorNormalize2( ent->axis[i], vs.axis[i] );
	}
	vs.radius = g.lensRadius;
	vs.weapon = weapon;
	vs.frame = cg.clientFrame;
}

/*
==============
CG_VR_ScopeView

The scene through the scope, into the "*vrScope" texture.
==============
*/
static void CG_VR_ScopeView( void ) {
	refdef_t rd;
	trace_t tr;
	vec3_t fwd, end, left;
	float dist, fov;
	int size;

	size = trap_R_VR_BeginScope();
	if ( size <= 0 ) {
		return;
	}

	rd = cg.refdef;
	rd.x = rd.y = 0;
	rd.width = rd.height = size;
	rd.rdflags = ( cg.refdef.rdflags & ~( RDF_VRVIEW | RDF_SKYBOXPORTAL | RDF_DRAWSKYBOX ) ) | RDF_VRSCOPE;
	if ( CG_VR_Snooper( vs.weapon ) ) {
		rd.rdflags |= RDF_SNOOPERVIEW;
	}

	// true magnification: the lens covers this much of the view from the head, and shows
	// the world that many times bigger
	dist = Distance( cg.refdef.vieworg, vs.centre );
	if ( dist < 2.0f ) {
		dist = 2.0f;
	}
	fov = 2.0f * RAD2DEG( atan( vs.radius / dist ) ) / CG_VR_ScopePower( vs.weapon );
	if ( fov < 1.5f ) {
		fov = 1.5f;
	} else if ( fov > 90.0f ) {
		fov = 90.0f;
	}
	rd.fov_x = rd.fov_y = fov;

	// from inside the tube, zeroed on where the bullet goes
	VectorMA( vs.centre, 2.0f, vs.axis[0], rd.vieworg );
	VectorCopy( vs.axis[0], fwd );
	if ( cgVR->muzzleValid ) {
		AngleVectors( cgvr.weaponAngles, fwd, NULL, NULL );
		VectorMA( cgvr.muzzle, 8192, fwd, end );
		CG_Trace( &tr, cgvr.muzzle, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
		VectorSubtract( tr.endpos, rd.vieworg, fwd );
		if ( VectorNormalize( fwd ) < 16.0f || DotProduct( fwd, vs.axis[0] ) < 0.9f ) {
			VectorCopy( vs.axis[0], fwd );
		}
	}
	CrossProduct( vs.axis[2], fwd, left );
	VectorNormalize( left );
	VectorCopy( fwd, rd.viewaxis[0] );
	VectorCopy( left, rd.viewaxis[1] );
	CrossProduct( fwd, left, rd.viewaxis[2] );

	trap_R_RenderScene( &rd );
	trap_R_VR_EndScope();
}

static void CG_VR_LensPoint( float u, float v, float lift, vec3_t out ) {
	VectorMA( vs.centre, u * vs.radius, vs.axis[1], out );
	VectorMA( out, v * vs.radius, vs.axis[2], out );
	VectorMA( out, -lift, vs.axis[0], out );
}

// a black bar of the reticle, u (left) and v (up) in lens radii
static void CG_VR_ReticleBar( float u0, float u1, float v0, float v1 ) {
	polyVert_t q[4];
	int i;

	memset( q, 0, sizeof( q ) );
	CG_VR_LensPoint( u0, v0, 0.03f, q[0].xyz );
	CG_VR_LensPoint( u0, v1, 0.03f, q[1].xyz );
	CG_VR_LensPoint( u1, v1, 0.03f, q[2].xyz );
	CG_VR_LensPoint( u1, v0, 0.03f, q[3].xyz );
	for ( i = 0; i < 4; i++ ) {
		q[i].st[0] = q[i].st[1] = 0.5f;
		q[i].modulate[3] = 255;
	}
	trap_R_AddPolyToScene( vs.shader, 4, q );
}

/*
==============
CG_VR_Eyepiece

The disc this eye sees on the rear lens.
==============
*/
static void CG_VR_Eyepiece( void ) {
	polyVert_t ring[VS_RINGS + 1][VS_SEGS];
	polyVert_t p[4];
	vec3_t eye, rel;
	float back, ex, ey, open, u, v, d, b, a;
	int r, s, i, n;
	qboolean snooper = CG_VR_Snooper( vs.weapon );

	// this eye
	VectorCopy( cg.refdef.vieworg, eye );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( eye, cgVR->eyeOffset[i], cg.refdef.viewaxis[i], eye );
	}
	VectorSubtract( eye, vs.centre, rel );
	back = -DotProduct( rel, vs.axis[0] );
	{
		static int next;
		char dbg[8];
		trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
		if ( atoi( dbg ) == 4 && cg.time >= next ) {
			next = cg.time + 300;
			CG_Printf( "VR scope: weapon %d, eye %.1f behind it, %.1f left %.1f up, radius %.2f, sights %d\n", vs.weapon,
					   back, DotProduct( rel, vs.axis[1] ), DotProduct( rel, vs.axis[2] ), vs.radius, cgVR->ironSights );
		}
	}
	if ( back < 0.5f ) {
		return;     // not behind it
	}

	// an eye off the scope's axis sees a dark crescent come in from the far side, and
	// one held far back sees the picture shrink
	ex = DotProduct( rel, vs.axis[1] ) / ( vs.radius * 1.5f );
	ey = DotProduct( rel, vs.axis[2] ) / ( vs.radius * 1.5f );
	open = 1.0f - 0.35f * Com_Clamp( 0, 1, ( back - 8.0f ) / 16.0f );

	for ( r = 0; r <= VS_RINGS; r++ ) {
		for ( s = 0; s < VS_SEGS; s++ ) {
			polyVert_t *pv = &ring[r][s];

			a = 2.0f * M_PI * s / VS_SEGS;
			u = (float)r / VS_RINGS * cos( a );
			v = (float)r / VS_RINGS * sin( a );
			CG_VR_LensPoint( u, v, 0.0f, pv->xyz );
			// the view's left is the texture's -s, its up is +t
			pv->st[0] = 0.5f - 0.5f * u;
			pv->st[1] = 0.5f + 0.5f * v;

			d = sqrt( ( u - ex ) * ( u - ex ) + ( v - ey ) * ( v - ey ) );
			b = Com_Clamp( 0, 1, ( open - d ) / 0.3f );
			pv->modulate[0] = (byte)( 255 * b * ( snooper ? 0.6f : 1.0f ) );
			pv->modulate[1] = (byte)( 255 * b );
			pv->modulate[2] = (byte)( 255 * b * ( snooper ? 0.6f : 1.0f ) );
			pv->modulate[3] = 255;
			if ( r == 0 ) {
				break;      // the centre is one point
			}
		}
	}
	for ( s = 0; s < VS_SEGS; s++ ) {
		n = ( s + 1 ) % VS_SEGS;
		p[0] = ring[0][0];
		p[1] = ring[1][s];
		p[2] = ring[1][n];
		trap_R_AddPolyToScene( vs.shader, 3, p );
		for ( r = 1; r < VS_RINGS; r++ ) {
			p[0] = ring[r][s];
			p[1] = ring[r + 1][s];
			p[2] = ring[r + 1][n];
			p[3] = ring[r][n];
			trap_R_AddPolyToScene( vs.shader, 4, p );
		}
	}

	// post reticle: side bars and a post from below, the aim point at its tip
	CG_VR_ReticleBar( 0.30f, 0.97f, -0.025f, 0.025f );
	CG_VR_ReticleBar( -0.97f, -0.30f, -0.025f, 0.025f );
	CG_VR_ReticleBar( -0.03f, 0.03f, -0.97f, 0.0f );
	CG_VR_ReticleBar( -0.30f, 0.30f, -0.006f, 0.006f );
}

/*
==============================================================================

Binoculars: the off hand raised to the eyes (cl_vr.c) zooms; each eye sees a lens in
front of it, black all around, showing the view ahead magnified.

==============================================================================
*/

#define VB_FOV          10.0f   // what the binoculars show
#define VB_DIST         1.5f    // the lens in front of each eye...
#define VB_RADIUS       0.75f   // ...this big (53 degrees across): about 5x

static void CG_VR_BinocView( void ) {
	refdef_t rd;
	int size;

	size = trap_R_VR_BeginScope();
	if ( size <= 0 ) {
		return;
	}
	rd = cg.refdef;
	rd.x = rd.y = 0;
	rd.width = rd.height = size;
	rd.rdflags = ( cg.refdef.rdflags & ~( RDF_VRVIEW | RDF_SKYBOXPORTAL | RDF_DRAWSKYBOX ) ) | RDF_VRSCOPE;
	rd.fov_x = rd.fov_y = VB_FOV / Com_Clamp( 0.1f, 4.0f, vr_scopeZoom.value );
	trap_R_RenderScene( &rd );
	trap_R_VR_EndScope();
}

static void CG_VR_BinocEyepiece( void ) {
	polyVert_t ring[VS_RINGS + 1][VS_SEGS];
	polyVert_t p[4];
	float u, v, f, b, a;
	int r, s, i, n;

	// a lens straight ahead of this eye
	VectorCopy( cg.refdef.vieworg, vs.centre );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( vs.centre, cgVR->eyeOffset[i], cg.refdef.viewaxis[i], vs.centre );
		VectorCopy( cg.refdef.viewaxis[i], vs.axis[i] );
	}
	VectorMA( vs.centre, VB_DIST, vs.axis[0], vs.centre );
	vs.radius = VB_RADIUS;

	for ( r = 0; r <= VS_RINGS; r++ ) {
		for ( s = 0; s < VS_SEGS; s++ ) {
			polyVert_t *pv = &ring[r][s];

			a = 2.0f * M_PI * s / VS_SEGS;
			f = (float)r / VS_RINGS;
			u = f * cos( a );
			v = f * sin( a );
			CG_VR_LensPoint( u, v, 0.0f, pv->xyz );
			pv->st[0] = 0.5f - 0.5f * u;
			pv->st[1] = 0.5f + 0.5f * v;
			b = Com_Clamp( 0, 1, ( 1.0f - f ) / 0.3f + 0.2f );
			pv->modulate[0] = pv->modulate[1] = pv->modulate[2] = (byte)( 255 * b );
			pv->modulate[3] = 255;
			if ( r == 0 ) {
				break;
			}
		}
	}
	for ( s = 0; s < VS_SEGS; s++ ) {
		n = ( s + 1 ) % VS_SEGS;
		p[0] = ring[0][0];
		p[1] = ring[1][s];
		p[2] = ring[1][n];
		trap_R_AddPolyToScene( vs.shader, 3, p );
		for ( r = 1; r < VS_RINGS; r++ ) {
			p[0] = ring[r][s];
			p[1] = ring[r + 1][s];
			p[2] = ring[r + 1][n];
			p[3] = ring[r][n];
			trap_R_AddPolyToScene( vs.shader, 4, p );
		}

		// the body of the binoculars blocks everything else
		memset( p, 0, sizeof( p ) );
		a = 2.0f * M_PI * s / VS_SEGS;
		b = 2.0f * M_PI * n / VS_SEGS;
		CG_VR_LensPoint( cos( a ), sin( a ), 0.0f, p[0].xyz );
		CG_VR_LensPoint( 8 * cos( a ), 8 * sin( a ), 0.0f, p[1].xyz );
		CG_VR_LensPoint( 8 * cos( b ), 8 * sin( b ), 0.0f, p[2].xyz );
		CG_VR_LensPoint( cos( b ), sin( b ), 0.0f, p[3].xyz );
		for ( i = 0; i < 4; i++ ) {
			p[i].modulate[3] = 255;
		}
		trap_R_AddPolyToScene( vs.shader, 4, p );
	}
}

/*
==============
CG_VR_RenderScope

Before the eye view is rendered: the scope's picture (first eye only, the view through it
is the same for both), then the eyepiece.
==============
*/
void CG_VR_RenderScope( void ) {
	if ( !CG_VR_EyePass() ) {
		return;
	}
	if ( cg.zoomedBinoc && !cg.renderingThirdPerson ) {
		if ( cgVR->eye == 0 ) {
			CG_VR_BinocView();
		}
		CG_VR_BinocEyepiece();
		return;
	}
	if ( vs.frame != cg.clientFrame ) {
		return;
	}
	if ( cgVR->eye == 0 ) {
		CG_VR_ScopeView();
	}
	CG_VR_Eyepiece();
}

// a scope is being looked through this frame (for the reticle and such)
qboolean CG_VR_ScopeActive( void ) {
	return vs.frame == cg.clientFrame;
}

/*
==============================================================================

Iron sights: an eye on the line along the barrel, behind the gun, steadies the aim
(half the spread, see G_VR_SpreadScale). Scopes count too, their eye line is close.

==============================================================================
*/

vmCvar_t vr_ironSights;

void CG_VR_IronSights( void ) {
	static qboolean registered;
	vec3_t fwd, eye, rel;
	float along, perp;
	int side;

	if ( !registered ) {
		trap_Cvar_Register( &vr_ironSights, "vr_ironSights", "1", CVAR_ARCHIVE );
		registered = qtrue;
	}
	if ( !cgVR ) {
		return;
	}
	cgVR->ironSights = 0;
	trap_Cvar_Update( &vr_ironSights );
	if ( !vr_ironSights.integer || !cgVR->muzzleValid || !CG_VR_EyePass() ) {
		return;
	}

	AngleVectors( cgvr.weaponAngles, fwd, NULL, NULL );
	for ( side = -1; side <= 1; side += 2 ) {
		// either eye, about 32 mm off the middle of the head
		VectorMA( cg.refdef.vieworg, side * 1.25f, cg.refdef.viewaxis[1], eye );
		VectorSubtract( eye, cgvr.muzzle, rel );
		along = -DotProduct( rel, fwd );
		if ( along < 8.0f || along > 110.0f ) {     // long rifles reach far out
			continue;
		}
		VectorMA( rel, along, fwd, rel );
		perp = VectorLength( rel );
		{
			static int next;
			char dbg[8];
			trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
			if ( atoi( dbg ) == 4 && cg.time >= next ) {
				next = cg.time + 300;
				CG_Printf( "VR sights: eye %d %.1f behind the muzzle, %.1f off the line\n", side, along, perp );
			}
		}
		// looking along the barrel: within 6 degrees of the bore line (scopes sit a few
		// units over it, on long rifles far back from the muzzle)
		if ( perp < 8.0f && perp < along * 0.105f ) {
			cgVR->ironSights = 1;
			return;
		}
	}
}
