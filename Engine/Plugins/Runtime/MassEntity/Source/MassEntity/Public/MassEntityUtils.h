// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassProcessingTypes.h"

class UWorld;
class UMassEntitySubsystem;
struct FMassEntityHandle;
struct FArchetypeChunkCollection;

namespace UE::Mass::Utils
{

/** returns the current execution mode for the processors calculated from the world network mode */
MASSENTITY_API extern EProcessorExecutionFlags GetProcessorExecutionFlagsForWold(const UWorld& World);


MASSENTITY_API extern void CreateSparseChunks(const UMassEntitySubsystem& EntitySystem, const TConstArrayView<FMassEntityHandle> Entities, TArray<FArchetypeChunkCollection>& OutChunkCollections);

} // namespace UE::Mass::Utils