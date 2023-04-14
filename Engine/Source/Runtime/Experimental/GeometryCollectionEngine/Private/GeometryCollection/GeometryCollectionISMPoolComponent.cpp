// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeometryCollection/GeometryCollectionISMPoolComponent.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"

#include "GeometryCollection/GeometryCollectionComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GeometryCollectionISMPoolComponent)

FGeometryCollectionMeshGroup::FMeshId FGeometryCollectionMeshGroup::AddMesh(const FGeometryCollectionStaticMeshInstance& MeshInstance, int32 InstanceCount, const FGeometryCollectionMeshInfo& ISMInstanceInfo)
{
	FMeshId* MeshIndex = Meshes.Find(MeshInstance);
	if (MeshIndex)
	{
		return *MeshIndex;
	}

	const FMeshId MeshInfoIndex = MeshInfos.Emplace(ISMInstanceInfo);
	Meshes.Add(MeshInstance, MeshInfoIndex);
	return MeshInfoIndex;
}

bool FGeometryCollectionMeshGroup::BatchUpdateInstancesTransforms(FGeometryCollectionISMPool& ISMPool, FMeshId MeshId, int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (MeshInfos.IsValidIndex(MeshId))
	{
		return ISMPool.BatchUpdateInstancesTransforms(MeshInfos[MeshId], StartInstanceIndex, NewInstancesTransforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);
	}
	UE_LOG(LogChaos, Warning, TEXT("UGeometryCollectionISMPoolComponent : Invalid mesh Id (%d) for this mesh group"), MeshId);
	return false;
}

void FGeometryCollectionMeshGroup::RemoveAllMeshes(FGeometryCollectionISMPool& ISMPool)
{
	for (const FGeometryCollectionMeshInfo& MeshInfo: MeshInfos)
	{
		ISMPool.RemoveISM(MeshInfo);
	}
	MeshInfos.Empty();
	Meshes.Empty();
}

FGeometryCollectionISM::FGeometryCollectionISM(AActor* OwmingActor, const FGeometryCollectionStaticMeshInstance& InMeshInstance)
{
	MeshInstance = InMeshInstance;

	check(MeshInstance.StaticMesh);
	check(OwmingActor);

	UHierarchicalInstancedStaticMeshComponent* HISMC = nullptr;
	UInstancedStaticMeshComponent* ISMC = nullptr;
	
	if (MeshInstance.Desc.bUseHISM)
	{
		const FName ISMName = MakeUniqueObjectName(OwmingActor, UHierarchicalInstancedStaticMeshComponent::StaticClass(), MeshInstance.StaticMesh->GetFName());
		ISMC = HISMC = NewObject<UHierarchicalInstancedStaticMeshComponent>(OwmingActor, ISMName, RF_Transient | RF_DuplicateTransient);
	}
	else
	{
		const FName ISMName = MakeUniqueObjectName(OwmingActor, UInstancedStaticMeshComponent::StaticClass(), MeshInstance.StaticMesh->GetFName());
		ISMC = NewObject<UInstancedStaticMeshComponent>(OwmingActor, ISMName, RF_Transient | RF_DuplicateTransient);
	}

	if (!ensure(ISMC != nullptr))
	{
		return;
	}

	ISMC->SetStaticMesh(MeshInstance.StaticMesh);
	for (int32 MaterialIndex = 0; MaterialIndex < MeshInstance.MaterialsOverrides.Num(); MaterialIndex++)
	{
		ISMC->SetMaterial(MaterialIndex, MeshInstance.MaterialsOverrides[MaterialIndex]);
	}

	ISMC->NumCustomDataFloats = MeshInstance.Desc.NumCustomDataFloats;
	ISMC->SetReverseCulling(MeshInstance.Desc.bReverseCulling);
	ISMC->SetMobility(MeshInstance.Desc.bIsStaticMobility ? EComponentMobility::Static : EComponentMobility::Stationary);
	ISMC->SetCullDistances(MeshInstance.Desc.StartCullDistance, MeshInstance.Desc.EndCullDistance);
	ISMC->SetCastShadow(MeshInstance.Desc.bAffectShadow);
	ISMC->bAffectDynamicIndirectLighting = MeshInstance.Desc.bAffectDynamicIndirectLighting;
	ISMC->bAffectDistanceFieldLighting = MeshInstance.Desc.bAffectDistanceFieldLighting;
	ISMC->SetCanEverAffectNavigation(false);
	ISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ISMC->bOverrideMinLOD = MeshInstance.Desc.MinLod > 0;	
	ISMC->MinLOD = MeshInstance.Desc.MinLod;
	
	if (HISMC)
	{
		HISMC->SetLODDistanceScale(MeshInstance.Desc.LodScale);
	}
	
	OwmingActor->AddInstanceComponent(ISMC);
	ISMC->RegisterComponent();
	ISMComponent = ISMC;
}

