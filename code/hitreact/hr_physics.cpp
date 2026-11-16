/*
===========================================================================

HitReact -- active ragdoll hit reactions on Jolt Physics
Copyright (C) 2026 RealRTCW-VR contributors

This file is part of HitReact, which grew out of RealRTCW-VR's ragdolls.

HitReact is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. Jolt Physics (code/JoltPhysics) is MIT licensed.

===========================================================================
*/

// hr_physics.cpp -- the Jolt layer (HRP_*): one physics world, the level's static geometry,
// kinematic movers, and ragdolls made of capsule and box parts joined by swing-twist and
// hinge joints, whose motors can chase a pose (the muscles of hr_humanoid.c).
//
// Single threaded (JobSystemSingleThreaded): the same results every run on every machine,
// and nothing to manage when a game loads and unloads the library on every map change.
//
// Library space: Z up, 1 unit = 1 inch (0.0254 m), as in Quake-engine games. The level and
// the movers are given in the host's space (HR_SetHostSpace) and turned into library space
// here; everything about ragdolls is in library space.

#include "hitreact.h"

#ifndef HR_NO_JOLT

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

using namespace JPH;

#define PHYS_SCALE          0.0254f     // game units are about an inch: Jolt wants metres
#define PHYS_MAX_RAGDOLLS   128
#define PHYS_MAX_MOVERS     1024
#define PHYS_WORLD_CHUNK    2048        // hulls per static body
#define PHYS_HULL_RADIUS    0.01f       // rounded hull edges (m), so bodies slide off corners
#define PHYS_TONE_TORQUE    220.0f      // N m a joint holds the death pose with, at first
#define PHYS_DRIVE_TORQUE   220.0f      // N m at full strength when a joint chases a pose (HRP_RagdollDrive)
#define PHYS_DRIVE_HZ       4.0f        // how stiffly it chases it (spring frequency)
#define PHYS_UPRIGHT_K      900.0f      // N m per radian of tilt at full upright support (HRP_RagdollSupport)
#define PHYS_UPRIGHT_D      140.0f      // N m s: and its damping
#define HRP_JOINT_FRICTION 0.4f        // N m: (1.5 held a forearm straight up in the air for good)
#define PHYS_MAX_SPEED      16.5f       // m/s (650 units/s, as the old ragdolls): nothing is flung faster
#define PHYS_MAX_SPIN       12.0f       // rad/s (about two turns a second): no helicopters
#define PHYS_DEEP_OVERLAP   0.04f       // m: two bodies this far inside each other pass through instead
#define PHYS_OWN_OVERLAP    0.01f       // m: parts of one body that started inside each other collide from here

namespace {

enum : ObjectLayer {
	LAYER_WORLD,        // the level, never moves
	LAYER_MOVER,        // doors, lifts (kinematic)
	LAYER_BODY,         // ragdoll parts
	LAYER_HAND,         // the kinematic point a held body hangs from: touches nothing
	NUM_LAYERS
};

const BroadPhaseLayer BP_STATIC( 0 );
const BroadPhaseLayer BP_MOVING( 1 );

void ( *physPrint )( const char *msg );

void PhysTrace( const char *fmt, ... ) {
	char buf[1024];
	va_list ap;

	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	if ( physPrint ) {
		physPrint( buf );
		physPrint( "\n" );
	}
}

inline Vec3 ToJolt( const float *v ) {
	return Vec3( v[0] * PHYS_SCALE, v[1] * PHYS_SCALE, v[2] * PHYS_SCALE );
}

inline Vec3 Dir( const float *v ) {
	Vec3 d( v[0], v[1], v[2] );
	float l = d.Length();
	return l > 1e-6f ? d / l : Vec3::sAxisZ();
}

inline void FromJolt( Vec3Arg v, float *out ) {
	out[0] = v.GetX() / PHYS_SCALE;
	out[1] = v.GetY() / PHYS_SCALE;
	out[2] = v.GetZ() / PHYS_SCALE;
}

inline float Rad( float deg ) {
	return deg * ( JPH_PI / 180.0f );
}

// a unit vector square to d, as close to want as possible
Vec3 Square( Vec3Arg d, Vec3Arg want ) {
	Vec3 p = want - d * d.Dot( want );
	if ( p.Length() < 1e-4f ) {
		p = d.GetNormalizedPerpendicular();
	}
	return p.Normalized();
}

Quat AxisToQuat( const float *m ) {
	Mat44 r( Vec4( m[0], m[1], m[2], 0 ), Vec4( m[3], m[4], m[5], 0 ), Vec4( m[6], m[7], m[8], 0 ), Vec4( 0, 0, 0, 1 ) );
	return r.GetQuaternion().Normalized();
}

struct Ragdoll {
	bool                inUse;
	int                 numParts;
	BodyID              body[HRP_MAX_PARTS];
	Ref<Constraint>     joint[HRP_MAX_PARTS];      // part i to its parent
	int                 jointType[HRP_MAX_PARTS];
	float               toneMs[HRP_MAX_PARTS];
	bool                toneOn[HRP_MAX_PARTS];
	int                 parent[HRP_MAX_PARTS];
	Quat                rot0[HRP_MAX_PARTS];       // each part's rotation when it was made
	Vec3                hingeAxis[HRP_MAX_PARTS];  // a hinge's axis in its parent's own space
	bool                driven[HRP_MAX_PARTS];     // its motor chases a pose (HRP_RagdollDrive)
	float               totalMass;                  // kg
	float               supUpright, supLift, supDrift;     // HRP_RagdollSupport
	Vec3                supUp;                      // the way the torso is held (world)
	float               pinW;                       // HRP_RagdollPin: spring rate (rad/s), 0 none
	int                 legFoot[2];                 // HRP_RagdollLegPush: the feet (-1 none)
	float               legShare[2];                // and the share of his weight each leg pushes up
	Vec3                pinPos;
	Quat                pinRot;
	float               tqScale[HRP_MAX_PARTS];    // hrpPart_t.torqueScale
	float               toneHz[HRP_MAX_PARTS];     // HRP_RagdollTone: a joint's drive frequency and torque share (0: by depth)
	float               toneTq[HRP_MAX_PARTS];
	float               catchZ;                     // HRP_RagdollCatch: torso height he's caught below (0 off)
	float               catchSlack;                 // how far he sinks freely first
	float               catchTilt;                  // cos of the lean he tips freely to
	float               age;                        // ms since it went limp
	Ref<GroupFilterTable> group;
	unsigned char       inside[HRP_MAX_PARTS][HRP_MAX_PARTS];    // started inside each other, not yet apart
	BodyID              hand;
	Ref<Constraint>     grab;
	int                 grabPart;
	Vec3                grabTarget;
	float               grabSpeed;
};

struct Mover {
	bool                inUse;
	Ref<StaticCompoundShapeSettings> hulls;
	int                 numHulls;
	BodyID              body;
	bool                added;
	bool                hasTarget;
	bool                moving;         // given a speed by the last step (HRP_Step stops it)
	RVec3               targetPos;
	Quat                targetRot;
};

struct World {
	BroadPhaseLayerInterfaceTable       *bpLayers;
	ObjectLayerPairFilterTable          *pairFilter;
	ObjectVsBroadPhaseLayerFilterTable  *bpFilter;
	PhysicsSystem                       *system;
	TempAllocatorImpl                   *temp;
	JobSystemSingleThreaded             *jobs;

