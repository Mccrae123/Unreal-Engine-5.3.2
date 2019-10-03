// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/PBDCollisionConstraint.h"
#include "Chaos/PBDCollisionConstraintPGS.h"
#include "Chaos/PBDConstraintGraph.h"
#include "Chaos/PBDRigidClustering.h"
#include "Chaos/PBDRigidParticles.h"
#include "Chaos/PBDConstraintRule.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Transform.h"
#include "Chaos/Framework/DebugSubstep.h"
#include "HAL/Event.h"
#include "Chaos/PBDRigidsSOAs.h"

namespace Chaos
{
template<class T, int d> class TPBDRigidsEvolutionGBF;
class FChaosArchive;

struct CHAOS_API FEvolutionStats
{
	int32 ActiveCollisionPoints;
	int32 ActiveShapes;
	int32 ShapesForAllConstraints;
	int32 CollisionPointsForAllConstraints;

	FEvolutionStats()
	{
		Reset();
	}

	void Reset()
	{
		ActiveCollisionPoints = 0;
		ActiveShapes = 0;
		ShapesForAllConstraints = 0;
		CollisionPointsForAllConstraints = 0;
	}

	FEvolutionStats& operator+=(const FEvolutionStats& Other)
	{
		ActiveCollisionPoints += Other.ActiveCollisionPoints;
		ActiveShapes += Other.ActiveShapes;
		ShapesForAllConstraints += Other.ShapesForAllConstraints;
		CollisionPointsForAllConstraints += Other.CollisionPointsForAllConstraints;
		return *this;
	}
};

template<class FPBDRigidsEvolution, class FPBDCollisionConstraint, class T, int d>
class TPBDRigidsEvolutionBase
{
  public:
	typedef TFunction<void(TTransientPBDRigidParticleHandle<T,d>& Particle, const T)> FForceRule;
	typedef TFunction<void(const TParticleView<TPBDRigidParticles<T, d>>&, const T)> FUpdateVelocityRule;
	typedef TFunction<void(const TParticleView<TPBDRigidParticles<T, d>>&, const T)> FUpdatePositionRule;
	typedef TFunction<void(TPBDRigidParticles<T, d>&, const T, const T, const int32)> FKinematicUpdateRule;

	CHAOS_API TPBDRigidsEvolutionBase(TPBDRigidsSOAs<T, d>& InParticles, int32 InNumIterations = 1);
	CHAOS_API virtual ~TPBDRigidsEvolutionBase()
	{
		Particles.GetParticleHandles().RemoveArray(&PhysicsMaterials);
		Particles.GetParticleHandles().RemoveArray(&PerParticlePhysicsMaterials);
		Particles.GetParticleHandles().RemoveArray(&ParticleDisableCount);
		Particles.GetParticleHandles().RemoveArray(&Collided);
		WaitOnAccelerationStructure();
	}

	CHAOS_API TArray<TGeometryParticleHandle<T, d>*> CreateStaticParticles(int32 NumParticles, const TGeometryParticleParameters<T, d>& Params = TGeometryParticleParameters<T, d>())
	{
		auto NewParticles = Particles.CreateStaticParticles(NumParticles, Params);
		for (auto& Particle : NewParticles)
		{
			DirtyParticle(*Particle);
		}
		return NewParticles;
	}

	CHAOS_API TArray<TKinematicGeometryParticleHandle<T, d>*> CreateKinematicParticles(int32 NumParticles, const TKinematicGeometryParticleParameters<T, d>& Params = TKinematicGeometryParticleParameters<T, d>())
	{
		auto NewParticles = Particles.CreateKinematicParticles(NumParticles, Params);
		for (auto& Particle : NewParticles)
		{
			DirtyParticle(*Particle);
		}
		return NewParticles;
	}

	CHAOS_API TArray<TPBDRigidParticleHandle<T, d>*> CreateDynamicParticles(int32 NumParticles, const TPBDRigidParticleParameters<T, d>& Params = TPBDRigidParticleParameters<T, d>())
	{
		auto NewParticles = Particles.CreateDynamicParticles(NumParticles, Params);
		for (auto& Particle : NewParticles)
		{
			DirtyParticle(*Particle);
		}
		return NewParticles;
	}

