// Copyright Epic Games, Inc. All Rights Reserved.

#include "IO/IoDispatcherFileBackend.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "HAL/PlatformFilemanager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/IConsoleManager.h"
#include "Async/AsyncWork.h"
#include "Async/MappedFileHandle.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"

TRACE_DECLARE_MEMORY_COUNTER(IoDispatcherTotalBytesRead, TEXT("IoDispatcher/TotalBytesRead"));
TRACE_DECLARE_MEMORY_COUNTER(IoDispatcherTotalBytesScattered, TEXT("IoDispatcher/TotalBytesScattered"));
TRACE_DECLARE_INT_COUNTER(IoDispatcherCacheHits, TEXT("IoDispatcher/CacheHits"));
TRACE_DECLARE_INT_COUNTER(IoDispatcherCacheMisses, TEXT("IoDispatcher/CacheMisses"));

//PRAGMA_DISABLE_OPTIMIZATION

int32 GIoDispatcherBufferSizeKB = 256;
static FAutoConsoleVariableRef CVar_IoDispatcherBufferSizeKB(
	TEXT("s.IoDispatcherBufferSizeKB"),
	GIoDispatcherBufferSizeKB,
	TEXT("IoDispatcher read buffer size (in kilobytes).")
);

int32 GIoDispatcherBufferAlignment = 4096;
static FAutoConsoleVariableRef CVar_IoDispatcherBufferAlignment(
	TEXT("s.IoDispatcherBufferAlignment"),
	GIoDispatcherBufferAlignment,
	TEXT("IoDispatcher read buffer alignment.")
);

int32 GIoDispatcherBufferMemoryMB = 8;
static FAutoConsoleVariableRef CVar_IoDispatcherBufferMemoryMB(
	TEXT("s.IoDispatcherBufferMemoryMB"),
	GIoDispatcherBufferMemoryMB,
	TEXT("IoDispatcher buffer memory size (in megabytes).")
);

int32 GIoDispatcherDecompressionWorkerCount = 4;
static FAutoConsoleVariableRef CVar_IoDispatcherDecompressionWorkerCount(
	TEXT("s.IoDispatcherDecompressionWorkerCount"),
	GIoDispatcherDecompressionWorkerCount,
	TEXT("IoDispatcher decompression worker count.")
);

int32 GIoDispatcherCacheSizeMB = 0;
static FAutoConsoleVariableRef CVar_IoDispatcherCacheSizeMB(
	TEXT("s.IoDispatcherCacheSizeMB"),
	GIoDispatcherCacheSizeMB,
	TEXT("IoDispatcher cache memory size (in megabytes).")
);

class FMappedFileProxy final : public IMappedFileHandle
{
public:
	FMappedFileProxy(IMappedFileHandle* InSharedMappedFileHandle, uint64 InSize)
		: IMappedFileHandle(InSize)
		, SharedMappedFileHandle(InSharedMappedFileHandle)
	{
		check(InSharedMappedFileHandle != nullptr);
	}

	virtual ~FMappedFileProxy() { }

	virtual IMappedFileRegion* MapRegion(int64 Offset = 0, int64 BytesToMap = MAX_int64, bool bPreloadHint = false) override
	{
		return SharedMappedFileHandle->MapRegion(Offset, BytesToMap, bPreloadHint);
	}
private:
	IMappedFileHandle* SharedMappedFileHandle;
};

FFileIoStoreEncryptionKeys::FFileIoStoreEncryptionKeys()
{
	FCoreDelegates::GetRegisterEncryptionKeyMulticastDelegate().AddRaw(this, &FFileIoStoreEncryptionKeys::RegisterEncryptionKey);
}

FFileIoStoreEncryptionKeys::~FFileIoStoreEncryptionKeys()
{
	FCoreDelegates::GetRegisterEncryptionKeyMulticastDelegate().RemoveAll(this);
}

bool FFileIoStoreEncryptionKeys::GetEncryptionKey(const FGuid& Guid, FAES::FAESKey& OutKey) const
{
	OutKey.Reset();

	{
		FScopeLock _(&EncryptionKeysCritical);
		if (const FAES::FAESKey* ExistingKey = EncryptionKeysByGuid.Find(Guid))
		{
			OutKey = *ExistingKey;
			return OutKey.IsValid();
		}
	}

	if (!Guid.IsValid() && FCoreDelegates::GetPakEncryptionKeyDelegate().IsBound())
	{
		FCoreDelegates::GetPakEncryptionKeyDelegate().Execute(OutKey.Key);
		return OutKey.IsValid();
	}

	return false;
}

void FFileIoStoreEncryptionKeys::RegisterEncryptionKey(const FGuid& Guid, const FAES::FAESKey& Key)
{
	{
		FScopeLock _(&EncryptionKeysCritical);
		EncryptionKeysByGuid.Add(Guid, Key);
	}

	KeyRegisteredCallback(Guid, Key);
}

void FFileIoStoreBufferAllocator::Initialize(uint64 MemorySize, uint64 BufferSize, uint32 BufferAlignment)
{
	uint64 BufferCount = MemorySize / BufferSize;
	MemorySize = BufferCount * BufferSize;
	BufferMemory = reinterpret_cast<uint8*>(FMemory::Malloc(MemorySize, BufferAlignment));
	for (uint64 BufferIndex = 0; BufferIndex < BufferCount; ++BufferIndex)
	{
		FFileIoStoreBuffer* Buffer = new FFileIoStoreBuffer();
		Buffer->Memory = BufferMemory + BufferIndex * BufferSize;
		Buffer->Next = FirstFreeBuffer;
		FirstFreeBuffer = Buffer;
	}
}

FFileIoStoreBuffer* FFileIoStoreBufferAllocator::AllocBuffer()
{
	FScopeLock Lock(&BuffersCritical);
	FFileIoStoreBuffer* Buffer = FirstFreeBuffer;
	if (Buffer)
	{
		FirstFreeBuffer = Buffer->Next;
		return Buffer;
	}
	return nullptr;
}

void FFileIoStoreBufferAllocator::FreeBuffer(FFileIoStoreBuffer* Buffer)
{
	check(Buffer);
	FScopeLock Lock(&BuffersCritical);
	Buffer->Next = FirstFreeBuffer;
	FirstFreeBuffer = Buffer;
}

