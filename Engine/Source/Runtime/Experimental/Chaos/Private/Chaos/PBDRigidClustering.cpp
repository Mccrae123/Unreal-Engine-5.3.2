// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/PBDRigidClustering.h"

#include "Chaos/ErrorReporter.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/Levelset.h"
#include "Chaos/MassProperties.h"
#include "Chaos/PBDRigidsEvolution.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDRigidClusteringAlgo.h"
#include "Chaos/Sphere.h"
#include "Chaos/UniformGrid.h"
#include "ChaosStats.h"
#include "Containers/Queue.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "Voronoi/Voronoi.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PerParticleInitForce.h"
#include "Chaos/PerParticleEulerStepVelocity.h"
#include "Chaos/PerParticleEtherDrag.h"
#include "Chaos/PerParticlePBDEulerStep.h"
#include "Chaos/StrainModification.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "CoreMinimal.h"

namespace Chaos
{
	//
	//  Connectivity PVar
	//
	FRealSingle ClusterDistanceThreshold = 100.f;
	FAutoConsoleVariableRef CVarClusterDistance(TEXT("p.ClusterDistanceThreshold"), ClusterDistanceThreshold, TEXT("How close a cluster child must be to a contact to break off"));

	int32 UseConnectivity = 1;
	FAutoConsoleVariableRef CVarUseConnectivity(TEXT("p.UseConnectivity"), UseConnectivity, TEXT("Whether to use connectivity graph when breaking up clusters"));

	int32 ComputeClusterCollisionStrains = 1;
	FAutoConsoleVariableRef CVarComputeClusterCollisionStrains(TEXT("p.ComputeClusterCollisionStrains"), ComputeClusterCollisionStrains, TEXT("Whether to use collision constraints when processing clustering."));

	int32 DeactivateClusterChildren = 0;
	FAutoConsoleVariableRef CVarDeactivateClusterChildren(TEXT("p.DeactivateClusterChildren"), DeactivateClusterChildren, TEXT("If children should be decativated when broken and put into another cluster."));

	int32 UseBoundingBoxForConnectionGraphFiltering = 0;
	FAutoConsoleVariableRef CVarUseBoundingBoxForConnectionGraphFiltering(TEXT("p.UseBoundingBoxForConnectionGraphFiltering"), UseBoundingBoxForConnectionGraphFiltering, TEXT("when on, use bounding box overlaps to filter connection during the connection graph generation [def: 0]"));

	float BoundingBoxMarginForConnectionGraphFiltering = 0;
	FAutoConsoleVariableRef CVarBoundingBoxMarginForConnectionGraphFiltering(TEXT("p.BoundingBoxMarginForConnectionGraphFiltering"), BoundingBoxMarginForConnectionGraphFiltering, TEXT("when UseBoundingBoxForConnectionGraphFiltering is on, the margin to use for the oevrlap test [def: 0]"));

	int32 GraphPropagationBasedCollisionImpulseProcessing = 0;
	FAutoConsoleVariableRef CVarGraphPropagationBasedCollisionImpulseProcessing(TEXT("p.GraphPropagationBasedCollisionImpulseProcessing"), GraphPropagationBasedCollisionImpulseProcessing, TEXT("when processing collision impulse toc ompute strain, pick the closest child from the impact point and propagate using the connection graph [def: 0]"));

	float GraphPropagationBasedCollisionFactor = 1;
	FAutoConsoleVariableRef CVarGraphPropagationBasedCollisionFactor(TEXT("p.GraphPropagationBasedCollisionFactor"), GraphPropagationBasedCollisionFactor, TEXT("when p.GraphPropagationBasedCollisionImpulseProcessing is on, the percentage [0-1] of remaining damage that is distributed to the connected pieces"));

	float RestoreBreakingMomentumPercent = .5;
	FAutoConsoleVariableRef CVarRestoreBreakingMomentumPercent(TEXT("p.RestoreBreakingMomentumPercent"), RestoreBreakingMomentumPercent, TEXT("When a rigid cluster is broken, objects that its in contact with will receive an impulse to restore this percent of their momentum prior to the break."));

	template <typename TProxy=FGeometryCollectionPhysicsProxy>
	TProxy* GetConcreteProxy(FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		if (ClusteredParticle)
		{
			if (IPhysicsProxyBase* Proxy = ClusteredParticle->PhysicsProxy())
			{
				if (Proxy->GetType() == TProxy::ConcreteType())
				{
					return static_cast<TProxy*>(Proxy);
				}
			}
		}
		return nullptr;
	}

	template <typename TProxy=FGeometryCollectionPhysicsProxy>
	const TProxy* GetConcreteProxy(const FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		if (ClusteredParticle)
		{
			if (const IPhysicsProxyBase* Proxy = ClusteredParticle->PhysicsProxy())
			{
				if (Proxy->GetType() == TProxy::ConcreteType())
				{
					return static_cast<const TProxy*>(Proxy);
				}
			}
		}
		return nullptr;
	}
	
	//==========================================================================
	// TPBDRigidClustering
	//==========================================================================

	FRigidClustering::FRigidClustering(FPBDRigidsEvolution& InEvolution, FPBDRigidClusteredParticles& InParticles, const TArray<ISimCallbackObject*>* InStrainModifiers)
		: MEvolution(InEvolution)
		, MParticles(InParticles)
		, ClusterUnionManager(*this, InEvolution)
		, MCollisionImpulseArrayDirty(true)
		, DoGenerateBreakingData(false)
		, MClusterConnectionFactor(1.0)
		, MClusterUnionConnectionType(FClusterCreationParameters::EConnectionMethod::DelaunayTriangulation)
		, StrainModifiers(InStrainModifiers)
	{}

