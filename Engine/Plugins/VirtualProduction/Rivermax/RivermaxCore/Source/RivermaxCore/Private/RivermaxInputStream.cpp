// Copyright Epic Games, Inc. All Rights Reserved.

#include "RivermaxInputStream.h"

#include "Async/Async.h"
#include "CudaModule.h"
#include "ID3D12DynamicRHI.h"
#include "IRivermaxCoreModule.h"
#include "IRivermaxManager.h"
#include "Misc/ByteSwap.h"
#include "RivermaxLog.h"
#include "RivermaxUtils.h"

#if PLATFORM_WINDOWS
#include <WS2tcpip.h>
#endif

namespace UE::RivermaxCore::Private
{
	static TAutoConsoleVariable<float> CVarWaitForCompletionTimeout(
		TEXT("Rivermax.Input.WaitCompletionTimeout"),
		0.25,
		TEXT("Maximum time to wait, in seconds, when waiting for a memory copy operation to complete on the gpu."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarExpectedPayloadSize(
		TEXT("Rivermax.Input.ExpectedPayloadSize"),
		1500,
		TEXT("Expected payload size used to initialize rivermax stream."),
		ECVF_Default);


#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(push, 1)
#endif
	struct FRawRTPHeader
	{
		uint32 ContributingSourceCount : 4;
		uint32 ExtensionBit : 1;
		uint32 PaddingBit : 1;
		uint32 Version : 2;
		uint32 PayloadType : 7;
		uint32 MarkerBit : 1;
		uint32 SequenceNumber : 16;
		uint32 Timestamp : 32;
		uint32 SynchronizationSource : 32;
		uint32 ExtendedSequenceNumber : 16;
		//SRD 1
		uint32 SRDLength1 : 16;
		uint32 SRDRowNumberHigh1 : 7;
		uint32 FieldIdentification1 : 1;
		uint32 SRDRowNumberLow1 : 8;
		uint32 SRDOffsetHigh1 : 7;
		uint32 ContinuationBit1 : 1;
		uint32 SRDOffsetLow1 : 8;
		//SRD 2
		uint32 SRDLength2 : 16;
		uint32 SRDRowNumberHigh2 : 7;
		uint32 FieldIdentification2 : 1;
		uint32 SRDRowNumberLow2 : 8;
		uint32 SRDOffsetHigh2 : 7;
		uint32 ContinuationBit2 : 1;
		uint32 SRDOffsetLow2 : 8;

		uint16 GetSrd1RowNumber() const { return ((SRDRowNumberHigh1 << 8) | SRDRowNumberLow1); }
		uint16 GetSrd1Offset() const { return ((SRDOffsetHigh1 << 8) | SRDOffsetLow1); }

		uint16 GetSrd2RowNumber() const { return ((SRDRowNumberHigh2 << 8) | SRDRowNumberLow2); }
		uint16 GetSrd2Offset() const { return ((SRDOffsetHigh2 << 8) | SRDOffsetLow2); }
	};
#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(pop)
#endif

	struct FSRDHeader
	{
		/** Length of payload. Is a multiple of pgroup (see pixel formats) */
		uint16 Length = 0;

		/** False if progressive or first field of interlace. True if second field of interlace */
		bool bIsFieldOne = false;

		/** Video line number, starts at 0 */
		uint16 RowNumber = 0;

		/** Whether another SRD is following this one */
		bool bHasContinuation = false;
		
		/** Location of the first pixel in payload, in pixel */
		uint16 DataOffset = 0;
	};
	
	struct FRTPHeader
	{
		/** Sequence number including extension if present */
		uint32 SequencerNumber = 0;

		/** Timestamp of frame in the specified clock resolution. Video is typically 90kHz */
		uint32 Timestamp = 0;

		/** Identification of this stream */
		uint32 SyncSouceId = 0;

		/** Whether extensions (SRD headers) are present */
		bool bHasExtension = false;

		/** True if RTP packet is last of video stream */
		bool bIsMarkerBit = false;

		/** Only supports 2 SRD for now. Adjust if needed */
		FSRDHeader SRD1;
		FSRDHeader SRD2;
	};

	uint8* GetRTPHeaderPointer(uint8* InHeader)
	{
		check(InHeader);

		static constexpr uint32 ETH_TYPE_802_1Q = 0x8100;          /* 802.1Q VLAN Extended Header  */
		static constexpr uint32 RTP_HEADER_SIZE = 12;
		uint16* ETHProto = (uint16_t*)(InHeader + RTP_HEADER_SIZE);
		if (ETH_TYPE_802_1Q == ByteSwap(*ETHProto))
		{
			InHeader += 46; // 802 + 802.1Q + IP + UDP
		}
		else
		{
			InHeader += 42; // 802 + IP + UDP
		}
		return InHeader;
	}

	FRivermaxInputStream::FRivermaxInputStream()
	{

	}

	FRivermaxInputStream::~FRivermaxInputStream()
	{
		Uninitialize();
	}

	bool FRivermaxInputStream::Initialize(const FRivermaxStreamOptions& InOptions, IRivermaxInputStreamListener& InListener)
	{
		IRivermaxCoreModule& RivermaxModule = FModuleManager::LoadModuleChecked<IRivermaxCoreModule>(TEXT("RivermaxCore"));
		if (RivermaxModule.GetRivermaxManager()->IsInitialized() == false)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't create Rivermax Input Stream. Library isn't initialized."));
			return false;
		}

		Options = InOptions;
		Listener = &InListener;
		FormatInfo = FStandardVideoFormat::GetVideoFormatInfo(Options.PixelFormat);
		ExpectedPayloadSize = CVarExpectedPayloadSize.GetValueOnGameThread();

		InitTaskFuture = Async(EAsyncExecution::TaskGraph, [this]()
		{
			// If the stream is trying to shutdown before the init task has even started, don't bother
			if (bIsShuttingDown)
			{
				return;
			}

			bool bWasSuccessful = false;
			int32 FlowId = 0; //todo configure

			//Configure local IP interface
			const rmax_in_stream_type StreamType = RMAX_RAW_PACKET;
			sockaddr_in RivermaxInterface;
			memset(&RivermaxInterface, 0, sizeof(RivermaxInterface));
			if (inet_pton(AF_INET, StringCast<ANSICHAR>(*Options.InterfaceAddress).Get(), &RivermaxInterface.sin_addr) != 1)
			{
				UE_LOG(LogRivermax, Warning, TEXT("inet_pton failed to %s"), *Options.InterfaceAddress);
			}
			else
			{
				RivermaxInterface.sin_family = AF_INET;

				//Configure Flow and destination IP (multicast)
				memset(&FlowAttribute, 0, sizeof(FlowAttribute));
				FlowAttribute.local_addr.sin_family = AF_INET;
				FlowAttribute.flow_id = FlowId;
				if (inet_pton(AF_INET, StringCast<ANSICHAR>(*Options.StreamAddress).Get(), &FlowAttribute.local_addr.sin_addr) != 1)
				{
					UE_LOG(LogRivermax, Warning, TEXT("inet_pton failed to %s"), *Options.StreamAddress);
				}
				else
				{
					FlowAttribute.local_addr.sin_port = ByteSwap((uint16)Options.Port);

					const rmax_in_buffer_attr_flags_t BufferAttributeFlags = RMAX_IN_BUFFER_ATTER_FLAG_NONE; //todo whether ordering is based on sequence or extended sequence
					uint32 BufferElement = 1 << 18;//todo number of packets to allocate memory for
					rmax_in_buffer_attr BufferAttributes;
					FMemory::Memset(&BufferAttributes, 0, sizeof(BufferAttributes));
					BufferAttributes.num_of_elements = BufferElement;
					BufferAttributes.attr_flags = BufferAttributeFlags;

					FMemory::Memset(&BufferConfiguration.DataMemory, 0, sizeof(BufferConfiguration.DataMemory));

					BufferConfiguration.DataMemory.min_size = ExpectedPayloadSize;
					BufferConfiguration.DataMemory.max_size = ExpectedPayloadSize;
					BufferAttributes.data = &BufferConfiguration.DataMemory;

					FMemory::Memset(&BufferConfiguration.HeaderMemory, 0, sizeof(BufferConfiguration.HeaderMemory));
					BufferConfiguration.HeaderMemory.max_size = BufferConfiguration.HeaderMemory.min_size = BufferConfiguration.HeaderExpectedSize;
					BufferAttributes.hdr = &BufferConfiguration.HeaderMemory;

					rmax_status_t Status = rmax_in_query_buffer_size(StreamType, &RivermaxInterface, &BufferAttributes, &BufferConfiguration.PayloadSize, &BufferConfiguration.HeaderSize);
					if (Status == RMAX_OK)
					{
						AllocateBuffers();

						//Buffers configured, now configure stream and attach flow
						const rmax_in_timestamp_format TimestampFormat = rmax_in_timestamp_format::RMAX_PACKET_TIMESTAMP_RAW_NANO; //how packets are stamped. counter or nanoseconds
						const rmax_in_flags InputFlags = rmax_in_flags::RMAX_IN_CREATE_STREAM_INFO_PER_PACKET; //default value for 2110 in example
						Status = rmax_in_create_stream(StreamType, &RivermaxInterface, &BufferAttributes, TimestampFormat, InputFlags, &StreamId);
						if (Status == RMAX_OK)
						{
							Status = rmax_in_attach_flow(StreamId, &FlowAttribute);
							if (Status == RMAX_OK)
							{
								bIsActive = true;
								RivermaxThread = FRunnableThread::Create(this, TEXT("Rivermax InputStream Thread"), 128 * 1024, TPri_AboveNormal, FPlatformAffinity::GetPoolThreadMask());
								bWasSuccessful = true;

								UE_LOG(LogRivermax, Display, TEXT("Input started receiving %dx%d%s")
									, Options.AlignedResolution.X
									, Options.AlignedResolution.Y
									, bIsUsingGPUDirect ? TEXT(" using GPUDirect") : TEXT(""));
							}
							else
							{
								UE_LOG(LogRivermax, Warning, TEXT("Could not attach flow to stream. Status: %d."), Status);
							}
						}
						else
						{
							UE_LOG(LogRivermax, Warning, TEXT("Could not create stream. Status: %d."), Status);
						}
					}
					else
					{
						UE_LOG(LogRivermax, Warning, TEXT("Could not query buffer size. Status: %d"), Status);
					}
				}
			}

			Listener->OnInitializationCompleted(bWasSuccessful);
		});
		
		return true;
	}

	void FRivermaxInputStream::Uninitialize()
	{
		bIsShuttingDown = true;

		//If init task is ongoing, wait till it's done
		if (InitTaskFuture.IsReady() == false)
		{
			InitTaskFuture.Wait();
		}

		if (RivermaxThread != nullptr)
		{
			Stop();
			RivermaxThread->Kill(true);
			delete RivermaxThread;
			RivermaxThread = nullptr;
			UE_LOG(LogRivermax, Log, TEXT("Rivermax Input stream has shutdown"));
		}

		DeallocateBuffers();
	}

	void FRivermaxInputStream::Process_AnyThread()
	{
		const size_t MinChunkSize = 0;
		const size_t MaxChunkSize = 5000;
		const int Timeout = 0;
		const int Flags = 0;
		rmax_in_completion Completion;
		FMemory::Memset(&Completion, 0, sizeof(Completion));
		rmax_status_t Status = rmax_in_get_next_chunk(StreamId, MinChunkSize, MaxChunkSize, Timeout, Flags, &Completion);
		if (Status == RMAX_OK)
		{
			ParseChunks(Completion);
		}
		else
		{
			UE_LOG(LogRivermax, Warning, TEXT("Rivermax Input stream failed to get next chunk. Status: %d"), Status);
		}
	}

	bool FRivermaxInputStream::Init()
	{
		return true;
	}

	uint32 FRivermaxInputStream::Run()
	{
		while (bIsActive)
		{
			Process_AnyThread();
			LogStats();
		}

		if (StreamId)
		{
			rmax_status_t Status = rmax_in_detach_flow(StreamId, &FlowAttribute);
			if (Status != RMAX_OK)
			{
				UE_LOG(LogRivermax, Warning, TEXT("Failed to detach rivermax flow %d from input stream %d. Status: %d"), FlowAttribute.flow_id, StreamId, Status);
			}

			Status = rmax_in_destroy_stream(StreamId);

			if (Status != RMAX_OK)
			{
				UE_LOG(LogRivermax, Warning, TEXT("Failed to destroy input stream %d correctly. Status: %d"), StreamId, Status);
			}
		}

		return 0;
	}

	void FRivermaxInputStream::Stop()
	{
		bIsActive = false;
	}

	void FRivermaxInputStream::Exit()
	{

	}

	bool FRivermaxInputStream::TranslateRTPHeader(const FRawRTPHeader& RawRTPHeader, FRTPHeader& OutRTPHeader)
	{
		OutRTPHeader.Timestamp = 0;
		
		if (RawRTPHeader.Version != 2)
		{
			return false;
		}

		// Pretty sure some data needs to be swapped but can't validate that until we have other hardware generating data
		OutRTPHeader.SequencerNumber = (ByteSwap((uint16)RawRTPHeader.ExtendedSequenceNumber) << 16) | ByteSwap((uint16)RawRTPHeader.SequenceNumber);
		OutRTPHeader.Timestamp = ByteSwap(RawRTPHeader.Timestamp);
		OutRTPHeader.bIsMarkerBit = RawRTPHeader.MarkerBit;

		OutRTPHeader.SyncSouceId = RawRTPHeader.SynchronizationSource;

		OutRTPHeader.SRD1.Length = ByteSwap((uint16)RawRTPHeader.SRDLength1);
		OutRTPHeader.SRD1.DataOffset = RawRTPHeader.GetSrd1Offset();
		OutRTPHeader.SRD1.RowNumber = RawRTPHeader.GetSrd1RowNumber();
		OutRTPHeader.SRD1.bIsFieldOne = RawRTPHeader.FieldIdentification1;
		OutRTPHeader.SRD1.bHasContinuation = RawRTPHeader.ContinuationBit1;

		if (OutRTPHeader.SRD1.bHasContinuation)
		{
			OutRTPHeader.SRD2.Length = ByteSwap((uint16)RawRTPHeader.SRDLength2);
			OutRTPHeader.SRD2.DataOffset = RawRTPHeader.GetSrd2Offset();
			OutRTPHeader.SRD2.RowNumber = RawRTPHeader.GetSrd2RowNumber();
			OutRTPHeader.SRD2.bIsFieldOne = RawRTPHeader.FieldIdentification2;
			OutRTPHeader.SRD2.bHasContinuation = RawRTPHeader.ContinuationBit2;

			if (OutRTPHeader.SRD2.bHasContinuation == true)
			{
				UE_LOG(LogRivermax, Verbose, TEXT("Received SRD with more than 2 SRD which isn't supported."));
			}
		}

		return true;
	}

	void FRivermaxInputStream::ParseChunks(const rmax_in_completion& Completion)
	{
		for (uint64 StrideIndex = 0; StrideIndex < Completion.chunk_size; ++StrideIndex)
		{
			++StreamStats.ChunksReceived;
		
			ensure(Completion.hdr_ptr);
			if (Completion.hdr_ptr == nullptr)
			{
				break;
			}

			uint8* RawHeaderPtr = (uint8*)Completion.hdr_ptr + StrideIndex * (size_t)BufferConfiguration.HeaderMemory.stride_size; 
			uint8* DataPtr = (uint8*)Completion.data_ptr + StrideIndex * (size_t)BufferConfiguration.DataMemory.stride_size; // The payload is our data

			if (Completion.packet_info_arr[StrideIndex].data_size && RawHeaderPtr && DataPtr)
			{
				FRTPHeader RTPHeader;

				// Get RTPHeader address from the raw net header
				const FRawRTPHeader& RawRTPHeaderPtr = reinterpret_cast<const FRawRTPHeader&>(*GetRTPHeaderPointer(RawHeaderPtr));
				const bool bIsValid = TranslateRTPHeader(RawRTPHeaderPtr, RTPHeader);
				if (bIsValid)
				{
					// Add trace for the first packet of a frame to help visualize reception of a full frame in time
					if (bIsFirstPacketReceived == false)
					{
						const FString TraceName = FString::Format(TEXT("RmaxInput::StartingFrame {0}"), { RTPHeader.Timestamp });
						TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*TraceName);
						bIsFirstPacketReceived = true;
					}

					if (bIsFirstFrameReceived)
					{
						StreamStats.BytesReceived += Completion.packet_info_arr[StrideIndex].data_size + Completion.packet_info_arr[StrideIndex].hdr_size;

						uint64 LastSequenceNumberIncremented = StreamData.LastSequenceNumber + 1;

						bool bCanProcessSRD = true;
						const uint64 LostPackets = ((uint64)RTPHeader.SequencerNumber + 0x100000000 - LastSequenceNumberIncremented) & 0xFFFFFFFF;
						if (LostPackets > 0)
						{
							bCanProcessSRD = false;
							StreamData.WritingOffset = 0;
							StreamData.ReceivedSize = 0;
							++StreamStats.TotalPacketLossCount;
							++StreamStats.FramePacketLossCount;
							
							// For now, if packets were lost, skip the incoming frame. We could improve that and have corrupted frames instead of skipping them but can be added later
							bIsFirstFrameReceived = false;
							
							UE_LOG(LogRivermax, Warning, TEXT("Lost %llu packets"), LostPackets);
						}

						StreamData.LastSequenceNumber = RTPHeader.SequencerNumber;

						//if flags are RMAX_IN_CREATE_STREAM_INFO_PER_PACKET todo 
						{
							if (FlowAttribute.flow_id && Completion.packet_info_arr[StrideIndex].flow_id != FlowAttribute.flow_id)
							{
								UE_LOG(LogRivermax, Error, TEXT("Received data from unexpected FlowId '%d'. Expected '%d'."), Completion.packet_info_arr[StrideIndex].flow_id, FlowAttribute.flow_id);
							}
						}

						if (bCanProcessSRD)
						{
							ProcessSRD(RTPHeader, DataPtr);
							
							if (StreamData.ReceivedSize > StreamData.ExpectedSize)
							{
								UE_LOG(LogRivermax, Warning, TEXT("Received too much data (%d). Expected %d but received (%d)"), StreamData.ReceivedSize - StreamData.ExpectedSize, StreamData.ExpectedSize, StreamData.ReceivedSize);
								StreamData.WritingOffset = 0;
								StreamData.ReceivedSize = 0;
								++StreamStats.BiggerFramesCount;
							}
							else if (RTPHeader.bIsMarkerBit)
							{
								ProcessLastSRD(RTPHeader, DataPtr);
								
								StreamStats.FramePacketLossCount = 0;
								++StreamStats.EndOfFrameReceived;
							}
						}
					}
					else
					{
						if (RTPHeader.bIsMarkerBit)
						{
							StreamData.LastSequenceNumber = RTPHeader.SequencerNumber;
							PrepareNextFrame();
							bIsFirstFrameReceived = (StreamData.CurrentFrame != nullptr);
						}
					}
				}
				else
				{
					++StreamStats.InvalidHeadercount;
				}
			}
			else
			{
				++StreamStats.EmptyCompletionCount;
			}
		}
	}

