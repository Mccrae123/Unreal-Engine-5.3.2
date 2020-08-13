// Copyright Epic Games, Inc. All Rights Reserved.

#include "IO/IoStore.h"
#include "Containers/Map.h"
#include "HAL/FileManager.h"
#include "Templates/UniquePtr.h"
#include "Misc/Paths.h"
#include "Misc/Compression.h"
#include "Serialization/BufferWriter.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/StringBuilder.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

//////////////////////////////////////////////////////////////////////////

constexpr char FIoStoreTocHeader::TocMagicImg[];

//////////////////////////////////////////////////////////////////////////

template<typename ArrayType>
bool WriteArray(IFileHandle* FileHandle, const ArrayType& Array)
{
	return FileHandle->Write(reinterpret_cast<const uint8*>(Array.GetData()), Array.GetTypeSize() * Array.Num());
}

static IEngineCrypto* GetEngineCrypto()
{
	static TArray<IEngineCrypto*> Features = IModularFeatures::Get().GetModularFeatureImplementations<IEngineCrypto>(IEngineCrypto::GetFeatureName());
	checkf(Features.Num() > 0, TEXT("RSA functionality was used but no modular feature was registered to provide it. Please make sure your project has the PlatformCrypto plugin enabled!"));
	return Features[0];
}

static bool IsSigningEnabled()
{
	return FCoreDelegates::GetPakSigningKeysDelegate().IsBound();
}

static FRSAKeyHandle GetPublicSigningKey()
{
	static FRSAKeyHandle PublicKey = InvalidRSAKeyHandle;
	static bool bInitializedPublicKey = false;
	if (!bInitializedPublicKey)
	{
		FCoreDelegates::FPakSigningKeysDelegate& Delegate = FCoreDelegates::GetPakSigningKeysDelegate();
		if (Delegate.IsBound())
		{
			TArray<uint8> Exponent;
			TArray<uint8> Modulus;
			Delegate.Execute(Exponent, Modulus);
			PublicKey = GetEngineCrypto()->CreateRSAKey(Exponent, TArray<uint8>(), Modulus);
		}
		bInitializedPublicKey = true;
	}

	return PublicKey;
}

static FIoStatus CreateContainerSignature(
	const FRSAKeyHandle PrivateKey,
	const FIoStoreTocHeader& TocHeader,
	TArrayView<const FSHAHash> BlockSignatureHashes,
	TArray<uint8>& OutTocSignature,
	TArray<uint8>& OutBlockSignature)
{
	if (PrivateKey == InvalidRSAKeyHandle)
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Invalid signing key"));
	}

	FSHAHash TocHash, BlocksHash;

	FSHA1::HashBuffer(reinterpret_cast<const uint8*>(&TocHeader), sizeof(FIoStoreTocHeader), TocHash.Hash);
	FSHA1::HashBuffer(BlockSignatureHashes.GetData(), BlockSignatureHashes.Num() * sizeof(FSHAHash), BlocksHash.Hash);

	int32 BytesEncrypted = GetEngineCrypto()->EncryptPrivate(TArrayView<const uint8>(TocHash.Hash, UE_ARRAY_COUNT(FSHAHash::Hash)), OutTocSignature, PrivateKey);

	if (BytesEncrypted < 1)
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Failed to encrypt TOC signature"));
	}

	BytesEncrypted = GetEngineCrypto()->EncryptPrivate(TArrayView<const uint8>(BlocksHash.Hash, UE_ARRAY_COUNT(FSHAHash::Hash)), OutBlockSignature, PrivateKey);

	return BytesEncrypted > 0 ? FIoStatus::Ok : FIoStatus(EIoErrorCode::SignatureError, TEXT("Failed to encrypt block signature"));
}

static FIoStatus ValidateContainerSignature(
	const FRSAKeyHandle PublicKey,
	const FIoStoreTocHeader& TocHeader,
	TArrayView<const FSHAHash> BlockSignatureHashes,
	TArrayView<const uint8> TocSignature,
	TArrayView<const uint8> BlockSignature)
{
	if (PublicKey == InvalidRSAKeyHandle)
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Invalid signing key"));
	}

	TArray<uint8> DecryptedTocHash, DecryptedBlocksHash;

	int32 BytesDecrypted = GetEngineCrypto()->DecryptPublic(TocSignature, DecryptedTocHash, PublicKey);
	if (BytesDecrypted != UE_ARRAY_COUNT(FSHAHash::Hash))
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Failed to decrypt TOC signature"));
	}

	BytesDecrypted = GetEngineCrypto()->DecryptPublic(BlockSignature, DecryptedBlocksHash, PublicKey);
	if (BytesDecrypted != UE_ARRAY_COUNT(FSHAHash::Hash))
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Failed to decrypt block signature"));
	}

	FSHAHash TocHash, BlocksHash;
	FSHA1::HashBuffer(reinterpret_cast<const uint8*>(&TocHeader), sizeof(FIoStoreTocHeader), TocHash.Hash);
	FSHA1::HashBuffer(BlockSignatureHashes.GetData(), BlockSignatureHashes.Num() * sizeof(FSHAHash), BlocksHash.Hash);

	if (FMemory::Memcmp(DecryptedTocHash.GetData(), TocHash.Hash, DecryptedTocHash.Num()) != 0)
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Invalid TOC signature"));
	}

	if (FMemory::Memcmp(DecryptedBlocksHash.GetData(), BlocksHash.Hash, DecryptedBlocksHash.Num()) != 0)
	{
		return FIoStatus(EIoErrorCode::SignatureError, TEXT("Invalid block signature"));
	}

	return FIoStatus::Ok;
}

FIoStoreEnvironment::FIoStoreEnvironment()
{
}

FIoStoreEnvironment::~FIoStoreEnvironment()
{
}

void FIoStoreEnvironment::InitializeFileEnvironment(FStringView InPath, int32 InOrder)
{
	Path = InPath;
	Order = InOrder;
}

//////////////////////////////////////////////////////////////////////////

struct FChunkBlock
{
	uint64 Offset = 0;
	uint64 Size = 0;
	uint64 CompressedSize = 0;
	uint64 UncompressedSize = 0;
	FName CompressionMethod = NAME_None;
};