	FRigidClustering::~FRigidClustering()
	{}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::CreateClusterParticle"), STAT_CreateClusterParticle, STATGROUP_Chaos);
	Chaos::FPBDRigidClusteredParticleHandle* FRigidClustering::CreateClusterParticle(
		const int32 ClusterGroupIndex, 
		TArray<Chaos::FPBDRigidParticleHandle*>&& Children, 
		const FClusterCreationParameters& Parameters, 
		TSharedPtr<Chaos::FImplicitObject, ESPMode::ThreadSafe> ProxyGeometry, 
		const FRigidTransform3* ForceMassOrientation, 
		const FUniqueIdx* ExistingIndex)
	{
		SCOPE_CYCLE_COUNTER(STAT_CreateClusterParticle);

		Chaos::FPBDRigidClusteredParticleHandle* NewParticle = Parameters.ClusterParticleHandle;
		if (!NewParticle)
		{
			NewParticle = MEvolution.CreateClusteredParticles(1, ExistingIndex)[0]; // calls Evolution.DirtyParticle()
		}

		// Must do this so that the constraint graph knows about this particle 
		MEvolution.EnableParticle(NewParticle);
		NewParticle->SetCollisionGroup(INT_MAX);
		TopLevelClusterParents.Add(NewParticle);

		NewParticle->SetInternalCluster(false);
		NewParticle->SetClusterId(ClusterId(nullptr, Children.Num()));
		NewParticle->SetClusterGroupIndex(ClusterGroupIndex);
		NewParticle->SetInternalStrains(0.0);
		UpdateTopLevelParticle(NewParticle);
		NewParticle->SetIsAnchored(Parameters.bIsAnchored);

		// Update clustering data structures.
		if (MChildren.Contains(NewParticle))
		{
			MChildren[NewParticle] = MoveTemp(Children);
		}
		else
		{
			MChildren.Add(NewParticle, MoveTemp(Children));
		}

		const TArray<FPBDRigidParticleHandle*>& ChildrenArray = MChildren[NewParticle];
		TSet<FPBDRigidParticleHandle*> ChildrenSet(ChildrenArray);

		// Disable the children
		MEvolution.DisableParticles(reinterpret_cast<TSet<FGeometryParticleHandle*>&>(ChildrenSet));

		bool bClusterIsAsleep = true;
		for (FPBDRigidParticleHandle* Child : ChildrenSet)
		{
			bClusterIsAsleep &= Child->Sleeping();

			if (FPBDRigidClusteredParticleHandle* ClusteredChild = Child->CastToClustered())
			{
				TopLevelClusterParents.Remove(ClusteredChild);
				TopLevelClusterParentsStrained.Remove(ClusteredChild);

				// Cluster group id 0 means "don't union with other things"
				// TODO: Use INDEX_NONE instead of 0?
				ClusteredChild->SetClusterGroupIndex(0);
				ClusteredChild->ClusterIds().Id = NewParticle;
				NewParticle->SetInternalStrains(NewParticle->GetInternalStrains() + ClusteredChild->GetInternalStrains());
				UpdateTopLevelParticle(NewParticle);

				NewParticle->SetCollisionImpulses(FMath::Max(NewParticle->CollisionImpulses(), ClusteredChild->CollisionImpulses()));

				const int32 NewCG = NewParticle->CollisionGroup();
				const int32 ChildCG = ClusteredChild->CollisionGroup();
				NewParticle->SetCollisionGroup(NewCG < ChildCG ? NewCG : ChildCG);
			}
		}
		if (ChildrenSet.Num())
		{
			NewParticle->SetInternalStrains(NewParticle->GetInternalStrains() / static_cast<FRealSingle>(ChildrenSet.Num()));
			UpdateTopLevelParticle(NewParticle);
		}

		// TODO: This needs to be rotated to diagonal, used to update I()/InvI() from diagonal, and update transform with rotation.
		FMatrix33 ClusterInertia(0);
		UpdateClusterMassProperties(NewParticle, ChildrenSet, ClusterInertia, ForceMassOrientation);
		UpdateKinematicProperties(NewParticle, MChildren, MEvolution);
		UpdateGeometry(NewParticle, ChildrenSet, MChildren, ProxyGeometry, Parameters);
		GenerateConnectionGraph(NewParticle, Parameters);

		NewParticle->SetSleeping(bClusterIsAsleep);

		auto AddToClusterUnion = [this](int32 ClusterID, FPBDRigidClusteredParticleHandle* Handle)
		{
			if (ClusterID <= 0)
			{
				return;
			}

			ClusterUnionManager.AddPendingExplicitIndexOperation(ClusterID, EClusterUnionOperation::AddReleased, { Handle });
		};

		if(ClusterGroupIndex)
		{
			AddToClusterUnion(ClusterGroupIndex, NewParticle);
		}

		return NewParticle;
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::AddParticlesToCluster"), STAT_AddParticlesToCluster, STATGROUP_Chaos);
	void
	FRigidClustering::AddParticlesToCluster(
		FPBDRigidClusteredParticleHandle* Cluster,
		const TArray<FPBDRigidParticleHandle*>& InChildren,
		const TMap<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*>& ChildToParentMap)
	{
		SCOPE_CYCLE_COUNTER(STAT_AddParticlesToCluster);
		if (!Cluster || InChildren.IsEmpty())
		{
			return;
		}

		FRigidHandleArray& Children = MChildren.FindOrAdd(Cluster);
		const int32 OldNumChildren = Children.Num();
		Children.Append(InChildren);

		// Disable all the input children since they no longer need to be simulated.
		TSet<FPBDRigidParticleHandle*> InChildrenSet(InChildren);
		for (FPBDRigidParticleHandle* Handle : InChildren)
		{
			MEvolution.DisableParticle(Handle);
			MEvolution.GetParticles().MarkTransientDirtyParticle(Handle);

			if (FPBDRigidClusteredParticleHandle* ClusteredChild = Handle->CastToClustered())
			{
				TopLevelClusterParents.Remove(ClusteredChild);
				TopLevelClusterParentsStrained.Remove(ClusteredChild);

				ClusteredChild->ClusterIds().Id = Cluster;
			}
		}

		// Note that we want to compute the internal strain on the cluster the same if we build it up incrementally as well as if we
		// build it all at the same time. The parent cluster's internal strain should be the average of all the child strains.
		// The easy way to compute the new average is the multiply the old average by the number of old elements, add in the new strains,
		// and then divide by the new total number of elements.
		Cluster->SetInternalStrains(Cluster->GetInternalStrains() * static_cast<FRealSingle>(OldNumChildren));
		Cluster->ClusterIds().NumChildren = Children.Num();

		UpdateClusterParticlePropertiesFromChildren(Cluster, InChildren, ChildToParentMap);
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::RemoveParticlesFromCluster"), STAT_RemoveParticlesFromCluster, STATGROUP_Chaos);
	void
	FRigidClustering::RemoveParticlesFromCluster(
		FPBDRigidClusteredParticleHandle* Cluster,
		const TArray<FPBDRigidParticleHandle*>& InChildren)
	{
		SCOPE_CYCLE_COUNTER(STAT_RemoveParticlesFromCluster);

		FRigidHandleArray& Children = MChildren.FindOrAdd(Cluster);
		for (FPBDRigidParticleHandle* Child : InChildren)
		{
			if (int32 Index = Children.Find(Child); Child && Index != INDEX_NONE)
			{
				RemoveChildFromParent(Child, Cluster);
				Children.RemoveAtSwap(Index);
				MEvolution.DirtyParticle(*Child);
				MEvolution.GetParticles().MarkTransientDirtyParticle(Child);
			}
		}

		Cluster->ClusterIds().NumChildren = Children.Num();
		Cluster->SetInternalStrains(0.0);
		Cluster->SetCollisionGroup(INT_MAX);
		Cluster->ClearPhysicsProxies();

		// We need to fully rebuild the cluster properties from the set of children.
		UpdateClusterParticlePropertiesFromChildren(Cluster, Children, {});
		MEvolution.DirtyParticle(*Cluster);
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::UpdateClusterParticlePropertiesFromChildren"), STAT_UpdateClusterParticlePropertiesFromChildren, STATGROUP_Chaos);
	void
	FRigidClustering::UpdateClusterParticlePropertiesFromChildren(
		FPBDRigidClusteredParticleHandle* Cluster,
		const FRigidHandleArray& Children,
		const TMap<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*>& ChildToParentMap)
	{
		// An initial pass through the children to transfer some of their cluster properties to their new parent.
		for (FPBDRigidParticleHandle* Child : Children)
		{
			if (FPBDRigidClusteredParticleHandle* ClusteredChild = Child->CastToClustered())
			{
				Cluster->SetInternalStrains(Cluster->GetInternalStrains() + ClusteredChild->GetInternalStrains());
				Cluster->SetCollisionImpulses(FMath::Max(Cluster->CollisionImpulses(), ClusteredChild->CollisionImpulses()));

				const int32 NewCG = Cluster->CollisionGroup();
				const int32 ChildCG = ClusteredChild->CollisionGroup();
				Cluster->SetCollisionGroup(NewCG < ChildCG ? NewCG : ChildCG);
			}

			FPBDRigidParticleHandle* const* OldParent = ChildToParentMap.Find(Child);
			FPBDRigidParticleHandle* ProxyParticle = (OldParent != nullptr) ? *OldParent : Child;
			MEvolution.DoInternalParticleInitilization(ProxyParticle, Cluster);
		}

		if (Cluster->ClusterIds().NumChildren > 0)
		{
			Cluster->SetInternalStrains(Cluster->GetInternalStrains() / static_cast<FRealSingle>(Cluster->ClusterIds().NumChildren));
		}
	}

	int32 UnionsHaveCollisionParticles = 0;
	FAutoConsoleVariableRef CVarUnionsHaveCollisionParticles(TEXT("p.UnionsHaveCollisionParticles"), UnionsHaveCollisionParticles, TEXT(""));

	bool
	FRigidClustering::ShouldUnionsHaveCollisionParticles()
	{
		return !!UnionsHaveCollisionParticles;
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::CreateClusterParticleFromClusterChildren"), STAT_CreateClusterParticleFromClusterChildren, STATGROUP_Chaos);
	Chaos::FPBDRigidClusteredParticleHandle* 
	FRigidClustering::CreateClusterParticleFromClusterChildren(
		TArray<FPBDRigidParticleHandle*>&& Children, 
		FPBDRigidClusteredParticleHandle* Parent, 
		const FRigidTransform3& ClusterWorldTM, 
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_CreateClusterParticleFromClusterChildren);

		//This cluster is made up of children that are currently in a cluster. This means we don't need to update or disable as much
		Chaos::FPBDRigidClusteredParticleHandle* NewParticle = Parameters.ClusterParticleHandle;
		if (!NewParticle)
		{
			NewParticle = MEvolution.CreateClusteredParticles(1)[0]; // calls Evolution.DirtyParticle()
		}
		MEvolution.EnableParticle(NewParticle);

		NewParticle->SetCollisionGroup(INT_MAX);
		TopLevelClusterParents.Add(NewParticle);
		NewParticle->SetInternalCluster(true);
		NewParticle->SetClusterId(ClusterId(nullptr, Children.Num()));
		NewParticle->SetIsAnchored(false);
		for (auto& Constituent : Children) MEvolution.DoInternalParticleInitilization(Constituent, NewParticle);

		//
		// Update clustering data structures.
		//
		if (MChildren.Contains(NewParticle))
		{
			MChildren[NewParticle] = MoveTemp(Children);
		}
		else
		{
			MChildren.Add(NewParticle, MoveTemp(Children));
		}

		TArray<FPBDRigidParticleHandle*>& ChildrenArray = MChildren[NewParticle];
		//child transforms are out of date, need to update them. @todo(ocohen): if children transforms are relative we would not need to update this, but would simply have to do a final transform on the new cluster index
		// TODO(mlentine): Why is this not needed? (Why is it ok to have DeactivateClusterChildren==false?)
		if (DeactivateClusterChildren)
		{
			//TODO: avoid iteration just pass in a view
			TSet<FGeometryParticleHandle*> ChildrenHandles(static_cast<TArray<FGeometryParticleHandle*>>(ChildrenArray));
			MEvolution.DisableParticles(ChildrenHandles);
		}
		for (FPBDRigidParticleHandle* Child : ChildrenArray)
		{
			if (FPBDRigidClusteredParticleHandle* ClusteredChild = Child->CastToClustered())
			{
				FRigidTransform3 ChildFrame = ClusteredChild->ChildToParent() * ClusterWorldTM;
				ClusteredChild->SetX(ChildFrame.GetTranslation());
				ClusteredChild->SetR(ChildFrame.GetRotation());
				ClusteredChild->ClusterIds().Id = NewParticle;
				ClusteredChild->SetClusterGroupIndex(0);
				if (DeactivateClusterChildren)
				{
					TopLevelClusterParents.Remove(ClusteredChild);
					TopLevelClusterParentsStrained.Remove(ClusteredChild);
				}

				ClusteredChild->SetCollisionImpulses(FMath::Max(NewParticle->CollisionImpulses(), ClusteredChild->CollisionImpulses()));
				Child->SetCollisionGroup(FMath::Min(NewParticle->CollisionGroup(), Child->CollisionGroup()));
			}
		}

		FClusterCreationParameters NoCleanParams = Parameters;
		NoCleanParams.bCleanCollisionParticles = false;
		NoCleanParams.bCopyCollisionParticles = !!UnionsHaveCollisionParticles;

		TSet<FPBDRigidParticleHandle*> ChildrenSet(ChildrenArray);
		
		// TODO: This needs to be rotated to diagonal, used to update I()/InvI() from diagonal, and update transform with rotation.
		FMatrix33 ClusterInertia(0);
		UpdateClusterMassProperties(NewParticle, ChildrenSet, ClusterInertia, nullptr);
		UpdateKinematicProperties(NewParticle, MChildren, MEvolution);

		UpdateGeometry(NewParticle, ChildrenSet, MChildren, nullptr, NoCleanParams);

		return NewParticle;
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::UnionClusterGroups"), STAT_UnionClusterGroups, STATGROUP_Chaos);
	void 
	FRigidClustering::UnionClusterGroups()
	{
		SCOPE_CYCLE_COUNTER(STAT_UnionClusterGroups);
		ClusterUnionManager.FlushPendingOperations();
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::DeactivateClusterParticle"), STAT_DeactivateClusterParticle, STATGROUP_Chaos);
	TSet<FPBDRigidParticleHandle*> 
	FRigidClustering::DeactivateClusterParticle(
		FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		SCOPE_CYCLE_COUNTER(STAT_DeactivateClusterParticle);

		TSet<FPBDRigidParticleHandle*> ActivatedChildren;
		check(!ClusteredParticle->Disabled());
		if (MChildren.Contains(ClusteredParticle))
		{
			ActivatedChildren = ReleaseClusterParticles(MChildren[ClusteredParticle]);
		}
		return ActivatedChildren;
	}

	void FRigidClustering::ResetAllEvents()
	{
		ResetAllClusterBreakings();
		ResetAllClusterCrumblings();
		CrumbledSinceLastUpdate.Reset();
	}

	void FRigidClustering::TrackBreakingCollision(FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		if (auto Rigid = ClusteredParticle->CastToRigidParticle())
		{
			Rigid->ParticleCollisions().VisitCollisions([this, Rigid](FPBDCollisionConstraint& Collision)
			{
				// Get a generic handle for the "other" particle
				uint8 OtherIdx = Collision.GetParticle0() == Rigid ? 1 : 0;

				// Make sure this collision actually includes the clustered particle
				if (!ensure(Collision.GetParticle(1 - OtherIdx) == Rigid))
				{
					return ECollisionVisitorResult::Continue;
				}

				FGeometryParticleHandle* OtherGeometry = Collision.GetParticle(OtherIdx);
				if (OtherGeometry == nullptr)
				{
					return ECollisionVisitorResult::Continue;
				}

				FPBDRigidParticleHandle* OtherRigid = OtherGeometry->CastToRigidParticle();
				if (OtherRigid == nullptr)
				{
					return ECollisionVisitorResult::Continue;
				}

				if (Collision.AccumulatedImpulse.SizeSquared() <= SMALL_NUMBER)
				{
					return ECollisionVisitorResult::Continue;
				}

				// Track this collision
				BreakingCollisions.Add(TPair<FPBDCollisionConstraint*, FPBDRigidParticleHandle*>(&Collision, OtherRigid));

				return ECollisionVisitorResult::Continue;
			});
		}
	}

	void FRigidClustering::RestoreBreakingMomentum()
	{
		for (TPair<FPBDCollisionConstraint*, FPBDRigidParticleHandle*>& Pair : BreakingCollisions)
		{
			FPBDCollisionConstraint& Collision = *Pair.Key;
			FPBDRigidParticleHandle& Rigid = *Pair.Value;
			FConstGenericParticleHandle Generic(&Rigid);

			// Flip the impulse if we're restoring particle 0's momentum.
			// This is because by convention constraint impulses point from 1 to 0.
			uint8 OtherIdx = Collision.GetParticle0() == &Rigid ? 1 : 0;
			const FVec3 Impulse
				= OtherIdx == 0
				? Collision.AccumulatedImpulse
				: -Collision.AccumulatedImpulse;

			// Compute the angular impulse based on distance from the contact point to the CoM
			const FVec3 Location = Collision.CalculateWorldContactLocation();
			const Chaos::FVec3 AngularImpulse = Chaos::FVec3::CrossProduct(Location - Generic->PCom(), Impulse);

			// Compute impulse velocities
			const FVec3 ImpulseVelocity = Generic->InvM() * Impulse;

			const FMatrix33 OtherInvI = Utilities::ComputeWorldSpaceInertia(Generic->QCom(), Generic->ConditionedInvI());
			const FVec3 AngularImpulseVelocity = OtherInvI * AngularImpulse;

			// Update linear and angular impulses for the body, to be integrated next solve
			const float RestorationPercent = RestoreBreakingMomentumPercent;
			Rigid.V() += ImpulseVelocity * RestorationPercent;
			Rigid.W() += AngularImpulseVelocity * RestorationPercent;
		}
	}

	void FRigidClustering::SendBreakingEvent(FPBDRigidClusteredParticleHandle* ClusteredParticle, bool bFromCrumble)
	{
		// only emit break event if the proxy needs it 
		if (FGeometryCollectionPhysicsProxy* ConcreteProxy = GetConcreteProxy(ClusteredParticle))
		{
			const FSimulationParameters& SimParams = ConcreteProxy->GetSimParameters();
			if (SimParams.bGenerateBreakingData)
			{
				FBreakingData& ClusterBreak = MAllClusterBreakings.AddDefaulted_GetRef();
				ClusterBreak.Proxy = ClusteredParticle->PhysicsProxy();
				ClusterBreak.Location = ClusteredParticle->X();
				ClusterBreak.Velocity = ClusteredParticle->V();
				ClusterBreak.AngularVelocity = ClusteredParticle->W();
				ClusterBreak.Mass = ClusteredParticle->M();
				if (ClusteredParticle->Geometry() && ClusteredParticle->Geometry()->HasBoundingBox())
				{
					ClusterBreak.BoundingBox = ClusteredParticle->Geometry()->BoundingBox();
				}
				ClusterBreak.TransformGroupIndex = ConcreteProxy->GetTransformGroupIndexFromHandle(ClusteredParticle);
				ClusterBreak.bFromCrumble = bFromCrumble;
			}
		}
	}

	
	void FRigidClustering::SendCrumblingEvent(FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		// only emit crumble events if the proxy needs it 
		if (FGeometryCollectionPhysicsProxy* ConcreteProxy = GetConcreteProxy(ClusteredParticle))
		{
			const FSimulationParameters& SimParams = ConcreteProxy->GetSimParameters(); 
			if (SimParams.bGenerateCrumblingData)
			{
				FCrumblingData& ClusterCrumbling = MAllClusterCrumblings.AddDefaulted_GetRef();
				ClusterCrumbling.Proxy = ClusteredParticle->PhysicsProxy();
				ClusterCrumbling.Location = ClusteredParticle->X();
				ClusterCrumbling.Orientation = ClusteredParticle->R();
				ClusterCrumbling.LinearVelocity = ClusteredParticle->V();
				ClusterCrumbling.AngularVelocity = ClusteredParticle->W();
				ClusterCrumbling.Mass = ClusteredParticle->M();
				if (ClusteredParticle->Geometry() && ClusteredParticle->Geometry()->HasBoundingBox())
				{
					ClusterCrumbling.LocalBounds = ClusteredParticle->Geometry()->BoundingBox();
				}
				if (SimParams.bGenerateCrumblingChildrenData)
				{
					// when sending this event, children are still attached
					if (const FRigidHandleArray* Children = MChildren.Find(ClusteredParticle))
					{
						ConcreteProxy->GetTransformGroupIndicesFromHandles(*Children, ClusterCrumbling.Children);
					}
				}
			}
		}
	}

	TArray<FRigidClustering::FParticleIsland> FRigidClustering::FindIslandsInChildren(const FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		const TArray<FPBDRigidParticleHandle*>& Children = MChildren[ClusteredParticle];
		
		TArray<FParticleIsland> Islands;

		// traverse connectivity and see how many connected pieces we have
		TSet<FPBDRigidParticleHandle*> ProcessedChildren;
		ProcessedChildren.Reserve(Children.Num());

		for (FPBDRigidParticleHandle* Child : Children)
		{
			if (ProcessedChildren.Contains(Child))
			{
				continue;
			}
			TArray<FPBDRigidParticleHandle*>& Island = Islands.AddDefaulted_GetRef();

			TArray<FPBDRigidParticleHandle*> ProcessingQueue;
			ProcessingQueue.Add(Child);
			while (ProcessingQueue.Num())
			{
				FPBDRigidParticleHandle* ChildToProcess = ProcessingQueue.Pop();
				if (!ProcessedChildren.Contains(ChildToProcess))
				{
					ProcessedChildren.Add(ChildToProcess);
					Island.Add(ChildToProcess);
					for (const TConnectivityEdge<FReal>& Edge : ChildToProcess->CastToClustered()->ConnectivityEdges())
					{
						if (!ProcessedChildren.Contains(Edge.Sibling))
						{
							ProcessingQueue.Add(Edge.Sibling);
						}
					}
				}
			}
		}

		return Islands;
	}

	void FRigidClustering::RemoveChildFromParent(FPBDRigidParticleHandle* Child, const FPBDRigidClusteredParticleHandle* ClusteredParent)
	{
		if (ensure(Child != nullptr && ClusteredParent != nullptr))
		{
			FPBDRigidClusteredParticleHandle* ClusteredChild = Child->CastToClustered();

			MEvolution.EnableParticle(Child);
			TopLevelClusterParents.Add(ClusteredChild);

			ClusteredChild->SetClusterId(ClusterId(nullptr, ClusteredChild->ClusterIds().NumChildren)); // clear Id but retain number of children

			const FRigidTransform3 PreSolveTM(ClusteredParent->P(), ClusteredParent->Q());
			const FRigidTransform3 ChildFrame = ClusteredChild->ChildToParent() * PreSolveTM;
			Child->SetX(ChildFrame.GetTranslation());
			Child->SetR(ChildFrame.GetRotation());

			Child->SetP(Child->X());
			Child->SetQ(Child->R());

			//todo(ocohen): for now just inherit velocity at new COM. This isn't quite right for rotation
			//todo(ocohen): in the presence of collisions, this will leave all children with the post-collision
			// velocity. This should be controlled by material properties so we can allow the broken pieces to
			// maintain the clusters pre-collision velocity.
			Child->SetV(Child->V() + ClusteredParent->V());
			Child->SetW(Child->W() + ClusteredParent->W());
			Child->SetPreV(Child->PreV() + ClusteredParent->PreV());
			Child->SetPreW(Child->PreW() + ClusteredParent->PreW());
		}
	};

	TArray<FPBDRigidParticleHandle*> FRigidClustering::CreateClustersFromNewIslands(
		TArray<FParticleIsland>& Islands,
		FPBDRigidClusteredParticleHandle* ClusteredParent
		)
	{
		TArray<FPBDRigidParticleHandle*> NewClusters;
		
		// only for island with more than one particle
		int32 NumNewClusters = 0;
		for (const TArray<FPBDRigidParticleHandle*>& Island : Islands)
		{
			if (Island.Num() > 1)
			{
				NumNewClusters++;
			}
		}
		NewClusters.Reserve(NumNewClusters);

		const FRigidTransform3 PreSolveTM = FRigidTransform3(ClusteredParent->P(), ClusteredParent->Q());
		
		TArray<Chaos::FPBDRigidClusteredParticleHandle*> NewClusterHandles = MEvolution.CreateClusteredParticles(NumNewClusters);
		int32 ClusterHandlesIdx = 0;
		for (TArray<FPBDRigidParticleHandle*>& Island : Islands)
		{
			if (Island.Num() > 1) //now build the remaining pieces
			{
				FClusterCreationParameters CreationParameters;
				CreationParameters.ClusterParticleHandle = NewClusterHandles[ClusterHandlesIdx++];
				Chaos::FPBDRigidClusteredParticleHandle* NewCluster = 
					CreateClusterParticleFromClusterChildren(
						MoveTemp(Island), 
						ClusteredParent, 
						PreSolveTM, 
						CreationParameters);

				MEvolution.SetPhysicsMaterial(NewCluster, MEvolution.GetPhysicsMaterial(ClusteredParent));

				NewCluster->SetInternalStrains(ClusteredParent->GetInternalStrains());
				NewCluster->SetV(ClusteredParent->V());
				NewCluster->SetW(ClusteredParent->W());
				NewCluster->SetPreV(ClusteredParent->PreV());
				NewCluster->SetPreW(ClusteredParent->PreW());
				NewCluster->SetP(NewCluster->X());
				NewCluster->SetQ(NewCluster->R());

				UpdateTopLevelParticle(NewCluster);

				// Need to get the material from the previous particle and apply it to the new one
				const FShapesArray& ChildShapes = ClusteredParent->ShapesArray();
				const FShapesArray& NewShapes = NewCluster->ShapesArray();
				const int32 NumChildShapes = ClusteredParent->ShapesArray().Num();

				if(NumChildShapes > 0)
				{
					// Can only take materials if the child has any - otherwise we fall back on defaults.
					// Due to GC initialisation however, we should always have a valid material as even
					// when one cannot be found we fall back on the default on GEngine
					const int32 NumChildMaterials = ChildShapes[0]->NumMaterials();
					if(NumChildMaterials > 0)
					{
						Chaos::FMaterialHandle ChildMat = ChildShapes[0]->GetMaterial(0);

						for(const TUniquePtr<FPerShapeData>& PerShape : NewShapes)
						{
							PerShape->SetMaterial(ChildMat);
						}
					}
				}
				NewClusters.Add(NewCluster);
			}
		}
		return NewClusters;
	}

	void FRigidClustering::SetInternalStrain(FPBDRigidClusteredParticleHandle* Particle, FRealSingle Strain)
	{
		Particle->SetInternalStrains(Strain);
		UpdateTopLevelParticle(Particle);
	}

	void FRigidClustering::SetExternalStrain(FPBDRigidClusteredParticleHandle* Particle, FRealSingle Strain)
	{
		Particle->SetExternalStrains(Strain);
		UpdateTopLevelParticle(Particle);
	}

	void FRigidClustering::UpdateTopLevelParticle(FPBDRigidClusteredParticleHandle* Particle)
	{
		FPBDRigidClusteredParticleHandle* Parent = Particle->Parent();
		if (Parent != nullptr)
		{
			TopLevelClusterParentsStrained.Add(Parent);
		}
		else
		{
			TopLevelClusterParentsStrained.Add(Particle);
		}
	}
	
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::ReleaseClusterParticles(STRAIN)"), STAT_ReleaseClusterParticles_STRAIN, STATGROUP_Chaos);
	TSet<FPBDRigidParticleHandle*> FRigidClustering::ReleaseClusterParticles(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		bool bForceRelease)
	{
		SCOPE_CYCLE_COUNTER(STAT_ReleaseClusterParticles_STRAIN);

		if (FPBDRigidClusteredParticleHandle* Parent = ClusteredParticle->Parent())
		{
			// Having a parent is only OK if the parent is a cluster union since ReleaseClusterParticlesImpl will
			// cause it to be ejected from the cluster union.
			if (!ensureMsgf((ClusterUnionManager.FindClusterUnionIndexFromParticle(Parent) != INDEX_NONE), TEXT("Removing a cluster that still has a non-cluster union parent")))
			{
				TSet<FPBDRigidParticleHandle*> EmptySet;
				return EmptySet;
			}
		}

		return ReleaseClusterParticlesImpl(ClusteredParticle, bForceRelease, true /*bCreateNewClusters*/);
	}
	
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::ReleaseClusterParticlesNoInternalCluster"), STAT_ReleaseClusterParticlesNoInternalCluster, STATGROUP_Chaos);
	TSet<FPBDRigidParticleHandle*> FRigidClustering::ReleaseClusterParticlesNoInternalCluster(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		bool bForceRelease)
	{
		/* This is a near duplicate of the ReleaseClusterParticles() method with the internal cluster creation removed.
		*  This method should be used exclusively by the GeometryCollectionComponentCacheAdaptor in order to implement
		*  correct behavior when cluster grouping is used. 
		*/
		
		SCOPE_CYCLE_COUNTER(STAT_ReleaseClusterParticlesNoInternalCluster);

		return ReleaseClusterParticlesImpl(ClusteredParticle, bForceRelease, false /*bCreateNewClusters*/);
	}
	
	TSet<FPBDRigidParticleHandle*> FRigidClustering::ReleaseClusterParticlesImpl(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		bool bForceRelease,
		bool bCreateNewClusters)
	{	
		TSet<FPBDRigidParticleHandle*> ActivatedChildren;

		if (ClusteredParticle && ClusteredParticle->Unbreakable())
		{
			return ActivatedChildren;
		}

		if (!ensureMsgf(MChildren.Contains(ClusteredParticle), TEXT("Removing Cluster that does not exist!")))
		{
			return ActivatedChildren;
		}

		// gather propagation information from the parent proxy
		bool bUseDamagePropagation = false;
		float BreakDamagePropagationFactor = 0.0f;
		float ShockDamagePropagationFactor = 0.0f;
		if (const FGeometryCollectionPhysicsProxy* ConcreteProxy = GetConcreteProxy(ClusteredParticle))
		{
			const FSimulationParameters& SimParams = ConcreteProxy->GetSimParameters();
			bUseDamagePropagation = SimParams.bUseDamagePropagation;
			BreakDamagePropagationFactor = SimParams.BreakDamagePropagationFactor;
			ShockDamagePropagationFactor = SimParams.ShockDamagePropagationFactor;
		}

		TArray<FPBDRigidParticleHandle*>& Children = MChildren[ClusteredParticle];
		const bool bParentCrumbled = CrumbledSinceLastUpdate.Contains(ClusteredParticle);

		bool bFoundFirstRelease = false;

		// only used for propagation
		TMap<FPBDRigidParticleHandle*, FRealSingle> AppliedStrains;

		// We'll pass these particles to the cluster union manager to remove. This can't be done within the same loop
		// since it'll be modifying the children array.
		TArray<FPBDRigidParticleHandle*> DeferredRemoveFromClusterUnion;

		// Grab cluster union parent if there is one
		FPBDRigidParticleHandle* ParentRigid = ClusteredParticle->ClusterIds().Id;
		FPBDRigidClusteredParticleHandle* Parent = ParentRigid ? ParentRigid->CastToClustered() : nullptr;

		for (int32 ChildIdx = Children.Num() - 1; ChildIdx >= 0; --ChildIdx)
		{
			FPBDRigidClusteredParticleHandle* Child = Children[ChildIdx]->CastToClustered();
			
			if (!Child)
			{
				continue;
			}

			// @todo(chaos) eventually should get rid of collision impulse array and only use external strain
			const FRealSingle MaxAppliedStrain = FMath::Max(Child->CollisionImpulses(), Child->GetExternalStrain());
			if ((MaxAppliedStrain >= Child->GetInternalStrains()) || bForceRelease)
			{
				if (!bFoundFirstRelease)
				{
					// Restore some of the momentum of whatever collided with the parent
					// NOTE: This has to come before HandleRemoveOperationWithClusterLookup, because
					// in FClusterUnionManager::UpdateAllClusterUnionProperties, the particle is
					// invalidated with MEvolution.InvalidateParticle, which clears its contacts
					if (RestoreBreakingMomentumPercent > 0.f)
					{
						if (Parent)
						{
							TrackBreakingCollision(Parent);
						}
						else
						{
							TrackBreakingCollision(ClusteredParticle);
						}
					}

					ClusterUnionManager.HandleRemoveOperationWithClusterLookup({ ClusteredParticle }, EClusterUnionOperationTiming::Defer);
					bFoundFirstRelease = true;
				}

				// There's a possibility that the child is in a cluster union so we'd need to be able to remove the child particle from the cluster union as well.
				const FClusterUnionIndex ClusterUnionIndex = ClusterUnionManager.FindClusterUnionIndexFromParticle(Child);
				const bool bIsInClusterUnion = ClusterUnionIndex != INDEX_NONE;

				//UE_LOG(LogTemp, Warning, TEXT("Releasing child %d from parent %p due to strain %.5f Exceeding internal strain %.5f (Source: %s)"), ChildIdx, ClusteredParticle, ChildStrain, Child->Strain(), bForceRelease ? TEXT("Forced by caller") : ExternalStrainMap ? TEXT("External") : TEXT("Collision"));
				if (bIsInClusterUnion)
				{
					DeferredRemoveFromClusterUnion.Add(Child);
				}
				else
				{
					// The piece that hits just breaks off - we may want more control 
					// by looking at the edges of this piece which would give us cleaner 
					// breaks (this approach produces more rubble)
					RemoveChildFromParent(Child, ClusteredParticle);
					UpdateTopLevelParticle(Child);

					// Remove from the children array without freeing memory yet. 
					// We're looping over Children and it'd be silly to free the array
					// 1 entry at a time.
					Children.RemoveAtSwap(ChildIdx, 1, false);
				}
				
				ActivatedChildren.Add(Child);
				SendBreakingEvent(Child, bParentCrumbled);
			}
			if (bUseDamagePropagation)
			{
				AppliedStrains.Add(Child, MaxAppliedStrain);
			}
			Child->SetExternalStrains(0.0);
		}

		if (!DeferredRemoveFromClusterUnion.IsEmpty())
		{
			ClusterUnionManager.HandleRemoveOperationWithClusterLookup(DeferredRemoveFromClusterUnion, EClusterUnionOperationTiming::Defer);
		}

		// if necessary propagate strain through the graph
		// IMPORTANT: this assumes that the connectivity graph has not yet been updated from pieces that broke off
		if (bUseDamagePropagation)
		{
			for (const auto& AppliedStrain: AppliedStrains)
			{
				FPBDRigidClusteredParticleHandle* ClusteredChild = AppliedStrain.Key->CastToClustered();

				const FRealSingle AppliedStrainValue = AppliedStrain.Value;
				FRealSingle PropagatedStrainPerConnection = 0.0f;

				// @todo(chaos) : may not be optimal, but good enough for now
				if (BreakDamagePropagationFactor > 0 && ActivatedChildren.Contains(AppliedStrain.Key))
				{
					// break damage propagation case: we only look at the broken pieces and propagate the strain remainder 
					const FRealSingle RemainingStrain = (AppliedStrainValue - ClusteredChild->GetInternalStrains());
					if (RemainingStrain > 0)
					{
						const FRealSingle AdjustedRemainingStrain = BreakDamagePropagationFactor * RemainingStrain;
						// todo(chaos) : could do better and have something weighted on distance with a falloff maybe ?  
						PropagatedStrainPerConnection = AdjustedRemainingStrain / static_cast<FRealSingle>(ClusteredChild->ConnectivityEdges().Num());
					}
				}
				else if (ShockDamagePropagationFactor > 0)
				{
					// shock damage propagation case : for all the non broken pieces, propagate the actual applied strain 
					PropagatedStrainPerConnection = ShockDamagePropagationFactor * AppliedStrainValue;
				}

				if (PropagatedStrainPerConnection > 0)
				{
					for (const TConnectivityEdge<FReal>& Edge : ClusteredChild->ConnectivityEdges())
					{
						if (Edge.Sibling)
						{
							if (FPBDRigidClusteredParticleHandle* ClusteredSibling = Edge.Sibling->CastToClustered())
							{
								// todo(chaos) this may currently be non optimal as we are in the apply loop and this may be cleared right after
								SetExternalStrain(ClusteredSibling, FMath::Max(ClusteredSibling->GetExternalStrain(), PropagatedStrainPerConnection));
							}
						}
					}
				}
			}
		}

		if (ActivatedChildren.Num() > 0)
		{
			const bool bIsClusterUnion = ClusterUnionManager.IsClusterUnionParticle(ClusteredParticle);
			if (Children.Num() == 0)
			{
				// Free the memory if we can do so cheaply (no data copies).
				Children.Empty(); 
			}

			if (UseConnectivity)
			{
				// The cluster may have contained forests, so find the connected pieces and cluster them together.

				//first update the connected graph of the children we already removed
				for (FPBDRigidParticleHandle* Child : ActivatedChildren)
				{
					RemoveNodeConnections(Child);
				}

				// If we're breaking a geometry collection, we'll need to create internal clusters to parent the remaining particles.
				// However, we do not need to do this if we're currently operating on a cluster union! Its remaining particles should stay
				// attached to the cluster union because they can handle particles being dynamically added/removed.
				if (Children.Num() && !bIsClusterUnion)
				{
					TArray<FParticleIsland> Islands = FindIslandsInChildren(ClusteredParticle);
					for (const FParticleIsland& Island: Islands)
					{
						if (Island.Num() == 1) //need to break single pieces first
						{
							FPBDRigidParticleHandle* Child = Island[0];
							RemoveChildFromParent(Child, ClusteredParticle);
							ActivatedChildren.Add(Child);
						}
					}

					if (bCreateNewClusters)
					{
						TArray<FPBDRigidParticleHandle*> NewClusters = CreateClustersFromNewIslands(Islands, ClusteredParticle); 
						ActivatedChildren.Append(MoveTemp(NewClusters));
					}
				}
			}

			for (FPBDRigidParticleHandle* Child : ActivatedChildren)
			{
				UpdateKinematicProperties(Child, MChildren, MEvolution);
			}

			// Disable the cluster only if we're not a cluster union. Cluster unions will handle themselves separately.
			if (!bIsClusterUnion)
			{
				DisableCluster(ClusteredParticle);
			}
		}

		return ActivatedChildren;
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::ReleaseClusterParticles(LIST)"), STAT_ReleaseClusterParticles_LIST, STATGROUP_Chaos);
	TSet<FPBDRigidParticleHandle*> 
	FRigidClustering::ReleaseClusterParticles(
		TArray<FPBDRigidParticleHandle*> ChildrenParticles, bool bTriggerBreakEvents /* = false */)
	{
		SCOPE_CYCLE_COUNTER(STAT_ReleaseClusterParticles_LIST);
		TSet<FPBDRigidParticleHandle*> ActivatedBodies;
		if (ChildrenParticles.Num())
		{
			//for now just assume these all belong to same cluster
			FPBDRigidParticleHandle* ClusterHandle = nullptr;
			
			TMap<FGeometryParticleHandle*, FReal> FakeStrain;

			bool bPreDoGenerateData = DoGenerateBreakingData;
			DoGenerateBreakingData = bTriggerBreakEvents;

			for (FPBDRigidParticleHandle* ChildHandle : ChildrenParticles)
			{
				if (FPBDRigidClusteredParticleHandle* ClusteredChildHandle = ChildHandle->CastToClustered())
				{
					if (ClusteredChildHandle->Disabled() && ClusteredChildHandle->ClusterIds().Id != nullptr)
					{
						if (ensure(!ClusterHandle || ClusteredChildHandle->ClusterIds().Id == ClusterHandle))
						{
							SetExternalStrain(ClusteredChildHandle, TNumericLimits<FRealSingle>::Max());
							ClusterHandle = ClusteredChildHandle->ClusterIds().Id;
						}
						else
						{
							break; //shouldn't be here
						}
					}
				}
			}
			if (ClusterHandle)
			{
				ActivatedBodies = ReleaseClusterParticles(ClusterHandle->CastToClustered());
			}
			DoGenerateBreakingData = bPreDoGenerateData;
		}
		return ActivatedBodies;
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::ReleaseChildrenParticleAndParents"), ReleaseChildrenParticleAndParents, STATGROUP_Chaos);
	void FRigidClustering::ForceReleaseChildParticleAndParents(FPBDRigidClusteredParticleHandle* ChildClusteredParticle, bool bTriggerBreakEvents)
	{
		SCOPE_CYCLE_COUNTER(ReleaseChildrenParticleAndParents);

		if (ChildClusteredParticle)
		{
			// make sure we set unbreakable to false so that the children can be released
			ChildClusteredParticle->SetUnbreakable(false);
			if (ChildClusteredParticle->Disabled())
			{
				// first release any parent if any
				if (FPBDRigidClusteredParticleHandle* ParentCluster = ChildClusteredParticle->Parent())
				{
					// we need now to force parents to break
					ForceReleaseChildParticleAndParents(ParentCluster, bTriggerBreakEvents);

					SetExternalStrain(ChildClusteredParticle, TNumericLimits<FRealSingle>::Max());
					ReleaseClusterParticles(ParentCluster, bTriggerBreakEvents);
				}
			}
		}
	}

	static int32 GClusterBreakOnlyStrained = 1;
	FAutoConsoleVariableRef CVarBreakMode(TEXT("p.chaos.clustering.breakonlystrained"), GClusterBreakOnlyStrained, 
										  TEXT("If enabled we only process strained clusters for breaks, if disabled all clusters are traversed and checked"));

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::AdvanceClustering"), STAT_AdvanceClustering, STATGROUP_Chaos);
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::Update Impulse from Strain"), STAT_UpdateImpulseStrain, STATGROUP_Chaos);
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::Update Dirty Impulses"), STAT_UpdateDirtyImpulses, STATGROUP_Chaos);
	void 
	FRigidClustering::AdvanceClustering(
		const FReal Dt, 
		FPBDCollisionConstraints& CollisionRule)
	{
		SCOPE_CYCLE_COUNTER(STAT_AdvanceClustering);
		UE_LOG(LogChaos, Verbose, TEXT("START FRAME with Dt %f"), Dt);

		double FrameTime = 0, Time = 0;
		FDurationTimer Timer(Time);
		Timer.Start();

		if(MChildren.Num())
		{
			//
			//  Grab collision impulses for processing
			//
			if (ComputeClusterCollisionStrains)
			{
				ComputeStrainFromCollision(CollisionRule);
			}
			else
			{
				ResetCollisionImpulseArray();
			}

			//
			// Modify internal strains
			//
			if (StrainModifiers)
			{
				ApplyStrainModifiers();
			}

			//  Monitor the MStrain array for 0 or less values.
			//  That will trigger a break too.
			//
			bool bPotentialBreak = false;
			TArray<FPBDRigidClusteredParticleHandle*> ParticlesToProcess;

			auto ProcessClusteredParticle = [&ParticlesToProcess, &bPotentialBreak, this](FPBDRigidClusteredParticleHandle* Particle)
			{
				TArray<FRigidHandle>& ParentToChildren = MChildren[Particle];

				bool bAddParent = false;
				for(FRigidHandle Child : ParentToChildren)
				{
					if(FClusterHandle ClusteredChild = Child->CastToClustered())
					{
						if(ClusteredChild->GetInternalStrains() <= 0.f)
						{
							bAddParent = true;
							// #TODO remove need to set this here so we can early out as soon as we
							// find one child that requires processing for breaks
							ClusteredChild->CollisionImpulse() = FLT_MAX;
							MCollisionImpulseArrayDirty = true;
						}
						else if(ClusteredChild->GetExternalStrain() > 0 || ClusteredChild->CollisionImpulse() > 0)
						{
							bAddParent = true;
							bPotentialBreak = true;
						}
					}
				}

				// Ensure we only add the parent once.
				if(bAddParent)
				{
					ParticlesToProcess.Add(Particle);
				}
			};

			{
				SCOPE_CYCLE_COUNTER(STAT_UpdateDirtyImpulses);
				for (const auto& ActiveCluster : TopLevelClusterParentsStrained)
				{
					FClusterUnion* ClusterUnion = ClusterUnionManager.FindClusterUnionFromParticle(ActiveCluster);
					if (ClusterUnion && ClusterUnion->InternalCluster != ActiveCluster)
					{
						// Need to pre-emptively remove the particle from the cluster union otherwise we won't be passing the disabled check.
						ClusterUnionManager.HandleRemoveOperationWithClusterLookup({ ActiveCluster }, EClusterUnionOperationTiming::Defer);
					}

					if (!ActiveCluster->Disabled())
					{
						if (ActiveCluster->ClusterIds().NumChildren > 0) //active index is a cluster
						{
							if(ClusterUnion)
							{
								if(ClusterUnion->InternalCluster == ActiveCluster)
								{
									// ActiveCluster is itself a cluster union, so loop over its children and add those
									// to process for breaking.
									for(FPBDRigidParticleHandle* ChildParticle : ClusterUnion->ChildParticles)
									{
										if(ChildParticle)
										{
											if(FPBDRigidClusteredParticleHandle* ClusteredChild = ChildParticle->CastToClustered())
											{
												if(ClusteredChild->ClusterIds().NumChildren > 0)
												{
													ProcessClusteredParticle(ClusteredChild);
												}
											}
										}
									}
								}
								else
								{
									// Clustered is inside a clustered union, but not a clustered union itself
									ProcessClusteredParticle(ActiveCluster);
								}
							}
							else
							{
								ProcessClusteredParticle(ActiveCluster);
							}
						}
					}
				}
			}

			ClusterUnionManager.HandleDeferredClusterUnionUpdateProperties();

			// Breaking can populate this again with relevant children - so we clear before running the breaking model
			TopLevelClusterParentsStrained.Reset();

			if (MCollisionImpulseArrayDirty || bPotentialBreak)
			{
				SCOPE_CYCLE_COUNTER(STAT_UpdateDirtyImpulses);

				// Call our breaking model
				// #TODO convert to visitor pattern to avoid TArray allocations above.
				if(GClusterBreakOnlyStrained == 1)
				{
					BreakingModel(ParticlesToProcess);
				}
				else
				{
					BreakingModel();
				}
			} // end if MCollisionImpulseArrayDirty
		}
		Timer.Stop();
		UE_LOG(LogChaos, Verbose, TEXT("Cluster Break Update Time is %f"), Time);
	}

	DECLARE_CYCLE_STAT(TEXT("BreakingModel_AllParticles"), STAT_BreakingModel_AllParticles, STATGROUP_Chaos);
	DECLARE_CYCLE_STAT(TEXT("BreakingModel"), STAT_BreakingModel, STATGROUP_Chaos);
	void FRigidClustering::BreakingModel()
	{
		SCOPE_CYCLE_COUNTER(STAT_BreakingModel_AllParticles);
		
		// Clear the set tracking breaking collisions
		BreakingCollisions.Empty();

		//make copy because release cluster modifies active indices. We want to iterate over original active indices
		TArray<FPBDRigidClusteredParticleHandle*> ClusteredParticlesToProcess;
		for(FTransientPBDRigidParticleHandle& Particle : MEvolution.GetNonDisabledClusteredView())
		{
			if (FPBDRigidClusteredParticleHandle* Clustered = Particle.Handle()->CastToClustered())
			{
				if (Clustered->ClusterIds().NumChildren > 0)
				{
					if (FClusterUnion* ClusterUnion = ClusterUnionManager.FindClusterUnionFromParticle(Clustered))
					{
						if (ClusterUnion->InternalCluster == Clustered)
						{
							// Clustered is itself a cluster union, so loop over its children and add those
							// to process for breaking.
							for (FPBDRigidParticleHandle* ChildParticle : ClusterUnion->ChildParticles)
							{
								if (ChildParticle)
								{
									if (FPBDRigidClusteredParticleHandle* ClusteredChild = ChildParticle->CastToClustered())
									{
										if (ClusteredChild->ClusterIds().NumChildren > 0)
										{
											ClusteredParticlesToProcess.Add(ClusteredChild);
										}
									}
								}
							}
						}
						else
						{
							// Clustered is inside a clustered union, but not a clustered union itself
							ClusteredParticlesToProcess.Add(Clustered);
						}
					}
					else
					{
						// Clustered is not a clustered union, and not _in_ a clustered union
						ClusteredParticlesToProcess.Add(Clustered);
					}
				}
			}
		}

		BreakingModel(ClusteredParticlesToProcess);
	}
	
	void FRigidClustering::BreakingModel(TArray<FPBDRigidClusteredParticleHandle*>& InParticles)
	{
		SCOPE_CYCLE_COUNTER(STAT_BreakingModel);

		// Clear the set tracking breaking collisions
		BreakingCollisions.Empty();

		for(FPBDRigidClusteredParticleHandle* ClusteredParticle : InParticles)
		{
			if(ClusteredParticle->ClusterIds().NumChildren)
			{
				ReleaseClusterParticles(ClusteredParticle);
			}
		}

		// This way if we break apart a large cluster union here (i.e. many of its children want to be released from ReleaseClusterParticles due to strain)
		// we'll only update the cluster properties once here (connection graph, geometry, etc.).
		ClusterUnionManager.HandleDeferredClusterUnionUpdateProperties();
		// Restore some of the momentum of objects that were touching rigid clusters that broke
		if (RestoreBreakingMomentumPercent > 0.f)
		{
			RestoreBreakingMomentum();
		}
	}

	DECLARE_CYCLE_STAT(TEXT("FRigidClustering::Visitor"), STAT_ClusterVisitor, STATGROUP_Chaos);
	void FRigidClustering::Visitor(FClusterHandle Cluster, FVisitorFunction Function)
	{
		if (Cluster)
		{
			if (MChildren.Contains(Cluster) && MChildren[Cluster].Num())
			{
				SCOPE_CYCLE_COUNTER(STAT_ClusterVisitor);

				// TQueue is a linked list, which has no preallocator.
				TQueue<FRigidHandle> Queue;
				for (Chaos::FPBDRigidParticleHandle* Child : MChildren[Cluster])
				{
					Queue.Enqueue(Child);
				}

				FRigidHandle CurrentHandle = nullptr;
				while (Queue.Dequeue(CurrentHandle))
				{
					if (CurrentHandle)
					{
						if (FClusterHandle CurrentClusterHandle = CurrentHandle->CastToClustered())
						{
							// @question : Maybe we should just store the leaf node bodies in a
							// map, that will require Memory(n*log(n))
							if (MChildren.Contains(CurrentClusterHandle))
							{
								for (Chaos::FPBDRigidParticleHandle* Child : MChildren[CurrentClusterHandle])
								{
									Queue.Enqueue(Child);
								}
							}
						}
						if (CurrentHandle)
						{
							Function(*this, CurrentHandle);
						}
					}
				}
			}
		}
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::GetActiveClusterIndex"), STAT_GetActiveClusterIndex, STATGROUP_Chaos);
	FPBDRigidParticleHandle* 
	FRigidClustering::GetActiveClusterIndex(
		FPBDRigidParticleHandle* Child)
	{
		SCOPE_CYCLE_COUNTER(STAT_GetActiveClusterIndex);
		while (Child && Child->Disabled())
		{
			Child = Child->CastToClustered()->ClusterIds().Id;
		}
		return Child; 
	}

	FPBDRigidParticleHandle* FRigidClustering::FindClosestChild(const FPBDRigidClusteredParticleHandle* ClusteredParticle, const FVec3& WorldLocation) const
	{
		if (const TArray<FPBDRigidParticleHandle*>* ChildrenHandles = GetChildrenMap().Find(ClusteredParticle))
		{
			return FindClosestParticle(*ChildrenHandles, WorldLocation); 
		}
		return nullptr;
	}

	FPBDRigidParticleHandle* FRigidClustering::FindClosestParticle(const TArray<FPBDRigidParticleHandle*>& Particles, const FVec3& WorldLocation)
	{
		FPBDRigidParticleHandle* ClosestChildHandle = nullptr;
		
		// @todo(chaos) we should offer a more precise way to query than the distance from center of mass
		FReal ClosestSquaredDist = TNumericLimits<FReal>::Max();
		for (FPBDRigidParticleHandle* ChildHandle: Particles)
        {
        	const FReal SquaredDist = (ChildHandle->X() - WorldLocation).SizeSquared();
        	if (SquaredDist < ClosestSquaredDist)
        	{
        		ClosestSquaredDist = SquaredDist;
        		ClosestChildHandle = ChildHandle;
        	}
        }
        return ClosestChildHandle;
	}

	TArray<FPBDRigidParticleHandle*> FRigidClustering::FindChildrenWithinRadius(const FPBDRigidClusteredParticleHandle* ClusteredParticle, const FVec3& WorldLocation, FReal Radius, bool bAlwaysReturnClosest) const
	{
		if (const TArray<FPBDRigidParticleHandle*>* ChildrenHandles = GetChildrenMap().Find(ClusteredParticle))
		{
			return FindParticlesWithinRadius(*ChildrenHandles, WorldLocation, Radius, bAlwaysReturnClosest); 
		}
		TArray<FPBDRigidParticleHandle*> EmptyArray;
		return EmptyArray;
	}

	TArray<FPBDRigidParticleHandle*> FRigidClustering::FindParticlesWithinRadius(const TArray<FPBDRigidParticleHandle*>& Particles, const FVec3& WorldLocation, FReal Radius, bool bAlwaysReturnClosest)
	{
		TArray<FPBDRigidParticleHandle*> Result;
		
		FPBDRigidParticleHandle* ClosestChildHandle = nullptr;
		
		// @todo(chaos) we should offer a more precise way to query than the distance from center of mass
		FReal ClosestSquaredDist = TNumericLimits<FReal>::Max();
		
		const FReal RadiusSquared = Radius * Radius;
		for (FPBDRigidParticleHandle* ChildHandle: Particles)
		{
			const FReal SquaredDist = (ChildHandle->X() - WorldLocation).SizeSquared();
			if (SquaredDist <= RadiusSquared)
			{
				Result.Add(ChildHandle);
			}
			if (bAlwaysReturnClosest)
			{
				if (SquaredDist < ClosestSquaredDist)
				{
					ClosestSquaredDist = SquaredDist;
					ClosestChildHandle = ChildHandle;
				}
			}
		}
		if (bAlwaysReturnClosest && ClosestChildHandle && Result.Num() == 0)
		{
			Result.Add(ClosestChildHandle);
		}
		return Result;
	}
	
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::GenerateConnectionGraph"), STAT_GenerateConnectionGraph, STATGROUP_Chaos);
	void 
	FRigidClustering::GenerateConnectionGraph(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_GenerateConnectionGraph);
		if (!MChildren.Contains(Parent)) 
			return;

		// Connectivity Graph
		//    Build a connectivity graph for the cluster. If the PointImplicit is specified
		//    and the ClusterIndex has collision particles then use the expensive connection
		//    method. Otherwise try the DelaunayTriangulation if not none.
		//
		if (Parameters.bGenerateConnectionGraph)
		{
			FClusterCreationParameters::EConnectionMethod LocalConnectionMethod = Parameters.ConnectionMethod;

			if (LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::None ||
				(LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::PointImplicit && 
				 !Parent->CollisionParticles()))
			{
				LocalConnectionMethod = FClusterCreationParameters::EConnectionMethod::MinimalSpanningSubsetDelaunayTriangulation; // default method
			}

			if (LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::PointImplicit ||
				LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::PointImplicitAugmentedWithMinimalDelaunay)
			{
				UpdateConnectivityGraphUsingPointImplicit(Parent, Parameters);
			}

			if (LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::DelaunayTriangulation)
			{
				UpdateConnectivityGraphUsingDelaunayTriangulation(Parent, Parameters); // not thread safe
			}

			if (LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::BoundsOverlapFilteredDelaunayTriangulation)
			{
				UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps(Parent, Parameters);
			}

			if (LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::PointImplicitAugmentedWithMinimalDelaunay ||
				LocalConnectionMethod == FClusterCreationParameters::EConnectionMethod::MinimalSpanningSubsetDelaunayTriangulation)
			{
				FixConnectivityGraphUsingDelaunayTriangulation(Parent, Parameters);
			}
		}
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::ClearConnectionGraph"), STAT_ClearConnectionGraph, STATGROUP_Chaos);
	void
	FRigidClustering::ClearConnectionGraph(FPBDRigidClusteredParticleHandle* Parent)
	{
		SCOPE_CYCLE_COUNTER(STAT_ClearConnectionGraph);
		if (!MChildren.Contains(Parent))
		{
			return;
		}

		const TArray<FPBDRigidParticleHandle*>& Children = MChildren[Parent];
		for (FPBDRigidParticleHandle* Handle : Children)
		{
			if (!Handle)
			{
				continue;
			}

			RemoveNodeConnections(Handle);
		}
	}

	FRealSingle MinImpulseForStrainEval = 980 * 2 * 1.f / 30.f; //ignore impulses caused by just keeping object on ground. This is a total hack, we should not use accumulated impulse directly. Instead we need to look at delta v along constraint normal
	FAutoConsoleVariableRef CVarMinImpulseForStrainEval(TEXT("p.chaos.MinImpulseForStrainEval"), MinImpulseForStrainEval, TEXT("Minimum accumulated impulse before accumulating for strain eval "));

	bool bUseContactSpeedForStrainThreshold = true;
	FAutoConsoleVariableRef CVarUseContactSpeedForStrainEval(TEXT("p.chaos.UseContactSpeedForStrainEval"), bUseContactSpeedForStrainThreshold, TEXT("Whether to use contact speed to discard contacts when updating cluster strain (true: use speed, false: use impulse)"));

	FRealSingle MinContactSpeedForStrainEval = 1.0f; // Ignore contacts where the two bodies are resting together
	FAutoConsoleVariableRef CVarMinContactSpeedForStrainEval(TEXT("p.chaos.MinContactSpeedForStrainEval"), MinContactSpeedForStrainEval, TEXT("Minimum speed at the contact before accumulating for strain eval "));

	DECLARE_CYCLE_STAT(TEXT("ComputeStrainFromCollision"), STAT_ComputeStrainFromCollision, STATGROUP_Chaos);
	void FRigidClustering::ComputeStrainFromCollision(const FPBDCollisionConstraints& CollisionRule)
	{
		SCOPE_CYCLE_COUNTER(STAT_ComputeStrainFromCollision);
		FClusterMap& MParentToChildren = GetChildrenMap();

		ResetCollisionImpulseArray();

		for (Chaos::FPBDCollisionConstraintHandle* ContactHandle : CollisionRule.GetConstraintHandles())
		{
			if (ContactHandle == nullptr)
			{
				continue;
			}

			TVector<FGeometryParticleHandle*, 2> ConstrainedParticles = ContactHandle->GetConstrainedParticles();
			
			// make sure we only compute things if one of the two particle is clustered
			FPBDRigidClusteredParticleHandle* ClusteredConstrainedParticles0 = ConstrainedParticles[0]->CastToClustered();
			FPBDRigidClusteredParticleHandle* ClusteredConstrainedParticles1 = ConstrainedParticles[1]->CastToClustered();
			if (!ClusteredConstrainedParticles0 && !ClusteredConstrainedParticles1)
			{
				continue;
			}

			const FPBDRigidParticleHandle* Rigid0 = ConstrainedParticles[0]->CastToRigidParticle();
			const FPBDRigidParticleHandle* Rigid1 = ConstrainedParticles[1]->CastToRigidParticle();

			if (bUseContactSpeedForStrainThreshold)
			{
				// Get dV between the two particles and project onto the normal to get the approach speed (take PreV as V is the new velocity post-solve)
				const FVec3 V0 = Rigid0 ? Rigid0->PreV() : FVec3(0);
				const FVec3 V1 = Rigid1 ? Rigid1->PreV() : FVec3(0);
				const FVec3 DeltaV = V0 - V1;
				const FReal SpeedAlongNormal = FVec3::DotProduct(DeltaV, ContactHandle->GetContact().CalculateWorldContactNormal());

				// If we're not approaching at more than the min speed, reject the contact
				if (SpeedAlongNormal > -MinContactSpeedForStrainEval && ContactHandle->GetAccumulatedImpulse().SizeSquared() > FReal(0))
				{
					continue;
				}
			}
			else if (ContactHandle->GetAccumulatedImpulse().Size() < MinImpulseForStrainEval)
			{
				continue;
			}

			auto ComputeStrainLambda = [this, &ContactHandle](
				const FPBDRigidClusteredParticleHandle* Cluster,
				FRealSingle& OutTotalImpulseAccumulator)
			{
				const FVec3 ContactWorldLocation = ContactHandle->GetContact().CalculateWorldContactLocation();
				
				const FRealSingle AccumulatedImpulse = static_cast<FRealSingle>(ContactHandle->GetAccumulatedImpulse().Size());
				if (AccumulatedImpulse > UE_SMALL_NUMBER && FMath::IsFinite(AccumulatedImpulse))
				{
					if (const FClusterUnionPhysicsProxy* ClusterUnionProxy = GetConcreteProxy<FClusterUnionPhysicsProxy>(Cluster))
					{
						// At the moment, we don't want to apply strains to children of ClusterUnions, we want instead
						// to apply the strains to GRANDchildren of ClusterUnions.
						FPBDRigidParticleHandle* ClosestChild = FindClosestChild(Cluster, ContactWorldLocation);

						// If Closest child is not a clustered, then there is no substructure to apply strain to,
						// so null Cluster
						Cluster
							= ClosestChild
							? ClosestChild->CastToClustered()
							: nullptr;
					}

					if (Cluster == nullptr)
					{
						return;
					}

					// gather propagation information from the parent proxy
					bool bUseDamagePropagation = false;
					if (const FGeometryCollectionPhysicsProxy* GeometryCollectionProxy = GetConcreteProxy<FGeometryCollectionPhysicsProxy>(Cluster))
					{
						const FSimulationParameters& SimParams = GeometryCollectionProxy->GetSimParameters();
						bUseDamagePropagation = SimParams.bUseDamagePropagation;
						if (!SimParams.bEnableStrainOnCollision)
						{
							return;
						}
					}

					if (bUseDamagePropagation)
					{
						// propagation based breaking model start from the closest particle and propagate through the connection graph
						// propagation logic is dealt when evaluating the strain
						if (FPBDRigidParticleHandle* ClosestChild = FindClosestChild(Cluster, ContactWorldLocation))
						{
							if (TPBDRigidClusteredParticleHandle<FReal, 3>* ClusteredChild = ClosestChild->CastToClustered())
							{
								ClusteredChild->CollisionImpulses() += AccumulatedImpulse;
								UpdateTopLevelParticle(ClusteredChild);
								OutTotalImpulseAccumulator += AccumulatedImpulse;
							}
						}
					}
					else
					{
						const FRigidTransform3 WorldToClusterTM = FRigidTransform3(Cluster->P(), Cluster->Q());
						const FVec3 ContactLocationClusterLocal = WorldToClusterTM.InverseTransformPosition(ContactWorldLocation);
						FAABB3 ContactBox(ContactLocationClusterLocal, ContactLocationClusterLocal);
						ContactBox.Thicken(ClusterDistanceThreshold);
						if (Cluster->ChildrenSpatial())
						{
							// todo(chaos): FindAllIntersectingChildren may return an unfiltered list of children ( when num children is under a certain threshold )   
							const TArray<FPBDRigidParticleHandle*> Intersections = Cluster->ChildrenSpatial()->FindAllIntersectingChildren(ContactBox);
							for (FPBDRigidParticleHandle* Child : Intersections)
							{
								if (TPBDRigidClusteredParticleHandle<FReal, 3>*ClusteredChild = Child->CastToClustered())
								{
									ClusteredChild->CollisionImpulses() += AccumulatedImpulse;
									UpdateTopLevelParticle(ClusteredChild);
									OutTotalImpulseAccumulator += AccumulatedImpulse;
								}
							}
						}
					}
				}
			};

			// We only need to dirty the impulse array if any of the active contacts actually added 
			// a collision impulse to a particle. If they are all resting or otherwise non-impulsive
			// contacts then we can skip dirtying the impulse array and avoid running the breaking
			// model when we know nothing will break
			FRealSingle TotalImpulses[] = { 0.0f, 0.0f };

			if (ClusteredConstrainedParticles0)
			{
				ComputeStrainLambda(ClusteredConstrainedParticles0, TotalImpulses[0]);
				MCollisionImpulseArrayDirty |= TotalImpulses[0] > 0.0f;
			}

			if (ClusteredConstrainedParticles1)
			{
				ComputeStrainLambda(ClusteredConstrainedParticles1, TotalImpulses[1]);
				MCollisionImpulseArrayDirty |= TotalImpulses[1] > 0.0f;
			}
		}
	}

	DECLARE_CYCLE_STAT(TEXT("ResetCollisionImpulseArray"), STAT_ResetCollisionImpulseArray, STATGROUP_Chaos);
	void FRigidClustering::ResetCollisionImpulseArray()
	{
		SCOPE_CYCLE_COUNTER(STAT_ResetCollisionImpulseArray);
		if (MCollisionImpulseArrayDirty)
		{
			FPBDRigidsSOAs& ParticleStructures = MEvolution.GetParticles();
			ParticleStructures.GetGeometryCollectionParticles().CollisionImpulsesArray().Fill(0.0f);
			ParticleStructures.GetClusteredParticles().CollisionImpulsesArray().Fill(0.0f);
			MCollisionImpulseArrayDirty = false;
		}
	}

	void FRigidClustering::DisableCluster(FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		// #note: we don't recursively descend to the children
		MEvolution.DisableParticle(ClusteredParticle);
		TopLevelClusterParents.Remove(ClusteredParticle);
		TopLevelClusterParentsStrained.Remove(ClusteredParticle);
		GetChildrenMap().Remove(ClusteredParticle);
		ClusteredParticle->ClusterIds() = ClusterId();
		ClusteredParticle->ClusterGroupIndex() = 0;
	}

	void FRigidClustering::ApplyStrainModifiers()
	{
		if (StrainModifiers)
		{
			for (ISimCallbackObject* Modifier : *StrainModifiers)
			{
				FStrainModifierAccessor Accessor(*this);
				Modifier->StrainModification_Internal(Accessor);
			}
		}
	}

	FPBDRigidClusteredParticleHandle*
	FRigidClustering::DestroyClusterParticle(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		const FClusterDestoryParameters& Parameters)
	{
		FClusterHandle ParentParticle = nullptr;

		// detach connections to thie parent from the children
		if (MChildren.Contains(ClusteredParticle))
		{
			for (FRigidHandle Child : MChildren[ClusteredParticle])
			{
				if (FClusterHandle ClusteredChild = Child->CastToClustered())
				{
					ClusteredChild->ClusterIds() = ClusterId();
					ClusteredChild->ClusterGroupIndex() = 0;
				}
			}

			MChildren.Remove(ClusteredParticle);
		}

		// disable within the solver
		if (!ClusteredParticle->Disabled())
		{
			MEvolution.DisableParticle(ClusteredParticle);
			ensure(ClusteredParticle->ClusterIds().Id == nullptr);
		}


		// need to disconnect from any other particles ( this can be from being a child of a cluster or a cluster union)
		RemoveNodeConnections(ClusteredParticle);

		// disconnect from the parents
		if (ClusteredParticle->ClusterIds().Id)
		{
			ParentParticle = ClusteredParticle->Parent();

			ClusteredParticle->ClusterIds() = ClusterId();
			ClusteredParticle->ClusterGroupIndex() = 0;

			// Need to also check if the particle is a cluster union and remove from that as well.
			ClusterUnionManager.HandleRemoveOperationWithClusterLookup({ ClusteredParticle }, EClusterUnionOperationTiming::Defer);

			if (MChildren.Contains(ParentParticle))
			{
				FRigidHandleArray& Children = MChildren[ParentParticle];

				// disconnect from your parents children list
				Children.Remove(ClusteredParticle);

				// disable internal parents that have lost all their children
				if (!MChildren[ParentParticle].Num() && ParentParticle->InternalCluster())
				{
					DisableCluster(ClusteredParticle);
				}
			}
		}

		// remove internal parents that have no children. 
		if (ClusteredParticle->InternalCluster())
		{
			FUniqueIdx UniqueIdx = ClusteredParticle->UniqueIdx();
			MEvolution.DestroyParticle(ClusteredParticle);
			MEvolution.ReleaseUniqueIdx(UniqueIdx);
		}

		if (Parameters.bReturnInternalOnly && ParentParticle && !ParentParticle->InternalCluster())
		{
			ParentParticle = nullptr;
		}

		// reset the structures
		// Note: this needs to be at the end to make sure that no other operations above may re-add it 
		// ( for example HandleRemoveOperationWithClusterLookup )
		TopLevelClusterParents.Remove(ClusteredParticle);
		TopLevelClusterParentsStrained.Remove(ClusteredParticle);

		return ParentParticle;

	}

	bool FRigidClustering::BreakCluster(FPBDRigidClusteredParticleHandle* ClusteredParticle)
	{
		if (!ClusteredParticle)
		{
			return false;
		}

		// max strain will allow to unconditionally release the children when strain is evaluated
		constexpr FRealSingle MaxStrain = TNumericLimits<FRealSingle>::Max();
		if (TArray<FPBDRigidParticleHandle*>* ChildrenHandles = GetChildrenMap().Find(ClusteredParticle))
		{
			for (FPBDRigidParticleHandle* ChildHandle: *ChildrenHandles)
			{
				if (Chaos::FPBDRigidClusteredParticleHandle* ClusteredChildHandle = ChildHandle->CastToClustered())
				{
					ClusteredChildHandle->SetExternalStrains(MaxStrain);
					SetExternalStrain(ClusteredChildHandle, MaxStrain);
				}
			}
			if (ChildrenHandles->Num() > 0)
			{
				CrumbledSinceLastUpdate.Add(ClusteredParticle);
				SendCrumblingEvent(ClusteredParticle);
			}
			return true;
		}
		return false;
	}

	bool FRigidClustering::BreakClustersByProxy(const IPhysicsProxyBase* Proxy)
	{
		bool bCrumbledAnyCluster = false;
		// max strain will allow to unconditionally release the children when strain is evaluated
		constexpr FRealSingle MaxStrain = TNumericLimits<FRealSingle>::Max();

		// we should probably have a way to retrieve all the active clusters per proxy instead of having to do this iteration
		for (FPBDRigidClusteredParticleHandle* ClusteredHandle : GetTopLevelClusterParents())
		{
			if (!ClusteredHandle)
			{
				continue;
			}

			const bool bIsInputProxy = ClusteredHandle->PhysicsProxy() == Proxy;

			// This handles the case where we want to break a GC but it's still in a cluster union.
			const bool bIsInPhysicsProxiesSet = ClusteredHandle->PhysicsProxies().Contains(Proxy);
			if (bIsInputProxy || bIsInPhysicsProxiesSet)
			{
				// Now we need to go from the parent cluster union particle to the GC particle that corresponds to the proxy.
				if (bIsInPhysicsProxiesSet)
				{
					if (TArray<FPBDRigidParticleHandle*>* Children = MChildren.Find(ClusteredHandle))
					{
						ClusteredHandle = nullptr;
						FPBDRigidParticleHandle** Candidate = Children->FindByPredicate(
							[Proxy](FPBDRigidParticleHandle* Particle)
							{
								return Particle->PhysicsProxy() == Proxy && Particle->CastToClustered();
							}
						);

						if (Candidate)
						{
							ClusteredHandle = (*Candidate)->CastToClustered();
						}
					}

					if (!ClusteredHandle)
					{
						continue;
					}
				}

				if (const TArray<FPBDRigidParticleHandle*>* Children = MChildren.Find(ClusteredHandle))
				{
					for (FPBDRigidParticleHandle* ChildHandle : *Children)
					{
						if (Chaos::FPBDRigidClusteredParticleHandle* ClusteredChildHandle = ChildHandle->CastToClustered())
						{
							SetExternalStrain(ClusteredChildHandle, MaxStrain);
						}
					}
					if (Children->Num() > 0)
					{
						CrumbledSinceLastUpdate.Add(ClusteredHandle);
						SendCrumblingEvent(ClusteredHandle);
					}
				}
				bCrumbledAnyCluster = true;
			}
		}

		return bCrumbledAnyCluster;
	}

	static void ConnectClusteredNodes(FPBDRigidClusteredParticleHandle* ClusteredChild1, FPBDRigidClusteredParticleHandle* ClusteredChild2)
	{
		check(ClusteredChild1 && ClusteredChild2);
		if (ClusteredChild1 == ClusteredChild2)
			return;
		const FRealSingle AvgStrain = (ClusteredChild1->GetInternalStrains() + ClusteredChild2->GetInternalStrains()) * 0.5f;
		TArray<TConnectivityEdge<FReal>>& Edges1 = ClusteredChild1->ConnectivityEdges();
		TArray<TConnectivityEdge<FReal>>& Edges2 = ClusteredChild2->ConnectivityEdges();
		if (//Edges1.Num() < Parameters.MaxNumConnections && 
			!Edges1.FindByKey(ClusteredChild2))
		{
			Edges1.Add(TConnectivityEdge<FReal>(ClusteredChild2, AvgStrain));
		}
		if (//Edges2.Num() < Parameters.MaxNumConnections && 
			!Edges2.FindByKey(ClusteredChild1))
		{
			Edges2.Add(TConnectivityEdge<FReal>(ClusteredChild1, AvgStrain));
		}
	}
	
	static void ConnectNodes(FPBDRigidParticleHandle* Child1, FPBDRigidParticleHandle* Child2)
	{
		check(Child1 != Child2);
		FPBDRigidClusteredParticleHandle* ClusteredChild1 = Child1->CastToClustered();
		FPBDRigidClusteredParticleHandle* ClusteredChild2 = Child2->CastToClustered();
		ConnectClusteredNodes(ClusteredChild1, ClusteredChild2);
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::UpdateConnectivityGraphUsingPointImplicit"), STAT_UpdateConnectivityGraphUsingPointImplicit, STATGROUP_Chaos);
	void 
	FRigidClustering::UpdateConnectivityGraphUsingPointImplicit(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateConnectivityGraphUsingPointImplicit);

		if (!UseConnectivity)
		{
			return;
		}

		const FReal Delta = FMath::Min(FMath::Max(Parameters.CoillisionThicknessPercent, FReal(0)), FReal(1));
		const TArray<FPBDRigidParticleHandle*>& Children = MChildren[Parent];

		typedef TPair<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*> ParticlePair;
		typedef TSet<ParticlePair> ParticlePairArray;

		TArray<ParticlePairArray> Connections;
		Connections.Init(ParticlePairArray(), Children.Num());

		PhysicsParallelFor(Children.Num(), [&](int32 i)
			{
				FPBDRigidParticleHandle* Child1 = Children[i];
				if (Child1->Geometry() && Child1->Geometry()->HasBoundingBox())
				{
					ParticlePairArray& ConnectionList = Connections[i];

					const FVec3& Child1X = Child1->X();
					FRigidTransform3 TM1 = FRigidTransform3(Child1X, Child1->R());

					const int32 Offset = i + 1;
					const int32 NumRemainingChildren = Children.Num() - Offset;

					for (int32 Idx = 0; Idx < NumRemainingChildren; ++Idx)
					{
						const int32 ChildrenIdx = Offset + Idx;
						FPBDRigidParticleHandle* Child2 = Children[ChildrenIdx];
						if (Child2->CollisionParticles())
						{

							const FVec3& Child2X = Child2->X();
							const FRigidTransform3 TM = TM1.GetRelativeTransform(FRigidTransform3(Child2X, Child2->R()));
							const uint32 NumCollisionParticles = Child2->CollisionParticles()->Size();
							for (uint32 CollisionIdx = 0; CollisionIdx < NumCollisionParticles; ++CollisionIdx)
							{
								const FVec3 LocalPoint =
									TM.TransformPositionNoScale(Child2->CollisionParticles()->X(CollisionIdx));
								const FReal Phi = Child1->Geometry()->SignedDistance(LocalPoint - (LocalPoint * Delta));
								if (Phi < 0.0)
								{
									ConnectionList.Add(ParticlePair(Child1, Child2));
									break;
								}

							}
						}
					}
				}
			});

		// join results and make connections
		for (const ParticlePairArray& ConnectionList : Connections)
		{
			for (const ParticlePair& Edge : ConnectionList)
			{
				ConnectNodes(Edge.Key, Edge.Value);
			}
		}

	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::FixConnectivityGraphUsingDelaunayTriangulation"), STAT_FixConnectivityGraphUsingDelaunayTriangulation, STATGROUP_Chaos);
	void 
	FRigidClustering::FixConnectivityGraphUsingDelaunayTriangulation(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_FixConnectivityGraphUsingDelaunayTriangulation);

		const TArray<FPBDRigidParticleHandle*>& Children = MChildren[Parent];

		// Compute Delaunay neighbor graph on children centers
		TArray<FVector> Pts;
		Pts.AddUninitialized(Children.Num());
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			Pts[i] = Children[i]->X();
		}
		TArray<TArray<int32>> Neighbors; // Indexes into Children
		VoronoiNeighbors(Pts, Neighbors);

		// Build a UnionFind graph to find (indirectly) connected children
		struct UnionFindInfo
		{
			FPBDRigidParticleHandle* GroupId;
			int32 Size;
		};
		TMap<FPBDRigidParticleHandle*, UnionFindInfo> UnionInfo;
		UnionInfo.Reserve(Children.Num());

		// Initialize UnionInfo:
		//		0: GroupId = Children[0], Size = 1
		//		1: GroupId = Children[1], Size = 1
		//		2: GroupId = Children[2], Size = 1
		//		3: GroupId = Children[3], Size = 1

		for(FPBDRigidParticleHandle* Child : Children)
		{
			UnionInfo.Add(Child, { Child, 1 }); // GroupId, Size
		}

		auto FindGroup = [&](FPBDRigidParticleHandle* Id) 
		{
			FPBDRigidParticleHandle* GroupId = Id;
			if (GroupId)
			{
				int findIters = 0;
				while (UnionInfo[GroupId].GroupId != GroupId)
				{
					ensure(findIters++ < 10); // if this while loop iterates more than a few times, there's probably a bug in the unionfind
					auto& CurrInfo = UnionInfo[GroupId];
					auto& NextInfo = UnionInfo[CurrInfo.GroupId];
					CurrInfo.GroupId = NextInfo.GroupId;
					GroupId = NextInfo.GroupId;
					if (!GroupId) break; // error condidtion
				}
			}
			return GroupId;
		};

		// MergeGroup(Children[0], Children[1])
		//		0: GroupId = Children[1], Size = 0
		//		1: GroupId = Children[1], Size = 2
		//		2: GroupId = Children[2], Size = 1
		//		3: GroupId = Children[3], Size = 1

		auto MergeGroup = [&](FPBDRigidParticleHandle* A, FPBDRigidParticleHandle* B) 
		{
			FPBDRigidParticleHandle* GroupA = FindGroup(A);
			FPBDRigidParticleHandle* GroupB = FindGroup(B);
			if (GroupA == GroupB)
			{
				return;
			}
			// Make GroupA the smaller of the two
			if (UnionInfo[GroupA].Size > UnionInfo[GroupB].Size)
			{
				Swap(GroupA, GroupB);
			}
			// Overwrite GroupA with GroupB
			UnionInfo[GroupA].GroupId = GroupB;
			UnionInfo[GroupB].Size += UnionInfo[GroupA].Size;
			UnionInfo[GroupA].Size = 0; // not strictly necessary, but more correct
		};

		// Merge all groups with edges connecting them.
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			FPBDRigidParticleHandle* Child = Children[i];
			const TArray<TConnectivityEdge<FReal>>& Edges = Child->CastToClustered()->ConnectivityEdges();
			for (const TConnectivityEdge<FReal>& Edge : Edges)
			{
				if (UnionInfo.Contains(Edge.Sibling))
				{
					MergeGroup(Child, Edge.Sibling);
				}
			}
		}

		// Find candidate edges from the Delaunay graph to consider adding
		struct LinkCandidate
		{
			//int32 A, B;
			FPBDRigidParticleHandle* A;
			FPBDRigidParticleHandle* B;
			FReal DistSq;
		};
		TArray<LinkCandidate> Candidates;
		Candidates.Reserve(Neighbors.Num());

		const FReal AlwaysAcceptBelowDistSqThreshold = 50.f*50.f*100.f*MClusterConnectionFactor;
		for (int32 i = 0; i < Neighbors.Num(); i++)
		{
			FPBDRigidParticleHandle* Child1 = Children[i];
			const TArray<int32>& Child1Neighbors = Neighbors[i];
			for (const int32 Nbr : Child1Neighbors)
			{
				if (Nbr < i)
				{ // assume we'll get the symmetric connection; don't bother considering this one
					continue;
				}
				FPBDRigidParticleHandle* Child2 = Children[Nbr];

				const FReal DistSq = FVector::DistSquared(Pts[i], Pts[Nbr]);
				if (DistSq < AlwaysAcceptBelowDistSqThreshold)
				{ // below always-accept threshold: don't bother adding to candidates array, just merge now
					MergeGroup(Child1, Child2);
					ConnectNodes(Child1, Child2);
					continue;
				}

				if (FindGroup(Child1) == FindGroup(Child2))
				{ // already part of the same group so we don't need Delaunay edge  
					continue;
				}

				// add to array to sort and add as-needed
				Candidates.Add({ Child1, Child2, DistSq });
			}
		}

		// Only add edges that would connect disconnected components, considering shortest edges first
		Candidates.Sort([](const LinkCandidate& A, const LinkCandidate& B) { return A.DistSq < B.DistSq; });
		for (const LinkCandidate& Candidate : Candidates)
		{
			FPBDRigidParticleHandle* Child1 = Candidate.A;
			FPBDRigidParticleHandle* Child2 = Candidate.B;
			if (FindGroup(Child1) != FindGroup(Child2))
			{
				MergeGroup(Child1, Child2);
				ConnectNodes(Child1, Child2);
			}
		}
	}
	
	// connection filters
	static bool IsAlwaysValidConnection(const FPBDRigidParticleHandle* Child1, const FPBDRigidParticleHandle* Child2)
	{
		return true;
	}
	
	static bool IsOverlappingConnection(const FPBDRigidParticleHandle* Child1, const FPBDRigidParticleHandle* Child2, FReal Margin)
	{
		if (ensure(Child1 != nullptr && Child2 != nullptr ))
		{
			FAABB3 Bounds1 = Child1->WorldSpaceInflatedBounds();
			Bounds1.Thicken(Margin);
			return Bounds1.Intersects(Child2->WorldSpaceInflatedBounds());
		}
		return false;
	}

	template <typename FilterLambda>
	static void UpdateConnectivityGraphUsingDelaunayTriangulationWithFiltering(
		const TArray<FPBDRigidParticleHandle*>& Children, FilterLambda ShouldKeepConnection)
	{
		TArray<FVector> Pts;
		Pts.AddUninitialized(Children.Num());
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			Pts[i] = Children[i]->X();
		}
		TArray<TArray<int>> Neighbors;
		VoronoiNeighbors(Pts, Neighbors);

		TSet<TPair<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*>> UniqueEdges;
		for (int32 i = 0; i < Neighbors.Num(); i++)
		{
			for (int32 j = 0; j < Neighbors[i].Num(); j++)
			{
				FPBDRigidParticleHandle* Child1 = Children[i];
				FPBDRigidParticleHandle* Child2 = Children[Neighbors[i][j]];
				const bool bFirstSmaller = Child1 < Child2;
				TPair<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*> SortedPair(
					bFirstSmaller ? Child1 : Child2, 
					bFirstSmaller ? Child2 : Child1);
				if (!UniqueEdges.Find(SortedPair))
				{
					if (ShouldKeepConnection(Child1, Child2))
					{
						// this does not use ConnectNodes because Neighbors is bi-direction : as in (1,2),(2,1)
						ConnectNodes(Child1, Child2);
						UniqueEdges.Add(SortedPair);
					}
				}
			}
		}
	}
	
	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::UpdateConnectivityGraphUsingDelaunayTriangulation"), STAT_UpdateConnectivityGraphUsingDelaunayTriangulation, STATGROUP_Chaos);
	void FRigidClustering::UpdateConnectivityGraphUsingDelaunayTriangulation(
		const Chaos::FPBDRigidClusteredParticleHandle* Parent, 
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateConnectivityGraphUsingDelaunayTriangulation);

		if (UseBoundingBoxForConnectionGraphFiltering)
		{
			constexpr auto IsOverlappingConnectionUsingCVarMargin =
				[] (const FPBDRigidParticleHandle* Child1, const FPBDRigidParticleHandle* Child2)
				{
					return IsOverlappingConnection(Child1, Child2, BoundingBoxMarginForConnectionGraphFiltering);
				};
			UpdateConnectivityGraphUsingDelaunayTriangulationWithFiltering(MChildren[Parent], IsOverlappingConnectionUsingCVarMargin);
		}
		else
		{
			UpdateConnectivityGraphUsingDelaunayTriangulationWithFiltering(MChildren[Parent], IsAlwaysValidConnection);
		}
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps"), STAT_UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps, STATGROUP_Chaos);
	void FRigidClustering::UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps(
		const Chaos::FPBDRigidClusteredParticleHandle* Parent, 
		const FClusterCreationParameters& Parameters)
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps);

		const auto IsOverlappingConnectionUsingCVarMargin =
			[&Parameters] (const FPBDRigidParticleHandle* Child1, const FPBDRigidParticleHandle* Child2)
			{
				return IsOverlappingConnection(Child1, Child2, Parameters.ConnectionGraphBoundsFilteringMargin);
			};
		
		UpdateConnectivityGraphUsingDelaunayTriangulationWithFiltering(MChildren[Parent], IsOverlappingConnectionUsingCVarMargin);
	}

	void 
	FRigidClustering::RemoveNodeConnections(
		FPBDRigidParticleHandle* Child)
	{
		RemoveNodeConnections(Child->CastToClustered());
	}

	DECLARE_CYCLE_STAT(TEXT("TPBDRigidClustering<>::RemoveNodeConnections"), STAT_RemoveNodeConnections, STATGROUP_Chaos);
	void 
	FRigidClustering::RemoveNodeConnections(
		FPBDRigidClusteredParticleHandle* ClusteredChild)
	{
		SCOPE_CYCLE_COUNTER(STAT_RemoveNodeConnections);
		check(ClusteredChild);
		TArray<TConnectivityEdge<FReal>>& Edges = ClusteredChild->ConnectivityEdges();
		for (TConnectivityEdge<FReal>& Edge : Edges)
		{
			TArray<TConnectivityEdge<FReal>>& OtherEdges = Edge.Sibling->CastToClustered()->ConnectivityEdges();
			const int32 Idx = OtherEdges.IndexOfByKey(ClusteredChild);
			if (Idx != INDEX_NONE)
				OtherEdges.RemoveAtSwap(Idx);
			// Make sure there are no duplicates!
			check(OtherEdges.IndexOfByKey(ClusteredChild) == INDEX_NONE);
		}
		Edges.SetNum(0);
	}


} // namespace Chaos
