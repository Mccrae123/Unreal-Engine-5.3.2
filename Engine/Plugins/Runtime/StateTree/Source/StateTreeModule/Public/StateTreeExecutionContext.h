// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "StateTree.h"
#include "InstancedStruct.h"
#include "Containers/StaticArray.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeExecutionContext.generated.h"


USTRUCT()
struct STATETREEMODULE_API FStateTreeExecutionState
{
	GENERATED_BODY()

	/** Currently active state */
	FStateTreeHandle CurrentState = FStateTreeHandle::Invalid;

	/** Result of last tick */
	EStateTreeRunStatus LastTickStatus = EStateTreeRunStatus::Failed;

	/** Running status of the instance */
	EStateTreeRunStatus TreeRunStatus = EStateTreeRunStatus::Unset;

	/** Delayed transition handle, if exists */
	int16 GatedTransitionIndex = INDEX_NONE;

	/** Running time of the delayed transition */
	float GatedTransitionTime = 0.0f;
};

UENUM()
enum class EStateTreeStorage : uint8
{
	/** Execution context has internal storage */
	Internal,
	/** Execution context assumes external storage */
	External,
};

/**
 * Runs StateTrees defined in UStateTree asset.
 * Uses constant data from StateTree, keeps local storage of variables, and creates instanced Evaluators and Tasks.
 */
USTRUCT()
struct STATETREEMODULE_API FStateTreeExecutionContext
{
	GENERATED_BODY()

public:
	FStateTreeExecutionContext();
	virtual ~FStateTreeExecutionContext();

	/** Initializes the StateTree instance to be used with specific owner and StateTree asset. */
	bool Init(UObject& InOwner, const UStateTree& InStateTree, const EStateTreeStorage InStorageType);

	/** Returns the StateTree asset in use. */
	const UStateTree* GetStateTree() const { return StateTree; }

	UObject* GetOwner() const { return Owner; }
	UWorld* GetWorld() const { return World; };
	void SetWorld(UWorld* InWorld) { World = InWorld; };

	/** Start executing. */
	void Start(FStateTreeItemView ExternalStorage = FStateTreeItemView());
	/** Stop executing. */
	void Stop(FStateTreeItemView ExternalStorage = FStateTreeItemView());

	/** Tick the state tree logic. */
	EStateTreeRunStatus Tick(const float DeltaTime, FStateTreeItemView ExternalStorage = FStateTreeItemView());

	/** @return Pointer to a State or null if state not found */ 
	const FBakedStateTreeState* GetStateFromHandle(const FStateTreeHandle StateHandle) const
	{
		return (StateTree && StateTree->States.IsValidIndex(StateHandle.Index)) ? &StateTree->States[StateHandle.Index] : nullptr;
	}

	/** @return Array view to external item descriptors associated with this context. Note: Init() must be called before calling this method. */
	TConstArrayView<FStateTreeExternalItemDesc> GetExternalItems() const
	{
		check(StateTree);
		return StateTree->ExternalItems;
	}

	/** @return True if all required external item pointers are set. */ 
	bool AreExternalItemsValid() const
	{
		check(StateTree);
		bool bResult = true;
		for (const FStateTreeExternalItemDesc& ItemDesc : StateTree->ExternalItems)
		{
			const FStateTreeItemView& ItemView = ItemViews[ItemDesc.Handle.GetIndex()];
			if (!ItemDesc.bOptional && (ItemView.GetMemory() == nullptr || ItemView.GetStruct() != ItemDesc.Struct))
			{
				bResult = false;
				break;
			}
		}
		return bResult;
	}

	/** @return Handle to external item of type InStruct, or invalid handle if not found */ 
	FStateTreeExternalItemHandle GetExternalItemHandleByStruct(const UStruct* InStruct) const
	{
		check(StateTree);
		const FStateTreeExternalItemDesc* Item = StateTree->ExternalItems.FindByPredicate([InStruct](const FStateTreeExternalItemDesc& Item) { return Item.Struct == InStruct; });
		return Item != nullptr ? Item->Handle : FStateTreeExternalItemHandle();
	}