	Array<BodyID>                       staticBodies;
	Ref<StaticCompoundShapeSettings>    chunk;
	int                                 chunkHulls;
	TriangleList                        tris;
	int                                 totalHulls, totalTris;
};

Ragdoll     ragdolls[PHYS_MAX_RAGDOLLS];

/*
Two bodies dying on the same spot (a pile after a grenade, a squad cut down in a doorway) start
inside each other, and pushing them apart threw both into the air. Parts of different ragdolls
that are deep inside each other pass through until they are apart; touching ones collide as
usual, so bodies still pile up.
Parts of one body that were inside each other at death (an arm through the chest) pass through
the same way, but only until they have worked their way out: from then on they collide, so a
limb can't swing through its own body.
*/
class OverlapListener : public ContactListener {
public:
	void Check( const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s ) {
		if ( b1.GetObjectLayer() != LAYER_BODY || b2.GetObjectLayer() != LAYER_BODY ) {
			return;
		}
		const CollisionGroup &g1 = b1.GetCollisionGroup(), &g2 = b2.GetCollisionGroup();
		if ( g1.GetGroupID() != g2.GetGroupID() ) {
			if ( m.mPenetrationDepth > PHYS_DEEP_OVERLAP ) {
				s.mIsSensor = true;
			}
			return;
		}
		unsigned h = g1.GetGroupID(), i = g1.GetSubGroupID(), j = g2.GetSubGroupID();
		if ( h < 1 || h > PHYS_MAX_RAGDOLLS || i >= HRP_MAX_PARTS || j >= HRP_MAX_PARTS ) {
			return;
		}
		unsigned char *inside = &ragdolls[h - 1].inside[i][j];
		if ( !*inside ) {
			return;
		}
		if ( m.mPenetrationDepth > PHYS_OWN_OVERLAP ) {
			s.mIsSensor = true;
		} else {
			*inside = 0;        // (only this pair's contact writes it)
			ragdolls[h - 1].inside[j][i] = 0;
		}
	}
	void OnContactAdded( const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s ) override {
		Check( b1, b2, m, s );
	}
	void OnContactPersisted( const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s ) override {
		Check( b1, b2, m, s );
	}
};

World       w;
OverlapListener overlapListener;
bool        initialized;
Mover       movers[PHYS_MAX_MOVERS];

BodyInterface &BI() {
	return w.system->GetBodyInterfaceNoLock();     // only ever used from the main thread
}

Ragdoll *GetRagdoll( int h ) {
	if ( !initialized || h <= 0 || h > PHYS_MAX_RAGDOLLS || !ragdolls[h - 1].inUse ) {
		return nullptr;
	}
	return &ragdolls[h - 1];
}

Mover *GetMover( int h ) {
	if ( !initialized || h <= 0 || h > PHYS_MAX_MOVERS || !movers[h - 1].inUse ) {
		return nullptr;
	}
	return &movers[h - 1];
}

// a convex hull from points (game units), placed at their middle
RefConst<Shape> MakeHull( const float *points, int numPoints, Vec3 &centre ) {
	Array<Vec3> pts;
	Vec3 sum = Vec3::sZero();
	int i;

	if ( numPoints < 4 ) {
		return nullptr;
	}
	for ( i = 0; i < numPoints; i++ ) {
		sum += ToJolt( points + i * 3 );
	}
	centre = sum / (float)numPoints;
	pts.reserve( numPoints );
	for ( i = 0; i < numPoints; i++ ) {
		pts.push_back( ToJolt( points + i * 3 ) - centre );
	}
	ConvexHullShapeSettings settings( pts, PHYS_HULL_RADIUS );
	settings.mHullTolerance = 0.002f;
	ShapeSettings::ShapeResult r = settings.Create();
	if ( r.HasError() ) {
		return nullptr;     // flat or degenerate brush
	}
	return r.Get();
}

void FlushChunk( void ) {
	if ( !w.chunk || !w.chunkHulls ) {
		return;
	}
	ShapeSettings::ShapeResult r = w.chunk->Create();
	if ( r.HasError() ) {
		PhysTrace( "physics: level chunk failed: %s", r.GetError().c_str() );
	} else {
		BodyCreationSettings bcs( r.Get(), RVec3::sZero(), Quat::sIdentity(), EMotionType::Static, LAYER_WORLD );
		bcs.mFriction = 0.8f;
		BodyID id = BI().CreateAndAddBody( bcs, EActivation::DontActivate );
		if ( !id.IsInvalid() ) {
			w.staticBodies.push_back( id );
		}
	}
	w.chunk = nullptr;
	w.chunkHulls = 0;
}

void RemoveBody( BodyID &id ) {
	if ( id.IsInvalid() ) {
		return;
	}
	if ( BI().IsAdded( id ) ) {
		BI().RemoveBody( id );
	}
	BI().DestroyBody( id );
	id = BodyID();
}

void ClearAll( void ) {
	int i;

	for ( i = 0; i < PHYS_MAX_RAGDOLLS; i++ ) {
		if ( ragdolls[i].inUse ) {
			HRP_DestroyRagdoll( i + 1 );
		}
	}
	for ( i = 0; i < PHYS_MAX_MOVERS; i++ ) {
		if ( movers[i].inUse ) {
			RemoveBody( movers[i].body );
			movers[i].hulls = nullptr;
			movers[i].inUse = false;
		}
	}
	for ( BodyID &id : w.staticBodies ) {
		RemoveBody( id );
	}
	w.staticBodies.clear();
	w.chunk = nullptr;
	w.chunkHulls = 0;
	w.tris.clear();
	w.totalHulls = w.totalTris = 0;
}

// the joint's muscle tone fades: the death pose is held a moment, then he goes limp
void UpdateTone( Ragdoll &rd, float dtMs ) {
	int i;

	rd.age += dtMs;
	for ( i = 1; i < rd.numParts; i++ ) {
		float k;

		if ( !rd.toneOn[i] || !rd.joint[i] || rd.driven[i] ) {
			continue;
		}
		k = rd.toneMs[i] > 0 ? 1.0f - rd.age / rd.toneMs[i] : 0;
		if ( k <= 0 ) {
			rd.toneOn[i] = false;
			if ( rd.jointType[i] == HRP_JOINT_SWINGTWIST ) {
				SwingTwistConstraint *c = static_cast<SwingTwistConstraint *>( rd.joint[i].GetPtr() );
				c->SetSwingMotorState( EMotorState::Off );
				c->SetTwistMotorState( EMotorState::Off );
			} else {
				static_cast<HingeConstraint *>( rd.joint[i].GetPtr() )->SetMotorState( EMotorState::Off );
			}
			continue;
		}
		k = PHYS_TONE_TORQUE * k * k;
		if ( rd.jointType[i] == HRP_JOINT_SWINGTWIST ) {
			SwingTwistConstraint *c = static_cast<SwingTwistConstraint *>( rd.joint[i].GetPtr() );
			c->GetSwingMotorSettings().SetTorqueLimit( k );
			c->GetTwistMotorSettings().SetTorqueLimit( k );
		} else {
			static_cast<HingeConstraint *>( rd.joint[i].GetPtr() )->GetMotorSettings().SetTorqueLimit( k );
		}
	}
}

} // namespace

