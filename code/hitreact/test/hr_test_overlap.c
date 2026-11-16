// parts of one body that start inside each other must pass through, not be thrown apart,
// in every ragdoll slot (the first, and ones after it)
#include "hitreact.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int make( float x ) {
	hrpPart_t p[3];
	unsigned char nc[9] = { 0 };
	int i;
	memset( p, 0, sizeof( p ) );
	for ( i = 0; i < 3; i++ ) {
		p[i].parent = i ? 0 : -1;
		p[i].shape = HRP_SHAPE_CAPSULE;
		p[i].radius = 3; p[i].mass = 5;
	}
	p[0].a[0] = x; p[0].a[2] = 100; p[0].b[0] = x; p[0].b[2] = 120;
	// two arms from the same shoulder point, lying across each other
	for ( i = 1; i < 3; i++ ) {
		p[i].a[0] = x; p[i].a[2] = 122; p[i].b[0] = x + 20; p[i].b[2] = 122 + ( i == 1 ? 1.5f : -1.5f );
		p[i].a[1] = p[i].b[1] = 6;
		p[i].joint = HRP_JOINT_SWINGTWIST;
		p[i].pivot[0] = x; p[i].pivot[1] = 6; p[i].pivot[2] = 122;
		p[i].twist1[0] = 1; p[i].twist2[0] = 1; p[i].plane[2] = 1;
		p[i].planeHalf = p[i].normalHalf = 80; p[i].twistMin = -30; p[i].twistMax = 30;
	}
	nc[1 * 3 + 2] = 1;
	return HRP_CreateRagdoll( p, 3, nc );
}
int main( void ) {
	int rd[3], i, k, bad = 0;
	HR_SetHostSpace( 1, 0 );
	HRP_Init( NULL );
	HRP_BeginWorld(); HRP_EndWorld();
	for ( i = 0; i < 3; i++ ) rd[i] = make( i * 200.0f );
	for ( i = 0; i < 3; i++ ) {
		float pos0[9], ax[27], pos[9], most = 0;
		HRP_RagdollPose( rd[i], pos0, ax );
		for ( k = 0; k < 5; k++ ) HRP_Step( 0.008f, 0.001f );     // (no gravity to speak of)
		HRP_RagdollPose( rd[i], pos, ax );
		for ( k = 0; k < 9; k++ ) most = fmaxf( most, fabsf( pos[k] - pos0[k] ) );
		printf( "ragdoll %d: parts moved %.2f in 40 ms\n", rd[i], most );
		bad |= most > 0.5f;
	}
	HRP_Shutdown();
	printf( bad ? "FAIL\n" : "PASS\n" );
	return bad;
}