FFileIoStoreBlockCache::FFileIoStoreBlockCache()
{
	CacheLruHead.LruNext = &CacheLruTail;
	CacheLruTail.LruPrev = &CacheLruHead;
}

FFileIoStoreBlockCache::~FFileIoStoreBlockCache()
{
	FCachedBlock* CachedBlock = CacheLruHead.LruNext;
	while (CachedBlock != &CacheLruTail)
	{
		FCachedBlock* Next = CachedBlock->LruNext;
		delete CachedBlock;
		CachedBlock = Next;
	}
	FMemory::Free(CacheMemory);
}

void FFileIoStoreBlockCache::Initialize(uint64 InCacheMemorySize, uint64 InReadBufferSize)
{
	ReadBufferSize = InReadBufferSize;
	uint64 CacheBlockCount = InCacheMemorySize / InReadBufferSize;
	if (CacheBlockCount)
	{
		InCacheMemorySize = CacheBlockCount * InReadBufferSize;
		CacheMemory = reinterpret_cast<uint8*>(FMemory::Malloc(InCacheMemorySize));
		FCachedBlock* Prev = &CacheLruHead;
		for (uint64 CacheBlockIndex = 0; CacheBlockIndex < CacheBlockCount; ++CacheBlockIndex)
		{
			FCachedBlock* CachedBlock = new FCachedBlock();
			CachedBlock->Key = uint64(-1);
			CachedBlock->Buffer = CacheMemory + CacheBlockIndex * InReadBufferSize;
			Prev->LruNext = CachedBlock;
			CachedBlock->LruPrev = Prev;
			Prev = CachedBlock;
		}
		Prev->LruNext = &CacheLruTail;
		CacheLruTail.LruPrev = Prev;
	}
}

bool FFileIoStoreBlockCache::Read(FFileIoStoreReadRequest* Block)
{
	bool bIsCacheableBlock = CacheMemory != nullptr && Block->bIsCacheable;
	if (!bIsCacheableBlock)
	{
		return false;
	}
	check(Block->Buffer);
	FCachedBlock* CachedBlock = nullptr;
	{
		FScopeLock Lock(&CriticalSection);
		CachedBlock = CachedBlocks.FindRef(Block->Key.Hash);
		if (CachedBlock)
		{
			CachedBlock->bLocked = true;

			CachedBlock->LruPrev->LruNext = CachedBlock->LruNext;
			CachedBlock->LruNext->LruPrev = CachedBlock->LruPrev;

			CachedBlock->LruPrev = &CacheLruHead;
			CachedBlock->LruNext = CacheLruHead.LruNext;

			CachedBlock->LruPrev->LruNext = CachedBlock;
			CachedBlock->LruNext->LruPrev = CachedBlock;
		}
	}

	if (!CachedBlock)
	{
		TRACE_COUNTER_INCREMENT(IoDispatcherCacheMisses);
		return false;
	}
	check(CachedBlock->Buffer);
	FMemory::Memcpy(Block->Buffer->Memory, CachedBlock->Buffer, ReadBufferSize);
	{
		FScopeLock Lock(&CriticalSection);
		CachedBlock->bLocked = false;
	}
	TRACE_COUNTER_INCREMENT(IoDispatcherCacheHits);
	return true;
}

void FFileIoStoreBlockCache::Store(const FFileIoStoreReadRequest* Block)
{
	bool bIsCacheableBlock = CacheMemory != nullptr && Block->bIsCacheable;
	if (!bIsCacheableBlock)
	{
		return;
	}
	check(Block->Buffer);
	check(Block->Buffer->Memory);
	FCachedBlock* BlockToReplace = nullptr;
	{
		FScopeLock Lock(&CriticalSection);
		BlockToReplace = CacheLruTail.LruPrev;
		while (BlockToReplace != &CacheLruHead && BlockToReplace->bLocked)
		{
			BlockToReplace = BlockToReplace->LruPrev;
		}
		if (BlockToReplace == &CacheLruHead)
		{
			return;
		}
		CachedBlocks.Remove(BlockToReplace->Key);
		BlockToReplace->bLocked = true;
		BlockToReplace->Key = Block->Key.Hash;

		BlockToReplace->LruPrev->LruNext = BlockToReplace->LruNext;
		BlockToReplace->LruNext->LruPrev = BlockToReplace->LruPrev;

		BlockToReplace->LruPrev = &CacheLruHead;
		BlockToReplace->LruNext = CacheLruHead.LruNext;

		BlockToReplace->LruPrev->LruNext = BlockToReplace;
		BlockToReplace->LruNext->LruPrev = BlockToReplace;
	}
	check(BlockToReplace);
	check(BlockToReplace->Buffer);
	FMemory::Memcpy(BlockToReplace->Buffer, Block->Buffer->Memory, ReadBufferSize);
	{
		FScopeLock Lock(&CriticalSection);
		BlockToReplace->bLocked = false;
		CachedBlocks.Add(BlockToReplace->Key, BlockToReplace);
	}
}

FFileIoStoreReadRequest* FFileIoStoreRequestQueue::Peek()
{
	for (int32 Priority = IoDispatcherPriority_Count - 1; Priority >= 0; --Priority)
	{
		FByPriority& Queue = ByPriority[Priority];
		if (Queue.Head)
		{
			return Queue.Head;
		}
	}
	return nullptr;
}

void FFileIoStoreRequestQueue::Pop(FFileIoStoreReadRequest& Request)
{
	check(Request.Priority < IoDispatcherPriority_Count);
	FByPriority& Queue = ByPriority[Request.Priority];
	check(Queue.Head == &Request);
	Queue.Head = Queue.Head->Next;
	if (!Queue.Head)
	{
		Queue.Tail = nullptr;
	}
	Request.Next = nullptr;
}

void FFileIoStoreRequestQueue::Push(FFileIoStoreReadRequest& Request)
{
	check(Request.Priority < IoDispatcherPriority_Count);
	FByPriority& Queue = ByPriority[Request.Priority];
	if (Queue.Tail)
	{
		Queue.Tail->Next = &Request;
		Queue.Tail = &Request;
	}
	else
	{
		Queue.Head = Queue.Tail = &Request;
	}
	Request.Next = nullptr;
}

