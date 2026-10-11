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

// g_vrextras.c -- GORE & EXTRAS settings: one cvar each, published to every cgame in g_vrx
//
// Single player sets them on the GORE & EXTRAS page, a co-op host on HOST GAME's EXTRAS &
// GORE page; either way they apply to everyone in the game, with or without a headset.

#include "g_local.h"
#include "../botlib/botlib.h"
#include "../botlib/be_aas.h"
#include "../botlib/be_ea.h"
#include "../botlib/be_ai_gen.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/be_ai_weap.h"
#include "../botlib/botai.h"
#include "ai_cast.h"
#include "bg_vrextras.h"

static vmCvar_t vrxCvars[VRX_COUNT];
static vmCvar_t g_vrx;
static const char *vrxNames[VRX_COUNT] = {
	"g_gore",
	"g_shotReactions",
	"g_woundDecals",
	"g_throatSlit",
	"g_propGrab",
	"g_propHit",
	"g_dismember",
	"g_decapitate",
	"g_headScare",
	"g_stockGibs",
};
static qboolean vrxRegistered;

static void G_VRX_PropsFrame( void );
static void G_VRX_MarkersFrame( void );
static gentity_t *G_VRX_Marker( gentity_t *body, qboolean create );
static gentity_t *G_VRX_SpawnHead( gentity_t *body, const vec3_t pos, gentity_t *by );
static void G_VRX_DropWeapon( gentity_t *e, cast_state_t *cs );

static void G_VRX_Register( void ) {
	char def[2] = { 0, 0 };
	int i;

	if ( vrxRegistered ) {
		return;
	}
	for ( i = 0; i < VRX_COUNT; i++ ) {
		def[0] = VRX_DEFAULTS[i];
		trap_Cvar_Register( &vrxCvars[i], vrxNames[i], def, CVAR_ARCHIVE );
	}
	trap_Cvar_Register( &g_vrx, "g_vrx", VRX_DEFAULTS, CVAR_SERVERINFO );
	vrxRegistered = qtrue;
}

// every server frame: publish the settings when one changes
void G_VRX_Frame( void ) {
	char s[VRX_COUNT + 1];
	int i, v;

	G_VRX_Register();
	for ( i = 0; i < VRX_COUNT; i++ ) {
		trap_Cvar_Update( &vrxCvars[i] );
		v = vrxCvars[i].integer;
		s[i] = '0' + ( v < 0 ? 0 : ( v > 9 ? 9 : v ) );
	}
	s[VRX_COUNT] = 0;
	trap_Cvar_Update( &g_vrx );
	if ( strcmp( s, g_vrx.string ) ) {
		trap_Cvar_Set( "g_vrx", s );
	}

	G_VRX_PropsFrame();
	G_VRX_MarkersFrame();
}

// one of the settings, for game code
int G_VRX( int which ) {
	G_VRX_Register();
	if ( which < 0 || which >= VRX_COUNT ) {
		return 0;
	}
	return vrxCvars[which].integer;
}

// a gory one: only with the master switch on too
qboolean G_VRX_Gore( int which ) {
	return G_VRX( VRX_GORE ) && G_VRX( which );
}

/*
=================
G_VRX_Dismember (g_dismember)

A blast (1), or a heavy hit too (2), as it kills someone or lands on a body: limbs off.
A blast takes one or two, close ones at the shoulder or hip; a heavy round takes the one
it hit, at the elbow or knee. Never on the living.
=================
*/
static qboolean G_VRX_Blast( int mod ) {
	switch ( mod ) {
	case MOD_GRENADE:
	case MOD_GRENADE_SPLASH:
	case MOD_ROCKET:
	case MOD_ROCKET_SPLASH:
	case MOD_PANZERFAUST:
	case MOD_GRENADE_LAUNCHER:
	case MOD_GRENADE_PINEAPPLE:
	case MOD_MORTAR:
	case MOD_MORTAR_SPLASH:
	case MOD_DYNAMITE:
	case MOD_DYNAMITE_SPLASH:
	case MOD_AIRSTRIKE:
	case MOD_EXPLOSIVE:
		return qtrue;
	default:
		return qfalse;
	}
}

// did a shot from point along dir go through targ's head? (the hit box is far bigger than the
// body, so the limb it met is guessed from its path, and a path through the head takes no limb)
static qboolean G_VRX_ShotAtHead( gentity_t *targ, const vec3_t dir, const vec3_t point ) {
	orientation_t or;
	vec3_t head, d, rel;
	float t;

	if ( !point || !dir ) {
		return qfalse;
	}
	if ( trap_GetTag( targ->s.number, "tag_head", &or ) ) {
		VectorMA( or.origin, 4, or.axis[2], head );     // the tag is at the bottom of the head
	} else {
		VectorCopy( targ->r.currentOrigin, head );
		head[2] += targ->client ? targ->client->ps.viewheight : 40;
	}
	VectorNormalize2( dir, d );
	VectorSubtract( head, point, rel );
	t = DotProduct( rel, d );
	VectorMA( rel, -t, d, rel );        // from the path to the head's centre
	return VectorLength( rel ) < 7.0f;
}

static void G_VRX_Dismember( gentity_t *targ, gentity_t *inflictor, const vec3_t dir, const vec3_t point, int mod, int damage, qboolean headshot ) {
	static const int limbs[4] = { VRX_GB_LARM, VRX_GB_RARM, VRX_GB_LLEG, VRX_GB_RLEG };
	gentity_t *m, *te;
	int have, cut = 0, high = 0, i, k;
	qboolean blast = G_VRX_Blast( mod );

	if ( !G_VRX_Gore( VRX_DISMEMBER ) || !targ->client || !targ->aiCharacter || targ->health > 0 ) {
		return;
	}
	if ( !blast && !( G_VRX( VRX_DISMEMBER ) >= 2 && ( mod == MOD_VENOM || damage >= 45 ) ) ) {
		return;
	}
	if ( !blast && ( headshot || G_VRX_ShotAtHead( targ, dir, point ) ) ) {
		return;     // a round through the head takes no limb
	}
	have = G_VRX_GoreBits( targ );
	if ( blast ) {
		float d = inflictor ? Distance( inflictor->r.currentOrigin, targ->r.currentOrigin ) : 999;
		if ( damage < 40 && d > 100 ) {
			return;     // too far off
		}
		for ( k = 0; k < 2; k++ ) {
			i = rand() % 4;
			if ( k == 0 || random() < 0.5f ) {
				cut |= limbs[i];
				if ( d < 90 || random() < 0.3f ) {
					high |= limbs[i];
				}
			}
		}
	} else {
		// the limb that was hit: by height, and which side (for someone who was standing)
		vec3_t right, ang, rel;
		qboolean leg = point && point[2] < targ->r.currentOrigin[2] - 4;
		int side = rand() & 1;
		if ( point && !( targ->client->ps.eFlags & EF_DEAD ) ) {
			VectorSet( ang, 0, targ->client->ps.viewangles[YAW], 0 );
			AngleVectors( ang, NULL, right, NULL );
			VectorSubtract( point, targ->r.currentOrigin, rel );
			side = DotProduct( rel, right ) > 0;
		}
		cut = limbs[( leg ? 2 : 0 ) + side];
	}
	cut &= ~have;
	if ( !cut ) {
		return;
	}
	G_VRX_AddGoreBits( targ, cut );
	m = G_VRX_Marker( targ, qtrue );
	m->s.frame |= high & cut;

	te = G_TempEntity( point ? (float *)point : targ->r.currentOrigin, EV_VR_GORE );
	te->s.eventParm = VRG_LIMBS;
	te->s.otherEntityNum = targ->s.number;
	te->s.density = cut;
	te->s.frame = high & cut;
	if ( dir ) {
		VectorCopy( dir, te->s.origin2 );
	}
}

