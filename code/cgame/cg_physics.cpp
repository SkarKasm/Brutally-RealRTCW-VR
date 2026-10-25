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

// cg_physics.cpp -- Jolt Physics (MIT licence, code/JoltPhysics) for the client's ragdolls.
//
// The only C++ in the cgame: a small wrapper the C code calls through cg_physics.h. One
// physics world per level, stepped by cg_ragdoll.c at a fixed rate on the main thread
// (single threaded job system: the same results every run, no threads to manage across
// the DLL being loaded and unloaded on every map change).
//
// The world is static: the level's brushes as convex hulls and its curves as triangles,
// built once on map load (cg_physworld.c). Doors, lifts and other brush entities are
// kinematic bodies the game moves. Nothing here is seen by the server or saved.

#include "cg_physics.h"

#ifdef USE_JOLT

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

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <new>

using namespace JPH;

#define PHYS_SCALE          0.0254f     // game units are about an inch: Jolt wants metres
#define PHYS_MAX_RAGDOLLS   128
#define PHYS_MAX_MOVERS     1024
#define PHYS_WORLD_CHUNK    2048        // hulls per static body
#define PHYS_HULL_RADIUS    0.01f       // rounded hull edges (m), so bodies slide off corners
#define PHYS_TONE_TORQUE    220.0f      // N m a joint holds the death pose with, at first
#define PHYS_MAX_SPEED      16.5f       // m/s (650 units/s, as the old ragdolls): nothing is flung faster
#define PHYS_MAX_SPIN       12.0f       // rad/s (about two turns a second): no helicopters
#define PHYS_DEEP_OVERLAP   0.04f       // m: two bodies this far inside each other pass through instead

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
	BodyID              body[PHYS_MAX_PARTS];
	Ref<Constraint>     joint[PHYS_MAX_PARTS];      // part i to its parent
	int                 jointType[PHYS_MAX_PARTS];
	float               toneMs[PHYS_MAX_PARTS];
	bool                toneOn[PHYS_MAX_PARTS];
	float               age;                        // ms since it went limp
	Ref<GroupFilterTable> group;
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