FFileIoStoreReader::FFileIoStoreReader(FFileIoStoreImpl& InPlatformImpl)
	: PlatformImpl(InPlatformImpl)
{
}

FIoStatus FFileIoStoreReader::Initialize(const FIoStoreEnvironment& Environment)
{
	IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();

	TStringBuilder<256> ContainerFilePath;
	ContainerFilePath.Append(Environment.GetPath());

	TStringBuilder<256> TocFilePath;
	TocFilePath.Append(ContainerFilePath);

	UE_LOG(LogIoDispatcher, Display, TEXT("Reading toc: %s"), *TocFilePath);

	ContainerFilePath.Append(TEXT(".ucas"));
	TocFilePath.Append(TEXT(".utoc"));

	if (!PlatformImpl.OpenContainer(*ContainerFilePath, ContainerFile.FileHandle, ContainerFile.FileSize))
	{
		return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore container file '") << *ContainerFilePath << TEXT("'");
	}

	ContainerFile.FilePath = ContainerFilePath;

	TUniquePtr<FIoStoreTocResource> TocResourcePtr = MakeUnique<FIoStoreTocResource>();
	FIoStoreTocResource& TocResource = *TocResourcePtr;
	FIoStatus Status = FIoStoreTocResource::Read(*TocFilePath, EIoStoreTocReadOptions::ExcludeTocMeta, TocResource);
	if (!Status.IsOk())
	{
		return Status;
	}

	uint64 ContainerUncompressedSize = TocResource.Header.TocCompressedBlockEntryCount > 0
		? uint64(TocResource.Header.TocCompressedBlockEntryCount) * uint64(TocResource.Header.CompressionBlockSize)
		: ContainerFile.FileSize;

	Toc.Reserve(TocResource.Header.TocEntryCount);

	for (uint32 ChunkIndex = 0; ChunkIndex < TocResource.Header.TocEntryCount; ++ChunkIndex)
	{
		const FIoOffsetAndLength& ChunkOffsetLength = TocResource.ChunkOffsetLengths[ChunkIndex];
		if (ChunkOffsetLength.GetOffset() + ChunkOffsetLength.GetLength() > ContainerUncompressedSize)
		{
			return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC TocEntry out of container bounds while reading '") << *TocFilePath << TEXT("'");
		}

		Toc.Add(TocResource.ChunkIds[ChunkIndex], ChunkOffsetLength);
	}

	for (const FIoStoreTocCompressedBlockEntry& CompressedBlockEntry : TocResource.CompressionBlocks)
	{
		if (CompressedBlockEntry.GetOffset() + CompressedBlockEntry.GetCompressedSize() > ContainerFile.FileSize)
		{
			return (FIoStatus)(FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC TocCompressedBlockEntry out of container bounds while reading '") << *TocFilePath << TEXT("'"));
		}
	}

	ContainerFile.CompressionMethods	= MoveTemp(TocResource.CompressionMethods);
	ContainerFile.CompressionBlockSize	= TocResource.Header.CompressionBlockSize;
	ContainerFile.CompressionBlocks		= MoveTemp(TocResource.CompressionBlocks);
	ContainerFile.ContainerFlags		= TocResource.Header.ContainerFlags;
	ContainerFile.EncryptionKeyGuid		= TocResource.Header.EncryptionKeyGuid;
	ContainerFile.BlockSignatureHashes	= MoveTemp(TocResource.ChunkBlockSignatures);

	ContainerId = TocResource.Header.ContainerId;
	Order = Environment.GetOrder();
	return FIoStatus::Ok;
}

bool FFileIoStoreReader::DoesChunkExist(const FIoChunkId& ChunkId) const
{
	return Toc.Find(ChunkId) != nullptr;
}

TIoStatusOr<uint64> FFileIoStoreReader::GetSizeForChunk(const FIoChunkId& ChunkId) const
{
	const FIoOffsetAndLength* OffsetAndLength = Toc.Find(ChunkId);

	if (OffsetAndLength != nullptr)
	{
		return OffsetAndLength->GetLength();
	}
	else
	{
		return FIoStatus(EIoErrorCode::NotFound);
	}
}

const FIoOffsetAndLength* FFileIoStoreReader::Resolve(const FIoChunkId& ChunkId) const
{
	return Toc.Find(ChunkId);
}

IMappedFileHandle* FFileIoStoreReader::GetMappedContainerFileHandle()
{
	if (!ContainerFile.MappedFileHandle)
	{
		IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();
		ContainerFile.MappedFileHandle.Reset(Ipf.OpenMapped(*ContainerFile.FilePath));
	}

	check(ContainerFile.FileSize > 0);
	return new FMappedFileProxy(ContainerFile.MappedFileHandle.Get(), ContainerFile.FileSize);
}

FFileIoStore::FFileIoStore(FIoDispatcherEventQueue& InEventQueue, FIoSignatureErrorEvent& InSignatureErrorEvent, bool bInIsMultithreaded)
	: EventQueue(InEventQueue)
	, SignatureErrorEvent(InSignatureErrorEvent)
	, PlatformImpl(InEventQueue, BufferAllocator, BlockCache)
	, bIsMultithreaded(bInIsMultithreaded)
{
	EncryptionKeys.SetKeyRegisteredCallback([this](const FGuid& Guid, const FAES::FAESKey& Key)
	{
		FReadScopeLock _(IoStoreReadersLock);
		for (FFileIoStoreReader* Reader : UnorderedIoStoreReaders)
		{
			if (Reader->IsEncrypted() && !Reader->GetEncryptionKey().IsValid() && Reader->GetEncryptionKeyGuid() == Guid)
			{
				UE_LOG(LogIoDispatcher, Verbose, TEXT("Updating container '%d' with encryption key guid '%s'"), Reader->GetContainerId().Value(), *Guid.ToString());
				Reader->SetEncryptionKey(Key);
			}
		}
	});
}

FFileIoStore::~FFileIoStore()
{
	delete Thread;
}

void FFileIoStore::Initialize()
{
	ReadBufferSize = (GIoDispatcherBufferSizeKB > 0 ? uint64(GIoDispatcherBufferSizeKB) << 10 : 256 << 10);

	uint64 BufferMemorySize = uint64(GIoDispatcherBufferMemoryMB) << 20ull;
	uint64 BufferSize = uint64(GIoDispatcherBufferSizeKB) << 10ull;
	uint32 BufferAlignment = uint32(GIoDispatcherBufferAlignment);
	BufferAllocator.Initialize(BufferMemorySize, BufferSize, BufferAlignment);

	uint64 CacheMemorySize = uint64(GIoDispatcherCacheSizeMB) << 20ull;
	BlockCache.Initialize(CacheMemorySize, BufferSize);

	uint64 DecompressionContextCount = uint64(GIoDispatcherDecompressionWorkerCount > 0 ? GIoDispatcherDecompressionWorkerCount : 4);
	for (uint64 ContextIndex = 0; ContextIndex < DecompressionContextCount; ++ContextIndex)
	{
		FFileIoStoreCompressionContext* Context = new FFileIoStoreCompressionContext();
		Context->Next = FirstFreeCompressionContext;
		FirstFreeCompressionContext = Context;
	}

	Thread = FRunnableThread::Create(this, TEXT("IoService"), 0, TPri_AboveNormal);
}

TIoStatusOr<FIoContainerId> FFileIoStore::Mount(const FIoStoreEnvironment& Environment)
{
	TUniquePtr<FFileIoStoreReader> Reader(new FFileIoStoreReader(PlatformImpl));
	FIoStatus IoStatus = Reader->Initialize(Environment);
	if (!IoStatus.IsOk())
	{
		return IoStatus;
	}

	if (Reader->IsEncrypted())
	{
		FAES::FAESKey EncryptionKey;
		if (EncryptionKeys.GetEncryptionKey(Reader->GetEncryptionKeyGuid(), EncryptionKey))
		{
			Reader->SetEncryptionKey(EncryptionKey);
		}
		else
		{
			UE_LOG(LogIoDispatcher, Warning, TEXT("Mounting container '%s' with invalid encryption key"), *FPaths::GetBaseFilename(Environment.GetPath()));
		}
	}

	int32 InsertionIndex;
	FIoContainerId ContainerId = Reader->GetContainerId();
	{
		FWriteScopeLock _(IoStoreReadersLock);
		Reader->SetIndex(UnorderedIoStoreReaders.Num());
		InsertionIndex = Algo::UpperBound(OrderedIoStoreReaders, Reader.Get(), [](const FFileIoStoreReader* A, const FFileIoStoreReader* B)
		{
			if (A->GetOrder() != B->GetOrder())
			{
				return A->GetOrder() > B->GetOrder();
			}
			return A->GetIndex() > B->GetIndex();
		});
		FFileIoStoreReader* RawReader = Reader.Release();
		UnorderedIoStoreReaders.Add(RawReader);
		OrderedIoStoreReaders.Insert(RawReader, InsertionIndex);
	}
	return ContainerId;
}

EIoStoreResolveResult FFileIoStore::Resolve(FIoRequestImpl* Request)
{
	FReadScopeLock _(IoStoreReadersLock);
	FFileIoStoreResolvedRequest ResolvedRequest;
	ResolvedRequest.Request = Request;
	for (FFileIoStoreReader* Reader : OrderedIoStoreReaders)
	{
		if (const FIoOffsetAndLength* OffsetAndLength = Reader->Resolve(ResolvedRequest.Request->ChunkId))
		{
			uint64 RequestedOffset = ResolvedRequest.Request->Options.GetOffset();
			ResolvedRequest.ResolvedOffset = OffsetAndLength->GetOffset() + RequestedOffset;
			if (RequestedOffset > OffsetAndLength->GetLength())
			{
				ResolvedRequest.ResolvedSize = 0;
			}
			else
			{
				ResolvedRequest.ResolvedSize = FMath::Min(ResolvedRequest.Request->Options.GetSize(), OffsetAndLength->GetLength() - RequestedOffset);
			}

			Request->UnfinishedReadsCount = 0;
			if (ResolvedRequest.ResolvedSize > 0)
			{
				if (void* TargetVa = Request->Options.GetTargetVa())
				{
					ResolvedRequest.Request->IoBuffer = FIoBuffer(FIoBuffer::Wrap, TargetVa, ResolvedRequest.ResolvedSize);
				}
				else
				{
					ResolvedRequest.Request->IoBuffer.SetSize(ResolvedRequest.ResolvedSize);
				}

				ReadBlocks(*Reader, ResolvedRequest);
			}

			return IoStoreResolveResult_OK;
		}
	}

	return IoStoreResolveResult_NotFound;
}

bool FFileIoStore::DoesChunkExist(const FIoChunkId& ChunkId) const
{
	FReadScopeLock _(IoStoreReadersLock);
	for (FFileIoStoreReader* Reader : UnorderedIoStoreReaders)
	{
		if (Reader->DoesChunkExist(ChunkId))
		{
			return true;
		}
	}
	return false;
}

TIoStatusOr<uint64> FFileIoStore::GetSizeForChunk(const FIoChunkId& ChunkId) const
{
	FReadScopeLock _(IoStoreReadersLock);
	for (FFileIoStoreReader* Reader : OrderedIoStoreReaders)
	{
		TIoStatusOr<uint64> ReaderResult = Reader->GetSizeForChunk(ChunkId);
		if (ReaderResult.IsOk())
		{
			return ReaderResult;
		}
	}
	return FIoStatus(EIoErrorCode::NotFound);
}

bool FFileIoStore::IsValidEnvironment(const FIoStoreEnvironment& Environment)
{
	TStringBuilder<256> TocFilePath;
	TocFilePath.Append(Environment.GetPath());
	TocFilePath.Append(TEXT(".utoc"));
	return FPlatformFileManager::Get().GetPlatformFile().FileExists(*TocFilePath);
}

FAutoConsoleTaskPriority CPrio_IoDispatcherTaskPriority(
	TEXT("TaskGraph.TaskPriorities.IoDispatcherAsyncTasks"),
	TEXT("Task and thread priority for IoDispatcher decompression."),
	ENamedThreads::BackgroundThreadPriority, // if we have background priority task threads, then use them...
	ENamedThreads::NormalTaskPriority, // .. at normal task priority
	ENamedThreads::NormalTaskPriority // if we don't have background threads, then use normal priority threads at normal task priority instead
);

ENamedThreads::Type FFileIoStore::FDecompressAsyncTask::GetDesiredThread()
{
	return CPrio_IoDispatcherTaskPriority.Get();
}

void FFileIoStore::ScatterBlock(FFileIoStoreCompressedBlock* CompressedBlock, bool bIsAsync)
{
	LLM_SCOPE(ELLMTag::FileSystem);
	TRACE_CPUPROFILER_EVENT_SCOPE(IoDispatcherScatter);
	
	FFileIoStoreCompressionContext* CompressionContext = CompressedBlock->CompressionContext;
	check(CompressionContext);
	uint8* CompressedBuffer;
	if (CompressedBlock->RawBlocksCount > 1)
	{
		check(CompressedBlock->CompressedDataBuffer);
		CompressedBuffer = CompressedBlock->CompressedDataBuffer;
	}
	else
	{
		FFileIoStoreReadRequest* RawBlock = CompressedBlock->SingleRawBlock;
		check(CompressedBlock->RawOffset >= RawBlock->Offset);
		uint64 OffsetInBuffer = CompressedBlock->RawOffset - RawBlock->Offset;
		CompressedBuffer = RawBlock->Buffer->Memory + OffsetInBuffer;
	}
	if (CompressedBlock->SignatureHash)
	{
		FSHAHash BlockHash;
		FSHA1::HashBuffer(CompressedBuffer, CompressedBlock->RawSize, BlockHash.Hash);
		if (*CompressedBlock->SignatureHash != BlockHash)
		{
			FIoSignatureError Error;
			{
				FReadScopeLock _(IoStoreReadersLock);
				const FFileIoStoreReader& Reader = *UnorderedIoStoreReaders[CompressedBlock->Key.FileIndex];
				Error.ContainerName = FPaths::GetBaseFilename(Reader.GetContainerFile().FilePath);
				Error.BlockIndex = CompressedBlock->Key.BlockIndex;
				Error.ExpectedHash = *CompressedBlock->SignatureHash;
				Error.ActualHash = BlockHash;
			}

			UE_LOG(LogIoDispatcher, Warning, TEXT("Signature error detected in container '%s' at block index '%d'"), *Error.ContainerName, Error.BlockIndex);

			FScopeLock _(&SignatureErrorEvent.CriticalSection);
			if (SignatureErrorEvent.SignatureErrorDelegate.IsBound())
			{
				SignatureErrorEvent.SignatureErrorDelegate.Broadcast(Error);
			}
		}
	}
	if (CompressedBlock->EncryptionKey.IsValid())
	{
		FAES::DecryptData(CompressedBuffer, CompressedBlock->RawSize, CompressedBlock->EncryptionKey);
	}
	uint8* UncompressedBuffer;
	if (CompressedBlock->CompressionMethod.IsNone())
	{
		UncompressedBuffer = CompressedBuffer;
	}
	else
	{
		if (CompressionContext->UncompressedBufferSize < CompressedBlock->UncompressedSize)
		{
			FMemory::Free(CompressionContext->UncompressedBuffer);
			CompressionContext->UncompressedBuffer = reinterpret_cast<uint8*>(FMemory::Malloc(CompressedBlock->UncompressedSize));
			CompressionContext->UncompressedBufferSize = CompressedBlock->UncompressedSize;
		}
		UncompressedBuffer = CompressionContext->UncompressedBuffer;

		bool bFailed = !FCompression::UncompressMemory(CompressedBlock->CompressionMethod, UncompressedBuffer, int32(CompressedBlock->UncompressedSize), CompressedBuffer, int32(CompressedBlock->CompressedSize));
		check(!bFailed);
	}

	for (FFileIoStoreBlockScatter& Scatter : CompressedBlock->ScatterList)
	{
		FMemory::Memcpy(Scatter.Request->IoBuffer.Data() + Scatter.DstOffset, UncompressedBuffer + Scatter.SrcOffset, Scatter.Size);
	}

	if (bIsAsync)
	{
		FScopeLock Lock(&DecompressedBlocksCritical);
		CompressedBlock->Next = FirstDecompressedBlock;
		FirstDecompressedBlock = CompressedBlock;

		EventQueue.DispatcherNotify();
	}
}

void FFileIoStore::AllocMemoryForRequest(FIoRequestImpl* Request)
{
	LLM_SCOPE(ELLMTag::FileSystem);
	if (!Request->IoBuffer.Data())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AllocMemoryForRequest);
		Request->IoBuffer = FIoBuffer(Request->IoBuffer.DataSize());
	}
}

