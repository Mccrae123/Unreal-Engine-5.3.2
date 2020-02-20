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

//////////////////////////////////////////////////////////////////////////

constexpr char FIoStoreTocHeader::TocMagicImg[];

//////////////////////////////////////////////////////////////////////////

FIoStoreEnvironment::FIoStoreEnvironment()
{
}

FIoStoreEnvironment::~FIoStoreEnvironment()
{
}

void FIoStoreEnvironment::InitializeFileEnvironment(FStringView InPath)
{
	Path = InPath;
}

//////////////////////////////////////////////////////////////////////////

struct FIoStoreWriterBlock
{
	FIoStoreWriterBlock* Next = nullptr;
	FName CompressionMethod = NAME_None;
	uint64 CompressedSize = 0;
	int64 AlignmentOrPadding = 0;
	FIoBuffer UncompressedData;
	FIoBuffer CompressedData;
	FGraphEventRef CompressionTask;
	bool bShouldCompress = false;
};

class FIoStoreWriterContextImpl
{
public:
	FIoStoreWriterContextImpl()
	{

	}

	~FIoStoreWriterContextImpl()
	{
		if (FreeBlockEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(FreeBlockEvent);
		}
	}

	UE_NODISCARD FIoStatus Initialize(const FIoStoreWriterSettings& InWriterSettings)
	{
		check(InWriterSettings.CompressionBlockSize > 0);
		WriterSettings = InWriterSettings;
		FreeBlockEvent = FPlatformProcess::GetSynchEventFromPool(false);
		int32 BlockCount = int32((2ull << 30) / WriterSettings.CompressionBlockSize);
		Blocks.SetNum(BlockCount);
		for (int32 BlockIndex = 0; BlockIndex < BlockCount; ++BlockIndex)
		{
			FIoStoreWriterBlock& Block = Blocks[BlockIndex];
			Block.UncompressedData = FIoBuffer(WriterSettings.CompressionBlockSize);
			int32 MaxCompressedSize = FCompression::CompressMemoryBound(WriterSettings.CompressionMethod, int32(WriterSettings.CompressionBlockSize));
			Block.CompressedData = FIoBuffer(MaxCompressedSize);
			Block.Next = FirstFreeBlock;
			FirstFreeBlock = &Block;
		}

		return FIoStatus::Ok;
	}

	const FIoStoreWriterSettings& Settings() const
	{
		return WriterSettings;
	}

	FIoStoreWriterBlock* AllocBlock()
	{
		for (;;)
		{
			{
				FScopeLock Lock(&FreeBlocksCritical);
				if (FirstFreeBlock)
				{
					FIoStoreWriterBlock* Result = FirstFreeBlock;
					FirstFreeBlock = FirstFreeBlock->Next;
					return Result;
				}
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(WaitForBlock);
				FreeBlockEvent->Wait();
			}
		}
	}

	void FreeBlock(FIoStoreWriterBlock* Block)
	{
		Block->AlignmentOrPadding = 0;
		{
			FScopeLock Lock(&FreeBlocksCritical);
			Block->Next = FirstFreeBlock;
			FirstFreeBlock = Block;
		}
		FreeBlockEvent->Trigger();
	}

private:
	FIoStoreWriterSettings WriterSettings;
	TArray<FIoStoreWriterBlock> Blocks;
	FCriticalSection FreeBlocksCritical;
	FIoStoreWriterBlock* FirstFreeBlock = nullptr;
	FEvent* FreeBlockEvent = nullptr;
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

static int64 GetPadding(const int64 Offset, const int64 Alignment)
{
	return (Alignment - (Offset % Alignment)) % Alignment;
}

class FChunkWriter
{
public:
	FChunkWriter() { }

	virtual ~FChunkWriter()
	{
		if (WriteQueueEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(WriteQueueEvent);
		}
	}

