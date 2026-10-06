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
};
static qboolean vrxRegistered;

static void G_VRX_PropsFrame( void );
static void G_VRX_MarkersFrame( void );

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
G_VRX_Kill

From G_Damage, as a blow kills a character (before its die function): the gory ones are
sent to every cgame (EV_VR_GORE). Only ever killing blows, never someone alive.

A knife kill from behind, or one that lands high, cuts the throat; one that goes in low
(a stab to the belly in VR, from a crouch without a headset) disembowels.
=================
*/
void G_VRX_Kill( gentity_t *targ, gentity_t *attacker, const vec3_t dir, const vec3_t point, int mod ) {
	gentity_t *te;
	int gore = VRG_NONE;
	float neck;

	if ( !targ->client || !targ->aiCharacter || !G_VRX_Gore( VRX_THROAT ) ) {
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
	int             time;
} vrxThrown_t;

static vrxHeld_t vrxHeld[MAX_CLIENTS];
static vrxThrown_t vrxThrown[VRX_MAX_THROWN];
static qboolean vrxPropsReady;

static void G_VRX_Launch( vrxThrown_t *t, gentity_t *e, gentity_t *ent, const vec3_t vel, const vrxPropState_t *saved );

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
		if ( G_VRX_Grabbable( e ) && ( d = Distance( e->r.currentOrigin, ent->r.currentOrigin ) ) < bestDist ) {
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
			 ( body->health > 0 && !( e->s.density & ( VRX_GB_SCARED | VRX_GB_SURRENDER ) ) ) ) {
			G_FreeEntity( e );
			continue;
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
	if ( Distance( neck, ent->r.currentOrigin ) > 120 || Distance( neck, body->r.currentOrigin ) > 80 ) {
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