/*
==============================================================================

Tight hit boxes

A shot that meets a character's box only hits if it goes through the body itself: the
skeleton the cgame drew him with (CG_GetTag "vrcaps:N"), as capsules round the bones,
a ball for the head and one for the groin. Through the gaps it carries on past him.
Where it met him (VRH_*) goes to the damage: head shots, groin shots, which limb.

==============================================================================
*/

static int vrxHitEnt = -1;
static int vrxHitRegion;
static vec3_t vrxHitPoint;

// where the ray o + t d (d unit, t up to maxT) first meets the capsule a-b of radius r
static qboolean G_VRX_RayCapsule( const vec3_t o, const vec3_t d, float maxT, const vec3_t a, const vec3_t b, float r, float *tOut ) {
	vec3_t ab, w, p, q;
	float B, C, D, E, den, s, t, dist;

	VectorSubtract( b, a, ab );
	VectorSubtract( o, a, w );
	B = DotProduct( d, ab );
	C = DotProduct( ab, ab );
	D = DotProduct( d, w );
	E = DotProduct( ab, w );
	den = C - B * B;
	if ( C < 1e-6f ) {
		s = 0;
	} else if ( den < 1e-6f ) {
		s = 0;
	} else {
		s = ( E - B * D ) / den;    // the segment parameter nearest the line
	}
	s = s < 0 ? 0 : ( s > 1 ? 1 : s );
	VectorMA( a, s, ab, q );
	VectorSubtract( q, o, p );
	t = DotProduct( p, d );
	VectorMA( o, t, d, p );
	dist = Distance( p, q );
	if ( dist > r ) {
		return qfalse;
	}
	t -= sqrt( r * r - dist * dist );
	if ( t > maxT || t < -2 * r ) {
		return qfalse;
	}
	*tOut = t < 0 ? 0 : t;
	return qtrue;
}

// 1: the shot from start to end goes through e's body (hit: where, vrxHitRegion: what);
// 0: through the gaps; -1: no skeleton to go by (the box it is)
static int G_VRX_BodyTrace( gentity_t *e, const vec3_t start, const vec3_t end, vec3_t hit ) {
	static const struct { int a, b; float r; int region; } caps[] = {
		{ VRB_SPINE2, VRB_NECK, 7.0f, VRH_CHEST },
		{ VRB_PELVIS, VRB_SPINE2, 7.0f, VRH_BELLY },
		{ VRB_NECK, VRB_HEAD, 2.8f, VRH_CHEST },
		{ VRB_LUA, VRB_LFA, 3.2f, VRH_LARM }, { VRB_LFA, VRB_LHAND, 2.8f, VRH_LARM },
		{ VRB_RUA, VRB_RFA, 3.2f, VRH_RARM }, { VRB_RFA, VRB_RHAND, 2.8f, VRH_RARM },
		{ VRB_LTHIGH, VRB_LCALF, 4.8f, VRH_LLEG }, { VRB_LCALF, VRB_LFOOT, 3.8f, VRH_LLEG }, { VRB_LFOOT, VRB_LTOE, 2.8f, VRH_LLEG },
		{ VRB_RTHIGH, VRB_RCALF, 4.8f, VRH_RLEG }, { VRB_RCALF, VRB_RFOOT, 3.8f, VRH_RLEG }, { VRB_RFOOT, VRB_RTOE, 2.8f, VRH_RLEG },
	};
	vec3_t pt[VRB_GROUPS * 4], d, u, a, b, up, right, fwd, groin;
	orientation_t or;
	float maxT, t, best = 1e9f, groinT = 1e9f;
	int g, k, region = VRH_NONE;

	for ( g = 0; g < VRB_GROUPS; g++ ) {
		if ( !trap_GetTag( e->s.number, va( "vrcaps:%i", g ), &or ) ) {
			return -1;
		}
		VectorCopy( or.origin, pt[g * 4] );
		for ( k = 0; k < 3; k++ ) {
			VectorCopy( or.axis[k], pt[g * 4 + 1 + k] );
		}
	}
	if ( Distance( pt[VRB_PELVIS], e->r.currentOrigin ) > 96.0f ) {
		return -1;      // not where the server has him: the cgame's drawing is stale
	}
	VectorSubtract( end, start, d );
	maxT = VectorNormalize( d );

	for ( k = 0; k < (int)ARRAY_LEN( caps ); k++ ) {
		if ( G_VRX_RayCapsule( start, d, maxT, pt[caps[k].a], pt[caps[k].b], caps[k].r, &t ) && t < best ) {
			best = t;
			region = caps[k].region;
		}
	}
	// the head: a ball above the neck; the hands
	VectorSubtract( pt[VRB_HEAD], pt[VRB_NECK], u );
	VectorNormalize( u );
	VectorMA( pt[VRB_HEAD], 3.5f, u, a );
	if ( G_VRX_RayCapsule( start, d, maxT, a, a, 5.0f, &t ) && t < best ) {
		best = t;
		region = VRH_HEAD;
	}
	for ( k = 0; k < 2; k++ ) {
		int fa = k ? VRB_RFA : VRB_LFA, hand = k ? VRB_RHAND : VRB_LHAND;
		VectorSubtract( pt[hand], pt[fa], u );
		VectorMA( pt[hand], 0.5f, u, b );
		if ( G_VRX_RayCapsule( start, d, maxT, pt[hand], b, 2.4f, &t ) && t < best ) {
			best = t;
			region = k ? VRH_RARM : VRH_LARM;
		}
	}
	// the groin: between the tops of the thighs, a little down and forward
	VectorSubtract( pt[VRB_SPINE2], pt[VRB_PELVIS], up );
	VectorSubtract( pt[VRB_RTHIGH], pt[VRB_LTHIGH], right );
	if ( VectorNormalize( up ) > 0.1f && VectorNormalize( right ) > 0.1f ) {
		CrossProduct( up, right, fwd );
		VectorAdd( pt[VRB_LTHIGH], pt[VRB_RTHIGH], groin );
		VectorScale( groin, 0.5f, groin );
		VectorMA( groin, -4.0f, up, groin );
		VectorMA( groin, 2.5f, fwd, groin );
		if ( G_VRX_RayCapsule( start, d, maxT, groin, groin, 3.6f, &groinT ) && groinT < best + 3.0f ) {
			best = groinT < best ? groinT : best;
			region = VRH_GROIN;
		}
	}
	if ( region == VRH_NONE ) {
		return 0;
	}
	VectorMA( start, best, d, hit );
	vrxHitEnt = e->s.number;
	vrxHitRegion = region;
	VectorCopy( hit, vrxHitPoint );
	return 1;
}

