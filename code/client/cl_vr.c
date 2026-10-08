/*
===========================================================================

RealRTCW-VR
Copyright (C) 2026 RealRTCW-VR contributors

This file is part of RealRTCW-VR, a VR port of RealRTCW.

RealRTCW-VR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Parts of the OpenXR setup follow the structure of DrBeef's JKXR / RTCWQuest
(GPL), adapted for desktop OpenGL and this engine.

===========================================================================
*/

// cl_vr.c -- OpenXR headset support: session, swapchains, controllers and frame loop.
//
// Frame flow (all on the main thread, GL context current):
//   CL_Frame -> CL_VR_BeginFrame   xrWaitFrame/xrBeginFrame, locate head/hands, input
//            -> CL_SendCmd         CL_VR_CreateCmd injects aim, movement, room-scale
//            -> SCR_UpdateScreen   CL_VR_RenderFrame: left eye, right eye, 2D layer,
//                                  desktop mirror, xrEndFrame
//
// The OpenXR loader is loaded at runtime, so the game still runs on a monitor when no
// loader, runtime or headset is present.

#include "client.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>     // openxr_platform.h's win32 section references IUnknown

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

void CL_KeyEvent( int key, qboolean down, unsigned time );
void SCR_DrawScreenField( stereoFrame_t stereoFrame );

/*
==============================================================================

OpenGL entry points (the client doesn't link GL, the renderer DLL owns it)

==============================================================================
*/

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;

#define GL_TEXTURE_2D           0x0DE1
#define GL_FRAMEBUFFER          0x8D40
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8                0x8058
#define GL_SRGB8_ALPHA8         0x8C43
#define GL_FRAMEBUFFER_SRGB     0x8DB9

typedef void ( APIENTRY *PFNGLGENFRAMEBUFFERS )( GLsizei n, GLuint *ids );
typedef void ( APIENTRY *PFNGLDELETEFRAMEBUFFERS )( GLsizei n, const GLuint *ids );
typedef void ( APIENTRY *PFNGLBINDFRAMEBUFFER )( GLenum target, GLuint fb );
typedef void ( APIENTRY *PFNGLFRAMEBUFFERTEXTURE2D )( GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level );
typedef GLenum ( APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUS )( GLenum target );
typedef void ( APIENTRY *PFNGLDISABLE )( GLenum cap );
typedef PROC ( WINAPI *PFNWGLGETPROCADDRESS )( LPCSTR name );
typedef HGLRC ( WINAPI *PFNWGLGETCURRENTCONTEXT )( void );
typedef HDC ( WINAPI *PFNWGLGETCURRENTDC )( void );

static PFNGLGENFRAMEBUFFERS qglGenFramebuffers;
static PFNGLDELETEFRAMEBUFFERS qglDeleteFramebuffers;
static PFNGLBINDFRAMEBUFFER qglBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2D qglFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUS qglCheckFramebufferStatus;
static PFNGLDISABLE qglDisable;
static PFNWGLGETCURRENTCONTEXT qwglGetCurrentContext;
static PFNWGLGETCURRENTDC qwglGetCurrentDC;

static void *VR_GetGLProc( const char *name ) {
	HMODULE gl = GetModuleHandleA( "opengl32.dll" );
	PFNWGLGETPROCADDRESS getProc;
	void *p = NULL;

	if ( !gl ) {
		return NULL;
	}
	getProc = (PFNWGLGETPROCADDRESS)GetProcAddress( gl, "wglGetProcAddress" );
	if ( getProc ) {
		p = (void *)getProc( name );
	}
	// wglGetProcAddress returns small sentinel values for unsupported names on some drivers
	if ( !p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1 ) {
		p = (void *)GetProcAddress( gl, name );
	}
	return p;
}

static qboolean VR_LoadGL( void ) {
	HMODULE gl = GetModuleHandleA( "opengl32.dll" );
	if ( !gl ) {
		return qfalse;
	}
	qwglGetCurrentContext = (PFNWGLGETCURRENTCONTEXT)GetProcAddress( gl, "wglGetCurrentContext" );
	qwglGetCurrentDC = (PFNWGLGETCURRENTDC)GetProcAddress( gl, "wglGetCurrentDC" );
	qglGenFramebuffers = VR_GetGLProc( "glGenFramebuffers" );
	qglDeleteFramebuffers = VR_GetGLProc( "glDeleteFramebuffers" );
	qglBindFramebuffer = VR_GetGLProc( "glBindFramebuffer" );
	qglFramebufferTexture2D = VR_GetGLProc( "glFramebufferTexture2D" );
	qglCheckFramebufferStatus = VR_GetGLProc( "glCheckFramebufferStatus" );
	qglDisable = VR_GetGLProc( "glDisable" );

	return qwglGetCurrentContext && qwglGetCurrentDC && qglGenFramebuffers && qglDeleteFramebuffers &&
		   qglBindFramebuffer && qglFramebufferTexture2D && qglCheckFramebufferStatus && qglDisable;
}

/*
==============================================================================

OpenXR entry points, resolved from the loader at runtime

==============================================================================
*/

#define VR_XR_GLOBAL_FUNCS( X ) \
	X( xrEnumerateInstanceExtensionProperties ) \
	X( xrCreateInstance )

#define VR_XR_INSTANCE_FUNCS( X ) \
	X( xrDestroyInstance ) \
	X( xrGetInstanceProperties ) \
	X( xrGetSystem ) \
	X( xrGetSystemProperties ) \
	X( xrEnumerateViewConfigurationViews ) \
	X( xrGetOpenGLGraphicsRequirementsKHR ) \
	X( xrCreateSession ) \
	X( xrDestroySession ) \
	X( xrBeginSession ) \
	X( xrEndSession ) \
	X( xrRequestExitSession ) \
	X( xrPollEvent ) \
	X( xrEnumerateReferenceSpaces ) \
	X( xrCreateReferenceSpace ) \
	X( xrDestroySpace ) \
	X( xrLocateSpace ) \
	X( xrLocateViews ) \
	X( xrWaitFrame ) \
	X( xrBeginFrame ) \
	X( xrEndFrame ) \
	X( xrEnumerateSwapchainFormats ) \
	X( xrCreateSwapchain ) \
	X( xrDestroySwapchain ) \
	X( xrEnumerateSwapchainImages ) \
	X( xrAcquireSwapchainImage ) \
	X( xrWaitSwapchainImage ) \
	X( xrReleaseSwapchainImage ) \
	X( xrStringToPath ) \
	X( xrPathToString ) \
	X( xrCreateActionSet ) \
	X( xrDestroyActionSet ) \
	X( xrCreateAction ) \
	X( xrSuggestInteractionProfileBindings ) \
	X( xrAttachSessionActionSets ) \
	X( xrCreateActionSpace ) \
	X( xrSyncActions ) \
	X( xrGetActionStateBoolean ) \
	X( xrGetActionStateFloat ) \
	X( xrGetActionStateVector2f ) \
	X( xrGetActionStatePose ) \
	X( xrApplyHapticFeedback ) \
	X( xrGetCurrentInteractionProfile )

#define VR_XR_DECLARE( name ) static PFN_##name q##name;
VR_XR_GLOBAL_FUNCS( VR_XR_DECLARE )
VR_XR_INSTANCE_FUNCS( VR_XR_DECLARE )

static PFN_xrGetInstanceProcAddr qxrGetInstanceProcAddr;

/*
==============================================================================

State

==============================================================================
*/

#define VR_MAX_SWAPCHAIN_IMAGES 8

typedef struct {
	XrSwapchain     handle;
	int             width, height;
	uint32_t        imageCount;
	XrSwapchainImageOpenGLKHR images[VR_MAX_SWAPCHAIN_IMAGES];
	GLuint          fbos[VR_MAX_SWAPCHAIN_IMAGES];
	uint32_t        current;
	qboolean        acquired;
} vrSwapchain_t;

// logical buttons, per hand
typedef enum {
	VRB_TRIGGER,
	VRB_SQUEEZE,
	VRB_LOWER,          // A / X
	VRB_UPPER,          // B / Y
	VRB_STICK,          // thumbstick click
	VRB_MENU,           // left menu button
	VRB_STICK_UP,
	VRB_STICK_DOWN,
	VRB_STICK_LEFT,
	VRB_STICK_RIGHT,
	VRB_COUNT
} vrButton_t;

typedef struct {
	qboolean        down[VRB_COUNT];
	qboolean        wasDown[VRB_COUNT];
	int             downTime[VRB_COUNT];
	float           trigger, squeeze;
	float           stick[2];
	XrPosef         grip, aim;
	qboolean        gripValid, aimValid;
} vrHand_t;

static struct {
	HMODULE         loader;
	XrInstance      instance;
	XrSystemId      systemId;
	qboolean        instanceReady;      // instance + system found, render size known

	XrSession       session;
	XrSessionState  state;
	qboolean        sessionRunning;
	XrSpace         playSpace;          // stage (floor origin) or local
	qboolean        playSpaceIsStage;
	XrSpace         viewSpace;
	int64_t         colorFormat;

	XrViewConfigurationView configViews[2];
	int             width, height;      // render size, shared by eyes and 2D layer

	vrSwapchain_t   eyes[2];
	vrSwapchain_t   ui;

	XrFrameState    frameState;
	qboolean        frameBegun;
	XrView          views[2];           // eye poses in play space
	XrView          headViews[2];       // eye poses relative to the head
	XrPosef         head;
	qboolean        headValid;

	XrActionSet     actionSet;
	XrAction        actGrip, actAim, actTrigger, actSqueeze, actStick, actStickClick;
	XrAction        actLower, actUpper, actMenu, actHaptic;
	XrPath          handPath[2];
	XrSpace         gripSpace[2], aimSpace[2];
	vrHand_t        hands[2];

	// game side
	float           turnYaw;            // artificial turn added to play space
	qboolean        smoothTurnActive;
	vec3_t          lastRawHead;        // for room-scale displacement
	qboolean        lastRawHeadValid;
	vec2_t          pendingMove;        // room-scale displacement not yet sent, play units

	// menus
	qboolean        menuMode;           // UI layer is a world locked panel
	XrPosef         menuPose;
	qboolean        menuPlaced;
	int             pointerHand;
	qboolean        pointerHit;
	float           pointerU, pointerV;
	vec3_t          pointerStart, pointerEnd;   // play space, game units
	float           uiCursorX, uiCursorY;       // where we believe the UI cursor is
	int             lastMenuScrollTime;

	qboolean        gameButtonsLive;    // +commands are currently held down
	char            heldCommands[16][32];
	int             numHeldCommands;
	qboolean        rightGripCombo;     // right grip was used as a modifier while held
	qboolean        attackHeld;         // the trigger fires: goes straight into the usercmd
	qboolean        attackOffHeld;      // dual pistols: the off hand's trigger
	float           headHeight;         // metres above the floor (physical crouch)
	qboolean        teleportPending;    // a teleport goes in the next usercmd
	short           teleportMove[2];
	qboolean        akimboLastLeft;     // which of the two was pulled last
	qboolean        steadyLatched;      // off hand grip closed in front of the gun: two handed aim
} xr;

static cvar_t *vr_enable;
static cvar_t *vr_resolutionScale;
static cvar_t *vr_worldScale;
static cvar_t *vr_rightHanded;
static cvar_t *vr_binoculars;       // off hand to the eyes: binoculars
static cvar_t *vr_hipNotebook;      // off hand grip at the hip: the notebook
static cvar_t *vr_teleport;         // move by pointing the off hand stick and letting go
static cvar_t *vr_seated;           // playing seated: stand the view up, no physical crouch
static cvar_t *vr_seatedHeight;     // metres added to the head when seated
static cvar_t *vr_crouchHeight;     // head below this (metres) crouches, 0 off
static cvar_t *vr_snapTurn;
static cvar_t *vr_turnSpeed;
static cvar_t *vr_walkDirection;
static cvar_t *vr_deadzone;
static cvar_t *vr_roomScale;
static cvar_t *vr_weaponPitch;
static cvar_t *vr_menuDistance;
static cvar_t *vr_menuWidth;
static cvar_t *vr_hudDistance;
static cvar_t *vr_hudWidth;
static cvar_t *vr_hudHeight;
static cvar_t *vr_mirror;
static cvar_t *vr_hapticScale;
static cvar_t *vr_debug;

static void VR_InitBindings( void );

#define VR_OK( call ) VR_Check( ( call ), #call )

static qboolean VR_Check( XrResult res, const char *what ) {
	if ( XR_SUCCEEDED( res ) ) {
		return qtrue;
	}
	Com_Printf( S_COLOR_YELLOW "VR: %s failed (%d)\n", what, (int)res );
	return qfalse;
}

/*
==============================================================================

Math: OpenXR (right handed, y up, -z forward, metres) -> Quake (x fwd, y left, z up)

==============================================================================
*/

