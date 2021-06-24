// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MultiSelectionTool.h"
#include "InteractiveToolBuilder.h"
#include "PropertySets/CreateMeshObjectTypeProperties.h"
#include "SplitMeshesTool.generated.h"

class UMaterialInterface;


UCLASS()
class MESHMODELINGTOOLS_API USplitMeshesToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()
public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;

protected:
	virtual const FToolTargetTypeRequirements& GetTargetRequirements() const override;
};



UCLASS()
class MESHMODELINGTOOLS_API USplitMeshesToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = Options)
	bool bTransferMaterials = true;
};



UCLASS()
class MESHMODELINGTOOLS_API USplitMeshesTool : public UMultiSelectionTool
{
	GENERATED_BODY()

public:
	virtual void SetWorld(UWorld* World);

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;

	virtual bool HasCancel() const override { return true; }
	virtual bool HasAccept() const override { return true; }
	virtual bool CanAccept() const override;

	UPROPERTY()
	USplitMeshesToolProperties* BasicProperties;

	UPROPERTY()
	UCreateMeshObjectTypeProperties* OutputTypeProperties;

protected:
	UWorld* TargetWorld;

	struct FSourceMeshInfo
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		TArray<UMaterialInterface*> Materials;
	};
	TArray<FSourceMeshInfo> SourceMeshes;


	struct FComponentsInfo
	{
		bool bNoComponents;
		TArray<UE::Geometry::FDynamicMesh3> Meshes;
		TArray<TArray<UMaterialInterface*>> Materials;
		TArray<FVector3d> Origins;
	};
	TArray<FComponentsInfo> SplitMeshes;

	int32 NoSplitCount = 0;

	void UpdateSplitMeshes();
};
