// movers: after two steps per frame the mover must be where it was placed, in any host space
#include "hitreact.h"
#include <stdio.h>
#include <math.h>
static float box[8*3];
static float probe( float y, float z, int yup ) {   // x of the first face met from -x
	float s[3] = { -1000, y, z }, e[3] = { 1000, y, z }, f, ep[3];
	(void)yup;
	HRP_RayCast( s, e, &f, ep );
	return f < 1 ? ep[0] : NAN;
}
int main( void ) {
	int space, i, bad = 0;
	for ( space = 0; space < 2; space++ ) {
		float u = space ? 0.0254f : 1.0f;     // host units per inch
		float h = 10 * u, id[9] = { 1,0,0, 0,1,0, 0,0,1 }, o[3] = { 0, 0, 0 }, worst = 0, x;
		int m, n = 0, frame;
		HR_SetHostSpace( u, space );
		HRP_Init( NULL );
		HRP_BeginWorld(); HRP_EndWorld();
		for ( i = 0; i < 8; i++ ) { box[n++] = ( i & 1 ? h : -h ); box[n++] = ( i & 2 ? h : -h ) * 2; box[n++] = ( i & 4 ? h : -h ) * 3; }
		m = HRP_CreateMover(); HRP_MoverAddHull( m, box, 8 ); HRP_MoverFinish( m );
		HRP_MoverPlace( m, o, id, 0 );
		for ( frame = 0; frame < 40; frame++ ) {
			if ( frame < 30 ) o[0] += 5 * u;         // moves for 30 frames, then stands
			HRP_MoverPlace( m, o, id, 0 );
			HRP_Step( 0.008f, 400 * u ); HRP_Step( 0.008f, 400 * u );
			x = probe( 0, 0, space );
			if ( fabsf( x - ( o[0] - h ) ) > worst ) worst = fabsf( x - ( o[0] - h ) );
		}
		printf( "space %d: worst mover error %.3f inches\n", space, worst / u );
		bad |= worst / u > 0.1f;
		// turned a quarter about the host's up axis: its long side (30) now lies along x
		{
			float rz[9] = { 0,1,0, -1,0,0, 0,0,1 }, ry[9] = { 0,0,-1, 0,1,0, 1,0,0 };
			HRP_MoverPlace( m, o, space ? ry : rz, 1 );
			HRP_Step( 0.008f, 400 * u );
			x = probe( 0, 0, space );
			printf( "space %d: turned about up, face at %.2f in from centre (want %s)\n", space, ( o[0] - x ) / u, space ? "30" : "20" );
			bad |= fabsf( ( o[0] - x ) / u - ( space ? 30 : 20 ) ) > 0.1f;
		}
		HRP_Shutdown();
	}
	printf( bad ? "FAIL\n" : "PASS\n" );
	return bad;
}