// a bullet's trace, but through the gaps round a character's body
void G_VRX_ShotRefine( trace_t *tr, const vec3_t start, const vec3_t end, int mask ) {
	vec3_t from, hit;
	float total = Distance( start, end );
	int k, r;

	vrxHitEnt = -1;
	for ( k = 0; k < 4; k++ ) {
		gentity_t *e;
		if ( tr->fraction >= 1.0f || tr->entityNum >= MAX_CLIENTS || tr->startsolid ) {
			return;
		}
		e = &g_entities[tr->entityNum];
		if ( !e->client || !e->aiCharacter || e->health <= 0 ) {
			return;
		}
		r = G_VRX_BodyTrace( e, start, end, hit );
		if ( r < 0 ) {
			return;
		}
		if ( r > 0 ) {
			VectorCopy( hit, tr->endpos );
			tr->fraction = total > 0 ? Distance( start, hit ) / total : 0;
			return;
		}
		// missed him: on past
		VectorCopy( tr->endpos, from );
		trap_Trace( tr, from, NULL, NULL, (float *)end, e->s.number, mask );
		tr->fraction = total > 0 ? Distance( start, tr->endpos ) / total : 1;
	}
}

// where the shot that hit targ at point met him (VRH_NONE: not known)
int G_VRX_HitRegion( gentity_t *targ, const vec3_t point ) {
	if ( vrxHitEnt != targ->s.number || !point || Distance( point, vrxHitPoint ) > 3.0f ) {
		return VRH_NONE;
	}
	return vrxHitRegion;
}

/*
=================
G_VRX_Hit (g_dismember 2)

From G_Damage, before a player's blow lands on someone alive (take: the damage it does,
returned changed). Brutal Half-Life style:

- a heavy round (a rifle, the Venom, a shotgun close up) into an arm can take it off: the
  man drops his gun and runs, screaming and bleeding out (VRX_GB_CRIPPLED);
- one into a leg takes the leg and him with it;
- the downed (crippled, given up, or nearly dead) are executed: a knife or a kick finishes
  them, the knife taking the head, the kick stamping it in, and so does a shot to the head
  from close by.
=================
*/
static qboolean vrxExecution;       // the blow being dealt finishes someone downed

static qboolean G_VRX_Shotgun( int mod ) {
	return mod == MOD_SHOTGUN;     // (co-op has the one)
}

static qboolean G_VRX_Knife( int mod ) {
	return mod == MOD_KNIFE || mod == MOD_KNIFE2 || mod == MOD_KNIFE_STEALTH;
}

// hits hard enough to take a limb or a head
static qboolean G_VRX_Heavy( gentity_t *targ, gentity_t *attacker, int mod, int damage ) {
	if ( mod == MOD_VENOM || damage >= 45 ) {
		return qtrue;
	}
	return G_VRX_Shotgun( mod ) && attacker && Distance( attacker->r.currentOrigin, targ->r.currentOrigin ) < 300.0f;
}

// crippled, given up, or nearly dead
static qboolean G_VRX_Downed( gentity_t *e ) {
	int max;

	if ( !e->client || !e->aiCharacter || e->health <= 0 ) {
		return qfalse;
	}
	return ( G_VRX_GoreBits( e ) & ( VRX_GB_SURRENDER | VRX_GB_CRIPPLED | VRX_GB_DOWNED ) ) != 0;
}

// a body shot that leaves someone nearly dead can put them on their knees
static void G_VRX_KnockDown( gentity_t *targ, gentity_t *attacker ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m;

	G_VRX_AddGoreBits( targ, VRX_GB_DOWNED );
	m = G_VRX_Marker( targ, qtrue );
	m->s.time2 = 1 + rand() % 3;       // how he goes down (cgame CG_VRX_Downed)
	m->count = attacker->s.number;
	m->timestamp = level.time + 900;
	if ( cs ) {
		G_VRX_DropWeapon( targ, cs );
		cs->noAttackTime = level.time + 3600000;
		cs->pauseTime = level.time + 3600000;       // stays down
	}
	VectorClear( targ->client->ps.velocity );
}

// an arm off someone alive: no gun, running, bleeding
static void G_VRX_Cripple( gentity_t *targ, gentity_t *attacker, int arm, const vec3_t dir, const vec3_t point ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m, *te;
	int high = random() < 0.4f ? arm : 0;

	G_VRX_AddGoreBits( targ, arm | VRX_GB_CRIPPLED );
	m = G_VRX_Marker( targ, qtrue );
	m->s.frame |= high;
	m->count = attacker->s.number;      // who he runs from (and who gets him when he bleeds out)
	m->timestamp = level.time + 700;    // the first bleed

	te = G_TempEntity( (float *)point, EV_VR_GORE );
	te->s.eventParm = VRG_LIMBS;
	te->s.otherEntityNum = targ->s.number;
	te->s.density = arm;
	te->s.frame = high;
	if ( dir ) {
		VectorCopy( dir, te->s.origin2 );
	}
	if ( cs ) {
		G_VRX_DropWeapon( targ, cs );
		cs->noAttackTime = level.time + 3600000;    // never fights again
		cs->dangerEntity = attacker->s.number;
		VectorCopy( attacker->r.currentOrigin, cs->dangerEntityPos );
		cs->dangerEntityValidTime = level.time + 3600000;
		cs->dangerDist = 1200;
		cs->dangerEntityTimestamp = level.time;
	}
}

// a shot in the balls: he doubles over clutching them for a few seconds (cgame CG_VRX_Downed)
static void G_VRX_GroinHit( gentity_t *targ, gentity_t *attacker ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m = G_VRX_Marker( targ, qtrue );

	m->s.time = level.time;
	m->count = attacker->s.number;
	if ( cs ) {
		if ( cs->noAttackTime < level.time + VRX_GROIN_MS ) {
			cs->noAttackTime = level.time + VRX_GROIN_MS;
		}
		if ( cs->pauseTime < level.time + VRX_GROIN_MS ) {
			cs->pauseTime = level.time + VRX_GROIN_MS;
		}
	}
	VectorClear( targ->client->ps.velocity );
}

// vrtestbody down N / groin (test aid): put him down (style N, 0 random) or double him over
void G_VRX_TestState( gentity_t *targ, gentity_t *by, qboolean groin, int style ) {
	if ( groin ) {
		G_VRX_GroinHit( targ, by );
		return;
	}
	G_VRX_KnockDown( targ, by );
	if ( style >= 1 && style <= 3 ) {
		G_VRX_Marker( targ, qtrue )->s.time2 = style;
	}
}

