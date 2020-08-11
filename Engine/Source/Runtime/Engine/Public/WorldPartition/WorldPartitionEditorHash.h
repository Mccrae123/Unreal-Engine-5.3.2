// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartitionEditorHash.generated.h"

class UWorldPartitionEditorCell;

UCLASS(Abstract, Config=Engine, Within = WorldPartition)
class ENGINE_API UWorldPartitionEditorHash : public UObject
{
	GENERATED_UCLASS_BODY()

public:
#if WITH_EDITOR
	virtual void Initialize() PURE_VIRTUAL(UWorldPartitionEditorHash::Initialize, return;);
	virtual void SetDefaultValues() PURE_VIRTUAL(UWorldPartitionEditorHash::SetDefaultValues, return;);
	virtual FName GetWorldPartitionEditorName() PURE_VIRTUAL(UWorldPartitionEditorHash::GetWorldPartitionEditorName, return FName(NAME_None););
	virtual void HashActor(FWorldPartitionActorDesc* InActorDesc) PURE_VIRTUAL(UWorldPartitionEditorHash::HashActor, ;);
	virtual void UnhashActor(FWorldPartitionActorDesc* InActorDesc) PURE_VIRTUAL(UWorldPartitionEditorHash::UnhashActor, ;);
	virtual int32 ForEachIntersectingActor(const FBox& Box, TFunctionRef<void(FWorldPartitionActorDesc*)> InOperation) PURE_VIRTUAL(UWorldPartitionEditorHash::ForEachIntForEachIntersectingActorersectingCell, return 0;);
	virtual int32 ForEachIntersectingCell(const FBox& Box, TFunctionRef<void(UWorldPartitionEditorCell*)> InOperation) PURE_VIRTUAL(UWorldPartitionEditorHash::ForEachIntersectingCell, return 0;);
	virtual int32 ForEachCell(TFunctionRef<void(UWorldPartitionEditorCell*)> InOperation) PURE_VIRTUAL(UWorldPartitionEditorHash::ForEachCell, return 0;);
	virtual UWorldPartitionEditorCell* GetAlwaysLoadedCell() PURE_VIRTUAL(UWorldPartitionEditorHash::GetAlwaysLoadedCell, return nullptr;);
	virtual bool GetCellAtLocation(const FVector& Location, FVector& Center, UWorldPartitionEditorCell*& Cell) const PURE_VIRTUAL(UWorldPartitionEditorHash::GetCellAtLocation, return false;);

	// Helpers
	inline int32 GetIntersectingActors(const FBox& Box, TArray<FWorldPartitionActorDesc*>& OutActors)
	{
		return ForEachIntersectingActor(Box, [&OutActors](FWorldPartitionActorDesc* ActorDesc)
		{
			OutActors.Add(ActorDesc);
		});
	}

	int32 GetIntersectingCells(const FBox& Box, TArray<UWorldPartitionEditorCell*>& OutCells)
	{
		return ForEachIntersectingCell(Box, [&OutCells](UWorldPartitionEditorCell* Cell)
		{
			OutCells.Add(Cell);
		});
	}

	int32 GetAllCells(TArray<UWorldPartitionEditorCell*>& OutCells)
	{
		return ForEachCell([&OutCells](UWorldPartitionEditorCell* Cell)
		{
			OutCells.Add(Cell);
		});
	}
#endif
};
