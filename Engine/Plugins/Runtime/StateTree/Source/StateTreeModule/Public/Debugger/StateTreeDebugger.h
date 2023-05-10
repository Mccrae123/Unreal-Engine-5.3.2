// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_STATETREE_DEBUGGER

#include "IStateTreeTraceProvider.h"
#include "StateTree.h"
#include "StateTreeDebuggerTypes.h"
#include "StateTreeTypes.h"
#include "Tickable.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Diagnostics.h"
#include "TraceServices/Model/Frames.h"

namespace UE::Trace
{
	class FStoreClient;
}

class IStateTreeModule;
class UStateTreeState;
class UStateTree;

DECLARE_DELEGATE_OneParam(FOnStateTreeDebuggerScrubStateChanged, const UE::StateTreeDebugger::FScrubState& ScrubState);
DECLARE_DELEGATE_TwoParams(FOnStateTreeDebuggerBreakpointHit, FStateTreeInstanceDebugId InstanceId, FStateTreeStateHandle StateHandle);
DECLARE_DELEGATE_OneParam(FOnStateTreeDebuggerBreakpointsChanged, TConstArrayView<FStateTreeStateHandle> Breakpoints);
DECLARE_DELEGATE_OneParam(FOnStateTreeDebuggerActiveStatesChanges, TConstArrayView<FStateTreeStateHandle> ActiveStates);
DECLARE_DELEGATE_OneParam(FOnStateTreeDebuggerNewInstance, FStateTreeInstanceDebugId InstanceId);
DECLARE_DELEGATE(FOnStateTreeDebuggerDebuggedInstanceSet);

struct STATETREEMODULE_API FStateTreeDebugger : FTickableGameObject
{
	struct FTraceDescriptor
	{
		FTraceDescriptor() = default;
		FTraceDescriptor(const FString& Name, const uint32 Id) : Name(Name), TraceId(Id) {}
		
		bool operator==(const FTraceDescriptor& Other) const { return Other.TraceId == TraceId; }
		bool operator!=(const FTraceDescriptor& Other) const { return !(Other == *this); }
		bool IsValid() const { return TraceId != INDEX_NONE; }

		FString Name;
		uint32 TraceId = INDEX_NONE;

		TraceServices::FSessionInfo SessionInfo;
	};
	
	FStateTreeDebugger();
	virtual ~FStateTreeDebugger() override;

	const UStateTree* GetAsset() const { return StateTreeAsset.Get(); }
	void SetAsset(const UStateTree* InStateTreeAsset) { StateTreeAsset = InStateTreeAsset; }
	
	/** Stops reading traces every frame to preserve current state. */
	void Pause();

	/** Indicates that the debugger was explicitly paused and is no longer fetching new events from the analysis session. */
	bool IsPaused() const { return bPaused; }

	/** Resumes reading traces every frame. */
	void Unpause();

	/** Forces a single refresh to latest state. Useful when simulation is paused. */
	void SyncToCurrentSessionDuration();
	
	bool CanStepBackToPreviousStateWithEvents() const;
	void StepBackToPreviousStateWithEvents();

	bool CanStepForwardToNextStateWithEvents() const;
	void StepForwardToNextStateWithEvents();

	bool CanStepBackToPreviousStateChange() const;
	void StepBackToPreviousStateChange();

	bool CanStepForwardToNextStateChange() const;
	void StepForwardToNextStateChange();

	bool IsAnalysisSessionActive() const { return GetAnalysisSession() != nullptr; }
	const TraceServices::IAnalysisSession* GetAnalysisSession() const;

	bool IsActiveInstance(double Time, FStateTreeInstanceDebugId InstanceId) const;
	FText GetInstanceDescription(FStateTreeInstanceDebugId InstanceId) const;
	void SelectInstance(const FStateTreeInstanceDebugId InstanceId);

	static FText DescribeTrace(const FTraceDescriptor& TraceDescriptor);
	static FText DescribeInstance(const UE::StateTreeDebugger::FInstanceDescriptor& StateTreeInstanceDesc);

	/**
	 * Finds and returns the event collection associated to a given instance Id.
	 * An invalid empty collection is returned if not found (IsValid needs to be called).
	 * @param InstanceId Id of the instance for which the event collection is returned. 
	 * @return Event collection associated to the provided Id or an invalid one if not found.
	 */
	const UE::StateTreeDebugger::FInstanceEventCollection& GetEventCollection(FStateTreeInstanceDebugId InstanceId) const;
	
	double GetRecordingDuration() const { return RecordingDuration; }
	double GetScrubTime() const	{ return ScrubState.ScrubTime; }
	void SetScrubTime(double ScrubTime);