FInstanceGroups::FInstanceGroupId FGeometryCollectionISM::AddInstanceGroup(int32 InstanceCount, TArrayView<const float> CustomDataFloats)
{
	// When adding new group it will always have a single range
	const FInstanceGroups::FInstanceGroupId InstanceGroupIndex = InstanceGroups.AddGroup(InstanceCount);
	const FInstanceGroups::FInstanceGroupRange& NewInstanceGroup = InstanceGroups.GroupRanges[InstanceGroupIndex];

	FTransform ZeroScaleTransform;
	ZeroScaleTransform.SetIdentityZeroScale();
	TArray<FTransform> ZeroScaleTransforms;
	ZeroScaleTransforms.Init(ZeroScaleTransform, InstanceCount);

	ISMComponent->PreAllocateInstancesMemory(InstanceCount);
	TArray<int32> RenderInstances = ISMComponent->AddInstances(ZeroScaleTransforms, true, true);

	// Ensure that remapping arrays are big enough to hold any new items.
	InstanceIndexToRenderIndex.SetNum(InstanceGroups.GetMaxInstanceIndex(), false);
	RenderIndexToInstanceIndex.SetNum(ISMComponent->PerInstanceSMData.Num(), false);
	// Store mapping between our fixed instance index and the mutable ISM render index.
	// todo: Improve ISM API so that we don't need to pay the memory overhead here to manage this.
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
	{
		InstanceIndexToRenderIndex[NewInstanceGroup.Start + InstanceIndex] = RenderInstances[InstanceIndex];
		RenderIndexToInstanceIndex[RenderInstances[InstanceIndex]] = NewInstanceGroup.Start + InstanceIndex;
	}

	// Set any custom data.
	if (CustomDataFloats.Num())
	{
		const int32 NumCustomDataFloats = ISMComponent->NumCustomDataFloats;
		if (ensure(NumCustomDataFloats * InstanceCount == CustomDataFloats.Num()))
		{
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
			{
				ISMComponent->SetCustomData(RenderInstances[InstanceIndex], CustomDataFloats.Slice(InstanceIndex * NumCustomDataFloats, NumCustomDataFloats));
			}
		}
	}

	return InstanceGroupIndex;
}

FGeometryCollectionMeshInfo FGeometryCollectionISMPool::AddISM(UGeometryCollectionISMPoolComponent* OwningComponent, const FGeometryCollectionStaticMeshInstance& MeshInstance, int32 InstanceCount, TArrayView<const float> CustomDataFloats)
{
	FGeometryCollectionMeshInfo Info;

	FISMIndex* ISMIndex = MeshToISMIndex.Find(MeshInstance);
	if (!ISMIndex)
	{
		if (FreeList.Num())
		{
			// Take an ISM from the current FreeList instead of allocating a new slot.
			Info.ISMIndex = FreeList.Last();
			FreeList.RemoveAt(FreeList.Num() - 1);
			ISMs[Info.ISMIndex] = FGeometryCollectionISM(OwningComponent->GetOwner(), MeshInstance);
		}
		else
		{
			Info.ISMIndex = ISMs.Emplace(OwningComponent->GetOwner(), MeshInstance);
		}
		MeshToISMIndex.Add(MeshInstance, Info.ISMIndex);
		ISMComponentToISMIndex.Add(ISMs[Info.ISMIndex].ISMComponent, Info.ISMIndex);
	}
	else
	{
		Info.ISMIndex = *ISMIndex;
	}
	// add to the ISM 
	Info.InstanceGroupIndex = ISMs[Info.ISMIndex].AddInstanceGroup(InstanceCount, CustomDataFloats);
	return Info;
}

