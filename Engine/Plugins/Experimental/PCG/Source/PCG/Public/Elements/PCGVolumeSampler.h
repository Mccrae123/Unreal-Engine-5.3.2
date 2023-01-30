// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Math/Box.h"
#include "PCGSettings.h"

#include "PCGPin.h"
#include "PCGVolumeSampler.generated.h"

class UPCGPointData;
class UPCGSpatialData;

namespace PCGVolumeSampler
{
	struct FVolumeSamplerSettings
	{
		FVector VoxelSize;
	};

	UPCGPointData* SampleVolume(FPCGContext* InContext, const UPCGSpatialData* InVolume, const FVolumeSamplerSettings& InSamplerSettings);
	UPCGPointData* SampleVolume(FPCGContext* InContext, const UPCGSpatialData* InVolume, const UPCGSpatialData* InBoundingShape, const FBox& InBounds, const FVolumeSamplerSettings& InSamplerSettings);

	void SampleVolume(FPCGContext* InContext, const UPCGSpatialData* InVolume, const UPCGSpatialData* InBoundingShape, const FBox& InBounds, const FVolumeSamplerSettings& InSamplerSettings, UPCGPointData* InOutputData);
}

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGVolumeSamplerSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Data", meta = (PCG_Overridable))
	FVector VoxelSize = FVector(100.0, 100.0, 100.0);

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("Volume Sampler")); }
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Sampler; }
#endif

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override { return Super::DefaultPointOutputPinProperties(); }
	
#if WITH_EDITOR
	virtual void ApplyDeprecationBeforeUpdatePins(UPCGNode* InOutNode, TArray<TObjectPtr<UPCGPin>>& InputPins, TArray<TObjectPtr<UPCGPin>>& OutputPins) override;
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface
};

class FPCGVolumeSamplerElement : public FSimplePCGElement
{
protected:
	bool ExecuteInternal(FPCGContext* Context) const override;
};
