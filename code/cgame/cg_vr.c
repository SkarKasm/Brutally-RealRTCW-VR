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
//
// Co-op: the hand reloading state comes from the server in playerState vrState/vrGrab
// and is unpacked into the shared struct every frame (CG_VR_BeginFrame).

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
CG_VR_MapSkin

RealRTCW picks the hands' and the HD body's gloves and sleeves by map with a .smartskin
(the renderer's RegisterSmartSkin). The co-op engine has no smart skins, so the profile
is chosen here from the same file's "profile <name> maps <map> ... {" lists, and a plain
skin made for it is used: "<smartskin without extension>_<profile>.skin", or
"..._default.skin" (the base block) for any other map.
=================
*/
static qhandle_t CG_VR_MapSkin( const char *smartSkin, const char *map ) {
	char base[MAX_QPATH], profile[MAX_QPATH], text[8192];
	char *p, *tok;
	fileHandle_t f = 0;
	int len;
	qhandle_t skin;

	COM_StripExtension( smartSkin, base, sizeof( base ) );
	Q_strncpyz( profile, "default", sizeof( profile ) );

	len = trap_FS_FOpenFile( smartSkin, &f, FS_READ );
	if ( len > 0 && len < (int)sizeof( text ) ) {
		trap_FS_Read( text, len, f );
		text[len] = 0;
		p = text;
		while ( 1 ) {
			tok = COM_ParseExt( &p, qtrue );
			if ( !tok[0] ) {
				break;
			}
			if ( !Q_stricmp( tok, "{" ) ) {
				SkipBracedSection( &p, 1 );     // the surfaces: not needed here
				continue;
			}
			if ( Q_stricmp( tok, "profile" ) ) {
				continue;
			}
			{
				char name[MAX_QPATH];
				qboolean match = qfalse;

				Q_strncpyz( name, COM_ParseExt( &p, qtrue ), sizeof( name ) );
				if ( Q_stricmp( COM_ParseExt( &p, qtrue ), "maps" ) ) {
					continue;
				}
				while ( 1 ) {
					tok = COM_ParseExt( &p, qtrue );
					if ( !tok[0] || !Q_stricmp( tok, "{" ) ) {
						break;
					}
					if ( !Q_stricmp( tok, map ) ) {
						match = qtrue;
					}
				}
				if ( tok[0] ) {
					SkipBracedSection( &p, 1 );
				}
				if ( match && name[0] ) {
					Q_strncpyz( profile, name, sizeof( profile ) );
					break;
				}
			}
		}
	}
	if ( f ) {
		trap_FS_FCloseFile( f );
	}

	skin = trap_R_RegisterSkin( va( "%s_%s.skin", base, profile ) );
	if ( !skin && Q_stricmp( profile, "default" ) ) {
		skin = trap_R_RegisterSkin( va( "%s_default.skin", base ) );
	}
	return skin;
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
		cgvr.handSkin = CG_VR_MapSkin( "models/vr/hands.smartskin", map );
		cgvr.bodySkin = CG_VR_MapSkin( "models/players/player/body_vr.smartskin", map );
	}
	cgvr.lastMagDrop = cgVR ? cgVR->magDropCount : 0;
}

// the current weapon's measured geometry (see bg_vr.c)
// the chosen weapon size (one of the presets, so co-op servers can match it)
float CG_VR_WeaponScale( void ) {
	char buf[16];
	float s;

	trap_Cvar_VariableStringBuffer( "vr_weaponScale", buf, sizeof( buf ) );
	s = buf[0] ? atof( buf ) : 1.0f;
	s = BG_VR_WeaponScale( BG_VR_ScalePreset( s ) );
	if ( cgVR ) {
		cgVR->weaponScale = s;
	}
	return s;
}

