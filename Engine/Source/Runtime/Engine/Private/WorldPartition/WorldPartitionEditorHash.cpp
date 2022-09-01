// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/WorldPartitionEditorHash.h"

UWorldPartitionEditorHash::UWorldPartitionEditorHash(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{}

#if WITH_EDITOR
UWorldPartitionEditorHash::FForEachIntersectingActorParams::FForEachIntersectingActorParams()
	: bIncludeSpatiallyLoadedActors(true)
	, bIncludeNonSpatiallyLoadedActors(true)
{}

int32 UWorldPartitionEditorHash::ForEachIntersectingActor(const FBox& Box, TFunctionRef<void(FWorldPartitionActorDesc*)> InOperation, bool bIncludeSpatiallyLoadedActors, bool bIncludeNonSpatiallyLoadedActors)
{
	UWorldPartitionEditorHash::FForEachIntersectingActorParams ForEachIntersectingActorParams;
	ForEachIntersectingActorParams.bIncludeSpatiallyLoadedActors = bIncludeSpatiallyLoadedActors;
	ForEachIntersectingActorParams.bIncludeNonSpatiallyLoadedActors = bIncludeNonSpatiallyLoadedActors;
	return ForEachIntersectingActor(Box, InOperation, ForEachIntersectingActorParams);
}
#endif