struct FIoStoreWriteQueueEntry
{
	FIoStoreWriteQueueEntry* Next = nullptr;
	FIoChunkId ChunkId;
	FIoChunkHash ChunkHash;
	FIoBuffer ChunkBuffer;
	uint64 ChunkSize = 0;
	TArray<FChunkBlock> ChunkBlocks;
	FIoWriteOptions Options;
	FGraphEventRef CreateChunkBlocksTask = FGraphEventRef();
};

class FIoStoreWriteQueue
{
public:
	FIoStoreWriteQueue()
		: Event(FPlatformProcess::GetSynchEventFromPool(false))
	{ }
	
	~FIoStoreWriteQueue()
	{
		check(Head == nullptr && Tail == nullptr);
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void Enqueue(FIoStoreWriteQueueEntry* Entry)
	{
		check(!bIsDoneAdding);
		{
			FScopeLock _(&CriticalSection);

			if (!Tail)
			{
				Head = Tail = Entry;
			}
			else
			{
				Tail->Next = Entry;
				Tail = Entry;
			}
		}

		Event->Trigger();
	}

	FIoStoreWriteQueueEntry* DequeueOrWait()
	{
		for (;;)
		{
			{
				FScopeLock _(&CriticalSection);
				if (Head)
				{
					FIoStoreWriteQueueEntry* Entry = Head;
					Head = Tail = nullptr;
					return Entry;
				}
			}

			if (bIsDoneAdding)
			{
				break;
			}

			Event->Wait();
		}

		return nullptr;
	}

	void CompleteAdding()
	{
		bIsDoneAdding = true;
		Event->Trigger();
	}

	bool IsDoneAdding() const
	{
		return bIsDoneAdding;
	}

	bool IsEmpty() const
	{
		FScopeLock _(&CriticalSection);
		return Head == nullptr;
	}

private:
	mutable FCriticalSection CriticalSection;
	FEvent* Event = nullptr;
	FIoStoreWriteQueueEntry* Head = nullptr;
	FIoStoreWriteQueueEntry* Tail = nullptr;
	TAtomic<bool> bIsDoneAdding { false };
};

class FIoStoreWriterContextImpl
{
	static constexpr uint64 DefaultMemoryLimit = 5ull * (2ull << 30ull);
public:
	FIoStoreWriterContextImpl()
	{
	}

	~FIoStoreWriterContextImpl()
	{
		if (MemoryFreedEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(MemoryFreedEvent);
		}
	}

	UE_NODISCARD FIoStatus Initialize(const FIoStoreWriterSettings& InWriterSettings)
	{
		WriterSettings = InWriterSettings;
		MemoryFreedEvent = FPlatformProcess::GetSynchEventFromPool(false);

		if (InWriterSettings.WriterMemoryLimit > 0)
		{
			NumBytesAvailable = InWriterSettings.WriterMemoryLimit;
		}

		return FIoStatus::Ok;
	}

	const FIoStoreWriterSettings& GetSettings() const
	{
		return WriterSettings;
	}

	FIoStoreWriteQueueEntry* AllocQueueEntry(
		const FIoChunkId& ChunkId,
		const FIoChunkHash& ChunkHash,
		FIoBuffer ChunkBuffer,
		const FIoWriteOptions& Options)
	{
		const uint64 ChunkSize = ChunkBuffer.DataSize();

		for (;;)
		{
			{
				FScopeLock _(&CriticalSection);
				if (NumBytesAvailable > ChunkSize)
				{
					NumBytesAvailable -= ChunkSize;

					ChunkBuffer.MakeOwned();
					FIoStoreWriteQueueEntry* Entry = new FIoStoreWriteQueueEntry();
					Entry->ChunkId = ChunkId;
					Entry->ChunkHash = ChunkHash;
					Entry->ChunkBuffer = ChunkBuffer;
					Entry->ChunkSize = ChunkSize;
					Entry->Options = Options;

					return Entry;
				}
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(WaitForMemory);
				MemoryFreedEvent->Wait();
			}
		}
	}

	void FreeQueueEntry(FIoStoreWriteQueueEntry* QueueEntry)
	{
		{
			FScopeLock _(&CriticalSection);
			NumBytesAvailable += QueueEntry->ChunkSize;
		}

		delete QueueEntry;
		MemoryFreedEvent->Trigger();
	}

private:
	FIoStoreWriterSettings WriterSettings;
	FCriticalSection CriticalSection;
	FEvent* MemoryFreedEvent = nullptr;
	uint64 NumBytesAvailable = DefaultMemoryLimit;
};

FIoStoreWriterContext::FIoStoreWriterContext()
	: Impl(new FIoStoreWriterContextImpl())
{

}

FIoStoreWriterContext::~FIoStoreWriterContext()
{
	delete Impl;
}

UE_NODISCARD FIoStatus FIoStoreWriterContext::Initialize(const FIoStoreWriterSettings& InWriterSettings)
{
	return Impl->Initialize(InWriterSettings);
}

static uint64 GetPadding(const uint64 Offset, const uint64 Alignment)
{
	return (Alignment - (Offset % Alignment)) % Alignment;
}

class FIoStoreToc
{
public:
	FIoStoreToc()
	{
		FMemory::Memzero(&Toc.Header, sizeof(FIoStoreTocHeader));
	}

	void Initialize()
	{
		ChunkIdToIndex.Empty(false);

		for (int32 ChunkIndex = 0; ChunkIndex < Toc.ChunkIds.Num(); ++ChunkIndex)
		{
			ChunkIdToIndex.Add(Toc.ChunkIds[ChunkIndex], ChunkIndex);
		}
	}

	bool AddChunkEntry(const FIoChunkId& ChunkId, const FIoOffsetAndLength& OffsetLength, const FIoStoreTocEntryMeta& Meta)
	{
		int32& Index = ChunkIdToIndex.FindOrAdd(ChunkId);

		if (!Index)
		{
			Index = Toc.ChunkIds.Add(ChunkId);
			Toc.ChunkOffsetLengths.Add(OffsetLength);
			Toc.ChunkMetas.Add(Meta);

			return true;
		}

		return false;
	}

	FIoStoreTocCompressedBlockEntry& AddCompressionBlockEntry()
	{
		return Toc.CompressionBlocks.AddDefaulted_GetRef();
	}

	FSHAHash& AddBlockSignatureEntry()
	{
		return Toc.ChunkBlockSignatures.AddDefaulted_GetRef();
	}