void CG_VR_Geom( int weapon, vrWeaponGeom_t *g ) {
	BG_VR_WeaponGeom( weapon, qfalse, g );
	g->scale = CG_VR_WeaponScale();
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
		if ( !cgVR->handValid[h] || !cgvr.handModel[h] ) {
			continue;
		}
		memset( &ent, 0, sizeof( ent ) );
		ent.hModel = cgvr.handModel[h];
		ent.customSkin = cgvr.handSkin;
		VectorCopy( cgvr.handOrigin[h], ent.origin );
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		AnglesToAxis( cgvr.handAngles[h], ent.axis );
		// the hands come from the same oversized first person models as the guns
		if ( CG_VR_WeaponScale() != 1.0f ) {
			float s = CG_VR_WeaponScale();
			VectorScale( ent.axis[0], s, ent.axis[0] );
			VectorScale( ent.axis[1], s, ent.axis[1] );
			VectorScale( ent.axis[2], s, ent.axis[2] );
			ent.nonNormalizedAxes = qtrue;
		}
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
		if ( CG_VR_BodyDrawn() ) {
			ent.renderfx |= RF_VR_NOSLEEVES;    // the full body's arms lead up to the hands
		}
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
		Com_sprintf( base, sizeof( base ), "w%d", weapon );     // see make_vr_assets.py
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
CG_VR_BeginFrame
=================
*/
// the server's hand reloading state for this player, and its controller buzzes
static void CG_VR_UnpackState( void ) {
	static int lastPulse[2] = { -1, -1 };
	const playerState_t *ps = &cg.snap->ps;
	int st = cg.predictedPlayerState.vrState, h;

	cgVR->manualReload = ( st & VRS_MANUAL ) != 0;
	cgVR->chambered = ( st & VRS_CHAMBERED ) != 0;
	cgVR->magOut = ( st & VRS_MAGOUT ) != 0;
	cgVR->heldItem = ( st >> VRS_HELD_SHIFT ) & 3;
	cgVR->chargeHeld = ( st & VRS_CHARGEHELD ) != 0;
	cgVR->chargePull = ( ( st >> VRS_PULL_SHIFT ) & 255 ) / 255.0f;
	cgVR->magDropCount = ( st >> VRS_DROP_SHIFT ) & 255;
	cgVR->grabbedEntity = cg.predictedPlayerState.vrGrab - 1;

	// the off hand just came onto something it can take: a light buzz
	trap_Cvar_Update( &vr_grabHint );
	if ( ( ps->vrState & VRS_CANGRAB ) && !vrCanGrab && vr_grabHint.integer >= 1 ) {
		CG_VR_Haptic( !CG_VR_DominantHand(), 0.25f, 25 );
	}
	vrCanGrab = ( ps->vrState & VRS_CANGRAB ) != 0;

	// buzzes come with the snapshots (prediction doesn't make them)
	for ( h = 0; h < 2; h++ ) {
		int n = ( ps->vrState >> ( h == VR_HAND_LEFT ? VRS_PULSE_L_SHIFT : VRS_PULSE_R_SHIFT ) ) & 15;
		if ( lastPulse[h] >= 0 && n != lastPulse[h] ) {
			CG_VR_Haptic( h, 0.7f, 50 );
		}
		lastPulse[h] = n;
	}
}

void CG_VR_BeginFrame( void ) {
	CG_VR_RecoilUpdate();
	cgvr.active = CG_VR_Active();

	if ( !cgvr.active || !cg.snap ) {
		return;
	}

	CG_VR_UnpackState();
	CG_VR_ReloadSounds();
	CG_VRX_KnifeFrame();        // GORE & EXTRAS: the blade through a dead body's neck

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
	BG_VR_WeaponFrame( &geom, cgvr.handOrigin[CG_VR_DominantHand()], cgvr.weaponAngles, adj, origin, axis );
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
		// how the magazine sits in the gun, for drawing it in the hand / falling out
		int i, j;
		for ( i = 0; i < 3; i++ ) {
			for ( j = 0; j < 3; j++ ) {
				cgvr.magAxis[i][j] = DotProduct( ent->axis[i], parent->axis[j] );
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
		} else if ( !chambered && ( cls == VRR_MAG || cls == VRR_BOLT ) ) {
			amount = 1.0f;      // slide locked back / bolt open
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

	CG_CenterPrint( va( "weapon %d  class %d  mag part %d  charge part %d  travel %.1f",
						ps->weapon, BG_VR_ReloadClass( ps->weapon ),
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
		model = weapon->wpPartModels[W_FP_MODEL][geom.magPart];
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
		CG_VR_MagAxis( cgvr.gunAxis, ent.axis );    // lined up with the gun, ready to go in
		if ( model ) {
			BG_VR_ModelDir( ent.axis, geom.magLocal, ent.origin );
			VectorSubtract( cgvr.handOrigin[off], ent.origin, ent.origin );
		} else {
			VectorCopy( cgvr.handOrigin[off], ent.origin );
		}
		VectorCopy( ent.origin, ent.oldorigin );
		VectorCopy( ent.origin, ent.lightingOrigin );
		ent.renderfx = RF_FIRST_PERSON | RF_MINLIGHT | RF_LIGHTING_ORIGIN;
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
			VectorCopy( fwd, ent.axis[0] );
			VectorScale( right, -1, ent.axis[1] );
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
CG_VR_SetMuzzle

Called with the first person gun's muzzle (tag_flash) every frame. The server fires from
the same spot relative to the weapon hand, so bullets leave the barrel.
=================
*/
void CG_VR_SetMuzzle( const vec3_t muzzle ) {
	vec3_t d, aim[3];
	int dom = CG_VR_DominantHand();

	VectorSubtract( muzzle, cgvr.handOrigin[dom], d );
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
	case WP_AKIMBO:
	case WP_GRENADE_LAUNCHER:
	case WP_GRENADE_PINEAPPLE:
	case WP_DYNAMITE:
		return qtrue;
	default:
		return qfalse;
	}
}

// weapons that fire bullets or projectiles along the barrel get an aim reticle
static qboolean CG_VR_HasReticle( int weapon ) {
	return !CG_VR_OneHanded( weapon ) || weapon == WP_LUGER || weapon == WP_SILENCER || weapon == WP_COLT ||
		   weapon == WP_AKIMBO;
}

/*
=================
CG_VR_AddReticle

A dot where the shot would land: traced from the muzzle along the aim, like the server
fires it. Kept at the same apparent size at any distance.
=================
*/
void CG_VR_AddReticle( void ) {
	refEntity_t ent;
	trace_t tr;
	vec3_t start, end, aim[3], toEye;
	int weapon = cg.predictedPlayerState.weapon;
	float dist;

	trap_Cvar_Update( &vr_reticle );
	if ( !cgvr.active || !vr_reticle.integer || cgVR->menuPointer || cg.cameraMode ||
		 !CG_VR_HasReticle( weapon ) || cg.predictedPlayerState.stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	AnglesToAxis( cgvr.weaponAngles, aim );
	if ( cgVR->muzzleValid ) {
		VectorCopy( cgvr.muzzle, start );
	} else {
		VectorCopy( cgvr.handOrigin[CG_VR_DominantHand()], start );
	}
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
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_MAUSER:
	case WP_GARAND:
	case WP_FG42:
	case WP_FG42SCOPE:
	case WP_SNIPERRIFLE:
	case WP_SNOOPERSCOPE:
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
	default:
		return BG_DualActive( ps ) ? ps->weapon : 0;     // any pistol or SMG, dual wielded
	}
}

void CG_VR_AddDualGuns( playerState_t *ps, int single ) {
	centity_t *cent = &cg.predictedPlayerEntity;
	weaponInfo_t *weapon;
	refEntity_t hand;
	vrWeaponGeom_t geom;
	vec3_t angles;
	float adj[6] = { 0, 0, 0, 0, 0, 0 };
	char buf[32];
	int side, savedWeapon = cent->currentState.weapon, savedFlash = cent->muzzleFlashTime;
	int off = !CG_VR_DominantHand();

	CG_RegisterWeapon( single );
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
			CG_VR_Geom( single, &geom );
			BG_VR_WeaponFrame( &geom, cgvr.handOrigin[off], angles, adj, hand.origin, hand.axis );
		}
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
		if ( side == 0 ) {
			// the off hand's muzzle, for drawing its shots' hits
			VectorCopy( cgvr.muzzle, cgvr.muzzleOff );
			VectorCopy( angles, cgvr.aimOff );
		}
	}
	cent->currentState.weapon = savedWeapon;
	cent->muzzleFlashTime = savedFlash;
}
