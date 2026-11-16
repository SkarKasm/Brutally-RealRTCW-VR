#include "steam.h"

#include "../steamshim/steamshim_child.h"

#include <stdint.h>

#ifdef STEAM

void steamRun(void)
{
	STEAMSHIM_pump();
	
	return;
}

int steamInit(void)
{
	if (!STEAMSHIM_init())
	{
		return 0;
	}

	STEAMSHIM_requestStats();

	return 1;
}

void steamSetAchievement(const char* id)
{
	STEAMSHIM_setAchievement(id, 1);

	STEAMSHIM_storeStats();

	return;
}

void steamResetStats(const int bAlsoAchievements)
{
	STEAMSHIM_resetStats(bAlsoAchievements);

	return;
}

int steamAlive()
{
	return STEAMSHIM_alive();
}

void steamSetRichPresence(const char* key, const char* value)
{
	STEAMSHIM_setRichPresence(key, value);
}

#else

/*
RealRTCW-VR: achievements without RealRTCW's launcher.

The game's own way to Steam is the launcher that starts it (steamshim), and this build isn't
started by it, so every achievement was thrown away. But the engine has Steam running in this
very process, as RealRTCW (qcommon/net_steam.c, for co-op over Steam: "Steam: ready" in the
console), and keeps its callbacks served: an achievement is given to that, by the same name,
under the same rules (the callers': no cheats, and so on). Without Steam (not running, another
engine, net_steam 0) nothing happens, as before. Nothing else of the shim's is done this way:
stats aren't reset, and the presence shown to friends stays the engine's.
*/
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef void *( *steamStats_t )( void );
typedef int ( *steamPipe_t )( void );
typedef unsigned char ( *steamSet_t )( void *stats, const char *name );      // (bool)
typedef unsigned char ( *steamStore_t )( void *stats );
typedef unsigned char ( *steamRequest_t )( void *stats );

static struct {
	void            *stats;         // ISteamUserStats, 0 until Steam is there
	steamSet_t      Set;
	steamStore_t    Store;
} steamDirect;

static int steamDirectReady( void ) {
	static const char *versions[] = { "SteamAPI_SteamUserStats_v013", "SteamAPI_SteamUserStats_v012", "SteamAPI_SteamUserStats_v011" };
	HMODULE dll;
	steamPipe_t pipe;
	steamRequest_t request;
	int i;

	if ( steamDirect.stats ) {
		return 1;
	}
	// only what the engine loaded and started: never started from here
	dll = GetModuleHandleA( "steam_api64.dll" );
	if ( !dll ) {
		return 0;
	}
	pipe = (steamPipe_t)GetProcAddress( dll, "SteamAPI_GetHSteamPipe" );
	if ( !pipe || !pipe() ) {
		return 0;       // (loaded, not started)
	}
	steamDirect.Set = (steamSet_t)GetProcAddress( dll, "SteamAPI_ISteamUserStats_SetAchievement" );
	steamDirect.Store = (steamStore_t)GetProcAddress( dll, "SteamAPI_ISteamUserStats_StoreStats" );
	if ( !steamDirect.Set || !steamDirect.Store ) {
		return 0;
	}
	for ( i = 0; i < (int)( sizeof( versions ) / sizeof( versions[0] ) ) && !steamDirect.stats; i++ ) {
		steamStats_t stats = (steamStats_t)GetProcAddress( dll, versions[i] );
		if ( stats ) {
			steamDirect.stats = stats();
		}
	}
	if ( !steamDirect.stats ) {
		return 0;
	}
	// (an older Steam library wants the player's stats asked for before any is set; RealRTCW's
	// has them from the start and no such call)
	request = (steamRequest_t)GetProcAddress( dll, "SteamAPI_ISteamUserStats_RequestCurrentStats" );
	if ( request ) {
		request( steamDirect.stats );
	}
	return 1;
}

void steamRun(void)
{
	return;     // (the engine serves Steam's callbacks)
}

int steamInit(void)
{
	return steamDirectReady();
}

void steamSetAchievement(const char* id)
{
	if ( !id || !id[0] || !steamDirectReady() ) {
		return;
	}
	if ( steamDirect.Set( steamDirect.stats, id ) ) {
		steamDirect.Store( steamDirect.stats );
	}
}

#else

void steamRun(void)
{
	return;
}

int steamInit(void)
{
	return 0;
}

void steamSetAchievement(const char* id)
{
	return;
}

#endif

int steamAlive()
{
	return 1;
}

void steamSetRichPresence(const char* key, const char* value)
{
	return;
}

void steamResetStats(const int bAlsoAchievements)
{
	return;
}

#endif