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

// net_steam.c -- co-op over Steam: no IP addresses, no port forwarding.
//
// Game packets to and from "steam:<SteamID64>" addresses (NA_STEAM) travel through
// ISteamNetworkingMessages, which punches through NATs or falls back to Valve's relays.
// A player hosting a game advertises it in Steam rich presence, so friends can join from
// the Steam friends list ("Join Game") or with the JOIN FRIEND button / "steamjoin".
//
// The Steam API comes from RealRTCW's own steam_api64.dll (next to the co-op folder) and
// runs as RealRTCW's app. It is loaded at run time: without Steam everything else works
// as before.
//
// net_steamFake 1 (testing): no Steam, "steam:<n>" is the game on UDP port <n> of this PC.

#include "q_shared.h"
#include "qcommon.h"

#if defined( _WIN32 ) && !defined( DEDICATED )

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define STEAM_APPID_REALRTCW    "1379630"
#define STEAM_CHANNEL           0
#define STEAM_MAX_RECEIVE       64

#define k_nSteamNetworkingSend_UnreliableNoNagle        1
#define k_nSteamNetworkingSend_AutoRestartBrokenSession 32
#define k_EFriendFlagImmediate                          0x04

#define CB_SessionRequest       1251    // SteamNetworkingMessagesSessionRequest_t
#define CB_SessionFailed        1252
#define CB_RichPresenceJoin     337     // GameRichPresenceJoinRequested_t

typedef unsigned long long uint64_steam;
typedef int HSteamPipe;

typedef struct {
	int     type;
	int     size;
	byte    data[128];
} steamIdentity_t;          // SteamNetworkingIdentity

typedef struct {
	int     user;
	int     id;
	byte    *param;
	int     paramSize;
} steamCallback_t;          // CallbackMsg_t

// SteamNetworkingMessage_t, the fields used here
#define MSG_DATA( m )       ( *(void **)( (byte *)( m ) + 0 ) )
#define MSG_SIZE( m )       ( *(int *)( (byte *)( m ) + 8 ) )
#define MSG_PEER( m )       ( (steamIdentity_t *)( (byte *)( m ) + 16 ) )

typedef int ( *pInitFlat_t )( char *err );
typedef unsigned char ( *pInit_t )( void );                 // C++ bool
typedef void ( *pShutdown_t )( void );
typedef void *( *pInterface_t )( void );
typedef HSteamPipe ( *pGetPipe_t )( void );
typedef void ( *pDispatchInit_t )( void );
typedef void ( *pDispatchFrame_t )( HSteamPipe );
typedef unsigned char ( *pDispatchNext_t )( HSteamPipe, steamCallback_t * );
typedef void ( *pDispatchFree_t )( HSteamPipe );
typedef uint64_steam ( *pGetSteamID_t )( void *user );
typedef void ( *pInitRelay_t )( void *utils );
typedef int ( *pSendTo_t )( void *msgs, const steamIdentity_t *to, const void *data, unsigned int size, int flags, int channel );
typedef int ( *pReceive_t )( void *msgs, int channel, void **out, int max );
typedef unsigned char ( *pAccept_t )( void *msgs, const steamIdentity_t *who );
typedef void ( *pRelease_t )( void *msg );
typedef void ( *pIdSet_t )( steamIdentity_t *id, uint64_steam steamID );
typedef uint64_steam ( *pIdGet_t )( const steamIdentity_t *id );
typedef unsigned char ( *pSetRP_t )( void *friends, const char *key, const char *value );
typedef int ( *pFriendCount_t )( void *friends, int flags );
typedef uint64_steam ( *pFriendByIndex_t )( void *friends, int index, int flags );
typedef const char *( *pFriendRP_t )( void *friends, uint64_steam friendID, const char *key );
typedef const char *( *pFriendName_t )( void *friends, uint64_steam friendID );

static struct {
	qboolean        tried;
	qboolean        active;
	HMODULE         dll;
	HSteamPipe      pipe;
	void            *msgs, *friends, *user, *utils;
	uint64_steam    self;