	FIoStatus Initialize(FIoStoreWriterContextImpl& InContext, const TCHAR* InFileName, bool bInIsContainerCompressed)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(InitializeChunkWriter);
		WriterContext = &InContext;
		bIsContainerCompressed = bInIsContainerCompressed;
		check(WriterContext->Settings().CompressionBlockSize > 0);
		CompressionInfo.BlockSize = WriterContext->Settings().CompressionBlockSize;

		IPlatformFile& Ipf = IPlatformFile::GetPlatformPhysical();
		FileHandle.Reset(Ipf.OpenWrite(InFileName, /* append */ false, /* allowread */ true));
		if (!FileHandle)
		{
			return FIoStatus(EIoErrorCode::FileNotOpen, TEXT("Failed to open container file handle"));
		}

		WriteQueueEvent = FPlatformProcess::GetSynchEventFromPool(false);
		WriterTask = Async(EAsyncExecution::Thread, [this]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WriteContainerThread);
			for (;;)
			{
				FIoStoreWriterBlock* NextWriteQueueItem = nullptr;
				{
					FScopeLock Lock(&WriteQueueCritical);
					NextWriteQueueItem = WriteQueueHead;
					WriteQueueHead = WriteQueueTail = nullptr;
				}
				if (!NextWriteQueueItem && bAllScheduled)
				{
					return;
				}
				while (NextWriteQueueItem)
				{
					FIoStoreWriterBlock* CurrentWriteQueueItem = NextWriteQueueItem;
					NextWriteQueueItem = NextWriteQueueItem->Next;

					if (CurrentWriteQueueItem->bShouldCompress)
					{
						FTaskGraphInterface::Get().WaitUntilTaskCompletes(CurrentWriteQueueItem->CompressionTask);
					}
					if (CurrentWriteQueueItem->AlignmentOrPadding > 0)
					{
						const int64 Padding = bIsContainerCompressed
							? GetPadding(FileHandle->Tell(), CurrentWriteQueueItem->AlignmentOrPadding)
							: CurrentWriteQueueItem->AlignmentOrPadding;
						if (Padding > 0)
						{
							AlignmentPaddingBuffer.SetNumZeroed(int32(Padding), false);
							FileHandle->Write(AlignmentPaddingBuffer.GetData(), Padding);
						}
						check(bIsContainerCompressed && (FileHandle->Tell() % CurrentWriteQueueItem->AlignmentOrPadding) == 0);
					}
					const int64 CompressedFileOffset = FileHandle->Tell();
					FIoStoreCompressedBlockEntry CompressedBlockEntry;
					CompressedBlockEntry.OffsetAndLength.SetOffset(CompressedFileOffset);
					const uint8* SourceData;
					uint64 SourceDataSize;
					if (CurrentWriteQueueItem->CompressionMethod == NAME_None)
					{
						SourceData = CurrentWriteQueueItem->UncompressedData.Data();
						SourceDataSize = CurrentWriteQueueItem->UncompressedData.DataSize();
					}
					else
					{
						SourceData = CurrentWriteQueueItem->CompressedData.Data();
						SourceDataSize = CurrentWriteQueueItem->CompressedSize;
					}

					FIoBuffer& SourceBuffer = CurrentWriteQueueItem->CompressionMethod == NAME_None ? CurrentWriteQueueItem->UncompressedData : CurrentWriteQueueItem->CompressedData;
					CompressedBlockEntry.OffsetAndLength.SetLength(SourceDataSize);
					CompressedBlockEntry.CompressionMethodIndex = CompressionInfo.GetCompressionMethodIndex(CurrentWriteQueueItem->CompressionMethod);
					CompressionInfo.BlockEntries.Emplace(CompressedBlockEntry);
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(WriteBlockToFile);
						FileHandle->Write(SourceData, SourceDataSize);
					}
					WriterContext->FreeBlock(CurrentWriteQueueItem);
				}
				if (!bAllScheduled)
				{
					WriteQueueEvent->Wait();
				}
			}
		});
		return FIoStatus::Ok;
	}

	TIoStatusOr<FIoStoreTocEntry> Write(FIoChunkId ChunkId, FIoBuffer Chunk, FIoWriteOptions WriteOptions)
	{
		if (CurrentBlockOffset > 0 && WriteOptions.bCompressed != CurrentBlock->bShouldCompress)
		{
			FlushCurrentBlock();
		}

		if (WriteOptions.Alignment > 0)
		{
			check(!WriteOptions.bCompressed);
			FlushCurrentBlock();

			CurrentBlock = WriterContext->AllocBlock();
			CurrentBlock->AlignmentOrPadding = WriteOptions.Alignment;
			if (!bIsContainerCompressed)
			{
				CurrentBlock->AlignmentOrPadding = GetPadding(UncompressedOffset, WriteOptions.Alignment); 
				UncompressedOffset += CurrentBlock->AlignmentOrPadding;
			}
		}

		FIoStoreTocEntry TocEntry;
		TocEntry.SetOffset(UncompressedOffset);
		TocEntry.SetLength(Chunk.DataSize());
		TocEntry.ChunkId = ChunkId;

		UncompressedOffset += Chunk.DataSize();
	
		uint64 RemainingBytesInChunk = Chunk.DataSize();
		uint64 OffsetInChunk = 0;
		while (RemainingBytesInChunk > 0)
		{
			if (!CurrentBlock)
			{
				CurrentBlock = WriterContext->AllocBlock();
			}
			check(CurrentBlockOffset == 0 || CurrentBlock->bShouldCompress == WriteOptions.bCompressed);
			check(CurrentBlock->UncompressedData.DataSize() > CurrentBlockOffset);
			CurrentBlock->bShouldCompress = WriteOptions.bCompressed;
			uint64 BytesToWrite = FMath::Min(RemainingBytesInChunk, CurrentBlock->UncompressedData.DataSize() - CurrentBlockOffset);
			FMemory::Memcpy(CurrentBlock->UncompressedData.Data() + CurrentBlockOffset, Chunk.Data() + OffsetInChunk, BytesToWrite);
			OffsetInChunk += BytesToWrite;
			check(RemainingBytesInChunk >= BytesToWrite);
			RemainingBytesInChunk -= BytesToWrite;
			CurrentBlockOffset += BytesToWrite;
			if (CurrentBlockOffset == CurrentBlock->UncompressedData.DataSize())
			{
				FlushCurrentBlock();
			}
		}
		
		return TocEntry;
	}

	FIoStatus Flush()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EndWriteContainer);
		FlushCurrentBlock();
		bAllScheduled = true;
		WriteQueueEvent->Trigger();
		WriterTask.Wait();
		
		check(!WriteQueueHead);
		check(!WriteQueueTail);
		bool bIsUncompressed = true;
		for (const FIoStoreCompressedBlockEntry& CompressedBlock : CompressionInfo.BlockEntries)
		{
			if (CompressedBlock.CompressionMethodIndex != FIoStoreCompressionInfo::InvalidCompressionIndex)
			{
				bIsUncompressed = false;
				break;
			}
		}
		if (bIsUncompressed)
		{
			CompressionInfo.BlockEntries.Empty();
			CompressionInfo.BlockSize = 0;
		}
		CompressionInfo.UncompressedContainerSize = UncompressedOffset;
		CompressionInfo.CompressedContainerSize = FileHandle.IsValid() ? FileHandle->Tell() : 0;

		return FIoStatus::Ok;
	}

	const FIoStoreCompressionInfo& GetCompressionInfo()
	{
		return CompressionInfo;
	}

