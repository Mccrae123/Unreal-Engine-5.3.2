// Copyright Epic Games, Inc. All Rights Reserved.

#include "BakeMeshAttributeMapsTool.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshTransforms.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Sampling/MeshNormalMapEvaluator.h"
#include "Sampling/MeshOcclusionMapEvaluator.h"
#include "Sampling/MeshCurvatureMapEvaluator.h"
#include "Sampling/MeshPropertyMapEvaluator.h"
#include "Sampling/MeshResampleImageEvaluator.h"

#include "ImageUtils.h"

#include "AssetUtils/Texture2DBuilder.h"
#include "AssetUtils/Texture2DUtil.h"
#include "ModelingObjectsCreationAPI.h"

#include "TargetInterfaces/MaterialProvider.h"
#include "TargetInterfaces/MeshDescriptionProvider.h"
#include "TargetInterfaces/PrimitiveComponentBackedTarget.h"
#include "TargetInterfaces/StaticMeshBackedTarget.h"
#include "ToolTargetManager.h"
#include "ModelingToolTargetUtil.h"

// required to pass UStaticMesh asset so we can save at same location
#include "Engine/Classes/Components/StaticMeshComponent.h"
#include "Engine/Classes/Engine/StaticMesh.h"

#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UBakeMeshAttributeMapsTool"


/*
 * ToolBuilder
 */

const FToolTargetTypeRequirements& UBakeMeshAttributeMapsToolBuilder::GetTargetRequirements() const
{
	static FToolTargetTypeRequirements TypeRequirements({
		UMeshDescriptionProvider::StaticClass(),
		UPrimitiveComponentBackedTarget::StaticClass(),
		UStaticMeshBackedTarget::StaticClass(),			// currently only supports StaticMesh targets
		UMaterialProvider::StaticClass()
		});
	return TypeRequirements;
}

bool UBakeMeshAttributeMapsToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	const int32 NumTargets = SceneState.TargetManager->CountSelectedAndTargetable(SceneState, GetTargetRequirements());
	return (NumTargets == 1 || NumTargets == 2);
}

UInteractiveTool* UBakeMeshAttributeMapsToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UBakeMeshAttributeMapsTool* NewTool = NewObject<UBakeMeshAttributeMapsTool>(SceneState.ToolManager);

	TArray<TObjectPtr<UToolTarget>> Targets = SceneState.TargetManager->BuildAllSelectedTargetable(SceneState, GetTargetRequirements());
	NewTool->SetTargets(MoveTemp(Targets));
	NewTool->SetWorld(SceneState.World);
	return NewTool;
}




TArray<FString> UBakeMeshAttributeMapsToolProperties::GetUVLayerNamesFunc()
{
	return UVLayerNamesList;
}


/*
 * Operators
 */

class FMeshMapBakerOp : public TGenericDataOperator<FMeshMapBaker>
{
public:
	using ImagePtr = TSharedPtr<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe>;
	
	// General bake settings
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> DetailMesh;
	TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3, ESPMode::ThreadSafe> DetailSpatial;
	TSharedPtr<UE::Geometry::TMeshTangents<double>, ESPMode::ThreadSafe> DetailMeshTangents;
	UE::Geometry::FDynamicMesh3* BaseMesh;
	TUniquePtr<UE::Geometry::FMeshMapBaker> Baker;
	UBakeMeshAttributeMapsTool::FBakeCacheSettings BakeCacheSettings;
	TSharedPtr<UE::Geometry::TMeshTangents<double>, ESPMode::ThreadSafe> BaseMeshTangents;

	// Map Type settings
	EBakeMapType Maps;
	FNormalMapSettings NormalSettings;
	FOcclusionMapSettings OcclusionSettings;
	FCurvatureMapSettings CurvatureSettings;
	FMeshPropertyMapSettings PropertySettings;
	FTexture2DImageSettings TextureSettings;

	// NormalMap settings
	ImagePtr DetailMeshNormalMap;
	int32 DetailMeshNormalUVLayer = 0;

	// Texture2DImage & MultiTexture settings
	ImagePtr TextureImage;
	TMap<int32, ImagePtr> MaterialToTextureImageMap;

