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
//
// Co-op: every headset player is handled separately. Their hands, buttons and settings
// arrive in each usercmd (vr* fields, see q_shared.h), and the result goes back in
// playerState vrState / vrGrab for their cgame to draw and for pmove on both sides.

#include "g_local.h"

#define VR_CHEST_RADIUS     18.0f   // ~45cm around the chest counts as "the pouch"
#define VR_CHEST_DROP       12.0f   // chest is this far below the eyes
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
	float       chargePull;

	int         lastTime;
	vec3_t      lastHand;
	vec3_t      handVel;

	int         grabbed;            // entity number, -1 none
	vec3_t      grabOffset;
	int         grabbedPmType;
	vec3_t      grabbedMins, grabbedMaxs;   // the body's own box, while it hangs from the hand

	int         thrown;             // body flying after a throw, -1 none
	vec3_t      thrownVel;
	int         thrownTime;

	qboolean    knifeValid;         // knife tip on the previous usercmd
	vec3_t      knifeTip;
	int         knifeTime;
	int         knifeNextHit;
} vrPlayer_t;

static vrPlayer_t vrPlayers[MAX_CLIENTS];

static qboolean G_VR_IsPlayer( gentity_t *ent );
static vrPlayer_t *G_VR_Init( gentity_t *ent );

/*
=================
G_VR_SpreadScale

Multiplier for a player's bullet spread when aiming with a tracked controller (their
own "shooting accuracy" setting).
=================
*/
float G_VR_SpreadScale( gentity_t *ent ) {
	if ( !G_VR_IsPlayer( ent ) ) {
		return 1.0f;
	}
	return BG_VR_SpreadScale( ent->client->pers.cmd.vrFlags );
}

void G_VR_Reset( void ) {
	memset( vrPlayers, 0, sizeof( vrPlayers ) );
}

static qboolean G_VR_IsPlayer( gentity_t *ent ) {
	return ent->client && !ent->aiCharacter && ent->s.number < MAX_CLIENTS &&
		   ( ent->client->pers.cmd.vrFlags & VRF_ACTIVE );
}

static vrPlayer_t *G_VR_Init( gentity_t *ent ) {
	vrPlayer_t *vrp = &vrPlayers[ent->s.number];
	int i;

	if ( vrp->initialised ) {
		return vrp;
	}
	memset( vrp, 0, sizeof( *vrp ) );
	for ( i = 0; i < WP_NUM_WEAPONS; i++ ) {
		vrp->chambered[i] = qtrue;
	}
	vrp->weapon = -1;
	vrp->grabbed = vrp->thrown = -1;
	vrp->lastEject = ent->client->pers.cmd.vrEject;
	vrp->initialised = qtrue;
	return vrp;
}

// a buzz in one of the player's controllers (their cgame watches the counters)
static void G_VR_Haptic( gentity_t *ent, int hand, float amplitude, int msec ) {
	int shift = hand == VR_HAND_LEFT ? VRS_PULSE_L_SHIFT : VRS_PULSE_R_SHIFT;
	int *st = &ent->client->ps.vrState;
	int n = ( ( *st >> shift ) + 1 ) & 15;

	(void)amplitude;
	(void)msec;
	*st = ( *st & ~( 15 << shift ) ) | ( n << shift );
}