private:
	void CompressBlock(FIoStoreWriterBlock* Block)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CompressBlock);
		int32 CompressedSize = int32(Block->CompressedData.DataSize());
		const bool bCompressed = FCompression::CompressMemory(
			WriterContext->Settings().CompressionMethod,
			Block->CompressedData.Data(),
			CompressedSize,
			Block->UncompressedData.Data(),
			int32(Block->UncompressedData.DataSize()));
		check(bCompressed);
		check(CompressedSize > 0);

		if (CompressedSize > Block->UncompressedData.DataSize())
		{
			Block->CompressionMethod = NAME_None;
			Block->CompressedSize = Block->UncompressedData.DataSize();
		}
		else
		{
			Block->CompressionMethod = WriterContext->Settings().CompressionMethod;
			Block->CompressedSize = CompressedSize;
		}
	}

	void FlushCurrentBlock()
	{
		if (!CurrentBlock)
		{
			return;
		}
		if (CurrentBlockOffset < CurrentBlock->UncompressedData.DataSize())
		{
			uint64 PadCount = CurrentBlock->UncompressedData.DataSize() - CurrentBlockOffset;
			FMemory::Memzero(CurrentBlock->UncompressedData.Data() + CurrentBlockOffset, PadCount);
			UncompressedOffset += PadCount;
		}
		if (CurrentBlock->bShouldCompress)
		{
			FIoStoreWriterBlock* ReadyToCompressBlock = CurrentBlock;
			CurrentBlock->CompressionTask = FFunctionGraphTask::CreateAndDispatchWhenReady([this, ReadyToCompressBlock]()
			{
				CompressBlock(ReadyToCompressBlock);
			}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask);
		}
		else
		{
			CurrentBlock->CompressionMethod = NAME_None;
			CurrentBlock->CompressedSize = CurrentBlock->UncompressedData.DataSize();
		}
		{
			FScopeLock Lock(&WriteQueueCritical);
			if (!WriteQueueTail)
			{
				WriteQueueHead = WriteQueueTail = CurrentBlock;
			}
			else
			{
				WriteQueueTail->Next = CurrentBlock;
				WriteQueueTail = CurrentBlock;
			}
			CurrentBlock->Next = nullptr;
		}
		WriteQueueEvent->Trigger();
		CurrentBlock = nullptr;
		CurrentBlockOffset = 0;
	}

	FIoStoreWriterContextImpl* WriterContext = nullptr;
	FIoStoreCompressionInfo CompressionInfo;
	TUniquePtr<IFileHandle> FileHandle;
	uint64 UncompressedOffset = 0;
	FIoStoreWriterBlock* CurrentBlock = nullptr;
	uint64 CurrentBlockOffset = 0;
	TFuture<void> WriterTask;
	FCriticalSection WriteQueueCritical;
	FIoStoreWriterBlock* WriteQueueHead = nullptr;
	FIoStoreWriterBlock* WriteQueueTail = nullptr;
	FEvent* WriteQueueEvent = nullptr;
	TAtomic<bool> bAllScheduled{ false };
	TArray<uint8> AlignmentPaddingBuffer;
	bool bIsContainerCompressed = false;
};