	uint8 AddCompressionMethodEntry(FName CompressionMethod)
	{
		if (CompressionMethod == NAME_None)
		{
			return 0;
		}

		uint8 Index = 1;
		for (const FName& Name : Toc.CompressionMethods)
		{
			if (Name == CompressionMethod)
			{
				return Index;
			}
			++Index;
		}

		return 1 + uint8(Toc.CompressionMethods.Add(CompressionMethod));
	}

	FIoStoreTocResource& GetTocResource()
	{
		return Toc;
	}

	const FIoStoreTocResource& GetTocResource() const
	{
		return Toc;
	}

	const FIoOffsetAndLength* GetOffsetAndLength(const FIoChunkId& ChunkId) const
	{
		if (const int32* Index = ChunkIdToIndex.Find(ChunkId))
		{
			return &Toc.ChunkOffsetLengths[*Index];
		}

		return nullptr;
	}

private:
	TMap<FIoChunkId, int32> ChunkIdToIndex;
	FIoStoreTocResource Toc;
};

class FIoStoreWriterImpl
{
public:
	FIoStoreWriterImpl(FIoStoreEnvironment& InEnvironment)
		: Environment(InEnvironment)
	{
	}

	UE_NODISCARD FIoStatus Initialize(FIoStoreWriterContextImpl& InContext, const FIoContainerSettings& InContainerSettings)
	{
		WriterContext = &InContext;
		ContainerSettings = InContainerSettings;

		TocFilePath = Environment.GetPath() + TEXT(".utoc");
		FString ContainerFilePath = Environment.GetPath() + TEXT(".ucas");

		IPlatformFile& Ipf = IPlatformFile::GetPlatformPhysical();
		Ipf.CreateDirectoryTree(*FPaths::GetPath(ContainerFilePath));

		ContainerFileHandle.Reset(Ipf.OpenWrite(*ContainerFilePath, /* append */ false, /* allowread */ true));

		if (!ContainerFileHandle)
		{
			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore container file '") << *ContainerFilePath << TEXT("'");
		}

		FIoStatus Status = FIoStatus::Ok;
		if (InContext.GetSettings().bEnableCsvOutput)
		{
			Status = EnableCsvOutput();
		}

		WriterThread = Async(EAsyncExecution::Thread, [this]() { ProcessChunksThread(); });

		return Status;
	}

	FIoStatus EnableCsvOutput()
	{
		FString CsvFilePath = Environment.GetPath() + TEXT(".csv");
		CsvArchive.Reset(IFileManager::Get().CreateFileWriter(*CsvFilePath));
		if (!CsvArchive)
		{
			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore CSV file '") << *CsvFilePath << TEXT("'");
		}
		ANSICHAR Header[] = "Name,Offset,Size\n";
		CsvArchive->Serialize(Header, sizeof(Header) - 1);

		return FIoStatus::Ok;
	}

	UE_NODISCARD FIoStatus Append(const FIoChunkId& ChunkId, FIoBuffer Chunk, const FIoWriteOptions& WriteOptions)
	{
		return Append(ChunkId, FIoChunkHash::HashBuffer(Chunk.Data(), Chunk.DataSize()), Chunk, WriteOptions);
	}

	UE_NODISCARD FIoStatus Append(const FIoChunkId& ChunkId, const FIoChunkHash& ChunkHash, FIoBuffer Chunk, const FIoWriteOptions& WriteOptions)
	{
		if (!ChunkId.IsValid())
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("ChunkId is not valid!"));
		}

		IsMetadataDirty = true;

		FIoStoreWriteQueueEntry* Entry = WriterContext->AllocQueueEntry(ChunkId, ChunkHash, Chunk, WriteOptions);

		Entry->CreateChunkBlocksTask = FFunctionGraphTask::CreateAndDispatchWhenReady([this, Entry]()
		{ 
			CreateChunkBlocks(Entry, ContainerSettings, WriterContext->GetSettings());
		}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask);

		WriteQueue.Enqueue(Entry);

		return FIoStatus::Ok;
	}

	UE_NODISCARD FIoStatus AppendPadding(uint64 Count)
	{
		return FIoStatus::Ok;
	}

	UE_NODISCARD TIoStatusOr<FIoStoreWriterResult> Flush()
	{
		if (!IsMetadataDirty)
		{
			return Result;
		}

		IsMetadataDirty = false;

		WriteQueue.CompleteAdding();
		WriterThread.Wait();

		FIoStoreTocResource& TocResource = Toc.GetTocResource();
		TIoStatusOr<uint64> TocSize = FIoStoreTocResource::Write(*TocFilePath, TocResource, ContainerSettings, WriterContext->GetSettings());
		if (!TocSize.IsOk())
		{
			TocSize.Status();
		}

		Result.ContainerId = ContainerSettings.ContainerId;
		Result.ContainerName = FPaths::GetBaseFilename(TocFilePath);
		Result.ContainerFlags = ContainerSettings.ContainerFlags;
		Result.TocSize = TocSize.ConsumeValueOrDie();
		Result.TocEntryCount = TocResource.Header.TocEntryCount;
		Result.PaddingSize = TotalPaddedBytes;
		Result.UncompressedContainerSize = UncompressedContainerSize;
		Result.CompressedContainerSize = CompressedContainerSize;
		Result.CompressionMethod = EnumHasAnyFlags(ContainerSettings.ContainerFlags, EIoContainerFlags::Compressed)
			? WriterContext->GetSettings().CompressionMethod
			: NAME_None;

		return Result;
	}

