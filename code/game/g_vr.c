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

// g_vr.c -- the player's hands with a headset: reloading by hand, and grabbing bodies.
//
// Reloading follows the weapon (see BG_VR_ReloadClass):
//   magazine   grip tap drops the mag, grab one from the chest, push it into the well,
//              rack the slide/bolt if the gun had run dry
//   bolt       work the bolt after every shot, stripper clips top it up
//   en bloc    the clip pings out when empty, a new one closes the bolt
//   pump       shells one at a time, pump after every shot
//   shells     shells one at a time, rack it if it ran dry
//   revolver   grip tap opens it, a speedloader fills it
// Everything is deliberately forgiving: generous distances, rounds left in a dropped
// magazine go back to the reserve.
//
// The off hand's grip button that isn't used for any of that grabs dead bodies; they
// follow the hand and fly off with its speed when let go.

#include "g_local.h"

#define VR_CHEST_RADIUS     14.0f   // ~35cm around the pouch counts as reaching into it
#define VR_CHEST_DROP       22.0f   // the pouch is this far below the eyes (low on the chest,
									// clear of a shouldered rifle's bolt; cgame draws it there)
#define VR_INSERT_RADIUS    8.0f    // ~20cm around the magazine well / loading port
#define VR_CHARGE_RADIUS    11.0f   // ~28cm around the slide / bolt / pump
#define VR_ACTION_RADIUS    9.0f    // ~23cm around the gun hand also reaches the action
#define VR_CHARGE_PULL      0.6f    // fraction of the travel that counts as racked
#define VR_GRAB_MARGIN      18.0f   // reach around a body's box (its ragdoll lies a little off it)
#define VR_THROW_MAX        900.0f
#define VR_THROW_SCALE      1.5f    // throws carry a little further than the hand moved
#define VR_THROW_SPEED      40.0f   // a let-go faster than this is a throw

typedef struct {
	qboolean    initialised;
	qboolean    chambered[WP_NUM_WEAPONS];
	qboolean    magOut[WP_NUM_WEAPONS];
	int         weapon;             // weapon the shared state describes
	int         lastEject;
	qboolean    gripWas;

	int         heldItem;
	qboolean    chargeHeld;
	qboolean    chargeDone;
	vec3_t      chargeStart;

	int         lastTime;
	vec3_t      lastHand;
	vec3_t      domVel;             // the weapon hand's velocity (grenade throws)
	vec3_t      throwVel;           // its fastest in the last moments: grenades leave a little
	int         throwVelTime;       // after the trigger is let go, when the hand has stopped
	vec3_t      lastDom;
	vec3_t      handVel;

	int         grabbed;            // entity number, -1 none
	vec3_t      grabOffset;
	int         grabbedPmType;
	vec3_t      grabbedMins, grabbedMaxs;   // the body's own box, while it hangs from the hand

	float       flatDist;           // without a headset: how far in front the body is held
	qboolean    useWas, attackWas;

	int         thrown;             // body flying after a throw, -1 none
	vec3_t      thrownVel;
	int         thrownTime;
} vrPlayer_t;

static vrPlayer_t vrp;
static vmCvar_t vr_manualReload;
static vmCvar_t vr_aimSpread;
static vmCvar_t g_flatGrab;         // without a headset: hold use on a body to pick it up
static vmCvar_t g_vrGrabAssist;     // how far hands reach for things: 0 off, 1 normal, 2 strong

static qboolean G_VR_IsPlayer( gentity_t *ent );
static void G_VR_Init( void );
static qboolean vrCvarsRegistered;

/*
=================
G_VR_SpreadScale

Multiplier for the player's bullet spread when aiming with a tracked controller.
=================
*/
float G_VR_SpreadScale( gentity_t *ent ) {
	if ( !G_VR_IsPlayer( ent ) ) {
		return 1.0f;
	}
	G_VR_Init();
	trap_Cvar_Update( &vr_aimSpread );
	return ( vr_aimSpread.value < 0 ? 0 : ( vr_aimSpread.value > 1 ? 1 : vr_aimSpread.value ) ) *
		   ( gVR->ironSights ? 0.5f : 1.0f );       // down the sights
}

void G_VR_Reset( void ) {
	vrp.initialised = qfalse;
}

static qboolean G_VR_IsPlayer( gentity_t *ent ) {
	return gVR && gVR->active && ent->client && ent->s.number == 0 && !ent->aiCharacter;
}

static void G_VR_Init( void ) {
	int i;

	if ( !vrCvarsRegistered ) {
		trap_Cvar_Register( &vr_manualReload, "vr_manualReload", "1", CVAR_ARCHIVE );
		trap_Cvar_Register( &vr_aimSpread, "vr_aimSpread", "0.25", CVAR_ARCHIVE );
		trap_Cvar_Register( &g_flatGrab, "g_flatGrab", "1", CVAR_ARCHIVE );
		trap_Cvar_Register( &g_vrGrabAssist, "g_vrGrabAssist", "1", CVAR_ARCHIVE );
		vrCvarsRegistered = qtrue;
	}
	if ( vrp.initialised ) {
		return;
	}
	memset( &vrp, 0, sizeof( vrp ) );
	for ( i = 0; i < WP_NUM_WEAPONS; i++ ) {
		vrp.chambered[i] = qtrue;
	}
	vrp.weapon = -1;
	vrp.grabbed = vrp.thrown = -1;
	vrp.lastEject = gVR->ejectCount;
	vrp.initialised = qtrue;
}

