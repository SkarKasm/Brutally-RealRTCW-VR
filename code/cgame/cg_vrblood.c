// cg_vrblood.c -- RealRTCW-VR: blood as droplets (vr_bloodFX)
//
// The base game's blood is a few flat sprites tumbling out of a hit. This throws real
// droplets instead, drawn with RTCW-VR's own blood (vr_blooddrop, vr_bloodstream in
// z_vr_bloodfx.pk3, both cut from bodyblood.tga, so the colour is the same as on the bodies):
//
// - a hit: a fine mist sprayed out of the exit side and a little back towards the shooter,
//   and a few heavier drops that splash on the floor and the wall behind him;
// - a wound that bleeds (CG_VRB_Wound): an artery (the throat, the thigh, a stump) pumps a
//   thin stream in time with his heartbeat, drawn as one ribbon through its drops, so it
//   reads as a thin unbroken stream arcing down until it breaks up into drops as it falls;
//   a chest or belly wound pulses weakly; any wound drips, the drops leaving spots where
//   they land. The heart stops a few seconds after he dies;
// - blood that lands on someone (CG_VRB_Body: the bodies drawn this frame, as capsules round
//   their bones) stains his clothes there, by how much landed (CG_VRX_BloodOnBody): the mist
//   off a hit speckles whoever is next to him, a stream from his neck soaks down his front.

#include "cg_local.h"

#define VRB_MAX         2048
#define VRB_GRAVITY     800.0f
#define VRB_TRACE_MS    40          // each drop checks for the world this often
#define VRB_MAX_MARKS   40          // spots a second at most (marks share one pool)
#define VRB_MAX_BODIES  16
#define VRB_MAX_CAPS    48
#define VRB_BREAK       7.0f        // a stream's drops further apart than this: it has broken into drops

enum { VRB_MIST, VRB_GLOB, VRB_STREAM, VRB_DRIP };

typedef struct {
	vec3_t  pos, vel, traced, last; // traced: where it was at its last world check; last: the frame before
	int     born, life, lastTrace;
	float   size, stretch, drag, alpha, amount;
	int     kind, shader;
	qboolean mark;                  // leaves a spot where it lands
	int     owner;                  // whose blood (-1 nobody's)
	qboolean clear;                 // out of its owner's body (before that it can't land on him)
	int     prev, prevBorn;         // a stream: the drop before it, joined to it by the ribbon
} vrDrop_t;

typedef struct {
	int     entityNum, time, num;
	vec3_t  mins, maxs;
	vec3_t  a[VRB_MAX_CAPS], b[VRB_MAX_CAPS];
	float   r[VRB_MAX_CAPS];
	int     bone[VRB_MAX_CAPS];
	vec3_t  mid[VRB_MAX_CAPS];      // a ball round each: its middle, how far it reaches
	float   reach[VRB_MAX_CAPS];
} vrbBody_t;

static vrDrop_t vrbDrops[VRB_MAX];
static int vrbNext, vrbLastUpdate, vrbMarkSecond, vrbMarksThisSecond;
static qhandle_t vrbDots[5], vrbDropShader, vrbStreamShader, vrbRopeShader;
static vmCvar_t vr_bloodFX;
static qboolean vrbRegistered;
static vrbBody_t vrbBodies[VRB_MAX_BODIES];
static vec3_t vrbLive[VRB_MAX];     // where the drops were at the last update (CG_VRB_Near)
static int vrbNumLive;

qboolean CG_VRB_On( void ) {
	if ( !vrbRegistered ) {
		trap_Cvar_Register( &vr_bloodFX, "vr_bloodFX", "1", CVAR_ARCHIVE );
		vrbRegistered = qtrue;
	}
	trap_Cvar_Update( &vr_bloodFX );
	return vr_bloodFX.integer && cg_blood.integer;
}

/*
=================
CG_VR_GoreQuality, CG_VRPerf_*

vr_goreLite: how much the new gore may cost a frame, for headsets that can't keep up.
CG_VR_GoreQuality gives 2 for High (vr_goreLite 0, the default: everything as it was made),
1 for Medium (vr_goreLite 1): half the mist off a hit, half as
many drops in the air at once, round 3D blood only up close, fewer drop marks on clothes,
blood layers drawn on bodies a little less far off, the physics given at most 4 ms a frame.
0 for Low (vr_goreLite 2): less again (and flat blood in the air), 3 ms of physics.

vr_perfStats 1: once a second, how long the gore and physics took a frame (console).
=================
*/
static vmCvar_t vr_goreQuality, vr_perfStats;
static qboolean vrqRegistered;

int CG_VR_GoreQuality( void ) {
	if ( !vrqRegistered ) {
		// (the cvar counts down from full detail, so one not set yet, as the menu sees it
		// before a level has loaded, reads as High)
		trap_Cvar_Register( &vr_goreQuality, "vr_goreLite", "0", CVAR_ARCHIVE );
		trap_Cvar_Register( &vr_perfStats, "vr_perfStats", "0", 0 );
		vrqRegistered = qtrue;
	}
	return vr_goreQuality.integer <= 0 ? 2 : ( vr_goreQuality.integer >= 2 ? 0 : 1 );
}

