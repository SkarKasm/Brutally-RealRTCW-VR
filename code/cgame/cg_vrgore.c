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

// cg_vrgore.c -- voxel gore (g_voxelGore): bullets open the body they hit (cg_gore.cpp)
//
// After the layered gore of Max Payne 2 VR (evilSheep): a bullet carves a tunnel through
// the character's own body, out the other side if it has the energy, through bone too.
// The skin opens over it and the wound walls show fat, muscle and bone, and move with the
// body alive, dead or as a ragdoll. A part shot clean off (a hand, a foot, a forearm) falls
// away as itself, drawn from the body's own skin, from the living too.
//
// All on the client, like the rest of the gore: each player's cgame carves what it sees.
// The server's own gore (knife decapitation) is untouched. Heads are separate models and
// keep their own wounds (cg_vrextras.c).

#include "cg_local.h"
#include "cg_gore.h"
#include "../game/bg_vrextras.h"

void CG_AddBloodTrails( vec3_t origin, vec3_t dir, int speed, int duration, int count, float randScale );     // cg_effects.c

#define VRG_MAX_ENTS        24          // wounded bodies tracked at once
#define VRG_MAX_PIECES      8
#define VRG_PIECE_LIFE      30000
#define VRG_MAX_BONES       128
#define VRG_POSE_FLOATS     ( VRG_MAX_BONES * 12 + 2 + 4 + VRG_TRAILER_FLOATS )     // pose, a cut, ours
#define VRX_CUT_MAGIC       0x56524354      // "VRCT", as tr_animation.c
#define VRX_CUT_FLOATS      ( 2 + 4 )

typedef union { float f; int i; unsigned u; } vrgBits_t;

// a skeletal model read for gore (0: not tried yet, -1: no good)
typedef struct {
	qhandle_t   hModel;
	int         gore;
	int         numBones;       // 1: a head
	char        names[VRG_MAX_BONES][32];
} vrgModel_t;

// head models as they were registered (their file names), for reading them
typedef struct {
	qhandle_t   hModel;
	char        path[MAX_QPATH];
} vrgHeadName_t;

#define VRG_TRAIL_SEGS      24
typedef struct {
	int         bone;
	vec3_t      pos, dir;       // rest pose: where it starts, which way it ran
	vec3_t      normal;         // which way the skin faces there
	goreAnchor_t anchor;        // the spot of the skin it starts on (it follows the skin as he bends)
	float       len;            // inches
} vrgSeg_t;

// blood round a hole: where on the body (rest pose), which way the skin faces there, how big
#define VRG_MAX_SPLATS      24
#define VRX_WOUND_MAGIC     0x56525744      // "VRWD", as tr_animation.c R_VRX_BodyWounds
typedef struct {
	int         bone;
	vec3_t      pos, normal;
	float       size, flip;     // the hole's rim splash: half its width (inches), mirrored or not
	int         exit;
	int         time;           // when it was made: the blood soaks out and runs down from then on
	int         nextDrip;
	// the blood running from it (Overgrowth's): a trail over the skin, each stretch of it the
	// way down was when it ran there, so it turns as he falls and stops where he lies flat
	int         flowTime, numSegs;
	float       rate;
	vrgSeg_t    segs[VRG_TRAIL_SEGS];
	vrbWound_t  vrb;            // vr_bloodFX: the stream it pumps out, and its drips
	int         nextMark;       // vr_bloodFX: when its stream next marks his own clothes under it
	vec3_t      base;           // the way the trail is heading overall (turning towards down)
	float       heading;        // and how far off that it has wandered (radians, drifting)
	qboolean    joined;         // it ran into another trail (or back into itself): it goes on in that one
	qboolean    snapped;        // normal is the skin's (till then: back along the shot)
	goreAnchor_t anchor;        // the spot of the skin the wound is on: its blood stays on it however he bends
	int         wait;           // nowhere to run just now: not looked at again before this
} vrgSplat_t;

// without the atlas, a draw for each kind of blood, 32 marks each: the rim splash, the soak
// round it, the run's tip, its trail (four draws)
#define VRG_BLOOD_SETS      7
#define VRG_SET_MARKS       32
#define VRG_SET_FLOATS      ( VRG_POSE_FLOATS + 2 + VRG_SET_MARKS * 12 )
#define VRG_HEAD_SET_FLOATS ( 2 + 2 + VRG_SET_MARKS * 12 )
#define VRG_TRAIL_MARKS     ( 4 * VRG_SET_MARKS )   // stretches of trail drawn on one body
// (with the atlas all of it is one draw, in the same room)
typedef char vrgBodyRoom_t[VRG_BLOOD_SETS * VRG_SET_FLOATS >= VRG_POSE_FLOATS + 2 + VRX_DECAL_MAX * 12 ? 1 : -1];
#define VRG_HEAD_MARKS      ( ( VRG_BLOOD_SETS * VRG_HEAD_SET_FLOATS - 4 ) / 12 )
#define VRG_SOAK_MS         15000
#define VRG_BLEED_MS        30000   // how long a wound runs
#define VRG_DRIP_MS         12000
#define VRG_TURN            0.5f    // radians a trail can turn from one stretch to the next
#define VRG_SEG_LEN         0.9f    // inches a stretch of the trail goes before the next starts: short, so it curves
#define VRG_RUN_SPEED       2.6f    // inches a second at first, straight down

// a body being drawn: its last pose (to take shots into), and its voxel body once hit
typedef struct {
	int         entityNum;
	qhandle_t   hModel;
	int         body;           // cg_gore.cpp handle, 0 until shot
	int         lastSeen;
	qboolean    dead;
	int         numBones;
	refEntity_t ref;            // as last drawn
	qboolean    posed;          // pose is that of ref (worked out only when it's needed)
	float       pose[VRG_MAX_BONES * 12];
	vec3_t      origin, axis[3];
	float       draw[VRG_POSE_FLOATS];      // what the renderer reads this frame

	// its head (a model of its own on tag_head)
	qhandle_t   hHead;
	int         head;           // cg_gore.cpp handle, 0 until shot
	int         headSeen;
	vec3_t      headOrigin, headAxis[3];
	float       headDraw[2];
	refEntity_t headRef;        // as last drawn
	qboolean    hasHeadRef;

	// the blood round each hole in and out (bodies: rest pose and bone; heads: the head's space)
	int         numSplats, nextSplat, numHeadSplats, nextHeadSplat;
	vrgSplat_t  splats[VRG_MAX_SPLATS], headSplats[VRG_MAX_SPLATS];
	float       splatDraw[VRG_BLOOD_SETS][VRG_SET_FLOATS];
	float       headSplatDraw[VRG_BLOOD_SETS][VRG_HEAD_SET_FLOATS];
	// what landed on him (cg_vrextras.c), drawn with the above this frame
	float       stains[VRX_STAIN_MARKS][12];
	int         numStains, stainFrame;
} vrgEnt_t;

typedef struct {
	int         start;
	int         body;
	refEntity_t ref;
	float       pose[VRG_POSE_FLOATS];
	int         numBones;
	vec3_t      center;         // its middle in the model's space
	vec3_t      pos, vel, axis[3], spinAxis;
	float       spin;
	qboolean    resting;
	int         nextDrip;
} vrgPiece_t;

