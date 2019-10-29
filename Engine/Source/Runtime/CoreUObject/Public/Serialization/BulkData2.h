// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/AsyncFileHandle.h"
#include "IO/IoDispatcher.h"

// Any place marked with BULKDATA_NOT_IMPLEMENTED_FOR_RUNTIME marks a method that we do not support
// but needs to exist in order for the code to compile. This pretty much means code that is editor only
// but is not compiled out at runtime etc.
// It can also be found inside of methods to show code paths that the old path implements but I have
// not been able to find any way to actually execute/test.
#define BULKDATA_NOT_IMPLEMENTED_FOR_RUNTIME PLATFORM_BREAK()

struct FOwnedBulkDataPtr;

/**
 * Represents an IO request from the BulkData streaming API.
 *
 * It functions pretty much the same as IAsyncReadRequest expect that it also holds
 * the file handle as well.
 *
 *
 */
class COREUOBJECT_API IBulkDataIORequest
{
public:
	virtual ~IBulkDataIORequest() {}

	virtual bool PollCompletion() const = 0;
	virtual bool WaitCompletion(float TimeLimitSeconds = 0.0f) const = 0;

	virtual uint8* GetReadResults() = 0;
	virtual int64 GetSize() const = 0;

	virtual void Cancel() = 0;
};

/**
 * @documentation @todo documentation
 */
class COREUOBJECT_API FBulkDataBase
{
public:
	using BulkDataRangeArray = TArray<FBulkDataBase*, TInlineAllocator<8>>;

	static void SetIODispatcher(FIoDispatcher* InIoDispatcher) { IoDispatcher = InIoDispatcher; }

public:
	using FileToken = uint64;
	static constexpr FileToken InvalidToken = 0;

	FBulkDataBase(const FBulkDataBase& Other) { *this = Other; }
	FBulkDataBase(FBulkDataBase&& Other);
	FBulkDataBase& operator=(const FBulkDataBase& Other);

	FBulkDataBase() = default;
	~FBulkDataBase();

protected:

	void Serialize(FArchive& Ar, UObject* Owner, int32 Index, bool bAttemptFileMapping, int32 ElementSize);

public:
	// Unimplemented:
	void* Lock(uint32 LockFlags);
	const void* LockReadOnly() const;
	void Unlock();
	bool IsLocked() const;

	void* Realloc(int64 InElementCount);
	void GetCopy(void** Dest, bool bDiscardInternalCopy);

	int64 GetBulkDataSize() const;

	void SetBulkDataFlags(uint32 BulkDataFlagsToSet);
	void ResetBulkDataFlags(uint32 BulkDataFlagsToSet);
	void ClearBulkDataFlags(uint32 BulkDataFlagsToClear);
	uint32 GetBulkDataFlags() const { return BulkDataFlags; }

	bool CanLoadFromDisk() const { return IsUsingIODispatcher() || Fallback.Token != InvalidToken; }

	bool IsStoredCompressedOnDisk() const;
	FName GetDecompressionFormat() const;

	bool IsBulkDataLoaded() const { return DataBuffer != nullptr; }

	// TODO: The flag tests could be inline if we fixed the header dependency issues (the flags are defined in Bulkdata.h at the moment)
	bool IsAvailableForUse() const;
	bool IsDuplicateNonOptional() const;
	bool IsOptional() const;
	bool IsInlined() const;
	bool InSeperateFile() const;
	bool IsSingleUse() const;
	bool IsMemoryMapped() const;
	bool IsUsingIODispatcher() const;

	IBulkDataIORequest* CreateStreamingRequest(EAsyncIOPriorityAndFlags Priority, FAsyncFileCallBack* CompleteCallback, uint8* UserSuppliedMemory) const;
	IBulkDataIORequest* CreateStreamingRequest(int64 OffsetInBulkData, int64 BytesToRead, EAsyncIOPriorityAndFlags Priority, FAsyncFileCallBack* CompleteCallback, uint8* UserSuppliedMemory) const;
	
	static IBulkDataIORequest* CreateStreamingRequestForRange(const BulkDataRangeArray& RangeArray, EAsyncIOPriorityAndFlags Priority, FAsyncFileCallBack* CompleteCallback);

	void RemoveBulkData();

	bool IsAsyncLoadingComplete() const { return true; }

	// Added for compatibility with the older BulkData system
	int64 GetBulkDataOffsetInFile() const;
	FString GetFilename() const;

public:
	// The following methods are for compatibility with SoundWave.cpp which assumes memory mapping.
	void ForceBulkDataResident(); // Is closer to MakeSureBulkDataIsLoaded in the old system but kept the name due to existing use
	FOwnedBulkDataPtr* StealFileMapping();

private:
	void LoadDataDirectly(void** DstBuffer);

	void SerializeDuplicateData(FArchive& Ar, FLinkerLoad* Linker, uint32& OutBulkDataFlags, int64& OutBulkDataSizeOnDisk, int64& OutBulkDataOffsetInFile);
	void SerializeBulkData(FArchive& Ar, void* DstBuffer, int64 DataLength);

	void AllocateData(SIZE_T SizeInBytes);
	void FreeData();

	FString ConvertFilenameFromFlags(const FString& Filename);

private:
	friend class FBulkDataIoDispatcherRequest;
	static FIoDispatcher* IoDispatcher;

	union
	{
		// Inline data or fallback path
		struct
		{
			uint64 BulkDataSize;
			FileToken Token;
			
		} Fallback;

		// For IODispatcher
		FIoChunkId ChunkID;	
	}; // Note that the union will end up being 16 bytes with padding
	
	void* DataBuffer = nullptr;
	uint32 BulkDataFlags = 0;
	mutable uint8 LockStatus = 0; // Mutable so that the read only lock can be const
};

/**
 * @documentation @todo documentation
 */
template<typename ElementType>
class COREUOBJECT_API FUntypedBulkData2 : public FBulkDataBase
{
	// In the older Bulkdata system the data was being loaded as if it was POD with the option to opt out
	// but nothing actually opted out. This check should help catch if any non-POD data was actually being
	// used or not.
	static_assert(TIsPODType<ElementType>::Value, "FUntypedBulkData2 is limited to POD types!");
public:
	void Serialize(FArchive& Ar, UObject* Owner, int32 Index, bool bAttemptFileMapping)
	{
		FBulkDataBase::Serialize(Ar, Owner, Index, bAttemptFileMapping, sizeof(ElementType));
	}

	int32 GetElementSize() const
	{
		return sizeof(ElementType);
	}

	ElementType* Lock(uint32 LockFlags)
	{
		return (ElementType*)FBulkDataBase::Lock(LockFlags);
	}

	const ElementType* LockReadOnly() const
	{
		return (const ElementType*)FBulkDataBase::LockReadOnly();
	}

	ElementType* Realloc(int64 InElementCount)
	{
		return (ElementType*)FBulkDataBase::Realloc(InElementCount);
	}
};

// Commonly used types
using FByteBulkData2 = FUntypedBulkData2<uint8>;
using FWordBulkData2 = FUntypedBulkData2<uint16>;
using FIntBulkData2 = FUntypedBulkData2<int32>;
using FFloatBulkData2 = FUntypedBulkData2<float>;
