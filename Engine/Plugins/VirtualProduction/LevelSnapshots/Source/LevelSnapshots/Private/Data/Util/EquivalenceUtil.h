﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class UObject;

struct FWorldSnapshotData;

namespace SnapshotUtil
{
	typedef TFunction<void(UActorComponent* SnapshotComponent, UActorComponent* WorldComponent)> FHandleMatchedActorComponent;
	typedef TFunction<void(UActorComponent*)> FHandleUnmatchedActorComponent;

	/**
	 * Iterates through both actors' component lists and calls the appropriate callback.
	 *
	 * @param SnapshotActor Actor stored in ULevelSnapshot's internal world
	 * @param WorldActor Actor stored in level viewport world
	 * @param OnComponentsMatched Called when component exists on both actors
	 * @param OnSnapshotComponentUnmatched Called when component exists on snapshot but not on editor world
	 * @param OnWorldComponentUnmatched Called when component exists in editor world but not in snapshot world
	 */
	void IterateComponents(AActor* SnapshotActor, AActor* WorldActor, FHandleMatchedActorComponent OnComponentsMatched, FHandleUnmatchedActorComponent OnSnapshotComponentUnmatched, FHandleUnmatchedActorComponent OnWorldComponentUnmatched);

	/** Checks whether the original actor has any properties that changed since the snapshot was taken.  */
	bool HasOriginalChangedPropertiesSinceSnapshotWasTaken(const FWorldSnapshotData& WorldData, AActor* SnapshotActor, AActor* WorldActor);

	/**
	 * Checks whether the snapshot and original property value should be considered equal.
	 * Primitive properties are trivial. Special support is needed for object references.
	 */
	bool AreSnapshotAndOriginalPropertiesEquivalent(const FWorldSnapshotData& WorldData, const FProperty* LeafProperty, void* SnapshotContainer, void* WorldContainer, AActor* SnapshotActor, AActor* WorldActor);

	/**
	 * Two object properties are equivalent if they are
	 *  - both null
	 *  - asset references and point to the same asset
	 *  - actor or component references
	 *  - subobject references where 1. the subobject have the same name, 2. have the same classes, and 3. the properties are equal
	 */
	bool AreObjectPropertiesEquivalent(const FWorldSnapshotData& WorldData, const FObjectPropertyBase* ObjectProperty, void* SnapshotValuePtr, void* WorldValuePtr, AActor* SnapshotActor, AActor* WorldActor);
	
	/** Checks whether two pointers point to "equivalent" objects. */
	bool AreReferencesEquivalent(const FWorldSnapshotData& WorldData, UObject* SnapshotPropertyValue, UObject* OriginalPropertyValue, AActor* SnapshotActor, AActor* OriginalActor);
}