int G_VRX_Hit( gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, const vec3_t dir, const vec3_t point,
			   int mod, int take, qboolean headshot ) {
	vec3_t ang, right, rel;
	float side;
	int region;

	vrxExecution = qfalse;
	if ( mod == MOD_UNKNOWN || !targ->client || !targ->aiCharacter || targ->health <= 0 || take <= 0 || !attacker || !attacker->client ||
		 attacker->aiCharacter || !G_VRX_Gore( VRX_DISMEMBER ) || G_VRX( VRX_DISMEMBER ) < 2 ||
		 !VRX_HUMAN_MALE( targ->aiCharacter ) ) {
		return take;
	}
	// finishing the downed: a knife, a kick, or any shot from close by
	if ( G_VRX_Downed( targ ) && ( G_VRX_Knife( mod ) || mod == MOD_KICKED ||
		 Distance( attacker->r.currentOrigin, targ->r.currentOrigin ) < 160.0f ) ) {
		vrxExecution = qtrue;
		return targ->health + 50;
	}
	// where the tight hit boxes say it landed (VRH_NONE: not known, go by height)
	region = point ? G_VRX_HitRegion( targ, point ) : VRH_NONE;
	if ( region == VRH_GROIN && take < targ->health && !G_VRX_Downed( targ ) ) {
		G_VRX_GroinHit( targ, attacker );
		return take;
	}
	// a body shot that leaves them nearly dead: down on their knees
	if ( !headshot && take < targ->health && !G_VRX_Downed( targ ) && ( !point || !G_VRX_ShotAtHead( targ, dir, point ) ) ) {
		int max = targ->client->ps.stats[STAT_MAX_HEALTH] > 0 ? targ->client->ps.stats[STAT_MAX_HEALTH] : 100;
		if ( targ->health - take <= max * 0.3f && random() < 0.6f ) {
			G_VRX_KnockDown( targ, attacker );
			return take;
		}
	}
	if ( headshot || !point || take >= targ->health || !G_VRX_Heavy( targ, attacker, mod, take ) ||
		 G_VRX_ShotAtHead( targ, dir, point ) ) {
		return take;
	}
	if ( region == VRH_LLEG || region == VRH_RLEG ) {
		return random() < 0.5f ? targ->health + 50 : take;
	}
	if ( region == VRH_LARM || region == VRH_RARM ) {
		if ( !( G_VRX_GoreBits( targ ) & ( VRX_GB_LARM | VRX_GB_RARM ) ) && random() < 0.6f ) {
			G_VRX_Cripple( targ, attacker, region == VRH_RARM ? VRX_GB_RARM : VRX_GB_LARM, dir, point );
		}
		return take;
	}
	if ( region != VRH_NONE ) {
		return take;        // the head, chest, belly or groin: no limb
	}
	// which limb: by height, and how far out to the side (for someone standing)
	VectorSet( ang, 0, targ->client->ps.viewangles[YAW], 0 );
	AngleVectors( ang, NULL, right, NULL );
	VectorSubtract( point, targ->r.currentOrigin, rel );
	side = DotProduct( rel, right );
	if ( point[2] < targ->r.currentOrigin[2] - 4 ) {
		if ( random() < 0.5f ) {
			return targ->health + 50;   // the leg goes, and he goes down with it (G_VRX_Dismember)
		}
	} else if ( fabs( side ) > 6.0f && point[2] < targ->r.currentOrigin[2] + targ->client->ps.viewheight - 8 &&
				!( G_VRX_GoreBits( targ ) & ( VRX_GB_LARM | VRX_GB_RARM ) ) && random() < 0.6f ) {
		G_VRX_Cripple( targ, attacker, side > 0 ? VRX_GB_RARM : VRX_GB_LARM, dir, point );
	}
	return take;
}

/*
=================
G_VRX_Kill

From G_Damage, as a blow kills a character (before its die function): the gory ones are
sent to every cgame (EV_VR_GORE). Only ever killing blows, never someone alive.

A knife kill from behind, or one that lands high, cuts the throat; one that goes in low
(a stab to the belly in VR, from a crouch without a headset) disembowels.
=================
*/
void G_VRX_Kill( gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, const vec3_t dir, const vec3_t point,
				 int mod, int damage, qboolean headshot ) {
	gentity_t *te;
	int gore = VRG_NONE;
	float neck;

	if ( !targ->client || !targ->aiCharacter ) {
		return;
	}
	// a blast close by tears the body open (head shots are the cgame's: cg_vrextras.c head wounds)
	if ( VRX_HUMAN_MALE( targ->aiCharacter ) && G_VRX_Blast( mod ) && G_VRX_Gore( VRX_DISMEMBER ) &&
		 ( !inflictor || Distance( inflictor->r.currentOrigin, targ->r.currentOrigin ) < 200.0f ) ) {
		G_VRX_AddGoreBits( targ, VRX_GB_BLOWN );
	}
	G_VRX_Dismember( targ, inflictor, dir, point, mod, damage, headshot );
	{
		gentity_t *m = G_VRX_Marker( targ, qfalse );
		if ( m ) {
			m->s.density &= ~( VRX_GB_CRIPPLED | VRX_GB_DOWNED );   // (dead now; the arm stays off)
		}
	}

	// the head: an execution with a knife takes it off, a stamp or a heavy round blows it apart
	if ( VRX_HUMAN_MALE( targ->aiCharacter ) && G_VRX_Gore( VRX_DISMEMBER ) && G_VRX( VRX_DISMEMBER ) >= 2 &&
		 !( G_VRX_GoreBits( targ ) & VRX_GB_HEAD ) ) {
		qboolean execution = vrxExecution || mod == MOD_KICKED;    // a kick to the head of the down or the dead stamps it in
		orientation_t or;
		vec3_t neck;

		vrxExecution = qfalse;
		if ( trap_GetTag( targ->s.number, "tag_head", &or ) ) {
			VectorCopy( or.origin, neck );
		} else {
			VectorCopy( targ->r.currentOrigin, neck );
			neck[2] += targ->client->ps.viewheight - 4;
		}
		if ( execution && G_VRX_Knife( mod ) && G_VRX_Gore( VRX_DECAP ) ) {
			G_VRX_AddGoreBits( targ, VRX_GB_HEAD );
			G_VRX_SpawnHead( targ, neck, attacker );
			te = G_TempEntity( neck, EV_VR_GORE );
			te->s.eventParm = VRG_DECAP;
			te->s.otherEntityNum = targ->s.number;
			te->s.otherEntityNum2 = attacker ? attacker->s.number : ENTITYNUM_WORLD;
			G_AddEvent( targ, EV_GENERAL_SOUND, G_SoundIndex( "sound/weapons/knife/knife_hit3.wav" ) );
			return;
		}
		if ( execution ||
			 ( headshot && G_VRX_Heavy( targ, attacker, mod, damage ) ) ) {
			G_VRX_AddGoreBits( targ, VRX_GB_HEAD );
			te = G_TempEntity( neck, EV_VR_GORE );
			te->s.eventParm = VRG_HEADPOP;
			te->s.otherEntityNum = targ->s.number;
			te->s.otherEntityNum2 = attacker ? attacker->s.number : ENTITYNUM_WORLD;
			if ( dir ) {
				VectorCopy( dir, te->s.origin2 );
			}
			return;
		}
	}
	vrxExecution = qfalse;
	if ( !G_VRX_Gore( VRX_THROAT ) ) {
		return;
	}
	if ( mod == MOD_KNIFE || mod == MOD_KNIFE2 || mod == MOD_KNIFE_STEALTH ) {
		neck = targ->r.currentOrigin[2] + targ->client->ps.viewheight - 12.0f;
		if ( mod == MOD_KNIFE_STEALTH || !point || point[2] > neck - 6.0f ) {
			gore = VRG_THROAT;
		} else {
			gore = VRG_BELLY;
		}
		// without a headset a knife reaches about neck high: a crouching attacker goes low
		if ( mod != MOD_KNIFE_STEALTH && attacker && attacker->client &&
			 ( attacker->client->ps.pm_flags & PMF_DUCKED ) ) {
			gore = VRG_BELLY;
		}
	}
	if ( gore == VRG_NONE ) {
		return;
	}
	te = G_TempEntity( targ->r.currentOrigin, EV_VR_GORE );
	te->s.eventParm = gore;
	te->s.otherEntityNum = targ->s.number;
	te->s.otherEntityNum2 = attacker ? attacker->s.number : ENTITYNUM_WORLD;
	if ( dir ) {
		VectorCopy( dir, te->s.origin2 );
	}
}