void FFileIoStore::FinalizeCompressedBlock(FFileIoStoreCompressedBlock* CompressedBlock)
{
	if (CompressedBlock->RawBlocksCount > 1)
	{
		check(CompressedBlock->CompressedDataBuffer);
		FMemory::Free(CompressedBlock->CompressedDataBuffer);
	}
	else
	{
		FFileIoStoreReadRequest* RawBlock = CompressedBlock->SingleRawBlock;
		check(RawBlock->RefCount > 0);
		if (--RawBlock->RefCount == 0)
		{
			check(RawBlock->Buffer);
			FreeBuffer(*RawBlock->Buffer);
			delete RawBlock;
		}
	}
	check(CompressedBlock->CompressionContext);
	FreeCompressionContext(CompressedBlock->CompressionContext);
	for (FFileIoStoreBlockScatter& Scatter : CompressedBlock->ScatterList)
	{
		TRACE_COUNTER_ADD(IoDispatcherTotalBytesScattered, Scatter.Size);
		check(Scatter.Request->UnfinishedReadsCount > 0);
		if (--Scatter.Request->UnfinishedReadsCount == 0)
		{
			if (!CompletedRequestsTail)
			{
				CompletedRequestsHead = CompletedRequestsTail = Scatter.Request;
			}
			else
			{
				CompletedRequestsTail->NextRequest = Scatter.Request;
				CompletedRequestsTail = Scatter.Request;
			}
			CompletedRequestsTail->NextRequest = nullptr;
		}
	}
	delete CompressedBlock;
}