static int G_VR_DomHand( gentity_t *ent ) {
	return ( ent->client->pers.cmd.vrFlags & VRF_RIGHTHANDED ) ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

static qboolean G_VR_HandValid( gentity_t *ent, int hand ) {
	return ( ent->client->pers.cmd.vrFlags & ( hand == VR_HAND_LEFT ? VRF_HAND_LEFT : VRF_HAND_RIGHT ) ) != 0;
}

// eye position the view used
static void G_VR_Eye( gentity_t *ent, vec3_t eye ) {
	int eh = ent->client->pers.cmd.vrEyeHeight;

	VectorCopy( ent->r.currentOrigin, eye );
	eye[2] += eh > 0 ? eh - 64 : ent->client->ps.viewheight;
}

// a tracked hand in the world (not clipped against walls, for interactions)
static void G_VR_HandWorld( gentity_t *ent, int hand, vec3_t out ) {
	const short *h = ent->client->pers.cmd.vrHand[hand];
	vec3_t offset, eye;
	float yaw = DEG2RAD( SHORT2ANGLE( ent->client->ps.delta_angles[YAW] ) );
	float s = sin( yaw ), c = cos( yaw );

	G_VR_Eye( ent, eye );
	VectorSet( offset, h[0] * 0.125f, h[1] * 0.125f, h[2] * 0.125f );
	out[0] = eye[0] + offset[0] * c - offset[1] * s;
	out[1] = eye[1] + offset[0] * s + offset[1] * c;
	out[2] = eye[2] + offset[2];
}

// the gun's aim in the world: pitch and yaw from the usercmd angles, roll from the hand
static void G_VR_AimAngles( gentity_t *ent, vec3_t aim ) {
	VectorSet( aim, ent->client->ps.viewangles[PITCH], ent->client->ps.viewangles[YAW],
			   SHORT2ANGLE( ent->client->pers.cmd.vrRoll ) );
}

// hand reloading state -> playerState, for the player's cgame and for pmove
static void G_VR_PublishState( gentity_t *ent, vrPlayer_t *vrp, int weapon, qboolean manual, float pull ) {
	int st = ent->client->ps.vrState & ( ( 255 << VRS_DROP_SHIFT ) | ( 15 << VRS_PULSE_L_SHIFT ) | ( 15 << VRS_PULSE_R_SHIFT ) );
	int p = (int)( pull * 255.0f );

	if ( manual ) {
		st |= VRS_MANUAL;
	}
	if ( !manual || vrp->chambered[weapon] ) {
		st |= VRS_CHAMBERED;
	}
	if ( manual && vrp->magOut[weapon] ) {
		st |= VRS_MAGOUT;
	}
	st |= ( vrp->heldItem & 3 ) << VRS_HELD_SHIFT;
	if ( vrp->chargeHeld ) {
		st |= VRS_CHARGEHELD;
		st |= ( p < 0 ? 0 : ( p > 255 ? 255 : p ) ) << VRS_PULL_SHIFT;
	}
	ent->client->ps.vrState = st;
}

static void G_VR_MagDropped( gentity_t *ent ) {
	int *st = &ent->client->ps.vrState;
	int n = ( ( *st >> VRS_DROP_SHIFT ) + 1 ) & 255;
	*st = ( *st & ~( 255 << VRS_DROP_SHIFT ) ) | ( n << VRS_DROP_SHIFT );
}

static qboolean G_VR_Manual( gentity_t *ent, int weapon ) {
	return ( ent->client->pers.cmd.vrFlags & VRF_MANUAL ) && BG_VR_ReloadClass( weapon ) != VRR_NONE;
}

/*
=================
G_VR_UpdateWeaponState

Before every pmove: tell pmove (and the player's cgame) whether the current weapon is
reloaded by hand and whether it can fire.
=================
*/
void G_VR_UpdateWeaponState( gentity_t *ent ) {
	vrPlayer_t *vrp;
	int weapon = ent->client->ps.weapon;

	if ( !G_VR_IsPlayer( ent ) ) {
		ent->client->ps.vrState = 0;
		ent->client->ps.vrGrab = 0;
		return;
	}
	vrp = G_VR_Init( ent );

	if ( weapon != vrp->weapon ) {
		// switching weapons puts whatever the off hand held back
		vrp->heldItem = 0;
		vrp->chargeHeld = qfalse;
		vrp->weapon = weapon;
	}
	if ( weapon < 0 || weapon >= WP_NUM_WEAPONS ) {
		weapon = 0;
	}
	G_VR_PublishState( ent, vrp, weapon, G_VR_Manual( ent, weapon ), vrp->chargePull );
}

/*
=================
G_VR_OnFire

A shot left a player's gun.
=================
*/
void G_VR_OnFire( gentity_t *ent ) {
	vrPlayer_t *vrp;
	int weapon, clip;

	if ( !G_VR_IsPlayer( ent ) ) {
		return;
	}
	weapon = ent->client->ps.weapon;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS || !G_VR_Manual( ent, weapon ) ) {
		return;
	}
	vrp = G_VR_Init( ent );
	clip = ent->client->ps.ammoclip[BG_FindClipForWeapon( weapon )];

	switch ( BG_VR_ReloadClass( weapon ) ) {
	case VRR_BOLT:
	case VRR_PUMP:
		vrp->chambered[weapon] = qfalse;    // work the action for the next round
		break;
	case VRR_MAG:
	case VRR_SHELLS:
		if ( clip <= 0 ) {
			vrp->chambered[weapon] = qfalse;    // ran dry: slide / bolt locks back
		}
		break;
	case VRR_ENBLOC:
		if ( clip <= 0 ) {
			vrp->chambered[weapon] = qfalse;    // *ping*
			vrp->magOut[weapon] = qtrue;
			G_VR_MagDropped( ent );
		}
		break;
	default:
		break;
	}
	G_VR_PublishState( ent, vrp, weapon, qtrue, vrp->chargePull );
}