extern "C" {

int HRP_Init( void ( *print )( const char *msg ) ) {
	physPrint = print;
	if ( initialized ) {
		HRP_Shutdown();
	}

	RegisterDefaultAllocator();
	Trace = PhysTrace;
	Factory::sInstance = new Factory();
	RegisterTypes();

	w.bpLayers = new BroadPhaseLayerInterfaceTable( NUM_LAYERS, 2 );
	w.bpLayers->MapObjectToBroadPhaseLayer( LAYER_WORLD, BP_STATIC );
	w.bpLayers->MapObjectToBroadPhaseLayer( LAYER_MOVER, BP_MOVING );
	w.bpLayers->MapObjectToBroadPhaseLayer( LAYER_BODY, BP_MOVING );
	w.bpLayers->MapObjectToBroadPhaseLayer( LAYER_HAND, BP_MOVING );
	w.pairFilter = new ObjectLayerPairFilterTable( NUM_LAYERS );
	w.pairFilter->EnableCollision( LAYER_BODY, LAYER_WORLD );
	w.pairFilter->EnableCollision( LAYER_BODY, LAYER_MOVER );
	w.pairFilter->EnableCollision( LAYER_BODY, LAYER_BODY );      // bodies pile up on each other
	w.bpFilter = new ObjectVsBroadPhaseLayerFilterTable( *w.bpLayers, 2, *w.pairFilter, NUM_LAYERS );

	w.temp = new TempAllocatorImpl( 16 * 1024 * 1024 );
	w.jobs = new JobSystemSingleThreaded( cMaxPhysicsJobs );
	w.system = new PhysicsSystem();
	w.system->Init( 8192, 0, 16384, 8192, *w.bpLayers, *w.bpFilter, *w.pairFilter );

	PhysicsSettings ps = w.system->GetPhysicsSettings();
	ps.mNumVelocitySteps = 12;      // long chains of light parts: a few more iterations than the default
	ps.mNumPositionSteps = 4;
	w.system->SetPhysicsSettings( ps );
	w.system->SetGravity( Vec3( 0, 0, -9.81f ) );      // z is up, as in the game
	w.system->SetContactListener( &overlapListener );

	initialized = true;
	return 1;
}

void HRP_Shutdown( void ) {
	if ( !initialized ) {
		return;
	}
	ClearAll();
	delete w.system;
	delete w.jobs;
	delete w.temp;
	delete w.bpFilter;
	delete w.pairFilter;
	delete w.bpLayers;
	w.system = nullptr;
	w.jobs = nullptr;
	w.temp = nullptr;
	w.bpFilter = nullptr;
	w.pairFilter = nullptr;
	w.bpLayers = nullptr;
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
	initialized = false;
}

int HRP_Active( void ) {
	return initialized;
}


/*
==============================================================================

The level

==============================================================================
*/

void HRP_BeginWorld( void ) {
	if ( !initialized ) {
		return;
	}
	ClearAll();
}

void HRP_AddWorldHull( const float *points, int numPoints ) {
	Vec3 centre;
	std::vector<float> lib;

	if ( !initialized || !points || numPoints < 4 ) {
		return;
	}
	if ( !HR_HostIsLib() ) {
		lib.resize( numPoints * 3 );
		for ( int i = 0; i < numPoints; i++ ) {
			HR_PointToLib( points + i * 3, &lib[i * 3] );
		}
		points = lib.data();
	}
	RefConst<Shape> hull = MakeHull( points, numPoints, centre );
	if ( !hull ) {
		return;
	}
	if ( !w.chunk ) {
		w.chunk = new StaticCompoundShapeSettings();
	}
	w.chunk->AddShape( centre, Quat::sIdentity(), hull );
	w.totalHulls++;
	if ( ++w.chunkHulls >= PHYS_WORLD_CHUNK ) {
		FlushChunk();
	}
}

void HRP_AddWorldTriangle( const float *a, const float *b, const float *c ) {
	if ( !initialized ) {
		return;
	}
	float la[3], lb[3], lc[3];
	if ( !HR_HostIsLib() ) {
		HR_PointToLib( a, la );
		HR_PointToLib( b, lb );
		HR_PointToLib( c, lc );
		a = la;
		b = lb;
		c = lc;
	}
	Vec3 va = ToJolt( a ), vb = ToJolt( b ), vc = ToJolt( c );
	w.tris.push_back( Triangle( Float3( va.GetX(), va.GetY(), va.GetZ() ), Float3( vb.GetX(), vb.GetY(), vb.GetZ() ),
								Float3( vc.GetX(), vc.GetY(), vc.GetZ() ) ) );
}

void HRP_EndWorld( void ) {
	if ( !initialized ) {
		return;
	}
	FlushChunk();
	if ( !w.tris.empty() ) {
		MeshShapeSettings settings( w.tris );
		ShapeSettings::ShapeResult r = settings.Create();
		if ( r.HasError() ) {
			PhysTrace( "physics: level curves failed: %s", r.GetError().c_str() );
		} else {
			BodyCreationSettings bcs( r.Get(), RVec3::sZero(), Quat::sIdentity(), EMotionType::Static, LAYER_WORLD );
			bcs.mFriction = 0.8f;
			BodyID id = BI().CreateAndAddBody( bcs, EActivation::DontActivate );
			if ( !id.IsInvalid() ) {
				w.staticBodies.push_back( id );
			}
		}
		w.totalTris = (int)w.tris.size();
		w.tris.clear();
		w.tris.shrink_to_fit();
	}
	w.system->OptimizeBroadPhase();
}

/*
==============================================================================

Movers: brush entities (doors, lifts, platforms, crates) as kinematic bodies

==============================================================================
*/

int HRP_CreateMover( void ) {
	int i;

	if ( !initialized ) {
		return 0;
	}
	for ( i = 0; i < PHYS_MAX_MOVERS; i++ ) {
		if ( !movers[i].inUse ) {
			Mover &m = movers[i];
			m.inUse = true;
			m.hulls = new StaticCompoundShapeSettings();
			m.numHulls = 0;
			m.body = BodyID();
			m.added = false;
			m.hasTarget = false;
			m.moving = false;
			return i + 1;
		}
	}
	return 0;
}

void HRP_MoverAddHull( int h, const float *points, int numPoints ) {
	Mover *m = GetMover( h );
	Vec3 centre;
	std::vector<float> lib;

	if ( !m || !m->hulls || !points || numPoints < 4 ) {
		return;
	}
	if ( !HR_HostIsLib() ) {
		lib.resize( numPoints * 3 );
		for ( int i = 0; i < numPoints; i++ ) {
			HR_PointToLib( points + i * 3, &lib[i * 3] );
		}
		points = lib.data();
	}
	RefConst<Shape> hull = MakeHull( points, numPoints, centre );
	if ( hull ) {
		m->hulls->AddShape( centre, Quat::sIdentity(), hull );
		m->numHulls++;
	}
}

int HRP_MoverFinish( int h ) {
	Mover *m = GetMover( h );

	if ( !m ) {
		return 0;
	}
	if ( m->numHulls ) {
		ShapeSettings::ShapeResult r = m->hulls->Create();
		if ( !r.HasError() ) {
			BodyCreationSettings bcs( r.Get(), RVec3::sZero(), Quat::sIdentity(), EMotionType::Kinematic, LAYER_MOVER );
			bcs.mFriction = 0.8f;
			Body *body = BI().CreateBody( bcs );
			m->body = body ? body->GetID() : BodyID();
		}
	}
	m->hulls = nullptr;
	if ( m->body.IsInvalid() ) {
		m->inUse = false;
		return 0;
	}
	return 1;
}

void HRP_MoverPlace( int h, const float *origin, const float *axis, int teleport ) {
	Mover *m = GetMover( h );

	float lo[3], la[9];

	if ( !m || m->body.IsInvalid() ) {
		return;
	}
	if ( !HR_HostIsLib() ) {
		HR_PointToLib( origin, lo );
		HR_AxisToLib( axis, la );
		origin = lo;
		axis = la;
	}
	RVec3 pos( ToJolt( origin ) );
	Quat rot = AxisToQuat( axis );
	if ( !m->added ) {
		BI().SetPositionAndRotation( m->body, pos, rot, EActivation::DontActivate );
		BI().AddBody( m->body, EActivation::DontActivate );
		m->added = true;
		m->hasTarget = false;
		return;
	}
	if ( teleport ) {
		BI().SetPositionAndRotation( m->body, pos, rot, EActivation::Activate );
		BI().SetLinearAndAngularVelocity( m->body, Vec3::sZero(), Vec3::sZero() );
		m->hasTarget = false;
		m->moving = false;
		return;
	}
	m->targetPos = pos;
	m->targetRot = rot;
	m->hasTarget = true;
}

void HRP_MoverHide( int h ) {
	Mover *m = GetMover( h );

	if ( m && m->added ) {
		BI().SetLinearAndAngularVelocity( m->body, Vec3::sZero(), Vec3::sZero() );
		BI().RemoveBody( m->body );
		m->added = false;
		m->hasTarget = false;
		m->moving = false;
	}
}

/*
==============================================================================

Ragdolls

==============================================================================
*/

int HRP_CreateRagdoll( const hrpPart_t *parts, int numParts, const unsigned char *noCollide ) {
	Ragdoll *rd = nullptr;
	Quat rot[HRP_MAX_PARTS];
	int i, j, h;

	if ( !initialized || numParts <= 0 || numParts > HRP_MAX_PARTS ) {
		return 0;
	}
	for ( h = 0; h < PHYS_MAX_RAGDOLLS; h++ ) {
		if ( !ragdolls[h].inUse ) {
			rd = &ragdolls[h];
			break;
		}
	}
	if ( !rd ) {
		return 0;
	}
	rd->numParts = numParts;
	rd->age = 0;
	rd->totalMass = 0;
	rd->supUpright = rd->supLift = rd->supDrift = 0;
	rd->legFoot[0] = rd->legFoot[1] = -1;
	rd->legShare[0] = rd->legShare[1] = 0;
	rd->supUp = Vec3::sAxisZ();
	rd->pinW = 0;
	rd->catchZ = 0;
	for ( i = 0; i < HRP_MAX_PARTS; i++ ) {
		rd->toneHz[i] = rd->toneTq[i] = 0;
	}
	for ( i = 0; i < numParts; i++ ) {
		rd->totalMass += parts[i].mass > 0 ? parts[i].mass : 2.0f;
		rd->tqScale[i] = parts[i].torqueScale > 0 ? parts[i].torqueScale : 1.0f;
	}
	rd->grab = nullptr;
	rd->hand = BodyID();
	rd->grabPart = -1;

	// parts next to each other never collide. Parts that were already inside each other at death
	// (an arm through the chest in the death animation) pass through until they are apart (see
	// OverlapListener): pushing them apart at once spins bodies
	rd->group = new GroupFilterTable( numParts );
	memset( rd->inside, 0, sizeof( rd->inside ) );
	for ( i = 0; i < numParts; i++ ) {
		if ( parts[i].parent >= 0 ) {
			rd->group->DisableCollision( i, parts[i].parent );
		}
		for ( j = i + 1; j < numParts; j++ ) {
			if ( noCollide && noCollide[i * numParts + j] ) {
				rd->inside[i][j] = rd->inside[j][i] = 1;
			}
		}
	}

	for ( i = 0; i < numParts; i++ ) {
		const hrpPart_t *p = &parts[i];
		RefConst<Shape> shape;
		Vec3 a = ToJolt( p->a ), b = ToJolt( p->b ), centre = ( a + b ) * 0.5f;
		float r = p->radius * PHYS_SCALE;

		if ( p->shape == HRP_SHAPE_BOX ) {
			Vec3 half = Vec3( p->boxHalf[0], p->boxHalf[1], p->boxHalf[2] ) * PHYS_SCALE;
			shape = new BoxShape( half, std::fmin( 0.02f, half.ReduceMin() * 0.5f ) );
			rot[i] = AxisToQuat( p->boxAxis );
		} else {
			Vec3 d = b - a;
			float len = d.Length();
			float halfHeight = len * 0.5f - r * 0.5f;
			if ( halfHeight < 0.01f ) {
				halfHeight = 0.01f;
			}
			shape = new CapsuleShape( halfHeight, r );
			rot[i] = len > 1e-4f ? Quat::sFromTo( Vec3::sAxisY(), d / len ) : Quat::sIdentity();
		}
		BodyCreationSettings bcs( shape, RVec3( centre ), rot[i], EMotionType::Dynamic, LAYER_BODY );
		// (the group is the handle, slot + 1: OverlapListener looks the ragdoll up by it)
		bcs.mCollisionGroup = CollisionGroup( rd->group, (CollisionGroup::GroupID)( h + 1 ), (CollisionGroup::SubGroupID)i );
		bcs.mMotionQuality = EMotionQuality::LinearCast;     // small fast parts don't tunnel through thin walls
		bcs.mFriction = 1.1f;       // (cloth and flesh on stone: he stays where he fell, on a step or a slope)
		bcs.mRestitution = 0.0f;
		bcs.mLinearDamping = 0.1f;
		bcs.mAngularDamping = 0.8f;      // limbs settle instead of swinging on and on
		bcs.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
		bcs.mMassPropertiesOverride.mMass = p->mass > 0 ? p->mass : 2.0f;
		bcs.mMaxLinearVelocity = PHYS_MAX_SPEED;
		bcs.mMaxAngularVelocity = PHYS_MAX_SPIN;
		bcs.mLinearVelocity = ToJolt( p->vel );
		rd->body[i] = BI().CreateAndAddBody( bcs, EActivation::Activate );
		if ( rd->body[i].IsInvalid() ) {
			for ( j = 0; j < i; j++ ) {
				RemoveBody( rd->body[j] );
			}
			rd->group = nullptr;
			return 0;
		}
	}

	for ( i = 0; i < numParts; i++ ) {
		const hrpPart_t *p = &parts[i];
		TwoBodyConstraint *c = nullptr;

		rd->joint[i] = nullptr;
		rd->jointType[i] = HRP_JOINT_NONE;
		rd->toneOn[i] = false;
		rd->toneMs[i] = p->toneMs;
		rd->parent[i] = p->parent;
		rd->rot0[i] = rot[i];
		rd->driven[i] = false;
		rd->hingeAxis[i] = Vec3::sAxisX();
		if ( p->parent < 0 || p->parent >= numParts || p->joint == HRP_JOINT_NONE ) {
			continue;
		}
		RVec3 pivot( ToJolt( p->pivot ) );
		if ( p->joint == HRP_JOINT_HINGE ) {
			HingeConstraintSettings s;
			Vec3 axis = Dir( p->hingeAxis ), normal = axis.GetNormalizedPerpendicular();
			s.mSpace = EConstraintSpace::WorldSpace;
			s.mPoint1 = s.mPoint2 = pivot;
			s.mHingeAxis1 = s.mHingeAxis2 = axis;
			s.mNormalAxis1 = s.mNormalAxis2 = normal;
			s.mLimitsMin = Rad( p->hingeMin );
			s.mLimitsMax = Rad( p->hingeMax );
			s.mMaxFrictionTorque = HRP_JOINT_FRICTION;
			s.mMotorSettings = MotorSettings( 4.0f, 1.0f, FLT_MAX, PHYS_TONE_TORQUE );
			c = BI().CreateConstraint( &s, rd->body[p->parent], rd->body[i] );
			rd->hingeAxis[i] = rot[p->parent].Conjugated() * axis;
		} else {
			SwingTwistConstraintSettings s;
			Vec3 t1 = Dir( p->twist1 ), t2 = Dir( p->twist2 ), pl = Square( t1, Dir( p->plane ) );
			s.mSpace = EConstraintSpace::WorldSpace;
			s.mPosition1 = s.mPosition2 = pivot;
			s.mTwistAxis1 = t1;
			s.mPlaneAxis1 = pl;
			s.mTwistAxis2 = t2;
			s.mPlaneAxis2 = Square( t2, Quat::sFromTo( t1, t2 ) * pl );
			s.mSwingType = ESwingType::Cone;
			s.mPlaneHalfConeAngle = Rad( p->planeHalf );
			s.mNormalHalfConeAngle = Rad( p->normalHalf );
			s.mTwistMinAngle = Rad( p->twistMin );
			s.mTwistMaxAngle = Rad( p->twistMax );
			s.mMaxFrictionTorque = HRP_JOINT_FRICTION;
			s.mSwingMotorSettings = s.mTwistMotorSettings = MotorSettings( 4.0f, 1.0f, FLT_MAX, PHYS_TONE_TORQUE );
			c = BI().CreateConstraint( &s, rd->body[p->parent], rd->body[i] );
		}
		if ( !c ) {
			continue;
		}
		w.system->AddConstraint( c );
		rd->joint[i] = c;
		rd->jointType[i] = p->joint;
		if ( p->toneMs > 0 ) {
			rd->toneOn[i] = true;
			if ( p->joint == HRP_JOINT_HINGE ) {
				HingeConstraint *hc = static_cast<HingeConstraint *>( c );
				hc->SetMotorState( EMotorState::Position );
				hc->SetTargetAngle( 0 );
			} else {
				SwingTwistConstraint *sc = static_cast<SwingTwistConstraint *>( c );
				sc->SetSwingMotorState( EMotorState::Position );
				sc->SetTwistMotorState( EMotorState::Position );
				sc->SetTargetOrientationBS( rot[p->parent].Conjugated() * rot[i] );
			}
		}
	}

	rd->inUse = true;
	return h + 1;
}

void HRP_DestroyRagdoll( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return;
	}
	HRP_RagdollRelease( h );
	for ( i = 0; i < rd->numParts; i++ ) {
		if ( rd->joint[i] ) {
			w.system->RemoveConstraint( rd->joint[i] );
			rd->joint[i] = nullptr;
		}
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		RemoveBody( rd->body[i] );
	}
	rd->group = nullptr;
	rd->inUse = false;
}

