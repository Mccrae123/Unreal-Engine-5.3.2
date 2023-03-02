// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineBaseTypes.h"

UENUM(BlueprintType)
enum class EDeformableExecutionModel : uint8
{
	Chaos_Deformable_PrePhysics UMETA(DisplayName = "Before Physics"),
	Chaos_Deformable_DuringPhysics UMETA(DisplayName = "During Physics"),
	Chaos_Deformable_PostPhysics UMETA(DisplayName = "After Physics"),
	//
	Chaos_Max UMETA(Hidden)
};

struct FChaosEngineDeformableCVarParams
{
	bool bEnableDeformableSolver = true;
#ifdef WITH_ENGINE
	bool bDoDrawSimulationMesh = true;
#else
	bool bDoDrawSimulationMesh = false;
#endif
	bool bDoDrawSkeletalMeshBindingPositions = false;
	float DrawSkeletalMeshBindingPositionsAnimationBlendWeight = 0.f;
};