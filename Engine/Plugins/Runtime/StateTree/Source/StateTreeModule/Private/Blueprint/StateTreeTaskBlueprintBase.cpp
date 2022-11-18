// Copyright Epic Games, Inc. All Rights Reserved.

#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "CoreMinimal.h"
#include "StateTreeExecutionContext.h"
#include "BlueprintNodeHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StateTreeTaskBlueprintBase)

//----------------------------------------------------------------------//
//  UStateTreeTaskBlueprintBase
//----------------------------------------------------------------------//

UStateTreeTaskBlueprintBase::UStateTreeTaskBlueprintBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bShouldStateChangeOnReselect(true)
	, bShouldCallTickOnlyOnEvents(false)
	, bShouldCopyBoundPropertiesOnTick(true)
	, bShouldCopyBoundPropertiesOnExitState(true)
{
	bHasEnterState = BlueprintNodeHelpers::HasBlueprintFunction(TEXT("ReceiveEnterState"), *this, *StaticClass());
	bHasExitState = BlueprintNodeHelpers::HasBlueprintFunction(TEXT("ReceiveExitState"), *this, *StaticClass());
	bHasStateCompleted = BlueprintNodeHelpers::HasBlueprintFunction(TEXT("ReceiveStateCompleted"), *this, *StaticClass());
	bHasTick = BlueprintNodeHelpers::HasBlueprintFunction(TEXT("ReceiveTick"), *this, *StaticClass());
}

EStateTreeRunStatus UStateTreeTaskBlueprintBase::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition)
{
	if (bHasEnterState)
	{
		FScopedCurrentContext(*this, Context);
		return ReceiveEnterState(Transition);
	}
	return EStateTreeRunStatus::Running;
}

void UStateTreeTaskBlueprintBase::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition)
{
	if (bHasExitState)
	{
		FScopedCurrentContext(*this, Context);
		ReceiveExitState(Transition);
	}
}

void UStateTreeTaskBlueprintBase::StateCompleted(FStateTreeExecutionContext& Context, const EStateTreeRunStatus CompletionStatus, const FStateTreeActiveStates& CompletedActiveStates)
{
	if (bHasStateCompleted)
	{
		FScopedCurrentContext(*this, Context);
		ReceiveStateCompleted(CompletionStatus, CompletedActiveStates);
	}
}

EStateTreeRunStatus UStateTreeTaskBlueprintBase::Tick(FStateTreeExecutionContext& Context, const float DeltaTime)
{
	if (bHasTick)
	{
		FScopedCurrentContext(*this, Context);
		return ReceiveTick(DeltaTime);
	}
	return EStateTreeRunStatus::Running;
}

//----------------------------------------------------------------------//
//  FStateTreeBlueprintTaskWrapper
//----------------------------------------------------------------------//

EDataValidationResult FStateTreeBlueprintTaskWrapper::Compile(FStateTreeDataView InstanceDataView, TArray<FText>& ValidationMessages)
{
	const UStateTreeTaskBlueprintBase& InstanceData = InstanceDataView.Get<UStateTreeTaskBlueprintBase>();
	
	// Copy over ticking related options.
	bShouldStateChangeOnReselect = InstanceData.bShouldStateChangeOnReselect;
	bShouldCallTick = InstanceData.bHasTick;
	bShouldCallTickOnlyOnEvents = InstanceData.bShouldCallTickOnlyOnEvents;
	bShouldCopyBoundPropertiesOnTick = InstanceData.bShouldCopyBoundPropertiesOnTick;
	bShouldCopyBoundPropertiesOnExitState = InstanceData.bShouldCopyBoundPropertiesOnExitState;

	return EDataValidationResult::Valid;
}

EStateTreeRunStatus FStateTreeBlueprintTaskWrapper::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	UStateTreeTaskBlueprintBase* Instance = Context.GetInstanceDataPtr<UStateTreeTaskBlueprintBase>(*this);
	check(Instance);
	return Instance->EnterState(Context, Transition);
}

void FStateTreeBlueprintTaskWrapper::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	UStateTreeTaskBlueprintBase* Instance = Context.GetInstanceDataPtr<UStateTreeTaskBlueprintBase>(*this);
	check(Instance);
	Instance->ExitState(Context, Transition);
}

void FStateTreeBlueprintTaskWrapper::StateCompleted(FStateTreeExecutionContext& Context, const EStateTreeRunStatus CompletionStatus, const FStateTreeActiveStates& CompletedActiveStates) const
{
	UStateTreeTaskBlueprintBase* Instance = Context.GetInstanceDataPtr<UStateTreeTaskBlueprintBase>(*this);
	check(Instance);
	Instance->StateCompleted(Context, CompletionStatus, CompletedActiveStates);
}

EStateTreeRunStatus FStateTreeBlueprintTaskWrapper::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	UStateTreeTaskBlueprintBase* Instance = Context.GetInstanceDataPtr<UStateTreeTaskBlueprintBase>(*this);
	check(Instance);
	return Instance->Tick(Context, DeltaTime);
}

