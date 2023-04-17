// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "BaseTools/SingleSelectionMeshEditingTool.h"
#include "BoneIndices.h"
#include "ModelingOperators.h"
#include "ClothTransferSkinWeightsTool.generated.h"

class UClothTransferSkinWeightsTool;
class USkeletalMesh;
class UClothEditorContextObject;
class UTransformProxy;
class UCombinedTransformGizmo;
class UMeshOpPreviewWithBackgroundCompute;
class AInternalToolFrameworkActor;
class UDynamicMeshComponent;

UCLASS()
class CHAOSCLOTHASSETEDITORTOOLS_API UClothTransferSkinWeightsToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Category = Source)
	TObjectPtr<USkeletalMesh> SourceMesh;

	UPROPERTY(EditAnywhere, Category = Source)
	FTransform SourceMeshTransform;

	UPROPERTY(EditAnywhere, Category = Source)
	int32 SourceMeshLOD = 0;

	UPROPERTY(EditAnywhere, Category = Source)
	bool bHideSourceMesh = false;

	UPROPERTY(EditAnywhere, Category = Visualization, meta = (GetOptions = GetBoneNameList))
	FName BoneName;

	// Get the list of valid bone names
	UFUNCTION()
	TArray<FName> GetBoneNameList()
	{
		return BoneNameList;
	}

	UPROPERTY(meta = (TransientToolProperty))
	TArray<FName> BoneNameList;

};


UCLASS()
class CHAOSCLOTHASSETEDITORTOOLS_API UClothTransferSkinWeightsToolBuilder : public USingleSelectionMeshEditingToolBuilder
{
	GENERATED_BODY()

private:

	virtual USingleSelectionMeshEditingTool* CreateNewTool(const FToolBuilderState& SceneState) const override;	

};

UCLASS()
class CHAOSCLOTHASSETEDITORTOOLS_API UClothTransferSkinWeightsTool : public USingleSelectionMeshEditingTool, public UE::Geometry::IDynamicMeshOperatorFactory
{
	GENERATED_BODY()

private:

	friend class UClothTransferSkinWeightsToolBuilder;

	// UInteractiveTool
	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual bool HasAccept() const override { return true; }
	virtual bool HasCancel() const override { return true; }
	virtual bool CanAccept() const override;
	virtual void OnTick(float DeltaTime) override;

	// IDynamicMeshOperatorFactory
	virtual TUniquePtr<UE::Geometry::FDynamicMeshOperator> MakeNewOperator() override;
	
	void SetClothEditorContextObject(TObjectPtr<UClothEditorContextObject> InClothEditorContextObject);

	void AddNewNode();

	void SetPreviewMeshColorFunction();

	void UpdateSourceMesh();

	void PreviewMeshUpdatedCallback(UMeshOpPreviewWithBackgroundCompute* Preview);


	UPROPERTY(Transient)
	TObjectPtr<UClothTransferSkinWeightsToolProperties> ToolProperties;

	UPROPERTY(Transient)
	TObjectPtr<UClothEditorContextObject> ClothEditorContextObject;

	UPROPERTY(Transient)
	TObjectPtr<UMeshOpPreviewWithBackgroundCompute> TargetClothPreview;

	UPROPERTY(Transient)
	TObjectPtr<AInternalToolFrameworkActor> SourceMeshParentActor;

	UPROPERTY(Transient)
	TObjectPtr<UDynamicMeshComponent> SourceMeshComponent;

	// Source mesh transform gizmo support
	UPROPERTY(Transient)
	TObjectPtr<UTransformProxy> SourceMeshTransformProxy;

	UPROPERTY(Transient)
	TObjectPtr<UCombinedTransformGizmo> SourceMeshTransformGizmo;

	// Used to lookup the index of the currently selected-by-name bone
	TMap<FName, FBoneIndexType> TargetMeshBoneNameToIndex;

	bool bHasInvalidLODWarning = false;
};


