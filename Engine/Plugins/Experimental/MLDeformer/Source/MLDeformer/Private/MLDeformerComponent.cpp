// Copyright Epic Games, Inc. All Rights Reserved.

#include "MLDeformerComponent.h"
#include "MLDeformer.h"
#include "MLDeformerAsset.h"
#include "MLDeformerInstance.h"
#include "Components/SkeletalMeshComponent.h"

UMLDeformerComponent::UMLDeformerComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bTickInEditor = true;
	bAutoActivate = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bCanEverTick = true;
}

void UMLDeformerComponent::SetupComponent(UMLDeformerAsset* InDeformerAsset, USkeletalMeshComponent* InSkelMeshComponent)
{
	DeformerAsset = InDeformerAsset;
	SkelMeshComponent = InSkelMeshComponent;
	DeformerInstance.Init(InDeformerAsset, InSkelMeshComponent);
}

void UMLDeformerComponent::Activate(bool bReset)
{
	// If we haven't pointed to some skeletal mesh component to use, then try to find one on the actor.
	USkeletalMeshComponent* SkelMeshComponentToUse = SkelMeshComponent;
	if (SkelMeshComponentToUse == nullptr)
	{
		AActor* Actor = Cast<AActor>(GetOuter());
		SkelMeshComponent = Actor->FindComponentByClass<USkeletalMeshComponent>();
	}

	SetupComponent(DeformerAsset, SkelMeshComponent);
}

void UMLDeformerComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (TickType != ELevelTick::LEVELTICK_PauseTick)
	{
		// Update the deformer, which runs the inference.
		DeformerInstance.Update();
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}