static void VR_QuatRotate( const XrQuaternionf *q, const float v[3], float out[3] ) {
	// t = 2 * cross( q.xyz, v ); out = v + q.w * t + cross( q.xyz, t )
	float tx = 2.0f * ( q->y * v[2] - q->z * v[1] );
	float ty = 2.0f * ( q->z * v[0] - q->x * v[2] );
	float tz = 2.0f * ( q->x * v[1] - q->y * v[0] );

	out[0] = v[0] + q->w * tx + ( q->y * tz - q->z * ty );
	out[1] = v[1] + q->w * ty + ( q->z * tx - q->x * tz );
	out[2] = v[2] + q->w * tz + ( q->x * ty - q->y * tx );
}

static void VR_XrDirToQuake( const float xrv[3], vec3_t out ) {
	out[0] = -xrv[2];
	out[1] = -xrv[0];
	out[2] = xrv[1];
}

// metres in XR space -> game units in Quake axes
static void VR_XrPosToQuake( const XrVector3f *p, vec3_t out ) {
	float s = vr_worldScale->value;
	out[0] = -p->z * s;
	out[1] = -p->x * s;
	out[2] = p->y * s;
}

static void VR_AxisToAngles( const vec3_t fwd, const vec3_t left, const vec3_t up, vec3_t angles ) {
	float sp = -fwd[2];

	if ( sp > 1.0f ) {
		sp = 1.0f;
	} else if ( sp < -1.0f ) {
		sp = -1.0f;
	}
	angles[PITCH] = RAD2DEG( asin( sp ) );

	if ( fabs( fwd[2] ) < 0.9999f ) {
		angles[YAW] = RAD2DEG( atan2( fwd[1], fwd[0] ) );
		angles[ROLL] = RAD2DEG( atan2( left[2], up[2] ) );
	} else {
		angles[YAW] = RAD2DEG( atan2( -left[0], left[1] ) );
		angles[ROLL] = 0;
	}
}

static void VR_XrQuatToAngles( const XrQuaternionf *q, vec3_t angles ) {
	static const float xrFwd[3] = { 0, 0, -1 }, xrLeft[3] = { -1, 0, 0 }, xrUp[3] = { 0, 1, 0 };
	float f[3], l[3], u[3];
	vec3_t qf, ql, qu;

	VR_QuatRotate( q, xrFwd, f );
	VR_QuatRotate( q, xrLeft, l );
	VR_QuatRotate( q, xrUp, u );
	VR_XrDirToQuake( f, qf );
	VR_XrDirToQuake( l, ql );
	VR_XrDirToQuake( u, qu );
	VR_AxisToAngles( qf, ql, qu, angles );
}

// rotate a play space (Quake axes) vector about z
static void VR_RotateYaw( const vec3_t in, float yaw, vec3_t out ) {
	float s = sin( DEG2RAD( yaw ) ), c = cos( DEG2RAD( yaw ) );
	float x = in[0], y = in[1];

	out[0] = x * c - y * s;
	out[1] = x * s + y * c;
	out[2] = in[2];
}

// raw tracking pose -> play space (turned) origin and angles
static void VR_PoseToPlay( const XrPosef *pose, vec3_t origin, vec3_t angles ) {
	vec3_t raw;

	VR_XrPosToQuake( &pose->position, raw );
	VR_RotateYaw( raw, xr.turnYaw, origin );
	if ( !xr.playSpaceIsStage ) {
		origin[2] += 1.65f * vr_worldScale->value;     // local space has no floor
	}

	VR_XrQuatToAngles( &pose->orientation, angles );
	angles[YAW] = AngleNormalize180( angles[YAW] + xr.turnYaw );
}

/*
==============================================================================

Loader / instance

==============================================================================
*/

