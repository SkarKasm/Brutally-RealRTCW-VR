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

// cg_gore.cpp -- voxel gore: bullets carve each body's own voxel copy of itself (gore/gorecore.cpp)
//
// Each skeletal model is read from its .mds file once, put in one pose (the rest pose) and
// filled with voxels, its bones inside it as rods. A body that gets shot takes a copy of
// that. A shot is taken into the rest pose through the bone it hit nearest and carves a
// tube there (out the other side if it has the energy), so wounds stay put on the body
// whatever it does afterwards. From the voxels come what the renderer draws (vr_gore.h):
// the skin with the holes left out, and the wound walls (the flesh), each vertex moving
// with its own bone. A part shot clean off (once the body is dead) becomes a body of its own.
//
// gorecore works in metres, everything here in the model's units (inches): METRES converts.
// Like cg_physics.cpp, a plain C interface (cg_gore.h) and nothing of the cgame's.

#include "cg_gore.h"
#include "../gore/gorecore.h"

#include <vector>
#include <algorithm>
#include <memory>
#include <string.h>
#include <math.h>

using gore::V3;

extern "C" {
static V3 TriNearest( V3 p, V3 a, V3 b, V3 c );
}

namespace {

const float METRES = 0.0254f;       // a model unit (an inch) in metres
const int MAX_MODELS = 16;
const int MAX_BODIES = 24;          // bodies and the pieces shot off them
const int MAX_BONES = 128;

// --------------------------------------------------------------------------- the .mds file

#pragma pack(push, 4)
struct MdsHeader { int ident, version; char name[64]; float lodScale, lodBias; int numFrames, numBones, ofsFrames, ofsBones, torsoParent, numSurfaces, ofsSurfaces, numTags, ofsTags, ofsEnd; };
struct MdsSurface { int ident; char name[64], shader[64]; int shaderIndex, minLod, ofsHeader, numVerts, ofsVerts, numTriangles, ofsTriangles, ofsCollapseMap, numBoneReferences, ofsBoneReferences, ofsEnd; };
struct MdsWeight { int boneIndex; float boneWeight; float offset[3]; };
struct MdsVertex { float normal[3]; float texCoords[2]; int numWeights, fixedParent; float fixedDist; MdsWeight weights[1]; };
// heads: .md3, or .mdc (md3 with frames stored as small offsets from a few full ones)
struct Md3Header { int ident, version; char name[64]; int flags, numFrames, numTags, numSurfaces, numSkins, ofsFrames, ofsTags, ofsSurfaces, ofsEnd; };
struct Md3Surface { int ident; char name[64]; int flags, numFrames, numShaders, numVerts, numTriangles, ofsTriangles, ofsShaders, ofsSt, ofsXyzNormals, ofsEnd; };
struct MdcHeader { int ident, version; char name[64]; int flags, numFrames, numTags, numSurfaces, numSkins, ofsFrames, ofsTagNames, ofsTags, ofsSurfaces, ofsEnd; };
struct MdcSurface { int ident; char name[64]; int flags, numCompFrames, numBaseFrames, numShaders, numVerts, numTriangles, ofsTriangles, ofsShaders, ofsSt, ofsXyzNormals, ofsXyzCompressed, ofsFrameBaseFrames, ofsFrameCompFrames, ofsEnd; };
struct Md3XyzNormal { short xyz[3]; short normal; };
#pragma pack(pop)

// --------------------------------------------------------------------------- poses

struct Xf { float m[9]; float t[3]; };          // p = m p' + t (m by rows)

Xf PoseBone( const float *pose, int b ) {
	Xf x;
	memcpy( x.m, pose + b * 12, sizeof( x.m ) );
	memcpy( x.t, pose + b * 12 + 9, sizeof( x.t ) );
	return x;
}

Xf Inverse( const Xf &a ) {
	const float *m = a.m;
	float det = m[0] * ( m[4] * m[8] - m[5] * m[7] ) - m[1] * ( m[3] * m[8] - m[5] * m[6] ) + m[2] * ( m[3] * m[7] - m[4] * m[6] );
	float id = fabsf( det ) > 1e-12f ? 1.0f / det : 0.0f;
	Xf r;
	r.m[0] = ( m[4] * m[8] - m[5] * m[7] ) * id;
	r.m[1] = ( m[2] * m[7] - m[1] * m[8] ) * id;
	r.m[2] = ( m[1] * m[5] - m[2] * m[4] ) * id;
	r.m[3] = ( m[5] * m[6] - m[3] * m[8] ) * id;
	r.m[4] = ( m[0] * m[8] - m[2] * m[6] ) * id;
	r.m[5] = ( m[2] * m[3] - m[0] * m[5] ) * id;
	r.m[6] = ( m[3] * m[7] - m[4] * m[6] ) * id;
	r.m[7] = ( m[1] * m[6] - m[0] * m[7] ) * id;
	r.m[8] = ( m[0] * m[4] - m[1] * m[3] ) * id;
	for ( int i = 0; i < 3; i++ ) {
		r.t[i] = -( r.m[i * 3] * a.t[0] + r.m[i * 3 + 1] * a.t[1] + r.m[i * 3 + 2] * a.t[2] );
	}
	return r;
}

Xf Mul( const Xf &a, const Xf &b ) {        // a after b
	Xf r;
	for ( int i = 0; i < 3; i++ ) {
		for ( int j = 0; j < 3; j++ ) {
			r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
		}
		r.t[i] = a.m[i * 3] * b.t[0] + a.m[i * 3 + 1] * b.t[1] + a.m[i * 3 + 2] * b.t[2] + a.t[i];
	}
	return r;
}

V3 Point( const Xf &x, V3 p ) {
	return { x.m[0] * p.x + x.m[1] * p.y + x.m[2] * p.z + x.t[0],
			 x.m[3] * p.x + x.m[4] * p.y + x.m[5] * p.z + x.t[1],
			 x.m[6] * p.x + x.m[7] * p.y + x.m[8] * p.z + x.t[2] };
}

V3 Dir( const Xf &x, V3 p ) {
	return { x.m[0] * p.x + x.m[1] * p.y + x.m[2] * p.z,
			 x.m[3] * p.x + x.m[4] * p.y + x.m[5] * p.z,
			 x.m[6] * p.x + x.m[7] * p.y + x.m[8] * p.z };
}

V3 ToV3( const float *f ) { return { f[0], f[1], f[2] }; }

float SegDist( V3 p, V3 a, V3 b, float *tOut = nullptr ) {
	V3 ab = b - a;
	float l2 = gore::dot( ab, ab );
	float t = l2 > 1e-12f ? gore::dot( p - a, ab ) / l2 : 0.0f;
	t = t < 0 ? 0 : ( t > 1 ? 1 : t );
	if ( tOut ) {
		*tOut = t;
	}
	return gore::len( p - ( a + ab * t ) );
}

// how thick the bone in the limb from this bone to its child is (inches)
float BoneRadius( const char *name ) {
	static const struct { const char *part; float r; } bones[] = {
		{ "Spine", 0.7f }, { "Pelvis", 1.2f }, { "Neck", 0.45f }, { "Clavicle", 0.3f }, { "UpperArm", 0.42f },
		{ "Forearm", 0.35f }, { "Thigh", 0.55f }, { "Calf", 0.45f }, { "Foot", 0.3f },
	};
	for ( auto &b : bones ) {
		if ( strstr( name, b.part ) ) {
			return b.r;
		}
	}
	return 0;
}

// --------------------------------------------------------------------------- models and bodies

struct Surf {
	int numVerts = 0, numTris = 0;
	std::vector<V3> xyz, nrm;       // rest pose, metres
	std::vector<int> tri;
	// what each vertex hangs on (skeletal models): vertex v's weights are wts[wFirst[v]] to wts[wFirst[v + 1] - 1]
	struct Wt { int bone; float w; V3 off; };
	std::vector<int> wFirst;
	std::vector<Wt> wts;
};

struct Model {
	bool head = false;              // a rigid head: one bone, a skull shell, brains
	int numBones = 0;
	std::vector<Xf> rest, restInv;
	std::vector<int> parents, flags;
	std::vector<Surf> surfs;
	gore::VoxelBody vb;
	std::vector<uint8_t> island;    // filled voxels not joined to the body from the start (a flap, a pouch)
};

int rebuildsLeft = 1;               // wounds worked out again this frame (Gore_NewFrame)

// the model's own loose parts, so they're never taken for pieces shot off (or crumbs)
void MarkIslands( Model &m ) {
	gore::VoxelBody vb = m.vb;
	std::vector<gore::Piece> pcs = vb.pieces( 1 );
	m.island.assign( vb.state.size(), 0 );
	for ( size_t c = 0; c < vb.state.size(); c++ ) {
		int l = vb.labels()[c];
		if ( l >= 0 && !pcs[l].isMain ) {
			m.island[c] = 1;
		}
	}
}

// One step of cutting a triangle up round its wounds (Refiner::Run), in the order they were
// taken: how many steps it and those under it are, and which of the parts kept are theirs.
struct SkinNode { int size, emitAt, emitN; };

struct Body {
	int model = 0;
	bool piece = false;
	int sliceBone = -1;             // a knife's cut part way through a limb (Gore_Slice): its bone,
	float sliceAlong = 0;           // and how far along it
	gore::VoxelBody vb;
	std::vector<unsigned short> carvedBlocks;   // CountCarved: how many carved voxels in each block
	int cbx = 0, cby = 0, cbz = 0;
	bool dirty = true;
	std::vector<std::vector<int>> keep;
	std::vector<std::vector<float>> sub;
	// what keep and sub were last worked out from (the voxels as they were), and each
	// triangle's own share of them: a shot changes a few voxels, and only the triangles near
	// those are worked out again (BuildSkin)
	std::vector<uint8_t> seen;
	std::vector<std::vector<unsigned char>> triKeep;
	std::vector<std::vector<std::vector<float>>> triSub;
	std::vector<std::vector<std::vector<SkinNode>>> triTree;    // how each was cut up (Refiner)
	mutable std::vector<uint8_t> stays;     // CellStays' answer for each cell: 0 not asked yet, 1 stays, 2 gone
	gore::Mesh flesh;               // the wound walls, rest pose (metres)
	std::vector<float> fxyz, fnrm;
	std::vector<unsigned char> frgba;
	std::vector<int> fidx;
	vrGoreDraw_t draw;
	// what draw was worked out for: the same again (the other eye, a body lying still) is reused
	bool drawValid = false;
	std::vector<float> drawPose;
	float drawLight[9] = {};
};

std::unique_ptr<Model> models[MAX_MODELS + 1];
std::unique_ptr<Body> bodies[MAX_BODIES + 1];

Model *GetModel( int h ) { return h > 0 && h <= MAX_MODELS ? models[h].get() : nullptr; }
Body *GetBody( int h ) { return h > 0 && h <= MAX_BODIES ? bodies[h].get() : nullptr; }

int NewBodySlot() {
	for ( int i = 1; i <= MAX_BODIES; i++ ) {
		if ( !bodies[i] ) {
			return i;
		}
	}
	return 0;
}

// --------------------------------------------------------------------------- the skin's holes

// Is skin still there whose point a cell in from it is in cell x y z: that voxel is body.
// The answer for a cell outside the body takes a look all round it, and is asked over and over
// (every part a triangle is cut into asks at its corners), so it is kept (stays) until a voxel
// near it changes (BuildSkin).
bool CellStays( const Body &b, int x, int y, int z ) {
	const gore::VoxelBody &vb = b.vb;
	uint8_t *known = nullptr;
	if ( vb.in( x, y, z ) ) {
		const int i = vb.id( x, y, z );
		uint8_t s = vb.state[i];
		if ( s == gore::SOLID || s == gore::BONE ) {
			return true;
		}
		if ( s == gore::CARVED ) {
			return false;
		}
		known = &b.stays[i];
		if ( *known ) {
			return *known == 1;
		}
	}
	bool stays = !b.piece;      // nothing near: the body keeps it, a piece doesn't
	// outside the body (a thin part, under a cell across): the nearest voxel that isn't
	for ( int r = 1; r <= 2; r++ ) {
		float best = 1e9f;
		int found = -1;
		for ( int dz = -r; dz <= r; dz++ ) {
			for ( int dy = -r; dy <= r; dy++ ) {
				for ( int dx = -r; dx <= r; dx++ ) {
					int X = x + dx, Y = y + dy, Z = z + dz;
					if ( !vb.in( X, Y, Z ) ) {
						continue;
					}
					uint8_t s = vb.state[vb.id( X, Y, Z )];
					if ( s == gore::EMPTY ) {
						continue;
					}
					float d = (float)( dx * dx + dy * dy + dz * dz );
					if ( d < best ) {
						best = d;
						found = s;
					}
				}
			}
		}
		if ( found >= 0 ) {
			stays = found != gore::CARVED;
			break;
		}
	}
	if ( known ) {
		*known = stays ? 1 : 2;
	}
	return stays;
}

// is the skin at p (rest, metres, facing n) still there
bool SkinStays( const Body &b, V3 p, V3 n ) {
	const gore::VoxelBody &vb = b.vb;
	V3 q = p - n * ( vb.cell * 1.1f );     // a cell in: the cloth opens no wider than the wound under it
	return CellStays( b, (int)floorf( ( q.x - vb.origin.x ) / vb.cell ), (int)floorf( ( q.y - vb.origin.y ) / vb.cell ),
					  (int)floorf( ( q.z - vb.origin.z ) / vb.cell ) );
}

/*
Is the skin gone at every point of the triangle P (N: which way it faces at its corners): at
every cell a point a cell in from it can be in. Over a limb that has come off, say: told at
once, where cutting the triangle down to the size of a cell to find nothing left of it was a
tenth of a second's freeze.

Those points make a sheet a cell under the triangle: the triangle Q of its corners' own, bent
by no more than the way it faces differs from corner to corner (d: SkinStays goes in along
the corners' directions blended and made unit length, which is within d + d*d/5 of the same
blend of them left as it is). So every cell whose middle is within that, and half a cell's
diagonal, of Q is asked: more than the sheet touches, never fewer.
*/
bool AllGone( const Body &b, const V3 *P, const V3 *N ) {
	const gore::VoxelBody &vb = b.vb;
	const float cell = vb.cell, in = cell * 1.1f;
	const float d = std::max( gore::len( N[0] - N[1] ), std::max( gore::len( N[1] - N[2] ), gore::len( N[2] - N[0] ) ) );
	if ( d > 0.7f || gore::dot( N[0], N[0] ) < 0.98f || gore::dot( N[1], N[1] ) < 0.98f || gore::dot( N[2], N[2] ) < 0.98f ) {
		return false;       // (facing every way: asked the long way)
	}
	const V3 Q[3] = { P[0] - N[0] * in, P[1] - N[1] * in, P[2] - N[2] * in };
	const float bend = in * ( d + 0.2f * d * d ) + cell * 0.03f, reach = bend + cell * 0.87f;
	const float o[3] = { vb.origin.x, vb.origin.y, vb.origin.z };
	const float mn[3] = { std::min( Q[0].x, std::min( Q[1].x, Q[2].x ) ), std::min( Q[0].y, std::min( Q[1].y, Q[2].y ) ), std::min( Q[0].z, std::min( Q[1].z, Q[2].z ) ) };
	const float mx[3] = { std::max( Q[0].x, std::max( Q[1].x, Q[2].x ) ), std::max( Q[0].y, std::max( Q[1].y, Q[2].y ) ), std::max( Q[0].z, std::max( Q[1].z, Q[2].z ) ) };
	const int n[3] = { vb.nx, vb.ny, vb.nz };
	int lo[3], hi[3];
	for ( int k = 0; k < 3; k++ ) {
		lo[k] = (int)floorf( ( mn[k] - bend - o[k] ) / cell );
		hi[k] = (int)floorf( ( mx[k] + bend - o[k] ) / cell );
		if ( lo[k] < 0 || hi[k] >= n[k] ) {
			return false;       // (off the grid: asked the long way)
		}
	}
	for ( int z = lo[2]; z <= hi[2]; z++ ) {
		for ( int y = lo[1]; y <= hi[1]; y++ ) {
			for ( int x = lo[0]; x <= hi[0]; x++ ) {
				const V3 c = vb.centre( x, y, z );
				if ( gore::len( c - TriNearest( c, Q[0], Q[1], Q[2] ) ) <= reach && CellStays( b, x, y, z ) ) {
					return false;
				}
			}
		}
	}
	return true;
}

// Where the wounds are, to ask whether there is one in a box (Near): the carved voxels counted
// by blocks of CARVED_BLOCK cells a side. A body with a few bullet holes has a few dozen blocks
// with anything in them, found by going down its rows for the carved ones, so this costs next
// to nothing, where a table of sums over the whole grid (a million cells) cost more than
// everything else a shot did.
const int CARVED_BLOCK = 4;

void CountCarved( Body &b ) {
	const gore::VoxelBody &vb = b.vb;
	b.cbx = ( vb.nx + CARVED_BLOCK - 1 ) / CARVED_BLOCK;
	b.cby = ( vb.ny + CARVED_BLOCK - 1 ) / CARVED_BLOCK;
	b.cbz = ( vb.nz + CARVED_BLOCK - 1 ) / CARVED_BLOCK;
	b.carvedBlocks.assign( (size_t)b.cbx * b.cby * b.cbz, 0 );
	for ( int z = 0; z < vb.nz; z++ ) {
		for ( int y = 0; y < vb.ny; y++ ) {
			const uint8_t *row = &vb.state[vb.id( 0, y, z )], *end = row + vb.nx, *p = row;
			unsigned short *blocks = &b.carvedBlocks[( (size_t)( z / CARVED_BLOCK ) * b.cby + y / CARVED_BLOCK ) * b.cbx];
			while ( p < end && ( p = (const uint8_t *)memchr( p, gore::CARVED, end - p ) ) != nullptr ) {
				blocks[( p - row ) / CARVED_BLOCK]++;
				p++;
			}
		}
	}
}

// is there a wound (or a cut) within r of c: a carved voxel among those the box round c reaches
bool Near( const Body &b, V3 c, float r ) {
	const gore::VoxelBody &vb = b.vb;
	int lo[3], hi[3];
	float cc[3] = { c.x, c.y, c.z }, o[3] = { vb.origin.x, vb.origin.y, vb.origin.z };
	int n[3] = { vb.nx, vb.ny, vb.nz };
	for ( int k = 0; k < 3; k++ ) {
		lo[k] = std::max( 0, (int)floorf( ( cc[k] - r - o[k] ) / vb.cell ) );
		hi[k] = std::min( n[k], (int)floorf( ( cc[k] + r - o[k] ) / vb.cell ) + 1 );
		if ( lo[k] >= hi[k] ) {
			return false;
		}
	}
	const int B = CARVED_BLOCK;
	for ( int bz = lo[2] / B; bz <= ( hi[2] - 1 ) / B; bz++ ) {
		for ( int by = lo[1] / B; by <= ( hi[1] - 1 ) / B; by++ ) {
			for ( int bx = lo[0] / B; bx <= ( hi[0] - 1 ) / B; bx++ ) {
				if ( !b.carvedBlocks[( (size_t)bz * b.cby + by ) * b.cbx + bx] ) {
					continue;
				}
				// the part of this block inside the box: all of it, or its cells looked at
				int x0 = std::max( lo[0], bx * B ), x1 = std::min( hi[0], bx * B + B );
				int y0 = std::max( lo[1], by * B ), y1 = std::min( hi[1], by * B + B );
				int z0 = std::max( lo[2], bz * B ), z1 = std::min( hi[2], bz * B + B );
				if ( x0 == bx * B && y0 == by * B && z0 == bz * B && x1 == std::min( n[0], bx * B + B ) &&
					 y1 == std::min( n[1], by * B + B ) && z1 == std::min( n[2], bz * B + B ) ) {
					return true;
				}
				for ( int z = z0; z < z1; z++ ) {
					for ( int y = y0; y < y1; y++ ) {
						if ( memchr( &vb.state[vb.id( x0, y, z )], gore::CARVED, x1 - x0 ) ) {
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

// The voxels that changed since the skin was last worked out: which they are, a bit for each
// of a block's 64 (blocks as CountCarved's), and the box round them all; and seen brought up
// to date. false: none did.
struct Changed {
	std::vector<uint64_t> block;
	int lo[3], hi[3];       // cells, hi past the last
};

inline int BlockBit( int x, int y, int z ) {
	return ( x % CARVED_BLOCK ) + CARVED_BLOCK * ( ( y % CARVED_BLOCK ) + CARVED_BLOCK * ( z % CARVED_BLOCK ) );
}

bool FindChanged( Body &b, Changed &ch ) {
	const gore::VoxelBody &vb = b.vb;
	const size_t slice = (size_t)vb.nx * vb.ny;
	ch.block.assign( b.carvedBlocks.size(), 0 );
	for ( int k = 0; k < 3; k++ ) {
		ch.lo[k] = 1 << 30;
		ch.hi[k] = -1;
	}
	for ( int z = 0; z < vb.nz; z++ ) {
		if ( !memcmp( &vb.state[z * slice], &b.seen[z * slice], slice ) ) {
			continue;
		}
		for ( int y = 0; y < vb.ny; y++ ) {
			const uint8_t *now = &vb.state[vb.id( 0, y, z )];
			uint8_t *was = &b.seen[vb.id( 0, y, z )];
			if ( !memcmp( now, was, vb.nx ) ) {
				continue;
			}
			int x0 = 0, x1 = vb.nx - 1;
			while ( now[x0] == was[x0] ) {
				x0++;
			}
			while ( now[x1] == was[x1] ) {
				x1--;
			}
			uint64_t *blocks = &ch.block[( (size_t)( z / CARVED_BLOCK ) * b.cby + y / CARVED_BLOCK ) * b.cbx];
			for ( int x = x0; x <= x1; x++ ) {
				if ( now[x] != was[x] ) {
					was[x] = now[x];
					blocks[x / CARVED_BLOCK] |= (uint64_t)1 << BlockBit( x, y, z );
				}
			}
			ch.lo[0] = std::min( ch.lo[0], x0 );
			ch.hi[0] = std::max( ch.hi[0], x1 + 1 );
			ch.lo[1] = std::min( ch.lo[1], y );
			ch.hi[1] = std::max( ch.hi[1], y + 1 );
			ch.lo[2] = std::min( ch.lo[2], z );
			ch.hi[2] = std::max( ch.hi[2], z + 1 );
		}
	}
	return ch.hi[0] >= 0;
}

// did any voxel within r of the triangle P's box change
bool NearChanged( const Body &b, const Changed &ch, const V3 *P, float r ) {
	const gore::VoxelBody &vb = b.vb;
	int lo[3], hi[3];
	const float o[3] = { vb.origin.x, vb.origin.y, vb.origin.z };
	const float mn[3] = { std::min( P[0].x, std::min( P[1].x, P[2].x ) ), std::min( P[0].y, std::min( P[1].y, P[2].y ) ), std::min( P[0].z, std::min( P[1].z, P[2].z ) ) };
	const float mx[3] = { std::max( P[0].x, std::max( P[1].x, P[2].x ) ), std::max( P[0].y, std::max( P[1].y, P[2].y ) ), std::max( P[0].z, std::max( P[1].z, P[2].z ) ) };
	for ( int k = 0; k < 3; k++ ) {
		lo[k] = std::max( ch.lo[k], (int)floorf( ( mn[k] - r - o[k] ) / vb.cell ) );
		hi[k] = std::min( ch.hi[k], (int)floorf( ( mx[k] + r - o[k] ) / vb.cell ) + 1 );
		if ( lo[k] >= hi[k] ) {
			return false;       // (nowhere near any of it: nearly every triangle)
		}
	}
	const int B = CARVED_BLOCK;
	for ( int bz = lo[2] / B; bz <= ( hi[2] - 1 ) / B; bz++ ) {
		for ( int by = lo[1] / B; by <= ( hi[1] - 1 ) / B; by++ ) {
			for ( int bx = lo[0] / B; bx <= ( hi[0] - 1 ) / B; bx++ ) {
				const uint64_t changed = ch.block[( (size_t)bz * b.cby + by ) * b.cbx + bx];
				if ( !changed ) {
					continue;
				}
				// the cells of this block inside the box
				int x0 = std::max( lo[0], bx * B ), x1 = std::min( hi[0], bx * B + B );
				int y0 = std::max( lo[1], by * B ), y1 = std::min( hi[1], by * B + B );
				int z0 = std::max( lo[2], bz * B ), z1 = std::min( hi[2], bz * B + B );
				const uint64_t row = ( ( (uint64_t)1 << ( x1 - x0 ) ) - 1 ) << ( x0 - bx * B );
				uint64_t box = 0;
				for ( int z = z0; z < z1; z++ ) {
					for ( int y = y0; y < y1; y++ ) {
						box |= row << BlockBit( 0, y, z );
					}
				}
				if ( changed & box ) {
					return true;
				}
			}
		}
	}
	return false;
}

// Cuts a triangle of the skin up round the wounds under it, finer and finer, and keeps the
// parts with body still under them. How it went is kept (tree, with out), and the next time
// any part of it no changed voxel can reach is taken over as it was, with all under it: a new
// hole in a triangle that has ten already is worked out round the new one only.
struct Refiner {
	const Body *b;
	const V3 *X, *N;        // the triangle's corners
	float cell;
	const Changed *ch;      // what changed since oldTree and oldOut were made
	const std::vector<SkinNode> *oldTree;
	const std::vector<float> *oldOut;
	std::vector<SkinNode> *tree;
	std::vector<float> *out;
	int tri;

	// How far outside a part (the box round its corners) a voxel can be and still count for it,
	// or for anything it is cut into. SkinStays looks two cells round a point 1.1 cells in from
	// one on it: 3.2 cells. Near looks round the middle of it, or of a part of it, as far as
	// that part's corners and half a cell: no part's corner is further from its middle than
	// two thirds of its longest side, and none has a longer side than the whole.
	static float Reach( const V3 *P, float cell ) {
		float side = std::max( gore::len( P[1] - P[2] ), std::max( gore::len( P[2] - P[0] ), gore::len( P[0] - P[1] ) ) );
		return std::max( 3.2f * cell, side * 0.67f + 0.6f * cell );
	}

	void Done( int me ) {
		( *tree )[me].size = (int)tree->size() - me;
		( *tree )[me].emitN = (int)( out->size() / 10 ) - ( *tree )[me].emitAt;
	}

	V3 At( const float *w, const V3 *v ) const { return v[0] * w[0] + v[1] * w[1] + v[2] * w[2]; }

	void Emit( float B[3][3] ) {
		out->push_back( (float)tri );
		for ( int k = 0; k < 3; k++ ) {
			for ( int j = 0; j < 3; j++ ) {
				out->push_back( B[k][j] );
			}
		}
	}

	// returns 1 if the whole node stays (only asked of the top), else emits what stays.
	// old: this same part in oldTree, or -1 (it wasn't cut this far before)
	int Run( float B[3][3], int depth, int old ) {
		V3 P[3], Nn[3];
		for ( int k = 0; k < 3; k++ ) {
			P[k] = At( B[k], X );
		}
		V3 c = ( P[0] + P[1] + P[2] ) * ( 1.0f / 3 );
		float rad = std::max( gore::len( P[0] - c ), std::max( gore::len( P[1] - c ), gore::len( P[2] - c ) ) );
		const int me = (int)tree->size(), emitAt = (int)( out->size() / 10 );
		if ( old >= 0 && depth > 0 && !NearChanged( *b, *ch, P, Reach( P, cell ) ) ) {
			// nothing under it changed: as it was, and all it was cut into
			const SkinNode o = ( *oldTree )[old];
			for ( int i = 0; i < o.size; i++ ) {
				SkinNode n = ( *oldTree )[old + i];
				n.emitAt += emitAt - o.emitAt;
				tree->push_back( n );
			}
			out->insert( out->end(), oldOut->begin() + (size_t)o.emitAt * 10, oldOut->begin() + (size_t)( o.emitAt + o.emitN ) * 10 );
			return 0;
		}
		tree->push_back( SkinNode{ 1, emitAt, 0 } );
		for ( int k = 0; k < 3; k++ ) {
			Nn[k] = gore::norm( At( B[k], N ) );
		}
		V3 nc = gore::norm( Nn[0] + Nn[1] + Nn[2] );
		bool kc = SkinStays( *b, c, nc );
		bool k0 = SkinStays( *b, P[0], Nn[0] ), k1 = SkinStays( *b, P[1], Nn[1] ), k2 = SkinStays( *b, P[2], Nn[2] );
		bool same = k0 == kc && k1 == kc && k2 == kc;
		if ( same && !Near( *b, c, rad + cell * 0.5f ) ) {
			if ( !kc ) {
				return 0;
			}
			if ( depth == 0 ) {
				return 1;
			}
			Emit( B );
			Done( me );
			return 0;
		}
		if ( same && !kc && AllGone( *b, P, Nn ) ) {
			return 0;       // nothing of it stays, however fine it were cut
		}
		float e[3] = { gore::len( P[1] - P[2] ), gore::len( P[2] - P[0] ), gore::len( P[0] - P[1] ) };
		int longest = e[0] > e[1] ? ( e[0] > e[2] ? 0 : 2 ) : ( e[1] > e[2] ? 1 : 2 );
		if ( e[longest] < cell || depth >= 10 ) {
			if ( kc ) {
				Emit( B );
				Done( me );
			}
			return 0;
		}
		// split the longest edge (between the other two corners)
		int i = ( longest + 1 ) % 3, j = ( longest + 2 ) % 3;
		float M[3], C1[3][3], C2[3][3];
		for ( int k = 0; k < 3; k++ ) {
			M[k] = ( B[i][k] + B[j][k] ) * 0.5f;
		}
		memcpy( C1, B, sizeof( C1 ) );
		memcpy( C2, B, sizeof( C2 ) );
		memcpy( C1[j], M, sizeof( M ) );
		memcpy( C2[i], M, sizeof( M ) );
		// (the same two parts it was cut into before, if it was: where it is cut is its shape's)
		const bool had = old >= 0 && ( *oldTree )[old].size > 1;
		const int old1 = had ? old + 1 : -1, old2 = had ? old1 + ( *oldTree )[old1].size : -1;
		Run( C1, depth + 1, old1 );
		Run( C2, depth + 1, old2 );
		Done( me );
		return 0;
	}
};

// does any cell within r of the triangle X's box lie in the box of cells lo..hi
bool ReachesBox( const gore::VoxelBody &vb, const V3 *X, float r, const int *lo, const int *hi ) {
	const float o[3] = { vb.origin.x, vb.origin.y, vb.origin.z };
	const float mn[3] = { std::min( X[0].x, std::min( X[1].x, X[2].x ) ), std::min( X[0].y, std::min( X[1].y, X[2].y ) ), std::min( X[0].z, std::min( X[1].z, X[2].z ) ) };
	const float mx[3] = { std::max( X[0].x, std::max( X[1].x, X[2].x ) ), std::max( X[0].y, std::max( X[1].y, X[2].y ) ), std::max( X[0].z, std::max( X[1].z, X[2].z ) ) };
	for ( int k = 0; k < 3; k++ ) {
		if ( (int)floorf( ( mx[k] + r - o[k] ) / vb.cell ) < lo[k] || (int)floorf( ( mn[k] - r - o[k] ) / vb.cell ) > hi[k] ) {
			return false;
		}
	}
	return true;
}

/*
Which of the skin's triangles stay whole (keep), and what is left of the others (sub), by the
voxels under them. A triangle's answer depends only on the voxels close to it, so after the
first time only those near a voxel that changed since are asked again, and of those only the
parts the change can reach (Refiner): a shot is worked out round its own hole, not the whole
body's thousands of triangles cut finer and finer round every wound it has. (That was most of
a frame for every bullet that hit, more the more wounds he had.)
*/
void BuildSkin( Body &b, const Model &m ) {
	const float cell = b.vb.cell;
	Changed ch;
	std::vector<float> out;
	std::vector<SkinNode> tree;
	// a piece shot off: the box round what there is of it (cells). Nearly all the body's skin
	// is nowhere near that, and gone: told by the box, not by asking after every bit of it.
	int pieceLo[3] = { 1 << 30, 1 << 30, 1 << 30 }, pieceHi[3] = { -1, -1, -1 };
	if ( b.piece ) {
		const gore::VoxelBody &vb = b.vb;
		for ( int z = 0; z < vb.nz; z++ ) {
			for ( int y = 0; y < vb.ny; y++ ) {
				const uint8_t *row = &vb.state[vb.id( 0, y, z )];
				int x0 = 0, x1 = vb.nx - 1;
				while ( x0 <= x1 && row[x0] != gore::SOLID && row[x0] != gore::BONE ) {
					x0++;
				}
				if ( x0 > x1 ) {
					continue;
				}
				while ( row[x1] != gore::SOLID && row[x1] != gore::BONE ) {
					x1--;
				}
				pieceLo[0] = std::min( pieceLo[0], x0 );
				pieceHi[0] = std::max( pieceHi[0], x1 );
				pieceLo[1] = std::min( pieceLo[1], y );
				pieceHi[1] = std::max( pieceHi[1], y );
				pieceLo[2] = std::min( pieceLo[2], z );
				pieceHi[2] = std::max( pieceHi[2], z );
			}
		}
	}
	bool all = b.seen.size() != b.vb.state.size() || b.triKeep.size() != m.surfs.size();

	if ( all ) {
		b.seen = b.vb.state;
		b.stays.assign( b.vb.state.size(), 0 );
		b.triKeep.assign( m.surfs.size(), std::vector<unsigned char>() );
		b.triSub.assign( m.surfs.size(), std::vector<std::vector<float>>() );
		b.triTree.assign( m.surfs.size(), std::vector<std::vector<SkinNode>>() );
		for ( size_t s = 0; s < m.surfs.size(); s++ ) {
			b.triKeep[s].assign( m.surfs[s].numTris, 0 );
			b.triSub[s].assign( m.surfs[s].numTris, std::vector<float>() );
			b.triTree[s].assign( m.surfs[s].numTris, std::vector<SkinNode>() );
		}
	} else {
		if ( !FindChanged( b, ch ) && b.keep.size() == m.surfs.size() ) {
			return;     // nothing did: as it was
		}
		// what CellStays said of the cells within its reach (two) of a changed one: to be asked again
		const gore::VoxelBody &vb = b.vb;
		const int x0 = std::max( 0, ch.lo[0] - 2 ), x1 = std::min( vb.nx, ch.hi[0] + 2 );
		for ( int z = std::max( 0, ch.lo[2] - 2 ); z < std::min( vb.nz, ch.hi[2] + 2 ); z++ ) {
			for ( int y = std::max( 0, ch.lo[1] - 2 ); y < std::min( vb.ny, ch.hi[1] + 2 ); y++ ) {
				if ( x1 > x0 ) {
					memset( &b.stays[vb.id( x0, y, z )], 0, x1 - x0 );
				}
			}
		}
	}
	b.keep.assign( m.surfs.size(), std::vector<int>() );
	b.sub.assign( m.surfs.size(), std::vector<float>() );
	for ( size_t s = 0; s < m.surfs.size(); s++ ) {
		const Surf &sf = m.surfs[s];
		Refiner r;
		r.b = &b;
		r.cell = cell;
		r.ch = &ch;
		r.tree = &tree;
		r.out = &out;
		for ( int t = 0; t < sf.numTris; t++ ) {
			std::vector<float> &pieces = b.triSub[s][t];
			std::vector<SkinNode> &cuts = b.triTree[s][t];
			V3 X[3], N[3];
			for ( int k = 0; k < 3; k++ ) {
				X[k] = sf.xyz[sf.tri[t * 3 + k]];
				N[k] = sf.nrm[sf.tri[t * 3 + k]];
			}
			V3 c = ( X[0] + X[1] + X[2] ) * ( 1.0f / 3 );
			float rad = std::max( gore::len( X[0] - c ), std::max( gore::len( X[1] - c ), gore::len( X[2] - c ) ) );
			if ( all || NearChanged( b, ch, X, Refiner::Reach( X, cell ) ) ) {
				out.clear();
				tree.clear();
				if ( !b.piece && !Near( b, c, rad + 3.2f * cell ) ) {
					// nowhere near a wound: it stays, told without asking after the voxels under
					// its corners (SkinStays could only say yes). A piece shot off is asked the
					// long way: its skin goes where there's no voxel at all.
					b.triKeep[s][t] = 1;
				} else if ( b.piece && !ReachesBox( b.vb, X, 3.2f * cell, pieceLo, pieceHi ) ) {
					// a piece, and nothing of it within SkinStays' reach of this: gone
					b.triKeep[s][t] = 0;
				} else {
					float B[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
					r.X = X;
					r.N = N;
					r.tri = t;
					r.oldTree = &cuts;
					r.oldOut = &pieces;
					b.triKeep[s][t] = (unsigned char)r.Run( B, 0, all || cuts.empty() ? -1 : 0 );
				}
				pieces.assign( out.begin(), out.end() );
				cuts.assign( tree.begin(), tree.end() );
			}
			if ( b.triKeep[s][t] ) {
				b.keep[s].push_back( t );
			} else {
				b.sub[s].insert( b.sub[s].end(), pieces.begin(), pieces.end() );
			}
		}
	}
}

void Rebuild( Body &b ) {
	const Model *m = GetModel( b.model );
	if ( !m ) {
		return;
	}
	CountCarved( b );
	BuildSkin( b, *m );
	b.flesh = b.vb.remesh( true );
	b.dirty = false;
}

}   // namespace

// =========================================================================== the C interface

extern "C" {

void Gore_NewFrame( void ) {
	rebuildsLeft = 1;
}

void Gore_Shutdown( void ) {
	for ( auto &m : models ) {
		m.reset();
	}
	for ( auto &b : bodies ) {
		b.reset();
	}
}

int Gore_LoadModel( const void *data, int size, const float *restPose, int numBones,
					const int *parents, const int *flags, const char *names, float cell ) {
	const MdsHeader *h = (const MdsHeader *)data;
	int slot = 0;

	if ( !data || size < (int)sizeof( MdsHeader ) || h->ident != ( ( 'W' << 24 ) + ( 'S' << 16 ) + ( 'D' << 8 ) + 'M' ) ||
		 h->version != 4 || h->numBones != numBones || numBones > MAX_BONES || h->numSurfaces > VRG_MAX_SURFS ||
		 h->ofsEnd > size ) {
		return 0;
	}
	for ( int i = 1; i <= MAX_MODELS; i++ ) {
		if ( !models[i] ) {
			slot = i;
			break;
		}
	}
	if ( !slot ) {
		return 0;
	}
	std::unique_ptr<Model> m( new Model );
	m->numBones = numBones;
	for ( int i = 0; i < numBones; i++ ) {
		m->rest.push_back( PoseBone( restPose, i ) );
		m->restInv.push_back( Inverse( m->rest.back() ) );
		m->parents.push_back( parents[i] );
		m->flags.push_back( flags[i] );
	}

	// the skin in the rest pose, as the renderer would draw it
	gore::Body body;
	const char *base = (const char *)data;
	const MdsSurface *s = (const MdsSurface *)( base + h->ofsSurfaces );
	for ( int si = 0; si < h->numSurfaces; si++ ) {
		if ( (const char *)s < base || (const char *)s + sizeof( MdsSurface ) > base + size ) {
			return 0;
		}
		Surf sf;
		sf.numVerts = s->numVerts;
		sf.numTris = s->numTriangles;
		uint32_t first = (uint32_t)body.pos.size();
		const MdsVertex *v = (const MdsVertex *)( (const char *)s + s->ofsVerts );
		for ( int j = 0; j < s->numVerts; j++ ) {
			V3 p { 0, 0, 0 };
			float best = -1;
			int bone = 0;
			sf.wFirst.push_back( (int)sf.wts.size() );
			for ( int k = 0; k < v->numWeights; k++ ) {
				const MdsWeight &w = v->weights[k];
				if ( w.boneIndex < 0 || w.boneIndex >= numBones ) {
					return 0;
				}
				sf.wts.push_back( Surf::Wt { w.boneIndex, w.boneWeight, ToV3( w.offset ) } );
				p = p + Point( m->rest[w.boneIndex], ToV3( w.offset ) ) * w.boneWeight;
				if ( w.boneWeight > best ) {
					best = w.boneWeight;
					bone = w.boneIndex;
				}
			}
			V3 n = v->numWeights > 0 ? gore::norm( Dir( m->rest[v->weights[0].boneIndex], ToV3( v->normal ) ) ) : V3 { 0, 0, 1 };
			sf.xyz.push_back( p * METRES );
			sf.nrm.push_back( n );
			body.pos.push_back( p * METRES );
			body.bone.push_back( (uint8_t)bone );
			v = (const MdsVertex *)&v->weights[v->numWeights];
		}
		sf.wFirst.push_back( (int)sf.wts.size() );
		const int *tri = (const int *)( (const char *)s + s->ofsTriangles );
		for ( int t = 0; t < s->numTriangles * 3; t++ ) {
			if ( tri[t] < 0 || tri[t] >= s->numVerts ) {
				return 0;
			}
			sf.tri.push_back( tri[t] );
			body.idx.push_back( first + tri[t] );
		}
		m->surfs.push_back( std::move( sf ) );
		s = (const MdsSurface *)( (const char *)s + s->ofsEnd );
	}
	if ( !m->vb.build( body, cell * METRES ) ) {
		return 0;
	}
	// the bones inside it
	for ( int i = 0; i < numBones; i++ ) {
		int p = parents[i];
		if ( p < 0 || ( flags[i] & 1 ) || ( flags[p] & 1 ) ) {
			continue;
		}
		float r = BoneRadius( names + p * 32 );
		if ( r > 0 ) {
			m->vb.addBoneRod( ToV3( m->rest[p].t ) * METRES, ToV3( m->rest[i].t ) * METRES, r * METRES, (uint8_t)p );
		}
	}
	MarkIslands( *m );
	models[slot] = std::move( m );
	return slot;
}

static V3 LatLong( short n ) {
	float lat = ( ( n >> 8 ) & 255 ) * ( 2 * 3.14159265f / 255 ), lng = ( n & 255 ) * ( 2 * 3.14159265f / 255 );
	return { cosf( lat ) * sinf( lng ), sinf( lat ) * sinf( lng ), cosf( lng ) };
}

int Gore_LoadHead( const void *data, int size, float cell ) {
	const char *base = (const char *)data;
	int ident = size >= 4 ? *(const int *)data : 0, slot = 0, numSurfaces;
	const int MD3 = ( '3' << 24 ) + ( 'P' << 16 ) + ( 'D' << 8 ) + 'I', MDC = ( 'C' << 24 ) + ( 'P' << 16 ) + ( 'D' << 8 ) + 'I';
	const char *surf;

	if ( ident == MD3 && size >= (int)sizeof( Md3Header ) ) {
		numSurfaces = ( (const Md3Header *)data )->numSurfaces;
		surf = base + ( (const Md3Header *)data )->ofsSurfaces;
	} else if ( ident == MDC && size >= (int)sizeof( MdcHeader ) ) {
		numSurfaces = ( (const MdcHeader *)data )->numSurfaces;
		surf = base + ( (const MdcHeader *)data )->ofsSurfaces;
	} else {
		return 0;
	}
	if ( numSurfaces <= 0 || numSurfaces > VRG_MAX_SURFS ) {
		return 0;
	}
	for ( int i = 1; i <= MAX_MODELS; i++ ) {
		if ( !models[i] ) {
			slot = i;
			break;
		}
	}
	if ( !slot ) {
		return 0;
	}
	std::unique_ptr<Model> m( new Model );
	m->head = true;
	m->numBones = 1;
	Xf id = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 0, 0 } };
	m->rest.push_back( id );
	m->restInv.push_back( id );
	m->parents.push_back( -1 );
	m->flags.push_back( 0 );

	gore::Body body;
	for ( int si = 0; si < numSurfaces; si++ ) {
		if ( surf < base || surf + sizeof( MdcSurface ) > base + size ) {
			return 0;
		}
		Surf sf;
		int numVerts, numTris, ofsTris, ofsEnd;
		const Md3XyzNormal *xyz;
		const unsigned *comp = nullptr;
		if ( ident == MD3 ) {
			const Md3Surface *s = (const Md3Surface *)surf;
			numVerts = s->numVerts; numTris = s->numTriangles; ofsTris = s->ofsTriangles; ofsEnd = s->ofsEnd;
			xyz = (const Md3XyzNormal *)( surf + s->ofsXyzNormals );       // frame 0
		} else {
			const MdcSurface *s = (const MdcSurface *)surf;
			numVerts = s->numVerts; numTris = s->numTriangles; ofsTris = s->ofsTriangles; ofsEnd = s->ofsEnd;
			short baseFrame = ( (const short *)( surf + s->ofsFrameBaseFrames ) )[0];
			short compFrame = ( (const short *)( surf + s->ofsFrameCompFrames ) )[0];
			xyz = (const Md3XyzNormal *)( surf + s->ofsXyzNormals ) + baseFrame * numVerts;
			if ( compFrame >= 0 ) {
				comp = (const unsigned *)( surf + s->ofsXyzCompressed ) + compFrame * numVerts;
			}
		}
		if ( (const char *)( xyz + numVerts ) > base + size || surf + ofsTris + numTris * 12 > base + size ) {
			return 0;
		}
		sf.numVerts = numVerts;
		sf.numTris = numTris;
		uint32_t first = (uint32_t)body.pos.size();
		for ( int j = 0; j < numVerts; j++ ) {
			V3 p { xyz[j].xyz[0] / 64.0f, xyz[j].xyz[1] / 64.0f, xyz[j].xyz[2] / 64.0f };
			if ( comp ) {       // tr_local.h R_MDC_DecodeXyzCompressed
				unsigned o = comp[j];
				p = p + V3 { ( (float)( o & 255 ) - 127.0f ) * 0.05f, ( (float)( ( o >> 8 ) & 255 ) - 127.0f ) * 0.05f,
							 ( (float)( ( o >> 16 ) & 255 ) - 127.0f ) * 0.05f };
			}
			sf.xyz.push_back( p * METRES );
			sf.nrm.push_back( LatLong( xyz[j].normal ) );
			body.pos.push_back( p * METRES );
			body.bone.push_back( 0 );
		}
		const int *tri = (const int *)( surf + ofsTris );
		for ( int t = 0; t < numTris * 3; t++ ) {
			if ( tri[t] < 0 || tri[t] >= numVerts ) {
				return 0;
			}
			sf.tri.push_back( tri[t] );
			body.idx.push_back( first + tri[t] );
		}
		m->surfs.push_back( std::move( sf ) );
		surf += ofsEnd;
	}
	if ( !m->vb.build( body, cell * METRES ) ) {
		return 0;
	}
	// the skull: a shell from just under the scalp to 2.5 cm in
	for ( size_t i = 0; i < m->vb.state.size(); i++ ) {
		float d = m->vb.depth[i] * m->vb.cell;
		if ( m->vb.state[i] == gore::SOLID && d >= 0.006f && d <= 0.025f ) {
			m->vb.state[i] = gore::BONE;
		}
	}
	MarkIslands( *m );
	models[slot] = std::move( m );
	return slot;
}

void Gore_ModelCentre( int model, float out[3] ) {
	Model *m = GetModel( model );
	out[0] = out[1] = out[2] = 0;
	if ( m ) {
		const gore::VoxelBody &vb = m->vb;
		V3 c = ( vb.origin + V3 { vb.nx * 0.5f, vb.ny * 0.5f, vb.nz * 0.5f } * vb.cell ) * ( 1.0f / METRES );
		memcpy( out, &c, sizeof( float ) * 3 );
	}
}

int Gore_NewBody( int model ) {
	Model *m = GetModel( model );
	int slot = NewBodySlot();
	if ( !m || !slot ) {
		return 0;
	}
	bodies[slot].reset( new Body );
	bodies[slot]->model = model;
	bodies[slot]->vb = m->vb;
	return slot;
}

void Gore_FreeBody( int body ) {
	if ( GetBody( body ) ) {
		bodies[body].reset();
	}
}

int Gore_BodyModel( int body ) {
	Body *b = GetBody( body );
	return b ? b->model : 0;
}

int Gore_BodiesInUse( void ) {
	int n = 0;
	for ( auto &b : bodies ) {
		n += b ? 1 : 0;
	}
	return n;
}

int Gore_Shot( int body, const float *pose, const float point[3], const float dir[3],
			   float radiusIn, float radiusOut, float energy, goreShot_t *out ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	goreShot_t res;

	memset( &res, 0, sizeof( res ) );
	if ( out ) {
		*out = res;
	}
	if ( !m || !pose || b->piece ) {
		return 0;
	}
	// the limb it hit: the bone segment nearest the point, moving with the parent bone
	V3 p = ToV3( point ), d = gore::norm( ToV3( dir ) );
	int bone = -1, child = -1;
	float best = 1e9f;
	for ( int i = 0; i < m->numBones; i++ ) {
		int pa = m->parents[i];
		if ( pa < 0 || ( m->flags[i] & 1 ) || ( m->flags[pa] & 1 ) ) {
			continue;
		}
		float dist = SegDist( p, ToV3( pose + pa * 12 + 9 ), ToV3( pose + i * 12 + 9 ) );
		if ( dist < best ) {
			best = dist;
			bone = pa;
			child = i;
		}
	}
	if ( m->numBones == 1 ) {
		bone = child = 0;   // rigid
	}
	if ( bone < 0 ) {
		return 0;
	}
	Xf toRest = Mul( m->rest[bone], Inverse( PoseBone( pose, bone ) ) );
	Xf toPose = Inverse( toRest );
	V3 pr = Point( toRest, p ) * METRES, dr = gore::norm( Dir( toRest, d ) );
	const float back = 0.35f;       // start outside the body
	V3 entry = pr - dr * back;
	gore::Wound w = b->vb.carve( entry, dr, radiusIn, radiusOut, energy );
	if ( !w.cellsRemoved ) {
		// it missed the drawn body (hit boxes are bigger): aimed at the bone it passed, if close
		V3 a = ToV3( m->rest[bone].t ) * METRES, c = ToV3( m->rest[child].t ) * METRES;
		if ( m->numBones == 1 ) {       // a head: aimed at its middle
			a = c = b->vb.origin + V3 { b->vb.nx * 0.5f, b->vb.ny * 0.5f, b->vb.nz * 0.5f } * b->vb.cell;
		}
		float t;
		V3 onRay = pr + dr * gore::dot( a - pr, dr );
		SegDist( onRay, a, c, &t );
		V3 onBone = a + ( c - a ) * t;
		V3 off = onBone - pr;
		off = off - dr * gore::dot( off, dr );
		if ( gore::len( off ) > 0.08f ) {
			return 0;
		}
		entry = entry + off;
		w = b->vb.carve( entry, dr, radiusIn, radiusOut, energy );
		if ( !w.cellsRemoved ) {
			return 0;
		}
	}
	b->dirty = true;

	res.cells = w.cellsRemoved;
	res.boneCells = w.boneCellsHit;
	res.exited = w.exited;
	res.length = w.length / METRES;
	// where the skin was met: the first voxel along the path that isn't outside the body
	float t0 = back;
	for ( float t = 0; t < back + 0.05f; t += b->vb.cell * 0.5f ) {
		V3 q = entry + dr * t;
		int x = (int)floorf( ( q.x - b->vb.origin.x ) / b->vb.cell ), y = (int)floorf( ( q.y - b->vb.origin.y ) / b->vb.cell ),
			z = (int)floorf( ( q.z - b->vb.origin.z ) / b->vb.cell );
		if ( b->vb.in( x, y, z ) && b->vb.state[b->vb.id( x, y, z )] != gore::EMPTY ) {
			t0 = t;
			break;
		}
	}
	V3 r0 = ( entry + dr * t0 ) * ( 1.0f / METRES ), r1 = ( entry + dr * ( t0 + w.length ) ) * ( 1.0f / METRES );
	V3 e0 = Point( toPose, r0 ), e1 = Point( toPose, r1 );
	memcpy( res.entry, &e0, sizeof( res.entry ) );
	memcpy( res.exit, &e1, sizeof( res.exit ) );
	res.bone = bone;
	memcpy( res.restEntry, &r0, sizeof( res.restEntry ) );
	memcpy( res.restExit, &r1, sizeof( res.restExit ) );
	memcpy( res.restDir, &dr, sizeof( res.restDir ) );
	if ( out ) {
		*out = res;
	}
	return res.cells;
}

// the bone segment (bone to child) nearest a point of the pose, as Gore_Shot picks it
static bool NearBone( Model *m, const float *pose, V3 p, int &bone, int &child ) {
	float best = 1e9f;
	bone = child = -1;
	for ( int i = 0; i < m->numBones; i++ ) {
		int pa = m->parents[i];
		if ( pa < 0 || ( m->flags[i] & 1 ) || ( m->flags[pa] & 1 ) ) {
			continue;
		}
		float dist = SegDist( p, ToV3( pose + pa * 12 + 9 ), ToV3( pose + i * 12 + 9 ) );
		if ( dist < best ) {
			best = dist;
			bone = pa;
			child = i;
		}
	}
	return bone >= 0;
}

int Gore_NearBone( int body, const float *pose, const float point[3] ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	int bone, child;
	if ( !m || !pose || m->numBones <= 1 || !NearBone( m, pose, ToV3( point ), bone, child ) ) {
		return -1;
	}
	return bone;
}

// the first voxel still there along a path (metres, rest pose), of those ok passes
} // extern "C"
template <class F> static bool FirstCell( const gore::VoxelBody &vb, V3 from, V3 d, float far, F ok, V3 &hit ) {
	for ( float t = 0; t < far; t += vb.cell * 0.5f ) {
		V3 q = from + d * t;
		int x = (int)floorf( ( q.x - vb.origin.x ) / vb.cell ), y = (int)floorf( ( q.y - vb.origin.y ) / vb.cell ),
			z = (int)floorf( ( q.z - vb.origin.z ) / vb.cell );
		if ( vb.in( x, y, z ) && vb.filled( vb.id( x, y, z ) ) && ok( vb.centre( x, y, z ) ) ) {
			hit = vb.centre( x, y, z );
			return true;
		}
	}
	return false;
}
extern "C" {

int Gore_Slice( int body, const float *pose, const float point[3], const float dir[3],
				float limbRadius, float reach, float roll, goreShot_t *out, int *through ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	goreShot_t res;
	int bone, child, cells = 0, boneCells = 0;

	memset( &res, 0, sizeof( res ) );
	if ( out ) {
		*out = res;
	}
	if ( through ) {
		*through = 0;
	}
	if ( !m || !pose || b->piece || m->numBones <= 1 || !NearBone( m, pose, ToV3( point ), bone, child ) ) {
		return 0;
	}
	gore::VoxelBody &vb = b->vb;
	Xf toRest = Mul( m->rest[bone], Inverse( PoseBone( pose, bone ) ) );
	Xf toPose = Inverse( toRest );
	V3 pr = Point( toRest, ToV3( point ) ) * METRES, dr = gore::norm( Dir( toRest, gore::norm( ToV3( dir ) ) ) );
	V3 a = ToV3( m->rest[bone].t ) * METRES, c = ToV3( m->rest[child].t ) * METRES;
	V3 axis = gore::norm( c - a ), h;
	float L = gore::len( c - a );
	const float back = 0.35f;

	if ( limbRadius > 0 ) {
		// across the limb: a thin slab square to it, as much of it as the blade reaches from
		// where it meets what's left. The next blow nearby goes on in the same cut.
		float along = gore::dot( pr - a, axis ), R = limbRadius * METRES, half = vb.cell;
		if ( L < 0.01f ) {
			return 0;
		}
		along = along < 0 ? 0 : ( along > L ? L : along );
		if ( b->sliceBone == bone && fabsf( along - b->sliceAlong ) < 3.0f * METRES ) {
			along = b->sliceAlong;
		}
		auto slab = [&]( V3 q ) {
			V3 d = q - a;
			float al = gore::dot( d, axis );
			return fabsf( al - along ) <= half && gore::len( d - axis * al ) <= R;
		};
		V3 mid = a + axis * along;
		V3 pp = pr + axis * ( along - gore::dot( pr - a, axis ) );
		V3 dp = dr - axis * gore::dot( dr, axis );
		if ( gore::len( dp ) < 0.3f ) {
			dp = mid - pp;      // (stabbed along the limb: in from the side it was hit on)
		}
		dp = gore::norm( dp );
		if ( gore::len( dp ) < 0.5f ) {
			return 0;
		}
		if ( !FirstCell( vb, pp - dp * back, dp, back + 0.4f, slab, h ) &&
			 !FirstCell( vb, mid - dp * back, dp, back + 0.4f, slab, h ) ) {
			return 0;
		}
		float r = reach * METRES;
		bool left = false;
		for ( int z = 0; z < vb.nz; z++ ) {
			for ( int y = 0; y < vb.ny; y++ ) {
				for ( int x = 0; x < vb.nx; x++ ) {
					int i = vb.id( x, y, z );
					if ( !vb.filled( i ) ) {
						continue;
					}
					V3 q = vb.centre( x, y, z );
					if ( !slab( q ) ) {
						continue;
					}
					if ( gore::len( q - h ) > r ) {
						left = true;
						continue;
					}
					boneCells += vb.state[i] == gore::BONE;
					vb.state[i] = gore::CARVED;
					cells++;
				}
			}
		}
		b->dirty = true;
		if ( left ) {
			b->sliceBone = bone;
			b->sliceAlong = along;
		} else {
			b->sliceBone = -1;
			Gore_Chop( body, bone, child, along / L, 0 );   // (parts it from what it lies against)
			if ( through ) {
				*through = 1;
			}
		}
	} else {
		// a slit: a narrow cut where the blade went in, deepest in its middle
		auto any = []( V3 ) { return true; };
		if ( !FirstCell( vb, pr - dr * back, dr, back + 0.12f, any, h ) ) {
			// (hit boxes are bigger than the body: aimed at the bone it passed, if close)
			float t;
			V3 onRay = pr + dr * gore::dot( a - pr, dr );
			SegDist( onRay, a, c, &t );
			V3 off = a + ( c - a ) * t - pr;
			off = off - dr * gore::dot( off, dr );
			if ( gore::len( off ) > 0.1f || !FirstCell( vb, pr + off - dr * back, dr, back + 0.12f, any, h ) ) {
				return 0;
			}
		}
		V3 s0 = gore::norm( gore::cross( dr, axis ) );
		if ( gore::len( s0 ) < 0.5f ) {
			s0 = gore::norm( gore::cross( dr, V3 { 0, 0, 1 } ) );
			if ( gore::len( s0 ) < 0.5f ) {
				s0 = V3 { 1, 0, 0 };
			}
		}
		V3 s1 = gore::cross( dr, s0 );
		V3 s = gore::norm( s1 * cosf( roll ) + s0 * sinf( roll ) ), n = gore::cross( dr, s );
		float halfLen = reach * METRES, deep = 0.035f, wide = vb.cell * 0.6f;
		int span = (int)( ( halfLen + deep ) / vb.cell ) + 2;
		int hx = (int)floorf( ( h.x - vb.origin.x ) / vb.cell ), hy = (int)floorf( ( h.y - vb.origin.y ) / vb.cell ),
			hz = (int)floorf( ( h.z - vb.origin.z ) / vb.cell );
		for ( int z = hz - span; z <= hz + span; z++ ) {
			for ( int y = hy - span; y <= hy + span; y++ ) {
				for ( int x = hx - span; x <= hx + span; x++ ) {
					if ( !vb.in( x, y, z ) ) {
						continue;
					}
					int i = vb.id( x, y, z );
					if ( !vb.filled( i ) ) {
						continue;
					}
					V3 d = vb.centre( x, y, z ) - h;
					float u = gore::dot( d, s ) / halfLen, in = gore::dot( d, dr );
					if ( fabsf( u ) > 1.0f || fabsf( gore::dot( d, n ) ) > wide ||
						 in < -vb.cell * 1.5f || in > deep * ( 1.0f - u * u ) + vb.cell * 0.5f ) {
						continue;
					}
					boneCells += vb.state[i] == gore::BONE;
					vb.state[i] = gore::CARVED;
					cells++;
				}
			}
		}
		if ( !cells ) {
			return 0;
		}
		b->dirty = true;
	}
	res.cells = cells;
	res.boneCells = boneCells;
	res.length = 0.035f / METRES;
	V3 r0 = h * ( 1.0f / METRES );
	V3 e0 = Point( toPose, r0 );
	memcpy( res.entry, &e0, sizeof( res.entry ) );
	memcpy( res.exit, &e0, sizeof( res.exit ) );
	res.bone = bone;
	memcpy( res.restEntry, &r0, sizeof( res.restEntry ) );
	memcpy( res.restExit, &r0, sizeof( res.restExit ) );
	memcpy( res.restDir, &dr, sizeof( res.restDir ) );
	if ( out ) {
		*out = res;
	}
	return cells ? cells : 1;
}

int Gore_Sever( int body, int *pieces, float centres[][3], int *bones, int max ) {
	Body *b = GetBody( body );
	int n = 0;

	if ( !b || b->piece ) {
		return 0;
	}
	Model *m = GetModel( b->model );
	std::vector<gore::Piece> pcs = b->vb.pieces( 1 );
	if ( pcs.size() < 2 || !m ) {
		return 0;
	}
	const std::vector<int> labels = b->vb.labels();
	std::vector<int> own( pcs.size(), 0 );      // voxels of each that were joined to the body at first
	for ( size_t c = 0; c < labels.size(); c++ ) {
		if ( labels[c] >= 0 && !m->island[c] ) {
			own[labels[c]]++;
		}
	}
	for ( size_t i = 0; i < pcs.size(); i++ ) {
		if ( pcs[i].isMain || !own[i] ) {
			continue;       // the body, or a loose part of the model it always had
		}
		b->dirty = true;
		if ( own[i] < 40 ) {        // a crumb: gone
			for ( size_t c = 0; c < labels.size(); c++ ) {
				if ( labels[c] == (int)i ) {
					b->vb.state[c] = gore::CARVED;
				}
			}
			continue;
		}
		int slot = n < max ? NewBodySlot() : 0;
		if ( slot ) {
			std::unique_ptr<Body> p( new Body );
			p->model = b->model;
			p->piece = true;
			p->vb = b->vb;
			for ( size_t c = 0; c < labels.size(); c++ ) {
				if ( p->vb.filled( (int)c ) && labels[c] != (int)i ) {
					p->vb.state[c] = gore::CARVED;
				}
			}
			bodies[slot] = std::move( p );
			pieces[n] = slot;
			V3 c = pcs[i].centroid * ( 1.0f / METRES );
			memcpy( centres[n], &c, sizeof( float ) * 3 );
			bones[n] = pcs[i].mainBone;
			n++;
		}
		// off the body, whether it could be drawn flying or not
		for ( size_t c = 0; c < labels.size(); c++ ) {
			if ( labels[c] == (int)i ) {
				b->vb.state[c] = gore::CARVED;
			}
		}
	}
	return n;
}

int Gore_Chop( int body, int from, int to, float t, float radius ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	int cells = 0;

	if ( !m || b->piece || from < 0 || to < 0 || from >= m->numBones || to >= m->numBones ) {
		return 0;
	}
	gore::VoxelBody &vb = b->vb;
	V3 a = ToV3( m->rest[from].t ) * METRES, c = ToV3( m->rest[to].t ) * METRES;
	V3 axis = gore::norm( c - a ), at = a + ( c - a ) * t;
	float r = radius * METRES, half = vb.cell * 1.5f;
	for ( int z = 0; z < vb.nz; z++ ) {
		for ( int y = 0; y < vb.ny; y++ ) {
			for ( int x = 0; x < vb.nx; x++ ) {
				int i = vb.id( x, y, z );
				if ( !vb.filled( i ) ) {
					continue;
				}
				V3 d = vb.centre( x, y, z ) - at;
				float along = gore::dot( d, axis );
				if ( fabsf( along ) > half || gore::len( d - axis * along ) > r ) {
					continue;
				}
				vb.state[i] = gore::CARVED;
				cells++;
			}
		}
	}
	// the limb beyond the cut is often pressed against the body (an arm at the side, a thigh
	// under a coat): part it from whatever it touches that isn't the limb, so it comes away
	std::vector<char> limb( m->numBones, 0 );
	for ( int i = 0; i < m->numBones; i++ ) {
		for ( int j = i; j >= 0; j = m->parents[j] ) {
			if ( j == from ) {
				limb[i] = 1;
				break;
			}
		}
	}
	auto inLimb = [&]( int x, int y, int z ) {
		int i = vb.id( x, y, z );
		return vb.filled( i ) && vb.boneOf[i] < m->numBones && limb[vb.boneOf[i]] &&
			   gore::dot( vb.centre( x, y, z ) - at, axis ) > 0;
	};
	std::vector<int> part;
	for ( int z = 1; z < vb.nz - 1; z++ ) {
		for ( int y = 1; y < vb.ny - 1; y++ ) {
			for ( int x = 1; x < vb.nx - 1; x++ ) {
				if ( !inLimb( x, y, z ) ) {
					continue;
				}
				static const int D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
				for ( auto &dd : D ) {
					int j = vb.id( x + dd[0], y + dd[1], z + dd[2] );
					if ( vb.filled( j ) && !inLimb( x + dd[0], y + dd[1], z + dd[2] ) ) {
						part.push_back( vb.id( x, y, z ) );
						break;
					}
				}
			}
		}
	}
	for ( int i : part ) {
		vb.state[i] = gore::CARVED;
		cells++;
	}
	b->dirty = true;
	return cells;
}

void Gore_RestToPose( int body, const float *pose, int bone, const float in[3], float out[3] ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	if ( !m || bone < 0 || bone >= m->numBones ) {
		memcpy( out, in, sizeof( float ) * 3 );
		return;
	}
	V3 p = Point( Mul( PoseBone( pose, bone ), m->restInv[bone] ), ToV3( in ) );
	memcpy( out, &p, sizeof( float ) * 3 );
}

int Gore_RestBone( int body, const float in[3] ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	int bone = 0;
	float best = 1e9f;
	if ( !m || m->numBones <= 1 ) {
		return 0;
	}
	for ( int i = 0; i < m->numBones; i++ ) {
		int pa = m->parents[i];
		if ( pa < 0 || ( m->flags[i] & 1 ) || ( m->flags[pa] & 1 ) ) {
			continue;
		}
		float dist = SegDist( ToV3( in ), ToV3( m->rest[pa].t ), ToV3( m->rest[i].t ) );
		if ( dist < best ) {
			best = dist;
			bone = pa;
		}
	}
	return bone;
}

// the point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5)
static V3 TriNearest( V3 p, V3 a, V3 b, V3 c ) {
	V3 ab = b - a, ac = c - a, ap = p - a;
	float d1 = gore::dot( ab, ap ), d2 = gore::dot( ac, ap );
	if ( d1 <= 0 && d2 <= 0 ) {
		return a;
	}
	V3 bp = p - b;
	float d3 = gore::dot( ab, bp ), d4 = gore::dot( ac, bp );
	if ( d3 >= 0 && d4 <= d3 ) {
		return b;
	}
	float vc = d1 * d4 - d3 * d2;
	if ( vc <= 0 && d1 >= 0 && d3 <= 0 ) {
		return a + ab * ( d1 / ( d1 - d3 ) );
	}
	V3 cp = p - c;
	float d5 = gore::dot( ab, cp ), d6 = gore::dot( ac, cp );
	if ( d6 >= 0 && d5 <= d6 ) {
		return c;
	}
	float vb = d5 * d2 - d1 * d6;
	if ( vb <= 0 && d2 >= 0 && d6 <= 0 ) {
		return a + ac * ( d2 / ( d2 - d6 ) );
	}
	float va = d3 * d6 - d5 * d4;
	if ( va <= 0 && ( d4 - d3 ) >= 0 && ( d5 - d6 ) >= 0 ) {
		return b + ( c - b ) * ( ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ) );
	}
	float den = va + vb + vc;
	if ( fabsf( den ) < 1e-20f ) {
		return a;
	}
	return a + ab * ( vb / den ) + ac * ( vc / den );
}

float Gore_RestSurface( int body, const float in[3], const float hint[3], float minDot,
						float out[3], float normal[3], goreAnchor_t *anchor ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	if ( !m ) {
		return -1.0f;
	}
	V3 p = ToV3( in ) * METRES, h = gore::norm( ToV3( hint ) ), bestP{ 0, 0, 0 }, bestN{ 0, 0, 0 };
	float best = 1e9f;
	int bestS = -1, bestT = 0;
	if ( anchor ) {
		anchor->surf = -1;
	}
	for ( size_t si = 0; si < m->surfs.size(); si++ ) {
		const Surf &sf = m->surfs[si];
		for ( int t = 0; t + 2 < (int)sf.tri.size(); t += 3 ) {
			int i0 = sf.tri[t], i1 = sf.tri[t + 1], i2 = sf.tri[t + 2];
			V3 n = gore::norm( sf.nrm[i0] + sf.nrm[i1] + sf.nrm[i2] );
			if ( gore::dot( n, h ) < minDot ) {
				continue;
			}
			V3 q = TriNearest( p, sf.xyz[i0], sf.xyz[i1], sf.xyz[i2] );
			float d = gore::len( q - p );
			if ( d < best ) {
				best = d;
				bestP = q;
				bestN = n;
				bestS = (int)si;
				bestT = t;
			}
		}
	}
	if ( best > 1e8f ) {
		return -1.0f;
	}
	out[0] = bestP.x / METRES; out[1] = bestP.y / METRES; out[2] = bestP.z / METRES;
	normal[0] = bestN.x; normal[1] = bestN.y; normal[2] = bestN.z;
	if ( anchor ) {
		// where on its triangle
		const Surf &sf = m->surfs[bestS];
		V3 a = sf.xyz[sf.tri[bestT]], e1 = sf.xyz[sf.tri[bestT + 1]] - a, e2 = sf.xyz[sf.tri[bestT + 2]] - a, r = bestP - a;
		float d11 = gore::dot( e1, e1 ), d12 = gore::dot( e1, e2 ), d22 = gore::dot( e2, e2 );
		float det = d11 * d22 - d12 * d12, u = 1.0f / 3, v = 1.0f / 3;
		if ( det > 1e-20f ) {
			u = ( gore::dot( r, e1 ) * d22 - gore::dot( r, e2 ) * d12 ) / det;
			v = ( gore::dot( r, e2 ) * d11 - gore::dot( r, e1 ) * d12 ) / det;
		}
		anchor->surf = bestS;
		anchor->tri = bestT;
		anchor->bary[0] = 1.0f - u - v;
		anchor->bary[1] = u;
		anchor->bary[2] = v;
	}
	return best / METRES;
}

int Gore_AnchorPose( int body, const float *pose, const goreAnchor_t *an, const float restDir[3],
					 float at[3], float normal[3], float dir[3] ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	if ( !m || !pose || !an || an->surf < 0 || an->surf >= (int)m->surfs.size() ) {
		return 0;
	}
	const Surf &sf = m->surfs[an->surf];
	if ( an->tri < 0 || an->tri + 2 >= (int)sf.tri.size() || (int)sf.wFirst.size() != sf.numVerts + 1 ) {
		return 0;
	}
	V3 P[3], R[3], vn{ 0, 0, 0 };
	for ( int k = 0; k < 3; k++ ) {
		int v = sf.tri[an->tri + k];
		V3 p{ 0, 0, 0 };
		if ( sf.wFirst[v + 1] <= sf.wFirst[v] ) {
			return 0;
		}
		for ( int w = sf.wFirst[v]; w < sf.wFirst[v + 1]; w++ ) {
			const Surf::Wt &wt = sf.wts[w];
			p = p + Point( PoseBone( pose, wt.bone ), wt.off ) * wt.w;
		}
		P[k] = p;
		R[k] = sf.xyz[v] * ( 1.0f / METRES );
		vn = vn + sf.nrm[v];
	}
	V3 pe1 = P[1] - P[0], pe2 = P[2] - P[0], re1 = R[1] - R[0], re2 = R[2] - R[0];
	V3 p = P[0] * an->bary[0] + P[1] * an->bary[1] + P[2] * an->bary[2];
	V3 n = gore::norm( gore::cross( pe1, pe2 ) );
	if ( gore::dot( gore::cross( re1, re2 ), vn ) < 0 ) {       // (the way the vertex normals face, whatever the winding)
		n = n * -1.0f;
	}
	V3 d = ToV3( restDir ), o{ 0, 0, 0 };
	float d11 = gore::dot( re1, re1 ), d12 = gore::dot( re1, re2 ), d22 = gore::dot( re2, re2 );
	float det = d11 * d22 - d12 * d12;
	if ( det > 1e-12f ) {
		float u = ( gore::dot( d, re1 ) * d22 - gore::dot( d, re2 ) * d12 ) / det;
		float v = ( gore::dot( d, re2 ) * d11 - gore::dot( d, re1 ) * d12 ) / det;
		o = gore::norm( pe1 * u + pe2 * v );
	}
	at[0] = p.x; at[1] = p.y; at[2] = p.z;
	normal[0] = n.x; normal[1] = n.y; normal[2] = n.z;
	dir[0] = o.x; dir[1] = o.y; dir[2] = o.z;
	return 1;
}

const vrGoreDraw_t *Gore_Draw( int body, const float *pose, const float ambient[3],
							   const float directed[3], const float lightDir[3] ) {
	Body *b = GetBody( body );
	Model *m = b ? GetModel( b->model ) : nullptr;
	if ( !m || !pose ) {
		return nullptr;
	}
	if ( b->dirty && ( rebuildsLeft > 0 || b->keep.empty() ) ) {
		Rebuild( *b );
		rebuildsLeft--;
		b->drawValid = false;
		b->fidx.assign( b->flesh.idx.begin(), b->flesh.idx.end() );
	}
	if ( b->keep.size() != m->surfs.size() ) {
		return nullptr;
	}
	// RealRTCW-VR: unchanged since last time (VR draws each eye, and a body lying dead still
	// keeps its pose): the same flesh, not posed and lit again
	{
		const size_t np = (size_t)m->numBones * 12;
		float light[9];
		memcpy( light, ambient, sizeof( float ) * 3 );
		memcpy( light + 3, directed, sizeof( float ) * 3 );
		memcpy( light + 6, lightDir, sizeof( float ) * 3 );
		if ( b->drawValid && b->drawPose.size() == np && !memcmp( b->drawPose.data(), pose, sizeof( float ) * np ) &&
			 !memcmp( b->drawLight, light, sizeof( light ) ) ) {
			return &b->draw;
		}
		b->drawPose.assign( pose, pose + np );
		memcpy( b->drawLight, light, sizeof( light ) );
		b->drawValid = true;
	}
	vrGoreDraw_t &dr = b->draw;
	memset( &dr, 0, sizeof( dr ) );
	dr.numSurfaces = (int)m->surfs.size();
	for ( int s = 0; s < dr.numSurfaces; s++ ) {
		vrGoreSurf_t &o = dr.surf[s];
		o.numVerts = m->surfs[s].numVerts;
		o.numTriangles = m->surfs[s].numTris;
		if ( !b->piece && (int)b->keep[s].size() == m->surfs[s].numTris ) {
			o.numKeep = -1;     // untouched
			continue;
		}
		o.numKeep = (int)b->keep[s].size();
		o.keep = b->keep[s].data();
		o.numSub = (int)( b->sub[s].size() / VRG_SUB_FLOATS );
		o.sub = b->sub[s].data();
	}

	// the flesh, each vertex with its bone
	const gore::Mesh &f = b->flesh;
	size_t nv = f.pos.size();
	if ( nv && !f.idx.empty() ) {
		std::vector<Xf> delta( m->numBones );
		std::vector<char> have( m->numBones, 0 );
		b->fxyz.resize( nv * 3 );
		b->fnrm.resize( nv * 3 );
		b->frgba.resize( nv * 4 );
		V3 L = gore::norm( ToV3( lightDir ) );
		for ( size_t i = 0; i < nv; i++ ) {
			int bone = f.bone[i] < m->numBones ? f.bone[i] : 0;
			if ( !have[bone] ) {
				delta[bone] = Mul( PoseBone( pose, bone ), m->restInv[bone] );
				have[bone] = 1;
			}
			V3 p = Point( delta[bone], f.pos[i] * ( 1.0f / METRES ) );
			V3 n = gore::norm( Dir( delta[bone], f.nrm[i] ) );
			memcpy( &b->fxyz[i * 3], &p, sizeof( float ) * 3 );
			memcpy( &b->fnrm[i * 3], &n, sizeof( float ) * 3 );
			// raw and bloody: a thin pink-red layer under the skin, dark red muscle deeper; bone
			float tint[3];
			if ( m->head && f.depthCm[i] != 255 && f.depthCm[i] > 25 ) {
				tint[0] = 0.72f; tint[1] = 0.48f; tint[2] = 0.46f;     // brains
			} else if ( f.depthCm[i] == 255 ) {
				tint[0] = 0.85f; tint[1] = 0.78f; tint[2] = 0.66f;
			} else {
				float t = f.depthCm[i] / 2.0f, deep = f.depthCm[i] / 8.0f;
				t = t > 1 ? 1 : t;
				deep = deep > 1 ? 1 : deep;
				tint[0] = 0.78f + ( 0.55f - 0.78f ) * t - 0.2f * deep;
				tint[1] = 0.22f + ( 0.04f - 0.22f ) * t - 0.02f * deep;
				tint[2] = 0.18f + ( 0.04f - 0.18f ) * t - 0.02f * deep;
			}
			float lit = gore::dot( n, L );
			lit = lit < 0 ? 0 : lit;
			for ( int k = 0; k < 3; k++ ) {
				float c = tint[k] * ( ambient[k] + directed[k] * lit );
				b->frgba[i * 4 + k] = (unsigned char)( c > 255 ? 255 : ( c < 0 ? 0 : c ) );
			}
			b->frgba[i * 4 + 3] = 255;
		}
		if ( b->fidx.size() != f.idx.size() ) {
			b->fidx.assign( f.idx.begin(), f.idx.end() );
		}
		dr.numFleshVerts = (int)nv;
		dr.numFleshIndexes = (int)b->fidx.size();
		dr.fleshXyz = b->fxyz.data();
		dr.fleshNormal = b->fnrm.data();
		dr.fleshRGBA = b->frgba.data();
		dr.fleshIndexes = b->fidx.data();
	}
	return &dr;
}

}   // extern "C"