FIoRequestImpl* FFileIoStore::GetCompletedRequests()
{
	LLM_SCOPE(ELLMTag::FileSystem);
	//TRACE_CPUPROFILER_EVENT_SCOPE(GetCompletedRequests);
	
	if (!bIsMultithreaded)
	{
		while (PlatformImpl.StartRequests(RequestQueue));
	}

	FFileIoStoreReadRequest* CompletedRequest = PlatformImpl.GetCompletedRequests();
	while (CompletedRequest)
	{
		++CompletedRequests;

		FFileIoStoreReadRequest* NextRequest = CompletedRequest->Next;

		TRACE_COUNTER_ADD(IoDispatcherTotalBytesRead, CompletedRequest->Size);

		if (CompletedRequest->bIsRawBlock)
		{
			check(CompletedRequest->Buffer);
			EIoDispatcherPriority Priority = CompletedRequest->Priority;
			check(Priority < IoDispatcherPriority_Count);
			FBlockMaps& BlockMaps = BlockMapsByPrority[Priority];

			BlockMaps.RawBlocksMap.Remove(CompletedRequest->Key);

			//TRACE_CPUPROFILER_EVENT_SCOPE(ProcessCompletedBlock);
			for (FFileIoStoreCompressedBlock* CompressedBlock : CompletedRequest->CompressedBlocks)
			{
				if (CompressedBlock->RawBlocksCount > 1)
				{
					//TRACE_CPUPROFILER_EVENT_SCOPE(HandleComplexBlock);
					if (!CompressedBlock->CompressedDataBuffer)
					{
						CompressedBlock->CompressedDataBuffer = reinterpret_cast<uint8*>(FMemory::Malloc(CompressedBlock->RawSize));
					}

					uint8* Src = CompletedRequest->Buffer->Memory;
					uint8* Dst = CompressedBlock->CompressedDataBuffer;
					uint64 CopySize = CompletedRequest->Size;
					int64 CompletedBlockOffsetInBuffer = int64(CompletedRequest->Offset) - int64(CompressedBlock->RawOffset);
					if (CompletedBlockOffsetInBuffer < 0)
					{
						Src -= CompletedBlockOffsetInBuffer;
						CopySize += CompletedBlockOffsetInBuffer;
					}
					else
					{
						Dst += CompletedBlockOffsetInBuffer;
					}
					uint64 CompressedBlockRawEndOffset = CompressedBlock->RawOffset + CompressedBlock->RawSize;
					uint64 CompletedBlockEndOffset = CompletedRequest->Offset + CompletedRequest->Size;
					if (CompletedBlockEndOffset > CompressedBlockRawEndOffset)
					{
						CopySize -= CompletedBlockEndOffset - CompressedBlockRawEndOffset;
					}
					FMemory::Memcpy(Dst, Src, CopySize);
					check(CompletedRequest->RefCount > 0);
					--CompletedRequest->RefCount;
				}

				check(CompressedBlock->UnfinishedRawBlocksCount > 0);
				if (--CompressedBlock->UnfinishedRawBlocksCount == 0)
				{
					BlockMaps.CompressedBlocksMap.Remove(CompressedBlock->Key);
					if (!ReadyForDecompressionTail)
					{
						ReadyForDecompressionHead = ReadyForDecompressionTail = CompressedBlock;
					}
					else
					{
						ReadyForDecompressionTail->Next = CompressedBlock;
						ReadyForDecompressionTail = CompressedBlock;
					}
					CompressedBlock->Next = nullptr;
				}
			}
			if (CompletedRequest->RefCount == 0)
			{
				FreeBuffer(*CompletedRequest->Buffer);
				delete CompletedRequest;
			}
		}
		else
		{
			check(!CompletedRequest->Buffer);
			check(CompletedRequest->DirectToRequest);
			FIoRequestImpl* CompletedIoDispatcherRequest = CompletedRequest->DirectToRequest;
			delete CompletedRequest;
			check(CompletedIoDispatcherRequest->UnfinishedReadsCount == 1);
			CompletedIoDispatcherRequest->UnfinishedReadsCount = 0;
			if (!CompletedRequestsTail)
			{
				CompletedRequestsHead = CompletedRequestsTail = CompletedIoDispatcherRequest;
			}
			else
			{
				CompletedRequestsTail->NextRequest = CompletedIoDispatcherRequest;
				CompletedRequestsTail = CompletedIoDispatcherRequest;
			}
			CompletedRequestsTail->NextRequest = nullptr;
		}
		
		CompletedRequest = NextRequest;
	}
	
	FFileIoStoreCompressedBlock* BlockToReap;
	{
		FScopeLock Lock(&DecompressedBlocksCritical);
		BlockToReap = FirstDecompressedBlock;
		FirstDecompressedBlock = nullptr;
	}

	while (BlockToReap)
	{
		FFileIoStoreCompressedBlock* Next = BlockToReap->Next;
		FinalizeCompressedBlock(BlockToReap);
		BlockToReap = Next;
	}

	FFileIoStoreCompressedBlock* BlockToDecompress = ReadyForDecompressionHead;
	while (BlockToDecompress)
	{
		FFileIoStoreCompressedBlock* Next = BlockToDecompress->Next;
		BlockToDecompress->CompressionContext = AllocCompressionContext();
		if (!BlockToDecompress->CompressionContext)
		{
			break;
		}
		for (FFileIoStoreBlockScatter& Scatter : BlockToDecompress->ScatterList)
		{
			AllocMemoryForRequest(Scatter.Request);
		}
		// Scatter block asynchronous when the block is compressed, encrypted or signed
		const bool bScatterAsync = bIsMultithreaded && (!BlockToDecompress->CompressionMethod.IsNone() || BlockToDecompress->EncryptionKey.IsValid() || BlockToDecompress->SignatureHash);
		if (bScatterAsync)
		{
			TGraphTask<FDecompressAsyncTask>::CreateTask().ConstructAndDispatchWhenReady(*this, BlockToDecompress);
		}
		else
		{
			ScatterBlock(BlockToDecompress, false);
			FinalizeCompressedBlock(BlockToDecompress);
		}
		BlockToDecompress = Next;
	}
	ReadyForDecompressionHead = BlockToDecompress;
	if (!ReadyForDecompressionHead)
	{
		ReadyForDecompressionTail = nullptr;
	}

	FIoRequestImpl* Result = CompletedRequestsHead;
	CompletedRequestsHead = CompletedRequestsTail = nullptr;
	return Result;
}