static vrgModel_t vrgModels[32];
static vrgHeadName_t vrgHeadNames[64];
static const float vrgIdentity[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
static vrgEnt_t vrgEnts[VRG_MAX_ENTS];
static vrgPiece_t vrgPieces[VRG_MAX_PIECES];
static int vrgPieceNext;
static qhandle_t vrgFleshShader, vrgInsideShader;
static qhandle_t vrgBloodShader[VRG_BLOOD_SETS];     // Soldier of Fortune 2's splash, the soak, the run
static qhandle_t vrgAtlasShader;                    // all of them in one (0: not to be had)
static vmCvar_t vr_goreCell, vr_goreDebug, vr_bloodAtlas;

static qboolean VRG_On( void ) {
	return CG_VRX_Gore( VRX_VOXEL );
}

/*
=================
CG_VRGore_Init / CG_VRGore_Shutdown
=================
*/
void CG_VRGore_Init( void ) {
	Gore_Shutdown();
	memset( vrgModels, 0, sizeof( vrgModels ) );
	// (vrgHeadNames stay: heads are registered before this, and again only for new clients)
	memset( vrgEnts, 0, sizeof( vrgEnts ) );
	memset( vrgPieces, 0, sizeof( vrgPieces ) );
	trap_Cvar_Register( &vr_goreCell, "vr_goreVoxel", "0.4", CVAR_LATCH );   // voxel size, units (1 cm)
	trap_Cvar_Register( &vr_goreDebug, "vr_goreDebug", "0", 0 );
	vrgFleshShader = trap_R_RegisterShader( "vr_goreflesh" );
	vrgInsideShader = trap_R_RegisterShader( "vr_goreinside" );
	vrgBloodShader[0] = trap_R_RegisterShader( "vr_bodywound" );
	vrgBloodShader[1] = trap_R_RegisterShader( "vr_gorebloodsoak" );
	vrgBloodShader[2] = trap_R_RegisterShader( "vr_gorebloodrun" );
	vrgBloodShader[3] = trap_R_RegisterShader( "vr_gorebloodtrail" );
	if ( !vrgBloodShader[3] ) {
		vrgBloodShader[3] = vrgBloodShader[2];      // (an older z_vr_voxelgore.pk3)
	}
	vrgBloodShader[4] = vrgBloodShader[5] = vrgBloodShader[6] = vrgBloodShader[3];
	// every kind of blood as one draw: the pictures in one atlas (z_vr_voxelgore.pk3), and a
	// renderer that takes a picture for each mark (tr_init.c r_vrDecals)
	{
		char buf[16];
		trap_Cvar_Register( &vr_bloodAtlas, "vr_bloodAtlas", "1", 0 );     // (0: the old way, to compare)
		trap_Cvar_VariableStringBuffer( "r_vrDecals", buf, sizeof( buf ) );
		vrgAtlasShader = atoi( buf ) >= 2 ? trap_R_RegisterShader( "vr_gorebloodatlas" ) : 0;
	}
}

qhandle_t CG_VRGore_BloodAtlas( void ) {
	trap_Cvar_Update( &vr_bloodAtlas );
	return vr_bloodAtlas.integer ? vrgAtlasShader : 0;
}

void CG_VRGore_Shutdown( void ) {
	Gore_Shutdown();
}

/*
=================
VRG_Model

The model's voxel body: read from its .mds file and made in the pose of its first frame,
the first time a body of it is drawn.
=================
*/
static int VRG_Model( qhandle_t hModel, const char *path ) {
	static int parents[VRG_MAX_BONES], flags[VRG_MAX_BONES];
	static char names[VRG_MAX_BONES * 32];
	static float rest[VRG_MAX_BONES * 12];
	vrgModel_t *m = NULL;
	refEntity_t re;
	fileHandle_t f;
	int i, len, n, t;
	void *buf;
	float cell;

	for ( i = 0; i < (int)ARRAY_LEN( vrgModels ); i++ ) {
		if ( vrgModels[i].hModel == hModel ) {
			return vrgModels[i].gore;
		}
		if ( !vrgModels[i].hModel ) {
			m = &vrgModels[i];
			break;
		}
	}
	if ( !m || !path ) {
		return -1;
	}
	m->hModel = hModel;
	m->gore = -1;

	memset( &re, 0, sizeof( re ) );
	re.hModel = hModel;
	AxisClear( re.axis );
	AxisClear( re.torsoAxis );
	n = trap_R_GetSkeleton( &re, VRG_MAX_BONES, parents, flags, names, rest );
	if ( n <= 0 ) {
		return -1;
	}
	m->numBones = n;
	for ( i = 0; i < n; i++ ) {
		Q_strncpyz( m->names[i], names + i * 32, sizeof( m->names[i] ) );
	}
	len = trap_FS_FOpenFile( path, &f, FS_READ );
	if ( len <= 0 || !f ) {
		return -1;
	}
	buf = malloc( len );
	if ( !buf ) {
		trap_FS_FCloseFile( f );
		return -1;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	cell = vr_goreCell.value < 0.3f ? 0.3f : ( vr_goreCell.value > 2.0f ? 2.0f : vr_goreCell.value );
	t = trap_Milliseconds();
	m->gore = Gore_LoadModel( buf, len, rest, n, parents, flags, names, cell );
	free( buf );
	if ( vr_goreDebug.integer ) {
		CG_Printf( "vr gore: %s %s (%d ms)\n", path, m->gore > 0 ? "ready" : "FAILED", trap_Milliseconds() - t );
	}
	if ( m->gore <= 0 ) {
		m->gore = -1;
	}
	return m->gore;
}

/*
=================
CG_VRGore_NoteHead / VRG_HeadModel

A head model's voxel body (a skull inside): read from its file the first time one is shot.
=================
*/
void CG_VRGore_NoteHead( qhandle_t hModel, const char *path ) {
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( vrgHeadNames ); i++ ) {
		if ( !vrgHeadNames[i].hModel || vrgHeadNames[i].hModel == hModel ) {
			vrgHeadNames[i].hModel = hModel;
			Q_strncpyz( vrgHeadNames[i].path, path, sizeof( vrgHeadNames[i].path ) );
			return;
		}
	}
}

static int VRG_HeadModel( qhandle_t hModel ) {
	vrgModel_t *m = NULL;
	const char *path = NULL;
	char name[MAX_QPATH];
	fileHandle_t f = 0;
	int i, len = 0;
	void *buf;

	for ( i = 0; i < (int)ARRAY_LEN( vrgModels ); i++ ) {
		if ( vrgModels[i].hModel == hModel ) {
			return vrgModels[i].gore;
		}
		if ( !vrgModels[i].hModel ) {
			m = &vrgModels[i];
			break;
		}
	}
	for ( i = 0; i < (int)ARRAY_LEN( vrgHeadNames ) && vrgHeadNames[i].hModel; i++ ) {
		if ( vrgHeadNames[i].hModel == hModel ) {
			path = vrgHeadNames[i].path;
		}
	}
	if ( !m || !path ) {
		return -1;
	}
	m->hModel = hModel;
	m->gore = -1;
	m->numBones = 1;
	// the renderer takes an .mdc in place of an .md3 of the same name
	COM_StripExtension( path, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".mdc" );
	len = trap_FS_FOpenFile( name, &f, FS_READ );
	if ( len <= 0 || !f ) {
		len = trap_FS_FOpenFile( path, &f, FS_READ );
	}
	if ( len <= 0 || !f ) {
		return -1;
	}
	buf = malloc( len );
	if ( buf ) {
		trap_FS_Read( buf, len, f );
		m->gore = Gore_LoadHead( buf, len, 0.3f );      // 7.5 mm
		free( buf );
	}
	trap_FS_FCloseFile( f );
	if ( vr_goreDebug.integer ) {
		CG_Printf( "vr gore: head %s %s\n", path, m->gore > 0 ? "ready" : "FAILED" );
	}
	if ( m->gore <= 0 ) {
		m->gore = -1;
	}
	return m->gore;
}

static int VRG_ModelBones( qhandle_t hModel ) {
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( vrgModels ) && vrgModels[i].hModel; i++ ) {
		if ( vrgModels[i].hModel == hModel ) {
			return vrgModels[i].gore > 0 ? vrgModels[i].numBones : 0;
		}
	}
	return 0;
}

// the pose it was last drawn in (animated bodies' only worked out when it's needed)
static qboolean VRG_Pose( vrgEnt_t *e ) {
	static int parents[VRG_MAX_BONES], flags[VRG_MAX_BONES];
	static char names[VRG_MAX_BONES * 32];

	if ( !e->posed ) {
		if ( trap_R_GetSkeleton( &e->ref, VRG_MAX_BONES, parents, flags, names, e->pose ) != e->numBones ) {
			return qfalse;
		}
		e->posed = qtrue;
	}
	return qtrue;
}

static vrgEnt_t *VRG_Ent( int entityNum ) {
	int i;

	for ( i = 0; i < VRG_MAX_ENTS; i++ ) {
		if ( vrgEnts[i].lastSeen && vrgEnts[i].entityNum == entityNum ) {
			return &vrgEnts[i];
		}
	}
	return NULL;
}

static void VRG_FreeEnt( vrgEnt_t *e ) {
	if ( e->body ) {
		Gore_FreeBody( e->body );
	}
	if ( e->head ) {
		Gore_FreeBody( e->head );
	}
	memset( e, 0, sizeof( *e ) );
}

// a slot for a body seen now: a free one, else the one longest out of sight
static vrgEnt_t *VRG_NewEnt( int entityNum ) {
	vrgEnt_t *best = NULL;
	int i;

	for ( i = 0; i < VRG_MAX_ENTS; i++ ) {
		vrgEnt_t *e = &vrgEnts[i];
		if ( !e->lastSeen ) {
			best = e;
			break;
		}
		if ( !best || e->lastSeen < best->lastSeen ) {
			best = e;
		}
	}
	VRG_FreeEnt( best );
	best->entityNum = entityNum;
	return best;
}

// world to the model's space of a body drawn at origin with axis, and back
static void VRG_ToModel( const vrgEnt_t *e, const vec3_t world, vec3_t out, qboolean isDir ) {
	vec3_t d;
	int k;

	if ( isDir ) {
		VectorCopy( world, d );
	} else {
		VectorSubtract( world, e->origin, d );
	}
	for ( k = 0; k < 3; k++ ) {
		out[k] = DotProduct( d, e->axis[k] );
	}
}

static void VRG_ToWorld( const vec3_t origin, vec3_t axis[3], const vec3_t local, vec3_t out ) {
	int k;

	VectorCopy( origin, out );
	for ( k = 0; k < 3; k++ ) {
		VectorMA( out, local[k], axis[k], out );
	}
}

// which limb a piece that came off belongs to (its first bone), VRX_GB_* or 0 (fingers, bits)
static int VRG_PieceLimb( const vrgEnt_t *e, int bone ) {
	static const struct { const char *name; int limb; } parts[] = {
		{ "L Hand", VRX_GB_LARM }, { "L Forearm", VRX_GB_LARM }, { "L UpperArm", VRX_GB_LARM }, { "L Clavicle", VRX_GB_LARM },
		{ "R Hand", VRX_GB_RARM }, { "R Forearm", VRX_GB_RARM }, { "R UpperArm", VRX_GB_RARM }, { "R Clavicle", VRX_GB_RARM },
		{ "L Finger", VRX_GB_LARM }, { "R Finger", VRX_GB_RARM },     // (a hand's main bone can be a finger)
		{ "L Foot", VRX_GB_LLEG }, { "L Calf", VRX_GB_LLEG }, { "L Thigh", VRX_GB_LLEG }, { "L Toe", VRX_GB_LLEG },
		{ "R Foot", VRX_GB_RLEG }, { "R Calf", VRX_GB_RLEG }, { "R Thigh", VRX_GB_RLEG }, { "R Toe", VRX_GB_RLEG },
	};
	int m, i;

	if ( bone < 0 || bone >= e->numBones ) {
		return 0;
	}
	for ( m = 0; m < (int)ARRAY_LEN( vrgModels ); m++ ) {
		if ( vrgModels[m].hModel != e->hModel ) {
			continue;
		}
		for ( i = 0; i < (int)ARRAY_LEN( parts ); i++ ) {
			if ( strstr( vrgModels[m].names[bone], parts[i].name ) ) {
				return parts[i].limb;
			}
		}
	}
	return 0;
}