/*
==============================================================================

Props in the hand (g_propGrab, g_propHit)

Chairs, bottles, lamps and the other loose decorations can be picked up (the VR off hand,
or hold use without a headset) and thrown. A thrown one flies as a physics object (G_RunItem),
knocks whoever it meets (a stun, or a stun and some damage), and something breakable
breaks on a hard hit. Only separate model entities: nothing built into the map, nothing a
script needs, nothing big.

==============================================================================
*/

#define VRX_MAX_THROWN      16
#define VRX_PROP_MAX_SIZE   52.0f

typedef struct {
	int         eType, contents, clipmask, ownerNum;
	qboolean    physicsObject;
	float       physicsBounce;
	void ( *think )( gentity_t *self );
	void ( *touch )( gentity_t *self, gentity_t *other, trace_t *trace );
	int         nextthink;
} vrxPropState_t;

typedef struct {
	int             prop;           // entity number, -1 none
	vrxPropState_t  saved;
	vec3_t          offset;         // prop origin from the hand
} vrxHeld_t;

typedef struct {
	int             prop;           // -1 free
	int             thrower;
	vrxPropState_t  saved;
	vec3_t          lastPos, lastVel;
	qboolean        harmless;       // already hit someone
	qboolean        scared;         // a head: has already scared whoever saw it land
	int             time;
} vrxThrown_t;

static vrxHeld_t vrxHeld[MAX_CLIENTS];
static vrxThrown_t vrxThrown[VRX_MAX_THROWN];
static qboolean vrxPropsReady;

static void G_VRX_Launch( vrxThrown_t *t, gentity_t *e, gentity_t *ent, const vec3_t vel, const vrxPropState_t *saved );
static void G_VRX_HeadScare( vrxThrown_t *t, gentity_t *head );

static void G_VRX_PropsInit( void ) {
	int i;

	if ( vrxPropsReady ) {
		return;
	}
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		vrxHeld[i].prop = -1;
	}
	for ( i = 0; i < VRX_MAX_THROWN; i++ ) {
		vrxThrown[i].prop = -1;
	}
	vrxPropsReady = qtrue;
}

// what can be picked up
static qboolean G_VRX_Grabbable( gentity_t *e ) {
	static const char *never[] = {
		"props_decorBRUSH", "props_statue", "props_statueBRUSH", "props_bench", "props_box_",
		"props_crate", "props_flamebarrel", "props_flippy", "props_58x112", "props_castlebed",
		"props_footlocker", "props_snowGenerator", "props_shard", "props_damageinflictor",
		"props_sparks", "props_gunsparks", "props_flamethrower", "props_ExploPart", "props_skyportal",
		"props_FireColumn", "props_smoke", "props_locker", "props_radioSEVEN",
	};
	int i, k;

	if ( !e->inuse || !e->r.linked || !e->classname || Q_stricmpn( e->classname, "props_", 6 ) ) {
		return qfalse;
	}
	for ( i = 0; i < (int)ARRAY_LEN( never ); i++ ) {
		if ( !Q_stricmpn( e->classname, never[i], strlen( never[i] ) ) ) {
			return qfalse;
		}
	}
	// a model of its own, not part of the map; nothing scripts or triggers point at
	if ( !e->s.modelindex || ( e->model && e->model[0] == '*' ) || e->targetname || e->scriptName ) {
		return qfalse;
	}
	if ( e->active || e->r.contents == 0 ) {
		return qfalse;      // in someone's hands the old way (chairs), or broken
	}
	for ( k = 0; k < 3; k++ ) {
		if ( e->r.maxs[k] - e->r.mins[k] > VRX_PROP_MAX_SIZE ) {
			return qfalse;
		}
	}
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( vrxHeld[i].prop == e->s.number ) {
			return qfalse;
		}
	}
	return qtrue;
}

