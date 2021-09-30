// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassStateTreeProcessors.h"
#include "StateTree.h"
#include "MassStateTreeExecutionContext.h"
#include "EntityView.h"
#include "MassAIMovementTypes.h"
#include "MassComponentHitTypes.h"
#include "MassSmartObjectTypes.h"
#include "MassZoneGraphAnnotationTypes.h"
#include "MassStateTreeSubsystem.h"
#include "MassSignals/Public/MassSignalSubsystem.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Engine/World.h"

CSV_DEFINE_CATEGORY(StateTreeProcessor, true);

namespace UE::MassBehavior
{

bool SetExternalFragments(FMassStateTreeExecutionContext& Context, const UMassEntitySubsystem& EntitySubsystem)
{
	bool bFoundAllFragments = true;
	const FMassEntityView EntityView(EntitySubsystem, Context.GetEntity());
	for (const FStateTreeExternalItemDesc& ItemDesc : Context.GetExternalItems())
	{
		if (ItemDesc.Struct && ItemDesc.Struct->IsChildOf(FMassFragment::StaticStruct()))
		{
			const UScriptStruct* ScriptStruct = Cast<const UScriptStruct>(ItemDesc.Struct);
			FStructView Fragment = EntityView.GetComponentDataStruct(ScriptStruct);
			if (Fragment.IsValid())
			{
				Context.SetExternalItem(ItemDesc.Handle, FStateTreeItemView(Fragment));
			}
			else
			{
				if (!ItemDesc.bOptional)
				{
					// Note: Not breaking here, so that we can validate all missing ones in one go with FMassStateTreeExecutionContext::AreExternalItemsValid().
					bFoundAllFragments = false;
				}
			}
		}
	}
	return bFoundAllFragments;
}

bool SetExternalSubsystems(FMassStateTreeExecutionContext& Context)
{
	const UWorld* World = Context.GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	bool bFoundAllSubsystems = true;
	for (const FStateTreeExternalItemDesc& ItemDesc : Context.GetExternalItems())
	{
		if (ItemDesc.Struct && ItemDesc.Struct->IsChildOf(UWorldSubsystem::StaticClass()))
		{
			const TSubclassOf<UWorldSubsystem> SubClass = Cast<UClass>(const_cast<UStruct*>(ItemDesc.Struct));
			UWorldSubsystem* Subsystem = World->GetSubsystemBase(SubClass);
			if (Subsystem)
			{
				Context.SetExternalItem(ItemDesc.Handle, FStateTreeItemView(Subsystem));
			}
			else
			{
				if (!ItemDesc.bOptional)
				{
					// Note: Not breaking here, so that we can validate all missing ones in one go with FMassStateTreeExecutionContext::AreExternalItemsValid().
					bFoundAllSubsystems = false;
				}
			}
		}
	}
	return bFoundAllSubsystems;
}

void ProcessChunk(
	FMassStateTreeExecutionContext& StateTreeContext,
	UMassStateTreeSubsystem& MassStateTreeSubsystem,
	const TFunctionRef<void(FMassStateTreeExecutionContext&, FStateTreeItemView)> ForEachEntityCallback)
{
	const FMassExecutionContext& Context = StateTreeContext.GetEntitySubsystemExecutionContext();
	const TConstArrayView<FMassStateTreeFragment> StateTreeList = Context.GetComponentView<FMassStateTreeFragment>();

	// Assuming that all the entities share same StateTree, because they all should have the same storage fragment.
	const int32 NumEntities = Context.GetEntitiesNum();
	check(NumEntities > 0);
	UStateTree* StateTree = MassStateTreeSubsystem.GetRegisteredStateTreeAsset(StateTreeList[0].StateTreeHandle);

	// Initialize the execution context if changed between chunks.
	if (StateTreeContext.GetStateTree() != StateTree)
	{
		StateTreeContext.Init(MassStateTreeSubsystem, *StateTree, EStateTreeStorage::External);

		// Gather subsystems.
		{
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(StateTreeProcessorExternalSubsystems);
			if (!ensureMsgf(UE::MassBehavior::SetExternalSubsystems(StateTreeContext), TEXT("StateTree will not execute due to missing subsystem requirements.")))
			{
				return;
			}
		}
	}

	const UScriptStruct* StorageScriptStruct = StateTree->GetRuntimeStorageStruct();
	for (int32 i = 0; i < NumEntities; ++i)
	{
		const FMassEntityHandle Entity = Context.GetEntity(i);
		StateTreeContext.SetEntity(Entity);
		StateTreeContext.SetEntityIndex(i);

		// Gather all required fragments.
		{
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(StateTreeProcessorExternalFragments);
			if (!ensureMsgf(UE::MassBehavior::SetExternalFragments(StateTreeContext, StateTreeContext.GetEntitySubsystem()), TEXT("StateTree will not execute due to missing required fragments.")))
			{
				break;
			}
		}

		// Make sure all required external items are set.
		{
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(StateTreeProcessorExternalItemsValidation);
			// TODO: disable this when not in debug.
			if (!ensureMsgf(StateTreeContext.AreExternalItemsValid(), TEXT("StateTree will not execute due to missing external items.")))
			{
				break;
			}
		}

		ForEachEntityCallback(StateTreeContext, StateTreeContext.GetEntitySubsystem().GetComponentDataStruct(Entity, StorageScriptStruct));
	}
}

} // UE::MassBehavior