/*
=================
VRG_Sever

A body's parts shot clean off: each flies off as itself. Someone still alive who loses a hand,
an arm, a foot or a leg this way is told to the server (G_VRX_Severed_f): crippled, or down.
=================
*/
static void VRG_Sever( vrgEnt_t *e, const vec3_t shot ) {
	int pieces[4], bones[4], n, i, k, limbs = 0;
	float centres[4][3];

	if ( !e->body ) {
		return;
	}
	n = Gore_Sever( e->body, pieces, centres, bones, 4 );
	for ( i = 0; i < n; i++ ) {
		vrgPiece_t *p = &vrgPieces[vrgPieceNext];
		vrgPieceNext = ( vrgPieceNext + 1 ) % VRG_MAX_PIECES;
		if ( p->body ) {
			Gore_FreeBody( p->body );
		}
		memset( p, 0, sizeof( *p ) );
		p->body = pieces[i];
		p->numBones = e->numBones;
		memcpy( p->pose, e->pose, sizeof( float ) * 12 * e->numBones );
		Gore_RestToPose( p->body, p->pose, bones[i], centres[i], p->center );
		for ( k = 0; k < 3; k++ ) {
			VectorCopy( e->axis[k], p->axis[k] );
		}
		VRG_ToWorld( e->origin, e->axis, p->center, p->pos );
		memset( &p->ref, 0, sizeof( p->ref ) );
		p->ref.hModel = e->hModel;
		{
			centity_t *cent = &cg_entities[e->entityNum];
			p->ref.customSkin = cent->pe.legsRefEnt.customSkin;
			p->ref.renderfx = ( cent->pe.legsRefEnt.renderfx & ~( RF_THIRD_PERSON | RF_FIRST_PERSON ) ) | RF_VR_TWOSIDED;
		}
		if ( shot ) {
			VectorScale( shot, 150, p->vel );
		}
		p->vel[0] += crandom() * 60;
		p->vel[1] += crandom() * 60;
		p->vel[2] += 120 + random() * 120;
		VectorSet( p->spinAxis, crandom(), crandom(), crandom() );
		if ( VectorNormalize( p->spinAxis ) < 0.1f ) {
			VectorSet( p->spinAxis, 0, 0, 1 );
		}
		p->spin = 300 + random() * 400;
		p->start = cg.time;
		if ( cg_blood.integer ) {
			CG_AddBloodTrails( p->pos, p->vel, 60, 900, 4, 0.5f );
		}
		if ( vr_goreDebug.integer ) {
			CG_Printf( "vr gore: %d lost a piece (bone %d)\n", e->entityNum, bones[i] );
		}
		limbs |= VRG_PieceLimb( e, bones[i] );
	}
	if ( limbs && !e->dead && e->entityNum > 0 && e->entityNum < MAX_CLIENTS ) {
		trap_SendClientCommand( va( "vrsever %d %d", e->entityNum, limbs ) );
	}
}

// the blood round the hole a shot made going in, and coming out (sizes from its radii, metres)
static void VRG_AddSplat( vrgSplat_t *list, int *num, int *next, const goreShot_t *r, float rIn, float rOut ) {
	int k;

	for ( k = 0; k < 2; k++ ) {
		vrgSplat_t *s;
		if ( k == 1 && !r->exited ) {
			break;
		}
		s = &list[*next];
		*next = ( *next + 1 ) % VRG_MAX_SPLATS;
		if ( *num < VRG_MAX_SPLATS ) {
			( *num )++;
		}
		s->bone = r->bone;
		s->exit = k;
		VectorCopy( k ? r->restExit : r->restEntry, s->pos );
		VectorScale( r->restDir, k ? 1.0f : -1.0f, s->normal );
		// the rim: a splash a little bigger than the hole (inches, half its width)
		s->size = ( k ? rOut * 1.6f : rIn * 2.0f ) / 0.0254f;
		s->flip = ( rand() & 1 ) ? 1.0f : -1.0f;
		s->time = cg.time;
		s->nextDrip = cg.time + 200;
		s->flowTime = cg.time;
		s->numSegs = 0;
		s->nextMark = 0;
		memset( &s->vrb, 0, sizeof( s->vrb ) );
		s->heading = 0;
		s->joined = qfalse;
		s->snapped = qfalse;
		s->anchor.surf = -1;
		s->wait = 0;
		s->rate = ( k ? 1.3f : 1.0f ) * ( 0.6f + 0.7f * random() );    // more out of the exit
	}
}

// how far a round goes through flesh and how wide it cuts (metres), by its weapon's damage
static void VRG_Round( int weapon, float *energy, float *rIn, float *rOut ) {
	float dmg = 20;

	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && GetWeaponTableData( weapon )->playerDamage > 0 ) {
		dmg = GetWeaponTableData( weapon )->playerDamage;
	}
	*energy = 0.25f + dmg * 0.02f;
	*rIn = 0.009f + dmg * 0.0001f;
	if ( *rIn > 0.02f ) {
		*rIn = 0.02f;
	}
	*rOut = *rIn * 1.8f > 0.035f ? 0.035f : *rIn * 1.8f;
}

// a shot that passes through the head (drawn lately) carves the head
static qboolean VRG_HeadShot( vrgEnt_t *e, const vec3_t point, const vec3_t dir, int weapon ) {
	vec3_t p, d, rel, near;
	float c[3], energy, rIn, rOut, t;
	goreShot_t r;
	int model, k;

	if ( !e->hHead || cg.time - e->headSeen > 500 ) {
		return qfalse;
	}
	model = VRG_HeadModel( e->hHead );
	if ( model <= 0 ) {
		return qfalse;
	}
	// the path in the head's space, and how close it passes its middle
	VectorSubtract( point, e->headOrigin, rel );
	for ( k = 0; k < 3; k++ ) {
		p[k] = DotProduct( rel, e->headAxis[k] );
		d[k] = DotProduct( dir, e->headAxis[k] );
	}
	VectorNormalize( d );
	Gore_ModelCentre( model, c );
	VectorSubtract( c, p, rel );
	t = DotProduct( rel, d );
	VectorMA( p, t, d, near );
	if ( Distance( near, c ) > 6.0f || Distance( p, c ) > 14.0f ) {
		return qfalse;      // went by it, or hit somewhere else
	}
	if ( !e->head ) {
		e->head = Gore_NewBody( model );
		if ( !e->head ) {
			return qfalse;
		}
	}
	VRG_Round( weapon, &energy, &rIn, &rOut );
	if ( !Gore_Shot( e->head, vrgIdentity, near, d, rIn, rOut, energy, &r ) ) {
		return qfalse;
	}
	if ( vr_goreDebug.integer ) {
		CG_Printf( "vr gore: %d shot in the head: %d voxels, %d skull, %s\n", e->entityNum, r.cells, r.boneCells,
				   r.exited ? "out the other side" : "stayed in" );
	}
	VRG_AddSplat( e->headSplats, &e->numHeadSplats, &e->nextHeadSplat, &r, rIn, rOut );
	if ( r.exited && cg_blood.integer ) {
		vec3_t out;
		VRG_ToWorld( e->headOrigin, e->headAxis, r.exit, out );
		CG_AddBloodTrails( out, (float *)dir, 140, 600, 4, 0.5f );
	}
	return qtrue;
}

/*
=================
CG_VRGore_Shot

A bullet from a weapon (or -1) hit entityNum at point, travelling along dir. qtrue when the
voxel gore took it (so the old wound decals don't).
=================
*/
qboolean CG_VRGore_Shot( int entityNum, const vec3_t point, const vec3_t dir, int weapon ) {
	vrgEnt_t *e;
	vec3_t p, d;
	goreShot_t r;
	float energy, rIn, rOut;

	if ( !VRG_On() || entityNum < 0 || entityNum >= MAX_GENTITIES || ( cg.snap && entityNum == cg.snap->ps.clientNum ) ) {
		return qfalse;
	}
	e = VRG_Ent( entityNum );
	if ( !e ) {
		return qfalse;
	}
	if ( VRG_HeadShot( e, point, dir, weapon ) ) {
		return qtrue;
	}
	if ( !e->numBones || cg.time - e->lastSeen > 500 || !VRG_Pose( e ) ) {
		return qfalse;      // not drawn lately: nothing to take the shot into
	}
	if ( !e->body ) {
		int model = VRG_Model( e->hModel, NULL );
		if ( model <= 0 ) {
			return qfalse;
		}
		e->body = Gore_NewBody( model );
		if ( !e->body ) {
			// out of room: the wounded body longest out of sight gives up its wounds
			vrgEnt_t *old = NULL;
			int i;
			for ( i = 0; i < VRG_MAX_ENTS; i++ ) {
				if ( vrgEnts[i].body && &vrgEnts[i] != e && ( !old || vrgEnts[i].lastSeen < old->lastSeen ) ) {
					old = &vrgEnts[i];
				}
			}
			if ( old ) {
				Gore_FreeBody( old->body );
				old->body = 0;
			}
			e->body = Gore_NewBody( model );
			if ( !e->body ) {
				return qfalse;
			}
		}
	}
	VRG_Round( weapon, &energy, &rIn, &rOut );

	VRG_ToModel( e, point, p, qfalse );
	VectorNormalize2( dir, d );
	VRG_ToModel( e, d, d, qtrue );
	if ( !Gore_Shot( e->body, e->pose, p, d, rIn, rOut, energy, &r ) ) {
		if ( vr_goreDebug.integer ) {
			CG_Printf( "vr gore: shot at %d missed its body\n", entityNum );
		}
		return qfalse;
	}
	if ( vr_goreDebug.integer ) {
		CG_Printf( "vr gore: %d shot (weapon %d): %d voxels, %d bone, %.1f in deep, %s\n", entityNum, weapon,
				   r.cells, r.boneCells, r.length, r.exited ? "out the back" : "stayed in" );
	}
	VRG_AddSplat( e->splats, &e->numSplats, &e->nextSplat, &r, rIn, rOut );
	// an exit wound sprays out of the back
	if ( r.exited && cg_blood.integer ) {
		vec3_t out;
		VRG_ToWorld( e->origin, e->axis, r.exit, out );
		CG_AddBloodTrails( out, (float *)dir, 120, 500, 3, 0.4f );
	}
	VRG_Sever( e, dir );        // alive or dead: what's shot clean off comes off
	return qtrue;
}

