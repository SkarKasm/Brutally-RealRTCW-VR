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

// cg_vrbody.c -- the headset player's own body (vr_fullBody, on by default).
//
// The player's character is drawn below the headset: it stands where the player stands,
// turns with the head (with some slack, like a neck), keeps the game's leg animations,
// and its arms reach the controllers with two-bone IK. The head is left out (the eyes are
// inside it), and so are the body's own hands: the tracked VR hands are drawn at the
// wrists instead, without their sleeves, since the body's arms now lead up to them.
//
// The animated skeleton comes from the renderer (trap_R_GetSkeleton), the arm bones are
// turned, and the result is drawn through refEntity.ragdollBones like a ragdoll.

#include "cg_local.h"

#ifndef BONEFLAG_TAG
#define BONEFLAG_TAG        1       // qfiles.h: this bone is actually a tag
#endif

#define VB_MAX_BONES        128
#define VB_NECK_SLACK       45.0f   // degrees the head turns before the body follows
#define VB_TURN_SPEED       90.0f   // degrees per second the body catches up with the head
#define VB_TURN_SPEED_MOVE  360.0f  // ... while walking
#define VB_EYES_AHEAD       9.0f    // the eyes are this far in front of the neck
#define VB_EYES_ABOVE       7.0f    // ... and above it
#define VB_PALM_TO_WRIST    3.5f    // tracked palm (grip pose) to the wrist joint

enum { SIDE_L, SIDE_R };

static vmCvar_t vr_fullBody;

static struct {
	qhandle_t   model;              // skeleton the bone indices are for
	int         numBones;
	int         parents[VB_MAX_BONES];
	int         flags[VB_MAX_BONES];
	char        names[VB_MAX_BONES * 32];
	float       pose[VB_MAX_BONES * 12];
	int         upper[2], fore[2], hand[2], finger[2];     // -1 when missing
	int         neck;
	qboolean    valid;

	float       bodyYaw;
	int         lastTime;
	int         drawnTime;          // cg.time the body was last drawn

	vec3_t      rot[VB_MAX_BONES][3];   // per bone rotation (columns)
	vec3_t      pos[VB_MAX_BONES];
	float       out[VB_MAX_BONES * 12];
} vb;

void CG_VR_BodyInit( void ) {
	memset( &vb, 0, sizeof( vb ) );
	trap_Cvar_Register( &vr_fullBody, "vr_fullBody", "1", CVAR_ARCHIVE );
}

/*
=================
CG_VR_FullBody

The local player in VR, alive and in control.
=================
*/
qboolean CG_VR_FullBody( centity_t *cent ) {
	trap_Cvar_Update( &vr_fullBody );
	return cgvr.active && vr_fullBody.integer && cent->currentState.number == cg.snap->ps.clientNum &&
		   !cg.renderingThirdPerson && !cg.cameraMode && cg.predictedPlayerState.stats[STAT_HEALTH] > 0 &&
		   !( cent->currentState.eFlags & EF_DEAD );
}

// the full body was drawn this frame (the VR hands then leave out their sleeves)
qboolean CG_VR_BodyDrawn( void ) {
	return vb.drawnTime == cg.time;
}

/*
=================
CG_VR_BodyAngles

The body faces where the head looks, a neck's worth of turning later.
=================
*/
void CG_VR_BodyAngles( centity_t *cent, vec3_t legs[3], vec3_t torso[3] ) {
	float head = cg.refdefViewAngles[YAW], diff, step, speed;
	vec3_t angles;

	if ( vb.lastTime <= 0 || cg.time < vb.lastTime || cg.time - vb.lastTime > 500 ) {
		vb.bodyYaw = head;      // first frame, or after a pause
	} else if ( cg.time > vb.lastTime ) {
		diff = AngleNormalize180( head - vb.bodyYaw );
		if ( diff > VB_NECK_SLACK ) {
			vb.bodyYaw += diff - VB_NECK_SLACK;
		} else if ( diff < -VB_NECK_SLACK ) {
			vb.bodyYaw += diff + VB_NECK_SLACK;
		}
		diff = AngleNormalize180( head - vb.bodyYaw );
		speed = VectorLength( cg.predictedPlayerState.velocity ) > 20.0f ? VB_TURN_SPEED_MOVE : VB_TURN_SPEED;
		step = speed * ( cg.time - vb.lastTime ) * 0.001f;
		vb.bodyYaw += diff > step ? step : ( diff < -step ? -step : diff );
		vb.bodyYaw = AngleNormalize360( vb.bodyYaw );
	}
	vb.lastTime = cg.time;

	VectorSet( angles, 0, vb.bodyYaw, 0 );
	AnglesToAxis( angles, legs );
	AxisClear( torso );     // no twist between the torso and the legs
}