//////////////////////////////////////////////////////////////////////////

class FIoStoreWriterImpl
{
public:
	FIoStoreWriterImpl(FIoStoreEnvironment& InEnvironment)
	:	Environment(InEnvironment)
	{
	}

	UE_NODISCARD FIoStatus Initialize(FIoStoreWriterContextImpl& InContext, bool bInIsContainerCompressed)
	{
		IPlatformFile& Ipf = IPlatformFile::GetPlatformPhysical();

		FString TocFilePath = Environment.GetPath() + TEXT(".utoc");
		FString ContainerFilePath = Environment.GetPath() + TEXT(".ucas");

		Result.ContainerName = FPaths::GetBaseFilename(Environment.GetPath());
		Result.CompressionMethod = InContext.Settings().CompressionMethod;

		Ipf.CreateDirectoryTree(*FPaths::GetPath(ContainerFilePath));

		ChunkWriter = MakeUnique<FChunkWriter>();
		FIoStatus Status = ChunkWriter->Initialize(InContext, *ContainerFilePath, bInIsContainerCompressed);

		if (!Status.IsOk())
		{ 
			ChunkWriter.Reset();

			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore container file '") << *ContainerFilePath << TEXT("'");
		}

		TocFileHandle.Reset(Ipf.OpenWrite(*TocFilePath, /* append */ false, /* allowread */ true));

		if (!TocFileHandle)
		{
			return FIoStatusBuilder(EIoErrorCode::FileOpenFailed) << TEXT("Failed to open IoStore TOC file '") << *TocFilePath << TEXT("'");
		}

		if (InContext.Settings().bEnableCsvOutput)
		{
			Status = EnableCsvOutput();
		}

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

	UE_NODISCARD FIoStatus Append(FIoChunkId ChunkId, FIoBuffer Chunk, FIoWriteOptions WriteOptions)
	{
		if (!ChunkWriter)
		{
			return FIoStatus(EIoErrorCode::FileNotOpen, TEXT("No container file to append to"));
		}

		if (!ChunkId.IsValid())
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("ChunkId is not valid!"));
		}