//----------------------------------------------------------------------//
// UMassStateTreeFragmentInitializer
//----------------------------------------------------------------------//
UMassStateTreeFragmentInitializer::UMassStateTreeFragmentInitializer()
{
	ExecutionFlags = (int32)(EProcessorExecutionFlags::Standalone | EProcessorExecutionFlags::Server);
	FragmentType = FMassStateTreeFragment::StaticStruct();
}

void UMassStateTreeFragmentInitializer::ConfigureQueries()
{
	EntityQuery.AddRequirement<FMassStateTreeFragment>(EMassFragmentAccess::ReadOnly);
}

void UMassStateTreeFragmentInitializer::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	// StateTree processor relies on signals to be ticked but we need an 'initial tick' to set the tree in the proper state.
	// The initializer provides that by sending a signal to all new entities that use StateTree.
	TArray<FMassEntityHandle> EntitiesToSignal;

	FMassStateTreeExecutionContext StateTreeContext(EntitySubsystem, Context);
	UMassStateTreeSubsystem* MassStateTreeSubsystem = UWorld::GetSubsystem<UMassStateTreeSubsystem>(EntitySubsystem.GetWorld());

	EntityQuery.ForEachEntityChunk(
		EntitySubsystem,
		Context,
		[this, &StateTreeContext, &MassStateTreeSubsystem, &EntitiesToSignal](const FMassExecutionContext& Context)
		{
			UE::MassBehavior::ProcessChunk(
				StateTreeContext,
				*MassStateTreeSubsystem,
				[](FMassStateTreeExecutionContext& StateTreeExecutionContext, FStateTreeItemView Storage)
				{
					// Start the tree instance
					StateTreeExecutionContext.Start(Storage);
				});

			// Append all entities of the current chunk to the consolidated list to send signal once
			EntitiesToSignal.Append(Context.GetEntities().GetData(), Context.GetEntities().Num());
		});

	// Signal all entities inside the consolidated list
	if (EntitiesToSignal.Num())
	{
		UMassSignalSubsystem* SignalSubsystem = UWorld::GetSubsystem<UMassSignalSubsystem>(EntitySubsystem.GetWorld());
		checkf(SignalSubsystem != nullptr, TEXT("MassSignalSubsystem should exist when executing fragment initializers."));
		SignalSubsystem->SignalEntities(UE::Mass::Signals::StateTreeInitializationRequested, EntitiesToSignal);
	}
}

//----------------------------------------------------------------------//
// UMassStateTreeFragmentDestructor
//----------------------------------------------------------------------//
UMassStateTreeFragmentDestructor::UMassStateTreeFragmentDestructor()
{
	ExecutionFlags = (int32)(EProcessorExecutionFlags::Standalone | EProcessorExecutionFlags::Server);
	FragmentType = FMassStateTreeFragment::StaticStruct();
}

void UMassStateTreeFragmentDestructor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FMassStateTreeFragment>(EMassFragmentAccess::ReadOnly);
}

void UMassStateTreeFragmentDestructor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	FMassStateTreeExecutionContext StateTreeContext(EntitySubsystem, Context);
	UMassStateTreeSubsystem* MassStateTreeSubsystem = UWorld::GetSubsystem<UMassStateTreeSubsystem>(EntitySubsystem.GetWorld());

	EntityQuery.ForEachEntityChunk(
		EntitySubsystem,
		Context,
		[this, &StateTreeContext, &MassStateTreeSubsystem](FMassExecutionContext&)
		{
			UE::MassBehavior::ProcessChunk(
				StateTreeContext,
				*MassStateTreeSubsystem,
				[](FMassStateTreeExecutionContext& StateTreeExecutionContext, FStateTreeItemView Storage)
				{
					// Stop the tree instance
					StateTreeExecutionContext.Stop(Storage);
				});
		});
}

//----------------------------------------------------------------------//
// UMassStateTreeProcessor
//----------------------------------------------------------------------//
UMassStateTreeProcessor::UMassStateTreeProcessor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bRequiresGameThreadExecution = true;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Behavior;

	// `Behavior` doesn't run on clients but `Tasks` do.
	// We define the dependencies here so task won't need to set their dependency on `Behavior`,
	// but only on `SyncWorldToMass`
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::SyncWorldToMass);
	ExecutionOrder.ExecuteBefore.Add(UE::Mass::ProcessorGroupNames::Tasks);
}

