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

// cg_vrhandtweak.c -- hand alignment: line the hands on each gun up by hand, in the headset.
//
// The hands on the guns are the flat game's own first person hands, modelled into each gun
// (cg_vr.c CG_VR_HandPose), so their fingers can't bend; what can be tuned per weapon is
//
//   gun      the whole gun with its hands, in the controller (where your real fingers are)
//   trigger  the trigger hand's palm and fingers, on the gun (turned about the wrist)
//   support  the support hand's palm and fingers, on the gun (turned about the wrist)
//
// each as forward / left / up (model units), pitch / yaw / roll (degrees) and a size. They
// are kept per weapon in vr_handalign.txt (the game's home folder, next to the configs),
// read at every map load and written on every change.
//
// In the headset: "handtweak" in the console (or a key bound to it) turns the mode on.
// Hold the off hand's grip and move / turn it: the chosen part follows (vr_handTweakSpeed,
// 0.5 = half as far, for fine work). Off hand trigger tap: next part. Hold it 1.5 seconds:
// that part back to zero. A readout shows on the HUD layer.

#include "cg_local.h"

#define HT_MAX_WEAPONS  64
#define HT_FILE         "vr_handalign.txt"

typedef struct {
	char            weap[MAX_QPATH];
	vrHandTweak_t   part[VR_HT_PARTS];
} htEntry_t;

static htEntry_t htEntries[HT_MAX_WEAPONS];
static int htNumEntries;

static const vrHandTweak_t htIdentity = { { 0, 0, 0, 0, 0, 0 }, 1.0f };
static const char *htPartNames[VR_HT_PARTS] = { "gun", "trigger", "support" };
static const char *htPartTitles[VR_HT_PARTS] = { "WHOLE GUN IN HAND", "TRIGGER HAND", "SUPPORT HAND" };

static vmCvar_t vr_handTweak;
static vmCvar_t vr_handTweakSpeed;

static struct {
	int             part;           // VR_HT_*
	// off hand trigger: tap = next part, hold = reset
	qboolean        trigWas;
	int             trigStart;
	qboolean        resetDone;
	// the drag (off hand grip held)
	qboolean        dragging;
	int             dragWeapon;
	int             dragPart;
	vrHandTweak_t   dragStart;
	vec3_t          p0;             // off hand from the gun hand, in the gun hand's aim frame
	vec3_t          q0[3];          // off hand's axes in that frame
	// one step back
	qboolean        undoValid;
	int             undoWeapon;
	int             undoPart;
	vrHandTweak_t   undo;
	int             savedTime;      // last write, for the readout
	qboolean        blocked;        // the off hand is busy (on the gun, a bolt, a body)
} ht;

/*
=================
Storage
=================
*/
static const char *HT_WeapKey( int weapon ) {
	const char *file;

	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return NULL;
	}
	file = BG_GetWeaponFilename( weapon );
	if ( !file || !*file ) {
		return NULL;
	}
	return COM_SkipPath( (char *)file );
}

static htEntry_t *HT_Find( const char *key, qboolean create ) {
	int i;

	if ( !key ) {
		return NULL;
	}
	for ( i = 0; i < htNumEntries; i++ ) {
		if ( !Q_stricmp( htEntries[i].weap, key ) ) {
			return &htEntries[i];
		}
	}
	if ( !create || htNumEntries >= HT_MAX_WEAPONS ) {
		return NULL;
	}
	memset( &htEntries[htNumEntries], 0, sizeof( htEntries[0] ) );
	Q_strncpyz( htEntries[htNumEntries].weap, key, sizeof( htEntries[0].weap ) );
	for ( i = 0; i < VR_HT_PARTS; i++ ) {
		htEntries[htNumEntries].part[i] = htIdentity;
	}
	return &htEntries[htNumEntries++];
}