/*
=================
G_VR_Reload

Hand reloading for the current weapon. Returns qtrue if the off hand's grip press was
used by it.
=================
*/
static qboolean G_VR_Reload( gentity_t *ent, vrPlayer_t *vrp, const vec3_t offHand, qboolean pressed, qboolean released, int dom ) {
	playerState_t *ps = &ent->client->ps;
	int weapon = ps->weapon;
	int cls = BG_VR_ReloadClass( weapon );
	int clipIndex, ammoIndex, maxclip;
	int *clip, *reserve;
	vrWeaponGeom_t geom;
	vec3_t hand, aim, origin, axis[3], magPoint, chargePoint, chargeDir, eye, chest, d;
	qboolean used = qfalse, wantsAmmo;
	int off = !dom, n;

	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS || !G_VR_Manual( ent, weapon ) ) {
		vrp->heldItem = 0;
		vrp->chargeHeld = qfalse;
		return qfalse;
	}
	if ( ps->weaponstate == WEAPON_RAISING || ps->weaponstate == WEAPON_DROPPING ||
		 ps->weaponstate == WEAPON_DROPPING_TORELOAD || ps->weaponstate == WEAPON_RAISING_TORELOAD ) {
		return qfalse;
	}

	clipIndex = BG_FindClipForWeapon( weapon );
	ammoIndex = BG_FindAmmoForWeapon( weapon );
	maxclip = ammoTable[weapon].maxclip;
	clip = &ps->ammoclip[clipIndex];
	reserve = &ps->ammo[ammoIndex];

	// where the gun and its parts are, exactly as cgame draws them
	BG_VR_WeaponGeom( weapon, qfalse, &geom );
	G_VR_HandWorld( ent, dom, hand );
	G_VR_AimAngles( ent, aim );
	BG_VR_WeaponFrame( &geom, hand, aim, NULL, origin, axis );
	BG_VR_ModelPoint( origin, axis, geom.mag, magPoint );
	BG_VR_ModelPoint( origin, axis, geom.charge, chargePoint );
	BG_VR_ModelDir( axis, geom.chargeDir, chargeDir );
	VectorNormalize( chargeDir );

	G_VR_Eye( ent, eye );
	VectorCopy( eye, chest );
	chest[2] -= VR_CHEST_DROP;

	// drop the magazine / open the cylinder (grip tap on the gun hand)
	if ( ent->client->pers.cmd.vrEject != vrp->lastEject ) {
		vrp->lastEject = ent->client->pers.cmd.vrEject;
		if ( ( cls == VRR_MAG || cls == VRR_ENBLOC || cls == VRR_REVOLVER ) && !vrp->magOut[weapon] ) {
			if ( cls == VRR_MAG ) {
				// a round left in the chamber means no need to rack after the new mag
				vrp->chambered[weapon] = *clip > 0 && vrp->chambered[weapon];
			} else if ( cls == VRR_ENBLOC ) {
				vrp->chambered[weapon] = qfalse;
			}
			*reserve += *clip;      // forgiving: the rounds aren't lost
			*clip = 0;
			vrp->magOut[weapon] = qtrue;
			G_VR_MagDropped( ent );
			G_VR_Haptic( ent, dom, 0.5f, 40 );
		}
	}

	// take a magazine / clip / shell from the chest
	if ( cls == VRR_MAG || cls == VRR_ENBLOC || cls == VRR_REVOLVER ) {
		wantsAmmo = vrp->magOut[weapon];
	} else {
		wantsAmmo = *clip < maxclip;
	}
	if ( pressed && !vrp->heldItem && wantsAmmo && *reserve > 0 && Distance( offHand, chest ) < VR_CHEST_RADIUS ) {
		vrp->heldItem = ( cls == VRR_PUMP || cls == VRR_SHELLS ) ? 2 : 1;
		G_VR_Haptic( ent, off, 0.4f, 30 );
		used = qtrue;
	}
	if ( released && vrp->heldItem ) {
		vrp->heldItem = 0;  // let go of it: back in the pouch
	}

	// push it in
	if ( vrp->heldItem && Distance( offHand, magPoint ) < VR_INSERT_RADIUS ) {
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
			vrp->magOut[weapon] = qfalse;
			break;
		}
		n = MIN( n, *reserve );
		if ( n > 0 ) {
			*clip += n;
			*reserve -= n;
		}
		if ( cls == VRR_ENBLOC || cls == VRR_REVOLVER ) {
			vrp->chambered[weapon] = qtrue;     // the bolt closes on the clip
		}
		vrp->heldItem = 0;
		G_VR_Haptic( ent, off, 0.8f, 60 );
		G_VR_Haptic( ent, dom, 0.6f, 60 );
	}

	// rack the slide / work the bolt / pump
	if ( cls == VRR_MAG || cls == VRR_BOLT || cls == VRR_PUMP || cls == VRR_SHELLS ) {
		// the measured handle, or (except for a pump's forestock) anywhere close to the gun
		// hand, where slides and bolts are
		qboolean nearAction = Distance( offHand, chargePoint ) < VR_CHARGE_RADIUS ||
							  ( cls != VRR_PUMP && Distance( offHand, hand ) < VR_ACTION_RADIUS );

		if ( pressed && !used && !vrp->heldItem && !vrp->chargeHeld && nearAction ) {
			vrp->chargeHeld = qtrue;
			vrp->chargeDone = qfalse;
			VectorCopy( offHand, vrp->chargeStart );
			G_VR_Haptic( ent, off, 0.3f, 20 );
			used = qtrue;
		}
		if ( vrp->chargeHeld ) {
			float pull;

			VectorSubtract( offHand, vrp->chargeStart, d );
			pull = DotProduct( d, chargeDir ) / geom.travel;
			vrp->chargePull = pull < 0 ? 0 : ( pull > 1 ? 1 : pull );
			if ( pull >= VR_CHARGE_PULL && !vrp->chargeDone ) {
				vrp->chargeDone = qtrue;
				if ( *clip > 0 ) {
					vrp->chambered[weapon] = qtrue;
				}
				G_VR_Haptic( ent, off, 0.9f, 50 );
				G_VR_Haptic( ent, dom, 0.5f, 50 );
			}
			if ( released ) {
				vrp->chargeHeld = qfalse;
				vrp->chargePull = 0;
			}
		}
	} else {
		vrp->chargeHeld = qfalse;
	}

	G_VR_PublishState( ent, vrp, weapon, qtrue, vrp->chargePull );
	return used || vrp->heldItem || vrp->chargeHeld;
}