static qboolean VR_LoadLoader( void ) {
	static const char *names[] = { "openxr_loader.dll", "libopenxr_loader.dll" };
	int i;

	for ( i = 0; i < ARRAY_LEN( names ) && !xr.loader; i++ ) {
		xr.loader = LoadLibraryA( names[i] );
	}
	if ( !xr.loader ) {
		Com_Printf( "VR: no OpenXR loader found, running on the monitor\n" );
		return qfalse;
	}

	qxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress( xr.loader, "xrGetInstanceProcAddr" );
	if ( !qxrGetInstanceProcAddr ) {
		Com_Printf( "VR: OpenXR loader has no xrGetInstanceProcAddr\n" );
		return qfalse;
	}

#define VR_XR_LOAD_GLOBAL( name ) \
	if ( XR_FAILED( qxrGetInstanceProcAddr( XR_NULL_HANDLE, #name, (PFN_xrVoidFunction *)&q##name ) ) ) return qfalse;
	VR_XR_GLOBAL_FUNCS( VR_XR_LOAD_GLOBAL )

	return qtrue;
}

static qboolean VR_LoadInstanceFuncs( void ) {
#define VR_XR_LOAD_INSTANCE( name ) \
	if ( XR_FAILED( qxrGetInstanceProcAddr( xr.instance, #name, (PFN_xrVoidFunction *)&q##name ) ) ) { \
		Com_Printf( "VR: missing %s\n", #name ); return qfalse; }
	VR_XR_INSTANCE_FUNCS( VR_XR_LOAD_INSTANCE )
	return qtrue;
}

static qboolean VR_HasExtension( XrExtensionProperties *exts, uint32_t count, const char *name ) {
	uint32_t i;
	for ( i = 0; i < count; i++ ) {
		if ( !strcmp( exts[i].extensionName, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean VR_CreateInstance( void ) {
	XrExtensionProperties *exts;
	uint32_t extCount = 0, i;
	const char *enabled[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfo ici;
	XrInstanceProperties props;
	XrSystemGetInfo sgi;
	XrSystemProperties sysProps;
	uint32_t viewCount = 0;
	float scale;

	if ( XR_FAILED( qxrEnumerateInstanceExtensionProperties( NULL, 0, &extCount, NULL ) ) || !extCount ) {
		Com_Printf( "VR: no OpenXR runtime available\n" );
		return qfalse;
	}
	exts = Z_Malloc( sizeof( *exts ) * extCount );
	for ( i = 0; i < extCount; i++ ) {
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
		exts[i].next = NULL;
	}
	qxrEnumerateInstanceExtensionProperties( NULL, extCount, &extCount, exts );
	if ( !VR_HasExtension( exts, extCount, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ) ) {
		Com_Printf( "VR: the OpenXR runtime does not support OpenGL\n" );
		Z_Free( exts );
		return qfalse;
	}
	Z_Free( exts );

	Com_Memset( &ici, 0, sizeof( ici ) );
	ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
	Q_strncpyz( ici.applicationInfo.applicationName, "RealRTCW-VR", XR_MAX_APPLICATION_NAME_SIZE );
	Q_strncpyz( ici.applicationInfo.engineName, "RealRTCW", XR_MAX_ENGINE_NAME_SIZE );
	ici.applicationInfo.applicationVersion = 1;
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = enabled;

	if ( !VR_OK( qxrCreateInstance( &ici, &xr.instance ) ) ) {
		return qfalse;
	}
	if ( !VR_LoadInstanceFuncs() ) {
		return qfalse;
	}

	Com_Memset( &props, 0, sizeof( props ) );
	props.type = XR_TYPE_INSTANCE_PROPERTIES;
	if ( XR_SUCCEEDED( qxrGetInstanceProperties( xr.instance, &props ) ) ) {
		Com_Printf( "VR: OpenXR runtime %s %u.%u.%u\n", props.runtimeName,
					XR_VERSION_MAJOR( props.runtimeVersion ), XR_VERSION_MINOR( props.runtimeVersion ),
					XR_VERSION_PATCH( props.runtimeVersion ) );
	}

	Com_Memset( &sgi, 0, sizeof( sgi ) );
	sgi.type = XR_TYPE_SYSTEM_GET_INFO;
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if ( XR_FAILED( qxrGetSystem( xr.instance, &sgi, &xr.systemId ) ) ) {
		Com_Printf( "VR: no headset found, is it connected and switched on? Running on the monitor\n" );
		return qfalse;
	}

	Com_Memset( &sysProps, 0, sizeof( sysProps ) );
	sysProps.type = XR_TYPE_SYSTEM_PROPERTIES;
	if ( XR_SUCCEEDED( qxrGetSystemProperties( xr.instance, xr.systemId, &sysProps ) ) ) {
		Com_Printf( "VR: headset %s\n", sysProps.systemName );
	}

	xr.configViews[0].type = xr.configViews[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if ( !VR_OK( qxrEnumerateViewConfigurationViews( xr.instance, xr.systemId,
				 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, xr.configViews ) ) || viewCount != 2 ) {
		return qfalse;
	}

	scale = vr_resolutionScale->value;
	if ( scale < 0.3f ) {
		scale = 0.3f;
	} else if ( scale > 2.0f ) {
		scale = 2.0f;
	}
	xr.width = (int)( xr.configViews[0].recommendedImageRectWidth * scale ) & ~1;
	xr.height = (int)( xr.configViews[0].recommendedImageRectHeight * scale ) & ~1;
	if ( xr.width > (int)xr.configViews[0].maxImageRectWidth ) {
		xr.width = xr.configViews[0].maxImageRectWidth;
	}
	if ( xr.height > (int)xr.configViews[0].maxImageRectHeight ) {
		xr.height = xr.configViews[0].maxImageRectHeight;
	}

	Com_Printf( "VR: recommended %ux%u per eye, rendering %dx%d\n",
				xr.configViews[0].recommendedImageRectWidth, xr.configViews[0].recommendedImageRectHeight,
				xr.width, xr.height );
	return qtrue;
}

/*
==============================================================================

Input actions

==============================================================================
*/

static XrPath VR_Path( const char *s ) {
	XrPath p = XR_NULL_PATH;
	qxrStringToPath( xr.instance, s, &p );
	return p;
}

static void VR_CreateAction( XrAction *action, XrActionType type, const char *name, const char *localized ) {
	XrActionCreateInfo aci;

	Com_Memset( &aci, 0, sizeof( aci ) );
	aci.type = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = type;
	Q_strncpyz( aci.actionName, name, XR_MAX_ACTION_NAME_SIZE );
	Q_strncpyz( aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE );
	aci.countSubactionPaths = 2;
	aci.subactionPaths = xr.handPath;
	VR_OK( qxrCreateAction( xr.actionSet, &aci, action ) );
}

typedef struct {
	XrAction   *action;
	const char *path;
} vrBinding_t;

static void VR_SuggestBindings( const char *profile, const vrBinding_t *bindings, int count ) {
	XrActionSuggestedBinding sb[32];
	XrInteractionProfileSuggestedBinding ipsb;
	int i;

	for ( i = 0; i < count && i < ARRAY_LEN( sb ); i++ ) {
		sb[i].action = *bindings[i].action;
		sb[i].binding = VR_Path( bindings[i].path );
	}

	Com_Memset( &ipsb, 0, sizeof( ipsb ) );
	ipsb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	ipsb.interactionProfile = VR_Path( profile );
	ipsb.suggestedBindings = sb;
	ipsb.countSuggestedBindings = i;
	if ( XR_FAILED( qxrSuggestInteractionProfileBindings( xr.instance, &ipsb ) ) ) {
		Com_DPrintf( "VR: runtime rejected bindings for %s\n", profile );
	}
}

#define L( p ) "/user/hand/left/" p
#define R( p ) "/user/hand/right/" p

static qboolean VR_CreateActions( void ) {
	XrActionSetCreateInfo asci;
	XrActionSpaceCreateInfo spaceInfo;
	XrSessionActionSetsAttachInfo attach;
	int h;

	static const vrBinding_t touch[] = {
		{ &xr.actGrip, L( "input/grip/pose" ) },          { &xr.actGrip, R( "input/grip/pose" ) },
		{ &xr.actAim, L( "input/aim/pose" ) },            { &xr.actAim, R( "input/aim/pose" ) },
		{ &xr.actTrigger, L( "input/trigger/value" ) },   { &xr.actTrigger, R( "input/trigger/value" ) },
		{ &xr.actSqueeze, L( "input/squeeze/value" ) },   { &xr.actSqueeze, R( "input/squeeze/value" ) },
		{ &xr.actStick, L( "input/thumbstick" ) },        { &xr.actStick, R( "input/thumbstick" ) },
		{ &xr.actStickClick, L( "input/thumbstick/click" ) }, { &xr.actStickClick, R( "input/thumbstick/click" ) },
		{ &xr.actLower, L( "input/x/click" ) },           { &xr.actLower, R( "input/a/click" ) },
		{ &xr.actUpper, L( "input/y/click" ) },           { &xr.actUpper, R( "input/b/click" ) },
		{ &xr.actMenu, L( "input/menu/click" ) },
		{ &xr.actHaptic, L( "output/haptic" ) },          { &xr.actHaptic, R( "output/haptic" ) },
	};
	static const vrBinding_t index[] = {
		{ &xr.actGrip, L( "input/grip/pose" ) },          { &xr.actGrip, R( "input/grip/pose" ) },
		{ &xr.actAim, L( "input/aim/pose" ) },            { &xr.actAim, R( "input/aim/pose" ) },
		{ &xr.actTrigger, L( "input/trigger/value" ) },   { &xr.actTrigger, R( "input/trigger/value" ) },
		{ &xr.actSqueeze, L( "input/squeeze/value" ) },   { &xr.actSqueeze, R( "input/squeeze/value" ) },
		{ &xr.actStick, L( "input/thumbstick" ) },        { &xr.actStick, R( "input/thumbstick" ) },
		{ &xr.actStickClick, L( "input/thumbstick/click" ) }, { &xr.actStickClick, R( "input/thumbstick/click" ) },
		{ &xr.actLower, L( "input/a/click" ) },           { &xr.actLower, R( "input/a/click" ) },
		{ &xr.actUpper, R( "input/b/click" ) },           { &xr.actMenu, L( "input/b/click" ) },
		{ &xr.actHaptic, L( "output/haptic" ) },          { &xr.actHaptic, R( "output/haptic" ) },
	};
	static const vrBinding_t simple[] = {
		{ &xr.actGrip, L( "input/grip/pose" ) },          { &xr.actGrip, R( "input/grip/pose" ) },
		{ &xr.actAim, L( "input/aim/pose" ) },            { &xr.actAim, R( "input/aim/pose" ) },
		{ &xr.actTrigger, L( "input/select/click" ) },    { &xr.actTrigger, R( "input/select/click" ) },
		{ &xr.actMenu, L( "input/menu/click" ) },
		{ &xr.actHaptic, L( "output/haptic" ) },          { &xr.actHaptic, R( "output/haptic" ) },
	};

	xr.handPath[VR_HAND_LEFT] = VR_Path( "/user/hand/left" );
	xr.handPath[VR_HAND_RIGHT] = VR_Path( "/user/hand/right" );

	Com_Memset( &asci, 0, sizeof( asci ) );
	asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	Q_strncpyz( asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE );
	Q_strncpyz( asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE );
	if ( !VR_OK( qxrCreateActionSet( xr.instance, &asci, &xr.actionSet ) ) ) {
		return qfalse;
	}

	VR_CreateAction( &xr.actGrip, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand pose" );
	VR_CreateAction( &xr.actAim, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointer pose" );
	VR_CreateAction( &xr.actTrigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger" );
	VR_CreateAction( &xr.actSqueeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip" );
	VR_CreateAction( &xr.actStick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick" );
	VR_CreateAction( &xr.actStickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick click" );
	VR_CreateAction( &xr.actLower, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "A / X" );
	VR_CreateAction( &xr.actUpper, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "B / Y" );
	VR_CreateAction( &xr.actMenu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu" );
	VR_CreateAction( &xr.actHaptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration" );

	VR_SuggestBindings( "/interaction_profiles/oculus/touch_controller", touch, ARRAY_LEN( touch ) );
	VR_SuggestBindings( "/interaction_profiles/valve/index_controller", index, ARRAY_LEN( index ) );
	VR_SuggestBindings( "/interaction_profiles/khr/simple_controller", simple, ARRAY_LEN( simple ) );

	for ( h = 0; h < 2; h++ ) {
		Com_Memset( &spaceInfo, 0, sizeof( spaceInfo ) );
		spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
		spaceInfo.subactionPath = xr.handPath[h];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;
		spaceInfo.action = xr.actGrip;
		VR_OK( qxrCreateActionSpace( xr.session, &spaceInfo, &xr.gripSpace[h] ) );
		spaceInfo.action = xr.actAim;
		VR_OK( qxrCreateActionSpace( xr.session, &spaceInfo, &xr.aimSpace[h] ) );
	}

	Com_Memset( &attach, 0, sizeof( attach ) );
	attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attach.countActionSets = 1;
	attach.actionSets = &xr.actionSet;
	return VR_OK( qxrAttachSessionActionSets( xr.session, &attach ) );
}

#undef L
#undef R

static float VR_GetFloat( XrAction action, int hand ) {
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
	gi.action = action;
	gi.subactionPath = xr.handPath[hand];
	if ( XR_FAILED( qxrGetActionStateFloat( xr.session, &gi, &st ) ) || !st.isActive ) {
		return 0.0f;
	}
	return st.currentState;
}

static qboolean VR_GetBool( XrAction action, int hand ) {
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
	gi.action = action;
	gi.subactionPath = xr.handPath[hand];
	if ( XR_FAILED( qxrGetActionStateBoolean( xr.session, &gi, &st ) ) || !st.isActive ) {
		return qfalse;
	}
	return st.currentState ? qtrue : qfalse;
}

static void VR_GetStick( int hand, float out[2] ) {
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
	gi.action = xr.actStick;
	gi.subactionPath = xr.handPath[hand];
	out[0] = out[1] = 0.0f;
	if ( XR_SUCCEEDED( qxrGetActionStateVector2f( xr.session, &gi, &st ) ) && st.isActive ) {
		out[0] = st.currentState.x;
		out[1] = st.currentState.y;
	}
}

static qboolean VR_LocatePose( XrSpace space, XrSpace base, XrPosef *pose ) {
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;

	if ( space == XR_NULL_HANDLE || XR_FAILED( qxrLocateSpace( space, base, xr.frameState.predictedDisplayTime, &loc ) ) ) {
		return qfalse;
	}
	if ( ( loc.locationFlags & need ) != need ) {
		return qfalse;
	}
	*pose = loc.pose;
	return qtrue;
}

static void VR_ReadInput( void ) {
	XrActiveActionSet active;
	XrActionsSyncInfo sync;
	int h, b;

	if ( xr.state != XR_SESSION_STATE_FOCUSED ) {
		// the runtime owns input (system menu etc.): treat everything as released
		for ( h = 0; h < 2; h++ ) {
			Com_Memcpy( xr.hands[h].wasDown, xr.hands[h].down, sizeof( xr.hands[h].down ) );
			Com_Memset( xr.hands[h].down, 0, sizeof( xr.hands[h].down ) );
			xr.hands[h].trigger = xr.hands[h].squeeze = 0.0f;
			xr.hands[h].stick[0] = xr.hands[h].stick[1] = 0.0f;
		}
		return;
	}

	active.actionSet = xr.actionSet;
	active.subactionPath = XR_NULL_PATH;
	Com_Memset( &sync, 0, sizeof( sync ) );
	sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	qxrSyncActions( xr.session, &sync );

	for ( h = 0; h < 2; h++ ) {
		vrHand_t *hand = &xr.hands[h];
		float dz = vr_deadzone->value;

		Com_Memcpy( hand->wasDown, hand->down, sizeof( hand->down ) );

		hand->trigger = VR_GetFloat( xr.actTrigger, h );
		hand->squeeze = VR_GetFloat( xr.actSqueeze, h );
		VR_GetStick( h, hand->stick );

		// analog -> digital with hysteresis
		// a light pull fires, like a real trigger's break; released well below that
		hand->down[VRB_TRIGGER] = hand->trigger > ( hand->wasDown[VRB_TRIGGER] ? 0.15f : 0.25f );
		hand->down[VRB_SQUEEZE] = hand->squeeze > ( hand->wasDown[VRB_SQUEEZE] ? 0.35f : 0.6f );
		hand->down[VRB_LOWER] = VR_GetBool( xr.actLower, h );
		hand->down[VRB_UPPER] = VR_GetBool( xr.actUpper, h );
		hand->down[VRB_STICK] = VR_GetBool( xr.actStickClick, h );
		hand->down[VRB_MENU] = VR_GetBool( xr.actMenu, h );
		hand->down[VRB_STICK_UP] = hand->stick[1] > ( hand->wasDown[VRB_STICK_UP] ? 0.5f : 0.75f );
		hand->down[VRB_STICK_DOWN] = hand->stick[1] < -( hand->wasDown[VRB_STICK_DOWN] ? 0.5f : 0.75f );
		hand->down[VRB_STICK_RIGHT] = hand->stick[0] > ( hand->wasDown[VRB_STICK_RIGHT] ? 0.5f : 0.75f );
		hand->down[VRB_STICK_LEFT] = hand->stick[0] < -( hand->wasDown[VRB_STICK_LEFT] ? 0.5f : 0.75f );

		for ( b = 0; b < VRB_COUNT; b++ ) {
			if ( hand->down[b] && !hand->wasDown[b] ) {
				hand->downTime[b] = cls.realtime;
			}
		}

		if ( fabs( hand->stick[0] ) < dz ) {
			hand->stick[0] = 0.0f;
		}
		if ( fabs( hand->stick[1] ) < dz ) {
			hand->stick[1] = 0.0f;
		}

		hand->gripValid = VR_LocatePose( xr.gripSpace[h], xr.playSpace, &hand->grip );
		hand->aimValid = VR_LocatePose( xr.aimSpace[h], xr.playSpace, &hand->aim );
	}
}

static qboolean VR_Pressed( int hand, vrButton_t b ) {
	return xr.hands[hand].down[b] && !xr.hands[hand].wasDown[b];
}

static qboolean VR_Released( int hand, vrButton_t b ) {
	return !xr.hands[hand].down[b] && xr.hands[hand].wasDown[b];
}

static void VR_Haptic( int hand, float amplitude, int msec ) {
	XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };

	if ( !xr.sessionRunning || xr.state != XR_SESSION_STATE_FOCUSED ) {
		return;
	}
	amplitude *= vr_hapticScale->value;
	if ( amplitude <= 0.0f ) {
		return;
	}
	hai.action = xr.actHaptic;
	hai.subactionPath = xr.handPath[hand];
	vib.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
	vib.duration = (XrDuration)msec * 1000000;
	vib.frequency = XR_FREQUENCY_UNSPECIFIED;
	qxrApplyHapticFeedback( xr.session, &hai, (XrHapticBaseHeader *)&vib );
}

/*
==============================================================================

Swapchains

==============================================================================
*/

static qboolean VR_CreateSwapchain( vrSwapchain_t *sc, int width, int height ) {
	XrSwapchainCreateInfo sci;
	uint32_t i;

	Com_Memset( sc, 0, sizeof( *sc ) );
	Com_Memset( &sci, 0, sizeof( sci ) );
	sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = xr.colorFormat;
	sci.sampleCount = 1;
	sci.width = width;
	sci.height = height;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	if ( !VR_OK( qxrCreateSwapchain( xr.session, &sci, &sc->handle ) ) ) {
		return qfalse;
	}
	sc->width = width;
	sc->height = height;

	for ( i = 0; i < VR_MAX_SWAPCHAIN_IMAGES; i++ ) {
		sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		sc->images[i].next = NULL;
	}
	if ( !VR_OK( qxrEnumerateSwapchainImages( sc->handle, 0, &sc->imageCount, NULL ) ) ) {
		return qfalse;
	}
	if ( sc->imageCount > VR_MAX_SWAPCHAIN_IMAGES ) {
		Com_Printf( "VR: too many swapchain images (%u)\n", sc->imageCount );
		return qfalse;
	}
	if ( !VR_OK( qxrEnumerateSwapchainImages( sc->handle, sc->imageCount, &sc->imageCount,
			   (XrSwapchainImageBaseHeader *)sc->images ) ) ) {
		return qfalse;
	}

	qglGenFramebuffers( sc->imageCount, sc->fbos );
	for ( i = 0; i < sc->imageCount; i++ ) {
		qglBindFramebuffer( GL_FRAMEBUFFER, sc->fbos[i] );
		qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[i].image, 0 );
		if ( qglCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) {
			Com_Printf( "VR: swapchain framebuffer incomplete\n" );
			qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
			return qfalse;
		}
	}
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	return qtrue;
}

static void VR_DestroySwapchain( vrSwapchain_t *sc ) {
	if ( sc->imageCount && qglDeleteFramebuffers ) {
		qglDeleteFramebuffers( sc->imageCount, sc->fbos );
	}
	if ( sc->handle != XR_NULL_HANDLE ) {
		qxrDestroySwapchain( sc->handle );
	}
	Com_Memset( sc, 0, sizeof( *sc ) );
}

static GLuint VR_AcquireSwapchain( vrSwapchain_t *sc ) {
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if ( XR_FAILED( qxrAcquireSwapchainImage( sc->handle, &ai, &sc->current ) ) ) {
		return 0;
	}
	wi.timeout = XR_INFINITE_DURATION;
	if ( XR_FAILED( qxrWaitSwapchainImage( sc->handle, &wi ) ) ) {
		return 0;
	}
	sc->acquired = qtrue;
	return sc->fbos[sc->current];
}

static void VR_ReleaseSwapchain( vrSwapchain_t *sc ) {
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if ( sc->acquired ) {
		qxrReleaseSwapchainImage( sc->handle, &ri );
		sc->acquired = qfalse;
	}
}

static int64_t VR_PickColorFormat( void ) {
	int64_t formats[64];
	uint32_t count = 0, i;

	if ( XR_FAILED( qxrEnumerateSwapchainFormats( xr.session, ARRAY_LEN( formats ), &count, formats ) ) ) {
		return GL_SRGB8_ALPHA8;
	}
	// the game renders gamma encoded colour: an sRGB swapchain written without
	// GL_FRAMEBUFFER_SRGB stores it untouched and the compositor reads it correctly
	for ( i = 0; i < count; i++ ) {
		if ( formats[i] == GL_SRGB8_ALPHA8 ) {
			return GL_SRGB8_ALPHA8;
		}
	}
	for ( i = 0; i < count; i++ ) {
		if ( formats[i] == GL_RGBA8 ) {
			return GL_RGBA8;
		}
	}
	return count ? formats[0] : GL_SRGB8_ALPHA8;
}

/*
==============================================================================

Session

==============================================================================
*/

static qboolean VR_CreatePlaySpace( void ) {
	XrReferenceSpaceType types[8];
	uint32_t count = 0, i;
	XrReferenceSpaceCreateInfo rsci;

	xr.playSpaceIsStage = qfalse;
	if ( XR_SUCCEEDED( qxrEnumerateReferenceSpaces( xr.session, ARRAY_LEN( types ), &count, types ) ) ) {
		for ( i = 0; i < count; i++ ) {
			if ( types[i] == XR_REFERENCE_SPACE_TYPE_STAGE ) {
				xr.playSpaceIsStage = qtrue;
			}
		}
	}

	Com_Memset( &rsci, 0, sizeof( rsci ) );
	rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	rsci.referenceSpaceType = xr.playSpaceIsStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
	if ( !VR_OK( qxrCreateReferenceSpace( xr.session, &rsci, &xr.playSpace ) ) ) {
		return qfalse;
	}

	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if ( !VR_OK( qxrCreateReferenceSpace( xr.session, &rsci, &xr.viewSpace ) ) ) {
		return qfalse;
	}

	Com_Printf( "VR: using %s tracking space\n", xr.playSpaceIsStage ? "stage (room scale)" : "local (seated)" );
	return qtrue;
}

/*
=================
CL_VR_InitSession

Called once the renderer has created its window and GL context.
=================
*/
void CL_VR_InitSession( void ) {
	XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	XrSessionCreateInfo sci;

	if ( !xr.instanceReady || xr.session != XR_NULL_HANDLE ) {
		return;
	}

	if ( !VR_LoadGL() ) {
		Com_Printf( S_COLOR_YELLOW "VR: required OpenGL framebuffer functions are missing\n" );
		return;
	}

	// mandatory before xrCreateSession
	qxrGetOpenGLGraphicsRequirementsKHR( xr.instance, xr.systemId, &req );

	binding.hDC = qwglGetCurrentDC();
	binding.hGLRC = qwglGetCurrentContext();
	if ( !binding.hDC || !binding.hGLRC ) {
		Com_Printf( S_COLOR_YELLOW "VR: no current GL context\n" );
		return;
	}

	Com_Memset( &sci, 0, sizeof( sci ) );
	sci.type = XR_TYPE_SESSION_CREATE_INFO;
	sci.next = &binding;
	sci.systemId = xr.systemId;
	if ( !VR_OK( qxrCreateSession( xr.instance, &sci, &xr.session ) ) ) {
		return;
	}

	xr.colorFormat = VR_PickColorFormat();

	if ( !VR_CreatePlaySpace() ||
		 !VR_CreateSwapchain( &xr.eyes[0], xr.width, xr.height ) ||
		 !VR_CreateSwapchain( &xr.eyes[1], xr.width, xr.height ) ||
		 !VR_CreateSwapchain( &xr.ui, xr.width, xr.height ) ||
		 !VR_CreateActions() ) {
		Com_Printf( S_COLOR_YELLOW "VR: session setup failed\n" );
		CL_VR_ShutdownSession();
		return;
	}

	xr.state = XR_SESSION_STATE_IDLE;
	xr.menuPlaced = qfalse;
	xr.lastRawHeadValid = qfalse;
	Com_Printf( "VR: session created\n" );
}

/*
=================
CL_VR_ShutdownSession

Must run before the renderer destroys its GL context.
=================
*/
void CL_VR_ShutdownSession( void ) {
	int h;

	if ( xr.session == XR_NULL_HANDLE ) {
		return;
	}

	VR_DestroySwapchain( &xr.eyes[0] );
	VR_DestroySwapchain( &xr.eyes[1] );
	VR_DestroySwapchain( &xr.ui );

	for ( h = 0; h < 2; h++ ) {
		if ( xr.gripSpace[h] ) {
			qxrDestroySpace( xr.gripSpace[h] );
		}
		if ( xr.aimSpace[h] ) {
			qxrDestroySpace( xr.aimSpace[h] );
		}
		xr.gripSpace[h] = xr.aimSpace[h] = XR_NULL_HANDLE;
	}
	if ( xr.actionSet ) {
		qxrDestroyActionSet( xr.actionSet );
		xr.actionSet = XR_NULL_HANDLE;
	}
	if ( xr.playSpace ) {
		qxrDestroySpace( xr.playSpace );
		xr.playSpace = XR_NULL_HANDLE;
	}
	if ( xr.viewSpace ) {
		qxrDestroySpace( xr.viewSpace );
		xr.viewSpace = XR_NULL_HANDLE;
	}

	qxrDestroySession( xr.session );
	xr.session = XR_NULL_HANDLE;
	xr.sessionRunning = qfalse;
	xr.frameBegun = qfalse;
	com_vr.active = qfalse;
	com_vr.pass = VR_PASS_FLAT;
}

static void VR_PollEvents( void ) {
	XrEventDataBuffer ev;

	for ( ;; ) {
		Com_Memset( &ev, 0, sizeof( ev ) );
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		if ( qxrPollEvent( xr.instance, &ev ) != XR_SUCCESS ) {
			break;
		}

		switch ( ev.type ) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
			xr.state = sc->state;
			if ( vr_debug->integer ) {
				Com_Printf( "VR: session state %d\n", (int)sc->state );
			}

			if ( sc->state == XR_SESSION_STATE_READY ) {
				XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xr.sessionRunning = VR_OK( qxrBeginSession( xr.session, &bi ) );
			} else if ( sc->state == XR_SESSION_STATE_STOPPING ) {
				qxrEndSession( xr.session );
				xr.sessionRunning = qfalse;
			} else if ( sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING ) {
				Com_Printf( "VR: the runtime ended the session, continuing on the monitor\n" );
				xr.sessionRunning = qfalse;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			Com_Printf( "VR: OpenXR runtime is going away\n" );
			xr.sessionRunning = qfalse;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			// the user recentred: reposition the menu panel too
			xr.menuPlaced = qfalse;
			xr.lastRawHeadValid = qfalse;
			break;
		default:
			break;
		}
	}
}

/*
==============================================================================

Init / shutdown

==============================================================================
*/

/*
=================
CL_VR_Init

Runs before the renderer starts, so it can size the renderer for the headset.
=================
*/
void CL_VR_Init( void ) {
	vr_enable = Cvar_Get( "vr_enable", "1", CVAR_LATCH );
	// never saved: a one-off flat launch must not stick (an old "seta" line in a config
	// would otherwise keep re-marking it for saving)
	vr_enable->flags &= ~CVAR_ARCHIVE;
	vr_resolutionScale = Cvar_Get( "vr_resolutionScale", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	vr_worldScale = Cvar_Get( "vr_worldScale", "39.37", CVAR_ARCHIVE );
	vr_rightHanded = Cvar_Get( "vr_rightHanded", "1", CVAR_ARCHIVE );
	vr_snapTurn = Cvar_Get( "vr_snapTurn", "45", CVAR_ARCHIVE );
	vr_turnSpeed = Cvar_Get( "vr_turnSpeed", "180", CVAR_ARCHIVE );
	vr_walkDirection = Cvar_Get( "vr_walkDirection", "0", CVAR_ARCHIVE );
	vr_deadzone = Cvar_Get( "vr_deadzone", "0.15", CVAR_ARCHIVE );
	vr_roomScale = Cvar_Get( "vr_roomScale", "1", CVAR_ARCHIVE );
	vr_weaponPitch = Cvar_Get( "vr_weaponPitch", "0", CVAR_ARCHIVE );
	vr_menuDistance = Cvar_Get( "vr_menuDistance", "2.0", CVAR_ARCHIVE );
	vr_menuWidth = Cvar_Get( "vr_menuWidth", "2.4", CVAR_ARCHIVE );
	vr_hudDistance = Cvar_Get( "vr_hudDistance", "1.3", CVAR_ARCHIVE );
	vr_hudWidth = Cvar_Get( "vr_hudWidth", "1.5", CVAR_ARCHIVE );
	vr_hudHeight = Cvar_Get( "vr_hudHeight", "-0.05", CVAR_ARCHIVE );
	vr_mirror = Cvar_Get( "vr_mirror", "1", CVAR_ARCHIVE );
	vr_hapticScale = Cvar_Get( "vr_hapticScale", "1.0", CVAR_ARCHIVE );
	vr_debug = Cvar_Get( "vr_debug", "0", 0 );
	Cvar_Get( "vr_renderWidth", "0", CVAR_ROM );
	Cvar_Get( "vr_renderHeight", "0", CVAR_ROM );

	// settings owned by the game modules, created here so the VR options menu works before
	// a level is loaded (same defaults as where they are used)
	Cvar_Get( "vr_manualReload", "1", CVAR_ARCHIVE );   // g_vr.c
	Cvar_Get( "vr_emptySwitch", "0", CVAR_ARCHIVE );    // cg_event.c
	Cvar_Get( "vr_aimSpread", "0.25", CVAR_ARCHIVE );   // g_weapon.c
	Cvar_Get( "vr_reticle", "1", CVAR_ARCHIVE );        // cg_vr.c
	Cvar_Get( "vr_drawHands", "1", CVAR_ARCHIVE );      // cg_vr.c
	Cvar_Get( "vr_ragdolls", "1", CVAR_ARCHIVE );       // cg_ragdoll.c
	Cvar_Get( "vr_fullBody", "1", CVAR_ARCHIVE );       // cg_vrbody.c
	Cvar_Get( "vr_scopes", "1", CVAR_ARCHIVE );         // cg_vrscope.c
	Cvar_Get( "vr_recoil", "1", CVAR_ARCHIVE );         // cg_vrrecoil.c
	vr_binoculars = Cvar_Get( "vr_binoculars", "1", CVAR_ARCHIVE );
	vr_hipNotebook = Cvar_Get( "vr_hipNotebook", "1", CVAR_ARCHIVE );
	vr_teleport = Cvar_Get( "vr_teleport", "0", CVAR_ARCHIVE );
	vr_seated = Cvar_Get( "vr_seated", "0", CVAR_ARCHIVE );
	vr_seatedHeight = Cvar_Get( "vr_seatedHeight", "0.5", CVAR_ARCHIVE );
	vr_crouchHeight = Cvar_Get( "vr_crouchHeight", "1.1", CVAR_ARCHIVE );
	Cvar_Get( "vr_vignette", "0", CVAR_ARCHIVE );            // cg_vrcomfort.c
	Cvar_Get( "vr_wristHud", "1", CVAR_ARCHIVE );           // cg_vrwrist.c
	Cvar_Get( "vr_pouchMags", "1", CVAR_ARCHIVE );          // spare magazines shown in the pouch
	Cvar_Get( "vr_weaponScale", "1", CVAR_ARCHIVE );        // guns and hands: 1, 0.7 or 0.42 (life size)
	VR_InitBindings();
	Cbuf_AddText( "exec vr_controls.cfg\n" );              // the player's button layout

	Com_Memset( &xr, 0, sizeof( xr ) );
	Com_Memset( &com_vr, 0, sizeof( com_vr ) );
	com_vr.grabbedEntity = -1;
	com_vr.chambered = qtrue;
	com_vr.worldScale = vr_worldScale->value;
	com_vr.rightHanded = 1;

	if ( !vr_enable->integer ) {
		Com_Printf( "VR: disabled (vr_enable 0)\n" );
		return;
	}

	if ( !VR_LoadLoader() || !VR_CreateInstance() ) {
		if ( xr.instance ) {
			qxrDestroyInstance( xr.instance );
			xr.instance = XR_NULL_HANDLE;
		}
		return;
	}

	xr.instanceReady = qtrue;
	Cvar_Set( "vr_renderWidth", va( "%d", xr.width ) );
	Cvar_Set( "vr_renderHeight", va( "%d", xr.height ) );

	// settings the headset path depends on
	Cvar_Set( "r_fbo", "1" );               // eyes are composited through the offscreen FBO
	Cvar_Set( "r_swapInterval", "0" );      // xrWaitFrame paces the game, not the monitor
	Cvar_Set( "com_maxfps", "0" );
	Cvar_Set( "ui_fixedAspect", "0" );      // 640x480 UI stretched over the 4:3 panel
	Cvar_Set( "cg_fixedAspect", "0" );
	Cvar_Set( "cg_viewsize", "100" );
	Cvar_Set( "r_znear", "1" );             // hands and guns are close to the eyes
}

void CL_VR_Shutdown( void ) {
	CL_VR_ShutdownSession();
	if ( xr.instance ) {
		qxrDestroyInstance( xr.instance );
		xr.instance = XR_NULL_HANDLE;
	}
	if ( xr.loader ) {
		FreeLibrary( xr.loader );
		xr.loader = NULL;
	}
	xr.instanceReady = qfalse;
}

qboolean CL_VR_Active( void ) {
	return xr.session != XR_NULL_HANDLE && xr.sessionRunning;
}

/*
==============================================================================

Per frame: tracking

==============================================================================
*/

static void VR_UpdateTracking( void ) {
	XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	uint32_t count = 0;
	int h, dom;
	vec3_t rawHead;

	xr.views[0].type = xr.views[1].type = XR_TYPE_VIEW;
	xr.headViews[0].type = xr.headViews[1].type = XR_TYPE_VIEW;

	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = xr.frameState.predictedDisplayTime;
	vli.space = xr.playSpace;
	qxrLocateViews( xr.session, &vli, &vs, 2, &count, xr.views );
	vli.space = xr.viewSpace;
	qxrLocateViews( xr.session, &vli, &vs, 2, &count, xr.headViews );

	xr.headValid = VR_LocatePose( xr.viewSpace, xr.playSpace, &xr.head );
	if ( !xr.headValid ) {
		return;
	}

	VR_ReadInput();

	// artificial turning before poses are converted, so everything agrees this frame
	dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	com_vr.worldScale = vr_worldScale->value;
	com_vr.rightHanded = vr_rightHanded->integer ? 1 : 0;

	// room-scale: head movement since the last frame, in the turned play space
	VR_XrPosToQuake( &xr.head.position, rawHead );
	if ( xr.lastRawHeadValid ) {
		vec3_t delta, turned;
		VectorSubtract( rawHead, xr.lastRawHead, delta );
		VR_RotateYaw( delta, xr.turnYaw, turned );
		xr.pendingMove[0] += turned[0];
		xr.pendingMove[1] += turned[1];
	}
	VectorCopy( rawHead, xr.lastRawHead );
	xr.lastRawHeadValid = qtrue;

	VR_PoseToPlay( &xr.head, com_vr.hmdOrigin, com_vr.hmdAngles );

	// hands sit at the grip pose (the palm), but point along the aim pose: the grip pose's
	// forward axis runs up the controller handle, the aim pose points like a gun barrel
	for ( h = 0; h < 2; h++ ) {
		com_vr.handValid[h] = xr.hands[h].gripValid;
		if ( xr.hands[h].gripValid ) {
			VR_PoseToPlay( &xr.hands[h].grip, com_vr.handOrigin[h], com_vr.handAngles[h] );
			if ( xr.hands[h].aimValid ) {
				vec3_t unused;
				VR_PoseToPlay( &xr.hands[h].aim, unused, com_vr.handAngles[h] );
			}
		}
	}

	// physical crouch: the head's real height, before seated play stands it up
	xr.headHeight = com_vr.hmdOrigin[2] / vr_worldScale->value;
	if ( vr_seated->integer ) {
		float up = vr_seatedHeight->value * vr_worldScale->value;
		com_vr.hmdOrigin[2] += up;
		com_vr.handOrigin[0][2] += up;
		com_vr.handOrigin[1][2] += up;
	}

	// weapon aim: the dominant grip pose, optionally steadied by the off hand
	if ( com_vr.handValid[dom] ) {
		VectorCopy( com_vr.handAngles[dom], com_vr.weaponAngles );
		com_vr.weaponAngles[PITCH] += vr_weaponPitch->value;

		// Two handed aim is decided when the off hand closes its grip: only a hold in front
		// of the gun hand, along the barrel, steadies the gun. Anywhere else (the bolt, the
		// slide, a magazine well) leaves the aim alone so the game can use the grab.
		if ( !xr.hands[!dom].down[VRB_SQUEEZE] || !com_vr.handValid[!dom] ) {
			xr.steadyLatched = qfalse;
		} else if ( !xr.hands[!dom].wasDown[VRB_SQUEEZE] ) {
			vec3_t rel, fwd, lateral;
			float along;

			VectorSubtract( com_vr.handOrigin[!dom], com_vr.handOrigin[dom], rel );
			AngleVectors( com_vr.weaponAngles, fwd, NULL, NULL );
			along = DotProduct( rel, fwd );
			VectorMA( rel, -along, fwd, lateral );
			xr.steadyLatched = along > 0.25f * vr_worldScale->value &&
							   VectorLength( lateral ) < 0.15f * vr_worldScale->value;
		}

		com_vr.twoHanded = qfalse;
		// (not while the off hand is busy reloading or carrying a body)
		if ( xr.steadyLatched && com_vr.twoHandAllowed &&
			 !com_vr.heldItem && !com_vr.chargeHeld && com_vr.grabbedEntity < 0 ) {
			vec3_t dir, ang;
			VectorSubtract( com_vr.handOrigin[!dom], com_vr.handOrigin[dom], dir );
			if ( VectorLength( dir ) > 0.12f * vr_worldScale->value ) {
				vectoangles( dir, ang );
				com_vr.weaponAngles[PITCH] = AngleNormalize180( ang[PITCH] );
				com_vr.weaponAngles[YAW] = ang[YAW];
				com_vr.twoHanded = qtrue;
			}
		}
		// recoil, from cgame (cg_vrrecoil.c): the shots go where the kicked gun points
		com_vr.weaponAngles[PITCH] += com_vr.recoilPitch;
	} else {
		// no controller: aim with the head
		VectorCopy( com_vr.hmdAngles, com_vr.weaponAngles );
	}
}

/*
=================
CL_VR_BeginFrame

Waits for the headset, then samples head, hands and buttons. Safe to call more than
once per frame; only the first call does work.
=================
*/
void CL_VR_BeginFrame( void ) {
	XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };

	if ( xr.session == XR_NULL_HANDLE ) {
		return;
	}

	VR_PollEvents();

	if ( !xr.sessionRunning ) {
		com_vr.active = qfalse;
		return;
	}
	if ( xr.frameBegun ) {
		return;
	}

	Com_Memset( &xr.frameState, 0, sizeof( xr.frameState ) );
	xr.frameState.type = XR_TYPE_FRAME_STATE;
	if ( !VR_OK( qxrWaitFrame( xr.session, &fwi, &xr.frameState ) ) ) {
		return;
	}
	if ( !VR_OK( qxrBeginFrame( xr.session, &fbi ) ) ) {
		return;
	}
	xr.frameBegun = qtrue;
	com_vr.active = qtrue;

	VR_UpdateTracking();
}

/*
==============================================================================

Controls

==============================================================================
*/

// "+command" style actions that need a matching "-command" when released
static void VR_ButtonCommand( const char *cmd, qboolean down ) {
	int i;

	if ( down ) {
		for ( i = 0; i < xr.numHeldCommands; i++ ) {
			if ( !strcmp( xr.heldCommands[i], cmd ) ) {
				return;
			}
		}
		if ( xr.numHeldCommands < ARRAY_LEN( xr.heldCommands ) ) {
			Q_strncpyz( xr.heldCommands[xr.numHeldCommands++], cmd, sizeof( xr.heldCommands[0] ) );
		}
		Cbuf_AddText( va( "+%s\n", cmd ) );
	} else {
		for ( i = 0; i < xr.numHeldCommands; i++ ) {
			if ( !strcmp( xr.heldCommands[i], cmd ) ) {
				xr.heldCommands[i][0] = 0;
				memmove( &xr.heldCommands[i], &xr.heldCommands[i + 1],
						 ( xr.numHeldCommands - i - 1 ) * sizeof( xr.heldCommands[0] ) );
				xr.numHeldCommands--;
				Cbuf_AddText( va( "-%s\n", cmd ) );
				return;
			}
		}
	}
}

static void VR_ClearActionHolds( void );

static void VR_ReleaseAllCommands( void ) {
	VR_ClearActionHolds();
	xr.attackHeld = qfalse;
	xr.attackOffHeld = qfalse;
	while ( xr.numHeldCommands > 0 ) {
		VR_ButtonCommand( xr.heldCommands[0], qfalse );
	}
}

static void VR_Hold( const char *cmd, int hand, vrButton_t b ) {
	if ( VR_Pressed( hand, b ) ) {
		VR_ButtonCommand( cmd, qtrue );
	} else if ( VR_Released( hand, b ) ) {
		VR_ButtonCommand( cmd, qfalse );
	}
}

/*
=================
VR_HandAtHip

The off hand down at the hip on its own side (the notebook's pocket).
=================
*/
static qboolean VR_HandAtHip( int hand ) {
	float rel[3], f[3], fwd[2], right[2], len, side, ahead;
	static const float minusZ[3] = { 0, 0, -1 };

	if ( !xr.hands[hand].gripValid || !xr.headValid ) {
		return qfalse;
	}
	rel[0] = xr.hands[hand].grip.position.x - xr.head.position.x;
	rel[1] = xr.hands[hand].grip.position.y - xr.head.position.y;
	rel[2] = xr.hands[hand].grip.position.z - xr.head.position.z;
	VR_QuatRotate( &xr.head.orientation, minusZ, f );     // where the head faces, level
	len = sqrt( f[0] * f[0] + f[2] * f[2] );
	if ( len < 0.01f ) {
		return qfalse;
	}
	fwd[0] = f[0] / len;
	fwd[1] = f[2] / len;
	right[0] = -fwd[1];
	right[1] = fwd[0];
	ahead = rel[0] * fwd[0] + rel[2] * fwd[1];
	side = rel[0] * right[0] + rel[2] * right[1];
	if ( hand == VR_HAND_LEFT ) {
		side = -side;
	}
	return rel[1] < -0.45f && rel[1] > -0.95f && side > 0.10f && ahead < 0.15f && ahead > -0.25f;
}

/*
==============================================================================

Remappable game controls

Each action is a cvar, vr_bind_<action>, holding one or more inputs separated by
spaces. vr_controls.cfg (exec'd at startup) sets them, so players on other
controllers can move things around. Input names, as on Quest Touch for a right
handed player (the weapon hand is "R"; vr_rightHanded 0 swaps the hands):

  A B          weapon hand lower / upper face button
  X Y          off hand lower / upper face button
  RTRIG LTRIG  triggers          RGRIP LGRIP  grips
  RCLICK LCLICK  stick clicks
  RUP RDOWN RLEFT RRIGHT / LUP LDOWN LLEFT LRIGHT  stick pushed that way
  G+<input>    only while the weapon hand grip is held

A plain input does nothing while the weapon hand grip is held, so the same button
can carry a plain action and a G+ action. Fire, the off hand trigger (sprint, or
the second gun), grip tap reload, stick movement and turning are not remapped.

==============================================================================
*/

typedef enum {
	VRA_JUMP,
	VRA_CROUCH,
	VRA_STAND,
	VRA_USE,
	VRA_RELOAD,
	VRA_WEAPNEXT,
	VRA_WEAPPREV,
	VRA_KICK,
	VRA_NOTEBOOK,
	VRA_HUD,
	VRA_USEITEM,
	VRA_ITEMNEXT,
	VRA_ALTFIRE,
	VRA_COUNT
} vrAction_t;

static const struct {
	const char *cvar;
	const char *def;
} vrActions[VRA_COUNT] = {
	{ "vr_bind_jump",     "A" },
	{ "vr_bind_crouch",   "RDOWN" },
	{ "vr_bind_stand",    "RUP" },
	{ "vr_bind_use",      "X" },
	{ "vr_bind_reload",   "B Y" },
	{ "vr_bind_weapnext", "RCLICK" },
	{ "vr_bind_weapprev", "G+RCLICK" },
	{ "vr_bind_kick",     "LCLICK" },
	{ "vr_bind_notebook", "G+Y" },
	{ "vr_bind_hud",      "G+A" },
	{ "vr_bind_useitem",  "G+X" },
	{ "vr_bind_itemnext", "G+B" },
	{ "vr_bind_altfire",  "G+RTRIG" },
};

static const struct {
	const char *name;
	qboolean    offHand;
	vrButton_t  button;
} vrInputs[] = {
	{ "A", qfalse, VRB_LOWER },        { "B", qfalse, VRB_UPPER },
	{ "X", qtrue, VRB_LOWER },         { "Y", qtrue, VRB_UPPER },
	{ "RTRIG", qfalse, VRB_TRIGGER },  { "LTRIG", qtrue, VRB_TRIGGER },
	{ "RGRIP", qfalse, VRB_SQUEEZE },  { "LGRIP", qtrue, VRB_SQUEEZE },
	{ "RCLICK", qfalse, VRB_STICK },   { "LCLICK", qtrue, VRB_STICK },
	{ "RUP", qfalse, VRB_STICK_UP },   { "RDOWN", qfalse, VRB_STICK_DOWN },
	{ "RLEFT", qfalse, VRB_STICK_LEFT }, { "RRIGHT", qfalse, VRB_STICK_RIGHT },
	{ "LUP", qtrue, VRB_STICK_UP },    { "LDOWN", qtrue, VRB_STICK_DOWN },
	{ "LLEFT", qtrue, VRB_STICK_LEFT }, { "LRIGHT", qtrue, VRB_STICK_RIGHT },
};

#define VR_MAX_ACTION_INPUTS 4

typedef struct {
	int         count;
	int         hand[VR_MAX_ACTION_INPUTS];         // 0 weapon hand, 1 off hand
	vrButton_t  button[VR_MAX_ACTION_INPUTS];
	qboolean    grip[VR_MAX_ACTION_INPUTS];         // needs the weapon hand grip
} vrActionBinding_t;

static cvar_t      *vr_bindCvars[VRA_COUNT];
static vrActionBinding_t vrBindings[VRA_COUNT];
static qboolean    vrActionHeld[VRA_COUNT];
static qboolean    vrTriggerBound;      // something uses the weapon trigger with the grip

static void VR_ParseBinding( int a ) {
	vrActionBinding_t *b = &vrBindings[a];
	char buf[MAX_CVAR_VALUE_STRING];
	char *tok, *p;
	int i;

	Com_Memset( b, 0, sizeof( *b ) );
	Q_strncpyz( buf, vr_bindCvars[a]->string, sizeof( buf ) );
	for ( p = buf; ( tok = strtok( p, " ,\t" ) ) != NULL && b->count < VR_MAX_ACTION_INPUTS; p = NULL ) {
		qboolean grip = qfalse;

		if ( !Q_stricmpn( tok, "G+", 2 ) ) {
			grip = qtrue;
			tok += 2;
		}
		for ( i = 0; i < ARRAY_LEN( vrInputs ); i++ ) {
			if ( !Q_stricmp( tok, vrInputs[i].name ) ) {
				break;
			}
		}
		if ( i == ARRAY_LEN( vrInputs ) ) {
			if ( Q_stricmp( tok, "none" ) ) {
				Com_Printf( "%s: unknown VR input \"%s\"\n", vr_bindCvars[a]->name, tok );
			}
			continue;
		}
		b->hand[b->count] = vrInputs[i].offHand;
		b->button[b->count] = vrInputs[i].button;
		b->grip[b->count] = grip;
		b->count++;
	}
}

static void VR_InitBindings( void ) {
	int a;

	for ( a = 0; a < VRA_COUNT; a++ ) {
		vr_bindCvars[a] = Cvar_Get( vrActions[a].cvar, vrActions[a].def, 0 );
		vr_bindCvars[a]->modified = qtrue;
	}
}

static void VR_UpdateBindings( void ) {
	int a, i;

	vrTriggerBound = qfalse;
	for ( a = 0; a < VRA_COUNT; a++ ) {
		if ( vr_bindCvars[a]->modified ) {
			vr_bindCvars[a]->modified = qfalse;
			VR_ParseBinding( a );
		}
		for ( i = 0; i < vrBindings[a].count; i++ ) {
			if ( vrBindings[a].hand[i] == 0 && vrBindings[a].button[i] == VRB_TRIGGER && vrBindings[a].grip[i] ) {
				vrTriggerBound = qtrue;
			}
		}
	}
}

/*
=================
VR_ActionPressed / VR_ActionUpdate

Pressed: one of the action's inputs went down this frame (with the grip as asked).
Update: for held actions, also reports when the input that started it lets go.
=================
*/
static qboolean VR_ActionPressed( vrAction_t a ) {
	int dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	qboolean gripHeld = xr.hands[dom].down[VRB_SQUEEZE];
	const vrActionBinding_t *b = &vrBindings[a];
	int i;

	for ( i = 0; i < b->count; i++ ) {
		int hand = b->hand[i] ? !dom : dom;
		qboolean isGrip = !b->hand[i] && b->button[i] == VRB_SQUEEZE;

		if ( !VR_Pressed( hand, b->button[i] ) ) {
			continue;
		}
		if ( b->grip[i] ? gripHeld : ( !gripHeld || isGrip ) ) {
			if ( b->grip[i] ) {
				xr.rightGripCombo = qtrue;      // not a grip tap reload
			}
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean VR_ActionDown( vrAction_t a ) {
	int dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	const vrActionBinding_t *b = &vrBindings[a];
	int i;

	for ( i = 0; i < b->count; i++ ) {
		if ( xr.hands[b->hand[i] ? !dom : dom].down[b->button[i]] ) {
			return qtrue;
		}
	}
	return qfalse;
}

// a "+command" for as long as the action's input is held
static void VR_ActionHold( vrAction_t a, const char *cmd ) {
	if ( VR_ActionPressed( a ) ) {
		vrActionHeld[a] = qtrue;
		VR_ButtonCommand( cmd, qtrue );
	} else if ( vrActionHeld[a] && !VR_ActionDown( a ) ) {
		vrActionHeld[a] = qfalse;
		VR_ButtonCommand( cmd, qfalse );
	}
}

static void VR_ClearActionHolds( void ) {
	Com_Memset( vrActionHeld, 0, sizeof( vrActionHeld ) );
}

static qboolean VR_CommandHeld( const char *cmd ) {
	int i;

	for ( i = 0; i < xr.numHeldCommands; i++ ) {
		if ( !strcmp( xr.heldCommands[i], cmd ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

// in-game controls (see vr_controls.cfg for the layout)
static void VR_GameControls( void ) {
	int dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	int off = !dom;
	vrHand_t *d = &xr.hands[dom];
	qboolean modifier = d->down[VRB_SQUEEZE];

	VR_UpdateBindings();

	// weapon hand
	// firing doesn't go through the command buffer (+attack would only reach the usercmd
	// a frame later): CL_VR_CreateCmd sets the button directly
	if ( VR_Pressed( dom, VRB_TRIGGER ) ) {
		if ( modifier && vrTriggerBound ) {
			if ( VR_ActionPressed( VRA_ALTFIRE ) ) {
				Cbuf_AddText( "weapalt\n" );        // grip + trigger: alternate fire mode
			}
			xr.rightGripCombo = qtrue;
		} else {
			xr.attackHeld = qtrue;
			xr.akimboLastLeft = qfalse;
		}
	} else if ( !d->down[VRB_TRIGGER] ) {
		xr.attackHeld = qfalse;
	}
	if ( !VR_Pressed( dom, VRB_TRIGGER ) && VR_ActionPressed( VRA_ALTFIRE ) ) {
		Cbuf_AddText( "weapalt\n" );                // altfire moved off the trigger
	}

	if ( VR_Pressed( dom, VRB_SQUEEZE ) ) {
		xr.rightGripCombo = qfalse;
	}
	if ( VR_Released( dom, VRB_SQUEEZE ) && !xr.rightGripCombo &&
		 cls.realtime - d->downTime[VRB_SQUEEZE] < 400 ) {
		// tap grip: drop the magazine for weapons reloaded by hand (the game decides),
		// a normal reload for the rest
		com_vr.ejectCount++;
		Cbuf_AddText( "+reload\nwait\n-reload\n" );
	}

	// slide release: reload (two guns reload only with both hands down at the waist;
	// the game checks that)
	if ( VR_ActionPressed( VRA_RELOAD ) ) {
		if ( !com_vr.dualWield ) {
			com_vr.ejectCount++;
		}
		Cbuf_AddText( "+reload\nwait\n-reload\n" );
	}

	// crouch stays on until stand (or a jump)
	if ( VR_ActionPressed( VRA_CROUCH ) ) {
		VR_ButtonCommand( "movedown", qtrue );
	}
	if ( VR_ActionPressed( VRA_STAND ) && VR_CommandHeld( "movedown" ) ) {
		VR_ButtonCommand( "movedown", qfalse );
	}
	if ( VR_ActionPressed( VRA_JUMP ) ) {
		VR_ButtonCommand( "movedown", qfalse );
		vrActionHeld[VRA_JUMP] = qtrue;
		VR_ButtonCommand( "moveup", qtrue );
	} else if ( vrActionHeld[VRA_JUMP] && !VR_ActionDown( VRA_JUMP ) ) {
		vrActionHeld[VRA_JUMP] = qfalse;
		VR_ButtonCommand( "moveup", qfalse );
	}

	VR_ActionHold( VRA_USE, "activate" );
	VR_ActionHold( VRA_KICK, "kick" );

	if ( VR_ActionPressed( VRA_WEAPNEXT ) ) {
		Cbuf_AddText( "weapnext\n" );
	}
	if ( VR_ActionPressed( VRA_WEAPPREV ) ) {
		Cbuf_AddText( "weapprev\n" );
	}
	if ( VR_ActionPressed( VRA_NOTEBOOK ) ) {
		Cbuf_AddText( "notebook\n" );
	}
	if ( VR_ActionPressed( VRA_HUD ) ) {
		Cvar_SetValue( "cg_draw2D", Cvar_VariableIntegerValue( "cg_draw2D" ) ? 0 : 1 );
	}
	if ( VR_ActionPressed( VRA_USEITEM ) ) {
		Cbuf_AddText( "+useitem\nwait\n-useitem\n" );
	}
	if ( VR_ActionPressed( VRA_ITEMNEXT ) ) {
		Cbuf_AddText( "itemnext\n" );
	}

	// off hand
	// the notebook, from its pocket at the hip
	if ( VR_Pressed( off, VRB_SQUEEZE ) && vr_hipNotebook->integer && VR_HandAtHip( off ) ) {
		Cbuf_AddText( "notebook\n" );
	}

	if ( com_vr.dualWield ) {
		// a gun in this hand too: its trigger fires it
		if ( VR_Pressed( off, VRB_TRIGGER ) ) {
			xr.attackOffHeld = qtrue;
			xr.akimboLastLeft = qtrue;
		} else if ( !xr.hands[off].down[VRB_TRIGGER] ) {
			xr.attackOffHeld = qfalse;
		}
		if ( VR_Released( off, VRB_TRIGGER ) ) {
			VR_ButtonCommand( "sprint", qfalse );   // in case it was sprinting before the switch
		}
	} else {
		xr.attackOffHeld = qfalse;
		VR_Hold( "sprint", off, VRB_TRIGGER );      // trigger: sprint
	}
}

// snap / smooth turning on the weapon hand stick
static void VR_Turning( float frameSeconds ) {
	int dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	float x = xr.hands[dom].stick[0];

	if ( vr_snapTurn->value > 0.0f ) {
		if ( VR_Pressed( dom, VRB_STICK_RIGHT ) ) {
			xr.turnYaw -= vr_snapTurn->value;
		} else if ( VR_Pressed( dom, VRB_STICK_LEFT ) ) {
			xr.turnYaw += vr_snapTurn->value;
		}
	} else if ( fabs( x ) > 0.0f ) {
		xr.turnYaw -= x * vr_turnSpeed->value * frameSeconds;
	}
	xr.turnYaw = AngleNormalize360( xr.turnYaw );
}

/*
=================
VR_BinocularGesture

The off hand held up in front of the eyes: look through the binoculars (the game ignores
it without them). A little wider to leave than to enter, so it doesn't flicker.
=================
*/
static qboolean VR_BinocularGesture( int off ) {
	static qboolean held;
	XrQuaternionf inv;
	float rel[3], local[3], slack;

	if ( !vr_binoculars->integer || !xr.hands[off].gripValid ) {
		held = qfalse;
		return qfalse;
	}
	rel[0] = xr.hands[off].grip.position.x - xr.head.position.x;
	rel[1] = xr.hands[off].grip.position.y - xr.head.position.y;
	rel[2] = xr.hands[off].grip.position.z - xr.head.position.z;
	inv = xr.head.orientation;
	inv.x = -inv.x;
	inv.y = -inv.y;
	inv.z = -inv.z;
	VR_QuatRotate( &inv, rel, local );      // head space: x right, y up, -z forward

	slack = held ? 0.04f : 0.0f;
	held = -local[2] > 0.02f - slack && -local[2] < 0.20f + slack &&
		   fabs( local[1] ) < 0.10f + slack && fabs( local[0] ) < 0.12f + slack;
	return held;
}

/*
=================
CL_VR_CreateCmd

Called from CL_CreateCmd: aim, locomotion and room-scale for this usercmd.
=================
*/
void CL_VR_CreateCmd( usercmd_t *cmd ) {
	int dom, off;
	float yaw, fwd, side, a, c, s;
	float frameSeconds = cls.frametime * 0.001f;

	if ( !com_vr.active || !xr.headValid ) {
		return;
	}

	dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	off = !dom;

	// aim: the server fires along the usercmd angles, so send the gun's angles
	cl.viewangles[PITCH] = com_vr.weaponAngles[PITCH];
	cl.viewangles[YAW] = com_vr.weaponAngles[YAW];
	cl.viewangles[ROLL] = 0;

	// the trigger, sampled this frame
	if ( xr.attackHeld && !Key_GetCatcher() ) {
		cmd->buttons |= BUTTON_ATTACK;
	}
	if ( !Key_GetCatcher() && VR_BinocularGesture( off ) ) {
		cmd->wbuttons |= WBUTTON_ZOOM;
	}

	// dual pistols: the off hand's trigger fires the off hand's gun, aimed where it points
	com_vr.akimboLeft = 0;
	if ( com_vr.dualWield && xr.attackOffHeld && ( !xr.attackHeld || xr.akimboLastLeft ) &&
		 com_vr.handValid[off] && !Key_GetCatcher() ) {
		cmd->buttons |= BUTTON_ATTACK;
		com_vr.akimboLeft = 1;
		cl.viewangles[PITCH] = AngleNormalize180( com_vr.handAngles[off][PITCH] + vr_weaponPitch->value );
		cl.viewangles[YAW] = com_vr.handAngles[off][YAW];
	}

	if ( Key_GetCatcher() ) {
		xr.pendingMove[0] = xr.pendingMove[1] = 0;
		return;
	}

	VR_Turning( frameSeconds );

	// physical crouch: ducking down crouches
	if ( !vr_seated->integer && vr_crouchHeight->value > 0 && xr.headHeight > 0.2f &&
		 xr.headHeight < vr_crouchHeight->value && cmd->upmove <= 0 ) {
		cmd->upmove = -127;
	}

	// ladders: a hand gripping pulls the body up (or lets it down) as far as it moves
	{
		static float lastZ[2];
		static qboolean had[2];
		float climb = 0, best = 0;
		int h;

		for ( h = 0; h < 2; h++ ) {
			qboolean grip = com_vr.onLadder && com_vr.handValid[h] && xr.hands[h].down[VRB_SQUEEZE];
			if ( grip && had[h] && frameSeconds > 0 ) {
				float v = -( com_vr.handOrigin[h][2] - lastZ[h] ) / frameSeconds;    // units/s, up
				if ( fabs( v ) > fabs( best ) ) {
					best = v;
				}
			}
			had[h] = grip;
			lastZ[h] = com_vr.handOrigin[h][2];
		}
		if ( best != 0 ) {
			climb = best / 3.0f;
			cmd->upmove = ClampChar( (int)climb );
			cmd->forwardmove = 0;
		}
	}

	// teleport: push the off stick forward to aim (cgame draws the arc), let go to go there
	if ( vr_teleport->integer ) {
		float stick = xr.hands[off].stick[1];

		if ( stick > 0.6f ) {
			com_vr.teleportAiming = 1;
		} else if ( com_vr.teleportAiming && stick < 0.3f ) {
			com_vr.teleportAiming = 0;
			if ( com_vr.teleportValid ) {
				// world -> play space -> the frame of this command's yaw, like room scale
				float d[2], mx, my;
				a = DEG2RAD( -com_vr.yawOffset );
				c = cos( a );
				s = sin( a );
				d[0] = com_vr.teleportDelta[0] * c - com_vr.teleportDelta[1] * s;
				d[1] = com_vr.teleportDelta[0] * s + com_vr.teleportDelta[1] * c;
				a = DEG2RAD( cl.viewangles[YAW] );
				c = cos( a );
				s = sin( a );
				mx = d[0] * c + d[1] * s;
				my = -d[0] * s + d[1] * c;
				xr.teleportMove[0] = (short)( mx * 8.0f );
				xr.teleportMove[1] = (short)( my * 8.0f );
				xr.teleportPending = qtrue;
			}
		}
	} else {
		com_vr.teleportAiming = 0;
	}

	// thumbstick locomotion relative to the head (or off hand), rotated into the
	// frame of the usercmd yaw (not when the stick aims teleports)
	yaw = vr_walkDirection->integer && com_vr.handValid[off] ? com_vr.handAngles[off][YAW] : com_vr.hmdAngles[YAW];
	fwd = vr_teleport->integer ? 0 : xr.hands[off].stick[1];
	side = vr_teleport->integer ? 0 : xr.hands[off].stick[0];
	if ( fwd || side ) {
		a = DEG2RAD( yaw - cl.viewangles[YAW] );
		c = cos( a );
		s = sin( a );
		// desired = fwd * F(yaw) + side * R(yaw); expressed against F/R of the cmd yaw
		cmd->forwardmove = ClampChar( cmd->forwardmove + (int)( 127.0f * ( fwd * c + side * s ) ) );
		cmd->rightmove = ClampChar( cmd->rightmove + (int)( 127.0f * ( side * c - fwd * s ) ) );
	}

	// room-scale: carry the body along with the head, in the usercmd yaw frame
	if ( vr_roomScale->integer && ( xr.pendingMove[0] || xr.pendingMove[1] ) ) {
		float limit = 16.0f;        // per command, anything bigger is a tracking glitch
		float mx, my;

		a = DEG2RAD( cl.viewangles[YAW] );
		c = cos( a );
		s = sin( a );
		mx = xr.pendingMove[0] * c + xr.pendingMove[1] * s;     // forward
		my = -xr.pendingMove[0] * s + xr.pendingMove[1] * c;    // left
		if ( fabs( mx ) < limit && fabs( my ) < limit ) {
			cmd->vrMove[0] = (short)( mx * 8.0f );
			cmd->vrMove[1] = (short)( my * 8.0f );
		}
	}
	xr.pendingMove[0] = xr.pendingMove[1] = 0;

	// a teleport goes in as one big step (PM_VRRoomScaleMove tells it apart by its size)
	if ( xr.teleportPending ) {
		cmd->vrMove[0] = xr.teleportMove[0];
		cmd->vrMove[1] = xr.teleportMove[1];
		xr.teleportPending = qfalse;
	}
}

/*
==============================================================================

Menus: world locked panel and laser pointer

==============================================================================
*/

static void VR_PlaceMenu( void ) {
	static const float xrFwd[3] = { 0, 0, -1 };
	float f[3], len, theta, dist = vr_menuDistance->value;

	VR_QuatRotate( &xr.head.orientation, xrFwd, f );
	f[1] = 0;
	len = sqrt( f[0] * f[0] + f[2] * f[2] );
	if ( len < 0.001f ) {
		f[0] = 0;
		f[2] = -1;
	} else {
		f[0] /= len;
		f[2] /= len;
	}

	xr.menuPose.position.x = xr.head.position.x + f[0] * dist;
	xr.menuPose.position.y = xr.head.position.y;
	xr.menuPose.position.z = xr.head.position.z + f[2] * dist;
	// the quad faces +z: turn it so +z points back at the player
	theta = atan2( -f[0], -f[2] );
	xr.menuPose.orientation.x = 0;
	xr.menuPose.orientation.y = sin( theta * 0.5f );
	xr.menuPose.orientation.z = 0;
	xr.menuPose.orientation.w = cos( theta * 0.5f );
	xr.menuPlaced = qtrue;
}

// intersect a hand's aim ray with the menu panel, gives panel u,v in 0..1
static qboolean VR_PointerHit( int hand, float *u, float *v, XrVector3f *hitPos ) {
	static const float zAxis[3] = { 0, 0, 1 }, xAxis[3] = { 1, 0, 0 }, yAxis[3] = { 0, 1, 0 }, fwd[3] = { 0, 0, -1 };
	const XrPosef *aim = &xr.hands[hand].aim;
	float n[3], px[3], py[3], d[3], rel[3];
	float denom, t, w, h, lx, ly;

	if ( !xr.hands[hand].aimValid ) {
		return qfalse;
	}

	VR_QuatRotate( &xr.menuPose.orientation, zAxis, n );
	VR_QuatRotate( &xr.menuPose.orientation, xAxis, px );
	VR_QuatRotate( &xr.menuPose.orientation, yAxis, py );
	VR_QuatRotate( &aim->orientation, fwd, d );

	denom = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
	if ( denom > -0.0001f ) {
		return qfalse;      // pointing away from the panel
	}
	rel[0] = xr.menuPose.position.x - aim->position.x;
	rel[1] = xr.menuPose.position.y - aim->position.y;
	rel[2] = xr.menuPose.position.z - aim->position.z;
	t = ( rel[0] * n[0] + rel[1] * n[1] + rel[2] * n[2] ) / denom;
	if ( t < 0 ) {
		return qfalse;
	}

	hitPos->x = aim->position.x + d[0] * t;
	hitPos->y = aim->position.y + d[1] * t;
	hitPos->z = aim->position.z + d[2] * t;

	rel[0] = hitPos->x - xr.menuPose.position.x;
	rel[1] = hitPos->y - xr.menuPose.position.y;
	rel[2] = hitPos->z - xr.menuPose.position.z;
	lx = rel[0] * px[0] + rel[1] * px[1] + rel[2] * px[2];
	ly = rel[0] * py[0] + rel[1] * py[1] + rel[2] * py[2];

	w = vr_menuWidth->value;
	h = w * 0.75f;
	*u = lx / w + 0.5f;
	*v = 0.5f - ly / h;
	return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

// keep our idea of the UI cursor in step with real mouse input
void CL_VR_MouseMoved( int dx, int dy ) {
	xr.uiCursorX = Com_Clamp( 0, SCREEN_WIDTH, xr.uiCursorX + dx );
	xr.uiCursorY = Com_Clamp( 0, SCREEN_HEIGHT, xr.uiCursorY + dy );
}

static void VR_MenuControls( void ) {
	int h, dom = vr_rightHanded->integer ? VR_HAND_RIGHT : VR_HAND_LEFT;
	XrVector3f hit;
	float u = 0, v = 0;

	// the hand whose trigger was pulled last is the pointer
	for ( h = 0; h < 2; h++ ) {
		if ( VR_Pressed( h, VRB_TRIGGER ) ) {
			xr.pointerHand = h;
		}
	}
	if ( !xr.hands[xr.pointerHand].aimValid ) {
		xr.pointerHand = dom;
	}

	xr.pointerHit = VR_PointerHit( xr.pointerHand, &u, &v, &hit );

	// laser geometry in play space, drawn by the menu environment or by cgame
	com_vr.pointerVisible = xr.hands[xr.pointerHand].aimValid;
	if ( com_vr.pointerVisible ) {
		vec3_t ang, f;
		VR_PoseToPlay( &xr.hands[xr.pointerHand].aim, com_vr.pointerStart, ang );
		if ( xr.pointerHit ) {
			XrPosef hp;
			hp.orientation = xr.hands[xr.pointerHand].aim.orientation;
			hp.position = hit;
			VR_PoseToPlay( &hp, com_vr.pointerEnd, ang );
		} else {
			AngleVectors( ang, f, NULL, NULL );
			VectorMA( com_vr.pointerStart, 4.0f * vr_worldScale->value, f, com_vr.pointerEnd );
		}
	}

	if ( xr.pointerHit ) {
		int x = (int)( u * SCREEN_WIDTH ), y = (int)( v * SCREEN_HEIGHT );
		int dx = x - (int)xr.uiCursorX, dy = y - (int)xr.uiCursorY;

		xr.pointerU = u;
		xr.pointerV = v;
		if ( ( dx || dy ) && uivm && ( Key_GetCatcher() & KEYCATCH_UI ) ) {
			VM_Call( uivm, UI_MOUSE_EVENT, dx, dy );
			xr.uiCursorX = x;
			xr.uiCursorY = y;
		}
	}

	// trigger clicks, B/Y backs out
	for ( h = 0; h < 2; h++ ) {
		if ( h == xr.pointerHand && VR_Pressed( h, VRB_TRIGGER ) ) {
			CL_KeyEvent( K_MOUSE1, qtrue, Sys_Milliseconds() );
		} else if ( h == xr.pointerHand && VR_Released( h, VRB_TRIGGER ) ) {
			CL_KeyEvent( K_MOUSE1, qfalse, Sys_Milliseconds() );
		}
		if ( VR_Pressed( h, VRB_UPPER ) ) {
			CL_KeyEvent( K_ESCAPE, qtrue, Sys_Milliseconds() );
			CL_KeyEvent( K_ESCAPE, qfalse, Sys_Milliseconds() );
		}
	}

	// thumbstick scrolls lists
	if ( cls.realtime - xr.lastMenuScrollTime > 150 ) {
		if ( xr.hands[xr.pointerHand].stick[1] > 0.6f ) {
			CL_KeyEvent( K_MWHEELUP, qtrue, Sys_Milliseconds() );
			CL_KeyEvent( K_MWHEELUP, qfalse, Sys_Milliseconds() );
			xr.lastMenuScrollTime = cls.realtime;
		} else if ( xr.hands[xr.pointerHand].stick[1] < -0.6f ) {
			CL_KeyEvent( K_MWHEELDOWN, qtrue, Sys_Milliseconds() );
			CL_KeyEvent( K_MWHEELDOWN, qfalse, Sys_Milliseconds() );
			xr.lastMenuScrollTime = cls.realtime;
		}
	}
}

/*
=================
CL_VR_ProcessInput

Turns this frame's controller state into game commands or menu input.
=================
*/
void CL_VR_ProcessInput( void ) {
	int off;
	qboolean menu;

	if ( !com_vr.active || !xr.headValid ) {
		return;
	}

	off = vr_rightHanded->integer ? VR_HAND_LEFT : VR_HAND_RIGHT;

	// cutscenes and videos: any face button or trigger skips them (the engine treats
	// Escape as "skip" while they play)
	if ( ( cl.cameraMode || SCR_LevelCinematicActive() || clc.state == CA_CINEMATIC ) &&
		 !( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) ) {
		int h;
		qboolean skip = qfalse;

		for ( h = 0; h < 2; h++ ) {
			skip |= VR_Pressed( h, VRB_TRIGGER ) || VR_Pressed( h, VRB_LOWER ) ||
					VR_Pressed( h, VRB_UPPER ) || VR_Pressed( h, VRB_MENU );
		}
		if ( skip ) {
			CL_KeyEvent( K_ESCAPE, qtrue, Sys_Milliseconds() );
			CL_KeyEvent( K_ESCAPE, qfalse, Sys_Milliseconds() );
		}
		VR_ReleaseAllCommands();
		return;
	}

	// left menu button toggles the game menu, whatever is going on
	if ( VR_Pressed( VR_HAND_LEFT, VRB_MENU ) ) {
		CL_KeyEvent( K_ESCAPE, qtrue, Sys_Milliseconds() );
		CL_KeyEvent( K_ESCAPE, qfalse, Sys_Milliseconds() );
	}

	menu = ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) || clc.state != CA_ACTIVE;

	// grip buttons for the game's hand interactions (reloading, grabbing)
	com_vr.gripDown[VR_HAND_LEFT] = !menu && xr.hands[VR_HAND_LEFT].down[VRB_SQUEEZE];
	com_vr.gripDown[VR_HAND_RIGHT] = !menu && xr.hands[VR_HAND_RIGHT].down[VRB_SQUEEZE];
	if ( menu != xr.menuMode ) {
		xr.menuMode = menu;
		VR_ReleaseAllCommands();
		if ( menu ) {
			xr.menuPlaced = qfalse;
		}
	}
	com_vr.menuPointer = menu;
	if ( !menu ) {
		com_vr.pointerVisible = qfalse;
	}

	if ( menu ) {
		if ( !xr.menuPlaced ) {
			VR_PlaceMenu();
		}
		VR_MenuControls();
	} else if ( !Key_GetCatcher() ) {
		VR_GameControls();
	}

	(void)off;

	// haptic requests from the game modules
	if ( com_vr.hapticAmplitude[0] > 0 ) {
		VR_Haptic( VR_HAND_LEFT, com_vr.hapticAmplitude[0], com_vr.hapticDuration[0] );
		com_vr.hapticAmplitude[0] = 0;
	}
	if ( com_vr.hapticAmplitude[1] > 0 ) {
		VR_Haptic( VR_HAND_RIGHT, com_vr.hapticAmplitude[1], com_vr.hapticDuration[1] );
		com_vr.hapticAmplitude[1] = 0;
	}
}

/*
==============================================================================

Rendering

==============================================================================
*/

// eye offset from the head centre, game units on Quake axes
static void VR_EyeOffset( int eye, vec3_t out ) {
	vec3_t raw;
	XrVector3f p = xr.headViews[eye].pose.position;

	VR_XrPosToQuake( &p, raw );
	VectorCopy( raw, out );
}

static void VR_EyeTangents( int eye, float t[4] ) {
	const XrFovf *f = &xr.views[eye].fov;
	t[0] = tan( f->angleLeft );
	t[1] = tan( f->angleRight );
	t[2] = tan( f->angleUp );
	t[3] = tan( f->angleDown );
}

// a thin camera facing quad between two points
static void VR_AddBeam( const vec3_t start, const vec3_t end, const vec3_t viewOrg, float width, const byte rgba[4] ) {
	polyVert_t verts[4];
	vec3_t dir, toView, side;
	int i;

	VectorSubtract( end, start, dir );
	VectorSubtract( viewOrg, start, toView );
	CrossProduct( dir, toView, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		return;
	}
	VectorScale( side, width * 0.5f, side );

	VectorAdd( start, side, verts[0].xyz );
	VectorSubtract( start, side, verts[1].xyz );
	VectorSubtract( end, side, verts[2].xyz );
	VectorAdd( end, side, verts[3].xyz );
	for ( i = 0; i < 4; i++ ) {
		verts[i].st[0] = ( i == 1 || i == 2 ) ? 1 : 0;
		verts[i].st[1] = ( i >= 2 ) ? 1 : 0;
		verts[i].modulate[0] = rgba[0];
		verts[i].modulate[1] = rgba[1];
		verts[i].modulate[2] = rgba[2];
		verts[i].modulate[3] = rgba[3];
	}
	re.AddPolyToScene( cls.whiteShader, 4, verts );
}

// what the eyes see when there is no game world: an empty room with the laser pointer
static void VR_DrawMenuEnvironment( void ) {
	refdef_t rd;
	static const byte laserColor[4] = { 255, 60, 40, 255 };

	re.BeginFrame( STEREO_CENTER );
	re.ClearScene();

	Com_Memset( &rd, 0, sizeof( rd ) );
	rd.x = rd.y = 0;
	rd.width = cls.glconfig.vidWidth;
	rd.height = cls.glconfig.vidHeight;
	rd.fov_x = rd.fov_y = 90;
	rd.time = cls.realtime;
	rd.rdflags = RDF_NOWORLDMODEL | RDF_VRVIEW;
	VectorCopy( com_vr.hmdOrigin, rd.vieworg );
	AnglesToAxis( com_vr.hmdAngles, rd.viewaxis );

	if ( com_vr.pointerVisible ) {
		VR_AddBeam( com_vr.pointerStart, com_vr.pointerEnd, rd.vieworg, 0.006f * vr_worldScale->value, laserColor );
	}

	re.RenderScene( &rd );
}

static XrCompositionLayerProjectionView projViews[2];
static XrCompositionLayerProjection projLayer;
static XrCompositionLayerQuad quadLayer;

static void VR_SubmitFrame( qboolean eyes, qboolean ui, qboolean uiInWorld ) {
	XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
	const XrCompositionLayerBaseHeader *layers[2];
	int n = 0, eye;

	if ( eyes ) {
		Com_Memset( &projLayer, 0, sizeof( projLayer ) );
		projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		projLayer.space = xr.playSpace;
		projLayer.viewCount = 2;
		projLayer.views = projViews;
		for ( eye = 0; eye < 2; eye++ ) {
			Com_Memset( &projViews[eye], 0, sizeof( projViews[eye] ) );
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose = xr.views[eye].pose;
			projViews[eye].fov = xr.views[eye].fov;
			projViews[eye].subImage.swapchain = xr.eyes[eye].handle;
			projViews[eye].subImage.imageRect.extent.width = xr.eyes[eye].width;
			projViews[eye].subImage.imageRect.extent.height = xr.eyes[eye].height;
		}
		layers[n++] = (XrCompositionLayerBaseHeader *)&projLayer;
	}

	if ( ui ) {
		float w = uiInWorld ? vr_menuWidth->value : vr_hudWidth->value;

		Com_Memset( &quadLayer, 0, sizeof( quadLayer ) );
		quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quadLayer.subImage.swapchain = xr.ui.handle;
		quadLayer.subImage.imageRect.extent.width = xr.ui.width;
		quadLayer.subImage.imageRect.extent.height = xr.ui.height;
		quadLayer.size.width = w;
		quadLayer.size.height = w * 0.75f;  // the UI is a 4:3 virtual screen
		if ( uiInWorld ) {
			quadLayer.space = xr.playSpace;
			quadLayer.pose = xr.menuPose;
		} else {
			quadLayer.space = xr.viewSpace;
			quadLayer.pose.orientation.w = 1.0f;
			quadLayer.pose.position.y = vr_hudHeight->value;
			quadLayer.pose.position.z = -vr_hudDistance->value;
		}
		layers[n++] = (XrCompositionLayerBaseHeader *)&quadLayer;
	}

	fei.displayTime = xr.frameState.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = n;
	fei.layers = layers;
	VR_OK( qxrEndFrame( xr.session, &fei ) );
	xr.frameBegun = qfalse;
}

/*
=================
CL_VR_RenderFrame

Draws both eyes and the 2D layer for this frame and hands them to the compositor.
Returns qfalse when the monitor path should draw instead.
=================
*/
qboolean CL_VR_RenderFrame( void ) {
	static const float menuClear[4] = { 0.02f, 0.02f, 0.025f, 1.0f };
	static const float uiClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	qboolean uiFullscreen, worldVisible, uiInWorld;
	vec3_t eyeOffset;
	float tangents[4];
	GLuint fbo;
	int eye, mirror;

	if ( xr.session == XR_NULL_HANDLE ) {
		return qfalse;
	}

	CL_VR_BeginFrame();
	if ( !xr.sessionRunning || !xr.frameBegun ) {
		return qfalse;
	}

	if ( !xr.frameState.shouldRender || !xr.headValid ) {
		VR_SubmitFrame( qfalse, qfalse, qfalse );
		return qtrue;
	}

	uiFullscreen = uivm && VM_Call( uivm, UI_IS_FULLSCREEN );
	worldVisible = clc.state == CA_ACTIVE && cls.cgameStarted && !uiFullscreen;
	uiInWorld = xr.menuMode || !worldVisible;
	if ( uiInWorld && !xr.menuPlaced ) {
		VR_PlaceMenu();
	}

	// 1 = left eye in game, the UI panel in menus; 2 = always the UI layer
	mirror = vr_mirror->integer;

	for ( eye = 0; eye < 2; eye++ ) {
		fbo = VR_AcquireSwapchain( &xr.eyes[eye] );
		if ( !fbo ) {
			continue;
		}

		VR_EyeTangents( eye, tangents );
		VR_EyeOffset( eye, eyeOffset );
		re.VR_SetEye( eye, tangents, eyeOffset );
		VectorCopy( eyeOffset, com_vr.eyeOffset );
		com_vr.pass = VR_PASS_EYE;
		com_vr.eye = eye;

		re.VR_BeginPass( fbo, worldVisible ? NULL : menuClear );
		if ( worldVisible ) {
			SCR_DrawScreenField( STEREO_CENTER );
		} else {
			VR_DrawMenuEnvironment();
		}
		re.EndFrame( NULL, NULL );

		if ( eye == 0 && mirror == 1 && !uiInWorld ) {
			re.VR_PresentMirror( fbo, xr.eyes[eye].width, xr.eyes[eye].height );
		}
		VR_ReleaseSwapchain( &xr.eyes[eye] );
	}

	// HUD, menus, cinematics and the console
	re.VR_SetEye( -1, NULL, NULL );
	com_vr.pass = VR_PASS_2D;
	fbo = VR_AcquireSwapchain( &xr.ui );
	if ( fbo ) {
		re.VR_BeginPass( fbo, uiInWorld ? menuClear : uiClear );
		SCR_DrawScreenField( STEREO_CENTER );
		re.EndFrame( NULL, NULL );
		if ( mirror == 2 || ( mirror == 1 && uiInWorld ) ) {
			re.VR_PresentMirror( fbo, xr.ui.width, xr.ui.height );
		}
		VR_ReleaseSwapchain( &xr.ui );
	}

	re.VR_EndPasses();
	com_vr.pass = VR_PASS_FLAT;

	VR_SubmitFrame( qtrue, fbo != 0, uiInWorld );
	return qtrue;
}

/*
=================
CL_VR_EndFrame

Closes a frame that was begun but never rendered (minimised, early returns...), the
runtime requires every xrBeginFrame to be matched.
=================
*/
void CL_VR_EndFrame( void ) {
	if ( xr.session != XR_NULL_HANDLE && xr.frameBegun ) {
		VR_SubmitFrame( qfalse, qfalse, qfalse );
	}
}

void CL_VR_Recenter_f( void ) {
	xr.turnYaw = 0;
	xr.menuPlaced = qfalse;
}

/*
=================
CL_VR_Key_f

vr_key <key>: press and release a key, for scripted testing (e.g. getting past the
mission briefing from a config file).
=================
*/
void CL_VR_Key_f( void ) {
	int key;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: vr_key <key>\n" );
		return;
	}
	key = Key_StringToKeynum( Cmd_Argv( 1 ) );
	if ( key < 0 ) {
		Com_Printf( "vr_key: unknown key %s\n", Cmd_Argv( 1 ) );
		return;
	}
	CL_KeyEvent( key, qtrue, Sys_Milliseconds() );
	CL_KeyEvent( key, qfalse, Sys_Milliseconds() );
}

/*
=================
CL_VR_Cursor_f

vr_cursor <x> <y>: put the menu cursor on a 640x480 position, the way the laser pointer
does, for scripted menu tests.
=================
*/
void CL_VR_Cursor_f( void ) {
	if ( Cmd_Argc() != 3 || !uivm || !( Key_GetCatcher() & KEYCATCH_UI ) ) {
		Com_Printf( "usage: vr_cursor <x> <y> (with a menu open)\n" );
		return;
	}
	VM_Call( uivm, UI_MOUSE_EVENT, -SCREEN_WIDTH * 2, -SCREEN_HEIGHT * 2 );     // to the corner...
	VM_Call( uivm, UI_MOUSE_EVENT, atoi( Cmd_Argv( 1 ) ), atoi( Cmd_Argv( 2 ) ) );  // ...then out
	xr.uiCursorX = atoi( Cmd_Argv( 1 ) );
	xr.uiCursorY = atoi( Cmd_Argv( 2 ) );
}

#else // !_WIN32

void CL_VR_Init( void ) { Com_Memset( &com_vr, 0, sizeof( com_vr ) ); }
void CL_VR_Shutdown( void ) {}
void CL_VR_InitSession( void ) {}
void CL_VR_ShutdownSession( void ) {}
void CL_VR_BeginFrame( void ) {}
void CL_VR_EndFrame( void ) {}
void CL_VR_ProcessInput( void ) {}
void CL_VR_CreateCmd( usercmd_t *cmd ) {}
void CL_VR_MouseMoved( int dx, int dy ) {}
qboolean CL_VR_RenderFrame( void ) { return qfalse; }
qboolean CL_VR_Active( void ) { return qfalse; }
void CL_VR_Recenter_f( void ) {}
void CL_VR_Key_f( void ) {}
void CL_VR_Cursor_f( void ) {}

#endif