/*
=================
CG_VRGore_Knife

A knife into entityNum at point, along dir. Someone on his feet gets a slit where it went in.
cut (someone down, or dead): an arm or a leg is cut across where it was hit, deeper with each
blow there, and comes off once it's cut through; anywhere else it's a slit as well.
=================
*/
qboolean CG_VRGore_Knife( int entityNum, const vec3_t point, const vec3_t dir, qboolean cut ) {
	vrgEnt_t *e;
	vec3_t p, d, out, back;
	goreShot_t r;
	float radius = 0;
	int through = 0;

	if ( !VRG_On() || entityNum < 0 || entityNum >= MAX_GENTITIES || ( cg.snap && entityNum == cg.snap->ps.clientNum ) ) {
		return qfalse;
	}
	e = VRG_Ent( entityNum );
	if ( !e || !e->numBones || cg.time - e->lastSeen > 500 || !VRG_Pose( e ) ) {
		return qfalse;
	}
	if ( !e->body ) {
		int model = VRG_Model( e->hModel, NULL );
		if ( model <= 0 || !( e->body = Gore_NewBody( model ) ) ) {
			return qfalse;
		}
	}
	VRG_ToModel( e, point, p, qfalse );
	VectorNormalize2( dir, d );
	VRG_ToModel( e, d, d, qtrue );
	if ( cut ) {
		int bone = Gore_NearBone( e->body, e->pose, p ), limb = VRG_PieceLimb( e, bone ), m;
		for ( m = 0; limb && m < (int)ARRAY_LEN( vrgModels ); m++ ) {
			if ( vrgModels[m].hModel == e->hModel && strstr( vrgModels[m].names[bone], "Clavicle" ) ) {
				limb = 0;       // (the shoulder: that's his chest)
			}
		}
		if ( limb ) {
			radius = ( limb & ( VRX_GB_LLEG | VRX_GB_RLEG ) ) ? 4.6f : 3.2f;
		}
	}
	if ( !Gore_Slice( e->body, e->pose, p, d, radius, radius > 0 ? 2.6f : ( cut ? 2.2f : 1.6f ), crandom() * 0.6f, &r, &through ) ) {
		if ( vr_goreDebug.integer ) {
			CG_Printf( "vr gore: knife at %d missed its body\n", entityNum );
		}
		return qfalse;
	}
	if ( vr_goreDebug.integer ) {
		CG_Printf( "vr gore: %d knifed: %d voxels%s%s\n", entityNum, r.cells, radius > 0 ? ", across a limb" : ", a slit",
				   through ? ", through" : "" );
	}
	VRG_AddSplat( e->splats, &e->numSplats, &e->nextSplat, &r, 0.008f, 0.008f );
	if ( cg_blood.integer ) {
		VRG_ToWorld( e->origin, e->axis, r.entry, out );
		VectorNegate( dir, back );
		CG_AddBloodTrails( out, back, 90, 400, 3, 0.4f );
	}
	if ( through ) {
		VRG_Sever( e, dir );
	}
	return qtrue;
}

/*
=================
VRG_BloodHoles

The blood round wounds as the renderer lays it on the skin (tr_animation.c R_VRX_Decals):
each wound's rim splash at once (Soldier of Fortune 2's), then a stain soaking out round it,
and blood running from it the way Overgrowth's does: a trail that creeps over the skin the
way down is now, so it runs down a standing man's chest, turns across it when he falls on
his side, and stops where the skin lies flat. What has run stays where it ran.
=================
*/

// a point and two directions of the rest pose as they are now (model space), through a bone.
// qtrue: the point is on the skin itself (its anchor), not just near it
static qboolean VRG_Place( const vrgEnt_t *e, qboolean head, int bone, const vec3_t pos, const vec3_t restNormal,
						   const vec3_t restDir, const goreAnchor_t *anchor, vec3_t at, vec3_t normal, vec3_t down ) {
	vec3_t tip, q;
	qboolean onSkin = qfalse;

	if ( !head && anchor && anchor->surf >= 0 && Gore_AnchorPose( e->body, e->pose, anchor, restDir, at, normal, down ) ) {
		// on the skin itself, wherever its bones have taken it (by one bone alone it slid about
		// on skin that hangs on several)
		onSkin = qtrue;
	} else if ( head ) {       // rigid, its own space
		VectorCopy( pos, at );
		VectorCopy( restNormal, normal );
		VectorCopy( restDir, down );
	} else {
		Gore_RestToPose( e->body, e->pose, bone, pos, at );
		VectorAdd( pos, restNormal, tip );
		Gore_RestToPose( e->body, e->pose, bone, tip, q );
		VectorSubtract( q, at, normal );
		VectorAdd( pos, restDir, tip );
		Gore_RestToPose( e->body, e->pose, bone, tip, q );
		VectorSubtract( q, at, down );
	}
	if ( VectorNormalize( normal ) < 0.01f ) {
		VectorClear( normal );
		return qfalse;
	}
	VectorMA( down, -DotProduct( down, normal ), normal, down );
	if ( VectorNormalize( down ) < 0.1f ) {
		PerpendicularVector( down, normal );
	}
	return onSkin;
}

// which way the world's down is at a point of the rest pose (in the rest pose's own space)
static void VRG_RestDown( const vrgEnt_t *e, qboolean head, vec3_t axis[3], int bone, const vec3_t pos, vec3_t out ) {
	vec3_t gm, at, q, col;
	int j, k;

	for ( k = 0; k < 3; k++ ) {
		gm[k] = -axis[k][2];
	}
	if ( head ) {
		VectorCopy( gm, out );
		return;
	}
	Gore_RestToPose( e->body, e->pose, bone, pos, at );
	for ( j = 0; j < 3; j++ ) {
		vec3_t tip;
		VectorCopy( pos, tip );
		tip[j] += 1.0f;
		Gore_RestToPose( e->body, e->pose, bone, tip, q );
		VectorSubtract( q, at, col );
		out[j] = DotProduct( gm, col );
	}
	VectorNormalize( out );
}

// where a wound's blood has got to (rest pose), and the bone that's on
static int VRG_TrailTip( const vrgSplat_t *s, vec3_t tip ) {
	const vrgSeg_t *seg;

	if ( !s->numSegs ) {
		VectorCopy( s->pos, tip );
		return s->bone;
	}
	seg = &s->segs[s->numSegs - 1];
	VectorMA( seg->pos, seg->len, seg->dir, tip );
	return seg->bone;
}

// the wounds' blood runs on a little: once a frame. It runs over the skin itself (the model's
// own, cg_gore.cpp Gore_RestSurface), each short stretch lying on the skin where it is, so it
// goes round a chest or a limb instead of off into the air, and it can only turn so far from
// one stretch to the next: a change of the way down (he fell, he was picked up) is a bend.
static void VRG_Flow( const vrgEnt_t *e, vrgSplat_t *list, int num, qboolean head, vec3_t axis[3] ) {
	int i, body = head ? e->head : e->body;

	for ( i = 0; i < num; i++ ) {
		vrgSplat_t *s = &list[i];
		vrgSeg_t *seg = s->numSegs ? &s->segs[s->numSegs - 1] : NULL;
		float dt = ( cg.time - s->flowTime ) * 0.001f, k = 1.0f - (float)( cg.time - s->time ) / VRG_BLEED_MS;
		float steep, speed, off;
		vec3_t tip, g, t, q, nq, side;
		goreAnchor_t anchor;
		int bone, o, j;

		s->flowTime = cg.time;
		if ( !s->snapped ) {
			// the wound itself: lying on the skin, not square to the shot (which smeared a
			// shot from the side along the body)
			s->snapped = qtrue;
			off = Gore_RestSurface( body, s->pos, s->normal, 0.15f, q, nq, &s->anchor );
			if ( off >= 0 && off < 3.0f ) {
				VectorCopy( nq, s->normal );
				VectorCopy( q, s->pos );
			} else {
				s->anchor.surf = -1;
			}
		}
		if ( dt <= 0 || k <= 0 || s->joined || cg.time < s->wait ) {
			continue;
		}
		if ( dt > 0.1f ) {
			dt = 0.1f;
		}
		bone = VRG_TrailTip( s, tip );
		VRG_RestDown( e, head, axis, bone, tip, g );
		VectorCopy( seg ? seg->normal : s->normal, nq );
		VectorMA( g, -DotProduct( g, nq ), nq, t );
		steep = VectorNormalize( t );
		if ( steep < 0.3f ) {
			continue;       // the skin lies flat there (facing up or down): it pools, or drips off
		}
		speed = VRG_RUN_SPEED * s->rate * k * k * steep;
		if ( seg && seg->len < VRG_SEG_LEN ) {
			seg->len += speed * dt;
			continue;
		}
		if ( s->numSegs >= VRG_TRAIL_SEGS ) {
			continue;
		}
		// the next stretch, from the skin nearest where the last one got to
		off = Gore_RestSurface( body, tip, nq, 0.25f, q, nq, &anchor );
		if ( off < 0 || off > 2.0f ) {
			if ( seg ) {
				s->joined = qtrue;      // it ran off the edge of him: it drips from there
				continue;
			}
			VectorCopy( tip, q );
			VectorCopy( s->normal, nq );
			anchor.surf = -1;
		}
		bone = head ? 0 : Gore_RestBone( e->body, q );
		VRG_RestDown( e, head, axis, bone, q, g );
		VectorMA( g, -DotProduct( g, nq ), nq, t );
		steep = VectorNormalize( t );
		if ( steep < 0.3f ) {
			s->wait = cg.time + 400;    // flat there: it waits (till he's moved)
			continue;
		}
		// it wanders a little from side to side as it goes, a slow drift that keeps going one
		// way for a while, the way blood finds its way over cloth
		s->heading = s->heading * 0.9f + 0.34f * crandom();
		s->heading = s->heading > 0.9f ? 0.9f : s->heading < -0.9f ? -0.9f : s->heading;
		CrossProduct( nq, t, side );
		VectorScale( t, cos( s->heading ), t );
		VectorMA( t, sin( s->heading ), side, t );
		VectorNormalize( t );
		if ( seg ) {
			// no sharper than this off the stretch before
			vec3_t prev, perp;
			float c;
			VectorMA( seg->dir, -DotProduct( seg->dir, nq ), nq, prev );
			if ( VectorNormalize( prev ) > 0.1f ) {
				c = DotProduct( prev, t );
				if ( c < cos( VRG_TURN ) ) {
					VectorMA( t, -c, prev, perp );
					if ( VectorNormalize( perp ) < 0.05f ) {        // right back the way it came: round one side
						CrossProduct( nq, prev, perp );
						VectorScale( perp, s->flip, perp );
					}
					VectorScale( prev, cos( VRG_TURN ), t );
					VectorMA( t, sin( VRG_TURN ), perp, t );
					VectorNormalize( t );
				}
			}
			// where another trail already runs (or this one, come back on itself) it joins that
			// one: no second line laid over or across the first
			for ( o = 0; o < num && !s->joined && s->numSegs >= 2; o++ ) {
				const vrgSplat_t *os = &list[o];
				int upTo = os == s ? s->numSegs - 6 : os->numSegs;
				for ( j = 0; j < upTo; j++ ) {
					const vrgSeg_t *oq = &os->segs[j];
					vec3_t rel;
					float along;
					if ( DotProduct( oq->normal, nq ) < 0.3f ) {
						continue;
					}
					VectorSubtract( q, oq->pos, rel );
					along = DotProduct( rel, oq->dir );
					along = along < 0 ? 0 : along > oq->len ? oq->len : along;
					VectorMA( rel, -along, oq->dir, rel );
					if ( VectorLengthSquared( rel ) < 0.5f * 0.5f ) {
						s->joined = qtrue;
						break;
					}
				}
			}
			if ( s->joined ) {
				continue;
			}
		}
		seg = &s->segs[s->numSegs++];
		seg->bone = bone;
		VectorCopy( q, seg->pos );
		VectorCopy( t, seg->dir );
		VectorCopy( nq, seg->normal );
		seg->anchor = anchor;
		seg->len = speed * dt;
	}
}