#ifdef _WIN32
// (kernel32's, declared here: windows.h doesn't mix with the game's headers)
__declspec( dllimport ) int __stdcall QueryPerformanceCounter( long long *count );
__declspec( dllimport ) int __stdcall QueryPerformanceFrequency( long long *freq );
#else
#include <time.h>
#endif

double CG_VRPerf_Now( void ) {      // milliseconds, fine grained
#ifdef _WIN32
	static long long freq;
	long long c;
	if ( !freq ) {
		QueryPerformanceFrequency( &freq );
		if ( !freq ) {
			freq = 1;
		}
	}
	QueryPerformanceCounter( &c );
	return (double)c * 1000.0 / (double)freq;
#else
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

static double vrpSum[VRP_NUM], vrpMax[VRP_NUM], vrpFrame[VRP_NUM];
static int vrpCount[VRP_NUM], vrpFrames, vrpSecond;

void CG_VRPerf_Add( int what, double ms ) {
	if ( what < 0 || what >= VRP_NUM ) {
		return;
	}
	vrpSum[what] += ms;
	vrpFrame[what] += ms;
	vrpCount[what]++;
}

// once a frame (the first eye)
static void VRP_Frame( void ) {
	static const char *names[VRP_NUM] = { "physics", "gore shots", "gore meshes", "blood drops", "body blood" };
	int i;

	CG_VR_GoreQuality();
	trap_Cvar_Update( &vr_goreQuality );
	trap_Cvar_Update( &vr_perfStats );
	for ( i = 0; i < VRP_NUM; i++ ) {
		if ( vrpFrame[i] > vrpMax[i] ) {
			vrpMax[i] = vrpFrame[i];
		}
		vrpFrame[i] = 0;
	}
	vrpFrames++;
	if ( cg.time / 1000 == vrpSecond ) {
		return;
	}
	vrpSecond = cg.time / 1000;
	if ( vr_perfStats.integer && vrpFrames > 0 ) {
		char line[512];
		int live = 0;
		Com_sprintf( line, sizeof( line ), "perf (quality %d): %d frames", CG_VR_GoreQuality(), vrpFrames );
		for ( i = 0; i < VRP_NUM; i++ ) {
			Q_strcat( line, sizeof( line ), va( " | %s %.2f ms/f (worst %.1f)", names[i], vrpSum[i] / vrpFrames, vrpMax[i] ) );
		}
		for ( i = 0; i < VRB_MAX; i++ ) {
			live += vrbDrops[i].life != 0;
		}
		Q_strcat( line, sizeof( line ), va( " | %d drops\n", live ) );
		CG_Printf( "%s", line );
	}
	for ( i = 0; i < VRP_NUM; i++ ) {
		vrpSum[i] = vrpMax[i] = 0;
		vrpCount[i] = 0;
	}
	vrpFrames = 0;
}

static void VRB_Shaders( void ) {
	int k;

	if ( !vrbDots[0] ) {
		for ( k = 0; k < 5; k++ ) {
			vrbDots[k] = trap_R_RegisterShader( va( "blood_dot%i", k + 1 ) );
		}
		vrbDropShader = trap_R_RegisterShader( "vr_blooddrop" );        // (0 without z_vr_bloodfx.pk3: the dots)
		vrbStreamShader = trap_R_RegisterShader( "vr_bloodstream" );
		vrbRopeShader = trap_R_RegisterShader( "vr_bloodrope" );    // (0 with an older z_vr_voxelgore.pk3: the flat ribbon)
	}
}

static vrDrop_t *VRB_New( int kind, const vec3_t pos, const vec3_t vel, float size, int life, int owner ) {
	vrDrop_t *d = &vrbDrops[vrbNext];

	{
		// (full: the oldest goes. vr_goreLite: fewer in the air at once)
		int q = CG_VR_GoreQuality(), most = q >= 2 ? VRB_MAX : ( q == 1 ? VRB_MAX / 2 : VRB_MAX / 4 );
		vrbNext = vrbNext + 1 >= most ? 0 : vrbNext + 1;
	}
	memset( d, 0, sizeof( *d ) );
	d->kind = kind;
	VectorCopy( pos, d->pos );
	VectorCopy( pos, d->traced );
	VectorCopy( pos, d->last );
	VectorCopy( vel, d->vel );
	d->born = d->lastTrace = cg.time;
	d->life = life;
	d->size = size;
	d->alpha = 1.0f;
	d->shader = rand() % 5;
	d->owner = owner;
	d->clear = owner < 0;
	d->prev = -1;
	switch ( kind ) {
	case VRB_MIST:   d->stretch = 0.02f; d->drag = 3.0f; d->alpha = 0.8f; d->amount = 0.6f * size * size; break;
	case VRB_GLOB:   d->stretch = 0.025f; d->drag = 0.6f; d->mark = qtrue; d->amount = size * size; break;
	case VRB_STREAM: d->stretch = 0.03f; d->drag = 0.3f; d->mark = ( rand() % 5 ) == 0; d->amount = 0.5f * size * size; break;
	default:         d->stretch = 0.035f; d->drag = 0.2f; d->mark = qtrue; d->amount = size * size; break;
	}
	return d;
}

// a spread direction round dir
static void VRB_Spread( const vec3_t dir, float spread, vec3_t out ) {
	out[0] = dir[0] + crandom() * spread;
	out[1] = dir[1] + crandom() * spread;
	out[2] = dir[2] + crandom() * spread;
	VectorNormalize( out );
}

/*
=================
CG_VRB_Impact

A bullet into flesh at point, flying along dir.
=================
*/
void CG_VRB_Impact( const vec3_t point, const vec3_t dir, int entityNum ) {
	vec3_t d, back, v;
	int i, n;

	if ( !CG_VRB_On() ) {
		return;
	}
	VectorNormalize2( dir, d );
	VectorNegate( d, back );
	n = 34 + rand() % 14;
	if ( CG_VR_GoreQuality() < 2 ) {
		n = CG_VR_GoreQuality() == 1 ? n / 2 : n / 3;
	}
	for ( i = 0; i < n; i++ ) {
		// most out of the far side, some back out of the hole
		qboolean out = i < n * 0.65f;
		VRB_Spread( out ? d : back, out ? 0.35f : 0.65f, v );
		v[2] += 0.15f;
		VectorScale( v, out ? 140 + random() * 260 : 50 + random() * 150, v );
		VRB_New( VRB_MIST, point, v, 0.14f + random() * 0.26f, 250 + rand() % 450, entityNum );
	}
	n = 4 + rand() % 4;
	if ( CG_VR_GoreQuality() == 0 ) {
		n = 2 + rand() % 2;
	}
	for ( i = 0; i < n; i++ ) {
		VRB_Spread( d, 0.45f, v );
		v[2] += 0.25f;
		VectorScale( v, 70 + random() * 200, v );
		VRB_New( VRB_GLOB, point, v, 0.5f + random() * 0.5f, 1500, entityNum );
	}
}

/*
=================
CG_VRB_Spurt

A spurt of blood thrown from origin along dir: one thin rope of it, its front going faster
than its tail so it draws out as it flies, then breaks into drops. Stands in for the game's
own blood trails (CG_AddBloodTrails: flat sprites as wide as a hand) wherever they're asked for.
=================
*/
void CG_VRB_Spurt( const vec3_t origin, const vec3_t dir, float speed, int count, float spread ) {
	vec3_t d, v;
	int i, n, last = -1, lastBorn = 0;

	VRB_Shaders();
	VRB_Spread( dir, spread * 0.5f, d );
	n = 5 + count * 2;
	n = n > 18 ? 18 : n;
	for ( i = 0; i < n; i++ ) {
		vrDrop_t *dr;
		float f = 1.0f - 0.55f * i / (float)n;      // (the tail slower)
		VRB_Spread( d, 0.04f, v );
		VectorScale( v, speed * f, v );
		dr = VRB_New( VRB_STREAM, origin, v, 0.38f + 0.12f * random(), 1100 + rand() % 500, -1 );
		dr->mark = ( i % 3 ) == 0;
		if ( last >= 0 ) {
			dr->prev = last;
			dr->prevBorn = lastBorn;
		}
		last = dr - vrbDrops;
		lastBorn = dr->born;
	}
}

// a drop falling off a wound (cg_vrgore.c): it marks the floor, or whoever it lands on
void CG_VRB_Drip( const vec3_t pos, const vec3_t vel, int owner ) {
	if ( !CG_VRB_On() ) {
		return;
	}
	VRB_Shaders();
	VRB_New( VRB_DRIP, pos, vel, 0.3f + random() * 0.3f, 2500, owner );
}

/*
=================
CG_VRB_Wound

Called each time a body with a bleeding wound is drawn (maybe more than once a frame: one
emission per frame). p: the wound, n: out of it, down: the way blood runs on the skin there.
kind: VRB_ARTERY, VRB_FLOW (chest, belly) or VRB_SEEP (drips only). strength 0..1.
=================
*/
void CG_VRB_Wound( vrbWound_t *b, const vec3_t p, const vec3_t n, const vec3_t down, int kind, float strength ) {
	int dt, i, count;
	float t, beat, speed;
	vec3_t v, side, at;

	if ( !CG_VRB_On() || strength <= 0.01f ) {
		return;
	}
	dt = b->last ? cg.time - b->last : 16;
	b->last = cg.time;
	if ( dt <= 0 ) {
		return;
	}
	dt = dt > 100 ? 100 : dt;
	if ( !b->phase ) {
		b->phase = 1 + rand() % 1000;
	}
	t = ( cg.time + b->phase ) * 0.001f;
	CrossProduct( n, down, side );

	if ( kind == VRB_ARTERY || kind == VRB_FLOW ) {
		// his heartbeat: a strong push, then a weak one, about 80 a minute
		float ph = fmod( t * 1.35f, 1.0f );
		beat = ph < 0.18f ? sin( ph / 0.18f * M_PI ) : ph < 0.38f ? 0.45f * sin( ( ph - 0.18f ) / 0.2f * M_PI ) : 0;
		if ( kind == VRB_ARTERY ) {
			// (a drop every frame or so: the ribbon through them is the stream)
			speed = ( 35 + 120 * beat ) * ( 0.4f + 0.6f * strength );
			b->acc += dt * 0.001f * ( 45 + 50 * beat ) * ( 0.3f + 0.7f * strength );
		} else {
			speed = ( 12 + 45 * beat ) * ( 0.5f + 0.5f * strength );
			b->acc += dt * 0.001f * 40 * beat * strength;
		}
		count = (int)b->acc;
		b->acc -= count;
		for ( i = 0; i < count && i < 8; i++ ) {
			float wob = sin( t * 7.0f ) * 0.15f;    // the jet wavers
			vrDrop_t *d;
			VectorMA( n, wob, side, v );
			VectorMA( v, 0.25f + 0.04f * crandom(), down, v );
			VectorNormalize( v );
			VectorScale( v, speed * ( 0.97f + 0.06f * random() ), v );
			VectorMA( p, 0.4f, n, at );
			d = VRB_New( VRB_STREAM, at, v, ( kind == VRB_ARTERY ? 0.4f + 0.25f * beat : 0.5f ) + random() * 0.08f, 1800, b->owner );
			// joined on to the drop before (one stream until it gives out between beats)
			if ( b->lastDropBorn && cg.time - b->lastDropBorn <= 90 ) {
				d->prev = b->lastDrop;
				d->prevBorn = b->lastDropBorn;
			}
			b->lastDrop = d - vrbDrops;
			b->lastDropBorn = d->born;
		}
	}
	// and it runs and drips off him
	if ( cg.time >= b->nextDrip ) {
		VectorMA( p, 0.6f + random() * 1.2f, down, at );
		VectorScale( down, 8, v );
		v[2] -= 15;
		VRB_New( VRB_DRIP, at, v, 0.5f + random() * 0.3f, 3000, b->owner );
		b->nextDrip = cg.time + (int)( ( kind == VRB_SEEP ? 450 : 220 ) / ( 0.3f + strength ) ) + rand() % 250;
	}
}

/*
=================
CG_VRB_Body

A body drawn this frame, as capsules round its bones (a to b, radius r; bone: the frame the
skin there moves with): blood that flies into one lands on him.
=================
*/
void CG_VRB_Body( int entityNum, int numCaps, vec3_t a[], vec3_t b[], const float *r, const int *bone ) {
	vrbBody_t *bd = NULL, *old = &vrbBodies[0];
	int i, k;

	for ( i = 0; i < VRB_MAX_BODIES; i++ ) {
		if ( vrbBodies[i].entityNum == entityNum && vrbBodies[i].time ) {
			bd = &vrbBodies[i];
			break;
		}
		if ( vrbBodies[i].time < old->time ) {
			old = &vrbBodies[i];
		}
	}
	if ( !bd ) {
		bd = old;
	}
	bd->entityNum = entityNum;
	bd->time = cg.time;
	bd->num = numCaps > VRB_MAX_CAPS ? VRB_MAX_CAPS : numCaps;
	ClearBounds( bd->mins, bd->maxs );
	for ( i = 0; i < bd->num; i++ ) {
		VectorCopy( a[i], bd->a[i] );
		VectorCopy( b[i], bd->b[i] );
		bd->r[i] = r[i];
		bd->bone[i] = bone[i];
		VectorAdd( a[i], b[i], bd->mid[i] );
		VectorScale( bd->mid[i], 0.5f, bd->mid[i] );
		bd->reach[i] = 0.5f * Distance( a[i], b[i] ) + r[i];
		for ( k = 0; k < 3; k++ ) {
			float lo = ( a[i][k] < b[i][k] ? a[i][k] : b[i][k] ) - r[i], hi = ( a[i][k] > b[i][k] ? a[i][k] : b[i][k] ) + r[i];
			bd->mins[k] = lo < bd->mins[k] ? lo : bd->mins[k];
			bd->maxs[k] = hi > bd->maxs[k] ? hi : bd->maxs[k];
		}
	}
}

/*
=================
CG_VRB_Near

Any blood in the air within radius of org (so a body there should be told of: CG_VRB_Body)
=================
*/
qboolean CG_VRB_Near( const vec3_t org, float radius ) {
	int i;
	float r2 = radius * radius;

	for ( i = 0; i < vrbNumLive; i++ ) {
		vec3_t d;
		VectorSubtract( vrbLive[i], org, d );
		if ( DotProduct( d, d ) < r2 ) {
			return qtrue;
		}
	}
	return qfalse;
}

// the closest two points of segments p0-p1 and q0-q1; the squared distance between them
static float VRB_SegSeg( const vec3_t p0, const vec3_t p1, const vec3_t q0, const vec3_t q1, vec3_t cp, vec3_t cq ) {
	vec3_t d1, d2, r, e;
	float a, b, c, f, ee, den, s, t;

	VectorSubtract( p1, p0, d1 );
	VectorSubtract( q1, q0, d2 );
	VectorSubtract( p0, q0, r );
	a = DotProduct( d1, d1 );
	ee = DotProduct( d2, d2 );
	f = DotProduct( d2, r );
	c = DotProduct( d1, r );
	b = DotProduct( d1, d2 );
	den = a * ee - b * b;
	s = den > 1e-6f ? ( b * f - c * ee ) / den : 0;
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	t = ee > 1e-6f ? ( b * s + f ) / ee : 0;
	if ( t < 0 ) {
		t = 0;
		s = a > 1e-6f ? -c / a : 0;
	} else if ( t > 1 ) {
		t = 1;
		s = a > 1e-6f ? ( b - c ) / a : 0;
	}
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	VectorMA( p0, s, d1, cp );
	VectorMA( q0, t, d2, cq );
	VectorSubtract( cp, cq, e );
	return DotProduct( e, e );
}

// did this drop, moving from d->last to d->pos, land on anyone drawn this frame?
static qboolean VRB_HitBodies( vrDrop_t *d ) {
	int i, j, k;
	vec3_t lo, hi, mid;
	float reach;

	for ( k = 0; k < 3; k++ ) {
		lo[k] = d->last[k] < d->pos[k] ? d->last[k] : d->pos[k];
		hi[k] = d->last[k] > d->pos[k] ? d->last[k] : d->pos[k];
		mid[k] = 0.5f * ( lo[k] + hi[k] );
	}
	reach = 0.5f * Distance( d->last, d->pos );
	for ( i = 0; i < VRB_MAX_BODIES; i++ ) {
		vrbBody_t *bd = &vrbBodies[i];
		float best = 1e9f;
		int bj = -1;
		vec3_t bp = { 0, 0, 0 }, bq = { 0, 0, 0 };
		qboolean own;

		if ( !bd->time || cg.time - bd->time > 100 || !bd->num ) {
			continue;
		}
		if ( hi[0] < bd->mins[0] || lo[0] > bd->maxs[0] || hi[1] < bd->mins[1] || lo[1] > bd->maxs[1] ||
			 hi[2] < bd->mins[2] || lo[2] > bd->maxs[2] ) {
			if ( bd->entityNum == d->owner ) {
				d->clear = qtrue;       // well away from him
			}
			continue;
		}
		own = bd->entityNum == d->owner;
		for ( j = 0; j < bd->num; j++ ) {
			vec3_t cp, cq, off;
			float d2, rr = bd->r[j];
			// (not near this limb at all: most of them, told without working out where the two pass)
			VectorSubtract( mid, bd->mid[j], off );
			if ( DotProduct( off, off ) > Square( reach + bd->reach[j] ) ) {
				continue;
			}
			d2 = VRB_SegSeg( d->last, d->pos, bd->a[j], bd->b[j], cp, cq );
			if ( d2 < rr * rr && d2 - rr * rr < best ) {
				best = d2 - rr * rr;
				bj = j;
				VectorCopy( cp, bp );
				VectorCopy( cq, bq );
			}
		}
		if ( own && !d->clear ) {
			// still coming out of him: it can land on him once it has left him
			if ( bj < 0 ) {
				d->clear = qtrue;
			}
			continue;
		}
		if ( bj >= 0 ) {
			vec3_t nrm, at;
			VectorSubtract( bp, bq, nrm );
			if ( VectorNormalize( nrm ) < 0.01f ) {
				VectorSubtract( d->last, bq, nrm );
				if ( VectorNormalize( nrm ) < 0.01f ) {
					VectorNegate( d->vel, nrm );
					VectorNormalize( nrm );
				}
			}
			VectorMA( bq, bd->r[bj], nrm, at );
			CG_VRX_BloodOnBody( bd->entityNum, bd->bone[bj], at, nrm, d->vel, d->amount );
			return qtrue;
		}
	}
	return qfalse;
}

static void VRB_Mark( vrDrop_t *d, const trace_t *tr ) {
	int sec = cg.time / 1000;
	float r;

	if ( sec != vrbMarkSecond ) {
		vrbMarkSecond = sec;
		vrbMarksThisSecond = 0;
	}
	if ( vrbMarksThisSecond >= VRB_MAX_MARKS ) {
		return;
	}
	vrbMarksThisSecond++;
	r = d->kind == VRB_GLOB ? 2.0f + random() * 3.5f : d->kind == VRB_DRIP ? 1.0f + random() * 1.5f : 1.5f + random() * 2.0f;
	CG_ImpactMark( vrbDots[rand() % 5], tr->endpos, tr->plane.normal, random() * 360, 1, 1, 1, 1, qfalse, r, qfalse,
				   cg_bloodTime.integer > 0 ? cg_bloodTime.integer * 1000 : 60000 );
}

// blood in the air as a solid thing, not a line: a stream is lengths of round rope whose
// thickness changes from drop to drop (thick where it has gathered, thin where it's drawn
// out), with a bead at each drop, and a loose drop is a bead drawn out along its flight. All
// of it dark, wet blood: nearly black in its own shadow, a small bright glint where it faces
// the eye, which is what makes it read as round.
#define VRB_ROPE_SIDES  6
static void VRB_Shade( polyVert_t *v, const vec3_t nrm, const vec3_t toEye, float alpha ) {
	float up = nrm[2] * 0.5f + 0.5f, face = -DotProduct( nrm, toEye );
	float lit = 0.28f + 0.5f * up + 0.22f * ( face > 0 ? face : 0 );
	float glint = face > 0.86f ? ( face - 0.86f ) * 5.0f : 0;
	float r = lit * 44 + glint * 120, g = lit * 2 + glint * 55, b2 = lit * 1 + glint * 50;
	v->st[0] = v->st[1] = 0.5f;
	v->modulate[0] = (byte)( r > 255 ? 255 : r );
	v->modulate[1] = (byte)( g > 255 ? 255 : g );
	v->modulate[2] = (byte)( b2 > 255 ? 255 : b2 );
	v->modulate[3] = (byte)( 255 * alpha );
}

// never thinner than about a pixel and a half, so far blood doesn't flicker away. *thin: it is
// no wider than a hair's breadth more than that (or three pixels of this view), where its
// roundness can't be made out
static float vrbPixel;      // how wide a pixel of this view is at distance 1
static qboolean vrbFlat;    // Low: all of it flat
static float vrbFlatFar;    // Medium: flat further off than this

static float VRB_Least( const vec3_t at, float r, float *alpha, vec3_t toEye, qboolean *thin ) {
	float least, dist;
	VectorSubtract( at, cg.refdef.vieworg, toEye );
	dist = VectorNormalize( toEye );
	least = dist * 0.0012f;
	if ( r < least ) {
		*alpha *= r / least;
		r = least;
	}
	if ( *alpha > 1 ) {
		*alpha = 1;
	}
	*thin = r < 1.3f * least || r < 1.5f * vrbPixel * dist || vrbFlat || ( vrbFlatFar > 0 && dist > vrbFlatFar );
	return r;
}

// the round of normals about an axis that a rope or a bead is made on
static void VRB_Round( const vec3_t axis, vec3_t nrm[VRB_ROPE_SIDES + 1] ) {
	vec3_t u, v;
	int i;

	PerpendicularVector( u, axis );
	CrossProduct( axis, u, v );
	for ( i = 0; i <= VRB_ROPE_SIDES; i++ ) {
		float ang = i * 2.0f * M_PI / VRB_ROPE_SIDES;
		VectorScale( u, cos( ang ), nrm[i] );
		VectorMA( nrm[i], sin( ang ), v, nrm[i] );
	}
}

// a few pixels wide, a round thing and a flat one facing the eye are the same picture: it is
// drawn as one polygon of the colour its sides come to together (and as solid as its near and
// far sides were, one over the other), not eighteen. In a fight that is most of the blood in
// the air, and it was thousands of polygons a frame: more than the renderer takes, so what
// came after them wasn't drawn at all.
static void VRB_Flat( const vec3_t axis, const vec3_t toEye, float alpha, polyVert_t *verts, int numVerts, vec3_t side ) {
	vec3_t nrm[VRB_ROPE_SIDES + 1];
	polyVert_t one;
	int i, k, sum[3] = { 0, 0, 0 };

	VRB_Round( axis, nrm );
	for ( i = 0; i < VRB_ROPE_SIDES; i++ ) {
		VRB_Shade( &one, nrm[i], toEye, alpha );
		for ( k = 0; k < 3; k++ ) {
			sum[k] += one.modulate[k];
		}
	}
	for ( i = 0; i < numVerts; i++ ) {
		verts[i].st[0] = verts[i].st[1] = 0.5f;
		for ( k = 0; k < 3; k++ ) {
			verts[i].modulate[k] = (byte)( sum[k] / VRB_ROPE_SIDES );
		}
		verts[i].modulate[3] = (byte)( 255 * ( 1.0f - ( 1.0f - alpha ) * ( 1.0f - alpha ) ) );
	}
	CrossProduct( axis, toEye, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		PerpendicularVector( side, axis );      // (seen end on)
	}
}

// a length of it from a (ra thick) to b (rb thick)
static void VRB_Rope( const vec3_t a, const vec3_t b, float ra, float rb, float alpha ) {
	polyVert_t verts[VRB_ROPE_SIDES * 4];
	vec3_t along, to, mid, nrm[VRB_ROPE_SIDES + 1];
	float scale;
	qboolean thin;
	int i, k;

	VectorSubtract( b, a, along );
	if ( VectorNormalize( along ) < 0.0001f ) {
		return;
	}
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	scale = VRB_Least( mid, ( ra + rb ) * 0.5f, &alpha, to, &thin ) / ( ( ra + rb ) * 0.5f );
	ra *= scale;
	rb *= scale;
	if ( thin ) {
		vec3_t side;
		VRB_Flat( along, to, alpha, verts, 4, side );
		VectorMA( a, -ra, side, verts[0].xyz );
		VectorMA( a, ra, side, verts[1].xyz );
		VectorMA( b, rb, side, verts[2].xyz );
		VectorMA( b, -rb, side, verts[3].xyz );
		trap_R_AddPolyToScene( vrbRopeShader, 4, verts );
		return;
	}
	VRB_Round( along, nrm );
	for ( i = 0; i < VRB_ROPE_SIDES; i++ ) {
		for ( k = 0; k < 4; k++ ) {
			int j = ( k == 0 || k == 3 ) ? i : i + 1;
			VectorMA( k < 2 ? a : b, k < 2 ? ra : rb, nrm[j], verts[i * 4 + k].xyz );
			VRB_Shade( &verts[i * 4 + k], nrm[j], to, alpha );
		}
	}
	trap_R_AddPolysToScene( vrbRopeShader, 4, verts, VRB_ROPE_SIDES );
}

// a bead of it at p, r thick, drawn out to len either way along dir: two cones base to base
static void VRB_Bead( const vec3_t p, const vec3_t dir, float r, float len, float alpha ) {
	polyVert_t verts[2 * VRB_ROPE_SIDES * 3], *vt = verts;
	vec3_t to, nrm[VRB_ROPE_SIDES + 1], tip[2];
	qboolean thin;
	int i, e;

	r = VRB_Least( p, r, &alpha, to, &thin );
	len = len < r ? r : len;
	VectorMA( p, len, dir, tip[0] );
	VectorMA( p, -len, dir, tip[1] );
	if ( thin ) {
		vec3_t side;
		VRB_Flat( dir, to, alpha, verts, 4, side );
		VectorCopy( tip[0], verts[0].xyz );
		VectorMA( p, r, side, verts[1].xyz );
		VectorCopy( tip[1], verts[2].xyz );
		VectorMA( p, -r, side, verts[3].xyz );
		trap_R_AddPolyToScene( vrbRopeShader, 4, verts );
		return;
	}
	VRB_Round( dir, nrm );
	for ( e = 0; e < 2; e++ ) {
		vec3_t tn;
		VectorScale( dir, e ? -1 : 1, tn );
		for ( i = 0; i < VRB_ROPE_SIDES; i++, vt += 3 ) {
			vec3_t n0, n1;
			// (the sides lean towards the tip: shaded as a rounded thing, not a cylinder)
			VectorMA( nrm[i], 0.5f, tn, n0 );
			VectorMA( nrm[i + 1], 0.5f, tn, n1 );
			VectorNormalize( n0 );
			VectorNormalize( n1 );
			VectorMA( p, r, nrm[i], vt[0].xyz );
			VectorMA( p, r, nrm[i + 1], vt[1].xyz );
			VectorCopy( tip[e], vt[2].xyz );
			VRB_Shade( &vt[0], n0, to, alpha );
			VRB_Shade( &vt[1], n1, to, alpha );
			VRB_Shade( &vt[2], tn, to, alpha );
		}
	}
	trap_R_AddPolysToScene( vrbRopeShader, 3, verts, 2 * VRB_ROPE_SIDES );
}

// how thick the stream is at a drop: its own, the same every frame, a few thick ones among thin
static float VRB_Thick( const vrDrop_t *d ) {
	unsigned h = (unsigned)( d - vrbDrops ) * 2654435761u + (unsigned)d->born * 40503u;
	float f = ( ( h >> 8 ) & 1023 ) / 1023.0f;
	return d->size * ( 0.16f + 0.34f * f * f );
}

// a quad from a to b, w either side, turned to face the eye; t0/t1 the texture down its length
static void VRB_Quad( qhandle_t shader, const vec3_t a, const vec3_t b, float w, float t0, float t1, float alpha ) {
	polyVert_t verts[4];
	vec3_t along, to, side, mid;
	int k;

	VectorSubtract( b, a, along );
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( mid, cg.refdef.vieworg, to );
	CrossProduct( along, to, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		return;
	}
	{
		// never thinner than about a pixel and a half, so a far stream doesn't flicker away
		float least = VectorLength( to ) * 0.0012f;
		if ( w < least ) {
			alpha *= w / least;
			w = least;
		}
	}
	alpha = alpha > 1 ? 1 : alpha;
	for ( k = 0; k < 4; k++ ) {
		const float *e = k < 2 ? a : b;
		float s = ( k == 0 || k == 3 ) ? -w : w;
		VectorMA( e, s, side, verts[k].xyz );
		verts[k].st[0] = ( k == 0 || k == 3 ) ? 0 : 1;
		verts[k].st[1] = k < 2 ? t0 : t1;
		verts[k].modulate[0] = verts[k].modulate[1] = verts[k].modulate[2] = 255;
		verts[k].modulate[3] = (byte)( 255 * alpha );
	}
	trap_R_AddPolyToScene( shader, 4, verts );
}

/*
=================
CG_VRB_AddDrops

Every view (both eyes): the drops moved on once a frame, then drawn.
=================
*/
void CG_VRB_AddDrops( void ) {
	int i, dt;
	float sec;

	double t0 = CG_VRPerf_Now();

	if ( cg.time != vrbLastUpdate ) {
		VRP_Frame();
	}
	if ( !CG_VRB_On() ) {
		vrbNumLive = 0;
		vrbLastUpdate = cg.time;
		return;
	}
	VRB_Shaders();
	vrbFlat = CG_VR_GoreQuality() == 0;
	vrbFlatFar = CG_VR_GoreQuality() == 1 ? 160.0f : 0;
	// (how wide a pixel of this view is at distance 1: what is too thin to show as round)
	vrbPixel = cg.refdef.width > 0 ? 2.0f * tan( DEG2RAD( cg.refdef.fov_x ) * 0.5f ) / cg.refdef.width : 0.001f;
	dt = cg.time - vrbLastUpdate;
	if ( dt < 0 || dt > 200 ) {
		dt = 0;
	}
	vrbLastUpdate = cg.time;
	sec = dt * 0.001f;

	// moved on (once a frame)
	if ( sec > 0 ) {
		vrbNumLive = 0;
		for ( i = 0; i < VRB_MAX; i++ ) {
			vrDrop_t *d = &vrbDrops[i];
			float k2, age;

			if ( !d->life ) {
				continue;
			}
			age = cg.time - d->born;
			if ( age >= d->life || age < 0 ) {
				d->life = 0;
				continue;
			}
			k2 = 1.0f - d->drag * sec;
			k2 = k2 < 0 ? 0 : k2;
			VectorScale( d->vel, k2, d->vel );
			d->vel[2] -= VRB_GRAVITY * sec;
			VectorCopy( d->pos, d->last );
			VectorMA( d->pos, sec, d->vel, d->pos );
			// on someone: it soaks into his clothes there (Low: not the mist)
			if ( ( d->kind != VRB_MIST || CG_VR_GoreQuality() > 0 ) && VRB_HitBodies( d ) ) {
				d->life = 0;
				continue;
			}
			if ( d->kind != VRB_MIST && cg.time - d->lastTrace >= VRB_TRACE_MS ) {
				trace_t tr;
				trap_CM_BoxTrace( &tr, d->traced, d->pos, NULL, NULL, 0, MASK_SOLID );
				d->lastTrace = cg.time;
				if ( tr.fraction < 1.0f || tr.startsolid ) {
					if ( d->mark && !tr.startsolid ) {
						VRB_Mark( d, &tr );
					}
					d->life = 0;
					continue;
				}
				VectorCopy( d->pos, d->traced );
			}
			VectorCopy( d->pos, vrbLive[vrbNumLive++] );
		}
	}

	// drawn
	for ( i = 0; i < VRB_MAX; i++ ) {
		vrDrop_t *d = &vrbDrops[i], *p = NULL;
		vec3_t fwd, a, b;
		float speed, len, age, alpha;

		if ( !d->life ) {
			continue;
		}
		age = cg.time - d->born;
		if ( age >= d->life || age < 0 ) {
			continue;
		}
		alpha = d->alpha;
		if ( d->kind == VRB_MIST ) {
			alpha *= 1.0f - age / d->life;
		} else if ( d->life - age < 200 ) {
			alpha *= ( d->life - age ) / 200.0f;
		}
		if ( d->kind == VRB_STREAM && d->prev >= 0 ) {
			p = &vrbDrops[d->prev];
			if ( !p->life || p->born != d->prevBorn || p->kind != VRB_STREAM || Distance( p->pos, d->pos ) > VRB_BREAK ) {
				p = NULL;       // the drop before it is gone, or they've pulled apart: drops now
			}
		}
		if ( p && vrbRopeShader ) {
			vec3_t along;
			float ra = VRB_Thick( d ), rb = VRB_Thick( p );
			VRB_Rope( d->pos, p->pos, ra, rb, alpha );
			VectorSubtract( p->pos, d->pos, along );
			if ( VectorNormalize( along ) > 0.001f ) {
				VRB_Bead( d->pos, along, ra * 1.25f, ra * 2.2f, alpha );     // (and the joint is covered)
			}
			continue;
		}
		if ( p && vrbStreamShader ) {
			// the stream: a thin ribbon from this drop to the one before, the texture flowing along it
			VRB_Quad( vrbStreamShader, d->pos, p->pos, d->size * 0.24f, ( d->born % 100000 ) * 0.004f, ( p->born % 100000 ) * 0.004f, alpha );
			continue;
		}

		// a drop: drawn out along the way it flies
		speed = VectorNormalize2( d->vel, fwd );
		if ( speed < 1.0f ) {
			VectorSet( fwd, 0, 0, 1 );
		}
		len = d->size + speed * d->stretch;
		VectorMA( d->pos, -len * 0.5f, fwd, a );
		VectorMA( d->pos, len * 0.5f, fwd, b );
		if ( vrbRopeShader && d->kind != VRB_MIST ) {
			VRB_Bead( d->pos, fwd, VRB_Thick( d ) * 1.3f, len * 0.5f, alpha );     // a drop, solid too
		} else if ( vrbDropShader ) {
			VRB_Quad( vrbDropShader, a, b, d->size * 0.32f, 0, 1, alpha );
		} else {
			VRB_Quad( vrbDots[d->shader], a, b, d->size, 0, 1, alpha );
		}
	}
	CG_VRPerf_Add( VRP_DROPS, CG_VRPerf_Now() - t0 );
}
