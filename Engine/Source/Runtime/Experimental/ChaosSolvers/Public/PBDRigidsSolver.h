// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Defines.h"
#include "Chaos/Framework/MultiBufferResource.h"
#include "Chaos/Framework/PhysicsProxy.h"
#include "Chaos/Framework/PhysicsSolverBase.h"
#include "Chaos/PBDRigidParticles.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDRigidDynamicSpringConstraints.h"
#include "Chaos/PBDPositionConstraints.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/PBDConstraintRule.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Transform.h"
#include "Chaos/Framework/PhysicsProxy.h"
#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "EventManager.h"
#include "Field/FieldSystem.h"
#include "PBDRigidActiveParticlesBuffer.h"
#include "PhysicsProxy/SingleParticlePhysicsProxyFwd.h"
#include "PhysicsProxy/JointConstraintProxy.h"
#include "SolverEventFilters.h"
#include "Chaos/EvolutionTraits.h"
#include "Chaos/PBDRigidsEvolutionFwd.h"
#include "ChaosSolversModule.h"

class FPhysInterface_Chaos;

class FSkeletalMeshPhysicsProxy;
class FStaticMeshPhysicsProxy;
class FPerSolverFieldSystem;

#define PBDRIGID_PREALLOC_COUNT 1024
#define KINEMATIC_GEOM_PREALLOC_COUNT 100
#define GEOMETRY_PREALLOC_COUNT 100

extern int32 ChaosSolverParticlePoolNumFrameUntilShrink;

namespace ChaosTest
{
	template <typename TSolver>
	void AdvanceSolverNoPushHelper(TSolver* Solver,float Dt);
}

/**
*
*/
namespace Chaos
{
	class FPersistentPhysicsTask;
	class FChaosArchive;
	class FRewindData;

	template <typename T,typename R,int d>
	class ISpatialAccelerationCollection;

	template <typename T,int d>
	class TAccelerationStructureHandle;

	enum class ELockType : uint8
	{
		Read,
		Write
	};

	template<ELockType LockType>
	struct TSolverQueryMaterialScope
	{
		TSolverQueryMaterialScope() = delete;
	};

	/**
	*
	*/
	template <typename Traits>
	class TPBDRigidsSolver : public FPhysicsSolverBase
	{

		TPBDRigidsSolver(const EMultiBufferMode BufferingModeIn, UObject* InOwner);

	public:

		typedef FPhysicsSolverBase Super;

		friend class FPersistentPhysicsTask;
		friend class ::FChaosSolversModule;

		template<EThreadingMode Mode>
		friend class FDispatcher;
		
		template <typename Traits2>
		friend class TEventDefaults;

		friend class FPhysInterface_Chaos;
		friend class FPhysScene_ChaosInterface;
		friend class FPBDRigidDirtyParticlesBuffer;

		void* PhysSceneHack;	//This is a total hack for now to get at the owning scene

		typedef TPBDRigidsSOAs<float, 3> FParticlesType;
		typedef FPBDRigidDirtyParticlesBuffer FDirtyParticlesBuffer;

		typedef Chaos::TGeometryParticle<float, 3> FParticle;
		typedef Chaos::TGeometryParticleHandle<float, 3> FHandle;
		typedef Chaos::TPBDRigidsEvolutionGBF<Traits> FPBDRigidsEvolution;

		typedef TPBDRigidDynamicSpringConstraints<float, 3> FRigidDynamicSpringConstraints;
		typedef TPBDPositionConstraints<float, 3> FPositionConstraints;

		typedef TPBDConstraintIslandRule<FPBDJointConstraints> FJointConstraintsRule;
		typedef TPBDConstraintIslandRule<FRigidDynamicSpringConstraints> FRigidDynamicSpringConstraintsRule;
		typedef TPBDConstraintIslandRule<FPositionConstraints> FPositionConstraintsRule;