/*
=================
G_VR_FindBody

A dead body whose box (plus a margin) the hand is in.
=================
*/
static gentity_t *G_VR_FindBody( gentity_t *self, const vec3_t hand ) {
	gentity_t *e;
	int i, k;

	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e == self || !e->inuse || !e->client || !e->aiCharacter || e->health > 0 || !e->r.linked ) {
			continue;
		}
		for ( k = 0; k < 3; k++ ) {
			if ( hand[k] < e->r.absmin[k] - VR_GRAB_MARGIN || hand[k] > e->r.absmax[k] + VR_GRAB_MARGIN ) {
				break;
			}
		}
		if ( k == 3 ) {
			return e;
		}
	}
	return NULL;
}

// is anyone else holding or throwing this body
static qboolean G_VR_BodyTaken( int body, int self ) {
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( i != self && vrPlayers[i].initialised &&
			 ( vrPlayers[i].grabbed == body || vrPlayers[i].thrown == body ) ) {
			return qtrue;
		}
	}
	return qfalse;
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

static void G_VR_ReleaseBody( vrPlayer_t *vrp, gentity_t *e, qboolean thrown ) {
	trace_t tr;
	int up;

	// back to the body's own box, out of anything it was held into
	VectorCopy( vrp->grabbedMins, e->r.mins );
	VectorCopy( vrp->grabbedMaxs, e->r.maxs );
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

	e->client->ps.pm_type = vrp->grabbedPmType;
	// thrown, or just let go: either way it falls (dead bodies don't move by themselves)
	{
		vrp->thrown = e->s.number;
		VectorScale( vrp->handVel, thrown ? VR_THROW_SCALE : 0.5f, vrp->thrownVel );
		if ( VectorLength( vrp->thrownVel ) > VR_THROW_MAX ) {
			VectorNormalize( vrp->thrownVel );
			VectorScale( vrp->thrownVel, VR_THROW_MAX, vrp->thrownVel );
		}
		vrp->thrownTime = 0;
		e->client->ps.pm_type = PM_FREEZE;  // we fly it until it lands
	}
}