void HRP_RagdollPose( int h, float *pos, float *axis ) {
	Ragdoll *rd = GetRagdoll( h );
	int i, k;

	if ( !rd ) {
		return;
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		RVec3 p;
		Quat q;
		BI().GetPositionAndRotation( rd->body[i], p, q );
		Mat44 m = Mat44::sRotation( q );
		FromJolt( Vec3( p ), pos + i * 3 );
		for ( k = 0; k < 3; k++ ) {
			Vec3 col = m.GetColumn3( k );
			axis[i * 9 + k * 3 + 0] = col.GetX();
			axis[i * 9 + k * 3 + 1] = col.GetY();
			axis[i * 9 + k * 3 + 2] = col.GetZ();
		}
	}
}

// every part put where it's given (pos[3] and axis[9] per part, as HRP_RagdollPose), at rest
void HRP_RagdollSetPose( int h, const float *pos, const float *axis ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return;
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		BI().SetPositionAndRotation( rd->body[i], RVec3( ToJolt( pos + i * 3 ) ), AxisToQuat( axis + i * 9 ).Normalized(), EActivation::Activate );
		BI().SetLinearAndAngularVelocity( rd->body[i], Vec3::sZero(), Vec3::sZero() );
	}
}

void HRP_RagdollMove( int h, const float *delta ) {
	Ragdoll *rd = GetRagdoll( h );
	Vec3 d;
	int i;

	if ( !rd ) {
		return;
	}
	d = ToJolt( delta );
	for ( i = 0; i < rd->numParts; i++ ) {
		BI().SetPosition( rd->body[i], BI().GetPosition( rd->body[i] ) + d, EActivation::Activate );
	}
}

