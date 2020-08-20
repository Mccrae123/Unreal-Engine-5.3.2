// Copyright Epic Games, Inc. All Rights Reserved.

#include "PBDRigidsSolver.h"

#include "Async/AsyncWork.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/PBDCollisionConstraintsUtil.h"
#include "Chaos/Utilities.h"
#include "Chaos/ChaosDebugDraw.h"
#include "ChaosStats.h"
#include "ChaosSolversModule.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsProxy/SkeletalMeshPhysicsProxy.h"
#include "PhysicsProxy/StaticMeshPhysicsProxy.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsProxy/PerSolverFieldSystem.h"
#include "EventDefaults.h"
#include "EventsData.h"
#include "RewindData.h"


DEFINE_LOG_CATEGORY_STATIC(LogPBDRigidsSolver, Log, All);

// DebugDraw CVars
#if CHAOS_DEBUG_DRAW
int32 ChaosSolverDrawCollisions = 0;
int32 ChaosSolverDrawBPBounds = 0;
FAutoConsoleVariableRef CVarChaosSolverDrawCollisions(TEXT("p.Chaos.Solver.DebugDrawCollisions"), ChaosSolverDrawCollisions, TEXT("Draw Collisions (0 = never; 1 = end of frame)."));
FAutoConsoleVariableRef CVarChaosSolverDrawBPBounds(TEXT("p.Chaos.Solver.DrawBPBounds"), ChaosSolverDrawBPBounds, TEXT("Draw bounding volumes inside the broadphase (0 = never; 1 = end of frame)."));
#endif

bool ChaosSolverUseParticlePool = true;
FAutoConsoleVariableRef CVarChaosSolverUseParticlePool(TEXT("p.Chaos.Solver.UseParticlePool"), ChaosSolverUseParticlePool, TEXT("Whether or not to use dirty particle pool (Optim)"));

int32 ChaosSolverParticlePoolNumFrameUntilShrink = 30;
FAutoConsoleVariableRef CVarChaosSolverParticlePoolNumFrameUntilShrink(TEXT("p.Chaos.Solver.ParticlePoolNumFrameUntilShrink"), ChaosSolverParticlePoolNumFrameUntilShrink, TEXT("Num Frame until we can potentially shrink the pool"));

int32 ChaosSolverCollisionDefaultIterationsCVar = 4;
FAutoConsoleVariableRef CVarChaosSolverCollisionDefaultIterations(TEXT("p.ChaosSolverCollisionDefaultIterations"), ChaosSolverCollisionDefaultIterationsCVar, TEXT("Default collision iterations for the solver.[def:1]"));

int32 ChaosSolverCollisionDefaultPushoutIterationsCVar = 3;
FAutoConsoleVariableRef CVarChaosSolverCollisionDefaultPushoutIterations(TEXT("p.ChaosSolverCollisionDefaultPushoutIterations"), ChaosSolverCollisionDefaultPushoutIterationsCVar, TEXT("Default collision pushout iterations for the solver.[def:1]"));

int32 ChaosSolverCleanupCommandsOnDestruction = 1;
FAutoConsoleVariableRef CVarChaosSolverCleanupCommandsOnDestruction(TEXT("p.Chaos.Solver.CleanupCommandsOnDestruction"), ChaosSolverCleanupCommandsOnDestruction, TEXT("Whether or not to run internal command queue cleanup on solver destruction (0 = no cleanup, >0 = cleanup all commands)"));

int32 ChaosSolverCollisionDeferNarrowPhase = 0;
FAutoConsoleVariableRef CVarChaosSolverCollisionDeferNarrowPhase(TEXT("p.Chaos.Solver.Collision.DeferNarrowPhase"), ChaosSolverCollisionDeferNarrowPhase, TEXT("Create contacts for all broadphase pairs, perform NarrowPhase later."));

int32 ChaosSolverCollisionUseManifolds = 0;
FAutoConsoleVariableRef CVarChaosSolverCollisionUseManifolds(TEXT("p.Chaos.Solver.Collision.UseManifolds"), ChaosSolverCollisionUseManifolds, TEXT("Enable/Disable use of manifoldes in collision."));



namespace Chaos
{

	template <typename Traits>
	class AdvanceOneTimeStepTask : public FNonAbandonableTask
	{
		friend class FAutoDeleteAsyncTask<AdvanceOneTimeStepTask>;
	public:
		AdvanceOneTimeStepTask(
			TPBDRigidsSolver<Traits>* Scene,
			const float DeltaTime)
			: MSolver(Scene)
			, MDeltaTime(DeltaTime)
		{
			UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("AdvanceOneTimeStepTask::AdvanceOneTimeStepTask()"));
		}

		void DoWork()
		{
			LLM_SCOPE(ELLMTag::Chaos);
			UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("AdvanceOneTimeStepTask::DoWork()"));

			MSolver->ApplyCallbacks_Internal();
			MSolver->GetEvolution()->GetRigidClustering().ResetAllClusterBreakings();

			{
				SCOPE_CYCLE_COUNTER(STAT_UpdateParams);
				Chaos::TPBDPositionConstraints<float, 3> PositionTarget; // Dummy for now
				TMap<int32, int32> PositionTargetedParticles;
				//TArray<FKinematicProxy> AnimatedPositions;
				Chaos::TArrayCollectionArray<float> Strains;
				{
					FPerSolverFieldSystem& FieldObj = MSolver->GetPerSolverField();
					auto& GeomCollectionParticles = MSolver->GetEvolution()->GetParticles().GetGeometryCollectionParticles();
					FieldObj.FieldParameterUpdateCallback(MSolver, GeomCollectionParticles, Strains,
						PositionTarget, PositionTargetedParticles, /*AnimatedPositions,*/ MSolver->GetSolverTime());
					auto& ClusteredParticles = MSolver->GetEvolution()->GetParticles().GetClusteredParticles();
					FieldObj.FieldParameterUpdateCallback(MSolver, ClusteredParticles, Strains,
						PositionTarget, PositionTargetedParticles, /*AnimatedPositions,*/ MSolver->GetSolverTime());
				}

				for (TGeometryCollectionPhysicsProxy<Traits>* Obj : MSolver->GetGeometryCollectionPhysicsProxies())
				{
					Obj->ParameterUpdateCallback(MSolver->GetEvolution()->GetParticles().GetGeometryCollectionParticles(), MSolver->GetSolverTime());
				}

			}

