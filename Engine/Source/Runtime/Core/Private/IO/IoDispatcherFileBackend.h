// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IO/IoDispatcherPrivate.h"
#include "IO/IoStore.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Stats/Stats.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/Runnable.h"
#include "Misc/AES.h"

class IMappedFileHandle;

struct FFileIoStoreContainerFile
{
	uint64 FileHandle = 0;
	uint64 FileSize = 0;
	uint64 CompressionBlockSize = 0;
	TArray<FName> CompressionMethods;
	TArray<FIoStoreTocCompressedBlockEntry> CompressionBlocks;
	FString FilePath;
	TUniquePtr<IMappedFileHandle> MappedFileHandle;
	FGuid EncryptionKeyGuid;
	FAES::FAESKey EncryptionKey;
	EIoContainerFlags ContainerFlags;
	TArray<FSHAHash> BlockSignatureHashes;
};

struct FFileIoStoreBuffer
{
	FFileIoStoreBuffer* Next = nullptr;
	uint8* Memory = nullptr;
};

class FFileIoStoreBufferAllocator
{
public:
	FFileIoStoreBuffer* AllocBuffer();
};

struct FFileIoStoreBlockKey
{
	union
	{
		struct
		{
			uint32 FileIndex;
			uint32 BlockIndex;
		};
		uint64 Hash;
	};
		

	friend bool operator==(const FFileIoStoreBlockKey& A, const FFileIoStoreBlockKey& B)
	{
		return A.Hash == B.Hash;
	}

	friend uint32 GetTypeHash(const FFileIoStoreBlockKey& Key)
	{
		return GetTypeHash(Key.Hash);
	}
};

struct FFileIoStoreBlockScatter
{
	FIoRequestImpl* Request = nullptr;
	uint64 DstOffset;
	uint64 SrcOffset;
	uint64 Size;
};

struct FFileIoStoreCompressionContext
{
	FFileIoStoreCompressionContext* Next = nullptr;
	uint64 UncompressedBufferSize = 0;
	uint8* UncompressedBuffer = nullptr;
};

struct FFileIoStoreCompressedBlock
{
	FFileIoStoreCompressedBlock* Next = nullptr;
	FFileIoStoreBlockKey Key;
	FName CompressionMethod;
	uint64 RawOffset;
	uint32 UncompressedSize;
	uint32 CompressedSize;
	uint32 RawSize;
	uint32 RawBlocksCount = 0;
	uint32 UnfinishedRawBlocksCount = 0;
	struct FFileIoStoreRawBlock* SingleRawBlock;
	TArray<FFileIoStoreBlockScatter, TInlineAllocator<16>> ScatterList;
	FFileIoStoreCompressionContext* CompressionContext = nullptr;
	uint8* CompressedDataBuffer = nullptr;
	FAES::FAESKey EncryptionKey;
	const FSHAHash* SignatureHash = nullptr;
};

struct FFileIoStoreRawBlock
{
	enum EFlags
	{
		None = 0,
		Cacheable = 1
	};

	FFileIoStoreRawBlock* Next = nullptr;
	FFileIoStoreBlockKey Key;
	uint64 FileHandle = uint64(-1);
	uint64 Offset = uint64(-1);
	uint64 Size = uint64(-1);
	FFileIoStoreBuffer* Buffer = nullptr;
	TArray<FFileIoStoreCompressedBlock*, TInlineAllocator<4>> CompressedBlocks;
	uint32 RefCount = 0;
	uint8 Flags = None;
};

struct FFileIoStoreResolvedRequest
{
	FIoRequestImpl* Request;
	uint64 ResolvedOffset;
	uint64 ResolvedSize;
};

class FFileIoStoreEncryptionKeys
{
public:
	using FKeyRegisteredCallback = TFunction<void(const FGuid&, const FAES::FAESKey&)>;

	FFileIoStoreEncryptionKeys();
	~FFileIoStoreEncryptionKeys();

	bool GetEncryptionKey(const FGuid& Guid, FAES::FAESKey& OutKey) const;
	void SetKeyRegisteredCallback(FKeyRegisteredCallback&& Callback)
	{
		KeyRegisteredCallback = Callback;
	}

private:
	void RegisterEncryptionKey(const FGuid& Guid, const FAES::FAESKey& Key);

	TMap<FGuid, FAES::FAESKey> EncryptionKeysByGuid;
	mutable FCriticalSection EncryptionKeysCritical;
	FKeyRegisteredCallback KeyRegisteredCallback;
};