	// Begin TGenericDataOperator interface
	virtual void CalculateResult(FProgressCancel* Progress) override
	{
		Baker = MakeUnique<FMeshMapBaker>();
		Baker->CancelF = [Progress]() {
			return Progress && Progress->Cancelled();
		};
		Baker->SetTargetMesh(BaseMesh);
		Baker->SetDimensions(BakeCacheSettings.Dimensions);
		Baker->SetUVLayer(BakeCacheSettings.UVLayer);
		Baker->SetThickness(BakeCacheSettings.Thickness);
		Baker->SetMultisampling(BakeCacheSettings.Multisampling);
		Baker->SetTargetMeshTangents(BaseMeshTangents);
		
		FMeshBakerDynamicMeshSampler DetailSampler(DetailMesh.Get(), DetailSpatial.Get(), DetailMeshTangents.Get());
		Baker->SetDetailSampler(&DetailSampler);

		for (const EBakeMapType MapType : ALL_BAKE_MAP_TYPES)
		{
			switch (BakeCacheSettings.BakeMapTypes & MapType)
			{
			case EBakeMapType::TangentSpaceNormalMap:
			{
				TSharedPtr<FMeshNormalMapEvaluator, ESPMode::ThreadSafe> NormalEval = MakeShared<FMeshNormalMapEvaluator, ESPMode::ThreadSafe>();
				DetailSampler.SetNormalMap(DetailMesh.Get(), IMeshBakerDetailSampler::FBakeDetailTexture(DetailMeshNormalMap.Get(), DetailMeshNormalUVLayer));
				Baker->AddEvaluator(NormalEval);
				break;
			}
			case EBakeMapType::AmbientOcclusion:
			case EBakeMapType::BentNormal:
			case EBakeMapType::Occlusion:
			{
				TSharedPtr<FMeshOcclusionMapEvaluator> OcclusionEval = MakeShared<FMeshOcclusionMapEvaluator>();
				OcclusionEval->OcclusionType = EMeshOcclusionMapType::None;
				if ((bool)(BakeCacheSettings.BakeMapTypes & EBakeMapType::AmbientOcclusion))
				{
					OcclusionEval->OcclusionType |= EMeshOcclusionMapType::AmbientOcclusion;
				}
				if ((bool)(BakeCacheSettings.BakeMapTypes & EBakeMapType::BentNormal))
				{
					OcclusionEval->OcclusionType |= EMeshOcclusionMapType::BentNormal;
				}
				OcclusionEval->NumOcclusionRays = OcclusionSettings.OcclusionRays;
				OcclusionEval->MaxDistance = OcclusionSettings.MaxDistance;
				OcclusionEval->SpreadAngle = OcclusionSettings.SpreadAngle;
				OcclusionEval->BiasAngleDeg = OcclusionSettings.BiasAngle;

				switch (OcclusionSettings.Distribution)
				{
				case EOcclusionMapDistribution::Cosine:
					OcclusionEval->Distribution = FMeshOcclusionMapEvaluator::EDistribution::Cosine;
					break;
				case EOcclusionMapDistribution::Uniform:
					OcclusionEval->Distribution = FMeshOcclusionMapEvaluator::EDistribution::Uniform;
					break;
				}

				switch (OcclusionSettings.NormalSpace)
				{
				case ENormalMapSpace::Tangent:
					OcclusionEval->NormalSpace = FMeshOcclusionMapEvaluator::ESpace::Tangent;
					break;
				case ENormalMapSpace::Object:
					OcclusionEval->NormalSpace = FMeshOcclusionMapEvaluator::ESpace::Object;
					break;
				}
				Baker->AddEvaluator(OcclusionEval);
				break;
			}
			case EBakeMapType::Curvature:
			{
				TSharedPtr<FMeshCurvatureMapEvaluator, ESPMode::ThreadSafe> CurvatureEval = MakeShared<FMeshCurvatureMapEvaluator, ESPMode::ThreadSafe>();
				CurvatureEval->RangeScale = FMathd::Clamp(CurvatureSettings.RangeMultiplier, 0.0001, 1000.0);
				CurvatureEval->MinRangeScale = FMathd::Clamp(CurvatureSettings.MinRangeMultiplier, 0.0, 1.0);
				CurvatureEval->UseCurvatureType = (FMeshCurvatureMapEvaluator::ECurvatureType)CurvatureSettings.CurvatureType;
				CurvatureEval->UseColorMode = (FMeshCurvatureMapEvaluator::EColorMode)CurvatureSettings.ColorMode;
				CurvatureEval->UseClampMode = (FMeshCurvatureMapEvaluator::EClampMode)CurvatureSettings.ClampMode;
				Baker->AddEvaluator(CurvatureEval);
				break;
			}
			case EBakeMapType::NormalImage:
			{
				TSharedPtr<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe> PropertyEval = MakeShared<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe>();
				PropertyEval->Property = EMeshPropertyMapType::Normal;
				DetailSampler.SetNormalMap(DetailMesh.Get(), IMeshBakerDetailSampler::FBakeDetailTexture(DetailMeshNormalMap.Get(), DetailMeshNormalUVLayer));
				Baker->AddEvaluator(PropertyEval);
				break;
			}
			case EBakeMapType::FaceNormalImage:
			{
				TSharedPtr<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe> PropertyEval = MakeShared<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe>();
				PropertyEval->Property = EMeshPropertyMapType::FacetNormal;
				Baker->AddEvaluator(PropertyEval);
				break;
			}
			case EBakeMapType::PositionImage:
			{
				TSharedPtr<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe> PropertyEval = MakeShared<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe>();
				PropertyEval->Property = EMeshPropertyMapType::Position;
				Baker->AddEvaluator(PropertyEval);
				break;
			}
			case EBakeMapType::MaterialID:
			{
				TSharedPtr<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe> PropertyEval = MakeShared<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe>();
				PropertyEval->Property = EMeshPropertyMapType::MaterialID;
				Baker->AddEvaluator(PropertyEval);
				break;
			}
			case EBakeMapType::VertexColorImage:
			{
				TSharedPtr<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe> PropertyEval = MakeShared<FMeshPropertyMapEvaluator, ESPMode::ThreadSafe>();
				PropertyEval->Property = EMeshPropertyMapType::VertexColor;
				Baker->AddEvaluator(PropertyEval);
				break;
			}
			case EBakeMapType::Texture2DImage:
			{
				TSharedPtr<FMeshResampleImageEvaluator, ESPMode::ThreadSafe> TextureEval = MakeShared<FMeshResampleImageEvaluator, ESPMode::ThreadSafe>();
				DetailSampler.SetColorMap(DetailMesh.Get(), IMeshBakerDetailSampler::FBakeDetailTexture(TextureImage.Get(), TextureSettings.UVLayer));
				Baker->AddEvaluator(TextureEval);
				break;
			}
			case EBakeMapType::MultiTexture:
			{
				TSharedPtr<FMeshMultiResampleImageEvaluator, ESPMode::ThreadSafe> TextureEval = MakeShared<FMeshMultiResampleImageEvaluator, ESPMode::ThreadSafe>();
				TextureEval->DetailUVLayer = TextureSettings.UVLayer;
				TextureEval->MultiTextures = MaterialToTextureImageMap;
				Baker->AddEvaluator(TextureEval);
				break;
			}
			default:
				break;
			}
		}
		Baker->Bake();
		SetResult(MoveTemp(Baker));
	}
	// End TGenericDataOperator interface
};