private:
	void ProcessChunksThread()
	{
		const FIoStoreWriterSettings& Settings = WriterContext->GetSettings();
		TArray<uint8> PaddingBuffer;
		uint64 UncompressedFileOffset = 0;

		auto CrossesBlockBoundry = [](const uint64 FileOffset, const uint64 ChunkSize, const uint64 ChunkAlignment, const uint64 BlockSize) -> bool
		{
			const uint64 AlignedOffset = ChunkAlignment > 0 ? Align(FileOffset, ChunkAlignment) : FileOffset;
			return BlockSize > 0 ? Align(AlignedOffset, BlockSize) != Align(AlignedOffset + ChunkSize - 1, BlockSize) : false;
		};

		auto WritePadding = [&PaddingBuffer](IFileHandle& FileHandle, const uint64 BlockSize) -> uint64
		{
			const uint64 Padding = GetPadding(FileHandle.Tell(), BlockSize);
			if (Padding > 0)
			{
				PaddingBuffer.SetNumZeroed(int32(Padding), false);
				FileHandle.Write(PaddingBuffer.GetData(), Padding);
			}
			return Padding;
		};
		
		for (;;)
		{
			FIoStoreWriteQueueEntry* Entry = WriteQueue.DequeueOrWait();

			if (!Entry && WriteQueue.IsDoneAdding())
			{
				break;
			}

			while (Entry)
			{
				if (Entry->CreateChunkBlocksTask.IsValid())
				{
					FTaskGraphInterface::Get().WaitUntilTaskCompletes(Entry->CreateChunkBlocksTask);
				}

				const uint64 ChunkAlignment = Entry->Options.bIsMemoryMapped ? Settings.MemoryMappingAlignment : 0;
				if (CrossesBlockBoundry(ContainerFileHandle->Tell(), Entry->ChunkBuffer.DataSize(), ChunkAlignment, Settings.CompressionBlockAlignment))
				{
					TotalPaddedBytes += WritePadding(*ContainerFileHandle, Settings.CompressionBlockAlignment);
				}

				if (ChunkAlignment > 0)
				{
					TotalPaddedBytes += WritePadding(*ContainerFileHandle, ChunkAlignment);
				}

				const uint64 FileOffset = ContainerFileHandle->Tell();

				FIoOffsetAndLength OffsetLength;
				OffsetLength.SetOffset(UncompressedFileOffset);
				OffsetLength.SetLength(Entry->ChunkSize);

				FIoStoreTocEntryMeta ChunkMeta { Entry->ChunkHash, FIoStoreTocEntryMetaFlags::None };
				if (Entry->Options.bIsMemoryMapped)
				{
					ChunkMeta.Flags |= FIoStoreTocEntryMetaFlags::MemoryMapped;
				}

				for (const FChunkBlock& ChunkBlock : Entry->ChunkBlocks)
				{
					check(ChunkBlock.Offset + ChunkBlock.Size <= Entry->ChunkBuffer.DataSize());

					FIoStoreTocCompressedBlockEntry& BlockEntry = Toc.AddCompressionBlockEntry();
					BlockEntry.SetOffset(FileOffset + ChunkBlock.Offset);
					BlockEntry.SetCompressedSize(uint32(ChunkBlock.CompressedSize));
					BlockEntry.SetUncompressedSize(uint32(ChunkBlock.UncompressedSize));
					BlockEntry.SetCompressionMethodIndex(Toc.AddCompressionMethodEntry(ChunkBlock.CompressionMethod));

					if (!ChunkBlock.CompressionMethod.IsNone())
					{
						ChunkMeta.Flags |= FIoStoreTocEntryMetaFlags::Compressed;
					}

					if (ContainerSettings.IsSigned())
					{
						FSHAHash& Signature = Toc.AddBlockSignatureEntry();
						FSHA1::HashBuffer(Entry->ChunkBuffer.Data() + ChunkBlock.Offset, ChunkBlock.Size, Signature.Hash);
					}
				}

				const bool bAdded = Toc.AddChunkEntry(Entry->ChunkId, OffsetLength, ChunkMeta);
				check(bAdded);

				ContainerFileHandle->Write(Entry->ChunkBuffer.Data(), Entry->ChunkBuffer.DataSize());
				UncompressedFileOffset += Align(Entry->ChunkSize, Settings.CompressionBlockSize);

				FIoStoreWriteQueueEntry* Free = Entry;
				Entry = Entry->Next;
				WriterContext->FreeQueueEntry(Free);
			}
		}

		UncompressedContainerSize = UncompressedFileOffset + TotalPaddedBytes;
		CompressedContainerSize = ContainerFileHandle->Tell();

		check(WriteQueue.IsEmpty());
	}

	static void CreateChunkBlocks(
		FIoStoreWriteQueueEntry* Entry,
		const FIoContainerSettings& ContainerSettings,
		const FIoStoreWriterSettings& WriterSettings)
	{
		check(WriterSettings.CompressionBlockSize > 0);

		const uint64 NumChunkBlocks = Align(Entry->ChunkBuffer.DataSize(), WriterSettings.CompressionBlockSize) / WriterSettings.CompressionBlockSize;
		Entry->ChunkBlocks.Reserve(int32(NumChunkBlocks));

		auto CreateUncompressedBlocks = [](FIoStoreWriteQueueEntry* UncompressedEntry, const uint64 BlockSize) -> void
		{
			UncompressedEntry->ChunkBlocks.Empty();

			FIoBuffer& ChunkBuffer = UncompressedEntry->ChunkBuffer;

			uint64 UncompressedSize = ChunkBuffer.DataSize();
			uint64 RawSize = UncompressedSize;
			if (!IsAligned(RawSize, FAES::AESBlockSize))
			{
				RawSize = Align(RawSize, FAES::AESBlockSize);
				FIoBuffer AlignedBuffer(RawSize);
				FMemory::Memcpy(AlignedBuffer.Data(), ChunkBuffer.Data(), UncompressedSize);

				for (uint64 FillIndex = UncompressedSize; FillIndex < RawSize ; ++FillIndex)
				{
					AlignedBuffer.Data()[FillIndex] = AlignedBuffer.Data()[(FillIndex - UncompressedSize) % UncompressedSize];
				}

				ChunkBuffer = AlignedBuffer;
			}

			uint64 UncompressedOffset = 0;
			uint64 RemainingSize = UncompressedSize;
			while (RemainingSize)
			{
				const uint64 UncompressedBlockSize = FMath::Min<uint64>(RemainingSize, BlockSize);
				const uint64 RawBlockSize = Align(UncompressedBlockSize, FAES::AESBlockSize);
				UncompressedEntry->ChunkBlocks.Add(FChunkBlock { UncompressedOffset, RawBlockSize, UncompressedBlockSize, UncompressedBlockSize, NAME_None });
				RemainingSize -= UncompressedBlockSize;
				UncompressedOffset += RawBlockSize;
			}
		};

		if (ContainerSettings.IsCompressed() && !Entry->Options.bForceUncompressed && !Entry->Options.bIsMemoryMapped)
		{
			check(!WriterSettings.CompressionMethod.IsNone());

			const uint8* UncompressedBlock = Entry->ChunkBuffer.Data();
			TArray<TUniquePtr<uint8[]>> CompressedBlocks;
			CompressedBlocks.Reserve(int32(NumChunkBlocks));

			uint64 BytesToProcess	= Entry->ChunkBuffer.DataSize();
			uint64 BlockOffset		= 0;

			while (BytesToProcess > 0)
			{
				const int32 UncompressedBlockSize = static_cast<int32>(FMath::Min(BytesToProcess, WriterSettings.CompressionBlockSize));
				int32 CompressedBlockSize = FCompression::CompressMemoryBound(WriterSettings.CompressionMethod, UncompressedBlockSize);
				TUniquePtr<uint8[]>& CompressedBlock = CompressedBlocks.AddDefaulted_GetRef();
				CompressedBlock = MakeUnique<uint8[]>(CompressedBlockSize);

				FName CompressionMethod = WriterSettings.CompressionMethod;
				const bool bCompressed = FCompression::CompressMemory(
					CompressionMethod,
					CompressedBlock.Get(),
					CompressedBlockSize,
					UncompressedBlock,
					UncompressedBlockSize);

				check(bCompressed);
				check(CompressedBlockSize > 0);

				if (CompressedBlockSize >= UncompressedBlockSize)
				{
					memcpy(CompressedBlock.Get(), UncompressedBlock, UncompressedBlockSize);
					CompressedBlockSize = UncompressedBlockSize;
					CompressionMethod = NAME_None;
				}

				// Always align each compressed block to AES block size but store the compressed block size in the TOC
				uint64 AlignedCompressedBlockSize = CompressedBlockSize;
				if (!IsAligned(CompressedBlockSize, FAES::AESBlockSize))
				{
					AlignedCompressedBlockSize = Align(CompressedBlockSize, FAES::AESBlockSize);
					TUniquePtr<uint8[]> AlignedBlock = MakeUnique<uint8[]>(AlignedCompressedBlockSize);

					FMemory::Memcpy(AlignedBlock.Get(), CompressedBlock.Get(), CompressedBlockSize);

					for (uint64 FillIndex = CompressedBlockSize; FillIndex < AlignedCompressedBlockSize ; ++FillIndex)
					{
						AlignedBlock.Get()[FillIndex] = AlignedBlock.Get()[(FillIndex - CompressedBlockSize) % CompressedBlockSize];
					}

					CompressedBlock.Reset(AlignedBlock.Release());
				}

				Entry->ChunkBlocks.Add(FChunkBlock { BlockOffset, AlignedCompressedBlockSize, uint64(CompressedBlockSize), uint64(UncompressedBlockSize), CompressionMethod });

				BytesToProcess		-= UncompressedBlockSize;
				BlockOffset			+= AlignedCompressedBlockSize;
				UncompressedBlock	+= UncompressedBlockSize;
			}

			const uint64 CompressedSize = BlockOffset;
			
			Entry->ChunkBuffer = FIoBuffer(CompressedSize);
			
			uint8* CompressedChunkBuffer = Entry->ChunkBuffer.Data();
			FMemory::Memzero(CompressedChunkBuffer, CompressedSize); 

			for (int32 BlockIndex = 0; BlockIndex < CompressedBlocks.Num(); ++BlockIndex)
			{
				TUniquePtr<uint8[]>& CompressedBlock = CompressedBlocks[BlockIndex];
				const FChunkBlock& ChunkBlock = Entry->ChunkBlocks[BlockIndex];
				FMemory::Memcpy(CompressedChunkBuffer, CompressedBlock.Get(), ChunkBlock.Size);
				CompressedChunkBuffer += ChunkBlock.Size;
			}
		}
		else
		{
			CreateUncompressedBlocks(Entry, WriterSettings.CompressionBlockSize);
		}

		if (ContainerSettings.IsEncrypted())
		{
			for (const FChunkBlock& ChunkBlock : Entry->ChunkBlocks)
			{
				check(IsAligned(ChunkBlock.Size, FAES::AESBlockSize));
				FAES::EncryptData(Entry->ChunkBuffer.Data() + ChunkBlock.Offset, static_cast<uint32>(ChunkBlock.Size), ContainerSettings.EncryptionKey);
			}
		}
	}

	FIoStoreEnvironment&		Environment;
	FIoStoreWriterContextImpl*	WriterContext = nullptr;
	FIoContainerSettings		ContainerSettings;
	FString						TocFilePath;
	FIoStoreToc					Toc;
	TUniquePtr<IFileHandle>		ContainerFileHandle;
	TUniquePtr<FArchive>		CsvArchive;
	FIoStoreWriterResult		Result;
	TFuture<void>				WriterThread;
	FIoStoreWriteQueue			WriteQueue;
	uint64						TotalPaddedBytes = 0;
	uint64						UncompressedContainerSize = 0;
	uint64						CompressedContainerSize = 0;
	bool						IsMetadataDirty = true;
};

