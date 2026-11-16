/*
===========================================================================

HitReact demo -- the library on its own, no game engine

Copyright (C) 2026 RealRTCW-VR contributors. GPL v3 or later (see hitreact.h).

===========================================================================
*/

// A floor, a wall, seven men standing in a row, and a script of what happens to each:
//
//   0 stunned    three chest shots: staggers, catches himself, comes to on his feet
//   1 gut shot   killed: doubled over, fights for his feet, gives out, writhes
//   2 chest shot killed: reels back, goes down
//   3 knockdown  knocked down, lies there, stood up by "the game" at 3.5 s, gets up as physics
//   4 headshot   killed: limp at once
//   5 blast      a grenade behind him: thrown, limp
//   6 leg shot   killed: hops on the shot leg, goes down
//
// It plays the part of a game: it owns the clock, says who is stunned or dead, moves its
// "game body" when HitReact says the body has staggered somewhere, and stands the knocked
// down man up. No trace callback: HitReact traces against the level it was given.
//
//   hr_demo [inches|metres|inches-yup|metres-zup] [timeline.txt] [frames.csv]
//
// inches: Z up, 1 unit an inch (Quake-engine games). metres: Y up, 1 unit a metre (Renegade,
// Unity, most physics engines); the same scene, converted on the way in and out. The two runs
// should tell the same story. frames.csv gets every point of every man each frame, in inches
// Z up whichever space the run used, for tools/hr_render.py.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../hitreact.h"

#define NUM_MEN         7
#define FRAME_MS        16
#define STEP_MS         8           // realWeights: two 8 ms physics steps a frame
#define END_MS          12000
#define SPACING         150.0f      // inches between men (room to stagger)
#define KILL_PUSH       110.0f      // a dying man falls away from his killer this fast (inches/s)

static float hostScale = 1;         // host units per inch
static int hostYUp;

// the scene is written in inches, Z up; these put it into the host's space and back
// (in and out may be the same)
static void ToHost( const float in[3], float out[3] ) {
	float x = in[0], y = in[1], z = in[2];

	if ( hostYUp ) {
		out[0] = x * hostScale;
		out[1] = z * hostScale;
		out[2] = -y * hostScale;
	} else {
		out[0] = x * hostScale;
		out[1] = y * hostScale;
		out[2] = z * hostScale;
	}
}

static void FromHost( const float in[3], float out[3] ) {
	float x = in[0], y = in[1], z = in[2];

	if ( hostYUp ) {
		out[0] = x / hostScale;
		out[1] = -z / hostScale;
		out[2] = y / hostScale;
	} else {
		out[0] = x / hostScale;
		out[1] = y / hostScale;
		out[2] = z / hostScale;
	}
}

typedef struct {
	const char  *name;
	float       home[3];            // where he stands (inches)
	hrBody_t    body;
	int         built;              // a HitReact body (else he's "animated", standing)
	int         dead;
	int         stunUntil;          // the game has him stunned until then
	int         downUntil;          // the game has him knocked down until then
	float       origin[3];          // the game's body for him (host space)
	float       shot[3];            // where the last bullet went in (host space)
	int         lastEu;
	int         himself;
	float       standAt[2];         // where he came to (inches), standing there in his animation
} man_t;

static man_t men[NUM_MEN] = {
	{ "stunned" }, { "gut shot" }, { "chest shot" }, { "knockdown" },
	{ "headshot" }, { "blast" }, { "leg shot" }
};

static FILE *timeline;
static int now;

