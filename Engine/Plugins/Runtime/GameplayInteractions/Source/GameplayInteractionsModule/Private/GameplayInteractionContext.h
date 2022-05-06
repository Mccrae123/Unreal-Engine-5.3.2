﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "SmartObjectRuntime.h"
#include "StateTreeExecutionContext.h"
#include "GameplayInteractionContext.generated.h"

class UGameplayInteractionSmartObjectBehaviorDefinition;

/**
 * Struct that holds data required to perform the interaction
 * and wraps StateTree execution
 */
USTRUCT()
struct FGameplayInteractionContext
{
	GENERATED_BODY()

public:
	void SetClaimedHandle(const FSmartObjectClaimHandle& InClaimedHandle) { ClaimedHandle = InClaimedHandle; }
	void SetInteractorActor(AActor* InInteractorActor) { InteractorActor = InInteractorActor; }
	void SetInteractableActor(AActor* InInteractableActor) { InteractableActor = InInteractableActor; }
	
	bool IsValid() const { return ClaimedHandle.IsValid() && InteractorActor != nullptr && InteractableActor != nullptr; }

	/**
	 * Prepares the StateTree execution context using provided Definition then starts the underlying StateTree 
	 * @return True if interaction has been properly initialized and ready to be ticked.
	 */
	bool Activate(const UGameplayInteractionSmartObjectBehaviorDefinition& Definition);

	/**
	 * Updates the underlying StateTree
	 * @return True if still requires to be ticked, false if done.
	 */
	bool Tick(const float DeltaTime);
	
	/**
	 * Stops the underlying StateTree
	 */
	void Deactivate();

protected:	
	/**
	 * Updates all external data views from the provided interaction context.  
	 * @return True if all external data views are valid, false otherwise.
	 */
	bool SetContextRequirements();

	UPROPERTY()
	FStateTreeExecutionContext StateTreeContext;
    
    UPROPERTY()
    FSmartObjectClaimHandle ClaimedHandle;

	UPROPERTY()
    TObjectPtr<AActor> InteractorActor = nullptr;
    
    UPROPERTY()
    TObjectPtr<AActor> InteractableActor = nullptr;
};