// set's marks (12 floats each) to out, at most max; how many. picture: with the atlas, which
// of its pictures they are (VRD_*; 0 without: the draw's own texture), and a mark known to
// lie on the skin says so (100 and up), which saves the renderer looking for the skin under it
static int VRG_BloodHoles( const vrgEnt_t *e, const vrgSplat_t *list, int num, int next, int set, qboolean head,
						   int picture, float *out, int max ) {
	int i, j, count = 0, skip = 0;
	float onSkin;

	for ( i = 0; i < num && count < max; i++ ) {
		// (the newest wound first: when there's more trail than gets drawn, the oldest goes without)
		const vrgSplat_t *s = &list[( next - 1 - i + 2 * VRG_MAX_SPLATS ) % VRG_MAX_SPLATS];
		vec3_t p, n, down;
		float age = (float)( cg.time - s->time ), size = s->size;
		float *h;

		if ( set <= 1 ) {
			vec3_t rDown;
			// (which way up the splash is: as he stood)
			VectorSet( rDown, 0, 0, -1 );
			VectorMA( rDown, -DotProduct( rDown, s->normal ), s->normal, rDown );
			if ( VectorNormalize( rDown ) < 0.2f ) {
				PerpendicularVector( rDown, s->normal );
			}
			onSkin = VRG_Place( e, head, s->bone, s->pos, s->normal, rDown, &s->anchor, p, n, down ) && picture ? 90.0f : 0;
			if ( VectorLength( n ) < 0.5f ) {
				continue;
			}
			if ( set == 1 ) {           // soaking out: from the rim's size to three times it
				float f = age / VRG_SOAK_MS;
				f = f > 1 ? 1 : ( f < 0 ? 0 : f );
				size *= 1.1f + 1.9f * sqrtf( f );
			}
			h = out + count * 12;
			VectorCopy( p, h );
			VectorCopy( n, h + 3 );
			VectorCopy( down, h + 6 );
			h[9] = size;
			h[10] = s->flip * ( picture + 1 );
			h[11] = 11.0f + onSkin;
			count++;
			continue;
		}
		// the run: its last stretch (set 2, ending in a drop), the ones before it (3 to 6, 32 each)
		size *= 0.55f;      // a trickle, narrower than the hole
		if ( size < 0.4f ) {
			size = 0.4f;
		}
		for ( j = 0; j < s->numSegs && count < max; j++ ) {
			const vrgSeg_t *seg = &s->segs[j];
			vec3_t mid;
			float len = seg->len, stretch, from = 0, wide;

			if ( ( set == 2 ) != ( j == s->numSegs - 1 ) ) {
				continue;
			}
			if ( set > 3 && skip++ < ( set - 3 ) * VRG_SET_MARKS ) {
				continue;
			}
			if ( j > 0 ) {
				from = -0.25f;      // (back into the one before, so a bend has no gap)
			}
			if ( set != 2 ) {
				len += 0.25f;       // (and on into the next)
			}
			len -= from;
			if ( len < 0.2f ) {
				continue;
			}
			// (the textures are clear at their ends, so nothing of them smears on past the stretch:
			// the stretch is the part between, bloodtrail.tga's 1/8 to 7/8, bloodrun.tga's 1/8 to 9/10)
			if ( set == 2 ) {
				len /= 0.775f;
				mid[0] = from - 0.125f * len + 0.5f * len;
			} else {
				mid[0] = from + 0.5f * len;
				len /= 0.75f;
			}
			onSkin = VRG_Place( e, head, seg->bone, seg->pos, seg->normal, seg->dir, &seg->anchor, p, n, down ) && picture ? 90.0f : 0;
			if ( VectorLength( n ) < 0.5f ) {
				continue;
			}
			VectorMA( p, mid[0], down, p );
			wide = size * ( 1.0f - 0.45f * j / VRG_TRAIL_SEGS );     // thinner the further it has run
			stretch = len / ( 2.0f * wide );
			if ( stretch < 0.3f ) {
				stretch = 0.3f;
			}
			h = out + count * 12;
			VectorCopy( p, h );
			VectorCopy( n, h + 3 );
			VectorCopy( down, h + 6 );
			h[9] = wide;
			h[10] = s->flip * ( picture + 1 );
			h[11] = 10.0f + stretch + onSkin;       // (10 and up: the voxel gore's, stretched down)
			count++;
		}
	}
	return count;
}

// all of the wounds' blood as marks of the atlas, for one draw: the rim splashes, the soak
// over them, the runs' tips, their trails (not far off: a trickle couldn't be made out)
static int VRG_AllBlood( const vrgEnt_t *e, const vrgSplat_t *list, int num, int next, qboolean head, qboolean trails,
						 float *out, int max ) {
	static const int picture[4] = { VRD_RIM, VRD_SOAK, VRD_RUN, VRD_TRAIL };
	int set, count = 0;

	for ( set = 1; set < ( trails ? 4 : 2 ); set++ ) {     // (not 0, the rim: no picture of a hole round the real one, just the soak)
		int room = max - count;
		if ( set == 3 && room > VRG_TRAIL_MARKS ) {
			room = VRG_TRAIL_MARKS;
		}
		count += VRG_BloodHoles( e, list, num, next, set, head, picture[set], out + count * 12, room );
	}
	return count;
}

#if 0     // (no longer used: marks only where real drops land, VRX_SplatAdd)
// vr_bloodFX: a wound pumping a stream marks his own clothes under it as it does, a streak
// down the cloth or a scatter of drops, a few to a wound: on the skin (the model's own) a
// little way down from the wound as he is now, and off to one side or the other
static void VRG_StreamMark( const vrgEnt_t *e, vrgSplat_t *s, const vec3_t origin, vec3_t axis[3] ) {
	vec3_t g, t, side, target, q, nq, p, n, down, w, wn, wd;
	float along, off, strength = 1.0f - ( cg.time - s->time ) / 5000.0f;
	goreAnchor_t anchor;
	qboolean streak = ( rand() & 3 ) != 0;
	int bone, k;

	if ( !s->nextMark ) {
		s->nextMark = cg.time + 250 + rand() % 400;    // (the first once it has had time to fall)
		return;
	}
	s->nextMark = cg.time + 700 + rand() % 800;
	if ( !s->snapped ) {
		return;
	}
	VRG_RestDown( e, qfalse, axis, s->bone, s->pos, g );
	VectorMA( g, -DotProduct( g, s->normal ), s->normal, t );
	if ( VectorNormalize( t ) < 0.35f ) {
		return;     // the skin lies flat there: nothing runs or falls down it
	}
	CrossProduct( s->normal, t, side );
	along = 1.2f + 7.0f * random() * random();
	VectorMA( s->pos, along, t, target );
	VectorMA( target, crandom() * ( 0.5f + 0.22f * along ), side, target );
	off = Gore_RestSurface( e->body, target, s->normal, 0.3f, q, nq, &anchor );
	if ( off < 0 || off > 1.6f ) {
		return;     // off the edge of him
	}
	// which way is down the cloth there, as he is now
	bone = Gore_RestBone( e->body, q );
	VRG_RestDown( e, qfalse, axis, bone, q, g );
	VectorMA( g, -DotProduct( g, nq ), nq, t );
	if ( VectorNormalize( t ) < 0.3f ) {
		streak = qfalse;
		PerpendicularVector( t, nq );
	} else {
		// (never ruled: each a little off true)
		vec3_t s2;
		float ang = crandom() * 0.3f;
		CrossProduct( nq, t, s2 );
		VectorScale( t, cos( ang ), t );
		VectorMA( t, sin( ang ), s2, t );
	}
	VRG_Place( e, qfalse, bone, q, nq, t, &anchor, p, n, down );
	if ( VectorLength( n ) < 0.5f ) {
		return;
	}
	VRG_ToWorld( origin, axis, p, w );
	VectorClear( wn );
	VectorClear( wd );
	for ( k = 0; k < 3; k++ ) {
		VectorMA( wn, n[k], axis[k], wn );
		VectorMA( wd, down[k], axis[k], wd );
	}
	CG_VRX_BloodMark( e->entityNum, bone, w, wn, wd, ( 0.15f + 0.5f * random() ) * ( 0.4f + 0.6f * strength ) * ( s->rate > 1 ? 1 : s->rate ), streak );
}