qboolean CG_VR_HandTweakMoves( const vrHandTweak_t *t ) {
	int i;

	for ( i = 0; i < 6; i++ ) {
		if ( fabs( t->v[i] ) > 0.0005f ) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean HT_IsIdentity( const vrHandTweak_t *t ) {
	return !CG_VR_HandTweakMoves( t ) && fabs( t->size - 1.0f ) < 0.0005f;
}

const vrHandTweak_t *CG_VR_HandTweakGet( int weapon, int part ) {
	htEntry_t *e;

	if ( part < 0 || part >= VR_HT_PARTS ) {
		return &htIdentity;
	}
	e = HT_Find( HT_WeapKey( weapon ), qfalse );
	return e ? &e->part[part] : &htIdentity;
}

static vrHandTweak_t *HT_Edit( int weapon, int part ) {
	htEntry_t *e = HT_Find( HT_WeapKey( weapon ), qtrue );
	return e ? &e->part[part] : NULL;
}

static void HT_Save( void ) {
	fileHandle_t f;
	const char *line;
	int i, p, n = 0;

	if ( trap_FS_FOpenFile( HT_FILE, &f, FS_WRITE ) < 0 || !f ) {
		CG_Printf( S_COLOR_RED "handtweak: couldn't write %s\n", HT_FILE );
		return;
	}
	line = "// Brutally RealRTCW VR hand alignment, written by the game (handtweak). One line per weapon and part:\n"
		   "// <weapon file> <gun|trigger|support> <forward> <left> <up> <pitch> <yaw> <roll> <size>\n"
		   "// forward/left/up in model units, pitch/yaw/roll in degrees. Delete a line to put it back.\n";
	trap_FS_Write( line, strlen( line ), f );
	for ( i = 0; i < htNumEntries; i++ ) {
		for ( p = 0; p < VR_HT_PARTS; p++ ) {
			const vrHandTweak_t *t = &htEntries[i].part[p];
			if ( HT_IsIdentity( t ) ) {
				continue;
			}
			line = va( "%s %s %.3f %.3f %.3f %.2f %.2f %.2f %.3f\n", htEntries[i].weap, htPartNames[p],
					   t->v[0], t->v[1], t->v[2], t->v[3], t->v[4], t->v[5], t->size );
			trap_FS_Write( line, strlen( line ), f );
			n++;
		}
	}
	trap_FS_FCloseFile( f );
	ht.savedTime = cg.time;
}

static int HT_PartByName( const char *s ) {
	int p;

	for ( p = 0; p < VR_HT_PARTS; p++ ) {
		if ( !Q_stricmp( s, htPartNames[p] ) ) {
			return p;
		}
	}
	return -1;
}

static void HT_Load( void ) {
	static char buf[32768];
	fileHandle_t f;
	char *s, *eol;
	int len;

	htNumEntries = 0;
	len = trap_FS_FOpenFile( HT_FILE, &f, FS_READ );
	if ( len <= 0 || !f ) {
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;

	for ( s = buf; s && *s; s = eol ) {
		char weap[MAX_QPATH], part[16];
		vrHandTweak_t t;
		htEntry_t *e;
		int p;

		eol = strchr( s, '\n' );
		if ( eol ) {
			*eol++ = 0;
		}
		while ( *s == ' ' || *s == '\t' ) {
			s++;
		}
		if ( !*s || ( s[0] == '/' && s[1] == '/' ) ) {
			continue;
		}
		t = htIdentity;
		if ( sscanf( s, "%63s %15s %f %f %f %f %f %f %f", weap, part, &t.v[0], &t.v[1], &t.v[2],
					 &t.v[3], &t.v[4], &t.v[5], &t.size ) < 8 ) {
			continue;
		}
		p = HT_PartByName( part );
		if ( p < 0 ) {
			continue;
		}
		if ( t.size < 0.25f || t.size > 3.0f ) {
			t.size = 1.0f;
		}
		e = HT_Find( weap, qtrue );
		if ( e ) {
			e->part[p] = t;
		}
	}
}

/*
=================
Rotation helpers (axes as rows: forward, left, up, the AnglesToAxis way)
=================
*/
static void HT_AxisToAngles( vec3_t axis[3], vec3_t angles ) {
	float s = -axis[0][2];

	s = s > 1.0f ? 1.0f : ( s < -1.0f ? -1.0f : s );
	angles[PITCH] = RAD2DEG( asin( s ) );
	angles[YAW] = RAD2DEG( atan2( axis[0][1], axis[0][0] ) );
	angles[ROLL] = RAD2DEG( atan2( axis[1][2], axis[2][2] ) );
}

// out = a * b (3x3)
static void HT_Mul( vec3_t a[3], vec3_t b[3], vec3_t out[3] ) {
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
	}
}

static void HT_Transpose( vec3_t a[3], vec3_t out[3] ) {
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			out[i][j] = a[j][i];
		}
	}
}