FIoStoreWriter::FIoStoreWriter(FIoStoreEnvironment& InEnvironment)
:	Impl(new FIoStoreWriterImpl(InEnvironment))
{
}

FIoStoreWriter::~FIoStoreWriter()
{
	(void)Impl->Flush();
}

FIoStatus FIoStoreWriter::Initialize(const FIoStoreWriterContext& Context, const FIoContainerSettings& ContainerSettings)
{
	return Impl->Initialize(*Context.Impl, ContainerSettings);
}

FIoStatus FIoStoreWriter::Append(const FIoChunkId& ChunkId, FIoBuffer Chunk, const FIoWriteOptions& WriteOptions)
{
	return Impl->Append(ChunkId, Chunk, WriteOptions);
}

FIoStatus FIoStoreWriter::Append(const FIoChunkId& ChunkId, const FIoChunkHash& ChunkHash, FIoBuffer Chunk, const FIoWriteOptions& WriteOptions)
{
	return Impl->Append(ChunkId, ChunkHash, Chunk, WriteOptions);
}

TIoStatusOr<FIoStoreWriterResult> FIoStoreWriter::Flush()
{
	return Impl->Flush();
}

class FIoStoreReaderImpl
{
public:
	FIoStoreReaderImpl()
	{

	}

	UE_NODISCARD FIoStatus Initialize(const FIoStoreEnvironment& InEnvironment, const TMap<FGuid, FAES::FAESKey>& InDecryptionKeys)
	{
		TStringBuilder<256> ContainerFilePath;
		ContainerFilePath.Append(InEnvironment.GetPath());

		TStringBuilder<256> TocFilePath;
		TocFilePath.Append(ContainerFilePath);

		ContainerFilePath.Append(TEXT(".ucas"));
		TocFilePath.Append(TEXT(".utoc"));

		IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();
		ContainerFileHandle.Reset(Ipf.OpenRead(*ContainerFilePath, /* allowwrite */ false));
		if (!ContainerFileHandle)
		{
			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore container file '") << *TocFilePath << TEXT("'");
		}

		FIoStoreTocResource& TocResource = Toc.GetTocResource();
		FIoStatus TocStatus = FIoStoreTocResource::Read(*TocFilePath, EIoStoreTocReadOptions::IncludeTocMeta, TocResource);
		if (!TocStatus.IsOk())
		{
			return TocStatus;
		}

		Toc.Initialize();

		if (EnumHasAnyFlags(TocResource.Header.ContainerFlags, EIoContainerFlags::Encrypted))
		{
			const FAES::FAESKey* FindKey = InDecryptionKeys.Find(TocResource.Header.EncryptionKeyGuid);
			if (!FindKey)
			{
				return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Missing decryption key for IoStore container file '") << *TocFilePath << TEXT("'");
			}
			DecryptionKey = *FindKey;
		}

		return FIoStatus::Ok;
	}

