// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ExternalCollisionData.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/CollisionResolutionTypes.h"

namespace Chaos
{
	namespace Collisions
	{
		template<class T = float>
		struct TPlaneContactParticleParameters {
			TArrayCollectionArray<bool>* Collided;
			const TArrayCollectionArray<TSerializablePtr<FChaosPhysicsMaterial>>* PhysicsMaterials;
			T FrictionOverride;
			T AngularFrictionOverride;
		};

		template<class T = float>
		struct TPlaneContactIterationParameters {
			const T Dt;
			const int32 Iteration;
			const int32 NumIterations;
			const int32 NumPairIterations;
			bool* NeedsAnotherIteration;
		};

		template<ECollisionUpdateType UpdateType, typename T, int d>
		void Update(const T Thickness, TRigidBodyPlaneContactConstraint<T, d>& Constraint);

		template<typename T, int d>
		void Apply(TRigidBodyPlaneContactConstraint<T, d>& Constraint, T Thickness, TPlaneContactIterationParameters<T> & IterationParameters, TPlaneContactParticleParameters<T> & ParticleParameters);

		template<typename T, int d>
		void ApplyPushOut(TRigidBodyPlaneContactConstraint<T, d>& Constraint, T Thickness, const TSet<const TGeometryParticleHandle<T, d>*>& IsTemporarilyStatic,
			TPlaneContactIterationParameters<T> & IterationParameters, TPlaneContactParticleParameters<T> & ParticleParameters);


	}

}