	pShutdown_t     Shutdown;
	pDispatchFrame_t DispatchFrame;
	pDispatchNext_t DispatchNext;
	pDispatchFree_t DispatchFree;
	pSendTo_t       SendTo;
	pReceive_t      Receive;
	pAccept_t       Accept;
	pRelease_t      Release;
	pIdSet_t        IdSet;
	pIdGet_t        IdGet;
	pSetRP_t        SetRP;
	pFriendCount_t  FriendCount;
	pFriendByIndex_t FriendByIndex;
	pFriendRP_t     FriendRP;
	pFriendName_t   FriendName;

	int             nextPresence;
	char            presenceConnect[64];
	char            presenceStatus[128];
} st;

static cvar_t *net_steam;
static cvar_t *net_steamFake;


static void *NET_Steam_Proc( const char *name ) {
	void *p = (void *)GetProcAddress( st.dll, name );
	if ( !p ) {
		Com_Printf( "Steam: %s missing from steam_api64.dll\n", name );
	}
	return p;
}

/*
==================
NET_Steam_Load

Finds RealRTCW's steam_api64.dll and starts the Steam API as RealRTCW.
==================
*/
static qboolean NET_Steam_Load( void ) {
	const char *base = Cvar_VariableString( "fs_basepath" );
	char path[MAX_OSPATH];
	char err[1024];
	pInitFlat_t initFlat;
	pDispatchInit_t dispatchInit;
	pGetPipe_t getPipe;
	pGetSteamID_t getSteamID;
	pInitRelay_t initRelay;
	int i;

	// the co-op folder sits inside RealRTCW's, which has the dll
	for ( i = 0; i < 2 && !st.dll; i++ ) {
		Com_sprintf( path, sizeof( path ), i == 0 ? "%s\\..\\steam_api64.dll" : "%s\\steam_api64.dll", base );
		st.dll = LoadLibraryA( path );
	}
	if ( !st.dll ) {
		Com_Printf( "Steam: steam_api64.dll not found next to %s, Steam joining is off\n", base );
		return qfalse;
	}

	// not started by Steam: tell the API which game this is
	SetEnvironmentVariableA( "SteamAppId", STEAM_APPID_REALRTCW );
	SetEnvironmentVariableA( "SteamGameId", STEAM_APPID_REALRTCW );

	initFlat = (pInitFlat_t)GetProcAddress( st.dll, "SteamAPI_InitFlat" );
	err[0] = 0;
	if ( initFlat ) {
		if ( initFlat( err ) != 0 ) {
			Com_Printf( "Steam: not available (%s), Steam joining is off\n", err[0] ? err : "is Steam running?" );
			return qfalse;
		}
	} else {
		pInit_t init = (pInit_t)NET_Steam_Proc( "SteamAPI_Init" );
		if ( !init || !init() ) {
			Com_Printf( "Steam: not available (is Steam running?), Steam joining is off\n" );
			return qfalse;
		}
	}

	st.Shutdown = (pShutdown_t)NET_Steam_Proc( "SteamAPI_Shutdown" );
	dispatchInit = (pDispatchInit_t)NET_Steam_Proc( "SteamAPI_ManualDispatch_Init" );
	getPipe = (pGetPipe_t)NET_Steam_Proc( "SteamAPI_GetHSteamPipe" );
	st.DispatchFrame = (pDispatchFrame_t)NET_Steam_Proc( "SteamAPI_ManualDispatch_RunFrame" );
	st.DispatchNext = (pDispatchNext_t)NET_Steam_Proc( "SteamAPI_ManualDispatch_GetNextCallback" );
	st.DispatchFree = (pDispatchFree_t)NET_Steam_Proc( "SteamAPI_ManualDispatch_FreeLastCallback" );
	st.SendTo = (pSendTo_t)NET_Steam_Proc( "SteamAPI_ISteamNetworkingMessages_SendMessageToUser" );
	st.Receive = (pReceive_t)NET_Steam_Proc( "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel" );
	st.Accept = (pAccept_t)NET_Steam_Proc( "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser" );
	st.Release = (pRelease_t)NET_Steam_Proc( "SteamAPI_SteamNetworkingMessage_t_Release" );
	st.IdSet = (pIdSet_t)NET_Steam_Proc( "SteamAPI_SteamNetworkingIdentity_SetSteamID64" );
	st.IdGet = (pIdGet_t)NET_Steam_Proc( "SteamAPI_SteamNetworkingIdentity_GetSteamID64" );
	st.SetRP = (pSetRP_t)NET_Steam_Proc( "SteamAPI_ISteamFriends_SetRichPresence" );
	st.FriendCount = (pFriendCount_t)NET_Steam_Proc( "SteamAPI_ISteamFriends_GetFriendCount" );
	st.FriendByIndex = (pFriendByIndex_t)NET_Steam_Proc( "SteamAPI_ISteamFriends_GetFriendByIndex" );
	st.FriendRP = (pFriendRP_t)NET_Steam_Proc( "SteamAPI_ISteamFriends_GetFriendRichPresence" );
	st.FriendName = (pFriendName_t)NET_Steam_Proc( "SteamAPI_ISteamFriends_GetFriendPersonaName" );
	getSteamID = (pGetSteamID_t)NET_Steam_Proc( "SteamAPI_ISteamUser_GetSteamID" );
	initRelay = (pInitRelay_t)NET_Steam_Proc( "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess" );

	{
		pInterface_t msgs = (pInterface_t)NET_Steam_Proc( "SteamAPI_SteamNetworkingMessages_SteamAPI_v002" );
		pInterface_t friends = (pInterface_t)NET_Steam_Proc( "SteamAPI_SteamFriends_v018" );
		pInterface_t user = (pInterface_t)NET_Steam_Proc( "SteamAPI_SteamUser_v023" );
		pInterface_t utils = (pInterface_t)NET_Steam_Proc( "SteamAPI_SteamNetworkingUtils_SteamAPI_v004" );

		if ( !msgs || !friends || !user || !utils || !dispatchInit || !getPipe || !st.DispatchFrame ||
			 !st.DispatchNext || !st.DispatchFree || !st.SendTo || !st.Receive || !st.Accept || !st.Release ||
			 !st.IdSet || !st.IdGet || !st.SetRP || !st.FriendCount || !st.FriendByIndex || !st.FriendRP ||
			 !st.FriendName || !getSteamID || !initRelay ) {
			return qfalse;
		}
		st.msgs = msgs();
		st.friends = friends();
		st.user = user();
		st.utils = utils();
	}
	if ( !st.msgs || !st.friends || !st.user || !st.utils ) {
		Com_Printf( "Steam: interfaces unavailable, Steam joining is off\n" );
		return qfalse;
	}

	dispatchInit();
	st.pipe = getPipe();
	st.self = getSteamID( st.user );
	initRelay( st.utils );     // start finding relays now, the first connection is quicker
	return qtrue;
}

