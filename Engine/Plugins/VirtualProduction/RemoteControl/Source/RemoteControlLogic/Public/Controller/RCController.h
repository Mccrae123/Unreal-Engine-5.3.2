﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RCVirtualProperty.h"
#include "RCController.generated.h"

class URCBehaviour;
class URCBehaviourNode;

DECLARE_MULTICAST_DELEGATE(FOnBehaviourListModified);

/**
 * Remote Control Controller. Container for Behaviours and Actions
 */
UCLASS(BlueprintType)
class REMOTECONTROLLOGIC_API URCController : public URCVirtualPropertyInContainer
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	/** Called after applying a transaction to the object. Used to broadcast Undo related container changes to UI Used to broadcast Undo related container changes to UI */
	virtual void PostEditUndo();
#endif

	/** Create and add behaviour to behaviour set */
	virtual URCBehaviour* AddBehaviour(TSubclassOf<URCBehaviourNode> InBehaviourNodeClass);

	/** Create new behaviour */
	virtual URCBehaviour* CreateBehaviour(TSubclassOf<URCBehaviourNode> InBehaviourNodeClass);

	/** Remove the behaviour by behaviour UObject pointer */
	virtual int32 RemoveBehaviour(URCBehaviour* InBehaviour);

	/** Remove the behaviour by behaviour id */
	virtual int32 RemoveBehaviour(const FGuid InBehaviourId);

	/** Removes all behaviours. */
	virtual void EmptyBehaviours();

	/** Execute all behaviours for this controller. */
	virtual void ExecuteBehaviours();

	/** Handles modifications to controller value; evaluates all behaviours */
	virtual void OnModifyPropertyValue() override
	{
		ExecuteBehaviours();
	}

	/** Delegate that notifies changes to the list of behaviours*/
	FOnBehaviourListModified OnBehaviourListModified;

public:
	/** Set of the behaviours */
	UPROPERTY()
	TSet<TObjectPtr<URCBehaviour>> Behaviours;
};
