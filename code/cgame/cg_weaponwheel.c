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
//
// In VR, cl_vr.c opens it while the gun hand's grip is held, and the hand's movement points
// at a weapon (CG_JOYSTICK_EVENT); letting go of the grip or pulling the trigger switches.
// It's in pages of VR_WHEEL_PAGE weapons turned with the stick (vrwheelpage), the last page
// holding the binoculars once found.

#include "cg_local.h"

#define WW_CX   ( SCREEN_WIDTH * ( cgvr.active ? 0.5f : 0.35f ) )     // VR: in front of the eyes
#define WW_CY   ( SCREEN_HEIGHT * 0.5f )

// the weapons that show, in wheel order
static const int wwOrder[] = {
	WP_KNIFE, WP_LUGER, WP_SILENCER, WP_COLT, WP_AKIMBO,
	WP_MP40, WP_THOMPSON, WP_STEN,
	WP_MAUSER, WP_GARAND, WP_FG42,
	WP_PANZERFAUST, WP_FLAMETHROWER, WP_VENOM, WP_TESLA,
	WP_GRENADE_LAUNCHER, WP_GRENADE_PINEAPPLE, WP_DYNAMITE,
};

static int WW_CollectAll( int *visible, int maxWeapons ) {
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

int CG_WeaponWheelPages( void ) {
	int all[MAX_WEAPONS];

	return ( WW_CollectAll( all, MAX_WEAPONS ) + VR_WHEEL_PAGE - 1 ) / VR_WHEEL_PAGE +
		   ( ( cg.snap && ( cg.snap->ps.stats[STAT_KEYS] & ( 1 << INV_BINOCS ) ) ) ? 1 : 0 );
}

int CG_CollectWeaponWheelWeapons( int *visible, int maxWeapons ) {
	int all[MAX_WEAPONS];
	int n, pages, first, i, count = 0;

	n = WW_CollectAll( all, MAX_WEAPONS );
	if ( !cgvr.active ) {
		for ( i = 0; i < n && count < maxWeapons; i++ ) {
			visible[count++] = all[i];
		}
		return count;
	}
	pages = CG_WeaponWheelPages();
	if ( cg.weaponWheel.vrPage < 0 || cg.weaponWheel.vrPage >= pages ) {
		cg.weaponWheel.vrPage = 0;
	}
	first = cg.weaponWheel.vrPage * VR_WHEEL_PAGE;
	if ( first >= n ) {
		// the last page: the binoculars
		if ( cg.snap->ps.stats[STAT_KEYS] & ( 1 << INV_BINOCS ) ) {
			visible[count++] = VR_WHEEL_BINOCS;
		}
		return count;
	}
	for ( i = first; i < n && i < first + VR_WHEEL_PAGE && count < maxWeapons; i++ ) {
		visible[count++] = all[i];
	}
	return count;
}

// the hand's direction on the wheel (cl_vr.c)
void CG_JoystickEvent( int axis, int value ) {
	if ( !cg.weaponWheel.active ) {
		return;
	}
	if ( axis == 2 ) {
		cg.weaponWheel.stickX = value / 32767.0f;
	} else if ( axis == 3 ) {
		cg.weaponWheel.stickY = value / 32767.0f;
	}
}

void CG_WeaponWheelDown_f( void ) {
	int visible[MAX_WEAPONS];

	if ( !cg.snap || ( cg.snap->ps.eFlags & ( EF_MG42_ACTIVE | EF_DEAD ) ) || cg.zoomedScope || cg.zoomedBinoc ||
		 cg.snap->ps.pm_type == PM_FREEZE || cg.snap->ps.pm_type == PM_INTERMISSION ) {
		return;
	}
	cg.weaponWheel.stickX = cg.weaponWheel.stickY = 0;
	if ( cgvr.active ) {
		// start on the page with the weapon in the hand
		int all[MAX_WEAPONS], n = WW_CollectAll( all, MAX_WEAPONS ), i;
		cg.weaponWheel.vrPage = 0;
		for ( i = 0; i < n; i++ ) {
			if ( all[i] == cg.weaponSelect ) {
				cg.weaponWheel.vrPage = i / VR_WHEEL_PAGE;
			}
		}
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
	// VR: the binoculars go in the gun hand, a weapon puts them away
	if ( cg.weaponWheel.hoveredWeapon == VR_WHEEL_BINOCS ) {
		if ( cgVR ) {
			cgVR->binocsOut = 1;
		}
		cgvr.binocsWeapon = cg.snap->ps.weapon;
		return;
	}
	if ( cg.weaponWheel.hoveredWeapon > 0 && cgVR ) {
		cgVR->binocsOut = 0;
	}
	if ( cg.weaponWheel.hoveredWeapon > 0 && cg.weaponWheel.hoveredWeapon != cg.weaponSelect ) {
		CG_FinishWeaponChange( cg.weaponSelect, cg.weaponWheel.hoveredWeapon );
	}
}

// VR: the wheel closed by another button, nothing picked
void CG_WeaponWheelCancel_f( void ) {
	cg.weaponWheel.hoveredWeapon = 0;
	CG_WeaponWheelUp_f();
}

// VR: the gun hand's stick turns the wheel's pages (vrwheelpage 1 / -1)
void CG_WeaponWheelPage_f( void ) {
	int pages = CG_WeaponWheelPages();

	if ( !cg.weaponWheel.active || pages < 2 ) {
		return;
	}
	cg.weaponWheel.vrPage = ( cg.weaponWheel.vrPage + ( atoi( CG_Argv( 1 ) ) < 0 ? -1 : 1 ) + pages ) % pages;
	cg.weaponWheel.hoveredWeapon = 0;
	CG_VR_Haptic( CG_VR_DominantHand(), 0.4f, 25 );
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
	if ( cgvr.active ) {
		// the hand points at an icon: the nearest by direction, and it only gives way to a
		// neighbour once that's clearly nearer, so it doesn't flicker
		float sx = cg.weaponWheel.stickX, sy = cg.weaponWheel.stickY, a, best = 1e9f, cur = 1e9f, d, x, y;
		int i, pick = 0;

		if ( sqrt( sx * sx + sy * sy ) < 0.3f ) {
			return;
		}
		a = atan2( sy, sx );
		for ( i = 0; i < num; i++ ) {
			WW_Position( i, num, 1.0f, &x, &y );
			d = fabs( AngleNormalize180( RAD2DEG( atan2( y - WW_CY, x - WW_CX ) - a ) ) );
			if ( d < best ) {
				best = d;
				pick = visible[i];
			}
			if ( visible[i] == cg.weaponWheel.hoveredWeapon ) {
				cur = d;
			}
		}
		if ( cur < 1e9f && cur - best < 12.0f ) {
			pick = cg.weaponWheel.hoveredWeapon;
		}
		if ( pick != cg.weaponWheel.hoveredWeapon ) {
			CG_VR_Haptic( CG_VR_DominantHand(), 0.2f, 15 );    // a tick for each new weapon
		}
		cg.weaponWheel.hoveredWeapon = pick;
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
	if ( cgvr.active ) {
		radius = 78.0f * t;     // a page of a few, round a smaller ring (it filled the view)
	}

	CG_FillRect( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, shade );
	for ( i = 0; i < num; i++ ) {
		w = visible[i];
		WW_Position( i, num, radius, &x, &y );
		h = ( w == cg.weaponWheel.hoveredWeapon ? 1.3f : 1.0f ) * ( num > 10 ? 32.0f : 40.0f );
		if ( cgvr.active ) {
			h *= 0.65f;
		}
		if ( w == VR_WHEEL_BINOCS ) {
			// (gfx/vr/wheel_binocs*.tga, misc/vr/make_wheel_icons.py)
			static qhandle_t binocIcons[2];
			int sel = w == cg.weaponWheel.hoveredWeapon;
			if ( !binocIcons[sel] ) {
				binocIcons[sel] = trap_R_RegisterShaderNoMip( sel ? "gfx/vr/wheel_binocs_select" : "gfx/vr/wheel_binocs" );
			}
			CG_DrawPic( x - h, y - h * 0.5f, h * 2.0f, h, binocIcons[sel] );
			continue;
		}
		CG_RegisterWeapon( w );
		wd = ( w == WP_KNIFE || w == WP_LUGER || w == WP_SILENCER || w == WP_COLT || w == WP_AKIMBO ||
			   w == WP_GRENADE_LAUNCHER || w == WP_GRENADE_PINEAPPLE || w == WP_DYNAMITE ) ? h : h * 2.0f;
		CG_DrawPic( x - wd * 0.5f, y - h * 0.5f, wd, h,
					cg_weapons[w].weaponIcon[w == cg.weaponWheel.hoveredWeapon ? 1 : 0] );
	}

	// VR: which page, under the wheel
	if ( cgvr.active && CG_WeaponWheelPages() > 1 ) {
		const char *pg = va( "< %i / %i >", cg.weaponWheel.vrPage + 1, CG_WeaponWheelPages() );
		CG_DrawStringExt2( WW_CX - CG_DrawStrlen( pg ) * 4, WW_CY + radius + 28.0f, pg, white, qfalse, qtrue, 8, 8, 0 );
	}

	// the picked one's name and rounds
	w = cg.weaponWheel.hoveredWeapon;
	if ( w == VR_WHEEL_BINOCS ) {
		CG_DrawStringExt2( WW_CX - CG_DrawStrlen( "Binoculars" ) * 5, WW_CY + 20, "Binoculars", white, qfalse, qtrue, 10, 10, 0 );
	} else if ( w > 0 ) {
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
	if ( !cgvr.active ) {
		CG_DrawPic( cgs.cursorX - 8, cgs.cursorY - 8, 16, 16, cgs.media.selectCursor );
	}
}