	void GetLiveTraces(TArray<FTraceDescriptor>& OutTraceDescriptors) const;
	void StartLastLiveSessionAnalysis();
	void StartSessionAnalysis(const FTraceDescriptor& TraceDescriptor);
	FTraceDescriptor GetSelectedTraceDescriptor() const { return ActiveSessionTraceDescriptor; }
	FText GetSelectedTraceDescription() const;
	
	void ToggleBreakpoints(const TConstArrayView<FStateTreeStateHandle> SelectedStates);

	FOnStateTreeDebuggerNewInstance OnNewInstance;
	FOnStateTreeDebuggerDebuggedInstanceSet OnSelectedInstanceCleared;
	FOnStateTreeDebuggerScrubStateChanged OnScrubStateChanged;
	FOnStateTreeDebuggerBreakpointHit OnBreakpointHit;
	FOnStateTreeDebuggerBreakpointsChanged OnBreakpointsChanged;
	FOnStateTreeDebuggerActiveStatesChanges OnActiveStatesChanged;

protected:
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return true; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FStateTreeDebugger, STATGROUP_Tickables); }
	
private:
	void StopAnalysis();

	void ReadTrace(double ScrubTime);
	void ReadTrace(uint64 FrameIndex);
	void ReadTrace(
		const TraceServices::IAnalysisSession& Session,
		const TraceServices::IFrameProvider& FrameProvider,
		const TraceServices::FFrame& Frame
		);

	void SendNotifications();

	void SetActiveStates(const TConstArrayView<FStateTreeStateHandle> NewActiveStates);

	/**
	 * Recompute index of the span that contains the active states change event and update the active states.
	 * This method handles unselected instances in which case it will reset the active states and set the span index to INDEX_NONE
	 * */
	void RefreshActiveStates();

	UE::Trace::FStoreClient* GetStoreClient() const;
	void GetSessionInstances(TArray<UE::StateTreeDebugger::FInstanceDescriptor>& OutInstances) const;
	void UpdateInstances();

	bool ProcessEvent(const FStateTreeInstanceDebugId InstanceId, const TraceServices::FFrame& Frame, const FStateTreeTraceEventVariantType& Event);
	void AddEvents(double StartTime, double EndTime, const TraceServices::IFrameProvider& FrameProvider, const IStateTreeTraceProvider& StateTreeTraceProvider);
	void UpdateMetadata(FTraceDescriptor& TraceDescriptor) const;

	/** Module used to access the store client and analysis sessions .*/
	IStateTreeModule& StateTreeModule;

	/** The StateTree asset associated to this debugger. All instances will be using this asset. */ 
	TWeakObjectPtr<const UStateTree> StateTreeAsset;
	
	/** The trace analysis session. */
	TSharedPtr<const TraceServices::IAnalysisSession> AnalysisSession;

	/** Descriptor of the currently selected session */
	FTraceDescriptor ActiveSessionTraceDescriptor;

	/** Descriptors for all instances of the StateTree asset that have traces in the analysis session and still active. */
	TArray<UE::StateTreeDebugger::FInstanceDescriptor> InstanceDescs;

	/** Processed events for each instance. */
	TArray<UE::StateTreeDebugger::FInstanceEventCollection> EventCollections;

	/** Specific instance selected for more details */
	FStateTreeInstanceDebugId SelectedInstanceId;

	/** Handles of states on which a breakpoint has been set. This is per asset and not specific to an instance. */
	TArray<FStateTreeStateHandle> StatesWithBreakpoint;

	/** List of currently active states in the selected instance */
	TArray<FStateTreeStateHandle> ActiveStates;

	inline static constexpr double UnsetTime = -1;

	/** Recording duration of the analysis session. This is not related to the world simulation time. */
	double RecordingDuration = UnsetTime;

	/** Last time in the recording that we use to fetch events and we will use for the next read. */
	double LastTraceReadTime = UnsetTime;

	/** Combined information regarding current scrub time (e.g. frame index, event collection index, etc.) */ 
	UE::StateTreeDebugger::FScrubState ScrubState;

	/** Indicates the instance for which a breakpoint has been hit */
	FStateTreeInstanceDebugId HitBreakpointInstanceId = FStateTreeInstanceDebugId::Invalid;

	/** Indicates the state for which a breakpoint has been hit */
	int32 HitBreakpointStateIndex = INDEX_NONE;

	/** List of new instances discovered by processing event in the analysis session. */
	TArray<FStateTreeInstanceDebugId> NewInstances;

	/** Indicates that the debugger was explicitly paused and will wait before fetching new events from the analysis session provider. */
	bool bPaused = false;
};

#endif // WITH_STATETREE_DEBUGGER