	FIoContainerId GetContainerId() const
	{
		return Toc.GetTocResource().Header.ContainerId;
	}

	EIoContainerFlags GetContainerFlags() const
	{
		return Toc.GetTocResource().Header.ContainerFlags;
	}

	FGuid GetEncryptionKeyGuid() const
	{
		return Toc.GetTocResource().Header.EncryptionKeyGuid;
	}

	void EnumerateChunks(TFunction<bool(const FIoStoreTocChunkInfo&)>&& Callback) const
	{
		const FIoStoreTocResource& TocResource = Toc.GetTocResource();
		const bool bIsContainerCompressed = EnumHasAnyFlags(TocResource.Header.ContainerFlags, EIoContainerFlags::Compressed);

		for (int32 ChunkIndex = 0; ChunkIndex < TocResource.ChunkIds.Num(); ++ChunkIndex)
		{
			const FIoStoreTocEntryMeta& Meta = TocResource.ChunkMetas[ChunkIndex];
			const FIoOffsetAndLength& OffsetLength = TocResource.ChunkOffsetLengths[ChunkIndex];

			FIoStoreTocChunkInfo ChunkInfo;
			ChunkInfo.Id = TocResource.ChunkIds[ChunkIndex];
			ChunkInfo.Hash = Meta.ChunkHash;
			ChunkInfo.bIsMemoryMapped = EnumHasAnyFlags(Meta.Flags, FIoStoreTocEntryMetaFlags::MemoryMapped);
			ChunkInfo.bForceUncompressed = bIsContainerCompressed && !EnumHasAnyFlags(Meta.Flags, FIoStoreTocEntryMetaFlags::Compressed);
			ChunkInfo.Offset = OffsetLength.GetOffset();
			ChunkInfo.Size = OffsetLength.GetLength();
			if (!Callback(ChunkInfo))
			{
				break;
			}
		}
	}

	TIoStatusOr<FIoBuffer> Read(const FIoChunkId& ChunkId, const FIoReadOptions& Options) const
	{
		const FIoOffsetAndLength* OffsetAndLength = Toc.GetOffsetAndLength(ChunkId);
		if (!OffsetAndLength )
		{
			return FIoStatus(EIoErrorCode::NotFound, TEXT("Unknown chunk ID"));
		}

		const FIoStoreTocResource& TocResource = Toc.GetTocResource();
		const uint64 CompressionBlockSize = TocResource.Header.CompressionBlockSize;
		FIoBuffer IoBuffer = FIoBuffer(OffsetAndLength->GetLength());
		int32 FirstBlockIndex = int32(OffsetAndLength->GetOffset() / CompressionBlockSize);
		int32 LastBlockIndex = int32((Align(OffsetAndLength->GetOffset() + OffsetAndLength->GetLength(), CompressionBlockSize) - 1) / CompressionBlockSize);
		uint64 OffsetInBlock = OffsetAndLength->GetOffset() % CompressionBlockSize;
		uint8* Dst = IoBuffer.Data();
		uint8* Src = nullptr;
		uint64 RemainingSize = OffsetAndLength->GetLength();
		for (int32 BlockIndex = FirstBlockIndex; BlockIndex <= LastBlockIndex; ++BlockIndex)
		{
			const FIoStoreTocCompressedBlockEntry& CompressionBlock = TocResource.CompressionBlocks[BlockIndex];
			uint32 RawSize = Align(CompressionBlock.GetCompressedSize(), FAES::AESBlockSize);
			if (uint32(CompressedBuffer.Num()) < RawSize)
			{
				CompressedBuffer.SetNumUninitialized(RawSize);
			}
			uint32 UncompressedSize = CompressionBlock.GetUncompressedSize();
			if (uint32(UncompressedBuffer.Num()) < UncompressedSize)
			{
				UncompressedBuffer.SetNumUninitialized(UncompressedSize);
			}
			ContainerFileHandle->Seek(CompressionBlock.GetOffset());
			ContainerFileHandle->Read(CompressedBuffer.GetData(), RawSize);
			if (EnumHasAnyFlags(TocResource.Header.ContainerFlags, EIoContainerFlags::Encrypted))
			{
				FAES::DecryptData(CompressedBuffer.GetData(), RawSize, DecryptionKey);
			}
			if (CompressionBlock.GetCompressionMethodIndex() == 0)
			{
				Src = CompressedBuffer.GetData();
			}
			else
			{
				FName CompressionMethod = TocResource.CompressionMethods[CompressionBlock.GetCompressionMethodIndex()];
				bool bUncompressed = FCompression::UncompressMemory(CompressionMethod, UncompressedBuffer.GetData(), UncompressedSize, CompressedBuffer.GetData(), CompressionBlock.GetCompressedSize());
				if (!bUncompressed)
				{
					return FIoStatus(EIoErrorCode::CorruptToc, TEXT("Failed uncompressing block"));
				}
				Src = UncompressedBuffer.GetData();
			}
			uint64 SizeInBlock = FMath::Min(CompressionBlockSize - OffsetInBlock, RemainingSize);
			FMemory::Memcpy(Dst, Src + OffsetInBlock, SizeInBlock);
			OffsetInBlock = 0;
			RemainingSize -= SizeInBlock;
			Dst += SizeInBlock;
		}
		
		return IoBuffer;
	}

private:
	FIoStoreToc Toc;
	FAES::FAESKey DecryptionKey;
	TUniquePtr<IFileHandle> ContainerFileHandle;
	mutable TArray<uint8> CompressedBuffer;
	mutable TArray<uint8> UncompressedBuffer;
};

