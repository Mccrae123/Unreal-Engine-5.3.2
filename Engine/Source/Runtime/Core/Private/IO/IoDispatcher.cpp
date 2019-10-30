// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "IO/IoDispatcher.h"
#include "IO/IoStore.h"
#include "Async/MappedFileHandle.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CString.h"
#include "Misc/EventPool.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Misc/StringBuilder.h"
#include "Misc/LazySingleton.h"
#include "Misc/CoreDelegates.h"
#include "Trace/Trace.h"

#define IODISPATCHER_TRACE_ENABLED !UE_BUILD_SHIPPING

DEFINE_LOG_CATEGORY(LogIoDispatcher);

const FIoChunkId FIoChunkId::InvalidChunkId = FIoChunkId::CreateEmptyId();

#if !defined(PLATFORM_IMPLEMENTS_IO)
//////////////////////////////////////////////////////////////////////////

UE_TRACE_EVENT_BEGIN(IoDispatcher, BatchIssued, Always)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, BatchId)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(IoDispatcher, BatchResolved, Always)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, BatchId)
	UE_TRACE_EVENT_FIELD(uint64, TotalSize)
UE_TRACE_EVENT_END()

template <typename T, uint32 BlockSize = 128>
class TBlockAllocator
{
public:
	~TBlockAllocator()
	{
		FreeBlocks();
	}

	FORCEINLINE T* Alloc()
	{
		FScopeLock _(&CriticalSection);

		if (!NextFree)
		{
			//TODO: Virtual alloc
			FBlock* Block = new FBlock;
			
			for (int32 ElementIndex = 0; ElementIndex < BlockSize; ++ElementIndex)
			{
				FElement* Element	= &Block->Elements[ElementIndex];
				Element->Next		= NextFree;
				NextFree			= Element;
			}

			Block->Next	= Blocks;
			Blocks		= Block;
		}

		FElement* Element	= NextFree;
		NextFree			= Element->Next;
		
		++NumElements;

		return Element->Buffer.GetTypedPtr();
	}

	FORCEINLINE void Free(T* Ptr)
	{
		FScopeLock _(&CriticalSection);

		FElement* Element	= reinterpret_cast<FElement*>(Ptr);
		Element->Next		= NextFree;
		NextFree			= Element;

		--NumElements;
	}

	template <typename... ArgsType>
	T* Construct(ArgsType&&... Args)
	{
		return new(Alloc()) T(Forward<ArgsType>(Args)...);
	}

	void Destroy(T* Ptr)
	{
		Ptr->~T();
		Free(Ptr);
	}

	void Trim()
	{
		FScopeLock _(&CriticalSection);
		if (!NumElements)
		{
			FreeBlocks();
		}
	}

private:
	void FreeBlocks()
	{
		FBlock* Block = Blocks;
		while (Block)
		{
			FBlock* Tmp = Block;
			Block = Block->Next;
			delete Tmp;
		}

		Blocks		= nullptr;
		NextFree	= nullptr;
		NumElements = 0;
	}

	struct FElement
	{
		TTypeCompatibleBytes<T> Buffer;
		FElement* Next;
	};

	struct FBlock
	{
		FElement Elements[BlockSize];
		FBlock* Next = nullptr;
	};

	FBlock*				Blocks		= nullptr;
	FElement*			NextFree	= nullptr;
	int32				NumElements = 0;
	FCriticalSection	CriticalSection;
};

//////////////////////////////////////////////////////////////////////////

class FIoStoreReaderImpl
{
public:
	FIoStoreReaderImpl(FIoStoreEnvironment& InEnvironment)
		: Environment(InEnvironment)
	{
	}

	FIoStatus Open(FStringView UniqueId);

	TIoStatusOr<FIoBuffer> Lookup(const FIoChunkId& ChunkId)
	{
		const FIoStoreTocEntry* Entry = Toc.Find(ChunkId);

		if (!Entry)
		{
			return FIoStatus(EIoErrorCode::NotFound);
		}

		return FIoBuffer(FIoBuffer::Wrap, MappedRegion->GetMappedPtr() + Entry->GetOffset(), Entry->GetLength());
	}

	TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const
	{
		const FIoStoreTocEntry* Entry = Toc.Find(ChunkId);

		if (Entry != nullptr)
		{
			return Entry->GetLength();
		}
		else
		{
			return FIoStatus(EIoErrorCode::NotFound);
		}
	}

private:
	FIoStoreEnvironment&				Environment;
	FString								UniqueId;
	TMap<FIoChunkId, FIoStoreTocEntry>	Toc;
	TUniquePtr<IFileHandle>				ContainerFileHandle;
	TUniquePtr<IMappedFileHandle>		ContainerMappedFileHandle;
	TUniquePtr<IMappedFileRegion>		MappedRegion;
};

FIoStatus FIoStoreReaderImpl::Open(FStringView InUniqueId)
{
	IPlatformFile& Ipf = IPlatformFile::GetPlatformPhysical();

	UniqueId = InUniqueId.ToString();

	const FString& RootPath = Environment.GetRootPath();

	TStringBuilder<256> ContainerFilePath;
	ContainerFilePath.Append(RootPath);
	if (ContainerFilePath.LastChar() != '/')
		ContainerFilePath.Append(TEXT('/'));

	TStringBuilder<256> TocFilePath;
	TocFilePath.Append(ContainerFilePath);
	TocFilePath.Append(TEXT("Container.utoc"));

	ContainerFilePath.Append(TEXT("Container.ucas"));
	ContainerFileHandle.Reset(Ipf.OpenRead(*ContainerFilePath, /* allowwrite */ false));
	
	if (!ContainerFileHandle)
	{
		return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore container file '") << *ContainerFilePath << TEXT("'");
	}

	const uint64 ContainerSize = ContainerFileHandle->Size();

	ContainerMappedFileHandle.Reset(Ipf.OpenMapped(*ContainerFilePath));
	MappedRegion.Reset(ContainerMappedFileHandle->MapRegion());

	if (!ContainerMappedFileHandle)
	{
		return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to memory map IoStore container file '") << *ContainerFilePath << TEXT("'");
	}

	TUniquePtr<uint8[]> TocBuffer;
	bool bTocReadOk = false;

	{
		TUniquePtr<IFileHandle>	TocFileHandle(Ipf.OpenRead(*TocFilePath, /* allowwrite */ false));

		if (!TocFileHandle)
		{
			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore TOC file '") << *TocFilePath << TEXT("'");
		}

		const int64 TocSize = TocFileHandle->Size();
		TocBuffer = MakeUnique<uint8[]>(TocSize);
		bTocReadOk = TocFileHandle->Read(TocBuffer.Get(), TocSize);
	}

	if (!bTocReadOk)
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("Failed to read IoStore TOC file '") << *TocFilePath << TEXT("'");
	}

	const FIoStoreTocHeader* Header = reinterpret_cast<const FIoStoreTocHeader*>(TocBuffer.Get());

	if (!Header->CheckMagic())
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC header magic mismatch while reading '") << *TocFilePath << TEXT("'");
	}

	if (Header->TocHeaderSize != sizeof(FIoStoreTocHeader))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC header size mismatch while reading '") << *TocFilePath << TEXT("'");
	}

	if (Header->TocEntrySize != sizeof(FIoStoreTocEntry))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC entry size mismatch while reading '") << *TocFilePath << TEXT("'");
	}

	const FIoStoreTocEntry* Entry = reinterpret_cast<const FIoStoreTocEntry*>(TocBuffer.Get() + sizeof(FIoStoreTocHeader));
	uint32 EntryCount = Header->TocEntryCount;

	Toc.Reserve(EntryCount);

	while(EntryCount--)
	{
		if ((Entry->GetOffset() + Entry->GetLength()) > ContainerSize)
		{
			// TODO: add details
			return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC entry out of container bounds while reading '") << *TocFilePath << TEXT("'");
		}

		Toc.Add(Entry->ChunkId, *Entry);

		++Entry;
	}

	return FIoStatus::Ok;
}

FIoStoreReader::FIoStoreReader(FIoStoreEnvironment& Environment)
:	Impl(new FIoStoreReaderImpl(Environment))
{
}

FIoStoreReader::~FIoStoreReader()
{
	delete Impl;
}

FIoStatus 
FIoStoreReader::Initialize(FStringView UniqueId)
{
	return Impl->Open(UniqueId);
}