/*
 * Tool
 */

void UBakeMeshAttributeMapsTool::Setup()
{
	Super::Setup();

	// Initialize preview mesh
	bIsBakeToSelf = (Targets.Num() == 1);

	PreviewMesh->ProcessMesh([this](const FDynamicMesh3& Mesh)
	{
		BaseMesh.Copy(Mesh);
		BaseSpatial.SetMesh(&BaseMesh, true);
		BaseMeshTangents = MakeShared<FMeshTangentsd, ESPMode::ThreadSafe>(&BaseMesh);
		BaseMeshTangents->CopyTriVertexTangents(Mesh);
	});

	// Setup tool property sets
	Settings = NewObject<UBakeMeshAttributeMapsToolProperties>(this);
	Settings->RestoreProperties(this);
	Settings->UVLayerNamesList.Reset();
	int32 FoundIndex = -1;
	for (int32 k = 0; k < BaseMesh.Attributes()->NumUVLayers(); ++k)
	{
		Settings->UVLayerNamesList.Add(FString::FromInt(k));
		if (Settings->UVLayer == Settings->UVLayerNamesList.Last())
		{
			FoundIndex = k;
		}
	}
	if (FoundIndex == -1)
	{
		Settings->UVLayer = Settings->UVLayerNamesList[0];
	}
	AddToolPropertySource(Settings);

	Settings->WatchProperty(Settings->MapTypes, [this](int32) { bInputsDirty = true; UpdateOnModeChange(); });
	Settings->WatchProperty(Settings->MapPreview, [this](int32) { UpdateVisualization(); GetToolManager()->PostInvalidation(); });
	Settings->WatchProperty(Settings->Resolution, [this](EBakeTextureResolution) { bInputsDirty = true; });
	Settings->WatchProperty(Settings->UVLayer, [this](FString) { bInputsDirty = true; });
	Settings->WatchProperty(Settings->bUseWorldSpace, [this](bool) { bDetailMeshValid = false; bInputsDirty = true; });
	Settings->WatchProperty(Settings->Thickness, [this](float) { bInputsDirty = true; });
	Settings->WatchProperty(Settings->Multisampling, [this](EBakeMultisampling) { bInputsDirty = true; });


	UToolTarget* DetailTarget = Targets[bIsBakeToSelf ? 0 : 1];
	IStaticMeshBackedTarget* DetailStaticMeshTarget = Cast<IStaticMeshBackedTarget>(DetailTarget);
	UStaticMesh* DetailStaticMesh = DetailStaticMeshTarget ? DetailStaticMeshTarget->GetStaticMesh() : nullptr;

	DetailMeshProps = NewObject<UDetailMeshToolProperties>(this);
	AddToolPropertySource(DetailMeshProps);
	SetToolPropertySourceEnabled(DetailMeshProps, true);
	DetailMeshProps->DetailMesh = DetailStaticMesh;
	DetailMeshProps->DetailMeshNormalMap = nullptr;
	DetailMeshProps->WatchProperty(DetailMeshProps->DetailNormalUVLayer, [this](int) { bInputsDirty = true; });
	DetailMeshProps->WatchProperty(DetailMeshProps->DetailMeshNormalMap, [this](UTexture2D*)
	{
		// Only invalidate detail mesh if we need to recompute tangents.
		if (!DetailMeshTangents)
		{
			bDetailMeshValid = false;
		}
		bInputsDirty = true;
	});
	

	NormalMapProps = NewObject<UBakedNormalMapToolProperties>(this);
	NormalMapProps->RestoreProperties(this);
	AddToolPropertySource(NormalMapProps);
	SetToolPropertySourceEnabled(NormalMapProps, false);


	OcclusionMapProps = NewObject<UBakedOcclusionMapToolProperties>(this);
	OcclusionMapProps->RestoreProperties(this);
	AddToolPropertySource(OcclusionMapProps);
	SetToolPropertySourceEnabled(OcclusionMapProps, false);
	OcclusionMapProps->WatchProperty(OcclusionMapProps->OcclusionRays, [this](int32) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->MaxDistance, [this](float) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->SpreadAngle, [this](float) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->Distribution, [this](EOcclusionMapDistribution) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->BlurRadius, [this](float) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->bGaussianBlur, [this](float) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->BiasAngle, [this](float) { bInputsDirty = true; });
	OcclusionMapProps->WatchProperty(OcclusionMapProps->NormalSpace, [this](ENormalMapSpace) { bInputsDirty = true; });


	CurvatureMapProps = NewObject<UBakedCurvatureMapToolProperties>(this);
	CurvatureMapProps->RestoreProperties(this);
	AddToolPropertySource(CurvatureMapProps);
	SetToolPropertySourceEnabled(CurvatureMapProps, false);
	CurvatureMapProps->WatchProperty(CurvatureMapProps->RangeMultiplier, [this](float) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->MinRangeMultiplier, [this](float) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->CurvatureType, [this](EBakedCurvatureTypeMode) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->ColorMode, [this](EBakedCurvatureColorMode) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->Clamping, [this](EBakedCurvatureClampMode) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->BlurRadius, [this](float) { bInputsDirty = true; });
	CurvatureMapProps->WatchProperty(CurvatureMapProps->bGaussianBlur, [this](float) { bInputsDirty = true; });


	Texture2DProps = NewObject<UBakedTexture2DImageProperties>(this);
	Texture2DProps->RestoreProperties(this);
	AddToolPropertySource(Texture2DProps);
	SetToolPropertySourceEnabled(Texture2DProps, false);
	Texture2DProps->WatchProperty(Texture2DProps->UVLayer, [this](float) { bInputsDirty = true; });
	Texture2DProps->WatchProperty(Texture2DProps->SourceTexture, [this](UTexture2D*) { bInputsDirty = true; });

	MultiTextureProps = NewObject<UBakedMultiTexture2DImageProperties>(this);
	MultiTextureProps->RestoreProperties(this);
	AddToolPropertySource(MultiTextureProps);
	SetToolPropertySourceEnabled(MultiTextureProps, false);

	auto SetDirtyCallback = [this](decltype(MultiTextureProps->MaterialIDSourceTextureMap)) { bInputsDirty = true; };
	auto NotEqualsCallback = [](const decltype(MultiTextureProps->MaterialIDSourceTextureMap)& A, const decltype(MultiTextureProps->MaterialIDSourceTextureMap)& B) -> bool { return !(A.OrderIndependentCompareEqual(B)); };
	MultiTextureProps->WatchProperty(MultiTextureProps->MaterialIDSourceTextureMap, SetDirtyCallback, NotEqualsCallback);
	MultiTextureProps->WatchProperty(MultiTextureProps->UVLayer, [this](float) { bInputsDirty = true; });

	UpdateOnModeChange();

	bInputsDirty = true;
	bDetailMeshValid = false;

	SetToolDisplayName(LOCTEXT("ToolName", "Bake Textures"));
	GetToolManager()->DisplayMessage(
		LOCTEXT("OnStartTool", "Bake Maps. Select Bake Mesh (LowPoly) first, then (optionally) Detail Mesh second. Texture Assets will be created on Accept. "),
		EToolMessageLevel::UserNotification);

	SetupBaseToolProperties();
}