// the rotation m (column vectors) taken k of the way from no rotation
static void HT_ScaleRotation( vec3_t m[3], float k, vec3_t out[3] ) {
	float tr = m[0][0] + m[1][1] + m[2][2];
	float c = ( tr - 1.0f ) * 0.5f, ang, s, x, y, z, C, S, t, len;

	c = c > 1.0f ? 1.0f : ( c < -1.0f ? -1.0f : c );
	ang = acos( c );
	x = m[2][1] - m[1][2];
	y = m[0][2] - m[2][0];
	z = m[1][0] - m[0][1];
	len = sqrt( x * x + y * y + z * z );
	if ( ang < 1e-4f || len < 1e-6f ) {
		AxisClear( out );
		return;
	}
	x /= len;
	y /= len;
	z /= len;
	ang *= k;
	C = cos( ang );
	S = sin( ang );
	t = 1.0f - C;
	out[0][0] = t * x * x + C;     out[0][1] = t * x * y - S * z; out[0][2] = t * x * z + S * y;
	out[1][0] = t * x * y + S * z; out[1][1] = t * y * y + C;     out[1][2] = t * y * z - S * x;
	out[2][0] = t * x * z - S * y; out[2][1] = t * y * z + S * x; out[2][2] = t * z * z + C;
	(void)s;
}

/*
=================
The drag
=================
*/
static int HT_Weapon( void ) {
	playerState_t *ps = &cg.predictedPlayerState;

	if ( cgVR && cgVR->dualWield ) {
		return CG_VR_DualSingle( ps );
	}
	return ps->weapon;
}

// the parts this weapon has (bit per VR_HT_*)
static int HT_Parts( int weapon ) {
	return CG_VR_HandParts( weapon ) | ( 1 << VR_HT_GUN );
}

static void HT_ValidPart( int weapon ) {
	int parts = HT_Parts( weapon ), i;

	for ( i = 0; i < VR_HT_PARTS; i++ ) {
		if ( parts & ( 1 << ht.part ) ) {
			return;
		}
		ht.part = ( ht.part + 1 ) % VR_HT_PARTS;
	}
	ht.part = VR_HT_GUN;
}

static void HT_NextPart( int weapon ) {
	ht.part = ( ht.part + 1 ) % VR_HT_PARTS;
	HT_ValidPart( weapon );
}

// the off hand's pose in the gun hand's aim frame (play space, game units)
static void HT_RelPose( vec3_t p, vec3_t q[3] ) {
	int dom = CG_VR_DominantHand(), off = !dom, i, j;
	vec3_t aim[3], oa[3], d;

	AnglesToAxis( cgVR->weaponAngles, aim );
	AnglesToAxis( cgVR->handAngles[off], oa );
	VectorSubtract( cgVR->handOrigin[off], cgVR->handOrigin[dom], d );
	for ( i = 0; i < 3; i++ ) {
		p[i] = DotProduct( d, aim[i] );
		for ( j = 0; j < 3; j++ ) {
			q[i][j] = DotProduct( oa[i], aim[j] );
		}
	}
}