	/** Sets external item view based on handle */ 
	void SetExternalItem(const FStateTreeExternalItemHandle ItemHandle, FStateTreeItemView Item)
	{
		check(StateTree);
		check(ItemHandle.IsValid());
		ItemViews[ItemHandle.GetIndex()] = Item;
	}

	/** @return item view to external item based on handle */ 
	FStateTreeItemView GetExternalItem(const FStateTreeExternalItemHandle ItemHandle) const
	{
		check(StateTree);
		check(ItemHandle.IsValid());
		return ItemViews[ItemHandle.GetIndex()];
	}

	EStateTreeRunStatus GetLastTickStatus(FStateTreeItemView ExternalStorage = FStateTreeItemView()) const;
	EStateTreeRunStatus GetEnterStateStatus() const { return EnterStateStatus; }

#if WITH_GAMEPLAY_DEBUGGER
	/** @return Debug string describing the current state of the execution */
	FString GetDebugInfoString(FStateTreeItemView ExternalStorage = FStateTreeItemView()) const;
#endif // WITH_GAMEPLAY_DEBUGGER

#if WITH_STATETREE_DEBUG
	FString GetActiveStateName(FStateTreeItemView ExternalStorage = FStateTreeItemView()) const;
	
	void DebugPrintInternalLayout(FStateTreeItemView ExternalStorage);
#endif
	
protected:

	/** @return Prefix that will be used by STATETREE_LOG and STATETREE_CLOG, empty by default. */
	virtual FString GetInstanceDescription() const { return TEXT(""); }

	/** Callback when gated transition is triggered. Contexts that are event based can use this to trigger a future event. */
	virtual void BeginGatedTransition(const FStateTreeExecutionState& Exec) {};
	
	/**
	 * Resets the instance to initial empty state. Note: Does not call ExitState().
	 */
	void Reset();

	/**
	 * Handles logic for entering State. EnterState is called on new active Evaluators and Tasks that are part of the re-planned tree.
	 * Re-planned tree is from the transition target up to the leaf state. States that are parent to the transition target state
	 * and still active after the transition will remain intact.
	 * @return Run status returned by the tasks.
	 */
	EStateTreeRunStatus EnterState(FStateTreeItemView Storage, const FStateTreeTransitionResult& Transition);

	/**
	 * Handles logic for exiting State. ExitState is called on current active Evaluators and Tasks that are part of the re-planned tree.
	 * Re-planned tree is from the transition target up to the leaf state. States that are parent to the transition target state
	 * and still active after the transition will remain intact.
	 */
	void ExitState(FStateTreeItemView Storage, const FStateTreeTransitionResult& Transition);

	/**
	 * Handles logic for exiting State. ExitState is called on current active Evaluators and Tasks in reverse order (from leaf to root).
	 */
	void StateCompleted(FStateTreeItemView Storage, const FStateTreeHandle CurrentState, const EStateTreeRunStatus CompletionStatus);

	/**
	 * Ticks evaluators of all active states starting from current state by delta time.
	 * If TickEvaluators() is called multiple times per frame (i.e. during selection when visiting new states), each state and evaluator is ticked only once.
	 */
	void TickEvaluators(FStateTreeItemView Storage, const FStateTreeHandle CurrentState, const EStateTreeEvaluationType EvalType, const float DeltaTime);

	/**
	 * Ticks tasks of all active states starting from current state by delta time.
	 * @return Run status returned by the tasks.
	 */
	EStateTreeRunStatus TickTasks(FStateTreeItemView Storage, const FStateTreeHandle CurrentState, const float DeltaTime);

	/**
	 * Checks all conditions at given range
	 * @return True if all conditions pass.
	 */
	bool TestAllConditions(const uint32 ConditionsOffset, const uint32 ConditionsNum);

	/**
	 * Triggers transitions based on current run status. CurrentStatus is used to select which transitions events are triggered.
	 * If CurrentStatus is "Running", "Conditional" transitions pass, "Completed/Failed" will trigger "OnCompleted/OnSucceeded/OnFailed" transitions.
	 * Transition target state can point to a selector state. For that reason the result contains both the target state, as well ass
	 * the actual next state returned by the selector.
	 * @return Transition result describing the source state, state transitioned to, and next selected state.
	 */
	FStateTreeTransitionResult TriggerTransitions(FStateTreeItemView Storage, const FStateTreeStateStatus CurrentStatus, const int Depth);

