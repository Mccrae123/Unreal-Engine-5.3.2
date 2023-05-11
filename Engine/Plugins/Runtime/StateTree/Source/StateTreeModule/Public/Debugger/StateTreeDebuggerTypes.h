// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "StateTreeTraceTypes.h"

#if WITH_STATETREE_DEBUGGER

#include "StateTreeTypes.h"
#include "StateTree.h"
#include "TraceServices/Model/Frames.h"

class UStateTree;

namespace UE::StateTreeDebugger
{
/**
 * Struct indicating the index of the first event for a given trace recording frame.
 */
struct FFrameSpan
{
	FFrameSpan() = default;
	FFrameSpan(const TraceServices::FFrame& Frame, const uint32 EventIdx)
		: Frame(Frame)
		, EventIdx(EventIdx)
	{
	}

	/** Frame index in the analysis session */
	TraceServices::FFrame Frame;

	/** Index of the first event for that Frame index */
	int32 EventIdx = INDEX_NONE;
};


/**
 * Struct describing a state tree instance for a given StateTree asset
 */
struct STATETREEMODULE_API FInstanceDescriptor
{
	FInstanceDescriptor() = default;
	FInstanceDescriptor(const UStateTree* InStateTree, const FStateTreeInstanceDebugId InId, const FString& InName, const TRange<double> InLifetime);

	bool IsValid() const;

	bool operator==(const FInstanceDescriptor& Other) const
	{
		return StateTree == Other.StateTree && Id == Other.Id;
	}

	bool operator!=(const FInstanceDescriptor& Other) const
	{
		return !(*this == Other);
	}

	friend FString LexToString(const FInstanceDescriptor& InstanceDesc)
	{
		return FString::Printf(TEXT("%s | %s | %s"),
			*GetNameSafe(InstanceDesc.StateTree.Get()),
			*LexToString(InstanceDesc.Id),
			*InstanceDesc.Name);
	}

	friend uint32 GetTypeHash(const FInstanceDescriptor& Desc)
	{
		return GetTypeHash(Desc.Id);
	}

	TRange<double> Lifetime = TRange<double>(0);
	TWeakObjectPtr<const UStateTree> StateTree = nullptr;
	FString Name;
	FStateTreeInstanceDebugId Id = FStateTreeInstanceDebugId::Invalid;
};


/**
 * Struct holding organized events associated to a given state tree instance.
 */
struct STATETREEMODULE_API FInstanceEventCollection
{
	FInstanceEventCollection() = default;
	explicit FInstanceEventCollection(const FStateTreeInstanceDebugId& InstanceId)
		: InstanceId(InstanceId)
	{
	}

	friend bool operator==(const FInstanceEventCollection& Lhs, const FInstanceEventCollection& RHS)
	{
		return Lhs.InstanceId == RHS.InstanceId;
	}

	friend bool operator!=(const FInstanceEventCollection& Lhs, const FInstanceEventCollection& RHS)
	{
		return !(Lhs == RHS);
	}

	bool IsValid() const { return InstanceId.IsValid(); }
	bool IsInvalid() const { return !IsValid(); }

	/** Id of the instance associated to the stored events. */
	FStateTreeInstanceDebugId InstanceId;

	/** All events received for this instance. */
	TArray<FStateTreeTraceEventVariantType> Events;

	/** Spans for frames with events. Each span contains the frame information and the index of the first event for that frame. */
	TArray<FFrameSpan> FrameSpans;

	/** Indices of span and event for frames with a change of activate states. */
	TArray<TTuple<uint32, uint32>> ActiveStatesChanges;

	/**
	 * Returns the event collection associated to the currently selected instance.
	 * An invalid empty collection is returned if there is no selected instance. (IsValid needs to be called).
	 * @return Event collection associated to the selected instance or an invalid one if not found.
	 */
	static const FInstanceEventCollection Invalid;
};

struct STATETREEMODULE_API FScrubState
{
	explicit FScrubState(const TArray<FInstanceEventCollection>& EventCollections)
		: EventCollections(EventCollections)
	{
	}

private:
	const TArray<FInstanceEventCollection>& EventCollections;
	
public:
	double ScrubTime = 0;
	uint32 EventCollectionIndex = INDEX_NONE;
	uint64 TraceFrameIndex = INDEX_NONE;
	uint32 FrameSpanIndex = INDEX_NONE;
	uint32 PreviousFrameSpanIndex = INDEX_NONE;
	uint32 ActiveStatesIndex = INDEX_NONE;
	

	void SetScrubTime(double NewScrubTime);

	/**
	 * Indicates if the current scrub state points to a valid frame.
	 * @return True if the frame index is set
	 */
	bool IsInBounds() const { return FrameSpanIndex != INDEX_NONE; }

	/**
	 * Indicates if the current scrub state points to an active states entry in the event collection.
	 * @return True if the collection and active states indices are set
	 */
	bool IsPointingToValidActiveStates() const { return EventCollectionIndex != INDEX_NONE && ActiveStatesIndex != INDEX_NONE; }

	/** Indicates if there is a frame before with events. */
	bool HasPreviousFrame() const;
	
	/**
	 * Set scrubbing info using the previous frame with events.
	 * HasPreviousFrame must be used to validate that this method can be called otherwise some checks might fail.
	 * @return Adjusted scrub time
	 */
	double GotoPreviousFrame();
	
	/** Indicates if there is a frame after with events. */
	bool HasNextFrame() const;
	
	/**
	 * Set scrubbing info using the next frame with events.
	 * HasPreviousFrame must be used to validate that this method can be called otherwise some checks might fail.
	 * @return Adjusted scrub time
	 */
	double GotoNextFrame();
	
	/** Indicates if there is a frame before where the StateTree has a different list of active states. */
	bool HasPreviousActiveStates() const;
	
	/**
	 * Set scrubbing info using the previous frame where the StateTree has a different list of active states.
	 * HasPreviousActiveStates must be used to validate that this method can be called otherwise some checks might fail.
	 * @return Adjusted scrub time
	 */
	double GotoPreviousActiveStates();
	
	/** Indicates if there is a frame after where the StateTree has a different list of active states. */
	bool HasNextActiveStates() const;
	
	/**
	 * Set scrubbing info using the next frame where the StateTree has a different list of active states.
	 * HasNextActiveStates must be used to validate that this method can be called otherwise some checks might fail.
	 * @return Adjusted scrub time
	 */
	double GotoNextActiveStates();

	/**
	 * Returns the event collection associated to the selected instance.
	 * An invalid empty collection is returned if there is no selected instance (IsValid needs to be called).
	 * @return Event collection associated to the selected instance or an invalid one if not found.
	 */
	const FInstanceEventCollection& GetEventCollection() const;

private:
	void SetFrameSpanIndex(uint32 NewFrameSpanIndex);
	void SetActiveStatesIndex(uint32 NewActiveStatesIndex);
	void UpdateActiveStatesIndex(uint32 SpanIndex);
};

} // UE::StateTreeDebugger

#endif // WITH_STATETREE_DEBUGGER