static void G_VR_Haptic( int hand, float amplitude, int msec ) {
	if ( amplitude > gVR->hapticAmplitude[hand] ) {
		gVR->hapticAmplitude[hand] = amplitude;
		gVR->hapticDuration[hand] = msec;
	}
}

// eye position the view used
static void G_VR_Eye( gentity_t *ent, vec3_t eye ) {
	VectorCopy( ent->r.currentOrigin, eye );
	eye[2] += gVR->eyeHeight > 0 ? gVR->eyeHeight : ent->client->ps.viewheight;
}

// a tracked hand in the world (not clipped against walls, for interactions)
static void G_VR_HandWorld( gentity_t *ent, int hand, vec3_t out ) {
	vec3_t offset, eye;
	float yaw = DEG2RAD( SHORT2ANGLE( ent->client->ps.delta_angles[YAW] ) );
	float s = sin( yaw ), c = cos( yaw );

	G_VR_Eye( ent, eye );
	VectorSubtract( gVR->handOrigin[hand], gVR->hmdOrigin, offset );
	out[0] = eye[0] + offset[0] * c - offset[1] * s;
	out[1] = eye[1] + offset[0] * s + offset[1] * c;
	out[2] = eye[2] + offset[2];
}

/*
=================
G_VR_UpdateWeaponState

Before every pmove: tell pmove (and cgame, which reads the same struct) whether the
current weapon is reloaded by hand and whether it can fire.
=================
*/
void G_VR_UpdateWeaponState( gentity_t *ent ) {
	int weapon = ent->client->ps.weapon;

	if ( !G_VR_IsPlayer( ent ) ) {
		return;
	}
	G_VR_Init();
	trap_Cvar_Update( &vr_manualReload );

	if ( weapon != vrp.weapon ) {
		// switching weapons puts whatever the off hand held back
		vrp.heldItem = 0;
		vrp.chargeHeld = qfalse;
		vrp.weapon = weapon;
	}
	if ( weapon < 0 || weapon >= WP_NUM_WEAPONS ) {
		weapon = 0;
	}

	gVR->manualReload = vr_manualReload.integer && BG_VR_ReloadClass( weapon ) != VRR_NONE;
	gVR->chambered = vrp.chambered[weapon];
	gVR->magOut = gVR->manualReload && vrp.magOut[weapon];
	gVR->heldItem = vrp.heldItem;
	gVR->chargeHeld = vrp.chargeHeld;
}

/*
=================
G_VR_OnFire

A shot left the player's gun.
=================
*/
void G_VR_OnFire( gentity_t *ent ) {
	int weapon, clip;

	if ( !G_VR_IsPlayer( ent ) || !gVR->manualReload ) {
		return;
	}
	weapon = ent->client->ps.weapon;
	clip = ent->client->ps.ammoclip[BG_FindClipForWeapon( weapon )];

	switch ( BG_VR_ReloadClass( weapon ) ) {
	case VRR_BOLT:
	case VRR_PUMP:
		vrp.chambered[weapon] = qfalse;     // work the action for the next round
		break;
	case VRR_MAG:
	case VRR_SHELLS:
		if ( clip <= 0 ) {
			vrp.chambered[weapon] = qfalse; // ran dry: slide / bolt locks back
		}
		break;
	case VRR_ENBLOC:
		if ( clip <= 0 ) {
			vrp.chambered[weapon] = qfalse; // *ping*
			vrp.magOut[weapon] = qtrue;
			gVR->magDropCount++;
		}
		break;
	default:
		break;
	}
	gVR->chambered = vrp.chambered[weapon];
}