#endif

// fresh wounds drip from where their blood has run to (world space)
static void VRG_Drips( vrgSplat_t *list, int num, const vec3_t origin, vec3_t axis[3], const vrgEnt_t *e, qboolean head ) {
	int i;

	if ( !cg_blood.integer ) {
		return;
	}
	for ( i = 0; i < num; i++ ) {
		vrgSplat_t *s = &list[i];
		vec3_t tip, p, n, down, w, dir, rDown;
		int bone, age = cg.time - s->time;
		if ( age > VRG_DRIP_MS || cg.time < s->nextDrip ) {
			continue;
		}
		if ( CG_VRB_On() ) {
			// vr_bloodFX: the wound pumps (a stream in the air with his heartbeat, for the first
			// seconds; out of the head, or where the round came out, a jet), then seeps and drips.
			// What falls back on him, or on anyone, marks where it lands (cg_vrextras.c VRX_SplatAdd)
			vec3_t wn, wd;
			int k;
			VectorSet( rDown, 0, 0, -1 );
			VRG_Place( e, head, s->bone, s->pos, s->normal, rDown, &s->anchor, p, n, down );
			if ( VectorLength( n ) < 0.5f ) {
				continue;
			}
			VRG_ToWorld( origin, axis, p, w );
			VectorClear( wn );
			for ( k = 0; k < 3; k++ ) {
				VectorMA( wn, n[k], axis[k], wn );
			}
			VectorSet( wd, 0, 0, -1 );
			VectorMA( wd, -DotProduct( wd, wn ), wn, wd );
			if ( VectorNormalize( wd ) < 0.1f ) {
				PerpendicularVector( wd, wn );
			}
			s->vrb.owner = e->entityNum;
			if ( age < 5000 ) {
				CG_VRB_Wound( &s->vrb, w, wn, wd, head || s->exit ? VRB_ARTERY : VRB_FLOW, ( 1.0f - age / 5000.0f ) * ( s->rate > 1 ? 1 : s->rate ) );
				// (what it throws marks his clothes only where its drops come down on him: no marks
				// made up under the wound on a timer, VRG_StreamMark, any more)
			} else {
				CG_VRB_Wound( &s->vrb, w, wn, wd, VRB_SEEP, 0.6f * ( 1.0f - (float)age / VRG_DRIP_MS ) );
			}
			continue;
		}
		s->nextDrip = cg.time + 180 + rand() % 320 + age / 12;      // slower as it stops
		bone = VRG_TrailTip( s, tip );
		VectorSet( rDown, 0, 0, -1 );
		VRG_Place( e, head, bone, tip, s->normal, rDown, NULL, p, n, down );
		VectorMA( p, 0.5f, n, p );
		VRG_ToWorld( origin, axis, p, w );
		VectorSet( dir, 0, 0, -1 );
		VectorMA( dir, 0.3f, n, dir );
		if ( CG_VRB_On() ) {       // vr_bloodFX: a drop that marks where it lands, on the floor or on someone
			VectorScale( dir, 10 + rand() % 20, dir );
			CG_VRB_Drip( w, dir, e->entityNum );
		} else {
			CG_AddBloodTrails( w, dir, 10 + rand() % 20, 700, 1, 0.3f );
		}
	}
}

/*
=================
VRG_AddBodyBlood

The body drawn again (with the same holes cut) for each kind of blood, the renderer laying
it on the skin round the holes, so it bends with the body.
=================
*/
static void VRG_AddBodyBlood( vrgEnt_t *e, const refEntity_t *body, int trailerEnd ) {
	vrgBits_t bits;
	refEntity_t ov;
	int set, count;
	float far;
	qhandle_t atlas = CG_VRGore_BloodAtlas();
	int numStains = atlas && e->stainFrame == cg.clientFrame ? e->numStains : 0;

	if ( ( !e->numSplats && !numStains ) || !cg_blood.integer ) {
		return;
	}
	if ( e->numSplats ) {
		VRG_Flow( e, e->splats, e->numSplats, qfalse, (vec3_t *)body->axis );
		VRG_Drips( e->splats, e->numSplats, body->origin, (vec3_t *)body->axis, e, qfalse );
	}
	// (the blood is the whole body drawn again: far off, where a trickle or a stain couldn't be
	// made out, it isn't)
	far = Distance( cg.refdef.vieworg, body->origin );
	if ( far > 1500.0f ) {
		return;
	}
	if ( atlas ) {
		// all of it in one draw: the wounds' own, then what landed on him
		float *b = e->splatDraw[0], *marks = b + trailerEnd + 2;

		count = VRG_AllBlood( e, e->splats, e->numSplats, e->nextSplat, qfalse, far <= 700.0f, marks, VRX_DECAL_MAX - numStains );
		memcpy( marks + count * 12, e->stains, sizeof( float ) * 12 * numStains );
		count += numStains;
		if ( !count ) {
			return;
		}
		memcpy( b, e->draw, sizeof( float ) * trailerEnd );     // the pose, a cut, the holes
		bits.i = VRX_WOUND_MAGIC;
		b[trailerEnd] = bits.f;
		b[trailerEnd + 1] = count;
		ov = *body;
		ov.customShader = atlas;
		ov.customSkin = 0;
		ov.ragdollBones = b;
		ov.ragdollMagic = RAGDOLL_MAGIC;
		trap_R_AddRefEntityToScene( &ov );
		return;
	}
	for ( set = 0; set < VRG_BLOOD_SETS; set++ ) {
		float *b = e->splatDraw[set];
		if ( !vrgBloodShader[set] || ( set >= 2 && far > 700.0f ) ) {
			continue;
		}
		memcpy( b, e->draw, sizeof( float ) * trailerEnd );     // the pose, a cut, the holes
		count = VRG_BloodHoles( e, e->splats, e->numSplats, e->nextSplat, set, qfalse, 0, b + trailerEnd + 2, VRG_SET_MARKS );
		if ( !count ) {
			continue;
		}
		bits.i = VRX_WOUND_MAGIC;
		b[trailerEnd] = bits.f;
		b[trailerEnd + 1] = count;
		ov = *body;
		ov.customShader = vrgBloodShader[set];
		ov.customSkin = 0;
		ov.ragdollBones = b;
		ov.ragdollMagic = RAGDOLL_MAGIC;
		trap_R_AddRefEntityToScene( &ov );
	}
}

/*
=================
CG_VRGore_Limbs

The server took limbs off this character (EV_VR_GORE VRG_LIMBS: a blast, a heavy round, or
an arm or a leg shot off someone still alive, who goes down screaming): each cut clean
through its own body at the shoulder / hip (high) or the elbow / knee, falling away as
itself, and a blast's shrapnel through what's left.
=================
*/
static int VRG_BoneIndex( const vrgEnt_t *e, const char *name ) {
	int i, m;

	for ( m = 0; m < (int)ARRAY_LEN( vrgModels ); m++ ) {
		if ( vrgModels[m].hModel == e->hModel ) {
			for ( i = 0; i < e->numBones; i++ ) {
				if ( strstr( vrgModels[m].names[i], name ) ) {
					return i;
				}
			}
		}
	}
	return -1;
}

qboolean CG_VRGore_Limbs( int entityNum, int limbs, int high, const vec3_t point, const vec3_t dir ) {
	static const struct { int bit; const char *high[2], *low[2]; float tHigh, tLow, radius; } cuts[4] = {
		{ VRX_GB_LARM, { "L UpperArm", "L Forearm" }, { "L Forearm", "L Hand" }, 0.25f, 0.35f, 3.2f },
		{ VRX_GB_RARM, { "R UpperArm", "R Forearm" }, { "R Forearm", "R Hand" }, 0.25f, 0.35f, 3.2f },
		{ VRX_GB_LLEG, { "L Thigh", "L Calf" }, { "L Calf", "L Foot" }, 0.2f, 0.3f, 4.6f },
		{ VRX_GB_RLEG, { "R Thigh", "R Calf" }, { "R Calf", "R Foot" }, 0.2f, 0.3f, 4.6f },
	};
	vrgEnt_t *e;
	vec3_t d;
	int k, cut = 0;

	if ( !VRG_On() ) {
		return qfalse;
	}
	e = VRG_Ent( entityNum );
	if ( !e || !e->numBones || !VRG_Pose( e ) ) {
		return qtrue;       // (ours to do, nothing to do it to)
	}
	if ( !e->body ) {
		int model = VRG_Model( e->hModel, NULL );
		if ( model <= 0 || !( e->body = Gore_NewBody( model ) ) ) {
			return qtrue;
		}
	}
	VectorNormalize2( dir, d );
	for ( k = 0; k < 4; k++ ) {
		const char *const *pair;
		int a, b;
		if ( !( limbs & cuts[k].bit ) ) {
			continue;
		}
		pair = ( high & cuts[k].bit ) ? cuts[k].high : cuts[k].low;
		a = VRG_BoneIndex( e, pair[0] );
		b = VRG_BoneIndex( e, pair[1] );
		cut += Gore_Chop( e->body, a, b, ( high & cuts[k].bit ) ? cuts[k].tHigh : cuts[k].tLow, cuts[k].radius ) > 0;
	}
	// two or more at once is a blast: shrapnel through what's left
	if ( cut >= 2 ) {
		vec3_t c, p, q, s;
		VectorCopy( e->origin, c );
		c[2] += 8;
		for ( k = 0; k < 6; k++ ) {
			VectorSet( s, crandom(), crandom(), crandom() * 0.5f );
			VectorMA( d, 0.6f, s, s );
			VectorNormalize( s );
			VectorSet( q, crandom() * 6, crandom() * 6, crandom() * 14 );
			VectorAdd( c, q, p );
			CG_VRGore_Shot( entityNum, p, s, WP_MP40 );
		}
	}
	if ( cut ) {
		VRG_Sever( e, d );      // (alive or not: the server said so)
	}
	return qtrue;
}