bool UBakeMeshAttributeMapsTool::CanAccept() const
{
	bool bCanAccept = Compute ? Compute->HaveValidResult() && Settings->MapTypes != (int) EBakeMapType::None : false;
	if (bCanAccept)
	{
		// Allow Accept if all non-None types have valid results.
		const int NumResults = Settings->Result.Num();
		for (int ResultIdx = 0; ResultIdx < NumResults; ++ResultIdx)
		{
			bCanAccept = bCanAccept && Settings->Result[ResultIdx];
		}
	}
	return bCanAccept;
}


TUniquePtr<UE::Geometry::TGenericDataOperator<FMeshMapBaker>> UBakeMeshAttributeMapsTool::MakeNewOperator()
{
	TUniquePtr<FMeshMapBakerOp> Op = MakeUnique<FMeshMapBakerOp>();
	Op->DetailMesh = DetailMesh;
	Op->DetailSpatial = DetailSpatial;
	Op->BaseMesh = &BaseMesh;
	Op->BakeCacheSettings = CachedBakeCacheSettings;

	constexpr EBakeMapType RequiresTangents = EBakeMapType::TangentSpaceNormalMap | EBakeMapType::BentNormal;
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & RequiresTangents))
	{
		Op->BaseMeshTangents = BaseMeshTangents;
	}

	if (CachedDetailNormalMap)
	{
		Op->DetailMeshTangents = DetailMeshTangents;
		Op->DetailMeshNormalMap = CachedDetailNormalMap;
		Op->DetailMeshNormalUVLayer = CachedDetailMeshSettings.UVLayer;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::TangentSpaceNormalMap))
	{
		Op->NormalSettings = CachedNormalMapSettings;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::AmbientOcclusion) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::BentNormal))
	{
		Op->OcclusionSettings = CachedOcclusionMapSettings;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::Curvature))
	{
		Op->CurvatureSettings = CachedCurvatureMapSettings;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::NormalImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::FaceNormalImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::PositionImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::MaterialID) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::VertexColorImage))
	{
		Op->PropertySettings = CachedMeshPropertyMapSettings;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::Texture2DImage))
	{
		Op->TextureSettings = CachedTexture2DImageSettings;
		Op->TextureImage = CachedTextureImage;
	}

	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::MultiTexture))
	{
		Op->TextureSettings = CachedTexture2DImageSettings;
		Op->MaterialToTextureImageMap = CachedMultiTextures;
	}

	return Op;
}