	CHAOS_API TArray<TPBDRigidClusteredParticleHandle<T, d>*> CreateClusteredParticles(int32 NumParticles, const TPBDRigidParticleParameters<T, d>& Params = TPBDRigidParticleParameters<T, d>())
	{
		auto NewParticles = Particles.CreateClusteredParticles(NumParticles, Params);
		for (auto& Particle : NewParticles)
		{
			DirtyParticle(*Particle);
		}
		return NewParticles;
	}

	CHAOS_API void AddForceFunction(FForceRule ForceFunction) { ForceRules.Add(ForceFunction); }
	CHAOS_API void SetParticleUpdateVelocityFunction(FUpdateVelocityRule ParticleUpdate) { ParticleUpdateVelocity = ParticleUpdate; }
	CHAOS_API void SetParticleUpdatePositionFunction(FUpdatePositionRule ParticleUpdate) { ParticleUpdatePosition = ParticleUpdate; }

	CHAOS_API TGeometryParticleHandles<T, d>& GetParticleHandles() { return Particles.GetParticleHandles(); }
	CHAOS_API const TGeometryParticleHandles<T, d>& GetParticleHandles() const { return Particles.GetParticleHandles(); }

	CHAOS_API TPBDRigidsSOAs<T,d>& GetParticles() { return Particles; }
	CHAOS_API const TPBDRigidsSOAs<T, d>& GetParticles() const { return Particles; }

	typedef TPBDConstraintGraph<T, d> FConstraintGraph;
	typedef TPBDConstraintGraphRule<T, d> FConstraintRule;

	CHAOS_API void AddConstraintRule(FConstraintRule* ConstraintRule)
	{
		uint32 ContainerId = (uint32)ConstraintRules.Num();
		ConstraintRules.Add(ConstraintRule);
		ConstraintRule->BindToGraph(ConstraintGraph, ContainerId);
	}

	CHAOS_API void SetNumIterations(int32 InNumIterations)
	{
		NumIterations = InNumIterations;
	}

	CHAOS_API void EnableParticle(TGeometryParticleHandle<T,d>* Particle, const TGeometryParticleHandle<T, d>* ParentParticle)
	{
		DirtyParticle(*Particle);
		Particles.EnableParticle(Particle);
		ConstraintGraph.EnableParticle(Particle, ParentParticle);
	}

	CHAOS_API void DisableParticle(TGeometryParticleHandle<T,d>* Particle)
	{
		RemoveParticleFromAccelerationStructure(*Particle);
		Particles.DisableParticle(Particle);
		ConstraintGraph.DisableParticle(Particle);

		RemoveConstraints(TSet<TGeometryParticleHandle<T,d>*>({ Particle }));
	}

	template <bool bPersistent>
	FORCEINLINE_DEBUGGABLE void DirtyParticle(TGeometryParticleHandleImp<T, d, bPersistent>& Particle)
	{
		FPendingSpatialData& SpatialData = InternalAccelerationQueue.FindOrAdd(Particle.Handle());
		SpatialData.AccelerationHandle = TAccelerationStructureHandle<T,d>(Particle);
		SpatialData.bUpdate = true;

		AsyncAccelerationQueue.FindOrAdd(Particle.Handle()) = SpatialData;
		ExternalAccelerationQueue.FindOrAdd(Particle.Handle()) = SpatialData;
	}

	void DestroyParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		RemoveParticleFromAccelerationStructure(*Particle);
		ConstraintGraph.RemoveParticle(Particle);
		RemoveConstraints(TSet<TGeometryParticleHandle<T, d>*>({ Particle }));
		Particles.DestroyParticle(Particle);
	}

	CHAOS_API void CreateParticle(TGeometryParticleHandle<T, d>* ParticleAdded)
	{
		ConstraintGraph.AddParticle(ParticleAdded);
		DirtyParticle(*ParticleAdded);
	}

	CHAOS_API void DisableParticles(const TSet<TGeometryParticleHandle<T,d>*>& InParticles)
	{
		for (TGeometryParticleHandle<T, d>* Particle : InParticles)
		{
			Particles.DisableParticle(Particle);
			RemoveParticleFromAccelerationStructure(*Particle);
		}

		ConstraintGraph.DisableParticles(InParticles);

		RemoveConstraints(InParticles);
	}

