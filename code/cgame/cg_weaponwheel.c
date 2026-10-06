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

// cg_weaponwheel.c -- RealRTCW's weapon wheel, brought to co-op
//
// Hold +weaponwheel (Tab): every weapon with ammo is laid out in a circle, the mouse picks
// one, letting go switches to it. The client keeps the view still and hands the mouse here
// while it's open (cg_weaponWheelActive, cl_input.c). Unlike single player the world doesn't
// slow down: there are other players in it.

#include "cg_local.h"

#define WW_CX   ( SCREEN_WIDTH * 0.35f )
#define WW_CY   ( SCREEN_HEIGHT * 0.5f )

// the weapons that show, in wheel order
static const int wwOrder[] = {
	WP_KNIFE, WP_LUGER, WP_SILENCER, WP_COLT, WP_AKIMBO,
	WP_MP40, WP_THOMPSON, WP_STEN,
	WP_MAUSER, WP_GARAND, WP_FG42,
	WP_PANZERFAUST, WP_FLAMETHROWER, WP_VENOM, WP_TESLA,
	WP_GRENADE_LAUNCHER, WP_GRENADE_PINEAPPLE, WP_DYNAMITE,
};

int CG_CollectWeaponWheelWeapons( int *visible, int maxWeapons ) {
	int i, w, n = 0, ammoIndex, clipIndex;

	if ( !cg.snap ) {
		return 0;
	}
	for ( i = 0; i < (int)ARRAY_LEN( wwOrder ) && n < maxWeapons; i++ ) {
		w = wwOrder[i];
		if ( !COM_BitCheck( cg.snap->ps.weapons, w ) ) {
			continue;
		}
		if ( w != WP_KNIFE ) {
			ammoIndex = BG_FindAmmoForWeapon( w );
			clipIndex = BG_FindClipForWeapon( w );
			if ( ( ammoIndex <= 0 || cg.snap->ps.ammo[ammoIndex] <= 0 ) &&
				 ( clipIndex <= 0 || cg.snap->ps.ammoclip[clipIndex] <= 0 ) ) {
				continue;
			}
		}
		visible[n++] = w;
	}
	return n;
}

void CG_WeaponWheelDown_f( void ) {
	int visible[MAX_WEAPONS];

	if ( !cg.snap || ( cg.snap->ps.eFlags & ( EF_MG42_ACTIVE | EF_DEAD ) ) || cg.zoomedScope || cg.zoomedBinoc ||
		 cg.snap->ps.pm_type == PM_FREEZE || cg.snap->ps.pm_type == PM_INTERMISSION || cgvr.active ) {
		return;
	}
	if ( CG_CollectWeaponWheelWeapons( visible, MAX_WEAPONS ) <= 0 ) {
		return;
	}
	cg.weaponWheel.openTime = cg.time;
	cgs.cursorX = WW_CX;
	cgs.cursorY = WW_CY;
	cg.weaponWheel.active = qtrue;
	cg.weaponWheel.hoveredWeapon = 0;
	trap_Cvar_Set( "cg_weaponWheelActive", "1" );
}

void CG_WeaponWheelUp_f( void ) {
	if ( !cg.weaponWheel.active ) {
		return;
	}
	cg.weaponWheel.active = qfalse;
	trap_Cvar_Set( "cg_weaponWheelActive", "0" );
	if ( cg.weaponWheel.hoveredWeapon > 0 && cg.weaponWheel.hoveredWeapon != cg.weaponSelect ) {
		CG_FinishWeaponChange( cg.weaponSelect, cg.weaponWheel.hoveredWeapon );
	}
}

// the mouse while the wheel is open: a cursor kept inside the wheel
qboolean CG_WeaponWheelMouse( int x, int y ) {
	float dx, dy, len;

	if ( !cg.weaponWheel.active ) {
		return qfalse;
	}
	cgs.cursorX += x;
	cgs.cursorY += y;
	dx = cgs.cursorX - WW_CX;
	dy = cgs.cursorY - WW_CY;
	len = sqrt( dx * dx + dy * dy );
	if ( len > 120.0f ) {
		cgs.cursorX = WW_CX + dx * 120.0f / len;
		cgs.cursorY = WW_CY + dy * 120.0f / len;
	}
	return qtrue;
}

