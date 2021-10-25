﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "Restorability/ClassRestorationSkipper.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

ISnapshotRestorabilityOverrider::ERestorabilityOverride FClassRestorationSkipper::IsActorDesirableForCapture(const AActor* Actor)
{
	const FSkippedClassList& SkippedClasses = GetSkippedClassListCallback.Execute();
	for (UClass* ActorClass = Actor->GetClass(); ActorClass; ActorClass = ActorClass->GetSuperClass())
	{
		if (SkippedClasses.ActorClasses.Contains(ActorClass))
		{
			return ERestorabilityOverride::Disallow;
		}
	}
	
	return ERestorabilityOverride::DoNotCare;
}

ISnapshotRestorabilityOverrider::ERestorabilityOverride FClassRestorationSkipper::IsComponentDesirableForCapture(const UActorComponent* Component)
{
	const FSkippedClassList& SkippedClasses = GetSkippedClassListCallback.Execute();
	for (UClass* ComponentClass = Component->GetClass(); ComponentClass; ComponentClass = ComponentClass->GetSuperClass())
	{
		if (SkippedClasses.ComponentClasses.Contains(ComponentClass))
		{
			return ERestorabilityOverride::Disallow;
		}
	}
	
	return ERestorabilityOverride::DoNotCare;
}