	CHAOS_API void WakeIsland(const int32 Island)
	{
		ConstraintGraph.WakeIsland(Island);
		//Update Particles SOAs
		/*for (auto Particle : ContactGraph.GetIslandParticles(Island))
		{
			ActiveIndices.Add(Particle);
		}*/
	}

	// @todo(ccaulfield): Remove the uint version
	CHAOS_API void RemoveConstraints(const TSet<TGeometryParticleHandle<T, d>*>& RemovedParticles)
	{
		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->RemoveConstraints(RemovedParticles);
		}
	}

	//TEMP: this is only needed while clustering continues to use indices directly
	const auto& GetActiveClusteredArray() const { return Particles.GetActiveClusteredArray(); }
	const auto& GetNonDisabledClusteredArray() const { return Particles.GetNonDisabledClusteredArray(); }

	CHAOS_API TSerializablePtr<TChaosPhysicsMaterial<T>> GetPhysicsMaterial(const TGeometryParticleHandle<T, d>* Particle) const { return Particle->AuxilaryValue(PhysicsMaterials); }
	CHAOS_API void SetPhysicsMaterial(TGeometryParticleHandle<T,d>* Particle, TSerializablePtr<TChaosPhysicsMaterial<T>> InMaterial)
	{
		check(!Particle->AuxilaryValue(PerParticlePhysicsMaterials)); //shouldn't be setting non unique material if a unique one already exists
		Particle->AuxilaryValue(PhysicsMaterials) = InMaterial;
	}

	CHAOS_API const TArray<TGeometryParticleHandle<T,d>*>& GetIslandParticles(const int32 Island) const { return ConstraintGraph.GetIslandParticles(Island); }
	CHAOS_API int32 NumIslands() const { return ConstraintGraph.NumIslands(); }

	void InitializeAccelerationStructures()
	{
		ConstraintGraph.InitializeGraph(Particles.GetNonDisabledView());

		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->AddToGraph();
		}

		ConstraintGraph.ResetIslands(Particles.GetNonDisabledDynamicView());

		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->InitializeAccelerationStructures();
		}
	}

	void UpdateAccelerationStructures(int32 Island)
	{
		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->UpdateAccelerationStructures(Island);
		}
	}

	void ApplyConstraints(const T Dt, int32 Island)
	{
		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->UpdateAccelerationStructures(Island);
		}

		for (int i = 0; i < NumIterations; ++i)
		{
			for (FConstraintRule* ConstraintRule : ConstraintRules)
			{
				ConstraintRule->ApplyConstraints(Dt, Island, i, NumIterations);
			}
		}
	}

	/** Make a copy of the acceleration structure to allow for external modification. This is needed for supporting sync operations on SQ structure from game thread */
	CHAOS_API void UpdateExternalAccelerationStructure(TUniquePtr<ISpatialAcceleration<TAccelerationStructureHandle<T, d>, T, d>>& ExternalStructure);
	ISpatialAcceleration<TAccelerationStructureHandle<T, d>, T, d>* GetSpatialAcceleration() { return InternalAcceleration.Get(); }

	const auto& GetRigidClustering() const { return Clustering; }
	auto& GetRigidClustering() { return Clustering; }

	void Serialize(FChaosArchive& Ar)
	{
		Particles.Serialize(Ar);

		Ar.UsingCustomVersion(FExternalPhysicsCustomObjectVersion::GUID);
		if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) >= FExternalPhysicsCustomObjectVersion::SerializeEvolutionBV)
		{
			Ar << InternalAcceleration;

			SerializePendingMap(Ar, InternalAccelerationQueue);
			SerializePendingMap(Ar, AsyncAccelerationQueue);
			SerializePendingMap(Ar, ExternalAccelerationQueue);

			ScratchExternalAcceleration = InternalAcceleration->Copy();
		}
		else if (Ar.IsLoading())
		{
			AccelerationStructureTaskComplete = nullptr;
			for (auto& Particle : Particles.GetNonDisabledView())
			{
				DirtyParticle(Particle);
			}

			//force build acceleration structure with latest data
			ComputeIntermediateSpatialAcceleration(true);
			ComputeIntermediateSpatialAcceleration(true);	//having to do it multiple times because of the various caching involved over multiple frames.
			ComputeIntermediateSpatialAcceleration(true);
		}
	}