/*
=================
G_VR_Reload

Hand reloading for the current weapon. Returns qtrue if the off hand's grip press was
used by it.
=================
*/
static qboolean G_VR_Reload( gentity_t *ent, const vec3_t offHand, qboolean pressed, qboolean released, int dom ) {
	playerState_t *ps = &ent->client->ps;
	int weapon = ps->weapon;
	int cls = BG_VR_ReloadClass( weapon );
	int clipIndex = BG_FindClipForWeapon( weapon );
	int ammoIndex = BG_FindAmmoForWeapon( weapon );
	int maxclip = BG_GetMaxClip( ps, weapon );
	int *clip = &ps->ammoclip[clipIndex];
	int *reserve = &ps->ammo[ammoIndex];
	vrWeaponGeom_t geom;
	vec3_t hand, aim, origin, axis[3], magPoint, chargePoint, chargeDir, eye, chest, d;
	qboolean used = qfalse, wantsAmmo;
	int off = !dom, n;

	if ( !gVR->manualReload || cls == VRR_NONE ) {
		vrp.heldItem = 0;
		vrp.chargeHeld = qfalse;
		return qfalse;
	}
	if ( ps->weaponstate == WEAPON_RAISING || ps->weaponstate == WEAPON_DROPPING ||
		 ps->weaponstate == WEAPON_DROPPING_TORELOAD || ps->weaponstate == WEAPON_RAISING_TORELOAD ) {
		return qfalse;
	}

	// where the gun and its parts are, exactly as cgame draws them
	BG_VR_WeaponGeom( weapon, trap_Cvar_VariableIntegerValue( "cg_vanilla_guns" ), &geom );
	geom.scale = gVR->weaponScale > 0 ? gVR->weaponScale : 1.0f;
	geom.travel *= geom.scale;      // a smaller bolt moves less
	G_VR_HandWorld( ent, dom, hand );
	VectorSet( aim, gVR->weaponAngles[PITCH], ps->viewangles[YAW], gVR->weaponAngles[ROLL] );
	BG_VR_WeaponFrame( &geom, hand, aim, NULL, origin, axis );
	BG_VR_ModelPoint( origin, axis, geom.mag, magPoint );
	BG_VR_ModelPoint( origin, axis, geom.charge, chargePoint );
	BG_VR_ModelDir( axis, geom.chargeDir, chargeDir );
	VectorNormalize( chargeDir );

	G_VR_Eye( ent, eye );
	VectorCopy( eye, chest );
	chest[2] -= VR_CHEST_DROP;

	// drop the magazine / open the cylinder (grip tap on the gun hand)
	if ( gVR->ejectCount != vrp.lastEject ) {
		vrp.lastEject = gVR->ejectCount;
		if ( ( cls == VRR_MAG || cls == VRR_ENBLOC || cls == VRR_REVOLVER ) && !vrp.magOut[weapon] ) {
			if ( cls == VRR_MAG ) {
				// a round left in the chamber means no need to rack after the new mag
				vrp.chambered[weapon] = *clip > 0 && vrp.chambered[weapon];
			} else if ( cls == VRR_ENBLOC ) {
				vrp.chambered[weapon] = qfalse;
			}
			*reserve += *clip;      // forgiving: the rounds aren't lost
			*clip = 0;
			vrp.magOut[weapon] = qtrue;
			gVR->magDropCount++;
			G_VR_Haptic( dom, 0.5f, 40 );
		}
	}

	// take a magazine / clip / shell from the chest
	if ( cls == VRR_MAG || cls == VRR_ENBLOC || cls == VRR_REVOLVER ) {
		wantsAmmo = vrp.magOut[weapon];
	} else {
		wantsAmmo = *clip < maxclip;
	}
	// (a hand on the gun's action works the action: a shouldered rifle's bolt is close to the
	// pouch, and after a shot the rifle both wants ammo and needs its bolt worked)
	if ( pressed && !vrp.heldItem && wantsAmmo && *reserve > 0 && Distance( offHand, chest ) < VR_CHEST_RADIUS &&
		 !( ( cls == VRR_MAG || cls == VRR_BOLT || cls == VRR_PUMP || cls == VRR_SHELLS ) &&
			( Distance( offHand, chargePoint ) < VR_CHARGE_RADIUS || ( cls != VRR_PUMP && Distance( offHand, hand ) < VR_ACTION_RADIUS ) ) &&
			( !vrp.chambered[weapon] || Distance( offHand, chargePoint ) < Distance( offHand, chest ) ) ) ) {
		vrp.heldItem = ( cls == VRR_PUMP || cls == VRR_SHELLS ) ? 2 : 1;
		G_VR_Haptic( off, 0.4f, 30 );
		used = qtrue;
	}
	if ( released && vrp.heldItem ) {
		vrp.heldItem = 0;   // let go of it: back in the pouch
	}

	// push it in
	if ( vrp.heldItem && Distance( offHand, magPoint ) < VR_INSERT_RADIUS ) {
		switch ( cls ) {
		case VRR_BOLT:
			n = MIN( 5, maxclip - *clip );  // a stripper clip
			break;
		case VRR_PUMP:
		case VRR_SHELLS:
			n = MIN( 1, maxclip - *clip );
			break;
		default:
			n = maxclip - *clip;
			vrp.magOut[weapon] = qfalse;
			break;
		}
		n = MIN( n, *reserve );
		if ( n > 0 ) {
			*clip += n;
			*reserve -= n;
		}
		if ( cls == VRR_ENBLOC || cls == VRR_REVOLVER ) {
			vrp.chambered[weapon] = qtrue;  // the Garand's bolt closes on the clip
		}
		vrp.heldItem = 0;
		G_VR_Haptic( off, 0.8f, 60 );
		G_VR_Haptic( dom, 0.6f, 60 );
	}

	// rack the slide / work the bolt / pump
	if ( cls == VRR_MAG || cls == VRR_BOLT || cls == VRR_PUMP || cls == VRR_SHELLS ) {
		// the measured handle, or (except for a pump's forestock) anywhere close to the gun
		// hand, where slides and bolts are
		qboolean nearAction = Distance( offHand, chargePoint ) < VR_CHARGE_RADIUS ||
							  ( cls != VRR_PUMP && Distance( offHand, hand ) < VR_ACTION_RADIUS );

		if ( pressed && !used && !vrp.heldItem && !vrp.chargeHeld && nearAction ) {
			vrp.chargeHeld = qtrue;
			vrp.chargeDone = qfalse;
			VectorCopy( offHand, vrp.chargeStart );
			G_VR_Haptic( off, 0.3f, 20 );
			used = qtrue;
		}
		if ( vrp.chargeHeld ) {
			float pull;

			VectorSubtract( offHand, vrp.chargeStart, d );
			pull = DotProduct( d, chargeDir ) / geom.travel;
			gVR->chargePull = pull < 0 ? 0 : ( pull > 1 ? 1 : pull );
			if ( pull >= VR_CHARGE_PULL && !vrp.chargeDone ) {
				vrp.chargeDone = qtrue;
				if ( *clip > 0 ) {
					vrp.chambered[weapon] = qtrue;
				}
				G_VR_Haptic( off, 0.9f, 50 );
				G_VR_Haptic( dom, 0.5f, 50 );
			}
			if ( released ) {
				vrp.chargeHeld = qfalse;
				gVR->chargePull = 0;
			}
		}
	} else {
		vrp.chargeHeld = qfalse;
	}

	gVR->chambered = vrp.chambered[weapon];
	gVR->magOut = vrp.magOut[weapon];
	gVR->heldItem = vrp.heldItem;
	gVR->chargeHeld = vrp.chargeHeld;
	return used || vrp.heldItem || vrp.chargeHeld;
}