static void NET_Steam_Init( void ) {
	if ( st.tried ) {
		return;
	}
	st.tried = qtrue;
	net_steam = Cvar_Get( "net_steam", "1", 0 );
	net_steamFake = Cvar_Get( "net_steamFake", "0", 0 );
	Cvar_Get( "net_steamID", "", CVAR_ROM );

	if ( net_steamFake->integer ) {
		st.active = qtrue;
		st.self = (uint64_steam)Cvar_VariableIntegerValue( "net_port" );
		Com_Printf( "Steam: fake mode, this game is steam:%llu\n", st.self );
	} else if ( net_steam->integer && NET_Steam_Load() ) {
		st.active = qtrue;
		Com_Printf( "Steam: ready, this game is steam:%llu (friends can join with JOIN FRIEND)\n", st.self );
	}
	if ( st.active ) {
		Cvar_Set( "net_steamID", va( "%llu", st.self ) );
	}
}

qboolean NET_Steam_Active( void ) {
	NET_Steam_Init();
	return st.active;
}

static uint64_steam NET_Steam_AdrID( const netadr_t *a ) {
	uint64_steam id;
	Com_Memcpy( &id, a->ip6, sizeof( id ) );
	return id;
}

void NET_Steam_SetAdr( netadr_t *a, unsigned long long id ) {
	Com_Memset( a, 0, sizeof( *a ) );
	a->type = NA_STEAM;
	Com_Memcpy( a->ip6, &id, sizeof( id ) );
}

