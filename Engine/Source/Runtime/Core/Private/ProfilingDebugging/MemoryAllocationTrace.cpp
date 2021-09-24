// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProfilingDebugging/MemoryAllocationTrace.h"

#if UE_MEMORY_TRACE_ENABLED

////////////////////////////////////////////////////////////////////////////////
namespace UE {
namespace Trace {

TRACELOG_API void Update();

} // namespace Trace
} // namespace UE

////////////////////////////////////////////////////////////////////////////////
UE_TRACE_CHANNEL_DEFINE(MemAllocChannel)

UE_TRACE_EVENT_BEGIN(Memory, Init, NoSync|Important)
	UE_TRACE_EVENT_FIELD(uint32, MarkerPeriod)
	UE_TRACE_EVENT_FIELD(uint8, Version)
	UE_TRACE_EVENT_FIELD(uint8, MinAlignment)
	UE_TRACE_EVENT_FIELD(uint8, SizeShift)
	UE_TRACE_EVENT_FIELD(uint8, Mode)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, Marker)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, Alloc)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint32, CallstackId)
	UE_TRACE_EVENT_FIELD(uint32, Size)
	UE_TRACE_EVENT_FIELD(uint8, AlignmentPow2_SizeLower)
	UE_TRACE_EVENT_FIELD(uint8, RootHeap)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, AllocSystem)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint32, CallstackId)
	UE_TRACE_EVENT_FIELD(uint32, Size)
	UE_TRACE_EVENT_FIELD(uint8, AlignmentPow2_SizeLower)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, AllocVideo)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint32, CallstackId)
	UE_TRACE_EVENT_FIELD(uint32, Size)
	UE_TRACE_EVENT_FIELD(uint8, AlignmentPow2_SizeLower)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, Free)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint8, RootHeap)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, FreeSystem)
	UE_TRACE_EVENT_FIELD(uint64, Address)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, FreeVideo)
	UE_TRACE_EVENT_FIELD(uint64, Address)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, ReallocAlloc)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint32, CallstackId)
	UE_TRACE_EVENT_FIELD(uint32, Size)
	UE_TRACE_EVENT_FIELD(uint8, AlignmentPow2_SizeLower)
	UE_TRACE_EVENT_FIELD(uint8, RootHeap)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, ReallocAllocSystem)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint32, CallstackId)
	UE_TRACE_EVENT_FIELD(uint32, Size)
	UE_TRACE_EVENT_FIELD(uint8, AlignmentPow2_SizeLower)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, ReallocFree)
	UE_TRACE_EVENT_FIELD(uint64, Address_RootHeap)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, HeapSpec, NoSync|Important)
	UE_TRACE_EVENT_FIELD(HeapId, Id)
	UE_TRACE_EVENT_FIELD(HeapId, ParentId)
	UE_TRACE_EVENT_FIELD(uint16, Flags)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, Name)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, HeapMarkAlloc)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(uint16, Flags)
	UE_TRACE_EVENT_FIELD(HeapId, Heap)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Memory, HeapUnmarkAlloc)
	UE_TRACE_EVENT_FIELD(uint64, Address)
	UE_TRACE_EVENT_FIELD(HeapId, Heap)
UE_TRACE_EVENT_END()

