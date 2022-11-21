// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/PCGWorldData.h"

#include "PCGComponent.h"
#include "PCGHelpers.h"
#include "Data/PCGPointData.h"
#include "Elements/PCGSurfaceSampler.h"
#include "Elements/PCGVolumeSampler.h"
#include "Helpers/PCGBlueprintHelpers.h"

#include "Components/BrushComponent.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGWorldData)

void UPCGWorldVolumetricData::Initialize(UWorld* InWorld, const FBox& InBounds)
{
	Super::Initialize(InBounds, nullptr);
	TargetActor = nullptr;
	World = InWorld;
	check(World.IsValid());
}

bool UPCGWorldVolumetricData::SamplePoint(const FTransform& InTransform, const FBox& InBounds, FPCGPoint& OutPoint, UPCGMetadata* OutMetadata) const
{
	check(World.IsValid());

	FCollisionObjectQueryParams ObjectQueryParams(QueryParams.CollisionChannel);
	FCollisionShape CollisionShape = FCollisionShape::MakeBox(InBounds.GetExtent() * InTransform.GetScale3D());
	FCollisionQueryParams Params; // TODO: apply properties from the settings when/if they exist

	TArray<FOverlapResult> Overlaps;
	/*bool bOverlaps =*/ World->OverlapMultiByObjectType(Overlaps, InTransform.TransformPosition(InBounds.GetCenter()), InTransform.GetRotation(), ObjectQueryParams, CollisionShape, Params);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		// Skip invisible walls / triggers / volumes
		const UPrimitiveComponent* OverlappedComponent = Overlap.GetComponent();
		if (OverlappedComponent->IsA<UBrushComponent>())
		{
			continue;
		}

		// Skip "no collision" type actors
		if (!OverlappedComponent->IsQueryCollisionEnabled() || OverlappedComponent->GetCollisionResponseToChannel(QueryParams.CollisionChannel) != ECR_Block)
		{
			continue;
		}

		// Skip PCG-created objects optionally
		if (QueryParams.bIgnorePCGHits && OverlappedComponent->ComponentTags.Contains(PCGHelpers::DefaultPCGTag))
		{
			continue;
		}

		// Skip self-generated PCG objects optionally
		if (QueryParams.bIgnoreSelfHits && OriginatingComponent.IsValid() && OverlappedComponent->ComponentTags.Contains(OriginatingComponent->GetFName()))
		{
			continue;
		}

		// TODO: additional filtering?

		if (QueryParams.bSearchForOverlap)
		{
			OutPoint = FPCGPoint(InTransform, 1.0f, 0);
			UPCGBlueprintHelpers::SetSeedFromPosition(OutPoint);
			OutPoint.SetLocalBounds(InBounds);
			return true;
		}
		else
		{
			return false;
		}
	}

	// No valid hits found
	if (!QueryParams.bSearchForOverlap)
	{
		OutPoint = FPCGPoint(InTransform, 1.0f, 0);
		UPCGBlueprintHelpers::SetSeedFromPosition(OutPoint);
		OutPoint.SetLocalBounds(InBounds);
		return true;
	}
	else
	{
		return false;
	}
}

const UPCGPointData* UPCGWorldVolumetricData::CreatePointData(FPCGContext* Context, const FBox& InBounds) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UPCGWorldVolumetricData::CreatePointData);

	UPCGPointData* Data = NewObject<UPCGPointData>();
	Data->InitializeFromData(this);
	TArray<FPCGPoint>& Points = Data->GetMutablePoints();

	FBox EffectiveBounds = Bounds;
	if (InBounds.IsValid)
	{
		if (Bounds.IsValid)
		{
			EffectiveBounds = Bounds.Overlap(InBounds);
		}
		else
		{
			EffectiveBounds = InBounds;
		}
	}
	
	// Early out
	if (!EffectiveBounds.IsValid)
	{
		if (!Bounds.IsValid && !InBounds.IsValid)
		{
			UE_LOG(LogPCG, Error, TEXT("PCG World Volumetric Data cannot generate if there are no framing bounds"));
		}
		
		return Data;
	}
	
	PCGVolumeSampler::FVolumeSamplerSettings SamplerSettings;
	SamplerSettings.VoxelSize = VoxelSize;

	PCGVolumeSampler::SampleVolume(Context, this, SamplerSettings, Data, EffectiveBounds);
	UE_LOG(LogPCG, Verbose, TEXT("Volumetric world extracted %d points"), Data->GetPoints().Num());
	
	return Data;
}

