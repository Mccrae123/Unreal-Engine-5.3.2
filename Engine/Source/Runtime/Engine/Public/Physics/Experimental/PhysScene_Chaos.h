// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "EventsData.h"
#include "Physics/PhysScene.h"
#include "Physics/Experimental/ChaosEventType.h"
#include "GameFramework/Actor.h"
#include "PhysicsPublic.h"
#include "PhysInterface_Chaos.h"
#include "Physics/PhysicsInterfaceUtils.h"
#include "Chaos/ChaosScene.h"
#include "Chaos/ContactModification.h"
#include "Chaos/Real.h"
#include "UObject/ObjectKey.h"

#ifndef CHAOS_WITH_PAUSABLE_SOLVER
#define CHAOS_WITH_PAUSABLE_SOLVER 1
#endif

// Currently compilation issue with Incredibuild when including headers required by event template functions
#define XGE_FIXED 0

class UPrimitiveComponent;

class AdvanceOneTimeStepTask;
class IPhysicsReplication;
class FPhysInterface_Chaos;
class FChaosSolversModule;
struct FForceFieldProxy;
struct FSolverStateStorage;

class FSkeletalMeshPhysicsProxy;
class FStaticMeshPhysicsProxy;
class FPerSolverFieldSystem;

class IPhysicsProxyBase;

class UWorld;
class UChaosEventRelay;
class AWorldSettings;
class FPhysicsReplicationFactory;
class FContactModifyCallbackFactory;
struct FConstraintInstanceBase;

namespace Chaos
{
	class FPhysicsProxy;
	class FClusterUnionPhysicsProxy;

	struct FCollisionEventData;

	enum class EEventType : int32;

	template<typename PayloadType, typename HandlerType>
	class TRawEventHandler;

	class FAccelerationStructureHandle;

	template <typename TPayload, typename T, int d>
	class ISpatialAcceleration;

	template <typename TPayload, typename T, int d>
	class ISpatialAccelerationCollection;

	template <typename T>
	class TArrayCollectionArray;

}


extern int32 GEnableKinematicDeferralStartPhysicsCondition;

struct FConstraintBrokenDelegateWrapper
{
	FConstraintBrokenDelegateWrapper(FConstraintInstanceBase* ConstraintInstance);

	void DispatchOnBroken();

	FOnConstraintBroken OnConstraintBrokenDelegate;
	int32 ConstraintIndex;
};

struct FPlasticDeformationDelegateWrapper
{
	FPlasticDeformationDelegateWrapper(FConstraintInstanceBase* ConstraintInstance);

	void DispatchPlasticDeformation();

	FOnPlasticDeformation OnPlasticDeformationDelegate;
	int32 ConstraintIndex;
};

/**
* Low level Chaos scene used when building custom simulations that don't exist in the main world physics scene.
*/
class ENGINE_API FPhysScene_Chaos : public FChaosScene
{
public:

	using Super = FChaosScene;
	
	FPhysScene_Chaos(AActor* InSolverActor=nullptr
#if CHAOS_DEBUG_NAME
	, const FName& DebugName=NAME_None
#endif
);

	virtual ~FPhysScene_Chaos();

	/** Returns the actor that owns this solver. */
	AActor* GetSolverActor() const;

	void RegisterForCollisionEvents(UPrimitiveComponent* Component);
	void UnRegisterForCollisionEvents(UPrimitiveComponent* Component);

	void RegisterForGlobalCollisionEvents(UPrimitiveComponent* Component);
	void UnRegisterForGlobalCollisionEvents(UPrimitiveComponent* Component);

	void RegisterForGlobalRemovalEvents(UPrimitiveComponent* Component);
	void UnRegisterForGlobalRemovalEvents(UPrimitiveComponent* Component);

	void RegisterAsyncPhysicsTickComponent(UActorComponent* Component);
	void UnregisterAsyncPhysicsTickComponent(UActorComponent* Component);

	void RegisterAsyncPhysicsTickActor(AActor* Actor);
	void UnregisterAsyncPhysicsTickActor(AActor* Actor);

	void EnqueueAsyncPhysicsCommand(int32 PhysicsStep, UObject* OwningObject, const TFunction<void()>& Command, const bool bEnableResim = false);


	/**
	 * Called during creation of the physics state for gamethread objects to pass off an object to the physics thread
	 */
	void AddObject(UPrimitiveComponent* Component, FSkeletalMeshPhysicsProxy* InObject);
	void AddObject(UPrimitiveComponent* Component, FStaticMeshPhysicsProxy* InObject);
	void AddObject(UPrimitiveComponent* Component, Chaos::FSingleParticlePhysicsProxy* InObject);
	void AddObject(UPrimitiveComponent* Component, FGeometryCollectionPhysicsProxy* InObject);
	void AddObject(UPrimitiveComponent* Component, Chaos::FClusterUnionPhysicsProxy* InObject);
	