bool FGeometryCollectionISMPool::BatchUpdateInstancesTransforms(FGeometryCollectionMeshInfo& MeshInfo, int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (ISMs.IsValidIndex(MeshInfo.ISMIndex))
	{
		FGeometryCollectionISM& ISM = ISMs[MeshInfo.ISMIndex];
		const FInstanceGroups::FInstanceGroupRange& InstanceGroup = ISM.InstanceGroups.GroupRanges[MeshInfo.InstanceGroupIndex];
		ensure((StartInstanceIndex + NewInstancesTransforms.Num()) <= InstanceGroup.Count);

		int32 StartIndex = ISM.InstanceIndexToRenderIndex[InstanceGroup.Start];
		int32 TransformIndex = 0;
		int32 BatchCount = 1;
		TArray<FTransform> BatchTransforms; //< Can't use TArrayView because blueprint function doesn't support that 
		BatchTransforms.Reserve(NewInstancesTransforms.Num());
		BatchTransforms.Add(NewInstancesTransforms[TransformIndex++]);
		for (int InstanceIndex = StartInstanceIndex + 1; InstanceIndex < NewInstancesTransforms.Num(); ++InstanceIndex)
		{
			// flush batch?
			int32 RenderIndex = ISM.InstanceIndexToRenderIndex[InstanceGroup.Start + InstanceIndex];
			if (RenderIndex != (StartIndex + BatchCount))
			{
				ISM.ISMComponent->BatchUpdateInstancesTransforms(StartIndex, BatchTransforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);
				StartIndex = RenderIndex;
				BatchTransforms.SetNum(0, false);
				BatchCount = 0;
			}
			
			BatchTransforms.Add(NewInstancesTransforms[TransformIndex++]);
			BatchCount++;
		}

		return ISM.ISMComponent->BatchUpdateInstancesTransforms(StartIndex, BatchTransforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);
	}
	UE_LOG(LogChaos, Warning, TEXT("UGeometryCollectionISMPoolComponent : Invalid ISM Id (%d) when updating the transform "), MeshInfo.ISMIndex);
	return false;
}

void FGeometryCollectionISMPool::RemoveISM(const FGeometryCollectionMeshInfo& MeshInfo)
{
	if (ISMs.IsValidIndex(MeshInfo.ISMIndex))
	{
		FGeometryCollectionISM& ISM = ISMs[MeshInfo.ISMIndex];
		const FInstanceGroups::FInstanceGroupRange& InstanceGroup = ISM.InstanceGroups.GroupRanges[MeshInfo.InstanceGroupIndex];
		
		TArray<int32> InstancesToRemove;
		InstancesToRemove.SetNum(InstanceGroup.Count);
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceGroup.Count; ++InstanceIndex)
		{
			InstancesToRemove[InstanceIndex] = ISM.InstanceIndexToRenderIndex[InstanceGroup.Start + InstanceIndex];
		}
		ISM.ISMComponent->RemoveInstances(InstancesToRemove);

		ISM.InstanceGroups.RemoveGroup(MeshInfo.InstanceGroupIndex);
	
		if (ISM.InstanceGroups.IsEmpty() && ISM.ISMComponent->PerInstanceSMData.Num() == 0)
		{
			// Remove component and push this ISM slot to the free list.
			// todo: profile if it is better to push component into a free pool and recycle it.
			ISM.ISMComponent->GetOwner()->RemoveInstanceComponent(ISM.ISMComponent);
			ISM.ISMComponent->UnregisterComponent();
			ISM.ISMComponent->DestroyComponent();
			
			ISM.InstanceGroups.Reset();
			MeshToISMIndex.Remove(ISM.MeshInstance);
			ISMComponentToISMIndex.Remove(ISM.ISMComponent);
			FreeList.Add(MeshInfo.ISMIndex);

			ISM.ISMComponent = nullptr;
		}
	}
}

void FGeometryCollectionISMPool::OnISMInstanceIndexUpdated(UInstancedStaticMeshComponent* InComponent, TArrayView<const FInstancedStaticMeshDelegates::FInstanceIndexUpdateData> InIndexUpdates)
{
	FISMIndex* ISMIndex = ISMComponentToISMIndex.Find(InComponent);
	if (!ISMIndex)
	{
		return;
	}

	FGeometryCollectionISM& ISM = ISMs[*ISMIndex];
	check(ISM.ISMComponent == InComponent);

	for (const FInstancedStaticMeshDelegates::FInstanceIndexUpdateData& IndexUpdateData : InIndexUpdates)
	{
		if (IndexUpdateData.Type == FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Removed)
		{
			ISM.IndexRemoved(IndexUpdateData.Index);
		}
		else if (IndexUpdateData.Type == FInstancedStaticMeshDelegates::EInstanceIndexUpdateType::Relocated)
		{
			ISM.IndexReallocated(IndexUpdateData.OldIndex, IndexUpdateData.Index);
		}
	}
}