		using FJointConstraints = FPBDJointConstraints;
		using FJointConstraintRule = TPBDConstraintIslandRule<FJointConstraints>;
		//
		// Execution API
		//

		void ChangeBufferMode(Chaos::EMultiBufferMode InBufferMode);

		//
		//  Object API
		//

		void RegisterObject(Chaos::TGeometryParticle<float, 3>* GTParticle);
		void UnregisterObject(Chaos::TGeometryParticle<float, 3>* GTParticle);

		void RegisterObject(TGeometryCollectionPhysicsProxy<Traits>* InProxy);
		bool UnregisterObject(TGeometryCollectionPhysicsProxy<Traits>* InProxy);

		void RegisterObject(Chaos::FJointConstraint* GTConstraint);
		bool UnregisterObject(Chaos::FJointConstraint* GTConstraint);

		bool IsSimulating() const;

		void EnableRewindCapture(int32 NumFrames, bool InUseCollisionResimCache);
		FRewindData* GetRewindData()
		{
			if(Traits::IsRewindable())
			{
				return MRewindData.Get();
			}
			else
			{
				return nullptr;
			}
		}

		template<typename Lambda>
		void ForEachPhysicsProxy(Lambda InCallable)
		{
			for (FGeometryParticlePhysicsProxy* Obj : GeometryParticlePhysicsProxies)
			{
				InCallable(Obj);
			}
			for (FKinematicGeometryParticlePhysicsProxy* Obj : KinematicGeometryParticlePhysicsProxies)
			{
				InCallable(Obj);
			}
			for (FRigidParticlePhysicsProxy* Obj : RigidParticlePhysicsProxies)
			{
				InCallable(Obj);
			}
			for (FSkeletalMeshPhysicsProxy* Obj : SkeletalMeshPhysicsProxies)
			{
				InCallable(Obj);
			}
			for (FStaticMeshPhysicsProxy* Obj : StaticMeshPhysicsProxies)
			{
				InCallable(Obj);
			}
			for (TGeometryCollectionPhysicsProxy<Traits>* Obj : GeometryCollectionPhysicsProxies)
			{
				InCallable(Obj);
			}
			for (FJointConstraintPhysicsProxy* Obj : JointConstraintPhysicsProxies)
			{
				InCallable(Obj);
			}
		}