	void FRivermaxInputStream::PrepareNextFrame()
	{
		using namespace UE::RivermaxCore::Private::Utils;

		FRivermaxInputVideoFrameDescriptor Descriptor;
		Descriptor.bIsUsingGPUDirect = bIsUsingGPUDirect;
		FRivermaxInputVideoFrameRequest Request;
		const int32 Stride = Options.AlignedResolution.X / FormatInfo.PixelGroupCoverage * FormatInfo.PixelGroupSize;
		Descriptor.VideoBufferSize = Options.Resolution.Y * Stride;
		Listener->OnVideoFrameRequested(Descriptor, Request);

		// Reset current frame to know when we have a valid one
		StreamData.CurrentFrame = nullptr;
		if (bIsUsingGPUDirect)
		{
			if(Request.GPUBuffer)
			{
				StreamData.CurrentFrame = GetMappedBuffer(Request.GPUBuffer);
			}
		}
		else
		{	
			if (Request.VideoBuffer)
			{
				StreamData.CurrentFrame = Request.VideoBuffer;
			}
		}

		// Verify if we were able to request a valid frame. If engine is blocked, it could happen that there is none available 
		if (StreamData.CurrentFrame == nullptr)
		{
			// If we failed getting one, reset the valid first frame received and wait for the next one
			bIsFirstFrameReceived = false;
		}

		StreamData.WritingOffset = 0;
		StreamData.ReceivedSize = 0;
		StreamData.ExpectedSize = Descriptor.VideoBufferSize;
		StreamData.DeviceWritePointerOne = nullptr;
		StreamData.SizeToWriteOne = 0;
		StreamData.DeviceWritePointerTwo = nullptr;
		StreamData.SizeToWriteTwo = 0;
		bIsFirstPacketReceived = false;

		// New frame starting, reset tracked SRD
		LastSRDLength.Reset();
	}

