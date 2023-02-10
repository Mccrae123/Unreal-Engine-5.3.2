// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Elements/PCGDataFromActor.h"

#include "PCGTypedGetter.generated.h"

/** Builds a collection of landscape data from the selected actors. */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGGetLandscapeSettings : public UPCGDataFromActorSettings
{
	GENERATED_BODY()

public:
	UPCGGetLandscapeSettings();

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("Get Landscape Data")); }
	virtual FText GetNodeTooltipText() const override;
#endif

protected:
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	//~End UPCGSettings

public:
	//~Begin UPCGDataFromActorSettings interface
	virtual bool DataFilter(EPCGDataType InDataType) const { return !!(InDataType & EPCGDataType::Landscape); }
	//~End UPCGDataFromActorSettings
};

/** Builds a collection of spline data from the selected actors. */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGGetSplineSettings : public UPCGDataFromActorSettings
{
	GENERATED_BODY()

public:
	UPCGGetSplineSettings();

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("Get Spline Data")); }
	virtual FText GetNodeTooltipText() const override;
#endif

protected:
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	//~End UPCGSettings

public:
	//~Begin UPCGDataFromActorSettings interface
	virtual bool DataFilter(EPCGDataType InDataType) const { return !!(InDataType & EPCGDataType::PolyLine); }
	//~End UPCGDataFromActorSettings
};

/** Builds a collection of volume data from the selected actors. */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGGetVolumeSettings : public UPCGDataFromActorSettings
{
	GENERATED_BODY()

public:
	UPCGGetVolumeSettings();

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("Get Volume Data")); }
	virtual FText GetNodeTooltipText() const override;
#endif

protected:
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	//~End UPCGSettings

public:
	//~Begin UPCGDataFromActorSettings interface
	virtual bool DataFilter(EPCGDataType InDataType) const { return !!(InDataType & EPCGDataType::Volume); }
	//~End UPCGDataFromActorSettings
};
