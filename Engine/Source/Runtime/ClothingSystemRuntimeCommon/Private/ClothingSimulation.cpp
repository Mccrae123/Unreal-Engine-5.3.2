// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothingSimulation.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsSettings.h"

//==============================================================================
// FClothingSimulationContextCommon
//==============================================================================

FClothingSimulationContextCommon::FClothingSimulationContextCommon()
	: ComponentToWorld(FTransform::Identity)
	, WorldGravity(FVector::ZeroVector)
	, WindVelocity(FVector::ZeroVector)
	, DeltaSeconds(0.f)
{}

FClothingSimulationContextCommon::~FClothingSimulationContextCommon()
{}

void FClothingSimulationContextCommon::Fill(const USkeletalMeshComponent* InComponent, float InDeltaSeconds, float InMaxPhysicsDelta)
{
	check(InComponent);
	FillBoneTransforms(InComponent);
	FillRefToLocals(InComponent);
	FillComponentToWorld(InComponent);
	FillWorldGravity(InComponent);
	FillWindVelocity(InComponent);
	FillDeltaSeconds(InDeltaSeconds, InMaxPhysicsDelta);
}

void FClothingSimulationContextCommon::FillBoneTransforms(const USkeletalMeshComponent* InComponent)
{
	const USkeletalMesh* const SkeletalMesh = InComponent->SkeletalMesh;

	if (USkinnedMeshComponent* const MasterComponent = InComponent->MasterPoseComponent.Get())
	{
		const TArray<int32>& MasterBoneMap = InComponent->GetMasterBoneMap();
		int32 NumBones = MasterBoneMap.Num();

		if (NumBones == 0)
		{
			if (SkeletalMesh)
			{
				// This case indicates an invalid master pose component (e.g. no skeletal mesh)
				NumBones = SkeletalMesh->RefSkeleton.GetNum();

				BoneTransforms.Empty(NumBones);
				BoneTransforms.AddDefaulted(NumBones);
			}
		}
		else
		{
			BoneTransforms.Reset(NumBones);
			BoneTransforms.AddDefaulted(NumBones);

			const TArray<FTransform>& MasterTransforms = MasterComponent->GetComponentSpaceTransforms();
			for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
			{
				bool bFoundMaster = false;
				if (MasterBoneMap.IsValidIndex(BoneIndex))
				{
					const int32 MasterIndex = MasterBoneMap[BoneIndex];
					if (MasterIndex != INDEX_NONE && MasterIndex < MasterTransforms.Num())
					{
						BoneTransforms[BoneIndex] = MasterTransforms[MasterIndex];
						bFoundMaster = true;
					}
				}

				if (!bFoundMaster && SkeletalMesh)
				{
					const int32 ParentIndex = SkeletalMesh->RefSkeleton.GetParentIndex(BoneIndex);

					BoneTransforms[BoneIndex] =
						BoneTransforms.IsValidIndex(ParentIndex) && ParentIndex < BoneIndex ?
						BoneTransforms[ParentIndex] * SkeletalMesh->RefSkeleton.GetRefBonePose()[BoneIndex] :
						SkeletalMesh->RefSkeleton.GetRefBonePose()[BoneIndex];
				}
			}
		}
	}
	else
	{
		BoneTransforms = InComponent->GetComponentSpaceTransforms();
	}
}

void FClothingSimulationContextCommon::FillRefToLocals(const USkeletalMeshComponent* InComponent)
{
	RefToLocals.Reset();
	InComponent->GetCurrentRefToLocalMatrices(RefToLocals, 0);
}

void FClothingSimulationContextCommon::FillComponentToWorld(const USkeletalMeshComponent* InComponent)
{
	ComponentToWorld = InComponent->GetComponentTransform();
}

void FClothingSimulationContextCommon::FillWorldGravity(const USkeletalMeshComponent* InComponent)
{
	const UWorld* const ComponentWorld = InComponent->GetWorld();
	check(ComponentWorld);
	WorldGravity = FVector(0.f, 0.f, ComponentWorld->GetGravityZ());
}

void FClothingSimulationContextCommon::FillWindVelocity(const USkeletalMeshComponent* InComponent)
{
	SetWindFromComponent(InComponent);
}

void FClothingSimulationContextCommon::FillDeltaSeconds(float InDeltaSeconds, float InMaxPhysicsDelta)
{
	DeltaSeconds = FMath::Min(InDeltaSeconds, InMaxPhysicsDelta);
}

float FClothingSimulationContextCommon::SetWindFromComponent(const USkeletalMeshComponent* Component)
{
	float WindAdaption;
	Component->GetWindForCloth_GameThread(WindVelocity, WindAdaption);
	return WindAdaption;
}

//==============================================================================
// FClothingSimulationCommon
//==============================================================================

FClothingSimulationCommon::FClothingSimulationCommon()
{
	MaxPhysicsDelta = UPhysicsSettings::Get()->MaxPhysicsDeltaTime;
}

FClothingSimulationCommon::~FClothingSimulationCommon()
{}

void FClothingSimulationCommon::FillContext(USkeletalMeshComponent* InComponent, float InDeltaTime, IClothingSimulationContext* InOutContext)
{
	check(InOutContext);
	FClothingSimulationContextCommon* const Context = static_cast<FClothingSimulationContextCommon*>(InOutContext);

	Context->Fill(InComponent, InDeltaTime, MaxPhysicsDelta);

	// Checking the component here to track rare issue leading to invalid contexts
	if (InComponent->IsPendingKill())
	{
		const AActor* const CompOwner = InComponent->GetOwner();
		UE_LOG(LogSkeletalMesh, Warning, 
			TEXT("Attempting to fill a clothing simulation context for a PendingKill skeletal mesh component (Comp: %s, Actor: %s). "
				"Pending kill skeletal mesh components should be unregistered before marked pending kill."), 
			*InComponent->GetName(), CompOwner ? *CompOwner->GetName() : TEXT("None"));

		// Make sure we clear this out to skip any attempted simulations
		Context->BoneTransforms.Reset();
	}

	if (Context->BoneTransforms.Num() == 0)
	{
		const AActor* const CompOwner = InComponent->GetOwner();
		const USkinnedMeshComponent* const Master = InComponent->MasterPoseComponent.Get();
		UE_LOG(LogSkeletalMesh, Warning, TEXT("Attempting to fill a clothing simulation context for a skeletal mesh component that has zero bones (Comp: %s, Master: %s, Actor: %s)."), *InComponent->GetName(), Master ? *Master->GetName() : TEXT("None"), CompOwner ? *CompOwner->GetName() : TEXT("None"));

		// Make sure we clear this out to skip any attempted simulations
		Context->BoneTransforms.Reset();
	}
}
