// Stress test for the vr_physics (Jolt) ragdolls: do bodies go flying? (see stress() below)
// Shares the stubs of harness.c. Offline test for the vr_physics (Jolt) ragdolls: real RTCW maps, a fake human skeleton, no game.
//
// Drops a ragdoll at every AI and player start of each map (up to 24), kicks one, blows one up,
// carries one, moves the first 20 brush entities about, and after 8 s reports where each body
// lies. A body that fell through the floor or went NaN fails the run. Build and run: build.sh
#include "../../code/cgame/cg_local.h"
#include "../../code/qcommon/qfiles.h"
#include <time.h>
#include <stdarg.h>

cg_t cg;
cgs_t cgs;
centity_t cg_entities[MAX_GENTITIES];
cgVRState_t cgvr;
static snapshot_t snap;

void QDECL CG_Printf( const char *msg, ... ) {
	va_list ap; va_start( ap, msg ); vprintf( msg, ap ); va_end( ap );
}
void QDECL CG_Error( const char *msg, ... ) {
	va_list ap; va_start( ap, msg ); vprintf( msg, ap ); va_end( ap ); exit( 1 );
}
void QDECL Com_Error( int level, const char *msg, ... ) {
	va_list ap; va_start( ap, msg ); vprintf( msg, ap ); va_end( ap ); exit( 1 );
}
void QDECL Com_Printf( const char *msg, ... ) {
	va_list ap; va_start( ap, msg ); vprintf( msg, ap ); va_end( ap );
}
void CG_Trace( trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int skipNumber, int mask ) {
	memset( result, 0, sizeof( *result ) );
	result->fraction = 1;
	VectorCopy( end, result->endpos );
}
float CG_VRX_Knock( qboolean dead ) { return 1.0f; }
void CG_VRX_Flinch( int entityNum, const vec3_t point, const vec3_t dir ) { }
int CG_VR_DominantHand( void ) { return 0; }

static int cvPhysics = 1, cvRagdolls = 1;
static const char *mapDir = ".";     // holds maps/*.bsp (unzipped from the game's pk3s)
void trap_Cvar_Register( vmCvar_t *v, const char *name, const char *def, int flags ) {
	Q_strncpyz( v->string, def, sizeof( v->string ) );
	v->value = atof( def ); v->integer = atoi( def );
	if ( !strcmp( name, "vr_physics" ) ) { v->integer = cvPhysics; v->value = cvPhysics; }
	if ( !strcmp( name, "vr_ragdolls" ) ) { v->integer = cvRagdolls; v->value = cvRagdolls; }
}
void trap_Cvar_Update( vmCvar_t *v ) { }
int trap_Milliseconds( void ) { return (int)( clock() * 1000.0 / CLOCKS_PER_SEC ); }

static FILE *files[8];
int trap_FS_FOpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode ) {
	char path[256];
	FILE *fp;
	snprintf( path, sizeof( path ), "%s/%s", mapDir, qpath );
	fp = fopen( path, "rb" );
	if ( !fp ) { *f = 0; return -1; }
	files[1] = fp; *f = 1;
	fseek( fp, 0, SEEK_END ); { int len = ftell( fp ); fseek( fp, 0, SEEK_SET ); return len; }
}
void trap_FS_Read( void *buffer, int len, fileHandle_t f ) { fread( buffer, 1, len, files[f] ); }
void trap_FS_FCloseFile( fileHandle_t f ) { fclose( files[f] ); files[f] = NULL; }