class FFileIoStoreReader
{
public:
	FFileIoStoreReader(FFileIoStoreImpl& InPlatformImpl);
	FIoStatus Initialize(const FIoStoreEnvironment& Environment);
	bool DoesChunkExist(const FIoChunkId& ChunkId) const;
	TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const;
	const FIoOffsetAndLength* Resolve(const FIoChunkId& ChunkId) const;
	const FFileIoStoreContainerFile& GetContainerFile() const { return ContainerFile; }
	IMappedFileHandle* GetMappedContainerFileHandle();
	const FIoContainerId& GetContainerId() const { return ContainerId; }
	int32 GetOrder() const { return Order; }
	bool IsEncrypted() const { return EnumHasAnyFlags(ContainerFile.ContainerFlags, EIoContainerFlags::Encrypted); }
	bool IsSigned() const { return EnumHasAnyFlags(ContainerFile.ContainerFlags, EIoContainerFlags::Signed); }
	const FGuid& GetEncryptionKeyGuid() const { return ContainerFile.EncryptionKeyGuid; }
	void SetEncryptionKey(const FAES::FAESKey& Key) { ContainerFile.EncryptionKey = Key; }
	const FAES::FAESKey& GetEncryptionKey() const { return ContainerFile.EncryptionKey; }

private:
	FFileIoStoreImpl& PlatformImpl;

	TMap<FIoChunkId, FIoOffsetAndLength> Toc;
	FFileIoStoreContainerFile ContainerFile;
	FIoContainerId ContainerId;
	int32 Order;
};

class FFileIoStore
	: public FRunnable
{
public:
	FFileIoStore(FIoDispatcherEventQueue& InEventQueue, FIoSignatureErrorEvent& InSignatureErrorEvent);
	~FFileIoStore();
	TIoStatusOr<FIoContainerId> Mount(const FIoStoreEnvironment& Environment);
	EIoStoreResolveResult Resolve(FIoRequestImpl* Request);
	bool DoesChunkExist(const FIoChunkId& ChunkId) const;
	TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const;
	bool ReadPendingBlock();
	void ProcessCompletedBlocks(const bool bIsMultithreaded);
	TIoStatusOr<FIoMappedRegion> OpenMapped(const FIoChunkId& ChunkId, const FIoReadOptions& Options);
	void FlushReads()
	{ 
		PlatformImpl.FlushReads();
	}

	static bool IsValidEnvironment(const FIoStoreEnvironment& Environment);

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	class FDecompressAsyncTask
	{
	public:
		FDecompressAsyncTask(FFileIoStore& InOuter, FFileIoStoreCompressedBlock* InCompressedBlock)
			: Outer(InOuter)
			, CompressedBlock(InCompressedBlock)
		{

		}

		static FORCEINLINE TStatId GetStatId()
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FIoStoreDecompressTask, STATGROUP_TaskGraphTasks);
		}

		static ENamedThreads::Type GetDesiredThread();

		FORCEINLINE static ESubsequentsMode::Type GetSubsequentsMode()
		{
			return ESubsequentsMode::FireAndForget;
		}

		void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
		{
			Outer.ScatterBlock(CompressedBlock, true);
		}

	private:
		FFileIoStore& Outer;
		FFileIoStoreCompressedBlock* CompressedBlock;
	};

	void InitCache();
	void ReadBlocks(uint32 ReaderIndex, const FFileIoStoreResolvedRequest& ResolvedRequest);
	FFileIoStoreBuffer* AllocBuffer();
	void FreeBuffer(FFileIoStoreBuffer* Buffer);
	FFileIoStoreCompressionContext* AllocCompressionContext();
	void FreeCompressionContext(FFileIoStoreCompressionContext* CompressionContext);
	void ScatterBlock(FFileIoStoreCompressedBlock* CompressedBlock, bool bIsAsync);
	void AllocMemoryForRequest(FIoRequestImpl* Request);
	void FinalizeCompressedBlock(FFileIoStoreCompressedBlock* CompressedBlock);

	const uint64 ReadBufferSize;
	FIoDispatcherEventQueue& EventQueue;
	FIoSignatureErrorEvent& SignatureErrorEvent;
	FFileIoStoreImpl PlatformImpl;

	FRunnableThread* Thread;
	TAtomic<bool> bStopRequested{ false };
	mutable FRWLock IoStoreReadersLock;
	TArray<FFileIoStoreReader*> IoStoreReaders;
	TMap<FFileIoStoreBlockKey, FFileIoStoreCompressedBlock*> CompressedBlocksMap;
	TMap<FFileIoStoreBlockKey, FFileIoStoreRawBlock*> RawBlocksMap;
	uint8* BufferMemory = nullptr;
	FEvent* BufferAvailableEvent;
	FCriticalSection BuffersCritical;
	FFileIoStoreBuffer* FirstFreeBuffer = nullptr;
	FFileIoStoreCompressionContext* FirstFreeCompressionContext = nullptr;
	FEvent* PendingBlockEvent;
	FCriticalSection PendingBlocksCritical;
	FFileIoStoreRawBlock* PendingBlocksHead = nullptr;
	FFileIoStoreRawBlock* PendingBlocksTail = nullptr;
	FFileIoStoreCompressedBlock* ReadyForDecompressionHead = nullptr;
	FFileIoStoreCompressedBlock* ReadyForDecompressionTail = nullptr;
	FFileIoStoreRawBlock* ScheduledBlocksHead = nullptr;
	FFileIoStoreRawBlock* ScheduledBlocksTail = nullptr;
	FCriticalSection DecompressedBlocksCritical;
	FFileIoStoreCompressedBlock* FirstDecompressedBlock = nullptr;
	FFileIoStoreEncryptionKeys EncryptionKeys;
};