	void FRivermaxInputStream::LogStats()
	{
		static constexpr double LoggingInterval = 1.0;

		const double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLoggingTimestamp >= LoggingInterval)
		{
			LastLoggingTimestamp = CurrentTime;
			UE_LOG(LogRivermax, Verbose, TEXT("Stream %d stats: FrameCount: %llu, EndOfFrame: %llu, Chunks: %llu, Bytes: %llu, PacketLossInFrame: %llu, TotalPacketLoss: %llu, BiggerFrames: %llu, InvalidFrames: %llu, InvalidHeader: %llu, EmptyCompletion: %llu")
			, StreamId
			, StreamStats.FramesReceived
			, StreamStats.EndOfFrameReceived
			, StreamStats.ChunksReceived
			, StreamStats.BytesReceived
			, StreamStats.FramePacketLossCount
			, StreamStats.TotalPacketLossCount
			, StreamStats.BiggerFramesCount
			, StreamStats.InvalidFramesCount
			, StreamStats.InvalidHeadercount
			, StreamStats.EmptyCompletionCount
			);
		}
	}

	void FRivermaxInputStream::AllocateBuffers()
	{
		IRivermaxCoreModule& RivermaxModule = FModuleManager::LoadModuleChecked<IRivermaxCoreModule>(TEXT("RivermaxCore"));
		if (RivermaxModule.GetRivermaxManager()->IsGPUDirectSupported() && Options.bUseGPUDirect)
		{
			bIsUsingGPUDirect = AllocateGPUBuffers();
		}

		constexpr uint32 CacheLineSize = PLATFORM_CACHE_LINE_SIZE;
		if (bIsUsingGPUDirect == false)
		{
			BufferConfiguration.DataMemory.ptr = FMemory::Malloc(BufferConfiguration.PayloadSize, CacheLineSize);
		}
		
		BufferConfiguration.HeaderMemory.ptr = FMemory::Malloc(BufferConfiguration.HeaderSize, CacheLineSize);
	}

	bool FRivermaxInputStream::AllocateGPUBuffers()
	{
		// Allocate memory space where rivermax input will write received buffer to

		TRACE_CPUPROFILER_EVENT_SCOPE(FRivermaxInputStream::AllocateGPUBuffers);

		const ERHIInterfaceType RHIType = RHIGetInterfaceType();
		if (RHIType != ERHIInterfaceType::D3D12)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. RHI is %d but only Dx12 is supported at the moment."), RHIType);
			return false;
		}

		FCUDAModule& CudaModule = FModuleManager::GetModuleChecked<FCUDAModule>("CUDA");

		CudaModule.DriverAPI()->cuCtxPushCurrent(CudaModule.GetCudaContextForDevice(DeviceIndex));

		// Todo: Add support for mgpu. 
		CUdevice CudaDevice;
		CUresult Status = CudaModule.DriverAPI()->cuDeviceGet(&CudaDevice, DeviceIndex);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to get a Cuda device for GPU %d. Status: %d"), DeviceIndex, Status);
			return false;
		}

		CUmemAllocationProp AllocationProperties = {};
		AllocationProperties.type = CU_MEM_ALLOCATION_TYPE_PINNED;
		AllocationProperties.allocFlags.gpuDirectRDMACapable = 1; //is that required?
		AllocationProperties.allocFlags.usage = 0;
		AllocationProperties.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
		AllocationProperties.location.id = CudaDevice;

		// Get memory granularity required for cuda device. We need to align allocation with this.
		size_t Granularity;
		Status = CudaModule.DriverAPI()->cuMemGetAllocationGranularity(&Granularity, &AllocationProperties, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to get allocation granularity. Status: %d"), Status);
			return false;
		}

		// Cuda requires allocated memory to be aligned with a certain granularity
		const size_t CudaAlignedAllocation = (BufferConfiguration.PayloadSize % Granularity) ? BufferConfiguration.PayloadSize + (Granularity - (BufferConfiguration.PayloadSize % Granularity)) : BufferConfiguration.PayloadSize;
		
		CUdeviceptr CudaBaseAddress;
		constexpr CUdeviceptr InitialAddress = 0;
		constexpr int32 Flags = 0;
		Status = CudaModule.DriverAPI()->cuMemAddressReserve(&CudaBaseAddress, CudaAlignedAllocation, Granularity, InitialAddress, Flags);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to reserve memory for %d bytes. Status: %d"), CudaAlignedAllocation, Status);
			return false;
		}

		// Make the allocation on device memory
		CUmemGenericAllocationHandle Handle;
		Status = CudaModule.DriverAPI()->cuMemCreate(&Handle, CudaAlignedAllocation, &AllocationProperties, Flags);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to create memory on device. Status: %d"), Status);
			return false;
		}
		UE_LOG(LogRivermax, Verbose, TEXT("Allocated %d cuda memory"), CudaAlignedAllocation);

		bool bExit = false;
		constexpr int32 Offset = 0;
		Status = CudaModule.DriverAPI()->cuMemMap(CudaBaseAddress, CudaAlignedAllocation, Offset, Handle, Flags);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to map memory. Status: %d"), Status);
			// Need to release handle no matter what
			bExit = true;
		}

		GPUAllocatedMemorySize = CudaAlignedAllocation;
		GPUAllocatedMemoryBaseAddress = reinterpret_cast<void*>(CudaBaseAddress);

		Status = CudaModule.DriverAPI()->cuMemRelease(Handle);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to release handle. Status: %d"), Status);
			return false;
		}

		if (bExit)
		{
			return false;
		}

		// Setup access description.
		CUmemAccessDesc MemoryAccessDescription = {};
		MemoryAccessDescription.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
		MemoryAccessDescription.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
		MemoryAccessDescription.location.id = CudaDevice;
		constexpr int32 Count = 1;
		Status = CudaModule.DriverAPI()->cuMemSetAccess(CudaBaseAddress, CudaAlignedAllocation, &MemoryAccessDescription, Count);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to configure memory access. Status: %d"), Status);
			return false;
		}

		CUstream CudaStream;
		Status = CudaModule.DriverAPI()->cuStreamCreate(&CudaStream, CU_STREAM_NON_BLOCKING);
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to create its stream. Status: %d"), Status);
			return false;
		}

		GPUStream = CudaStream;

		Status = CudaModule.DriverAPI()->cuCtxSynchronize();
		if (Status != CUDA_SUCCESS)
		{
			UE_LOG(LogRivermax, Warning, TEXT("Can't initialize input to use GPUDirect. Failed to synchronize context. Status: %d"), Status);
			return false;
		}

		// Give rivermax input buffer config the pointer to gpu allocated memory
		BufferConfiguration.DataMemory.ptr = GPUAllocatedMemoryBaseAddress;

		CallbackPayload = MakeShared<FCallbackPayload>();

		return true;
	}

	void FRivermaxInputStream::DeallocateBuffers()
	{
		if (GPUAllocatedMemorySize > 0)
		{
			FCUDAModule& CudaModule = FModuleManager::GetModuleChecked<FCUDAModule>("CUDA");
			CudaModule.DriverAPI()->cuCtxPushCurrent(CudaModule.GetCudaContextForDevice(DeviceIndex));

			const CUdeviceptr BaseAddress = reinterpret_cast<CUdeviceptr>(GPUAllocatedMemoryBaseAddress);
			CUresult Status = CudaModule.DriverAPI()->cuMemUnmap(BaseAddress, GPUAllocatedMemorySize);
			if (Status != CUDA_SUCCESS)
			{
				UE_LOG(LogRivermax, Warning, TEXT("Failed to unmap cuda memory used for input stream. Status: %d"), Status);
			}

			Status = CudaModule.DriverAPI()->cuMemAddressFree(BaseAddress, GPUAllocatedMemorySize);
			if (Status != CUDA_SUCCESS)
			{
				UE_LOG(LogRivermax, Warning, TEXT("Failed to free cuda memory used for input stream. Status: %d"), Status);
			}
			UE_LOG(LogRivermax, Verbose, TEXT("Deallocated %d cuda memory at address %d"), GPUAllocatedMemorySize, GPUAllocatedMemoryBaseAddress);

			GPUAllocatedMemorySize = 0;
			GPUAllocatedMemoryBaseAddress = 0;


			for (const TPair<FRHIBuffer*, void*>& Entry : BufferGPUMemoryMap)
			{
				if (Entry.Value)
				{
					CudaModule.DriverAPI()->cuMemFree(reinterpret_cast<CUdeviceptr>(Entry.Value));
				}
			}
			BufferGPUMemoryMap.Empty();

			Status = CudaModule.DriverAPI()->cuStreamDestroy(reinterpret_cast<CUstream>(GPUStream));
			if (Status != CUDA_SUCCESS)
			{
				UE_LOG(LogRivermax, Warning, TEXT("Failed to destroy cuda stream. Status: %d"), Status);
			}
			GPUStream = nullptr;


			CudaModule.DriverAPI()->cuCtxPopCurrent(nullptr);
		}
	}

	void* FRivermaxInputStream::GetMappedBuffer(const FBufferRHIRef& InBuffer)
	{
		// If we are here, d3d12 had to have been validated
		const ERHIInterfaceType RHIType = RHIGetInterfaceType();
		check(RHIType == ERHIInterfaceType::D3D12);

		//Do we already have a mapped address for this buffer
		if (BufferGPUMemoryMap.Find((InBuffer)) == nullptr)
		{
			int64 BufferMemorySize = 0;
			CUexternalMemory MappedExternalMemory = nullptr;
			HANDLE D3D12BufferHandle = 0;
			CUDA_EXTERNAL_MEMORY_HANDLE_DESC CudaExtMemHandleDesc = {};

			// Create shared handle for our buffer
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(RmaxInput_D3D12CreateSharedHandle);

				ID3D12Resource* NativeD3D12Resource = GetID3D12DynamicRHI()->RHIGetResource(InBuffer);
				BufferMemorySize = GetID3D12DynamicRHI()->RHIGetResourceMemorySize(InBuffer);

				TRefCountPtr<ID3D12Device> OwnerDevice;
				HRESULT QueryResult;
				if ((QueryResult = NativeD3D12Resource->GetDevice(IID_PPV_ARGS(OwnerDevice.GetInitReference()))) != S_OK)
				{
					UE_LOG(LogRivermax, Error, TEXT("Failed to get D3D12 device for captured buffer ressource: %d)"), QueryResult);
					return nullptr;
				}

				if ((QueryResult = OwnerDevice->CreateSharedHandle(NativeD3D12Resource, NULL, GENERIC_ALL, NULL, &D3D12BufferHandle)) != S_OK)
				{
					UE_LOG(LogRivermax, Error, TEXT("Failed to create shared handle for captured buffer ressource: %d"), QueryResult);
					return nullptr;
				}

				CudaExtMemHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
				CudaExtMemHandleDesc.handle.win32.name = nullptr;
				CudaExtMemHandleDesc.handle.win32.handle = D3D12BufferHandle;
				CudaExtMemHandleDesc.size = BufferMemorySize;
				CudaExtMemHandleDesc.flags |= CUDA_EXTERNAL_MEMORY_DEDICATED;
			}

			FCUDAModule& CudaModule = FModuleManager::GetModuleChecked<FCUDAModule>("CUDA");

			CudaModule.DriverAPI()->cuCtxPushCurrent(CudaModule.GetCudaContext());

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Rmax_CudaImportMemory);

				const CUresult Result = FCUDAModule::CUDA().cuImportExternalMemory(&MappedExternalMemory, &CudaExtMemHandleDesc);

				if (D3D12BufferHandle)
				{
					CloseHandle(D3D12BufferHandle);
				}

				if (Result != CUDA_SUCCESS)
				{
					UE_LOG(LogRivermax, Error, TEXT("Failed to import shared buffer. Error: %d"), Result);
					return nullptr;
				}
			}

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Rmax_MapCudaMemory);

				CUDA_EXTERNAL_MEMORY_BUFFER_DESC BufferDescription = {};
				BufferDescription.offset = 0;
				BufferDescription.size = BufferMemorySize;
				CUdeviceptr NewMemory;
				const CUresult Result = FCUDAModule::CUDA().cuExternalMemoryGetMappedBuffer(&NewMemory, MappedExternalMemory, &BufferDescription);
				if (Result != CUDA_SUCCESS || NewMemory == 0)
				{
					UE_LOG(LogRivermax, Error, TEXT("Failed to get shared buffer mapped memory. Error: %d"), Result);
					CudaModule.DriverAPI()->cuCtxPushCurrent(nullptr);
					return nullptr;
				}

				BufferGPUMemoryMap.Add(InBuffer, reinterpret_cast<void*>(NewMemory));
			}

			CudaModule.DriverAPI()->cuCtxPushCurrent(nullptr);
		}

		// At this point, we have the mapped buffer in cuda space and we can use it to schedule a memcpy on cuda engine.
		return BufferGPUMemoryMap[InBuffer];
	}

	void FRivermaxInputStream::ProcessSRD(const FRTPHeader& RTPHeader, uint8* DataPtr)
	{
		uint32 DataOffset = 0;
		StreamData.ReceivedSize += RTPHeader.SRD1.Length;
		if (bIsUsingGPUDirect)
		{
			// Initial case (start address)
			if (StreamData.DeviceWritePointerOne == nullptr)
			{
				StreamData.DeviceWritePointerOne = DataPtr;
				StreamData.SizeToWriteOne = RTPHeader.SRD1.Length;
			}
			else
			{
				// Detection of wrap around -> Move tracking to second buffer
				if (StreamData.DeviceWritePointerTwo == nullptr && DataPtr < StreamData.DeviceWritePointerOne)
				{
					StreamData.DeviceWritePointerTwo = DataPtr;
					StreamData.SizeToWriteTwo = 0;
				}

				// Case where we track memory in first buffer
				if (StreamData.DeviceWritePointerTwo == nullptr)
				{
					StreamData.SizeToWriteOne += RTPHeader.SRD1.Length;
				}
				else // Tracking memory in second buffer
				{
					StreamData.SizeToWriteTwo += RTPHeader.SRD1.Length;
				}
			}
		}
		else
		{
			uint8* WriteBuffer = reinterpret_cast<uint8*>(StreamData.CurrentFrame);
			FMemory::Memcpy(&WriteBuffer[StreamData.WritingOffset], &DataPtr[DataOffset], RTPHeader.SRD1.Length);
			StreamData.WritingOffset += RTPHeader.SRD1.Length;

			if (RTPHeader.SRD1.bHasContinuation)
			{
				DataOffset += RTPHeader.SRD1.Length;
				FMemory::Memcpy(&WriteBuffer[StreamData.WritingOffset], &DataPtr[DataOffset], RTPHeader.SRD2.Length);
				StreamData.WritingOffset += RTPHeader.SRD2.Length;
				StreamData.ReceivedSize += RTPHeader.SRD2.Length;
			}
		}
	}

	void FRivermaxInputStream::ProcessLastSRD(const FRTPHeader& RTPHeader, uint8* DataPtr)
	{
		if (StreamData.ReceivedSize == StreamData.ExpectedSize)
		{
			const FString TraceName = FString::Format(TEXT("RmaxInput::ReceivedFrame {0}"), { RTPHeader.Timestamp });
			TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*TraceName);

			++StreamStats.FramesReceived;

			FRivermaxInputVideoFrameDescriptor Descriptor;
			Descriptor.Width = Options.Resolution.X;
			Descriptor.Height = Options.Resolution.Y;
			Descriptor.Stride = Options.AlignedResolution.X / FormatInfo.PixelGroupCoverage * FormatInfo.PixelGroupSize;
			Descriptor.bIsUsingGPUDirect = bIsUsingGPUDirect;
			FRivermaxInputVideoFrameReception NewFrame;

			if (bIsUsingGPUDirect)
			{
				if (LastSRDLength.IsSet() && LastSRDLength.GetValue() != RTPHeader.SRD1.Length)
				{
					UE_LOG(LogRivermax, Warning, TEXT("Unsupported variable SRD length detected while GPUDirect for input stream is used. Disable and reopen the stream. (Last: %d, New: %d)"), LastSRDLength.GetValue(), RTPHeader.SRD1.Length);
					Listener->OnStreamError();
					bIsShuttingDown = true;
				}
				LastSRDLength = RTPHeader.SRD1.Length;

				//Frame received entirely, time to copy it from rivermax gpu scratchpad to our own gpu memory
				FCUDAModule& CudaModule = FModuleManager::GetModuleChecked<FCUDAModule>("CUDA");
				CUresult Result = CudaModule.DriverAPI()->cuCtxPushCurrent(CudaModule.GetCudaContext());

				const CUdeviceptr DestinationGPUMemory = reinterpret_cast<CUdeviceptr>(StreamData.CurrentFrame);
				const CUdeviceptr SourceGPUMemoryOne = reinterpret_cast<CUdeviceptr>(StreamData.DeviceWritePointerOne);

				const uint32 NumSRDPartOne = RTPHeader.SRD1.Length > 0 ? StreamData.SizeToWriteOne / RTPHeader.SRD1.Length : 0;
				const uint32 NumSRDPartTwo = RTPHeader.SRD1.Length > 0 ? StreamData.SizeToWriteTwo / RTPHeader.SRD1.Length : 0;

				// Use cuda's 2d memcopy to do a source and destination stride difference memcopy
				// We initialize rivermax stream with a payload size blindly since we don't know what the sender will use
				// So, we use a big value by default and expect SRD to be smaller
				// This memcopy will consume the SRD size value but jump the init payload size value on the source address
				// Limitation is that this will only work for fixed SRD across a frame
				CUDA_MEMCPY2D StrideDescription;
				FMemory::Memset(StrideDescription, 0);
				StrideDescription.srcDevice = SourceGPUMemoryOne;
				StrideDescription.dstDevice = DestinationGPUMemory;
				StrideDescription.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;
				StrideDescription.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;
				StrideDescription.srcPitch = ExpectedPayloadSize; //Source pitch is the expected payload used at init
				StrideDescription.dstPitch = RTPHeader.SRD1.Length; //Destination pitch is the fixed SRD size we received
				StrideDescription.WidthInBytes = RTPHeader.SRD1.Length; //Width in bytes is the amount to copy, the SRD size
				StrideDescription.Height = NumSRDPartOne;
				Result = CudaModule.DriverAPI()->cuMemcpy2DAsync(&StrideDescription, reinterpret_cast<CUstream>(GPUStream));

				if (StreamData.DeviceWritePointerTwo != nullptr && StreamData.SizeToWriteTwo > 0)
				{
					StrideDescription.srcDevice = reinterpret_cast<CUdeviceptr>(StreamData.DeviceWritePointerTwo);
					StrideDescription.dstDevice = reinterpret_cast<CUdeviceptr>(StreamData.CurrentFrame) + StreamData.SizeToWriteOne;
					StrideDescription.Height = NumSRDPartTwo;
					Result = CudaModule.DriverAPI()->cuMemcpy2DAsync(&StrideDescription, reinterpret_cast<CUstream>(GPUStream));
				}

				if (Result != CUDA_SUCCESS)
				{
					UE_LOG(LogRivermax, Warning, TEXT("Failed to copy received buffer to shared memory. Error: %d"), Result);
					bIsFirstFrameReceived = false;
					return;
				}

				auto CudaCallback = [](void* userData)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FRivermaxInputStream::MemcopyCallback);
					if (userData)
					{
						// It might happen that our stream has been closed once the callback is triggered
						TWeakPtr<FCallbackPayload>* WeakPayloadPtr = reinterpret_cast<TWeakPtr<FCallbackPayload>*>(userData);
						TWeakPtr<FCallbackPayload> WeakPayload = *WeakPayloadPtr;
						if (TSharedPtr<FCallbackPayload> Payload = WeakPayload.Pin())
						{
							Payload->bIsWaitingForPendingCopy = false;
						}
					}
				};

				// Schedule a callback to know when to make the frame available
				CallbackPayload->bIsWaitingForPendingCopy = true;
				TWeakPtr<FCallbackPayload> WeakPayload = CallbackPayload;
				CudaModule.DriverAPI()->cuLaunchHostFunc(reinterpret_cast<CUstream>(GPUStream), CudaCallback, &WeakPayload);

				FCUDAModule::CUDA().cuCtxPopCurrent(nullptr);

				// For now, we wait for the cuda callback before we move on receiving next frame. 
				// Will need to update this and make the frame available from the cuda callback to avoid losing packets 
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FRivermaxInputStream:WaitingPendingOperation)
					const double CallbackTimestamp = FPlatformTime::Seconds();
					while (CallbackPayload->bIsWaitingForPendingCopy == true && bIsShuttingDown == false)
					{
						if (FPlatformTime::Seconds() - CallbackTimestamp > CVarWaitForCompletionTimeout.GetValueOnAnyThread())
						{
							Listener->OnStreamError();
							CallbackPayload->bIsWaitingForPendingCopy = false;
							break;
						}
					}
				}
			}

			NewFrame.VideoBuffer = reinterpret_cast<uint8*>(StreamData.CurrentFrame);

			// No need to provide the new frame and prepare the next one if we are shutting down
			if (bIsShuttingDown == false)
			{
				Listener->OnVideoFrameReceived(Descriptor, NewFrame);
				PrepareNextFrame();
			}
		}
		else
		{
			UE_LOG(LogRivermax, Warning, TEXT("End of frame received (Marker bit) but not enough data was received (missing %d). Expected %d but received (%d)"), StreamData.ExpectedSize - StreamData.ReceivedSize, StreamData.ExpectedSize, StreamData.ReceivedSize);
			StreamData.WritingOffset = 0;
			StreamData.ReceivedSize = 0;
			++StreamStats.InvalidFramesCount;
		}
	}
}

