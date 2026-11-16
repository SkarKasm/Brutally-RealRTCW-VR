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
#include "g_survival.h"

static vmCvar_t vrxCvars[VRX_COUNT];
static vmCvar_t g_vrx;
static vmCvar_t g_bleedOut;      // wounds that bleed him out over time (G_VRX_Bleed)
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
	"g_voxelGore",
};
static qboolean vrxRegistered;

static void G_VRX_PropsFrame( void );
static void G_VRX_MarkersFrame( void );
static void G_VRX_ScreamFrame( void );
static void G_VRX_GhostFrame( void );
static gentity_t *G_VRX_Marker( gentity_t *body, qboolean create );
static gentity_t *G_VRX_SpawnHead( gentity_t *body, const vec3_t pos, gentity_t *by );
static void G_VRX_DropWeapon( gentity_t *e, cast_state_t *cs );
static void G_VRX_Disarm( gentity_t *targ );
static void G_VRX_DisarmFrame( void );
static void G_VRX_DisarmItemFree( int n );
static int vrxDisarmed[MAX_CLIENTS];        // the weapon he dropped (0: none)
static int vrxDisarmItem[MAX_CLIENTS];      // where it lies: its entity (0: none)
static int vrxPickupAt[MAX_CLIENTS];        // stooping for it: he has it then
static void G_VRX_SurvivalTest( void );
static void G_VRX_BleedFrame( void );
static void G_VRX_Bleed( gentity_t *targ, gentity_t *attacker, const vec3_t point, int region, int mod, qboolean headshot );

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
	trap_Cvar_Register( &g_bleedOut, "g_bleedOut", "1", CVAR_ARCHIVE );
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
	G_VRX_BleedFrame();
	G_VRX_ScreamFrame();
	G_VRX_GhostFrame();
	G_VRX_DisarmFrame();
	G_VRX_SurvivalTest();     // (test aid, cheats only)
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

// the older body gore (wounds, throats, guts, limbs off): not with the voxel gore, which
// does bodies itself
static qboolean G_VRX_OldGore( int which ) {
	return G_VRX_Gore( which ) && !G_VRX_Gore( VRX_VOXEL );
}

/*
=================
G_VRX_Dismember (g_dismember)

A blast (1), or a heavy hit too (2), as it kills someone or lands on a body: limbs off.
A blast takes one or two, close ones at the shoulder or hip; a heavy round takes the one
it hit, at the elbow or knee. Never on the living.
=================
*/
// rockets, mortars, dynamite, air strikes, exploding barrels and the like: not hand grenades
static qboolean G_VRX_HeavyBlast( int mod, gentity_t *inflictor ) {
	switch ( mod ) {
	case MOD_ROCKET:
	case MOD_ROCKET_SPLASH:
	case MOD_PANZERFAUST:
	case MOD_MORTAR:
	case MOD_MORTAR_SPLASH:
	case MOD_DYNAMITE:
	case MOD_DYNAMITE_SPLASH:
	case MOD_AIRSTRIKE:
	case MOD_EXPLOSIVE:
		return qtrue;
	case MOD_GRENADE:
	case MOD_GRENADE_SPLASH:
		// a barrel or crate going up (props_explosion*) is passed off as a grenade
		return inflictor && inflictor->classname && !Q_strncmp( inflictor->classname, "props_explosion", 15 );
	default:
		return qfalse;
	}
}

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
	if ( mod == MOD_KNIFE || mod == MOD_KNIFE2 || mod == MOD_KNIFE_STEALTH ) {
		return;     // a knife takes nothing off someone on his feet: it cuts where it lands (the cgame's CG_VRX_KnifeHit)
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
static qboolean vrxHitHeadTop;      // a head hit in its upper half: eyes and up (the lethal one)

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
	vec3_t pt[VRB_GROUPS * 4], d, u, a, b, up, right, fwd, groin, headUp, headC;
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
	VectorCopy( u, headUp );
	VectorCopy( a, headC );
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
	vrxHitHeadTop = qfalse;
	if ( region == VRH_HEAD ) {
		// the upper half of the head and face, its own hit box. The ball sits on the head bone
		// 3.5 up: the mouth is about 2.5 below its middle, the eyes just above it, the chin 5
		// below and the crown 6 above, so halfway down the face (the nose) is 1.25 below the
		// middle. Not where the bullet first touches the ball but where it passes through the
		// head, its path's nearest point to the middle: a shot from below into the eyes goes up
		// into the brain, though it touched the ball low
		vec3_t rel, mid;
		float t0;
		VectorSubtract( headC, start, rel );
		t0 = DotProduct( rel, d );
		VectorMA( start, t0, d, mid );
		VectorSubtract( mid, headC, rel );
		vrxHitHeadTop = DotProduct( rel, headUp ) >= -1.25f;
	}
	VectorCopy( hit, vrxHitPoint );
	return 1;
}

static void G_VRX_ShotTraceLoose( trace_t *tr, const vec3_t start, const vec3_t end, int passEnt );
static void G_VRX_ShotTraceBox( trace_t *tr, const vec3_t start, const vec3_t end, int passEnt, int mask );

// a bullet's trace, but through the gaps round a character's body
static qboolean vrxGhost[MAX_CLIENTS];      // down and not in the way (G_VRX_GhostFrame)
static qboolean G_VRX_GhostOnFeet( int i ); // a ghost because he's staggering, not because he's on the floor
static qboolean G_VRX_Staggering( int i );  // stunned by a hit and on his feet, the cgame's physics moving him

void G_VRX_ShotTrace( trace_t *tr, const vec3_t start, const vec3_t end, int passEnt, int mask ) {
	int i;

	// a man on the floor is shot where he's drawn (the loose trace below), never by his box:
	// it stands man-high where he fell, and the player can be standing in it now, so every
	// shot started inside it and hit him at the muzzle
	// (a man staggering on his feet is still shot by his box, which keeps up with him
	// (G_VRX_DownPos_f), unless the shot starts inside it)
	for ( i = 1; i < MAX_CLIENTS; i++ ) {
		if ( vrxGhost[i] && g_entities[i].r.contents == CONTENTS_CORPSE ) {
			gentity_t *g = &g_entities[i];
			if ( G_VRX_GhostOnFeet( i ) &&
				 !( start[0] > g->r.absmin[0] - 8 && start[0] < g->r.absmax[0] + 8 && start[1] > g->r.absmin[1] - 8 &&
					start[1] < g->r.absmax[1] + 8 && start[2] > g->r.absmin[2] - 8 && start[2] < g->r.absmax[2] + 8 ) ) {
				continue;
			}
			g_entities[i].r.contents = 0;
		} else {
			vrxGhost[i] = vrxGhost[i] && g_entities[i].r.contents == CONTENTS_CORPSE;
		}
	}
	G_VRX_ShotTraceBox( tr, start, end, passEnt, mask );
	for ( i = 1; i < MAX_CLIENTS; i++ ) {
		if ( vrxGhost[i] && g_entities[i].r.contents == 0 ) {
			g_entities[i].r.contents = CONTENTS_CORPSE;
		}
	}
	if ( mask & CONTENTS_BODY ) {
		G_VRX_ShotTraceLoose( tr, start, end, passEnt );
	}
}