/*
Two bodies dying on the same spot (a pile after a grenade, a squad cut down in a doorway) start
inside each other, and pushing them apart threw both into the air. Parts of different ragdolls
that are deep inside each other pass through until they are apart; touching ones collide as
usual, so bodies still pile up.
*/
class OverlapListener : public ContactListener {
public:
	void Check( const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s ) {
		if ( b1.GetObjectLayer() == LAYER_BODY && b2.GetObjectLayer() == LAYER_BODY &&
			 b1.GetCollisionGroup().GetGroupID() != b2.GetCollisionGroup().GetGroupID() &&
			 m.mPenetrationDepth > PHYS_DEEP_OVERLAP ) {
			s.mIsSensor = true;
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
Ragdoll     ragdolls[PHYS_MAX_RAGDOLLS];
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
			Phys_DestroyRagdoll( i + 1 );
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

		if ( !rd.toneOn[i] || !rd.joint[i] ) {
			continue;
		}
		k = rd.toneMs[i] > 0 ? 1.0f - rd.age / rd.toneMs[i] : 0;
		if ( k <= 0 ) {
			rd.toneOn[i] = false;
			if ( rd.jointType[i] == PHYS_JOINT_SWINGTWIST ) {
				SwingTwistConstraint *c = static_cast<SwingTwistConstraint *>( rd.joint[i].GetPtr() );
				c->SetSwingMotorState( EMotorState::Off );
				c->SetTwistMotorState( EMotorState::Off );
			} else {
				static_cast<HingeConstraint *>( rd.joint[i].GetPtr() )->SetMotorState( EMotorState::Off );
			}
			continue;
		}
		k = PHYS_TONE_TORQUE * k * k;
		if ( rd.jointType[i] == PHYS_JOINT_SWINGTWIST ) {
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

int Phys_Init( void ( *print )( const char *msg ) ) {
	physPrint = print;
	if ( initialized ) {
		Phys_Shutdown();
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

void Phys_Shutdown( void ) {
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

int Phys_Active( void ) {
	return initialized;
}

/*
==============================================================================

The level

==============================================================================
*/

void Phys_BeginWorld( void ) {
	if ( !initialized ) {
		return;
	}
	ClearAll();
}

void Phys_AddWorldHull( const float *points, int numPoints ) {
	Vec3 centre;

	if ( !initialized ) {
		return;
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

void Phys_AddWorldTriangle( const float *a, const float *b, const float *c ) {
	if ( !initialized ) {
		return;
	}
	Vec3 va = ToJolt( a ), vb = ToJolt( b ), vc = ToJolt( c );
	w.tris.push_back( Triangle( Float3( va.GetX(), va.GetY(), va.GetZ() ), Float3( vb.GetX(), vb.GetY(), vb.GetZ() ),
								Float3( vc.GetX(), vc.GetY(), vc.GetZ() ) ) );
}

void Phys_EndWorld( void ) {
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

int Phys_CreateMover( void ) {
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
			return i + 1;
		}
	}
	return 0;
}

void Phys_MoverAddHull( int h, const float *points, int numPoints ) {
	Mover *m = GetMover( h );
	Vec3 centre;

	if ( !m || !m->hulls ) {
		return;
	}
	RefConst<Shape> hull = MakeHull( points, numPoints, centre );
	if ( hull ) {
		m->hulls->AddShape( centre, Quat::sIdentity(), hull );
		m->numHulls++;
	}
}

int Phys_MoverFinish( int h ) {
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

void Phys_MoverPlace( int h, const float *origin, const float *axis, int teleport ) {
	Mover *m = GetMover( h );

	if ( !m || m->body.IsInvalid() ) {
		return;
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
		m->hasTarget = false;
		return;
	}
	m->targetPos = pos;
	m->targetRot = rot;
	m->hasTarget = true;
}

void Phys_MoverHide( int h ) {
	Mover *m = GetMover( h );

	if ( m && m->added ) {
		BI().RemoveBody( m->body );
		m->added = false;
		m->hasTarget = false;
	}
}

/*
==============================================================================

Ragdolls

==============================================================================
*/

int Phys_CreateRagdoll( const physPart_t *parts, int numParts, const unsigned char *noCollide ) {
	Ragdoll *rd = nullptr;
	Quat rot[PHYS_MAX_PARTS];
	int i, j, h;

	if ( !initialized || numParts <= 0 || numParts > PHYS_MAX_PARTS ) {
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
	rd->grab = nullptr;
	rd->hand = BodyID();
	rd->grabPart = -1;

	// parts next to each other, and parts that were already inside each other at death (an arm
	// through the chest in the death animation), don't collide: pushing those apart spins bodies
	rd->group = new GroupFilterTable( numParts );
	for ( i = 0; i < numParts; i++ ) {
		if ( parts[i].parent >= 0 ) {
			rd->group->DisableCollision( i, parts[i].parent );
		}
		for ( j = i + 1; j < numParts; j++ ) {
			if ( noCollide && noCollide[i * numParts + j] ) {
				rd->group->DisableCollision( i, j );
			}
		}
	}

	for ( i = 0; i < numParts; i++ ) {
		const physPart_t *p = &parts[i];
		RefConst<Shape> shape;
		Vec3 a = ToJolt( p->a ), b = ToJolt( p->b ), centre = ( a + b ) * 0.5f;
		float r = p->radius * PHYS_SCALE;

		if ( p->shape == PHYS_SHAPE_BOX ) {
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
		bcs.mCollisionGroup = CollisionGroup( rd->group, (CollisionGroup::GroupID)h, (CollisionGroup::SubGroupID)i );
		bcs.mMotionQuality = EMotionQuality::LinearCast;     // small fast parts don't tunnel through thin walls
		bcs.mFriction = 0.8f;
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
		const physPart_t *p = &parts[i];
		TwoBodyConstraint *c = nullptr;

		rd->joint[i] = nullptr;
		rd->jointType[i] = PHYS_JOINT_NONE;
		rd->toneOn[i] = false;
		rd->toneMs[i] = p->toneMs;
		if ( p->parent < 0 || p->parent >= numParts || p->joint == PHYS_JOINT_NONE ) {
			continue;
		}
		RVec3 pivot( ToJolt( p->pivot ) );
		if ( p->joint == PHYS_JOINT_HINGE ) {
			HingeConstraintSettings s;
			Vec3 axis = Dir( p->hingeAxis ), normal = axis.GetNormalizedPerpendicular();
			s.mSpace = EConstraintSpace::WorldSpace;
			s.mPoint1 = s.mPoint2 = pivot;
			s.mHingeAxis1 = s.mHingeAxis2 = axis;
			s.mNormalAxis1 = s.mNormalAxis2 = normal;
			s.mLimitsMin = Rad( p->hingeMin );
			s.mLimitsMax = Rad( p->hingeMax );
			s.mMaxFrictionTorque = 1.5f;
			s.mMotorSettings = MotorSettings( 4.0f, 1.0f, FLT_MAX, PHYS_TONE_TORQUE );
			c = BI().CreateConstraint( &s, rd->body[p->parent], rd->body[i] );
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
			s.mMaxFrictionTorque = 1.5f;
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
			if ( p->joint == PHYS_JOINT_HINGE ) {
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

void Phys_DestroyRagdoll( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd ) {
		return;
	}
	Phys_RagdollRelease( h );
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

void Phys_RagdollPose( int h, float *pos, float *axis ) {
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

void Phys_RagdollMove( int h, const float *delta ) {
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

void Phys_RagdollAddVelocity( int h, int part, const float *vel ) {
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

void Phys_RagdollAddVelocityAt( int h, int part, const float *vel, const float *point ) {
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

int Phys_RagdollAwake( int h ) {
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

void Phys_RagdollSleep( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( rd && !rd->grab ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			BI().DeactivateBody( rd->body[i] );
		}
	}
}

void Phys_RagdollWake( int h ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( rd ) {
		for ( i = 0; i < rd->numParts; i++ ) {
			BI().ActivateBody( rd->body[i] );
		}
	}
}

void Phys_RagdollGrab( int h, int part, const float *point ) {
	Ragdoll *rd = GetRagdoll( h );
	int i;

	if ( !rd || part < 0 || part >= rd->numParts ) {
		return;
	}
	Phys_RagdollRelease( h );
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
	Phys_RagdollWake( h );
}

void Phys_RagdollGrabMove( int h, const float *target, float maxSpeed ) {
	Ragdoll *rd = GetRagdoll( h );

	if ( rd && rd->grab ) {
		rd->grabTarget = ToJolt( target );
		rd->grabSpeed = maxSpeed * PHYS_SCALE;
	}
}

void Phys_RagdollRelease( int h ) {
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
==============================================================================

Stepping

==============================================================================
*/

void Phys_Step( float seconds, float gravity ) {
	int i;

	if ( !initialized || seconds <= 0 ) {
		return;
	}
	w.system->SetGravity( Vec3( 0, 0, -gravity * PHYS_SCALE ) );
	for ( i = 0; i < PHYS_MAX_MOVERS; i++ ) {
		Mover &m = movers[i];
		if ( m.inUse && m.added && m.hasTarget ) {
			BI().MoveKinematic( m.body, m.targetPos, m.targetRot, seconds );
			m.hasTarget = false;
		}
	}
	for ( i = 0; i < PHYS_MAX_RAGDOLLS; i++ ) {
		Ragdoll &rd = ragdolls[i];
		if ( !rd.inUse ) {
			continue;
		}
		UpdateTone( rd, seconds * 1000.0f );
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

void Phys_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris ) {
	*bodies = initialized ? (int)w.system->GetNumBodies() : 0;
	*activeBodies = initialized ? (int)w.system->GetNumActiveBodies( EBodyType::RigidBody ) : 0;
	*worldHulls = w.totalHulls;
	*worldTris = w.totalTris;
}

} // extern "C"

#else // !USE_JOLT

extern "C" {
int Phys_Init( void ( *print )( const char *msg ) ) { (void)print; return 0; }
void Phys_Shutdown( void ) { }
int Phys_Active( void ) { return 0; }
void Phys_BeginWorld( void ) { }
void Phys_AddWorldHull( const float *points, int numPoints ) { (void)points; (void)numPoints; }
void Phys_AddWorldTriangle( const float *a, const float *b, const float *c ) { (void)a; (void)b; (void)c; }
void Phys_EndWorld( void ) { }
int Phys_CreateMover( void ) { return 0; }
void Phys_MoverAddHull( int mover, const float *points, int numPoints ) { (void)mover; (void)points; (void)numPoints; }
int Phys_MoverFinish( int mover ) { (void)mover; return 0; }
void Phys_MoverPlace( int mover, const float *origin, const float *axis, int teleport ) { (void)mover; (void)origin; (void)axis; (void)teleport; }
void Phys_MoverHide( int mover ) { (void)mover; }
int Phys_CreateRagdoll( const physPart_t *parts, int numParts, const unsigned char *noCollide ) { (void)parts; (void)numParts; (void)noCollide; return 0; }
void Phys_DestroyRagdoll( int rd ) { (void)rd; }
void Phys_RagdollPose( int rd, float *pos, float *axis ) { (void)rd; (void)pos; (void)axis; }
void Phys_RagdollMove( int rd, const float *delta ) { (void)rd; (void)delta; }
void Phys_RagdollAddVelocity( int rd, int part, const float *vel ) { (void)rd; (void)part; (void)vel; }
void Phys_RagdollAddVelocityAt( int rd, int part, const float *vel, const float *point ) { (void)rd; (void)part; (void)vel; (void)point; }
int Phys_RagdollAwake( int rd ) { (void)rd; return 0; }
void Phys_RagdollSleep( int rd ) { (void)rd; }
void Phys_RagdollWake( int rd ) { (void)rd; }
void Phys_RagdollGrab( int rd, int part, const float *point ) { (void)rd; (void)part; (void)point; }
void Phys_RagdollGrabMove( int rd, const float *target, float maxSpeed ) { (void)rd; (void)target; (void)maxSpeed; }
void Phys_RagdollRelease( int rd ) { (void)rd; }
void Phys_Step( float seconds, float gravity ) { (void)seconds; (void)gravity; }
void Phys_Stats( int *bodies, int *activeBodies, int *worldHulls, int *worldTris ) { *bodies = *activeBodies = *worldHulls = *worldTris = 0; }
}

#endif