FIoStoreReader::FIoStoreReader()
	: Impl(new FIoStoreReaderImpl())
{
}

FIoStoreReader::~FIoStoreReader()
{
	delete Impl;
}

FIoStatus FIoStoreReader::Initialize(const FIoStoreEnvironment& InEnvironment, const TMap<FGuid, FAES::FAESKey>& InDecryptionKeys)
{
	return Impl->Initialize(InEnvironment, InDecryptionKeys);
}

FIoContainerId FIoStoreReader::GetContainerId() const
{
	return Impl->GetContainerId();
}

EIoContainerFlags FIoStoreReader::GetContainerFlags() const
{
	return Impl->GetContainerFlags();
}

FGuid FIoStoreReader::GetEncryptionKeyGuid() const
{
	return Impl->GetEncryptionKeyGuid();
}

void FIoStoreReader::EnumerateChunks(TFunction<bool(const FIoStoreTocChunkInfo&)>&& Callback) const
{
	Impl->EnumerateChunks(MoveTemp(Callback));
}

TIoStatusOr<FIoBuffer> FIoStoreReader::Read(const FIoChunkId& Chunk, const FIoReadOptions& Options) const
{
	return Impl->Read(Chunk, Options);
}

FIoStatus FIoStoreTocResource::Read(const TCHAR* TocFilePath, EIoStoreTocReadOptions ReadOptions, FIoStoreTocResource& OutTocResource)
{
	check(TocFilePath != nullptr);

	IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle>	TocFileHandle(Ipf.OpenRead(TocFilePath, /* allowwrite */ false));

	if (!TocFileHandle)
	{
		return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore TOC file '") << TocFilePath << TEXT("'");
	}

	// Header
	if (!TocFileHandle->Read(reinterpret_cast<uint8*>(&OutTocResource.Header), sizeof(FIoStoreTocHeader)))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("Failed to read IoStore TOC file '") << TocFilePath << TEXT("'");
	}

	const FIoStoreTocHeader& Header = OutTocResource.Header;

	if (!Header.CheckMagic())
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC header magic mismatch while reading '") << TocFilePath << TEXT("'");
	}

	if (Header.TocHeaderSize != sizeof(FIoStoreTocHeader))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC header size mismatch while reading '") << TocFilePath << TEXT("'");
	}

	if (Header.TocCompressedBlockEntrySize != sizeof(FIoStoreTocCompressedBlockEntry))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("TOC compressed block entry size mismatch while reading '") << TocFilePath << TEXT("'");
	}

	const uint64 TocSize = ReadOptions == EIoStoreTocReadOptions::IncludeTocMeta
		? TocFileHandle->Size() - sizeof(FIoStoreTocHeader)
		: TocFileHandle->Size() - sizeof(FIoStoreTocHeader) - (Header.TocEntryCount * sizeof(FIoStoreTocEntryMeta));

	TUniquePtr<uint8[]> TocBuffer = MakeUnique<uint8[]>(TocSize);

	if (!TocFileHandle->Read(TocBuffer.Get(), TocSize))
	{
		return FIoStatusBuilder(EIoErrorCode::CorruptToc) << TEXT("Failed to read IoStore TOC file '") << TocFilePath << TEXT("'");
	}

	// Chunk IDs
	const FIoChunkId* ChunkIds = reinterpret_cast<const FIoChunkId*>(TocBuffer.Get());
	OutTocResource.ChunkIds = MakeArrayView<FIoChunkId const>(ChunkIds, Header.TocEntryCount);

	// Chunk offsets
	const FIoOffsetAndLength* ChunkOffsetLengths = reinterpret_cast<const FIoOffsetAndLength*>(ChunkIds + Header.TocEntryCount);
	OutTocResource.ChunkOffsetLengths = MakeArrayView<FIoOffsetAndLength const>(ChunkOffsetLengths, Header.TocEntryCount);

	// Compression blocks
	const FIoStoreTocCompressedBlockEntry* CompressionBlocks = reinterpret_cast<const FIoStoreTocCompressedBlockEntry*>(ChunkOffsetLengths + Header.TocEntryCount);
	OutTocResource.CompressionBlocks = MakeArrayView<FIoStoreTocCompressedBlockEntry const>(CompressionBlocks, Header.TocCompressedBlockEntryCount);

	// Compression methods
	OutTocResource.CompressionMethods.Reserve(Header.CompressionMethodNameCount + 1);
	OutTocResource.CompressionMethods.Add(NAME_None);

	const ANSICHAR* AnsiCompressionMethodNames = reinterpret_cast<const ANSICHAR*>(CompressionBlocks + Header.TocCompressedBlockEntryCount);
	for (uint32 CompressonNameIndex = 0; CompressonNameIndex < Header.CompressionMethodNameCount; CompressonNameIndex++)
	{
		const ANSICHAR* AnsiCompressionMethodName = AnsiCompressionMethodNames + CompressonNameIndex * Header.CompressionMethodNameLength;
		OutTocResource.CompressionMethods.Add(FName(AnsiCompressionMethodName));
	}

	// Chunk block signatures
	const uint8* SignatureBuffer = reinterpret_cast<const uint8*>(AnsiCompressionMethodNames + Header.CompressionMethodNameCount * Header.CompressionMethodNameLength);
	const uint8* TocMeta = SignatureBuffer;

	const bool bIsSigned = EnumHasAnyFlags(Header.ContainerFlags, EIoContainerFlags::Signed);
	if (IsSigningEnabled() || bIsSigned)
	{
		if (!bIsSigned)
		{
			return FIoStatus(EIoErrorCode::SignatureError, TEXT("Missing signature"));
		}

		const int32* HashSize = reinterpret_cast<const int32*>(SignatureBuffer);
		TArrayView<const uint8> TocSignature = MakeArrayView<const uint8>(reinterpret_cast<const uint8*>(HashSize + 1), *HashSize);
		TArrayView<const uint8> BlockSignature = MakeArrayView<const uint8>(TocSignature.GetData() + *HashSize, *HashSize);
		TArrayView<const FSHAHash> ChunkBlockSignatures = MakeArrayView<const FSHAHash>(reinterpret_cast<const FSHAHash*>(BlockSignature.GetData() + *HashSize), Header.TocCompressedBlockEntryCount);

		// Adjust address to meta data
		TocMeta = reinterpret_cast<const uint8*>(ChunkBlockSignatures.GetData() + ChunkBlockSignatures.Num());

		OutTocResource.ChunkBlockSignatures = ChunkBlockSignatures;

		if (IsSigningEnabled())
		{
			FIoStatus SignatureStatus = ValidateContainerSignature(GetPublicSigningKey(), Header, OutTocResource.ChunkBlockSignatures, TocSignature, BlockSignature);
			if (!SignatureStatus.IsOk())
			{
				return SignatureStatus;
			}
		}
	}

	// Meta
	if (ReadOptions == EIoStoreTocReadOptions::IncludeTocMeta)
	{
		const FIoStoreTocEntryMeta* ChunkMetas = reinterpret_cast<const FIoStoreTocEntryMeta*>(TocMeta);
		OutTocResource.ChunkMetas = MakeArrayView<FIoStoreTocEntryMeta const>(ChunkMetas, Header.TocEntryCount);
	}

	return FIoStatus::Ok;
}