/*
=================
G_VR_FindBody

A dead body whose box (plus a margin) the hand is in.
=================
*/
// grab assist (g_vrGrabAssist): the reach around things, and the nearest one is taken
static float G_VR_AssistMargin( float base ) {
	trap_Cvar_Update( &g_vrGrabAssist );
	switch ( g_vrGrabAssist.integer ) {
	case 0: return base;
	case 2: return base * 2.4f + 6.0f;
	default: return base * 1.6f + 2.0f;
	}
}

// how far a point is outside a box (0 inside)
static float G_VR_BoxDist( const vec3_t p, const vec3_t mins, const vec3_t maxs ) {
	float d = 0, x;
	int k;

	for ( k = 0; k < 3; k++ ) {
		x = p[k] < mins[k] ? mins[k] - p[k] : ( p[k] > maxs[k] ? p[k] - maxs[k] : 0 );
		d += x * x;
	}
	return sqrt( d );
}

static gentity_t *G_VR_FindBody( const vec3_t hand ) {
	gentity_t *e, *best = NULL;
	float margin = G_VR_AssistMargin( VR_GRAB_MARGIN ), d, bestDist = 1e9f;
	int i;

	for ( i = 1; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || !e->client || e->health > 0 || !e->r.linked ) {
			continue;
		}
		d = G_VR_BoxDist( hand, e->r.absmin, e->r.absmax );
		if ( d <= margin && d < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	return best;
}


// a carried body hangs from the hand: an upright box below it, so it covers the holder
static const vec3_t vrCarryMins = { -12, -12, -54 };
static const vec3_t vrCarryMaxs = { 12, 12, 6 };

// move a carried body to the hand, stopping where the hand meets a wall
static void G_VR_PlaceBody( gentity_t *e, const vec3_t target ) {
	trace_t tr;

	trap_Trace( &tr, e->client->ps.origin, NULL, NULL, target, e->s.number, MASK_SOLID );
	if ( tr.allsolid ) {
		return;
	}
	VectorCopy( tr.endpos, e->client->ps.origin );
	VectorCopy( tr.endpos, e->r.currentOrigin );
	VectorCopy( tr.endpos, e->s.pos.trBase );
	VectorClear( e->client->ps.velocity );
	trap_LinkEntity( e );
}

static void G_VR_ReleaseBody( gentity_t *e, qboolean thrown ) {
	trace_t tr;
	int up;

	// back to the body's own box, out of anything it was held into
	VectorCopy( vrp.grabbedMins, e->r.mins );
	VectorCopy( vrp.grabbedMaxs, e->r.maxs );
	for ( up = 0; up <= 48; up += 8 ) {
		vec3_t p;
		VectorCopy( e->client->ps.origin, p );
		p[2] += up;
		trap_Trace( &tr, p, e->r.mins, e->r.maxs, p, e->s.number, MASK_SOLID );
		if ( !tr.startsolid ) {
			VectorCopy( p, e->client->ps.origin );
			VectorCopy( p, e->r.currentOrigin );
			VectorCopy( p, e->s.pos.trBase );
			break;
		}
	}
	trap_LinkEntity( e );

	e->client->ps.pm_type = vrp.grabbedPmType;
	// thrown, or just let go: either way it falls (dead bodies don't move by themselves)
	{
		vrp.thrown = e->s.number;
		VectorScale( vrp.handVel, thrown ? VR_THROW_SCALE : 0.5f, vrp.thrownVel );
		if ( VectorLength( vrp.thrownVel ) > VR_THROW_MAX ) {
			VectorNormalize( vrp.thrownVel );
			VectorScale( vrp.thrownVel, VR_THROW_MAX, vrp.thrownVel );
		}
		vrp.thrownTime = 0;
		e->client->ps.pm_type = PM_FREEZE;  // we fly it until it lands
	}
}

// a thrown body: fly it under gravity until it settles
static void G_VR_FlyBody( float dt ) {
	gentity_t *e = &g_entities[vrp.thrown];
	vec3_t target;
	trace_t tr;

	if ( !e->inuse || !e->client || e->health > 0 ) {
		vrp.thrown = -1;
		return;
	}
	vrp.thrownTime += (int)( dt * 1000 );
	vrp.thrownVel[2] -= g_gravity.value * dt;
	VectorMA( e->client->ps.origin, dt, vrp.thrownVel, target );
	trap_Trace( &tr, e->client->ps.origin, e->r.mins, e->r.maxs, target, e->s.number, MASK_SOLID );
	if ( !tr.allsolid ) {
		VectorCopy( tr.endpos, e->client->ps.origin );
		VectorCopy( tr.endpos, e->r.currentOrigin );
		VectorCopy( tr.endpos, e->s.pos.trBase );
		trap_LinkEntity( e );
	}
	if ( tr.fraction < 1.0f ) {
		if ( tr.plane.normal[2] > 0.7f || tr.allsolid || vrp.thrownTime > 4000 ) {
			e->client->ps.pm_type = vrp.grabbedPmType;  // landed
			VectorClear( e->client->ps.velocity );
			vrp.thrown = -1;
			return;
		}
		// glance off walls
		VectorMA( vrp.thrownVel, -1.3f * DotProduct( vrp.thrownVel, tr.plane.normal ), tr.plane.normal, vrp.thrownVel );
		VectorScale( vrp.thrownVel, 0.5f, vrp.thrownVel );
	}
}

/*
=================
G_VR_GrabLiving

The off hand grabbing someone alive: an enemy soldier taken from behind is a silent
takedown (and then a body in the hand), anyone else is shoved back.
Returns the body if it was a takedown.
=================
*/
#define VR_GRAB_LIVING_MARGIN   8.0f

static qboolean G_VR_TakedownChar( int aiChar ) {
	switch ( aiChar ) {
	case AICHAR_SOLDIER:
	case AICHAR_ELITEGUARD:
	case AICHAR_BLACKGUARD:
	case AICHAR_MERCENARY:
	case AICHAR_TRENCH:
		return qtrue;
	default:
		return qfalse;
	}
}

static qboolean vrLivingTouched;    // the last G_VR_GrabLiving found someone

static gentity_t *G_VR_GrabLiving( gentity_t *ent, const vec3_t hand, int off ) {
	gentity_t *e;
	vec3_t fwd, toMe, push;
	int i, k;

	vrLivingTouched = qfalse;
	for ( i = 1; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e == ent || !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 || !e->r.linked ) {
			continue;
		}
		for ( k = 0; k < 3; k++ ) {
			if ( hand[k] < e->r.absmin[k] - VR_GRAB_LIVING_MARGIN || hand[k] > e->r.absmax[k] + VR_GRAB_LIVING_MARGIN ) {
				break;
			}
		}
		if ( k < 3 ) {
			continue;
		}
		vrLivingTouched = qtrue;

		AngleVectors( e->client->ps.viewangles, fwd, NULL, NULL );
		fwd[2] = 0;
		VectorNormalize( fwd );
		VectorSubtract( ent->r.currentOrigin, e->r.currentOrigin, toMe );
		toMe[2] = 0;
		VectorNormalize( toMe );

		if ( G_VR_TakedownChar( e->aiCharacter ) && DotProduct( fwd, toMe ) < -0.3f ) {
			// from behind: over before they know it
			G_Damage( e, ent, ent, NULL, NULL, 10000, DAMAGE_NO_PROTECTION, MOD_KNIFE_STEALTH );
			G_VR_Haptic( off, 1.0f, 120 );
			return e->health <= 0 ? e : NULL;
		}

		// face to face: a shove
		VectorScale( toMe, -350.0f, push );
		push[2] = 120.0f;
		VectorAdd( e->client->ps.velocity, push, e->client->ps.velocity );
		e->client->ps.pm_time = 200;
		e->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
		G_Damage( e, ent, ent, NULL, NULL, 2, DAMAGE_NO_KNOCKBACK, MOD_KICKED );
		G_VR_Haptic( off, 0.6f, 60 );
		return NULL;
	}
	return NULL;
}