// the nearest prop within reach of a point (the hand, or where the crosshair stops)
gentity_t *G_VRX_FindProp( const vec3_t p, float reach ) {
	gentity_t *e, *best = NULL;
	float d, bestDist = 1e9f;
	vec3_t c;
	int i, k;

	if ( !G_VRX( VRX_PROPGRAB ) ) {
		return NULL;
	}
	G_VRX_PropsInit();
	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( !G_VRX_Grabbable( e ) ) {
			continue;
		}
		// how far outside its box
		d = 0;
		for ( k = 0; k < 3; k++ ) {
			c[k] = p[k] < e->r.absmin[k] ? e->r.absmin[k] - p[k] : ( p[k] > e->r.absmax[k] ? p[k] - e->r.absmax[k] : 0 );
			d += c[k] * c[k];
		}
		d = sqrt( d );
		if ( d <= reach && d < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	return best;
}

static void G_VRX_SaveProp( gentity_t *e, vrxPropState_t *st ) {
	st->eType = e->s.eType;
	st->contents = e->r.contents;
	st->clipmask = e->clipmask;
	st->ownerNum = e->r.ownerNum;
	st->physicsObject = e->physicsObject;
	st->physicsBounce = e->physicsBounce;
	st->think = e->think;
	st->touch = e->touch;
	st->nextthink = e->nextthink;
}

// a thrown prop's spin and speed now (TR_LINEAR / TR_GRAVITY; single player's and co-op's
// BG_EvaluateTrajectory differ)
static void G_VRX_PropAngles( gentity_t *e, vec3_t out ) {
	VectorCopy( e->s.apos.trBase, out );
	if ( e->s.apos.trType == TR_LINEAR ) {
		VectorMA( out, ( level.time - e->s.apos.trTime ) * 0.001f, e->s.apos.trDelta, out );
	}
}

static void G_VRX_PropVelocity( gentity_t *e, vec3_t out ) {
	VectorClear( out );
	if ( e->s.pos.trType == TR_GRAVITY ) {
		VectorCopy( e->s.pos.trDelta, out );
		out[2] -= DEFAULT_GRAVITY * ( level.time - e->s.pos.trTime ) * 0.001f;
	} else if ( e->s.pos.trType == TR_LINEAR ) {
		VectorCopy( e->s.pos.trDelta, out );
	}
}

// back to sitting where it is, the way it was
static void G_VRX_RestoreProp( gentity_t *e, const vrxPropState_t *st ) {
	vec3_t angles;

	G_VRX_PropAngles( e, angles );
	angles[PITCH] = angles[ROLL] = 0;       // settles upright
	G_SetOrigin( e, e->r.currentOrigin );
	G_SetAngle( e, angles );
	e->s.eType = st->eType;
	e->r.contents = st->contents;
	e->clipmask = st->clipmask;
	e->r.ownerNum = st->ownerNum;
	e->physicsObject = st->physicsObject;
	e->physicsBounce = st->physicsBounce;
	e->think = st->think;
	e->touch = st->touch;
	e->nextthink = st->nextthink ? level.time + 50 : 0;
	trap_LinkEntity( e );
}

/*
=================
G_VRX_PropGrab / G_VRX_PropHold / G_VRX_PropRelease

A player's hand (or held-in-front point without a headset) taking a prop, carrying it,
and letting it go: dropped, or thrown with vel.
=================
*/
qboolean G_VRX_PropGrab( gentity_t *ent, gentity_t *e, const vec3_t hand ) {
	vec3_t c;
	int n = ent->s.number;

	G_VRX_PropsInit();
	if ( n < 0 || n >= MAX_CLIENTS || vrxHeld[n].prop >= 0 || !e || !G_VRX_Grabbable( e ) ) {
		return qfalse;
	}
	vrxHeld[n].prop = e->s.number;
	G_VRX_SaveProp( e, &vrxHeld[n].saved );
	// held by its middle
	VectorAdd( e->r.absmin, e->r.absmax, c );
	VectorScale( c, 0.5f, c );
	VectorSubtract( e->r.currentOrigin, c, vrxHeld[n].offset );

	e->s.eType = ET_GENERAL;
	e->r.contents = 0;              // nothing bumps into it while it's carried
	e->physicsObject = qfalse;
	e->think = NULL;
	e->touch = NULL;
	e->nextthink = 0;
	G_VRX_PropHold( ent, hand, NULL );
	return qtrue;
}

int G_VRX_PropHeld( gentity_t *ent ) {
	G_VRX_PropsInit();
	return ent->s.number < MAX_CLIENTS ? vrxHeld[ent->s.number].prop : -1;
}

void G_VRX_PropHold( gentity_t *ent, const vec3_t hand, const vec3_t angles ) {
	vrxHeld_t *h = &vrxHeld[ent->s.number];
	gentity_t *e;
	vec3_t p, eye;
	trace_t tr;

	if ( h->prop < 0 ) {
		return;
	}
	e = &g_entities[h->prop];
	if ( !e->inuse ) {
		h->prop = -1;
		return;
	}
	VectorAdd( hand, h->offset, p );
	// not through walls: stopped where the way from the holder's eyes meets one
	VectorCopy( ent->client->ps.origin, eye );
	eye[2] += ent->client->ps.viewheight;
	trap_Trace( &tr, eye, NULL, NULL, p, ent->s.number, MASK_SOLID );
	VectorCopy( tr.endpos, p );
	e->s.pos.trType = TR_INTERPOLATE;
	VectorCopy( p, e->s.pos.trBase );
	VectorCopy( p, e->r.currentOrigin );
	if ( angles ) {
		e->s.apos.trType = TR_INTERPOLATE;
		VectorCopy( angles, e->s.apos.trBase );
		VectorCopy( angles, e->r.currentAngles );
	}
	trap_LinkEntity( e );
}

void G_VRX_PropRelease( gentity_t *ent, const vec3_t vel ) {
	vrxHeld_t *h = &vrxHeld[ent->s.number];
	vrxThrown_t *t = NULL;
	gentity_t *e;
	int i;

	if ( h->prop < 0 ) {
		return;
	}
	e = &g_entities[h->prop];
	h->prop = -1;
	if ( !e->inuse ) {
		return;
	}
	for ( i = 0; i < VRX_MAX_THROWN; i++ ) {
		if ( vrxThrown[i].prop < 0 ) {
			t = &vrxThrown[i];
			break;
		}
	}
	if ( !t ) {
		G_VRX_RestoreProp( e, &h->saved );      // too much flying: just put down
		return;
	}
	G_VRX_Launch( t, e, ent, vel, &h->saved );
}

// something set flying from ent, with its state to settle back to
static void G_VRX_Launch( vrxThrown_t *t, gentity_t *e, gentity_t *ent, const vec3_t vel, const vrxPropState_t *saved ) {
	memset( t, 0, sizeof( *t ) );
	t->prop = e->s.number;
	t->thrower = ent->s.number;
	t->saved = *saved;
	t->time = level.time;
	VectorCopy( e->r.currentOrigin, t->lastPos );
	VectorCopy( vel, t->lastVel );

	// flies as a physics object (G_RunItem), tumbling
	e->r.contents = CONTENTS_CORPSE;        // shootable, not in the way
	e->clipmask = MASK_SOLID;
	e->r.ownerNum = ent->s.number;          // not stopped by the thrower
	e->physicsObject = qtrue;
	e->physicsBounce = 0.35f;
	e->s.groundEntityNum = ENTITYNUM_NONE;
	e->s.pos.trType = TR_GRAVITY;
	e->s.pos.trTime = level.time;
	VectorCopy( e->r.currentOrigin, e->s.pos.trBase );
	VectorCopy( vel, e->s.pos.trDelta );
	e->s.apos.trType = TR_LINEAR;
	e->s.apos.trTime = level.time;
	VectorCopy( e->r.currentAngles, e->s.apos.trBase );
	VectorSet( e->s.apos.trDelta, crandom() * 300, crandom() * 300, crandom() * 200 );
	if ( VectorLength( vel ) < 60 ) {
		VectorClear( e->s.apos.trDelta );   // put down, not thrown
	}
	trap_LinkEntity( e );
}

// break something breakable
static qboolean G_VRX_PropBreak( gentity_t *e, gentity_t *by ) {
	if ( !e->takedamage || !e->die ) {
		return qfalse;
	}
	G_Damage( e, by, by, NULL, NULL, 9999, 0, MOD_CRUSH );
	return qtrue;
}

// thrown props: who they hit, when they break, when they've landed
static void G_VRX_PropsFrame( void ) {
	int i;

	if ( !vrxPropsReady ) {
		return;
	}
	for ( i = 0; i < VRX_MAX_THROWN; i++ ) {
		vrxThrown_t *t = &vrxThrown[i];
		gentity_t *e, *thrower, *hit;
		vec3_t vel, pos, dir;
		trace_t tr;
		float speed;

		if ( t->prop < 0 ) {
			continue;
		}
		e = &g_entities[t->prop];
		thrower = &g_entities[t->thrower];
		if ( !e->inuse || !e->physicsObject || e->r.contents == 0 ) {
			t->prop = -1;       // broke (or someone else took it over)
			continue;
		}
		VectorCopy( e->r.currentOrigin, pos );
		G_VRX_PropVelocity( e, vel );
		speed = VectorLength( t->lastVel );

		// meeting someone on the way
		if ( !t->harmless && speed > 150 ) {
			vec3_t mins, maxs;
			VectorScale( e->r.mins, 0.6f, mins );
			VectorScale( e->r.maxs, 0.6f, maxs );
			trap_Trace( &tr, t->lastPos, mins, maxs, pos, t->thrower, MASK_SHOT );
			hit = tr.entityNum < ENTITYNUM_MAX_NORMAL ? &g_entities[tr.entityNum] : NULL;
			if ( hit && hit != e && hit->client && hit->health > 0 ) {
				t->harmless = qtrue;
				G_VRX_HeadScare( t, e );
				VectorNormalize2( t->lastVel, dir );
				if ( G_VRX( VRX_PROPHIT ) ) {
					// a stun (the pain of it, knocked back a little), and with 2 some real damage
					int dmg = G_VRX( VRX_PROPHIT ) >= 2 ? (int)( speed / 30.0f ) : 1;
					dmg = dmg < 1 ? 1 : ( dmg > 40 ? 40 : dmg );
					hit->client->ps.velocity[0] += dir[0] * 160;
					hit->client->ps.velocity[1] += dir[1] * 160;
					G_Damage( hit, e, thrower, dir, tr.endpos, dmg, 0, MOD_CRUSH );
				}
				if ( G_VRX_PropBreak( e, thrower ) ) {
					t->prop = -1;
					continue;
				}
				// bounces off them
				VectorScale( vel, -0.3f, e->s.pos.trDelta );
				e->s.pos.trDelta[2] = 60;
				VectorCopy( pos, e->s.pos.trBase );
				e->s.pos.trTime = level.time;
				VectorCopy( e->s.pos.trDelta, vel );
			}
		}

		// landed: a hard landing breaks what can break, else it settles
		if ( e->s.pos.trType == TR_STATIONARY || level.time - t->time > 8000 ) {
			G_VRX_HeadScare( t, e );
			if ( speed > 420 && G_VRX_PropBreak( e, thrower ) ) {
				t->prop = -1;
				continue;
			}
			G_VRX_RestoreProp( e, &t->saved );
			t->prop = -1;
			continue;
		}
		VectorCopy( pos, t->lastPos );
		VectorCopy( vel, t->lastVel );
	}
}

// vrtestprop (test aid, cheats): the nearest prop that can be picked up, at eye level in front
void G_VRX_TestProp( gentity_t *ent ) {
	gentity_t *e, *best = NULL;
	float d, bestDist = 1e9f;
	vec3_t fwd, p;
	int i;

	G_VRX_PropsInit();
	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( G_VRX_Grabbable( e ) && ( d = Distance( e->r.currentOrigin, ent->r.currentOrigin ) -
										 ( e->s.dmgFlags == VRX_ENT_HEAD ? 100000 : 0 ) ) < bestDist ) {     // heads first
			best = e;
			bestDist = d;
		}
	}
	if ( !best ) {
		G_Printf( "no prop to pick up on this level\n" );
		for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
			e = &g_entities[i];
			if ( e->inuse && e->classname && !Q_stricmpn( e->classname, "props_", 6 ) ) {
				G_Printf( "  %s model %d %s size %.0f %.0f %.0f%s%s%s\n", e->classname, e->s.modelindex,
						  e->model ? e->model : "", e->r.maxs[0] - e->r.mins[0], e->r.maxs[1] - e->r.mins[1],
						  e->r.maxs[2] - e->r.mins[2], e->targetname ? " named" : "", e->scriptName ? " scripted" : "",
						  e->r.linked ? "" : " unlinked" );
			}
		}
		return;
	}
	G_Printf( "%s (%s) brought over from %.0f away\n", best->classname, best->model2 ? best->model2 : "", bestDist );
	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	fwd[2] = 0;
	VectorNormalize( fwd );
	VectorMA( ent->r.currentOrigin, 32, fwd, p );
	p[2] += ent->client->ps.viewheight - ( best->r.maxs[2] + best->r.mins[2] ) * 0.5f;
	G_SetOrigin( best, p );
	trap_LinkEntity( best );
}

