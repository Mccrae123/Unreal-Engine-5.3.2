// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Class.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "LWComponentTypes.h"

class UEntitySubsystem;
struct FArchetypeData;
struct FLWComponentSystemExecutionContext;
struct FLWComponentData;
struct FArchetypeChunkIterator;
struct FLWComponentQuery;
struct FArchetypeChunkCollection;
struct FEntityView;

typedef TFunction< void(FLWComponentSystemExecutionContext& /*ExecutionContext*/) > FLWComponentSystemExecuteFunction;
typedef TFunction< bool(const FLWComponentSystemExecutionContext& /*ExecutionContext*/) > FLWComponentSystemChunkConditionFunction;


//////////////////////////////////////////////////////////////////////
//

// An opaque handle to an archetype
struct FArchetypeHandle final
{
	FArchetypeHandle() = default;
	bool IsValid() const { return DataPtr.IsValid(); }

	MASSENTITY_API bool operator==(const FArchetypeData* Other) const;
	bool operator==(const FArchetypeHandle& Other) const { return DataPtr == Other.DataPtr; }
	bool operator!=(const FArchetypeHandle& Other) const { return DataPtr != Other.DataPtr; }

	MASSENTITY_API friend uint32 GetTypeHash(const FArchetypeHandle& Instance);
private:
	FArchetypeHandle(const TSharedPtr<FArchetypeData>& InDataPtr)
	: DataPtr(InDataPtr)
	{}
	TSharedPtr<FArchetypeData> DataPtr;

	friend UEntitySubsystem;
	friend FArchetypeChunkCollection;
	friend FLWComponentQuery;
	friend FEntityView;
};


//////////////////////////////////////////////////////////////////////
//

/** A struct that converts an arbitrary array of entities of given Archetype into a sequence of continuous
 *  entity chunks. The goal is to have the user create an instance of this struct once and run through a bunch of
 *  systems. The runtime code usually uses FArchetypeChunkIterator to iterate on the chunk collection
 */
struct MASSENTITY_API FArchetypeChunkCollection
{
public:
	struct FChunkInfo
	{
		int32 ChunkIndex = INDEX_NONE;
		int32 SubchunkStart = 0;
		/** negative or 0-length means "all available entities within chunk" */
		int32 Length = 0;

		FChunkInfo() = default;
		explicit FChunkInfo(const int32 InChunkIndex, const int32 InSubchunkStart = 0, const int32 InLength = 0) : ChunkIndex(InChunkIndex), SubchunkStart(InSubchunkStart), Length(InLength) {}
		/** Note that we consider invalid-length chunks valid as long as ChunkIndex and SubchunkStart are valid */
		bool IsSet() const { return ChunkIndex != INDEX_NONE && SubchunkStart >= 0; }
	};
private:
	TArray<FChunkInfo> Chunks;
	/** entity indices indicated by SubChunks are only valid with given Archetype */
	FArchetypeHandle Archetype;

public:
	FArchetypeChunkCollection() = default;
	FArchetypeChunkCollection(const FArchetypeHandle& InArchetype, TConstArrayView<FLWEntity> InEntities);
	explicit FArchetypeChunkCollection(FArchetypeHandle& InArchetypeHandle);
	explicit FArchetypeChunkCollection(TSharedPtr<FArchetypeData>& InArchetype);

	TArrayView<const FChunkInfo> GetChunks() const { return Chunks; }
	const FArchetypeHandle& GetArchetype() const { return Archetype; }
	bool IsEmpty() const { return Chunks.Num() == 0 && Archetype.IsValid() == false; }
	bool IsSet() const { return Archetype.IsValid(); }
	void Reset() 
	{ 
		Archetype = FArchetypeHandle();
		Chunks.Reset();
	}

private:
	void GatherChunksFromArchetype(TSharedPtr<FArchetypeData>& InArchetype);
};

//////////////////////////////////////////////////////////////////////
//

/**
 *  The type used to iterate over given archetype's chunks, be it full, continuous chunks or sparse subchunks. It hides
 *  this details from the rest of the system.
 */
struct MASSENTITY_API FArchetypeChunkIterator
{
private:
	const FArchetypeChunkCollection& ChunkData;
	int32 CurrentChunkIndex = 0;

public:
	explicit FArchetypeChunkIterator(const FArchetypeChunkCollection& InChunkData) : ChunkData(InChunkData), CurrentChunkIndex(0) {}

	operator bool() const { return ChunkData.GetChunks().IsValidIndex(CurrentChunkIndex) && ChunkData.GetChunks()[CurrentChunkIndex].IsSet(); }
	FArchetypeChunkIterator& operator++() { ++CurrentChunkIndex; return *this; }

	const FArchetypeChunkCollection::FChunkInfo* operator->() const { check(bool(*this)); return &ChunkData.GetChunks()[CurrentChunkIndex]; }
	const FArchetypeChunkCollection::FChunkInfo& operator*() const { check(bool(*this)); return ChunkData.GetChunks()[CurrentChunkIndex]; }
};

//////////////////////////////////////////////////////////////////////
//
struct FInternalEntityHandle 
{
	FInternalEntityHandle() = default;
	FInternalEntityHandle(uint8* InChunkRawMemory, const int32 InIndexWithinChunk)
        : ChunkRawMemory(InChunkRawMemory), IndexWithinChunk(InIndexWithinChunk)
	{}
	bool IsValid() const { return ChunkRawMemory != nullptr && IndexWithinChunk != INDEX_NONE; }
	bool operator==(const FInternalEntityHandle & Other) const { return ChunkRawMemory == Other.ChunkRawMemory && IndexWithinChunk == Other.IndexWithinChunk; }

	uint8* ChunkRawMemory = nullptr;
	int32 IndexWithinChunk = INDEX_NONE;
};

typedef TArray<int32, TInlineAllocator<16>> FLWComponentIndicesMapping;
typedef TConstArrayView<int32> FLWComponentIndicesMappingView;
struct FLWRequirementIndicesMapping
{
	FLWRequirementIndicesMapping() = default;

	FLWComponentIndicesMapping EntityComponents;
	FLWComponentIndicesMapping ChunkComponents;
	FORCEINLINE bool IsEmpty() const
	{
		return EntityComponents.Num() == 0 || ChunkComponents.Num() == 0;
	}
};

template<typename T>
struct FLWComponentSorterOperator
{
	bool operator()(const T& A, const T& B) const
	{
		return (A.GetStructureSize() > B.GetStructureSize())
			|| (A.GetStructureSize() == B.GetStructureSize() && B.GetFName().FastLess(A.GetFName()));
	}
};