void FGeometryCollectionISMPool::Clear()
{
	MeshToISMIndex.Reset();
	ISMComponentToISMIndex.Reset();
	FreeList.Reset();
	if (ISMs.Num() > 0)
	{
		if (AActor* OwningActor = ISMs[0].ISMComponent->GetOwner())
		{
			for(FGeometryCollectionISM& ISM : ISMs)
			{
				ISM.ISMComponent->UnregisterComponent();
				ISM.ISMComponent->DestroyComponent();
				OwningActor->RemoveInstanceComponent(ISM.ISMComponent);
			}
		}
		ISMs.Reset();
	}
}

UGeometryCollectionISMPoolComponent::UGeometryCollectionISMPoolComponent(const FObjectInitializer& ObjectInitializer)
	: NextMeshGroupId(0)
{
}

void UGeometryCollectionISMPoolComponent::OnRegister()
{
	FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.AddUObject(this, &UGeometryCollectionISMPoolComponent::OnISMInstanceIndexUpdated);
	Super::OnRegister();
}

void UGeometryCollectionISMPoolComponent::OnUnregister()
{
	FInstancedStaticMeshDelegates::OnInstanceIndexUpdated.RemoveAll(this);
	Super::OnUnregister();
}

UGeometryCollectionISMPoolComponent::FMeshGroupId  UGeometryCollectionISMPoolComponent::CreateMeshGroup()
{
	MeshGroups.Add(NextMeshGroupId);
	return NextMeshGroupId++;
}

void UGeometryCollectionISMPoolComponent::DestroyMeshGroup(FMeshGroupId MeshGroupId)
{
	if (FGeometryCollectionMeshGroup* MeshGroup = MeshGroups.Find(MeshGroupId))
	{
		MeshGroup->RemoveAllMeshes(Pool);
		MeshGroups.Remove(MeshGroupId);
	}
}

UGeometryCollectionISMPoolComponent::FMeshId UGeometryCollectionISMPoolComponent::AddMeshToGroup(FMeshGroupId MeshGroupId, const FGeometryCollectionStaticMeshInstance& MeshInstance, int32 InstanceCount, TArrayView<const float> CustomDataFloats)
{
	if (FGeometryCollectionMeshGroup* MeshGroup = MeshGroups.Find(MeshGroupId))
	{
		const FGeometryCollectionMeshInfo ISMInstanceInfo = Pool.AddISM(this, MeshInstance, InstanceCount, CustomDataFloats);
		return MeshGroup->AddMesh(MeshInstance, InstanceCount, ISMInstanceInfo);
	}
	UE_LOG(LogChaos, Warning, TEXT("UGeometryCollectionISMPoolComponent : Trying to add a mesh to a mesh group (%d) that does not exists"), MeshGroupId);
	return INDEX_NONE;
}

bool UGeometryCollectionISMPoolComponent::BatchUpdateInstancesTransforms(FMeshGroupId MeshGroupId, FMeshId MeshId, int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, bool bWorldSpace, bool bMarkRenderStateDirty, bool bTeleport)
{
	if (FGeometryCollectionMeshGroup* MeshGroup = MeshGroups.Find(MeshGroupId))
	{
		return MeshGroup->BatchUpdateInstancesTransforms(Pool, MeshId, StartInstanceIndex, NewInstancesTransforms, bWorldSpace, bMarkRenderStateDirty, bTeleport);
	}
	UE_LOG(LogChaos, Warning, TEXT("UGeometryCollectionISMPoolComponent : Trying to update instance with mesh group (%d) that not exists"), MeshGroupId);
	return false;
}

void UGeometryCollectionISMPoolComponent::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	int32 SizeBytes =
		MeshGroups.GetAllocatedSize()
		+ Pool.MeshToISMIndex.GetAllocatedSize()
		+ Pool.ISMComponentToISMIndex.GetAllocatedSize()
		+ Pool.ISMs.GetAllocatedSize()
		+ Pool.FreeList.GetAllocatedSize();
	
	for (FGeometryCollectionISM ISM : Pool.ISMs)
	{
		SizeBytes += ISM.InstanceIndexToRenderIndex.GetAllocatedSize()
			+ ISM.RenderIndexToInstanceIndex.GetAllocatedSize()
			+ ISM.InstanceGroups.GroupRanges.GetAllocatedSize()
			+ ISM.InstanceGroups.FreeList.GetAllocatedSize();
	}

	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(SizeBytes);
}