static void HT_RememberUndo( int weapon, int part ) {
	ht.undoValid = qtrue;
	ht.undoWeapon = weapon;
	ht.undoPart = part;
	ht.undo = *CG_VR_HandTweakGet( weapon, part );
}

// the gun's own turn in the aim frame (rows), with the tweak t on top of its measured one
static void HT_GunAxes( int weapon, const vrHandTweak_t *gunTweak, vec3_t c[3], float *scale, vec3_t base ) {
	vrWeaponGeom_t geom;
	vec3_t ang;

	CG_VR_Geom( weapon, &geom );
	VectorSet( base, geom.pitch, geom.yaw, 0 );
	VectorSet( ang, base[0] + gunTweak->v[3], base[1] + gunTweak->v[4], base[2] + gunTweak->v[5] );
	AnglesToAxis( ang, c );
	*scale = geom.scale > 0.01f ? geom.scale : 1.0f;
}

static void HT_DragUpdate( void ) {
	vec3_t p, q[3], dt, qt[3], m[3], ms[3], mst[3], c[3], ct[3], tmp[3], l[3], l2[3], base, ang;
	vrHandTweak_t *t = HT_Edit( ht.dragWeapon, ht.dragPart );
	float k = vr_handTweakSpeed.value, scale;
	int i;

	if ( !t ) {
		return;
	}
	k = k < 0.05f ? 0.05f : ( k > 2.0f ? 2.0f : k );
	HT_RelPose( p, q );
	VectorSubtract( p, ht.p0, dt );

	// the off hand's turn since the grip closed, in the aim frame: q = m q0 (columns)
	HT_Transpose( q, qt );
	HT_Mul( qt, ht.q0, m );
	HT_ScaleRotation( m, k, ms );
	HT_Transpose( ms, mst );

	*t = ht.dragStart;
	if ( ht.dragPart == VR_HT_GUN ) {
		HT_GunAxes( ht.dragWeapon, &ht.dragStart, c, &scale, base );
		for ( i = 0; i < 3; i++ ) {
			t->v[i] = ht.dragStart.v[i] + k * DotProduct( dt, c[i] ) / scale;
		}
		HT_Mul( c, mst, l2 );       // the gun's rows turned with the hand
		HT_AxisToAngles( l2, ang );
		for ( i = 0; i < 3; i++ ) {
			t->v[3 + i] = AngleNormalize180( ang[i] - base[i] );
		}
	} else {
		// in the gun's own frame
		HT_GunAxes( ht.dragWeapon, CG_VR_HandTweakGet( ht.dragWeapon, VR_HT_GUN ), c, &scale, base );
		for ( i = 0; i < 3; i++ ) {
			t->v[i] = ht.dragStart.v[i] + k * DotProduct( dt, c[i] ) / scale;
		}
		HT_Transpose( c, ct );
		HT_Mul( c, ms, tmp );
		HT_Mul( tmp, ct, m );       // m: the turn in gun coordinates (columns)
		HT_Transpose( m, mst );
		VectorSet( ang, ht.dragStart.v[3], ht.dragStart.v[4], ht.dragStart.v[5] );
		AnglesToAxis( ang, l );
		HT_Mul( l, mst, l2 );
		HT_AxisToAngles( l2, ang );
		for ( i = 0; i < 3; i++ ) {
			t->v[3 + i] = AngleNormalize180( ang[i] );
		}
	}
}

static void HT_EndDrag( void ) {
	if ( ht.dragging ) {
		ht.dragging = qfalse;
		HT_Save();
	}
}

