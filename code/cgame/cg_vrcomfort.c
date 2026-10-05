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

// cg_vrcomfort.c -- comfort options (RealRTCW-VR)
//
// The teleport arc: while the off hand's stick is pushed forward (vr_teleport, cl_vr.c) an
// arc is thrown from that hand; where it lands is checked for room to stand, and the
// engine sends the move when the stick is let go (PM_VRRoomScaleMove). The vignette
// (vr_vignette): the edges of the view darken while moving or turning smoothly.

#include "cg_local.h"

vmCvar_t vr_vignette;

static qhandle_t cfWhite;
static float cfAmount;          // vignette now, 0..1
static float cfLastYaw;
static int cfLastTime;

void CG_VR_ComfortInit( void ) {
	trap_Cvar_Register( &vr_vignette, "vr_vignette", "0", CVAR_ARCHIVE );
	cfWhite = trap_R_RegisterShaderNoMip( "white" );      // flat: vertex colour and alpha, on top
	cfAmount = 0;
	cfLastTime = 0;
}

// a quad, added wound both ways (the flat shaders cull one side)
static void CF_Quad( const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t d,
					 const byte ca[4], const byte cb[4], const byte cc[4], const byte cd[4] ) {
	polyVert_t v[4], r[4];
	const float *p[4] = { a, b, c, d };
	const byte *col[4] = { ca, cb, cc, cd };
	int i;

	for ( i = 0; i < 4; i++ ) {
		VectorCopy( p[i], v[i].xyz );
		v[i].st[0] = v[i].st[1] = 0.5f;
		memcpy( v[i].modulate, col[i], 4 );
	}
	for ( i = 0; i < 4; i++ ) {
		r[i] = v[3 - i];
	}
	trap_R_AddPolyToScene( cfWhite, 4, v );
	trap_R_AddPolyToScene( cfWhite, 4, r );
}

/*
==============
CF_TeleportArc
==============
*/
#define TP_SPEED        650.0f
#define TP_STEP         0.04f
#define TP_MAX_DIST     450.0f

static void CF_Segment( const vec3_t a, const vec3_t b, const byte col[4] ) {
	vec3_t along, toEye, side, p[4];

	VectorSubtract( b, a, along );
	VectorSubtract( cg.refdef.vieworg, a, toEye );
	CrossProduct( along, toEye, side );
	if ( VectorNormalize( side ) < 0.001f ) {
		return;
	}
	VectorScale( side, 0.35f, side );
	VectorAdd( a, side, p[0] );
	VectorSubtract( a, side, p[1] );
	VectorSubtract( b, side, p[2] );
	VectorAdd( b, side, p[3] );
	CF_Quad( p[0], p[1], p[2], p[3], col, col, col, col );
}

