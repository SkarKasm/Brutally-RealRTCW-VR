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

#define VR_CHEST_RADIUS     14.0f   // ~35cm around the pouch counts as reaching into it
#define VR_CHEST_DROP       22.0f   // the pouch is this far below the eyes (low on the chest,
									// clear of a shouldered rifle's bolt; cgame draws it there)
#define VR_INSERT_RADIUS    8.0f    // ~20cm around the magazine well / loading port
#define VR_CHARGE_RADIUS    5.5f    // ~14cm around the slide / bolt / pump handle (its trigger takes it)
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
	int         lastEjectOff;
	int         hipSince[2];        // dual wielding: when each hand reached its hip, 0 not there
	qboolean    gripWas;
	qboolean    triggerWas;         // the off hand's trigger: it works the bolt / slide / pump

	int         heldItem;
	qboolean    chargeHeld;
	qboolean    chargeDone;
	vec3_t      chargeStart;
	float       chargePull;

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
	qboolean    canGrabWas;

	int         thrown;             // body flying after a throw, -1 none
	vec3_t      thrownVel;
	int         thrownTime;

	vec3_t      touchLast[2];       // hands touching doors and buttons (G_VR_PushDoors)
	int         touching[2], touchNextUse[2];
	qboolean    touchValid;

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

qboolean G_VR_Player( gentity_t *ent ) {
	return ent && G_VR_IsPlayer( ent );
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
	vrp->lastEjectOff = ent->client->pers.cmd.vrEjectOff;
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
	eye[0] += ent->client->pers.cmd.vrLean[0] * 0.125f;     // the head leaning out over the body (cg_view.c)
	eye[1] += ent->client->pers.cmd.vrLean[1] * 0.125f;
}

// a point relative to the head (play space, 1/8 units, like vrHand) in the world
static void G_VR_PlayWorld( gentity_t *ent, const short *h, vec3_t out ) {
	vec3_t offset, eye;
	float yaw = DEG2RAD( SHORT2ANGLE( ent->client->ps.delta_angles[YAW] ) );
	float s = sin( yaw ), c = cos( yaw );

	G_VR_Eye( ent, eye );
	VectorSet( offset, h[0] * 0.125f, h[1] * 0.125f, h[2] * 0.125f );
	out[0] = eye[0] + offset[0] * c - offset[1] * s;
	out[1] = eye[1] + offset[0] * s + offset[1] * c;
	out[2] = eye[2] + offset[2];
}

// the off hand holds the gun alone (a grip tap on a two handed gun, cl_vr.c)
static qboolean G_VR_GunInOff( gentity_t *ent ) {
	return ( ent->client->pers.cmd.vrFlags2 & VRF2_GUN_IN_OFF ) && !BG_DualActive( &ent->client->ps );
}

// the guns and hands drawn this size
static float G_VR_GunScale( gentity_t *ent ) {
	int sc = ent->client->pers.cmd.vrScale;
	return sc > 0 ? sc / 64.0f : 1.0f;
}

// a tracked hand in the world (not clipped against walls, for interactions)
static void G_VR_HandWorld( gentity_t *ent, int hand, vec3_t out ) {
	G_VR_PlayWorld( ent, ent->client->pers.cmd.vrHand[hand], out );
}

// where the gun is held in the world: the gun hand, or the off hand carrying it
static void G_VR_GunWorld( gentity_t *ent, int dom, vec3_t out ) {
	G_VR_PlayWorld( ent, G_VR_GunInOff( ent ) ? ent->client->pers.cmd.vrGun : ent->client->pers.cmd.vrHand[dom], out );
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
	return ( ent->client->pers.cmd.vrFlags & VRF_MANUAL ) && BG_VR_ReloadClass( weapon ) != VRR_NONE &&
		   !( ent->client->ps.persistant[PERS_DUAL] && BG_DualCapable( weapon ) );   // two guns: reloaded at the waist
}

/*
=================
G_VR_DualGuns

Dual wielding: the off hand's gun has its own magazine (BG_DUAL_LEFT_CLIP, which pmove fires
the off hand's shots from). Each hand's magazine button drops its gun's magazine, and a hand
held down at its hip reloads that gun.
=================
*/
#define VR_HIP_RELOAD_MS    450

void G_DualPutAway( playerState_t *ps ) {
	int w = ps->ammoclip[BG_DUAL_LEFT_WEAPON];

	if ( w > WP_NONE && w < WP_NUM_WEAPONS ) {
		ps->ammo[BG_FindAmmoForWeapon( w )] += ps->ammoclip[BG_DUAL_LEFT_CLIP];
	}
	ps->ammoclip[BG_DUAL_LEFT_CLIP] = 0;
	ps->ammoclip[BG_DUAL_LEFT_WEAPON] = 0;
}