/*
=================
CG_VRGore_Body

A character's body about to be drawn (the pose final): remembered for the shots that come,
and when it's wounded, drawn with its holes and its flesh.
=================
*/
static void VRG_DrawBody( vrgEnt_t *e, refEntity_t *body );
static void VRG_DrawHead( vrgEnt_t *e, refEntity_t *head );

// someone new in that entity: a clean body (cg_vrextras.c CG_VRX_NewLife)
void CG_VRGore_Clear( int entityNum ) {
	vrgEnt_t *e = VRG_Ent( entityNum );

	if ( e ) {
		VRG_FreeEnt( e );
	}
}

/*
=================
CG_VRGore_TakeBlood

The blood that landed on a body (cg_vrextras.c VRX_DrawStains, called before CG_VRGore_Body
each frame): if this body is one of ours to draw with its wounds, its marks go in the same
draw as the wounds' own blood, and it says so.
=================
*/
qboolean CG_VRGore_TakeBlood( int entityNum, const refEntity_t *body, qboolean dead, const float *marks, int count ) {
	vrgEnt_t *e;

	if ( !VRG_On() || !cg_blood.integer || !CG_VRGore_BloodAtlas() || ( cg.snap && entityNum == cg.snap->ps.clientNum ) ) {
		return qfalse;
	}
	e = VRG_Ent( entityNum );
	// (as CG_VRGore_Body will find him: wounded, and still the same one)
	if ( !e || !e->body || e->hModel != body->hModel || ( e->dead && !dead ) || cg.time - e->lastSeen > 30000 || e->numBones <= 0 ) {
		return qfalse;
	}
	count = count > VRX_STAIN_MARKS ? VRX_STAIN_MARKS : count;
	memcpy( e->stains, marks, sizeof( float ) * 12 * count );
	e->numStains = count;
	e->stainFrame = cg.clientFrame;
	return qtrue;
}

void CG_VRGore_Body( centity_t *cent, refEntity_t *body, const char *modelName ) {
	int num = cent->currentState.number, n, k;
	qboolean dead = ( cent->currentState.eFlags & EF_DEAD ) != 0;
	vrgEnt_t *e;

	if ( !VRG_On() || !body->hModel || ( cg.snap && num == cg.snap->ps.clientNum ) || !modelName || !modelName[0] ) {
		return;
	}
	e = VRG_Ent( num );
	// someone new in that entity (a respawn, another character): a clean body
	if ( e && ( e->hModel != body->hModel || ( e->dead && !dead ) || cg.time - e->lastSeen > 30000 ) ) {
		VRG_FreeEnt( e );
		e = NULL;
	}
	if ( !e ) {
		if ( VRG_Model( body->hModel, va( "models/players/%s/body.mds", modelName ) ) <= 0 ) {
			return;
		}
		e = VRG_NewEnt( num );
		e->hModel = body->hModel;
	}
	e->lastSeen = cg.time ? cg.time : 1;
	VectorCopy( body->origin, e->origin );
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( body->axis[k], e->axis[k] );
	}

	// the pose it's drawn in: a ragdoll's (or another pose of ours) as it is, the animation's
	// left to the renderer until a shot or the wounds need it
	n = e->numBones = VRG_ModelBones( body->hModel );
	if ( n <= 0 ) {
		return;
	}
	e->ref = *body;
	e->posed = qfalse;
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		memcpy( e->pose, body->ragdollBones, sizeof( float ) * 12 * n );
		e->posed = qtrue;
	}

	// it just died: what was shot through falls off
	if ( dead && !e->dead && e->body && VRG_Pose( e ) ) {
		VRG_Sever( e, NULL );
	}
	e->dead = dead;
	VRG_DrawBody( e, body );
}

/*
=================
VRG_DrawBody

A body with wounds, about to be drawn: its holes cut
in the skin, its flesh, the inside of it, the blood.
=================
*/
static void VRG_DrawBody( vrgEnt_t *e, refEntity_t *body ) {
	const vrGoreDraw_t *d;
	vrgBits_t bits;
	int n = e->numBones, at;

	if ( !e->body || !VRG_Pose( e ) ) {
		return;
	}
	{
		static const float white[3] = { 255, 255, 255 }, none[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
		d = Gore_Draw( e->body, e->pose, white, none, up );
	}
	if ( !d ) {
		return;
	}
	// the pose, any cut it had (cg_vrextras.c), then ours
	memcpy( e->draw, e->pose, sizeof( float ) * 12 * n );
	at = n * 12;
	if ( body->ragdollBones && body->ragdollMagic == RAGDOLL_MAGIC ) {
		bits.f = body->ragdollBones[at];
		if ( bits.i == VRX_CUT_MAGIC ) {
			memcpy( e->draw + at, body->ragdollBones + at, sizeof( float ) * VRX_CUT_FLOATS );
			at += VRX_CUT_FLOATS;
		}
	}
	bits.i = VRG_MAGIC;
	e->draw[at] = bits.f;
	{
		unsigned long long a = (unsigned long long)(uintptr_t)d;
		bits.u = (unsigned)( a & 0xffffffffu );
		e->draw[at + 1] = bits.f;
		bits.u = (unsigned)( a >> 32 );
		e->draw[at + 2] = bits.f;
	}
	CG_VRX_OverlayTrailer( e->entityNum, e->draw + at, VRG_TRAILER_FLOATS );    // (the holes and stains drawn over it: cut as it is)
	body->ragdollBones = e->draw;
	body->ragdollMagic = RAGDOLL_MAGIC;
	body->reFlags |= REFLAG_FULL_LOD;
	// drawn double sided: through a hole the inside of the uniform and the skin shows, where the
	// wound walls don't cover it, never the world behind
	body->renderfx |= RF_VR_TWOSIDED;

	// the wound walls: the body again, the renderer drawing its flesh instead
	if ( d->numFleshIndexes > 0 && vrgFleshShader ) {
		refEntity_t fl = *body;
		fl.customShader = vrgFleshShader;
		fl.customSkin = 0;
		trap_R_AddRefEntityToScene( &fl );
	}
	VRG_AddBodyBlood( e, body, at + VRG_TRAILER_FLOATS );
}

/*
=================
CG_VRGore_Head

A character's head about to be drawn: remembered for the shots that come, and drawn with
its holes and its flesh once it has some.
=================
*/
void CG_VRGore_Head( centity_t *cent, refEntity_t *head ) {
	vrgEnt_t *e;
	int k;

	if ( !VRG_On() || !head->hModel ) {
		return;
	}
	e = VRG_Ent( cent->currentState.number );
	if ( !e ) {
		return;
	}
	if ( e->hHead != head->hModel && e->head ) {
		Gore_FreeBody( e->head );
		e->head = 0;
	}
	e->hHead = head->hModel;
	e->headSeen = cg.time ? cg.time : 1;
	e->headRef = *head;
	e->hasHeadRef = qtrue;
	VectorCopy( head->origin, e->headOrigin );
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( head->axis[k], e->headAxis[k] );
	}
	VRG_DrawHead( e, head );
}