	void AddToComponentMaps(UPrimitiveComponent* Component, IPhysicsProxyBase* InObject);
	void RemoveFromComponentMaps(IPhysicsProxyBase* InObject);

	/**
	 * Called during physics state destruction for the game thread to remove objects from the simulation
	 * #BG TODO - Doesn't actually remove from the evolution at the moment
	 */
	void RemoveObject(FSkeletalMeshPhysicsProxy* InObject);
	void RemoveObject(FStaticMeshPhysicsProxy* InObject);
	void RemoveObject(Chaos::FSingleParticlePhysicsProxy* InObject);
	void RemoveObject(FGeometryCollectionPhysicsProxy* InObject);
	void RemoveObject(Chaos::FClusterUnionPhysicsProxy* InObject);

	IPhysicsReplication* GetPhysicsReplication();

	UE_DEPRECATED(5.3, "Can no longer direclty set physics replication at runtime. For now, specify a PhysicsReplication factory instead. This function will take ownership of the IPhysicsReplication's lifetime.")
	void SetPhysicsReplication(IPhysicsReplication* InPhysicsReplication);

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	/** Given a solver object, returns its associated component. */
	template<class OwnerType>
	OwnerType* GetOwningComponent(const IPhysicsProxyBase* PhysicsProxy) const
	{ 
		auto* CompPtr = PhysicsProxyToComponentMap.Find(PhysicsProxy);
		return CompPtr ? Cast<OwnerType>(*CompPtr) : nullptr;
	}

	/** Given a component, returns its associated solver objects. */
	const TArray<IPhysicsProxyBase*>* GetOwnedPhysicsProxies(UPrimitiveComponent* Comp) const
	{
		return ComponentToPhysicsProxyMap.Find(Comp);
	}

	/** Given a physics proxy, returns its associated body instance if any */
	FBodyInstance* GetBodyInstanceFromProxy(const IPhysicsProxyBase* PhysicsProxy) const;
	const FBodyInstance* GetBodyInstanceFromProxyAndShape(IPhysicsProxyBase* InProxy, int32 InShapeIndex) const;
	/**
	 * Callback when a world ends, to mark updated packages dirty. This can't be done in final
	 * sync as the editor will ignore packages being dirtied in PIE. Also used to clean up any other references
	 */
	void OnWorldEndPlay();
	void OnWorldBeginPlay();

	void AddAggregateToScene(const FPhysicsAggregateHandle& InAggregate);

	void SetOwningWorld(UWorld* InOwningWorld);

	UWorld* GetOwningWorld();
	const UWorld* GetOwningWorld() const;

	void ResimNFrames(int32 NumFrames);

	void RemoveBodyInstanceFromPendingLists_AssumesLocked(FBodyInstance* BodyInstance, int32 SceneType);
	void AddCustomPhysics_AssumesLocked(FBodyInstance* BodyInstance, FCalculateCustomPhysics& CalculateCustomPhysics);
	void AddForce_AssumesLocked(FBodyInstance* BodyInstance, const FVector& Force, bool bAllowSubstepping, bool bAccelChange);
	void AddForceAtPosition_AssumesLocked(FBodyInstance* BodyInstance, const FVector& Force, const FVector& Position, bool bAllowSubstepping, bool bIsLocalForce = false);
	void AddRadialForceToBody_AssumesLocked(FBodyInstance* BodyInstance, const FVector& Origin, const float Radius, const float Strength, const uint8 Falloff, bool bAccelChange, bool bAllowSubstepping);
	void ClearForces_AssumesLocked(FBodyInstance* BodyInstance, bool bAllowSubstepping);
	void AddTorque_AssumesLocked(FBodyInstance* BodyInstance, const FVector& Torque, bool bAllowSubstepping, bool bAccelChange);
	void ClearTorques_AssumesLocked(FBodyInstance* BodyInstance, bool bAllowSubstepping);
	void SetKinematicTarget_AssumesLocked(FBodyInstance* BodyInstance, const FTransform& TargetTM, bool bAllowSubstepping);
	bool GetKinematicTarget_AssumesLocked(const FBodyInstance* BodyInstance, FTransform& OutTM) const;

