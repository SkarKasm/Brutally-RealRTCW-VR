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

// tr_vrscope.c -- see-through scopes.
//
// Once a frame the cgame renders the view through the weapon's scope into an offscreen
// texture: RE_VR_BeginScope binds it, a scene flagged RDF_VRSCOPE is rendered (without the
// first person models, and without using up the frame's entities, so the eye view that
// follows still has them all), RE_VR_EndScope goes back to the eye's target. The eyepiece
// is then drawn with the shader "*vrScope", which samples that texture with vertex colours.

#include "tr_local.h"

#define VR_SCOPE_SIZE   512

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER              0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER             0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0        0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8         0x88F0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#endif

typedef void ( APIENTRY *vsGenFramebuffers_t )( GLsizei n, GLuint *ids );
typedef void ( APIENTRY *vsBindFramebuffer_t )( GLenum target, GLuint fb );
typedef void ( APIENTRY *vsFramebufferTexture2D_t )( GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level );
typedef void ( APIENTRY *vsGenRenderbuffers_t )( GLsizei n, GLuint *ids );
typedef void ( APIENTRY *vsBindRenderbuffer_t )( GLenum target, GLuint rb );
typedef void ( APIENTRY *vsRenderbufferStorage_t )( GLenum target, GLenum format, GLsizei w, GLsizei h );
typedef void ( APIENTRY *vsFramebufferRenderbuffer_t )( GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb );
typedef GLenum ( APIENTRY *vsCheckFramebufferStatus_t )( GLenum target );

static struct {
	vsGenFramebuffers_t         GenFramebuffers;
	vsBindFramebuffer_t         BindFramebuffer;
	vsFramebufferTexture2D_t    FramebufferTexture2D;
	vsGenRenderbuffers_t        GenRenderbuffers;
	vsBindRenderbuffer_t        BindRenderbuffer;
	vsRenderbufferStorage_t     RenderbufferStorage;
	vsFramebufferRenderbuffer_t FramebufferRenderbuffer;
	vsCheckFramebufferStatus_t  CheckFramebufferStatus;
	qboolean loaded;
} vsgl;

static struct {
	image_t     *image;         // what the eyepiece shows, "*vrScope"
	GLuint      fbo, depth;
	qboolean    failed;         // no FBO support, stay off
	qboolean    active;         // between RE_VR_BeginScope and RE_VR_EndScope
} scope;

static qboolean R_VR_ScopeLoadGL( void ) {
	if ( !vsgl.loaded ) {
		vsgl.loaded = qtrue;
		vsgl.GenFramebuffers = (vsGenFramebuffers_t)R_VR_GLProc( "glGenFramebuffers" );
		vsgl.BindFramebuffer = (vsBindFramebuffer_t)R_VR_GLProc( "glBindFramebuffer" );
		vsgl.FramebufferTexture2D = (vsFramebufferTexture2D_t)R_VR_GLProc( "glFramebufferTexture2D" );
		vsgl.GenRenderbuffers = (vsGenRenderbuffers_t)R_VR_GLProc( "glGenRenderbuffers" );
		vsgl.BindRenderbuffer = (vsBindRenderbuffer_t)R_VR_GLProc( "glBindRenderbuffer" );
		vsgl.RenderbufferStorage = (vsRenderbufferStorage_t)R_VR_GLProc( "glRenderbufferStorage" );
		vsgl.FramebufferRenderbuffer = (vsFramebufferRenderbuffer_t)R_VR_GLProc( "glFramebufferRenderbuffer" );
		vsgl.CheckFramebufferStatus = (vsCheckFramebufferStatus_t)R_VR_GLProc( "glCheckFramebufferStatus" );
	}
	return vsgl.GenFramebuffers && vsgl.BindFramebuffer && vsgl.FramebufferTexture2D && vsgl.GenRenderbuffers &&
		   vsgl.BindRenderbuffer && vsgl.RenderbufferStorage && vsgl.FramebufferRenderbuffer && vsgl.CheckFramebufferStatus;
}