/*
=================
G_VR_GrabItem / G_VR_HandUse

The off hand closing on things: an item is picked up, and a door, button, lever, locker
or friendly is used the way the use button would, by what the hand is reaching for.
=================
*/
#define VR_ITEM_MARGIN      6.0f

qboolean g_vrHandUse;           // Cmd_Activate_f traces g_vrHandFrom -> g_vrHandTo instead
vec3_t g_vrHandFrom, g_vrHandTo;

static gentity_t *G_VR_FindItem( const vec3_t hand ) {
	gentity_t *e, *best = NULL;
	float margin = G_VR_AssistMargin( VR_ITEM_MARGIN ), d, bestDist = 1e9f;
	int i;

	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || e->s.eType != ET_ITEM || !e->r.linked || !e->item ) {
			continue;
		}
		d = G_VR_BoxDist( hand, e->r.absmin, e->r.absmax );
		if ( d <= margin && d < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	return best;
}

static qboolean G_VR_GrabItem( gentity_t *ent, const vec3_t hand ) {
	gentity_t *e = G_VR_FindItem( hand );
	trace_t tr;

	if ( e ) {
		memset( &tr, 0, sizeof( tr ) );
		e->active = qtrue;      // as if the use button were pressed on it
		Touch_Item( e, ent, &tr );
		if ( e->inuse ) {
			e->active = qfalse;
		}
		return qtrue;
	}
	return qfalse;
}