// If layout of the above events are changed, bump this version number
constexpr uint8 MemoryTraceVersion = 1;

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::Initialize() const
{
	UE_TRACE_LOG(Memory, Init, MemAllocChannel)
		<< Init.MarkerPeriod(MarkerSamplePeriod + 1)
		<< Init.Version(MemoryTraceVersion)
		<< Init.MinAlignment(uint8(MIN_ALIGNMENT))
		<< Init.SizeShift(uint8(SizeShift));

	const HeapId SystemRootHeap = RootHeapSpec(TEXT("System memory"));
	check(SystemRootHeap == EMemoryTraceRootHeap::SystemMemory);
	const HeapId VideoRootHeap = RootHeapSpec(TEXT("VideoMemory"));
	check(VideoRootHeap == EMemoryTraceRootHeap::VideoMemory);

	static_assert((1 << SizeShift) - 1 <= MIN_ALIGNMENT, "Not enough bits to pack size fields");
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::EnableTracePump()
{
	bPumpTrace = true;
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::Update()
{
	const uint32 TheCount = MarkerCounter.fetch_add(1, std::memory_order_relaxed);
	if ((TheCount & MarkerSamplePeriod) == 0)
	{
		UE_TRACE_LOG(Memory, Marker, MemAllocChannel)
			<< Marker.Cycle(FPlatformTime::Cycles64());
	}

	if (bPumpTrace)
	{
		UE::Trace::Update();
	}
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::Alloc(void* Address, size_t Size, uint32 Alignment, uint32 Owner, HeapId RootHeap)
{
	check(RootHeap < 16);
	const uint32 AlignmentPow2 = uint32(FPlatformMath::CountTrailingZeros(Alignment));
	const uint32 Alignment_SizeLower = (AlignmentPow2 << SizeShift) | uint32(Size & ((1 << SizeShift) - 1));

	switch (RootHeap)
	{
		case EMemoryTraceRootHeap::SystemMemory:
			{
				UE_TRACE_LOG(Memory, AllocSystem, MemAllocChannel)
					<< AllocSystem.CallstackId(Owner)
					<< AllocSystem.Address(uint64(Address))
					<< AllocSystem.Size(uint32(Size >> SizeShift))
					<< AllocSystem.AlignmentPow2_SizeLower(uint8(Alignment_SizeLower));
				break;
			}
		case EMemoryTraceRootHeap::VideoMemory:
			{
				UE_TRACE_LOG(Memory, AllocVideo, MemAllocChannel)
					<< AllocVideo.CallstackId(Owner)
					<< AllocVideo.Address(uint64(Address))
					<< AllocVideo.Size(uint32(Size >> SizeShift))
					<< AllocVideo.AlignmentPow2_SizeLower(uint8(Alignment_SizeLower));
				break;
			}
		default:
			{
				UE_TRACE_LOG(Memory, Alloc, MemAllocChannel)
					<< Alloc.CallstackId(Owner)
					<< Alloc.Address(uint64(Address))
					<< Alloc.RootHeap(uint8(RootHeap))
					<< Alloc.Size(uint32(Size >> SizeShift))
					<< Alloc.AlignmentPow2_SizeLower(uint8(Alignment_SizeLower));
				break;
			}
	}

	Update();
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::Free(void* Address, HeapId RootHeap)
{
	check(RootHeap < 16);
	switch (RootHeap)
	{
		case EMemoryTraceRootHeap::SystemMemory:
			{
				UE_TRACE_LOG(Memory, FreeSystem, MemAllocChannel)
					<< FreeSystem.Address(uint64(Address));
			}
		case EMemoryTraceRootHeap::VideoMemory:
			{
				UE_TRACE_LOG(Memory, FreeVideo, MemAllocChannel)
					<< FreeVideo.Address(uint64(Address));
			}
		default:
			{
				UE_TRACE_LOG(Memory, Free, MemAllocChannel)
					<< Free.Address(uint64(Address))
					<< Free.RootHeap(uint8(RootHeap));
				break;
			}
	}
	
	Update();
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::ReallocAlloc(void* Address, size_t Size, uint32 Alignment, uint32 Owner, HeapId RootHeap)
{
	check(RootHeap < 16);
	const uint32 AlignmentPow2 = uint32(FPlatformMath::CountTrailingZeros(Alignment));
	const uint32 Alignment_SizeLower = (AlignmentPow2 << SizeShift) | uint32(Size & ((1 << SizeShift) - 1));
	
	switch (RootHeap)
	{
	case EMemoryTraceRootHeap::SystemMemory:
		{
			UE_TRACE_LOG(Memory, ReallocAllocSystem, MemAllocChannel)
				<< ReallocAllocSystem.CallstackId(Owner)
				<< ReallocAllocSystem.Address(uint64(Address))
				<< ReallocAllocSystem.Size(uint32(Size >> SizeShift))
				<< ReallocAllocSystem.AlignmentPow2_SizeLower(uint8(Alignment_SizeLower));
		}
	default:
		{
			UE_TRACE_LOG(Memory, ReallocAlloc, MemAllocChannel)
				<< ReallocAlloc.CallstackId(Owner)
				<< ReallocAlloc.Address(uint64(Address))
				<< ReallocAlloc.RootHeap(uint8(RootHeap))
				<< ReallocAlloc.Size(uint32(Size >> SizeShift))
				<< ReallocAlloc.AlignmentPow2_SizeLower(uint8(Alignment_SizeLower));
			break;
		}
	}

	Update();
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::ReallocFree(void* Address, HeapId RootHeap)
{
	check(RootHeap < 16);
	const uint64 Address_RootHeap = uint64(Address) | (uint64(RootHeap) << HeapShift);
	UE_TRACE_LOG(Memory, ReallocFree, MemAllocChannel)
		<< ReallocFree.Address_RootHeap(Address_RootHeap);

	Update();
}

////////////////////////////////////////////////////////////////////////////////
HeapId FAllocationTrace::HeapSpec(HeapId ParentId, const TCHAR* Name, EMemoryTraceHeapFlags Flags) const
{
	static std::atomic<HeapId> HeapIdCount(15); //Reserve indexes for root heaps
	const HeapId Id = HeapIdCount.fetch_add(1);
	const uint32 NameLen = FCString::Strlen(Name);
	const uint32 DataSize = NameLen * sizeof(TCHAR);
	check(ParentId < Id);

	UE_TRACE_LOG(Memory, HeapSpec, MemAllocChannel, DataSize)
		<< HeapSpec.Id(Id)
		<< HeapSpec.ParentId(ParentId)
		<< HeapSpec.Name(Name, NameLen)
		<< HeapSpec.Flags(uint16(Flags));

	return Id;
}

////////////////////////////////////////////////////////////////////////////////
HeapId FAllocationTrace::RootHeapSpec(const TCHAR* Name, EMemoryTraceHeapFlags Flags) const
{
	static std::atomic<HeapId> RootHeapCount(0);
	const HeapId Id = RootHeapCount.fetch_add(1);
	check(Id <= EMemoryTraceRootHeap::EndReserved);
	
	const uint32 NameLen = FCString::Strlen(Name);
	const uint32 DataSize = NameLen * sizeof(TCHAR);
	
	UE_TRACE_LOG(Memory, HeapSpec, MemAllocChannel, DataSize)
		<< HeapSpec.Id(Id)
		<< HeapSpec.ParentId(HeapId(~0))
		<< HeapSpec.Name(Name, NameLen)
		<< HeapSpec.Flags(uint16(EMemoryTraceHeapFlags::Root|Flags));

	return Id;
}

////////////////////////////////////////////////////////////////////////////////
void FAllocationTrace::MarkAllocAsHeap(void* Address, HeapId Heap, EMemoryTraceHeapAllocationFlags Flags)
{
	UE_TRACE_LOG(Memory, HeapMarkAlloc, MemAllocChannel)
		<< HeapMarkAlloc.Address(uint64(Address))
		<< HeapMarkAlloc.Heap(Heap)
		<< HeapMarkAlloc.Flags(uint16(EMemoryTraceHeapAllocationFlags::Heap | Flags));
	Update();
}

void FAllocationTrace::UnmarkAllocAsHeap(void* Address, HeapId Heap)
{
	// Sets all flags to zero
	UE_TRACE_LOG(Memory, HeapUnmarkAlloc, MemAllocChannel)
		<< HeapUnmarkAlloc.Address(uint64(Address))
		<< HeapUnmarkAlloc.Heap(Heap);
	Update();
}
#endif // UE_MEMORY_TRACE_ENABLED