//////////////////////////////////////////////////////////////////////////

class FIoBatchImpl
{
public:
	FIoRequestImpl* FirstRequest	= nullptr;
	FIoBatchImpl*	NextBatch		= nullptr;
	TAtomic<uint32>	OutstandingRequests { 0 };
};

class FIoRequestImpl
{
public:
	FIoChunkId				ChunkId;
	FIoReadOptions			Options;
	TIoStatusOr<FIoBuffer>	Result;
	uint64					UserData	= 0;
	FIoRequestImpl*			NextRequest = nullptr;
};

//////////////////////////////////////////////////////////////////////////

class FIoStoreImpl : public FRefCountBase
{
public:
	void Mount(FIoStoreReader* IoStore)
	{
		FWriteScopeLock _(RwLockIoStores);
		// TODO prevent duplicates?
		IoStores.Add(IoStore);
	}

	void Unmount(FIoStoreReader* IoStore)
	{
		FWriteScopeLock _(RwLockIoStores);
		IoStores.Remove(IoStore);
	}

	TIoStatusOr<FIoBuffer> Resolve(FIoChunkId& ChunkId)
	{
		for (const auto& IoStore : IoStores)
		{
			TIoStatusOr<FIoBuffer> Result = IoStore->Impl->Lookup(ChunkId);

			if (Result.IsOk())
			{
				return Result;
			}
		}

		return FIoStatus(EIoErrorCode::NotFound);
	}

	TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const
	{
		for (const auto& IoStore : IoStores)
		{
			TIoStatusOr<uint64> Result = IoStore->Impl->GetSizeForChunk(ChunkId);

			if (Result.IsOk())
			{
				return Result;
			}
		}

		return FIoStatus(EIoErrorCode::NotFound);
	}

private:
	FRWLock									RwLockIoStores;
	TArray<TRefCountPtr<FIoStoreReader>>	IoStores;
};

//////////////////////////////////////////////////////////////////////////

class FIoDispatcherImpl
{
public:
	FIoDispatcherImpl()
	{
		IoStore = new FIoStoreImpl;

		FCoreDelegates::GetMemoryTrimDelegate().AddLambda([this]()
		{
			RequestAllocator.Trim();
			BatchAllocator.Trim();
		});
	}

	FIoRequestImpl* AllocRequest(const FIoChunkId& ChunkId, FIoReadOptions Options, uint64 UserData = 0)
	{
		FIoRequestImpl* Request = RequestAllocator.Construct();

		Request->ChunkId		= ChunkId;
		Request->Options		= Options;
		Request->Result			= FIoStatus::Unknown;
		Request->UserData		= UserData;
		Request->NextRequest	= nullptr;

		return Request;
	}

	FIoRequestImpl* AllocRequest(FIoBatchImpl* Batch, const FIoChunkId& ChunkId, FIoReadOptions Options, uint64 UserData = 0)
	{
		FIoRequestImpl* Request	= AllocRequest(ChunkId, Options, UserData);

		Request->NextRequest	= Batch->FirstRequest;
		Batch->FirstRequest		= Request;

		return Request;
	}

	void FreeRequest(FIoRequestImpl* Request)
	{
		RequestAllocator.Destroy(Request);
	}

	FIoBatchImpl* AllocBatch(FIoRequestImpl* FirstRequest = nullptr)
	{
		FIoBatchImpl* Batch			= BatchAllocator.Construct();

		Batch->FirstRequest			= FirstRequest;
		Batch->OutstandingRequests	= 0;
	
		return Batch;
	}

	void FreeBatch(FIoBatchImpl* Batch)
	{
		FIoRequestImpl* Request = Batch->FirstRequest;

		while (Request)
		{
			FIoRequestImpl* Tmp = Request;	
			Request = Request->NextRequest;
			FreeRequest(Tmp);
		}

		BatchAllocator.Destroy(Batch);
	}

	TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const
	{
		return IoStore->GetSizeForChunk(ChunkId);
	}

	template<typename Func>
	void IterateBatch(const FIoBatchImpl* Batch, Func&& InCallbackFunction)
	{
		FIoRequestImpl* Request = Batch->FirstRequest;

		while (Request)
		{
			const bool bDoContinue = InCallbackFunction(Request);

			Request = bDoContinue ? Request->NextRequest : nullptr;
		}
	}

