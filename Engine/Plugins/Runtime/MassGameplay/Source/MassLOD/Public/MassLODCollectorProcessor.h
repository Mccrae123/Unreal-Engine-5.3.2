// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "LWComponentTypes.h"
#include "MassLODTypes.h"
#include "MassLODManager.h"
#include "MassLODCollector.h"

#include "MassLODCollectorProcessor.generated.h"

USTRUCT()
struct FMassCollectorLODConfig
{
	GENERATED_BODY()

	/** Component Tag that will be used to associate LOD config */
	UPROPERTY(EditAnywhere, Category = "Mass|LOD", config, meta = (BaseStruct = "ComponentTag"))
	FInstancedStruct TagFilter;

	/** Set FOV Angles to define the limits between using Visible or Base LOD Distances */
	UPROPERTY(EditAnywhere, Category = "Mass|LOD", meta = (ClampMin = "0.0", ClampMax = "180.0", UIMin = "0.0", UIMax = "180.0"), config)
	float FOVAnglesToDriveVisibility = 45.0f;
	UPROPERTY(EditAnywhere, Category = "Mass|LOD", meta = (ClampMin = "0.0", UIMin = "0.0"), config)
	float BufferHysteresisOnFOVPercentage = 10.0f;

	/** Runtime data for matching the LOD config */
	TMassLODCollector<FMassRepresentationLODLogic> RepresentationLODCollector;
	FLWComponentQuery CloseEntityQuery;
	FLWComponentQuery FarEntityQuery_Conditional;
	TMassLODCollector<FMassSimulationLODLogic> SimulationLODCollector;
	FLWComponentQuery OnLODEntityQuery;
	FLWComponentQuery OffLODEntityQuery_Conditional;
	TMassLODCollector<FMassCombinedLODLogic> CombinedLODCollector;
	// Keep these in this order as we are creating ArrayView from the first 3 queries.
	FLWComponentQuery CloseAndOnLODEntityQuery;
	FLWComponentQuery CloseAndOffLODEntityQuery;
	FLWComponentQuery FarAndOnLODEntityQuery;
	FLWComponentQuery FarAndOffLODEntityQuery_Conditional;
};

/*
 * LOD collector which combines collection of LOD information for both Viewer and Simulation LODing when possible.
 */
UCLASS(meta = (DisplayName = "LOD Collector"))
class MASSLOD_API UMassLODCollectorProcessor : public UMassProcessor_LODBase
{
	GENERATED_BODY()

public:
	UMassLODCollectorProcessor();

protected:
	virtual void Initialize(UObject& Owner) override;
	virtual void ConfigureQueries() override;
	virtual void Execute(UMassEntitySubsystem& EntitySubsystem, FLWComponentSystemExecutionContext& Context) override;

	UPROPERTY(EditAnywhere, Category = "Mass|LOD", config)
	TArray<FMassCollectorLODConfig> LODConfigs;
};