TIoStatusOr<FIoMappedRegion> FFileIoStore::OpenMapped(const FIoChunkId& ChunkId, const FIoReadOptions& Options)
{
	if (!FPlatformProperties::SupportsMemoryMappedFiles())
	{
		return FIoStatus(EIoErrorCode::Unknown, TEXT("Platform does not support memory mapped files"));
	}

	if (Options.GetTargetVa() != nullptr)
	{
		return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("Invalid read options"));
	}

	IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();

	FReadScopeLock _(IoStoreReadersLock);
	for (FFileIoStoreReader* Reader : OrderedIoStoreReaders)
	{
		if (const FIoOffsetAndLength* OffsetAndLength = Reader->Resolve(ChunkId))
		{
			uint64 ResolvedOffset = OffsetAndLength->GetOffset();
			uint64 ResolvedSize = FMath::Min(Options.GetSize(), OffsetAndLength->GetLength());
			
			const FFileIoStoreContainerFile& ContainerFile = Reader->GetContainerFile();
			
			IMappedFileHandle* MappedFileHandle = Reader->GetMappedContainerFileHandle();
			IMappedFileRegion* MappedFileRegion = nullptr;

			int32 BlockIndex = int32(ResolvedOffset / ContainerFile.CompressionBlockSize);
			const FIoStoreTocCompressedBlockEntry& CompressionBlockEntry = ContainerFile.CompressionBlocks[BlockIndex];
			const int64 BlockOffset = (int64)CompressionBlockEntry.GetOffset();
			check(BlockOffset > 0 && IsAligned(BlockOffset, FPlatformProperties::GetMemoryMappingAlignment()));

			MappedFileRegion = MappedFileHandle->MapRegion(BlockOffset + Options.GetOffset(), ResolvedSize);
			check(IsAligned(MappedFileRegion->GetMappedPtr(), FPlatformProperties::GetMemoryMappingAlignment()));
			
			return FIoMappedRegion { MappedFileHandle, MappedFileRegion };
		}
	}

	// We didn't find any entry for the ChunkId.
	return FIoStatus(EIoErrorCode::NotFound);
}