void HRP_RagdollAddVelocity( int h, int part, const float *vel ) {
	Ragdoll *rd = GetRagdoll( h );
	Vec3 v;
	int i;

	if ( !rd ) {
		return;
	}
	v = ToJolt( vel );
	for ( i = 0; i < rd->numParts; i++ ) {
		if ( part < 0 || part == i ) {
			BI().AddLinearVelocity( rd->body[i], v );
		}
	}
}

void HRP_RagdollAddVelocityAt( int h, int part, const float *vel, const float *point ) {
	Ragdoll *rd = GetRagdoll( h );
	float mass = 0;

	if ( !rd || part < 0 || part >= rd->numParts ) {
		return;
	}
	{
		BodyLockRead lock( w.system->GetBodyLockInterfaceNoLock(), rd->body[part] );
		if ( lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() ) {
			float inv = lock.GetBody().GetMotionPropertiesUnchecked()->GetInverseMass();
			mass = inv > 0 ? 1.0f / inv : 0;
		}
	}
	if ( mass > 0 ) {
		BI().AddImpulse( rd->body[part], ToJolt( vel ) * mass, RVec3( ToJolt( point ) ) );
	}
}

int HRP_RagdollAwake( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return 0;
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		if ( BI().IsActive( rd->body[i] ) ) {
			return 1;
		}
	}
	return 0;
}

void HRP_RagdollSleep( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( rd && !rd->grab ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			BI().DeactivateBody( rd->body[i] );
		}
	}
}

void HRP_RagdollWake( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( rd ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			BI().ActivateBody( rd->body[i] );
		}
	}
}

void HRP_RagdollGrab( int h, int part, const float *point ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd || part < 0 || part >= rd->numParts ) {
		return;
	}
	HRP_RagdollRelease( h );
	RVec3 at( ToJolt( point ) );
	BodyCreationSettings bcs( new SphereShape( 0.01f ), at, Quat::sIdentity(), EMotionType::Kinematic, LAYER_HAND );
	rd->hand = BI().CreateAndAddBody( bcs, EActivation::Activate );
	if ( rd->hand.IsInvalid() ) {
		return;
	}
	PointConstraintSettings s;
	s.mSpace = EConstraintSpace::WorldSpace;
	s.mPoint1 = s.mPoint2 = at;
	TwoBodyConstraint *c = BI().CreateConstraint( &s, rd->hand, rd->body[part] );
	if ( !c ) {
		RemoveBody( rd->hand );
		return;
	}
	w.system->AddConstraint( c );
	rd->grab = c;
	rd->grabPart = part;
	rd->grabTarget = Vec3( at );
	rd->grabSpeed = 0;
	// carried: the body hangs from the hand instead of whirling round it
	for ( i = 0; i < rd->numParts; i++ ) {
		BodyLockWrite lock( w.system->GetBodyLockInterfaceNoLock(), rd->body[i] );
		if ( lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() ) {
			lock.GetBody().GetMotionPropertiesUnchecked()->SetLinearDamping( 2.0f );
			lock.GetBody().GetMotionPropertiesUnchecked()->SetAngularDamping( 3.0f );
		}
	}
	HRP_RagdollWake( h );
}

void HRP_RagdollGrabMove( int h, const float *target, float maxSpeed ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( rd && rd->grab ) {
		rd->grabTarget = ToJolt( target );
		rd->grabSpeed = maxSpeed * PHYS_SCALE;
	}
}

void HRP_RagdollRelease( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd || !rd->grab ) {
		return;
	}
	w.system->RemoveConstraint( rd->grab );
	rd->grab = nullptr;
	RemoveBody( rd->hand );
	rd->grabPart = -1;
	for ( i = 0; i < rd->numParts; i++ ) {
		BodyLockWrite lock( w.system->GetBodyLockInterfaceNoLock(), rd->body[i] );
		if ( lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() ) {
			lock.GetBody().GetMotionPropertiesUnchecked()->SetLinearDamping( 0.1f );
			lock.GetBody().GetMotionPropertiesUnchecked()->SetAngularDamping( 0.8f );
		}
	}
}

/*
=================
HRP_RagdollSupport

Euphoria-style balance help while a body still fights to stay up: upright 0..1 keeps the
torso from tipping, lift 0..1 holds that share of his weight, maxDrift (units/s, 0 none)
caps how fast he slides across the floor. All 0: none. up: the way the torso is held
(world, unit; NULL straight up), so he can double over or reel back.
=================
*/
void HRP_RagdollSupport( int h, float upright, float lift, float maxDrift, const float *up ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( rd ) {
		rd->supUpright = upright > 0 ? upright : 0;
		rd->supLift = lift > 0 ? lift : 0;
		rd->supDrift = maxDrift > 0 ? maxDrift * PHYS_SCALE : 0;
		rd->supUp = up ? Vec3( up[0], up[1], up[2] ).NormalizedOr( Vec3::sAxisZ() ) : Vec3::sAxisZ();
	}
}

