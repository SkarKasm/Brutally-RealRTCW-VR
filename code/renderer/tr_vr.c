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

// tr_vr.c -- headset rendering support for the GL1 renderer.
//
// The engine (client/cl_vr.c) owns the OpenXR session and its swapchains. For every pass
// it binds a swapchain framebuffer through RE_VR_BeginPass; the renderer keeps drawing
// into tr.mainFbo exactly as on a monitor, and FBO_PostProcess composites into that
// swapchain image instead of the window. Scenes flagged RDF_VRVIEW additionally get the
// eye's asymmetric frustum and eye offset from RE_VR_SetEye.

#include "tr_local.h"

#ifdef USE_LOCAL_HEADERS
#	include "SDL3/SDL.h"
#else
#	include <SDL3/SDL.h>
#endif

vrRenderState_t vrRender = { -1 };

/*
==============
RE_VR_SetEye
==============
*/
void RE_VR_SetEye( int eye, const float *fovTangents, const vec3_t eyeOffset ) {
	vrRender.eye = eye;

	if ( eye < 0 ) {
		return;
	}

	vrRender.fovTan[0] = fovTangents[0];
	vrRender.fovTan[1] = fovTangents[1];
	vrRender.fovTan[2] = fovTangents[2];
	vrRender.fovTan[3] = fovTangents[3];
	VectorCopy( eyeOffset, vrRender.eyeOffset );
}

/*
==============
RE_VR_BeginPass
==============
*/
void RE_VR_BeginPass( unsigned int targetFramebuffer, const float *clearColor ) {
	// anything still queued was meant for the previous target
	R_IssuePendingRenderCommands();

	vrRender.targetFbo = targetFramebuffer;
	vrRender.passActive = qtrue;

	if ( !clearColor || !fboEnabled ) {
		return;
	}

	// the 2D passes have no world view to overwrite the previous eye's image, and need
	// transparent pixels around the HUD, so start them from a known colour
	FBO_Bind( tr.mainFbo );
	qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglDepthMask( GL_TRUE );
	qglClearColor( clearColor[0], clearColor[1], clearColor[2], clearColor[3] );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	qglDepthMask( ( glState.glStateBits & GLS_DEPTHMASK_TRUE ) ? GL_TRUE : GL_FALSE );
}

/*
==============
RE_VR_EndPasses
==============
*/
void RE_VR_EndPasses( void ) {
	R_IssuePendingRenderCommands();

	vrRender.targetFbo = 0;
	vrRender.passActive = qfalse;
	vrRender.eye = -1;

	if ( fboEnabled ) {
		FBO_Bind( tr.mainFbo );
	}
}

/*
==============
RE_VR_PresentMirror

Shows one of the headset images on the desktop window, letterboxed to keep its aspect.
==============
*/
void RE_VR_PresentMirror( unsigned int sourceFramebuffer, int sourceWidth, int sourceHeight ) {
	int winW, winH;
	int dstW, dstH, dstX, dstY;

	R_IssuePendingRenderCommands();

	GLimp_GetWindowPixelSize( &winW, &winH );
	if ( winW <= 0 || winH <= 0 || sourceWidth <= 0 || sourceHeight <= 0 ) {
		return;
	}

	// fit the source into the window
	dstW = winW;
	dstH = (int)( (float)winW * sourceHeight / sourceWidth );
	if ( dstH > winH ) {
		dstH = winH;
		dstW = (int)( (float)winH * sourceWidth / sourceHeight );
	}
	dstX = ( winW - dstW ) / 2;
	dstY = ( winH - dstH ) / 2;

	qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
	qglScissor( 0, 0, winW, winH );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT );

	qglBindFramebuffer( GL_READ_FRAMEBUFFER, sourceFramebuffer );
	qglBlitFramebuffer( 0, 0, sourceWidth, sourceHeight,
						dstX, dstY, dstX + dstW, dstY + dstH,
						GL_COLOR_BUFFER_BIT, GL_LINEAR );

	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	GLimp_EndFrame();

	// FBO_Bind() caches the bound framebuffer, resync it
	if ( fboEnabled ) {
		FBO_Bind( NULL );
		FBO_Bind( tr.mainFbo );
	}
}

/*
==============
R_VR_GLProc / R_VR_CurrentTarget
==============
*/
void *R_VR_GLProc( const char *name ) {
	return SDL_GL_GetProcAddress( name );
}

// what the renderer draws into right now: tr.mainFbo, or the pass's swapchain image
unsigned int R_VR_CurrentTarget( void ) {
	if ( fboEnabled ) {
		return tr.mainFbo->frameBuffer;
	}
	return vrRender.targetFbo;
}

/*
==============
R_VR_SetupViewParms

Called by RE_RenderScene for every scene. Headset world views get the current eye's
offset and frustum; everything else (UI models, etc.) renders untouched.
==============
*/
void R_VR_SetupViewParms( viewParms_t *parms, int rdflags ) {
	const float *t = vrRender.fovTan;
	const float *o = vrRender.eyeOffset;

	parms->vrEye = qfalse;
	R_VR_ScopeViewParms( parms, rdflags );

	if ( vrRender.eye < 0 || !( rdflags & RDF_VRVIEW ) ) {
		return;
	}

	parms->vrEye = qtrue;
	parms->vrFovTan[0] = t[0];
	parms->vrFovTan[1] = t[1];
	parms->vrFovTan[2] = t[2];
	parms->vrFovTan[3] = t[3];

	// the 3D sky (skybox portal) is a small set seen from a camera inside it: with the
	// eyes apart it reads as a miniature right in front of you, so both eyes look from
	// the same spot and it sits at infinity, like a skybox should
	if ( rdflags & RDF_SKYBOXPORTAL ) {
		o = vec3_origin;
	}

	// the eye sits off the head centre the refdef describes
	VectorMA( parms->or.origin, o[0], parms->or.axis[0], parms->or.origin );
	VectorMA( parms->or.origin, o[1], parms->or.axis[1], parms->or.origin );
	VectorMA( parms->or.origin, o[2], parms->or.axis[2], parms->or.origin );
	VectorCopy( parms->or.origin, parms->pvsOrigin );

	// keep the symmetric fov values meaningful for LOD and other fov based code
	parms->fovX = RAD2DEG( atan( -t[0] ) + atan( t[1] ) );
	parms->fovY = RAD2DEG( atan( t[2] ) + atan( -t[3] ) );
}