void FFileIoStore::OnNewPendingRequestsAdded()
{
	if (bIsMultithreaded)
	{
		EventQueue.ServiceNotify();
	}
	else
	{
		ProcessIncomingRequests();
	}
}

void FFileIoStore::ReadBlocks(const FFileIoStoreReader& Reader, const FFileIoStoreResolvedRequest& ResolvedRequest)
{
	/*TStringBuilder<256> ScopeName;
	ScopeName.Appendf(TEXT("ReadBlock %d"), BlockIndex);
	TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*ScopeName);*/

	UE_CLOG(Reader.IsEncrypted() && !Reader.GetEncryptionKey().IsValid(),
		LogIoDispatcher, Fatal, TEXT("Reading from encrypted container (ID = '%d') with invalid encryption key (Guid = '%s')"),
		Reader.GetContainerId().Value(),
		*Reader.GetEncryptionKeyGuid().ToString());
	const FFileIoStoreContainerFile& ContainerFile = Reader.GetContainerFile();
	const uint64 CompressionBlockSize = ContainerFile.CompressionBlockSize;
	const uint64 RequestEndOffset = ResolvedRequest.ResolvedOffset + ResolvedRequest.ResolvedSize;
	int32 RequestBeginBlockIndex = int32(ResolvedRequest.ResolvedOffset / CompressionBlockSize);
	int32 RequestEndBlockIndex = int32((RequestEndOffset - 1) / CompressionBlockSize);

	FFileIoStoreReadRequest* NewBlocksHead = nullptr;
	FFileIoStoreReadRequest* NewBlocksTail = nullptr;

	uint64 RequestStartOffsetInBlock = ResolvedRequest.ResolvedOffset - RequestBeginBlockIndex * CompressionBlockSize;
	uint64 RequestRemainingBytes = ResolvedRequest.ResolvedSize;
	uint64 OffsetInRequest = 0;
	for (int32 CompressedBlockIndex = RequestBeginBlockIndex; CompressedBlockIndex <= RequestEndBlockIndex; ++CompressedBlockIndex)
	{
		FFileIoStoreBlockKey CompressedBlockKey;
		CompressedBlockKey.FileIndex = Reader.GetIndex();
		CompressedBlockKey.BlockIndex = CompressedBlockIndex;
		EIoDispatcherPriority Priority = ResolvedRequest.Request->Priority;
		check(Priority < IoDispatcherPriority_Count);
		FBlockMaps& BlockMaps = BlockMapsByPrority[Priority];
		FFileIoStoreCompressedBlock* CompressedBlock = BlockMaps.CompressedBlocksMap.FindRef(CompressedBlockKey);
		if (!CompressedBlock)
		{
			CompressedBlock = new FFileIoStoreCompressedBlock();
			CompressedBlock->Key = CompressedBlockKey;
			CompressedBlock->EncryptionKey = Reader.GetEncryptionKey();
			BlockMaps.CompressedBlocksMap.Add(CompressedBlockKey, CompressedBlock);

			bool bCacheable = OffsetInRequest > 0 || RequestRemainingBytes < CompressionBlockSize;

			const FIoStoreTocCompressedBlockEntry& CompressionBlockEntry = ContainerFile.CompressionBlocks[CompressedBlockIndex];
			CompressedBlock->UncompressedSize = CompressionBlockEntry.GetUncompressedSize();
			CompressedBlock->CompressedSize = CompressionBlockEntry.GetCompressedSize();
			CompressedBlock->CompressionMethod = ContainerFile.CompressionMethods[CompressionBlockEntry.GetCompressionMethodIndex()];
			CompressedBlock->SignatureHash = Reader.IsSigned() ? &ContainerFile.BlockSignatureHashes[CompressedBlockIndex] : nullptr;
			uint64 RawOffset = CompressionBlockEntry.GetOffset();
			uint32 RawSize = Align(CompressionBlockEntry.GetCompressedSize(), FAES::AESBlockSize); // The raw blocks size is always aligned to AES blocks size
			CompressedBlock->RawOffset = RawOffset;
			CompressedBlock->RawSize = RawSize;
			const uint32 RawBeginBlockIndex = uint32(RawOffset / ReadBufferSize);
			const uint32 RawEndBlockIndex = uint32((RawOffset + RawSize - 1) / ReadBufferSize);
			const uint32 RawBlockCount = RawEndBlockIndex - RawBeginBlockIndex + 1;
			CompressedBlock->RawBlocksCount = RawBlockCount;
			check(RawBlockCount > 0);
			for (uint32 RawBlockIndex = RawBeginBlockIndex; RawBlockIndex <= RawEndBlockIndex; ++RawBlockIndex)
			{
				FFileIoStoreBlockKey RawBlockKey;
				RawBlockKey.BlockIndex = RawBlockIndex;
				RawBlockKey.FileIndex = Reader.GetIndex();

				FFileIoStoreReadRequest* RawBlock = BlockMaps.RawBlocksMap.FindRef(RawBlockKey);
				if (!RawBlock)
				{
					RawBlock = new FFileIoStoreReadRequest();
					RawBlock->bIsRawBlock = true;
					BlockMaps.RawBlocksMap.Add(RawBlockKey, RawBlock);

					RawBlock->Key = RawBlockKey;
					RawBlock->Priority = Priority;
					RawBlock->FileHandle = Reader.GetContainerFile().FileHandle;
					RawBlock->bIsCacheable = bCacheable;
					RawBlock->Offset = RawBlockIndex * ReadBufferSize;
					uint64 ReadSize = FMath::Min(ContainerFile.FileSize, RawBlock->Offset + ReadBufferSize) - RawBlock->Offset;
					RawBlock->Size = ReadSize;
					if (!NewBlocksTail)
					{
						NewBlocksHead = NewBlocksTail = RawBlock;
					}
					else
					{
						NewBlocksTail->Next = RawBlock;
						NewBlocksTail = RawBlock;
					}
				}
				if (RawBlockCount == 1)
				{
					CompressedBlock->SingleRawBlock = RawBlock;
				}
				RawBlock->CompressedBlocks.Add(CompressedBlock);
				++RawBlock->RefCount;
				++CompressedBlock->UnfinishedRawBlocksCount;
			}
		}
		check(CompressedBlock->UncompressedSize > RequestStartOffsetInBlock);
		uint64 RequestSizeInBlock = FMath::Min<uint64>(CompressedBlock->UncompressedSize - RequestStartOffsetInBlock, RequestRemainingBytes);
		check(OffsetInRequest + RequestSizeInBlock <= ResolvedRequest.Request->IoBuffer.DataSize());
		check(RequestStartOffsetInBlock + RequestSizeInBlock <= CompressedBlock->UncompressedSize);

		++ResolvedRequest.Request->UnfinishedReadsCount;
		FFileIoStoreBlockScatter& Scatter = CompressedBlock->ScatterList.AddDefaulted_GetRef();
		Scatter.Request = ResolvedRequest.Request;
		Scatter.DstOffset = OffsetInRequest;
		Scatter.SrcOffset = RequestStartOffsetInBlock;
		Scatter.Size = RequestSizeInBlock;

		RequestRemainingBytes -= RequestSizeInBlock;
		OffsetInRequest += RequestSizeInBlock;
		RequestStartOffsetInBlock = 0;
	}

	if (NewBlocksHead)
	{
		{
			FScopeLock Lock(&PendingRequestsCritical);
			if (!PendingRequestsTail)
			{
				PendingRequestsHead = NewBlocksHead;
			}
			else
			{
				PendingRequestsTail->Next = NewBlocksHead;
			}
			PendingRequestsTail = NewBlocksTail;
		}
		OnNewPendingRequestsAdded();
	}
}