static void CF_TeleportArc( void ) {
	static const vec3_t mins = { -18, -18, -24 }, maxs = { 18, 18, 48 };
	static const byte good[4] = { 80, 255, 120, 200 }, bad[4] = { 255, 70, 60, 200 };
	playerState_t *ps = &cg.predictedPlayerState;
	vec3_t dir, vel, p, next, target, flat;
	trace_t tr, room;
	qboolean hit = qfalse, valid = qfalse;
	const byte *col;
	int off = !CG_VR_DominantHand(), i;

	cgVR->teleportValid = 0;
	if ( !cgVR->teleportAiming || !cgVR->handValid[off] || ps->stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	AngleVectors( cgvr.handAngles[off], dir, NULL, NULL );
	VectorScale( dir, TP_SPEED, vel );
	VectorCopy( cgvr.handOrigin[off], p );

	// where it comes down
	for ( i = 0; i < 40 && !hit; i++ ) {
		VectorMA( p, TP_STEP, vel, next );
		vel[2] -= 800.0f * TP_STEP;
		CG_Trace( &tr, p, NULL, NULL, next, ps->clientNum, MASK_PLAYERSOLID );
		if ( tr.fraction < 1.0f ) {
			hit = qtrue;
		}
		VectorCopy( tr.endpos, next );
		VectorCopy( p, dir );       // (segment start, drawn below once valid is known)
		VectorCopy( next, p );
	}

	if ( hit && tr.plane.normal[2] > 0.7f ) {
		// room to stand there, and not too far
		VectorCopy( tr.endpos, target );
		target[2] -= mins[2] - 1.0f;
		CG_Trace( &room, target, mins, maxs, target, ps->clientNum, MASK_PLAYERSOLID );
		VectorSubtract( target, ps->origin, flat );
		flat[2] = 0;
		valid = !room.startsolid && !room.allsolid && VectorLength( flat ) <= TP_MAX_DIST;
		if ( valid ) {
			VectorSubtract( target, ps->origin, cgVR->teleportDelta );
			cgVR->teleportValid = 1;
		}
	}
	col = valid ? good : bad;

	// draw it: run the arc again with the colour known
	AngleVectors( cgvr.handAngles[off], dir, NULL, NULL );
	VectorScale( dir, TP_SPEED, vel );
	VectorCopy( cgvr.handOrigin[off], p );
	for ( i = 0; i < 40; i++ ) {
		VectorMA( p, TP_STEP, vel, next );
		vel[2] -= 800.0f * TP_STEP;
		CG_Trace( &tr, p, NULL, NULL, next, ps->clientNum, MASK_PLAYERSOLID );
		CF_Segment( p, tr.endpos, col );
		if ( tr.fraction < 1.0f ) {
			break;
		}
		VectorCopy( next, p );
	}

	// and a ring where it lands
	if ( hit ) {
		vec3_t c, a0, a1, b0, b1;
		float ang0, ang1;
		int s;

		VectorCopy( tr.endpos, c );
		c[2] += 1.0f;
		for ( s = 0; s < 16; s++ ) {
			ang0 = 2 * M_PI * s / 16;
			ang1 = 2 * M_PI * ( s + 1 ) / 16;
			VectorSet( a0, c[0] + 14 * cos( ang0 ), c[1] + 14 * sin( ang0 ), c[2] );
			VectorSet( a1, c[0] + 14 * cos( ang1 ), c[1] + 14 * sin( ang1 ), c[2] );
			VectorSet( b0, c[0] + 11 * cos( ang0 ), c[1] + 11 * sin( ang0 ), c[2] );
			VectorSet( b1, c[0] + 11 * cos( ang1 ), c[1] + 11 * sin( ang1 ), c[2] );
			CF_Quad( a0, a1, b1, b0, col, col, col, col );
		}
	}
}

/*
==============
CF_Vignette
==============
*/
static void CF_Vignette( void ) {
	static const byte clear[4] = { 0, 0, 0, 0 }, black[4] = { 0, 0, 0, 255 };
	playerState_t *ps = &cg.predictedPlayerState;
	vec3_t eye, centre, in0, in1, mid0, mid1, out0, out1;
	float dt, speed, turn, want, inner, feather, outer, a0, a1;
	int i, s;
	const float d = 3.0f;

	trap_Cvar_Update( &vr_vignette );

	// once a frame: how much
	if ( cg.time != cfLastTime ) {
		dt = ( cg.time - cfLastTime ) * 0.001f;
		if ( dt > 0 && dt < 0.2f ) {
			speed = sqrt( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] );
			turn = fabs( AngleNormalize180( cgvr.yawOffset - cfLastYaw ) ) / dt;
			want = speed / 280.0f;
			if ( turn / 120.0f > want ) {
				want = turn / 120.0f;
			}
			if ( want > 1 ) {
				want = 1;
			}
			cfAmount += ( want - cfAmount ) * ( dt * 8.0f > 1 ? 1 : dt * 8.0f );
		}
		cfLastYaw = cgvr.yawOffset;
		cfLastTime = cg.time;
	}
	if ( vr_vignette.value <= 0 || cfAmount < 0.02f ) {
		return;
	}

	// a ring in front of this eye: clear in the middle, fading to black at the edges
	VectorCopy( cg.refdef.vieworg, eye );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( eye, cgVR->eyeOffset[i], cg.refdef.viewaxis[i], eye );
	}
	VectorMA( eye, d, cg.refdef.viewaxis[0], centre );
	inner = d * tan( DEG2RAD( 62.0f - 30.0f * cfAmount * Com_Clamp( 0, 1, vr_vignette.value ) ) );
	feather = inner + d * 0.35f;
	outer = d * tan( DEG2RAD( 84.0f ) );

	for ( s = 0; s < 32; s++ ) {
		a0 = 2 * M_PI * s / 32;
		a1 = 2 * M_PI * ( s + 1 ) / 32;
#define RINGPT( out, r, a ) VectorMA( centre, ( r ) * cos( a ), cg.refdef.viewaxis[1], out ); \
	VectorMA( out, ( r ) * sin( a ), cg.refdef.viewaxis[2], out )
		RINGPT( in0, inner, a0 );
		RINGPT( in1, inner, a1 );
		RINGPT( mid0, feather, a0 );
		RINGPT( mid1, feather, a1 );
		RINGPT( out0, outer, a0 );
		RINGPT( out1, outer, a1 );
#undef RINGPT
		CF_Quad( in0, in1, mid1, mid0, clear, clear, black, black );
		CF_Quad( mid0, mid1, out1, out0, black, black, black, black );
	}
}