// a thrown body: fly it under gravity until it settles
static void G_VR_FlyBody( vrPlayer_t *vrp, float dt ) {
	gentity_t *e = &g_entities[vrp->thrown];
	vec3_t target;
	trace_t tr;

	if ( !e->inuse || !e->client || e->health > 0 ) {
		vrp->thrown = -1;
		return;
	}
	vrp->thrownTime += (int)( dt * 1000 );
	vrp->thrownVel[2] -= g_gravity.value * dt;
	VectorMA( e->client->ps.origin, dt, vrp->thrownVel, target );
	trap_Trace( &tr, e->client->ps.origin, e->r.mins, e->r.maxs, target, e->s.number, MASK_SOLID );
	if ( !tr.allsolid ) {
		VectorCopy( tr.endpos, e->client->ps.origin );
		VectorCopy( tr.endpos, e->r.currentOrigin );
		VectorCopy( tr.endpos, e->s.pos.trBase );
		trap_LinkEntity( e );
	}
	if ( tr.fraction < 1.0f ) {
		if ( tr.plane.normal[2] > 0.7f || tr.allsolid || vrp->thrownTime > 4000 ) {
			e->client->ps.pm_type = vrp->grabbedPmType;     // landed
			VectorClear( e->client->ps.velocity );
			vrp->thrown = -1;
			return;
		}
		// glance off walls
		VectorMA( vrp->thrownVel, -1.3f * DotProduct( vrp->thrownVel, tr.plane.normal ), tr.plane.normal, vrp->thrownVel );
		VectorScale( vrp->thrownVel, 0.5f, vrp->thrownVel );
	}
}