void FFileIoStore::FreeBuffer(FFileIoStoreBuffer& Buffer)
{
	BufferAllocator.FreeBuffer(&Buffer);
	EventQueue.ServiceNotify();
}

FFileIoStoreCompressionContext* FFileIoStore::AllocCompressionContext()
{
	FFileIoStoreCompressionContext* Result = FirstFreeCompressionContext;
	if (Result)
	{
		FirstFreeCompressionContext = FirstFreeCompressionContext->Next;
	}
	return Result;
}

void FFileIoStore::FreeCompressionContext(FFileIoStoreCompressionContext* CompressionContext)
{
	CompressionContext->Next = FirstFreeCompressionContext;
	FirstFreeCompressionContext = CompressionContext;
}

void FFileIoStore::ProcessIncomingRequests()
{
	FFileIoStoreReadRequest* RequestToSchedule = nullptr;
	{
		FScopeLock Lock(&PendingRequestsCritical);
		RequestToSchedule = PendingRequestsHead;
		PendingRequestsHead = PendingRequestsTail = nullptr;
	}

	while (RequestToSchedule)
	{
		FFileIoStoreReadRequest* NextRequest = RequestToSchedule->Next;
		RequestToSchedule->Next = nullptr;
		RequestQueue.Push(*RequestToSchedule);
		++SubmittedRequests;

		RequestToSchedule = NextRequest;
	}

	UpdateAsyncIOMinimumPriority();
}

void FFileIoStore::UpdateAsyncIOMinimumPriority()
{
	EAsyncIOPriorityAndFlags NewAsyncIOMinimumPriority = AIOP_MIN;
	if (FFileIoStoreReadRequest* NextRequest = RequestQueue.Peek())
	{
		if (NextRequest->Priority == IoDispatcherPriority_Medium)
		{
			NewAsyncIOMinimumPriority = AIOP_Normal;
		}
		else if (NextRequest->Priority == IoDispatcherPriority_High)
		{
			NewAsyncIOMinimumPriority = AIOP_MAX;
		}
	}
	if (NewAsyncIOMinimumPriority != CurrentAsyncIOMinimumPriority)
	{
		//TRACE_BOOKMARK(TEXT("SetAsyncMinimumPrioirity(%d)"), NewAsyncIOMinimumPriority);
		FPlatformFileManager::Get().GetPlatformFile().SetAsyncMinimumPriority(NewAsyncIOMinimumPriority);
		CurrentAsyncIOMinimumPriority = NewAsyncIOMinimumPriority;
	}
}

bool FFileIoStore::Init()
{
	return true;
}

void FFileIoStore::Stop()
{
	bStopRequested = true;
	EventQueue.ServiceNotify();
}

uint32 FFileIoStore::Run()
{
	while (!bStopRequested)
	{
		ProcessIncomingRequests();
		if (!PlatformImpl.StartRequests(RequestQueue))
		{
			UpdateAsyncIOMinimumPriority();
			EventQueue.ServiceWait();
		}
	}
	return 0;
}