void CG_VR_HandTweakFrame( void ) {
	int weapon, dom, off;
	qboolean trig, grip;

	trap_Cvar_Update( &vr_handTweak );
	trap_Cvar_Update( &vr_handTweakSpeed );
	if ( !vr_handTweak.integer || !cgvr.active || !cgVR || cgVR->menuPointer || cg.cameraMode ) {
		HT_EndDrag();
		ht.trigWas = qfalse;
		return;
	}
	weapon = HT_Weapon();
	if ( !HT_WeapKey( weapon ) || cgVR->binocsOut ) {
		HT_EndDrag();
		return;
	}
	HT_ValidPart( weapon );
	dom = CG_VR_DominantHand();
	off = !dom;

	// the off hand's trigger: tap for the next part, hold to put it back
	trig = cgVR->triggerDown[off] && !cgVR->dualWield;
	if ( trig && !ht.trigWas ) {
		ht.trigStart = cg.time;
		ht.resetDone = qfalse;
	}
	if ( trig && !ht.resetDone && !ht.dragging && cg.time - ht.trigStart > 1500 ) {
		HT_RememberUndo( weapon, ht.part );
		{
			vrHandTweak_t *t = HT_Edit( weapon, ht.part );
			if ( t ) {
				*t = htIdentity;
			}
		}
		HT_Save();
		CG_VR_Haptic( off, 0.6f, 120 );
		ht.resetDone = qtrue;
	}
	if ( !trig && ht.trigWas && !ht.resetDone && cg.time - ht.trigStart < 600 && !cgVR->chargeHeld && !ht.dragging ) {
		HT_NextPart( weapon );
		CG_VR_Haptic( off, 0.3f, 40 );
	}
	ht.trigWas = trig;

	// the off hand's grip: drag
	ht.blocked = cgVR->twoHanded || cgVR->chargeHeld || cgVR->heldItem || cgVR->grabbedEntity >= 0 ||
				 cgVR->gunInOffHand;
	grip = cgVR->gripDown[off] && cgVR->handValid[off] && cgVR->handValid[dom];
	if ( ht.dragging && ( !grip || weapon != ht.dragWeapon ) ) {
		HT_EndDrag();
	}
	if ( grip && !ht.dragging && !ht.blocked ) {
		if ( !HT_Edit( weapon, ht.part ) ) {
			return;     // (table full)
		}
		ht.dragging = qtrue;
		ht.dragWeapon = weapon;
		ht.dragPart = ht.part;
		HT_RememberUndo( weapon, ht.part );
		ht.dragStart = *CG_VR_HandTweakGet( weapon, ht.part );
		HT_RelPose( ht.p0, ht.q0 );
	}
	if ( ht.dragging && !ht.blocked ) {
		HT_DragUpdate();
	}
}