		if (Toc.Find(ChunkId) != nullptr)
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("ChunkId is already mapped"));
		}

		IsMetadataDirty = true;

		TIoStatusOr<FIoStoreTocEntry> TocEntryStatus = ChunkWriter->Write(ChunkId, Chunk, WriteOptions);

		if (TocEntryStatus.IsOk())
		{
			FIoStoreTocEntry TocEntry = TocEntryStatus.ConsumeValueOrDie();
			Toc.Add(ChunkId, TocEntry);

			if (CsvArchive)
			{
				ANSICHAR Line[MAX_SPRINTF];
				FCStringAnsi::Sprintf(Line, "%s,%lld,%lld\n", (WriteOptions.DebugName ? TCHAR_TO_ANSI(WriteOptions.DebugName) : ""), TocEntry.GetOffset(), TocEntry.GetLength());
				CsvArchive->Serialize(Line, FCStringAnsi::Strlen(Line));
			}

			return FIoStatus::Ok;
		}
		else
		{
			return FIoStatus(EIoErrorCode::WriteError, TEXT("Append failed"));
		}
	}

	UE_NODISCARD FIoStatus MapPartialRange(FIoChunkId OriginalChunkId, uint64 Offset, uint64 Length, FIoChunkId ChunkIdPartialRange)
	{
		//TODO: Does RelativeOffset + Length overflow?

		const FIoStoreTocEntry* Entry = Toc.Find(OriginalChunkId);
		if (Entry == nullptr)
		{
			return FIoStatus(EIoErrorCode::UnknownChunkID, TEXT("OriginalChunkId does not exist in the container"));
		}

		if (!ChunkIdPartialRange.IsValid())
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("ChunkIdPartialRange is not valid!"));
		}

		if (Toc.Find(ChunkIdPartialRange) != nullptr)
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("ChunkIdPartialRange is already mapped"));
		}

		if (Offset + Length > Entry->GetLength())
		{
			return FIoStatus(EIoErrorCode::InvalidParameter, TEXT("The given range (Offset/Length) is not within the bounds of OriginalChunkId's data"));
		}

		FIoStoreTocEntry TocEntry;

		TocEntry.SetOffset(Entry->GetOffset() + Offset);
		TocEntry.SetLength(Length);
		TocEntry.ChunkId = ChunkIdPartialRange;

		Toc.Add(ChunkIdPartialRange, TocEntry);

		IsMetadataDirty = true;

		return FIoStatus::Ok;
	}

	UE_NODISCARD TIoStatusOr<FIoStoreWriterResult> Flush()
	{
		if (!IsMetadataDirty)
		{
			return Result;
		}

		IsMetadataDirty = false;

		ChunkWriter->Flush();
		const FIoStoreCompressionInfo& CompressionInfo = ChunkWriter->GetCompressionInfo();

		FIoStoreTocHeader TocHeader;
		FMemory::Memzero(&TocHeader, sizeof(TocHeader));

		TocHeader.MakeMagic();
		TocHeader.TocHeaderSize = sizeof(TocHeader);
		TocHeader.TocEntryCount = Toc.Num();
		TocHeader.TocEntrySize = sizeof(FIoStoreTocEntry);
		TocHeader.CompressionBlockCount = CompressionInfo.BlockEntries.Num();
		TocHeader.CompressionBlockSize = uint32(CompressionInfo.BlockSize);
		TocHeader.CompressionNameCount = CompressionInfo.CompressionMethods.Num();

		TocFileHandle->Seek(0);
		if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(&TocHeader), sizeof(TocHeader)))
		{
			return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write TOC header"));
		}

		// Chunk entries
		for (auto& _: Toc)
		{
			FIoStoreTocEntry& TocEntry = _.Value;
			
			if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(&TocEntry), sizeof(TocEntry)))
			{
				return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write TOC entry"));
			}
		}

		// Compression blocks
		for (const FIoStoreCompressedBlockEntry& CompressedBlockEntry : CompressionInfo.BlockEntries)
		{
			if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(&CompressedBlockEntry), sizeof(CompressedBlockEntry)))
			{
				return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write compression block TOC entry"));
			}
		}

		// Compression methods
		ANSICHAR AnsiMethodName[FIoStoreCompressionInfo::CompressionMethodNameLen];

		for (FName MethodName : CompressionInfo.CompressionMethods)
		{
			FMemory::Memzero(AnsiMethodName, FIoStoreCompressionInfo::CompressionMethodNameLen);
			FCStringAnsi::Strcpy(AnsiMethodName, FIoStoreCompressionInfo::CompressionMethodNameLen, TCHAR_TO_ANSI(*MethodName.ToString()));

			if (!TocFileHandle->Write(reinterpret_cast<const uint8*>(AnsiMethodName), FIoStoreCompressionInfo::CompressionMethodNameLen))
			{
				return FIoStatus(EIoErrorCode::WriteError, TEXT("Failed to write compression method TOC entry"));
			}
		}

		Result.TocSize = TocFileHandle->Tell();
		Result.TocEntryCount = TocHeader.TocEntryCount;
		Result.UncompressedContainerSize = CompressionInfo.UncompressedContainerSize;
		Result.CompressedContainerSize = CompressionInfo.CompressedContainerSize;

		return Result;
	}