	/**
	 * Runs state selection logic starting at the specified state, walking towards the leaf states.
	 * If the preconditions of NextState are not met, "Invalid" is returned. 
	 * If NextState is a selector state, SelectState is called recursively (depth-first) to all child states (where NextState will be one of child states).
	 * If NextState is a leaf state, the NextState is returned.
	 * @param Storage View representing all runtime data used by tasks and evaluators
	 * @param InitialStateStatus Describes the current state and running status (will be passed intact to next selector)
	 * @param InitialTargetState The state the initial transition target state (will be passed intact to next selector) 
	 * @param NextState The state which we try to select next.
	 * @param Depth Depth of recursion.
	 * @return Transition result describing the source state, transition target state, and next selected state.
	 */
	FStateTreeTransitionResult SelectState(FStateTreeItemView Storage, const FStateTreeStateStatus InitialStateStatus, const FStateTreeHandle InitialTargetState, const FStateTreeHandle NextState, const int Depth);

	/** @return State handles from specified state handle back to the root, specified handle included. */
	int32 GetActiveStates(const FStateTreeHandle StateHandle, TStaticArray<FStateTreeHandle, 32>& OutStateHandles) const;

	/** @return Mutable storage based on storage settings. */
	FStateTreeItemView SelectMutableStorage(FStateTreeItemView ExternalStorage)
	{
		return StorageType == EStateTreeStorage::External ? ExternalStorage : FStateTreeItemView(StorageInstance);
	}

	/** @return Const storage based on storage settings. */
	const FStateTreeItemView SelectStorage(FStateTreeItemView ExternalStorage) const
	{
		return StorageType == EStateTreeStorage::External ? ExternalStorage : FStateTreeItemView(StorageInstance);
	}

	/** @return View to an Evaluator or a Task. */
	FStateTreeItemView GetItem(FStateTreeItemView Storage, const int32 Index) const
	{
		const FStateTreeRuntimeStorageItemOffset& ItemOffset = StateTree->RuntimeStorageOffsets[Index];
		return FStateTreeItemView(ItemOffset.Struct, Storage.GetMutableMemory() + ItemOffset.Offset);
	}

	/** @return StateTree execution state from the runtime storage. */
	FStateTreeExecutionState& GetExecState(FStateTreeItemView Storage) const
	{
		const FStateTreeRuntimeStorageItemOffset& ItemOffset = StateTree->RuntimeStorageOffsets[0];
		check(ItemOffset.Struct == FStateTreeExecutionState::StaticStruct());
		return *reinterpret_cast<FStateTreeExecutionState*>(Storage.GetMutableMemory() + ItemOffset.Offset);
	}

	/** @return String describing state status for logging and debug. */
	FString GetStateStatusString(const FStateTreeStateStatus StateStatus) const;

	/** @return String describing state name for logging and debug. */
	FString GetSafeStateName(const FStateTreeHandle State) const;

	/** @return String describing full path of an activate state for logging and debug. */
	FString DebugGetStatePath(TArrayView<FStateTreeHandle> ActiveStateHandles, int32 ActiveStateIndex) const;

	/** The StateTree asset the context is initialized for */
	UPROPERTY()
	const UStateTree* StateTree = nullptr;

	UPROPERTY()
	UObject* Owner = nullptr;
	
	UPROPERTY()
	UWorld* World = nullptr;

	/** States visited during a tick while updating evaluators. Initialized to match the number of states in the asset. */ 
	TArray<bool> VisitedStates;

	/** Array of item pointers (external items, tasks, evaluators), used during evaluation. Initialized to match the number of items in the asset. */
	TArray<FStateTreeItemView> ItemViews;

	/** Optional Instance of the storage */
	FInstancedStruct StorageInstance;

	/** Storage type of the context */
	EStateTreeStorage StorageType = EStateTreeStorage::Internal;

	/**
	 * Temporary status held within the context when calling 'EnterState' on multiple tasks.
	 * Since it is called on all tasks even if a failed status was returned, this allow other tasks to act accordingly.
	 * Note. This should be replaced by symmetrical unrolling of tasks on failure.
	 */
	EStateTreeRunStatus EnterStateStatus = EStateTreeRunStatus::Unset;
};