/*
=================
CG_VR_HandTweakDraw2D

The readout, on the HUD layer.
=================
*/
void CG_VR_HandTweakDraw2D( void ) {
	static vec4_t white = { 1, 1, 1, 1 }, yellow = { 1, 0.85f, 0.3f, 1 }, red = { 1, 0.35f, 0.3f, 1 };
	static vec4_t green = { 0.5f, 1, 0.5f, 1 }, back = { 0, 0, 0, 0.6f };
	const vrHandTweak_t *t;
	const char *key;
	int weapon, parts, p, y = 300;

	if ( !vr_handTweak.integer ) {
		return;
	}
	weapon = HT_Weapon();
	key = HT_WeapKey( weapon );
	CG_FillRect( 60, y - 4, 520, 74, back );
	if ( !key ) {
		CG_DrawStringExt( 68, y, "HAND ALIGN: pick up a weapon", yellow, qtrue, qtrue, 8, 10, 0 );
		return;
	}
	HT_ValidPart( weapon );
	parts = HT_Parts( weapon );
	CG_DrawStringExt( 68, y, va( "HAND ALIGN  %s", key ), yellow, qtrue, qtrue, 8, 10, 0 );
	{
		char line[128] = "";
		for ( p = 0; p < VR_HT_PARTS; p++ ) {
			if ( !( parts & ( 1 << p ) ) ) {
				continue;
			}
			Q_strcat( line, sizeof( line ), va( p == ht.part ? "[%s] " : " %s  ", htPartTitles[p] ) );
		}
		CG_DrawStringExt( 68, y + 13, line, white, qtrue, qtrue, 7, 10, 0 );
	}
	t = CG_VR_HandTweakGet( weapon, ht.part );
	CG_DrawStringExt( 68, y + 26, va( "fwd %+.2f  left %+.2f  up %+.2f   pitch %+.1f  yaw %+.1f  roll %+.1f   size %.2f",
		t->v[0], t->v[1], t->v[2], t->v[3], t->v[4], t->v[5], t->size ), white, qtrue, qtrue, 6, 10, 0 );
	if ( ht.dragging && ht.blocked ) {
		CG_DrawStringExt( 68, y + 39, "Off hand is on the gun / holding something: move it away", red, qtrue, qtrue, 7, 10, 0 );
	} else if ( ht.dragging ) {
		CG_DrawStringExt( 68, y + 39, va( "Moving... let go of the grip to keep it (speed %.2f)", vr_handTweakSpeed.value ), green, qtrue, qtrue, 7, 10, 0 );
	} else {
		CG_DrawStringExt( 68, y + 39, "Off hand: hold GRIP + move/turn = adjust   TRIGGER tap = next part, hold = reset", white, qtrue, qtrue, 6, 10, 0 );
	}
	if ( ht.savedTime && cg.time - ht.savedTime < 1500 && cg.time >= ht.savedTime ) {
		CG_DrawStringExt( 68, y + 52, "Saved to " HT_FILE, green, qtrue, qtrue, 7, 10, 0 );
	} else {
		CG_DrawStringExt( 68, y + 52, "Console: handtweak (off), handtweak_undo, handtweak_nudge, handtweak_print", white, qtrue, qtrue, 6, 10, 0 );
	}
}

/*
=================
Console commands
=================
*/
static void HT_Arg( int n, char *buf, int size ) {
	trap_Argv( n, buf, size );
}

// handtweak [0|1]: the mode on / off
void CG_VR_HandTweak_f( void ) {
	char buf[16];
	int on = !vr_handTweak.integer;

	if ( trap_Argc() > 1 ) {
		HT_Arg( 1, buf, sizeof( buf ) );
		on = atoi( buf ) != 0;
	}
	trap_Cvar_Set( "vr_handTweak", on ? "1" : "0" );
	trap_Cvar_Update( &vr_handTweak );
	if ( !on ) {
		HT_EndDrag();
	}
	CG_Printf( "Hand alignment %s\n", on ? "on: off hand GRIP + move = adjust, TRIGGER tap = next part, hold = reset" : "off" );
}

// handtweak_part <gun|trigger|support|next>
void CG_VR_HandTweakPart_f( void ) {
	char buf[32];
	int weapon = HT_Weapon(), p;

	HT_Arg( 1, buf, sizeof( buf ) );
	p = HT_PartByName( buf );
	if ( p < 0 ) {
		HT_NextPart( weapon );
	} else {
		ht.part = p;
		HT_ValidPart( weapon );
	}
	CG_Printf( "handtweak: %s\n", htPartNames[ht.part] );
}

// handtweak_nudge <f|l|u|p|y|r|size> <amount>: for keys (bind KP_8 "handtweak_nudge f 0.1")
void CG_VR_HandTweakNudge_f( void ) {
	static const char *axes[7] = { "f", "l", "u", "p", "y", "r", "size" };
	char a[16], b[32];
	int weapon = HT_Weapon(), i;
	vrHandTweak_t *t;

	HT_Arg( 1, a, sizeof( a ) );
	HT_Arg( 2, b, sizeof( b ) );
	if ( trap_Argc() < 3 ) {
		CG_Printf( "usage: handtweak_nudge <f|l|u|p|y|r|size> <amount>   (current part: %s)\n", htPartNames[ht.part] );
		return;
	}
	HT_ValidPart( weapon );
	for ( i = 0; i < 7; i++ ) {
		if ( !Q_stricmp( a, axes[i] ) ) {
			break;
		}
	}
	if ( i == 7 || !HT_WeapKey( weapon ) ) {
		CG_Printf( "handtweak_nudge: unknown axis, or no weapon\n" );
		return;
	}
	HT_RememberUndo( weapon, ht.part );
	t = HT_Edit( weapon, ht.part );
	if ( !t ) {
		return;
	}
	if ( i == 6 ) {
		t->size += atof( b );
		t->size = t->size < 0.5f ? 0.5f : ( t->size > 2.0f ? 2.0f : t->size );
	} else {
		t->v[i] += atof( b );
	}
	HT_Save();
}