// a standing man, feet at 0, facing +x, left +y
static const struct { const char *name; int parent; float x, y, z; } bonesDef[] = {
	{ "Bip01 Pelvis", -1, 0, 0, 36 }, { "Bip01 Spine1", 0, 0, 0, 42 }, { "Bip01 Spine2", 1, 0, 0, 48 },
	{ "Bip01 Neck", 2, 0, 0, 56 }, { "Bip01 Head", 3, 0, 0, 60 }, { "tag_head", 4, 0, 0, 61 },
	{ "Bip01 L UpperArm", 2, 0, 8, 54 }, { "Bip01 L Forearm", 6, 0, 8, 43 }, { "Bip01 L Hand", 7, 2, 8, 33 }, { "Bip01 L Finger1", 8, 3, 8, 29 },
	{ "Bip01 R UpperArm", 2, 0, -8, 54 }, { "Bip01 R Forearm", 10, 0, -8, 43 }, { "Bip01 R Hand", 11, 2, -8, 33 }, { "Bip01 R Finger1", 12, 3, -8, 29 },
	{ "Bip01 L Thigh", 0, 0, 4, 34 }, { "Bip01 L Calf", 14, 1, 4, 19 }, { "Bip01 L Foot", 15, 0, 4, 4 }, { "Bip01 L Toe0", 16, 5, 4, 1 },
	{ "Bip01 R Thigh", 0, 0, -4, 34 }, { "Bip01 R Calf", 18, 1, -4, 19 }, { "Bip01 R Foot", 19, 0, -4, 4 }, { "Bip01 R Toe0", 20, 5, -4, 1 },
};
#define NB ( (int)( sizeof( bonesDef ) / sizeof( bonesDef[0] ) ) )
enum { B_PELVIS = 0, B_NECK = 3, B_HEAD = 4, B_LTHIGH = 14, B_LCALF = 15, B_LFOOT = 16, B_LARM = 6, B_LFORE = 7, B_LHAND = 8 };

int trap_R_GetSkeleton( const refEntity_t *refent, int maxBones, int *parents, int *flags, char *names, float *bones ) {
	int i;
	for ( i = 0; i < NB && i < maxBones; i++ ) {
		float *b = bones + i * 12;
		parents[i] = bonesDef[i].parent;
		flags[i] = !strcmp( bonesDef[i].name, "tag_head" );
		Q_strncpyz( names + i * 32, bonesDef[i].name, 32 );
		memset( b, 0, sizeof( float ) * 12 );
		b[0] = b[4] = b[8] = 1;
		b[9] = bonesDef[i].x; b[10] = bonesDef[i].y; b[11] = bonesDef[i].z;
	}
	return NB;
}

// spawn points from the map's entities
static int ReadSpawns( const char *bspPath, vec3_t *out, int max ) {
	FILE *fp = fopen( bspPath, "rb" );
	dheader_t h; char *ents, *p; int n = 0, ofs, len;
	if ( !fp ) return 0;
	fread( &h, sizeof( h ), 1, fp );
	ofs = h.lumps[0].fileofs; len = h.lumps[0].filelen;
	ents = malloc( len + 1 ); fseek( fp, ofs, SEEK_SET ); fread( ents, 1, len, fp ); ents[len] = 0; fclose( fp );
	for ( p = ents; ( p = strchr( p, '{' ) ) && n < max; ) {
		char *end = strchr( p, '}' ), *cls, *org;
		if ( !end ) break;
		*end = 0;
		cls = strstr( p, "\"classname\"" ); org = strstr( p, "\"origin\"" );
		if ( cls && org && ( strstr( cls, "\"ai_" ) || strstr( cls, "info_player" ) ) && ( strstr( cls, "\"ai_" ) == cls + 12 || strstr( cls, "\"info_player" ) == cls + 12 ) ) {
			if ( sscanf( org + 9, " \"%f %f %f\"", &out[n][0], &out[n][1], &out[n][2] ) == 3 ) n++;
		}
		p = end + 1;
	}
	free( ents );
	return n;
}

static void BoneWorld( const refEntity_t *b, int i, vec3_t out ) {
	VectorAdd( b->origin, b->ragdollBones + i * 12 + 9, out );
}


#define MAXB 48
typedef struct {
	int     used;
	vec3_t  origin;         // server origin (feet + 24)
	float   yaw;
	vec3_t  vel;            // trDelta at death
	vec3_t  last, lastDir;
	int     calmAt, riseAt;
	float   maxStart;
	float   maxSpeed, maxRise, maxSpin, maxSpeedCalm, maxSpinCalm;
	float   startZ;
	int     restAt;
	int     inWall;
} sbody_t;
static sbody_t sb[MAXB];
static int lastBlast = -100000, lastHitBurst = -100000;