static void G_VR_HandUse( gentity_t *ent, const vec3_t hand ) {
	vec3_t eye, dir;

	G_VR_Eye( ent, eye );
	VectorSubtract( hand, eye, dir );
	if ( VectorNormalize( dir ) < 1.0f ) {
		return;
	}
	// a little behind the hand to a little past it, along the reach
	VectorMA( hand, -6.0f, dir, g_vrHandFrom );
	VectorMA( hand, 14.0f, dir, g_vrHandTo );
	g_vrHandUse = qtrue;
	Cmd_Activate_f( ent );
	g_vrHandUse = qfalse;
}

/*
=================
G_VR_FlatGrab

Without a headset (g_flatGrab): hold use on a dead body to take hold of it where the
crosshair is on it (its ragdoll hangs from that limb), it stays that far in front of the
view while use is held, attack throws it, letting go drops it.
=================
*/
#define VR_FLAT_REACH       80.0f

// the dead body under the crosshair, and where on it
static gentity_t *G_VR_FlatTarget( gentity_t *ent, vec3_t eye, vec3_t fwd, vec3_t at ) {
	trace_t tr;
	vec3_t end;
	gentity_t *e;

	VectorCopy( ent->client->ps.origin, eye );
	eye[2] += ent->client->ps.viewheight;
	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorMA( eye, VR_FLAT_REACH, fwd, end );
	trap_Trace( &tr, eye, NULL, NULL, end, ent->s.number, MASK_SOLID | CONTENTS_CORPSE | CONTENTS_BODY );
	if ( tr.entityNum > 0 && tr.entityNum < MAX_CLIENTS ) {
		e = &g_entities[tr.entityNum];
		if ( e->inuse && e->client && e->health <= 0 ) {
			VectorCopy( tr.endpos, at );
			return e;
		}
	}
	// grab assist: a body close to where the crosshair stops
	if ( g_vrGrabAssist.integer ) {
		e = G_VR_FindBody( tr.endpos );
		if ( e ) {
			VectorCopy( tr.endpos, at );
			return e;
		}
	}
	return NULL;
}

static void G_VR_FlatGrab( gentity_t *ent ) {
	usercmd_t *cmd = &ent->client->pers.cmd;
	qboolean use = ( cmd->buttons & BUTTON_ACTIVATE ) != 0;
	qboolean attack = ( cmd->buttons & BUTTON_ATTACK ) != 0;
	qboolean usePressed = use && !vrp.useWas, attackPressed = attack && !vrp.attackWas;
	vec3_t eye, fwd, at, hand;
	int now = ent->client->ps.commandTime;
	float dt = ( now - vrp.lastTime ) * 0.001f;

	G_VR_Init();
	trap_Cvar_Update( &g_flatGrab );
	vrp.useWas = use;
	vrp.attackWas = attack;

	if ( vrp.grabbed >= 0 ) {
		gentity_t *body = &g_entities[vrp.grabbed];

		VectorCopy( ent->client->ps.origin, eye );
		eye[2] += ent->client->ps.viewheight;
		AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
		VectorMA( eye, vrp.flatDist, fwd, hand );
		if ( hand[2] > eye[2] - 8 ) {
			hand[2] = eye[2] - 8;       // held in front, not over the head
		}
		if ( dt > 0.0f && dt < 0.2f ) {
			vec3_t v;
			VectorSubtract( hand, vrp.lastHand, v );
			VectorScale( v, 1.0f / dt, v );
			VectorScale( vrp.handVel, 0.5f, vrp.handVel );
			VectorMA( vrp.handVel, 0.5f, v, vrp.handVel );
		}
		VectorCopy( hand, vrp.lastHand );
		vrp.lastTime = now;

		if ( !body->inuse || !body->client || body->health > 0 || ent->client->ps.stats[STAT_HEALTH] <= 0 || !g_flatGrab.integer ) {
			if ( body->inuse ) {
				VectorCopy( vrp.grabbedMins, body->r.mins );
				VectorCopy( vrp.grabbedMaxs, body->r.maxs );
				body->client->ps.pm_type = vrp.grabbedPmType;
			}
			body->s.dl_intensity = 0;
			vrp.grabbed = -1;
		} else if ( attackPressed ) {
			// a throw, the way the view faces but never down into the floor
			if ( fwd[2] < 0.15f ) {
				fwd[2] = 0.15f;
				VectorNormalize( fwd );
			}
			VectorScale( fwd, 420.0f, vrp.handVel );
			vrp.handVel[2] += 140.0f;
			G_VR_ReleaseBody( body, qtrue );
			body->s.dl_intensity = 0;
			vrp.grabbed = -1;
		} else if ( !use ) {
			G_VR_ReleaseBody( body, qfalse );
			body->s.dl_intensity = 0;
			vrp.grabbed = -1;
		} else {
			G_VR_PlaceBody( body, hand );
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( hand, body->s.origin2 );
		}
	} else if ( usePressed && g_flatGrab.integer && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
		gentity_t *body = G_VR_FlatTarget( ent, eye, fwd, at );

		if ( body ) {
			vrp.grabbed = body->s.number;
			vrp.flatDist = Distance( eye, at );
			vrp.flatDist = vrp.flatDist < 40 ? 40 : ( vrp.flatDist > 64 ? 64 : vrp.flatDist );
			VectorClear( vrp.grabOffset );
			VectorClear( vrp.handVel );
			VectorCopy( at, vrp.lastHand );
			vrp.lastTime = now;
			VectorCopy( body->r.mins, vrp.grabbedMins );
			VectorCopy( body->r.maxs, vrp.grabbedMaxs );
			VectorCopy( vrCarryMins, body->r.mins );
			VectorCopy( vrCarryMaxs, body->r.maxs );
			vrp.grabbedPmType = body->client->ps.pm_type == PM_FREEZE ? PM_DEAD : body->client->ps.pm_type;
			body->client->ps.pm_type = PM_FREEZE;
			// first the point that was grabbed, so the ragdoll takes it by the right limb
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( at, body->s.origin2 );
			if ( vrp.thrown == body->s.number ) {
				vrp.thrown = -1;
			}
		}
	}
	gVR->flatGrabbed = vrp.grabbed + 1;

	if ( vrp.thrown >= 0 && dt > 0 && dt < 0.2f ) {
		G_VR_FlyBody( dt );
	}
	if ( vrp.grabbed < 0 ) {
		vrp.lastTime = now;
	}
}

