﻿// Copyright Epic Games, Inc. All Rights Reserved.


#include "ConstraintsActor.h"

#include "ConstraintsManager.h"

// Sets default values
AConstraintsActor::AConstraintsActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	
#if WITH_EDITORONLY_DATA
	bLockLocation = true;
	bHiddenEd = true;
#endif // WITH_EDITORONLY_DATA
}

AConstraintsActor::~AConstraintsActor()
{}

void AConstraintsActor::BeginDestroy()
{
	Super::BeginDestroy();
}

void AConstraintsActor::Destroyed()
{
	if(ConstraintsManager)
	{
		ConstraintsManager->Clear();
	}
	
	Super::Destroyed();
}

void AConstraintsActor::BeginPlay()
{
	Super::BeginPlay();
}

void AConstraintsActor::Tick(float DeltaTime)
{	
	Super::Tick(DeltaTime);
}

void AConstraintsActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();	
	RegisterConstraintsTickFunctions();
}

void AConstraintsActor::RegisterConstraintsTickFunctions() const
{
	// note ensure that this is not done when useless ( use Level->bIsAssociatingLevel ?)
	if (ULevel* Level = GetLevel())
	{
		if (ConstraintsManager)
		{
			// remove invalid pointers
			ConstraintsManager->Constraints.RemoveAll( [](const TObjectPtr<UTickableConstraint>& InConstraint)
			{
				return !IsValid(InConstraint);
			});
			
			// ensure registration
			for (TObjectPtr<UTickableConstraint> ConstPtr: ConstraintsManager->Constraints)
			{
				ConstPtr->ConstraintTick.RegisterTickFunction(Level);
			}
		}
	}
}