protected:
	int32 NumConstraints() const
	{
		int32 NumConstraints = 0;
		for (const FConstraintRule* ConstraintRule : ConstraintRules)
		{
			NumConstraints += ConstraintRule->NumConstraints();
		}
		return NumConstraints;
	}

	template <bool bPersistent>
	FORCEINLINE_DEBUGGABLE void RemoveParticleFromAccelerationStructure(TGeometryParticleHandleImp<T, d, bPersistent>& ParticleHandle)
	{
		auto Particle = ParticleHandle.Handle();
		FPendingSpatialData& AsyncSpatialData = AsyncAccelerationQueue.FindOrAdd(Particle);
		AsyncSpatialData.AccelerationHandle = TAccelerationStructureHandle<T, d>(ParticleHandle);
		AsyncSpatialData.bUpdate = false;	//don't bother updating since deleting anyway
		AsyncSpatialData.bDelete = true;
		ExternalAccelerationQueue.FindOrAdd(Particle) = AsyncSpatialData;

		//remove particle immediately for intermediate structure
		InternalAccelerationQueue.Remove(Particle);
		InternalAcceleration->RemoveElement(AsyncSpatialData.AccelerationHandle);
	}

	void UpdateConstraintPositionBasedState(T Dt)
	{
		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->UpdatePositionBasedState(Dt);
		}
	}

	void CreateConstraintGraph()
	{
		ConstraintGraph.InitializeGraph(Particles.GetNonDisabledView());

		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->AddToGraph();
		}
	}

	void CreateIslands()
	{
		ConstraintGraph.UpdateIslands(Particles.GetNonDisabledDynamicView(), Particles);

		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->InitializeAccelerationStructures();
		}
	}
	
	void UpdateVelocities(const T Dt, int32 Island)
	{
		ParticleUpdateVelocity(Particles.GetActiveParticlesView(), Dt);
	}

	void ApplyPushOut(const T Dt, int32 Island)
	{
		for (FConstraintRule* ConstraintRule : ConstraintRules)
		{
			ConstraintRule->ApplyPushOut(Dt, Island);
		}
	}

	using FAccelerationStructure = typename FPBDCollisionConstraint::FAccelerationStructure;

	void ComputeIntermediateSpatialAcceleration(bool bBlock = false);
	void FlushInternalAccelerationQueue();
	void FlushAsyncAccelerationQueue();
	void FlushExternalAccelerationQueue(FAccelerationStructure& Acceleration);
	void WaitOnAccelerationStructure();

	TArray<FForceRule> ForceRules;
	FUpdateVelocityRule ParticleUpdateVelocity;
	FUpdatePositionRule ParticleUpdatePosition;
	FKinematicUpdateRule KinematicUpdate;
	TArray<FConstraintRule*> ConstraintRules;
	FConstraintGraph ConstraintGraph;
	TArrayCollectionArray<TSerializablePtr<TChaosPhysicsMaterial<T>>> PhysicsMaterials;
	TArrayCollectionArray<TUniquePtr<TChaosPhysicsMaterial<T>>> PerParticlePhysicsMaterials;
	TArrayCollectionArray<int32> ParticleDisableCount;
	TArrayCollectionArray<bool> Collided;

	TPBDRigidsSOAs<T, d>& Particles;
	TUniquePtr<FAccelerationStructure> InternalAcceleration;
	TUniquePtr<FAccelerationStructure> AsyncInternalAcceleration;
	TUniquePtr<FAccelerationStructure> AsyncExternalAcceleration;
	TUniquePtr<FAccelerationStructure> ScratchExternalAcceleration;
	bool bExternalReady;

	TPBDRigidClustering<FPBDRigidsEvolution, FPBDCollisionConstraint, T, d> Clustering;

	/** Used for updating intermediate spatial structures when they are finished */
	struct FPendingSpatialData
	{
		TAccelerationStructureHandle<T, d> AccelerationHandle;
		bool bUpdate;
		bool bDelete;

		FPendingSpatialData()
			: bUpdate(false)
			, bDelete(false)
		{}

		void Serialize(FChaosArchive& Ar)
		{
			Ar << AccelerationHandle;
			Ar << bUpdate;
			Ar << bDelete;
		}
	};

	/** Pending operations for the internal acceleration structure */
	TMap<TGeometryParticleHandle<T, d>*, FPendingSpatialData> InternalAccelerationQueue;

	/** Pending operations for the acceleration structures being rebuilt asynchronously */
	TMap<TGeometryParticleHandle<T, d>*, FPendingSpatialData> AsyncAccelerationQueue;

	/** Pending operations for the external acceleration structure*/
	TMap<TGeometryParticleHandle<T, d>*, FPendingSpatialData> ExternalAccelerationQueue;

	void SerializePendingMap(FChaosArchive& Ar, TMap<TGeometryParticleHandle<T, d>*, FPendingSpatialData>& Map)
	{
		TArray<TGeometryParticleHandle<T, d>*> Keys;
		if (!Ar.IsLoading())
		{
			Map.GenerateKeyArray(Keys);
		}
		Ar << AsAlwaysSerializableArray(Keys);
		for (auto Key : Keys)
		{
			FPendingSpatialData& PendingData = Map.FindOrAdd(Key);
			PendingData.Serialize(Ar);
		}
	}


	/** Used for building an acceleration structure out of cached bounds and payloads */
	struct FAccelerationStructureBuilder
	{
		//todo: should these be arrays instead? might make it easier in some cases
		bool bHasBoundingBox;
		TBox<T, d> CachedSpatialBounds;
		TAccelerationStructureHandle<T,d> CachedSpatialPayload;

		const TBox<T, d>& BoundingBox() const
		{
			return CachedSpatialBounds;
		}

		bool HasBoundingBox() const
		{
			return bHasBoundingBox;
		}

		template <typename TPayloadType>
		TAccelerationStructureHandle<T, d> GetPayload(int32 Idx) const
		{
			return CachedSpatialPayload;
		}
	};

	/** Used for async acceleration rebuild */
	TArray<FAccelerationStructureBuilder> CachedSpatialBuilderData;
	TMap<TGeometryParticleHandle<T, d>*, int32> ParticleToCacheIdx;

	FORCEINLINE_DEBUGGABLE void ApplyParticlePendingData(TGeometryParticleHandle<T, d>* Particle, const FPendingSpatialData& PendingData, FAccelerationStructure& SpatialAcceleration, bool bAsync);

	class FChaosAccelerationStructureTask
	{
	public:
		FChaosAccelerationStructureTask(const TArray<FAccelerationStructureBuilder>& InBounds, TUniquePtr<FAccelerationStructure>& InAccelerationStructure,
			TUniquePtr<FAccelerationStructure>& InAccelerationStructureCopy);
		static FORCEINLINE TStatId GetStatId();
		static FORCEINLINE ENamedThreads::Type GetDesiredThread();
		static FORCEINLINE ESubsequentsMode::Type GetSubsequentsMode();
		void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent);

		const TArray<FAccelerationStructureBuilder>& CachedSpatialBuilderData;
		TUniquePtr<FAccelerationStructure>& AccelerationStructure;
		TUniquePtr<FAccelerationStructure>& AccelerationStructureCopy;
	};
	FGraphEventRef AccelerationStructureTaskComplete;

	int32 NumIterations;

	static ISpatialAcceleration<TAccelerationStructureHandle<T, d>, T, d>* CreateNewSpatialStructure(const TArray<FAccelerationStructureBuilder>& Bounds);
};

}

// Only way to make this compile at the moment due to visibility attribute issues. TODO: Change this once a fix for this problem is applied.
#if PLATFORM_MAC || PLATFORM_LINUX
extern template class CHAOS_API Chaos::TPBDRigidsEvolutionBase<Chaos::TPBDRigidsEvolutionGBF<float, 3>, Chaos::TPBDCollisionConstraint<float,3>, float, 3>;
#else
extern template class Chaos::TPBDRigidsEvolutionBase<Chaos::TPBDRigidsEvolutionGBF<float, 3>, Chaos::TPBDCollisionConstraint<float,3>, float, 3>;
#endif