/*
==============================================================================

Pings (co-op): a tap of Y marks where the gun points, for everyone, for a few seconds.
Seen through walls, with a blip.

==============================================================================
*/

#define VP_MAX      8
#define VP_TIME     6000

static struct {
	int     time;
	int     client;
	vec3_t  pos;
} vrPings[VP_MAX];
static int vrPingLastCount = -1;
static sfxHandle_t vrPingSound;

void CG_VR_PingReceived( int client, const vec3_t pos ) {
	int i, slot = 0;

	for ( i = 0; i < VP_MAX; i++ ) {
		if ( vrPings[i].time < vrPings[slot].time ) {
			slot = i;
		}
	}
	vrPings[slot].time = cg.time;
	vrPings[slot].client = client;
	VectorCopy( pos, vrPings[slot].pos );
	if ( !vrPingSound ) {
		vrPingSound = trap_S_RegisterSound( "sound/misc/w_pkup.wav" );
	}
	trap_S_StartLocalSound( vrPingSound, CHAN_LOCAL_SOUND );
}

// the ping button: where the gun (or the head) points
static void CF_SendPing( void ) {
	vec3_t start, fwd, end;
	trace_t tr;

	if ( vrPingLastCount < 0 || !cgVR ) {
		vrPingLastCount = cgVR ? cgVR->pingCount : 0;
		return;
	}
	if ( cgVR->pingCount == vrPingLastCount ) {
		return;
	}
	vrPingLastCount = cgVR->pingCount;

	if ( cgVR->muzzleValid ) {
		VectorCopy( cgvr.muzzle, start );
		AngleVectors( cgvr.weaponAngles, fwd, NULL, NULL );
	} else {
		VectorCopy( cg.refdef.vieworg, start );
		VectorCopy( cg.refdef.viewaxis[0], fwd );
	}
	VectorMA( start, 4096, fwd, end );
	CG_Trace( &tr, start, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
	VectorMA( tr.endpos, -4, fwd, end );
	trap_SendClientCommand( va( "vrping %d %d %d", (int)end[0], (int)end[1], (int)end[2] ) );
}

static void CF_DrawPings( void ) {
	static const byte colors[4][4] = {
		{ 255, 200, 60, 230 }, { 80, 200, 255, 230 }, { 120, 255, 120, 230 }, { 255, 110, 220, 230 }
	};
	vec3_t up, side, top, a, b, c, d;
	int i;

	VectorCopy( cg.refdef.viewaxis[2], up );
	VectorCopy( cg.refdef.viewaxis[1], side );
	for ( i = 0; i < VP_MAX; i++ ) {
		const byte *col = colors[vrPings[i].client & 3];
		float size;

		if ( !vrPings[i].time || cg.time - vrPings[i].time > VP_TIME || cg.time < vrPings[i].time ) {
			continue;
		}
		// a post up from the spot, and a diamond at its top that stays a readable size
		VectorMA( vrPings[i].pos, 40, up, top );
		size = Distance( cg.refdef.vieworg, top ) * 0.012f;
		if ( size < 2 ) {
			size = 2;
		}
		VectorMA( vrPings[i].pos, size * 0.15f, side, a );
		VectorMA( vrPings[i].pos, -size * 0.15f, side, b );
		VectorMA( top, -size * 0.15f, side, c );
		VectorMA( top, size * 0.15f, side, d );
		CF_Quad( a, b, c, d, col, col, col, col );
		VectorMA( top, size, up, a );
		VectorMA( top, size, side, b );
		VectorMA( top, -size, up, c );
		VectorMA( top, -size, side, d );
		CF_Quad( a, b, c, d, col, col, col, col );
	}
}

/*
==============
CG_VR_Comfort

Each eye pass, before the view is rendered.
==============
*/
void CG_VR_Comfort( void ) {
	// everyone sees the pings, headset or not
	if ( cfWhite && cg.snap ) {
		CF_DrawPings();
	}
	if ( !cgVR || !cgvr.active ) {
		return;
	}
	CF_SendPing();
	cgVR->onLadder = ( cg.predictedPlayerState.pm_flags & PMF_LADDER ) != 0;
	cgVR->yawOffset = cgvr.yawOffset;
	if ( !CG_VR_EyePass() || !cfWhite ) {
		return;
	}
	CF_TeleportArc();
	CF_Vignette();
}