UPCGSpatialData* UPCGWorldVolumetricData::CopyInternal() const
{
	UPCGWorldVolumetricData* NewVolumetricData = NewObject<UPCGWorldVolumetricData>();

	CopyBaseVolumeData(NewVolumetricData);

	NewVolumetricData->World = World;
	NewVolumetricData->OriginatingComponent = OriginatingComponent;
	NewVolumetricData->QueryParams = QueryParams;

	return NewVolumetricData;
}

/** World Ray Hit data implementation */
void UPCGWorldRayHitData::Initialize(UWorld* InWorld, const FBox& InBounds)
{
	World = InWorld;
	Bounds = InBounds;
}

bool UPCGWorldRayHitData::SamplePoint(const FTransform& InTransform, const FBox& InBounds, FPCGPoint& OutPoint, UPCGMetadata* OutMetadata) const
{
	check(World.IsValid());

	// Todo: consider prebuilding this
	FCollisionObjectQueryParams ObjectQueryParams(QueryParams.CollisionChannel);
	FCollisionQueryParams Params; // TODO: apply properties from the settings when/if they exist

	// Project the InTransform location on the ray origin plane
	const FVector PointLocation = InTransform.GetLocation();
	FVector RayStart = PointLocation - ((PointLocation - QueryParams.RayOrigin) | QueryParams.RayDirection) * QueryParams.RayDirection;
	FVector RayEnd = RayStart + QueryParams.RayDirection * QueryParams.RayLength;

	TArray<FHitResult> Hits;
	World->LineTraceMultiByObjectType(Hits, RayStart, RayEnd, ObjectQueryParams, Params);

	for (const FHitResult& Hit : Hits)
	{
		// Skip invisible walls / triggers / volumes
		const UPrimitiveComponent* HitComponent = Hit.GetComponent();
		if (HitComponent->IsA<UBrushComponent>())
		{
			continue;
		}

		// Skip "No collision" type actors
		if (!HitComponent->IsQueryCollisionEnabled() || HitComponent->GetCollisionResponseToChannel(QueryParams.CollisionChannel) != ECR_Block)
		{
			continue;
		}
		
		// Skip PCG-created objects optionally
		if (QueryParams.bIgnorePCGHits && HitComponent->ComponentTags.Contains(PCGHelpers::DefaultPCGTag))
		{
			continue;
		}

		// Skip self-generated PCG objects optionally
		if (QueryParams.bIgnoreSelfHits && OriginatingComponent.IsValid() && HitComponent->ComponentTags.Contains(OriginatingComponent->GetFName()))
		{
			continue;
		}

		// TODO: additional filtering?

		// Finally, fill in OutPoint - we're done
		// TODO: use normal to orient point?
		OutPoint = FPCGPoint(FTransform(Hit.ImpactNormal.Rotation(), Hit.ImpactPoint), 1.0f, 0);
		UPCGBlueprintHelpers::SetSeedFromPosition(OutPoint);

		// TODO: apply metadata
		// e.g. if hit on landscape -> metadata on landscape at that position
		// maybe material, etc.

		return true;
	}

	return false;
}

const UPCGPointData* UPCGWorldRayHitData::CreatePointData(FPCGContext* Context, const FBox& InBounds) const
{
	UPCGPointData* Data = NewObject<UPCGPointData>();
	Data->InitializeFromData(this);
	TArray<FPCGPoint>& Points = Data->GetMutablePoints();

	FBox EffectiveBounds = Bounds;
	if (InBounds.IsValid)
	{
		if (Bounds.IsValid)
		{
			EffectiveBounds = Bounds.Overlap(InBounds);
		}
		else
		{
			EffectiveBounds = InBounds;
		}
	}

	// Early out
	if (!EffectiveBounds.IsValid)
	{
		if (!Bounds.IsValid && !InBounds.IsValid)
		{
			UE_LOG(LogPCG, Error, TEXT("PCG World Volumetric Data cannot generate if there are no framing bounds"));
		}

		return Data;
	}

	PCGSurfaceSampler::FSurfaceSamplerSettings SamplerSettings;
	if (SamplerSettings.Initialize(nullptr, Context, EffectiveBounds))
	{
		PCGSurfaceSampler::SampleSurface(Context, this, SamplerSettings, Data);
	}

	return Data;
}

UPCGSpatialData* UPCGWorldRayHitData::CopyInternal() const
{
	UPCGWorldRayHitData* NewData = NewObject<UPCGWorldRayHitData>();

	CopyBaseSurfaceData(NewData);

	NewData->World = World;
	NewData->OriginatingComponent = OriginatingComponent;
	NewData->Bounds = Bounds;
	NewData->QueryParams = QueryParams;

	return NewData;
}
