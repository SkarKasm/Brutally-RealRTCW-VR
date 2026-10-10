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

// cg_vr.c -- headset view, hands and weapon placement for cgame.
//
// The engine tracks the head and hands in "play space" (see vr_shared.h). cgame turns
// that into the world: the head becomes the view, the hands get world positions from
// their offset to the head, and the gun is drawn where the weapon hand is.

#include "cg_local.h"

cgVRState_t cgvr;

static vmCvar_t vr_drawHands;
static vmCvar_t vr_grabHint;        // something in reach of the off hand: 0 nothing, 1 a buzz, 2 and a hand sign
static qboolean vrCanGrab;
static vmCvar_t vr_reticle;
static vmCvar_t vr_inspect;
static vmCvar_t vr_emptySwitch;

static qboolean CG_VR_OneHanded( int weapon );
static vmCvar_t vr_predictHits;

/*
=================
CG_VR_Active
=================
*/
qboolean CG_VR_Active( void ) {
	return cgVR && cgVR->active;
}

qboolean CG_VR_EyePass( void ) {
	return cgVR && cgVR->active && cgVR->pass == VR_PASS_EYE;
}

qboolean CG_VR_2DPass( void ) {
	return cgVR && cgVR->active && cgVR->pass == VR_PASS_2D;
}

// a cutscene drawn flat on the 2D layer (a screen in front of the player, cl_vr.c)
qboolean CG_VR_FlatCutscene( void ) {
	return cgVR && cgVR->active && cgVR->flatCutscene && cg.cameraMode;
}

// switch to another weapon when the current one runs dry (always outside VR)
qboolean CG_VR_EmptySwitch( void ) {
	if ( !CG_VR_Active() ) {
		return qtrue;
	}
	trap_Cvar_Update( &vr_emptySwitch );
	return vr_emptySwitch.integer != 0;
}

