﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassSmartObjectRequest.h"
#include "MassStateTreeTypes.h"
#include "MassClaimSmartObjectTask.generated.h"

struct FStateTreeExecutionContext;
struct FMassSmartObjectUserFragment;
class USmartObjectSubsystem;
class UMassSignalSubsystem;
struct FTransformFragment;
struct FMassMoveTargetFragment;

/**
 * Tasks to claim a smart object from search results and release it when done.
 */
USTRUCT()
struct MASSAIBEHAVIOR_API FMassClaimSmartObjectTaskInstanceData
{
	GENERATED_BODY()

	/** Result of the candidates search request (Input) */
	UPROPERTY(VisibleAnywhere, Category = Input)
	FMassSmartObjectCandidateSlots CandidateSlots;

	UPROPERTY(VisibleAnywhere, Category = Output)
	FSmartObjectClaimHandle ClaimedSlot;
};

USTRUCT(meta = (DisplayName = "Claim SmartObject"))
struct MASSAIBEHAVIOR_API FMassClaimSmartObjectTask : public FMassStateTreeTaskBase
{
	GENERATED_BODY()

	using FInstanceDataType = FMassClaimSmartObjectTaskInstanceData;

protected:
	virtual bool Link(FStateTreeLinker& Linker) override;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const EStateTreeStateChangeType ChangeType, const FStateTreeTransitionResult& Transition) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const EStateTreeStateChangeType ChangeType, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;

	TStateTreeExternalDataHandle<FMassSmartObjectUserFragment> SmartObjectUserHandle;
	TStateTreeExternalDataHandle<USmartObjectSubsystem> SmartObjectSubsystemHandle;
	TStateTreeExternalDataHandle<UMassSignalSubsystem> MassSignalSubsystemHandle;

	TStateTreeInstanceDataPropertyHandle<FMassSmartObjectCandidateSlots> CandidateSlotsHandle;
	TStateTreeInstanceDataPropertyHandle<FSmartObjectClaimHandle> ClaimedSlotHandle;

	/** Delay in seconds before trying to use another smart object */
	UPROPERTY(EditAnywhere, Category = Parameter)
	float InteractionCooldown = 0.f;
};
