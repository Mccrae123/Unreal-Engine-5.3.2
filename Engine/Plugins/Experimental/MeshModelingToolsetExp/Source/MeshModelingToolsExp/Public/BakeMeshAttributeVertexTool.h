// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MultiSelectionTool.h"
#include "InteractiveToolBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "ModelingOperators.h"
#include "MeshOpPreviewHelpers.h"
#include "PreviewMesh.h"
#include "Sampling/MeshVertexBaker.h"
#include "BakeMeshAttributeTool.h"
#include "BakeMeshAttributeVertexTool.generated.h"

// predeclarations
class UMaterialInstanceDynamic;


/**
 * Tool Builder
 */
UCLASS()
class MESHMODELINGTOOLSEXP_API UBakeMeshAttributeVertexToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;

protected:
	virtual const FToolTargetTypeRequirements& GetTargetRequirements() const override;
};


UENUM()
enum class EBakeVertexOutput
{
	/* Bake vertex data to RGBA */
	RGBA,
	/* Bake vertex data to individual color channels */
	PerChannel
};


UENUM()
enum class EBakeVertexChannel
{
	R,
	G,
	B,
	A,
	RGBA
};


UCLASS()
class MESHMODELINGTOOLSEXP_API UBakeMeshAttributeVertexToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()

public:
	/** The bake types to generate */
	UPROPERTY(EditAnywhere, Category = BakeOutput)
	EBakeVertexOutput VertexOutput = EBakeVertexOutput::RGBA;

	/** The vertex channel to preview */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta = (TransientToolProperty))
	EBakeVertexChannel VertexChannelPreview = EBakeVertexChannel::RGBA;

	/** The bake type to generate */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta=(
		ValidEnumValues="TangentSpaceNormal, AmbientOcclusion, BentNormal, Curvature, Texture, ObjectSpaceNormal, FaceNormal, Position, MaterialID, MultiTexture",
		EditCondition="VertexOutput == EBakeVertexOutput::RGBA", EditConditionHides))
	EBakeMapType BakeTypeRGBA = EBakeMapType::TangentSpaceNormal;

	/** The bake type to generate in the Red channel */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta=(
		ValidEnumValues="None, AmbientOcclusion, Curvature",
		EditCondition="VertexOutput == EBakeVertexOutput::PerChannel", EditConditionHides))
	EBakeMapType BakeTypeR = EBakeMapType::None;

	/** The bake type to generate in the Green channel */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta=(
		ValidEnumValues="None, AmbientOcclusion, Curvature",
		EditCondition="VertexOutput == EBakeVertexOutput::PerChannel", EditConditionHides))
	EBakeMapType BakeTypeG = EBakeMapType::None;

	/** The bake type to generate in the Blue channel */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta=(
		ValidEnumValues="None, AmbientOcclusion, Curvature",
		EditCondition="VertexOutput == EBakeVertexOutput::PerChannel", EditConditionHides))
	EBakeMapType BakeTypeB = EBakeMapType::None;

	/** The bake type to generate in the Alpha channel */
	UPROPERTY(EditAnywhere, Category = BakeOutput, meta=(
		ValidEnumValues="None, AmbientOcclusion, Curvature",
		EditCondition="VertexOutput == EBakeVertexOutput::PerChannel", EditConditionHides))
	EBakeMapType BakeTypeA = EBakeMapType::None;

	/** Split vertex colors at normal seams */
	UPROPERTY(EditAnywhere, Category = BakeOutput)
	bool bSplitAtNormalSeams = false;

	/** Split vertex colors at UV seams */
	UPROPERTY(EditAnywhere, Category = BakeOutput)
	bool bSplitAtUVSeams = false;
};


/**
 * Vertex Baking Tool
 */
UCLASS()
class MESHMODELINGTOOLSEXP_API UBakeMeshAttributeVertexTool : public UBakeMeshAttributeTool, public UE::Geometry::IGenericDataOperatorFactory<UE::Geometry::FMeshVertexBaker>
{
	GENERATED_BODY()

public:
	UBakeMeshAttributeVertexTool() = default;

	// Begin UInteractiveTool interface
	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;

	virtual void OnTick(float DeltaTime) override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;

	virtual bool HasCancel() const override { return true; }
	virtual bool HasAccept() const override { return true; }
	virtual bool CanAccept() const override;
	// End UInteractiveTool interface

	// Begin IGenericDataOperatorFactory interface
	virtual TUniquePtr<UE::Geometry::TGenericDataOperator<UE::Geometry::FMeshVertexBaker>> MakeNewOperator() override;
	// End IGenericDataOperatorFactory interface

protected:
	UPROPERTY()
	TObjectPtr<UBakeInputMeshProperties> MeshProps;
	
	UPROPERTY()
	TObjectPtr<UBakeMeshAttributeVertexToolProperties> Settings;

	UPROPERTY()
	TObjectPtr<UBakedOcclusionMapToolProperties> OcclusionSettings;

	UPROPERTY()
	TObjectPtr<UBakedCurvatureMapToolProperties> CurvatureSettings;

	UPROPERTY()
	TObjectPtr<UBakedTexture2DImageProperties> TextureSettings;