// held or flying
static qboolean G_VRX_PropBusy( gentity_t *e ) {
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( vrxHeld[i].prop == e->s.number ) {
			return qtrue;
		}
	}
	for ( i = 0; i < VRX_MAX_THROWN; i++ ) {
		if ( vrxThrown[i].prop == e->s.number ) {
			return qtrue;
		}
	}
	return qfalse;
}

// set a prop dropping / flying from by, settling back to how it is now
static void G_VRX_LaunchProp( gentity_t *e, gentity_t *by, const vec3_t vel ) {
	vrxPropState_t st;
	int i;

	G_VRX_PropsInit();
	G_VRX_SaveProp( e, &st );
	for ( i = 0; i < VRX_MAX_THROWN; i++ ) {
		if ( vrxThrown[i].prop < 0 ) {
			G_VRX_Launch( &vrxThrown[i], e, by, vel, &st );
			return;
		}
	}
}

/*
==============================================================================

Bodies cut up (g_decapitate), and what's cut off

What has been done to a body travels in a small broadcast entity of its own, a gore
marker (bits in s.density, VRX_GB_*), since a dead character's entity state is rebuilt
from its player state every frame. A head that comes off is a prop of its own
(props_vrhead): picked up and thrown like any other, drawn by every cgame with the
dead man's own head (s.otherEntityNum2 is the body).

==============================================================================
*/

#define VRX_MAX_HEADS       16

static gentity_t *G_VRX_Marker( gentity_t *body, qboolean create ) {
	gentity_t *e;
	int i;

	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( e->inuse && e->s.dmgFlags == VRX_ENT_GORE && e->s.otherEntityNum2 == body->s.number ) {
			return e;
		}
	}
	if ( !create ) {
		return NULL;
	}
	e = G_Spawn();
	e->classname = "vr_gore";
	e->s.eType = ET_GENERAL;
	e->s.modelindex = 0;                    // nothing drawn
	e->s.dmgFlags = VRX_ENT_GORE;
	e->s.otherEntityNum2 = body->s.number;
	e->s.density = 0;
	e->r.svFlags |= SVF_BROADCAST;
	G_SetOrigin( e, body->r.currentOrigin );
	trap_LinkEntity( e );
	return e;
}

int G_VRX_GoreBits( gentity_t *body ) {
	gentity_t *m = G_VRX_Marker( body, qfalse );
	return m ? m->s.density : 0;
}

void G_VRX_AddGoreBits( gentity_t *body, int bits ) {
	gentity_t *m = G_VRX_Marker( body, qtrue );
	m->s.density |= bits;
}

// markers follow their bodies, and go when the body does (or is alive again)
static void G_VRX_MarkersFrame( void ) {
	gentity_t *e, *body;
	int i;

	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || e->s.dmgFlags != VRX_ENT_GORE ) {
			continue;
		}
		body = &g_entities[e->s.otherEntityNum2];
		if ( !body->inuse || !body->client ||
			 ( body->health > 0 && !( e->s.density & ( VRX_GB_SCARED | VRX_GB_SURRENDER | VRX_GB_CRIPPLED | VRX_GB_DOWNED ) ) &&
			   !( e->s.time && level.time < e->s.time + VRX_GROIN_MS + 500 ) ) ) {
			G_FreeEntity( e );
			continue;
		}
		if ( body->health > 0 && ( e->s.density & ( VRX_GB_CRIPPLED | VRX_GB_DOWNED ) ) && level.time >= e->timestamp ) {
			// bleeding out, still running from whoever did it
			gentity_t *by = e->count >= 0 && e->count < MAX_CLIENTS && g_entities[e->count].inuse ? &g_entities[e->count] : NULL;
			cast_state_t *cs = AICast_GetCastState( body->s.number );
			e->timestamp = level.time + ( ( e->s.density & VRX_GB_CRIPPLED ) ? 1000 : 1500 );    // about half a minute either way
			if ( cs && by && ( e->s.density & VRX_GB_CRIPPLED ) ) {
				VectorCopy( by->r.currentOrigin, cs->dangerEntityPos );
				cs->dangerEntityValidTime = level.time + 3600000;
			}
			G_Damage( body, by, by ? by : body, NULL, NULL, ( e->s.density & VRX_GB_CRIPPLED ) ? 3 : 1, DAMAGE_NO_KNOCKBACK | DAMAGE_NO_PROTECTION, MOD_UNKNOWN );
			if ( !e->inuse ) {
				continue;
			}
		}
		if ( Distance( e->r.currentOrigin, body->r.currentOrigin ) > 1.0f ) {
			G_SetOrigin( e, body->r.currentOrigin );
			trap_LinkEntity( e );
		}
	}
}