/*
==============
R_VR_IsHandSurface

Names the view weapon models use for the player's hands and sleeves
(hqhand, hqarm, b3_lhand, w_hand, shotgun_sleeve, ...).
==============
*/
qboolean R_VR_IsSleeveSurface( const char *name ) {
	return ( Q_stristr( name, "arm" ) || Q_stristr( name, "sleeve" ) ) && !Q_stristr( name, "hand" );
}

qboolean R_VR_IsHandSurface( const char *name ) {
	return Q_stristr( name, "hand" ) || Q_stristr( name, "arm" ) ||
		   Q_stristr( name, "sleeve" ) || Q_stristr( name, "glove" );
}

/*
==============
R_VR_BendArm

A first person arm (hand, forearm, elbow, start of the upper arm), already in tess, posed
straight by the model. It bends like a skeleton would: past the wrist (bend[0..2]) the
forearm (along bend[3..5], bend[6] long) turns to run along bend[7..9]; past the elbow
the upper arm turns to run along bend[10..12], stretched by bend[13]. Each joint blends
over bend[14] either side so the sleeve bends instead of tearing.
==============
*/
static void R_VR_Rotation( const vec3_t a, const vec3_t d, vec3_t r[3] ) {
	vec3_t axis;
	float c, sn, k;
	int i, j;

	CrossProduct( a, d, axis );
	sn = VectorNormalize( axis );
	c = DotProduct( a, d );
	if ( sn < 0.0001f ) {
		sn = 0;
		if ( c > 0 ) {
			VectorSet( axis, 0, 0, 1 );
		} else {
			PerpendicularVector( axis, a );     // straight back: half a turn about anything
		}
	}
	k = 1.0f - c;
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			r[i][j] = k * axis[i] * axis[j] + ( i == j ? c : 0 );
		}
	}
	r[0][1] -= sn * axis[2]; r[1][0] += sn * axis[2];
	r[0][2] += sn * axis[1]; r[2][0] -= sn * axis[1];
	r[1][2] -= sn * axis[0]; r[2][1] += sn * axis[0];
}

static float R_VR_Smooth( float x ) {
	x = x < 0 ? 0 : ( x > 1 ? 1 : x );
	return x * x * ( 3 - 2 * x );
}

static void R_VR_Mul( vec3_t r[3], const vec3_t v, vec3_t out ) {
	out[0] = DotProduct( r[0], v );
	out[1] = DotProduct( r[1], v );
	out[2] = DotProduct( r[2], v );
}

void R_VR_BendArm( const float *bend, int first, int count ) {
	vec3_t w, a, d1, d2, r1[3], r2[3], e0, en, v, pf, pu, n1, n2, tmp;
	float len = bend[6], s = bend[13], blend = bend[14], t, tu, x1, x2;
	int i, k;

	VectorCopy( bend + 0, w );
	VectorCopy( bend + 3, a );
	VectorCopy( bend + 7, d1 );
	VectorCopy( bend + 10, d2 );
	if ( VectorNormalize( a ) < 0.001f || VectorNormalize( d1 ) < 0.001f || VectorNormalize( d2 ) < 0.001f ) {
		return;
	}
	if ( blend < 0.01f ) {
		blend = 0.01f;
	}
	R_VR_Rotation( a, d1, r1 );
	R_VR_Rotation( a, d2, r2 );
	VectorMA( w, len, a, e0 );
	VectorMA( w, len, d1, en );

	for ( i = first; i < first + count; i++ ) {
		float *xyz = tess.xyz[i], *nrm = tess.normal[i];
		VectorSubtract( xyz, w, v );
		t = DotProduct( v, a );
		if ( t < -blend ) {
			continue;       // the hand
		}
		x1 = R_VR_Smooth( ( t + blend ) / ( 2 * blend ) );
		x2 = R_VR_Smooth( ( t - len + blend ) / ( 2 * blend ) );
		// the forearm, turned at the wrist
		R_VR_Mul( r1, v, pf );
		VectorAdd( pf, w, pf );
		R_VR_Mul( r1, nrm, n1 );
		// the upper arm, turned at the (moved) elbow and stretched
		VectorSubtract( xyz, e0, v );
		tu = DotProduct( v, a );
		if ( tu > 0 ) {
			VectorMA( v, ( s - 1 ) * tu, a, v );
		}
		R_VR_Mul( r2, v, pu );
		VectorAdd( pu, en, pu );
		R_VR_Mul( r2, nrm, n2 );
		for ( k = 0; k < 3; k++ ) {
			tmp[k] = xyz[k] + ( pf[k] - xyz[k] ) * x1;
			xyz[k] = tmp[k] + ( pu[k] - tmp[k] ) * x2;
			tmp[k] = nrm[k] + ( n1[k] - nrm[k] ) * x1;
			nrm[k] = tmp[k] + ( n2[k] - tmp[k] ) * x2;
		}
		VectorNormalize( nrm );
	}
}