		template<typename Lambda>
		void ForEachPhysicsProxyParallel(Lambda InCallable)
		{
			Chaos::PhysicsParallelFor(GeometryParticlePhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FGeometryParticlePhysicsProxy* Obj = GeometryParticlePhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(KinematicGeometryParticlePhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FKinematicGeometryParticlePhysicsProxy* Obj = KinematicGeometryParticlePhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(RigidParticlePhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FRigidParticlePhysicsProxy* Obj = RigidParticlePhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(SkeletalMeshPhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FSkeletalMeshPhysicsProxy* Obj = SkeletalMeshPhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(StaticMeshPhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FStaticMeshPhysicsProxy* Obj = StaticMeshPhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(GeometryCollectionPhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				TGeometryCollectionPhysicsProxy<Traits>* Obj = GeometryCollectionPhysicsProxies[Index];
				InCallable(Obj);
			});
			Chaos::PhysicsParallelFor(JointConstraintPhysicsProxies.Num(), [this, &InCallable](const int32 Index)
			{
				FJointConstraintPhysicsProxy* Obj = JointConstraintPhysicsProxies[Index];
				InCallable(Obj);
			});
		}

		int32 GetNumPhysicsProxies() const {
			return GeometryParticlePhysicsProxies.Num() + KinematicGeometryParticlePhysicsProxies.Num() + RigidParticlePhysicsProxies.Num()
				+ SkeletalMeshPhysicsProxies.Num() + StaticMeshPhysicsProxies.Num()
				+ GeometryCollectionPhysicsProxies.Num()
				+ JointConstraintPhysicsProxies.Num();
		}

		//
		//  Simulation API
		//

		/**/
		bool Enabled() const { if (bEnabled) return this->IsSimulating(); return false; }
		void SetEnabled(bool bEnabledIn) { bEnabled = bEnabledIn; }
		bool HasActiveParticles() const { return !!GetNumPhysicsProxies(); }
		FDirtyParticlesBuffer* GetDirtyParticlesBuffer() const { return MDirtyParticlesBuffer.Get(); }


		//Make friend with unit test code so we can verify some behavior
		template <typename TSolver>
		friend void ChaosTest::AdvanceSolverNoPushHelper(TSolver* Solver,float Dt);

		/**/
		void Reset();

		/**/
		void BufferPhysicsResults();

		/**/
		void FlipBuffers();

		/**/
		void UpdateGameThreadStructures();


		/**/
		void SetCurrentFrame(const int32 CurrentFrameIn) { CurrentFrame = CurrentFrameIn; }
		int32& GetCurrentFrame() { return CurrentFrame; }

		/**/
		float& GetSolverTime() { return MTime; }
		const float GetSolverTime() const { return MTime; }

		/**/
		void SetMaxDeltaTime(const float InMaxDeltaTime) { MMaxDeltaTime = InMaxDeltaTime; }
		float GetLastDt() const { return MLastDt; }
		float GetMaxDeltaTime() const { return MMaxDeltaTime; }
		float GetMinDeltaTime() const { return MMinDeltaTime; }
		void SetMaxSubSteps(const int32 InMaxSubSteps) { MMaxSubSteps = InMaxSubSteps; }
		int32 GetMaxSubSteps() const { return MMaxSubSteps; }

		/**/
		void SetIterations(const int32 InNumIterations) { GetEvolution()->SetNumIterations(InNumIterations); }
		void SetPushOutIterations(const int32 InNumIterations) {  GetEvolution()->SetNumPushOutIterations(InNumIterations); }
		void SetPushOutPairIterations(const int32 InNumIterations) {  GetEvolution()->GetCollisionConstraints().SetPushOutPairIterations(InNumIterations); }
		void SetUseContactGraph(const bool bInUseContactGraph) { GetEvolution()->GetCollisionConstraintsRule().SetUseContactGraph(bInUseContactGraph); }

		/**/
		void SetGenerateCollisionData(bool bDoGenerate) { GetEventFilters()->SetGenerateCollisionEvents(bDoGenerate); }
		void SetGenerateBreakingData(bool bDoGenerate)
		{
			GetEventFilters()->SetGenerateBreakingEvents(bDoGenerate);
			GetEvolution()->GetRigidClustering().SetGenerateClusterBreaking(bDoGenerate);
		}
		void SetGenerateTrailingData(bool bDoGenerate) { GetEventFilters()->SetGenerateTrailingEvents(bDoGenerate); }
		void SetCollisionFilterSettings(const FSolverCollisionFilterSettings& InCollisionFilterSettings) { GetEventFilters()->GetCollisionFilter()->UpdateFilterSettings(InCollisionFilterSettings); }
		void SetBreakingFilterSettings(const FSolverBreakingFilterSettings& InBreakingFilterSettings) { GetEventFilters()->GetBreakingFilter()->UpdateFilterSettings(InBreakingFilterSettings); }
		void SetTrailingFilterSettings(const FSolverTrailingFilterSettings& InTrailingFilterSettings) { GetEventFilters()->GetTrailingFilter()->UpdateFilterSettings(InTrailingFilterSettings); }

		/**/
		FJointConstraints& GetJointConstraints() { return JointConstraints; }
		const FJointConstraints& GetJointConstraints() const { return JointConstraints; }

		FJointConstraintRule& GetJointConstraintsRule() { return JointConstraintRule; }
		const FJointConstraintRule& GetJointConstraintsRule() const { return JointConstraintRule; }

		/**/
		FPBDRigidsEvolution* GetEvolution() { return MEvolution.Get(); }
		FPBDRigidsEvolution* GetEvolution() const { return MEvolution.Get(); }

		FParticlesType& GetParticles() { return Particles; }
		const FParticlesType& GetParticles() const { return Particles; }

		void AddParticleToProxy(const Chaos::TGeometryParticleHandle<float, 3>* Particle, IPhysicsProxyBase* Proxy)
		{
			if (!MParticleToProxy.Find(Particle))
				MParticleToProxy.Add(Particle, TSet<IPhysicsProxyBase*>());
			MParticleToProxy[Particle].Add(Proxy); 
		}
		
		void RemoveParticleToProxy(const Chaos::TGeometryParticleHandle<float, 3>* Particle)
		{
			MParticleToProxy.Remove(Particle);
		}
		
		const TSet<IPhysicsProxyBase*> * GetProxies(const Chaos::TGeometryParticleHandle<float, 3>* Handle) const
		{
			const TSet<IPhysicsProxyBase*>* PhysicsProxyPtr = MParticleToProxy.Find(Handle);
			return PhysicsProxyPtr ? PhysicsProxyPtr : nullptr;
		}

		/**/
		TEventManager<Traits>* GetEventManager() { return MEventManager.Get(); }

		/**/
		FSolverEventFilters* GetEventFilters() { return MSolverEventFilters.Get(); }
		FSolverEventFilters* GetEventFilters() const { return MSolverEventFilters.Get(); }

		/**/
		void SyncEvents_GameThread();

		/**/
		void PostTickDebugDraw() const;

		TArray<TGeometryCollectionPhysicsProxy<Traits>*>& GetGeometryCollectionPhysicsProxies()
		{
			return GeometryCollectionPhysicsProxies;
		}

		TArray<FJointConstraintPhysicsProxy*>& GetJointConstraintPhysicsProxy()
		{
			return JointConstraintPhysicsProxies;
		}

		/** Events hooked up to the Chaos material manager */
		void UpdateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData);
		void CreateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData);
		void DestroyMaterial(Chaos::FMaterialHandle InHandle);
		void UpdateMaterialMask(Chaos::FMaterialMaskHandle InHandle, const Chaos::FChaosPhysicsMaterialMask& InNewData);
		void CreateMaterialMask(Chaos::FMaterialMaskHandle InHandle, const Chaos::FChaosPhysicsMaterialMask& InNewData);
		void DestroyMaterialMask(Chaos::FMaterialMaskHandle InHandle);

		/** Access to the internal material mirrors */
		const THandleArray<FChaosPhysicsMaterial>& GetQueryMaterials() const { return QueryMaterials; }
		const THandleArray<FChaosPhysicsMaterialMask>& GetQueryMaterialMasks() const { return QueryMaterialMasks; }
		const THandleArray<FChaosPhysicsMaterial>& GetSimMaterials() const { return SimMaterials; }
		const THandleArray<FChaosPhysicsMaterialMask>& GetSimMaterialMasks() const { return SimMaterialMasks; }

		/** Copy the simulation material list to the query material list, to be done when the SQ commits an update */
		void SyncQueryMaterials();

		void FinalizeRewindData(const TParticleView<TPBDRigidParticles<FReal,3>>& DirtyParticles);
		bool RewindUsesCollisionResimCache() const { return bUseCollisionResimCache; }

		FPerSolverFieldSystem& GetPerSolverField() { return *PerSolverField; }
		const FPerSolverFieldSystem& GetPerSolverField() const { return *PerSolverField; }

		void UpdateExternalAccelerationStructure_External(TUniquePtr<ISpatialAccelerationCollection<TAccelerationStructureHandle<FReal,3>,FReal,3>>& ExternalStructure);

	private:

		template<typename ParticleType>
		void FlipBuffer(Chaos::TGeometryParticleHandle<float, 3>* Handle)
		{
			if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(Handle))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					((ParticleType*)(Proxy))->FlipBuffer();
				}
			}
		}