			{
				//SCOPE_CYCLE_COUNTER(STAT_BeginFrame);
				//MSolver->StartFrameCallback(MDeltaTime, MSolver->GetSolverTime());
			}


			if(FRewindData* RewindData = MSolver->GetRewindData())
			{
				RewindData->AdvanceFrame(MDeltaTime,[Evolution = MSolver->GetEvolution()]()
				{
					return Evolution->CreateExternalResimCache();
				});
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EvolutionAndKinematicUpdate);

				// This outer loop can potentially cause the system to lose energy over integration
				// in a couple of different cases.
				//
				// * If we have a timestep that's smaller than MinDeltaTime, then we just won't step.
				//   Yes, we'll lose some teeny amount of energy, but we'll avoid 1/dt issues.
				//
				// * If we have used all of our substeps but still have time remaining, then some
				//   energy will be lost.
				const float MinDeltaTime = MSolver->GetMinDeltaTime();
				const float MaxDeltaTime = MSolver->GetMaxDeltaTime();
				int32 StepsRemaining = MSolver->GetMaxSubSteps();
				float TimeRemaining = MDeltaTime;
				bool bFirstStep = true;
				while (StepsRemaining > 0 && TimeRemaining > MinDeltaTime)
				{
					--StepsRemaining;
					const float DeltaTime = MaxDeltaTime > 0.f ? FMath::Min(TimeRemaining, MaxDeltaTime) : TimeRemaining;
					TimeRemaining -= DeltaTime;

					Chaos::TArrayCollectionArray<FVector> Forces, Torques;
					{
						FPerSolverFieldSystem& FieldObj = MSolver->GetPerSolverField();
						auto& GeomCollectionParticles = MSolver->GetEvolution()->GetParticles().GetGeometryCollectionParticles();
						FieldObj.FieldForcesUpdateCallback(MSolver, GeomCollectionParticles, Forces, Torques, MSolver->GetSolverTime());
						auto& ClusteredParticles = MSolver->GetEvolution()->GetParticles().GetClusteredParticles();
						FieldObj.FieldForcesUpdateCallback(MSolver, ClusteredParticles, Forces, Torques, MSolver->GetSolverTime());
					}

					for (TGeometryCollectionPhysicsProxy<Traits>* Obj : MSolver->GetGeometryCollectionPhysicsProxies())
					{
						Obj->ParameterUpdateCallback(MSolver->GetEvolution()->GetParticles().GetGeometryCollectionParticles(), MSolver->GetSolverTime());
					}

					if(FRewindData* RewindData = MSolver->GetRewindData())
					{
						//todo: make this work with sub-stepping
						MSolver->GetEvolution()->SetCurrentStepResimCache(bFirstStep ? RewindData->GetCurrentStepResimCache() : nullptr);
					}

					MSolver->GetEvolution()->AdvanceOneTimeStep(DeltaTime);
					bFirstStep = false;
				}

#if CHAOS_CHECKED
				// If time remains, then log why we have lost energy over the timestep.
				if (TimeRemaining > 0.f)
				{
					if (StepsRemaining == 0)
					{
						UE_LOG(LogPBDRigidsSolver, Warning, TEXT("AdvanceOneTimeStepTask::DoWork() - Energy lost over %fs due to too many substeps over large timestep"), TimeRemaining);
					}
					else
					{
						UE_LOG(LogPBDRigidsSolver, Warning, TEXT("AdvanceOneTimeStepTask::DoWork() - Energy lost over %fs due to small timestep remainder"), TimeRemaining);
					}
				}
#endif
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EventDataGathering);
				{
					SCOPE_CYCLE_COUNTER(STAT_FillProducerData);
					MSolver->GetEventManager()->FillProducerData(MSolver);
				}
				{
					SCOPE_CYCLE_COUNTER(STAT_FlipBuffersIfRequired);
					MSolver->GetEventManager()->FlipBuffersIfRequired();
				}
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EndFrame);
				MSolver->GetEvolution()->EndFrame(MDeltaTime);
			}

			if(FRewindData* RewindData = MSolver->GetRewindData())
			{
				RewindData->FinishFrame();
			}

			MSolver->GetSolverTime() += MDeltaTime;
			MSolver->GetCurrentFrame()++;
			MSolver->PostTickDebugDraw();
		}

	protected:

		TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(AdvanceOneTimeStepTask, STATGROUP_ThreadPoolAsyncTasks);
		}

		TPBDRigidsSolver<Traits>* MSolver;
		float MDeltaTime;
		TSharedPtr<FCriticalSection> PrevLock, CurrentLock;
		TSharedPtr<FEvent> PrevEvent, CurrentEvent;
	};

	template <typename Traits>
	TPBDRigidsSolver<Traits>::TPBDRigidsSolver(const EMultiBufferMode BufferingModeIn, UObject* InOwner)
		: Super(BufferingModeIn, BufferingModeIn == EMultiBufferMode::Single ? EThreadingModeTemp::SingleThread : EThreadingModeTemp::TaskGraph, InOwner, TraitToIdx<Traits>())
		, CurrentFrame(0)
		, MTime(0.0)
		, MLastDt(0.0)
		, MMaxDeltaTime(0.0)
		, MMinDeltaTime(SMALL_NUMBER)
		, MMaxSubSteps(1)
		, bEnabled(false)
		, bHasFloor(true)
		, bIsFloorAnalytic(false)
		, FloorHeight(0.f)
		, MEvolution(new FPBDRigidsEvolution(Particles, SimMaterials, ChaosSolverCollisionDefaultIterationsCVar, ChaosSolverCollisionDefaultPushoutIterationsCVar, BufferingModeIn == Chaos::EMultiBufferMode::Single))
		, MEventManager(new TEventManager<Traits>(BufferingModeIn))
		, MSolverEventFilters(new FSolverEventFilters())
		, MDirtyParticlesBuffer(new FDirtyParticlesBuffer(BufferingModeIn, BufferingModeIn == Chaos::EMultiBufferMode::Single))
		, MCurrentLock(new FCriticalSection())
		, bUseCollisionResimCache(false)
		, JointConstraintRule(JointConstraints)
		, PerSolverField(nullptr)
	{
		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("PBDRigidsSolver::PBDRigidsSolver()"));
		Reset();
		MEvolution->AddConstraintRule(&JointConstraintRule);

		MEvolution->SetInternalParticleInitilizationFunction(
			[this](const Chaos::TGeometryParticleHandle<float, 3>* OldParticle, const Chaos::TGeometryParticleHandle<float, 3>* NewParticle) {
				if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(OldParticle))
				{
					for (IPhysicsProxyBase* Proxy : *Proxies)
					{
						this->AddParticleToProxy(NewParticle, Proxy);
					}
				}
			});
	}

	float MaxBoundsForTree = 10000;
	FAutoConsoleVariableRef CVarMaxBoundsForTree(
		TEXT("p.MaxBoundsForTree"),
		MaxBoundsForTree,
		TEXT("The max bounds before moving object into a large objects structure. Only applies on object registration")
		TEXT(""),
		ECVF_Default);

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::RegisterObject(TGeometryParticle<float, 3>* GTParticle)
	{
		LLM_SCOPE(ELLMTag::Chaos);

		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("TPBDRigidsSolver::RegisterObject()"));

		// Make sure this particle doesn't already have a proxy
		checkSlow(GTParticle->GetProxy() == nullptr);

		if (GTParticle->Geometry() && GTParticle->Geometry()->HasBoundingBox() && GTParticle->Geometry()->BoundingBox().Extents().Max() >= MaxBoundsForTree)
		{
			GTParticle->SetSpatialIdx(FSpatialAccelerationIdx{ 1,0 });
		}
		if (!ensure(GTParticle->IsParticleValid()))
		{
			return;
		}

		// NOTE: Do we really need these lists of proxies if we can just
		// access them through the GTParticles list?
		
		IPhysicsProxyBase* ProxyBase;

		GTParticle->SetUniqueIdx(GetEvolution()->GenerateUniqueIdx());
		//Chaos::FParticlePropertiesData& RemoteParticleData = *DirtyPropertiesManager->AccessProducerBuffer()->NewRemoteParticleProperties();
		//Chaos::FShapeRemoteDataContainer& RemoteShapeContainer = *DirtyPropertiesManager->AccessProducerBuffer()->NewRemoteShapeContainer();

		// Make a physics proxy, giving it our particle and particle handle
		const EParticleType InParticleType = GTParticle->ObjectType();
		if (InParticleType == EParticleType::Rigid)
		{
			auto Proxy = new FRigidParticlePhysicsProxy(GTParticle->CastToRigidParticle(), nullptr);
			RigidParticlePhysicsProxies.Add((FRigidParticlePhysicsProxy*)Proxy);
			ProxyBase = Proxy;
		}
		else if (InParticleType == EParticleType::Kinematic)
		{
			auto Proxy = new FKinematicGeometryParticlePhysicsProxy(GTParticle->CastToKinematicParticle(), nullptr);
			KinematicGeometryParticlePhysicsProxies.Add((FKinematicGeometryParticlePhysicsProxy*)Proxy);
			ProxyBase = Proxy;
		}
		else // Assume it's a static (geometry) if it's not dynamic or kinematic
		{
			auto Proxy = new FGeometryParticlePhysicsProxy(GTParticle, nullptr);
			GeometryParticlePhysicsProxies.Add((FGeometryParticlePhysicsProxy*)Proxy);
			ProxyBase = Proxy;
		}

		ProxyBase->SetSolver(this);

		// Associate the proxy with the particle
		GTParticle->SetProxy(ProxyBase);

		AddDirtyProxy(ProxyBase);

		UpdateParticleInAccelerationStructure_External(GTParticle, /*bDelete=*/false);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::UnregisterObject(TGeometryParticle<float, 3>* GTParticle)
	{
		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("TPBDRigidsSolver::UnregisterObject()"));

		// Get the proxy associated with this particle
		IPhysicsProxyBase* InProxy = GTParticle->GetProxy();
		check(InProxy);

		// Grab the particle's type
		const EParticleType InParticleType = GTParticle->ObjectType();

		UpdateParticleInAccelerationStructure_External(GTParticle, /*bDelete=*/true);

		// remove the proxy from the invalidation list
		RemoveDirtyProxy(GTParticle->GetProxy());

		// Null out the particle's proxy pointer
		GTParticle->SetProxy(nullptr);

		// Remove the proxy from the GT proxy map
		if (InParticleType == EParticleType::Rigid)
		{
			RigidParticlePhysicsProxies.RemoveSingleSwap((FRigidParticlePhysicsProxy*)InProxy);
		}
		else if (InParticleType == EParticleType::Kinematic)
		{
			KinematicGeometryParticlePhysicsProxies.RemoveSingleSwap((FKinematicGeometryParticlePhysicsProxy*)InProxy);
		}
		else if (InParticleType == EParticleType::GeometryCollection)
		{
			check(false); // This shouldn't happen.
		}
		else
		{
			GeometryParticlePhysicsProxies.RemoveSingleSwap((FGeometryParticlePhysicsProxy*)InProxy);
		}

		// Enqueue a command to remove the particle and delete the proxy
		EnqueueCommandImmediate([InProxy, InParticleType, this]()
		{
			UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("TPBDRigidsSolver::UnregisterObject() ~ Dequeue"));

				// Generally need to remove stale events for particles that no longer exist
				GetEventManager()->template ClearEvents<FCollisionEventData>(EEventType::Collision, [InProxy]
				(FCollisionEventData& EventDataInOut)
				{
					Chaos::FCollisionDataArray const& CollisionData = EventDataInOut.CollisionData.AllCollisionsArray;
					if (CollisionData.Num() > 0)
					{
						check(InProxy);
						TArray<int32> const* const CollisionIndices = EventDataInOut.PhysicsProxyToCollisionIndices.PhysicsProxyToIndicesMap.Find(InProxy);
						if (CollisionIndices)
						{
							for (int32 EncodedCollisionIdx : *CollisionIndices)
							{
								bool bSwapOrder;
								int32 CollisionIdx = Chaos::TEventManager<Traits>::DecodeCollisionIndex(EncodedCollisionIdx, bSwapOrder);

								// invalidate but don't delete from array, as this would mean we'd need to reindex PhysicsProxyToIndicesMap to maintain the other collisions lookup
								Chaos::TCollisionData<float, 3>& CollisionDataItem = EventDataInOut.CollisionData.AllCollisionsArray[CollisionIdx];
								CollisionDataItem.ParticleProxy = nullptr;
								CollisionDataItem.LevelsetProxy = nullptr;
							}

							EventDataInOut.PhysicsProxyToCollisionIndices.PhysicsProxyToIndicesMap.Remove(InProxy);
						}
					}

				});

			// Get the physics thread-handle from the proxy, and then delete the proxy.
			//
			// NOTE: We have to delete the proxy from its derived version, because the
			// base destructor is protected. This makes everything just a bit uglier,
			// maybe that extra safety is not needed if we continue to contain all
			// references to proxy instances entirely in Chaos?
			TGeometryParticleHandle<float, 3>* Handle;
			if (InParticleType == Chaos::EParticleType::Rigid)
			{
				auto Proxy = (FRigidParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}
			else if (InParticleType == Chaos::EParticleType::Kinematic)
			{
				auto Proxy = (FKinematicGeometryParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}
			else
			{
				auto Proxy = (FGeometryParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}

			//If particle was created and destroyed before commands were enqueued just skip. I suspect we can skip entire lambda, but too much code to verify right now

			if(Handle)
			{
				// Remove from rewind data
				if(FRewindData* RewindData = GetRewindData())
				{
					RewindData->RemoveParticle(Handle->UniqueIdx());
				}

				// Remove game thread particle from ActiveGameThreadParticles so we won't crash when pulling physics state
				// if this particle was deleted after buffering results. 
				GetDirtyParticlesBuffer()->RemoveDirtyParticleFromConsumerBuffer(Handle->GTGeometryParticle());
  
				MParticleToProxy.Remove(Handle);
  
				// Use the handle to destroy the particle data
				GetEvolution()->DestroyParticle(Handle);
			}

		});

	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::RegisterObject(TGeometryCollectionPhysicsProxy<Traits>* InProxy)
	{
		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("TPBDRigidsSolver::RegisterObject(TGeometryCollectionPhysicsProxy*)"));
		GeometryCollectionPhysicsProxies.AddUnique(InProxy);
		InProxy->SetSolver(this);
		InProxy->Initialize();
		InProxy->NewData(); // Buffers data on the proxy.
		FParticlesType* InParticles = &GetParticles();

		// Finish registration on the physics thread...
		EnqueueCommandImmediate([InParticles, InProxy, this]()
		{
			UE_LOG(LogPBDRigidsSolver, Verbose, 
				TEXT("TPBDRigidsSolver::RegisterObject(TGeometryCollectionPhysicsProxy*)"));
			check(InParticles);
			InProxy->InitializeBodiesPT(this, *InParticles);
		});
	}

	template <typename Traits>
	bool TPBDRigidsSolver<Traits>::UnregisterObject(TGeometryCollectionPhysicsProxy<Traits>* InProxy)
	{
		EnqueueCommandImmediate([InProxy,this]()
		{
			InProxy->OnRemoveFromSolver(this);
			InProxy->SetSolver(static_cast<TPBDRigidsSolver<Traits>*>(nullptr));
		});
		
		return GeometryCollectionPhysicsProxies.Remove(InProxy) != 0;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::RegisterObject(Chaos::FJointConstraint* GTConstraint)
	{
		FJointConstraintPhysicsProxy* JointProxy = new FJointConstraintPhysicsProxy(GTConstraint, nullptr);
		JointProxy->SetSolver(this);

		JointConstraintPhysicsProxies.AddUnique(JointProxy);
		AddDirtyProxy(JointProxy);
	}

	template <typename Traits>
	bool TPBDRigidsSolver<Traits>::UnregisterObject(Chaos::FJointConstraint* GTConstraint)
	{
		FJointConstraintPhysicsProxy* JointProxy = GTConstraint->GetProxy<FJointConstraintPhysicsProxy>();
		check(JointProxy);

		JointProxy->SetSolver(static_cast<TPBDRigidsSolver<Traits>*>(nullptr));
		RemoveDirtyProxy(JointProxy);

		int32 NumRemoved = JointConstraintPhysicsProxies.Remove(JointProxy);
		GTConstraint->SetProxy(static_cast<FJointConstraintPhysicsProxy*>(nullptr));

		FParticlesType* InParticles = &GetParticles();

		// Finish registration on the physics thread...
		EnqueueCommandImmediate([InParticles, JointProxy, this]()
			{
				JointProxy->DestroyOnPhysicsThread(this);
				delete JointProxy;
			});

		return NumRemoved==1;
	}

	template <typename Traits>
	bool TPBDRigidsSolver<Traits>::IsSimulating() const
	{
		for (FGeometryParticlePhysicsProxy* Obj : GeometryParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FKinematicGeometryParticlePhysicsProxy* Obj : KinematicGeometryParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FRigidParticlePhysicsProxy* Obj : RigidParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FSkeletalMeshPhysicsProxy* Obj : SkeletalMeshPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FStaticMeshPhysicsProxy* Obj : StaticMeshPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (TGeometryCollectionPhysicsProxy<Traits>* Obj : GeometryCollectionPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FJointConstraintPhysicsProxy* Obj : JointConstraintPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		return false;
	}

	int32 RewindCaptureNumFrames = -1;
	FAutoConsoleVariableRef CVarRewindCaptureNumFrames(TEXT("p.RewindCaptureNumFrames"),RewindCaptureNumFrames,TEXT("The number of frames to capture rewind for. Requires restart of solver"));

	int32 UseResimCache = 0;
	FAutoConsoleVariableRef CVarUseResimCache(TEXT("p.UseResimCache"),UseResimCache,TEXT("Whether resim uses cache to skip work, requires recreating world to take effect"));
	
	template <typename Traits>
	void TPBDRigidsSolver<Traits>::Reset()
	{
		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("PBDRigidsSolver::Reset()"));

		MTime = 0;
		MLastDt = 0.0f;
		bEnabled = false;
		CurrentFrame = 0;
		MMaxDeltaTime = 1.f;
		MMinDeltaTime = SMALL_NUMBER;
		MMaxSubSteps = 1;
		MEvolution = TUniquePtr<FPBDRigidsEvolution>(new FPBDRigidsEvolution(Particles, SimMaterials, ChaosSolverCollisionDefaultIterationsCVar, ChaosSolverCollisionDefaultPushoutIterationsCVar, BufferMode == EMultiBufferMode::Single)); 

		PerSolverField = MakeUnique<FPerSolverFieldSystem>();

		//todo: do we need this?
		//MarshallingManager.Reset();

		if(RewindCaptureNumFrames >= 0)
		{
			EnableRewindCapture(RewindCaptureNumFrames, bUseCollisionResimCache || !!UseResimCache);
		}

		MEvolution->SetCaptureRewindDataFunction([this](const TParticleView<TPBDRigidParticles<FReal,3>>& ActiveParticles)
		{
			FinalizeRewindData(ActiveParticles);
		});

		TEventDefaults<Traits>::RegisterSystemEvents(*GetEventManager());
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::ChangeBufferMode(Chaos::EMultiBufferMode InBufferMode)
	{
		// This seems unused inside the solver? #BH
		BufferMode = InBufferMode;

		SetThreadingMode_External(BufferMode == EMultiBufferMode::Single ? EThreadingModeTemp::SingleThread : EThreadingModeTemp::TaskGraph);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::AdvanceSolverBy(const FReal DeltaTime)
	{
		MEvolution->GetCollisionDetector().GetNarrowPhase().GetContext().bDeferUpdate = (ChaosSolverCollisionDeferNarrowPhase != 0);
		MEvolution->GetCollisionDetector().GetNarrowPhase().GetContext().bAllowManifolds = (ChaosSolverCollisionUseManifolds != 0);

		UE_LOG(LogPBDRigidsSolver, Verbose, TEXT("PBDRigidsSolver::Tick(%3.5f)"), DeltaTime);
		if (bEnabled)
		{
			MLastDt = DeltaTime;
			EventPreSolve.Broadcast(DeltaTime);
			AdvanceOneTimeStepTask<Traits>(this, DeltaTime).DoWork();
			EventPreBuffer.Broadcast(DeltaTime);
		}
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::SetExternalTimeConsumed_External(const FReal Time)
	{
		MEvolution->LatestExternalTimeConsumed = Time;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::SyncEvents_GameThread()
	{
		GetEventManager()->DispatchEvents();
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::PushPhysicsState(const FReal DeltaTime)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_PushPhysicsState);
		FPushPhysicsData* PushData = MarshallingManager.GetProducerData_External();
		FDirtySet* DirtyProxiesData = &PushData->DirtyProxiesDataBuffer;
		FDirtyPropertiesManager* Manager = &PushData->DirtyPropertiesManager;

		Manager->SetNumParticles(DirtyProxiesData->NumDirtyProxies());
		Manager->SetNumShapes(DirtyProxiesData->NumDirtyShapes());
		FShapeDirtyData* ShapeDirtyData = DirtyProxiesData->GetShapesDirtyData();
		auto ProcessProxyGT =[ShapeDirtyData, Manager, DirtyProxiesData](auto Proxy, int32 ParticleDataIdx, FDirtyProxy& DirtyProxy)
		{
			auto Particle = Proxy->GetParticle();
			Particle->SyncRemoteData(*Manager,ParticleDataIdx,DirtyProxy.ParticleData,DirtyProxy.ShapeDataIndices,ShapeDirtyData);
			Proxy->ClearAccumulatedData();
			Proxy->ResetDirtyIdx();
		};


		//todo: if we allocate remote data ahead of time we could go wide
		DirtyProxiesData->ParallelForEachProxy([&ProcessProxyGT, this](int32 DataIdx, FDirtyProxy& Dirty)
		{
			switch(Dirty.Proxy->GetType())
			{
			case EPhysicsProxyType::SingleRigidParticleType:
			{
				auto Proxy = static_cast<FRigidParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyGT(Proxy,DataIdx,Dirty);
				break;
			}
			case EPhysicsProxyType::SingleKinematicParticleType:
			{
				auto Proxy = static_cast<FKinematicGeometryParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyGT(Proxy,DataIdx,Dirty);
				break;
			}
			case EPhysicsProxyType::SingleGeometryParticleType:
			{
				auto Proxy = static_cast<FGeometryParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyGT(Proxy,DataIdx,Dirty);
				break;
			}
			case EPhysicsProxyType::GeometryCollectionType:
			{
				// Not invalid but doesn't currently use the remote data process
				break;
			}
			case EPhysicsProxyType::JointConstraintType:
			{
				auto Proxy = static_cast<FJointConstraintPhysicsProxy*>(Dirty.Proxy);
				Proxy->PushStateOnGameThread(this);
				break;
			}
			default:
			ensure(0 && TEXT("Unknown proxy type in physics solver."));
			}
		});

		MarshallingManager.Step_External(DeltaTime);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::ProcessSinglePushedData_Internal(FPushPhysicsData& PushData)
	{
		FRewindData* RewindData = GetRewindData();

		FDirtySet* DirtyProxiesData = &PushData.DirtyProxiesDataBuffer;
		FDirtyPropertiesManager* Manager = &PushData.DirtyPropertiesManager;
		FShapeDirtyData* ShapeDirtyData = DirtyProxiesData->GetShapesDirtyData();

		auto ProcessProxyPT = [Manager,ShapeDirtyData,RewindData,this](auto& Proxy,int32 DataIdx,FDirtyProxy& Dirty,const auto& CreateHandleFunc)
		{
			const bool bIsNew = !Proxy->IsInitialized();
			if(bIsNew)
			{
				const auto* NonFrequentData = Dirty.ParticleData.FindNonFrequentData(*Manager,DataIdx);
				const FUniqueIdx* UniqueIdx = NonFrequentData ? &NonFrequentData->UniqueIdx() : nullptr;
				Proxy->SetHandle(CreateHandleFunc(UniqueIdx));

				auto Handle = Proxy->GetHandle();
				Handle->GTGeometryParticle() = Proxy->GetParticle();
			}

			if(RewindData)
			{
				//may want to remove branch by templatizing lambda
				if(RewindData->IsResim())
				{
					RewindData->PushGTDirtyData<true>(*Manager,DataIdx,Dirty);
				} else
				{
					RewindData->PushGTDirtyData<false>(*Manager,DataIdx,Dirty);
				}
			}

			Proxy->PushToPhysicsState(*Manager,DataIdx,Dirty,ShapeDirtyData,*GetEvolution());

			if(bIsNew)
			{
				auto Handle = Proxy->GetHandle();
				AddParticleToProxy(Handle,Proxy);
				GetEvolution()->CreateParticle(Handle);
				Proxy->SetInitialized(true);
			}

			Dirty.Clear(*Manager,DataIdx,ShapeDirtyData);
		};

		if(RewindData)
		{
			RewindData->PrepareFrame(DirtyProxiesData->NumDirtyProxies());
		}

		//need to create new particle handles
		DirtyProxiesData->ForEachProxy([this,&ProcessProxyPT](int32 DataIdx,FDirtyProxy& Dirty)
		{
			switch(Dirty.Proxy->GetType())
			{
			case EPhysicsProxyType::SingleRigidParticleType:
			{
				auto Proxy = static_cast<FRigidParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyPT(Proxy,DataIdx,Dirty,[this](const FUniqueIdx* UniqueIdx){ return Particles.CreateDynamicParticles(1,UniqueIdx)[0]; });
				break;
			}
			case EPhysicsProxyType::SingleKinematicParticleType:
			{
				auto Proxy = static_cast<FKinematicGeometryParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyPT(Proxy,DataIdx,Dirty,[this](const FUniqueIdx* UniqueIdx){ return Particles.CreateKinematicParticles(1,UniqueIdx)[0]; });
				break;
			}
			case EPhysicsProxyType::SingleGeometryParticleType:
			{
				auto Proxy = static_cast<FGeometryParticlePhysicsProxy*>(Dirty.Proxy);
				ProcessProxyPT(Proxy,DataIdx,Dirty,[this](const FUniqueIdx* UniqueIdx){ return Particles.CreateStaticParticles(1,UniqueIdx)[0]; });
				break;
			}
			case EPhysicsProxyType::GeometryCollectionType:
			{
				// Currently no push needed for geometry collections and they handle the particle creation internally
				// #TODO This skips the rewind data push so GC will not be rewindable until resolved.
				Dirty.Proxy->ResetDirtyIdx();
				break;
			}
			case EPhysicsProxyType::JointConstraintType:
			{
				// Pass until after all bodies are created. 
				break;
			}
			default:
			{
				ensure(0 && TEXT("Unknown proxy type in physics solver."));
				//Can't use, but we can still mark as "clean"
				Dirty.Proxy->ResetDirtyIdx();
			}

			}
		});

		//need to create new constraint handles
		DirtyProxiesData->ForEachProxy([this, &ProcessProxyPT](int32 DataIdx, FDirtyProxy& Dirty)
		{
			switch (Dirty.Proxy->GetType())
			{
			case EPhysicsProxyType::JointConstraintType:
			{
				auto JointProxy = static_cast<FJointConstraintPhysicsProxy*>(Dirty.Proxy);
				const bool bIsNew = !JointProxy->IsInitialized();
				if (bIsNew)
				{
					JointProxy->InitializeOnPhysicsThread(this);
					JointProxy->SetInitialized();
				}
				JointProxy->PushStateOnPhysicsThread(this);
				Dirty.Proxy->ResetDirtyIdx();
				break;
			}
			}
		});

		//MarshallingManager.FreeData_Internal(&PushData);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::ProcessPushedData_Internal(const TArray<FPushPhysicsData*>& PushDataArray)
	{
		for(FPushPhysicsData* PushData : PushDataArray)
		{
			//update callbacks
			{
				for(FSimCallbackHandle* NewCallback : PushData->SimCallbacksToAdd)
				{
					if(ensure(NewCallback->PTHandle == nullptr)) //if not null we have double registration
					{
						FSimCallbackHandlePT* PTCallback = new FSimCallbackHandlePT(NewCallback);	//todo: use better memory management
						SimCallbacks.Add(PTCallback);
						NewCallback->PTHandle = PTCallback;
					}
				}

				for(int32 Idx = 0; Idx < PushData->SimCallbacksToRemove.Num(); ++Idx)
				{
					FSimCallbackHandle* RemovedCallback = PushData->SimCallbacksToRemove[Idx];
					if(ensure(RemovedCallback->PTHandle != nullptr)) //if not null we are unregistering something that was never registered (or double delete) 
					{
						if(Idx == 0)
						{
							//callback was removed right away so skip it entirely (unless it was tagged as running at least once no matter what)
							RemovedCallback->PTHandle->bPendingDelete = !RemovedCallback->bRunOnceMore;
						}
						else
						{
							//want to delete, but came later in interval so need to run at least once
							RemovedCallback->bRunOnceMore = true;
						}
					}
				}

				//save any pending data for this particular interval
				for(const FSimCallbackDataPair& Pair : PushData->SimCallbackDataPairs)
				{
					const FSimCallbackHandle* Handle = Pair.Callback;
					check(Handle->PTHandle);	//must have been registered already
					Handle->PTHandle->IntervalData.Add(Pair.Data);
				}
			}

			ProcessSinglePushedData_Internal(*PushData);
			MarshallingManager.FreeData_Internal(PushData);
		}
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::BufferPhysicsResults()
	{
		//ensure(IsInPhysicsThread());
		TArray<TGeometryCollectionPhysicsProxy<Traits>*> ActiveGC;
		ActiveGC.Reserve(GeometryCollectionPhysicsProxies.Num());

		TParticleView<TPBDRigidParticles<float, 3>>& DirtyParticles = GetParticles().GetDirtyParticlesView();
		for (Chaos::TPBDRigidParticleHandleImp<float, 3, false>& DirtyParticle : DirtyParticles)
		{
			if( const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(DirtyParticle.Handle()))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					if (Proxy != nullptr)
					{
						switch (DirtyParticle.GetParticleType())
						{
						case Chaos::EParticleType::Rigid:
							((FRigidParticlePhysicsProxy*)(Proxy))->BufferPhysicsResults();
							break;
						case Chaos::EParticleType::Kinematic:
							((FKinematicGeometryParticlePhysicsProxy*)(Proxy))->BufferPhysicsResults();
							break;
						case Chaos::EParticleType::Static:
							((FGeometryParticlePhysicsProxy*)(Proxy))->BufferPhysicsResults();
							break;
						case Chaos::EParticleType::GeometryCollection:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						case Chaos::EParticleType::Clustered:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						default:
							check(false);
						}
					}
				}
			}
		}

		for (auto* GCProxy : ActiveGC)
		{
			GCProxy->BufferPhysicsResults();
		}

		for (FJointConstraintPhysicsProxy* Proxy : JointConstraintPhysicsProxies)
		{
			Proxy->BufferPhysicsResults();
		}

		if(bEnabled)
		{
			// Now that results have been buffered we have completed a solve step so we can broadcast that event
			EventPostSolve.Broadcast(MLastDt);
		}
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::FlipBuffers()
	{
		//ensure(IsInPhysicsThread());
		TArray<TGeometryCollectionPhysicsProxy<Traits>*> ActiveGC;
		ActiveGC.Reserve(GeometryCollectionPhysicsProxies.Num());

		TParticleView<TPBDRigidParticles<float, 3>>& DirtyParticles = GetParticles().GetDirtyParticlesView();
		for (Chaos::TPBDRigidParticleHandleImp<float, 3, false>& DirtyParticle : DirtyParticles)
		{
			if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(DirtyParticle.Handle()))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					if (Proxy != nullptr)
					{
						switch (DirtyParticle.GetParticleType())
						{
						case Chaos::EParticleType::Rigid:
							((FRigidParticlePhysicsProxy*)(Proxy))->FlipBuffer();
							break;
						case Chaos::EParticleType::Kinematic:
							((FKinematicGeometryParticlePhysicsProxy*)(Proxy))->FlipBuffer();
							break;
						case Chaos::EParticleType::Static:
							((FGeometryParticlePhysicsProxy*)(Proxy))->FlipBuffer();
							break;
						case Chaos::EParticleType::GeometryCollection:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						case Chaos::EParticleType::Clustered:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						default:
							check(false);
						}
					}
				}
			}
		}

		for (auto* GCProxy : ActiveGC)
		{
			GCProxy->FlipBuffer();
		}

		for (FJointConstraintPhysicsProxy* Proxy : JointConstraintPhysicsProxies)
		{
			Proxy->FlipBuffer();
		}

	}

	// This function is not called during normal Engine execution.  
	// FPhysScene_ChaosInterface::EndFrame() calls 
	// FPhysScene_ChaosInterface::SyncBodies() instead, and then immediately afterwards 
	// calls FPBDRigidsSovler::SyncEvents_GameThread().  This function is used by tests,
	// however.
	template <typename Traits>
	void TPBDRigidsSolver<Traits>::UpdateGameThreadStructures()
	{
		//ensure(IsInGameThread());
		TArray<TGeometryCollectionPhysicsProxy<Traits>*> ActiveGC;
		ActiveGC.Reserve(GeometryCollectionPhysicsProxies.Num());

		TParticleView<TPBDRigidParticles<float, 3>>& DirtyParticles = GetParticles().GetDirtyParticlesView();
		for (Chaos::TPBDRigidParticleHandleImp<float, 3, false>& DirtyParticle : DirtyParticles)
		{
			if (const TSet<IPhysicsProxyBase*>* Proxies = GetProxies(DirtyParticle.Handle()))
			{
				for (IPhysicsProxyBase* Proxy : *Proxies)
				{
					if (Proxy != nullptr)
					{
						switch (DirtyParticle.GetParticleType())
						{
						case Chaos::EParticleType::Rigid:
							((FRigidParticlePhysicsProxy*)(Proxy))->PullFromPhysicsState();
							break;
						case Chaos::EParticleType::Kinematic:
							((FKinematicGeometryParticlePhysicsProxy*)(Proxy))->PullFromPhysicsState();
							break;
						case Chaos::EParticleType::Static:
							((FGeometryParticlePhysicsProxy*)(Proxy))->PullFromPhysicsState();
							break;
						case Chaos::EParticleType::GeometryCollection:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						case Chaos::EParticleType::Clustered:
							ActiveGC.AddUnique((TGeometryCollectionPhysicsProxy<Traits>*)(Proxy));
							break;
						default:
							check(false);
						}
					}
				}
			}
		}

		for (auto* GCProxy : ActiveGC)
		{
			GCProxy->PullFromPhysicsState();
		}

		for (FJointConstraintPhysicsProxy* Proxy : JointConstraintPhysicsProxies)
		{
			Proxy->PullFromPhysicsState();
		}

	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::PostTickDebugDraw() const
	{
#if CHAOS_DEBUG_DRAW
		if (ChaosSolverDrawCollisions == 1) {
			DebugDraw::DrawCollisions(TRigidTransform<float, 3>(), GetEvolution()->GetCollisionConstraints(), 1.f);
		}
#endif
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::UpdateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData)
	{
		*SimMaterials.Get(InHandle.InnerHandle) = InNewData;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::CreateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData)
	{
		ensure(SimMaterials.Create(InNewData) == InHandle.InnerHandle);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::DestroyMaterial(Chaos::FMaterialHandle InHandle)
	{
		SimMaterials.Destroy(InHandle.InnerHandle);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::UpdateMaterialMask(Chaos::FMaterialMaskHandle InHandle, const Chaos::FChaosPhysicsMaterialMask& InNewData)
	{
		*SimMaterialMasks.Get(InHandle.InnerHandle) = InNewData;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::CreateMaterialMask(Chaos::FMaterialMaskHandle InHandle, const Chaos::FChaosPhysicsMaterialMask& InNewData)
	{
		ensure(SimMaterialMasks.Create(InNewData) == InHandle.InnerHandle);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::DestroyMaterialMask(Chaos::FMaterialMaskHandle InHandle)
	{
		SimMaterialMasks.Destroy(InHandle.InnerHandle);
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::SyncQueryMaterials()
	{
		TSolverQueryMaterialScope<ELockType::Write> Scope(this);
		QueryMaterials = SimMaterials;
		QueryMaterialMasks = SimMaterialMasks;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::EnableRewindCapture(int32 NumFrames, bool InUseCollisionResimCache)
	{
		check(Traits::IsRewindable());
		MRewindData = MakeUnique<FRewindData>(NumFrames, InUseCollisionResimCache);
		bUseCollisionResimCache = InUseCollisionResimCache;
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::FinalizeRewindData(const TParticleView<TPBDRigidParticles<FReal,3>>& DirtyParticles)
	{
		using namespace Chaos;
		//Simulated objects must have their properties captured for rewind
		if(MRewindData && DirtyParticles.Num())
		{
			QUICK_SCOPE_CYCLE_COUNTER(RecordRewindData);

			MRewindData->PrepareFrameForPTDirty(DirtyParticles.Num());
			
			int32 DataIdx = 0;
			for(TPBDRigidParticleHandleImp<float,3,false>& DirtyParticle : DirtyParticles)
			{
				//may want to remove branch using templates outside loop
				if (MRewindData->IsResim())
				{
					MRewindData->PushPTDirtyData<true>(*DirtyParticle.Handle(), DataIdx++);
				}
				else
				{
					MRewindData->PushPTDirtyData<false>(*DirtyParticle.Handle(), DataIdx++);
				}
			}
		}
	}

	template <typename Traits>
	void TPBDRigidsSolver<Traits>::UpdateExternalAccelerationStructure_External(TUniquePtr<ISpatialAccelerationCollection<TAccelerationStructureHandle<FReal,3>,FReal,3>>& ExternalStructure)
	{
		GetEvolution()->UpdateExternalAccelerationStructure_External(ExternalStructure,*PendingSpatialOperations_External);
	}

#define EVOLUTION_TRAIT(Trait) template class CHAOSSOLVERS_API TPBDRigidsSolver<Trait>;
#include "Chaos/EvolutionTraits.inl"
#undef EVOLUTION_TRAIT

}; // namespace Chaos
