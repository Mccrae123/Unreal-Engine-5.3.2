// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#if INCLUDE_CHAOS

#include "Physics/ImmediatePhysics/ImmediatePhysicsChaos/ImmediatePhysicsDeclares_Chaos.h"
#include "Physics/ImmediatePhysics/ImmediatePhysicsShared/ImmediatePhysicsCore.h"

#include "Chaos/ParticleHandleFwd.h"

namespace Chaos
{
	template<typename T, int D> class TImplicitObject;
	template<typename T, int D> struct TKinematicGeometryParticleParameters;
	template<typename P, typename T, int D> class TPBDConstraintIslandRule;
	template<typename T, int D> class TPBDJointConstraintHandle;
	template<typename T, int D> class TPBDJointConstraints;
	template<typename T, int D> class TPBD6DJointConstraintHandle;
	template<typename T, int D> class TPBD6DJointConstraints;
	template<typename T, int D> struct TPBDRigidParticleParameters;
	template<typename T, int D> class TPBDRigidsEvolutionGBF;
	template<typename T, int D> class TPBDRigidsSOAs;
	template<typename T, int D> class TPerShapeData;
}

namespace ImmediatePhysics_Chaos
{
	using FReal = float;
	const int Dimensions = 3;

	using EActorType = ImmediatePhysics_Shared::EActorType;
	using EForceType = ImmediatePhysics_Shared::EForceType;
}

struct FBodyInstance;
struct FConstraintInstance;

#endif

// Used to define out code that still has to be implemented to match PhysX
#define IMMEDIATEPHYSICS_CHAOS_TODO 0