/*
=================
G_VR_PlayerThink

After every pmove of a headset player.
=================
*/
void G_VR_PlayerThink( gentity_t *ent ) {
	vrPlayer_t *vrp;
	int dom, off, now, grip;
	vec3_t offHand;
	qboolean down, pressed, released, used;
	float dt;

	if ( !G_VR_IsPlayer( ent ) ) {
		return;
	}
	vrp = G_VR_Init( ent );

	dom = G_VR_DomHand( ent );
	off = !dom;
	G_VR_HandWorld( ent, off, offHand );

	now = ent->client->ps.commandTime;
	dt = ( now - vrp->lastTime ) * 0.001f;
	if ( dt > 0.0f && dt < 0.2f ) {
		vec3_t v;
		VectorSubtract( offHand, vrp->lastHand, v );
		VectorScale( v, 1.0f / dt, v );
		// smooth it a little, the throw speed comes from the last few frames
		VectorScale( vrp->handVel, 0.5f, vrp->handVel );
		VectorMA( vrp->handVel, 0.5f, v, vrp->handVel );
	} else {
		dt = 0;
	}
	VectorCopy( offHand, vrp->lastHand );
	vrp->lastTime = now;

	grip = off == VR_HAND_LEFT ? VRF_GRIP_LEFT : VRF_GRIP_RIGHT;
	down = ( ent->client->pers.cmd.vrFlags & grip ) && G_VR_HandValid( ent, off );
	pressed = down && !vrp->gripWas;
	released = !down && vrp->gripWas;
	vrp->gripWas = down;

	used = ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_Reload( ent, vrp, offHand, pressed, released, dom );

	// bodies
	if ( vrp->grabbed >= 0 ) {
		gentity_t *body = &g_entities[vrp->grabbed];
		if ( !body->inuse || !body->client || body->health > 0 ) {
			if ( body->inuse ) {
				VectorCopy( vrp->grabbedMins, body->r.mins );
				VectorCopy( vrp->grabbedMaxs, body->r.maxs );
			}
			body->s.dl_intensity = 0;
			vrp->grabbed = -1;
		} else if ( !down ) {
			G_VR_ReleaseBody( vrp, body, VectorLength( vrp->handVel ) > VR_THROW_SPEED );
			body->s.dl_intensity = 0;
			vrp->grabbed = -1;
		} else {
			vec3_t target;
			VectorAdd( offHand, vrp->grabOffset, target );
			G_VR_PlaceBody( body, target );
			// for every player's ragdoll: who holds the body, and where (cg_ragdoll.c)
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( offHand, body->s.origin2 );
		}
	} else if ( pressed && !used ) {
		gentity_t *body = G_VR_FindBody( ent, offHand );
		if ( body && !G_VR_BodyTaken( body->s.number, ent->s.number ) ) {
			vrp->grabbed = body->s.number;
			// held by the hand, hanging below it (a shield, see vrCarryMins)
			VectorClear( vrp->grabOffset );
			VectorCopy( body->r.mins, vrp->grabbedMins );
			VectorCopy( body->r.maxs, vrp->grabbedMaxs );
			VectorCopy( vrCarryMins, body->r.mins );
			VectorCopy( vrCarryMaxs, body->r.maxs );
			vrp->grabbedPmType = body->client->ps.pm_type == PM_FREEZE ? PM_DEAD : body->client->ps.pm_type;
			body->client->ps.pm_type = PM_FREEZE;   // stays put in the hand instead of falling
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( offHand, body->s.origin2 );
			if ( vrp->thrown == body->s.number ) {
				vrp->thrown = -1;
			}
			G_VR_Haptic( ent, off, 0.7f, 50 );
		}
	}
	ent->client->ps.vrGrab = vrp->grabbed + 1;

	if ( vrp->thrown >= 0 && dt > 0 ) {
		G_VR_FlyBody( vrp, dt );
	}
}

