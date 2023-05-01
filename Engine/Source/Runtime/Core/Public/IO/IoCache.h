// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IO/IoStatus.h"
#include "IO/IoCancellationToken.h"
#include "Memory/MemoryFwd.h"
#include "Tasks/Task.h"
#include <atomic>

class FIoBuffer;
class FIoReadOptions;
struct FIoHash;

CORE_API DECLARE_LOG_CATEGORY_EXTERN(LogIoCache, Log, All);

/** Cache for binary blobs with a 20 byte cache key. */
class IIoCache
{
public:
	virtual ~IIoCache() = default;
	/** Returns whether the specified cache key is present in the cache. */
	virtual bool ContainsChunk(const FIoHash& Key) const = 0;

	/** Get the chunk associated with the specified cache key. */
	virtual UE::Tasks::TTask<TIoStatusOr<FIoBuffer>> GetChunk(
		const FIoHash& Key,
		const FIoReadOptions& Options,
		const FIoCancellationToken* CancellationToken) = 0;

	/** Insert a new chunk into the cache. */
	virtual FIoStatus PutChunk(const FIoHash& Key, FMemoryView Data) = 0;
};