	bool MarkForPreSimKinematicUpdate(USkeletalMeshComponent* InSkelComp, ETeleportType InTeleport, bool bNeedsSkinning);
	void ClearPreSimKinematicUpdate(USkeletalMeshComponent* InSkelComp);

	void AddPendingOnConstraintBreak(FConstraintInstance* ConstraintInstance, int32 SceneType);
	void AddPendingSleepingEvent(FBodyInstance* BI, ESleepEvent SleepEventType, int32 SceneType);

	int32 DirtyElementCount(Chaos::ISpatialAccelerationCollection<Chaos::FAccelerationStructureHandle, Chaos::FReal, 3>& Collection);

	TArray<FCollisionNotifyInfo>& GetPendingCollisionNotifies(int32 SceneType);

	static bool SupportsOriginShifting();
	void ApplyWorldOffset(FVector InOffset);
	virtual float OnStartFrame(float InDeltaTime) override;

	bool HandleExecCommands(const TCHAR* Cmd, FOutputDevice* Ar);
	void ListAwakeRigidBodies(bool bIncludeKinematic);
	int32 GetNumAwakeBodies() const;

	static TSharedPtr<IPhysicsReplicationFactory> PhysicsReplicationFactory;

	void StartAsync();
	bool HasAsyncScene() const;
	void SetPhysXTreeRebuildRate(int32 RebuildRate);
	void EnsureCollisionTreeIsBuilt(UWorld* World);
	void KillVisualDebugger();

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhysScenePreTick, FPhysScene_Chaos*, float /*DeltaSeconds*/);
	FOnPhysScenePreTick OnPhysScenePreTick;
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhysSceneStep, FPhysScene_Chaos*, float /*DeltaSeconds*/);
	FOnPhysSceneStep OnPhysSceneStep;

	bool ExecPxVis(uint32 SceneType, const TCHAR* Cmd, FOutputDevice* Ar);
	bool ExecApexVis(uint32 SceneType, const TCHAR* Cmd, FOutputDevice* Ar);

	static Chaos::FCollisionModifierCallback CollisionModifierCallback;

	void DeferPhysicsStateCreation(UPrimitiveComponent* Component);
	void RemoveDeferredPhysicsStateCreation(UPrimitiveComponent* Component);
	void ProcessDeferredCreatePhysicsState();

	UChaosEventRelay* GetChaosEventRelay() const { return ChaosEventRelay.Get(); }

	/** Populate the replication cache from the list of registered components */
	void PopulateReplicationCache(const int32 PhysicsStep);

	// Storage structure for replication data
	// probably should just expose read/write API not the structure directly itself like this.
	struct FPrimitiveComponentReplicationCache
	{
		int32 ServerFrame = 0;
		TMap<FObjectKey, FRigidBodyState>	Map;
	};

	FPrimitiveComponentReplicationCache ReplicationCache;

private:
	TSet<UPrimitiveComponent*> CollisionEventRegistrations;
	TSet<UPrimitiveComponent*> GlobalCollisionEventRegistrations;
	TSet<UPrimitiveComponent*> GlobalRemovalEventRegistrations;

	// contains the set of properties that uniquely identifies a reported collision
	// Note that order matters, { Body0, Body1 } is not the same as { Body1, Body0 }
	struct FUniqueContactPairKey
	{
		const void* Body0;
		const void* Body1;

		friend bool operator==(const FUniqueContactPairKey& Lhs, const FUniqueContactPairKey& Rhs)
		{
			return Lhs.Body0 == Rhs.Body0 && Lhs.Body1 == Rhs.Body1;
		}

		friend inline uint32 GetTypeHash(FUniqueContactPairKey const& P)
		{
			return (uint32)((PTRINT)P.Body0 ^ ((PTRINT)P.Body1 << 18));
		}
	};

	FCollisionNotifyInfo& GetPendingCollisionForContactPair(const void* P0, const void* P1, Chaos::FReal SolverTime, bool& bNewEntry);
	/** Key is the unique pair, value is index into PendingNotifies array */
	TMultiMap<FUniqueContactPairKey, int32> ContactPairToPendingNotifyMap;

	/** Holds the list of pending legacy notifies that are to be processed */
	TArray<FCollisionNotifyInfo> PendingCollisionNotifies;

	// Chaos Event Handlers
	void HandleEachCollisionEvent(const TArray<int32>& CollisionIndices, IPhysicsProxyBase* PhysicsProxy0, UPrimitiveComponent* const Comp0, Chaos::FCollisionDataArray const& CollisionData, Chaos::FReal MinDeltaVelocityThreshold);
	void HandleGlobalCollisionEvent(Chaos::FCollisionDataArray const& CollisionData);
	void HandleCollisionEvents(const Chaos::FCollisionEventData& CollisionData);
	void DispatchPendingCollisionNotifies();

	void HandleBreakingEvents(const Chaos::FBreakingEventData& Event);
	void HandleRemovalEvents(const Chaos::FRemovalEventData& Event);
	void HandleCrumblingEvents(const Chaos::FCrumblingEventData& Event);

	/** Replication manager that updates physics bodies towards replicated physics state */
	TUniquePtr<IPhysicsReplication> PhysicsReplication;