		template<typename ParticleType>
		void PullFromPhysicsState(Chaos::TGeometryParticleHandle<float, 3>* Handle)
		{
			if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(Handle))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					((ParticleType*)(Proxy))->PullFromPhysicsState();
				}
			}
		}

		template<typename ParticleType>
		void BufferPhysicsResults(Chaos::TGeometryParticleHandle<float, 3>* Handle)
		{
			if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(Handle))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					((ParticleType*)(Proxy))->BufferPhysicsResults();
				}
			}
		}

		/**/
		virtual void AdvanceSolverBy(const FReal DeltaTime) override;
		virtual void PushPhysicsState(const FReal DeltaTime) override;
		virtual void SetExternalTimeConsumed_External(const FReal Time) override;

		//
		// Solver Data
		//
		int32 CurrentFrame;
		float MTime;
		float MLastDt;
		float MMaxDeltaTime;
		float MMinDeltaTime;
		int32 MMaxSubSteps;
		bool bEnabled;
		bool bHasFloor;
		bool bIsFloorAnalytic;
		float FloorHeight;

		FParticlesType Particles;
		TUniquePtr<FPBDRigidsEvolution> MEvolution;
		TUniquePtr<TEventManager<Traits>> MEventManager;
		TUniquePtr<FSolverEventFilters> MSolverEventFilters;
		TUniquePtr<FDirtyParticlesBuffer> MDirtyParticlesBuffer;
		TMap<const Chaos::TGeometryParticleHandle<float, 3>*, TSet<IPhysicsProxyBase*> > MParticleToProxy;
		TUniquePtr<FRewindData> MRewindData;

		//
		// Proxies
		//
		TSharedPtr<FCriticalSection> MCurrentLock;
		TArray< FGeometryParticlePhysicsProxy* > GeometryParticlePhysicsProxies;
		TArray< FKinematicGeometryParticlePhysicsProxy* > KinematicGeometryParticlePhysicsProxies;
		TArray< FRigidParticlePhysicsProxy* > RigidParticlePhysicsProxies;
		TArray< FSkeletalMeshPhysicsProxy* > SkeletalMeshPhysicsProxies; // dep
		TArray< FStaticMeshPhysicsProxy* > StaticMeshPhysicsProxies; // dep
		TArray< TGeometryCollectionPhysicsProxy<Traits>* > GeometryCollectionPhysicsProxies;
		TArray< FJointConstraintPhysicsProxy*> JointConstraintPhysicsProxies;
		bool bUseCollisionResimCache;

		//
		//  Constraints
		//
		FPBDJointConstraints JointConstraints;
		TPBDConstraintIslandRule<FPBDJointConstraints> JointConstraintRule;

		TUniquePtr<FPerSolverFieldSystem> PerSolverField;


		// Physics material mirrors for the solver. These should generally stay in sync with the global material list from
		// the game thread. This data is read only in the solver as we should never need to update it here. External threads can
		// Enqueue commands to change parameters.
		//
		// There are two copies here to enable SQ to lock only the solvers that it needs to handle the material access during a query
		// instead of having to lock the entire physics state of the runtime.
		
		THandleArray<FChaosPhysicsMaterial> QueryMaterials;
		THandleArray<FChaosPhysicsMaterialMask> QueryMaterialMasks;
		THandleArray<FChaosPhysicsMaterial> SimMaterials;
		THandleArray<FChaosPhysicsMaterialMask> SimMaterialMasks;

		void ProcessSinglePushedData_Internal(FPushPhysicsData& PushData);
		virtual void ProcessPushedData_Internal(const TArray<FPushPhysicsData*>& PushDataArray) override;

	public:

		template<typename ParticleEntry, typename ProxyEntry, SIZE_T PreAllocCount>
		class TFramePool
		{
		public:

			struct TPoolEntry
			{
				TPoolEntry()
				{
					Proxy = nullptr;
				}

				ParticleEntry Particle;
				ProxyEntry    Proxy;
			};

			TFramePool()
			{
				EntryCount = 0;
				MaxEntryCount = 0;
				FrameCount = 0;
				// Prealloc default object in the pool. 
				// Must call DirtyParticleData.Init + DirtyParticleData.Reset each 
				// time you use an entry in the pool.
				Pool.AddDefaulted(PreAllocCount);
			}

			void ResetPool() 
			{ 
				// try to shrink each (n) frames
				MaxEntryCount = (EntryCount >= MaxEntryCount) ? EntryCount : MaxEntryCount;
				
				if (FrameCount % ChaosSolverParticlePoolNumFrameUntilShrink == 0)
				{
					int32 NextLowerBound = (Pool.Num() / PreAllocCount) - 1;
					NextLowerBound = (NextLowerBound >= 1) ? NextLowerBound : 1;
					NextLowerBound *= PreAllocCount;
					if (MaxEntryCount < NextLowerBound)
					{
						Pool.SetNum(NextLowerBound);
					}
					MaxEntryCount = 0;
				}

				FrameCount++;
				EntryCount = 0;
			}

			int32 GetEntryCount()
			{
				return EntryCount;
			}

			TPoolEntry& GetEntry(int32 Index)
			{
				ensure(Index < Pool.Num());
				return Pool[Index];
			}

			TPoolEntry& GetNewEntry()
			{
				if (EntryCount >= Pool.Num())
				{
					Pool.Add(TPoolEntry());
				}
				return Pool[EntryCount++];
			}

		private:
			TArray<TPoolEntry> Pool;
			int32 EntryCount;
			int32 MaxEntryCount;
			int32 FrameCount;
		};

		TFramePool<Chaos::TPBDRigidParticleData<float, 3>, FSingleParticlePhysicsProxy< Chaos::TPBDRigidParticle<float, 3> >*, PBDRIGID_PREALLOC_COUNT> RigidParticlePool;
		TFramePool<Chaos::TKinematicGeometryParticleData<float, 3>, FSingleParticlePhysicsProxy< Chaos::TKinematicGeometryParticle<float, 3> >*, KINEMATIC_GEOM_PREALLOC_COUNT> KinematicGeometryParticlePool;
		TFramePool<Chaos::TGeometryParticleData<float, 3>, FSingleParticlePhysicsProxy< Chaos::TGeometryParticle<float, 3> >*, GEOMETRY_PREALLOC_COUNT> GeometryParticlePool;
	};

	template<>
	struct TSolverQueryMaterialScope<ELockType::Read>
	{
		TSolverQueryMaterialScope() = delete;


		explicit TSolverQueryMaterialScope(FPhysicsSolverBase* InSolver)
			: Solver(InSolver)
		{
			check(Solver);
			Solver->QueryMaterialLock.ReadLock();
		}

		~TSolverQueryMaterialScope()
		{
			Solver->QueryMaterialLock.ReadUnlock();
		}

	private:
		FPhysicsSolverBase* Solver;
	};

	template<>
	struct TSolverQueryMaterialScope<ELockType::Write>
	{
		TSolverQueryMaterialScope() = delete;

		explicit TSolverQueryMaterialScope(FPhysicsSolverBase* InSolver)
			: Solver(InSolver)
		{
			check(Solver);
			Solver->QueryMaterialLock.WriteLock();
		}

		~TSolverQueryMaterialScope()
		{
			Solver->QueryMaterialLock.WriteUnlock();
		}

	private:
		FPhysicsSolverBase* Solver;
	};

#define EVOLUTION_TRAIT(Trait) extern template class CHAOSSOLVERS_TEMPLATE_API TPBDRigidsSolver<Trait>;
#include "Chaos/EvolutionTraits.inl"
#undef EVOLUTION_TRAIT

}; // namespace Chaos
