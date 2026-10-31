// Offline test for the vr_physics (Jolt) ragdolls: real RTCW maps, a fake human skeleton, no game.
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

static int RunMap( const char *map, int physics, int count ) {
	vec3_t spawns[64], start[64];
	char path[128];
	int n, i, t, bad = 0, t0;
	refEntity_t body;

	snprintf( path, sizeof( path ), "%s/maps/%s.bsp", mapDir, map );
	n = ReadSpawns( path, spawns, count );
	printf( "== %s (%s): %d bodies\n", map, physics ? "Jolt" : "old", n );
	memset( &cg, 0, sizeof( cg ) ); memset( cg_entities, 0, sizeof( cg_entities ) );
	snprintf( cgs.mapname, sizeof( cgs.mapname ), "maps/%s.bsp", map );
	cvPhysics = physics;
	snap.ps.clientNum = 63;
	cg.snap = &snap;
	CG_Ragdoll_Init();
	CG_Physics_Init();
	cg.time = 1000;
	t0 = trap_Milliseconds();
	CG_Physics_Frame();
	printf( "first frame (builds the level): %d ms\n", trap_Milliseconds() - t0 );

	for ( i = 0; i < n; i++ ) {
		centity_t *c = &cg_entities[i];
		c->currentState.number = i;
		c->currentState.clientNum = i;
		c->currentState.eType = ET_PLAYER;
		c->currentState.eFlags = EF_DEAD;
		c->currentState.otherEntityNum2 = -1;
		c->currentValid = qtrue;
		VectorCopy( spawns[i], c->lerpOrigin );
		VectorCopy( spawns[i], c->currentState.pos.trBase );
		VectorCopy( spawns[i], start[i] );
	}
	// one killer for the first body, to push it over
	if ( n > 1 ) {
		cg_entities[0].currentState.otherEntityNum2 = 60;
		VectorCopy( spawns[0], cg_entities[60].lerpOrigin );
		cg_entities[60].lerpOrigin[0] -= 100;
	}

	// brush entities 1..20 in the snapshot (doors where the map has them), moving up and down, gone at 5 s
	snap.numEntities = 20;
	for ( i = 0; i < 20; i++ ) {
		memset( &snap.entities[i], 0, sizeof( snap.entities[i] ) );
		snap.entities[i].number = 100 + i;
		snap.entities[i].solid = SOLID_BMODEL;
		snap.entities[i].modelindex = i + 1;
		VectorClear( cg_entities[100 + i].lerpOrigin );
		VectorClear( cg_entities[100 + i].lerpAngles );
	}
	t0 = trap_Milliseconds();
	for ( t = 0; t < 8000; t += 11 ) {
		cg.time += 11;
		snap.serverTime = cg.time - cg.time % 50;
		for ( i = 0; i < 20; i++ ) {
			cg_entities[100 + i].lerpOrigin[2] = 20 * sin( t * 0.002 );
			cg_entities[100 + i].lerpAngles[YAW] = t * 0.01f;
		}
		if ( t >= 5000 ) {
			snap.numEntities = 10;
		}
		CG_Physics_Frame();
		if ( t == 1001 && n > 2 ) {
			vec3_t dir = { 0, 1, 0 }, pt;
			VectorCopy( spawns[2], pt ); pt[2] += 10;
			CG_Ragdoll_Hit( 2, pt, dir, 650.0f );      // a kick
		}
		if ( t == 2002 && n > 3 ) {
			vec3_t o;
			VectorCopy( spawns[3], o ); o[0] += 40;
			CG_Ragdoll_Explosion( o, 300.0f, 650.0f );
		}
		if ( n > 1 ) {      // body 1 is carried from 1.5 s to 3 s, its grab point 50 units up and moving
			centity_t *c = &cg_entities[1];
			if ( t >= 1500 && t < 3000 ) {
				c->currentState.dl_intensity = 64;
				VectorCopy( spawns[1], c->currentState.origin2 );
				c->currentState.origin2[2] += 50;
				c->currentState.origin2[0] += ( t - 1500 ) * 0.03f;
			} else {
				c->currentState.dl_intensity = 0;
			}
		}
		for ( i = 0; i < n; i++ ) {
			memset( &body, 0, sizeof( body ) );
			body.hModel = 1;
			VectorCopy( cg_entities[i].lerpOrigin, body.origin );
			body.origin[2] -= 24;
			AxisClear( body.axis );
			AxisClear( body.torsoAxis );
			CG_Ragdoll_Apply( &cg_entities[i], &body );
			if ( t + 11 >= 8000 && body.ragdollBones ) {
				vec3_t pel, neck, head, th, ca, ft, ua, fa, ha;
				float tl, kneeCos, elbowCos;
				vec3_t u, v;
				int k, nan = 0;
				for ( k = 0; k < NB * 12; k++ ) {
					if ( isnan( body.ragdollBones[k] ) || fabs( body.ragdollBones[k] ) > 1e5 ) nan = 1;
				}
				BoneWorld( &body, B_PELVIS, pel ); BoneWorld( &body, B_NECK, neck ); BoneWorld( &body, B_HEAD, head );
				BoneWorld( &body, B_LTHIGH, th ); BoneWorld( &body, B_LCALF, ca ); BoneWorld( &body, B_LFOOT, ft );
				BoneWorld( &body, B_LARM, ua ); BoneWorld( &body, B_LFORE, fa ); BoneWorld( &body, B_LHAND, ha );
				tl = Distance( pel, neck );
				VectorSubtract( ca, th, u ); VectorNormalize( u ); VectorSubtract( ft, ca, v ); VectorNormalize( v );
				kneeCos = DotProduct( u, v );
				VectorSubtract( fa, ua, u ); VectorNormalize( u ); VectorSubtract( ha, fa, v ); VectorNormalize( v );
				elbowCos = DotProduct( u, v );
				printf( "  body %2d: pelvis %7.1f %7.1f %7.1f (from %7.1f: dz %6.1f, moved %5.1f) torso %.1f knee %3.0f deg elbow %3.0f deg%s\n",
						i, pel[0], pel[1], pel[2], start[i][2] - 24 + 36, pel[2] - ( start[i][2] - 24 + 36 ),
						sqrt( ( pel[0] - start[i][0] ) * ( pel[0] - start[i][0] ) + ( pel[1] - start[i][1] ) * ( pel[1] - start[i][1] ) ),
						tl, RAD2DEG( acos( Com_Clamp( -1, 1, kneeCos ) ) ), RAD2DEG( acos( Com_Clamp( -1, 1, elbowCos ) ) ), nan ? " NaN/far!" : "" );
				if ( nan || pel[2] < start[i][2] - 24 - 150 ) bad++;
			}
		}
	}
	printf( "8 s of %d bodies: %d ms in all\n", n, trap_Milliseconds() - t0 );
	CG_Physics_Shutdown();
	return bad;
}

int main( int argc, char **argv ) {
	int i, bad = 0;
	if ( getenv( "PHYS_MAPS" ) ) {
		mapDir = getenv( "PHYS_MAPS" );
	}
	for ( i = 1; i < argc; i++ ) {
		bad += RunMap( argv[i], 2, 24 );
	}
	if ( argc > 1 ) {
		printf( "(the old ragdolls next: traces are stubbed here, so they fall through the floor; this only checks they still run)\n" );
		RunMap( argv[1], 0, 6 );
	}
	printf( "%s\n", bad ? "FAILED" : "ok" );
	return bad != 0;
}
