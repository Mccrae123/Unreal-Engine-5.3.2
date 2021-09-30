// Copyright Epic Games, Inc. All Rights Reserved.

#include "Evaluators/MassStateTreeSmartObjectEvaluator.h"

#include "MassAIBehaviorTypes.h"
#include "MassCommonFragments.h"
#include "MassAIMovementFragments.h"
#include "MassSignalSubsystem.h"
#include "MassStateTreeExecutionContext.h"
#include "MassSmartObjectHandler.h"
#include "MassSmartObjectProcessor.h"
#include "MassStateTreeProcessors.h"
#include "MassZoneGraphMovementFragments.h"
#include "SmartObjectSubsystem.h"
#include "Engine/World.h"

//----------------------------------------------------------------------//
// FMassStateTreeSmartObjectEvaluator
//----------------------------------------------------------------------//
void FMassStateTreeSmartObjectEvaluator::EnterState(FStateTreeExecutionContext& Context, const EStateTreeStateChangeType ChangeType, const FStateTreeTransitionResult& Transition)
{
	if (ChangeType != EStateTreeStateChangeType::Changed)
	{
		return;
	}

	Reset();
}

void FMassStateTreeSmartObjectEvaluator::ExitState(FStateTreeExecutionContext& Context, const EStateTreeStateChangeType ChangeType, const FStateTreeTransitionResult& Transition)
{
	if (ChangeType != EStateTreeStateChangeType::Changed)
	{
		return;
	}

	if (SearchRequestID.IsSet())
	{
		const FMassStateTreeExecutionContext& MassContext = static_cast<FMassStateTreeExecutionContext&>(Context);
		USmartObjectSubsystem& SmartObjectSubsystem = Context.GetExternalItem(SmartObjectSubsystemHandle).GetMutable<USmartObjectSubsystem>();
		const FMassSmartObjectHandler MassSmartObjectHandler(MassContext.GetEntitySubsystem(), MassContext.GetEntitySubsystemExecutionContext(), SmartObjectSubsystem);
		MassSmartObjectHandler.RemoveRequest(SearchRequestID);
		SearchRequestID.Reset();
	}
	Reset();
}

void FMassStateTreeSmartObjectEvaluator::Reset()
{
	bCandidatesFound = false;
	bClaimed = false;
	SearchRequestID.Reset();
}

void FMassStateTreeSmartObjectEvaluator::Evaluate(FStateTreeExecutionContext& Context, const EStateTreeEvaluationType EvalType,  const float DeltaTime)
{
	const FDataFragment_SmartObjectUser& SOUser = Context.GetExternalItem(SmartObjectUserHandle).Get<FDataFragment_SmartObjectUser>();

	bCandidatesFound = false;
	bClaimed = SOUser.GetClaimHandle().IsValid();

	// Already claimed, nothing to do
	if (bClaimed)
	{
		return;
	}

	const UWorld* World = Context.GetWorld();
	if (SOUser.GetCooldown() > World->GetTimeSeconds())
	{
		return;
	}

	// We need to track our next update cooldown since we can get ticked from any signals waking up the StateTree
	if (NextUpdate > World->GetTimeSeconds())
	{
		return;
	}
	NextUpdate = 0.f;

	USmartObjectSubsystem& SmartObjectSubsystem = Context.GetExternalItem(SmartObjectSubsystemHandle).GetMutable<USmartObjectSubsystem>();
	const FMassStateTreeExecutionContext& MassContext = static_cast<FMassStateTreeExecutionContext&>(Context);
	const FMassSmartObjectHandler MassSmartObjectHandler(MassContext.GetEntitySubsystem(), MassContext.GetEntitySubsystemExecutionContext(), SmartObjectSubsystem);

	// Nothing claimed -> search for candidates
	if (!SearchRequestID.IsSet())
	{
		// Use lanes if possible for faster queries using zone graph annotations
		const FMassEntityHandle RequestingEntity = MassContext.GetEntity();
		const FMassZoneGraphLaneLocationFragment* LaneLocation = Context.GetExternalItem(LocationHandle).GetPtr<FMassZoneGraphLaneLocationFragment>();
		bUsingZoneGraphAnnotations = LaneLocation != nullptr;
		if (bUsingZoneGraphAnnotations)
		{
			MASSBEHAVIOR_CLOG(!LaneLocation->LaneHandle.IsValid(), Error, TEXT("Always expecting a valid lane from the ZoneGraph movement"));
			if (LaneLocation->LaneHandle.IsValid())
			{
				SearchRequestID = MassSmartObjectHandler.FindCandidatesAsync(RequestingEntity, { LaneLocation->LaneHandle, LaneLocation->DistanceAlongLane });
			}
		}
		else
		{
			const FDataFragment_Transform& TransformFragment = Context.GetExternalItem(EntityTransformHandle).Get<FDataFragment_Transform>();
			SearchRequestID = MassSmartObjectHandler.FindCandidatesAsync(RequestingEntity, TransformFragment.GetTransform().GetLocation());
		}
	}
	else
	{
		// Fetch request results
		SearchRequestResult = MassSmartObjectHandler.GetRequestResult(SearchRequestID);

		// Check if results are ready
		if (SearchRequestResult.bProcessed)
		{
			// Remove requests
			MassSmartObjectHandler.RemoveRequest(SearchRequestID);
			SearchRequestID.Reset();

			// Update bindable flag to indicate to tasks and conditions if some candidates were found
			bCandidatesFound = SearchRequestResult.NumCandidates > 0;

			const FMassEntityHandle RequestingEntity = MassContext.GetEntity();
			MASSBEHAVIOR_CLOG(bCandidatesFound, Log, TEXT("Found %d smart object candidates for %s"), SearchRequestResult.NumCandidates, *RequestingEntity.DebugGetDescription());

			// When using ZoneGraph annotations we don't need to schedule a new update since we only need the CurrentLaneChanged signal.
			// Otherwise we reschedule with default interval on success or retry interval on failed attempt
			if (!bUsingZoneGraphAnnotations)
			{
				const float DelayInSeconds = bCandidatesFound ? TickInterval : RetryCooldown;
				NextUpdate = World->GetTimeSeconds() + DelayInSeconds;
				UMassSignalSubsystem& MassSignalSubsystem = Context.GetExternalItem(MassSignalSubsystemHandle).GetMutable<UMassSignalSubsystem>();
				MassSignalSubsystem.DelaySignalEntity(UE::Mass::Signals::SmartObjectRequestCandidates, RequestingEntity, DelayInSeconds);
			}
		}
		// else wait for the Evaluation that will be triggered by the "candidates ready" signal
	}
}
