// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/BoundingVolumeUtilities.h"
#include "Chaos/Collision/CollisionReceiver.h"
#include "Chaos/Collision/NarrowPhase.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDRigidsSOAs.h"
#include "ChaosStats.h"

namespace Chaos
{
	DECLARE_CYCLE_STAT_EXTERN(TEXT("Collisions::BroadPhase"), STAT_Collisions_BroadPhase, STATGROUP_ChaosCollision, CHAOS_API);

	/**
	 * Run through a list of particle pairs and pass them onto the collision detector if their AABBs overlap.
	 * No spatial acceleration, and the order is assumed to be already optimized for cache efficiency.
	 */
	class CHAOS_API FParticlePairBroadPhase
	{
	public:
		using FParticlePair = TVector<TGeometryParticleHandle<FReal, 3>*, 2>;
		using FAABB = TAABB<FReal, 3>;

		FParticlePairBroadPhase(const TArray<FParticlePair>& InParticlePairs)
			: ParticlePairs(InParticlePairs)
		{
		}

		/**
		 *
		 */
		void ProduceOverlaps(FReal Dt,
			FNarrowPhase& NarrowPhase,
			FSyncCollisionReceiver& Receiver,
			CollisionStats::FStatData& StatData)
		{
			SCOPE_CYCLE_COUNTER(STAT_Collisions_BroadPhase);

			const FReal AABBExpansion = 1.0f;		// WRONG!!!!

			FCollisionConstraintsArray NewConstraints;
			for (const FParticlePair& ParticlePair : ParticlePairs)
			{
				// Array is const, things in it are not...
				TGeometryParticleHandle<FReal, 3>* Particle0 = const_cast<TGeometryParticleHandle<FReal, 3>*>(ParticlePair[0]);
				TGeometryParticleHandle<FReal, 3>* Particle1 = const_cast<TGeometryParticleHandle<FReal, 3>*>(ParticlePair[1]);

				// Particles may have been disabled or made kinematic
				bool bAnyDisabled = TGenericParticleHandle<FReal, 3>(Particle0)->Disabled() || TGenericParticleHandle<FReal, 3>(Particle1)->Disabled();
				bool bAnyDynamic = TGenericParticleHandle<FReal, 3>(Particle0)->IsDynamic() || TGenericParticleHandle<FReal, 3>(Particle1)->IsDynamic();
				if (bAnyDynamic && !bAnyDisabled)
				{
					const TAABB<FReal, 3>& Box0 = Particle0->WorldSpaceInflatedBounds();
					const TAABB<FReal, 3>& Box1 = Particle1->WorldSpaceInflatedBounds();
					if (Box0.Intersects(Box1))
					{
						NarrowPhase.GenerateCollisions(NewConstraints, Dt,Particle0, Particle1, AABBExpansion, StatData);
						Receiver.ReceiveCollisions(NewConstraints);
						NewConstraints.Empty();
					}
				}
			}
		}

	private:
		const TArray<FParticlePair>& ParticlePairs;
	};
}