/*
=================
HRP_RagdollPin

The torso pulled towards pos/axis (world, as HRP_RagdollPose gives a part) by a critically
damped spring of omega rad/s, carrying the whole body's weight: a living body knocked about
by a shot, kept on his feet where his animation has him. omega 0: let go.
=================
*/
void HRP_RagdollPin( int h, const float *pos, const float *axis, float omega ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( !rd ) {
		return;
	}
	if ( !pos || !axis || omega <= 0 ) {
		rd->pinW = 0;
		return;
	}
	rd->pinW = omega;
	rd->pinPos = ToJolt( pos );
	rd->pinRot = AxisToQuat( axis );
}

/*
=================
HRP_RagdollTone

Per part, how stiffly its joint chases the pose (Hz) and with what share of the full torque,
over the default by depth (the trunk stiffest, the hands and feet softest): his legs stiff to
stand on while he staggers, say. NULL: back to the defaults.
=================
*/
void HRP_RagdollTone( int h, const float *hz, const float *torque ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return;
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		rd->toneHz[i] = hz ? hz[i] : 0;
		rd->toneTq[i] = torque ? torque[i] : 0;
	}
}

/*
=================
HRP_RagdollCatch

A safety net, not a spring: the torso is free to sink, rock and lean (a stagger), and only
once he's sunk more than slack below z, or leant past tiltDeg, does it start to hold him,
softly and harder the further past he goes, so he's caught rather than hoisted. z 0: off.
=================
*/
void HRP_RagdollCatch( int h, float z, float slack, float tiltDeg ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( !rd ) {
		return;
	}
	rd->catchZ = z * PHYS_SCALE;
	rd->catchSlack = ( slack > 1 ? slack : 1 ) * PHYS_SCALE;
	rd->catchTilt = cosf( tiltDeg * 0.0174533f );
}

/*
=================
HRP_RagdollScaleVelocity

every part's motion scaled by k (a stagger soaks up most of the shot that killed him)
=================
*/
/*
=================
HRP_RagdollGravity

Gravity on this body times k (and its air drag): a body fighting to stay up falls and
staggers with more weight than one at plain real gravity, which reads as floating.
=================
*/
/*
=================
HRP_RagdollLegPush

A leg standing on the floor pushes the hips up off the foot, share of his weight each: equal
and opposite forces on the hips and the foot, so a foot in the air just straightens and
only the floor under a planted one holds him up (muscles, not a lift). The joints only shape
the pose, so they can be soft as a man's.
=================
*/
void HRP_RagdollLegPush( int h, int footL, int footR, float shareL, float shareR ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( !rd ) {
		return;
	}
	rd->legFoot[0] = footL >= 0 && footL < rd->numParts ? footL : -1;
	rd->legFoot[1] = footR >= 0 && footR < rd->numParts ? footR : -1;
	rd->legShare[0] = shareL > 0 ? shareL : 0;
	rd->legShare[1] = shareR > 0 ? shareR : 0;
}

void HRP_RagdollGravity( int h, float k ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return;
	}
	for ( i = 0; i < rd->numParts; i++ ) {
		BodyLockWrite lock( w.system->GetBodyLockInterfaceNoLock(), rd->body[i] );
		if ( lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() ) {
			lock.GetBody().GetMotionPropertiesUnchecked()->SetGravityFactor( k );
			lock.GetBody().GetMotionPropertiesUnchecked()->SetAngularDamping( k > 1.0f ? 0.4f : 0.8f );
		}
	}
}

void HRP_RagdollScaleVelocity( int h, float k ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( rd ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			BI().SetLinearAndAngularVelocity( rd->body[i], BI().GetLinearVelocity( rd->body[i] ) * k,
											  BI().GetAngularVelocity( rd->body[i] ) * k );
		}
	}
}

/*
=================
HRP_RagdollDrive

An active ragdoll: each joint's motor chases the pose axis gives (per part, world rotations,
as HRP_RagdollPose returns them), with strength[part] of its full torque (0: limp). blend
eases from the pose the ragdoll was made in (0) to that pose (1). Only the turn of each part
against its parent counts: where the whole body lies is up to the physics.
=================
*/
void HRP_RagdollDrive( int h, const float *axis, const float *strength, float blend ) {
	Ragdoll *rd = GetRagdoll( h );
	bool any = false;
	int i;

	if ( !rd ) {
		return;
	}
	blend = blend < 0 ? 0 : ( blend > 1 ? 1 : blend );
	if ( !axis ) {
		// limp: the death pose isn't held either (UpdateTone), or a headshot stood him up a moment
		for ( i = 0; i < rd->numParts; i++ ) {
			rd->toneHz[i] = rd->toneTq[i] = 0;
		}
		for ( i = 1; i < rd->numParts; i++ ) {
			if ( rd->toneOn[i] && rd->joint[i] ) {
				rd->toneOn[i] = false;
				rd->driven[i] = true;       // (turned off just below)
			}
		}
	}
	for ( i = 1; i < rd->numParts; i++ ) {
		int p = rd->parent[i];
		float s = strength ? strength[i] : 0;

		if ( !rd->joint[i] || p < 0 || p >= rd->numParts ) {
			continue;
		}
		if ( s <= 0.01f || !axis ) {
			if ( rd->driven[i] || rd->toneOn[i] ) {     // (nor held stiff by tone: a plank)
				rd->driven[i] = false;
				rd->toneOn[i] = false;
				if ( rd->jointType[i] == HRP_JOINT_SWINGTWIST ) {
					SwingTwistConstraint *c = static_cast<SwingTwistConstraint *>( rd->joint[i].GetPtr() );
					c->SetSwingMotorState( EMotorState::Off );
					c->SetTwistMotorState( EMotorState::Off );
				} else {
					static_cast<HingeConstraint *>( rd->joint[i].GetPtr() )->SetMotorState( EMotorState::Off );
				}
			}
			continue;
		}
		s = s > 1 ? 1 : s;
		rd->driven[i] = true;
		rd->toneOn[i] = false;

		Quat rel0 = rd->rot0[p].Conjugated() * rd->rot0[i];
		Quat rel = ( AxisToQuat( axis + p * 9 ).Conjugated() * AxisToQuat( axis + i * 9 ) ).Normalized();
		if ( rel.Dot( rel0 ) < 0 ) {
			rel = -rel;
		}
		if ( blend < 1 ) {
			rel = rel0.SLERP( rel, blend ).Normalized();
		}
		// stiffest at the trunk (hips, shoulders, neck), softer at elbows and knees, softest
		// at hands and feet: the ends trail behind instead of moving like a plastic figure
		int depth = 0;
		for ( int q = p; q > 0 && q < rd->numParts && depth < 4; q = rd->parent[q] ) {
			depth++;
		}
		// soft as GTA IV's euphoria muscles (about 12 rad/s, 2 Hz, through most of the body):
		// hips and the spine 4 Hz, knees, shoulders and the neck 2.4, elbows, wrists and
		// ankles 1.8. The legs push his weight up off the floor (HRP_RagdollLegPush), so
		// the joints only shape the pose and give under every hit
		static const float hz[3] = { PHYS_DRIVE_HZ, PHYS_DRIVE_HZ * 0.6f, PHYS_DRIVE_HZ * 0.45f };
		static const float tq[3] = { 1.0f, 0.6f, 0.25f };
		depth = depth > 2 ? 2 : depth;
		float torque = PHYS_DRIVE_TORQUE * s * ( rd->toneTq[i] > 0 ? rd->toneTq[i] : tq[depth] ) * rd->tqScale[i];
		float freq = rd->toneHz[i] > 0 ? rd->toneHz[i] : hz[depth];
		if ( rd->jointType[i] == HRP_JOINT_SWINGTWIST ) {
			SwingTwistConstraint *c = static_cast<SwingTwistConstraint *>( rd->joint[i].GetPtr() );
			c->GetSwingMotorSettings().mSpringSettings.mFrequency = freq;
			c->GetTwistMotorSettings().mSpringSettings.mFrequency = freq;
			c->GetSwingMotorSettings().mSpringSettings.mDamping = 1.0f;
			c->GetTwistMotorSettings().mSpringSettings.mDamping = 1.0f;
			c->GetSwingMotorSettings().SetTorqueLimit( torque );
			c->GetTwistMotorSettings().SetTorqueLimit( torque );
			c->SetSwingMotorState( EMotorState::Position );
			c->SetTwistMotorState( EMotorState::Position );
			c->SetTargetOrientationBS( rel );
		} else {
			// R2 = R1 * rel, and at hinge angle a: rel = turn( axis, a ) * rel0
			HingeConstraint *c = static_cast<HingeConstraint *>( rd->joint[i].GetPtr() );
			Quat d = rel * rel0.Conjugated();
			if ( d.GetW() < 0 ) {
				d = -d;
			}
			float angle = 2.0f * atan2f( d.GetXYZ().Dot( rd->hingeAxis[i] ), d.GetW() );
			c->GetMotorSettings().mSpringSettings.mFrequency = freq;
			c->GetMotorSettings().mSpringSettings.mDamping = 1.0f;
			c->GetMotorSettings().SetTorqueLimit( torque );
			c->SetMotorState( EMotorState::Position );
			c->SetTargetAngle( angle );
		}
		any = true;
	}
	if ( any ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			if ( !BI().IsActive( rd->body[i] ) ) {
				BI().ActivateBody( rd->body[i] );
			}
		}
	}
}

