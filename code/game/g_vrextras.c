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
