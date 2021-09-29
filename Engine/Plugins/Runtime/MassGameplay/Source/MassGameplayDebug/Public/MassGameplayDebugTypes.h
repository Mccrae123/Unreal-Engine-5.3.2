// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "UObject/ObjectMacros.h"
#include "Logging/LogMacros.h"
#include "Templates/SubclassOf.h"
#include "Engine/DataTable.h"
#include "LWComponentTypes.h"
#include "MassSpawnerTypes.h"
#include "MassGameplayDebugTypes.generated.h"


DECLARE_LOG_CATEGORY_EXTERN(LogMassDebug, Warning, All);

#if WITH_EDITORONLY_DATA
class UBillboardComponent;
#endif // WITH_EDITORONLY_DATA
class UStaticMesh;
class UMaterialInterface;


USTRUCT()
struct FSimDebugDataRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = Debug)
	UStaticMesh* Mesh = nullptr;

	UPROPERTY(EditAnywhere, Category = Debug)
	UMaterialInterface* MaterialOverride = nullptr;

	UPROPERTY(EditAnywhere, Category = Debug)
	float Scale = 1.f;
};

USTRUCT()
struct FSimDebugVisComponent : public FLWComponentData
{
	GENERATED_BODY()
	int32 InstanceIndex = INDEX_NONE;
	int16 VisualType = INDEX_NONE;
};

UENUM()
enum class EMassEntityDebugShape : uint8
{
	Box,
	Cone,
	Cylinder,
	Capsule,
	MAX
};

USTRUCT()
struct FDataFragment_DebugVis : public FLWComponentData
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category = Debug)
	EMassEntityDebugShape Shape = EMassEntityDebugShape::Box;
};

USTRUCT()
struct FAgentDebugVisualization : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Mass|Debug")
	UStaticMesh* Mesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Mass|Debug")
	UMaterialInterface* MaterialOverride = nullptr;

	/** Near cull distance to override default value for that agent type */
	UPROPERTY(EditAnywhere, Category = "Mass|Debug")
	float VisualNearCullDistance = 5000.f;

	/** Far cull distance to override default value for that agent type */
	UPROPERTY(EditAnywhere, Category = "Mass|Debug")
	float VisualFarCullDistance = 7500.f;

	/** If Mesh is not set this WireShape will be used for debug drawing via GameplayDebugger */
	UPROPERTY(EditAnywhere, Category = "Mass|Debug")
	EMassEntityDebugShape WireShape = EMassEntityDebugShape::Box;
};

/** @todo comment functionality */
USTRUCT(DisplayName="MassSpawnProps_Debug")
struct FMassSpawnProps : public FMassSpawnConfigBase
{
	GENERATED_BODY()

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = "Mass|Spawn")
	FAgentDebugVisualization DebugShape;
#endif // WITH_EDITORONLY_DATA

	friend uint32 GetTypeHash(const FMassSpawnProps& Instance)
	{
#if WITH_EDITORONLY_DATA
		return HashCombine(GetTypeHash((const FMassSpawnConfigBase&)Instance)
				, HashCombine(GetTypeHash(Instance.DebugShape.Mesh), GetTypeHash(Instance.DebugShape.MaterialOverride)));
#else
		return GetTypeHash((const FMassSpawnConfigBase&)Instance);
#endif // WITH_EDITORONLY_DATA
	}
};