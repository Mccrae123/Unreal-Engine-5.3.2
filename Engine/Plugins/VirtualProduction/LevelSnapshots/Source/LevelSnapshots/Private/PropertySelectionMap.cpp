﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "PropertySelectionMap.h"

#include "GameFramework/Actor.h"

void FPropertySelectionMap::AddDeletedActorToRespawn(const FSoftObjectPath& Original)
{
	DeletedActorsToRespawn.Add(Original);
}

void FPropertySelectionMap::RemoveDeletedActorToRespawn(const FSoftObjectPath& Original)
{
	DeletedActorsToRespawn.Remove(Original);
}

void FPropertySelectionMap::AddNewActorToDespawn(AActor* WorldActor)
{
	NewActorsToDespawn.Add(WorldActor);
}

void FPropertySelectionMap::RemoveNewActorToDespawn(AActor* WorldActor)
{
	NewActorsToDespawn.Remove(WorldActor);
}

void FPropertySelectionMap::AddObjectProperties(UObject* WorldObject, const FPropertySelection& SelectedProperties)
{
	if (!SelectedProperties.IsEmpty() && ensure(WorldObject))
	{
		SelectedWorldObjectsToSelectedProperties.FindOrAdd(WorldObject) = SelectedProperties;
	}
}

void FPropertySelectionMap::RemoveObjectPropertiesFromMap(UObject* WorldObject)
{
	SelectedWorldObjectsToSelectedProperties.Remove(WorldObject);
}

const FPropertySelection* FPropertySelectionMap::GetSelectedProperties(UObject* WorldObject) const
{
	return SelectedWorldObjectsToSelectedProperties.Find(WorldObject); 
}

TArray<TWeakObjectPtr<UObject>> FPropertySelectionMap::GetKeys() const
{
	TArray<TWeakObjectPtr<UObject>> Result;
	SelectedWorldObjectsToSelectedProperties.GenerateKeyArray(Result);
	return Result;
}

const TSet<FSoftObjectPath>& FPropertySelectionMap::GetDeletedActorsToRespawn() const
{
	return DeletedActorsToRespawn;
}

const TSet<TWeakObjectPtr<AActor>>& FPropertySelectionMap::GetNewActorsToDespawn() const
{
	return NewActorsToDespawn;
}

void FPropertySelectionMap::Empty(bool bCanShrink)
{
	SelectedWorldObjectsToSelectedProperties.Empty(bCanShrink ? SelectedWorldObjectsToSelectedProperties.Num() : 0);
	DeletedActorsToRespawn.Empty(bCanShrink ? DeletedActorsToRespawn.Num() : 0);
	NewActorsToDespawn.Empty(bCanShrink ? NewActorsToDespawn.Num() : 0);
}
