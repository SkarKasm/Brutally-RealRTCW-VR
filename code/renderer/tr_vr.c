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

// tr_vr.c -- headset rendering support for the co-op GL1 renderer.
//
// The engine (client/cl_vr.c) owns the OpenXR session and its swapchains. This renderer
// has no offscreen pipeline, so for every pass the swapchain image's framebuffer (colour +
// depth) is bound directly and the renderer draws into it as if it were the window. Scenes
// flagged RDF_VRVIEW get the eye's asymmetric frustum and eye offset from RE_VR_SetEye.

#include "tr_local.h"

vrRenderState_t vrRender = { -1 };

#define VR_GL_FRAMEBUFFER       0x8D40
#define VR_GL_READ_FRAMEBUFFER  0x8CA8
#define VR_GL_DRAW_FRAMEBUFFER  0x8CA9

typedef void ( APIENTRY *vrBindFramebuffer_t )( GLenum target, GLuint fb );
typedef void ( APIENTRY *vrBlitFramebuffer_t )( GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum );

static vrBindFramebuffer_t vrBindFramebuffer;
static vrBlitFramebuffer_t vrBlitFramebuffer;

static qboolean R_VR_LoadGL( void ) {
	if ( !vrBindFramebuffer ) {
		vrBindFramebuffer = (vrBindFramebuffer_t)GLimp_VR_GetProc( "glBindFramebuffer" );
		vrBlitFramebuffer = (vrBlitFramebuffer_t)GLimp_VR_GetProc( "glBlitFramebuffer" );
	}
	return vrBindFramebuffer && vrBlitFramebuffer;
}

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
	if ( !R_VR_LoadGL() ) {
		return;
	}

	// anything still queued was meant for the previous target
	R_IssuePendingRenderCommands();

	vrRender.targetFbo = targetFramebuffer;
	vrRender.passActive = qtrue;
	vrBindFramebuffer( VR_GL_FRAMEBUFFER, targetFramebuffer );

	qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglDepthMask( GL_TRUE );
	if ( clearColor ) {
		qglClearColor( clearColor[0], clearColor[1], clearColor[2], clearColor[3] );
	} else {
		qglClearColor( 0, 0, 0, 1 );
	}
	// the swapchain images are recycled: always start from a clean colour and depth buffer
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

	if ( R_VR_LoadGL() ) {
		vrBindFramebuffer( VR_GL_FRAMEBUFFER, 0 );
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

	if ( !R_VR_LoadGL() ) {
		return;
	}
	R_IssuePendingRenderCommands();

	GLimp_GetWindowPixelSize( &winW, &winH );
	if ( winW <= 0 || winH <= 0 || sourceWidth <= 0 || sourceHeight <= 0 ) {
		return;
	}

	dstW = winW;
	dstH = (int)( (float)winW * sourceHeight / sourceWidth );
	if ( dstH > winH ) {
		dstH = winH;
		dstW = (int)( (float)winH * sourceWidth / sourceHeight );
	}
	dstX = ( winW - dstW ) / 2;
	dstY = ( winH - dstH ) / 2;

	vrBindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, 0 );
	qglScissor( 0, 0, winW, winH );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT );

	vrBindFramebuffer( VR_GL_READ_FRAMEBUFFER, sourceFramebuffer );
	vrBlitFramebuffer( 0, 0, sourceWidth, sourceHeight,
					   dstX, dstY, dstX + dstW, dstY + dstH,
					   GL_COLOR_BUFFER_BIT, GL_LINEAR );

	vrBindFramebuffer( VR_GL_FRAMEBUFFER, vrRender.passActive ? vrRender.targetFbo : 0 );

	GLimp_EndFrame();
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

	if ( vrRender.eye < 0 || !( rdflags & RDF_VRVIEW ) ) {
		return;
	}

	parms->vrEye = qtrue;
	parms->vrFovTan[0] = t[0];
	parms->vrFovTan[1] = t[1];
	parms->vrFovTan[2] = t[2];
	parms->vrFovTan[3] = t[3];

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
qboolean R_VR_IsHandSurface( const char *name ) {
	return Q_stristr( name, "hand" ) || Q_stristr( name, "arm" ) ||
		   Q_stristr( name, "sleeve" ) || Q_stristr( name, "glove" );
}
