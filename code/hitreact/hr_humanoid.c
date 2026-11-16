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

// hr_humanoid.c -- a humanoid's Jolt body and its reactions (see hitreact.h).
//
// Plain C, so a C game can build it with its own compiler and flags. Everything in here is
// in library space (Z up, inches); the public functions turn the host's space into it.
//
// The body: 15 parts made from 21 skeleton points (HR_BodyBuild), a box torso and chest,
// capsules for the rest, joints with limits a man has, weights a man has (realWeights) or
// heavier hands and feet for a coarser step.
//
// The brain (RD_Eu*): every frame it builds the pose each part wants as a world rotation,
// how hard each joint pulls towards it, how stiffly (tone), how much the torso is held up
// (support) and whether planted feet carry his weight (leg push), and hands those to the
// Jolt layer. The balancer (RD_Balance) places the feet.

#include "hitreact.h"

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef float vec3_t[3];
typedef int qboolean;
enum { qfalse, qtrue };

/*
==============================================================================

Vector maths, exactly as the Quake 3 engine has it (the reactions were tuned on it and a
game built on it gets the same numbers to the last bit)

==============================================================================
*/

#ifndef M_PI
#define M_PI                3.14159265358979323846f
#endif
#define RAD2DEG( a )        ( ( ( a ) * 180.0f ) / M_PI )
#define DotProduct( x,y )         ( ( x )[0] * ( y )[0] + ( x )[1] * ( y )[1] + ( x )[2] * ( y )[2] )
#define VectorSubtract( a,b,c )   ( ( c )[0] = ( a )[0] - ( b )[0],( c )[1] = ( a )[1] - ( b )[1],( c )[2] = ( a )[2] - ( b )[2] )
#define VectorAdd( a,b,c )        ( ( c )[0] = ( a )[0] + ( b )[0],( c )[1] = ( a )[1] + ( b )[1],( c )[2] = ( a )[2] + ( b )[2] )
#define VectorCopy( a,b )         ( ( b )[0] = ( a )[0],( b )[1] = ( a )[1],( b )[2] = ( a )[2] )
#define VectorScale( v, s, o )    ( ( o )[0] = ( v )[0] * ( s ),( o )[1] = ( v )[1] * ( s ),( o )[2] = ( v )[2] * ( s ) )
#define VectorMA( v, s, b, o )    ( ( o )[0] = ( v )[0] + ( b )[0] * ( s ),( o )[1] = ( v )[1] + ( b )[1] * ( s ),( o )[2] = ( v )[2] + ( b )[2] * ( s ) )
#define VectorClear( a )          ( ( a )[0] = ( a )[1] = ( a )[2] = 0 )
#define VectorNegate( a,b )       ( ( b )[0] = -( a )[0],( b )[1] = -( a )[1],( b )[2] = -( a )[2] )
#define VectorSet( v, x, y, z )   ( ( v )[0] = ( x ), ( v )[1] = ( y ), ( v )[2] = ( z ) )

