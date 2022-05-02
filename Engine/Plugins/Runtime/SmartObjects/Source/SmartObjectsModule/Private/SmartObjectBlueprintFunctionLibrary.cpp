// Copyright Epic Games, Inc. All Rights Reserved.

#include "SmartObjectBlueprintFunctionLibrary.h"
#include "SmartObjectSubsystem.h"
#include "BlackboardKeyType_SOClaimHandle.h"
#include "BehaviorTree/BlackboardComponent.h"

//----------------------------------------------------------------------//
// USmartObjectBlueprintFunctionLibrary 
//----------------------------------------------------------------------//
FSmartObjectClaimHandle USmartObjectBlueprintFunctionLibrary::GetValueAsSOClaimHandle(UBlackboardComponent* BlackboardComponent, const FName& KeyName)
{
	if (BlackboardComponent == nullptr)
	{
		return {};
	}
	return BlackboardComponent->GetValue<UBlackboardKeyType_SOClaimHandle>(KeyName);
}

void USmartObjectBlueprintFunctionLibrary::SetValueAsSOClaimHandle(UBlackboardComponent* BlackboardComponent, const FName& KeyName, const FSmartObjectClaimHandle Value)
{
	if (BlackboardComponent == nullptr)
	{
		return;
	}
	const FBlackboard::FKey KeyID = BlackboardComponent->GetKeyID(KeyName);
	BlackboardComponent->SetValue<UBlackboardKeyType_SOClaimHandle>(KeyID, Value);
}

bool USmartObjectBlueprintFunctionLibrary::K2_SetSmartObjectEnabled(AActor* SmartObject, const bool bEnabled)
{
	if (SmartObject == nullptr)
	{
		return false;
	}

	UWorld* World = SmartObject->GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(World);
	if (Subsystem == nullptr)
	{
		return false;
	}

	return bEnabled ? Subsystem->RegisterSmartObjectActor(*SmartObject)
		: Subsystem->UnregisterSmartObjectActor(*SmartObject);
}