	UPROPERTY()
	TObjectPtr<UBakedMultiTexture2DImageProperties> MultiTextureSettings;

protected:
	friend class FMeshVertexBakerOp;
	
	UPROPERTY()
	TObjectPtr<UPreviewMesh> PreviewMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PreviewMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PreviewAlphaMaterial;

	TUniquePtr<TGenericDataBackgroundCompute<UE::Geometry::FMeshVertexBaker>> Compute = nullptr;
	void OnResultUpdated(const TUniquePtr<UE::Geometry::FMeshVertexBaker>& NewResult);

	TSharedPtr<UE::Geometry::TMeshTangents<double>, ESPMode::ThreadSafe> BaseMeshTangents;
	UE::Geometry::FDynamicMesh3 BaseMesh;
	UE::Geometry::FDynamicMeshAABBTree3 BaseSpatial;

	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> DetailMesh;
	TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3, ESPMode::ThreadSafe> DetailSpatial;
	int32 DetailMeshTimestamp = 0;
	void UpdateDetailMesh();

	bool bColorTopologyValid = false;
	bool bIsBakeToSelf = false;
	void UpdateOnModeChange();
	void UpdateVisualization();
	void UpdateColorTopology();
	void UpdateResult();

	const bool bPreferPlatformData = false;

	struct FBakeSettings
	{
		EBakeVertexOutput VertexOutput = EBakeVertexOutput::RGBA;
		EBakeMapType BakeTypeRGBA = EBakeMapType::TangentSpaceNormal;
		EBakeMapType BakeTypePerChannel[4] = { EBakeMapType::None, EBakeMapType::None, EBakeMapType::None, EBakeMapType::None };
		EBakeVertexChannel VertexChannelPreview = EBakeVertexChannel::RGBA;
		float ProjectionDistance = 3.0;
		bool bProjectionInWorldSpace = false;
		bool bSplitAtNormalSeams = false;
		bool bSplitAtUVSeams = false;

		bool operator==(const FBakeSettings& Other) const
		{
			return (VertexOutput == Other.VertexOutput &&
				BakeTypeRGBA == Other.BakeTypeRGBA &&
				BakeTypePerChannel[0] == Other.BakeTypePerChannel[0] &&
				BakeTypePerChannel[1] == Other.BakeTypePerChannel[1] &&
				BakeTypePerChannel[2] == Other.BakeTypePerChannel[2] &&
				BakeTypePerChannel[3] == Other.BakeTypePerChannel[3] &&
				bProjectionInWorldSpace == Other.bProjectionInWorldSpace &&
				ProjectionDistance == Other.ProjectionDistance &&
				bSplitAtNormalSeams == Other.bSplitAtNormalSeams &&
				bSplitAtUVSeams == Other.bSplitAtUVSeams);
		}
	};
	FBakeSettings CachedBakeSettings;

	//FNormalMapSettings CachedNormalMapSettings;
	EBakeOpState UpdateResult_Normal();

	FOcclusionMapSettings CachedOcclusionMapSettings;
	EBakeOpState UpdateResult_Occlusion();

	FCurvatureMapSettings CachedCurvatureMapSettings;
	EBakeOpState UpdateResult_Curvature();

	//FMeshPropertyMapSettings CachedMeshPropertyMapSettings;
	EBakeOpState UpdateResult_MeshProperty();

	TSharedPtr<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe> CachedTextureImage;
	FTexture2DImageSettings CachedTexture2DImageSettings;
	EBakeOpState UpdateResult_Texture2DImage();

	TArray<TSharedPtr<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe>> CachedMultiTextures;
	EBakeOpState UpdateResult_MultiTexture();

	//
	// Analytics
	//
	struct FBakeAnalytics
	{
		double TotalBakeDuration = 0.0;

		struct FMeshSettings
		{
			int32 NumTargetMeshVerts = 0;
			int32 NumTargetMeshTris = 0;
			int32 NumDetailMesh = 0;
			int64 NumDetailMeshTris = 0;
		};
		FMeshSettings MeshSettings;

		FBakeSettings BakeSettings;
		FOcclusionMapSettings OcclusionSettings;
		FCurvatureMapSettings CurvatureSettings;
	};
	FBakeAnalytics BakeAnalytics;

	/**
	 * Computes the NumTargetMeshTris, NumDetailMesh and NumDetailMeshTris analytics.
	 * @param Data the mesh analytics data to compute
	 */
	virtual void GatherAnalytics(FBakeAnalytics::FMeshSettings& Data);

	/**
	 * Records bake timing and settings data for analytics.
	 * @param Result the result of the bake.
	 * @param Settings The bake settings used for the bake.
	 * @param Data the output bake analytics struct.
	 */
	static void GatherAnalytics(const UE::Geometry::FMeshVertexBaker& Result,
								const FBakeSettings& Settings,
								FBakeAnalytics& Data);

	/**
	 * Outputs an analytics event using the given analytics struct.
	 * @param Data the bake analytics struct to output.
	 * @param EventName the name of the analytics event to output.
	 */
	static void RecordAnalytics(const FBakeAnalytics& Data, const FString& EventName);
};