void UBakeMeshAttributeMapsTool::Shutdown(EToolShutdownType ShutdownType)
{
	Super::Shutdown(ShutdownType);
	
	Settings->SaveProperties(this);
	OcclusionMapProps->SaveProperties(this);
	NormalMapProps->SaveProperties(this);
	CurvatureMapProps->SaveProperties(this);
	Texture2DProps->SaveProperties(this);
	MultiTextureProps->SaveProperties(this);

	if (Compute)
	{
		Compute->Shutdown();
	}
	if (ShutdownType == EToolShutdownType::Accept)
	{
		UStaticMeshComponent* StaticMeshComponent = CastChecked<UStaticMeshComponent>(UE::ToolTarget::GetTargetComponent(Targets[0]));
		UStaticMesh* StaticMeshAsset = StaticMeshComponent->GetStaticMesh();
		check(StaticMeshAsset);
		FString BaseName = UE::ToolTarget::GetTargetActor(Targets[0])->GetName();

		bool bCreatedAssetOK = true;
		const int NumResults = Settings->Result.Num();
		for (int ResultIdx = 0; ResultIdx < NumResults; ++ResultIdx)
		{
			const FTexture2DBuilder::ETextureType TexType = GetTextureType(ResultTypes[ResultIdx]);
			FString TexName;
			GetTextureName(ResultTypes[ResultIdx], BaseName, TexName);
			FTexture2DBuilder::CopyPlatformDataToSourceData(Settings->Result[ResultIdx], TexType);
			bCreatedAssetOK = bCreatedAssetOK && UE::Modeling::CreateTextureObject(GetToolManager(), FCreateTextureObjectParams{ 0, StaticMeshAsset->GetWorld(), StaticMeshAsset, TexName, Settings->Result[ResultIdx] }).IsOK();
		}
		ensure(bCreatedAssetOK);
	}
}


void UBakeMeshAttributeMapsTool::UpdateDetailMesh()
{
	UToolTarget* DetailTarget = Targets[bIsBakeToSelf ? 0 : 1];

	const bool bWantMeshTangents = (DetailMeshProps->DetailMeshNormalMap != nullptr);
	DetailMesh = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(UE::ToolTarget::GetDynamicMeshCopy(DetailTarget, bWantMeshTangents));

	if (Settings->bUseWorldSpace && bIsBakeToSelf == false)
	{
		using FTransform3d = UE::Geometry::FTransform3d;
		const FTransform3d DetailToWorld = UE::ToolTarget::GetLocalToWorldTransform(DetailTarget);
		MeshTransforms::ApplyTransform(*DetailMesh, DetailToWorld);
		const FTransform3d WorldToBase = UE::ToolTarget::GetLocalToWorldTransform(Targets[0]);
		MeshTransforms::ApplyTransform(*DetailMesh, WorldToBase.Inverse());
	}
	
	DetailSpatial = MakeShared<FDynamicMeshAABBTree3, ESPMode::ThreadSafe>();
	DetailSpatial->SetMesh(DetailMesh.Get(), true);

	// Extract tangents if a DetailMesh normal map was provided.
	if (bWantMeshTangents)
	{
		DetailMeshTangents = MakeShared<FMeshTangentsd, ESPMode::ThreadSafe>(DetailMesh.Get());
		DetailMeshTangents->CopyTriVertexTangents(*DetailMesh);
	}
	else
	{
		DetailMeshTangents = nullptr;
	}

	ProcessComponentTextures(UE::ToolTarget::GetTargetComponent(DetailTarget), [this](const int MaterialID, const TArray<UTexture*>& Textures)
	{
		for (UTexture* Tex : Textures)
		{
			UTexture2D* Tex2D = Cast<UTexture2D>(Tex);
			if (Tex2D)
			{
				MultiTextureProps->AllSourceTextures.Add(Tex2D);
			}
		}

		constexpr bool bGuessAtTextures = true;
		if (bGuessAtTextures)
		{
			const int SelectedTextureIndex = SelectColorTextureToBake(Textures);
			if (SelectedTextureIndex >= 0)
			{
				UTexture2D* Tex2D = Cast<UTexture2D>(Textures[SelectedTextureIndex]);

				// if cast fails, this will set the value to nullptr, which is fine
				MultiTextureProps->MaterialIDSourceTextureMap.Add(MaterialID, Tex2D);	
			}
		}
		else
		{
			MultiTextureProps->MaterialIDSourceTextureMap.Add(MaterialID, nullptr);
		}
	});

	bInputsDirty = true;
	DetailMeshTimestamp++;
}