/*
=================
G_VR_GrabHint

After the cursor hints: without a headset, the hand hint on a body that can be picked up;
with one, gVR->canGrab while the off hand is on something it can take (the cgame buzzes
and shows it, vr_grabHint).
=================
*/
void G_VR_GrabHint( gentity_t *ent ) {
	vec3_t eye, fwd, at, hand;

	if ( !gVR || !ent->client || ent->aiCharacter || ent->s.number != 0 ) {
		return;
	}
	gVR->canGrab = 0;
	if ( ent->client->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}
	G_VR_Init();
	if ( !G_VR_IsPlayer( ent ) ) {
		if ( g_flatGrab.integer && vrp.grabbed < 0 && G_VR_FlatTarget( ent, eye, fwd, at ) ) {
			ent->client->ps.serverCursorHint = HINT_ACTIVATE;
			ent->client->ps.serverCursorHintVal = 0;
		}
		return;
	}
	if ( vrp.grabbed < 0 && !vrp.heldItem ) {
		G_VR_HandWorld( ent, gVR->rightHanded ? VR_HAND_LEFT : VR_HAND_RIGHT, hand );
		gVR->canGrab = G_VR_FindBody( hand ) || G_VR_FindItem( hand );
	}
}

/*
=================
G_VR_PlayerThink

After every pmove of the headset player.
=================
*/
void G_VR_PlayerThink( gentity_t *ent ) {
	int dom, off, now;
	vec3_t offHand;
	qboolean down, pressed, released, used;
	float dt;

	if ( !G_VR_IsPlayer( ent ) ) {
		if ( gVR && ent->client && !ent->aiCharacter && ent->s.number == 0 ) {
			G_VR_FlatGrab( ent );       // RealRTCW-VR: bodies picked up without a headset
		}
		return;
	}
	G_VR_Init();
	gVR->flatGrabbed = 0;

	dom = gVR->rightHanded ? VR_HAND_RIGHT : VR_HAND_LEFT;
	off = !dom;
	G_VR_HandWorld( ent, off, offHand );

	now = ent->client->ps.commandTime;
	dt = ( now - vrp.lastTime ) * 0.001f;
	if ( dt > 0.0f && dt < 0.2f ) {
		vec3_t v;
		VectorSubtract( offHand, vrp.lastHand, v );
		VectorScale( v, 1.0f / dt, v );
		// smooth it a little, the throw speed comes from the last few frames
		VectorScale( vrp.handVel, 0.5f, vrp.handVel );
		VectorMA( vrp.handVel, 0.5f, v, vrp.handVel );
	} else {
		dt = 0;
	}
	VectorCopy( offHand, vrp.lastHand );

	// the weapon hand's speed too, for throwing grenades
	{
		vec3_t domHand, dv;
		G_VR_HandWorld( ent, dom, domHand );
		if ( dt > 0.0f ) {
			VectorSubtract( domHand, vrp.lastDom, dv );
			VectorScale( dv, 1.0f / dt, dv );
			VectorScale( vrp.domVel, 0.5f, vrp.domVel );
			VectorMA( vrp.domVel, 0.5f, dv, vrp.domVel );
		}
		VectorCopy( domHand, vrp.lastDom );
		if ( VectorLength( vrp.domVel ) >= VectorLength( vrp.throwVel ) || now - vrp.throwVelTime > 300 ) {
			VectorCopy( vrp.domVel, vrp.throwVel );
			vrp.throwVelTime = now;
		}
	}
	vrp.lastTime = now;

	down = gVR->gripDown[off] && gVR->handValid[off];
	pressed = down && !vrp.gripWas;
	released = !down && vrp.gripWas;
	vrp.gripWas = down;

	used = ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_Reload( ent, offHand, pressed, released, dom );

	// bodies
	if ( vrp.grabbed >= 0 ) {
		gentity_t *body = &g_entities[vrp.grabbed];
		if ( !body->inuse || !body->client || body->health > 0 ) {
			if ( body->inuse ) {
				VectorCopy( vrp.grabbedMins, body->r.mins );
				VectorCopy( vrp.grabbedMaxs, body->r.maxs );
			}
			body->s.dl_intensity = 0;
			vrp.grabbed = -1;
		} else if ( !down ) {
			G_VR_ReleaseBody( body, VectorLength( vrp.handVel ) > VR_THROW_SPEED );
			body->s.dl_intensity = 0;
			vrp.grabbed = -1;
		} else {
			vec3_t target;
			VectorAdd( offHand, vrp.grabOffset, target );
			G_VR_PlaceBody( body, target );
			// for every player's ragdoll: who holds the body, and where (cg_ragdoll.c)
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( offHand, body->s.origin2 );
		}
	} else if ( pressed && !used ) {
		gentity_t *body = NULL;
		// an item in the hand, a body, someone alive (takedown or shove), or something to use
		if ( ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_GrabItem( ent, offHand ) ) {
			G_VR_Haptic( off, 0.4f, 40 );
		} else {
			body = G_VR_FindBody( offHand );
			if ( !body && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
				body = G_VR_GrabLiving( ent, offHand, off );
				if ( !body && !vrLivingTouched ) {
					G_VR_HandUse( ent, offHand );
				}
			}
		}
		if ( body ) {
			vrp.grabbed = body->s.number;
			// held by the hand, hanging below it (a shield, see vrCarryMins)
			VectorClear( vrp.grabOffset );
			VectorCopy( body->r.mins, vrp.grabbedMins );
			VectorCopy( body->r.maxs, vrp.grabbedMaxs );
			VectorCopy( vrCarryMins, body->r.mins );
			VectorCopy( vrCarryMaxs, body->r.maxs );
			vrp.grabbedPmType = body->client->ps.pm_type == PM_FREEZE ? PM_DEAD : body->client->ps.pm_type;
			body->client->ps.pm_type = PM_FREEZE;   // stays put in the hand instead of falling
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( offHand, body->s.origin2 );
			if ( vrp.thrown == body->s.number ) {
				vrp.thrown = -1;
			}
			G_VR_Haptic( off, 0.7f, 50 );
		}
	}
	gVR->grabbedEntity = vrp.grabbed;

	if ( vrp.thrown >= 0 && dt > 0 ) {
		G_VR_FlyBody( dt );
	}
}