static float VectorLength( const vec3_t v ) {
	return (float)sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

static float Distance( const vec3_t p1, const vec3_t p2 ) {
	vec3_t v;

	VectorSubtract( p2, p1, v );
	return VectorLength( v );
}

static void CrossProduct( const vec3_t v1, const vec3_t v2, vec3_t cross ) {
	cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
	cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
	cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

static float VectorNormalize( vec3_t v ) {
	float length, ilength;

	length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if ( length ) {
		ilength = 1 / (float)sqrt( length );
		length *= ilength;
		v[0] *= ilength;
		v[1] *= ilength;
		v[2] *= ilength;
	}
	return length;
}

static float VectorNormalize2( const vec3_t v, vec3_t out ) {
	float length, ilength;

	length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if ( length ) {
		ilength = 1 / (float)sqrt( length );
		length *= ilength;
		out[0] = v[0] * ilength;
		out[1] = v[1] * ilength;
		out[2] = v[2] * ilength;
	} else {
		VectorClear( out );
	}
	return length;
}

static void ProjectPointOnPlane( vec3_t dst, const vec3_t p, const vec3_t normal ) {
	float d, inv_denom;
	vec3_t n;

	inv_denom = 1.0F / DotProduct( normal, normal );
	d = DotProduct( normal, p ) * inv_denom;
	n[0] = normal[0] * inv_denom;
	n[1] = normal[1] * inv_denom;
	n[2] = normal[2] * inv_denom;
	dst[0] = p[0] - d * n[0];
	dst[1] = p[1] - d * n[1];
	dst[2] = p[2] - d * n[2];
}

static void PerpendicularVector( vec3_t dst, const vec3_t src ) {
	int pos, i;
	float minelem = 1.0F;
	vec3_t tempvec;

	for ( pos = 0, i = 0; i < 3; i++ ) {
		if ( fabs( src[i] ) < minelem ) {
			pos = i;
			minelem = fabs( src[i] );
		}
	}
	tempvec[0] = tempvec[1] = tempvec[2] = 0.0F;
	tempvec[pos] = 1.0F;
	ProjectPointOnPlane( dst, tempvec, src );
	VectorNormalize( dst );
}

static float AngleNormalize360( float angle ) {
	return ( 360.0 / 65536 ) * ( (int)( angle * ( 65536 / 360.0 ) ) & 65535 );
}

static float Com_Clamp( float min, float max, float value ) {
	if ( value < min ) {
		return min;
	}
	if ( value > max ) {
		return max;
	}
	return value;
}

#define RD_MatMul           HR_MatMul
#define RD_MatVec           HR_MatVec
#define RD_FrameRotation    HR_FrameRotation
#define RD_Align            HR_Align
#define RD_AxisAngle        HR_AxisAngle
#define RD_TorsoFrame       HR_TorsoFrame
#define RD_RotLerp          HR_RotLerp
#define RD_SegmentDist      HR_SegmentDist

/*
==============================================================================

The host

==============================================================================
*/

static hrHost_t hrHost;
static hrOptions_t hrOpt = { 400, 1, 1, 1, 12, 0 };
static float hrGravity = 400;      // library units
static int hrTime;

static float hrScale = 1;          // host units per inch
static int hrYUp;
static int hrIdentity = 1;

void HR_SetHostSpace( float hostUnitsPerInch, int yUp ) {
	hrScale = hostUnitsPerInch > 0 ? hostUnitsPerInch : 1;
	hrYUp = yUp != 0;
	hrIdentity = hrScale == 1.0f && !hrYUp;
	hrGravity = HR_LenToLib( hrOpt.gravity );
}

int HR_HostIsLib( void ) {
	return hrIdentity;
}

// host (x, y, z) with y up = library (x, -z, y)
void HR_DirToLib( const float *in, float *out ) {
	if ( hrYUp ) {
		float x = in[0], y = in[1], z = in[2];
		out[0] = x;
		out[1] = -z;
		out[2] = y;
	} else {
		VectorCopy( in, out );
	}
}

void HR_DirToHost( const float *in, float *out ) {
	if ( hrYUp ) {
		float x = in[0], y = in[1], z = in[2];
		out[0] = x;
		out[1] = z;
		out[2] = -y;
	} else {
		VectorCopy( in, out );
	}
}

void HR_PointToLib( const float *in, float *out ) {
	if ( hrIdentity ) {
		VectorCopy( in, out );
		return;
	}
	HR_DirToLib( in, out );
	VectorScale( out, 1.0f / hrScale, out );
}

void HR_PointToHost( const float *in, float *out ) {
	if ( hrIdentity ) {
		VectorCopy( in, out );
		return;
	}
	HR_DirToHost( in, out );
	VectorScale( out, hrScale, out );
}

// a rotation: each column turned, and each column's meaning (host axis k = library axis)
static void HR_AxisConvert( const float *in, float *out, int toLib ) {
	float c[9], r[9];
	int k, j;

	if ( !hrYUp ) {
		memcpy( out, in, sizeof( float ) * 9 );
		return;
	}
	for ( k = 0; k < 3; k++ ) {     // the columns (the axes' directions)
		if ( toLib ) {
			HR_DirToLib( in + k * 3, c + k * 3 );
		} else {
			HR_DirToHost( in + k * 3, c + k * 3 );
		}
	}
	// and the rows: R_lib = C R_host C^T, so the columns are recombined as the axes map
	for ( j = 0; j < 3; j++ ) {
		float row[3] = { c[0 * 3 + j], c[1 * 3 + j], c[2 * 3 + j] }, m[3];
		if ( toLib ) {
			HR_DirToLib( row, m );
		} else {
			HR_DirToHost( row, m );
		}
		r[0 * 3 + j] = m[0];
		r[1 * 3 + j] = m[1];
		r[2 * 3 + j] = m[2];
	}
	memcpy( out, r, sizeof( r ) );
}

void HR_AxisToLib( const float *in, float *out ) {
	HR_AxisConvert( in, out, 1 );
}

void HR_AxisToHost( const float *in, float *out ) {
	HR_AxisConvert( in, out, 0 );
}

float HR_LenToLib( float len ) {
	return hrIdentity || hrScale == 1.0f ? len : len / hrScale;
}

float HR_LenToHost( float len ) {
	return hrIdentity || hrScale == 1.0f ? len : len * hrScale;
}

void HR_SetHost( const hrHost_t *host ) {
	if ( host ) {
		hrHost = *host;
	} else {
		memset( &hrHost, 0, sizeof( hrHost ) );
	}
}

void HR_SetOptions( const hrOptions_t *options ) {
	hrOpt = *options;
	if ( hrOpt.maxActive <= 0 ) {
		hrOpt.maxActive = 12;
	}
	hrGravity = HR_LenToLib( hrOpt.gravity );
}

void HR_SetTime( int ms ) {
	hrTime = ms;
}

#if defined( __MINGW32__ )
static void HR_Print( const char *fmt, ... ) __attribute__( ( format( gnu_printf, 1, 2 ) ) );
#elif defined( __GNUC__ )
static void HR_Print( const char *fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
#endif
static void HR_Print( const char *fmt, ... ) {
	char buf[1024];
	va_list ap;

	if ( !hrHost.print ) {
		return;
	}
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	hrHost.print( buf );
}

static float HR_Random( void ) {
	if ( hrHost.random ) {
		return hrHost.random();
	}
	return ( rand() & 0x7fff ) / ( (float)0x7fff );
}

static void HR_Event( hrBody_t *rd, int event, const float *args ) {
	if ( hrHost.event ) {
		hrHost.event( rd->user, event, args );
	}
}

// where he's got to (host space): the host moves his game body there
static void HR_EventPos( hrBody_t *rd ) {
	float a[3];

	HR_PointToHost( rd->p[HR_P_PELVIS], a );
	HR_Event( rd, HR_EV_POSITION, a );
}

typedef struct {
	float       fraction;
	vec3_t      endpos;
	int         startsolid;
} hrTrace_t;

// a line through the level, never hitting him (library space)
static void HR_TraceLine( hrBody_t *rd, hrTrace_t *tr, const vec3_t start, const vec3_t end ) {
	tr->fraction = 1;
	tr->startsolid = 0;
	VectorCopy( end, tr->endpos );
	if ( hrIdentity ) {
		tr->startsolid = hrHost.trace ? hrHost.trace( rd->user, start, end, &tr->fraction, tr->endpos ) :
						 HRP_RayCast( start, end, &tr->fraction, tr->endpos );
	} else {
		vec3_t s, e, ep;
		HR_PointToHost( start, s );
		HR_PointToHost( end, e );
		VectorCopy( e, ep );        // (a host trace that hits nothing may leave it alone)
		tr->startsolid = hrHost.trace ? hrHost.trace( rd->user, s, e, &tr->fraction, ep ) : HRP_RayCast( s, e, &tr->fraction, ep );
		HR_PointToLib( ep, tr->endpos );
	}
}

// every body with a Jolt ragdoll (HR_NumActive counts the working ones)
#define HR_MAX_BODIES       1024
static hrBody_t *hrBodies[HR_MAX_BODIES];

static void HR_Register( hrBody_t *rd ) {
	int i, free = -1;

	for ( i = 0; i < HR_MAX_BODIES; i++ ) {
		if ( hrBodies[i] == rd ) {
			rd->registered = 1;
			return;
		}
		if ( free < 0 && ( !hrBodies[i] || !hrBodies[i]->registered ) ) {
			free = i;
		}
	}
	if ( free >= 0 ) {
		hrBodies[free] = rd;
		rd->registered = 1;
	}
}

static void HR_Unregister( hrBody_t *rd ) {
	int i;

	for ( i = 0; i < HR_MAX_BODIES; i++ ) {
		if ( hrBodies[i] == rd ) {
			hrBodies[i] = NULL;
		}
	}
	rd->registered = 0;
}

int HR_NumActive( void ) {
	int i, n = 0;

	for ( i = 0; i < HR_MAX_BODIES; i++ ) {
		n += hrBodies[i] && hrBodies[i]->registered && hrBodies[i]->active;
	}
	return n;
}

/*
==============================================================================

The skeleton

==============================================================================
*/

#define RD_HEAD_LEN         9.0f        // neck joint to the top of the skull
#define RD_ANCHOR_SLACK     20.0f       // how far the pelvis may drift from the host's origin for him
#define RD_SETTLE_MS        7000        // after this long on the ground a body stops for good (no twitching)
#define RD_GRAB_SPEED       900.0f      // a held point follows the hand this fast (no snapping)

static const struct {
	int parent;             // segment it hangs from
	int from, to;           // points along it
} hrSegments[HR_S_COUNT] = {
	{ -1, HR_P_PELVIS, HR_P_NECK },             // the belly and hips, up to the chest
	{ HR_S_TORSO, HR_P_CHEST, HR_P_NECK },      // the chest on a spine joint
	{ HR_S_CHEST, HR_P_HEAD, HR_P_HEADEND },
	{ HR_S_CHEST, HR_P_LSHOULDER, HR_P_LELBOW },
	{ HR_S_LUPPER, HR_P_LELBOW, HR_P_LWRIST },
	{ HR_S_LFORE, HR_P_LWRIST, HR_P_LHANDEND },
	{ HR_S_CHEST, HR_P_RSHOULDER, HR_P_RELBOW },
	{ HR_S_RUPPER, HR_P_RELBOW, HR_P_RWRIST },
	{ HR_S_RFORE, HR_P_RWRIST, HR_P_RHANDEND },
	{ HR_S_TORSO, HR_P_LHIP, HR_P_LKNEE },
	{ HR_S_LTHIGH, HR_P_LKNEE, HR_P_LANKLE },
	{ HR_S_LCALF, HR_P_LANKLE, HR_P_LTOE },
	{ HR_S_TORSO, HR_P_RHIP, HR_P_RKNEE },
	{ HR_S_RTHIGH, HR_P_RKNEE, HR_P_RANKLE },
	{ HR_S_RCALF, HR_P_RANKLE, HR_P_RTOE },
};

int HR_SegmentParent( int seg ) {
	return seg >= 0 && seg < HR_S_COUNT ? hrSegments[seg].parent : -1;
}

void HR_MatMul( vec3_t a[3], vec3_t b[3], vec3_t out[3] ) {
	int i, j;
	vec3_t r[3];

	for ( i = 0; i < 3; i++ ) {         // column of the result
		for ( j = 0; j < 3; j++ ) {     // row
			r[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

void HR_MatVec( vec3_t m[3], const vec3_t v, vec3_t out ) {
	vec3_t r;
	int j;

	for ( j = 0; j < 3; j++ ) {
		r[j] = m[0][j] * v[0] + m[1][j] * v[1] + m[2][j] * v[2];
	}
	VectorCopy( r, out );
}

// frame (columns) -> rotation from frame0 to frame: R = F * F0^T
void HR_FrameRotation( vec3_t f[3], vec3_t f0[3], vec3_t out[3] ) {
	vec3_t t[3];
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			t[i][j] = f0[j][i];         // transpose
		}
	}
	RD_MatMul( f, t, out );
}

// smallest rotation turning unit vector u onto v
void HR_Align( const vec3_t u, const vec3_t v, vec3_t out[3] ) {
	vec3_t axis;
	float c = DotProduct( u, v ), s, t, x, y, z;

	CrossProduct( u, v, axis );
	s = VectorNormalize( axis );
	if ( s < 1e-5f ) {
		memset( out, 0, sizeof( vec3_t ) * 3 );
		out[0][0] = out[1][1] = out[2][2] = 1;
		if ( c < 0 ) {      // opposite: half turn about any perpendicular axis
			PerpendicularVector( axis, u );
			x = axis[0]; y = axis[1]; z = axis[2];
			out[0][0] = 2 * x * x - 1; out[1][0] = 2 * x * y;     out[2][0] = 2 * x * z;
			out[0][1] = 2 * x * y;     out[1][1] = 2 * y * y - 1; out[2][1] = 2 * y * z;
			out[0][2] = 2 * x * z;     out[1][2] = 2 * y * z;     out[2][2] = 2 * z * z - 1;
		}
		return;
	}
	t = 1 - c;
	x = axis[0]; y = axis[1]; z = axis[2];
	// columns of the Rodrigues rotation
	out[0][0] = t * x * x + c;     out[0][1] = t * x * y + s * z; out[0][2] = t * x * z - s * y;
	out[1][0] = t * x * y - s * z; out[1][1] = t * y * y + c;     out[1][2] = t * y * z + s * x;
	out[2][0] = t * x * z + s * y; out[2][1] = t * y * z - s * x; out[2][2] = t * z * z + c;
}

// rotation by angle (radians) about a unit axis
void HR_AxisAngle( const vec3_t axis, float angle, vec3_t out[3] ) {
	float s = sin( angle ), c = cos( angle ), t = 1 - c;
	float x = axis[0], y = axis[1], z = axis[2];

	out[0][0] = t * x * x + c;     out[0][1] = t * x * y + s * z; out[0][2] = t * x * z - s * y;
	out[1][0] = t * x * y - s * z; out[1][1] = t * y * y + c;     out[1][2] = t * y * z + s * x;
	out[2][0] = t * x * z + s * y; out[2][1] = t * y * z - s * x; out[2][2] = t * z * z + c;
}

void HR_TorsoFrame( vec3_t p[HR_P_COUNT], vec3_t f[3] ) {
	VectorSubtract( p[HR_P_NECK], p[HR_P_PELVIS], f[0] );
	VectorNormalize( f[0] );
	VectorSubtract( p[HR_P_LSHOULDER], p[HR_P_RSHOULDER], f[1] );
	VectorMA( f[1], -DotProduct( f[1], f[0] ), f[0], f[1] );
	VectorNormalize( f[1] );
	CrossProduct( f[0], f[1], f[2] );
}


// closest points between segments p1-q1 and p2-q2: parameters s, t
float HR_SegmentDist( const vec3_t p1, const vec3_t q1, const vec3_t p2, const vec3_t q2, float *s, float *t ) {
	vec3_t d1, d2, r, c1, c2;
	float a, e, f, b, c, denom;

	VectorSubtract( q1, p1, d1 );
	VectorSubtract( q2, p2, d2 );
	VectorSubtract( p1, p2, r );
	a = DotProduct( d1, d1 );
	e = DotProduct( d2, d2 );
	f = DotProduct( d2, r );
	if ( a < 1e-6f && e < 1e-6f ) {
		*s = *t = 0;
	} else if ( a < 1e-6f ) {
		*s = 0;
		*t = Com_Clamp( 0, 1, f / e );
	} else {
		c = DotProduct( d1, r );
		if ( e < 1e-6f ) {
			*t = 0;
			*s = Com_Clamp( 0, 1, -c / a );
		} else {
			b = DotProduct( d1, d2 );
			denom = a * e - b * b;
			*s = denom > 1e-6f ? Com_Clamp( 0, 1, ( b * f - c * e ) / denom ) : 0;
			*t = ( b * *s + f ) / e;
			if ( *t < 0 ) {
				*t = 0;
				*s = Com_Clamp( 0, 1, -c / a );
			} else if ( *t > 1 ) {
				*t = 1;
				*s = Com_Clamp( 0, 1, ( b - c ) / a );
			}
		}
	}
	VectorMA( p1, *s, d1, c1 );
	VectorMA( p2, *t, d2, c2 );
	return Distance( c1, c2 );
}

// push a capsule's two points along n, shared by where the contact is (pinned points don't move)

#define RD_TONE_LEGS_MS     250
#define RD_TONE_BODY_MS     700

// how thick and heavy each segment is (kg; about 70 in all)
static const float rdSegRadius[HR_S_COUNT] = { 6.5f, 6.5f, 4.5f, 3.0f, 2.6f, 2.0f, 3.0f, 2.6f, 2.0f, 4.5f, 3.5f, 2.2f, 4.5f, 3.5f, 2.2f };
// (the hands, feet and forearms heavier than life: Jolt's joints go soft and jittery when a
// part is many times lighter than the one it hangs from)
static const float rdSegMass[HR_S_COUNT] = { 15, 15, 5, 4, 3, 2.5f, 4, 3, 2.5f, 10, 6, 3, 10, 6, 3 };
// vr_realWeights 1: a man's own weight distribution (about 89 kg): pelvis and lower back;
// chest, upper back and shoulders; the head; upper arm, forearm, hand; thigh, shin; foot.
// The physics steps twice as often for it (cg_physworld.c), which is what lets parts this light
// hang off heavy ones without going soft.
static const float rdSegMassReal[HR_S_COUNT] = { 11.75f, 16.45f, 8.3f, 3.5f, 1.6f, 1.0f, 3.5f, 1.6f, 1.0f, 9.65f, 5.7f, 4.6f, 9.65f, 5.7f, 4.6f };


// the segment that carries each point
static const int hrPointSeg[HR_P_COUNT] = {
	HR_S_TORSO, HR_S_CHEST, HR_S_CHEST, HR_S_HEAD, HR_S_HEAD,
	HR_S_CHEST, HR_S_LUPPER, HR_S_LFORE, HR_S_LHAND,
	HR_S_CHEST, HR_S_RUPPER, HR_S_RFORE, HR_S_RHAND,
	HR_S_TORSO, HR_S_LTHIGH, HR_S_LCALF, HR_S_LFOOT,
	HR_S_TORSO, HR_S_RTHIGH, HR_S_RCALF, HR_S_RFOOT,
};

static void RD_Dir( const vec3_t from, const vec3_t to, vec3_t out ) {
	VectorSubtract( to, from, out );
	if ( VectorNormalize( out ) < 1e-4f ) {
		VectorSet( out, 0, 0, 1 );
	}
}

// a ball joint: the limb swings round centre (planeHalf about plane, normalHalf across it) and twists
static void RD_SwingJoint( hrpPart_t *pp, const vec3_t pivot, const vec3_t centre, const vec3_t dir, const vec3_t plane,
						   float planeHalf, float normalHalf, float twist ) {
	pp->joint = HRP_JOINT_SWINGTWIST;
	VectorCopy( pivot, pp->pivot );
	VectorCopy( centre, pp->twist1 );
	VectorCopy( dir, pp->twist2 );
	VectorCopy( plane, pp->plane );
	pp->planeHalf = planeHalf;
	pp->normalHalf = normalHalf;
	pp->twistMin = -twist;
	pp->twistMax = twist;
}

// an elbow or knee: a-b the upper part, b-c the lower, bending one way only (defAxis: about
// which a straight limb bends) from straight to maxBend degrees
static void RD_HingeJoint( hrBody_t *rd, hrpPart_t *pp, int a, int b, int c, const vec3_t defAxis, float maxBend ) {
	vec3_t u, v, cr, axis;
	float s, bend0;

	RD_Dir( rd->p0[a], rd->p0[b], u );
	RD_Dir( rd->p0[b], rd->p0[c], v );
	VectorMA( defAxis, -DotProduct( defAxis, u ), u, axis );
	if ( VectorNormalize( axis ) < 1e-3f ) {
		PerpendicularVector( axis, u );
	}
	CrossProduct( u, v, cr );
	s = VectorLength( cr );
	if ( s > 0.3f && DotProduct( cr, axis ) > 0 ) {
		VectorScale( cr, 1.0f / s, axis );      // clearly bent the right way: exactly the plane it bends in
	}
	bend0 = RAD2DEG( atan2( DotProduct( cr, axis ), DotProduct( u, v ) ) );
	pp->joint = HRP_JOINT_HINGE;
	VectorCopy( rd->p0[b], pp->pivot );
	VectorCopy( axis, pp->hingeAxis );
	pp->hingeMin = -bend0 - 3.0f;
	pp->hingeMax = maxBend - bend0;
	if ( pp->hingeMax < pp->hingeMin + 1.0f ) {
		pp->hingeMax = pp->hingeMin + 1.0f;
	}
}

int HR_BodyBuild( hrBody_t *rd, int realWeights ) {
	hrpPart_t parts[HRP_MAX_PARTS];
	unsigned char noCollide[HRP_MAX_PARTS * HRP_MAX_PARTS];
	vec3_t f[3], up, down, left, fwd;
	int s, n = 0, i, j;

	HR_Register( rd );
	if ( !HRP_Active() ) {
		return qfalse;
	}
	memset( parts, 0, sizeof( parts ) );
	rd->realW = realWeights;
	RD_TorsoFrame( rd->p0, f );         // up, left, back
	VectorCopy( f[0], up );
	VectorNegate( f[0], down );
	VectorCopy( f[1], left );
	VectorNegate( f[2], fwd );
	memset( parts, 0, sizeof( parts ) );

	for ( s = 0; s < HR_S_COUNT; s++ ) {     // (parents come before their children)
		hrpPart_t *pp;
		int from = hrSegments[s].from, to = hrSegments[s].to;
		float side = ( s == HR_S_LUPPER || s == HR_S_LTHIGH ) ? 1 : -1, upper;
		vec3_t centre, dir, out, mid, v;

		if ( !rd->segValid[s] || n >= HRP_MAX_PARTS ) {
			rd->segPart[s] = s == HR_S_TORSO ? 0 : rd->segPart[hrSegments[s].parent];     // turns with its parent
			continue;
		}
		pp = &parts[n];
		rd->segPart[s] = n;
		rd->partSeg[n] = s;
		pp->parent = s == HR_S_TORSO ? -1 : rd->segPart[hrSegments[s].parent];
		pp->mass = rd->realW ? rdSegMassReal[s] : rdSegMass[s];
		// (the muscles were tuned on the first weights: each joint's torque goes with its part's weight)
		pp->torqueScale = rd->realW ? rdSegMassReal[s] / rdSegMass[s] : 1.0f;
		pp->radius = rdSegRadius[s];
		VectorScale( left, side, out );

		if ( s == HR_S_TORSO || s == HR_S_CHEST ) {
			// boxes a little narrower than the shoulders (the arms hang beside them): the belly and
			// hips from the pelvis up to the chest, and the chest from there to the neck (with no
			// chest, one box from the pelvis to the neck)
			float shoulders = Distance( rd->p0[HR_P_LSHOULDER], rd->p0[HR_P_RSHOULDER] );
			int lo = s == HR_S_CHEST ? HR_P_CHEST : HR_P_PELVIS, hi = s == HR_S_TORSO && rd->segValid[HR_S_CHEST] ? HR_P_CHEST : HR_P_NECK;
			pp->shape = HRP_SHAPE_BOX;
			VectorCopy( rd->p0[lo], pp->a );
			VectorCopy( rd->p0[hi], pp->b );
			VectorCopy( left, pp->boxAxis + 0 );
			VectorCopy( up, pp->boxAxis + 3 );
			VectorCopy( fwd, pp->boxAxis + 6 );
			pp->boxHalf[0] = shoulders * 0.375f > 4.0f ? shoulders * 0.375f : 4.0f;
			pp->boxHalf[1] = Distance( rd->p0[lo], rd->p0[hi] ) * 0.5f + ( s == HR_S_CHEST ? 2.0f : 1.0f );
			pp->boxHalf[2] = 4.0f;
			if ( s == HR_S_TORSO && !rd->segValid[HR_S_CHEST] ) {
				pp->mass = rd->realW ? rdSegMassReal[HR_S_TORSO] + rdSegMassReal[HR_S_CHEST] : rdSegMass[HR_S_TORSO] + rdSegMass[HR_S_CHEST];
				pp->boxHalf[1] += 1.0f;
			}
		} else {
			pp->shape = HRP_SHAPE_CAPSULE;
			VectorCopy( rd->p0[from], pp->a );
			VectorCopy( rd->p0[to], pp->b );
		}

		RD_Dir( rd->p0[from], rd->p0[to], dir );
		switch ( s ) {
		case HR_S_CHEST:           // the spine: bends forward and back more than sideways, twists a little
			RD_SwingJoint( pp, rd->p0[HR_P_CHEST], up, dir, left, 40, 25, 30 );
			pp->toneMs = RD_TONE_BODY_MS;
			break;
		case HR_S_HEAD:            // on the neck: nods further than it tilts
			RD_SwingJoint( pp, rd->p0[HR_P_NECK], up, dir, left, 50, 40, 50 );
			pp->toneMs = RD_TONE_BODY_MS;
			break;
		case HR_S_LUPPER:
		case HR_S_RUPPER:          // shoulders: almost anywhere, round a point out, down and a bit forward
			VectorScale( down, 0.7f, centre );
			VectorMA( centre, 0.7f, out, centre );
			VectorMA( centre, 0.3f, fwd, centre );
			VectorNormalize( centre );
			RD_SwingJoint( pp, rd->p0[from], centre, dir, fwd, 85, 85, 70 );
			pp->toneMs = RD_TONE_BODY_MS * 0.7f;
			break;
		case HR_S_LFORE:           // elbows bend forwards: about the body's right, a knee about its left
			VectorNegate( left, v );
			RD_HingeJoint( rd, pp, HR_P_LSHOULDER, HR_P_LELBOW, HR_P_LWRIST, v, 150 );
			pp->toneMs = RD_TONE_BODY_MS * 0.7f;
			break;
		case HR_S_RFORE:
			VectorNegate( left, v );
			RD_HingeJoint( rd, pp, HR_P_RSHOULDER, HR_P_RELBOW, HR_P_RWRIST, v, 150 );
			pp->toneMs = RD_TONE_BODY_MS * 0.7f;
			break;
		case HR_S_LHAND:
		case HR_S_RHAND:
			RD_Dir( rd->p0[hrSegments[hrSegments[s].parent].from], rd->p0[from], centre );
			RD_SwingJoint( pp, rd->p0[from], centre, dir, left, 60, 35, 35 );
			pp->toneMs = RD_TONE_BODY_MS * 0.7f;
			break;
		case HR_S_LTHIGH:
		case HR_S_RTHIGH:          // hips: well forward, a little back, a little across, further out
			VectorScale( down, 0.707f, centre );
			VectorMA( centre, 0.707f, fwd, centre );
			VectorMA( centre, 0.2f, out, centre );
			VectorNormalize( centre );
			RD_SwingJoint( pp, rd->p0[from], centre, dir, left, 70, 33, 30 );
			pp->toneMs = RD_TONE_LEGS_MS;
			break;
		case HR_S_LCALF:           // knees bend backwards
			RD_HingeJoint( rd, pp, HR_P_LHIP, HR_P_LKNEE, HR_P_LANKLE, left, 140 );
			pp->toneMs = RD_TONE_LEGS_MS;
			break;
		case HR_S_RCALF:
			RD_HingeJoint( rd, pp, HR_P_RHIP, HR_P_RKNEE, HR_P_RANKLE, left, 140 );
			pp->toneMs = RD_TONE_LEGS_MS;
			break;
		case HR_S_LFOOT:
		case HR_S_RFOOT:
			RD_SwingJoint( pp, rd->p0[from], dir, dir, left, 30, 15, 10 );
			pp->toneMs = RD_TONE_LEGS_MS;
			break;
		}

		if ( pp->joint == HRP_JOINT_HINGE ) {
			VectorCopy( pp->hingeAxis, rd->segHinge0[s] );
		}

		// starting motion, as RD_Build gives the points: the upper body pushed harder, so it tips over
		VectorAdd( pp->a, pp->b, mid );
		VectorScale( mid, 0.5f, mid );
		upper = ( mid[2] - rd->p0[HR_P_PELVIS][2] ) / 40.0f;
		upper = upper < 0 ? 0.3f : ( upper > 1 ? 1.3f : 0.3f + upper );
		VectorMA( rd->startVel, upper, rd->killPush, pp->vel );
		n++;
	}

	// parts that were already inside each other at death (an arm through the chest) pass through until apart
	memset( noCollide, 0, sizeof( noCollide ) );
	for ( i = 0; i < n; i++ ) {
		for ( j = i + 1; j < n; j++ ) {
			float st, tt, ri = parts[i].shape == HRP_SHAPE_BOX ? 6.5f : parts[i].radius;
			if ( RD_SegmentDist( parts[i].a, parts[i].b, parts[j].a, parts[j].b, &st, &tt ) < ri + parts[j].radius ) {
				noCollide[i * n + j] = 1;
			}
		}
	}

	rd->phys = HRP_CreateRagdoll( parts, n, noCollide );
	if ( !rd->phys ) {
		return qfalse;
	}
	rd->numParts = n;
	HRP_RagdollPose( rd->phys, rd->part0Pos[0], rd->part0Axis[0][0] );
	memcpy( rd->partPos, rd->part0Pos, sizeof( rd->partPos ) );
	memcpy( rd->partAxis, rd->part0Axis, sizeof( rd->partAxis ) );
	return qtrue;
}

// where the bodies are now -> each part's turn, and the points
void HR_BodyReadPose( hrBody_t *rd ) {
	int j, k;

	if ( !rd->phys ) {
		return;
	}
	HRP_RagdollPose( rd->phys, rd->partPos[0], rd->partAxis[0][0] );
	for ( j = 0; j < rd->numParts; j++ ) {
		RD_FrameRotation( rd->partAxis[j], rd->part0Axis[j], rd->partRot[j] );
	}
	for ( k = 0; k < HR_P_COUNT; k++ ) {
		vec3_t rel;
		if ( !rd->hasPoint[k] ) {
			continue;
		}
		j = rd->segPart[hrPointSeg[k]];
		VectorSubtract( rd->p0[k], rd->part0Pos[j], rel );
		RD_MatVec( rd->partRot[j], rel, rd->p[k] );
		VectorAdd( rd->p[k], rd->partPos[j], rd->p[k] );
	}
}

#define RD_PHYS_ANCHOR_STEP 2.0f   // most a body is pulled back towards the server's per frame

// stay with the host's body for him (origin, library space)
static void RD_PhysAnchor( hrBody_t *rd, const vec3_t origin ) {
	vec3_t target, err;
	qboolean originMoved;
	float d;

	originMoved = Distance( origin, rd->lastOrigin ) > 0.25f;
	VectorCopy( origin, rd->lastOrigin );
	VectorAdd( origin, rd->anchorOffset, target );
	VectorSubtract( target, rd->p[HR_P_PELVIS], err );
	err[2] = 0;
	d = VectorLength( err );
	// eased back a little each frame: pulled in one go, a body that slid away jumped back
	if ( d > ( originMoved ? RD_ANCHOR_SLACK : RD_ANCHOR_SLACK * 3 ) ) {
		float step = 0.25f * ( d - RD_ANCHOR_SLACK );
		VectorScale( err, ( step < RD_PHYS_ANCHOR_STEP ? step : RD_PHYS_ANCHOR_STEP ) / d, err );
		HRP_RagdollMove( rd->phys, err );
	}
	d = target[2] - rd->p[HR_P_PELVIS][2];
	if ( d > 40.0f && originMoved ) {
		float step = 0.25f * ( d - 40.0f );
		VectorSet( err, 0, 0, step < RD_PHYS_ANCHOR_STEP ? step : RD_PHYS_ANCHOR_STEP );
		HRP_RagdollMove( rd->phys, err );
	}
}


#define RD_FLOP_MS          300         // a limb that's shot goes limp this long, then pulls again

static qboolean RD_ActiveOn( void ) {
	return hrOpt.active != 0;
}

static qboolean RD_SegIsLeg( int s ) {
	return s == HR_S_LTHIGH || s == HR_S_LCALF || s == HR_S_LFOOT || s == HR_S_RTHIGH || s == HR_S_RCALF || s == HR_S_RFOOT;
}

// the limb a segment is part of: 1/2 left/right arm, 3/4 left/right leg, 0 the trunk or head
static int RD_LimbOf( int s ) {
	static const int limb[HR_S_COUNT] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4 };
	return s >= 0 && s < HR_S_COUNT ? limb[s] : 0;
}

/*
==============================================================================

Euphoria-style deaths (vr_activeEuphoria): instead of chasing a death animation, a body
that's still got some fight in it tries to stay on its feet. It staggers the way the shot
drove it, stepping to catch itself, doubled over a gut wound or reeling from a chest shot,
a hand clutching the wound and the other arm out for balance; then it gives, throws its
arms out to break the fall, and lies there curled and clutching until it goes limp. The
pose is built here per part (world directions for each limb, as the phase wants them) and
handed to the motors; HRP_RagdollSupport gives the torso the balance a body can't find on
its own.

==============================================================================
*/

#define HR_EU_STAGGER       1
#define HR_EU_FALL          2
#define HR_EU_LYING         3
#define RD_EU_LYING_MS      2500.0f     // curled and clutching, fading to limp
#define RD_EU_FALL_MS       1200.0f     // at most this long falling
#define RD_EU_STEP_HZ       2.2f        // stagger steps per second (each leg)
#define RD_EU_ROAM          64.0f       // the stagger carries him at most this far


static qboolean RD_EuOn( void ) {
	return hrOpt.euphoria != 0;
}

static qboolean RD_SegOk( hrBody_t *rd, int s ) {
	return s >= 0 && s < HR_S_COUNT && rd->segValid[s] && rd->partSeg[rd->segPart[s]] == s;
}

static void RD_RotV( const vec3_t v, const vec3_t axis, float angle, vec3_t out ) {
	vec3_t m[3];
	RD_AxisAngle( axis, angle, m );
	RD_MatVec( m, v, out );
}

static void RD_Norm3( float x, const vec3_t a, float y, const vec3_t b, float z, const vec3_t c, vec3_t out ) {
	int i;
	for ( i = 0; i < 3; i++ ) {
		out[i] = x * a[i] + y * b[i] + z * c[i];
	}
	if ( VectorNormalize( out ) < 1e-4f ) {
		VectorCopy( a, out );
	}
}

// orthonormal frame from a direction and a second vector across it (0 if they're parallel)
static qboolean RD_Frame2( const vec3_t d, const vec3_t h, vec3_t f[3] ) {
	VectorCopy( d, f[0] );
	VectorMA( h, -DotProduct( h, d ), d, f[1] );
	if ( VectorNormalize( f[1] ) < 0.05f ) {
		return qfalse;
	}
	CrossProduct( f[0], f[1], f[2] );
	return qtrue;
}

/*
=================
RD_EuLimb

a limb (upper segment upperSeg, then its hinge and its end) set as Q[part] world turns from
the death pose: the upper part along u with its elbow or knee bending about n, bent by bend
radians. R: the torso's turn, which the limb starts from.
=================
*/
static void RD_EuLimb( hrBody_t *rd, vec3_t Q[][3], vec3_t R[3], int upperSeg, const vec3_t u, const vec3_t n, float bend ) {
	int lower = upperSeg + 1, end = upperSeg + 2, ju, jl;
	vec3_t d0, h0, F0[3], F1[3], A[3], fw;

	if ( !RD_SegOk( rd, upperSeg ) ) {
		return;
	}
	ju = rd->segPart[upperSeg];
	RD_Dir( rd->p0[hrSegments[upperSeg].from], rd->p0[hrSegments[upperSeg].to], d0 );
	RD_MatVec( R, d0, d0 );
	if ( RD_SegOk( rd, lower ) ) {
		RD_MatVec( R, rd->segHinge0[lower], h0 );
	} else {
		PerpendicularVector( h0, d0 );
	}
	if ( RD_Frame2( d0, h0, F0 ) && RD_Frame2( u, n, F1 ) ) {
		RD_FrameRotation( F1, F0, A );      // along u, the hinge across n
	} else {
		RD_Align( d0, u, A );
	}
	RD_MatMul( A, R, Q[ju] );
	if ( !RD_SegOk( rd, lower ) ) {
		return;
	}
	jl = rd->segPart[lower];
	RD_Dir( rd->p0[hrSegments[lower].from], rd->p0[hrSegments[lower].to], d0 );
	RD_MatVec( Q[ju], d0, d0 );
	if ( !RD_Frame2( u, n, F1 ) ) {
		VectorCopy( u, fw );
	} else {
		RD_RotV( u, F1[1], bend, fw );
	}
	RD_Align( d0, fw, A );
	RD_MatMul( A, Q[ju], Q[jl] );
	if ( RD_SegOk( rd, end ) ) {
		memcpy( Q[rd->segPart[end]], Q[jl], sizeof( vec3_t ) * 3 );     // hand or foot: as it was on the limb
	}
}

// the same, given where the upper and lower parts point
static void RD_EuLimbTo( hrBody_t *rd, vec3_t Q[][3], vec3_t R[3], int upperSeg, const vec3_t u, const vec3_t f ) {
	vec3_t n;
	float c = DotProduct( u, f );

	CrossProduct( u, f, n );
	if ( VectorNormalize( n ) < 0.05f ) {
		if ( RD_SegOk( rd, upperSeg + 1 ) ) {
			RD_MatVec( R, rd->segHinge0[upperSeg + 1], n );
		} else {
			PerpendicularVector( n, u );
		}
	}
	RD_EuLimb( rd, Q, R, upperSeg, u, n, acos( c > 1 ? 1 : ( c < -1 ? -1 : c ) ) );
}

// an arm reaching its wrist to w, the elbow out towards pole
static void RD_EuReach( hrBody_t *rd, vec3_t Q[][3], vec3_t R[3], int upperSeg, int shoulder, const vec3_t w, const vec3_t pole ) {
	float a = Distance( rd->p0[shoulder], rd->p0[shoulder + 1] ), b = Distance( rd->p0[shoulder + 1], rd->p0[shoulder + 2] );
	float c, ca, sa;
	vec3_t dir, pp, e, u, f;

	VectorSubtract( w, rd->p[shoulder], dir );
	c = VectorNormalize( dir );
	if ( c > a + b - 0.5f ) {
		c = a + b - 0.5f;
	}
	if ( c < fabs( a - b ) + 1.0f ) {
		c = fabs( a - b ) + 1.0f;
	}
	ca = ( a * a + c * c - b * b ) / ( 2 * a * c );
	ca = ca > 1 ? 1 : ( ca < -1 ? -1 : ca );
	sa = sqrt( 1 - ca * ca );
	VectorMA( pole, -DotProduct( pole, dir ), dir, pp );
	if ( VectorNormalize( pp ) < 1e-3f ) {
		PerpendicularVector( pp, dir );
	}
	VectorScale( dir, a * ca, e );
	VectorMA( e, a * sa, pp, e );           // the elbow, from the shoulder
	VectorCopy( e, u );
	VectorNormalize( u );
	VectorScale( dir, c, f );
	VectorSubtract( f, e, f );
	VectorNormalize( f );
	RD_EuLimbTo( rd, Q, R, upperSeg, u, f );
}

/*
=================
RD_EuStart

hitSeg/hitPart: where the killing shot went in (-1 none), region HR_RG_*; wasDowned: already
on the floor writhing (finished off), so he goes straight to lying there
=================
*/
static void RD_EuStart( hrBody_t *rd, int hitSeg, int hitPart, const vec3_t hitPoint, int region, qboolean wasDowned ) {
	// how long a dying man stays on his feet (he fights for them; RD_EuFrame's balance decides
	// if it's shorter), and how long he writhes on the floor after, by where he was shot
	static const float dur[] = { 0, 2800, 2000, 2200, 1100 };
	static const float lyMs[] = { 5000, 10000, 7000, 7500, 7500 };
	vec3_t tf[3], rel;
	int k;

	rd->euRegion = region;
	rd->euHurtLimb = hitSeg >= 0 ? RD_LimbOf( hitSeg ) : 0;
	rd->euWoundPart = -1;
	if ( region != HR_RG_NONE && hitPart >= 0 && hitPart < rd->numParts ) {
		rd->euWoundPart = hitPart;
		VectorSubtract( hitPoint, rd->partPos[hitPart], rel );
		for ( k = 0; k < 3; k++ ) {
			rd->euWoundLocal[k] = DotProduct( rel, rd->partAxis[hitPart][k] );
		}
	}
	RD_TorsoFrame( rd->p, tf );     // up, left, back
	VectorSet( rd->euPush, rd->killPush[0], rd->killPush[1], 0 );
	if ( VectorNormalize( rd->euPush ) < 1.0f ) {
		VectorSet( rd->euPush, tf[2][0], tf[2][1], 0 );     // no push: he reels back
		if ( VectorNormalize( rd->euPush ) < 1e-3f ) {
			VectorSet( rd->euPush, 1, 0, 0 );
		}
	}
	VectorCopy( rd->euPush, rd->euFall );
	VectorCopy( rd->p[HR_P_PELVIS], rd->euStartPelvis );
	rd->euGait = HR_Random() * 2 * M_PI;
	rd->balInit = qfalse;
	rd->balComH = 99;
	rd->balCpOut = 0;
	rd->balTiltSince = rd->balOutSince = 0;
	rd->euDur = dur[region] * ( 0.8f + 0.4f * HR_Random() );
	rd->euLyMs = lyMs[region] * ( 0.8f + 0.5f * HR_Random() );
	rd->euStart = rd->euPhaseStart = hrTime;
	if ( !wasDowned && !rd->living && !rd->blasted ) {
		HRP_RagdollScaleVelocity( rd->phys, 0.35f );  // shot, not thrown: he reacts to it on his feet
	}
	if ( rd->living ) {
		rd->euDur *= 0.35f;         // knocked down: he goes over
	}
	if ( wasDowned ) {
		rd->eu = HR_EU_LYING;
		rd->euStart -= 1000;        // (no easing in from the pose he was shot down in)
	} else if ( rd->euDur > 0 && rd->segValid[HR_S_LTHIGH] && rd->segValid[HR_S_RTHIGH] ) {
		rd->eu = HR_EU_STAGGER;
	} else {
		rd->eu = HR_EU_FALL;
	}
	rd->active = qtrue;
	rd->activeStart = hrTime;
}

static void RD_EuEnd( hrBody_t *rd, const vec3_t origin ) {
	rd->eu = 0;
	rd->active = qfalse;
	HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
	if ( rd->gravK && rd->gravK != 1.0f ) {
		rd->gravK = 1.0f;
		HRP_RagdollGravity( rd->phys, 1.0f );
	}
	HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
	HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
	HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
	HRP_RagdollCatch( rd->phys, 0, 0, 0 );
	VectorSubtract( rd->p[HR_P_PELVIS], origin, rd->anchorOffset );     // stays where he fell
	VectorCopy( origin, rd->lastOrigin );
}

/*
=================
RD_Balance

A balancer, as euphoria's: where his weight is heading (the capture point: the centre of mass
carried on by its speed, sqrt(height / g) seconds) is where a foot has to go for him to stop.
While it's over his feet he just stands, knees giving; when a shot or a shove carries it
outside, the foot that's further from it steps there, quickly if he's going fast, and the
next one after it if that wasn't enough. Too far for any step, he goes over (RD_EuFrame).

Gives where each ankle should be this frame (footAt); qfalse: no legs to balance on.
=================
*/
#define RD_BAL_ANKLE        4.0f        // the ankle above the floor
#define RD_BAL_SLACK        3.0f        // the capture point this far out of his feet: a step
#define RD_BAL_STRIDE       30.0f       // the longest step
#define RD_BAL_LIFT         5.0f        // the stepping foot lifted this much

static float RD_Dist2D( const vec3_t a, const vec3_t b ) {
	return sqrt( ( a[0] - b[0] ) * ( a[0] - b[0] ) + ( a[1] - b[1] ) * ( a[1] - b[1] ) );
}

static float RD_BalGround( hrBody_t *rd, const vec3_t at, float guess ) {
	hrTrace_t tr;
	vec3_t a, b;

	VectorSet( a, at[0], at[1], guess + 24 );
	VectorSet( b, at[0], at[1], guess - 48 );
	HR_TraceLine( rd, &tr, a, b );
	return tr.fraction < 1.0f && !tr.startsolid ? tr.endpos[2] : guess;
}

static qboolean RD_Balance( hrBody_t *rd, const vec3_t F, const vec3_t L, int hurtLeg, vec3_t footAt[2] ) {
	vec3_t com, cp, mid, d;
	float mass = 0, h, g, omega, err, dt;
	int j, side;

	if ( !rd->hasPoint[HR_P_LANKLE] || !rd->hasPoint[HR_P_RANKLE] || !RD_SegOk( rd, HR_S_LCALF ) || !RD_SegOk( rd, HR_S_RCALF ) ) {
		return qfalse;
	}
	// his centre of mass, and how fast it's going (smoothed)
	VectorClear( com );
	for ( j = 0; j < rd->numParts; j++ ) {
		float m = rd->realW ? rdSegMassReal[rd->partSeg[j]] : rdSegMass[rd->partSeg[j]];
		VectorMA( com, m, rd->partPos[j], com );
		mass += m;
	}
	VectorScale( com, 1.0f / mass, com );
	if ( !rd->balInit ) {
		rd->balInit = qtrue;
		rd->balSwing = -1;
		rd->balLastStep = 0;
		VectorCopy( rd->p[HR_P_LANKLE], rd->balPlant[0] );
		VectorCopy( rd->p[HR_P_RANKLE], rd->balPlant[1] );
		VectorCopy( com, rd->balCom );
		VectorClear( rd->balComVel );
		rd->balComTime = hrTime;
	}
	dt = ( hrTime - rd->balComTime ) * 0.001f;
	if ( dt > 0.001f ) {
		vec3_t v;
		VectorSubtract( com, rd->balCom, v );
		VectorScale( v, 1.0f / dt, v );
		VectorScale( rd->balComVel, 0.5f, rd->balComVel );
		VectorMA( rd->balComVel, 0.5f, v, rd->balComVel );
		VectorCopy( com, rd->balCom );
		rd->balComTime = hrTime;
	}

	// a planted foot dragged away from where it stood stands where it is now
	for ( side = 0; side < 2; side++ ) {
		const float *ank = rd->p[side ? HR_P_RANKLE : HR_P_LANKLE];
		if ( side != rd->balSwing && Distance( ank, rd->balPlant[side] ) > 12.0f ) {
			VectorCopy( ank, rd->balPlant[side] );
		}
	}

	// the capture point
	h = com[2] - ( ( rd->balPlant[0][2] + rd->balPlant[1][2] ) * 0.5f - RD_BAL_ANKLE );
	rd->balComH = h;
	h = h < 16 ? 16 : h > 60 ? 60 : h;
	g = hrGravity * ( rd->gravK > 0 ? rd->gravK : 1.0f );
	g = g < 100 ? 100 : g;
	omega = sqrt( g / h );
	VectorMA( com, 1.0f / omega, rd->balComVel, cp );
	cp[2] = com[2];
	{
		// how far past where a step could still catch him (RD_EuFrame: he goes down when it stays so)
		float reach = RD_BAL_STRIDE + 8.0f, n0 = RD_Dist2D( cp, rd->balPlant[0] ), n1 = RD_Dist2D( cp, rd->balPlant[1] );
		rd->balCpOut = ( n0 < n1 ? n0 : n1 ) - reach;
	}

	if ( rd->balSwing < 0 ) {
		// out past his feet: step
		VectorAdd( rd->balPlant[0], rd->balPlant[1], mid );
		VectorScale( mid, 0.5f, mid );
		VectorSubtract( cp, mid, d );
		d[2] = 0;
		err = VectorLength( d );
		{
			// how far out of the line between his feet (standing wide covers a sideways shove)
			vec3_t ab, ac;
			float t, len;
			VectorSubtract( rd->balPlant[1], rd->balPlant[0], ab );
			ab[2] = 0;
			len = VectorNormalize( ab );
			VectorSubtract( cp, rd->balPlant[0], ac );
			ac[2] = 0;
			t = DotProduct( ac, ab );
			t = t < 0 ? 0 : t > len ? len : t;
			VectorMA( ac, -t, ab, ac );
			err = VectorLength( ac );
		}
		if ( err > RD_BAL_SLACK && hrTime - rd->balLastStep > 25 ) {
			float d0 = RD_Dist2D( cp, rd->balPlant[0] ), d1 = RD_Dist2D( cp, rd->balPlant[1] );
			float speed = sqrt( rd->balComVel[0] * rd->balComVel[0] + rd->balComVel[1] * rd->balComVel[1] );
			int sw = d0 > d1 ? 0 : 1;
			if ( sw == hurtLeg && HR_Random() < 0.5f ) {
				sw = !sw;       // (a shot leg doesn't want to take it: sometimes the other steps across)
			}
			rd->balSwing = sw;
			rd->balSwingStart = hrTime;
			// (quick: a man catching himself snatches his foot across, he doesn't stroll)
			rd->balSwingDur = (int)( 250 - speed * 0.6f );
			rd->balSwingDur = rd->balSwingDur < 120 ? 120 : rd->balSwingDur > 250 ? 250 : rd->balSwingDur;
			VectorCopy( rd->p[sw ? HR_P_RANKLE : HR_P_LANKLE], rd->balFrom );
		}
	}

	if ( rd->balSwing >= 0 ) {
		int sw = rd->balSwing;
		float s = ( hrTime - rd->balSwingStart ) / (float)rd->balSwingDur, e;
		const float *stance = rd->balPlant[!sw];
		vec3_t to, off;

		// where it lands: on the capture point, a hip's width to its own side, no further than a stride
		// (aimed again as he goes, until it's nearly down)
		if ( s < 0.6f ) {
			VectorScale( L, sw ? -4.0f : 4.0f, off );
			VectorAdd( cp, off, to );
			VectorSubtract( to, stance, d );
			d[2] = 0;
			if ( VectorLength( d ) > RD_BAL_STRIDE ) {
				VectorNormalize( d );
				VectorMA( stance, RD_BAL_STRIDE, d, to );
			}
			to[2] = RD_BalGround( rd, to, stance[2] - RD_BAL_ANKLE ) + RD_BAL_ANKLE;
			VectorCopy( to, rd->balTo );
		}
		if ( s >= 1.0f ) {
			VectorCopy( rd->balTo, rd->balPlant[sw] );
			rd->balSwing = -1;
			rd->balLastStep = hrTime;
		} else {
			e = s * s * ( 3 - 2 * s );
			for ( j = 0; j < 3; j++ ) {
				rd->balFoot[j] = rd->balFrom[j] + ( rd->balTo[j] - rd->balFrom[j] ) * e;
			}
			rd->balFoot[2] += RD_BAL_LIFT * sin( s * M_PI );
		}
	}

	for ( side = 0; side < 2; side++ ) {
		if ( side == rd->balSwing ) {
			VectorCopy( rd->balFoot, footAt[side] );
		} else {
			VectorCopy( rd->balPlant[side], footAt[side] );
			if ( side == hurtLeg ) {
				footAt[side][2] += 5.0f;        // (the knee gives on a shot leg)
			}
		}
	}
	(void)F;
	return qtrue;
}

/*
=================
RD_EuFrame

one frame of it: the phase, the pose each limb wants, how hard it pulls, and the balance help
=================
*/
static void RD_EuFrame( hrBody_t *rd, const vec3_t origin, qboolean grabbed, float scale, int off ) {
	static vec3_t Q[HRP_MAX_PARTS][3];
	vec3_t R[3], tf[3], U, L, F, G, W, up, a, u, f, pole, d, Z = { 0, 0, 1 }, down;
	float strength[HRP_MAX_PARTS], t, k = 0, fade = 1, tuck, phase, legS, armS, headS, clutchS, hurtArmS;
	float toneHz[HRP_MAX_PARTS], toneTq[HRP_MAX_PARTS], flail = 0;
	float wr = 0, gasp = 0, wts = 0, wseed = rd->id * 2.71f, life = 1;
	qboolean anyTone = qfalse;
	int j, side, clutch = 0, hurtLeg = rd->euHurtLimb == 3 ? 0 : rd->euHurtLimb == 4 ? 1 : -1;
	qboolean wound = rd->euWoundPart >= 0, balanced = qfalse;
	vec3_t footAt[2];

	RD_TorsoFrame( rd->p, tf );     // up, left, back
	t = hrTime - rd->euPhaseStart;
	{
		// staggering and falling with weight (real gravity alone reads as floating); lying, plain
		float gk = rd->eu == HR_EU_LYING ? 1.0f : 1.5f;
		if ( rd->gravK != gk ) {
			rd->gravK = gk;
			HRP_RagdollGravity( rd->phys, gk );
		}
	}
	if ( rd->eu == HR_EU_STAGGER ) {
		// he fights for his feet: only down when he really can't catch himself. Tipped past
		// 50 degrees for a while (a moment's lurch he rights), past 70 at once, his weight sunk
		// low (the legs gave), or the shove carrying him further than any step could catch
		if ( tf[0][2] < ( rd->stun && rd->living ? 0.57f : rd->living ? 0.64f : 0.5f ) ) {
			if ( !rd->balTiltSince ) {
				rd->balTiltSince = hrTime;
			}
		} else {
			rd->balTiltSince = 0;
		}
		if ( rd->balCpOut > 0 ) {
			if ( !rd->balOutSince ) {
				rd->balOutSince = hrTime;
			}
		} else {
			rd->balOutSince = 0;
		}
	}
	if ( rd->eu == HR_EU_STAGGER && rd->stun && rd->living ) {
		// shot and still alive: he goes down the way a man does, toppled. A leg gone, grabbed, or
		// driven over past 55 degrees and not righted within a moment, despite his steps and
		// the righting below; his legs hold his height meanwhile, so never by his knees folding
		if ( grabbed || ( off & ( ( 1 << 3 ) | ( 1 << 4 ) ) ) || ( rd->balTiltSince && hrTime - rd->balTiltSince > 300 ) ) {
			if ( ( hrOpt.debug & 2 ) ) HR_Print( "VRLOG fall %d: %s tilt %.2f comH %.0f\n", rd->id, grabbed ? "grabbed" : ( off & ( ( 1 << 3 ) | ( 1 << 4 ) ) ) ? "leg off" : "tipped over", tf[0][2], rd->balComH );
			t = rd->euDur;      // (into the fall below)
		} else {
			t = hrTime - rd->euPhaseStart;
		}
	}
	if ( rd->eu == HR_EU_STAGGER &&
		 ( rd->stun && rd->living ? t >= rd->euDur :
		   ( t >= rd->euDur || tf[0][2] < ( rd->living ? 0.34f : 0.26f ) || ( rd->balTiltSince && hrTime - rd->balTiltSince > ( rd->living ? 450 : 650 ) ) ||
			 rd->balComH < ( rd->living ? 20.0f : 16.0f ) ||
			 ( rd->balOutSince && hrTime - rd->balOutSince > ( rd->living ? 350 : 650 ) ) || grabbed ||
			 ( off & ( ( 1 << 3 ) | ( 1 << 4 ) ) ) ) ) ) {
		// given out (or tipped over, or sunk, or shoved past catching, or lost a leg): going down
		if ( rd->stun && rd->living ) {
			// shoved over while stunned: down, and he'll get up again (the server's knockdown)
			HR_Event( rd, HR_EV_FELL, NULL );
			rd->fellSent = qtrue;
			rd->stun = qfalse;
			rd->downed = qtrue;
			rd->downedAt = hrTime;
			if ( ( hrOpt.debug & 1 ) ) {
				HR_Print( "ragdoll %d: shoved over\n", rd->id );
			}
		}
		rd->eu = HR_EU_FALL;
		rd->euPhaseStart = hrTime;
		t = 0;
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		VectorSet( rd->euFall, tf[0][0], tf[0][1], 0 );
		if ( VectorNormalize( rd->euFall ) < 0.15f ) {
			VectorCopy( rd->euPush, rd->euFall );
		}
	}
	if ( rd->eu == HR_EU_FALL ) {
		hrTrace_t tr;
		vec3_t end;
		VectorCopy( rd->p[HR_P_NECK], end );
		end[2] -= 20;
		HR_TraceLine( rd, &tr, rd->p[HR_P_NECK], end );
		if ( t > RD_EU_FALL_MS || tr.fraction < 1.0f ) {
			rd->eu = HR_EU_LYING;       // down
			rd->euPhaseStart = hrTime;
			t = 0;
			VectorSubtract( rd->p[HR_P_PELVIS], origin, rd->anchorOffset );
			VectorCopy( origin, rd->lastOrigin );
			if ( rd->downed && rd->living ) {
				// alive: the server's body goes where he fell, so he gets up there (G_VRX_DownPos_f)
				HR_EventPos( rd );
				rd->anchorOffset[0] = rd->anchorOffset[1] = 0;
			}
		}
	}
	if ( rd->eu == HR_EU_LYING && rd->downed ) {
		fade = 0.6f;        // knocked down, alive: lies there hurting until he gets up
	} else if ( rd->eu == HR_EU_LYING ) {
		fade = 1.0f - t / ( rd->euLyMs > 500 ? rd->euLyMs : RD_EU_LYING_MS );
		if ( fade <= 0 ) {
			RD_EuEnd( rd, origin );
			return;
		}
	}
	if ( rd->eu == HR_EU_LYING ) {
		// writhing: how much is left in him (down but alive: plenty, it goes on; dying: draining
		// away, the gasps slowing), and the gasp itself, a sharp heave that everything rides on
		// (back arching, knees drawn up, head thrown back), never the same from one to the next
		float rate;
		wts = ( hrTime - rd->euStart ) * 0.001f;
		wr = rd->downed ? 1.15f : ( fade > 0 ? 1.5f * pow( fade, 0.4f ) : 0 );
		wr = wr > 1.3f ? 1.3f : wr;       // (a man fighting for breath: it holds, then goes quickly at the end)
		rate = rd->downed ? 0.9f : 0.55f + 0.5f * fade;     // gasps a second
		gasp = sin( 2 * M_PI * rate * wts + wseed ) * 0.75f + sin( 2 * M_PI * rate * 0.37f * wts + wseed * 1.9f ) * 0.25f;
		gasp = gasp > 0 ? gasp * gasp : 0;
	}

	// the frame limbs are placed in: standing, the world's up and his heading; lying, his own torso
	if ( rd->eu == HR_EU_LYING ) {
		VectorCopy( tf[0], U );
		VectorCopy( tf[1], L );
		VectorNegate( tf[2], F );
	} else {
		VectorCopy( Z, U );
		VectorSet( F, -tf[2][0], -tf[2][1], 0 );
		if ( VectorNormalize( F ) < 0.2f ) {
			VectorSet( F, tf[0][0], tf[0][1], 0 );
			if ( VectorNormalize( F ) < 1e-3f ) {
				VectorSet( F, 1, 0, 0 );
			}
		}
		CrossProduct( U, F, L );
	}
	VectorNegate( U, down );
	VectorCopy( rd->euFall, G );

	RD_FrameRotation( rd->partAxis[0], rd->part0Axis[0], R );
	for ( j = 0; j < rd->numParts; j++ ) {
		memcpy( Q[j], R, sizeof( R ) );     // to start with, as he was at death against his torso
	}

	// the wound, where it is now, a little out from the part so the hand lies on it
	if ( wound ) {
		int wp = rd->euWoundPart;
		RD_MatVec( rd->partAxis[wp], rd->euWoundLocal, W );
		VectorAdd( W, rd->partPos[wp], W );
		VectorSubtract( W, rd->partPos[wp], d );
		if ( VectorNormalize( d ) > 0.1f ) {
			VectorMA( W, 3.0f, d, W );
		}
		switch ( rd->euRegion ) {
		case HR_RG_GUT:
			// both hands; falling, one breaks the fall
			clutch = rd->eu != HR_EU_FALL ? 3 : Distance( W, rd->p[HR_P_LSHOULDER] ) < Distance( W, rd->p[HR_P_RSHOULDER] ) ? 1 : 2;
			break;
		case HR_RG_CHEST:
			// on his feet both hands go to it (the further one lets go to flail if he's going
			// over, below); falling or down, the nearer
			clutch = rd->eu == HR_EU_STAGGER ? 3 : Distance( W, rd->p[HR_P_LSHOULDER] ) < Distance( W, rd->p[HR_P_RSHOULDER] ) ? 1 : 2;
			break;
		case HR_RG_ARM:         // the other hand holds the arm
			clutch = rd->euHurtLimb == 1 ? 2 : 1;
			break;
		case HR_RG_LEG:
			clutch = rd->eu == HR_EU_FALL ? 0 : ( rd->euHurtLimb == 3 ? 1 : 2 );
			break;
		}
		if ( rd->euHurtLimb == 1 || ( off & ( 1 << 1 ) ) ) {
			clutch &= ~1;
		}
		if ( rd->euHurtLimb == 2 || ( off & ( 1 << 2 ) ) ) {
			clutch &= ~2;
		}
	}

	// how he holds his torso (HRP_RagdollSupport), and how far into the stagger he is
	if ( rd->eu == HR_EU_STAGGER ) {
		float roam;
		vec3_t dd;
		k = rd->stun ? 0 : t / rd->euDur;     // (stunned: no giving out, the bullets decide)
		switch ( rd->euRegion ) {
		case HR_RG_GUT:         // doubled over
			RD_Norm3( 0.85f, Z, 0.3f * ( t < 300 ? t / 300 : 1 ), F, 0, F, up );
			break;
		case HR_RG_CHEST:       // knocked back, then hunched
			if ( t < 250 ) {
				RD_Norm3( 1, Z, 0.35f, rd->euPush, 0, F, up );
			} else {
				RD_Norm3( 1, Z, 0.25f, F, 0, F, up );
			}
			break;
		case HR_RG_LEG:         // over onto the bad leg
			RD_Norm3( 1, Z, rd->euHurtLimb == 3 ? 0.3f : -0.3f, L, 0, F, up );
			break;
		default:
			RD_Norm3( 1, Z, 0.15f, rd->euPush, 0, F, up );
			break;
		}
		VectorSubtract( rd->p[HR_P_PELVIS], rd->euStartPelvis, dd );
		dd[2] = 0;
		roam = VectorLength( dd );
		if ( rd->stun ) {
			// stunned on his feet: just enough balance to stay up and stumble, so the bullets shove him about
			// (no lift: his legs carry him, or he goes down)
			// and the further he tips, the harder he fights to right himself (a righting reflex)
			// (and holds his weight up as he tips, so a hard shove makes him stagger, not fall)
			// (the upright help weakens as he tips, as euphoria's: his legs and his steps keep
			// him up, not a hand under him, and no lift: his legs carry his weight)
			float tip = ( 0.95f - tf[0][2] ) / 0.45f;
			// (a bullet knocks him back a step or two, not across the room: free to be shoved for a
			// quarter second after each hit, then he digs his heels in, and never further than a
			// couple of steps from where he stood)
			float since = rd->lastHitTime ? hrTime - rd->lastHitTime : 9999.0f;
			float drift = since < 250 ? 70.0f : since > 800 ? 10.0f : 70.0f - 60.0f * ( since - 250 ) / 550.0f;
			if ( roam > 40.0f && drift > 8.0f ) {
				drift = 8.0f;
			}
			tip = tip < 0 ? 0 : tip > 1 ? 1 : tip;
			HRP_RagdollSupport( rd->phys, grabbed ? 0 : ( 2.3f - 0.7f * tip ) * scale, 0, drift, up );
			// and a net under him: his knees buckle, the bullets rock and twist him and he
			// stumbles (all his own, free), but sinking well below how he stood, or leant far
			// past where he'd recover, he's caught, softly, so he can't go all the way down
			HRP_RagdollCatch( rd->phys, grabbed ? 0 : rd->stunPos[2], 4.0f, 48.0f );
		} else {
			HRP_RagdollCatch( rd->phys, 0, 0, 0 );
			// (dying on his feet: he holds himself up nearly as a living man would, and it
			// drains out of him towards the end, not from the first frame)
			HRP_RagdollSupport( rd->phys, grabbed ? 0 : ( rd->living ? 1.7f - 0.8f * k : 2.2f - 1.9f * k * k ) * scale * ( rd->downed ? 0.5f : 1.0f ),
								 grabbed || rd->downed ? 0 : 0.3f * ( 1.0f - k ),
								 roam > RD_EU_ROAM ? 4.0f : 40.0f, up );
		}
	} else {
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
	}

	// his weight on his legs (HRP_RagdollLegPush): each foot on the floor pushes him up, so
	// the leg muscles can be soft and his knees give under the shots; a shot leg carries less
	if ( rd->eu == HR_EU_STAGGER && !grabbed ) {
		// (all of it, on one leg too: at 0.9, and 0.7 of that while the other foot stepped, he sank
		// onto his heels with every step, a man with no knees. Dying, his legs hold nearly to the
		// end: he goes over, he doesn't fold)
		float total = rd->stun ? 1.0f : rd->living ? 0.95f * ( 1.0f - k ) : 1.0f * ( 1.0f - k * k * k * k ), share[2];     // (stunned alive: the torso's spring carries him)
		int foot[2], planted[2];
		for ( side = 0; side < 2; side++ ) {
			int fs = side ? HR_S_RFOOT : HR_S_LFOOT, cs = side ? HR_S_RCALF : HR_S_LCALF, ap = side ? HR_P_RANKLE : HR_P_LANKLE;
			hrTrace_t tr;
			vec3_t end;
			foot[side] = RD_SegOk( rd, fs ) ? rd->segPart[fs] : RD_SegOk( rd, cs ) ? rd->segPart[cs] : -1;
			planted[side] = 0;
			if ( foot[side] < 0 || ( off & ( 1 << ( 3 + side ) ) ) || !rd->hasPoint[ap] ) {
				foot[side] = -1;
				continue;
			}
			VectorCopy( rd->p[ap], end );
			end[2] -= 9.0f;
			HR_TraceLine( rd, &tr, rd->p[ap], end );
			planted[side] = tr.fraction < 1.0f && !tr.startsolid && !( rd->balInit && side == rd->balSwing );    // (not the foot stepping)
		}
		for ( side = 0; side < 2; side++ ) {
			share[side] = !planted[side] ? 0 : planted[!side] ? total * 0.5f : total;
			if ( rd->euHurtLimb == 3 + side ) {
				share[side] *= 0.4f;
			}
		}
		HRP_RagdollLegPush( rd->phys, foot[0], foot[1], share[0] * scale, share[1] * scale );
	} else {
		HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
	}

	// the legs: on his feet, the balancer (RD_Balance) places them
	balanced = rd->eu == HR_EU_STAGGER && !grabbed && RD_Balance( rd, F, L, hurtLeg, footAt );
	phase = rd->euGait + 2 * M_PI * RD_EU_STEP_HZ * ( hrTime - rd->euStart ) * 0.001f;
	CrossProduct( U, rd->euPush, a );
	if ( VectorNormalize( a ) < 0.1f ) {
		VectorCopy( L, a );
	}
	for ( side = 0; side < 2; side++ ) {
		float sgn = side ? -1 : 1, bend, ph = phase + side * M_PI;
		int thigh = side ? HR_S_RTHIGH : HR_S_LTHIGH;
		qboolean hurt = rd->euHurtLimb == ( side ? 4 : 3 );

		if ( rd->eu == HR_EU_STAGGER && balanced ) {
			// the balancer has each foot's target: the knee forward, a little out
			int hip = side ? HR_P_RHIP : HR_P_LHIP;
			if ( rd->hasPoint[hip] && rd->hasPoint[hip + 1] && rd->hasPoint[hip + 2] ) {
				RD_Norm3( 1, F, 0.15f * sgn, L, 0, U, pole );
				RD_EuReach( rd, Q, R, thigh, hip, footAt[side], pole );
				continue;
			}
		}
		if ( rd->eu == HR_EU_STAGGER ) {
			// stepping the way the shot drove him, the swinging leg lifted, the knees sagging
			RD_RotV( down, a, -0.35f * sin( ph ), u );
			bend = 0.4f + 0.6f * ( cos( ph ) > 0 ? cos( ph ) : 0 ) + 0.5f * k + ( hurt ? 0.7f : 0 );
		} else if ( rd->eu == HR_EU_FALL ) {
			RD_RotV( down, L, -0.45f, u );      // knees buckling
			bend = 0.9f;
		} else {
			// curled up, and writhing: each knee drawn up and pushed out again in its own time,
			// both drawn up at a gasp, the knees rolling side to side; a shot leg barely moves
			float flex = rd->euRegion == HR_RG_GUT ? 1.1f : ( wound ? 0.6f : 0.3f );
			float w = hurt ? wr * 0.25f : wr;
			float sw = sin( wts * 1.9f + side * M_PI + wseed ) * 0.7f + sin( wts * 3.1f + side * 2.2f + wseed * 1.3f ) * 0.3f;
			float sw2 = sin( wts * 1.1f + side * 1.3f + wseed * 2.0f );
			flex += w * ( 0.5f * sw + 0.4f * gasp );
			flex = flex < 0.05f ? 0.05f : flex;
			RD_RotV( down, L, -flex, u );
			bend = flex * 1.2f + w * 0.3f * sw2;
			bend = bend < 0.05f ? 0.05f : bend;
			VectorMA( u, w * 0.35f * sw2, L, u );
		}
		VectorMA( u, 0.1f * sgn, L, u );
		VectorNormalize( u );
		RD_EuLimb( rd, Q, R, thigh, u, L, bend );
	}

	// the arms. Off balance he flails to save himself: the arm on the high side thrown up and
	// out away from where he's going over (a counterweight), the one on the low side reaching
	// out that way for something to catch, both jerking about irregularly (no circles, no
	// rhythm you could see), harder the nearer he is to going over. Steady, they hang out for
	// balance, away from his lean, swinging with his steps. Each hit jerks them.
	vec3_t tipDir, lean;
	float tipK = 0, hitJerk = 0, seed = rd->id * 1.37f;
	if ( rd->eu == HR_EU_STAGGER ) {
		float speed = sqrt( rd->balComVel[0] * rd->balComVel[0] + rd->balComVel[1] * rd->balComVel[1] ), tilt;
		VectorSet( lean, tf[0][0], tf[0][1], 0 );
		tilt = VectorNormalize( lean );         // (sine of how far over he leans)
		if ( speed > 25.0f && speed / 120.0f > tilt * 1.6f ) {
			VectorSet( tipDir, rd->balComVel[0] / speed, rd->balComVel[1] / speed, 0 );
		} else if ( tilt > 0.05f ) {
			VectorCopy( lean, tipDir );
		} else {
			VectorCopy( rd->euPush, tipDir );
		}
		flail = speed / 140.0f;
		flail = flail > tilt * 2.2f ? flail : tilt * 2.2f;
		if ( rd->balCpOut > -6.0f ) {
			flail += 0.4f + ( rd->balCpOut + 6.0f ) / 20.0f;     // past where a step catches it
		}
		if ( rd->balTiltSince ) {
			flail += 0.5f;
		}
		flail = flail < 0.15f ? 0 : flail > 1 ? 1 : ( flail - 0.15f ) / 0.85f;
		// eased in and out so the arms don't snap between moods
		rd->armFlail += ( flail - rd->armFlail ) * ( flail > rd->armFlail ? 0.25f : 0.06f );
		flail = rd->armFlail;
		tipK = tilt;
	} else {
		rd->armFlail = 0;
	}
	if ( rd->lastHitTime && hrTime - rd->lastHitTime < 220 ) {
		float h = ( hrTime - rd->lastHitTime ) / 220.0f;
		hitJerk = sin( h * M_PI );          // up and back in a fifth of a second
	}
	for ( side = 0; side < 2; side++ ) {
		float sgn = side ? -1 : 1;
		int upper = side ? HR_S_RUPPER : HR_S_LUPPER, sh = side ? HR_P_RSHOULDER : HR_P_LSHOULDER;
		qboolean hurtArm = rd->euHurtLimb == ( side ? 2 : 1 );

		// fighting back (HR_Aim): staggering, he holds his gun on whoever shot him and fires,
		// each shot kicking the gun back and up (HR_Recoil). The shooting hand near the right
		// shoulder, the other out along the barrel; a shot arm hangs, and the other aims alone
		if ( rd->aimOn && rd->eu == HR_EU_STAGGER && !hurtArm ) {
			int shoot = rd->euHurtLimb == 2 ? HR_P_LSHOULDER : HR_P_RSHOULDER;
			float rt = (float)( hrTime - rd->recoilTime ), r = rd->recoilTime && rt >= 0 && rt < 400 ? exp( -rt / 90.0f ) : 0;
			vec3_t aim, hand;
			VectorSubtract( rd->aimPoint, rd->p[shoot], aim );
			if ( VectorNormalize( aim ) < 1.0f ) {
				VectorCopy( F, aim );
			}
			if ( ( side == 1 ) == ( shoot == HR_P_RSHOULDER ) ) {
				// the hand on the grip, the stock in the shoulder
				VectorMA( rd->p[shoot], 11.0f, aim, hand );
				VectorMA( hand, -2.0f, U, hand );
				VectorMA( hand, -5.0f * r, aim, hand );
				VectorMA( hand, 3.0f * r, U, hand );
			} else {
				// the other out along the barrel, a little in from the shoulder
				VectorMA( rd->p[shoot], 21.0f, aim, hand );
				VectorMA( hand, 3.0f * ( shoot == HR_P_RSHOULDER ? 1 : -1 ), L, hand );
				VectorMA( hand, -1.5f, U, hand );
				VectorMA( hand, -3.0f * r, aim, hand );
				VectorMA( hand, 4.5f * r, U, hand );
			}
			RD_Norm3( -0.8f, U, 0.45f * sgn, L, -0.2f, F, pole );     // elbow down and out
			RD_EuReach( rd, Q, R, upper, sh, hand, pole );
			continue;
		}

		// (a hand on the wound stays on it however hard he's fighting for his balance: the other
		// arm does the flailing. Both hands on his gut, the one further from the wound lets go
		// to flail when he's well off balance)
		if ( rd->eu == HR_EU_STAGGER && !hurtArm && flail > 0.05f &&
			 ( !( clutch & ( 1 << side ) ) ||
			   ( clutch == 3 && flail > 0.5f &&
				 ( Distance( W, rd->p[sh] ) > Distance( W, rd->p[side ? HR_P_LSHOULDER : HR_P_RSHOULDER] ) ) ) ) ) {
			// flailing to save himself, blended with the balance pose as it comes and goes
			float ts = ( hrTime - rd->euStart ) * 0.001f, low, n1, n2, n3, bendJ;
			vec3_t out, bu, bf, wu, wf, away;
			VectorScale( L, sgn, out );
			VectorNegate( tipDir, away );
			low = DotProduct( tipDir, out );        // > 0: he's going over towards this arm
			low = low < 0 ? 0 : low;
			// irregular jerks: a few unrelated wobbles summed, each arm its own (never a cycle)
			n1 = sin( ts * 7.3f + seed + side * 1.9f ) * 0.6f + sin( ts * 11.9f + seed * 2.3f + side ) * 0.4f;
			n2 = sin( ts * 5.1f + seed * 1.7f + side * 2.7f ) * 0.6f + sin( ts * 13.7f + seed + side * 0.6f ) * 0.4f;
			n3 = sin( ts * 9.1f + seed * 0.7f + side * 3.1f );
			if ( low > 0.3f ) {
				// the low side: out and down that way, grabbing at the air
				RD_Norm3( 0.9f, out, 0.7f * low, tipDir, -0.25f + 0.3f * n1 * flail, U, wu );
			} else {
				// the high side: flung up and out, away from the fall, a counterweight
				RD_Norm3( 0.8f, out, 0.25f + 0.55f * flail + 0.3f * n1 * flail, U, 0.5f * flail + 0.25f * n2 * flail, away, wu );
			}
			VectorMA( wu, 0.25f * n2 * flail, F, wu );
			VectorNormalize( wu );
			bendJ = 0.35f + 0.25f * n3 * flail;    // the elbow working too
			RD_Norm3( 1, wu, bendJ, U, 0.3f * n1, F, wf );
			// the balance pose to blend from
			RD_Norm3( -0.35f, U, 0.85f * sgn, L, -0.3f * tipK, tipDir, bu );
			RD_Norm3( 1, bu, 0.6f, F, 0.2f, U, bf );
			RD_Norm3( 1 - flail, bu, flail, wu, 0, U, u );
			RD_Norm3( 1 - flail, bf, flail, wf, 0, U, f );
			RD_EuLimbTo( rd, Q, R, upper, u, f );
			continue;
		}
		if ( clutch & ( 1 << side ) ) {
			vec3_t w;
			VectorCopy( W, w );
			if ( clutch == 3 ) {
				VectorMA( w, 2.5f * sgn, L, w );      // both hands, side by side
			}
			RD_Norm3( -0.4f, F, 0.7f * sgn, L, -0.6f, U, pole );     // elbow out and back
			RD_EuReach( rd, Q, R, upper, sh, w, pole );
			continue;
		}
		if ( rd->eu == HR_EU_STAGGER ) {
			// out for balance, away from the way he leans, swinging with his steps (each arm its
			// own way), jerked up at a hit; a shot arm hangs and swings
			float fl = 0.22f * sin( phase * 0.5f + side * 1.7f + seed ), fl2 = 0.15f * sin( phase * 0.31f + side + seed * 2 );
			if ( hurtArm ) {
				RD_Norm3( 1, down, 0.25f * sgn, L, 0.2f * fl, F, u );
				RD_Norm3( 1, down, 0.35f, F, 0.1f * sgn, L, f );
			} else {
				RD_Norm3( -0.45f + fl + 0.5f * hitJerk, U, 0.85f * sgn, L, 0.15f + fl2 - 0.6f * tipK, lean, u );
				RD_Norm3( 1, u, 0.6f + 0.6f * hitJerk, F, 0.2f + 0.3f * hitJerk, U, f );
			}
		} else if ( rd->eu == HR_EU_FALL ) {
			vec3_t bu, bf, su, sf;
			float s = t < 120 ? 0 : t > 420 ? 1 : ( t - 120 ) / 300.0f;
			if ( DotProduct( G, F ) > -0.3f ) {         // forwards or sideways: hands out to break it
				RD_Norm3( 0.8f, G, -0.5f, U, 0.3f * sgn, L, bu );
				RD_Norm3( 0.6f, G, -0.75f, U, 0.15f * sgn, L, bf );
			} else {                                     // backwards: arms thrown out and back
				RD_Norm3( 0.7f * sgn, L, 0.45f, G, -0.5f, U, bu );
				RD_Norm3( 0.5f * sgn, L, 0.3f, G, -0.8f, U, bf );
			}
			// first thrown up and out as he goes (the startle, a grab at the air), jerking,
			// then reaching for the floor
			{
				float ts = t * 0.001f;
				float n1 = sin( ts * 8.3f + seed + side * 2.1f ) * 0.6f + sin( ts * 12.7f + seed * 1.9f + side ) * 0.4f;
				float n2 = sin( ts * 6.1f + seed * 1.3f + side * 2.9f );
				RD_Norm3( 0.7f + 0.25f * n1, U, 0.8f * sgn, L, -0.5f + 0.25f * n2, G, su );
				RD_Norm3( 1, su, 0.5f, U, 0.25f * n2, F, sf );
			}
			s = s * s * ( 3 - 2 * s );
			RD_Norm3( 1 - s, su, s, bu, 0, U, u );
			RD_Norm3( 1 - s, sf, s, bf, 0, U, f );
		} else {
			// lying: drawn in over the chest, then out clawing at the floor beside him and back
			float aw = sin( wts * 1.3f + side * 2.0f + wseed * 0.8f ) * 0.6f + sin( wts * 2.7f + side + wseed * 1.6f ) * 0.4f;
			float reach = wr * ( aw > 0 ? aw : 0 );
			RD_Norm3( -0.3f - 0.3f * wr * aw, U, 0.7f - 0.6f * reach, F, ( 0.3f + 0.8f * reach ) * sgn, L, u );
			RD_Norm3( 0.5f - 0.5f * reach, U, 0.4f - 0.2f * reach + 0.3f * wr * gasp, F, ( -0.6f + 1.0f * reach ) * sgn, L, f );
		}
		RD_EuLimbTo( rd, Q, R, upper, u, f );
	}

	// the head: on his feet he looks at where he was just shot (an arm, his belly), held up
	// by his neck, not lolling; tucked as he goes down
	tuck = rd->eu == HR_EU_STAGGER ? 0.05f : rd->eu == HR_EU_FALL ? 0.45f : 0.3f;
	if ( RD_SegOk( rd, HR_S_CHEST ) ) {
		// the spine: hunched over a gut wound, rocked back by a chest shot then curling over
		// it, a little bowed as he staggers, curled as he falls and lies
		vec3_t m[3];
		float bend;
		if ( rd->eu == HR_EU_STAGGER ) {
			bend = rd->euRegion == HR_RG_GUT ? 0.55f : rd->euRegion == HR_RG_CHEST ? ( t < 250 ? -0.3f : 0.35f ) : 0.2f;
		} else if ( rd->eu == HR_EU_FALL ) {
			bend = 0.35f;
		} else {
			// curled over it, his back arching at each gasp, twisting from side to side
			bend = ( rd->euRegion == HR_RG_GUT ? 0.6f : 0.3f ) - wr * 0.75f * gasp;
		}
		RD_AxisAngle( L, bend, m );
		RD_MatMul( m, R, Q[rd->segPart[HR_S_CHEST]] );
		if ( rd->eu == HR_EU_LYING && wr > 0 ) {
			RD_AxisAngle( U, 0.3f * wr * sin( wts * 1.3f + wseed ), m );
			RD_MatMul( m, Q[rd->segPart[HR_S_CHEST]], Q[rd->segPart[HR_S_CHEST]] );
		}
	}
	if ( RD_SegOk( rd, HR_S_HEAD ) ) {
		vec3_t m[3], tf0[3], fh, to, ax;
		int hp = rd->segPart[HR_S_HEAD];
		if ( rd->eu == HR_EU_LYING ) {
			tuck -= wr * 0.7f * gasp;       // thrown back at a gasp
		}
		RD_AxisAngle( L, tuck, m );
		RD_MatMul( m, Q[rd->segPart[HR_S_CHEST]], Q[hp] );     // (the chest's, or with no chest the torso's)
		if ( rd->eu == HR_EU_LYING && wr > 0 ) {
			RD_AxisAngle( U, 0.55f * wr * ( sin( wts * 0.9f + wseed * 3.0f ) * 0.7f + sin( wts * 2.3f + wseed ) * 0.3f ), m );    // rolling side to side
			RD_MatMul( m, Q[hp], Q[hp] );
		}
		if ( rd->eu == HR_EU_STAGGER && wound && rd->euWoundPart != hp ) {
			float ang, most = 1.2f;     // (about 70 degrees: further, his eyes do the rest)
			RD_TorsoFrame( rd->p0, tf0 );
			VectorNegate( tf0[2], fh );         // the way he faced at the start
			RD_MatVec( Q[hp], fh, fh );         // and now
			VectorSubtract( W, rd->partPos[hp], to );
			if ( VectorNormalize( to ) > 0.1f ) {
				CrossProduct( fh, to, ax );
				ang = VectorNormalize( ax );
				ang = asin( ang > 1 ? 1 : ang );
				if ( DotProduct( fh, to ) < 0 ) {
					ang = M_PI - ang;
				}
				if ( ang > 0.02f ) {
					RD_AxisAngle( ax, ang > most ? most : ang, m );
					RD_MatMul( m, Q[hp], Q[hp] );
				}
			}
		}
	}

	// how hard each part pulls
	life = rd->living || rd->eu != HR_EU_STAGGER ? 1.0f : 1.0f - 0.6f * k * k * k;
	if ( rd->eu == HR_EU_STAGGER && rd->living ) {
		// alive: his arms, head and back keep working (balancing, clutching, looking), never slack
		headS = 1.0f; armS = 1.0f; clutchS = 1.3f; legS = balanced ? 1.0f : 0.85f; hurtArmS = 0.4f;
	} else if ( rd->eu == HR_EU_STAGGER ) {
		// dying on his feet: still a man's strength at first, draining as he goes
		headS = 1.0f - 0.4f * k * k * k; armS = 1.0f; clutchS = 1.0f; legS = ( balanced ? 1.0f : 0.8f ) * ( 1.0f - 0.5f * k * k * k ); hurtArmS = 0.3f;
	} else if ( rd->eu == HR_EU_FALL ) {
		headS = 0.6f; armS = 1.0f; clutchS = 0.8f; legS = 0.25f; hurtArmS = 0.15f;

	} else {
		// on the floor, dying (or down and bleeding): he writhes, harder at each gasp
		float e = wr * ( 0.45f + 0.55f * gasp );
		headS = 0.9f * e; armS = 1.0f * e; legS = 1.0f * e; clutchS = 0.9f * wr; hurtArmS = 0.25f * e;
	}
	for ( j = 0; j < rd->numParts; j++ ) {
		int s = rd->partSeg[j], limb = RD_LimbOf( s );
		float st, ht;
		vec3_t out[3];

		if ( limb == 1 || limb == 2 ) {
			st = limb == rd->euHurtLimb ? hurtArmS : ( clutch & ( 1 << ( limb - 1 ) ) ) ? clutchS : armS;
		} else if ( limb == 3 || limb == 4 ) {
			st = limb == rd->euHurtLimb && rd->eu != HR_EU_LYING ? legS * 0.35f : legS;
		} else {
			st = headS;
		}
		if ( off & ( 1 << limb ) ) {
			st = 0;
		}
		ht = hrTime - rd->partHitTime[j];
		if ( rd->partHitTime[j] && ht >= 0 && ht < RD_FLOP_MS ) {
			// the part shot goes slack a moment (alive: knocked, not dead, and back sooner)
			if ( rd->eu == HR_EU_STAGGER ) {
				st *= ht < 110 ? 0.65f + 0.35f * ht / 110.0f : 1.0f;      // (a flinch, not a faint)
			} else {
				st *= ht / RD_FLOP_MS;
			}
		}
		strength[j] = grabbed ? 0 : st * scale;
		RD_MatMul( Q[j], rd->part0Axis[j], out );
		memcpy( rd->target + j * 9, out[0], sizeof( float ) * 9 );
		// staggering alive: legs stiff to stand on (hips and knees 10 Hz at full torque, the
		// ankles 8 Hz, enough to lean on, as a man balances on his feet), while the rest stays
		// soft and gives under the bullets; a shot leg stays soft, it buckles
		toneHz[j] = toneTq[j] = 0;
		if ( rd->eu == HR_EU_STAGGER && ( limb == 3 || limb == 4 ) && limb != rd->euHurtLimb ) {
			int w = s == HR_S_LFOOT || s == HR_S_RFOOT;
			toneHz[j] = ( w ? 10.0f : 12.5f ) * ( 0.4f + 0.6f * life );
			toneTq[j] = ( w ? 1.0f : 1.25f ) * life;
			anyTone = qtrue;
		} else if ( rd->eu == HR_EU_LYING && j > 0 && wr > 0.08f ) {
			// writhing: enough spring in him to draw a knee up, arch his back, roll his head
			float hz = ( limb == 3 || limb == 4 ) ? 7.0f : s == HR_S_CHEST ? 7.5f : 5.5f;
			toneHz[j] = hz * ( 0.5f + 0.5f * wr );
			toneTq[j] = ( limb == rd->euHurtLimb ? 0.4f : s == HR_S_CHEST ? 1.3f : 1.0f ) * wr;
			anyTone = qtrue;
		} else if ( rd->eu == HR_EU_STAGGER && ( rd->living || life > 0.3f ) && j > 0 ) {
			// and up top, a living man's muscle: the back firm, the neck holding his head up,
			// the arms strong enough to reach out, clutch and swing (a shot arm a little less)
			float hz = s == HR_S_CHEST ? 11.0f : s == HR_S_HEAD ? 9.0f : s == HR_S_LUPPER || s == HR_S_RUPPER ? 8.0f :
					   s == HR_S_LFORE || s == HR_S_RFORE ? 6.0f : 4.0f;
			float tq = s == HR_S_CHEST ? 1.3f : s == HR_S_HEAD ? 1.0f : s == HR_S_LUPPER || s == HR_S_RUPPER ? 1.0f :
					   s == HR_S_LFORE || s == HR_S_RFORE ? 0.8f : 0.5f;
			if ( limb == rd->euHurtLimb ) {
				hz *= 0.6f;
				tq *= 0.6f;
			} else if ( ( limb == 1 || limb == 2 ) && ( clutch & ( 1 << ( limb - 1 ) ) ) ) {
				hz *= 1.45f;        // (a hand going to the wound: fast and firm, it gets there and stays)
				tq = 1.3f;
			} else if ( limb == 1 || limb == 2 ) {
				hz *= 1.0f + 0.5f * flail;     // (flailing: quick enough to jerk about)
				tq = tq * ( 1.0f + 0.4f * flail ) > 1.3f ? 1.3f : tq * ( 1.0f + 0.4f * flail );
			}
			toneHz[j] = hz * ( 0.4f + 0.6f * life );
			toneTq[j] = tq * life;
			anyTone = qtrue;
		} else if ( rd->eu == HR_EU_FALL && rd->living && limb != rd->euHurtLimb &&
					( s == HR_S_LUPPER || s == HR_S_RUPPER || s == HR_S_LFORE || s == HR_S_RFORE ) ) {
			// alive and going down: arms with the life still in them (thrown up, flailing,
			// reaching for the floor), not dropped like a dead man's
			toneHz[j] = s == HR_S_LUPPER || s == HR_S_RUPPER ? 7.0f : 5.0f;
			toneTq[j] = s == HR_S_LUPPER || s == HR_S_RUPPER ? 0.9f : 0.6f;
			anyTone = qtrue;
		}
	}
	HRP_RagdollTone( rd->phys, anyTone ? toneHz : NULL, anyTone ? toneTq : NULL );
	HRP_RagdollDrive( rd->phys, rd->target, strength, ( hrTime - rd->euStart ) / 150.0f );
}


/*
=================
RD_ActiveDeath

He's dead (or just finished off): how long each part keeps fighting, by where the killing
shot went in (hitPart -1: no shot, a blast or a fall). A headshot drops him like a stone; the
heart, quickly; the gut and legs, slowly, folding over the wound. The legs always give first,
and the limb that was hit gives at once.
=================
*/
static void RD_ActiveDeath( hrBody_t *rd, int hitPart, const vec3_t hitPoint ) {
	int hitSeg = hitPart >= 0 && hitPart < rd->numParts ? rd->partSeg[hitPart] : -1;
	float ms, factor;
	int j, region = HR_RG_NONE;
	qboolean wasDowned = rd->downed;

	rd->downed = qfalse;
	rd->active = qfalse;
	rd->eu = 0;
	if ( rd->phys ) {
		HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
		if ( rd->gravK && rd->gravK != 1.0f ) {
			rd->gravK = 1.0f;     // (limp from here on: plain gravity; RD_EuFrame sets its own)
			HRP_RagdollGravity( rd->phys, 1.0f );
		}
	}
	if ( !rd->phys || !RD_ActiveOn() || HR_NumActive() >= hrOpt.maxActive ) {
		HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
		return;
	}
	switch ( hitSeg ) {
	case HR_S_HEAD:
		ms = 0;
		break;
	case HR_S_TORSO:
	case HR_S_CHEST: {
		// low on the trunk (the gut) or high (the chest)
		vec3_t up, d;
		float len, t;
		VectorSubtract( rd->p[HR_P_NECK], rd->p[HR_P_PELVIS], up );
		len = VectorNormalize( up );
		VectorSubtract( hitPoint, rd->p[HR_P_PELVIS], d );
		t = len > 1 ? DotProduct( d, up ) / len : 0.5f;
		ms = t < 0.45f ? 2600 : 900;
		region = t < 0.45f ? HR_RG_GUT : HR_RG_CHEST;
		break;
	}
	case -1:
		ms = rd->blasted ? 0 : 700;     // a blast throws him limp
		break;
	default:
		ms = RD_SegIsLeg( hitSeg ) ? 2000 : 1600;
		region = RD_SegIsLeg( hitSeg ) ? HR_RG_LEG : HR_RG_ARM;
		break;
	}
	if ( ms <= 0 ) {
		HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
		return;
	}
	if ( RD_EuOn() ) {
		RD_EuStart( rd, hitSeg, hitPart, hitPoint, region, wasDowned );
		return;
	}
	(void)j; (void)factor;
	HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );      // vr_activeEuphoria 0: limp (no death animations)
}

#define RD_GETUP_MS         3200        // from the floor back into his animation (within the server's VRX_GETUP_MS)
#define RD_GETUP_GATHER     0.1f        // gathering himself: arms and legs drawn in under him
#define RD_GETUP_STAND      0.84f       // by then standing (RD_GetUpFrame's stages before)
#define RD_RECOVER_MS       450         // came to on his feet: back into his animation

void HR_RotLerp( vec3_t a[3], vec3_t b[3], float f, vec3_t out[3] ) {
	vec3_t r[3], axis, turn[3];
	float c, sn, ang;

	RD_FrameRotation( b, a, r );
	c = ( r[0][0] + r[1][1] + r[2][2] - 1 ) * 0.5f;
	c = c > 1 ? 1 : c < -1 ? -1 : c;
	ang = acos( c );
	sn = sin( ang );
	if ( sn < 1e-3f ) {
		memcpy( out, ( ang < 1 || f < 0.5f ) ? a : b, sizeof( vec3_t ) * 3 );
		return;
	}
	VectorSet( axis, r[1][2] - r[2][1], r[2][0] - r[0][2], r[0][1] - r[1][0] );
	VectorScale( axis, 0.5f / sn, axis );
	RD_AxisAngle( axis, ang * f, turn );
	RD_MatMul( turn, a, out );
}

/*
=================
RD_GetUpBegin, RD_GetUpFrame

Getting up off the floor as physics, the way a man does, which depends on how he lies:

  on his front: he pushes his chest up on his hands, draws his hips back over a knee and
  brings the other foot up ahead, comes up onto that knee with a hand on it, and stands;
  on his back:  he sits up leaning back on his hands, rolls to his right onto the knee, the
  other foot planted ahead, and stands.

His muscles chase each pose in turn (CG_VRX_GetUpPose, then his animation), each crossfading
into the next so he never stops in a pose, while his pelvis is sprung along the way (it
turns first, then rises), so his weight comes up after his limbs are under him. He faces the
way he lay the whole time (the server's body is turned to match: "vrgetupyaw"). Only at the
end is he eased into his animation.
=================
*/
#define RD_GU_ANIM      HR_GU_ANIM

static const int rdGuProne[] = { HR_GU_PRESS, HR_GU_TUCK, HR_GU_KNEEL, RD_GU_ANIM };
static const float rdGuProneAt[] = { 0.28f, 0.45f, 0.64f, RD_GETUP_STAND };
static const int rdGuSupine[] = { HR_GU_SIT, HR_GU_KNEEL, RD_GU_ANIM };
static const float rdGuSupineAt[] = { 0.32f, 0.62f, RD_GETUP_STAND };

static void RD_GetUpBegin( hrBody_t *rd ) {
	vec3_t tf[3], up;
	float yaw;

	RD_TorsoFrame( rd->p, tf );     // up, left, back
	rd->getUpProne = tf[2][2] > 0;  // his back to the sky: on his front
	VectorSet( up, tf[0][0], tf[0][1], 0 );
	if ( VectorNormalize( up ) < 0.1f ) {
		VectorSet( up, tf[2][0], tf[2][1], 0 );     // (curled up: the way his back faces)
		if ( !rd->getUpProne ) {
			VectorNegate( up, up );
		}
		VectorNormalize( up );
	}
	yaw = RAD2DEG( atan2( up[1], up[0] ) );
	if ( rd->getUpProne ) {
		rd->getUpSitYaw = rd->getUpYaw = yaw;               // facing the way his head lies
	} else {
		rd->getUpSitYaw = AngleNormalize360( yaw + 180 );   // sat up, facing his feet
		rd->getUpYaw = AngleNormalize360( yaw + 90 );       // then rolled to his right onto the knee
	}
	{
		float args[4];
		vec3_t d;
		args[0] = rd->getUpYaw;
		VectorSet( d, cos( rd->getUpYaw * ( M_PI / 180.0f ) ), sin( rd->getUpYaw * ( M_PI / 180.0f ) ), 0 );
		HR_DirToHost( d, args + 1 );
		HR_Event( rd, HR_EV_GETUP_YAW, args );
	}
}

// a get-up stage's pose: each part's target (rd->target) and where it has the torso
static qboolean RD_GetUpPose( hrBody_t *rd, int stage, vec3_t tPos, vec3_t tAxis[3] ) {
	vec3_t f0[3], fwd, z = { 0, 0, 1 }, rot[3];
	float yaw, foot;
	hrTrace_t tr;
	int j;

	if ( hrHost.getUpPose ) {
		return hrHost.getUpPose( rd->user, stage, rd->target, tPos, tAxis[0] ) != 0;
	}
	// none from the host: the pose his body was made in, turned the way he gets up, his pelvis
	// as high off the floor as it was then
	RD_TorsoFrame( rd->p0, f0 );
	VectorSet( fwd, -f0[2][0], -f0[2][1], 0 );
	if ( VectorNormalize( fwd ) < 0.1f ) {
		VectorSet( fwd, 1, 0, 0 );
	}
	yaw = ( stage == HR_GU_SIT ? rd->getUpSitYaw : rd->getUpYaw ) * ( M_PI / 180.0f ) - atan2( fwd[1], fwd[0] );
	RD_AxisAngle( z, yaw, rot );
	for ( j = 0; j < rd->numParts; j++ ) {
		vec3_t out[3];
		RD_MatMul( rot, rd->part0Axis[j], out );
		memcpy( rd->target + j * 9, out[0], sizeof( float ) * 9 );
	}
	RD_MatMul( rot, rd->part0Axis[0], tAxis );
	foot = rd->p0[HR_P_PELVIS][2] - 30.0f;
	if ( rd->hasPoint[HR_P_LANKLE] && rd->hasPoint[HR_P_RANKLE] ) {
		foot = ( rd->p0[HR_P_LANKLE][2] < rd->p0[HR_P_RANKLE][2] ? rd->p0[HR_P_LANKLE][2] : rd->p0[HR_P_RANKLE][2] ) - 4.0f;
	}
	VectorCopy( rd->partPos[0], tPos );
	{
		vec3_t a, b;
		VectorSet( a, tPos[0], tPos[1], tPos[2] + 8 );
		VectorSet( b, tPos[0], tPos[1], tPos[2] - 64 );
		HR_TraceLine( rd, &tr, a, b );
		tPos[2] = ( tr.fraction < 1.0f && !tr.startsolid ? tr.endpos[2] : tPos[2] ) + rd->part0Pos[0][2] - foot;
	}
	return qtrue;
}

static void RD_GetUpFrame( hrBody_t *rd, int off ) {
	static float fromTarget[HRP_MAX_PARTS * 9];
	float strength[HRP_MAX_PARTS], a = ( hrTime - rd->getUpStart ) / (float)RD_GETUP_MS, scale, s, f, fj, omega, t0;
	const int *stages = rd->getUpProne ? rdGuProne : rdGuSupine;
	const float *at = rd->getUpProne ? rdGuProneAt : rdGuSupineAt;
	int numStages = rd->getUpProne ? 4 : 3;
	vec3_t tPos, tAxis[3], fPos, fAxis[3], pinPos, pinAxis[3];
	int j, st;

	scale = hrOpt.strength > 0 ? hrOpt.strength : 1;
	a = a > 1 ? 1 : a;

	// which stage: each from the end of the one before (the first from when he's gathered himself)
	for ( st = 0; st < numStages - 1 && a >= at[st]; st++ ) {
	}
	t0 = st ? at[st - 1] : RD_GETUP_GATHER;
	if ( rd->getUpPhase != st + 1 ) {
		rd->getUpPhase = st + 1;    // a new stage starts from wherever he got to
		VectorCopy( rd->partPos[0], rd->getUpPos );
		memcpy( rd->getUpAxis, rd->partAxis[0], sizeof( rd->getUpAxis ) );
	}
	f = ( a - t0 ) / ( at[st] - t0 );
	f = f > 1 ? 1 : f < 0 ? 0 : f;
	f = f * f * ( 3 - 2 * f );

	// the stage before's pose, to crossfade the joints from
	if ( st > 0 ) {
		if ( !RD_GetUpPose( rd, stages[st - 1], fPos, fAxis ) ) {
			st = 0;
		} else {
			memcpy( fromTarget, rd->target, sizeof( float ) * 9 * rd->numParts );
		}
	}
	if ( !RD_GetUpPose( rd, stages[st], tPos, tAxis ) ) {
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
		return;
	}
	// the limbs lead: they're into the new pose well before the body has come up to it
	fj = a < RD_GETUP_GATHER ? 1 : ( f * 1.7f > 1 ? 1 : f * 1.7f );
	if ( st > 0 && fj < 1 ) {
		for ( j = 0; j < rd->numParts; j++ ) {
			vec3_t A[3], B[3], O[3];
			memcpy( A, fromTarget + j * 9, sizeof( A ) );
			memcpy( B, rd->target + j * 9, sizeof( B ) );
			RD_RotLerp( A, B, fj, O );
			memcpy( rd->target + j * 9, O, sizeof( O ) );
		}
	}

	// the muscles: coming in quickly as he gathers himself, his whole effort while he rises
	s = a < RD_GETUP_GATHER ? 0.35f + 0.65f * a / RD_GETUP_GATHER : 1.0f;
	for ( j = 0; j < rd->numParts; j++ ) {
		int limb = RD_LimbOf( rd->partSeg[j] ), on = 1;
		if ( limb && ( off & ( 1 << limb ) ) ) {
			on = 0;
		}
		strength[j] = on * s * scale * ( limb == 0 ? 0.85f : 1.0f );
	}
	HRP_RagdollTone( rd->phys, NULL, NULL );
	HRP_RagdollDrive( rd->phys, rd->target, strength, 1.0f );

	// the pelvis: on the floor while he gathers himself, then sprung along the way
	if ( a < RD_GETUP_GATHER ) {
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		VectorCopy( rd->partPos[0], rd->getUpPos );
		memcpy( rd->getUpAxis, rd->partAxis[0], sizeof( rd->getUpAxis ) );
		return;
	}
	{
		// the turn leads, the rise follows (getting his limbs under him before his weight
		// comes up), and standing, the last push is the hardest
		float fr = f * 1.4f > 1 ? 1 : f * 1.4f, fh = stages[st] == RD_GU_ANIM ? f * sqrt( f ) : f;
		RD_RotLerp( rd->getUpAxis, tAxis, fr, pinAxis );
		pinPos[0] = rd->getUpPos[0] + ( tPos[0] - rd->getUpPos[0] ) * f;
		pinPos[1] = rd->getUpPos[1] + ( tPos[1] - rd->getUpPos[1] ) * f;
		pinPos[2] = rd->getUpPos[2] + ( tPos[2] - rd->getUpPos[2] ) * fh;
	}
	omega = ( stages[st] == RD_GU_ANIM ? 9.0f : 7.0f ) * ( scale > 1.5f ? 1.5f : scale );
	HRP_RagdollPin( rd->phys, pinPos, pinAxis[0], omega );
}

// where a shot went in, as RD_ActiveDeath sorts it
static int RD_HitRegion( hrBody_t *rd, int hitSeg, const vec3_t hitPoint ) {
	vec3_t up, d;
	float len;

	if ( hitSeg < 0 ) {
		return HR_RG_NONE;
	}
	if ( hitSeg == HR_S_TORSO || hitSeg == HR_S_CHEST || hitSeg == HR_S_HEAD ) {
		VectorSubtract( rd->p[HR_P_NECK], rd->p[HR_P_PELVIS], up );
		len = VectorNormalize( up );
		VectorSubtract( hitPoint, rd->p[HR_P_PELVIS], d );
		return hitSeg != HR_S_HEAD && len > 1 && DotProduct( d, up ) / len < 0.45f ? HR_RG_GUT : HR_RG_CHEST;
	}
	return RD_SegIsLeg( hitSeg ) ? HR_RG_LEG : HR_RG_ARM;
}

// knocked down: staggers, goes over, and lies there until the server stands him up
static void RD_KnockDown( hrBody_t *rd ) {
	int part = rd->lastHitTime && hrTime - rd->lastHitTime < 500 ? rd->lastHitPart : -1;
	int seg = part >= 0 && part < rd->numParts ? rd->partSeg[part] : -1;

	rd->downed = qtrue;
	rd->downedAt = hrTime;
	rd->stun = qfalse;
	rd->resetDrawn = qtrue;
	HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
	HRP_RagdollCatch( rd->phys, 0, 0, 0 );
	RD_EuStart( rd, seg, part, rd->lastHitPoint, RD_HitRegion( rd, seg, rd->lastHitPoint ), qfalse );
	if ( ( hrOpt.debug & 1 ) ) {
		HR_Print( "ragdoll %d: knocked down (%s)\n", rd->id, rd->eu == HR_EU_STAGGER ? "stagger" : "fall" );
	}
}

// shot and stunned: staggering under the bullets, clutching the wound, until he comes to
static void RD_EuStun( hrBody_t *rd ) {
	int part = rd->lastHitTime && hrTime - rd->lastHitTime < 500 ? rd->lastHitPart : -1;
	int seg = part >= 0 && part < rd->numParts ? rd->partSeg[part] : -1;

	rd->stun = qtrue;
	VectorCopy( rd->partPos[0], rd->stunPos );
	memcpy( rd->stunAxis, rd->partAxis[0], sizeof( rd->stunAxis ) );
	RD_EuStart( rd, seg, part, rd->lastHitPoint, RD_HitRegion( rd, seg, rd->lastHitPoint ), qfalse );
	rd->eu = HR_EU_STAGGER;
	rd->euDur = 1e6f;
	if ( rd->lastHitTime ) {
		VectorSet( rd->euPush, rd->lastHitDir[0], rd->lastHitDir[1], 0 );
		if ( VectorNormalize( rd->euPush ) < 0.1f ) {
			VectorSet( rd->euPush, 1, 0, 0 );
		}
	}
	if ( ( hrOpt.debug & 1 ) ) {
		HR_Print( "ragdoll %d: stunned\n", rd->id );
	}
}

// hit again while stunned: shoved the new way, the hand goes to the new wound
static void RD_EuRehit( hrBody_t *rd ) {
	int part = rd->lastHitPart, seg, k;
	vec3_t rel;

	if ( rd->eu != HR_EU_STAGGER || part < 0 || part >= rd->numParts || hrTime - rd->lastHitTime > 50 ) {
		return;
	}
	seg = rd->partSeg[part];
	rd->euRegion = RD_HitRegion( rd, seg, rd->lastHitPoint );
	rd->euHurtLimb = RD_LimbOf( seg );
	rd->euWoundPart = part;
	VectorSubtract( rd->lastHitPoint, rd->partPos[part], rel );
	for ( k = 0; k < 3; k++ ) {
		rd->euWoundLocal[k] = DotProduct( rel, rd->partAxis[part][k] );
	}
	VectorSet( rel, rd->lastHitDir[0], rd->lastHitDir[1], 0 );
	if ( VectorNormalize( rel ) > 0.1f ) {
		VectorCopy( rel, rd->euPush );
	}
	rd->euPhaseStart = hrTime;     // (the lean: knocked back again)
}

/*
=================
RD_Steady, RD_AtRest

Whether a stagger has played out: on his feet, not mid-step, his weight over his feet and
hardly moving, upright, for a moment (RD_Steady); given up on after 2.5 s. Knocked down,
whether he's done falling and rolling and lies still (RD_AtRest); given up on after 2 s.
=================
*/
static qboolean RD_Steady( hrBody_t *rd ) {
	vec3_t tf[3];
	float speed;

	if ( !rd->stunOverAt ) {
		rd->stunOverAt = hrTime;
		rd->steadySince = 0;
	}
	if ( hrTime - rd->stunOverAt > 1500 || rd->eu != HR_EU_STAGGER || !rd->phys ) {
		return qtrue;
	}
	RD_TorsoFrame( rd->p, tf );
	speed = sqrt( rd->balComVel[0] * rd->balComVel[0] + rd->balComVel[1] * rd->balComVel[1] );
	if ( rd->balSwing >= 0 || rd->balCpOut > 0 || speed > 30.0f || tf[0][2] < 0.8f ) {
		rd->steadySince = 0;
		return qfalse;
	}
	if ( !rd->steadySince ) {
		rd->steadySince = hrTime;
	}
	return hrTime - rd->steadySince >= 300;
}

static qboolean RD_AtRest( hrBody_t *rd ) {
	if ( !rd->upAt ) {
		rd->upAt = hrTime;
		rd->restSince = hrTime;
		VectorCopy( rd->p[HR_P_PELVIS], rd->restPos );
	}
	if ( hrTime - rd->upAt > 2000 || !rd->phys ) {
		return qtrue;
	}
	if ( rd->eu != HR_EU_LYING || Distance( rd->p[HR_P_PELVIS], rd->restPos ) > 6.0f ) {
		VectorCopy( rd->p[HR_P_PELVIS], rd->restPos );
		rd->restSince = hrTime;
		return qfalse;
	}
	return hrTime - rd->restSince >= 400;
}
/*
==============================================================================

The public side

==============================================================================
*/

int HR_PointSegment( int point ) {
	return point >= 0 && point < HR_P_COUNT ? hrPointSeg[point] : -1;
}

void HR_BodySetSkeleton( hrBody_t *rd, void *user, int id, const float *points, const unsigned char *hasPoint,
						 const unsigned char *segValid, const float *vel, const float *push, const float *origin ) {
	vec3_t o;
	int k, s;

	if ( rd->registered || rd->phys ) {
		HR_BodyFree( rd );
	}
	memset( rd, 0, sizeof( *rd ) );
	rd->user = user;
	rd->id = id;
	for ( k = 0; k < HR_P_COUNT; k++ ) {
		rd->hasPoint[k] = hasPoint[k] != 0;
		if ( rd->hasPoint[k] ) {
			HR_PointToLib( points + k * 3, rd->p0[k] );
			VectorCopy( rd->p0[k], rd->p[k] );
		}
	}
	for ( s = 0; s < HR_S_COUNT; s++ ) {
		if ( segValid ) {
			rd->segValid[s] = segValid[s] != 0;
		} else {
			rd->segValid[s] = s == HR_S_TORSO ||
							  ( rd->hasPoint[hrSegments[s].from] && rd->hasPoint[hrSegments[s].to] && rd->segValid[hrSegments[s].parent] );
		}
	}
	if ( vel ) {
		HR_PointToLib( vel, rd->startVel );
	}
	if ( push ) {
		HR_PointToLib( push, rd->killPush );
	}
	if ( origin ) {
		HR_PointToLib( origin, o );
	} else {
		VectorCopy( rd->p0[HR_P_PELVIS], o );
	}
	VectorSubtract( rd->p0[HR_P_PELVIS], o, rd->anchorOffset );
	VectorCopy( o, rd->lastOrigin );
	rd->startTime = hrTime;
	rd->settleBy = hrTime + RD_SETTLE_MS;
}

void HR_BodyFree( hrBody_t *rd ) {
	if ( rd->phys ) {
		HRP_DestroyRagdoll( rd->phys );
		rd->phys = 0;
	}
	HR_Unregister( rd );
}

void HR_BodyAnchor( hrBody_t *rd, const float *origin ) {
	vec3_t o;

	HR_PointToLib( origin, o );
	RD_PhysAnchor( rd, o );
}

/*
=================
HR_BodyShot

a shot moves the whole body a little and the part it hit more (pushing only the part hit at
full speed set bodies spinning like helicopters); the part hit flops a moment
=================
*/
#define RD_HIT_RADIUS       24.0f       // a bullet moves the part this close to where it hit
#define RD_HIT_BODY         0.35f       // share of the push the whole body gets

int HR_BodyShot( hrBody_t *rd, const float *pointHost, const float *velHost ) {
	vec3_t v, point, vel;
	float best = 1e9f, d;
	int j, part = -1;

	if ( !rd->phys ) {
		return -1;
	}
	HR_PointToLib( pointHost, point );
	HR_PointToLib( velHost, vel );
	VectorScale( vel, RD_HIT_BODY, v );
	HRP_RagdollAddVelocity( rd->phys, -1, v );
	for ( j = 0; j < rd->numParts; j++ ) {
		// (a shot below his neck, an arm or the shoulder, never counts as the head: a
		// head flopping from an arm hit looks like his neck's broken)
		if ( rd->partSeg[j] == HR_S_HEAD && rd->hasPoint[HR_P_NECK] && point[2] < rd->p[HR_P_NECK][2] ) {
			continue;
		}
		if ( ( d = Distance( rd->partPos[j], point ) ) < best ) {
			best = d;
			part = j;
		}
	}
	if ( part >= 0 && best < RD_HIT_RADIUS * 1.5f ) {
		VectorScale( vel, 1.0f - RD_HIT_BODY, v );
		HRP_RagdollAddVelocityAt( rd->phys, part, v, point );
		rd->partHitTime[part] = hrTime;
		rd->lastHitPart = part;
		rd->lastHitTime = hrTime;
		VectorCopy( point, rd->lastHitPoint );
		VectorNormalize2( vel, rd->lastHitDir );
		return part;
	}
	return -1;
}

void HR_BodyBlast( hrBody_t *rd, const float *originHost, float radius, float strength ) {
	vec3_t dir, origin;
	float dist, w;
	int k;

	if ( !rd->phys ) {
		return;
	}
	HR_PointToLib( originHost, origin );
	radius = HR_LenToLib( radius );
	strength = HR_LenToLib( strength );
	if ( Distance( rd->p[HR_P_PELVIS], origin ) > radius ) {
		return;
	}
	for ( k = 0; k < rd->numParts; k++ ) {
		vec3_t v;
		VectorSubtract( rd->partPos[k], origin, dir );
		dist = VectorNormalize( dir );
		// mostly by how far the body is, a little by how far the part is: thrown, not spun
		w = 1.0f - ( 0.7f * Distance( rd->p[HR_P_PELVIS], origin ) + 0.3f * dist ) / radius;
		if ( w <= 0 ) {
			continue;
		}
		dir[2] += 0.6f;         // thrown up and away
		VectorNormalize( dir );
		VectorScale( dir, w * strength, v );
		HRP_RagdollAddVelocity( rd->phys, k, v );
	}
}

void HR_Die( hrBody_t *rd, int hitPart, const float *hitPoint ) {
	vec3_t p;

	if ( hitPoint ) {
		HR_PointToLib( hitPoint, p );
	} else if ( hitPart >= 0 && hitPart < rd->numParts ) {
		VectorCopy( rd->partPos[hitPart], p );      // (the part's middle: the belly box a gut shot, the chest box a chest shot)
	} else {
		VectorCopy( rd->p[HR_P_PELVIS], p );
	}
	RD_ActiveDeath( rd, hitPart, p );
}

void HR_Stun( hrBody_t *rd ) {
	rd->living = qtrue;      // (alive in physics until HR_Living gives him back, or HR_Killed)
	RD_EuStun( rd );
}

void HR_KnockDown( hrBody_t *rd ) {
	rd->living = qtrue;      // (alive in physics until HR_Living gives him back, or HR_Killed)
	RD_KnockDown( rd );
}

void HR_Rehit( hrBody_t *rd ) {
	RD_EuRehit( rd );
}

void HR_Aim( hrBody_t *rd, const float *point ) {
	if ( !point ) {
		rd->aimOn = qfalse;
		return;
	}
	HR_PointToLib( point, rd->aimPoint );
	rd->aimOn = qtrue;
}

void HR_Recoil( hrBody_t *rd ) {
	rd->recoilTime = hrTime;
}

void HR_GetUpBegin( hrBody_t *rd ) {
	RD_GetUpBegin( rd );
}

void HR_GetUpFrame( hrBody_t *rd, int limbsOff ) {
	RD_GetUpFrame( rd, limbsOff );
}

int HR_Steady( hrBody_t *rd ) {
	return RD_Steady( rd );
}

int HR_AtRest( hrBody_t *rd ) {
	return RD_AtRest( rd );
}

/*
=================
HR_ActiveFrame

the muscles this frame: the reaction's pose, or limp when they're off
=================
*/
static void RD_ActiveFrame( hrBody_t *rd, const vec3_t origin, qboolean grabbed, int off, int headOff ) {
	float scale = hrOpt.strength;

	if ( !RD_ActiveOn() || scale <= 0 || ( headOff && !rd->downed ) ) {
		rd->active = rd->downed = qfalse;
		rd->eu = 0;
		HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
		HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );         // (nor the net under a stunned man)
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
		if ( rd->gravK && rd->gravK != 1.0f ) {
			rd->gravK = 1.0f;
			HRP_RagdollGravity( rd->phys, 1.0f );
		}
		return;
	}
	if ( rd->eu ) {
		RD_EuFrame( rd, origin, grabbed, scale, off );
		return;
	}
	rd->active = qfalse;
	HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
}

void HR_ActiveFrame( hrBody_t *rd, const float *origin, int grabbed, int limbsOff, int headOff ) {
	vec3_t o;

	HR_PointToLib( origin, o );
	RD_ActiveFrame( rd, o, grabbed, limbsOff, headOff );
}

static void HR_GetUpAnimAbort( hrBody_t *rd ) {
	if ( hrHost.getUpAnimAbort ) {
		hrHost.getUpAnimAbort( rd->user );
	}
	rd->getUpAnimated = qfalse;
}

/*
=================
HR_Living

Alive in physics: stunned, staggering under the bullets, until he's steady on his feet and
comes to (then eased back into his animation); knocked down (the game's word, or shoved over
here: HR_EV_FELL), lying there until the game stands him up and he's come to rest, then
getting up. hit: the game has him stunned now; downed: knocked down now.
=================
*/
int HR_Living( hrBody_t *rd, int hit, int downed ) {
	if ( hit ) {
		rd->stunOverAt = 0;     // (shot again: the stun goes on)
	}
	if ( downed ) {
		rd->upAt = 0;
	}
	if ( rd->stun && !rd->recoverStart && !downed && rd->eu == 1 && hrTime - rd->lastPosSent > 150 ) {
		// staggering on his feet: the game's body (what bullets hit, what's in the way, where
		// he'll be standing when he comes to) keeps up with where the stagger has carried him
		rd->lastPosSent = hrTime;
		HR_EventPos( rd );
	}
	if ( rd->stun && !hit && !downed && !rd->recoverStart && !RD_Steady( rd ) ) {
		// the game's done with the stun, but he's still reeling: he staggers on until he's
		// steady on his feet (or goes down), the game's body held meanwhile
		if ( hrTime - rd->lastStaggerSent > 250 ) {
			rd->lastStaggerSent = hrTime;
			HR_Event( rd, HR_EV_HOLD, NULL );
		}
	} else if ( rd->stun && !hit && !downed && !rd->recoverStart ) {
		// came to on his feet: the game's body where he stumbled to, then back into his animation
		rd->stunOverAt = 0;
		rd->recoverStart = hrTime;
		rd->stun = qfalse;
		rd->eu = 0;
		rd->active = qfalse;
		HR_EventPos( rd );
		HRP_RagdollDrive( rd->phys, NULL, NULL, 0 );
		HRP_RagdollPin( rd->phys, NULL, NULL, 0 );
		HRP_RagdollCatch( rd->phys, 0, 0, 0 );
		HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
		HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
		HRP_RagdollSleep( rd->phys );
		if ( ( hrOpt.debug & 1 ) ) {
			HR_Print( "ragdoll %d: comes to\n", rd->id );
		}
	} else if ( rd->stun && downed ) {
		RD_KnockDown( rd );         // the game put him down (a leg, a heavy round)
	} else if ( rd->getUpStart && downed ) {
		// put down again on his way up: from the pose he's in
		HR_GetUpAnimAbort( rd );
		rd->getUpStart = 0;
		rd->recoverStart = 0;
		HRP_RagdollWake( rd->phys );
		RD_KnockDown( rd );
		rd->euStart -= 1000;
	} else if ( rd->downed && !downed && !rd->getUpStart && hrTime - rd->downedAt > 1000 && !RD_AtRest( rd ) ) {
		// the game stood him up, but he's still falling or rolling: he gets up once he's
		// come to rest (the game's body held meanwhile)
	} else if ( rd->downed && !downed && !rd->getUpStart && hrTime - rd->downedAt > 1000 ) {
		// the game stood him up: from where he lies back into his animation
		rd->upAt = 0;
		rd->getUpStart = hrTime;
		rd->getUpPhase = 0;
		HRP_RagdollLegPush( rd->phys, -1, -1, 0, 0 );
		rd->gravK = 1.0f;
		HRP_RagdollGravity( rd->phys, 1.0f );
		rd->downed = qfalse;
		rd->eu = 0;
		rd->active = qfalse;
		HRP_RagdollSupport( rd->phys, 0, 0, 0, NULL );
		VectorCopy( rd->partPos[0], rd->getUpPos );
		memcpy( rd->getUpAxis, rd->partAxis[0], sizeof( rd->getUpAxis ) );
		rd->getUpAnimated = hrHost.getUpAnimBegin ? hrHost.getUpAnimBegin( rd->user ) != 0 : qfalse;
		if ( !rd->getUpAnimated ) {
			HRP_RagdollWake( rd->phys );       // no animation for him: he gets up as physics (RD_GetUpFrame)
			RD_GetUpBegin( rd );
		}
		if ( ( hrOpt.debug & 1 ) ) {
			HR_Print( "ragdoll %d: getting up (%s%s)\n", rd->id, rd->getUpProne ? "front" : "back",
					  rd->getUpAnimated ? ", animated" : "" );
		}
	} else if ( !rd->downed && !rd->getUpStart && !rd->stun && ( downed || hit ) ) {
		// hit again as he comes to: back into it from the pose he's in
		rd->recoverStart = 0;
		rd->resetDrawn = qtrue;
		HRP_RagdollWake( rd->phys );
		if ( downed ) {
			RD_KnockDown( rd );
		} else {
			RD_EuStun( rd );
		}
		rd->euStart -= 1000;
	}
	return ( rd->getUpStart && hrTime - rd->getUpStart >= RD_GETUP_MS ) ||
		   ( rd->recoverStart && hrTime - rd->recoverStart >= RD_RECOVER_MS );
}

/*
=================
HR_Killed

killed while knocked about, down or getting up: the same body, now dying (no pop)
=================
*/
void HR_Killed( hrBody_t *rd ) {
	qboolean falling, lying;

	HR_GetUpAnimAbort( rd );    // (getting up as the host's animation: the physics from where he's drawn)
	falling = rd->downed && ( rd->eu == HR_EU_STAGGER || rd->eu == HR_EU_FALL );
	lying = rd->downed && rd->eu == HR_EU_LYING;

	rd->living = qfalse;
	rd->getUpStart = 0;
	rd->resetDrawn = qtrue;
	HRP_RagdollPin( rd->phys, NULL, NULL, 0 );     // (no longer held up: dead)
	if ( falling ) {
		rd->downed = qfalse;    // already going down: carries on, now for good
	} else {
		rd->downed = lying;
		rd->stun = qfalse;
		rd->recoverStart = 0;
		HRP_RagdollWake( rd->phys );
		RD_ActiveDeath( rd, rd->lastHitTime && hrTime - rd->lastHitTime < 500 ? rd->lastHitPart : -1, rd->lastHitPoint );
		rd->euStart -= 1000;        // (from the pose he's in now, not the one the body was made in)
	}
}

/*
=================
HR_Settle

dead and still long enough, lying on something: it stops for good (no helicopters, no
creeping about); held, fighting or alive it never does
=================
*/
void HR_Settle( hrBody_t *rd, int grabbed, const float *originHost ) {
	vec3_t origin;

	HR_PointToLib( originHost, origin );
	if ( grabbed || rd->downed || rd->eu || rd->living ) {
		rd->asleep = qfalse;
		rd->settled = qfalse;
		rd->settleBy = hrTime + 2500;
	} else if ( !rd->settled && hrTime >= rd->settleBy ) {
		hrTrace_t tr;
		vec3_t down;
		VectorCopy( rd->p[HR_P_PELVIS], down );
		down[2] -= 16;
		HR_TraceLine( rd, &tr, rd->p[HR_P_PELVIS], down );
		// (or lying long enough on whatever holds it up, a step's edge or a rail: kept awake, it
		// cost a frame's physics for ever)
		if ( tr.fraction < 1.0f || hrTime - rd->startTime > 10000 ) {
			rd->settled = qtrue;
			rd->asleep = qtrue;
			if ( rd->phys ) {
				HRP_RagdollSleep( rd->phys );
			}
		} else {
			rd->settleBy = hrTime + 500;
		}
	} else if ( rd->settled ) {
		rd->asleep = qtrue;
		VectorCopy( origin, rd->lastOrigin );
	} else if ( rd->asleep ) {
		// woken when the game moves the body (thrown, pushed by a mover...)
		if ( Distance( origin, rd->lastOrigin ) > 0.5f ) {
			rd->asleep = qfalse;
			rd->stillSteps = 0;
		}
	}
}

/*
=================
HR_Frame

once a frame: the hand holding him or the tether to the host's body for him, the muscles,
and the pose read back (1) for the host to skin his bones from
=================
*/
int HR_Frame( hrBody_t *rd, const float *originHost, int grabbed, const float *grabPointHost, int limbsOff, int headOff ) {
	vec3_t origin, grabPoint;

	if ( !rd->phys ) {
		return qfalse;
	}
	HR_PointToLib( originHost, origin );
	if ( grabbed ) {
		HR_PointToLib( grabPointHost, grabPoint );
		HRP_RagdollGrabMove( rd->phys, grabPoint, RD_GRAB_SPEED );
	} else if ( !rd->settled && !rd->getUpStart && !rd->recoverStart && rd->eu != HR_EU_STAGGER && rd->eu != HR_EU_FALL ) {
		RD_PhysAnchor( rd, origin );    // (not while he staggers about: re-tethered where he lands)
	}
	if ( rd->getUpStart && rd->getUpAnimated ) {
		// (the host's animation)
	} else if ( rd->getUpStart ) {
		RD_GetUpFrame( rd, limbsOff );
	} else if ( rd->active ) {
		RD_ActiveFrame( rd, origin, grabbed, limbsOff, headOff );
	}
	if ( !rd->recoverStart && !( rd->getUpStart && rd->getUpAnimated ) &&
		 ( rd->getUpStart || !rd->settled || HRP_RagdollAwake( rd->phys ) ) ) {
		HR_BodyReadPose( rd );
		return qtrue;
	}
	return qfalse;
}

void HR_Grab( hrBody_t *rd, int point ) {
	if ( rd->phys && point >= 0 && point < HR_P_COUNT && rd->hasPoint[point] ) {
		HRP_RagdollGrab( rd->phys, rd->segPart[hrPointSeg[point]], rd->p[point] );
	}
}

void HR_Release( hrBody_t *rd, const float *originHost ) {
	vec3_t origin;

	HR_PointToLib( originHost, origin );
	VectorSubtract( rd->p[HR_P_PELVIS], origin, rd->anchorOffset );
	if ( rd->phys ) {
		HRP_RagdollRelease( rd->phys );
	}
}

void HR_GetPoint( const hrBody_t *rd, int point, float *out ) {
	if ( point < 0 || point >= HR_P_COUNT ) {
		VectorClear( out );
		return;
	}
	HR_PointToHost( rd->p[point], out );
}

void HR_GetPart( const hrBody_t *rd, int part, float *pos, float *axis ) {
	if ( pos ) {
		HR_PointToHost( rd->partPos[part], pos );
	}
	if ( axis ) {
		HR_AxisToHost( rd->partAxis[part][0], axis );
	}
}

void HR_GetPartRotation( const hrBody_t *rd, int part, float *rot ) {
	HR_AxisToHost( rd->partRot[part][0], rot );
}

void HR_PartTargetFromBone( const hrBody_t *rd, int part, const float *boneAxis, const float *boneAxis0, float *out ) {
	vec3_t m[3], m0[3], delta[3], r[3];

	HR_AxisToLib( boneAxis, m[0] );
	HR_AxisToLib( boneAxis0, m0[0] );
	RD_FrameRotation( m, m0, delta );
	RD_MatMul( delta, (vec3_t *)rd->part0Axis[part], r );
	memcpy( out, r, sizeof( r ) );
}

void HR_TorsoTargetFromBone( const hrBody_t *rd, const float *boneAxis, const float *bonePos,
							 const float *boneAxis0, const float *bonePos0, float *torsoPos, float *torsoAxis ) {
	vec3_t m[3], m0[3], delta[3], r[3], rel, bp, bp0;

	HR_AxisToLib( boneAxis, m[0] );
	HR_AxisToLib( boneAxis0, m0[0] );
	HR_PointToLib( bonePos, bp );
	HR_PointToLib( bonePos0, bp0 );
	RD_FrameRotation( m, m0, delta );
	RD_MatMul( delta, (vec3_t *)rd->part0Axis[0], r );
	memcpy( torsoAxis, r, sizeof( r ) );
	VectorSubtract( rd->part0Pos[0], bp0, rel );
	RD_MatVec( delta, rel, rel );
	VectorAdd( bp, rel, torsoPos );
}