/*
==============================================================================

Stepping

==============================================================================
*/

void HRP_Step( float seconds, float gravity ) {
	int i;

	if ( !initialized || seconds <= 0 ) {
		return;
	}
	if ( !HR_HostIsLib() ) {
		gravity = HR_LenToLib( gravity );
	}
	w.system->SetGravity( Vec3( 0, 0, -gravity * PHYS_SCALE ) );
	for ( i = 0; i < PHYS_MAX_MOVERS; i++ ) {
		Mover &m = movers[i];
		if ( m.inUse && m.added && m.hasTarget ) {
			BI().MoveKinematic( m.body, m.targetPos, m.targetRot, seconds );
			m.hasTarget = false;
			m.moving = true;
		} else if ( m.inUse && m.added && m.moving ) {
			// it got there last step: a kinematic body keeps its speed, so with two steps to
			// a frame (or a mover the host stops placing) it ran on past where it was put
			BI().SetLinearAndAngularVelocity( m.body, Vec3::sZero(), Vec3::sZero() );
			m.moving = false;
		}
	}
	for ( i = 0; i < PHYS_MAX_RAGDOLLS; i++ ) {
		Ragdoll &rd = ragdolls[i];
		if ( !rd.inUse ) {
			continue;
		}
		UpdateTone( rd, seconds * 1000.0f );
		if ( rd.supUpright > 0 || rd.supLift > 0 || rd.supDrift > 0 ) {
			// HRP_RagdollSupport: the torso kept upright (tilt only, he may turn), part of his
			// weight held, and his drift across the floor held to a stagger
			BodyID t = rd.body[0];
			Vec3 up = BI().GetRotation( t ) * Vec3::sAxisY(), w = BI().GetAngularVelocity( t );
			Vec3 torque = up.Cross( rd.supUp ) * ( PHYS_UPRIGHT_K * rd.supUpright ) -
						  Vec3( w.GetX(), w.GetY(), 0 ) * ( PHYS_UPRIGHT_D * std::fmin( rd.supUpright, 1.1f * std::sqrt( rd.supUpright ) ) );     // (a stronger spring wants only its root more damping: more shakes him, the step can't follow it)
			Vec3 force( 0, 0, rd.supLift * rd.totalMass * gravity * PHYS_SCALE );
			if ( rd.supDrift > 0 ) {
				Vec3 v = BI().GetLinearVelocity( t ), hv( v.GetX(), v.GetY(), 0 );
				float s = hv.Length();
				if ( s > rd.supDrift ) {
					force -= hv / s * ( ( s - rd.supDrift ) * rd.totalMass * 4.0f );
				}
			}
			BI().AddForceAndTorque( t, force, torque, EActivation::Activate );
		}
		for ( int k = 0; k < 2; k++ ) {
			// HRP_RagdollLegPush: the hips pushed up off a planted foot, the foot down into the floor
			int f = rd.legFoot[k];
			if ( f < 0 || rd.legShare[k] <= 0 ) {
				continue;
			}
			BodyID hb = rd.body[0], fb = rd.body[f];
			Vec3 along = Vec3( BI().GetCenterOfMassPosition( hb ) - BI().GetCenterOfMassPosition( fb ) );
			float len = along.Length();
			if ( len < 1e-3f ) {
				continue;
			}
			along /= len;
			float gk = 1.0f;
			{
				BodyLockRead lock( w.system->GetBodyLockInterfaceNoLock(), hb );
				if ( lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() ) {
					gk = lock.GetBody().GetMotionPropertiesUnchecked()->GetGravityFactor();
				}
			}
			// (its upward part carries the share; leaning far over, it gives)
			float up = along.GetZ() > 0.5f ? along.GetZ() : 0.5f;
			Vec3 push = along * ( rd.legShare[k] * rd.totalMass * gravity * PHYS_SCALE * gk / up );
			BI().AddForce( hb, push, EActivation::Activate );
			BI().AddForce( fb, -push, EActivation::Activate );
		}
		if ( rd.pinW > 0 ) {
			// HRP_RagdollPin: a damped spring on the torso, his whole weight carried (the
			// spring, not a lift, holds him: the feet pushing on the floor can't launch him)
			BodyID t = rd.body[0];
			RVec3 p;
			Quat q;
			BI().GetPositionAndRotation( t, p, q );
			Vec3 v = BI().GetLinearVelocity( t ), av = BI().GetAngularVelocity( t ), err = rd.pinPos - Vec3( p );
			float wv = rd.pinW, m = rd.totalMass, most = m * gravity * PHYS_SCALE * 3.0f;
			Vec3 force = ( err * ( wv * wv ) - v * ( 2.0f * wv ) ) * m + Vec3( 0, 0, m * gravity * PHYS_SCALE );
			if ( force.Length() > most ) {
				force *= most / force.Length();
			}
			Quat d = rd.pinRot * q.Conjugated();
			if ( d.GetW() < 0 ) {
				d = -d;
			}
			Vec3 ang = d.GetXYZ() * 2.0f;       // (small turns: axis times angle)
			Vec3 torque = ( ang * ( wv * wv ) - av * ( 2.0f * wv ) ) * ( m * 0.05f );
			BI().AddForceAndTorque( t, force, torque, EActivation::Activate );
		}
		if ( rd.catchZ != 0 && !rd.grab ) {
			// HRP_RagdollCatch: nothing inside the slack; past it, a share of his weight that
			// grows with how far past, and only his fall is slowed (never a push up faster
			// than a man straightening his knees)
			BodyID t = rd.body[0];
			RVec3 p;
			Quat q;
			BI().GetPositionAndRotation( t, p, q );
			Vec3 v = BI().GetLinearVelocity( t ), av = BI().GetAngularVelocity( t );
			float m = rd.totalMass, weight = m * gravity * PHYS_SCALE;
			float sink = ( rd.catchZ - rd.catchSlack ) - (float)p.GetZ(), fz = 0;
			if ( sink > 0 ) {
				float k = sink / ( rd.catchSlack * 2.0f );
				k = k > 1 ? 1 : k;
				fz = weight * ( 0.3f + 0.9f * k );     // (past the slack: up to a bit more than his weight)
				float vz = v.GetZ(), rise = 30.0f * PHYS_SCALE;
				if ( vz < 0 ) {
					fz -= vz * m * 8.0f;                // his drop braked
				} else if ( vz > rise ) {
					fz = 0;                             // (rising already: let him come up on his own)
				}
				if ( fz > weight * 2.5f ) {
					fz = weight * 2.5f;
				}
			}
			Vec3 up = q * Vec3::sAxisY(), torque = Vec3::sZero();
			if ( up.GetZ() < rd.catchTilt ) {
				// leant past where he'd still recover by himself: pulled back toward upright,
				// and only his tipping further over is damped, so the rock of a hit stays
				Vec3 axis = up.Cross( Vec3::sAxisZ() );
				float s = axis.Length();
				if ( s > 1e-4f ) {
					axis /= s;
					float over = ( rd.catchTilt - up.GetZ() ) / ( rd.catchTilt + 0.2f );
					over = over > 1 ? 1 : over;
					float tipRate = -av.Dot( axis );    // > 0: tipping further
					float mag = m * 0.11f * ( 60.0f * over + ( tipRate > 0 ? tipRate * 5.0f : 0 ) );
					torque = axis * mag;
				}
			}
			if ( fz > 0 || torque.LengthSq() > 0 ) {
				BI().AddForceAndTorque( t, Vec3( 0, 0, fz ), torque, EActivation::Activate );
			}
		}
		if ( rd.grab ) {
			// towards the hand, quickly but not in one jump (a snap stretches the whole body)
			Vec3 at = Vec3( BI().GetPosition( rd.hand ) ), to = rd.grabTarget - at;
			float dist = to.Length(), most = rd.grabSpeed * seconds;
			if ( most > 0 && dist > most ) {
				to *= most / dist;
			}
			BI().MoveKinematic( rd.hand, RVec3( at + to ), Quat::sIdentity(), seconds );
		}
	}
	w.system->Update( seconds, 1, w.temp, w.jobs );
}