static void G_VR_DualGuns( gentity_t *ent, vrPlayer_t *vrp, int dom ) {
	playerState_t *ps = &ent->client->ps;
	const usercmd_t *cmd = &ent->client->pers.cmd;
	int weapon = ps->weapon, h, *clip, *reserve, max, n;
	const float drop = 0.45f * 39.37f;     // 45cm under the eyes
	qboolean dual = BG_DualActive( ps );

	// the off hand's magazine belongs to the gun in that hand
	if ( !dual || ps->ammoclip[BG_DUAL_LEFT_WEAPON] != weapon ) {
		G_DualPutAway( ps );
		if ( dual ) {
			// a fresh gun comes up loaded
			ps->ammoclip[BG_DUAL_LEFT_WEAPON] = weapon;
			reserve = &ps->ammo[BG_FindAmmoForWeapon( weapon )];
			n = MIN( BG_MaxClip( ps, weapon ), *reserve );
			ps->ammoclip[BG_DUAL_LEFT_CLIP] = n;
			*reserve -= n;
		}
	}
	if ( !dual || ps->stats[STAT_HEALTH] <= 0 ) {
		if ( dual ) {
			vrp->lastEject = cmd->vrEject;      // (one gun: G_VR_Reload takes it)
		}
		vrp->lastEjectOff = cmd->vrEjectOff;
		vrp->hipSince[0] = vrp->hipSince[1] = 0;
		return;
	}

	reserve = &ps->ammo[BG_FindAmmoForWeapon( weapon )];
	max = BG_MaxClip( ps, weapon );

	for ( h = 0; h < 2; h++ ) {
		clip = h == dom ? &ps->ammoclip[BG_FindClipForWeapon( weapon )] : &ps->ammoclip[BG_DUAL_LEFT_CLIP];

		// magazine release
		if ( h == dom ? cmd->vrEject != vrp->lastEject : cmd->vrEjectOff != vrp->lastEjectOff ) {
			if ( *clip > 0 ) {
				*reserve += *clip;      // forgiving: the rounds aren't lost
				*clip = 0;
				G_VR_Haptic( ent, h, 0.5f, 40 );
			}
		}

		// down at the hip: a new magazine (hands are relative to the head)
		if ( !G_VR_HandValid( ent, h ) || cmd->vrHand[h][2] * 0.125f > -drop ) {
			vrp->hipSince[h] = 0;
			continue;
		}
		if ( !vrp->hipSince[h] ) {
			vrp->hipSince[h] = level.time;
		}
		if ( level.time - vrp->hipSince[h] >= VR_HIP_RELOAD_MS && *clip < max && *reserve > 0 ) {
			n = MIN( max - *clip, *reserve );
			*clip += n;
			*reserve -= n;
			G_AddEvent( ent, EV_FILL_CLIP, 0 );
			G_VR_Haptic( ent, h, 0.8f, 60 );
			vrp->hipSince[h] = level.time + 100000;     // once per visit to the hip
		}
	}
	vrp->lastEject = cmd->vrEject;
	vrp->lastEjectOff = cmd->vrEjectOff;
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
		ent->client->ps.vrGrab = ( ent->s.number < MAX_CLIENTS && vrPlayers[ent->s.number].initialised ) ?
								 vrPlayers[ent->s.number].grabbed + 1 : 0;
		if ( !ent->client->ps.vrGrab && ent->s.number < MAX_CLIENTS ) {
			ent->client->ps.vrGrab = G_VRX_PropHeld( ent ) + 1;    // a prop held up front
		}
		ent->s.vrFlags = 0;
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
	vec3_t workHand;
	qboolean used = qfalse, wantsAmmo, trigPressed, trigReleased, trigDown;
	int off = !dom, n;
	// the free hand's trigger works the action: the off hand's, or the gun hand's while the
	// off hand holds the gun alone (a grip tap, cl_vr.c)
	int work = G_VR_GunInOff( ent ) ? dom : off;

	trigDown = ( ent->client->pers.cmd.vrFlags2 & ( work == VR_HAND_LEFT ? VRF2_TRIG_LEFT : VRF2_TRIG_RIGHT ) ) &&
			   G_VR_HandValid( ent, work );
	trigPressed = trigDown && !vrp->triggerWas;
	trigReleased = !trigDown && vrp->triggerWas;
	vrp->triggerWas = trigDown;

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
	maxclip = BG_MaxClip( ps, weapon );
	clip = &ps->ammoclip[clipIndex];
	reserve = &ps->ammo[ammoIndex];

	// where the gun and its parts are, exactly as cgame draws them
	BG_VR_WeaponGeom( weapon, qfalse, &geom );
	geom.scale = G_VR_GunScale( ent );
	geom.travel *= geom.scale;      // a smaller bolt moves less
	G_VR_GunWorld( ent, dom, hand );
	G_VR_HandWorld( ent, work, workHand );
	G_VR_AimAngles( ent, aim );
	BG_VR_WeaponFrame( &geom, hand, aim, NULL, origin, axis );
	BG_VR_ModelPoint( origin, axis, geom.mag, magPoint );
	BG_VR_ModelPoint( origin, axis, geom.charge, chargePoint );
	BG_VR_ModelDir( axis, geom.chargeDir, chargeDir );
	VectorNormalize( chargeDir );

	G_VR_Eye( ent, eye );
	VectorCopy( eye, chest );
	chest[2] -= VR_CHEST_DROP;
	if ( pressed && trap_Cvar_VariableIntegerValue( "vr_debug" ) == 11 ) {
		G_Printf( "VR reload grip: weapon %d, %.1f from the bolt, %.1f from the gun hand, %.1f from the loading point, %.1f from the chest, clip %d, chambered %d\n",
				  weapon, Distance( offHand, chargePoint ), Distance( offHand, hand ), Distance( offHand, magPoint ),
				  Distance( offHand, chest ), *clip, vrp->chambered[weapon] );
	}

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
	// (the grip takes it; the action is worked with the trigger, so the two don't compete)
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
		// the measured handle (the free hand's trigger takes it); unmeasured, anywhere close to
		// the gun hand, where slides and bolts are
		qboolean nearAction = geom.chargePart >= 0 ? Distance( workHand, chargePoint ) < VR_CHARGE_RADIUS :
							  ( cls != VRR_PUMP && Distance( workHand, hand ) < VR_ACTION_RADIUS );

		if ( trigPressed && !vrp->heldItem && !vrp->chargeHeld && nearAction ) {
			vrp->chargeHeld = qtrue;
			vrp->chargeDone = qfalse;
			VectorCopy( workHand, vrp->chargeStart );
			G_VR_Haptic( ent, work, 0.3f, 20 );
			used = qtrue;
		}
		if ( vrp->chargeHeld ) {
			float pull;

			VectorSubtract( workHand, vrp->chargeStart, d );
			pull = DotProduct( d, chargeDir ) / geom.travel;
			vrp->chargePull = pull < 0 ? 0 : ( pull > 1 ? 1 : pull );
			if ( pull >= VR_CHARGE_PULL && !vrp->chargeDone ) {
				vrp->chargeDone = qtrue;
				if ( *clip > 0 ) {
					vrp->chambered[weapon] = qtrue;
				}
				G_VR_Haptic( ent, 0, 0.7f, 50 );
				G_VR_Haptic( ent, 1, 0.7f, 50 );
			}
			if ( trigReleased ) {
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
// grab assist (g_vrGrabAssist): the reach around things, and the nearest one is taken
static float G_VR_AssistMargin( float base ) {
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

static gentity_t *G_VR_FindBody( gentity_t *self, const vec3_t hand ) {
	gentity_t *e, *best = NULL;
	float margin = G_VR_AssistMargin( VR_GRAB_MARGIN ), d, bestDist = 1e9f;
	int i;

	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e == self || !e->inuse || !e->client || !e->aiCharacter || e->health > 0 || !e->r.linked ) {
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

// how a let-go hand sends something: a throw carries a little further, a let-go just drops
static void G_VR_ThrowVelocity( const vec3_t handVel, vec3_t out ) {
	VectorScale( handVel, VectorLength( handVel ) > VR_THROW_SPEED ? VR_THROW_SCALE : 0.5f, out );
	if ( VectorLength( out ) > VR_THROW_MAX ) {
		VectorNormalize( out );
		VectorScale( out, VR_THROW_MAX, out );
	}
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
	case AICHAR_FROGMAN:
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
			G_VR_Haptic( ent, off, 1.0f, 120 );
			return e->health <= 0 ? e : NULL;
		}

		// face to face: a shove
		VectorScale( toMe, -350.0f, push );
		push[2] = 120.0f;
		VectorAdd( e->client->ps.velocity, push, e->client->ps.velocity );
		e->client->ps.pm_time = 200;
		e->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
		G_Damage( e, ent, ent, NULL, NULL, 2, DAMAGE_NO_KNOCKBACK, MOD_KICKED );
		G_VR_Haptic( ent, off, 0.6f, 60 );
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

// a hand's reach for items: out past the hand along the arm (eyes to hand), up to whatever
// is in the way, so things a little too far, deep on a shelf or over a ledge can be taken
#define VR_HAND_REACH       16.0f

static float G_VR_ReachDist( gentity_t *ent, const vec3_t hand, const vec3_t mins, const vec3_t maxs ) {
	vec3_t eye, dir, end, p;
	trace_t tr;
	float d, best, len, t;

	best = G_VR_BoxDist( hand, mins, maxs );
	if ( best > VR_HAND_REACH + 8.0f ) {
		return best;    // nowhere near
	}
	G_VR_Eye( ent, eye );
	VectorSubtract( hand, eye, dir );
	if ( VectorNormalize( dir ) < 1.0f ) {
		return best;
	}
	VectorMA( hand, VR_HAND_REACH, dir, end );
	trap_Trace( &tr, hand, NULL, NULL, end, ent->s.number, MASK_SOLID );
	if ( tr.startsolid ) {
		return best;
	}
	len = VR_HAND_REACH * tr.fraction;
	for ( t = 2.0f; t <= len; t += 2.0f ) {
		VectorMA( hand, t, dir, p );
		d = G_VR_BoxDist( p, mins, maxs );
		if ( d < best ) {
			best = d;
		}
	}
	return best;
}

static gentity_t *G_VR_FindItem( gentity_t *ent, const vec3_t hand ) {
	gentity_t *e, *best = NULL;
	float margin = G_VR_AssistMargin( VR_ITEM_MARGIN ), d, bestDist = 1e9f;
	int i;

	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || e->s.eType != ET_ITEM || !e->r.linked || !e->item ) {
			continue;
		}
		d = G_VR_ReachDist( ent, hand, e->r.absmin, e->r.absmax );
		if ( d <= margin && d < bestDist ) {
			best = e;
			bestDist = d;
		}
	}
	return best;
}

static qboolean G_VR_GrabItem( gentity_t *ent, const vec3_t hand ) {
	gentity_t *e = G_VR_FindItem( ent, hand );
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

// the use button in VR: from the eyes along the head's look (false without a headset)
qboolean G_VR_LookRay( gentity_t *ent, float dist, vec3_t from, vec3_t to ) {
	vec3_t angles, fwd;

	if ( !G_VR_IsPlayer( ent ) ) {
		return qfalse;
	}
	G_VR_Eye( ent, from );
	angles[PITCH] = SHORT2ANGLE( ent->client->pers.cmd.vrHead[0] );
	angles[YAW] = SHORT2ANGLE( ent->client->pers.cmd.vrHead[1] ) + SHORT2ANGLE( ent->client->ps.delta_angles[YAW] );
	angles[ROLL] = 0;
	AngleVectors( angles, fwd, NULL, NULL );
	VectorMA( from, dist, fwd, to );
	return qtrue;
}

/*
=================
G_VR_PushDoors

Either hand touching something the use button works on (doors, buttons, levers and the
game's other usable spots: func_invisible_user, footlockers, scripted movers) uses it,
like RTCWQuest: a small box around the hand is swept from where it was to where it is,
and whatever it newly comes into is used once. A door only opens from shut.
=================
*/
#define VR_TOUCH_REACH  3.0f        // half size of the hand's touch box, game units

static qboolean G_VR_Touchable( gentity_t *e ) {
	if ( !e->classname ) {
		return qfalse;
	}
	if ( !Q_stricmp( e->classname, "func_door" ) || !Q_stricmp( e->classname, "func_door_rotating" ) ) {
		return e->moverState == MOVER_POS1 || e->moverState == MOVER_POS1ROTATE;
	}
	if ( !Q_stricmp( e->classname, "func_button" ) ) {
		return e->s.apos.trType == TR_STATIONARY && e->s.pos.trType == TR_STATIONARY && !e->active;
	}
	return !Q_stricmp( e->classname, "func_invisible_user" ) || !Q_stricmp( e->classname, "props_footlocker" ) ||
		   !Q_stricmp( e->classname, "script_mover" );
}

static void G_VR_PushDoors( gentity_t *ent, vrPlayer_t *vrp ) {
	vec3_t *last = vrp->touchLast;
	int *touching = vrp->touching, *nextUse = vrp->touchNextUse;
	vec3_t hand, mins, maxs, dir;
	trace_t tr;
	gentity_t *e;
	int h, now = ent->client->ps.commandTime;

	VectorSet( mins, -VR_TOUCH_REACH, -VR_TOUCH_REACH, -VR_TOUCH_REACH );
	VectorSet( maxs, VR_TOUCH_REACH, VR_TOUCH_REACH, VR_TOUCH_REACH );
	for ( h = 0; h < 2; h++ ) {
		G_VR_HandWorld( ent, h, hand );
		e = NULL;
		if ( vrp->touchValid && G_VR_HandValid( ent, h ) && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
			trap_Trace( &tr, last[h], mins, maxs, hand, ent->s.number,
						CONTENTS_SOLID | CONTENTS_TRIGGER | CONTENTS_PLAYERCLIP );
			if ( ( tr.fraction < 1.0f || tr.startsolid ) && tr.entityNum < ENTITYNUM_WORLD ) {
				e = &g_entities[tr.entityNum];
			}
		}
		if ( e && tr.entityNum != touching[h] && now >= nextUse[h] && G_VR_Touchable( e ) ) {
			// what the use button would do on it, from this hand
			VectorSubtract( hand, last[h], dir );
			if ( VectorNormalize( dir ) < 0.01f ) {
				VectorSubtract( tr.endpos, hand, dir );
				VectorNormalize( dir );
			}
			VectorMA( tr.endpos, -4.0f, dir, g_vrHandFrom );
			VectorMA( tr.endpos, 8.0f, dir, g_vrHandTo );
			g_vrHandUse = qtrue;
			Cmd_Activate_f( ent );
			g_vrHandUse = qfalse;
			G_VR_Haptic( ent, h, 0.5f, 40 );
			nextUse[h] = now + 600;
		}
		touching[h] = e ? tr.entityNum : -1;
		VectorCopy( hand, last[h] );
	}
	vrp->touchValid = qtrue;
}

/*
=================
G_VR_Revive

The off hand on a frozen teammate frees them, like the knife does (g_freeze).
=================
*/
static qboolean G_VR_Revive( gentity_t *ent, const vec3_t hand ) {
	gentity_t *e;
	int i, k;

	if ( !g_freeze.integer ) {
		return qfalse;
	}
	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( e == ent || !e->inuse || !e->client || e->aiCharacter || !( e->client->ps.eFlags & EF_FROZEN ) ) {
			continue;
		}
		for ( k = 0; k < 3; k++ ) {
			if ( hand[k] < e->r.absmin[k] - VR_GRAB_MARGIN || hand[k] > e->r.absmax[k] + VR_GRAB_MARGIN ) {
				break;
			}
		}
		if ( k < 3 ) {
			continue;
		}
		e->client->ps.eFlags &= ~EF_FROZEN;
		e->flags &= ~FL_NOTARGET;
		e->client->ps.powerups[PW_INVULNERABLE] = level.time + 5000;      // some time to find cover
		return qtrue;
	}
	return qfalse;
}

/*
=================
G_VR_FlatGrab

Without a headset (g_flatGrab): hold use on a dead body to take hold of it where the
crosshair is on it (its ragdoll hangs from that limb), it stays that far in front of the
view while use is held, attack throws it, letting go drops it. Uses the same carrying as
the VR off hand, so everyone's ragdoll shows it.
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
	if ( tr.entityNum < MAX_CLIENTS ) {
		e = &g_entities[tr.entityNum];
		if ( e->inuse && e->client && e->aiCharacter && e->health <= 0 ) {
			VectorCopy( tr.endpos, at );
			return e;
		}
	}
	// grab assist: a body close to where the crosshair stops
	if ( g_vrGrabAssist.integer ) {
		e = G_VR_FindBody( ent, tr.endpos );
		if ( e ) {
			VectorCopy( tr.endpos, at );
			return e;
		}
	}
	return NULL;
}

// the prop under the crosshair, and where on it
static gentity_t *G_VR_FlatProp( gentity_t *ent, vec3_t eye, vec3_t fwd, vec3_t at ) {
	trace_t tr;
	vec3_t end;
	gentity_t *e;

	VectorCopy( ent->client->ps.origin, eye );
	eye[2] += ent->client->ps.viewheight;
	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorMA( eye, VR_FLAT_REACH, fwd, end );
	trap_Trace( &tr, eye, NULL, NULL, end, ent->s.number, MASK_SHOT );
	VectorCopy( tr.endpos, at );
	if ( tr.entityNum >= MAX_CLIENTS && tr.entityNum < ENTITYNUM_MAX_NORMAL &&
		 G_VRX_FindProp( tr.endpos, 2.0f ) == &g_entities[tr.entityNum] ) {
		return &g_entities[tr.entityNum];
	}
	e = G_VRX_FindProp( tr.endpos, g_vrGrabAssist.integer ? G_VR_AssistMargin( VR_ITEM_MARGIN ) : 2.0f );
	return e;
}

static void G_VR_FlatGrab( gentity_t *ent ) {
	vrPlayer_t *vrp = G_VR_Init( ent );
	usercmd_t *cmd = &ent->client->pers.cmd;
	qboolean use = ( cmd->buttons & BUTTON_ACTIVATE ) != 0;
	qboolean attack = ( cmd->buttons & BUTTON_ATTACK ) != 0;
	qboolean usePressed = use && !vrp->useWas, attackPressed = attack && !vrp->attackWas;
	vec3_t eye, fwd, at, hand;
	int now = ent->client->ps.commandTime;
	float dt = ( now - vrp->lastTime ) * 0.001f;

	vrp->useWas = use;
	vrp->attackWas = attack;

	// a prop held in front (GORE & EXTRAS): attack throws it, letting go puts it down
	if ( G_VRX_PropHeld( ent ) >= 0 ) {
		VectorCopy( ent->client->ps.origin, eye );
		eye[2] += ent->client->ps.viewheight;
		AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
		VectorMA( eye, vrp->flatDist, fwd, hand );
		if ( attackPressed ) {
			vec3_t vel;
			if ( fwd[2] < 0.15f ) {     // the way the view faces, but never down into the floor
				fwd[2] = 0.15f;
				VectorNormalize( fwd );
			}
			VectorScale( fwd, 650.0f, vel );
			vel[2] += 120.0f;
			G_VRX_PropRelease( ent, vel );
		} else if ( !use || ent->client->ps.stats[STAT_HEALTH] <= 0 ) {
			G_VRX_PropRelease( ent, vec3_origin );
		} else {
			G_VRX_PropHold( ent, hand, NULL );
		}
		ent->client->ps.vrGrab = G_VRX_PropHeld( ent ) + 1;
		return;
	}
	if ( usePressed && vrp->grabbed < 0 && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
		gentity_t *prop = G_VR_FlatProp( ent, eye, fwd, at );
		if ( prop && G_VRX_PropGrab( ent, prop, at ) ) {
			vrp->flatDist = Distance( eye, at );
			vrp->flatDist = vrp->flatDist < 32 ? 32 : ( vrp->flatDist > 56 ? 56 : vrp->flatDist );
			ent->client->ps.vrGrab = prop->s.number + 1;
			return;
		}
	}

	if ( vrp->grabbed >= 0 ) {
		gentity_t *body = &g_entities[vrp->grabbed];

		VectorCopy( ent->client->ps.origin, eye );
		eye[2] += ent->client->ps.viewheight;
		AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
		VectorMA( eye, vrp->flatDist, fwd, hand );
		if ( hand[2] > eye[2] - 8 ) {
			hand[2] = eye[2] - 8;       // held in front, not over the head
		}
		if ( dt > 0.0f && dt < 0.2f ) {
			vec3_t v;
			VectorSubtract( hand, vrp->lastHand, v );
			VectorScale( v, 1.0f / dt, v );
			VectorScale( vrp->handVel, 0.5f, vrp->handVel );
			VectorMA( vrp->handVel, 0.5f, v, vrp->handVel );
		}
		VectorCopy( hand, vrp->lastHand );
		vrp->lastTime = now;

		if ( !body->inuse || !body->client || body->health > 0 || ent->client->ps.stats[STAT_HEALTH] <= 0 || !g_flatGrab.integer ) {
			if ( body->inuse ) {
				VectorCopy( vrp->grabbedMins, body->r.mins );
				VectorCopy( vrp->grabbedMaxs, body->r.maxs );
				body->client->ps.pm_type = vrp->grabbedPmType;
			}
			body->s.dl_intensity = 0;
			vrp->grabbed = -1;
		} else if ( attackPressed ) {
			// a throw, the way the view faces but never down into the floor
			if ( fwd[2] < 0.15f ) {
				fwd[2] = 0.15f;
				VectorNormalize( fwd );
			}
			// (scaled up by VR_THROW_SCALE on release: about 450 forward, enough to get a body
			// over a castle wall's parapet, not across the room)
			VectorScale( fwd, 300.0f, vrp->handVel );
			vrp->handVel[2] += 120.0f;
			G_VR_ReleaseBody( vrp, body, qtrue );
			body->s.dl_intensity = 0;
			vrp->grabbed = -1;
		} else if ( !use ) {
			G_VR_ReleaseBody( vrp, body, qfalse );
			body->s.dl_intensity = 0;
			vrp->grabbed = -1;
		} else {
			G_VR_PlaceBody( body, hand );
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( hand, body->s.origin2 );
		}
	} else if ( usePressed && g_flatGrab.integer && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
		gentity_t *body = G_VR_FlatTarget( ent, eye, fwd, at );

		if ( body && !G_VR_BodyTaken( body->s.number, ent->s.number ) ) {
			vrp->grabbed = body->s.number;
			vrp->flatDist = Distance( eye, at );
			vrp->flatDist = vrp->flatDist < 40 ? 40 : ( vrp->flatDist > 64 ? 64 : vrp->flatDist );
			VectorClear( vrp->grabOffset );
			VectorClear( vrp->handVel );
			VectorCopy( at, vrp->lastHand );
			vrp->lastTime = now;
			VectorCopy( body->r.mins, vrp->grabbedMins );
			VectorCopy( body->r.maxs, vrp->grabbedMaxs );
			VectorCopy( vrCarryMins, body->r.mins );
			VectorCopy( vrCarryMaxs, body->r.maxs );
			vrp->grabbedPmType = body->client->ps.pm_type == PM_FREEZE ? PM_DEAD : body->client->ps.pm_type;
			body->client->ps.pm_type = PM_FREEZE;
			// first the point that was grabbed, so the ragdoll takes it by the right limb
			body->s.dl_intensity = ent->s.number + 1;
			VectorCopy( at, body->s.origin2 );
			if ( vrp->thrown == body->s.number ) {
				vrp->thrown = -1;
			}
		}
	}
	ent->client->ps.vrGrab = vrp->grabbed + 1;

	if ( vrp->thrown >= 0 && dt > 0 && dt < 0.2f ) {
		G_VR_FlyBody( vrp, dt );
	}
	if ( vrp->grabbed < 0 ) {
		vrp->lastTime = now;
	}
}

/*
=================
G_VR_GrabHint

After the cursor hints: without a headset, the hand hint on a body that can be picked up;
with one, VRS_CANGRAB while the off hand is on something it can take (the player's
cgame buzzes and shows it, vr_grabHint).
=================
*/
void G_VR_GrabHint( gentity_t *ent ) {
	vrPlayer_t *vrp;
	vec3_t eye, fwd, at, hand;
	qboolean can = qfalse;

	if ( !ent->client || ent->aiCharacter || ent->s.number >= MAX_CLIENTS || ent->client->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}
	vrp = G_VR_Init( ent );
	if ( !G_VR_IsPlayer( ent ) ) {
		if ( g_flatGrab.integer && vrp->grabbed < 0 && G_VRX_PropHeld( ent ) < 0 &&
			 ( G_VR_FlatTarget( ent, eye, fwd, at ) || G_VR_FlatProp( ent, eye, fwd, at ) ) ) {
			ent->client->ps.serverCursorHint = HINT_ACTIVATE;
			ent->client->ps.serverCursorHintVal = 0;
		}
		return;
	}
	if ( vrp->grabbed < 0 && !vrp->heldItem ) {
		G_VR_HandWorld( ent, !G_VR_DomHand( ent ), hand );
		can = G_VR_FindBody( ent, hand ) || G_VR_FindItem( ent, hand ) ||
			  ( G_VRX_PropHeld( ent ) < 0 && G_VRX_FindProp( hand, G_VR_AssistMargin( VR_ITEM_MARGIN ) ) );
	}
	if ( can ) {
		ent->client->ps.vrState |= VRS_CANGRAB;
	} else {
		ent->client->ps.vrState &= ~VRS_CANGRAB;
	}
}

/*
=================
G_VR_PlayerThink

After every pmove of a headset player.
=================
*/
// a hand touching an item picks it up as the body touching it would (treasure on a shelf
// or a mantel the body can't get up to): only what the body would take by touch
#define VR_HAND_TOUCH       4.0f

static void G_VR_HandTouchItems( gentity_t *ent ) {
	vec3_t hand;
	gentity_t *e;
	trace_t tr;
	int h, i;

	if ( ent->client->ps.stats[STAT_HEALTH] <= 0 || ent->client->ps.pm_type != PM_NORMAL ) {
		return;
	}
	for ( h = 0; h < 2; h++ ) {
		if ( !G_VR_HandValid( ent, h ) ) {
			continue;
		}
		G_VR_HandWorld( ent, h, hand );
		for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
			e = &g_entities[i];
			if ( !e->inuse || e->s.eType != ET_ITEM || !e->r.linked || !e->item || e->touch != Touch_Item_Auto ) {
				continue;
			}
			if ( G_VR_ReachDist( ent, hand, e->r.absmin, e->r.absmax ) <= VR_HAND_TOUCH ) {
				memset( &tr, 0, sizeof( tr ) );
				e->touch( e, ent, &tr );
			}
		}
	}
}

void G_VR_PlayerThink( gentity_t *ent ) {
	vrPlayer_t *vrp;
	int dom, off, now, grip;
	vec3_t offHand;
	qboolean down, pressed, released, used;
	float dt;

	if ( !G_VR_IsPlayer( ent ) ) {
		if ( ent->client && !ent->aiCharacter && ent->s.number < MAX_CLIENTS ) {
			G_VR_FlatGrab( ent );       // RealRTCW-VR: bodies picked up without a headset
		}
		return;
	}
	vrp = G_VR_Init( ent );
	G_VR_HandTouchItems( ent );

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

	// the weapon hand's speed too, for throwing grenades
	{
		vec3_t domHand, dv;
		G_VR_HandWorld( ent, dom, domHand );
		if ( dt > 0.0f ) {
			VectorSubtract( domHand, vrp->lastDom, dv );
			VectorScale( dv, 1.0f / dt, dv );
			VectorScale( vrp->domVel, 0.5f, vrp->domVel );
			VectorMA( vrp->domVel, 0.5f, dv, vrp->domVel );
		}
		VectorCopy( domHand, vrp->lastDom );
		if ( VectorLength( vrp->domVel ) >= VectorLength( vrp->throwVel ) || now - vrp->throwVelTime > 300 ) {
			VectorCopy( vrp->domVel, vrp->throwVel );
			vrp->throwVelTime = now;
		}
	}
	vrp->lastTime = now;

	grip = off == VR_HAND_LEFT ? VRF_GRIP_LEFT : VRF_GRIP_RIGHT;
	down = ( ent->client->pers.cmd.vrFlags & grip ) && G_VR_HandValid( ent, off );
	pressed = down && !vrp->gripWas;
	released = !down && vrp->gripWas;
	vrp->gripWas = down;

	G_VR_PushDoors( ent, vrp );
	G_VR_DualGuns( ent, vrp, dom );
	used = ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_Reload( ent, vrp, offHand, pressed, released, dom );

	// (a second pistol / SMG: a double press of the off hand grip, cl_vr.c)

	// a prop in the hand (GORE & EXTRAS): carried, then dropped or thrown
	if ( G_VRX_PropHeld( ent ) >= 0 ) {
		if ( down && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
			G_VRX_PropHold( ent, offHand, NULL );
		} else {
			vec3_t vel;
			G_VR_ThrowVelocity( vrp->handVel, vel );
			G_VRX_PropRelease( ent, vel );
		}
		used = qtrue;
	} else if ( pressed && !used && ent->client->ps.stats[STAT_HEALTH] > 0 &&
				G_VRX_PropGrab( ent, G_VRX_FindProp( offHand, G_VR_AssistMargin( VR_ITEM_MARGIN ) ), offHand ) ) {
		G_VR_Haptic( ent, off, 0.5f, 40 );
		used = qtrue;
	}

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
		gentity_t *body = NULL;
		// an item in the hand, a body, someone alive (takedown or shove), or something to use
		if ( ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_Revive( ent, offHand ) ) {
			G_VR_Haptic( ent, off, 0.8f, 100 );     // a frozen teammate freed
		} else if ( ent->client->ps.stats[STAT_HEALTH] > 0 && G_VR_GrabItem( ent, offHand ) ) {
			G_VR_Haptic( ent, off, 0.4f, 40 );
		} else {
			body = G_VR_FindBody( ent, offHand );
			if ( !body && ent->client->ps.stats[STAT_HEALTH] > 0 ) {
				body = G_VR_GrabLiving( ent, offHand, off );
				if ( !body && !vrLivingTouched ) {
					G_VR_HandUse( ent, offHand );
				}
			}
		}
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

	// the hands, for the other players' view of this one
	{
		int h, flags = VRE_ACTIVE;
		vec3_t hand;

		if ( dom == VR_HAND_RIGHT ) {
			flags |= VRE_RIGHTHANDED;
		}
		for ( h = 0; h < 2; h++ ) {
			if ( G_VR_HandValid( ent, h ) ) {
				flags |= h == VR_HAND_LEFT ? VRE_HAND_LEFT : VRE_HAND_RIGHT;
				G_VR_HandWorld( ent, h, hand );
				VectorSubtract( hand, ent->r.currentOrigin, ent->s.vrHand[h] );
				SnapVector( ent->s.vrHand[h] );     // whole units are plenty, and send small
			}
		}
		ent->s.vrFlags = flags;

		// and where the head looks (the usercmd's yaw space, like the aim)
		ent->s.vrHead[PITCH] = SHORT2ANGLE( ent->client->pers.cmd.vrHead[0] );
		ent->s.vrHead[YAW] = AngleNormalize360( SHORT2ANGLE( ent->client->pers.cmd.vrHead[1] ) +
												SHORT2ANGLE( ent->client->ps.delta_angles[YAW] ) );
		ent->s.vrHead[ROLL] = 0;
	}

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
	// (a dual pair goes by the shot's own event, not the latest command's flag)
	if ( g_dualLeftShot ||
		 ( ( cmd->vrFlags & VRF_AKIMBO_LEFT ) && ent->client->ps.weapon == WP_AKIMBO ) ) {
		dom = !dom;     // dual pistols / SMGs: the off hand's gun fired
	}
	if ( !G_VR_HandValid( ent, dom ) ) {
		return qfalse;
	}

	G_VR_Eye( ent, eye );
	if ( dom == G_VR_DomHand( ent ) ) {
		G_VR_GunWorld( ent, dom, end );
	} else {
		G_VR_HandWorld( ent, dom, end );
	}
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

/*
=================
G_VR_Training

HOST GAME's training mode (g_vrTraining): 1 = every weapon, the reserve ammo kept full
(magazines still need reloading) and no harm to players; 2 = and enemies ignore them, so
there's something to test the guns on.
=================
*/
void G_VR_Training( gentity_t *ent ) {
	static int nextRefill[MAX_CLIENTS];
	gclient_t *client = ent->client;
	int i, num;

	if ( !client || ent->aiCharacter || !g_vrTraining.integer ) {
		return;
	}
	num = ent - g_entities;
	if ( num < 0 || num >= MAX_CLIENTS ) {
		return;
	}

	ent->flags |= FL_GODMODE;
	if ( g_vrTraining.integer >= 2 ) {
		ent->flags |= FL_NOTARGET;
	} else {
		ent->flags &= ~FL_NOTARGET;
	}
	if ( client->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	// as "give all" does it
	for ( i = 0; i <= WP_DYNAMITE; i++ ) {
		if ( !COM_BitCheck( client->ps.weapons, i ) ) {
			COM_BitSet( client->ps.weapons, i );
			if ( i > WP_NONE && i < WP_MONSTER_ATTACK1 ) {
				Add_Ammo( ent, i, 999, qtrue );     // loaded, ready to try
			}
		}
	}
	client->ps.stats[STAT_KEYS] |= ( 1 << INV_BINOCS );

	if ( level.time >= nextRefill[num] || nextRefill[num] > level.time + 1000 ) {
		nextRefill[num] = level.time + 500;
		for ( i = 1; i < WP_MONSTER_ATTACK1; i++ ) {
			Add_Ammo( ent, i, 999, qfalse );
		}
		if ( client->ps.stats[STAT_ARMOR] < 100 ) {
			client->ps.stats[STAT_ARMOR] = 100;
		}
	}
}

// a headset player: their gun is aimed by their hands, not the flat game's view
qboolean G_VR_Aiming( gentity_t *ent ) {
	return G_VR_IsPlayer( ent );
}

/*
=================
G_VR_LobAt

The velocity, at this speed, that carries a throw from start onto the reticle (where the
aim points: a straight trace along the player's aim) on the low arc; out of reach, the
longest throw that way. qfalse if not a VR player or vr_throwAim is off.
=================
*/
qboolean G_VR_LobAt( gentity_t *ent, const vec3_t start, float speed, float gravity, vec3_t vel ) {
	vec3_t fwd, end, flat;
	trace_t tr;
	float x, y, v2, disc, th;

	if ( !ent || !ent->client || !G_VR_IsPlayer( ent ) || !( ent->client->pers.cmd.vrFlags2 & VRF2_THROWAIM ) ) {
		return qfalse;
	}
	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorMA( start, 8192, fwd, end );
	trap_Trace( &tr, start, NULL, NULL, end, ent->s.number, MASK_SHOT );
	VectorSubtract( tr.endpos, start, flat );
	y = flat[2];
	flat[2] = 0;
	x = VectorNormalize( flat );
	if ( x < 8.0f ) {
		VectorScale( fwd, speed, vel );     // straight down (or up)
		return qtrue;
	}
	v2 = speed * speed;
	disc = v2 * v2 - gravity * ( gravity * x * x + 2.0f * y * v2 );
	th = disc < 0 ? M_PI * 0.25f : atan2( v2 - sqrt( disc ), gravity * x );
	VectorScale( flat, speed * cos( th ), vel );
	vel[2] = speed * sin( th );
	return qtrue;
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
	dom = G_VR_DomHand( ent );
	if ( !G_VR_HandValid( ent, dom ) ) {
		return;
	}
	G_VR_Eye( ent, eye );
	G_VR_HandWorld( ent, dom, hand );
	// from the eye, so a hand through a wall can't throw on the far side
	trap_Trace( &tr, eye, tv( -4, -4, -4 ), tv( 4, 4, 4 ), hand, ent->s.number, MASK_MISSILESHOT );
	VectorCopy( tr.endpos, start );

	// onto the reticle (vr_throwAim), a little slower for dynamite
	if ( G_VR_LobAt( ent, start, weapon == WP_DYNAMITE ? 700.0f : 900.0f, DEFAULT_GRAVITY, dir ) ) {
		return;
	}

	VectorCopy( vrPlayers[ent - g_entities].throwVel, vel );
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

/*
=================
G_VR_CheckCompletion / G_VR_BonusHealth

Everything found: every secret and every piece of treasure in the level. Everyone is
healed, armoured and restocked, and from then on has 25 more health for the rest of the
session (it adds up, level after level, to 100 more). Called whenever a secret or a
treasure is found.
=================
*/
#define VR_BONUS_HEALTH     25
#define VR_BONUS_MAX        100

int G_VR_BonusHealth( void ) {
	int b = trap_Cvar_VariableIntegerValue( "g_vrBonusHealth" );
	return b < 0 ? 0 : ( b > VR_BONUS_MAX ? VR_BONUS_MAX : b );
}

void G_VR_CheckCompletion( void ) {
	gentity_t *e;
	int i, w, bonus;

	if ( level.vrAllFound || ( level.numSecrets <= 0 && level.numTreasure <= 0 ) ) {
		return;
	}
	if ( level.numSecretsFound < level.numSecrets || level.numTreasureFound < level.numTreasure ) {
		return;
	}
	level.vrAllFound = qtrue;

	bonus = G_VR_BonusHealth() + VR_BONUS_HEALTH;
	if ( bonus > VR_BONUS_MAX ) {
		bonus = VR_BONUS_MAX;
	}
	trap_Cvar_Set( "g_vrBonusHealth", va( "%d", bonus ) );

	trap_SendServerCommand( -1, "mu_play sound/misc/bonus.wav 0\n" );
	trap_SendServerCommand( -1, va( "cp \"^3EVERYTHING FOUND!\n^7All secrets and treasure: full health, armour and ammo\n^2+%d max health^7 for the rest of the session\"", VR_BONUS_HEALTH ) );

	for ( i = 0; i < level.maxclients; i++ ) {
		e = &g_entities[i];
		if ( !e->inuse || !e->client || ( e->r.svFlags & SVF_CASTAI ) || e->aiCharacter ) {
			continue;
		}
		e->client->pers.maxHealth = e->client->pers.maxHealth > 0 ? e->client->pers.maxHealth : 100;
		e->client->ps.stats[STAT_MAX_HEALTH] = e->client->pers.maxHealth + bonus;
		if ( e->health > 0 ) {
			e->health = e->client->ps.stats[STAT_HEALTH] = e->client->ps.stats[STAT_MAX_HEALTH];
		}
		e->client->ps.stats[STAT_ARMOR] = 100;
		for ( w = 1; w < WP_MONSTER_ATTACK1; w++ ) {
			if ( COM_BitCheck( e->client->ps.weapons, w ) ) {
				Add_Ammo( e, w, 999, qtrue );
			}
		}
	}
}

/*
=================
G_DualToggle

A second pistol / SMG in the other hand, or back to one ("dualwield", which VR sends for a
double press of the off hand grip). Putting the second gun away puts the
rounds its magazine held back with the rest. Per player: PERS_DUAL in their playerState.
=================
*/
void G_DualToggle( gentity_t *ent ) {
	playerState_t *ps;
	int w, clip, max;

	if ( !ent || !ent->client || ent->aiCharacter || ent->health <= 0 ) {
		return;
	}
	ps = &ent->client->ps;
	if ( !ps->persistant[PERS_DUAL] && !BG_DualCapable( ps->weapon ) ) {
		trap_SendServerCommand( ent - g_entities, "cp \"Only pistols and SMGs can be dual wielded\"" );
		return;
	}
	ps->persistant[PERS_DUAL] = !ps->persistant[PERS_DUAL];
	if ( !ps->persistant[PERS_DUAL] ) {
		G_DualPutAway( ps );
		for ( w = WP_NONE + 1; w < WP_NUM_WEAPONS; w++ ) {
			if ( !BG_DualCapable( w ) ) {
				continue;
			}
			clip = ps->ammoclip[BG_FindClipForWeapon( w )];
			max = BG_MaxClip( ps, w );
			if ( clip > max ) {
				ps->ammo[BG_FindAmmoForWeapon( w )] += clip - max;
				ps->ammoclip[BG_FindClipForWeapon( w )] = max;
			}
		}
	}
	// the guns come up again
	if ( ps->weapon && ( ps->weaponstate == WEAPON_READY || ps->weaponstate == WEAPON_FIRING ) ) {
		ps->weaponstate = WEAPON_RAISING;
		ps->weaponTime = 250;
		ps->weapAnim = ( ( ps->weapAnim & ANIM_TOGGLEBIT ) ^ ANIM_TOGGLEBIT ) | WEAP_RAISE;
	}
	trap_SendServerCommand( ent - g_entities, ps->persistant[PERS_DUAL] ? "cp \"Dual wielding\"" : "cp \"One gun\"" );
}