private:
	friend class FIoStoreWriter;
	FIoStoreEnvironment&				Environment;
	TMap<FIoChunkId, FIoStoreTocEntry>	Toc;
	TUniquePtr<FChunkWriter>			ChunkWriter;
	TUniquePtr<IFileHandle>				TocFileHandle;
	TUniquePtr<FArchive>				CsvArchive;
	FIoStoreWriterResult				Result;
	bool								IsMetadataDirty = true;
};

FIoStoreWriter::FIoStoreWriter(FIoStoreEnvironment& InEnvironment)
:	Impl(new FIoStoreWriterImpl(InEnvironment))
{
}

FIoStoreWriter::~FIoStoreWriter()
{
	TIoStatusOr<FIoStoreWriterResult> Status = Impl->Flush();
	check(Status.IsOk());
}

FIoStatus FIoStoreWriter::Initialize(const FIoStoreWriterContext& Context, bool bIsContainerCompressed)
{
	return Impl->Initialize(*Context.Impl, bIsContainerCompressed);
}

FIoStatus FIoStoreWriter::Append(FIoChunkId ChunkId, FIoBuffer Chunk, FIoWriteOptions WriteOptions)
{
	return Impl->Append(ChunkId, Chunk, WriteOptions);
}

FIoStatus FIoStoreWriter::MapPartialRange(FIoChunkId OriginalChunkId, uint64 Offset, uint64 Length, FIoChunkId ChunkIdPartialRange)
{
	return Impl->MapPartialRange(OriginalChunkId, Offset, Length, ChunkIdPartialRange);
}

TIoStatusOr<FIoStoreWriterResult> FIoStoreWriter::Flush()
{
	return Impl->Flush();
}