static void VRG_DrawHead( vrgEnt_t *e, refEntity_t *head ) {
	const vrGoreDraw_t *d;
	unsigned long long a;
	union { float f; unsigned u; } bits;

	if ( !e->head || ( head->ragdollBones && head->ragdollMagic ) ) {
		return;     // unhurt, or clipped already (a cut off head's neck)
	}
	{
		static const float white[3] = { 255, 255, 255 }, none[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
		d = Gore_Draw( e->head, vrgIdentity, white, none, up );
	}
	if ( !d ) {
		return;
	}
	a = (unsigned long long)(uintptr_t)d;
	bits.u = (unsigned)( a & 0xffffffffu );
	e->headDraw[0] = bits.f;
	bits.u = (unsigned)( a >> 32 );
	e->headDraw[1] = bits.f;
	head->ragdollBones = e->headDraw;
	head->ragdollMagic = VRG_HEAD_MAGIC;
	if ( d->numFleshIndexes > 0 && vrgFleshShader ) {
		refEntity_t fl = *head;
		fl.customShader = vrgFleshShader;
		fl.customSkin = 0;
		trap_R_AddRefEntityToScene( &fl );
		// and the inside of the skin, wherever the holes show more than the wound walls
		if ( vrgInsideShader ) {
			fl.customShader = vrgInsideShader;
			trap_R_AddRefEntityToScene( &fl );
		}
	}
	// the blood round its holes, laid on the head as on a body
	if ( e->numHeadSplats && cg_blood.integer ) {
		int set, count;
		qhandle_t atlas = CG_VRGore_BloodAtlas();
		VRG_Flow( e, e->headSplats, e->numHeadSplats, qtrue, (vec3_t *)head->axis );
		VRG_Drips( e->headSplats, e->numHeadSplats, head->origin, (vec3_t *)head->axis, e, qtrue );
		if ( atlas ) {      // in one draw
			float *b = e->headSplatDraw[0];
			refEntity_t ov;
			count = VRG_AllBlood( e, e->headSplats, e->numHeadSplats, e->nextHeadSplat, qtrue, qtrue, b + 4, VRG_HEAD_MARKS );
			if ( count ) {
				b[0] = e->headDraw[0];
				b[1] = e->headDraw[1];
				bits.u = VRX_WOUND_MAGIC;
				b[2] = bits.f;
				b[3] = count;
				ov = *head;
				ov.customShader = atlas;
				ov.customSkin = 0;
				ov.ragdollBones = b;
				trap_R_AddRefEntityToScene( &ov );
			}
			return;
		}
		for ( set = 0; set < VRG_BLOOD_SETS; set++ ) {
			float *b = e->headSplatDraw[set];
			refEntity_t ov;
			if ( !vrgBloodShader[set] ) {
				continue;
			}
			b[0] = e->headDraw[0];
			b[1] = e->headDraw[1];
			count = VRG_BloodHoles( e, e->headSplats, e->numHeadSplats, e->nextHeadSplat, set, qtrue, 0, b + 4, VRG_SET_MARKS );
			if ( !count ) {
				continue;
			}
			bits.u = VRX_WOUND_MAGIC;
			b[2] = bits.f;
			b[3] = count;
			ov = *head;
			ov.customShader = vrgBloodShader[set];
			ov.customSkin = 0;
			ov.ragdollBones = b;
			trap_R_AddRefEntityToScene( &ov );
		}
	}
}

/*
=================
CG_VRGore_AddPieces

Every frame: the parts shot off, falling, bouncing and lying where they land.
=================
*/
void CG_VRGore_AddPieces( void ) {
	static const float white[3] = { 255, 255, 255 }, none[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
	float dt = cg.frametime * 0.001f;
	vec3_t mins = { -2, -2, -2 }, maxs = { 2, 2, 2 }, end, dir;
	trace_t tr;
	int i, k;

	if ( dt > 0.1f ) {
		dt = 0.1f;
	}
	Gore_NewFrame();
	for ( i = 0; i < VRG_MAX_PIECES; i++ ) {
		vrgPiece_t *p = &vrgPieces[i];
		const vrGoreDraw_t *d;
		vrgBits_t bits;
		int at;

		if ( !p->start ) {
			continue;
		}
		if ( cg.time - p->start > VRG_PIECE_LIFE || cg.time < p->start || !VRG_On() ) {
			Gore_FreeBody( p->body );
			memset( p, 0, sizeof( *p ) );
			continue;
		}
		if ( !p->resting && dt > 0 ) {
			p->vel[2] -= 800 * dt;
			VectorMA( p->pos, dt, p->vel, end );
			CG_Trace( &tr, p->pos, mins, maxs, end, -1, CONTENTS_SOLID );
			VectorCopy( tr.endpos, p->pos );
			if ( tr.fraction < 1 ) {
				float into = DotProduct( p->vel, tr.plane.normal );
				VectorMA( p->vel, -1.6f * into, tr.plane.normal, p->vel );
				VectorScale( p->vel, 0.4f, p->vel );
				p->spin *= 0.4f;
				if ( into < -40 && cg_blood.integer ) {
					CG_ImpactMark( cgs.media.bloodDotShaders[rand() % 5], tr.endpos, tr.plane.normal, random() * 360, 1, 1, 1, 1, qfalse, 6 + random() * 4, qfalse, -1 );
				}
				if ( tr.plane.normal[2] > 0.7f && VectorLength( p->vel ) < 40 ) {
					p->resting = qtrue;
					p->spin = 0;
				}
			}
			if ( p->spin > 0 ) {
				for ( k = 0; k < 3; k++ ) {
					RotatePointAroundVector( dir, p->spinAxis, p->axis[k], p->spin * dt );
					VectorCopy( dir, p->axis[k] );
				}
			}
		}
		if ( cg.time - p->start < 3000 && cg.time >= p->nextDrip && cg_blood.integer ) {
			CG_AddBloodTrails( p->pos, p->axis[2], 30 + rand() % 30, 600, 1, 0.5f );
			p->nextDrip = cg.time + 150 + rand() % 200;
		}

		// the model's origin, from where its middle is
		VectorCopy( p->pos, p->ref.origin );
		for ( k = 0; k < 3; k++ ) {
			VectorMA( p->ref.origin, -p->center[k], p->axis[k], p->ref.origin );
			VectorCopy( p->axis[k], p->ref.axis[k] );
		}
		VectorCopy( p->ref.origin, p->ref.oldorigin );
		VectorCopy( p->pos, p->ref.lightingOrigin );
		p->ref.renderfx |= RF_LIGHTING_ORIGIN;
		p->ref.reFlags |= REFLAG_FULL_LOD;

		d = Gore_Draw( p->body, p->pose, white, none, up );
		if ( !d ) {
			continue;
		}
		at = p->numBones * 12;
		bits.i = VRG_MAGIC;
		p->pose[at] = bits.f;
		{
			unsigned long long a = (unsigned long long)(uintptr_t)d;
			bits.u = (unsigned)( a & 0xffffffffu );
			p->pose[at + 1] = bits.f;
			bits.u = (unsigned)( a >> 32 );
			p->pose[at + 2] = bits.f;
		}
		p->ref.ragdollBones = p->pose;
		p->ref.ragdollMagic = RAGDOLL_MAGIC;
		trap_R_AddRefEntityToScene( &p->ref );
		if ( d->numFleshIndexes > 0 && vrgFleshShader ) {
			refEntity_t fl = p->ref;
			fl.customShader = vrgFleshShader;
			fl.customSkin = 0;
			trap_R_AddRefEntityToScene( &fl );
		}
	}
}

/*
=================
CG_VRGore_Demo_f

"vrgoredemo head|chest|limbs" (test aid): the character nearest the view, shot from the
eye by a rifle: once through the middle of the head, three times through the chest, or
(a body) bursts across the left forearm and the right shin.
=================
*/
static qboolean VRG_BonePos( vrgEnt_t *e, const char *name, vec3_t out ) {
	int i, m;

	for ( m = 0; m < (int)ARRAY_LEN( vrgModels ); m++ ) {
		if ( vrgModels[m].hModel == e->hModel ) {
			break;
		}
	}
	if ( m == (int)ARRAY_LEN( vrgModels ) || !VRG_Pose( e ) ) {
		return qfalse;
	}
	for ( i = 0; i < e->numBones; i++ ) {
		if ( strstr( vrgModels[m].names[i], name ) ) {
			VRG_ToWorld( e->origin, e->axis, e->pose + i * 12 + 9, out );
			return qtrue;
		}
	}
	return qfalse;
}

static void VRG_DemoShot( vrgEnt_t *e, const vec3_t at ) {
	vec3_t dir;

	VectorSubtract( at, cg.refdef.vieworg, dir );
	VectorNormalize( dir );
	CG_VRGore_Shot( e->entityNum, at, dir, WP_MAUSER );
}

static void VRG_DemoBurst( vrgEnt_t *e, const char *from, const char *to ) {
	vec3_t a, b, mid, along, across, dir, p;
	int i;

	if ( !VRG_BonePos( e, from, a ) || !VRG_BonePos( e, to, b ) ) {
		CG_Printf( "vrgoredemo: no %s / %s\n", from, to );
		return;
	}
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( b, a, along );
	VectorNormalize( along );
	VectorSubtract( mid, cg.refdef.vieworg, dir );
	VectorNormalize( dir );
	CrossProduct( along, dir, across );
	VectorNormalize( across );
	for ( i = 0; i < 9; i++ ) {
		VectorMA( mid, ( i - 4 ) * 0.7f, across, p );
		VectorMA( p, ( ( i % 3 ) - 1 ) * 0.4f, along, p );
		VRG_DemoShot( e, p );
	}
}

void CG_VRGore_Demo_f( void ) {
	const char *what = CG_Argv( 1 );
	vrgEnt_t *e, *best = NULL;
	float d, bestD = 1e9f;
	vec3_t p;
	int i;

	for ( i = 0; i < VRG_MAX_ENTS; i++ ) {
		e = &vrgEnts[i];
		if ( !e->lastSeen || cg.time - e->lastSeen > 500 || !e->numBones ) {
			continue;
		}
		d = Distance( e->origin, cg.refdef.vieworg );
		if ( d < bestD ) {
			bestD = d;
			best = e;
		}
	}
	if ( !best ) {
		CG_Printf( "vrgoredemo: no one drawn\n" );
		return;
	}
	if ( !Q_stricmp( what, "head" ) ) {
		float c[3];
		int model = best->hHead ? VRG_HeadModel( best->hHead ) : -1;
		if ( model <= 0 ) {
			CG_Printf( "vrgoredemo: no head\n" );
			return;
		}
		Gore_ModelCentre( model, c );
		c[2] += 1.0f;
		VRG_ToWorld( best->headOrigin, best->headAxis, c, p );
		VRG_DemoShot( best, p );
	} else if ( !Q_stricmp( what, "chest" ) ) {
		vec3_t up;
		if ( !VRG_BonePos( best, "Spine2", p ) ) {
			return;
		}
		VectorCopy( best->axis[2], up );
		for ( i = 0; i < 3; i++ ) {
			vec3_t q;
			VectorMA( p, ( i - 1 ) * 0.6f, cg.refdef.viewaxis[1], q );
			VectorMA( q, ( i == 1 ) * 0.6f, up, q );
			VRG_DemoShot( best, q );
		}
	} else if ( !Q_stricmp( what, "limbs" ) ) {
		VRG_DemoBurst( best, "L Forearm", "L Hand" );
		VRG_DemoBurst( best, "R Calf", "R Foot" );
	}
	CG_Printf( "vrgoredemo %s on %d\n", what, best->entityNum );
}

/*
=================
CG_VRGore_Test_f

"vrgoretest [shots]" (test aid): the character nearest where the view looks shot through
along the view, a few rounds a little apart, as if by a rifle.
=================
*/
void CG_VRGore_Test_f( void ) {
	trace_t tr;
	vec3_t end, p, right, up;
	int shots = trap_Argc() > 1 ? atoi( CG_Argv( 1 ) ) : 1, i;

	VectorMA( cg.refdef.vieworg, 4096, cg.refdef.viewaxis[0], end );
	CG_Trace( &tr, cg.refdef.vieworg, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
	if ( tr.entityNum >= MAX_GENTITIES || cg_entities[tr.entityNum].currentState.eType != ET_PLAYER ) {
		CG_Printf( "vrgoretest: no one there\n" );
		return;
	}
	VectorCopy( cg.refdef.viewaxis[1], right );
	VectorCopy( cg.refdef.viewaxis[2], up );
	for ( i = 0; i < shots; i++ ) {
		VectorMA( tr.endpos, ( ( i % 5 ) - 2 ) * 0.8f, right, p );
		VectorMA( p, ( ( i / 5 ) % 3 - 1 ) * 0.8f, up, p );
		CG_Printf( "vrgoretest %d: %s\n", tr.entityNum, CG_VRGore_Shot( tr.entityNum, p, cg.refdef.viewaxis[0], WP_MAUSER ) ? "carved" : "missed" );
	}
}
