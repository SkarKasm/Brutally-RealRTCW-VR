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

// cg_physics.h -- the Jolt physics is HitReact's (code/hitreact): its HRP_* layer under the
// names the cgame has always used for it. Everything is in game units (about an inch), which
// are HitReact's own units, z up.

#ifndef CG_PHYSICS_H
#define CG_PHYSICS_H

#include "../hitreact/hitreact.h"

#define PHYS_MAX_PARTS              HRP_MAX_PARTS
#define PHYS_SHAPE_CAPSULE          HRP_SHAPE_CAPSULE
#define PHYS_SHAPE_BOX              HRP_SHAPE_BOX
#define PHYS_JOINT_NONE             HRP_JOINT_NONE
#define PHYS_JOINT_SWINGTWIST       HRP_JOINT_SWINGTWIST
#define PHYS_JOINT_HINGE            HRP_JOINT_HINGE
typedef hrpPart_t physPart_t;

#define Phys_Init                   HRP_Init
#define Phys_Shutdown               HRP_Shutdown
#define Phys_Active                 HRP_Active
#define Phys_BeginWorld             HRP_BeginWorld
#define Phys_AddWorldHull           HRP_AddWorldHull
#define Phys_AddWorldTriangle       HRP_AddWorldTriangle
#define Phys_EndWorld               HRP_EndWorld
#define Phys_CreateMover            HRP_CreateMover
#define Phys_MoverAddHull           HRP_MoverAddHull
#define Phys_MoverFinish            HRP_MoverFinish
#define Phys_MoverPlace             HRP_MoverPlace
#define Phys_MoverHide              HRP_MoverHide
#define Phys_CreateRagdoll          HRP_CreateRagdoll
#define Phys_DestroyRagdoll         HRP_DestroyRagdoll
#define Phys_RagdollPose            HRP_RagdollPose
#define Phys_RagdollMove            HRP_RagdollMove
#define Phys_RagdollSetPose         HRP_RagdollSetPose
#define Phys_RagdollAddVelocity     HRP_RagdollAddVelocity
#define Phys_RagdollAddVelocityAt   HRP_RagdollAddVelocityAt
#define Phys_RagdollAwake           HRP_RagdollAwake
#define Phys_RagdollSleep           HRP_RagdollSleep
#define Phys_RagdollWake            HRP_RagdollWake
#define Phys_RagdollGrab            HRP_RagdollGrab
#define Phys_RagdollGrabMove        HRP_RagdollGrabMove
#define Phys_RagdollRelease         HRP_RagdollRelease
#define Phys_RagdollDrive           HRP_RagdollDrive
#define Phys_RagdollSupport         HRP_RagdollSupport
#define Phys_RagdollScaleVelocity   HRP_RagdollScaleVelocity
#define Phys_RagdollGravity         HRP_RagdollGravity
#define Phys_RagdollLegPush         HRP_RagdollLegPush
#define Phys_RagdollPin             HRP_RagdollPin
#define Phys_RagdollTone            HRP_RagdollTone
#define Phys_RagdollCatch           HRP_RagdollCatch
#define Phys_Step                   HRP_Step
#define Phys_Stats                  HRP_Stats

#endif