void UBakeMeshAttributeMapsTool::UpdateResult()
{
	if (bDetailMeshValid == false)
	{
		UpdateDetailMesh();
		bDetailMeshValid = true;
		CachedBakeCacheSettings = FBakeCacheSettings();
	}

	// bInputsDirty ensures that we only validate parameters once per param
	// change. Parameter validation can be expensive (ex. UpdateResult_Texture2DImage).
	if (!bInputsDirty)
	{
		return;
	}

	// clear warning (ugh)
	GetToolManager()->DisplayMessage(FText(), EToolMessageLevel::UserWarning);

	int32 ImageSize = (int32)Settings->Resolution;
	FImageDimensions Dimensions(ImageSize, ImageSize);

	FBakeCacheSettings BakeCacheSettings;
	BakeCacheSettings.Dimensions = Dimensions;
	BakeCacheSettings.UVLayer = FCString::Atoi(*Settings->UVLayer);
	BakeCacheSettings.DetailTimestamp = this->DetailMeshTimestamp;
	BakeCacheSettings.Thickness = Settings->Thickness;
	BakeCacheSettings.Multisampling = (int32)Settings->Multisampling;

	// process the raw bitfield before caching which may add additional targets.
	BakeCacheSettings.BakeMapTypes = GetMapTypes(Settings->MapTypes);

	// update bake cache settings
	if (!(CachedBakeCacheSettings == BakeCacheSettings))
	{
		CachedBakeCacheSettings = BakeCacheSettings;

		CachedNormalMapSettings = FNormalMapSettings();
		CachedOcclusionMapSettings = FOcclusionMapSettings();
		CachedCurvatureMapSettings = FCurvatureMapSettings();
		CachedMeshPropertyMapSettings = FMeshPropertyMapSettings();
		CachedTexture2DImageSettings = FTexture2DImageSettings();
	}

	// Clear our invalid bitflag to check again for valid inputs.
	OpState = EBakeOpState::Evaluate;

	OpState |= UpdateResult_DetailNormalMap();

	// Update map type settings
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::TangentSpaceNormalMap))
	{
		OpState |= UpdateResult_Normal();
	}
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::AmbientOcclusion) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::BentNormal))
	{
		OpState |= UpdateResult_Occlusion();
	}
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::Curvature))
	{
		OpState |= UpdateResult_Curvature();
	}
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::NormalImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::FaceNormalImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::PositionImage) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::MaterialID) ||
		(bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::VertexColorImage))
	{
		OpState |= UpdateResult_MeshProperty();
	}
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::Texture2DImage))
	{
		OpState |= UpdateResult_Texture2DImage();
	}
	if ((bool)(CachedBakeCacheSettings.BakeMapTypes & EBakeMapType::MultiTexture))
	{
		OpState |= UpdateResult_MultiTexture();
	}

	// Early exit if op input parameters are invalid.
	if ((bool)(OpState & EBakeOpState::Invalid))
	{
		return;
	}

	// This should be the only point of compute invalidation to
	// minimize synchronization issues.
	bool bInvalidate = bInputsDirty || (bool)(OpState & EBakeOpState::Evaluate);
	if (!Compute)
	{
		Compute = MakeUnique<TGenericDataBackgroundCompute<FMeshMapBaker>>();
		Compute->Setup(this);
		Compute->OnResultUpdated.AddLambda([this](const TUniquePtr<FMeshMapBaker>& NewResult) { OnMapsUpdated(NewResult); });
		Compute->InvalidateResult();
	}
	else if (bInvalidate)
	{
		Compute->InvalidateResult();
	}
	bInputsDirty = false;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_DetailNormalMap()
{
	EBakeOpState ResultState = EBakeOpState::Complete;
	
	const FDynamicMeshUVOverlay* UVOverlay = DetailMesh->Attributes()->GetUVLayer(DetailMeshProps->DetailNormalUVLayer);
	if (UVOverlay == nullptr)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("InvalidUVWarning", "The Detail Mesh does not have the selected UV layer"), EToolMessageLevel::UserWarning);
		return EBakeOpState::Invalid;
	}
	
	UTexture2D* DetailMeshNormalMap = DetailMeshProps->DetailMeshNormalMap; 
	if (DetailMeshNormalMap)
	{
		CachedDetailNormalMap = MakeShared<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe>();
		if (!UE::AssetUtils::ReadTexture(DetailMeshNormalMap, *CachedDetailNormalMap, bPreferPlatformData))
		{
			// Report the failed texture read as a warning, but permit the bake to continue.
			GetToolManager()->DisplayMessage(LOCTEXT("CannotReadTextureWarning", "Cannot read from the detail normal map texture"), EToolMessageLevel::UserWarning);
		}
	}
	else
	{
		CachedDetailNormalMap = nullptr;
	}

	FDetailMeshSettings DetailMeshSettings;
	DetailMeshSettings.UVLayer = DetailMeshProps->DetailNormalUVLayer;

	if (!(CachedDetailMeshSettings == DetailMeshSettings))
	{
		CachedDetailMeshSettings = DetailMeshSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_Normal()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FNormalMapSettings NormalMapSettings;
	NormalMapSettings.Dimensions = Dimensions;

	if (!(CachedNormalMapSettings == NormalMapSettings))
	{
		CachedNormalMapSettings = NormalMapSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_Occlusion()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FOcclusionMapSettings OcclusionMapSettings;
	OcclusionMapSettings.Dimensions = Dimensions;
	OcclusionMapSettings.MaxDistance = (OcclusionMapProps->MaxDistance == 0) ? TNumericLimits<float>::Max() : OcclusionMapProps->MaxDistance;
	OcclusionMapSettings.OcclusionRays = OcclusionMapProps->OcclusionRays;
	OcclusionMapSettings.SpreadAngle = OcclusionMapProps->SpreadAngle;
	OcclusionMapSettings.Distribution = OcclusionMapProps->Distribution;
	OcclusionMapSettings.BlurRadius = (OcclusionMapProps->bGaussianBlur) ? OcclusionMapProps->BlurRadius : 0.0;
	OcclusionMapSettings.BiasAngle = OcclusionMapProps->BiasAngle;
	OcclusionMapSettings.NormalSpace = OcclusionMapProps->NormalSpace;

	if ( !(CachedOcclusionMapSettings == OcclusionMapSettings) )
	{
		CachedOcclusionMapSettings = OcclusionMapSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_Curvature()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FCurvatureMapSettings CurvatureMapSettings;
	CurvatureMapSettings.Dimensions = Dimensions;
	CurvatureMapSettings.RangeMultiplier = CurvatureMapProps->RangeMultiplier;
	CurvatureMapSettings.MinRangeMultiplier = CurvatureMapProps->MinRangeMultiplier;
	switch (CurvatureMapProps->CurvatureType)
	{
	default:
	case EBakedCurvatureTypeMode::MeanAverage:
		CurvatureMapSettings.CurvatureType = (int32)FMeshCurvatureMapEvaluator::ECurvatureType::Mean;
		break;
	case EBakedCurvatureTypeMode::Gaussian:
		CurvatureMapSettings.CurvatureType = (int32)FMeshCurvatureMapEvaluator::ECurvatureType::Gaussian;
		break;
	case EBakedCurvatureTypeMode::Max:
		CurvatureMapSettings.CurvatureType = (int32)FMeshCurvatureMapEvaluator::ECurvatureType::MaxPrincipal;
		break;
	case EBakedCurvatureTypeMode::Min:
		CurvatureMapSettings.CurvatureType = (int32)FMeshCurvatureMapEvaluator::ECurvatureType::MinPrincipal;
		break;
	}
	switch (CurvatureMapProps->ColorMode)
	{
	default:
	case EBakedCurvatureColorMode::Grayscale:
		CurvatureMapSettings.ColorMode = (int32)FMeshCurvatureMapEvaluator::EColorMode::BlackGrayWhite;
		break;
	case EBakedCurvatureColorMode::RedBlue:
		CurvatureMapSettings.ColorMode = (int32)FMeshCurvatureMapEvaluator::EColorMode::RedBlue;
		break;
	case EBakedCurvatureColorMode::RedGreenBlue:
		CurvatureMapSettings.ColorMode = (int32)FMeshCurvatureMapEvaluator::EColorMode::RedGreenBlue;
		break;
	}
	switch (CurvatureMapProps->Clamping)
	{
	default:
	case EBakedCurvatureClampMode::None:
		CurvatureMapSettings.ClampMode = (int32)FMeshCurvatureMapEvaluator::EClampMode::FullRange;
		break;
	case EBakedCurvatureClampMode::Positive:
		CurvatureMapSettings.ClampMode = (int32)FMeshCurvatureMapEvaluator::EClampMode::Positive;
		break;
	case EBakedCurvatureClampMode::Negative:
		CurvatureMapSettings.ClampMode = (int32)FMeshCurvatureMapEvaluator::EClampMode::Negative;
		break;
	}
	CurvatureMapSettings.BlurRadius = (CurvatureMapProps->bGaussianBlur) ? CurvatureMapProps->BlurRadius : 0.0;

	if (!(CachedCurvatureMapSettings == CurvatureMapSettings))
	{
		CachedCurvatureMapSettings = CurvatureMapSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_MeshProperty()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FMeshPropertyMapSettings MeshPropertyMapSettings;
	MeshPropertyMapSettings.Dimensions = Dimensions;

	if (!(CachedMeshPropertyMapSettings == MeshPropertyMapSettings))
	{
		CachedMeshPropertyMapSettings = MeshPropertyMapSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_Texture2DImage()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FTexture2DImageSettings NewSettings;
	NewSettings.Dimensions = Dimensions;
	NewSettings.UVLayer = 0;

	const FDynamicMeshUVOverlay* UVOverlay = DetailMesh->Attributes()->GetUVLayer(NewSettings.UVLayer);
	if (UVOverlay == nullptr)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("InvalidUVWarning", "The Source Mesh does not have the selected UV layer"), EToolMessageLevel::UserWarning);
		return EBakeOpState::Invalid;
	}
	
	if (Texture2DProps->SourceTexture == nullptr)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("InvalidTextureWarning", "The Source Texture is not valid"), EToolMessageLevel::UserWarning);
		return EBakeOpState::Invalid;
	}

	{
		CachedTextureImage = MakeShared<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe>();
		if (!UE::AssetUtils::ReadTexture(Texture2DProps->SourceTexture, *CachedTextureImage, bPreferPlatformData))
		{
			GetToolManager()->DisplayMessage(LOCTEXT("CannotReadTextureWarning", "Cannot read from the source texture"), EToolMessageLevel::UserWarning);
			return EBakeOpState::Invalid;
		}

		// The read texture data is always in linear space.
		NewSettings.bSRGB = false; 
	}

	if (!(CachedTexture2DImageSettings == NewSettings))
	{
		CachedTexture2DImageSettings = NewSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


EBakeOpState UBakeMeshAttributeMapsTool::UpdateResult_MultiTexture()
{
	EBakeOpState ResultState = EBakeOpState::Complete;

	const int32 ImageSize = (int32)Settings->Resolution;
	const FImageDimensions Dimensions(ImageSize, ImageSize);

	FTexture2DImageSettings NewSettings;
	NewSettings.Dimensions = Dimensions;
	NewSettings.UVLayer = MultiTextureProps->UVLayer;

	const FDynamicMeshUVOverlay* UVOverlay = DetailMesh->Attributes()->GetUVLayer(NewSettings.UVLayer);
	if (UVOverlay == nullptr)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("InvalidUVWarning", "The Source Mesh does not have the selected UV layer"), EToolMessageLevel::UserWarning);
		return EBakeOpState::Invalid;
	}

	for (auto& InputTexture : MultiTextureProps->MaterialIDSourceTextureMap)
	{
		if (InputTexture.Value == nullptr)
		{
			GetToolManager()->DisplayMessage(LOCTEXT("InvalidTextureWarning", "The Source Texture is not valid"), EToolMessageLevel::UserWarning);
			return EBakeOpState::Invalid;
		}
	}

	CachedMultiTextures.Reset();

	// The read texture data is always in linear space.
	NewSettings.bSRGB = false;
	
	for ( auto& InputTexture : MultiTextureProps->MaterialIDSourceTextureMap)
	{
		UTexture2D* Texture = InputTexture.Value;
		if (!ensure(Texture != nullptr))
		{
			GetToolManager()->DisplayMessage(LOCTEXT("InvalidTextureWarning", "The Source Texture is not valid"), EToolMessageLevel::UserWarning);
			return EBakeOpState::Invalid;
		}

		int32 MaterialID = InputTexture.Key;
		CachedMultiTextures.Add(MaterialID, MakeShared<UE::Geometry::TImageBuilder<FVector4f>, ESPMode::ThreadSafe>());
		if (!UE::AssetUtils::ReadTexture(Texture, *CachedMultiTextures[MaterialID], bPreferPlatformData))
		{
			GetToolManager()->DisplayMessage(LOCTEXT("CannotReadTextureWarning", "Cannot read from the source texture"), EToolMessageLevel::UserWarning);
			return EBakeOpState::Invalid;
		}
	}
	if (CachedMultiTextures.Num() == 0)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("InvalidTextureWarning", "The Source Texture is not valid"), EToolMessageLevel::UserWarning);
		return EBakeOpState::Invalid;
	}

	if (!(CachedTexture2DImageSettings == NewSettings))
	{
		CachedTexture2DImageSettings = NewSettings;
		ResultState = EBakeOpState::Evaluate;
	}
	return ResultState;
}