// a headset player: their gun is aimed by their hands, not the flat game's view
qboolean G_VR_Aiming( gentity_t *ent ) {
	return G_VR_IsPlayer( ent );
}

/*
=================
G_VR_GrenadeThrow

Grenades, dynamite and the rest leave the hand that threw them, as fast as it was moving
when the trigger was let go: a real throw carries, a gentle release just drops it.
=================
*/
#define VR_GRENADE_SCALE    1.6f    // a throw carries a little further than the hand moved
#define VR_GRENADE_MAX      1100.0f

void G_VR_GrenadeThrow( gentity_t *ent, int weapon, vec3_t start, vec3_t dir ) {
	vec3_t eye, hand, vel;
	trace_t tr;
	float speed, top;
	int dom;

	if ( !ent || !G_VR_IsPlayer( ent ) ) {
		return;
	}
	G_VR_Init();
	dom = gVR->rightHanded ? VR_HAND_RIGHT : VR_HAND_LEFT;
	if ( !gVR->handValid[dom] ) {
		return;
	}
	G_VR_Eye( ent, eye );
	G_VR_HandWorld( ent, dom, hand );
	// from the eye, so a hand through a wall can't throw on the far side
	trap_Trace( &tr, eye, tv( -4, -4, -4 ), tv( 4, 4, 4 ), hand, ent->s.number, MASK_MISSILESHOT );
	VectorCopy( tr.endpos, start );

	VectorCopy( vrp.throwVel, vel );
	speed = VectorLength( vel );
	top = weapon == WP_DYNAMITE ? 600.0f : VR_GRENADE_MAX;
	if ( speed > VR_THROW_SPEED ) {
		VectorScale( vel, VR_GRENADE_SCALE, vel );
		speed *= VR_GRENADE_SCALE;
		if ( speed > top ) {
			VectorScale( vel, top / speed, vel );
		}
	}
	VectorCopy( vel, dir );
	if ( trap_Cvar_VariableIntegerValue( "vr_debug" ) == 5 ) {
		G_Printf( "VR grenade: hand moving %.0f, thrown at %.0f\n", speed, VectorLength( dir ) );
	}
}