static void Body( int i, const vec3_t org, float yaw, float runSpeed ) {
	centity_t *c = &cg_entities[i];
	memset( &sb[i], 0, sizeof( sb[i] ) );
	sb[i].used = 1;
	VectorCopy( org, sb[i].origin );
	sb[i].yaw = yaw;
	sb[i].vel[0] = cos( DEG2RAD( yaw ) ) * runSpeed;
	sb[i].vel[1] = sin( DEG2RAD( yaw ) ) * runSpeed;
	sb[i].startZ = org[2] - 24 + 36;
	sb[i].restAt = -1;
	memset( c, 0, sizeof( *c ) );
	c->currentState.number = i;
	c->currentState.clientNum = i;
	c->currentState.eType = ET_PLAYER;
	c->currentState.eFlags = EF_DEAD;
	c->currentState.otherEntityNum2 = -1;
	VectorCopy( sb[i].vel, c->currentState.pos.trDelta );
	c->currentValid = qtrue;
	VectorCopy( org, c->lerpOrigin );
	VectorCopy( org, c->currentState.pos.trBase );
}

static int Stress( const char *map, int physics ) {
	vec3_t spawns[64];
	char path[128];
	int n, i, t, flyers = 0, nb = 0, k;
	refEntity_t body;
	unsigned seed = 1234;
#define RND() ( ( seed = seed * 1103515245u + 12345u ) >> 8 & 0xffff ) / 65535.0f

	snprintf( path, sizeof( path ), "%s/maps/%s.bsp", mapDir, map );
	n = ReadSpawns( path, spawns, 12 );
	memset( &cg, 0, sizeof( cg ) ); memset( cg_entities, 0, sizeof( cg_entities ) ); memset( sb, 0, sizeof( sb ) );
	snprintf( cgs.mapname, sizeof( cgs.mapname ), "maps/%s.bsp", map );
	cvPhysics = physics;
	snap.ps.clientNum = 63;
	snap.numEntities = 0;
	cg.snap = &snap;
	CG_Ragdoll_Init();
	CG_Physics_Init();
	cg.time = 1000;
	CG_Physics_Frame();
	if ( n < 4 ) { printf( "%s: too few spawns\n", map ); return 0; }

	// A: a pile, 8 bodies dying on the same spot, turned every way, some of them running
	for ( k = 0; k < 8; k++ ) Body( nb++, spawns[0], RND() * 360, k & 1 ? 280 : 0 );
	// B: bodies dying against walls: 4 directions at 6..18 units from each of 4 spawns (some end up half in a wall)
	for ( i = 1; i < 5 && i < n; i++ ) {
		for ( k = 0; k < 4; k++ ) {
			vec3_t o; float d = 6 + 12 * RND();
			VectorCopy( spawns[i], o );
			o[k >> 1] += ( k & 1 ? -1 : 1 ) * d * 3;
			Body( nb++, o, k * 90, 0 );
		}
	}
	// C: 4 bodies that get bursts and shotgun blasts
	for ( i = 5; i < 9 && i < n; i++ ) Body( nb++, spawns[i], RND() * 360, 150 );

	for ( t = 0; t < 10000; t += 11 ) {
		cg.time += 11;
		CG_Physics_Frame();
		// bursts on the C bodies: an MP40 from 1 to 2 s, a shotgun at 3 s, from above at 4 s
		for ( i = 24; i < nb; i++ ) {
			vec3_t dir, pt;
			if ( t >= 1000 && t < 2000 && ( t / 11 ) % 9 == 0 ) {
				VectorSet( dir, RND() - 0.5f, RND() - 0.5f, -0.2f );
				VectorCopy( sb[i].last, pt ); pt[0] += ( RND() - 0.5f ) * 20; pt[1] += ( RND() - 0.5f ) * 20;
				CG_Ragdoll_Hit( i, pt, dir, 160 ); lastHitBurst = t;
			}
			if ( t == 3003 || ( t == 4004 ) ) {
				for ( k = 0; k < 10; k++ ) {
					VectorSet( dir, 1, ( RND() - 0.5f ) * 0.2f, t == 4004 ? -2.0f : 0 );
					VectorCopy( sb[i].last, pt ); pt[2] += ( RND() - 0.5f ) * 10;
					CG_Ragdoll_Hit( i, pt, dir, 160 );
				}
				lastHitBurst = t;
			}
			if ( t == 5005 ) { VectorSet( dir, 0, 1, 0.3f ); CG_Ragdoll_Hit( i, sb[i].last, dir, 650 ); lastHitBurst = t; }   // kick
		}
		// D: three grenades into the pile, and one at a wall group
		if ( t == 2002 || t == 2497 || t == 2992 || t == 6006 ) {
			vec3_t o;
			VectorCopy( t == 6006 ? spawns[1] : spawns[0], o ); o[0] += 30; o[2] -= 16;
			CG_Ragdoll_Explosion( o, 450, 900 );
			lastBlast = t;
		}
		for ( i = 0; i < nb; i++ ) {
			vec3_t pel, head, dir;
			float sp, spin, ang = DEG2RAD( sb[i].yaw );
			int calm = t - lastBlast > 1500 && t - lastHitBurst > 1500 && t > 1500;
			memset( &body, 0, sizeof( body ) );
			body.hModel = 1;
			VectorCopy( sb[i].origin, body.origin );
			body.origin[2] -= 24;
			body.axis[0][0] = cos( ang ); body.axis[0][1] = sin( ang );
			body.axis[1][0] = -sin( ang ); body.axis[1][1] = cos( ang );
			body.axis[2][2] = 1;
			AxisClear( body.torsoAxis );
			CG_Ragdoll_Apply( &cg_entities[i], &body );
			if ( !body.ragdollBones ) continue;
			BoneWorld( &body, B_PELVIS, pel ); BoneWorld( &body, B_HEAD, head );
			VectorSubtract( head, pel, dir ); VectorNormalize( dir );
			if ( t > 0 ) {
				sp = Distance( pel, sb[i].last ) / 0.011f;
				spin = acos( Com_Clamp( -1, 1, DotProduct( dir, sb[i].lastDir ) ) ) / 0.011f;
				if ( sp > sb[i].maxSpeed ) sb[i].maxSpeed = sp;
				if ( t < 1000 && sp > sb[i].maxStart ) sb[i].maxStart = sp;
				if ( spin > sb[i].maxSpin ) sb[i].maxSpin = spin;
				if ( calm && sp > sb[i].maxSpeedCalm ) { sb[i].maxSpeedCalm = sp; sb[i].calmAt = t; }
				if ( calm && spin > sb[i].maxSpinCalm ) sb[i].maxSpinCalm = spin;
				if ( pel[2] - sb[i].startZ > sb[i].maxRise ) { sb[i].maxRise = pel[2] - sb[i].startZ; sb[i].riseAt = t; }
				if ( sp > 2 ) sb[i].restAt = -1; else if ( sb[i].restAt < 0 ) sb[i].restAt = t;
			}
			VectorCopy( pel, sb[i].last ); VectorCopy( dir, sb[i].lastDir );
		}
	}
	printf( "== %s (%s): %d bodies. speeds in units/s, spin in turns/s; 'calm' = more than 1.5 s after any blast or burst\n", map, physics ? "Jolt" : "old", nb );
	for ( i = 0; i < nb; i++ ) {
		const char *grp = i < 8 ? "pile " : i < 24 ? "wall " : "shots";
		int fly = sb[i].maxStart > 450 || sb[i].maxSpeedCalm > 250 || sb[i].maxSpinCalm / ( 2 * M_PI ) > 1.5f || sb[i].maxSpeed > 1500 || sb[i].maxRise > 300 ||
				  isnan( sb[i].last[0] ) || sb[i].last[2] < sb[i].startZ - 300;
		flyers += fly;
		printf( "  %2d %s start %4.0f max speed %6.0f (calm %5.0f at %5.2f s, rise at %5.2f s)  max spin %4.1f (calm %4.1f)  rose %5.1f  final dz %6.1f  at rest from %5.1f s%s\n",
				i, grp, sb[i].maxStart, sb[i].maxSpeed, sb[i].maxSpeedCalm, sb[i].calmAt / 1000.0f, sb[i].riseAt / 1000.0f, sb[i].maxSpin / ( 2 * M_PI ), sb[i].maxSpinCalm / ( 2 * M_PI ), sb[i].maxRise,
				sb[i].last[2] - sb[i].startZ, sb[i].restAt < 0 ? -1.0f : sb[i].restAt / 1000.0f, fly ? "   <-- FLYING" : "" );
	}
	CG_Physics_Shutdown();
	return flyers;
}

int main( int argc, char **argv ) {
	int i, bad = 0;
	if ( getenv( "PHYS_MAPS" ) ) mapDir = getenv( "PHYS_MAPS" );
	for ( i = 1; i < argc; i++ ) bad += Stress( argv[i], 1 );
	printf( "%d flying bodies\n", bad );
	return bad != 0;
}