/*
===============
G_VR_WeaponOrigin

World position of a headset player's gun hand. The hand is tracked relative to the
head, and the head sits at the eye height their cgame used for the view. Traced from
the eye so a hand pushed through a wall can't shoot or use things on the other side.
===============
*/
qboolean G_VR_WeaponOrigin( gentity_t *ent, vec3_t out ) {
	usercmd_t *cmd;
	vec3_t eye, end;
	int dom;
	trace_t tr;

	if ( !G_VR_IsPlayer( ent ) ) {
		return qfalse;
	}
	cmd = &ent->client->pers.cmd;
	dom = G_VR_DomHand( ent );
	if ( !G_VR_HandValid( ent, dom ) ) {
		return qfalse;
	}

	G_VR_Eye( ent, eye );
	G_VR_HandWorld( ent, dom, end );
	trap_Trace( &tr, eye, NULL, NULL, end, ent->s.number, MASK_SHOT );
	VectorCopy( tr.endpos, out );

	// from the hand out to the gun's muzzle, as drawn by cgame (not for the knife, its
	// blade is handled from the hand by G_VR_KnifeStab)
	if ( ( cmd->vrFlags & VRF_MUZZLE ) && ent->client->ps.weapon != WP_KNIFE && tr.fraction == 1.0f ) {
		vec3_t angles, axis[3], muzzle;

		G_VR_AimAngles( ent, angles );
		AnglesToAxis( angles, axis );
		VectorCopy( out, muzzle );
		VectorMA( muzzle, cmd->vrMuzzle[0] * 0.125f, axis[0], muzzle );
		VectorMA( muzzle, cmd->vrMuzzle[1] * 0.125f, axis[1], muzzle );
		VectorMA( muzzle, cmd->vrMuzzle[2] * 0.125f, axis[2], muzzle );
		trap_Trace( &tr, out, NULL, NULL, muzzle, ent->s.number, MASK_SHOT );
		VectorCopy( tr.endpos, out );
	}
	return qtrue;
}

/*
==============
G_VR_KnifeStab

With a headset the knife does damage when the blade is driven into something fast
enough. Every usercmd the blade is swept from where its tip was on the previous command
to where it is now, plus along its length.
==============
*/
#define VR_KNIFE_BLADE      18.0f   // hand to tip, game units (matches the first person knife)
#define VR_KNIFE_SPEED      1.2f    // metres per second needed to cut
#define VR_KNIFE_COOLDOWN   350     // msec between hits
#define VR_WORLD_SCALE      39.37f  // game units per metre

void G_VR_KnifeStab( gentity_t *ent ) {
	vrPlayer_t *vrp;
	vec3_t hand, fwd, tip, base;
	trace_t tr;
	float dist, speed;
	int dt;
	int now = ent->client->ps.commandTime;    // per usercmd, level.time only moves per server frame

	if ( !G_VR_IsPlayer( ent ) ) {
		return;
	}
	vrp = G_VR_Init( ent );
	if ( ent->client->ps.weapon != WP_KNIFE || ent->client->ps.stats[STAT_HEALTH] <= 0 ||
		 !G_VR_WeaponOrigin( ent, hand ) ) {
		vrp->knifeValid = qfalse;
		return;
	}

	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorMA( hand, VR_KNIFE_BLADE, fwd, tip );

	dt = now - vrp->knifeTime;
	if ( !vrp->knifeValid || dt <= 0 || dt > 200 ) {
		// first sample, or the frame came from a different timeline
		VectorCopy( tip, vrp->knifeTip );
		vrp->knifeTime = now;
		vrp->knifeValid = qtrue;
		return;
	}

	dist = Distance( tip, vrp->knifeTip );
	speed = ( dist / VR_WORLD_SCALE ) / ( dt * 0.001f );

	if ( speed >= VR_KNIFE_SPEED && now >= vrp->knifeNextHit ) {
		// the swept path of the tip, then the blade itself
		trap_Trace( &tr, vrp->knifeTip, NULL, NULL, tip, ent->s.number, MASK_SHOT );
		if ( tr.fraction == 1.0f ) {
			VectorMA( hand, 2.0f, fwd, base );
			trap_Trace( &tr, base, NULL, NULL, tip, ent->s.number, MASK_SHOT );
		}
		if ( tr.fraction < 1.0f && !( tr.surfaceFlags & SURF_NOIMPACT ) ) {
			G_KnifeHit( ent, &tr );
			vrp->knifeNextHit = now + VR_KNIFE_COOLDOWN;
			G_VR_Haptic( ent, G_VR_DomHand( ent ), 1.0f, 80 );
		}
	}

	VectorCopy( tip, vrp->knifeTip );
	vrp->knifeTime = now;
}