// handtweak_reset [all]: this part, or every part of this weapon, back to as measured
void CG_VR_HandTweakReset_f( void ) {
	char buf[16];
	int weapon = HT_Weapon(), p;
	vrHandTweak_t *t;

	if ( !HT_WeapKey( weapon ) ) {
		return;
	}
	HT_Arg( 1, buf, sizeof( buf ) );
	HT_RememberUndo( weapon, ht.part );
	for ( p = 0; p < VR_HT_PARTS; p++ ) {
		if ( p == ht.part || !Q_stricmp( buf, "all" ) ) {
			t = HT_Edit( weapon, p );
			if ( t ) {
				*t = htIdentity;
			}
		}
	}
	HT_Save();
	CG_Printf( "handtweak: %s reset\n", Q_stricmp( buf, "all" ) ? htPartNames[ht.part] : "all parts" );
}

// handtweak_undo: the last change back (one step)
void CG_VR_HandTweakUndo_f( void ) {
	vrHandTweak_t *t, cur;

	if ( !ht.undoValid ) {
		CG_Printf( "handtweak: nothing to undo\n" );
		return;
	}
	t = HT_Edit( ht.undoWeapon, ht.undoPart );
	if ( !t ) {
		return;
	}
	cur = *t;
	*t = ht.undo;
	ht.undo = cur;      // (undo again: redo)
	HT_Save();
	CG_Printf( "handtweak: undone (%s %s)\n", HT_WeapKey( ht.undoWeapon ), htPartNames[ht.undoPart] );
}

// handtweak_print [all]
void CG_VR_HandTweakPrint_f( void ) {
	char buf[16];
	int i, p;

	HT_Arg( 1, buf, sizeof( buf ) );
	for ( i = 0; i < htNumEntries; i++ ) {
		if ( Q_stricmp( buf, "all" ) && Q_stricmp( htEntries[i].weap, HT_WeapKey( HT_Weapon() ) ? HT_WeapKey( HT_Weapon() ) : "" ) ) {
			continue;
		}
		for ( p = 0; p < VR_HT_PARTS; p++ ) {
			const vrHandTweak_t *t = &htEntries[i].part[p];
			CG_Printf( "%s %s %.3f %.3f %.3f %.2f %.2f %.2f %.3f\n", htEntries[i].weap, htPartNames[p],
					   t->v[0], t->v[1], t->v[2], t->v[3], t->v[4], t->v[5], t->size );
		}
	}
}

void CG_VR_HandTweakLoad_f( void ) {
	HT_Load();
	CG_Printf( "handtweak: %d weapons read from %s\n", htNumEntries, HT_FILE );
}

void CG_VR_HandTweakSave_f( void ) {
	HT_Save();
	CG_Printf( "handtweak: written to %s\n", HT_FILE );
}

void CG_VR_HandTweakInit( void ) {
	trap_Cvar_Register( &vr_handTweak, "vr_handTweak", "0", 0 );
	trap_Cvar_Register( &vr_handTweakSpeed, "vr_handTweakSpeed", "0.5", CVAR_ARCHIVE );
	memset( &ht, 0, sizeof( ht ) );
	HT_Load();
}