	void IssueBatch(const FIoBatchImpl* Batch)
	{
		// At this point the batch is immutable and we should start
		// doing the work.

#if IODISPATCHER_TRACE_ENABLED
		UE_TRACE_LOG(IoDispatcher, BatchIssued)
			<< BatchIssued.Cycle(FPlatformTime::Cycles64())
			<< BatchIssued.BatchId(uint64(Batch));
#endif
		uint64 TotalBatchSize = 0;
		IterateBatch(Batch, [this, &TotalBatchSize](FIoRequestImpl* Request)
		{
			Request->Result = IoStore->Resolve(Request->ChunkId);

#if IODISPATCHER_TRACE_ENABLED
			TotalBatchSize += Request->Result.ValueOrDie().DataSize();
#endif
			return true;
		});

#if IODISPATCHER_TRACE_ENABLED
		UE_TRACE_LOG(IoDispatcher, BatchResolved)
			<< BatchResolved.Cycle(FPlatformTime::Cycles64())
			<< BatchResolved.BatchId(uint64(Batch))
			<< BatchResolved.TotalSize(TotalBatchSize);
#endif
	}

	bool IsBatchReady(const FIoBatchImpl* Batch)
	{
		bool bIsReady = true;

		IterateBatch(Batch, [&bIsReady](FIoRequestImpl* Request)
		{
			bIsReady &= Request->Result.Status().IsCompleted();
			return bIsReady;
		});

		return bIsReady;
	}

	void Mount(FIoStoreReader* IoStoreReader)
	{
		IoStore->Mount(IoStoreReader);
	}

	void Unmount(FIoStoreReader* IoStoreReader)
	{
		IoStore->Unmount(IoStoreReader);
	}

private:
	using FRequestAllocator		= TBlockAllocator<FIoRequestImpl, 4096>;
	using FBatchAllocator		= TBlockAllocator<FIoBatchImpl, 4096>;

	TRefCountPtr<FIoStoreImpl>	IoStore;
	FRequestAllocator			RequestAllocator;
	FBatchAllocator				BatchAllocator;
};

//////////////////////////////////////////////////////////////////////////

FIoDispatcher::FIoDispatcher()
:	Impl(new FIoDispatcherImpl())
{
}

FIoDispatcher::~FIoDispatcher()
{
	delete Impl;
}

void		
FIoDispatcher::Mount(FIoStoreReader* IoStore)
{
	Impl->Mount(IoStore);
}

void		
FIoDispatcher::Unmount(FIoStoreReader* IoStore)
{
	Impl->Unmount(IoStore);
}

FIoBatch
FIoDispatcher::NewBatch()
{
	return FIoBatch(Impl, Impl->AllocBatch());
}

void
FIoDispatcher::FreeBatch(FIoBatch Batch)
{
	Impl->FreeBatch(Batch.Impl);
}

// Polling methods
TIoStatusOr<uint64>	
FIoDispatcher::GetSizeForChunk(const FIoChunkId& ChunkId) const
{
	return Impl->GetSizeForChunk(ChunkId);
}

//////////////////////////////////////////////////////////////////////////

FIoBatch::FIoBatch(FIoDispatcherImpl* InDispatcher, FIoBatchImpl* InImpl)
:	Dispatcher(InDispatcher)
,	Impl(InImpl)
,	CompletionEvent()
{
}

FIoRequest
FIoBatch::Read(const FIoChunkId& ChunkId, FIoReadOptions Options)
{
	return FIoRequest(Dispatcher->AllocRequest(Impl, ChunkId, Options));
}

void 
FIoBatch::ForEachRequest(TFunction<bool(FIoRequest&)> Callback)
{
	Dispatcher->IterateBatch(Impl, [&](FIoRequestImpl* InRequest) {
		FIoRequest Request(InRequest);
		return Callback(Request);
	});
}

void 
FIoBatch::Issue()
{
	Dispatcher->IssueBatch(Impl);
}

void 
FIoBatch::Wait()
{
	//TODO: Create synchronization event here when it's actually needed
	unimplemented();
}

void 
FIoBatch::Cancel()
{
	unimplemented();
}