TIoStatusOr<uint64> FIoStoreTocResource::Write(
	const TCHAR* TocFilePath,
	FIoStoreTocResource& TocResource,
	const FIoContainerSettings& ContainerSettings,
	const FIoStoreWriterSettings& WriterSettings)
{
	check(TocFilePath != nullptr);

	IPlatformFile& Ipf = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> TocFileHandle(Ipf.OpenWrite(TocFilePath, /* append */ false, /* allowread */ true));

	if (!TocFileHandle)
	{
		FIoStatus Status = FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore TOC file '") << TocFilePath << TEXT("'");
		return Status;
	}

	if (TocResource.ChunkIds.Num() != TocResource.ChunkOffsetLengths.Num())
	{
		return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("Number of TOC chunk IDs doesn't match the number of offsets"));
	}

	if (TocResource.ChunkIds.Num() != TocResource.ChunkMetas.Num())
	{
		return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("Number of TOC chunk IDs doesn't match the number of chunk meta data"));
	}

	FMemory::Memzero(&TocResource.Header, sizeof(FIoStoreTocHeader));

	FIoStoreTocHeader& TocHeader = TocResource.Header;
	TocHeader.MakeMagic();
	TocHeader.Version = static_cast<uint8>(EIoStoreTocVersion::Latest);
	TocHeader.TocHeaderSize = sizeof(TocHeader);
	TocHeader.TocEntryCount = TocResource.ChunkIds.Num();
	TocHeader.TocCompressedBlockEntryCount = TocResource.CompressionBlocks.Num();
	TocHeader.TocCompressedBlockEntrySize = sizeof(FIoStoreTocCompressedBlockEntry);
	TocHeader.CompressionBlockSize = uint32(WriterSettings.CompressionBlockSize);
	TocHeader.CompressionMethodNameCount = TocResource.CompressionMethods.Num();
	TocHeader.CompressionMethodNameLength = FIoStoreTocResource::CompressionMethodNameLen;
	TocHeader.ContainerId = ContainerSettings.ContainerId;
	TocHeader.EncryptionKeyGuid = ContainerSettings.EncryptionKeyGuid;
	TocHeader.ContainerFlags = ContainerSettings.ContainerFlags;

	TocFileHandle->Seek(0);

	// Header
	if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(&TocResource.Header), sizeof(FIoStoreTocHeader)))
	{
		return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write TOC header"));
	}

	// Chunk IDs
	if (!WriteArray(TocFileHandle.Get(), TocResource.ChunkIds))
	{
		return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write chunk ids"));
	}

	// Chunk offsets
	if (!WriteArray(TocFileHandle.Get(), TocResource.ChunkOffsetLengths))
	{
		return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write chunk offsets"));
	}

	// Compression blocks
	if (!WriteArray(TocFileHandle.Get(), TocResource.CompressionBlocks))
	{
		return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write chunk block entries"));
	}

	// Compression methods
	ANSICHAR AnsiMethodName[FIoStoreTocResource::CompressionMethodNameLen];

	for (FName MethodName : TocResource.CompressionMethods)
	{
		FMemory::Memzero(AnsiMethodName, FIoStoreTocResource::CompressionMethodNameLen);
		FCStringAnsi::Strcpy(AnsiMethodName, FIoStoreTocResource::CompressionMethodNameLen, TCHAR_TO_ANSI(*MethodName.ToString()));

		if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(AnsiMethodName), FIoStoreTocResource::CompressionMethodNameLen))
		{
			return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write compression method TOC entry"));
		}
	}

	// Chunk block signatures
	if (EnumHasAnyFlags(TocHeader.ContainerFlags, EIoContainerFlags::Signed))
	{
		TArray<uint8> TocSignature, BlockSignature;
		check(TocResource.ChunkBlockSignatures.Num() == TocResource.CompressionBlocks.Num());

		FIoStatus SignatureStatus = CreateContainerSignature(
			ContainerSettings.SigningKey,
			TocHeader,
			TocResource.ChunkBlockSignatures,
			TocSignature,
			BlockSignature);

		if (!SignatureStatus .IsOk())
		{
			return SignatureStatus;
		}

		check(TocSignature.Num() == BlockSignature.Num());

		const int32 HashSize = TocSignature.Num();
		TocFileHandle->Write(reinterpret_cast<const uint8*>(&HashSize), sizeof(int32));
		TocFileHandle->Write(TocSignature.GetData(), HashSize);
		TocFileHandle->Write(BlockSignature.GetData(), HashSize);

		if (!WriteArray(TocFileHandle.Get(), TocResource.ChunkBlockSignatures))
		{
			return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write chunk block signatures"));
		}
	}

	// Meta
	if (!WriteArray(TocFileHandle.Get(), TocResource.ChunkMetas))
	{
		return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write chunk meta data"));
	}

	TocFileHandle->Flush(true);

	return TocFileHandle->Tell();
}
