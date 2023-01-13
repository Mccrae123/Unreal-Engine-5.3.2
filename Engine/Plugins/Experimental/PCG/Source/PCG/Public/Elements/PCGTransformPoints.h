// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGSettings.h"


#include "PCGTransformPoints.generated.h"

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGTransformPointsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGTransformPointsSettings();

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("TransformPoints")); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Spatial; }
#endif

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bApplyToAttribute = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition="bApplyToAttribute"))
	FName AttributeName;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FVector OffsetMin = FVector::Zero();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FVector OffsetMax = FVector::Zero();

	/** Set offset in world space */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bAbsoluteOffset = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FRotator RotationMin = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	FRotator RotationMax = FRotator::ZeroRotator;

	/** Set rotation directly instead of additively */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bAbsoluteRotation = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (AllowPreserveRatio))
	FVector ScaleMin = FVector::One();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (AllowPreserveRatio))
	FVector ScaleMax = FVector::One();

	/** Set scale directly instead of multiplicatively */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bAbsoluteScale = false;

	/** Scale uniformly on each axis. Uses the X component of ScaleMin and ScaleMax. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bUniformScale = true;

	/** Recompute the seed for each new point using its new location */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "!bApplyToAttribute"))
	bool bRecomputeSeed = false;
};

class FPCGTransformPointsElement : public FSimplePCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_2
#include "CoreMinimal.h"
#include "Elements/PCGPointProcessingElementBase.h"
#endif
