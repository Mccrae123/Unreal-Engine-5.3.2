// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorGizmos/TransformGizmo.h"
#include "EditorTransformGizmo.generated.h"

/**
 * UEditorTransformGizmo handles Editor-specific functionality for the TransformGizmo,
 * applied to a UEditorTransformProxy target object.
 */
UCLASS()
class EDITORINTERACTIVETOOLSFRAMEWORK_API UEditorTransformGizmo : public UTransformGizmo
{
	GENERATED_BODY()

protected:

	/** Apply translate delta to transform proxy */
	virtual void ApplyTranslateDelta(const FVector& InTranslateDelta);

	/** Apply scale delta to transform proxy */
	virtual void ApplyScaleDelta(const FVector& InScaleDelta);
};