void UBakeMeshAttributeMapsTool::UpdateVisualization()
{
	PreviewMesh->SetOverrideRenderMaterial(PreviewMaterial);

	// Map CachedMaps to Settings->Result
	int NumResults = Settings->Result.Num();
	for (int ResultIdx = 0; ResultIdx < NumResults; ResultIdx++)
	{
		Settings->Result[ResultIdx] = CachedMaps[CachedMapIndices[ResultTypes[ResultIdx]]];
	}

	// Set the preview material according to the preview index.
	if (Settings->MapPreview >= 0 && Settings->MapPreview < Settings->Result.Num())
	{
		UpdatePreview(Settings->MapPreview);
	}
}


void UBakeMeshAttributeMapsTool::UpdateOnModeChange()
{
	OnMapTypesUpdated(Settings->MapTypes);
	
	SetToolPropertySourceEnabled(NormalMapProps, false);
	SetToolPropertySourceEnabled(OcclusionMapProps, false);
	SetToolPropertySourceEnabled(CurvatureMapProps, false);
	SetToolPropertySourceEnabled(Texture2DProps, false);
	SetToolPropertySourceEnabled(MultiTextureProps, false);

	for (const EBakeMapType MapType : ALL_BAKE_MAP_TYPES)
	{
		switch ((EBakeMapType)Settings->MapTypes & MapType)
		{
		case EBakeMapType::TangentSpaceNormalMap:
			SetToolPropertySourceEnabled(NormalMapProps, true);
			break;
		case EBakeMapType::AmbientOcclusion:
		case EBakeMapType::BentNormal:
		case EBakeMapType::Occlusion:
			SetToolPropertySourceEnabled(OcclusionMapProps, true);
			break;
		case EBakeMapType::Curvature:
			SetToolPropertySourceEnabled(CurvatureMapProps, true);
			break;
		case EBakeMapType::NormalImage:
		case EBakeMapType::FaceNormalImage:
		case EBakeMapType::PositionImage:
		case EBakeMapType::MaterialID:
		case EBakeMapType::VertexColorImage:
			break;
		case EBakeMapType::Texture2DImage:
			SetToolPropertySourceEnabled(Texture2DProps, true);
			break;
		case EBakeMapType::MultiTexture:
			SetToolPropertySourceEnabled(MultiTextureProps, true);
			break;
		default:
			break;
		}
	}

	Settings->Result.Empty();
	Settings->Result.SetNum(ResultTypes.Num());
}


#undef LOCTEXT_NAMESPACE