/*
==============================================================================

Rotation helpers, matrices are column bases (m[k] is column k)

==============================================================================
*/

static void VB_MatMul( vec3_t a[3], vec3_t b[3], vec3_t out[3] ) {
	vec3_t r[3];
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			r[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

static void VB_MatVec( vec3_t m[3], const vec3_t v, vec3_t out ) {
	vec3_t r;
	int j;

	for ( j = 0; j < 3; j++ ) {
		r[j] = m[0][j] * v[0] + m[1][j] * v[1] + m[2][j] * v[2];
	}
	VectorCopy( r, out );
}

// smallest rotation turning unit vector u onto v
static void VB_Align( const vec3_t u, const vec3_t v, vec3_t out[3] ) {
	vec3_t axis;
	float c = DotProduct( u, v ), s, t, x, y, z;

	CrossProduct( u, v, axis );
	s = VectorNormalize( axis );
	memset( out, 0, sizeof( vec3_t ) * 3 );
	if ( s < 1e-5f ) {
		out[0][0] = out[1][1] = out[2][2] = 1;
		return;     // already lined up (opposite never happens for an arm)
	}
	t = 1 - c;
	x = axis[0]; y = axis[1]; z = axis[2];
	out[0][0] = t * x * x + c;     out[0][1] = t * x * y + s * z; out[0][2] = t * x * z - s * y;
	out[1][0] = t * x * y - s * z; out[1][1] = t * y * y + c;     out[1][2] = t * y * z + s * x;
	out[2][0] = t * x * z + s * y; out[2][1] = t * y * z - s * x; out[2][2] = t * z * z + c;
}

static qboolean VB_InSubtree( int bone, int root ) {
	for ( ; bone >= 0; bone = vb.parents[bone] ) {
		if ( bone == root ) {
			return qtrue;
		}
	}
	return qfalse;
}

// turn a bone and everything below it about a point
static void VB_Rotate( int root, vec3_t r[3], const vec3_t pivot ) {
	vec3_t rel;
	int i;

	for ( i = 0; i < vb.numBones; i++ ) {
		if ( !VB_InSubtree( i, root ) ) {
			continue;
		}
		VectorSubtract( vb.pos[i], pivot, rel );
		VB_MatVec( r, rel, rel );
		VectorAdd( pivot, rel, vb.pos[i] );
		VB_MatMul( r, vb.rot[i], vb.rot[i] );
	}
}

/*
==============================================================================

Skeleton

==============================================================================
*/

static int VB_FindBone( const char *a, const char *b ) {
	int i, pass;

	for ( pass = 0; pass < 2; pass++ ) {
		const char *want = pass ? b : a;
		if ( !want ) {
			continue;
		}
		for ( i = 0; i < vb.numBones; i++ ) {
			const char *name = vb.names + i * 32, *s;
			if ( !Q_stricmpn( name, "Bip0", 4 ) && ( s = strchr( name, ' ' ) ) ) {
				name = s + 1;
			}
			if ( !Q_stricmp( name, want ) ) {
				return i;
			}
		}
	}
	return -1;
}

static qboolean VB_Pose( refEntity_t *body ) {
	int n, i, k, s;

	n = trap_R_GetSkeleton( body, VB_MAX_BONES, vb.parents, vb.flags, vb.names, vb.pose );
	if ( n <= 0 ) {
		return qfalse;
	}
	if ( body->hModel != vb.model || n != vb.numBones ) {
		static const char *sides[2] = { "L", "R" };
		vb.model = body->hModel;
		vb.numBones = n;
		vb.valid = qtrue;
		vb.neck = VB_FindBone( "Neck", NULL );
		for ( s = 0; s < 2; s++ ) {
			vb.upper[s] = VB_FindBone( va( "%s UpperArm", sides[s] ), NULL );
			vb.fore[s] = VB_FindBone( va( "%s Forearm", sides[s] ), NULL );
			vb.hand[s] = VB_FindBone( va( "%s Hand", sides[s] ), NULL );
			vb.finger[s] = VB_FindBone( va( "%s Finger2", sides[s] ), va( "%s Finger1", sides[s] ) );
			if ( vb.upper[s] < 0 || vb.fore[s] < 0 || vb.hand[s] < 0 ) {
				vb.valid = qfalse;
			}
		}
	}
	if ( !vb.valid ) {
		return qfalse;
	}

	// The renderer skins with the stored matrix (out = M * offset) but reads a tag's
	// rows as its axes: keep the rotation each one really means, as columns.
	for ( i = 0; i < n; i++ ) {
		const float *bm = vb.pose + i * 12;
		for ( k = 0; k < 3; k++ ) {
			if ( vb.flags[i] & BONEFLAG_TAG ) {
				vb.rot[i][k][0] = bm[k * 3 + 0];
				vb.rot[i][k][1] = bm[k * 3 + 1];
				vb.rot[i][k][2] = bm[k * 3 + 2];
			} else {
				vb.rot[i][k][0] = bm[0 * 3 + k];
				vb.rot[i][k][1] = bm[1 * 3 + k];
				vb.rot[i][k][2] = bm[2 * 3 + k];
			}
		}
		VectorCopy( bm + 9, vb.pos[i] );
	}
	return qtrue;
}

// world point / direction -> the body's model space
static void VB_ToModel( refEntity_t *body, const vec3_t world, vec3_t out ) {
	vec3_t d;
	VectorSubtract( world, body->origin, d );
	out[0] = DotProduct( d, body->axis[0] );
	out[1] = DotProduct( d, body->axis[1] );
	out[2] = DotProduct( d, body->axis[2] );
}

static void VB_DirToModel( refEntity_t *body, const vec3_t world, vec3_t out ) {
	out[0] = DotProduct( world, body->axis[0] );
	out[1] = DotProduct( world, body->axis[1] );
	out[2] = DotProduct( world, body->axis[2] );
}

/*
=================
VB_ReachArm

Two-bone IK: shoulder -> elbow -> wrist onto the wrist target, the elbow falling
down, out and back like a relaxed arm; then the hand turned to point where the
controller points.
=================
*/
static void VB_ReachArm( int side, const vec3_t wrist, const vec3_t handDir ) {
	int ua = vb.upper[side], fa = vb.fore[side], hd = vb.hand[side], fg = vb.finger[side];
	vec3_t s, e, w, toT, n, pole, p, elbow, from, to, r[3];
	float a, b, d, cosA, sinA;

	VectorCopy( vb.pos[ua], s );
	VectorCopy( vb.pos[fa], e );
	VectorCopy( vb.pos[hd], w );
	a = Distance( s, e );
	b = Distance( e, w );
	if ( a < 0.1f || b < 0.1f ) {
		return;
	}

	VectorSubtract( wrist, s, toT );
	d = VectorNormalize( toT );
	VectorCopy( toT, n );
	if ( d > ( a + b ) * 0.999f ) {
		d = ( a + b ) * 0.999f;     // out of reach: the arm points at it
	} else if ( d < fabs( a - b ) + 0.5f ) {
		d = fabs( a - b ) + 0.5f;
	}
	cosA = ( a * a + d * d - b * b ) / ( 2 * a * d );
	cosA = cosA > 1 ? 1 : ( cosA < -1 ? -1 : cosA );
	sinA = sqrt( 1 - cosA * cosA );

	// elbow direction (model space: x forward, y left, z up)
	VectorSet( pole, -0.35f, side == SIDE_R ? -0.7f : 0.7f, -1.0f );
	VectorMA( pole, -DotProduct( pole, n ), n, p );
	if ( VectorNormalize( p ) < 1e-3f ) {
		VectorSet( p, 0, 0, -1 );
	}
	VectorScale( n, a * cosA, elbow );
	VectorMA( elbow, a * sinA, p, elbow );
	VectorAdd( s, elbow, elbow );

	// upper arm onto the elbow
	VectorSubtract( e, s, from );
	VectorNormalize( from );
	VectorSubtract( elbow, s, to );
	VectorNormalize( to );
	VB_Align( from, to, r );
	VB_Rotate( ua, r, s );

	// forearm onto the wrist
	VectorSubtract( vb.pos[hd], vb.pos[fa], from );
	VectorNormalize( from );
	VectorMA( s, d, n, to );
	VectorSubtract( to, vb.pos[fa], to );
	VectorNormalize( to );
	VB_Align( from, to, r );
	VB_Rotate( fa, r, vb.pos[fa] );

	// the hand along the controller
	if ( fg >= 0 ) {
		VectorSubtract( vb.pos[fg], vb.pos[hd], from );
		if ( VectorNormalize( from ) > 0.1f ) {
			VB_Align( from, handDir, r );
			VB_Rotate( hd, r, vb.pos[hd] );
		}
	}
}

/*
=================
CG_VR_BodyApply

Called with the local player's skeletal body just before it is drawn.
=================
*/
void CG_VR_BodyApply( centity_t *cent, refEntity_t *body ) {
	vec3_t fwd, target, dir, handFwd, yaw;
	int h, side, i, k;

	VectorSet( yaw, 0, vb.bodyYaw, 0 );
	AngleVectors( yaw, fwd, NULL, NULL );

	if ( !VB_Pose( body ) ) {
		body->origin[0] = cgvr.headOrigin[0] - fwd[0] * VB_EYES_AHEAD;
		body->origin[1] = cgvr.headOrigin[1] - fwd[1] * VB_EYES_AHEAD;
		VectorCopy( body->origin, body->oldorigin );
		body->renderfx &= ~RF_THIRD_PERSON;     // no arm bones: just show the animated body
		vb.drawnTime = cg.time;
		return;
	}

	// stand under the head: the neck behind and below the eyes, so the shoulders stay out
	// of view (the body sinks into the floor a little for short players, nobody looks there)
	if ( vb.neck >= 0 ) {
		vec3_t neck, want, move;
		for ( i = 0; i < 3; i++ ) {
			neck[i] = body->origin[i] + vb.pos[vb.neck][0] * body->axis[0][i] +
					  vb.pos[vb.neck][1] * body->axis[1][i] + vb.pos[vb.neck][2] * body->axis[2][i];
		}
		VectorMA( cgvr.headOrigin, -VB_EYES_AHEAD, fwd, want );
		want[2] -= VB_EYES_ABOVE;
		VectorSubtract( want, neck, move );
		move[2] = move[2] > 12.0f ? 12.0f : ( move[2] < -24.0f ? -24.0f : move[2] );
		VectorAdd( body->origin, move, body->origin );
	} else {
		body->origin[0] = cgvr.headOrigin[0] - fwd[0] * VB_EYES_AHEAD;
		body->origin[1] = cgvr.headOrigin[1] - fwd[1] * VB_EYES_AHEAD;
	}
	VectorCopy( body->origin, body->oldorigin );

	for ( h = 0; h < 2; h++ ) {
		if ( !cgVR->handValid[h] ) {
			continue;
		}
		side = h == VR_HAND_RIGHT ? SIDE_R : SIDE_L;
		AngleVectors( cgvr.handAngles[h], handFwd, NULL, NULL );
		// the grip pose is the palm; the wrist is behind it
		VectorMA( cgvr.handOrigin[h], -VB_PALM_TO_WRIST, handFwd, target );
		VB_ToModel( body, target, target );
		VB_DirToModel( body, handFwd, dir );
		VectorNormalize( dir );
		VB_ReachArm( side, target, dir );
	}

	for ( i = 0; i < vb.numBones; i++ ) {
		float *o = vb.out + i * 12;
		for ( k = 0; k < 3; k++ ) {
			if ( vb.flags[i] & BONEFLAG_TAG ) {
				o[k * 3 + 0] = vb.rot[i][k][0];
				o[k * 3 + 1] = vb.rot[i][k][1];
				o[k * 3 + 2] = vb.rot[i][k][2];
			} else {
				o[0 * 3 + k] = vb.rot[i][k][0];
				o[1 * 3 + k] = vb.rot[i][k][1];
				o[2 * 3 + k] = vb.rot[i][k][2];
			}
		}
		VectorCopy( vb.pos[i], o + 9 );
	}

	body->ragdollBones = vb.out;
	body->ragdollMagic = RAGDOLL_MAGIC;
	body->renderfx &= ~RF_THIRD_PERSON;     // seen from the eyes
	body->renderfx |= RF_VR_NOHANDS;        // the tracked hands are drawn instead
	body->renderfx |= RF_FIRST_PERSON;      // only for the eyes: not in mirrors or scopes
	vb.drawnTime = cg.time;
}

#ifdef VR_NET_HANDS
/*
==============================================================================

Other VR players (co-op): their arms reach where their hands really are. The server
sends each VR player's hands (entityState vrHand); the body keeps its own facing,
animation and hands, and its gun hangs from the hand that holds it.

==============================================================================
*/

static float vbRemote[MAX_CLIENTS][VB_MAX_BONES * 12];

/*
=================
CG_VR_RemoteAngles

Another VR player: the head turns where their head looks, the body follows it the way
our own does (VB_NECK_SLACK, turning to catch up), instead of everything facing the gun.
=================
*/
static float vbRemoteYaw[MAX_CLIENTS];
static int vbRemoteTime[MAX_CLIENTS];

void CG_VR_RemoteAngles( centity_t *cent, vec3_t legs[3], vec3_t torso[3], vec3_t headAxis[3] ) {
	entityState_t *es = &cent->currentState;
	int n = es->clientNum;
	float head, diff, step, speed;
	vec3_t angles;

	if ( n < 0 || n >= MAX_CLIENTS ) {
		return;
	}
	head = es->vrHead[YAW];
	if ( cent->interpolate && ( cent->nextState.vrFlags & VRE_ACTIVE ) ) {
		head += cg.frameInterpolation * AngleNormalize180( cent->nextState.vrHead[YAW] - head );
	}

	if ( vbRemoteTime[n] <= 0 || cg.time < vbRemoteTime[n] || cg.time - vbRemoteTime[n] > 500 ) {
		vbRemoteYaw[n] = head;
	} else if ( cg.time > vbRemoteTime[n] ) {
		diff = AngleNormalize180( head - vbRemoteYaw[n] );
		if ( diff > VB_NECK_SLACK ) {
			vbRemoteYaw[n] += diff - VB_NECK_SLACK;
		} else if ( diff < -VB_NECK_SLACK ) {
			vbRemoteYaw[n] += diff + VB_NECK_SLACK;
		}
		diff = AngleNormalize180( head - vbRemoteYaw[n] );
		speed = VectorLength( es->pos.trDelta ) > 20.0f ? VB_TURN_SPEED_MOVE : VB_TURN_SPEED;
		step = speed * ( cg.time - vbRemoteTime[n] ) * 0.001f;
		vbRemoteYaw[n] += diff > step ? step : ( diff < -step ? -step : diff );
		vbRemoteYaw[n] = AngleNormalize360( vbRemoteYaw[n] );
	}
	vbRemoteTime[n] = cg.time;

	VectorSet( angles, 0, vbRemoteYaw[n], 0 );
	AnglesToAxis( angles, legs );
	AxisClear( torso );
	// the head, relative to the body
	VectorSet( angles, es->vrHead[PITCH], AngleNormalize180( head - vbRemoteYaw[n] ), 0 );
	AnglesToAxis( angles, headAxis );
}

qboolean CG_VR_RemoteBody( centity_t *cent ) {
	trap_Cvar_Update( &vr_fullBody );
	return vr_fullBody.integer && ( cent->currentState.vrFlags & VRE_ACTIVE ) &&
		   cent->currentState.number != cg.snap->ps.clientNum && cent->currentState.clientNum >= 0 &&
		   cent->currentState.clientNum < MAX_CLIENTS && !( cent->currentState.eFlags & EF_DEAD );
}

void CG_VR_BodyApplyRemote( centity_t *cent, refEntity_t *body ) {
	entityState_t *es = &cent->currentState;
	int flags = es->vrFlags, h, side, i, k, gun;
	vec3_t hand, target, dir, aim;
	float *out = vbRemote[es->clientNum];

	if ( !VB_Pose( body ) ) {
		return;
	}
	gun = ( flags & VRE_RIGHTHANDED ) ? VR_HAND_RIGHT : VR_HAND_LEFT;
	AngleVectors( cent->lerpAngles, aim, NULL, NULL );

	for ( h = 0; h < 2; h++ ) {
		if ( !( flags & ( h == VR_HAND_LEFT ? VRE_HAND_LEFT : VRE_HAND_RIGHT ) ) ) {
			continue;
		}
		// smooth between snapshots like everything else about the entity
		VectorCopy( es->vrHand[h], hand );
		if ( cent->interpolate && ( cent->nextState.vrFlags & VRE_ACTIVE ) ) {
			for ( k = 0; k < 3; k++ ) {
				hand[k] += cg.frameInterpolation * ( cent->nextState.vrHand[h][k] - hand[k] );
			}
		}
		VectorAdd( cent->lerpOrigin, hand, hand );
		side = h == VR_HAND_RIGHT ? SIDE_R : SIDE_L;

		// the gun hand points where they aim; the other along the arm
		if ( h == gun ) {
			VB_DirToModel( body, aim, dir );
		} else {
			VB_ToModel( body, hand, dir );
			VectorSubtract( dir, vb.pos[vb.upper[side]], dir );
		}
		VectorNormalize( dir );
		VB_ToModel( body, hand, target );
		VectorMA( target, -VB_PALM_TO_WRIST, dir, target );
		VB_ReachArm( side, target, dir );
		{
			static int next;
			char dbg[8];
			trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
			if ( atoi( dbg ) == 3 && cg.time >= next ) {
				next = cg.time + 500;
				CG_Printf( "VR body %d: %s hand %.0f %.0f %.0f from the origin, wrist misses by %.1f\n",
						   es->clientNum, h == VR_HAND_RIGHT ? "right" : "left", es->vrHand[h][0], es->vrHand[h][1],
						   es->vrHand[h][2], Distance( vb.pos[vb.hand[side]], target ) );
			}
		}
	}

	for ( i = 0; i < vb.numBones; i++ ) {
		float *o = out + i * 12;
		for ( k = 0; k < 3; k++ ) {
			if ( vb.flags[i] & BONEFLAG_TAG ) {
				o[k * 3 + 0] = vb.rot[i][k][0];
				o[k * 3 + 1] = vb.rot[i][k][1];
				o[k * 3 + 2] = vb.rot[i][k][2];
			} else {
				o[0 * 3 + k] = vb.rot[i][k][0];
				o[1 * 3 + k] = vb.rot[i][k][1];
				o[2 * 3 + k] = vb.rot[i][k][2];
			}
		}
		VectorCopy( vb.pos[i], o + 9 );
	}
	body->ragdollBones = out;
	body->ragdollMagic = RAGDOLL_MAGIC;
}
#endif
