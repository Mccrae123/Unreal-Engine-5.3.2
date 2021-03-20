// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODSubsystem.h"
#include "Components/PrimitiveComponent.h"

#if WITH_EDITOR
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/HLOD/HLODActorDesc.h"
#include "WorldPartition/HLOD/HLODBuilder.h"
#endif

AWorldPartitionHLOD::AWorldPartitionHLOD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);

#if WITH_EDITORONLY_DATA
	HLODHash = 0;
	HLODBounds = FBox(EForceInit::ForceInit);
#endif
}

UPrimitiveComponent* AWorldPartitionHLOD::GetHLODComponent()
{
	return Cast<UPrimitiveComponent>(RootComponent);
}

void AWorldPartitionHLOD::SetVisibility(bool bInVisible)
{
	if (GetRootComponent())
	{
		GetRootComponent()->SetVisibility(bInVisible, /*bPropagateToChildren*/ true);
	}
}

void AWorldPartitionHLOD::BeginPlay()
{
	Super::BeginPlay();
	GetWorld()->GetSubsystem<UHLODSubsystem>()->RegisterHLODActor(this);
}

void AWorldPartitionHLOD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetSubsystem<UHLODSubsystem>()->UnregisterHLODActor(this);
	Super::EndPlay(EndPlayReason);
}

void AWorldPartitionHLOD::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if (CellName == NAME_None)
	{
		// Prior to the addition of the cell name member, the cell name was actually stored in the label
		CellName = *GetActorLabel();
	}
#endif
}

#if WITH_EDITOR

void AWorldPartitionHLOD::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	SetIsTemporarilyHiddenInEditor(true);
	bListedInSceneOutliner = true;
	SetFolderPath(*FString::Printf(TEXT("HLOD/HLOD%d"), GetLODLevel()));
}

EActorGridPlacement AWorldPartitionHLOD::GetDefaultGridPlacement() const
{
	return EActorGridPlacement::Location;
}

TUniquePtr<FWorldPartitionActorDesc> AWorldPartitionHLOD::CreateClassActorDesc() const
{
	return TUniquePtr<FWorldPartitionActorDesc>(new FHLODActorDesc());
}

void AWorldPartitionHLOD::SetHLODPrimitives(const TArray<UPrimitiveComponent*>& InHLODPrimitives)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AWorldPartitionHLOD::SetHLODPrimitive);
	check(!InHLODPrimitives.IsEmpty());

	TArray<USceneComponent*> ComponentsToRemove;
	GetComponents<USceneComponent>(ComponentsToRemove);

	SetRootComponent(InHLODPrimitives[0]);

	for(UPrimitiveComponent* InHLODPrimitive : InHLODPrimitives)
	{
		ComponentsToRemove.Remove(InHLODPrimitive);

		AddInstanceComponent(InHLODPrimitive);

		if (InHLODPrimitive != RootComponent)
		{
			InHLODPrimitive->SetupAttachment(RootComponent);
		}
	
		InHLODPrimitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		InHLODPrimitive->SetMobility(EComponentMobility::Static);

		InHLODPrimitive->RegisterComponent();
		InHLODPrimitive->MarkRenderStateDirty();
	}

	for (USceneComponent* ComponentToRemove : ComponentsToRemove)
	{
		ComponentToRemove->DestroyComponent();
	}
}

void AWorldPartitionHLOD::SetSubActors(const TArray<FGuid>& InSubActors)
{
	SubActors = InSubActors;
}

const TArray<FGuid>& AWorldPartitionHLOD::GetSubActors() const
{
	return SubActors;
}

void AWorldPartitionHLOD::SetCellName(FName InCellName)
{
	CellName = InCellName;
}

const FBox& AWorldPartitionHLOD::GetHLODBounds() const
{
	return HLODBounds;
}

void AWorldPartitionHLOD::SetHLODBounds(const FBox& InBounds)
{
	HLODBounds = InBounds;
}

void AWorldPartitionHLOD::GetActorBounds(bool bOnlyCollidingComponents, FVector& Origin, FVector& BoxExtent, bool bIncludeFromChildActors) const
{
	Super::GetActorBounds(bOnlyCollidingComponents, Origin, BoxExtent, bIncludeFromChildActors);

	FBox Bounds = FBox(Origin - BoxExtent, Origin + BoxExtent);
	Bounds += HLODBounds;
	Bounds.GetCenterAndExtents(Origin, BoxExtent);
}

void AWorldPartitionHLOD::GetActorLocationBounds(bool bOnlyCollidingComponents, FVector& Origin, FVector& BoxExtent, bool bIncludeFromChildActors) const
{
	GetActorBounds(bOnlyCollidingComponents, Origin, BoxExtent, bIncludeFromChildActors);
}

uint32 AWorldPartitionHLOD::GetHLODHash() const
{
	return HLODHash;
}

void AWorldPartitionHLOD::BuildHLOD(bool bForceBuild)
{
	if (bForceBuild)
	{
		HLODHash = 0;
	}

	HLODHash = FHLODBuilderUtilities::BuildHLOD(this);
}

#endif // WITH_EDITOR