/*
==================
NET_Steam_Send
==================
*/
void NET_Steam_Send( int length, const void *data, netadr_t to ) {
	uint64_steam id = NET_Steam_AdrID( &to );

	if ( !NET_Steam_Active() ) {
		return;
	}

	if ( net_steamFake->integer ) {
		byte buf[MAX_MSGLEN + 16];
		netadr_t adr;

		if ( length > MAX_MSGLEN ) {
			return;
		}
		Com_Memcpy( buf, "STMF", 4 );
		Com_Memcpy( buf + 4, &st.self, 8 );
		Com_Memcpy( buf + 12, data, length );
		NET_StringToAdr( "127.0.0.1", &adr, NA_IP );
		adr.port = BigShort( (short)id );
		Sys_SendPacket( length + 12, buf, adr );
		return;
	}

	{
		steamIdentity_t ident;
		Com_Memset( &ident, 0, sizeof( ident ) );
		st.IdSet( &ident, id );
		st.SendTo( st.msgs, &ident, data, length,
				   k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, STEAM_CHANNEL );
	}
}

/*
==================
NET_Steam_FakeReceive

net_steamFake: a UDP packet from this PC tagged as coming over Steam. Returns qtrue if it
was one, with the address and message rewritten.
==================
*/
qboolean NET_Steam_FakeReceive( netadr_t *from, msg_t *msg ) {
	uint64_steam id;

	if ( !net_steamFake || !net_steamFake->integer || msg->cursize < 12 || memcmp( msg->data, "STMF", 4 ) ) {
		return qfalse;
	}
	Com_Memcpy( &id, msg->data + 4, 8 );
	memmove( msg->data, msg->data + 12, msg->cursize - 12 );
	msg->cursize -= 12;
	NET_Steam_SetAdr( from, id );
	return qtrue;
}

static void NET_Steam_Dispatch( netadr_t *from, msg_t *msg ) {
	if ( com_sv_running->integer ) {
		Com_RunAndTimeServerPacket( from, msg );
	} else {
		CL_PacketEvent( *from, msg );
	}
}

/*
==================
NET_Steam_Presence

What friends see in Steam: hosting games can be joined straight from the friends list.
==================
*/
static void NET_Steam_SetPresence( const char *connect, const char *status ) {
	if ( strcmp( connect, st.presenceConnect ) ) {
		Q_strncpyz( st.presenceConnect, connect, sizeof( st.presenceConnect ) );
		st.SetRP( st.friends, "connect", connect );
		if ( connect[0] ) {
			Com_Printf( "Steam: friends can join you now (JOIN FRIEND, or Join Game in Steam)\n" );
		}
	}
	if ( strcmp( status, st.presenceStatus ) ) {
		Q_strncpyz( st.presenceStatus, status, sizeof( st.presenceStatus ) );
		st.SetRP( st.friends, "status", status );
	}
}

static void NET_Steam_Presence( void ) {
	const char *mod = Cvar_VariableString( "fs_game" );
	const char *game = !Q_stricmp( mod, "EE" ) ? "Cursed Sands" : "co-op";
	int now = Sys_Milliseconds();

	if ( now < st.nextPresence || net_steamFake->integer ) {
		return;
	}
	st.nextPresence = now + 2000;

	if ( com_sv_running->integer && !com_dedicated->integer ) {
		NET_Steam_SetPresence( va( "+connect steam:%llu", st.self ),
							   va( "Hosting %s %s (VR mod) - Join Game to play", game, Cvar_VariableString( "mapname" ) ) );
	} else if ( com_sv_running->integer ) {
		NET_Steam_SetPresence( "", "" );
	} else {
		NET_Steam_SetPresence( "", va( "In the %s menus (VR mod)", game ) );
	}
}

static void NET_Steam_JoinString( const char *connect ) {
	const char *adr = strstr( connect, "steam:" );

	if ( !adr ) {
		Com_Printf( "Steam: can't join \"%s\"\n", connect );
		return;
	}
	Com_Printf( "Steam: joining %s\n", adr );
	Cbuf_AddText( va( "connect %s\n", adr ) );
}