// (as the box trace finds him, then through the gaps)
static void G_VRX_ShotTraceBox( trace_t *tr, const vec3_t start, const vec3_t end, int passEnt, int mask ) {
	vec3_t from, hit;
	float total = Distance( start, end );
	int k, r;

	vrxHitEnt = -1;
	trap_Trace( tr, (float *)start, NULL, NULL, (float *)end, passEnt, mask );
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
		if ( G_VRX_Staggering( e->s.number ) ) {
			// staggering: the cgame's physics has his body, not the animation these gaps were
			// measured on (he's bent over, leaning, an arm across him), and shots at him went
			// through. So for him the box counts, the middle of it: within a body's width of
			// his upright axis
			float dx = e->r.currentOrigin[0] - start[0], dy = e->r.currentOrigin[1] - start[1];
			float ex = end[0] - start[0], ey = end[1] - start[1], len = sqrt( ex * ex + ey * ey );
			if ( len < 1.0f || fabs( dx * ey - dy * ex ) / len < 11.0f ) {
				vrxHitEnt = -1;     // (where on him: not known)
				return;
			}
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

// the shot that hit targ at point went into the upper half of his head (eyes and up): -1 not known
int G_VRX_HitHeadTop( gentity_t *targ, const vec3_t point ) {
	if ( G_VRX_HitRegion( targ, point ) != VRH_HEAD ) {
		return G_VRX_HitRegion( targ, point ) == VRH_NONE ? -1 : 0;
	}
	return vrxHitHeadTop ? 1 : 0;
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
	return mod == MOD_M97 || mod == MOD_AUTO5 || mod == MOD_M30 || mod == MOD_SHOTGUN;
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

// who a shot to the head always kills: people (not the bosses, the dogs, the lopers)
static qboolean G_VRX_HeadshotKills( gentity_t *e ) {
	switch ( e->aiCharacter ) {
	case AICHAR_SUPERSOLDIER: case AICHAR_SUPERSOLDIER_LAB: case AICHAR_PROTOSOLDIER: case AICHAR_HELGA:
	case AICHAR_HEINRICH: case AICHAR_LOPER: case AICHAR_LOPER_SPECIAL: case AICHAR_DOG:
		return qfalse;
	default:
		return qtrue;
	}
}

// the bosses (Helga, Heinrich, the Uebersoldaten): no hit physics on them while they live (no
// stagger, stun or knockdown, no dropped gun) and no bleeding out: they fight on till they die
static qboolean G_VRX_Boss( gentity_t *e ) {
	switch ( e->aiCharacter ) {
	case AICHAR_SUPERSOLDIER: case AICHAR_SUPERSOLDIER_LAB: case AICHAR_PROTOSOLDIER: case AICHAR_HELGA:
	case AICHAR_HEINRICH:
		return qtrue;
	default:
		return qfalse;
	}
}

// crippled, given up, or nearly dead
static qboolean G_VRX_Downed( gentity_t *e ) {
	int max;

	if ( !e->client || !e->aiCharacter || e->health <= 0 ) {
		return qfalse;
	}
	return ( G_VRX_GoreBits( e ) & ( VRX_GB_SURRENDER | VRX_GB_CRIPPLED | VRX_GB_DOWNED ) ) != 0;
}

// a body shot that leaves someone nearly dead puts them down, writhing in agony; which of the
// SoF2 animations (cgame CG_VRX_Downed) goes by where the shot went in:
// 1 gut, long writhe on his back   2 gut, to his knees looking at his bloody hand
// 3 chest, writhing onto his front  4/5 left/right leg, writhing on his back
// 6/7 left/right arm, writhing on his side   8 slumped, sitting, helpless
#define VRX_DOWN_GRACE  10000   // ms after going down that stray fire can't finish him
#define VRX_DOWN_MS     2200    // knocked down: on the floor this long (plus up to VRX_DOWN_RAND)...
#define VRX_DOWN_RAND   1500
#define VRX_GETUP_MS    3400    // ...then getting back up (the cgame's get-up, RD_GETUP_MS), still not fighting
#define VRX_DOWN_AGAIN  2500    // after getting up, not knocked straight back down
static int vrxDownedAt[MAX_GENTITIES];
static float vrxDownedYaw[MAX_GENTITIES];
static int vrxGetUpAt[MAX_GENTITIES];       // knocked down, gets up then (0: stays down, a leg off)
static int vrxUpAt[MAX_GENTITIES];          // got up then
static int vrxGetUpHold[MAX_GENTITIES];     // held facing the way the cgame's get-up turned him, until then
#define VRX_STUN_MS     1100    // shot and not down: stunned, shoved about by the bullets, this long (plus up to VRX_STUN_RAND)
#define VRX_STUN_RAND   500
#define VRX_STUN_MAX    5000    // however many more hits: he comes to after this long
static int vrxStunUntil[MAX_GENTITIES];
static int vrxStaggerUntil[MAX_GENTITIES];  // the stun's over, but the cgame has him still staggering ("vrstagger")
static int vrxStunStart[MAX_GENTITIES];
static int vrxKnifedAlive[MAX_GENTITIES];
static qboolean vrxFightBack[MAX_GENTITIES];  // stunned by a chest shot: fights back as he staggers (G_VRX_HoldStunned)   // a knife went into him while he was alive, then (G_VRX_Decap_f)

/*
=================
G_VRX_Scream

A limb gone, alive: the burning man's scream (the character's flame death), now and every few
seconds while he lasts (G_VRX_ScreamFrame).
=================
*/
static int vrxNextScream[MAX_GENTITIES];

static void G_VRX_Scream( gentity_t *ent ) {
	const char *snd;

	if ( !ent->client || !ent->aiCharacter || ent->health <= 0 ) {
		return;
	}
	snd = aiDefaults[ent->aiCharacter].soundScripts[FLAMEDEATHSOUNDSCRIPT];
	if ( !snd || !snd[0] ) {
		snd = aiDefaults[ent->aiCharacter].soundScripts[PAINSOUNDSCRIPT];
	}
	if ( snd && snd[0] ) {
		G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( (char *)snd ) );
	}
	vrxNextScream[ent->s.number] = level.time + 2600 + rand() % 2200;
}

/*
=================
G_VRX_GhostFrame

A man the physics has (staggering from a hit, knocked down and alive, or still getting up) isn't in the way: his box
stands where he fell, as tall as a man, and the player walked into thin air above him. So
down, he's like a corpse to movement (shots still find him, G_VRX_ShotTrace), and he's solid
again once he's up and nobody is standing in him.
=================
*/
static qboolean G_VRX_Staggering( int i ) {
	if ( i <= 0 || i >= MAX_CLIENTS || !G_VRX_GhostOnFeet( i ) ) {
		return qfalse;
	}
	return level.time < vrxStunUntil[i] + 300 || level.time < vrxStaggerUntil[i] + 300;
}

static qboolean G_VRX_GhostOnFeet( int i ) {
	gentity_t *e = &g_entities[i];
	return !( G_VRX_GoreBits( e ) & VRX_GB_DOWNED ) && !( vrxUpAt[i] && level.time < vrxUpAt[i] + VRX_GETUP_MS );
}

static void G_VRX_GhostFrame( void ) {
	int i, j;

	for ( i = 1; i < MAX_CLIENTS && i < level.maxclients; i++ ) {
		gentity_t *e = &g_entities[i];
		qboolean down;

		if ( !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 ) {
			vrxGhost[i] = qfalse;       // (dead: the game's own corpse now)
			continue;
		}
		// (and staggering from a hit: the cgame's physics has him anywhere round where his box stands)
		down = ( G_VRX_GoreBits( e ) & VRX_GB_DOWNED ) || ( vrxUpAt[i] && level.time < vrxUpAt[i] + VRX_GETUP_MS ) ||
			   level.time < vrxStunUntil[i] + 300 || level.time < vrxStaggerUntil[i] + 300;
		if ( down ) {
			if ( e->r.contents == CONTENTS_BODY ) {
				e->r.contents = CONTENTS_CORPSE;
				vrxGhost[i] = qtrue;
				trap_LinkEntity( e );       // (the player's own prediction goes by what's linked)
			}
			continue;
		}
		if ( !vrxGhost[i] ) {
			continue;
		}
		if ( e->r.contents != CONTENTS_CORPSE ) {
			vrxGhost[i] = qfalse;       // (something else changed it since)
			continue;
		}
		for ( j = 0; j < MAX_CLIENTS && j < level.maxclients; j++ ) {
			gentity_t *o = &g_entities[j];
			if ( j == i || !o->inuse || !o->client || o->health <= 0 || o->r.contents != CONTENTS_BODY ) {
				continue;
			}
			if ( o->r.absmin[0] < e->r.absmax[0] && o->r.absmax[0] > e->r.absmin[0] &&
				 o->r.absmin[1] < e->r.absmax[1] && o->r.absmax[1] > e->r.absmin[1] &&
				 o->r.absmin[2] < e->r.absmax[2] && o->r.absmax[2] > e->r.absmin[2] ) {
				break;      // someone's standing in him: not yet
			}
		}
		if ( j >= MAX_CLIENTS || j >= level.maxclients ) {
			e->r.contents = CONTENTS_BODY;
			vrxGhost[i] = qfalse;
			trap_LinkEntity( e );
		}
	}
}

static void G_VRX_ScreamFrame( void ) {
	int i;

	for ( i = 1; i < level.maxclients; i++ ) {
		gentity_t *e = &g_entities[i];
		if ( !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 || level.time < vrxNextScream[i] ||
			 !( G_VRX_GoreBits( e ) & ( VRX_GB_LARM | VRX_GB_RARM | VRX_GB_LLEG | VRX_GB_RLEG ) ) ) {
			continue;
		}
		G_VRX_Scream( e );
	}
}

// knocked off his feet (the cgame's physics: he staggers, falls, and lies there); with his
// legs he gets back up after a few seconds (permanent: a leg off, down for good, bleeding)
static void G_VRX_KnockDown( gentity_t *targ, gentity_t *attacker, int region, qboolean permanent ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m;
	int style, n = targ->s.number, down = VRX_DOWN_MS + rand() % VRX_DOWN_RAND;

	switch ( region ) {
	case VRH_BELLY: style = random() < 0.6f ? 1 : 2; break;
	case VRH_CHEST: style = random() < 0.6f ? 3 : 1; break;
	case VRH_LLEG:  style = 4; break;
	case VRH_RLEG:  style = 5; break;
	case VRH_LARM:  style = 6; break;
	case VRH_RARM:  style = 7; break;
	default:        { static const int any[4] = { 1, 2, 3, 8 }; style = any[rand() % 4]; } break;
	}
	G_VRX_AddGoreBits( targ, VRX_GB_DOWNED );
	m = G_VRX_Marker( targ, qtrue );
	m->s.time2 = style;       // how he goes down (cgame CG_VRX_Downed)
	m->count = attacker->s.number;
	m->timestamp = level.time + 900;
	vrxDownedAt[n] = level.time;
	vrxDownedYaw[n] = targ->client->ps.viewangles[YAW];
	vrxGetUpAt[n] = permanent ? 0 : level.time + down;
	vrxStunUntil[n] = 0;
	vrxStaggerUntil[n] = 0;
	if ( cs ) {
		if ( permanent ) {
			G_VRX_DropWeapon( targ, cs );
		}
		cs->noAttackTime = level.time + ( permanent ? 3600000 : down + VRX_GETUP_MS );
		cs->pauseTime = level.time + ( permanent ? 3600000 : down + VRX_GETUP_MS );
	}
	VectorClear( targ->client->ps.velocity );
}

/*
=================
G_VRX_DownPos_f

"vrdownpos <entity> <x> <y>" from the cgame: someone knocked down has landed there (his
ragdoll fell away from where he was hit). His body is moved there, as far as it fits, so he
gets up where he lies instead of sliding back.
=================
*/
void G_VRX_DownPos_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *targ;
	trace_t tr;
	vec3_t to;
	int n;

	if ( trap_Argc() < 4 || !ent->client ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	if ( n <= 0 || n >= MAX_CLIENTS ) {
		return;
	}
	targ = &g_entities[n];
	if ( !targ->inuse || !targ->client || !targ->aiCharacter || targ->health <= 0 ||
		 !( ( vrxGetUpAt[n] && ( G_VRX_GoreBits( targ ) & VRX_GB_DOWNED ) ) || vrxStunUntil[n] + 1500 > level.time ||
			vrxStaggerUntil[n] + 1500 > level.time ) ) {
		return;
	}
	trap_Argv( 2, arg, sizeof( arg ) );
	to[0] = atof( arg );
	trap_Argv( 3, arg, sizeof( arg ) );
	to[1] = atof( arg );
	to[2] = targ->client->ps.origin[2];
	if ( Distance( to, targ->client->ps.origin ) > 128.0f ) {
		return;
	}
	// (the world only: the player may be standing in him, he's not in the way now. And as a
	// man walks: up a step's height, across, and back down, so a kerb or a stair doesn't stop
	// it short and leave his body behind, to snap back to when he comes to)
	{
		vec3_t up, across, dn;
		VectorCopy( targ->client->ps.origin, up );
		up[2] += 18.0f;
		trap_Trace( &tr, targ->client->ps.origin, targ->r.mins, targ->r.maxs, up, n, MASK_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP );
		if ( tr.startsolid || tr.allsolid ) {
			return;
		}
		VectorCopy( tr.endpos, up );
		VectorCopy( to, across );
		across[2] = up[2];
		trap_Trace( &tr, up, targ->r.mins, targ->r.maxs, across, n, MASK_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP );
		if ( tr.startsolid || tr.allsolid ) {
			return;
		}
		VectorCopy( tr.endpos, across );
		VectorCopy( across, dn );
		dn[2] -= 18.0f + 24.0f;
		trap_Trace( &tr, across, targ->r.mins, targ->r.maxs, dn, n, MASK_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP );
		if ( tr.startsolid || tr.allsolid ) {
			return;
		}
		if ( tr.fraction >= 1.0f ) {
			return;     // nothing under him there (a ledge): his body stays on this side of it
		}
	}
	VectorCopy( tr.endpos, targ->client->ps.origin );
	VectorCopy( tr.endpos, targ->r.currentOrigin );
	VectorCopy( tr.endpos, targ->s.pos.trBase );
	trap_LinkEntity( targ );
}

/*
=================
G_VRX_GetUpYaw_f

"vrgetupyaw <entity> <yaw>" from the cgame: getting up off the floor, he comes up facing the
way he lay (pushing up off his front, or sitting up and rolling onto a knee), so his body
turns that way and stays so until he's up, and his animation takes over facing the same way
=================
*/
void G_VRX_GetUpYaw_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *targ;
	vec3_t ang;
	int n;

	if ( trap_Argc() < 3 || !ent->client ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	if ( n <= 0 || n >= MAX_CLIENTS ) {
		return;
	}
	targ = &g_entities[n];
	if ( !targ->inuse || !targ->client || !targ->aiCharacter || targ->health <= 0 ||
		 !( ( G_VRX_GoreBits( targ ) & VRX_GB_DOWNED ) || ( vrxUpAt[n] && level.time < vrxUpAt[n] + 2500 ) ) ) {
		return;
	}
	{
		cast_state_t *cs = AICast_GetCastState( n );
		if ( cs ) {     // (the get-up from now: the cgame may have waited for him to lie still)
			if ( cs->noAttackTime < level.time + VRX_GETUP_MS ) {
				cs->noAttackTime = level.time + VRX_GETUP_MS;
			}
			if ( cs->pauseTime < level.time + VRX_GETUP_MS ) {
				cs->pauseTime = level.time + VRX_GETUP_MS;
			}
		}
	}
	trap_Argv( 2, arg, sizeof( arg ) );
	vrxDownedYaw[n] = AngleNormalize360( atof( arg ) );
	vrxGetUpHold[n] = level.time + VRX_GETUP_MS - 400;
	VectorSet( ang, 0, vrxDownedYaw[n], 0 );
	SetClientViewAngle( targ, ang );
	if ( trap_Argc() >= 5 ) {
		// and where the get-up animation leaves him standing: his body goes there, as far as it fits
		trace_t tr;
		vec3_t to;
		trap_Argv( 3, arg, sizeof( arg ) );
		to[0] = atof( arg );
		trap_Argv( 4, arg, sizeof( arg ) );
		to[1] = atof( arg );
		to[2] = targ->client->ps.origin[2];
		if ( Distance( to, targ->client->ps.origin ) <= 96.0f ) {
			trap_Trace( &tr, targ->client->ps.origin, targ->r.mins, targ->r.maxs, to, n, MASK_PLAYERSOLID );
			if ( !tr.startsolid && !tr.allsolid ) {
				VectorCopy( tr.endpos, targ->client->ps.origin );
				VectorCopy( tr.endpos, targ->r.currentOrigin );
				VectorCopy( tr.endpos, targ->s.pos.trBase );
				trap_LinkEntity( targ );
			}
		}
	}
}

/*
=================
G_VRX_Stun

Shot and still on his feet: for a moment he doesn't fight or move, he's shocked, and the
cgame's physics has him stagger about under the bullets, clutching where he was hit (the
marker's time2 VRX_STUN_MARK). Each hit keeps it going, up to VRX_STUN_MAX.
=================
*/
// stunned or staggering till then: held where he is. Shot in the chest he fights back, aiming
// and firing as he reels (the cgame holds his gun on you, cg_ragdoll.c HR_Aim); he only can't
// walk. Otherwise he doesn't fight at all till he's steady
static void G_VRX_HoldStunned( cast_state_t *cs, int until, qboolean fightBack ) {
	if ( !cs ) {
		return;
	}
	if ( fightBack ) {
		if ( cs->castScriptStatus.scriptNoMoveTime < until ) {
			cs->castScriptStatus.scriptNoMoveTime = until;
		}
		return;
	}
	if ( cs->noAttackTime < until ) {
		cs->noAttackTime = until;
	}
	if ( cs->pauseTime < until ) {
		cs->pauseTime = until;
	}
}

static void G_VRX_Stun( gentity_t *targ, gentity_t *attacker, qboolean fightBack ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m;
	int n = targ->s.number, until;

	if ( !targ->client || !targ->aiCharacter || targ->health <= 0 || n >= MAX_CLIENTS || G_VRX_Downed( targ ) ||
		 !VRX_HUMAN_MALE( targ->aiCharacter ) || !G_VRX( VRX_REACTIONS ) ) {
		return;
	}
	if ( vrxStunUntil[n] <= level.time ) {
		vrxStunStart[n] = level.time;
	}
	until = level.time + VRX_STUN_MS + rand() % VRX_STUN_RAND;
	if ( until > vrxStunStart[n] + VRX_STUN_MAX ) {
		until = vrxStunStart[n] + VRX_STUN_MAX;
	}
	if ( until < vrxStunUntil[n] ) {
		until = vrxStunUntil[n];
	}
	vrxStunUntil[n] = until;
	m = G_VRX_Marker( targ, qtrue );
	m->s.time2 = VRX_STUN_MARK;
	if ( attacker ) {
		m->count = attacker->s.number;
	}
	vrxDownedYaw[n] = targ->client->ps.viewangles[YAW];
	vrxFightBack[n] = fightBack;
	G_VRX_HoldStunned( cs, until + 300, fightBack );
	VectorClear( targ->client->ps.velocity );
}

/*
=================
G_VRX_Fell_f

"vrfell <entity>" from the cgame: someone stunned was shoved over (his ragdoll tipped too far
to catch himself): knocked down, and he'll get up again
=================
*/
void G_VRX_Fell_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *targ;
	int n;

	if ( trap_Argc() < 2 || !ent->client ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	if ( n <= 0 || n >= MAX_CLIENTS ) {
		return;
	}
	targ = &g_entities[n];
	if ( !targ->inuse || !targ->client || !targ->aiCharacter || targ->health <= 0 ||
		 ( vrxStunUntil[n] + 1000 < level.time && vrxStaggerUntil[n] + 1000 < level.time ) || G_VRX_Downed( targ ) ) {
		return;
	}
	vrxStaggerUntil[n] = 0;
	G_VRX_KnockDown( targ, ent, VRH_NONE, qfalse );
}

/*
=================
G_VRX_Stagger_f

"vrstagger <entity>" from the cgame, every quarter second: his stun is over, but he's still
staggering, not yet steady on his feet. He's held where he is, not fighting, until he is (or
the cgame gives up on him, 3 s at most)
=================
*/
void G_VRX_Stagger_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *targ;
	cast_state_t *cs;
	int n, until;

	if ( trap_Argc() < 2 || !ent->client ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	if ( n <= 0 || n >= MAX_CLIENTS ) {
		return;
	}
	targ = &g_entities[n];
	if ( !targ->inuse || !targ->client || !targ->aiCharacter || targ->health <= 0 || G_VRX_Downed( targ ) ||
		 vrxStunUntil[n] + 3000 < level.time ) {
		return;
	}
	until = level.time + 500;
	if ( until > vrxStunUntil[n] + 3000 ) {
		until = vrxStunUntil[n] + 3000;
	}
	if ( until > vrxStaggerUntil[n] ) {
		vrxStaggerUntil[n] = until;
	}
	cs = AICast_GetCastState( n );
	G_VRX_HoldStunned( cs, until + 300, vrxFightBack[n] );
}

// back on his feet (G_VRX_MarkersFrame): the cgame stands him up, then he fights on
static void G_VRX_GetUp( gentity_t *body, gentity_t *m ) {
	cast_state_t *cs = AICast_GetCastState( body->s.number );

	m->s.density &= ~VRX_GB_DOWNED;
	vrxGetUpAt[body->s.number] = 0;
	vrxUpAt[body->s.number] = level.time;
	vrxGetUpHold[body->s.number] = level.time + VRX_GETUP_MS + 2000;     // (the cgame may wait for him to come to rest first)
	if ( cs ) {
		cs->noAttackTime = level.time + VRX_GETUP_MS;
		cs->pauseTime = level.time + VRX_GETUP_MS;
	}
}

// an arm off someone alive: no gun, running, bleeding
// (event: the cgame cuts the arm off at the shoulder or elbow; not when its voxel gore shot it off itself)
static void G_VRX_Cripple( gentity_t *targ, gentity_t *attacker, int arm, const vec3_t dir, const vec3_t point, qboolean event ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gentity_t *m, *te;
	int high = random() < 0.4f ? arm : 0;

	G_VRX_AddGoreBits( targ, arm | VRX_GB_CRIPPLED );
	G_VRX_Scream( targ );
	m = G_VRX_Marker( targ, qtrue );
	m->s.frame |= high;
	m->count = attacker->s.number;      // who he runs from (and who gets him when he bleeds out)
	m->timestamp = level.time + 700;    // the first bleed

	if ( event ) {
		te = G_TempEntity( (float *)point, EV_VR_GORE );
		te->s.eventParm = VRG_LIMBS;
		te->s.otherEntityNum = targ->s.number;
		te->s.density = arm;
		te->s.frame = high;
		if ( dir ) {
			VectorCopy( dir, te->s.origin2 );
		}
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

// the leg off someone alive (the voxel gore): down on the ground writhing and screaming, bleeding
// out, as SoF2's agony (cgame CG_VRX_Downed) with the leg cut through (cg_vrgore.c)
static void G_VRX_LegOff( gentity_t *targ, gentity_t *attacker, int leg, const vec3_t dir, const vec3_t point, qboolean event ) {
	gentity_t *te;
	int high = random() < 0.4f ? leg : 0;

	G_VRX_AddGoreBits( targ, leg );
	G_VRX_Marker( targ, qtrue )->s.frame |= high;
	G_VRX_KnockDown( targ, attacker, leg == VRX_GB_LLEG ? VRH_LLEG : VRH_RLEG, qtrue );
	if ( event ) {
		te = G_TempEntity( (float *)point, EV_VR_GORE );
		te->s.eventParm = VRG_LIMBS;
		te->s.otherEntityNum = targ->s.number;
		te->s.density = leg;
		te->s.frame = high;
		if ( dir ) {
			VectorCopy( dir, te->s.origin2 );
		}
	}
	G_VRX_Scream( targ );
}

/*
=================
G_VRX_Severed_f

"vrsever <entity> <limbs>" from the cgame: its voxel gore shot a hand, an arm, a foot or a leg
clean off someone still alive (cg_vrgore.c VRG_Sever). An arm: crippled (no gun, running,
bleeding out); a leg: down writhing. The cgame has already taken the part off itself.
=================
*/
void G_VRX_Severed_f( gentity_t *ent ) {
	char arg[MAX_TOKEN_CHARS];
	gentity_t *targ;
	int n, limbs, have;
	vec3_t dir;

	if ( trap_Argc() < 3 || !ent->client ) {
		return;
	}
	trap_Argv( 1, arg, sizeof( arg ) );
	n = atoi( arg );
	trap_Argv( 2, arg, sizeof( arg ) );
	limbs = atoi( arg ) & ( VRX_GB_LARM | VRX_GB_RARM | VRX_GB_LLEG | VRX_GB_RLEG );
	if ( n <= 0 || n >= MAX_CLIENTS || !limbs ) {
		return;
	}
	targ = &g_entities[n];
	if ( !targ->inuse || !targ->client || !targ->aiCharacter || targ->health <= 0 || targ == ent ||
		 Distance( targ->r.currentOrigin, ent->r.currentOrigin ) > 4096 ) {
		return;
	}
	have = G_VRX_GoreBits( targ );
	VectorSubtract( targ->r.currentOrigin, ent->r.currentOrigin, dir );
	VectorNormalize( dir );
	if ( ( limbs & ( VRX_GB_LARM | VRX_GB_RARM ) ) && !( have & ( VRX_GB_LARM | VRX_GB_RARM ) ) ) {
		G_VRX_Cripple( targ, ent, ( limbs & VRX_GB_RARM ) ? VRX_GB_RARM : VRX_GB_LARM, dir, targ->r.currentOrigin, qfalse );
	} else if ( limbs & ( VRX_GB_LARM | VRX_GB_RARM ) ) {
		G_VRX_AddGoreBits( targ, limbs & ( VRX_GB_LARM | VRX_GB_RARM ) );
	}
	if ( ( limbs & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) && !( have & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ) {
		G_VRX_LegOff( targ, ent, ( limbs & VRX_GB_RLEG ) ? VRX_GB_RLEG : VRX_GB_LLEG, dir, targ->r.currentOrigin, qfalse );
	} else if ( limbs & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) {
		G_VRX_AddGoreBits( targ, limbs & ( VRX_GB_LLEG | VRX_GB_RLEG ) );
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
	G_VRX_KnockDown( targ, by, VRH_NONE, qfalse );
	if ( style >= 1 && style <= VRX_DOWN_STYLES ) {
		G_VRX_Marker( targ, qtrue )->s.time2 = style;
	}
}

// finishing someone downed on purpose: the player's knife, a kick, or a shot to the head
// from close by (a body shot, close or not, is just more pain)
static qboolean G_VRX_Finisher( gentity_t *targ, gentity_t *attacker, const vec3_t dir, const vec3_t point, int mod,
								qboolean headshot ) {
	if ( !attacker || !attacker->client || attacker->aiCharacter ) {
		return qfalse;
	}
	if ( G_VRX_Knife( mod ) || mod == MOD_KICKED ) {
		return qtrue;
	}
	return Distance( attacker->r.currentOrigin, targ->r.currentOrigin ) < 200.0f &&
		   ( headshot || ( point && G_VRX_ShotAtHead( targ, dir, point ) ) );
}

// the downed stay where they fell: no walking, no sliding, no turning round (ClientThink_real)
void G_VRX_HoldDowned( gentity_t *ent, usercmd_t *ucmd ) {
	vec3_t ang;

	if ( !ent->client || !ent->aiCharacter || ent->health <= 0 ||
		 ( !( G_VRX_GoreBits( ent ) & VRX_GB_DOWNED ) && vrxStunUntil[ent->s.number] <= level.time &&
		   vrxStaggerUntil[ent->s.number] <= level.time &&
		   ( level.time >= vrxGetUpHold[ent->s.number] || vrxGetUpHold[ent->s.number] - level.time > VRX_GETUP_MS + 2500 ) ) ) {
		return;
	}
	ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
	ucmd->buttons &= ~( BUTTON_ATTACK | BUTTON_WALKING );
	ucmd->wbuttons = 0;
	ent->client->ps.velocity[0] = ent->client->ps.velocity[1] = 0;
	if ( ent->client->ps.velocity[2] > 0 ) {
		ent->client->ps.velocity[2] = 0;
	}
	VectorSet( ang, 0, vrxDownedYaw[ent->s.number], 0 );
	SetClientViewAngle( ent, ang );
}

/*
=================
G_VRX_ShotTraceLoose

Someone on the floor, getting up, or staggering is drawn by the cgame's physics, and his body
reaches well outside his bounding box (lying, his head and legs are a man's length from where
the box stands), so a shot at those parts never met the box and went through him: he looked
invincible. So for them the shot is also tried against the body as drawn, and hits it if it
gets there before anything else.
=================
*/
static void G_VRX_ShotTraceLoose( trace_t *tr, const vec3_t start, const vec3_t end, int passEnt ) {
	int i, bestEnt = -1, saveEnt = vrxHitEnt, saveRegion = vrxHitRegion;
	qboolean saveTop = vrxHitHeadTop;
	float total = Distance( start, end ), best = tr->fraction;
	vec3_t hit, bestHit, savePoint, d;

	if ( total < 1.0f ) {
		return;
	}
	VectorCopy( vrxHitPoint, savePoint );
	VectorClear( bestHit );
	VectorSubtract( end, start, d );
	VectorScale( d, 1.0f / total, d );
	for ( i = 1; i < MAX_CLIENTS && i < level.maxclients; i++ ) {
		gentity_t *e = &g_entities[i];
		vec3_t to, near;
		float t;
		if ( i == passEnt || !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 || !e->takedamage ) {
			continue;
		}
		if ( !( G_VRX_GoreBits( e ) & VRX_GB_DOWNED ) && !( vrxUpAt[i] && level.time < vrxUpAt[i] + VRX_GETUP_MS + 2500 ) &&
			 vrxStunUntil[i] + 500 < level.time && vrxStaggerUntil[i] + 500 < level.time ) {
			continue;       // on his feet as his box is
		}
		// (only if the shot passes near him at all)
		VectorSubtract( e->r.currentOrigin, start, to );
		t = DotProduct( to, d );
		t = t < 0 ? 0 : t > total ? total : t;
		VectorMA( start, t, d, near );
		if ( Distance( near, e->r.currentOrigin ) > 90.0f ) {
			continue;
		}
		if ( G_VRX_BodyTrace( e, start, end, hit ) > 0 && Distance( start, hit ) / total < best ) {
			best = Distance( start, hit ) / total;
			bestEnt = i;
			VectorCopy( hit, bestHit );
		}
	}
	if ( bestEnt < 0 ) {
		vrxHitEnt = saveEnt;        // (as the box trace left them)
		vrxHitRegion = saveRegion;
		vrxHitHeadTop = saveTop;
		VectorCopy( savePoint, vrxHitPoint );
		return;
	}
	G_VRX_BodyTrace( &g_entities[bestEnt], start, end, hit );   // (its region, for G_VRX_HitRegion)
	tr->fraction = best;
	VectorCopy( bestHit, tr->endpos );
	tr->entityNum = bestEnt;
	tr->surfaceFlags = 0;
	tr->contents = CONTENTS_BODY;
	tr->startsolid = tr->allsolid = qfalse;
	VectorNegate( d, tr->plane.normal );
}

int G_VRX_Hit( gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, const vec3_t dir, const vec3_t point,
			   int mod, int take, qboolean headshot ) {
	vec3_t ang, right, rel;
	float side;
	int region;

	vrxExecution = qfalse;
	// (a knife into someone still alive: the head can't come off for this blow, even if it kills
	// him, only once he's lying dead, G_VRX_Decap_f)
	if ( targ->client && targ->aiCharacter && targ->health > 0 && G_VRX_Knife( mod ) && targ->s.number < MAX_CLIENTS ) {
		vrxKnifedAlive[targ->s.number] = level.time;
	}
	// a shot into the upper half of the head (eyes and up) kills, whatever the gun and however far
	// (a person, not a monster); lower, the face and jaw, it's a wound (no tight boxes: the stock headshot)
	if ( targ->client && targ->aiCharacter && targ->health > 0 && take > 0 && point && attacker && attacker->client &&
		 !attacker->aiCharacter && attacker != targ && mod != MOD_UNKNOWN && mod != MOD_KICKED && !G_VRX_Blast( mod ) &&
		 !G_VRX_Knife( mod ) && G_VRX_HeadshotKills( targ ) &&
		 ( G_VRX_HitHeadTop( targ, point ) == 1 || ( headshot && G_VRX_HitHeadTop( targ, point ) < 0 ) ) ) {
		return targ->health + 50;
	}
	// the downed, writhing on the floor, aren't killed by stray fire: shots from further off
	// do half, and for the first seconds none can finish him (so he's seen going down); a
	// blast, or finishing him on purpose (knife, kick, a shot from close by), still does
	if ( targ->client && targ->aiCharacter && targ->health > 0 && take > 0 && mod != MOD_UNKNOWN && !vrxGetUpAt[targ->s.number] &&
		 G_VRX_Downed( targ ) && !G_VRX_Blast( mod ) && !G_VRX_Finisher( targ, attacker, dir, point, mod, headshot ) ) {
		// (only someone down for good, a leg off: knocked down, a shot kills as ever)
		take = take > 3 ? take / 4 : 1;
		if ( take >= targ->health && level.time < vrxDownedAt[targ->s.number] + VRX_DOWN_GRACE ) {
			take = targ->health - 1;
		}
		if ( take <= 0 ) {
			return 0;
		}
	}
	// a hit that doesn't kill or blow him off his feet: stunned, shoved about by the bullets
	if ( targ->client && targ->aiCharacter && targ->health > 0 && take > 0 && take < targ->health && mod != MOD_UNKNOWN &&
		 !G_VRX_Blast( mod ) && attacker && attacker->client && attacker != targ && !G_VRX_Boss( targ ) ) {
		G_VRX_Stun( targ, attacker, point && G_VRX_HitRegion( targ, point ) == VRH_CHEST );
		// shot in the arm he holds it with: it drops from his hand, and he picks it up again once
		// he's himself (G_VRX_DisarmFrame)
		if ( point && G_VRX_HitRegion( targ, point ) == VRH_RARM ) {
			G_VRX_Disarm( targ );
		}
	}
	// somewhere that bleeds: he keeps bleeding, and bleeds out in the end unless finished first
	if ( targ->client && targ->aiCharacter && targ->health > 0 && take > 0 && take < targ->health && mod != MOD_UNKNOWN && point &&
		 !G_VRX_Boss( targ ) ) {
		G_VRX_Bleed( targ, attacker, point, G_VRX_HitRegion( targ, point ), mod, headshot );
	}
	if ( mod == MOD_UNKNOWN || !targ->client || !targ->aiCharacter || targ->health <= 0 || take <= 0 || !attacker || !attacker->client ||
		 attacker->aiCharacter || !G_VRX_Gore( VRX_DISMEMBER ) || G_VRX( VRX_DISMEMBER ) < 2 ||
		 !VRX_HUMAN_MALE( targ->aiCharacter ) ) {
		return take;
	}
	// finishing the downed: a knife, a kick, or any shot from close by
	if ( G_VRX_Downed( targ ) && G_VRX_Finisher( targ, attacker, dir, point, mod, headshot ) ) {
		vrxExecution = qtrue;
		return targ->health + 50;
	}
	// where the tight hit boxes say it landed (VRH_NONE: not known, go by height)
	region = point ? G_VRX_HitRegion( targ, point ) : VRH_NONE;
	if ( region == VRH_GROIN && take < targ->health && !G_VRX_Downed( targ ) ) {
		G_VRX_GroinHit( targ, attacker );
		return take;
	}
	// (no dice to knock him down any more: hit, he staggers and fights to keep his feet, and
	// only goes down when he has to: a leg off, bleeding out, or dead)
	if ( headshot || !point || take >= targ->health || !G_VRX_Heavy( targ, attacker, mod, take ) ||
		 G_VRX_ShotAtHead( targ, dir, point ) ) {
		return take;
	}
	if ( region == VRH_LLEG || region == VRH_RLEG ) {
		if ( G_VRX_Gore( VRX_VOXEL ) ) {
			if ( !( G_VRX_GoreBits( targ ) & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) && random() < 0.5f ) {
				G_VRX_LegOff( targ, attacker, region == VRH_LLEG ? VRX_GB_LLEG : VRX_GB_RLEG, dir, point, qtrue );
			}
			return take < targ->health ? take : targ->health - 1;
		}
		return random() < 0.5f ? targ->health + 50 : take;
	}
	if ( region == VRH_LARM || region == VRH_RARM ) {
		if ( !( G_VRX_GoreBits( targ ) & ( VRX_GB_LARM | VRX_GB_RARM ) ) && random() < 0.6f ) {
			G_VRX_Cripple( targ, attacker, region == VRH_RARM ? VRX_GB_RARM : VRX_GB_LARM, dir, point, qtrue );
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
			if ( G_VRX_Gore( VRX_VOXEL ) ) {        // the leg goes, and he goes down screaming
				if ( !( G_VRX_GoreBits( targ ) & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ) {
					G_VRX_LegOff( targ, attacker, side > 0 ? VRX_GB_RLEG : VRX_GB_LLEG, dir, point, qtrue );
				}
				return take < targ->health ? take : targ->health - 1;
			}
			return targ->health + 50;   // the leg goes, and he goes down with it (G_VRX_Dismember)
		}
	} else if ( fabs( side ) > 6.0f && point[2] < targ->r.currentOrigin[2] + targ->client->ps.viewheight - 8 &&
				!( G_VRX_GoreBits( targ ) & ( VRX_GB_LARM | VRX_GB_RARM ) ) && random() < 0.6f ) {
		G_VRX_Cripple( targ, attacker, side > 0 ? VRX_GB_RARM : VRX_GB_LARM, dir, point, qtrue );
	}
	return take;
}

/*
=================
G_VRX_Bleed (g_bleedOut)

RDR2 style: a shot into somewhere that bleeds badly, and doesn't kill him outright, opens a
wound that keeps bleeding. He fights on, but his health drains, and when there's little left
he collapses and bleeds out on the floor. Wounds add up.

  the throat (a shot into the head box that isn't a head shot): about 8 s
  the upper thigh (the femoral) or the groin: about 14 s
  the chest (a lung): about 22 s
  the belly (the gut shot): about 35 s
  an arm or the lower leg: a light bleed that stops after a while, it never kills alone
=================
*/
#define VRX_BLEED_TICK      500         // ms between drains
#define VRX_BLEED_COLLAPSE  0.1f        // of his health: falls down to bleed out there
#define VRX_BLEED_LIGHT_MS  12000       // a light wound stops bleeding after this long
static float vrxBleedRate[MAX_CLIENTS];     // health a second, from the bad wounds
static float vrxBleedLight[MAX_CLIENTS];    // and from the light ones (they stop)
static int vrxBleedLightUntil[MAX_CLIENTS];
static float vrxBleedAcc[MAX_CLIENTS];
static int vrxBleedNext[MAX_CLIENTS];
static int vrxBleedBy[MAX_CLIENTS];
static int vrxBleedRegion[MAX_CLIENTS];

static void G_VRX_Bleed( gentity_t *targ, gentity_t *attacker, const vec3_t point, int region, int mod, qboolean headshot ) {
	int n = targ->s.number, max;
	float secs = 0;

	G_VRX_Register();
	trap_Cvar_Update( &g_bleedOut );
	if ( !g_bleedOut.integer || n < 0 || n >= MAX_CLIENTS || !targ->client || !targ->aiCharacter || headshot ||
		 G_VRX_Blast( mod ) || G_VRX_Knife( mod ) || mod == MOD_KICKED ) {
		return;
	}
	if ( region == VRH_NONE && point ) {
		// no tight hit box: by height
		float z = point[2] - targ->r.currentOrigin[2], eye = targ->client->ps.viewheight;
		region = z > eye - 4 ? VRH_HEAD : z > eye - 20 ? VRH_CHEST : z > -2 ? VRH_BELLY : z > -12 ? VRH_GROIN : VRH_LLEG;
	}
	switch ( region ) {
	case VRH_HEAD:  secs = 8; break;        // the throat
	case VRH_GROIN: secs = 14; break;
	case VRH_CHEST: secs = 22; break;
	case VRH_BELLY: secs = 35; break;
	case VRH_LLEG:
	case VRH_RLEG:
		if ( point && point[2] > targ->r.currentOrigin[2] - 12 ) {
			secs = 14;      // the upper thigh
		}
		break;
	case VRH_LARM:
	case VRH_RARM:
		break;
	default:
		return;
	}
	max = targ->client->ps.stats[STAT_MAX_HEALTH] > 0 ? targ->client->ps.stats[STAT_MAX_HEALTH] : 100;
	if ( secs > 0 ) {
		vrxBleedRate[n] += max / ( secs * ( 0.85f + 0.3f * random() ) );
		vrxBleedRegion[n] = region;
	} else {
		vrxBleedLight[n] += max / 60.0f;
		vrxBleedLightUntil[n] = level.time + VRX_BLEED_LIGHT_MS;
	}
	if ( !vrxBleedNext[n] ) {
		vrxBleedNext[n] = level.time + VRX_BLEED_TICK;
		vrxBleedAcc[n] = 0;
	}
	vrxBleedBy[n] = attacker ? attacker->s.number : ENTITYNUM_WORLD;
}

static void G_VRX_BleedClear( int n ) {
	vrxBleedRate[n] = vrxBleedLight[n] = vrxBleedAcc[n] = 0;
	vrxBleedNext[n] = vrxBleedLightUntil[n] = 0;
}

static void G_VRX_BleedFrame( void ) {
	int n;

	trap_Cvar_Update( &g_bleedOut );

	for ( n = 0; n < MAX_CLIENTS; n++ ) {
		gentity_t *e = &g_entities[n], *by;
		float rate;
		int max;

		if ( !vrxBleedNext[n] ) {
			continue;
		}
		if ( !e->inuse || !e->client || e->health <= 0 || !g_bleedOut.integer ) {
			G_VRX_BleedClear( n );      // dead (or gone): the next life starts clean
			continue;
		}
		if ( level.time < vrxBleedNext[n] ) {
			continue;
		}
		vrxBleedNext[n] = level.time + VRX_BLEED_TICK;
		if ( vrxBleedLight[n] > 0 && level.time >= vrxBleedLightUntil[n] ) {
			vrxBleedLight[n] = 0;       // a light wound has clotted
		}
		rate = vrxBleedRate[n] + vrxBleedLight[n];
		if ( rate <= 0 ) {
			G_VRX_BleedClear( n );
			continue;
		}
		max = e->client->ps.stats[STAT_MAX_HEALTH] > 0 ? e->client->ps.stats[STAT_MAX_HEALTH] : 100;
		vrxBleedAcc[n] += rate * VRX_BLEED_TICK * 0.001f;
		if ( vrxBleedAcc[n] < 1 ) {
			continue;
		}
		{
			int dmg = (int)vrxBleedAcc[n];
			vrxBleedAcc[n] -= dmg;
			if ( vrxBleedRate[n] <= 0 && dmg >= e->health ) {
				dmg = e->health - 1;        // light wounds alone never kill
				if ( dmg <= 0 ) {
					continue;
				}
			}
			by = vrxBleedBy[n] >= 0 && vrxBleedBy[n] < MAX_GENTITIES && g_entities[vrxBleedBy[n]].inuse ? &g_entities[vrxBleedBy[n]] : NULL;
			G_Damage( e, by, by ? by : e, NULL, NULL, dmg, DAMAGE_NO_KNOCKBACK | DAMAGE_NO_PROTECTION, MOD_UNKNOWN );
		}
		// weak from the blood lost: down on the floor, bleeding out there
		if ( e->inuse && e->health > 0 && e->aiCharacter && vrxBleedRate[n] > 0 && e->health <= max * VRX_BLEED_COLLAPSE &&
			 !( G_VRX_GoreBits( e ) & ( VRX_GB_DOWNED | VRX_GB_CRIPPLED ) ) && VRX_HUMAN_MALE( e->aiCharacter ) ) {
			by = vrxBleedBy[n] >= 0 && vrxBleedBy[n] < MAX_GENTITIES && g_entities[vrxBleedBy[n]].inuse ? &g_entities[vrxBleedBy[n]] : e;
			if ( trap_Cvar_VariableIntegerValue( "vr_log" ) ) G_Printf( "VRLOG down %d: bled to %d of %d\n", n, e->health, max );
			G_VRX_KnockDown( e, by, vrxBleedRegion[n], qtrue );
		}
	}
}

/*
=================
G_VRX_Kill

From G_Damage, as a blow kills a character (before its die function): the gory ones are
sent to every cgame (EV_VR_GORE). Only ever killing blows, never someone alive.

A knife kill from behind, or one that lands high, cuts the throat; one that goes in low
(a stab to the belly in VR, from a crouch without a headset) disembowels, and so does a
shotgun blast to the gut from close by.
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
	if ( VRX_HUMAN_MALE( targ->aiCharacter ) && G_VRX_HeavyBlast( mod, inflictor ) && G_VRX_OldGore( VRX_DISMEMBER ) &&
		 ( !inflictor || Distance( inflictor->r.currentOrigin, targ->r.currentOrigin ) < 220.0f ) ) {
		G_VRX_AddGoreBits( targ, VRX_GB_BLOWN );
	}
	G_VRX_Dismember( targ, inflictor, dir, point, mod, damage, headshot );
	{
		gentity_t *m = G_VRX_Marker( targ, qfalse );
		if ( m ) {
			m->s.density &= ~( VRX_GB_CRIPPLED | VRX_GB_DOWNED | VRX_GB_SURRENDER | VRX_GB_SCARED );   // (dead now; the arm stays off)
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
		// (a knife finishing someone downed: the head comes off only if the blade was at his neck,
		// which the cgame sees and asks for, G_VRX_Decap_f; on an arm or a leg it cuts that)
		if ( execution && G_VRX_Knife( mod ) ) {
			return;
		}
		if ( !G_VRX_Gore( VRX_VOXEL ) && ( execution ||
			 ( headshot && G_VRX_Heavy( targ, attacker, mod, damage ) ) ) ) {     // (the voxel gore holes heads itself)
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

	// a shotgun blast to the gut from close by spills it
	if ( ( mod == MOD_M97 || mod == MOD_AUTO5 || mod == MOD_M30 || mod == MOD_SHOTGUN ) && point && !headshot &&
		 VRX_HUMAN_MALE( targ->aiCharacter ) && G_VRX_OldGore( VRX_DISMEMBER ) &&
		 attacker && Distance( attacker->r.currentOrigin, targ->r.currentOrigin ) < 400.0f &&
		 !G_VRX_ShotAtHead( targ, dir, point ) ) {
		neck = targ->r.currentOrigin[2] + targ->client->ps.viewheight - 12.0f;
		if ( point[2] > targ->r.currentOrigin[2] - 6.0f && point[2] < neck - 4.0f ) {
			te = G_TempEntity( targ->r.currentOrigin, EV_VR_GORE );
			te->s.eventParm = VRG_BELLY;
			te->s.density = 1;      // torn open by the pellets: bigger than a knife's cut
			te->s.otherEntityNum = targ->s.number;
			te->s.otherEntityNum2 = attacker->s.number;
			if ( dir ) {
				VectorCopy( dir, te->s.origin2 );
			}
			return;
		}
	}
	if ( !G_VRX_OldGore( VRX_THROAT ) ) {
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
			   !( e->s.time && level.time < e->s.time + VRX_GROIN_MS + 500 ) &&
			   !( body->s.number < MAX_CLIENTS && ( level.time < vrxStunUntil[body->s.number] + 1000 ||
													level.time < vrxUpAt[body->s.number] + 1000 ) ) ) ) {
			G_FreeEntity( e );
			continue;
		}
		if ( e->s.time2 == VRX_STUN_MARK && ( body->health <= 0 || level.time >= vrxStunUntil[body->s.number] ) ) {
			e->s.time2 = 0;     // comes to (the cgame eases him back into his animation)
		}
		if ( body->health > 0 && ( e->s.density & VRX_GB_DOWNED ) && vrxGetUpAt[body->s.number] &&
			 level.time >= vrxGetUpAt[body->s.number] ) {
			G_VRX_GetUp( body, e );
		}
		if ( body->health > 0 && ( ( e->s.density & VRX_GB_CRIPPLED ) || ( ( e->s.density & VRX_GB_DOWNED ) && !vrxGetUpAt[body->s.number] ) ) &&
			 level.time >= e->timestamp ) {
			// bleeding out, still running from whoever did it
			gentity_t *by = e->count >= 0 && e->count < MAX_CLIENTS && g_entities[e->count].inuse ? &g_entities[e->count] : NULL;
			cast_state_t *cs = AICast_GetCastState( body->s.number );
			e->timestamp = level.time + ( ( e->s.density & VRX_GB_CRIPPLED ) ? 1000 : 1500 );    // about half a minute either way
			if ( cs && by && ( e->s.density & VRX_GB_CRIPPLED ) ) {
				VectorCopy( by->r.currentOrigin, cs->dangerEntityPos );
				cs->dangerEntityValidTime = level.time + 3600000;
			}
			G_Damage( body, by, by ? by : body, NULL, NULL, ( e->s.density & VRX_GB_CRIPPLED ) ? 3 : 2, DAMAGE_NO_KNOCKBACK | DAMAGE_NO_PROTECTION, MOD_UNKNOWN );
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
	// only the dead: not the blow that killed him (a living man stabbed, the downed finished off)
	if ( vrxKnifedAlive[n] && level.time - vrxKnifedAlive[n] < 1500 ) {
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		trap_Argv( 2 + i, arg, sizeof( arg ) );
		neck[i] = atof( arg );
	}
	// within reach of the player, and on (near) the body
	// (the ragdoll can slide or be thrown well away from where the server left the body)
	if ( Distance( neck, ent->r.currentOrigin ) > 120 || Distance( neck, body->r.currentOrigin ) > 200 ) {
		if ( trap_Cvar_VariableIntegerValue( "vr_debug" ) == 7 ) {
			G_Printf( "VR decap: %i too far (%.0f from you, %.0f from the body)\n", n,
					  Distance( neck, ent->r.currentOrigin ), Distance( neck, body->r.currentOrigin ) );
		}
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

// the weapons someone had before dropping them (downed, crippled, given up), back when they
// come back to life (G_VRX_Revive): a Survival enemy is the same entity respawned
static int vrxSavedWeapons[MAX_CLIENTS][MAX_WEAPONS / ( sizeof( int ) * 8 )];
static qboolean vrxWeaponsSaved[MAX_CLIENTS];

static void G_VRX_DropWeapon( gentity_t *e, cast_state_t *cs ) {
	gitem_t *item = e->client->ps.weapon > WP_NONE ? BG_FindItemForWeapon( e->client->ps.weapon ) : NULL;
	vec3_t org, vel;

	if ( e->s.number < MAX_CLIENTS && !vrxWeaponsSaved[e->s.number] ) {
		memcpy( vrxSavedWeapons[e->s.number], e->client->ps.weapons, sizeof( vrxSavedWeapons[0] ) );
		vrxWeaponsSaved[e->s.number] = qtrue;
	}
	// Survival enemies never leave weapons to pick up (TossClientWeapons): the gun just goes
	if ( item && g_gametype.integer != GT_SURVIVAL ) {
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

/*
=================
G_VRX_Revive

A character coming back to life (a Survival respawn, a scripted rebirth): whatever was done
to them last time goes. Without this a Survival enemy who had been put down, crippled or had
given up came back as the same entity unarmed, frozen for an hour (noAttackTime / pauseTime)
and, if they had surrendered, still with their hands up, so the waves stalled.
=================
*/
void G_VRX_Revive( gentity_t *ent ) {
	cast_state_t *cs;
	gentity_t *m;
	int n = ent->s.number;

	if ( !ent->client || n < 0 || n >= MAX_CLIENTS ) {
		return;
	}
	m = G_VRX_Marker( ent, qfalse );
	if ( m ) {
		G_FreeEntity( m );
	}
	vrxDownedAt[n] = 0;
	vrxGetUpAt[n] = 0;
	vrxStunUntil[n] = 0;
	vrxStaggerUntil[n] = 0;
	vrxUpAt[n] = 0;
	vrxDisarmed[n] = 0;
	vrxPickupAt[n] = 0;
	G_VRX_DisarmItemFree( n );
	ent->r.svFlags &= ~SVF_NOCLIENT;        // (a body that had gone, G_VRX_BodyGone)
	cs = AICast_GetCastState( n );
	if ( trap_Cvar_VariableIntegerValue( "vr_debug" ) == 8 ) {     // vr_debug 8 traces Survival respawns
		G_Printf( "VR revive: %i %s, gore marker %s, weapons %s, frozen %s\n", n, ent->aiName ? ent->aiName : "",
				  m ? "cleared" : "none", vrxWeaponsSaved[n] ? "given back" : "kept",
				  cs && ( cs->pauseTime > level.time || cs->noAttackTime > level.time ) ? "yes, cleared" : "no" );
	}
	if ( vrxWeaponsSaved[n] ) {
		memcpy( ent->client->ps.weapons, vrxSavedWeapons[n], sizeof( ent->client->ps.weapons ) );
		vrxWeaponsSaved[n] = qfalse;
	}
	if ( cs ) {
		cs->noAttackTime = 0;
		cs->pauseTime = 0;
		if ( cs->dangerEntityValidTime > level.time + 60000 ) {
			cs->dangerEntityValidTime = 0;      // no longer running from whoever crippled them
		}
		// (the weapon to hold is picked by the AI as usual, as after any death)
	}
}

/*
=================
G_VRX_Disarm, G_VRX_DisarmFrame

Shot in the right arm and still alive: his weapon falls from his hand and lies where it fell
(nobody else can take it). Once he's over the hit (no longer stunned, staggering, down or getting
up) he stoops for it, and has it again a moment later.
=================
*/
#define VRX_DISARM_MARK     0x56520000
#define VRX_PICKUP_MS       900
static void G_VRX_DisarmItemFree( int n ) {
	int k = vrxDisarmItem[n];

	vrxDisarmItem[n] = 0;
	if ( k > 0 && k < MAX_GENTITIES ) {
		gentity_t *it = &g_entities[k];
		if ( it->inuse && it->s.eType == ET_ITEM && it->damage == VRX_DISARM_MARK + n ) {
			G_FreeEntity( it );
		}
	}
}

static void G_VRX_Disarm( gentity_t *targ ) {
	cast_state_t *cs = AICast_GetCastState( targ->s.number );
	gitem_t *item;
	gentity_t *it;
	vec3_t org, vel, right;
	int n = targ->s.number;

	if ( !cs || !targ->client || n <= 0 || n >= MAX_CLIENTS || vrxDisarmed[n] || targ->client->ps.weapon <= WP_NONE ||
		 !VRX_HUMAN_MALE( targ->aiCharacter ) || !G_VRX( VRX_REACTIONS ) || G_VRX_Downed( targ ) ||
		 ( G_VRX_GoreBits( targ ) & ( VRX_GB_SURRENDER | VRX_GB_CRIPPLED ) ) ) {
		return;
	}
	vrxDisarmed[n] = targ->client->ps.weapon;
	vrxPickupAt[n] = 0;
	G_VRX_DisarmItemFree( n );
	if ( !vrxWeaponsSaved[n] ) {
		memcpy( vrxSavedWeapons[n], targ->client->ps.weapons, sizeof( vrxSavedWeapons[0] ) );
		vrxWeaponsSaved[n] = qtrue;
	}
	item = BG_FindItemForWeapon( targ->client->ps.weapon );
	if ( item ) {
		AngleVectors( targ->client->ps.viewangles, NULL, right, NULL );
		VectorCopy( targ->r.currentOrigin, org );
		org[2] += 26;
		VectorMA( org, 9, right, org );
		VectorSet( vel, crandom() * 40, crandom() * 40, 70 );
		VectorMA( vel, 40, right, vel );
#ifdef VR_NET_HANDS
		it = LaunchItem( item, org, vel, n );
#else
		it = LaunchItem( item, org, vel );
#endif
		if ( it ) {
			it->touch = NULL;               // his: not for the taking
			it->r.contents = 0;
			it->damage = VRX_DISARM_MARK + n;
			it->think = G_FreeEntity;       // (whatever becomes of him, it doesn't lie there for ever)
			it->nextthink = level.time + 60000;
			vrxDisarmItem[n] = it->s.number;
		}
	}
	memset( targ->client->ps.weapons, 0, sizeof( targ->client->ps.weapons ) );
	targ->client->ps.weapon = WP_NONE;
	cs->weaponNum = WP_NONE;
}

static void G_VRX_DisarmFrame( void ) {
	int n;

	for ( n = 1; n < MAX_CLIENTS && n < level.maxclients; n++ ) {
		gentity_t *e = &g_entities[n];
		cast_state_t *cs;

		if ( !vrxDisarmed[n] ) {
			continue;
		}
		if ( !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 ) {
			// dead: it stays where it fell a while (his weapons come back with him, G_VRX_Revive)
			vrxDisarmed[n] = 0;
			vrxPickupAt[n] = 0;
			continue;
		}
		if ( G_VRX_GoreBits( e ) & ( VRX_GB_SURRENDER | VRX_GB_CRIPPLED ) ) {
			vrxDisarmed[n] = 0;     // given up, or an arm off: he's done with it
			vrxPickupAt[n] = 0;
			continue;
		}
		cs = AICast_GetCastState( n );
		if ( !cs ) {
			continue;
		}
		if ( G_VRX_Downed( e ) || vrxGetUpAt[n] || ( vrxUpAt[n] && level.time < vrxUpAt[n] + VRX_GETUP_MS ) ||
			 level.time < vrxStunUntil[n] + 400 || level.time < vrxStaggerUntil[n] + 400 ) {
			vrxPickupAt[n] = 0;     // not himself yet (or hit again as he stooped)
			if ( cs->noAttackTime < level.time + 500 ) {
				cs->noAttackTime = level.time + 500;
			}
			continue;
		}
		if ( !vrxPickupAt[n] ) {
			// himself again: down for it
			vrxPickupAt[n] = level.time + VRX_PICKUP_MS;
			cs->attackcrouch_time = level.time + VRX_PICKUP_MS;
			if ( cs->noAttackTime < level.time + VRX_PICKUP_MS + 200 ) {
				cs->noAttackTime = level.time + VRX_PICKUP_MS + 200;
			}
			if ( cs->pauseTime < level.time + VRX_PICKUP_MS ) {
				cs->pauseTime = level.time + VRX_PICKUP_MS;
			}
			continue;
		}
		if ( level.time < vrxPickupAt[n] ) {
			continue;
		}
		// he has it
		if ( vrxWeaponsSaved[n] ) {
			memcpy( e->client->ps.weapons, vrxSavedWeapons[n], sizeof( e->client->ps.weapons ) );
			vrxWeaponsSaved[n] = qfalse;
		}
		e->client->ps.weapon = vrxDisarmed[n];
		cs->weaponNum = vrxDisarmed[n];
		G_VRX_DisarmItemFree( n );
		vrxDisarmed[n] = 0;
		vrxPickupAt[n] = 0;
	}
}

/*
=================
G_VRX_BodyGone

Survival: a body that has lain 30 seconds goes (ai_cast_think.c), whoever it was and whether or
not it's due back: it isn't sent to anyone and nothing touches it, until it's spawned again
(G_VRX_Revive). Bodies left lying cost every frame they're in view.
=================
*/
void G_VRX_BodyGone( gentity_t *ent ) {
	if ( !ent->client || ( ent->r.svFlags & SVF_NOCLIENT ) ) {
		return;
	}
	ent->r.svFlags |= SVF_NOCLIENT;
	ent->r.contents = 0;
	trap_LinkEntity( ent );
	if ( ent->s.number > 0 && ent->s.number < MAX_CLIENTS ) {
		G_VRX_DisarmItemFree( ent->s.number );
	}
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

/*
=================
G_VRX_SurvivalTest (test aid, cheats: vr_testSurvival 1)

Plays Survival by itself so the waves can be checked without anyone at the controls: every
couple of seconds the nearest living enemy is taken down a different way (shot down then
kicked, made to give up then shot, blown up in front of the player, or just shot), so the
ones that come back next wave are the ones the gore code had changed. Logs each wave, how
many of the living are unarmed or frozen, weapons lying about, and the player's dual wield
state; takes screenshots now and then; quits after vr_testSurvival waves (1: 8).
=================
*/
static void G_VRX_SurvivalTest( void ) {
	static int nextTime, step, pendingKill = -1, pendingTime, lastWave = -1, shots, lastShotWave = -1;
	gentity_t *player = &g_entities[0], *e, *best = NULL;
	int i, want = trap_Cvar_VariableIntegerValue( "vr_testSurvival" );
	float d, bestDist = 1e9f;
	vec3_t fwd, p, ang;

	if ( want == -100 && g_cheats.integer && g_gametype.integer == GT_SURVIVAL && player->inuse && player->client ) {
		// vr_testSurvival -100: a tour of the gore on the first enemies of wave 1, a screenshot each
		player->flags |= FL_GODMODE | FL_NOTARGET;
		if ( step || !svParams.waveInProgress || level.time < nextTime ) {
			if ( !svParams.waveInProgress ) {
				nextTime = level.time + 4000;     // (the first ones a few seconds to spawn)
			}
			return;
		}
		step = 1;
		G_Printf( "VR goretour: start\n" );
		trap_SendConsoleCommand( EXEC_APPEND,
			"vrtestbody down 3; wait 150; screenshotJPEG; wait 20;"
			"vrtestbody; wait 120; screenshotJPEG; wait 20;"
			"vrtestdecap; wait 100; screenshotJPEG; wait 20;"
			"vrtestbody throat; wait 120; screenshotJPEG; wait 20;"
			"vrtestbody belly; wait 120; screenshotJPEG; wait 20;"
			"vrtestaim; wait 5; +activate; wait 40; screenshotJPEG; wait 5; +attack; wait 3; -attack; -activate; wait 50; screenshotJPEG; wait 30;"
			"quit\n" );
		return;
	}
	if ( want == -300 && g_cheats.integer && g_gametype.integer == GT_SURVIVAL && player->inuse && player->client ) {
		// vr_testSurvival -300: once the first wave is up, run goredemo.cfg (the voxel gore's demo)
		player->flags |= FL_GODMODE | FL_NOTARGET;
		if ( step ) {
			// everyone stands still to be shot
			for ( i = 1; i < level.maxclients; i++ ) {
				cast_state_t *cs;
				e = &g_entities[i];
				if ( e->inuse && e->client && e->aiCharacter && e->health > 0 && ( cs = AICast_GetCastState( i ) ) != NULL ) {
					cs->pauseTime = level.time + 1000;
					VectorClear( e->client->ps.velocity );
					VectorSubtract( player->r.currentOrigin, e->r.currentOrigin, fwd );     // facing the camera
					vectoangles( fwd, ang );
					ang[PITCH] = ang[ROLL] = 0;
					SetClientViewAngle( e, ang );
					VectorCopy( ang, cs->ideal_viewangles );
					VectorCopy( ang, cs->viewangles );
				}
			}
			return;
		}
		if ( !svParams.waveInProgress || level.time < nextTime ) {
			if ( !svParams.waveInProgress ) {
				nextTime = level.time + 4000;
			}
			return;
		}
		step = 1;
		trap_SendConsoleCommand( EXEC_APPEND, "exec goredemo.cfg\n" );
		return;
	}
	if ( want < 0 && g_cheats.integer && g_gametype.integer == GT_SURVIVAL && player->inuse && player->client ) {
		// vr_testSurvival -N: shoot the nearest living enemy N times, a short burst each, with a screenshot
		// of the wounds after each (and stand him up close in front first)
		player->flags |= FL_GODMODE | FL_NOTARGET;
		if ( level.time < nextTime || !svParams.waveInProgress ) {
			return;
		}
		for ( i = 1; i < level.maxclients; i++ ) {
			e = &g_entities[i];
			if ( e->inuse && e->client && e->aiCharacter && e->aiTeam != 1 && !e->aiInactive && e->health > 0 && e->r.linked &&
				 ( d = Distance( e->r.currentOrigin, player->r.currentOrigin ) ) < bestDist ) {
				best = e;
				bestDist = d;
			}
		}
		if ( !best ) {
			nextTime = level.time + 500;
			return;
		}
		AngleVectors( player->client->ps.viewangles, fwd, NULL, NULL );
		fwd[2] = 0;
		VectorNormalize( fwd );
		VectorMA( player->r.currentOrigin, 48, fwd, p );
		G_SetOrigin( best, p );
		VectorCopy( p, best->client->ps.origin );
		VectorSet( ang, 0, player->client->ps.viewangles[YAW] + 180, 0 );
		SetClientViewAngle( best, ang );
		trap_LinkEntity( best );
		{
			cast_state_t *bcs = AICast_GetCastState( best->s.number );
			if ( bcs ) {
				bcs->pauseTime = level.time + 2500;     // stands still to be shot
			}
			VectorClear( best->client->ps.velocity );
		}
		if ( best->health < 300 ) {
			best->health = best->client->ps.stats[STAT_HEALTH] = 300;     // (survives the bursts, to show the holes)
		}
		VectorSubtract( p, player->r.currentOrigin, fwd );
		fwd[2] = 30 + ( step % 3 ) * 14 - player->client->ps.viewheight;     // legs, belly, chest
		step++;
		G_Printf( "VR woundtest: burst %i at %i (%s)\n", step, best->s.number, best->aiName ? best->aiName : "" );
		trap_SendConsoleCommand( EXEC_APPEND, step > -want ? "wait 30; quit\n" : "wait 10; vrtestaim head; wait 3; +lookdown; wait 14; -lookdown; wait 2; +attack; wait 2; -attack; wait 40; screenshotJPEG\n" );
		nextTime = level.time + 3000;
		return;
	}
	if ( want <= 0 || !g_cheats.integer || g_gametype.integer != GT_SURVIVAL || !player->inuse || !player->client ) {
		return;
	}
	if ( want == 1 ) {
		want = 8;
	}
	if ( level.time < nextTime - 60000 ) {
		nextTime = 0;   // a new level
	}
	player->flags |= FL_GODMODE | FL_NOTARGET;
	if ( svParams.waveCount != lastWave ) {
		int unarmed = 0, frozen = 0, alive = 0, items = 0;
		for ( i = 1; i < level.num_entities; i++ ) {
			e = &g_entities[i];
			if ( !e->inuse ) {
				continue;
			}
			if ( e->s.eType == ET_ITEM && e->item && e->item->giType == IT_WEAPON ) {
				items++;
			}
			if ( i < level.maxclients && e->client && e->aiCharacter && e->aiTeam != 1 && !e->aiInactive && e->health > 0 ) {
				cast_state_t *cs = AICast_GetCastState( i );
				alive++;
				if ( e->client->ps.weapon == WP_NONE && !( e->client->ps.weapons[0] | e->client->ps.weapons[1] ) &&
					 e->aiCharacter != AICHAR_ZOMBIE_SURV && e->aiCharacter != AICHAR_FLESH ) {
					unarmed++;
				}
				if ( cs && ( cs->pauseTime > level.time + 60000 || cs->noAttackTime > level.time + 60000 ) ) {
					frozen++;
				}
			}
		}
		G_Printf( "VR survtest: wave %i started (need %i kills), %i enemies up, %i unarmed, %i frozen, %i weapons on the ground, player dual %i\n",
				  svParams.waveCount, svParams.killCountRequirement, alive, unarmed, frozen, items, player->client->ps.persistant[PERS_DUAL] );
		lastWave = svParams.waveCount;
		if ( svParams.waveCount > want ) {
			G_Printf( "VR survtest: done, reached wave %i\n", svParams.waveCount );
			trap_SendConsoleCommand( EXEC_APPEND, "wait 30; quit\n" );
			trap_Cvar_Set( "vr_testSurvival", "0" );
			return;
		}
	}
	// the second half of a two part kill
	if ( pendingKill >= 0 && level.time >= pendingTime ) {
		e = &g_entities[pendingKill];
		if ( e->inuse && e->client && e->health > 0 ) {
			AngleVectors( player->client->ps.viewangles, fwd, NULL, NULL );
			G_Damage( e, player, player, fwd, e->r.currentOrigin, 10000, DAMAGE_NO_PROTECTION, step % 4 == 1 ? MOD_KICKED : MOD_MP40 );
			G_Printf( "VR survtest: wave %i, finished %i (%s) %s\n", svParams.waveCount, pendingKill, e->aiName ? e->aiName : "",
					  e->health > 0 ? "but they LIVED" : "dead" );
		}
		pendingKill = -1;
		nextTime = level.time + 1500;
	}
	if ( pendingKill >= 0 || level.time < nextTime || !svParams.waveInProgress || svParams.wavePending ) {
		return;
	}
	// a screenshot of the HUD with 3 or fewer left (the compass), once a wave for the first few
	if ( svParams.killCountRequirement - svParams.waveKillCount <= 3 && lastShotWave != svParams.waveCount && shots < 6 ) {
		trap_SendConsoleCommand( EXEC_APPEND, "screenshotJPEG\n" );
		G_Printf( "VR survtest: wave %i, %i left, screenshot of the compass\n", svParams.waveCount,
				  svParams.killCountRequirement - svParams.waveKillCount );
		lastShotWave = svParams.waveCount;
		shots++;
		nextTime = level.time + 400;
		return;
	}
	for ( i = 1; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e->inuse && e->client && e->aiCharacter && e->aiTeam != 1 && !e->aiInactive && e->health > 0 &&
			 e->r.linked && e->takedamage && ( d = Distance( e->r.currentOrigin, player->r.currentOrigin ) ) < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	if ( !best ) {
		nextTime = level.time + 500;
		return;
	}
	step++;
	AngleVectors( player->client->ps.viewangles, fwd, NULL, NULL );
	switch ( step % 4 ) {
	case 1:     // shot down (writhing, gun dropped), then finished with a kick
		G_VRX_KnockDown( best, player, VRH_BELLY, qfalse );
		pendingKill = best->s.number;
		pendingTime = level.time + 1200;
		G_Printf( "VR survtest: wave %i, %i (%s) shot down\n", svParams.waveCount, best->s.number, best->aiName ? best->aiName : "" );
		break;
	case 2:     // gives up (hands up, gun dropped), then shot
		if ( best->client->ps.weapon > WP_NONE || G_VRX_Scareable( best ) ) {
			G_VRX_Surrender( best, AICast_GetCastState( best->s.number ) );
			pendingKill = best->s.number;
			pendingTime = level.time + 1200;
			G_Printf( "VR survtest: wave %i, %i (%s) surrendered\n", svParams.waveCount, best->s.number, best->aiName ? best->aiName : "" );
			break;
		}
		// (fall through: a zombie can't give up)
	case 3:     // blown up in front of the player, with a screenshot of what's left of him
		fwd[2] = 0;
		VectorNormalize( fwd );
		VectorMA( player->r.currentOrigin, 80, fwd, p );
		G_SetOrigin( best, p );
		VectorCopy( p, best->client->ps.origin );
		VectorSet( ang, 0, player->client->ps.viewangles[YAW] + 180, 0 );
		SetClientViewAngle( best, ang );
		trap_LinkEntity( best );
		G_Damage( best, player, player, fwd, NULL, 10000, DAMAGE_NO_PROTECTION, MOD_ROCKET_SPLASH );
		G_Printf( "VR survtest: wave %i, %i (%s, char %i) blown up\n", svParams.waveCount, best->s.number, best->aiName ? best->aiName : "", best->aiCharacter );
		if ( shots < 60 ) {
			trap_SendConsoleCommand( EXEC_APPEND, "wait 70; screenshotJPEG\n" );
			shots++;
		}
		nextTime = level.time + 2500;
		break;
	default:    // just shot
		G_Damage( best, player, player, fwd, best->r.currentOrigin, 10000, DAMAGE_NO_PROTECTION, MOD_MP40 );
		G_Printf( "VR survtest: wave %i, %i (%s) shot\n", svParams.waveCount, best->s.number, best->aiName ? best->aiName : "" );
		nextTime = level.time + 1500;
		break;
	}
}