static void WW_Position( int idx, int num, float radius, float *x, float *y ) {
	float angle = ( (float)idx / (float)num ) * 2.0f * M_PI - M_PI * 0.5f;
	*x = WW_CX + cos( angle ) * radius;
	*y = WW_CY + sin( angle ) * radius;
}

static void WW_UpdateSelection( const int *visible, int num ) {
	float dx = cgs.cursorX - WW_CX, dy = cgs.cursorY - WW_CY, angle;
	int idx;

	if ( num == 1 ) {
		cg.weaponWheel.hoveredWeapon = visible[0];
		return;
	}
	if ( dx * dx + dy * dy < 30.0f * 30.0f ) {
		return;     // still in the middle: keep what was picked
	}
	// sectors centred on each icon, the first at the top
	angle = atan2( dy, dx ) + M_PI * 0.5f;
	if ( angle < 0 ) {
		angle += 2.0f * M_PI;
	}
	idx = (int)( angle / ( 2.0f * M_PI / num ) + 0.5f ) % num;
	cg.weaponWheel.hoveredWeapon = visible[idx];
}

void CG_DrawWeaponWheel( void ) {
	int visible[MAX_WEAPONS], num, i, w, ammoIndex, clipIndex;
	float t, radius, x, y, h, wd;
	char text[32];
	vec4_t white = { 1, 1, 1, 1 };
	vec4_t shade = { 0, 0, 0, 0.45f };
	gitem_t *item;

	if ( !cg.weaponWheel.active ) {
		return;
	}
	num = CG_CollectWeaponWheelWeapons( visible, MAX_WEAPONS );
	if ( num <= 0 ) {
		return;
	}
	WW_UpdateSelection( visible, num );

	t = ( cg.time - cg.weaponWheel.openTime ) / 150.0f;
	t = t > 1 ? 1 : t;
	radius = ( num < 5 ? 85.0f : 100.0f + num * 3.0f ) * t;
	if ( radius > 200.0f ) {
		radius = 200.0f;
	}

	CG_FillRect( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, shade );
	for ( i = 0; i < num; i++ ) {
		w = visible[i];
		CG_RegisterWeapon( w );
		WW_Position( i, num, radius, &x, &y );
		h = ( w == cg.weaponWheel.hoveredWeapon ? 1.3f : 1.0f ) * ( num > 10 ? 32.0f : 40.0f );
		wd = ( w == WP_KNIFE || w == WP_LUGER || w == WP_SILENCER || w == WP_COLT || w == WP_AKIMBO ||
			   w == WP_GRENADE_LAUNCHER || w == WP_GRENADE_PINEAPPLE || w == WP_DYNAMITE ) ? h : h * 2.0f;
		CG_DrawPic( x - wd * 0.5f, y - h * 0.5f, wd, h,
					cg_weapons[w].weaponIcon[w == cg.weaponWheel.hoveredWeapon ? 1 : 0] );
	}

	// the picked one's name and rounds
	w = cg.weaponWheel.hoveredWeapon;
	if ( w > 0 ) {
		item = BG_FindItemForWeapon( w );
		if ( item ) {
			const char *name = cgs.itemPrintNames[item - bg_itemlist][0] ? cgs.itemPrintNames[item - bg_itemlist] : item->pickup_name;
			CG_DrawStringExt2( WW_CX - CG_DrawStrlen( name ) * 5, WW_CY + 20, name, white, qfalse, qtrue, 10, 10, 0 );
		}
		ammoIndex = BG_FindAmmoForWeapon( w );
		clipIndex = BG_FindClipForWeapon( w );
		text[0] = 0;
		if ( w != WP_KNIFE && clipIndex > 0 && ammoIndex > 0 && clipIndex != ammoIndex ) {
			Com_sprintf( text, sizeof( text ), "%i/%i", cg.snap->ps.ammoclip[clipIndex], cg.snap->ps.ammo[ammoIndex] );
		} else if ( w != WP_KNIFE && ammoIndex > 0 ) {
			Com_sprintf( text, sizeof( text ), "%i", cg.snap->ps.ammo[ammoIndex] + cg.snap->ps.ammoclip[clipIndex] );
		}
		if ( text[0] ) {
			CG_DrawStringExt2( WW_CX - CG_DrawStrlen( text ) * 4, WW_CY + 34, text, white, qfalse, qtrue, 8, 8, 0 );
		}
	}
	CG_DrawPic( cgs.cursorX - 8, cgs.cursorY - 8, 16, 16, cgs.media.selectCursor );
}