/*
==================
NET_Steam_Frame

Steam callbacks and incoming packets. Called every time the engine looks for packets.
==================
*/
void NET_Steam_Frame( void ) {
	steamCallback_t cb;
	void *msgs[STEAM_MAX_RECEIVE];
	byte buf[MAX_MSGLEN + 1];
	netadr_t from;
	msg_t msg;
	int n, i;

	if ( !NET_Steam_Active() || net_steamFake->integer ) {
		return;
	}

	st.DispatchFrame( st.pipe );
	while ( st.DispatchNext( st.pipe, &cb ) ) {
		if ( cb.id == CB_SessionRequest && cb.param ) {
			// a friend's game talking to us: only hosts and the server we joined are answered
			// by the engine, so every session can be accepted
			st.Accept( st.msgs, (steamIdentity_t *)cb.param );
		} else if ( cb.id == CB_SessionFailed ) {
			Com_DPrintf( "Steam: a connection failed\n" );
		} else if ( cb.id == CB_RichPresenceJoin && cb.param && cb.paramSize >= 8 + 1 ) {
			NET_Steam_JoinString( (const char *)cb.param + 8 );     // after the friend's CSteamID
		}
		st.DispatchFree( st.pipe );
	}

	for ( ;; ) {
		n = st.Receive( st.msgs, STEAM_CHANNEL, msgs, STEAM_MAX_RECEIVE );
		if ( n <= 0 ) {
			break;
		}
		for ( i = 0; i < n; i++ ) {
			int size = MSG_SIZE( msgs[i] );

			if ( size > 0 && size <= MAX_MSGLEN ) {
				MSG_Init( &msg, buf, sizeof( buf ) );
				Com_Memcpy( buf, MSG_DATA( msgs[i] ), size );
				msg.cursize = size;
				NET_Steam_SetAdr( &from, st.IdGet( MSG_PEER( msgs[i] ) ) );
				NET_Steam_Dispatch( &from, &msg );
			}
			st.Release( msgs[i] );
		}
	}

	NET_Steam_Presence();
}

/*
==================
NET_Steam_Join_f

"steamjoin [name]": join a Steam friend who is hosting (the first one, or the one whose
name contains [name]).
==================
*/
static void NET_Steam_Join_f( void ) {
	const char *want = Cmd_Argc() > 1 ? Cmd_Args() : "";
	int count, i, found = 0;
	char first[64] = "";

	if ( !NET_Steam_Active() ) {
		Com_Error( ERR_DROP, "Steam isn't running, so friends can't be found. Start Steam, then restart the game." );
		return;
	}
	if ( net_steamFake->integer ) {
		Com_Printf( "steamjoin doesn't work with net_steamFake, use connect steam:<port>\n" );
		return;
	}

	count = st.FriendCount( st.friends, k_EFriendFlagImmediate );
	for ( i = 0; i < count; i++ ) {
		uint64_steam id = st.FriendByIndex( st.friends, i, k_EFriendFlagImmediate );
		const char *connect = st.FriendRP( st.friends, id, "connect" );
		const char *name = st.FriendName( st.friends, id );

		if ( !connect || !strstr( connect, "steam:" ) ) {
			continue;
		}
		Com_Printf( "  %s is hosting (%s)\n", name, connect );
		if ( !first[0] && ( !want[0] || Q_stristr( name, want ) ) ) {
			Q_strncpyz( first, connect, sizeof( first ) );
		}
		found++;
	}

	if ( first[0] ) {
		NET_Steam_JoinString( first );
		return;
	}
	if ( found ) {
		Com_Printf( "None of the hosting friends is called \"%s\"\n", want );
		return;
	}
	Com_Error( ERR_DROP, "No Steam friend is hosting a game right now. Your friend should start one first "
			   "(HOST GAME), then try JOIN FRIEND again." );
}

static void NET_Steam_Info_f( void ) {
	if ( !NET_Steam_Active() ) {
		Com_Printf( "Steam: off\n" );
		return;
	}
	Com_Printf( "Steam: on, this game is steam:%llu%s\n", st.self, net_steamFake->integer ? " (fake)" : "" );
}

/*
==================
NET_Steam_Host_f

"hostcoop [map]": start a co-op game others can join, from the first level unless a map
is given.
==================
*/
// the first level of the campaign being played
static const char *NET_Steam_FirstMap( void ) {
	return !Q_stricmp( Cvar_VariableString( "fs_game" ), "EE" ) ? "ee1" : "escape1";
}