#if CHAOS_WITH_PAUSABLE_SOLVER
	/** Callback that checks the status of the world settings for this scene before pausing/unpausing its solver. */
	void OnUpdateWorldPause();
#endif


#if WITH_EDITOR
	bool IsOwningWorldEditor() const;
#endif

	virtual void OnSyncBodies(Chaos::FPhysicsSolverBase* Solver) override;

	void EnableAsyncPhysicsTickCallback();

#if 0
	void SetKinematicTransform(FPhysicsActorHandle& InActorReference,const Chaos::TRigidTransform<float,3>& NewTransform)
	{
		// #todo : Initialize
		// Set the buffered kinematic data on the game and render thread
		// InActorReference.GetPhysicsProxy()->SetKinematicData(...)
	}

	void EnableCollisionPair(const TTuple<int32,int32>& CollisionPair)
	{
		// #todo : Implement
	}

	void DisableCollisionPair(const TTuple<int32,int32>& CollisionPair)
	{
		// #todo : Implement
	}
#endif

	FPhysicsConstraintHandle AddSpringConstraint(const TArray< TPair<FPhysicsActorHandle,FPhysicsActorHandle> >& Constraint);
	void RemoveSpringConstraint(const FPhysicsConstraintHandle& Constraint);

#if 0
	void AddForce(const Chaos::FVec3& Force,FPhysicsActorHandle& Handle)
	{
		// #todo : Implement
	}

	void AddTorque(const Chaos::FVec3& Torque,FPhysicsActorHandle& Handle)
	{
		// #todo : Implement
	}
#endif

	/** Process kinematic updates on any deferred skeletal meshes */
	void UpdateKinematicsOnDeferredSkelMeshes();

	/** Information about how to perform kinematic update before physics */
	struct FDeferredKinematicUpdateInfo
	{
		/** Whether to teleport physics bodies or not */
		ETeleportType	TeleportType;
		/** Whether to update skinning info */
		bool			bNeedsSkinning;
	};

	/** Map of SkeletalMeshComponents that need their bone transforms sent to the physics engine before simulation. */
	TArray<TPair<TWeakObjectPtr<USkeletalMeshComponent>, FDeferredKinematicUpdateInfo>> DeferredKinematicUpdateSkelMeshes;

	TSet<UPrimitiveComponent*> DeferredCreatePhysicsStateComponents;
	//Body Instances
	TUniquePtr<Chaos::TArrayCollectionArray<FBodyInstance*>> BodyInstances;
	// Temp Interface
	TArray<FCollisionNotifyInfo> MNotifies;

	// Maps PhysicsProxy to Component that created the PhysicsProxy
	TMap<IPhysicsProxyBase*, TObjectPtr<UPrimitiveComponent>> PhysicsProxyToComponentMap;

	// Maps Component to PhysicsProxy that is created
	TMap<UPrimitiveComponent*, TArray<IPhysicsProxyBase*>> ComponentToPhysicsProxyMap;

	//TSet<UActorComponent*> FixedTickComponents;
	//TSet<AActor*> FixedTickActors;

	/** The SolverActor that spawned and owns this scene */
	TWeakObjectPtr<AActor> SolverActor;

	TObjectPtr<UChaosEventRelay> ChaosEventRelay;

	Chaos::FReal LastEventDispatchTime;
	Chaos::FReal LastBreakEventDispatchTime;
	Chaos::FReal LastRemovalEventDispatchTime;
	Chaos::FReal LastCrumblingEventDispatchTime;
	
	class FAsyncPhysicsTickCallback* AsyncPhysicsTickCallback = nullptr;

#if WITH_EDITOR
	// Counter used to check a match with the single step status.
	int32 SingleStepCounter;
#endif
#if CHAOS_WITH_PAUSABLE_SOLVER
	// Cache the state of the game pause in order to avoid sending extraneous commands to the solver.
	bool bIsWorldPaused;
#endif

	// Allow other code to obtain read-locks when needed
	friend struct ChaosInterface::FScopedSceneReadLock;
	friend struct FScopedSceneLock_Chaos;
};

