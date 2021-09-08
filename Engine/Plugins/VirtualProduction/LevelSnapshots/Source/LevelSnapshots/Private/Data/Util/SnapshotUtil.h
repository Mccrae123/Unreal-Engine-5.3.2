﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
struct FActorSnapshotData;

namespace SnapshotUtil
{
	/** Finds the last subobject name in the path */
	FString ExtractLastSubobjectName(const FSoftObjectPath& ObjectPath);

	/** If path contains an actor, return subobject path to that actor. */
	TOptional<FSoftObjectPath> ExtractActorFromPath(const FSoftObjectPath& OriginalObjectPath, bool& bIsPathToActorSubobject);

	/**
	 * If Path contains a path to an actor, returns that actor.
	 * Example: /Game/MapName.MapName:PersistentLevel.StaticMeshActor_42.StaticMeshComponent returns StaticMeshActor_42's data
	 */
	TOptional<FActorSnapshotData*> FindSavedActorDataUsingObjectPath(TMap<FSoftObjectPath, FActorSnapshotData>& ActorData, const FSoftObjectPath& OriginalObjectPath, bool& bIsPathToActorSubobject);

	/**
	 * Takes an existing path to an actor's subobjects and replaces the actor bit with the path to another actor.
	 *
	 * E.g. /Game/MapName.MapName:PersistentLevel.StaticMeshActor_42.StaticMeshComponent could become /Game/MapName.MapName:PersistentLevel.SomeOtherActor.StaticMeshComponent
	 */
	FSoftObjectPath SetActorInPath(AActor* NewActor, const FSoftObjectPath& OriginalObjectPath);
};