/*
==================
NET_Steam_LevelMap

The n-th level of the loaded campaign, in scripts/arenas.txt order (the HOST GAME page
lists them the same way).
==================
*/
static qboolean NET_Steam_LevelMap( int n, char *out, int size ) {
	char *buf, *p, *token;
	int index = 0;
	qboolean found = qfalse;

	if ( n < 0 || FS_ReadFile( "scripts/arenas.txt", (void **)&buf ) <= 0 ) {
		return qfalse;
	}
	p = buf;
	while ( !found ) {
		token = COM_Parse( &p );
		if ( !token[0] ) {
			break;
		}
		if ( !Q_stricmp( token, "map" ) ) {
			token = COM_Parse( &p );
			if ( index++ == n ) {
				Q_strncpyz( out, token, size );
				found = qtrue;
			}
		}
	}
	FS_FreeFile( buf );
	return found;
}

static void NET_Steam_Host_f( void ) {
	char map[MAX_QPATH];
	int gametype = Cvar_VariableIntegerValue( "g_gametype" );

	if ( Cmd_Argc() > 1 ) {
		Q_strncpyz( map, Cmd_Argv( 1 ), sizeof( map ) );
	} else if ( !NET_Steam_LevelMap( Cvar_VariableIntegerValue( "vr_hostLevel" ), map, sizeof( map ) ) ) {
		Q_strncpyz( map, NET_Steam_FirstMap(), sizeof( map ) );
	}
	if ( FS_ReadFile( va( "maps/%s.bsp", map ), NULL ) <= 0 ) {     // in the paks too
		Q_strncpyz( map, NET_Steam_FirstMap(), sizeof( map ) );
	}
	if ( gametype < 0 || gametype > 2 ) {
		Cvar_Set( "g_gametype", "2" );      // GT_COOP
	}
	Cvar_Set( "dedicated", "0" );       // a game the host plays in, never a bare server
	Cbuf_AddText( va( "wait ; wait ; coopmap %s\n", map ) );
}

// defaults for the HOST GAME page, before the game code has registered them
static void NET_Steam_HostDefaults( void ) {
	static const char *defaults[][2] = {
		{ "vr_hostLevel", "0" }, { "g_gametype", "2" }, { "g_gameskill", "2" }, { "sv_maxcoopclients", "4" },
		{ "g_skipcutscenes", "0" }, { "g_reinforce", "0" }, { "g_airespawn", "0" }, { "g_friendlyFire", "0" },
		{ "g_maxlives", "0" }, { "g_sharedlives", "0" }, { "g_freeze", "0" }, { "g_spawnpoints", "0" },
		{ "g_maxspawnpoints", "0" }, { "g_limbotime", "10000" }, { "g_spawnInvul", "5" },
		{ "g_throwKnives", "0" }, { "g_shove", "0" }
	};
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( defaults ); i++ ) {
		Cvar_Get( defaults[i][0], defaults[i][1], CVAR_ARCHIVE );
	}
}


void NET_Steam_Startup( void ) {
	NET_Steam_Init();
	Cmd_AddCommand( "hostcoop", NET_Steam_Host_f );
	NET_Steam_HostDefaults();
	Cmd_AddCommand( "steamjoin", NET_Steam_Join_f );
	Cmd_AddCommand( "steaminfo", NET_Steam_Info_f );
}

void NET_Steam_Shutdown( void ) {
	if ( st.active && !net_steamFake->integer ) {
		steamCallback_t cb;

		NET_Steam_SetPresence( "", "" );
		// leave nothing queued, the API complains endlessly otherwise
		st.DispatchFrame( st.pipe );
		while ( st.DispatchNext( st.pipe, &cb ) ) {
			st.DispatchFree( st.pipe );
		}
		if ( st.Shutdown ) {
			st.Shutdown();
		}
	}
	st.active = qfalse;
}

#else   // no Steam: dedicated servers and other platforms

qboolean NET_Steam_Active( void ) { return qfalse; }
void NET_Steam_SetAdr( netadr_t *a, unsigned long long id ) {
	Com_Memset( a, 0, sizeof( *a ) );
	a->type = NA_STEAM;
	Com_Memcpy( a->ip6, &id, sizeof( id ) );
}
void NET_Steam_Send( int length, const void *data, netadr_t to ) {}
qboolean NET_Steam_FakeReceive( netadr_t *from, msg_t *msg ) { return qfalse; }
void NET_Steam_Frame( void ) {}
void NET_Steam_Startup( void ) {}
void NET_Steam_Shutdown( void ) {}

#endif