/*
=================
HRP_RayCast

a line through the level and the movers (never the ragdolls), host space: the fraction along
it where it first hits (1: nothing) and that point. Starting inside something gives 0.
=================
*/
namespace {
class LevelOnlyFilter : public ObjectLayerFilter {
public:
	bool ShouldCollide( ObjectLayer layer ) const override {
		return layer == LAYER_WORLD || layer == LAYER_MOVER;
	}
};
}

int HRP_RayCast( const float *start, const float *end, float *fraction, float *endpos ) {
	float s[3], e[3];
	int k;

	*fraction = 1;
	for ( k = 0; k < 3; k++ ) {
		endpos[k] = end[k];
	}
	if ( !initialized ) {
		return 0;
	}
	HR_PointToLib( start, s );
	HR_PointToLib( end, e );
	RRayCast ray( RVec3( ToJolt( s ) ), ToJolt( e ) - ToJolt( s ) );
	RayCastResult hit;
	LevelOnlyFilter filter;
	if ( w.system->GetNarrowPhaseQuery().CastRay( ray, hit, { }, filter ) ) {
		float l[3];
		*fraction = hit.mFraction;
		for ( k = 0; k < 3; k++ ) {
			l[k] = s[k] + ( e[k] - s[k] ) * hit.mFraction;
		}
		HR_PointToHost( l, endpos );
		return hit.mFraction <= 0;      // a ray starting inside a solid hits at once
	}
	return 0;
}

void HRP_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris ) {
	*bodies = initialized ? (int)w.system->GetNumBodies() : 0;
	*activeBodies = initialized ? (int)w.system->GetNumActiveBodies( EBodyType::RigidBody ) : 0;
	*worldHulls = w.totalHulls;
	*worldTris = w.totalTris;
}

} // extern "C"

#else // HR_NO_JOLT

extern "C" {
int HRP_Init( void ( *print )( const char *msg ) ) { (void)print; return 0; }
void HRP_Shutdown( void ) { }
int HRP_Active( void ) { return 0; }
void HRP_BeginWorld( void ) { }
void HRP_AddWorldHull( const float *points, int numPoints ) { (void)points; (void)numPoints; }
void HRP_AddWorldTriangle( const float *a, const float *b, const float *c ) { (void)a; (void)b; (void)c; }
void HRP_EndWorld( void ) { }
int HRP_CreateMover( void ) { return 0; }
void HRP_MoverAddHull( int mover, const float *points, int numPoints ) { (void)mover; (void)points; (void)numPoints; }
int HRP_MoverFinish( int mover ) { (void)mover; return 0; }
void HRP_MoverPlace( int mover, const float *origin, const float *axis, int teleport ) { (void)mover; (void)origin; (void)axis; (void)teleport; }
void HRP_MoverHide( int mover ) { (void)mover; }
int HRP_CreateRagdoll( const hrpPart_t *parts, int numParts, const unsigned char *noCollide ) { (void)parts; (void)numParts; (void)noCollide; return 0; }
void HRP_DestroyRagdoll( int rd ) { (void)rd; }
void HRP_RagdollPose( int rd, float *pos, float *axis ) { (void)rd; (void)pos; (void)axis; }
void HRP_RagdollMove( int rd, const float *delta ) { (void)rd; (void)delta; }
void HRP_RagdollSetPose( int rd, const float *pos, const float *axis ) { (void)rd; (void)pos; (void)axis; }
void HRP_RagdollAddVelocity( int rd, int part, const float *vel ) { (void)rd; (void)part; (void)vel; }
void HRP_RagdollAddVelocityAt( int rd, int part, const float *vel, const float *point ) { (void)rd; (void)part; (void)vel; (void)point; }
int HRP_RagdollAwake( int rd ) { (void)rd; return 0; }
void HRP_RagdollSleep( int rd ) { (void)rd; }
void HRP_RagdollWake( int rd ) { (void)rd; }
void HRP_RagdollGrab( int rd, int part, const float *point ) { (void)rd; (void)part; (void)point; }
void HRP_RagdollGrabMove( int rd, const float *target, float maxSpeed ) { (void)rd; (void)target; (void)maxSpeed; }
void HRP_RagdollRelease( int rd ) { (void)rd; }
void HRP_RagdollDrive( int rd, const float *axis, const float *strength, float blend ) { (void)rd; (void)axis; (void)strength; (void)blend; }
void HRP_RagdollScaleVelocity( int rd, float k ) { (void)rd; (void)k; }
void HRP_RagdollGravity( int rd, float k ) { (void)rd; (void)k; }
void HRP_RagdollLegPush( int rd, int footL, int footR, float shareL, float shareR ) { (void)rd; (void)footL; (void)footR; (void)shareL; (void)shareR; }
void HRP_RagdollPin( int rd, const float *pos, const float *axis, float omega ) { (void)rd; (void)pos; (void)axis; (void)omega; }
void HRP_RagdollSupport( int rd, float upright, float lift, float maxDrift, const float *up ) { (void)rd; (void)upright; (void)lift; (void)maxDrift; (void)up; }
void HRP_RagdollCatch( int rd, float z, float slack, float tiltDeg ) { (void)rd; (void)z; (void)slack; (void)tiltDeg; }
void HRP_RagdollTone( int rd, const float *hz, const float *torque ) { (void)rd; (void)hz; (void)torque; }
void HRP_Step( float seconds, float gravity ) { (void)seconds; (void)gravity; }
int HRP_RayCast( const float *start, const float *end, float *fraction, float *endpos ) { (void)start; *fraction = 1; endpos[0] = end[0]; endpos[1] = end[1]; endpos[2] = end[2]; return 0; }
void HRP_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris ) { *bodies = *activeBodies = *worldHulls = *worldTris = 0; }
}

#endif
