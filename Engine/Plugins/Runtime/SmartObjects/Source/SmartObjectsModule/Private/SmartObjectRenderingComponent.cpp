// Copyright Epic Games, Inc. All Rights Reserved.

#include "SmartObjectRenderingComponent.h"
#include "DebugRenderSceneProxy.h"
#include "SmartObjectComponent.h"
#include "ObjectEditorUtils.h"

//----------------------------------------------------------------------//
// FSORenderingSceneProxy
//----------------------------------------------------------------------//
class FSORenderingSceneProxy final : public FDebugRenderSceneProxy
{
	typedef TPair<FVector, FVector> FVectorPair;
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	/** Initialization constructor. */
	FSORenderingSceneProxy(const USmartObjectRenderingComponent& InComponent)
		: FDebugRenderSceneProxy(&InComponent)
	{
		AActor* Owner = InComponent.GetOwner();
		if (Owner == nullptr)
		{
			return;
		}

		USmartObjectComponent* SOComp = Owner->FindComponentByClass<USmartObjectComponent>();
		if (SOComp == nullptr)
		{
			return;
		}

		const float DebugCylinderRadius = 40.f;
		const float DebugCylinderHalfHeight = 100.f;
		FColor DebugColor = FColor::Yellow;

		const FTransform OwnerLocalToWorld = SOComp->GetComponentTransform();
		for (int32 i = 0; i < SOComp->GetConfig().GetSlots().Num(); ++i)
		{
			TOptional<FTransform> Transform = SOComp->GetConfig().GetSlotTransform(OwnerLocalToWorld, FSmartObjectSlotIndex(i));
			if (!Transform.IsSet())
			{
				continue;
			}
#if WITH_EDITORONLY_DATA
			DebugColor = SOComp->GetConfig().GetSlots()[i].DEBUG_DrawColor;
#endif
			const FVector DebugPosition = Transform.GetValue().GetLocation();
			const FVector Direction = Transform.GetValue().GetRotation().GetForwardVector();
			Cylinders.Emplace(DebugPosition, DebugCylinderRadius, DebugCylinderHalfHeight, DebugColor);
			ArrowLines.Emplace(DebugPosition, DebugPosition + Direction * 2.0f * DebugCylinderRadius, DebugColor);
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View) && IsSelected();
		Result.bDynamicRelevance = true;
		// ideally the TranslucencyRelevance should be filled out by the material, here we do it conservative
		Result.bSeparateTranslucency = Result.bNormalTranslucency = IsShown(View);
		return Result;
	}
};

//----------------------------------------------------------------------//
// USmartObjectRenderingComponent
//----------------------------------------------------------------------//
USmartObjectRenderingComponent::USmartObjectRenderingComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Allows updating in game, while optimizing rendering for the case that it is not modified
	Mobility = EComponentMobility::Stationary;

	BodyInstance.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	bIsEditorOnly = true;

	SetGenerateOverlapEvents(false);
}

FPrimitiveSceneProxy* USmartObjectRenderingComponent::CreateSceneProxy()
{
	return new FSORenderingSceneProxy(*this);
}

FBoxSphereBounds USmartObjectRenderingComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBox BoundingBox(EForceInit::ForceInitToZero);

	if (HasAnyFlags(RF_BeginDestroyed) == false && GetAttachParent() != nullptr)
	{
		USmartObjectComponent* SOComp = nullptr;

		const AActor* Owner = GetOwner();
		if (Owner)
		{
			BoundingBox += Owner->GetActorLocation();
			SOComp = Owner->FindComponentByClass<USmartObjectComponent>();
		}

		if (SOComp)
		{
			BoundingBox += SOComp->GetSmartObjectBounds();
		}
	}

	return FBoxSphereBounds(BoundingBox);
}

#if WITH_EDITOR
void USmartObjectRenderingComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName SmartObjectsName = TEXT("SmartObjects");

	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property
		&& FObjectEditorUtils::GetCategoryFName(PropertyChangedEvent.Property) == SmartObjectsName)
	{
		MarkRenderStateDirty();
	}
}
#endif // WITH_EDITOR