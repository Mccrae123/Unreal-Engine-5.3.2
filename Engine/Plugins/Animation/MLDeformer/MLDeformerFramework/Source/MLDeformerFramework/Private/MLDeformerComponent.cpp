// Copyright Epic Games, Inc. All Rights Reserved.

#include "MLDeformerComponent.h"
#include "MLDeformerAsset.h"
#include "MLDeformerModelInstance.h"
#include "MLDeformerModel.h"
#include "Components/SkeletalMeshComponent.h"

UMLDeformerComponent::UMLDeformerComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bTickInEditor = true;
	bAutoActivate = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bCanEverTick = true;
}

void UMLDeformerComponent::Init()
{
	// If there is no deformer asset linked, release what we currently have.
	if (DeformerAsset == nullptr)
	{
		ModelInstance = nullptr;
		return;
	}

	// Try to initialize the deformer model.
	UMLDeformerModel* Model = DeformerAsset->GetModel();
	if (Model)
	{
		if (ModelInstance)
		{
			ModelInstance->Release();
		}
		ModelInstance = Model->CreateModelInstance(this);
		ModelInstance->SetModel(Model);
		ModelInstance->Init(SkelMeshComponent);
		Model->PostMLDeformerComponentInit(ModelInstance);
	}
	else
	{
		ModelInstance = nullptr;
		UE_LOG(LogMLDeformer, Warning, TEXT("ML Deformer component on '%s' has a deformer asset that has no ML model setup."), *GetOuter()->GetName());
	}
}

void UMLDeformerComponent::SetupComponent(UMLDeformerAsset* InDeformerAsset, USkeletalMeshComponent* InSkelMeshComponent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UMLDeformerComponent::SetupComponent)

	if (InSkelMeshComponent)
	{		
		AddTickPrerequisiteComponent(InSkelMeshComponent);
	}

	DeformerAsset = InDeformerAsset;
	SkelMeshComponent = InSkelMeshComponent;

	// Initialize and make sure we have a model instance.
	RemoveNeuralNetworkModifyDelegate();
	Init();
	AddNeuralNetworkModifyDelegate();
}

void UMLDeformerComponent::AddNeuralNetworkModifyDelegate()
{
	if (DeformerAsset == nullptr)
	{
		return;
	}

	UMLDeformerModel* Model = DeformerAsset->GetModel();
	if (Model)
	{
		NeuralNetworkModifyDelegateHandle = Model->GetNeuralNetworkModifyDelegate().AddLambda
		(
			([this]()
			{
				Init();
			})
		);
	}
}

void UMLDeformerComponent::RemoveNeuralNetworkModifyDelegate()
{
	if (DeformerAsset && 
		NeuralNetworkModifyDelegateHandle != FDelegateHandle() && 
		DeformerAsset->GetModel())
	{
		DeformerAsset->GetModel()->GetNeuralNetworkModifyDelegate().Remove(NeuralNetworkModifyDelegateHandle);
	}
	
	NeuralNetworkModifyDelegateHandle = FDelegateHandle();
}

void UMLDeformerComponent::BeginDestroy()
{
	RemoveNeuralNetworkModifyDelegate();
	Super::BeginDestroy();
}

void UMLDeformerComponent::Activate(bool bReset)
{
	// If we haven't pointed to some skeletal mesh component to use, then try to find one on the actor.
	if (SkelMeshComponent == nullptr)
	{
		// First search for a skeletal mesh component with the expected number of vertices.
		// This will try to find a skeletal mesh with the same number of vertices.
		const UMLDeformerModel* Model = DeformerAsset ? DeformerAsset->GetModel() : nullptr;
		const int32 NumModelVertices = Model ? Model->GetVertexMap().Num() : -1;
		if (NumModelVertices > 0)
		{
			// Get a list of all skeletal mesh components on the actor.
			TArray<USkeletalMeshComponent*> Components;
			AActor* Actor = Cast<AActor>(GetOuter());
			Actor->GetComponents<USkeletalMeshComponent>(Components);
		
			// Find a component that uses a mesh with the same vertex count.
			for (USkeletalMeshComponent* Component : Components)
			{
				const USkeletalMesh* SkeletalMesh = Component->GetSkeletalMeshAsset();
				const FSkeletalMeshRenderData* RenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;
				const int32 NumComponentVertices = RenderData && RenderData->LODRenderData.IsValidIndex(0) ? RenderData->LODRenderData[0].GetNumVertices() : -1;

				if (NumComponentVertices == NumModelVertices)
				{
					SkelMeshComponent = Component;
					break;
				}
			}
		}
	}

	if (SkelMeshComponent == nullptr)
	{
		// Fall back to the first skeletal mesh component.
		const AActor* Actor = Cast<AActor>(GetOuter());
		SkelMeshComponent = Actor->FindComponentByClass<USkeletalMeshComponent>();
	}

	SetupComponent(DeformerAsset, SkelMeshComponent);
}

void UMLDeformerComponent::Deactivate()
{
	RemoveNeuralNetworkModifyDelegate();
	ModelInstance = nullptr;
}

void UMLDeformerComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (TickType != ELevelTick::LEVELTICK_PauseTick)
	{
		if (ModelInstance &&
			SkelMeshComponent && 
			SkelMeshComponent->GetPredictedLODLevel() == 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UMLDeformerComponent::TickComponent)
			ModelInstance->Tick(DeltaTime, Weight);
		}
	}
}

#if WITH_EDITOR
	void UMLDeformerComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
	{
		const FProperty* Property = PropertyChangedEvent.Property;
		if (Property == nullptr)
		{
			return;
		}

		if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMLDeformerComponent, DeformerAsset))
		{
			RemoveNeuralNetworkModifyDelegate();
			Init();
			AddNeuralNetworkModifyDelegate();
		}
	}
#endif