//////////////////////////////////////////////////////////////////////////

bool		
FIoRequest::IsOk() const 
{ 
	return Impl->Result.IsOk(); 
}

FIoStatus	
FIoRequest::Status() const
{ 
	return Impl->Result.Status();
}

FIoBuffer	
FIoRequest::GetChunk()
{
	return Impl->Result.ValueOrDie();
}

const FIoChunkId&
FIoRequest::GetChunkId() const
{
	return Impl->ChunkId;
}

const TIoStatusOr<FIoBuffer>&
FIoRequest::GetResult() const
{
	return Impl->Result;
}

//////////////////////////////////////////////////////////////////////////

class FIoQueueImpl
	: private FRunnable
{
public:
	FIoQueueImpl(FIoDispatcherImpl& InDispatcher, FIoQueue::FBatchReadyCallback&& InBatchReadyCallback)
		: Dispatcher(InDispatcher)
		, BatchReadyCallback(Forward<FIoQueue::FBatchReadyCallback>(InBatchReadyCallback))
	{
		WakeUpEvent = FGenericPlatformProcess::GetSynchEventFromPool(true);
		Thread = FRunnableThread::Create(this, TEXT("IoQueueThread"), 0, TPri_Normal);
	}

	~FIoQueueImpl()
	{
		Stop();
		Thread->Kill(true);
		Thread = nullptr;
		FPlatformProcess::ReturnSynchEventToPool(WakeUpEvent);
	}

	void Enqueue(const FIoChunkId& ChunkId, FIoReadOptions ReadOptions, uint64 UserData, bool bDeferBatch)
	{
		FIoRequestImpl* Request = Dispatcher.AllocRequest(ChunkId, ReadOptions, UserData);
		check(Request->NextRequest == nullptr);

		{
			// TODO: CAS
			FScopeLock _(&QueuedLock);

			Request->NextRequest	= FirstQueuedRequest;
			FirstQueuedRequest		= Request;
		}

		if (!bDeferBatch)
		{
			IssueBatch();
		}
	
		{
			FScopeLock _(&NumPendingLock);

			if (0 == NumPending++)
			{
				WakeUpEvent->Trigger();
			}
		}
	}

	bool Dequeue(FIoChunkId& ChunkId, TIoStatusOr<FIoBuffer>& Result, uint64& UserData)
	{
		FIoRequestImpl* CompletedRequest = nullptr;
		{
			// TODO: CAS
			FScopeLock _(&CompletedLock);
			
			CompletedRequest		= FirstCompletedRequest;
			FirstCompletedRequest	= CompletedRequest ? CompletedRequest->NextRequest : FirstCompletedRequest;
		}

		if (CompletedRequest)
		{
			ChunkId		= CompletedRequest->ChunkId;
			Result		= CompletedRequest->Result;
			UserData	= CompletedRequest->UserData;

			Dispatcher.FreeRequest(CompletedRequest);

			{
				FScopeLock _(&NumPendingLock);

				if (0 == --NumPending)
				{
					check(NumPending >= 0);
					WakeUpEvent->Reset();
				}
			}

			return true;
		}

		return false;
	}

	void IssueBatch()
	{
		FIoRequestImpl* QueuedRequests = nullptr;
		{
			// TODO: CAS
			FScopeLock _(&QueuedLock);

			QueuedRequests 		= FirstQueuedRequest;
			FirstQueuedRequest	= nullptr;
		}

		if (QueuedRequests)
		{
			FIoBatchImpl* NewBatch = Dispatcher.AllocBatch(QueuedRequests);
			{
				// TODO: CAS
				FScopeLock _(&PendingLock);

				NewBatch->NextBatch	= FirstPendingBatch;
				FirstPendingBatch	= NewBatch;
			}
		}
	}

	bool IsEmpty() const
	{
		FScopeLock _(&NumPendingLock);
		return NumPending == 0;
	}

private:
	
	uint32 Run() override
	{
		bIsRunning = true;

		FBatchQueue IssuedBatches;

		while (bIsRunning)
		{
			if (!bIsRunning)
			{
				break;
			}

			// Dispatch pending batch
			{
				FIoBatchImpl* PendingBatch = nullptr;
				{
					// TODO: CAS
					FScopeLock _(&PendingLock);

					PendingBatch		= FirstPendingBatch;
					FirstPendingBatch	= PendingBatch ? PendingBatch->NextBatch : nullptr;
				}

				if (PendingBatch)
				{
					PendingBatch->NextBatch = nullptr;
					
					Dispatcher.IssueBatch(PendingBatch);
					Enqueue(PendingBatch, IssuedBatches);
				}
			}

			// Process issued batches
			if (FIoBatchImpl* IssuedBatch = Peek(IssuedBatches))
			{
				if (Dispatcher.IsBatchReady(IssuedBatch))
				{
					Dequeue(IssuedBatches);

					FIoRequestImpl* Request = IssuedBatch->FirstRequest;
					while (Request)
					{
						FIoRequestImpl* CompletedRequest	= Request;
						Request								= Request->NextRequest;
						
						{
							// TODO: CAS
							FScopeLock _(&CompletedLock);

							CompletedRequest->NextRequest	= FirstCompletedRequest;
							FirstCompletedRequest			= CompletedRequest;
						}
					}

					BatchReadyCallback();

					IssuedBatch->FirstRequest = nullptr;
					Dispatcher.FreeBatch(IssuedBatch);
				}
			}
			else
			{
				WakeUpEvent->Wait();
			}
		}

		return 0;
	}

	void Stop() override
	{
		if (bIsRunning)
		{
			bIsRunning = false;
			WakeUpEvent->Trigger();
		}
	}

	struct FBatchQueue
	{
		FIoBatchImpl* Head = nullptr;
		FIoBatchImpl* Tail = nullptr;
	};

	void Enqueue(FIoBatchImpl* Batch, FBatchQueue& Batches)
	{
		if (Batches.Tail == nullptr)
		{
			Batches.Head = Batches.Tail	= Batch;
		}
		else
		{
			Batches.Tail->NextBatch	= Batch;
			Batches.Tail			= Batch;
		}
	}

	FIoBatchImpl* Dequeue(FBatchQueue& Batches)
	{
		if (FIoBatchImpl* Batch = Batches.Head)
		{
			Batches.Head		= Batch->NextBatch;
			Batches.Tail		= Batches.Head == nullptr ? nullptr : Batches.Tail;
			Batch->NextBatch	= nullptr;

			return Batch;
		}
		
		return nullptr;
	}

	FIoBatchImpl* Peek(FBatchQueue& Queue)
	{
		return Queue.Head;
	}

	FIoDispatcherImpl&				Dispatcher;
	FIoQueue::FBatchReadyCallback	BatchReadyCallback;
	FRunnableThread*				Thread					= nullptr;
	FEvent*							WakeUpEvent				= nullptr;
	bool							bIsRunning				= false;
	FIoRequestImpl*					FirstQueuedRequest		= nullptr;
	FIoBatchImpl*					FirstPendingBatch		= nullptr;
	FIoRequestImpl*					FirstCompletedRequest	= nullptr;
	int32							NumPending				= 0;
	FCriticalSection				QueuedLock;
	FCriticalSection				PendingLock;
	FCriticalSection				CompletedLock;
	mutable FCriticalSection		NumPendingLock;
};

//////////////////////////////////////////////////////////////////////////
FIoQueue::FIoQueue(FIoDispatcher& IoDispatcher, FBatchReadyCallback BatchReadyCallback)
: Impl(new FIoQueueImpl(*IoDispatcher.Impl, MoveTemp(BatchReadyCallback)))
{ 
}

FIoQueue::~FIoQueue() = default;

void
FIoQueue::Enqueue(const FIoChunkId& ChunkId, FIoReadOptions ReadOptions, uint64 UserData, bool bDeferBatch)
{
	Impl->Enqueue(ChunkId, ReadOptions, UserData, bDeferBatch);
}

bool
FIoQueue::Dequeue(FIoChunkId& ChunkId, TIoStatusOr<FIoBuffer>& Result, uint64& UserData)
{
	return Impl->Dequeue(ChunkId, Result, UserData);
}

void
FIoQueue::IssueBatch()
{
	return Impl->IssueBatch();
}

bool
FIoQueue::IsEmpty() const
{
	return Impl->IsEmpty();
}

#endif // PLATFORM_IMPLEMENTS_IO