void UMassStateTreeProcessor::Initialize(UObject& Owner)
{
	Super::Initialize(Owner);
	MassStateTreeSubsystem = UWorld::GetSubsystem<UMassStateTreeSubsystem>(Owner.GetWorld());

	SubscribeToSignal(UE::Mass::Signals::StateTreeInitializationRequested);
	SubscribeToSignal(UE::Mass::Signals::LookAtFinished);
	SubscribeToSignal(UE::Mass::Signals::NewStateTreeTaskRequired);
	SubscribeToSignal(UE::Mass::Signals::StandTaskFinished);
	SubscribeToSignal(UE::Mass::Signals::DelayedTransitionWakeup);

	// @todo MassStateTree: add a way to register/unregister from enter/exit state (need reference counting)
	SubscribeToSignal(UE::Mass::Signals::SmartObjectRequestCandidates);
	SubscribeToSignal(UE::Mass::Signals::SmartObjectCandidatesReady);
	SubscribeToSignal(UE::Mass::Signals::SmartObjectInteractionDone);

	SubscribeToSignal(UE::Mass::Signals::FollowPointPathStart);
	SubscribeToSignal(UE::Mass::Signals::FollowPointPathDone);
	SubscribeToSignal(UE::Mass::Signals::CurrentLaneChanged);

	SubscribeToSignal(UE::Mass::Signals::AnnotationTagsChanged);

	SubscribeToSignal(UE::Mass::Signals::HitReceived);

	// @todo MassStateTree: move this to its game plugin when possible
	SubscribeToSignal(UE::Mass::Signals::ContextualAnimTaskFinished);
}

void UMassStateTreeProcessor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FMassStateTreeFragment>(EMassFragmentAccess::ReadWrite);
}

void UMassStateTreeProcessor::SignalEntities(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context, FMassSignalNameLookup& EntitySignals)
{
	if (!MassStateTreeSubsystem)
	{
		return;
	}
	QUICK_SCOPE_CYCLE_COUNTER(StateTreeProcessor_Run);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(StateTreeProcessorExecute);

	const float TimeDelta = Context.GetDeltaTimeSeconds();
	FMassStateTreeExecutionContext StateTreeContext(EntitySubsystem, Context);
	const float TimeInSeconds = EntitySubsystem.GetWorld()->GetTimeSeconds();

	TArray<FMassEntityHandle> EntitiesToSignal;

	EntityQuery.ForEachEntityChunk(
		EntitySubsystem,
		Context,
		[this, &StateTreeContext, TimeDelta, TimeInSeconds, &EntitiesToSignal](FMassExecutionContext& Context)
		{
			TArrayView<FMassStateTreeFragment> StateTreeList = Context.GetMutableComponentView<FMassStateTreeFragment>();

			UE::MassBehavior::ProcessChunk(
				StateTreeContext,
				*MassStateTreeSubsystem,
				[&StateTreeList, TimeDelta, TimeInSeconds, &EntitiesToSignal](FMassStateTreeExecutionContext& StateTreeExecutionContext, const FStateTreeItemView Storage)
				{
					// Keep stats regarding the amount of tree instances ticked per frame
					CSV_CUSTOM_STAT(StateTreeProcessor, NumTickedStateTree, StateTreeExecutionContext.GetEntitySubsystemExecutionContext().GetEntitiesNum(), ECsvCustomStatOp::Accumulate);

					// Compute adjusted delta time
					TOptional<float>& LastUpdate = StateTreeList[StateTreeExecutionContext.GetEntityIndex()].LastUpdateTimeInSeconds;
					const float AdjustedTimeDelta = LastUpdate.IsSet() ? TimeDelta + (TimeInSeconds - LastUpdate.GetValue()) : TimeDelta;
					LastUpdate = TimeInSeconds;

					// Tick the tree instance
					StateTreeExecutionContext.Tick(AdjustedTimeDelta, Storage);

					// When last tick status is different than "Running", the state tree need to be tick again
					// For performance reason, tick again to see if we could find a new state right away instead of waiting the next frame.
					if (StateTreeExecutionContext.GetLastTickStatus(Storage) != EStateTreeRunStatus::Running)
					{
						StateTreeExecutionContext.Tick(0.0f, Storage);

						// Could not find new state yet, try again next frame
						if (StateTreeExecutionContext.GetLastTickStatus(Storage) != EStateTreeRunStatus::Running)
						{
							EntitiesToSignal.Add(StateTreeExecutionContext.GetEntity());
						}
					}
				});
		});

	if (EntitiesToSignal.Num())
	{
		SignalSubsystem->SignalEntities(UE::Mass::Signals::NewStateTreeTaskRequired, EntitiesToSignal);
	}
}