int CG_VR_DominantHand( void ) {
	return ( !cgVR || cgVR->rightHanded ) ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

/*
=================
CG_VR_Init
=================
*/
void CG_VR_Init( void ) {
	vmCvar_t dummy;

	memset( &cgvr, 0, sizeof( cgvr ) );
	CG_Ragdoll_Init();
	CG_VR_BodyInit();
	CG_VR_ScopeInit();
	CG_VR_RecoilInit();
	CG_VR_WristInit();
	CG_VR_ComfortInit();

	// Fine tuning on top of the measured grip, "forward left up pitch yaw roll" in model
	// units/degrees. vr_gunOffset_<weapon number> applies to a single weapon.
	trap_Cvar_Register( &dummy, "vr_gunOffset", "0 0 0 0 0 0", CVAR_ARCHIVE );
	trap_Cvar_Register( &dummy, "vr_laserPointerColor", "1 0.25 0.15", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_drawHands, "vr_drawHands", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_grabHint, "vr_grabHint", "2", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_reticle, "vr_reticle", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_inspect, "vr_inspect", "0", 0 );
	trap_Cvar_Register( &vr_emptySwitch, "vr_emptySwitch", "0", CVAR_ARCHIVE );
	trap_Cvar_Register( &vr_predictHits, "vr_predictHits", "1", CVAR_ARCHIVE );

	// built by misc/vr/make_vr_assets.py into main/z_vr_assets.pk3
	cgvr.handModel[VR_HAND_LEFT] = trap_R_RegisterModel( "models/vr/hand_l.md3" );
	cgvr.handModel[VR_HAND_RIGHT] = trap_R_RegisterModel( "models/vr/hand_r.md3" );
	// green, snow or night gloves and sleeves by map, like the flatscreen first person hands
	{
		char map[MAX_QPATH];
		trap_Cvar_VariableStringBuffer( "mapname", map, sizeof( map ) );
		cgvr.handSkin = trap_R_RegisterSmartSkin( "models/vr/hands.smartskin", map, qfalse );
		cgvr.bodySkin = trap_R_RegisterSmartSkin( "models/players/player/body_vr.smartskin", map, qfalse );
	}
	cgvr.lastMagDrop = cgVR ? cgVR->magDropCount : 0;
}

/*
=================
CG_VR_ScaleFor

The flat game's first person models aren't drawn to one scale (a pistol's hand is nearly
twice the size of the MP40's), so each gun is drawn at the size that makes its own hand life
size: the hand's size (misc/vr/vr_hand_size.py, measured the same way whatever the hand's
pose, in the MP40 right hand's span) brought to VR_HAND_SPAN. Every hand, on a gun or free,
comes out the same size, and the guns close to their real lengths. Where a gun's two hands
are modelled at different sizes, the gun is sized by the bigger one and the smaller one's palm
and fingers are stretched to match, its sleeve left as it is (CG_VR_StretchHand: shrinking a
hand closed round the gun sinks its fingers into it). VR OPTIONS, Weapon size, multiplies it
(vr_gunSize: one of the BG_VR_WeaponScale presets).
=================
*/
#define VR_HAND_SPAN        8.0f        // game units, about 20cm: a gloved hand around a grip
#define VR_FREE_HAND_SPAN_R 10.9f       // the free hands (hand_r / hand_l, from the MP40)
#define VR_FREE_HAND_SPAN_L 13.6f

typedef struct {
	const char  *weap;
	float       trig;       // the trigger hand's size, 0 none on the model
	float       sup;        // the support hand's
	vec3_t      supPalm;    // its palm (hands model space): where the off hand takes hold
} vrHandSpan_t;

static const vrHandSpan_t vrHandSpans[] = {
	{ "airstrike.weap", 15.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "akimbo.weap", 17.4f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "bar.weap", 16.7f, 17.9f, { 41.9f, -6.0f, -12.7f } },
	{ "browning.weap", 0.0f, 21.9f, { 48.5f, 4.1f, -15.1f } },
	{ "colt.weap", 18.6f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "dualtt33.weap", 15.0f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "dynamite.weap", 18.1f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "dynamite_eng.weap", 18.1f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "fg42.weap", 13.2f, 15.1f, { 33.1f, -5.9f, -8.7f } },
	{ "fg42scope.weap", 13.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "flamethrower.weap", 14.6f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "g43.weap", 0.0f, 10.3f, { 20.8f, -3.1f, -5.9f } },
	{ "garand.weap", 0.0f, 10.2f, { 19.2f, -2.9f, -7.3f } },
	{ "grenade.weap", 14.8f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "hdm.weap", 18.6f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "ithaca.weap", 0.0f, 19.7f, { 40.6f, -5.4f, -10.9f } },
	{ "knife.weap", 14.7f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "luger.weap", 18.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "luger_silenced.weap", 18.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "m1garand.weap", 0.0f, 19.3f, { 41.2f, -5.9f, -11.5f } },
	{ "m7.weap", 0.0f, 19.3f, { 41.2f, -5.9f, -11.5f } },
	{ "mauser.weap", 0.0f, 21.2f, { 44.2f, -6.8f, -11.9f } },
	{ "mg42m.weap", 0.0f, 24.0f, { 39.9f, -3.9f, -16.0f } },
	{ "mosin.weap", 0.0f, 12.3f, { 23.0f, -4.3f, -7.3f } },
	{ "mp34.weap", 0.0f, 17.2f, { 38.1f, -5.6f, -16.1f } },
	{ "mp40.weap", 10.9f, 13.6f, { 23.3f, -4.9f, -13.9f } },
	{ "mp44.weap", 0.0f, 16.5f, { 32.4f, -4.0f, -11.8f } },
	{ "panzerfaust.weap", 9.6f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "pineapple.weap", 14.8f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "poisongas.weap", 15.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "ppsh.weap", 14.8f, 14.0f, { 17.8f, -1.4f, -11.5f } },
	{ "revolver.weap", 16.8f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "smokebomb.weap", 15.2f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "sniperrifle.weap", 0.0f, 21.2f, { 44.2f, -6.8f, -11.9f } },
	{ "snooper.weap", 0.0f, 10.2f, { 19.2f, -2.9f, -7.3f } },
	{ "sten.weap", 0.0f, 18.2f, { 44.8f, -5.9f, -10.9f } },
	{ "tesla.weap", 13.4f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "thompson.weap", 11.6f, 11.0f, { 23.9f, -2.9f, -5.4f } },
	{ "tt33.weap", 15.0f, 0.0f, { 0.0f, 0.0f, 0.0f } },
	{ "venom.weap", 13.7f, 0.0f, { 0.0f, 0.0f, 0.0f } },
};

static float CG_VR_GunSize( void ) {
	char buf[16];

	trap_Cvar_VariableStringBuffer( "vr_gunSize", buf, sizeof( buf ) );
	return BG_VR_WeaponScale( BG_VR_ScalePreset( buf[0] ? atof( buf ) : 1.0f ) );
}

// the free hand models' size (from the MP40, CG_VR_AddHands)
static float CG_VR_FreeHandScale( int h ) {
	return VR_HAND_SPAN * CG_VR_GunSize() / ( h == VR_HAND_LEFT ? VR_FREE_HAND_SPAN_L : VR_FREE_HAND_SPAN_R );
}

float CG_VR_ScaleFor( int weapon ) {
	const char *file = weapon > WP_NONE ? BG_GetWeaponFilename( weapon ) : NULL;
	float s = 0.45f;
	int i;

	if ( file && *file && !cg_vanilla_guns.integer ) {
		file = COM_SkipPath( (char *)file );
		for ( i = 0; i < ARRAY_LEN( vrHandSpans ); i++ ) {
			if ( !Q_stricmp( vrHandSpans[i].weap, file ) ) {
				// the trigger hand, or (long guns) the support hand: the trigger hand put on
				// them (vr_rifle_hands.py) is made its size
				// (both: the bigger one, the smaller one's palm and fingers are stretched to
				// match, CG_VR_StretchHand)
				s = VR_HAND_SPAN / ( vrHandSpans[i].trig > vrHandSpans[i].sup ? vrHandSpans[i].trig : vrHandSpans[i].sup );
				break;
			}
		}
	}
	s *= CG_VR_GunSize();
	return s < 0.3f ? 0.3f : ( s > 1.0f ? 1.0f : s );
}

// the held gun's size, also for the game's reloading (g_vr.c)
float CG_VR_WeaponScale( void ) {
	float s = CG_VR_ScaleFor( cg.predictedPlayerState.weapon );

	if ( cgVR ) {
		cgVR->weaponScale = s;
	}
	return s;
}

void CG_VR_Geom( int weapon, vrWeaponGeom_t *g ) {
	BG_VR_WeaponGeom( weapon, cg_vanilla_guns.integer, g );
	g->scale = CG_VR_ScaleFor( weapon );
}

/*
=================
CG_VR_HandPose

With the full body, the gun is held by the flat game's own first person hand: it sits on
the grip with the finger on the trigger and stays fixed to the gun, and BJ's arm leads to
its wrist. Two handed, the support hand is the flat game's too, fixed where it holds the
gun on screen. Measured by misc/vr/make_vr_assets.py's posing of each first person model
(hands model space at the idle frame): which piece carries each hand (-1 the gun model,
0.. a part, -2 none) and where its wrist meets the sleeve.
=================
*/
typedef struct {
	const char  *weap;
	int         trigPiece;
	vec3_t      trigWrist;
	int         supPiece;
	vec3_t      supWrist;
} vrHandPose_t;

static const vrHandPose_t vrHandPoses[] = {
	{ "airstrike.weap", -1, { 9.2f, -15.4f, -15.6f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "akimbo.weap", -1, { 14.8f, -14.9f, -14.2f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "bar.weap", -1, { 2.9f, -10.3f, -7.4f }, 2, { 33.7f, -2.2f, -15.8f } },
	{ "browning.weap", -2, { 0.0f, 0.0f, 0.0f }, 1, { 39.2f, 5.5f, -14.8f } },
	{ "colt.weap", -1, { 13.4f, -10.4f, -12.4f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "dualtt33.weap", -1, { 16.1f, -13.8f, -13.7f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "dynamite.weap", -1, { 13.2f, -15.8f, -20.2f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "dynamite_eng.weap", -1, { 13.2f, -15.8f, -20.2f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "fg42.weap", -1, { 10.0f, -10.5f, -9.8f }, 2, { 29.0f, -1.9f, -12.8f } },
	{ "fg42scope.weap", -1, { 10.0f, -10.5f, -9.8f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "flamethrower.weap", -1, { 4.0f, -3.9f, -11.9f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "g43.weap", -3, { 0.1f, -5.3f, -3.5f }, 0, { 17.4f, -0.6f, -8.2f } },
	{ "garand.weap", -2, { 0.0f, 0.0f, 0.0f }, 0, { 15.2f, -0.4f, -7.1f } },
	{ "grenade.weap", -1, { 14.4f, -14.5f, -17.4f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "hdm.weap", -1, { 12.0f, -12.2f, -13.3f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "ithaca.weap", -3, { 2.0f, -10.7f, -6.7f }, 4, { 34.3f, -0.9f, -14.9f } },
	{ "knife.weap", -1, { 10.0f, -9.7f, -8.7f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "luger.weap", -1, { 14.4f, -10.9f, -12.8f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "luger_silenced.weap", -1, { 14.4f, -10.9f, -12.8f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "m1garand.weap", -3, { -0.4f, -11.1f, -6.8f }, -1, { 35.7f, -1.6f, -15.7f } },
	{ "m7.weap", -3, { -0.4f, -11.1f, -6.8f }, -1, { 35.7f, -1.6f, -15.7f } },
	{ "mauser.weap", -3, { 0.0f, -12.4f, -8.4f }, -1, { 36.9f, -3.3f, -17.3f } },
	{ "mg42m.weap", -2, { 0.0f, 0.0f, 0.0f }, 1, { 32.4f, 2.3f, -18.3f } },
	{ "mosin.weap", -3, { 1.0f, -7.0f, -5.1f }, -1, { 19.2f, -2.3f, -10.8f } },
	{ "mp34.weap", -3, { 5.2f, -10.3f, -12.7f }, 2, { 33.4f, -2.1f, -21.5f } },
	{ "mp40.weap", -1, { 1.0f, -8.0f, -9.7f }, 2, { 20.7f, 0.2f, -14.0f } },
	{ "mp44.weap", -3, { 10.3f, -8.7f, -10.6f }, 2, { 27.9f, 1.4f, -12.2f } },
	{ "panzerfaust.weap", 4, { 1.2f, -9.0f, -9.1f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "pineapple.weap", -1, { 15.4f, -14.7f, -17.1f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "poisongas.weap", -1, { 9.2f, -15.4f, -15.6f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "ppsh.weap", -1, { -3.3f, -8.8f, -7.3f }, 2, { 14.7f, 3.4f, -13.9f } },
	{ "revolver.weap", -1, { 8.5f, -12.3f, -12.0f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "smokebomb.weap", -1, { 9.2f, -15.4f, -15.6f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "sniperrifle.weap", -3, { 0.0f, -12.4f, -8.4f }, -1, { 36.9f, -3.3f, -17.3f } },
	{ "snooper.weap", -2, { 0.0f, 0.0f, 0.0f }, 0, { 15.2f, -0.4f, -7.1f } },
	{ "sten.weap", -3, { 6.9f, -10.9f, -9.0f }, 2, { 39.2f, -2.4f, -15.7f } },
	{ "tesla.weap", -1, { 6.5f, -2.7f, -10.1f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "thompson.weap", -1, { 5.7f, -6.1f, -7.4f }, 2, { 19.6f, -0.3f, -7.5f } },
	{ "tt33.weap", -1, { 15.9f, -9.6f, -11.8f }, -2, { 0.0f, 0.0f, 0.0f } },
	{ "venom.weap", 4, { 16.4f, -5.8f, -17.5f }, -2, { 0.0f, 0.0f, 0.0f } },
};

static const vrHandPose_t *CG_VR_HandPose( int weapon ) {
	const char *file;
	int i;

	if ( cg_vanilla_guns.integer || weapon <= WP_NONE ) {
		return NULL;
	}
	file = BG_GetWeaponFilename( weapon );
	if ( !file || !*file ) {
		return NULL;
	}
	for ( i = 0; i < ARRAY_LEN( vrHandPoses ); i++ ) {
		if ( !Q_stricmp( vrHandPoses[i].weap, file ) ) {
			return &vrHandPoses[i];
		}
	}
	return NULL;
}

// the flat game's arms are drawn on the gun. With two guns each one has its gun hand: the
// weapon hand's as modelled, the off hand's a mirrored copy of it (CG_VR_AddDualGuns), and
// the support hand isn't on either
static qboolean CG_VR_GunHands( void ) {
	return cgvr.active && vr_drawHands.integer && ( !cgVR->dualWield || cgvr.dualDraw == 1 );
}

// this hand is drawn as part of the held gun this frame
static qboolean CG_VR_HandOnGun( int h ) {
	const vrHandPose_t *hp;

	if ( !cgvr.active || !vr_drawHands.integer || cgVR->binocsOut ) {
		return qfalse;
	}
	if ( cgVR->dualWield ) {
		hp = CG_VR_HandPose( CG_VR_DualSingle( &cg.predictedPlayerState ) );
		return hp && hp->trigPiece != -2 && cgVR->handValid[h];
	}
	hp = CG_VR_HandPose( cg.predictedPlayerState.weapon );
	if ( !hp ) {
		return qfalse;
	}
	if ( h == CG_VR_DominantHand() ) {
		// (not while the off hand holds the gun alone: the gun hand is free for the bolt)
		return hp->trigPiece != -2 && !cgVR->gunInOffHand;
	}
	return hp->supPiece != -2 && ( cgVR->twoHanded || cgVR->gunInOffHand );
}

/*
=================
CG_VR_PieceHandFx

Which of the held weapon's hands to draw on one of its pieces (piece -1: the gun model):
0 draws that first person arm (hand and sleeve), RF_VR_NOHANDS hides it. With BJ's body
drawn the sleeve bends at the elbow up to his shoulder (CG_VR_PieceArmBend).
=================
*/
int CG_VR_PieceHandFx( int weapon, int piece ) {
	const vrHandPose_t *hp;

	if ( !CG_VR_DrawHands() ) {
		return 0;
	}
	hp = CG_VR_GunHands() ? CG_VR_HandPose( weapon ) : NULL;
	if ( hp && piece == hp->trigPiece && !cgVR->gunInOffHand ) {
		return 0;       // the whole first person arm, hand and sleeve
	}
	if ( hp && piece == hp->supPiece && !cgVR->dualWield && ( cgVR->twoHanded || cgVR->gunInOffHand ) ) {
		return 0;
	}
	return RF_VR_NOHANDS;
}

/*
=================
CG_VR_ArmTargets

Where BJ's own arm joins each HD first person arm drawn this frame: the end of its sleeve
towards the elbow (misc/vr/vr_arm_ends.py), and the way the sleeve runs from there to the
hand. cg_vrbody.c bends BJ's arm from the shoulder to that point, so shoulder, BJ's upper
arm and the HD forearm and hand read as one arm.
=================
*/
typedef struct {
	const char  *weap;
	int         hasTrig;
	vec3_t      trig;       // the trigger hand's sleeve end
	int         hasSup;
	vec3_t      sup;        // the support hand's
} vrArmEnd_t;

static const vrArmEnd_t vrArmEnds[] = {
	{ "airstrike.weap", 1, { -21.5f, -17.9f, -23.8f }, 0, { 0, 0, 0 } },
	{ "akimbo.weap", 1, { -14.9f, -35.6f, -16.7f }, 0, { 0, 0, 0 } },
	{ "bar.weap", 1, { -28.3f, -18.1f, 7.2f }, 1, { 25.5f, 2.9f, -16.5f } },
	{ "browning.weap", 0, { 0, 0, 0 }, 1, { -6.2f, 14.1f, -21.4f } },
	{ "colt.weap", 1, { -18.7f, -32.4f, -15.7f }, 0, { 0, 0, 0 } },
	{ "dualtt33.weap", 1, { -9.4f, -31.8f, -15.8f }, 0, { 0, 0, 0 } },
	{ "dynamite.weap", 1, { -21.8f, -22.4f, -34.7f }, 0, { 0, 0, 0 } },
	{ "dynamite_eng.weap", 1, { -21.8f, -22.4f, -34.7f }, 0, { 0, 0, 0 } },
	{ "fg42.weap", 1, { -16.3f, -17.8f, -3.5f }, 1, { 24.2f, 6.4f, -14.5f } },
	{ "fg42scope.weap", 1, { -16.3f, -17.8f, -3.5f }, 0, { 0, 0, 0 } },
	{ "flamethrower.weap", 1, { -18.7f, 16.0f, -10.9f }, 0, { 0, 0, 0 } },
	{ "g43.weap", 1, { -19.1f, -10.2f, 5.5f }, 1, { 0.3f, 10.4f, -15.5f } },
	{ "garand.weap", 0, { 0, 0, 0 }, 1, { -5.0f, 6.8f, -8.6f } },
	{ "grenade.weap", 1, { -12.1f, -30.5f, -21.5f }, 0, { 0, 0, 0 } },
	{ "hdm.weap", 1, { -19.1f, -36.5f, -14.6f }, 0, { 0, 0, 0 } },
	{ "ithaca.weap", 1, { -35.0f, -20.0f, 10.6f }, 1, { -0.4f, 23.4f, -13.5f } },
	{ "knife.weap", 1, { -16.3f, -26.7f, -10.8f }, 0, { 0, 0, 0 } },
	{ "luger.weap", 1, { -15.4f, -31.2f, -11.5f }, 0, { 0, 0, 0 } },
	{ "luger_silenced.weap", 1, { -15.0f, -31.9f, -7.6f }, 0, { 0, 0, 0 } },
	{ "m1garand.weap", 1, { -36.5f, -20.3f, 10.1f }, 1, { 3.9f, 17.8f, -26.7f } },
	{ "m7.weap", 1, { -36.5f, -20.3f, 10.1f }, 1, { 3.9f, 17.8f, -26.7f } },
	{ "mauser.weap", 1, { -39.7f, -22.4f, 10.1f }, 1, { -2.3f, 16.8f, -28.2f } },
	{ "mg42m.weap", 0, { 0, 0, 0 }, 1, { -9.0f, 31.2f, -30.5f } },
	{ "mosin.weap", 1, { -22.1f, -12.8f, 5.7f }, 1, { 5.8f, 9.5f, -15.9f } },
	{ "mp34.weap", 1, { -27.1f, -18.5f, 2.4f }, 1, { 7.0f, 13.6f, -41.2f } },
	{ "mp40.weap", 1, { -21.4f, -11.6f, -6.8f }, 1, { 13.6f, 9.9f, -17.5f } },
	{ "mp44.weap", 1, { -20.7f, -16.5f, 3.9f }, 1, { -2.2f, 17.0f, -17.2f } },
	{ "panzerfaust.weap", 1, { -15.7f, -13.2f, -19.3f }, 0, { 0, 0, 0 } },
	{ "pineapple.weap", 1, { -10.8f, -30.4f, -21.9f }, 0, { 0, 0, 0 } },
	{ "poisongas.weap", 1, { -21.5f, -17.8f, -23.8f }, 0, { 0, 0, 0 } },
	{ "ppsh.weap", 1, { -34.0f, -13.5f, -1.8f }, 1, { 0.8f, 17.9f, -29.9f } },
	{ "revolver.weap", 1, { -25.7f, -23.7f, -12.9f }, 0, { 0, 0, 0 } },
	{ "smokebomb.weap", 1, { -21.5f, -17.8f, -23.8f }, 0, { 0, 0, 0 } },
	{ "sniperrifle.weap", 1, { -39.7f, -22.4f, 10.1f }, 1, { -2.3f, 16.8f, -28.2f } },
	{ "snooper.weap", 0, { 0, 0, 0 }, 1, { -5.0f, 6.8f, -8.6f } },
	{ "sten.weap", 1, { -27.3f, -19.5f, 7.0f }, 1, { 6.6f, 19.0f, -21.1f } },
	{ "tesla.weap", 1, { -12.7f, 17.2f, -9.8f }, 0, { 0, 0, 0 } },
	{ "thompson.weap", 1, { -18.0f, -12.8f, -7.0f }, 1, { 17.8f, 2.9f, -8.0f } },
	{ "tt33.weap", 1, { -9.6f, -27.6f, -14.3f }, 0, { 0, 0, 0 } },
	{ "venom.weap", 1, { -10.0f, 5.2f, -21.7f }, 0, { 0, 0, 0 } },
};

#define VR_FOREARM  10.5f       // wrist to elbow, game units (about 27cm)

// the free hands (hand_r / hand_l), relative to their palm
static const vec3_t vrFreeArmEnd[2] = {
	{ -9.7f, 14.7f, -3.7f },     // VR_HAND_LEFT
	{ -27.0f, -5.8f, 3.9f },     // VR_HAND_RIGHT
};

static const vrArmEnd_t *CG_VR_ArmEnd( int weapon ) {
	const char *file = weapon > WP_NONE && !cg_vanilla_guns.integer ? BG_GetWeaponFilename( weapon ) : NULL;
	int i;

	for ( i = 0; file && *file && i < ARRAY_LEN( vrArmEnds ); i++ ) {
		if ( !Q_stricmp( vrArmEnds[i].weap, file ) ) {
			return &vrArmEnds[i];
		}
	}
	return NULL;
}

/*
=================
CG_VR_DualFrame

With two guns: the first person frame (hands model space) of hand h's gun. The off hand's
is mirrored across the grip, so the gun hand modelled for the right hand becomes a left
hand round the same grip (the gun itself is drawn unmirrored, CG_VR_AddDualGuns).
=================
*/
void CG_VR_DualFrame( int h, int single, vec3_t origin, vec3_t axis[3] ) {
	vrWeaponGeom_t geom;
	vec3_t angles;
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	char buf[32];

	if ( h == CG_VR_DominantHand() ) {
		CG_VR_WeaponPose( single, origin, axis );
		return;
	}
	trap_Cvar_VariableStringBuffer( "vr_weaponPitch", buf, sizeof( buf ) );
	VectorCopy( cgvr.handAngles[h], angles );
	angles[PITCH] += atof( buf );
	CG_VR_Geom( single, &geom );
	BG_VR_WeaponFrame( &geom, cgvr.handOrigin[h], angles, adj, origin, axis );
	VectorMA( origin, 2.0f * geom.grip[1], axis[1], origin );
	VectorNegate( axis[1], axis[1] );
}

void CG_VR_ArmTargets( qboolean has[2], vec3_t end[2], vec3_t dir[2] ) {
	const vrHandPose_t *hp;
	const vrArmEnd_t *ae;
	vec3_t origin, axis[3], hand;
	int weapon = cg.predictedPlayerState.weapon;
	int dom = CG_VR_DominantHand();
	float len;
	int h;

	has[0] = has[1] = qfalse;
	if ( !cgvr.active || cgVR->menuPointer || cg.cameraMode || !CG_VR_DrawHands() ) {
		return;
	}
	if ( cgVR->dualWield ) {
		weapon = CG_VR_DualSingle( &cg.predictedPlayerState );
	}
	hp = CG_VR_HandPose( weapon );
	ae = CG_VR_ArmEnd( weapon );
	for ( h = 0; h < 2; h++ ) {
		if ( !cgVR->handValid[h] ) {
			continue;
		}
		if ( CG_VR_HandOnGun( h ) ) {
			// on the gun: its own arm piece, posed with the gun (two guns: each hand's own,
			// the off hand's mirrored)
			qboolean trig = h == dom || cgVR->dualWield;
			if ( !hp || !ae || !( trig ? ae->hasTrig : ae->hasSup ) ) {
				continue;
			}
			if ( cgVR->dualWield ) {
				CG_VR_DualFrame( h, weapon, origin, axis );
			} else {
				CG_VR_WeaponPose( weapon, origin, axis );
			}
			BG_VR_ModelPoint( origin, axis, trig ? ae->trig : ae->sup, end[h] );
			BG_VR_ModelPoint( origin, axis, trig ? hp->trigWrist : hp->supWrist, hand );
		} else {
			// the free hand model on the controller, drawn as in CG_VR_AddHands
			float s = CG_VR_FreeHandScale( h );
			AnglesToAxis( cgvr.handAngles[h], axis );
			VectorScale( axis[0], s, axis[0] );
			VectorScale( axis[1], s, axis[1] );
			VectorScale( axis[2], s, axis[2] );
			BG_VR_ModelPoint( cgvr.handOrigin[h], axis, vrFreeArmEnd[h], end[h] );
			VectorCopy( cgvr.handOrigin[h], hand );
		}
		VectorSubtract( hand, end[h], dir[h] );
		len = VectorNormalize( dir[h] );
		if ( len > 0.1f ) {
			// the HD arm's elbow: a forearm's length up the sleeve from the wrist (the
			// free hands are measured from the palm); the rest of the sleeve is upper arm
			float forearm = CG_VR_HandOnGun( h ) ? VR_FOREARM : VR_FOREARM + 3.5f;
			if ( forearm > len - 1.0f ) {
				forearm = len - 1.0f;
			}
			cgvr.armStub[h] = len - forearm;
			VectorMA( hand, -forearm, dir[h], end[h] );
			// the wrist joint: the free hands' origin is the palm, a little ahead of it
			if ( CG_VR_HandOnGun( h ) ) {
				VectorCopy( hand, cgvr.armWrist[h] );
				cgvr.armForearm[h] = forearm;
			} else {
				VectorMA( hand, -3.5f, dir[h], cgvr.armWrist[h] );
				cgvr.armForearm[h] = forearm - 3.5f;
			}
			has[h] = qtrue;
		}
	}
}

/*
=================
CG_VR_ArmBend

The HD arm drawn by ent (hand h) gets a wrist and an elbow, solved like FRIK's arms: the
hand stays on the controller (or the gun), the shoulder is BJ's, and a two-bone IK puts
the elbow between them, falling down and out the way the flat game's forearm pointed.
The renderer (R_VR_BendArm) turns the forearm at the wrist and the sleeve past the elbow
up to the shoulder, stretched to reach it, so hand, forearm, elbow and upper arm are one
arm. Needs the full body drawn (it places the shoulders).
=================
*/
// world -> the entity's model space: by the inverse of its axes, which may be scaled, mirrored
// or (a support hand made narrower) skewed
static qboolean CG_VR_EntityInverse( const refEntity_t *ent, vec3_t inv[3] ) {
	vec3_t a[3];
	float det;
	int i;

	AxisCopy( (vec3_t *)ent->axis, a );
	// rows of the inverse of the matrix whose columns are the axes: the cross products
	CrossProduct( a[1], a[2], inv[0] );
	CrossProduct( a[2], a[0], inv[1] );
	CrossProduct( a[0], a[1], inv[2] );
	det = DotProduct( a[0], inv[0] );
	if ( fabs( det ) < 1e-6f ) {
		return qfalse;
	}
	for ( i = 0; i < 3; i++ ) {
		VectorScale( inv[i], 1.0f / det, inv[i] );
	}
	return qtrue;
}

static void CG_VR_ToEntityDir( vec3_t inv[3], const vec3_t world, float *out ) {
	int i;
	for ( i = 0; i < 3; i++ ) {
		out[i] = DotProduct( world, inv[i] );
	}
	VectorNormalize( out );
}

void CG_VR_ArmBend( int h, refEntity_t *ent ) {
	qboolean has[2];
	vec3_t elbow0[2], dir[2], fore0, u, pole, elbow, d1, d2, d, inv[3];
	float lf, lu, dist, a, hgt, upLen, s, sc;
	int i;

	ent->vrBendMagic = 0;
	if ( !CG_VR_BodyArms() || cg.time - cgvr.shoulderTime > 200 ) {
		return;
	}
	CG_VR_ArmTargets( has, elbow0, dir );
	if ( !has[h] || cgvr.armStub[h] < 0.5f || cgvr.armForearm[h] < 1.0f ) {
		return;
	}
	if ( !CG_VR_EntityInverse( ent, inv ) ) {
		return;
	}
	sc = VectorLength( ent->axis[0] ) + VectorLength( ent->axis[1] ) + VectorLength( ent->axis[2] );
	sc /= 3.0f;
	VectorNegate( dir[h], fore0 );      // wrist -> elbow as modelled
	lf = cgvr.armForearm[h];
	lu = cgvr.upperArm[h] > 1.0f ? cgvr.upperArm[h] : 11.5f;

	// two-bone IK, wrist -> shoulder
	VectorSubtract( cgvr.shoulder[h], cgvr.armWrist[h], u );
	dist = VectorNormalize( u );
	if ( dist < 1.0f ) {
		return;
	}
	if ( dist > lu + lf - 0.01f ) {
		a = lf;     // out of reach: straight, the upper arm stretches the rest
		hgt = 0;
	} else {
		float dd = dist < fabs( lu - lf ) + 0.1f ? fabs( lu - lf ) + 0.1f : dist;
		a = ( lf * lf - lu * lu + dd * dd ) / ( 2 * dd );
		hgt = lf * lf - a * a;
		hgt = hgt > 0 ? sqrt( hgt ) : 0;
	}
	// the elbow's side: where the modelled forearm points, pulled down and outwards
	VectorCopy( fore0, pole );
	pole[2] -= 0.6f;
	VectorMA( pole, 0.4f, cgvr.shoulderOut[h], pole );
	VectorMA( pole, -DotProduct( pole, u ), u, pole );
	if ( VectorNormalize( pole ) < 0.001f ) {
		VectorSet( pole, 0, 0, -1 );
		VectorMA( pole, -DotProduct( pole, u ), u, pole );
		VectorNormalize( pole );
	}
	VectorMA( cgvr.armWrist[h], a, u, elbow );
	VectorMA( elbow, hgt, pole, elbow );

	VectorSubtract( elbow, cgvr.armWrist[h], d1 );
	VectorNormalize( d1 );
	VectorSubtract( cgvr.shoulder[h], elbow, d2 );
	upLen = VectorNormalize( d2 );
	s = upLen / cgvr.armStub[h];
	s = s < 0.5f ? 0.5f : ( s > 3.5f ? 3.5f : s );

	// into the entity's model space (its axes may carry a uniform scale)
	VectorSubtract( cgvr.armWrist[h], ent->origin, d );
	for ( i = 0; i < 3; i++ ) {
		ent->vrBend[i] = DotProduct( d, inv[i] );
	}
	CG_VR_ToEntityDir( inv, fore0, ent->vrBend + 3 );
	ent->vrBend[6] = lf / sc;
	CG_VR_ToEntityDir( inv, d1, ent->vrBend + 7 );
	CG_VR_ToEntityDir( inv, d2, ent->vrBend + 10 );
	ent->vrBend[13] = s;
	ent->vrBend[14] = 1.2f / sc;
	ent->vrBendMagic = VR_BEND_MAGIC;
}

// the gun's own first person arms: which hand (if any) this piece of the held weapon is
void CG_VR_PieceArmBend( int weapon, int piece, refEntity_t *ent ) {
	const vrHandPose_t *hp = CG_VR_GunHands() ? CG_VR_HandPose( weapon ) : NULL;
	int dom = CG_VR_DominantHand();

	ent->vrBendMagic = 0;
	if ( !hp || !CG_VR_DrawHands() ) {
		return;
	}
	if ( piece == hp->trigPiece && CG_VR_HandOnGun( dom ) ) {
		CG_VR_ArmBend( dom, ent );      // (-3: the rifle hand, CG_VR_AddRifleHand)
	} else if ( piece == hp->supPiece && !cgVR->dualWield && CG_VR_HandOnGun( !dom ) ) {
		CG_VR_ArmBend( !dom, ent );
	}
}

/*
=================
CG_VR_StretchHand

The smaller of a gun's two modelled hands: its palm and fingers are drawn again stretched
about the wrist to the other hand's size, and left out of the piece itself (the sleeve stays
as modelled, so the arms don't change).
=================
*/
// how much the trigger (or support) hand's palm and fingers are stretched: 1 for the bigger
float CG_VR_HandStretch( int weapon, qboolean trig ) {
	const char *file = weapon > WP_NONE ? BG_GetWeaponFilename( weapon ) : NULL;
	int i;

	if ( !file || !*file ) {
		return 1.0f;
	}
	file = COM_SkipPath( (char *)file );
	for ( i = 0; i < ARRAY_LEN( vrHandSpans ); i++ ) {
		if ( !Q_stricmp( vrHandSpans[i].weap, file ) ) {
			if ( vrHandSpans[i].trig > 0 && vrHandSpans[i].sup > 0 ) {
				float f = trig ? vrHandSpans[i].sup / vrHandSpans[i].trig : vrHandSpans[i].trig / vrHandSpans[i].sup;
				return f > 1.0f ? f : 1.0f;
			}
			break;
		}
	}
	return 1.0f;
}

void CG_VR_StretchHand( int weapon, int piece, refEntity_t *ent ) {
	const vrHandPose_t *hp = CG_VR_GunHands() ? CG_VR_HandPose( weapon ) : NULL;
	const char *file = weapon > WP_NONE ? BG_GetWeaponFilename( weapon ) : NULL;
	vec3_t origin, axis[3], wrist;
	refEntity_t copy;
	float f = 0;
	int i, h, dom = CG_VR_DominantHand();


	if ( !hp || !file || !*file || ( ent->renderfx & RF_VR_NOHANDS ) || piece < -1 ) {
		return;
	}
	if ( piece == hp->trigPiece ) {
		f = CG_VR_HandStretch( weapon, qtrue );
	} else if ( piece == hp->supPiece ) {
		f = CG_VR_HandStretch( weapon, qfalse );
	}
	if ( f < 1.02f ) {
		return;     // the bigger hand (or the same): as modelled
	}
	h = piece == hp->trigPiece ? dom : !dom;
	CG_VR_WeaponPose( weapon, origin, axis );
	BG_VR_ModelPoint( origin, axis, piece == hp->trigPiece ? hp->trigWrist : hp->supWrist, wrist );

	copy = *ent;
	for ( i = 0; i < 3; i++ ) {
		copy.origin[i] = wrist[i] + ( ent->origin[i] - wrist[i] ) * f;
		VectorScale( ent->axis[i], f, copy.axis[i] );
	}
	VectorCopy( copy.origin, copy.oldorigin );
	copy.nonNormalizedAxes = qtrue;
	copy.renderfx |= RF_VR_ONLYHANDS | RF_VR_NOSLEEVES;
	CG_VR_ArmBend( h, &copy );
	trap_R_AddRefEntityToScene( &copy );
	ent->renderfx |= RF_VR_NOPALMS;
}

/*
=================
CG_VR_AddRifleHand

Long guns whose first person model has no trigger hand on screen get the BAR's, moved onto
their wrist (models/vr/rhand_<gun>.md3, misc/vr/vr_rifle_hands.py), drawn with the gun.
=================
*/
static qhandle_t CG_VR_RifleHandModel( int weapon ) {
	static qhandle_t models[WP_NUM_WEAPONS];
	static qboolean tried[WP_NUM_WEAPONS];
	char base[MAX_QPATH];

	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return 0;
	}
	if ( !tried[weapon] ) {
		tried[weapon] = qtrue;
		COM_StripExtension( COM_SkipPath( (char *)BG_GetWeaponFilename( weapon ) ), base, sizeof( base ) );
		models[weapon] = trap_R_RegisterModel( va( "models/vr/rhand_%s.md3", base ) );
	}
	return models[weapon];
}

void CG_VR_AddRifleHand( int weapon, refEntity_t *gun, const refEntity_t *parent ) {
	const vrHandPose_t *hp = CG_VR_GunHands() ? CG_VR_HandPose( weapon ) : NULL;
	qhandle_t model;
	refEntity_t ent;

	if ( !hp || hp->trigPiece != -3 || !CG_VR_HandOnGun( CG_VR_DominantHand() ) ) {
		return;
	}
	model = CG_VR_RifleHandModel( weapon );
	if ( !model ) {
		return;
	}
	// (the hand is modelled in the first person view's space, the hands model the gun hangs
	// from by tag_weapon: placed with the gun it sat off by that tag, forward and turned)
	ent = *gun;
	VectorCopy( parent->origin, ent.origin );
	VectorCopy( parent->origin, ent.oldorigin );
	AxisCopy( (vec3_t *)parent->axis, ent.axis );
	ent.nonNormalizedAxes = parent->nonNormalizedAxes;
	ent.frame = ent.oldframe = 0;
	ent.backlerp = 0;
	ent.hModel = model;
	ent.customSkin = cgvr.handSkin;
	ent.renderfx &= ~( RF_VR_NOHANDS | RF_VR_NOSLEEVES );
	CG_VR_PieceArmBend( weapon, -3, &ent );
	trap_R_AddRefEntityToScene( &ent );
}

// tracked hand models are available and wanted
qboolean CG_VR_DrawHands( void ) {
	trap_Cvar_Update( &vr_drawHands );
	return vr_drawHands.integer && cgvr.handModel[VR_HAND_LEFT] && cgvr.handModel[VR_HAND_RIGHT];
}

/*
=================
CG_VR_AddHands

Both hands, each on its own controller.
=================
*/
void CG_VR_AddHands( void ) {
	refEntity_t ent;
	int h;

	if ( !cgvr.active || cgVR->menuPointer || cg.cameraMode || !CG_VR_DrawHands() ) {
		return;
	}

	for ( h = 0; h < 2; h++ ) {
		if ( !cgVR->handValid[h] || !cgvr.handModel[h] || CG_VR_HandOnGun( h ) ) {
			continue;   // the gun's own first person arm is this hand
		}
		memset( &ent, 0, sizeof( ent ) );
		ent.hModel = cgvr.handModel[h];
		ent.customSkin = cgvr.handSkin;
		VectorCopy( cgvr.handOrigin[h], ent.origin );
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		AnglesToAxis( cgvr.handAngles[h], ent.axis );
		// the free hands are life size too, like the hands on the guns (CG_VR_ScaleFor)
		{
			float s = CG_VR_FreeHandScale( h );
			VectorScale( ent.axis[0], s, ent.axis[0] );
			VectorScale( ent.axis[1], s, ent.axis[1] );
			VectorScale( ent.axis[2], s, ent.axis[2] );
			ent.nonNormalizedAxes = qtrue;
		}
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
		CG_VR_ArmBend( h, &ent );       // its sleeve bends at the elbow up to BJ's shoulder
		trap_R_AddRefEntityToScene( &ent );
	}

	// and a hand sign over the off hand while it's on something it can take
	h = !CG_VR_DominantHand();
	if ( vrCanGrab && vr_grabHint.integer >= 2 && cgVR->handValid[h] ) {
		memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_SPRITE;
		VectorCopy( cgvr.handOrigin[h], ent.origin );
		ent.origin[2] += 4.0f;
		ent.radius = 1.6f;
		ent.customShader = cgs.media.hintShaders[HINT_ACTIVATE];
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
		ent.renderfx = RF_FIRST_PERSON;
		trap_R_AddRefEntityToScene( &ent );
	}
}

/*
=================
CG_VR_Binocs

Binoculars picked from the weapon wheel: the gun is put away and they're held in the gun
hand (the third person model, which sits on the hand like a gun); raised to the eyes they
zoom (cl_vr.c). Picking a weapon, or the weapon changing, puts them away.
=================
*/
qboolean CG_VR_Binocs( playerState_t *ps ) {
	refEntity_t ent;
	int dom = CG_VR_DominantHand();

	if ( !cgVR->binocsOut ) {
		return qfalse;
	}
	if ( !cgvr.active || ps->weapon != cgvr.binocsWeapon || ( ps->eFlags & EF_DEAD ) ||
		 !( ps->stats[STAT_KEYS] & ( 1 << INV_BINOCS ) ) ) {
		cgVR->binocsOut = 0;
		return qfalse;
	}
	cgVR->muzzleValid = qfalse;
	if ( cg.zoomedBinoc || !cgVR->handValid[dom] || !cgs.media.thirdPersonBinocModel ) {
		return qtrue;   // (looking through them: the lens view is drawn instead)
	}
	memset( &ent, 0, sizeof( ent ) );
	ent.hModel = cgs.media.thirdPersonBinocModel;
	AnglesToAxis( cgvr.weaponAngles, ent.axis );
	VectorCopy( cgvr.handOrigin[dom], ent.origin );
	VectorMA( ent.origin, 1.5f, ent.axis[2], ent.origin );     // in the palm, not through it
	VectorCopy( ent.origin, ent.oldorigin );
	VectorCopy( ent.origin, ent.lightingOrigin );
	ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
	trap_R_AddRefEntityToScene( &ent );
	return qtrue;
}

/*
=================
CG_VR_ReloadSounds

The weapon's own reload recording, cut into its clicks by make_vr_assets.py, played as
the player does each step by hand.
=================
*/
#define VR_SND_OUT  0
#define VR_SND_IN   1
#define VR_SND_RACK 2

static void CG_VR_ReloadSound( int weapon, int which ) {
	static sfxHandle_t sounds[WP_NUM_WEAPONS][3];
	static qboolean registered[WP_NUM_WEAPONS];
	static const char *names[3] = { "out", "in", "rack" };
	char base[MAX_QPATH];
	fileHandle_t f;
	int i;

	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return;
	}
	if ( !registered[weapon] ) {
		registered[weapon] = qtrue;
		COM_StripExtension( BG_GetWeaponFilename( weapon ), base, sizeof( base ) );
		for ( i = 0; i < 3; i++ ) {
			const char *path = va( "vr/sound/%s_%s.wav", base, names[i] );
			if ( trap_FS_FOpenFile( path, &f, FS_READ ) > 0 ) {
				trap_FS_FCloseFile( f );
				sounds[weapon][i] = trap_S_RegisterSound( path );
			}
		}
	}
	if ( sounds[weapon][which] ) {
		trap_S_StartLocalSound( sounds[weapon][which], CHAN_ITEM );
	}
}

static void CG_VR_ReloadSounds( void ) {
	static int lastWeapon = -1, lastClip, lastMagOut, lastChambered;
	static qboolean rackPlayed;
	int weapon = cg.predictedPlayerState.weapon;
	int clip = cg.predictedPlayerState.ammoclip[BG_FindClipForWeapon( weapon )];

	if ( weapon == lastWeapon && cgVR->manualReload ) {
		if ( cgVR->magOut && !lastMagOut ) {
			CG_VR_ReloadSound( weapon, VR_SND_OUT );
		} else if ( ( !cgVR->magOut && lastMagOut ) || ( clip > lastClip && !cgVR->magOut ) ) {
			CG_VR_ReloadSound( weapon, VR_SND_IN );
		}
		// the action was worked: once per pull, whether or not it chambered a round
		if ( cgVR->chargeHeld && cgVR->chargePull >= 0.6f && !rackPlayed ) {
			CG_VR_ReloadSound( weapon, VR_SND_RACK );
			rackPlayed = qtrue;
		} else if ( cgVR->chambered && !lastChambered && !rackPlayed ) {
			CG_VR_ReloadSound( weapon, VR_SND_RACK );   // e.g. the Garand's bolt closing on a clip
		}
	}
	if ( !cgVR->chargeHeld ) {
		rackPlayed = qfalse;
	}
	lastWeapon = weapon;
	lastClip = clip;
	lastMagOut = cgVR->magOut;
	lastChambered = cgVR->chambered;
}

/*
=================
CG_VR_SupportSpot

Where the flat game's support hand holds the current gun (the MP40's magazine well, a rifle's
forestock), relative to the gun hand: the client steadies the gun two handed when the off
hand closes its grip there.
=================
*/
static void CG_VR_SupportSpot( void ) {
	int weapon = cg.predictedPlayerState.weapon;
	const char *file = weapon > WP_NONE ? BG_GetWeaponFilename( weapon ) : NULL;
	vrWeaponGeom_t geom;
	int i;

	cgVR->supportValid = 0;
	if ( !file || !*file || cg_vanilla_guns.integer ) {
		return;
	}
	file = COM_SkipPath( (char *)file );
	for ( i = 0; i < ARRAY_LEN( vrHandSpans ); i++ ) {
		if ( !Q_stricmp( vrHandSpans[i].weap, file ) ) {
			break;
		}
	}
	if ( i == ARRAY_LEN( vrHandSpans ) || vrHandSpans[i].sup <= 0 ) {
		return;
	}
	CG_VR_Geom( weapon, &geom );
	cgVR->supportRel[0] = ( vrHandSpans[i].supPalm[0] - geom.grip[0] ) * geom.scale;
	cgVR->supportRel[1] = ( vrHandSpans[i].supPalm[1] - geom.grip[1] ) * geom.scale;
	cgVR->supportRel[2] = ( vrHandSpans[i].supPalm[2] - geom.grip[2] ) * geom.scale;
	cgVR->supportValid = 1;
}

/*
=================
CG_VR_BeginFrame
=================
*/
void CG_VR_BeginFrame( void ) {
	CG_VR_RecoilUpdate();
	cgvr.active = CG_VR_Active();

	if ( !cgvr.active ) {
		return;
	}

	CG_VR_ReloadSounds();
	CG_VR_WeaponScale();        // for the game's reloading too
	CG_VR_SupportSpot();        // where the off hand takes hold of it
	CG_VRX_KnifeFrame();        // GORE & EXTRAS: the blade through a dead body's neck

	// the off hand just came onto something it can take: a light buzz
	trap_Cvar_Update( &vr_grabHint );
	if ( cgVR->canGrab && !vrCanGrab && vr_grabHint.integer >= 1 ) {
		CG_VR_Haptic( !CG_VR_DominantHand(), 0.25f, 25 );
	}
	vrCanGrab = cgVR->canGrab != 0;

	cgVR->cutscene = cg.cameraMode;
	cgVR->twoHandAllowed = !CG_VR_OneHanded( cg.predictedPlayerState.weapon ) &&
						   !BG_DualActive( &cg.predictedPlayerState );   // the off hand has its own gun
	if ( !cg.cameraMode ) {
		cgvr.cutsceneBaseValid = qfalse;
	}
}

// play space offset -> world offset
static void CG_VR_PlayToWorld( const vec3_t playOffset, vec3_t out ) {
	float s = sin( DEG2RAD( cgvr.yawOffset ) ), c = cos( DEG2RAD( cgvr.yawOffset ) );
	float x = playOffset[0], y = playOffset[1];

	out[0] = x * c - y * s;
	out[1] = x * s + y * c;
	out[2] = playOffset[2];
}

/*
=================
CG_VR_SetHead

Fixes the view for this frame: the head is at headOrigin, play space yaw is turned into
world yaw by yawOffset. Writes the head's world angles to viewAngles and places the hands.
=================
*/
void CG_VR_SetHead( const vec3_t headOrigin, float yawOffset, vec3_t viewAngles ) {
	vec3_t offset;
	int h;

	cgvr.yawOffset = yawOffset;
	VectorCopy( headOrigin, cgvr.headOrigin );

	viewAngles[PITCH] = cgVR->hmdAngles[PITCH];
	viewAngles[YAW] = AngleNormalize360( cgVR->hmdAngles[YAW] + yawOffset );
	viewAngles[ROLL] = cgVR->hmdAngles[ROLL];

	for ( h = 0; h < 2; h++ ) {
		VectorSubtract( cgVR->handOrigin[h], cgVR->hmdOrigin, offset );
		CG_VR_PlayToWorld( offset, offset );
		VectorAdd( headOrigin, offset, cgvr.handOrigin[h] );

		VectorCopy( cgVR->handAngles[h], cgvr.handAngles[h] );
		cgvr.handAngles[h][YAW] += yawOffset;
	}

	VectorCopy( cgvr.handOrigin[CG_VR_DominantHand()], cgvr.gunGrip );
	if ( cgVR->gunInOffHand && !cgVR->dualWield ) {
		VectorSubtract( cgVR->gunOrigin, cgVR->hmdOrigin, offset );
		CG_VR_PlayToWorld( offset, offset );
		VectorAdd( headOrigin, offset, cgvr.gunGrip );
	}

	VectorCopy( cgVR->weaponAngles, cgvr.weaponAngles );
	cgvr.weaponAngles[YAW] += yawOffset;
}

/*
=================
CG_VR_CutsceneView

Scripted camera shots: the camera sets the position and direction, the player's head
looks around freely from there (and can lean a little).
=================
*/
void CG_VR_CutsceneView( vec3_t origin, vec3_t angles ) {
	vec3_t offset;
	float cameraYaw = angles[YAW];

	if ( !cgvr.active ) {
		return;
	}

	if ( !cgvr.cutsceneBaseValid ) {
		VectorCopy( cgVR->hmdOrigin, cgvr.cutsceneHmdBase );
		cgvr.cutsceneYawBase = cgVR->hmdAngles[YAW];
		cgvr.cutsceneBaseValid = qtrue;
	}

	// head movement since the shot started, relative to the camera's heading
	cgvr.yawOffset = cameraYaw - cgvr.cutsceneYawBase;
	VectorSubtract( cgVR->hmdOrigin, cgvr.cutsceneHmdBase, offset );
	CG_VR_PlayToWorld( offset, offset );
	VectorAdd( origin, offset, origin );

	CG_VR_SetHead( origin, cgvr.yawOffset, angles );
}

/*
=================
CG_VR_WeaponPose

Where to draw the first person hands+gun model so its grip sits in the player's hand.
=================
*/
void CG_VR_WeaponPose( int weapon, vec3_t origin, vec3_t axis[3] ) {
	char buf[MAX_CVAR_VALUE_STRING];
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	vrWeaponGeom_t geom;

	trap_Cvar_VariableStringBuffer( va( "vr_gunOffset_%d", weapon ), buf, sizeof( buf ) );
	if ( !buf[0] ) {
		trap_Cvar_VariableStringBuffer( "vr_gunOffset", buf, sizeof( buf ) );
	}
	if ( buf[0] ) {
		sscanf( buf, "%f %f %f %f %f %f", &adj[0], &adj[1], &adj[2], &adj[3], &adj[4], &adj[5] );
	}

	CG_VR_Geom( weapon, &geom );
	BG_VR_WeaponFrame( &geom, cgvr.gunGrip, cgvr.weaponAngles, adj, origin, axis );
	if ( cgVR ) {
		VectorMA( origin, -cgVR->recoilBack, axis[0], origin );    // the shot's jolt (cg_vrrecoil.c)
	}

	// remembered for the reload visuals
	if ( weapon != cgvr.gunWeapon ) {
		cgvr.magAxisValid = qfalse;
	}
	cgvr.gunWeapon = weapon;
	VectorCopy( origin, cgvr.gunOrigin );
	AxisCopy( axis, cgvr.gunAxis );
}

/*
=================
CG_VR_WeaponPart

Hand reloading on the first person weapon's linked parts: the magazine is hidden while
it's out of the gun, the slide / bolt / pump slides back while pulled or locked open.
Returns qfalse if the part shouldn't be drawn.
=================
*/
qboolean CG_VR_WeaponPart( int weapon, int part, refEntity_t *parent, refEntity_t *ent ) {
	vrWeaponGeom_t geom;
	vec3_t dir;
	float amount = 0;
	int cls;

	qboolean magOut, chargeHeld, chambered;
	float pull;

	if ( cgvr.inspect ) {
		// vr_inspect: cycle the action and the magazine so they can be checked
		pull = 0.5f + 0.5f * sin( cg.time * 0.003f );
		chargeHeld = qtrue;
		magOut = ( cg.time / 1500 ) & 1;
		chambered = qtrue;
	} else if ( !cgvr.active || !cgVR->manualReload ) {
		return qtrue;
	} else {
		pull = cgVR->chargePull;
		chargeHeld = cgVR->chargeHeld;
		magOut = cgVR->magOut;
		chambered = cgVR->chambered;
	}
	CG_VR_Geom( weapon, &geom );
	cls = BG_VR_ReloadClass( weapon );

	if ( part == geom.magPart ) {
		// how the magazine sits in the gun, for drawing it in the hand / falling out; both
		// axes carry the gun's size (CG_VR_ScaleFor), which mustn't count twice
		int i, j;
		for ( i = 0; i < 3; i++ ) {
			for ( j = 0; j < 3; j++ ) {
				float len = VectorLengthSquared( parent->axis[j] );
				cgvr.magAxis[i][j] = len > 0.0f ? DotProduct( ent->axis[i], parent->axis[j] ) / len : 0.0f;
			}
		}
		cgvr.magAxisValid = qtrue;
		if ( magOut ) {
			return qfalse;
		}
	}

	if ( part == geom.chargePart ) {
		if ( chargeHeld ) {
			amount = pull;
		} else if ( !chambered && cls == VRR_MAG ) {
			amount = 1.0f;      // slide locked back on an empty magazine (a bolt action's bolt
								// stays shut after a shot until it's worked)
		}
		if ( amount > 0 ) {
			BG_VR_ModelDir( parent->axis, geom.chargeDir, dir );
			VectorMA( ent->origin, amount * geom.travel, dir, ent->origin );
		}
	}
	return qtrue;
}

// the magazine's orientation in the world, given the gun's
static void CG_VR_MagAxis( vec3_t gunAxis[3], vec3_t out[3] ) {
	int i, j;

	if ( !cgvr.magAxisValid ) {
		AxisCopy( gunAxis, out );
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			out[i][j] = cgvr.magAxis[i][0] * gunAxis[0][j] + cgvr.magAxis[i][1] * gunAxis[1][j] +
						cgvr.magAxis[i][2] * gunAxis[2][j];
		}
	}
}

/*
=================
CG_VR_Inspect

vr_inspect 1 (monitor only): the current weapon posed exactly as in VR, seen from the
side in front of the camera, with the hand models and markers on the measured spots:
yellow = grip, red = magazine well, green = slide / bolt, blue = where it's pulled to.
The action cycles and the magazine pops in and out. For checking weapon geometry
without a headset.
=================
*/
static void CG_VR_Marker( const vec3_t p, byte r, byte g, byte b ) {
	refEntity_t ent;

	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_SPRITE;
	VectorCopy( p, ent.origin );
	ent.radius = 0.8f;
	ent.customShader = cgs.media.crosshairShader[0];
	ent.shaderRGBA[0] = r;
	ent.shaderRGBA[1] = g;
	ent.shaderRGBA[2] = b;
	ent.shaderRGBA[3] = 255;
	ent.renderfx = RF_DEPTHHACK | RF_FIRST_PERSON;
	trap_R_AddRefEntityToScene( &ent );
}

qboolean CG_VR_Inspect( playerState_t *ps ) {
	vrWeaponGeom_t geom;
	refEntity_t hand;
	vec3_t fwd, right, up, p, axis[3];
	weaponInfo_t *weapon;
	int dom = CG_VR_DominantHand();

	trap_Cvar_Update( &vr_inspect );
	cgvr.inspect = vr_inspect.integer && !cgvr.active;
	if ( !cgvr.inspect || ps->weapon <= WP_NONE ) {
		return qfalse;
	}

	// gun hand in front of the eyes, gun pointing to the right: a side view
	AngleVectors( cg.refdefViewAngles, fwd, right, up );
	VectorMA( cg.refdef.vieworg, 34, fwd, cgvr.handOrigin[dom] );
	VectorMA( cgvr.handOrigin[dom], -10, right, cgvr.handOrigin[dom] );
	VectorMA( cgvr.handOrigin[dom], -3, up, cgvr.handOrigin[dom] );
	VectorSet( cgvr.weaponAngles, 0, cg.refdefViewAngles[YAW] - 90, 0 );
	VectorCopy( cgvr.weaponAngles, cgvr.handAngles[dom] );
	VectorMA( cgvr.handOrigin[dom], -8, up, cgvr.handOrigin[!dom] );
	VectorCopy( cgvr.weaponAngles, cgvr.handAngles[!dom] );

	weapon = &cg_weapons[ps->weapon];
	memset( &hand, 0, sizeof( hand ) );
	CG_VR_WeaponPose( ps->weapon, hand.origin, hand.axis );
	CG_WeaponAnimation( ps, weapon, &hand.oldframe, &hand.frame, &hand.backlerp );
	VectorCopy( hand.origin, hand.lightingOrigin );
	hand.hModel = weapon->handsModel;
	hand.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_VR_NOHANDS;
	CG_AddPlayerWeapon( &hand, ps, &cg.predictedPlayerEntity );

	// our hand on the grip
	if ( cgvr.handModel[dom] ) {
		refEntity_t h;
		memset( &h, 0, sizeof( h ) );
		h.hModel = cgvr.handModel[dom];
		h.customSkin = cgvr.handSkin;
		VectorCopy( cgvr.handOrigin[dom], h.origin );
		AnglesToAxis( cgvr.handAngles[dom], h.axis );
		h.renderfx = RF_FIRST_PERSON | RF_MINLIGHT;
		trap_R_AddRefEntityToScene( &h );
	}

	CG_VR_Geom( ps->weapon, &geom );
	AxisCopy( hand.axis, axis );
	CG_VR_Marker( cgvr.handOrigin[dom], 255, 255, 0 );
	// ruler along the gun from the grip: a tick every 5 units, brighter every 10
	{
		int k;
		for ( k = -6; k <= 10; k++ ) {
			byte c = ( k % 2 ) ? 110 : 255;
			if ( k == 0 ) {
				continue;
			}
			VectorMA( cgvr.handOrigin[dom], k * 5.0f, axis[0], p );
			VectorMA( p, -6.0f, axis[2], p );
			CG_VR_Marker( p, c, c, c );
		}
	}
	BG_VR_ModelPoint( hand.origin, axis, geom.mag, p );
	CG_VR_Marker( p, 255, 40, 40 );
	BG_VR_ModelPoint( hand.origin, axis, geom.charge, p );
	CG_VR_Marker( p, 40, 255, 40 );
	{
		vec3_t d, q;
		BG_VR_ModelDir( axis, geom.chargeDir, d );
		VectorMA( p, geom.travel, d, q );
		CG_VR_Marker( q, 60, 120, 255 );
	}

	CG_CenterPrint( va( "%s  class %d  mag part %d  charge part %d  travel %.1f",
						BG_GetWeaponFilename( ps->weapon ), BG_VR_ReloadClass( ps->weapon ),
						geom.magPart, geom.chargePart, geom.travel ), SCREEN_HEIGHT * 0.85, SMALLCHAR_WIDTH );
	cg.predictedPlayerEntity.lastWeaponClientFrame = cg.clientFrame;
	return qtrue;
}

/*
=================
CG_VR_AddReloadItems

The magazine / clip / shell in the off hand, and magazines falling out of the gun.
=================
*/
void CG_VR_AddReloadItems( void ) {
	weaponInfo_t *weapon;
	vrWeaponGeom_t geom;
	refEntity_t ent;
	vec3_t magPoint;
	qhandle_t model = 0;
	int off = !CG_VR_DominantHand();
	char pouchBuf[8];

	if ( !cgvr.active || cgvr.gunWeapon <= WP_NONE ) {
		return;
	}
	weapon = &cg_weapons[cgvr.gunWeapon];
	CG_VR_Geom( cgvr.gunWeapon, &geom );
	if ( geom.magPart >= 0 && geom.magPart < W_MAX_PARTS ) {
		model = weapon->partModels[W_FP_MODEL][geom.magPart].model;
	}

	// a magazine fell out: let it drop
	if ( cgVR->magDropCount != cgvr.lastMagDrop ) {
		cgvr.lastMagDrop = cgVR->magDropCount;
		if ( model ) {
			localEntity_t *le = CG_AllocLocalEntity();
			refEntity_t *re = &le->refEntity;

			BG_VR_ModelPoint( cgvr.gunOrigin, cgvr.gunAxis, geom.mag, magPoint );
			CG_VR_MagAxis( cgvr.gunAxis, re->axis );
			BG_VR_ModelDir( re->axis, geom.magLocal, re->origin );
			VectorSubtract( magPoint, re->origin, re->origin );
			re->hModel = model;
			re->renderfx = RF_MINLIGHT;
			re->nonNormalizedAxes = qtrue;

			le->leType = LE_FRAGMENT;
			le->startTime = cg.time;
			le->endTime = cg.time + 10000;
			le->pos.trType = TR_GRAVITY;
			le->pos.trTime = cg.time;
			VectorCopy( re->origin, le->pos.trBase );
			BG_VR_ModelDir( cgvr.gunAxis, geom.magDir, le->pos.trDelta );
			VectorScale( le->pos.trDelta, 60, le->pos.trDelta );
			le->bounceFactor = 0.25f;
			le->angles.trType = TR_STATIONARY;
			le->leBounceSoundType = LEBS_BRASS;
			le->leMarkType = LEMT_NONE;
		}
	}

	// held in the off hand, lined up with the gun so it goes straight in
	if ( cgVR->heldItem && cgVR->handValid[off] ) {
		memset( &ent, 0, sizeof( ent ) );
		ent.hModel = model ? model : cgs.media.smallgunBrassModel;
		{
			// turns with the hand holding it, and lines up with the gun close to its well
			// so it goes straight in
			vec3_t handAxis[3], gunMag[3], well;
			float sc = VectorLength( cgvr.gunAxis[0] ), f;
			int i;

			AnglesToAxis( cgvr.handAngles[off], handAxis );
			for ( i = 0; i < 3; i++ ) {
				VectorScale( handAxis[i], sc, handAxis[i] );
			}
			CG_VR_MagAxis( handAxis, ent.axis );
			CG_VR_MagAxis( cgvr.gunAxis, gunMag );
			BG_VR_ModelPoint( cgvr.gunOrigin, cgvr.gunAxis, geom.mag, well );
			f = ( Distance( well, cgvr.handOrigin[off] ) - 4.0f ) / 8.0f;     // 0 at the well
			f = f < 0 ? 0 : ( f > 1 ? 1 : f );
			for ( i = 0; i < 3; i++ ) {
				float len;
				VectorScale( ent.axis[i], f, ent.axis[i] );
				VectorMA( ent.axis[i], 1.0f - f, gunMag[i], ent.axis[i] );
				len = VectorLength( gunMag[i] );
				VectorNormalize( ent.axis[i] );
				VectorScale( ent.axis[i], len, ent.axis[i] );
			}
		}
		if ( model ) {
			BG_VR_ModelDir( ent.axis, geom.magLocal, ent.origin );
			VectorSubtract( cgvr.handOrigin[off], ent.origin, ent.origin );
		} else {
			VectorCopy( cgvr.handOrigin[off], ent.origin );
		}
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
		ent.nonNormalizedAxes = qtrue;
		trap_R_AddRefEntityToScene( &ent );
	}
	// spare magazines in the chest pouch, where the off hand takes them from (g_vr.c)
	trap_Cvar_VariableStringBuffer( "vr_pouchMags", pouchBuf, sizeof( pouchBuf ) );
	if ( model && BG_VR_ReloadClass( cgvr.gunWeapon ) == VRR_MAG && CG_VR_BodyDrawn() && atoi( pouchBuf ) ) {
		int spare, i, clip = ammoTable[cgvr.gunWeapon].maxclip;
		vec3_t yaw, fwd, right, pouch;

		spare = clip > 0 ? cg.predictedPlayerState.ammo[BG_FindAmmoForWeapon( cgvr.gunWeapon )] / clip : 0;
		if ( spare > 3 ) {
			spare = 3;
		}
		VectorSet( yaw, 0, cg.refdefViewAngles[YAW], 0 );
		AngleVectors( yaw, fwd, right, NULL );
		for ( i = 0; i < spare; i++ ) {
			memset( &ent, 0, sizeof( ent ) );
			ent.hModel = model;
			AxisCopy( axisDefault, ent.axis );
			VectorScale( fwd, geom.scale, ent.axis[0] );
			VectorScale( right, -geom.scale, ent.axis[1] );
			VectorScale( ent.axis[2], geom.scale, ent.axis[2] );
			ent.nonNormalizedAxes = qtrue;
			// low on the chest, inside the server's pouch (VR_CHEST_DROP), side by side
			VectorMA( cg.refdef.vieworg, 5.0f, fwd, pouch );
			VectorMA( pouch, ( i - ( spare - 1 ) * 0.5f ) * 2.6f, right, pouch );
			pouch[2] -= 24.0f;
			BG_VR_ModelDir( ent.axis, geom.magLocal, ent.origin );
			VectorSubtract( pouch, ent.origin, ent.origin );
			VectorCopy( ent.origin, ent.oldorigin );
			VectorCopy( ent.origin, ent.lightingOrigin );
			ent.renderfx = RF_FIRST_PERSON | RF_LIGHTING_ORIGIN;
			trap_R_AddRefEntityToScene( &ent );
		}
	}
}

/*
=================
CG_VR_MuzzleFallback

A few first person guns have no tag_flash (the flash and the shots came out of the gun's
origin, behind the hand): their muzzles, measured from the models (misc/vr), in the hands
model's space.
=================
*/
static const struct {
	const char  *weap;
	vec3_t      muzzle;
} vrMuzzles[] = {
	{ "mp40.weap", { 40.5f, -4.85f, -5.5f } },
	{ "mp44.weap", { 67.1f, -4.95f, -9.9f } },
};

void CG_VR_MuzzleFallback( int weapon, const refEntity_t *parent, refEntity_t *flash ) {
	const char *file = weapon > WP_NONE ? BG_GetWeaponFilename( weapon ) : NULL;
	int i, j;

	if ( !cgvr.active || !file || !*file ) {
		return;
	}
	for ( i = 0; i < ARRAY_LEN( vrMuzzles ); i++ ) {
		if ( !Q_stricmp( vrMuzzles[i].weap, file ) ) {
			VectorCopy( parent->origin, flash->origin );
			for ( j = 0; j < 3; j++ ) {
				VectorMA( flash->origin, vrMuzzles[i].muzzle[j], parent->axis[j], flash->origin );
				VectorNormalize2( parent->axis[j], flash->axis[j] );
			}
			return;
		}
	}
}

/*
=================
CG_VR_SetMuzzle

Called with the first person gun's muzzle (tag_flash) every frame. The server fires from
the same spot relative to the weapon hand, so bullets leave the barrel.
=================
*/
void CG_VR_SetMuzzle( const vec3_t muzzle ) {
	vec3_t d, aim[3];

	VectorSubtract( muzzle, cgvr.gunGrip, d );
	if ( VectorLength( d ) > 80.0f ) {
		cgVR->muzzleValid = qfalse;     // tag missing or the gun is mid animation somewhere odd
		return;
	}
	AnglesToAxis( cgvr.weaponAngles, aim );
	cgVR->muzzleOffset[0] = DotProduct( d, aim[0] );
	cgVR->muzzleOffset[1] = DotProduct( d, aim[1] );
	cgVR->muzzleOffset[2] = DotProduct( d, aim[2] );
	cgVR->muzzleValid = qtrue;
	VectorCopy( muzzle, cgvr.muzzle );
}

// weapons held in one hand: the off hand grip doesn't turn them into a rifle hold
static qboolean CG_VR_OneHanded( int weapon ) {
	switch ( weapon ) {
	case WP_NONE:
	case WP_KNIFE:
	case WP_LUGER:
	case WP_SILENCER:
	case WP_COLT:
	case WP_TT33:
	case WP_REVOLVER:
	case WP_HDM:
	case WP_AKIMBO:
	case WP_DUAL_TT33:
	case WP_GRENADE_LAUNCHER:
	case WP_GRENADE_PINEAPPLE:
	case WP_DYNAMITE:
	case WP_DYNAMITE_ENG:
	case WP_AIRSTRIKE:
	case WP_ARTY:
	case WP_POISONGAS:
	case WP_SMOKETRAIL:
	case WP_SMOKE_BOMB:
	case WP_HOLYCROSS:
		return qtrue;
	default:
		return qfalse;
	}
}

// weapons that fire bullets or projectiles along the barrel get an aim reticle
static qboolean CG_VR_HasReticle( int weapon ) {
	return !CG_VR_OneHanded( weapon ) || weapon == WP_LUGER || weapon == WP_SILENCER || weapon == WP_COLT ||
		   weapon == WP_TT33 || weapon == WP_REVOLVER || weapon == WP_HDM || weapon == WP_AKIMBO ||
		   weapon == WP_DUAL_TT33 ||
		   // thrown things land on it (G_VR_LobAt)
		   weapon == WP_KNIFE || weapon == WP_GRENADE_LAUNCHER || weapon == WP_GRENADE_PINEAPPLE ||
		   weapon == WP_DYNAMITE || weapon == WP_DYNAMITE_ENG || weapon == WP_SMOKE_BOMB || weapon == WP_POISONGAS;
}

// a dot where a shot from start along angles lands, the same apparent size at any distance
static void CG_VR_ReticleDot( const vec3_t start, const vec3_t angles ) {
	refEntity_t ent;
	trace_t tr;
	vec3_t end, aim[3], toEye;
	float dist;

	AnglesToAxis( angles, aim );
	VectorMA( start, 8192, aim[0], end );
	CG_Trace( &tr, start, NULL, NULL, end, cg.predictedPlayerState.clientNum, MASK_SHOT );
	if ( tr.startsolid ) {
		return;
	}

	// pull it a little towards the viewer so it doesn't sink into the surface
	VectorSubtract( cg.refdef.vieworg, tr.endpos, toEye );
	dist = VectorNormalize( toEye );
	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_SPRITE;
	VectorMA( tr.endpos, 2.0f, toEye, ent.origin );
	ent.radius = dist * 0.006f;     // about 0.7 degrees across
	if ( ent.radius < 0.4f ) {
		ent.radius = 0.4f;
	}
	ent.customShader = cgs.media.crosshairShader[0];
	ent.shaderRGBA[0] = 255;
	ent.shaderRGBA[1] = 255;
	ent.shaderRGBA[2] = 255;
	ent.shaderRGBA[3] = 255;
	ent.renderfx = RF_DEPTHHACK | RF_FIRST_PERSON;
	trap_R_AddRefEntityToScene( &ent );
}

/*
=================
CG_VR_AddReticle

A dot where the shot would land: traced from the muzzle along the aim, like the server
fires it (and one for the off hand's gun of a pair). Thrown things are lobbed onto it.
=================
*/
void CG_VR_AddReticle( void ) {
	int weapon = cg.predictedPlayerState.weapon;

	trap_Cvar_Update( &vr_reticle );
	if ( !cgvr.active || !vr_reticle.integer || cgVR->menuPointer || cg.cameraMode ||
		 !CG_VR_HasReticle( weapon ) || cg.predictedPlayerState.stats[STAT_HEALTH] <= 0 ) {
		return;
	}
	CG_VR_ReticleDot( cgVR->muzzleValid ? cgvr.muzzle : cgvr.handOrigin[CG_VR_DominantHand()], cgvr.weaponAngles );
	if ( cgVR->dualWield && cgVR->handValid[!CG_VR_DominantHand()] ) {
		CG_VR_ReticleDot( cgvr.muzzleOff, cgvr.aimOff );
	}
}

/*
=================
CG_VR_AddPointer

Laser pointer for menus opened over the game world.
=================
*/
void CG_VR_AddPointer( void ) {
	polyVert_t verts[4];
	vec3_t start, end, dir, toView, side;
	char buf[64];
	float col[3] = { 1, 0.25f, 0.15f };
	float width;
	int i;

	if ( !cgvr.active || !cgVR->menuPointer || !cgVR->pointerVisible ) {
		return;
	}

	VectorSubtract( cgVR->pointerStart, cgVR->hmdOrigin, start );
	CG_VR_PlayToWorld( start, start );
	VectorAdd( cgvr.headOrigin, start, start );
	VectorSubtract( cgVR->pointerEnd, cgVR->hmdOrigin, end );
	CG_VR_PlayToWorld( end, end );
	VectorAdd( cgvr.headOrigin, end, end );

	trap_Cvar_VariableStringBuffer( "vr_laserPointerColor", buf, sizeof( buf ) );
	sscanf( buf, "%f %f %f", &col[0], &col[1], &col[2] );

	width = 0.006f * cgVR->worldScale;
	VectorSubtract( end, start, dir );
	VectorSubtract( cg.refdef.vieworg, start, toView );
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
		verts[i].modulate[0] = (byte)( 255 * col[0] );
		verts[i].modulate[1] = (byte)( 255 * col[1] );
		verts[i].modulate[2] = (byte)( 255 * col[2] );
		verts[i].modulate[3] = 255;
	}
	trap_R_AddPolyToScene( cgs.media.whiteShader, 4, verts );
}

/*
=================
CG_VR_Haptic
=================
*/
void CG_VR_Haptic( int hand, float amplitude, int msec ) {
	if ( !CG_VR_Active() || hand < 0 || hand > 1 ) {
		return;
	}
	if ( amplitude > cgVR->hapticAmplitude[hand] ) {
		cgVR->hapticAmplitude[hand] = amplitude;
		cgVR->hapticDuration[hand] = msec;
	}
}

/*
==============================================================================

Instant hits: the player's own bullet impacts are drawn the moment the gun fires, from
its muzzle along its aim, instead of when the server's impact events come back (a
server frame, plus snapshot interpolation, plus the round trip in co-op). The server
still decides damage; its impact events for the player's own bullets are skipped.
In VR only: the server's random spread is small there (at most ~0.7 degrees with the
VR accuracy setting, none for scoped weapons), so the drawn hit is where the real one is.

==============================================================================
*/

// weapons that fire single bullets straight from the gun (no pellets, no projectiles)
static qboolean CG_VR_HitscanWeapon( int weapon ) {
	switch ( weapon ) {
	case WP_LUGER:
	case WP_SILENCER:
	case WP_COLT:
	case WP_AKIMBO:
	case WP_TT33:
	case WP_DUAL_TT33:
	case WP_REVOLVER:
	case WP_HDM:
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_PPSH:
	case WP_MP34:
	case WP_MAUSER:
	case WP_GARAND:
	case WP_MOSIN:
	case WP_DELISLE:
	case WP_M1GARAND:
	case WP_G43:
	case WP_M1941:
	case WP_MP44:
	case WP_FG42:
	case WP_BAR:
	case WP_SNIPERRIFLE:
	case WP_SNOOPERSCOPE:
	case WP_DELISLESCOPE:
	case WP_FG42SCOPE:
	case WP_M1941SCOPE:
	case WP_MG42M:
	case WP_BROWNING:
		return qtrue;
	default:
		return qfalse;
	}
}

// vr_debug 2: when the server's impact for one of our bullets arrives
void CG_VR_DebugServerImpact( int shooter ) {
	char dbg[8];

	trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
	if ( atoi( dbg ) == 2 && shooter == cg.snap->ps.clientNum ) {
		CG_Printf( "VR fire: server impact at %d ms\n", trap_Milliseconds() );
	}
}

qboolean CG_VR_PredictsHits( int weapon ) {
	trap_Cvar_Update( &vr_predictHits );
	return cgvr.active && vr_predictHits.integer && !cg.demoPlayback && CG_VR_HitscanWeapon( weapon );
}

/*
=================
CG_VR_PredictBullet

The player's gun just fired (predicted EV_FIRE_WEAPON).
=================
*/
void CG_VR_PredictBullet( int weapon ) {
	vec3_t start, end, forward, reflect;
	trace_t tr;
	int self = cg.snap->ps.clientNum;
	char dbg[8];

	trap_Cvar_VariableStringBuffer( "vr_debug", dbg, sizeof( dbg ) );
	if ( atoi( dbg ) == 2 ) {
		CG_Printf( "VR fire: shot at %d ms\n", trap_Milliseconds() );
	}
	if ( !CG_VR_PredictsHits( weapon ) || !cgVR->muzzleValid ) {
		return;
	}
	if ( cgvr.lastShotLeft ) {
		VectorCopy( cgvr.muzzleOff, start );   // the off hand's gun of a pair
		AngleVectors( cgvr.aimOff, forward, NULL, NULL );
	} else {
		VectorCopy( cgvr.muzzle, start );
		AngleVectors( cgvr.weaponAngles, forward, NULL, NULL );
	}
	VectorMA( start, 8192 * 2, forward, end );

	CG_Trace( &tr, start, NULL, NULL, end, self, MASK_SHOT );
	if ( tr.fraction >= 1.0f || ( tr.surfaceFlags & ( SURF_NOIMPACT | SURF_SKY ) ) ) {
		return;
	}
	if ( atoi( dbg ) == 2 ) {
		CG_Printf( "VR fire: impact drawn %.0f units away (entity %d)\n", Distance( start, tr.endpos ), tr.entityNum );
	}

	if ( tr.entityNum < MAX_CLIENTS && tr.entityNum != self ) {
		CG_Bullet( tr.endpos, self, tr.plane.normal, qtrue, tr.entityNum, qfalse, ENTITYNUM_NONE );
		CG_Ragdoll_Hit( tr.entityNum, tr.endpos, forward, 160.0f );     // knocks a body where it's hit
		CG_VRX_Wound( tr.entityNum, tr.endpos, forward );
	} else {
		VectorMA( forward, -2 * DotProduct( forward, tr.plane.normal ), tr.plane.normal, reflect );
		VectorNormalize( reflect );
		CG_Bullet( tr.endpos, self, reflect, qfalse, ENTITYNUM_WORLD, qfalse, ENTITYNUM_NONE );
	}
}

/*
=================
CG_VR_DualSingle / CG_VR_AddDualGuns

Two pistols or SMGs: one in each hand, each fired by its own trigger (cl_vr.c,
BG_AkimboFireSequence, BG_DualLeftFires). They're drawn as two of the single gun, each its
own unmirrored copy (the akimbo pair's model has the left gun fixed to the right hand).
=================
*/
int CG_VR_DualSingle( const playerState_t *ps ) {
	switch ( ps->weapon ) {
	case WP_AKIMBO:
		return WP_COLT;
	case WP_DUAL_TT33:
		return WP_TT33;
	default:
		return BG_DualActive( ps ) ? ps->weapon : 0;     // any pistol or SMG, dual wielded
	}
}

// the off hand's gun hand: the gun hand's own modelled hand, mirrored into a left hand round
// the same grip (only the hand is drawn mirrored, the gun stays as modelled)
static void CG_VR_DualOffHand( int single, weaponInfo_t *weapon ) {
	const vrHandPose_t *hp = CG_VR_HandPose( single );
	int off = !CG_VR_DominantHand();
	refEntity_t parent, ent;
	float f;

	if ( !CG_VR_HandOnGun( off ) || !CG_VR_DrawHands() || !hp ) {
		return;
	}
	memset( &parent, 0, sizeof( parent ) );
	CG_VR_DualFrame( off, single, parent.origin, parent.axis );
	parent.hModel = weapon->handsModel;
	parent.frame = parent.oldframe = weapon->weapAnimations[WEAP_IDLE1].firstFrame;
	parent.nonNormalizedAxes = qtrue;

	memset( &ent, 0, sizeof( ent ) );
	if ( hp->trigPiece == -1 ) {
		ent.hModel = weapon->weaponModel[W_FP_MODEL].model;
		ent.customSkin = weapon->handsSkin;
		CG_PositionEntityOnTag( &ent, &parent, "tag_weapon", 0, NULL );
	} else if ( hp->trigPiece == -3 ) {
		ent.hModel = CG_VR_RifleHandModel( single );
		ent.customSkin = cgvr.handSkin;
		VectorCopy( parent.origin, ent.origin );
		AxisCopy( parent.axis, ent.axis );
	}
	if ( !ent.hModel ) {
		return;
	}
	VectorCopy( ent.origin, ent.oldorigin );
	VectorCopy( parent.origin, ent.lightingOrigin );
	ent.nonNormalizedAxes = qtrue;
	ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN | RF_VR_ONLYHANDS | RF_VR_MIRRORED;
	f = CG_VR_HandStretch( single, qtrue );
	if ( f > 1.02f ) {
		// as CG_VR_StretchHand: the palm and fingers stretched about the wrist, the sleeve not
		refEntity_t palm = ent;
		vec3_t origin, axis[3], wrist;
		int i;

		CG_VR_DualFrame( off, single, origin, axis );
		BG_VR_ModelPoint( origin, axis, hp->trigWrist, wrist );
		for ( i = 0; i < 3; i++ ) {
			palm.origin[i] = wrist[i] + ( ent.origin[i] - wrist[i] ) * f;
			VectorScale( ent.axis[i], f, palm.axis[i] );
		}
		VectorCopy( palm.origin, palm.oldorigin );
		palm.renderfx |= RF_VR_NOSLEEVES;
		CG_VR_ArmBend( off, &palm );
		trap_R_AddRefEntityToScene( &palm );
		ent.renderfx |= RF_VR_NOPALMS;
	}
	CG_VR_ArmBend( off, &ent );
	trap_R_AddRefEntityToScene( &ent );
}

// the y (model space, to the left) of the middle of each dual wielded gun's body, the median
// of its vertices (scratch measure with make_vr_assets' posing): the mirrored hand closes round
// the grip mirrored about grip y, so the unmirrored gun moves by twice the difference
typedef struct {
	const char  *weap;
	float       y;
} vrDualCenter_t;

static const vrDualCenter_t vrDualCenters[] = {
	{ "luger.weap", -4.69f },
	{ "luger_silenced.weap", -4.69f },
	{ "colt.weap", -5.05f },
	{ "tt33.weap", -5.33f },
	{ "revolver.weap", -6.16f },
	{ "hdm.weap", -6.90f },
	{ "mp40.weap", -5.03f },
	{ "thompson.weap", -2.78f },
	{ "sten.weap", -5.36f },
	{ "ppsh.weap", -4.38f },
	{ "mp34.weap", -5.91f },
};

static float CG_VR_DualCenterY( int weapon, float fallback ) {
	const char *file = BG_GetWeaponFilename( weapon );
	int i;

	if ( !file || cg_vanilla_guns.integer ) {
		return fallback;
	}
	for ( i = 0; i < (int)ARRAY_LEN( vrDualCenters ); i++ ) {
		if ( !Q_stricmp( vrDualCenters[i].weap, file ) ) {
			return vrDualCenters[i].y;
		}
	}
	return fallback;
}

void CG_VR_AddDualGuns( playerState_t *ps, int single ) {
	centity_t *cent = &cg.predictedPlayerEntity;
	weaponInfo_t *weapon;
	refEntity_t hand;
	vec3_t angles = { 0, 0, 0 };
	vrWeaponGeom_t geom;
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	char buf[32];
	int side, savedWeapon = cent->currentState.weapon, savedFlash = cent->muzzleFlashTime;
	int off = !CG_VR_DominantHand();

	CG_RegisterWeapon( single, qfalse );
	weapon = &cg_weapons[single];
	trap_Cvar_VariableStringBuffer( "vr_weaponPitch", buf, sizeof( buf ) );

	// the off hand's gun first, so the muzzle the server is told about is the weapon hand's
	for ( side = 0; side < 2; side++ ) {
		memset( &hand, 0, sizeof( hand ) );
		if ( side == 1 ) {
			CG_VR_WeaponPose( single, hand.origin, hand.axis );
		} else {
			if ( !cgVR->handValid[off] ) {
				continue;
			}
			VectorCopy( cgvr.handAngles[off], angles );
			angles[PITCH] += atof( buf );
			// the gun as modelled (its ejection port and parts where they really are), held by
			// the mirrored hand (CG_VR_DualOffHand): moved across so its body sits where the
			// mirrored hand closes round it, not through the left palm
			CG_VR_Geom( single, &geom );
			BG_VR_WeaponFrame( &geom, cgvr.handOrigin[off], angles, adj, hand.origin, hand.axis );
			VectorMA( hand.origin, 2.0f * ( geom.grip[1] - CG_VR_DualCenterY( single, geom.grip[1] ) ),
					  hand.axis[1], hand.origin );
			CG_VR_DualOffHand( single, weapon );
		}
		cgvr.dualDraw = side == 1 ? 1 : 2;
		// (runs the animation state the gun's parts read, with this pistol's own table)
		CG_WeaponAnimation( ps, weapon, &hand.oldframe, &hand.frame, &hand.backlerp );
		hand.oldframe = hand.frame = weapon->weapAnimations[WEAP_IDLE1].firstFrame;
		hand.backlerp = 0;
		VectorCopy( hand.origin, hand.lightingOrigin );
		hand.hModel = weapon->handsModel;
		hand.renderfx = RF_FIRST_PERSON | RF_MINLIGHT;
		if ( CG_VR_DrawHands() ) {
			hand.renderfx |= RF_VR_NOHANDS;
		}
		// only the gun that fired flashes
		cent->muzzleFlashTime = ( ( side == 0 ) == cgvr.lastShotLeft ) ? savedFlash : 0;
		cent->currentState.weapon = single;
		cg_dualOneHand = CG_DualSMG( single );
		CG_AddPlayerWeapon( &hand, ps, cent );
		cg_dualOneHand = qfalse;
		cgvr.dualDraw = 0;
		if ( side == 0 ) {
			// the off hand's muzzle, for drawing its shots' hits
			VectorCopy( cgvr.muzzle, cgvr.muzzleOff );
			VectorCopy( angles, cgvr.aimOff );
		}
	}
	cent->currentState.weapon = savedWeapon;
	cent->muzzleFlashTime = savedFlash;
}