static void Say( const char *fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
static void Say( const char *fmt, ... ) {
	va_list ap;
	fprintf( timeline, "%6.3f  ", now / 1000.0 );
	va_start( ap, fmt );
	vfprintf( timeline, fmt, ap );
	va_end( ap );
}

/*
==============================================================================

The skeleton: 21 points of a man 6 ft tall facing +X (his left is +Y), in inches

==============================================================================
*/
static const float standing[HR_P_COUNT][3] = {
	{ 0, 0, 38 },       // pelvis
	{ 0, 0, 50 },       // chest
	{ 0, 0, 60 },       // neck
	{ 0.5f, 0, 64 },    // head
	{ 0.5f, 0, 71 },    // top of the head
	{ 0, 7.5f, 58 }, { 0, 8.5f, 46.5f }, { 1, 8.5f, 35.5f }, { 1.5f, 8.5f, 31 },        // left arm
	{ 0, -7.5f, 58 }, { 0, -8.5f, 46.5f }, { 1, -8.5f, 35.5f }, { 1.5f, -8.5f, 31 },    // right arm
	{ 0, 4, 36 }, { 0.5f, 4, 20 }, { 0, 4, 3.5f }, { 6, 4, 0.8f },     // left leg
	{ 0, -4, 36 }, { 0.5f, -4, 20 }, { 0, -4, 3.5f }, { 6, -4, 0.8f }, // right leg
};

// killed: a push away from the shooter (at -X); the living get none
static void Build( man_t *m, int killed ) {
	float pts[HR_P_COUNT][3], o[3], p[3], push[3] = { KILL_PUSH, 0, 0 }, hpush[3];
	unsigned char has[HR_P_COUNT];
	int k;

	for ( k = 0; k < HR_P_COUNT; k++ ) {
		p[0] = standing[k][0] + m->home[0];
		p[1] = standing[k][1] + m->home[1];
		p[2] = standing[k][2] + m->home[2];
		ToHost( p, pts[k] );
		has[k] = 1;
	}
	ToHost( m->home, o );
	memcpy( m->origin, o, sizeof( o ) );
	ToHost( push, hpush );
	// (vel: none, he was standing still)
	HR_BodySetSkeleton( &m->body, m, (int)( m - men ), pts[0], has, NULL, NULL, killed ? hpush : NULL, o );
	if ( !HR_BodyBuild( &m->body, 1 ) ) {
		fprintf( stderr, "couldn't build %s\n", m->name );
		exit( 1 );
	}
	m->built = 1;
}

// a bullet from -X at a height above his pelvis (inches), speed in inches/s; the game scales a living man's
// shove down (he's knocked about, not thrown)
static int Shoot( man_t *m, float height, float side, float speed, int killing ) {
	float p[3], v[3], hp[3], hv[3];

	p[0] = m->home[0] - 4;
	p[1] = m->home[1] + side;
	p[2] = m->home[2] + standing[HR_P_PELVIS][2] + height;
	if ( m->built ) {
		HR_GetPoint( &m->body, HR_P_PELVIS, hp );
		FromHost( hp, hp );
		p[0] = hp[0] - 4;       // (wherever he's staggered to)
		p[1] = hp[1] + side;
	}
	v[0] = speed;
	v[1] = v[2] = 0;
	ToHost( p, hp );
	ToHost( v, hv );
	memcpy( m->shot, hp, sizeof( hp ) );
	if ( !m->built ) {
		Build( m, killing );
	}
	if ( m->body.living ) {
		hv[0] *= 0.25f, hv[1] *= 0.25f, hv[2] *= 0.25f;
	}
	return HR_BodyShot( &m->body, hp, hv );
}

/*
==============================================================================

The host's callbacks

==============================================================================
*/
static void HostPrint( const char *msg ) {
	fprintf( timeline, "%6.3f    | %s", now / 1000.0, msg );
}

static void HostEvent( void *user, int event, const float *args ) {
	man_t *m = user;
	float p[3];

	switch ( event ) {
	case HR_EV_FELL:
		Say( "%-10s event: shoved over (the game knocks him down for 3 s)\n", m->name );
		m->downUntil = now + 3000;
		break;
	case HR_EV_POSITION:
		memcpy( m->origin, args, sizeof( float ) * 3 );     // the game's body follows his
		FromHost( args, p );
		Say( "%-10s event: body at %.0f %.0f %.0f\n", m->name, p[0], p[1], p[2] );
		break;
	case HR_EV_HOLD:
		Say( "%-10s event: still reeling, hold on\n", m->name );
		break;
	case HR_EV_GETUP_YAW:
		Say( "%-10s event: gets up facing %.0f deg\n", m->name, args[0] );
		break;
	}
}

static const char *euNames[] = { "", "on his feet", "going down", "lying" };

int main( int argc, char **argv ) {
	hrHost_t host;
	hrOptions_t opt;
	float floorHull[8][3], wallHull[8][3], h[8][3];
	FILE *frames = NULL;
	int i, k, step;

	if ( argc > 1 && !strncmp( argv[1], "metres", 6 ) ) {
		hostScale = 0.0254f;
		hostYUp = 1;
	}
	if ( argc > 1 && !strcmp( argv[1], "inches-yup" ) ) {
		hostYUp = 1;            // (the axes alone: should match "inches" exactly)
	}
	if ( argc > 1 && !strcmp( argv[1], "metres-zup" ) ) {
		hostYUp = 0;            // (the units alone)
	}
	timeline = argc > 2 ? fopen( argv[2], "w" ) : stdout;
	if ( argc > 3 ) {
		frames = fopen( argv[3], "w" );
	}
	if ( !timeline ) {
		return 1;
	}
	HR_SetHostSpace( hostScale, hostYUp );

	if ( !HRP_Init( HostPrint ) ) {
		fprintf( stderr, "no physics\n" );
		return 1;
	}
	// the level: a floor, and a wall 90 inches in front of the row
	for ( i = 0; i < 8; i++ ) {
		floorHull[i][0] = ( i & 1 ) ? 2000 : -2000;
		floorHull[i][1] = ( i & 2 ) ? 2000 : -2000;
		floorHull[i][2] = ( i & 4 ) ? 0 : -32;
		wallHull[i][0] = ( i & 1 ) ? 110 : 90;
		wallHull[i][1] = ( i & 2 ) ? 2000 : -2000;
		wallHull[i][2] = ( i & 4 ) ? 120 : -32;
	}
	HRP_BeginWorld();
	for ( i = 0; i < 8; i++ ) {
		ToHost( floorHull[i], h[i] );
	}
	HRP_AddWorldHull( h[0], 8 );
	for ( i = 0; i < 8; i++ ) {
		ToHost( wallHull[i], h[i] );
	}
	HRP_AddWorldHull( h[0], 8 );
	HRP_EndWorld();

	memset( &host, 0, sizeof( host ) );
	host.print = HostPrint;
	host.event = HostEvent;     // (trace, getUpPose, getUpAnim*, random: HitReact's own)
	HR_SetHost( &host );
	opt.gravity = 400 * hostScale;
	opt.active = 1;
	opt.euphoria = 1;
	opt.strength = 1;
	opt.maxActive = 12;
	opt.debug = 1;
	HR_SetOptions( &opt );

	for ( i = 0; i < NUM_MEN; i++ ) {
		men[i].home[1] = ( i - ( NUM_MEN - 1 ) * 0.5f ) * SPACING;
	}
	fprintf( timeline, "HitReact demo, host space: %s, %s up\n\n", hostScale == 1 ? "inches" : "metres", hostYUp ? "Y" : "Z" );

	for ( now = 0; now <= END_MS; now += FRAME_MS ) {
		HR_SetTime( now );

		// the script
		if ( now == 496 ) {
			int part;
			// 0: stunned by three chest shots
			Shoot( &men[0], 15, 0, 300, 0 );
			HR_Stun( &men[0].body );
			men[0].stunUntil = now + 500;
			Say( "%-10s shot in the chest, stunned\n", men[0].name );
			// 1: gut shot, killed
			part = Shoot( &men[1], 3, 1, 300, 1 );
			HR_Die( &men[1].body, part, men[1].shot );
			men[1].dead = 1;
			Say( "%-10s shot in the gut, killed\n", men[1].name );
			// 2: chest shot, killed
			part = Shoot( &men[2], 16, -1, 300, 1 );
			HR_Die( &men[2].body, part, men[2].shot );
			men[2].dead = 1;
			Say( "%-10s shot in the chest, killed\n", men[2].name );
			// 3: knocked down
			Shoot( &men[3], 10, 0, 600, 0 );
			HR_KnockDown( &men[3].body );
			men[3].downUntil = now + 3000;
			Say( "%-10s knocked down\n", men[3].name );
			// 4: headshot
			part = Shoot( &men[4], 27, 0, 300, 1 );
			HR_Die( &men[4].body, part, men[4].shot );
			men[4].dead = 1;
			Say( "%-10s headshot\n", men[4].name );
			// 5: a grenade a yard behind him
			{
				float g[3], hg[3];
				Build( &men[5], 0 );     // (thrown by the blast, not pushed)
				g[0] = men[5].home[0] - 36;
				g[1] = men[5].home[1];
				g[2] = 8;
				ToHost( g, hg );
				men[5].body.blasted = 1;
				HR_BodyBlast( &men[5].body, hg, 300 * hostScale, 450 * hostScale );
				HR_Die( &men[5].body, -1, NULL );
				men[5].dead = 1;
				Say( "%-10s grenade behind him\n", men[5].name );
			}
			// 6: shot in the left thigh, killed
			part = Shoot( &men[6], -10, 4, 300, 1 );
			HR_Die( &men[6].body, part, men[6].shot );
			men[6].dead = 1;
			Say( "%-10s shot in the leg, killed\n", men[6].name );
		}
		if ( now == 800 || now == 1104 ) {
			Shoot( &men[0], 14, now == 800 ? 3 : -3, 300, 0 );
			HR_Rehit( &men[0].body );
			men[0].stunUntil = now + 500;
			Say( "%-10s shot again\n", men[0].name );
		}

		// the game's logic for each, then HitReact's frame
		for ( i = 0; i < NUM_MEN; i++ ) {
			man_t *m = &men[i];
			hrBody_t *b = &m->body;
			if ( !m->built ) {
				continue;
			}
			if ( b->living && !m->dead ) {
				int hit = now < m->stunUntil;
				int downed = now < m->downUntil;
				if ( HR_Living( b, hit, downed ) ) {
					float pel[3];
					Say( "%-10s himself again: back to his animation\n", m->name );
					HR_GetPoint( b, HR_P_PELVIS, pel );
					FromHost( pel, pel );
					m->standAt[0] = pel[0] - m->home[0];
					m->standAt[1] = pel[1] - m->home[1];
					HR_BodyFree( b );
					m->built = 0;
					m->himself = 1;
					continue;
				}
			}
			HR_Settle( b, 0, m->origin );
			HR_Frame( b, m->origin, 0, NULL, 0, 0 );
			if ( b->eu != m->lastEu ) {
				Say( "%-10s %s\n", m->name, b->eu ? euNames[b->eu] : b->getUpStart ? "getting up" : b->recoverStart ? "coming to" : "limp" );
				m->lastEu = b->eu;
			}
		}
		for ( step = 0; step < FRAME_MS / STEP_MS; step++ ) {
			HRP_Step( STEP_MS * 0.001f, opt.gravity );
		}

		// a line on each every second
		if ( now % 1008 == 0 && now ) {
			fprintf( timeline, "%6.3f  ---", now / 1000.0 );
			for ( i = 0; i < NUM_MEN; i++ ) {
				float p[3];
				if ( !men[i].built ) {
					fprintf( timeline, "  %s: %s", men[i].name, men[i].himself ? "himself" : "standing" );
					continue;
				}
				HR_GetPoint( &men[i].body, HR_P_HEAD, p );
				FromHost( p, p );
				fprintf( timeline, "  %s: head %.0f in", men[i].name, p[2] );
			}
			fprintf( timeline, "\n" );
		}
		if ( frames ) {
			fprintf( frames, "%d", now );
			for ( i = 0; i < NUM_MEN; i++ ) {
				for ( k = 0; k < HR_P_COUNT; k++ ) {
					float p[3];
					if ( men[i].built ) {
						HR_GetPoint( &men[i].body, k, p );
						FromHost( p, p );
					} else {
						p[0] = standing[k][0] + men[i].home[0] + men[i].standAt[0];
						p[1] = standing[k][1] + men[i].home[1] + men[i].standAt[1];
						p[2] = standing[k][2] + men[i].home[2];
					}
					fprintf( frames, ",%.2f,%.2f,%.2f", p[0], p[1], p[2] );
				}
			}
			fprintf( frames, "\n" );
		}
	}
	for ( i = 0; i < NUM_MEN; i++ ) {
		HR_BodyFree( &men[i].body );
	}
	HRP_Shutdown();
	if ( frames ) {
		fclose( frames );
	}
	return 0;
}