/*
==============
R_VR_CreateScopeImage

With the other built in images (so again after every vid_restart).
==============
*/
void R_VR_CreateScopeImage( void ) {
	byte *data;
	int i;

	data = ri.Hunk_AllocateTempMemory( VR_SCOPE_SIZE * VR_SCOPE_SIZE * 4 );
	for ( i = 0; i < VR_SCOPE_SIZE * VR_SCOPE_SIZE; i++ ) {
		data[i * 4 + 0] = data[i * 4 + 1] = data[i * 4 + 2] = 0;
		data[i * 4 + 3] = 255;
	}
	scope.image = R_CreateImage( "*vrScope", data, VR_SCOPE_SIZE, VR_SCOPE_SIZE, IMGTYPE_COLORALPHA,
								 IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, 0 );
	ri.Hunk_FreeTempMemory( data );

	// a render target wants exact 8 bit storage, whatever r_texturebits says
	GL_Bind( scope.image );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, VR_SCOPE_SIZE, VR_SCOPE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );

	// the old framebuffer went with the old context
	scope.fbo = scope.depth = 0;
	scope.failed = qfalse;
	scope.active = qfalse;
}

/*
==============
R_VR_ScopeShaderName

"*vrScope" becomes a plain opaque shader with exact vertex colours (R_FindShader).
==============
*/
qboolean R_VR_ScopeShaderName( const char *name ) {
	return !Q_stricmp( name, "*vrScope" );
}

static qboolean R_VR_ScopeCreateFBO( void ) {
	vsgl.GenFramebuffers( 1, &scope.fbo );
	vsgl.BindFramebuffer( GL_FRAMEBUFFER, scope.fbo );
	vsgl.FramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scope.image->texnum, 0 );

	vsgl.GenRenderbuffers( 1, &scope.depth );
	vsgl.BindRenderbuffer( GL_RENDERBUFFER, scope.depth );
	vsgl.RenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, VR_SCOPE_SIZE, VR_SCOPE_SIZE );
	vsgl.FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scope.depth );
	vsgl.BindRenderbuffer( GL_RENDERBUFFER, 0 );

	if ( vsgl.CheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) {
		ri.Printf( PRINT_WARNING, "VR: no framebuffer for the scopes, they stay dark\n" );
		vsgl.BindFramebuffer( GL_FRAMEBUFFER, R_VR_CurrentTarget() );
		return qfalse;
	}
	return qtrue;
}

/*
==============
RE_VR_BeginScope

Returns the size of the square view to render, 0 when there is nothing to render into.
==============
*/
int RE_VR_BeginScope( void ) {
	if ( scope.active || scope.failed || !scope.image || !tr.registered ) {
		return 0;
	}
	if ( !R_VR_ScopeLoadGL() ) {
		scope.failed = qtrue;
		return 0;
	}

	// whatever is queued belongs to the eye's target
	R_IssuePendingRenderCommands();

	if ( !scope.fbo && !R_VR_ScopeCreateFBO() ) {
		scope.failed = qtrue;
		return 0;
	}

	vsgl.BindFramebuffer( GL_FRAMEBUFFER, scope.fbo );
	qglScissor( 0, 0, VR_SCOPE_SIZE, VR_SCOPE_SIZE );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglDepthMask( GL_TRUE );
	qglClearColor( 0, 0, 0, 1 );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	qglDepthMask( ( glState.glStateBits & GLS_DEPTHMASK_TRUE ) ? GL_TRUE : GL_FALSE );

	scope.active = qtrue;
	return VR_SCOPE_SIZE;
}

/*
==============
RE_VR_EndScope
==============
*/
void RE_VR_EndScope( void ) {
	if ( !scope.active ) {
		return;
	}
	R_IssuePendingRenderCommands();
	scope.active = qfalse;
	vsgl.BindFramebuffer( GL_FRAMEBUFFER, R_VR_CurrentTarget() );
}

/*
==============
R_VR_ScopeViewParms

The scope scene fills the offscreen texture, wherever the cgame put its refdef.
==============
*/
void R_VR_ScopeViewParms( viewParms_t *parms, int rdflags ) {
	if ( !( rdflags & RDF_VRSCOPE ) || !scope.active ) {
		return;
	}
	parms->viewportX = 0;
	parms->viewportY = 0;
	parms->viewportWidth = VR_SCOPE_SIZE;
	parms->viewportHeight = VR_SCOPE_SIZE;
}
