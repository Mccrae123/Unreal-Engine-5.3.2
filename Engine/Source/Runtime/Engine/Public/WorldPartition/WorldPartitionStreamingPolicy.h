// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * WorldPartitionStreamingPolicy
 *
 * Base class for World Partition Runtime Streaming Policy
 *
 */

#pragma once

#include "Containers/Set.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartitionStreamingPolicy.generated.h"

class UWorldPartition;

struct FWorldPartitionStreamingSource
{
	FWorldPartitionStreamingSource(const FVector& InLocation, const FRotator& InRotation)
		: Location(InLocation)
		, Rotation(InRotation)
	{}

	FVector Location;
	FRotator Rotation;
};

UCLASS(Abstract, Within = WorldPartition)
class UWorldPartitionStreamingPolicy : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	virtual void UpdateStreamingState();
	virtual void LoadCells(const TSet<const UWorldPartitionRuntimeCell*>& ToLoadCells);
	virtual void UnloadCells(const TSet<const UWorldPartitionRuntimeCell*>& ToUnloadCells);
	virtual void LoadCell(const UWorldPartitionRuntimeCell* Cell) PURE_VIRTUAL(UWorldPartitionStreamingPolicy::LoadCell, );
	virtual void UnloadCell(const UWorldPartitionRuntimeCell* Cell) PURE_VIRTUAL(UWorldPartitionStreamingPolicy::UnloadCell, );
	FVector2D GetShowDebugDesiredFootprint(const FVector2D& CanvasSize);
	void ShowDebugInfo(class UCanvas* Canvas, const FVector2D& PartitionCanvasOffset, const FVector2D& PartitionCanvasSize);

#if WITH_EDITOR
	virtual TSubclassOf<class UWorldPartitionRuntimeCell> GetRuntimeCellClass() const PURE_VIRTUAL(UWorldPartitionStreamingPolicy::GetRuntimeCellClass, return UWorldPartitionRuntimeCell::StaticClass(); );
	virtual void PrepareForPIE() {}
	virtual void OnPreFixupForPIE(int32 InPIEInstanceID, FSoftObjectPath& ObjectPath) {}
#endif

protected:
	void UpdateStreamingSources();

	bool bIsServerLoadingDone;
	const UWorldPartition* WorldPartition;
	TSet<const UWorldPartitionRuntimeCell*> LoadedCells;
	TArray<FWorldPartitionStreamingSource> StreamingSources; // Streaming sources (local to world partition)
};