// a head off a body, at pos, dropping
static gentity_t *G_VRX_SpawnHead( gentity_t *body, const vec3_t pos, gentity_t *by ) {
	gentity_t *e, *oldest = NULL;
	vec3_t vel, angles;
	int i, heads = 0;

	// not too many lying about: the oldest goes
	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( e->inuse && e->s.dmgFlags == VRX_ENT_HEAD ) {
			heads++;
			if ( !oldest || e->timestamp < oldest->timestamp ) {
				oldest = e;
			}
		}
	}
	if ( heads >= VRX_MAX_HEADS && oldest && !G_VRX_PropBusy( oldest ) ) {
		G_FreeEntity( oldest );
	}

	e = G_Spawn();
	e->classname = "props_vrhead";
	e->s.eType = ET_GENERAL;
	e->s.modelindex = G_ModelIndex( "models/gibs/skull.mdc" );    // drawn as the man's own head
	e->s.dmgFlags = VRX_ENT_HEAD;
	e->s.otherEntityNum2 = body->s.number;
	VectorSet( e->r.mins, -5, -5, -1 );      // the origin is the bottom of the neck
	VectorSet( e->r.maxs, 5, 5, 11 );
	e->r.contents = CONTENTS_CORPSE;
	e->clipmask = MASK_SOLID;
	e->timestamp = level.time;
	G_SetOrigin( e, (float *)pos );
	VectorSet( angles, 0, body->r.currentAngles[YAW], 0 );
	G_SetAngle( e, angles );
	trap_LinkEntity( e );

	VectorSet( vel, crandom() * 40, crandom() * 40, 60 );
	G_VRX_LaunchProp( e, by, vel );
	return e;
}

/*
=================
G_VRX_Decap_f

"vrdecap <body> <x> <y> <z>": the player's cgame saw their knife go through a dead
body's neck at x y z (it knows where the ragdoll is, the server doesn't). Checked here
before anything happens.
=================
*/
void G_VRX_Decap_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *body, *te;
	vec3_t neck;
	int n, i;

	if ( !G_VRX_Gore( VRX_DECAP ) || !ent->client || ent->client->ps.stats[STAT_HEALTH] <= 0 ||
		 ent->client->ps.weapon != WP_KNIFE || trap_Argc() < 5 ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	if ( n < 0 || n >= MAX_CLIENTS ) {
		return;
	}
	body = &g_entities[n];
	if ( !body->inuse || !body->client || !body->aiCharacter || body->health > 0 ||
		 ( G_VRX_GoreBits( body ) & VRX_GB_HEAD ) ) {
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		trap_Argv( 2 + i, arg, sizeof( arg ) );
		neck[i] = atof( arg );
	}
	// within reach of the player, and on (near) the body
	if ( Distance( neck, ent->r.currentOrigin ) > 120 || Distance( neck, body->r.currentOrigin ) > 200 ) {     // (ragdolls slide)
		return;
	}
	G_VRX_AddGoreBits( body, VRX_GB_HEAD );
	G_VRX_SpawnHead( body, neck, ent );

	te = G_TempEntity( neck, EV_VR_GORE );
	te->s.eventParm = VRG_DECAP;
	te->s.otherEntityNum = body->s.number;
	te->s.otherEntityNum2 = ent->s.number;
	G_AddEvent( body, EV_GENERAL_SOUND, G_SoundIndex( "sound/weapons/knife/knife_hit3.wav" ) );
}

// vrtestdecap (test aid, cheats): the nearest dead body's head off, as a knife would
void G_VRX_TestDecap( gentity_t *ent ) {
	gentity_t *e, *best = NULL, *te;
	float d, bestDist = 1e9f;
	vec3_t neck;
	int i;

	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e->inuse && e->client && e->aiCharacter && e->health <= 0 && !( G_VRX_GoreBits( e ) & VRX_GB_HEAD ) &&
			 ( d = Distance( e->r.currentOrigin, ent->r.currentOrigin ) ) < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	if ( !best ) {
		return;
	}
	VectorCopy( best->r.currentOrigin, neck );
	neck[2] += 8;
	G_VRX_AddGoreBits( best, VRX_GB_HEAD );
	G_VRX_SpawnHead( best, neck, ent );
	te = G_TempEntity( neck, EV_VR_GORE );
	te->s.eventParm = VRG_DECAP;
	te->s.otherEntityNum = best->s.number;
}

/*
=================
G_VRX_HeadScare / G_VRX_Surrender (g_headScare)

A head a player threw, landing or hitting someone: the soldiers who see it run from it
(as from a grenade), and with 2 those it catches unawares, or lands right by, give up:
weapon dropped, hands up (cgame raises the arms, VRX_GB_SURRENDER), no more fighting.
=================
*/
#define VRX_SCARE_DIST      280
#define VRX_GIVEUP_DIST     160

static qboolean G_VRX_Scareable( gentity_t *e ) {
	switch ( e->aiCharacter ) {
	case AICHAR_SOLDIER:
	case AICHAR_ELITEGUARD:
	case AICHAR_BLACKGUARD:
	case AICHAR_VENOM:
		return qtrue;
	default:
		return qfalse;
	}
}

static void G_VRX_DropWeapon( gentity_t *e, cast_state_t *cs ) {
	gitem_t *item = e->client->ps.weapon > WP_NONE ? BG_FindItemForWeapon( e->client->ps.weapon ) : NULL;
	vec3_t org, vel;

	if ( item ) {
		VectorCopy( e->r.currentOrigin, org );
		org[2] += 24;
		VectorSet( vel, crandom() * 80, crandom() * 80, 140 );
#ifdef VR_NET_HANDS
		LaunchItem( item, org, vel, e->s.number );
#else
		LaunchItem( item, org, vel );
#endif
	}
	memset( e->client->ps.weapons, 0, sizeof( e->client->ps.weapons ) );
	e->client->ps.weapon = WP_NONE;
	cs->weaponNum = WP_NONE;
}

static void G_VRX_Surrender( gentity_t *e, cast_state_t *cs ) {
	G_VRX_DropWeapon( e, cs );
	cs->noAttackTime = level.time + 3600000;    // for good
	cs->pauseTime = level.time + 3600000;       // stays where they are
	G_VRX_AddGoreBits( e, VRX_GB_SURRENDER );
}

static void G_VRX_HeadScare( vrxThrown_t *t, gentity_t *head ) {
	gentity_t *e, *thrower = &g_entities[t->thrower];
	cast_state_t *cs;
	vec3_t eye;
	trace_t tr;
	float dist;
	int i;

	if ( t->scared || head->s.dmgFlags != VRX_ENT_HEAD || !G_VRX( VRX_HEADSCARE ) ||
		 !thrower->client || thrower->aiCharacter ) {
		return;
	}
	t->scared = qtrue;
	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 || !G_VRX_Scareable( e ) ||
			 ( G_VRX_GoreBits( e ) & VRX_GB_SURRENDER ) ) {
			continue;
		}
		dist = Distance( e->r.currentOrigin, head->r.currentOrigin );
		if ( dist > VRX_SCARE_DIST ) {
			continue;
		}
		VectorCopy( e->r.currentOrigin, eye );
		eye[2] += e->client->ps.viewheight;
		trap_Trace( &tr, head->r.currentOrigin, NULL, NULL, eye, head->s.number, MASK_SOLID );
		if ( tr.fraction < 1.0f ) {
			continue;       // didn't see it
		}
		cs = AICast_GetCastState( i );
		if ( G_VRX( VRX_HEADSCARE ) >= 2 && ( dist < VRX_GIVEUP_DIST || cs->aiState < AISTATE_COMBAT ) ) {
			G_VRX_Surrender( e, cs );
			continue;
		}
		// run from it
		cs->dangerEntity = head->s.number;
		VectorCopy( head->r.currentOrigin, cs->dangerEntityPos );
		cs->dangerEntityValidTime = level.time + 6000;
		cs->dangerDist = VRX_SCARE_DIST * 1.5f;
		cs->dangerEntityTimestamp = level.time;
		G_VRX_AddGoreBits( e, VRX_GB_SCARED );
	}
}
