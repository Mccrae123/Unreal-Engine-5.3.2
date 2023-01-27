// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosClothAsset/ClothTransferSkinWeightsTool.h"

#include "ChaosClothAsset/ClothComponent.h"
#include "ChaosClothAsset/ClothComponentToolTarget.h"
#include "ChaosClothAsset/ClothPatternToDynamicMesh.h"
#include "ChaosClothAsset/ClothAdapter.h"
#include "ChaosClothAsset/ClothCollection.h"

#include "BoneWeights.h"
#include "SkeletalMeshAttributes.h"

#include "ToolTargetManager.h"
#include "MeshDescriptionToDynamicMesh.h"

#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshModel.h"

#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicVertexSkinWeightsAttribute.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshTransforms.h"

#include "Operations/TransferBoneWeights.h"

#include "TransformTypes.h"

#include "InteractiveTool.h"
#include "InteractiveToolManager.h"

#include "Engine/SkeletalMesh.h"
#include "PreviewMesh.h"
#include "DynamicMeshEditor.h"
#include "Util/ColorConstants.h"
#include "ToolSetupUtil.h"
#include "ModelingToolTargetUtil.h"
#include "TargetInterfaces/SkeletalMeshBackedTarget.h"
#include "Components/SkeletalMeshComponent.h"

#define LOCTEXT_NAMESPACE "ClothSkinWeightRetargetingTool"


namespace ClothTransferSkinWeightsToolHelpers
{
	/** 
	 * Compute mappings between indices and bone names.
	 * 
     * @note We assume that each mesh inherits its reference skeleton from the same USkeleton asset. However, their  
     * internal indexing can be different and hence when transfering weights we need to make sure we reference the correct  
     * bones via their names instead of indices. 
     */
	void GetBoneMaps(const USkinnedAsset* SourceSkinnedAsset, 
						  const USkinnedAsset* TargetSkinnedAsset, 
						  TMap<FBoneIndexType, FName>& SourceIndexToBone,
						  TMap<FName, FBoneIndexType>& TargetBoneToIndex)
	{
		TargetBoneToIndex.Reset();
		SourceIndexToBone.Reset();
		const FReferenceSkeleton& SourceRefSkeleton = SourceSkinnedAsset->GetRefSkeleton();
		for (int32 Index = 0; Index < SourceRefSkeleton.GetRawBoneNum(); ++Index)
		{
			SourceIndexToBone.Add(Index, SourceRefSkeleton.GetRawRefBoneInfo()[Index].Name);
		}

		const FReferenceSkeleton& TargetRefSkeleton = TargetSkinnedAsset->GetRefSkeleton();
		for (int32 Index = 0; Index < TargetRefSkeleton.GetRawBoneNum(); ++Index)
		{
			TargetBoneToIndex.Add(TargetRefSkeleton.GetRawRefBoneInfo()[Index].Name, Index);
		}
	};

	void SkeletalMeshToDynamicMesh(USkeletalMesh* FromSkeletalMeshAsset, int32 SourceLODIdx, FDynamicMesh3& ToDynamicMesh)
	{
		FMeshDescription SourceMesh;

		// Check first if we have bulk data available and non-empty.
		if (FromSkeletalMeshAsset->IsLODImportedDataBuildAvailable(SourceLODIdx) && !FromSkeletalMeshAsset->IsLODImportedDataEmpty(SourceLODIdx))
		{
			FSkeletalMeshImportData SkeletalMeshImportData;
			FromSkeletalMeshAsset->LoadLODImportedData(SourceLODIdx, SkeletalMeshImportData);
			SkeletalMeshImportData.GetMeshDescription(SourceMesh);
		}
		else
		{
			// Fall back on the LOD model directly if no bulk data exists. When we commit
			// the mesh description, we override using the bulk data. This can happen for older
			// skeletal meshes, from UE 4.24 and earlier.
			const FSkeletalMeshModel* SkeletalMeshModel = FromSkeletalMeshAsset->GetImportedModel();
			if (SkeletalMeshModel && SkeletalMeshModel->LODModels.IsValidIndex(SourceLODIdx))
			{
				SkeletalMeshModel->LODModels[SourceLODIdx].GetMeshDescription(SourceMesh, FromSkeletalMeshAsset);
			}
		}

		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(&SourceMesh, ToDynamicMesh);
	};


	void ClothComponentToDynamicMesh(const UChaosClothComponent* ClothComponent, UE::Geometry::FDynamicMesh3& MeshOut)
	{
		const UChaosClothAsset* ChaosClothAsset = ClothComponent->GetClothAsset();
		if (!ChaosClothAsset)
		{
			return;
		}

		const UE::Chaos::ClothAsset::FClothConstAdapter ClothAdapter(ChaosClothAsset->GetClothCollection());
		constexpr int32 LodIndex = 0;
		const UE::Chaos::ClothAsset::FClothLodConstAdapter ClothLodAdapter = ClothAdapter.GetLod(LodIndex);
		constexpr bool bGet2DPattern = false;

		UE::Geometry::FDynamicMeshEditor MeshEditor(&MeshOut);
		FClothPatternToDynamicMesh Converter;

		for (int32 PatternIndex = 0; PatternIndex < ClothLodAdapter.GetNumPatterns(); ++PatternIndex)
		{
			UE::Geometry::FDynamicMesh3 PatternMesh;
			Converter.Convert(ChaosClothAsset, LodIndex, PatternIndex, bGet2DPattern, PatternMesh);

			UE::Geometry::FMeshIndexMappings IndexMaps;
			MeshEditor.AppendMesh(&PatternMesh, IndexMaps);
		}
	};
}


// ------------------- Properties -------------------

void UClothTransferSkinWeightsToolActionProperties::PostAction(EClothTransferSkinWeightsToolActions Action)
{
	if (ParentTool.IsValid())
	{
		ParentTool->RequestAction(Action);
	}
}


// ------------------- Builder -------------------

const FToolTargetTypeRequirements& UClothTransferSkinWeightsToolBuilder::GetTargetRequirements() const
{
	static FToolTargetTypeRequirements TypeRequirements({ 
		UPrimitiveComponentBackedTarget::StaticClass(),
		UClothAssetBackedTarget::StaticClass()
		});
	return TypeRequirements;
}

bool UClothTransferSkinWeightsToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	const bool ClothComponentSelected = (SceneState.TargetManager->CountSelectedAndTargetable(SceneState, GetTargetRequirements()) == 1);
	
	const static FToolTargetTypeRequirements SourceMeshRequirements(USkeletalMeshBackedTarget::StaticClass());
	const bool SkeletalMeshComponentSelected = (SceneState.TargetManager->CountSelectedAndTargetable(SceneState, SourceMeshRequirements) == 1);

	return ClothComponentSelected && SkeletalMeshComponentSelected;
}

USingleSelectionMeshEditingTool* UClothTransferSkinWeightsToolBuilder::CreateNewTool(const FToolBuilderState& SceneState) const
{
	UClothTransferSkinWeightsTool* NewTool = NewObject<UClothTransferSkinWeightsTool>(SceneState.ToolManager);

	// Setting Target and World on the new tool is handled in USingleSelectionMeshEditingToolBuilder::InitializeNewTool

	return NewTool;
}

void UClothTransferSkinWeightsToolBuilder::PostSetupTool(UInteractiveTool* Tool, const FToolBuilderState& SceneState) const
{
	if (UClothTransferSkinWeightsTool* NewTool = Cast<UClothTransferSkinWeightsTool>(Tool))
	{
		for (UActorComponent* SelectedComponent : SceneState.SelectedComponents)
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(SelectedComponent))
			{
				NewTool->ToolProperties->SourceMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
				NewTool->ToolProperties->SourceMeshTransform = SkeletalMeshComponent->GetComponentTransform();
				NewTool->SourceComponent = SkeletalMeshComponent;
				break;
			}
		}
	}
}

// ------------------- Tool -------------------

void UClothTransferSkinWeightsTool::Setup()
{
	USingleSelectionMeshEditingTool::Setup();

	UClothComponentToolTarget* ClothComponentToolTarget = Cast<UClothComponentToolTarget>(Target);
	ClothComponent = ClothComponentToolTarget->GetClothComponent();
	
	ToolProperties = NewObject<UClothTransferSkinWeightsToolProperties>(this);
	AddToolPropertySource(ToolProperties);

	ActionProperties = NewObject<UClothTransferSkinWeightsToolActionProperties>(this);
	ActionProperties->ParentTool = this;
	AddToolPropertySource(ActionProperties);


	PreviewMesh = NewObject<UPreviewMesh>(this);
	if (PreviewMesh == nullptr)
	{
		return;
	}
	PreviewMesh->CreateInWorld(GetTargetWorld(), FTransform::Identity);
	ToolSetupUtil::ApplyRenderingConfigurationToPreview(PreviewMesh, nullptr);


	PreviewMesh->SetTransform(ClothComponentToolTarget->GetWorldTransform());

	ToolProperties->WatchProperty(ToolProperties->SourceMesh, [this](TObjectPtr<USkeletalMesh>) { UpdatePreviewMesh(); });
	//ToolProperties->WatchProperty(ToolProperties->SourceMeshTransform, [this](const FTransform&) { UpdatePreviewMesh(); });
	ToolProperties->WatchProperty(ToolProperties->BoneName, [this](const FName&) {  UpdatePreviewMeshColor(); });
	ToolProperties->WatchProperty(ToolProperties->bHideSourceMesh, [this](bool) { UpdateSourceMeshRender(); });
}

void UClothTransferSkinWeightsTool::Shutdown(EToolShutdownType ShutdownType)
{
	USingleSelectionMeshEditingTool::Shutdown(ShutdownType);

	if (PreviewMesh)
	{
		PreviewMesh->Disconnect();
	}

	UE::ToolTarget::ShowSourceObject(Target);
	SourceComponent->SetVisibility(true);
}

void UClothTransferSkinWeightsTool::UpdatePreviewMeshColor()
{
	using namespace UE::AnimationCore;
	using namespace UE::Geometry;

	PreviewMesh->SetTriangleColorFunction([this](const FDynamicMesh3* Mesh, int TriangleID) -> FColor
	{
		const FName& CurrentBoneName = ToolProperties->BoneName;
		if (!TargetMeshBoneNameToIndex.Contains(CurrentBoneName))
		{
			return FColor::Black;
		}

		const FBoneIndexType CurrentBoneIndex = TargetMeshBoneNameToIndex[CurrentBoneName];
		const FIndex3i Tri = Mesh->GetTriangle(TriangleID);

		const FName ProfileName = FSkeletalMeshAttributes::DefaultSkinWeightProfileName; // always use default profile for now, later this will be set by the user
		FDynamicMeshVertexSkinWeightsAttribute* Attribute = PreviewMesh->GetPreviewDynamicMesh()->Attributes()->GetSkinWeightsAttribute(ProfileName);
		if (Attribute == nullptr)
		{
			FLinearColor Lin(1.0f, 0.3f, 0.3f, 1.0f);
			return Lin.ToFColor(/*bSRGB = */ true);
		}

		float AvgWeight = 0.0f;
		for (int32 VID = 0; VID < 3; ++VID)
		{
			int32 VertexID = Tri[VID];
			FBoneWeights Data;
			Attribute->GetValue(VertexID, Data);
			for (FBoneWeight Wt : Data)
			{
				if (Wt.GetBoneIndex() == CurrentBoneIndex)
				{
					AvgWeight += Wt.GetWeight();
				}
			}
		}

		AvgWeight /= 3.0f;
		FLinearColor Lin(AvgWeight, AvgWeight, AvgWeight, 1.0f);
		return Lin.ToFColor(/*bSRGB = */ true);
	},
	UPreviewMesh::ERenderUpdateMode::FullUpdate);
}

void UClothTransferSkinWeightsTool::UpdatePreviewMesh()
{
	using namespace UE::AnimationCore;
	using namespace UE::Geometry;
	
	//TODO: for now, assume we are always transfering from LOD 0, but make this a parameter in the future...
	constexpr int32 SourceLODIdx = 0; 

	// User hasn't specified the source mesh in the UI
	if (ToolProperties->SourceMesh == nullptr)
	{
		//TODO: Display error message
		return;
	}

	// Convert source Skeletal Mesh to Dynamic Mesh
	FDynamicMesh3 SourceDynamicMesh;
	USkeletalMesh* FromSkeletalMeshAsset = ToolProperties->SourceMesh;
	ClothTransferSkinWeightsToolHelpers::SkeletalMeshToDynamicMesh(FromSkeletalMeshAsset, SourceLODIdx, SourceDynamicMesh);
	MeshTransforms::ApplyTransform(SourceDynamicMesh, ToolProperties->SourceMeshTransform, true);

	// Convert target ClothComponent to Dynamic Mesh
	UE::Geometry::FDynamicMesh3 TargetDynamicMesh;
	TargetDynamicMesh.EnableAttributes();
	TargetDynamicMesh.Attributes()->AttachSkinWeightsAttribute(FSkeletalMeshAttributes::DefaultSkinWeightProfileName, new FDynamicMeshVertexSkinWeightsAttribute(&TargetDynamicMesh));
	ClothTransferSkinWeightsToolHelpers::ClothComponentToDynamicMesh(ClothComponent, TargetDynamicMesh);


	FTransferBoneWeights TransferBoneWeights(&SourceDynamicMesh, FSkeletalMeshAttributes::DefaultSkinWeightProfileName);

	// Compute bone index mappings
	TMap<FBoneIndexType, FName> SourceIndexToBone;
	ClothTransferSkinWeightsToolHelpers::GetBoneMaps(FromSkeletalMeshAsset, ClothComponent->GetClothAsset(), SourceIndexToBone, this->TargetMeshBoneNameToIndex);
	TransferBoneWeights.SourceIndexToBone = &SourceIndexToBone;
	TransferBoneWeights.TargetBoneToIndex = &this->TargetMeshBoneNameToIndex;

	// Do the actual transfer
	const FTransformSRT3d TargetToWorld = ClothComponent->GetComponentTransform();
	if (TransferBoneWeights.Validate() == EOperationValidationResult::Ok)
	{
		TransferBoneWeights.Compute(TargetDynamicMesh, TargetToWorld, FSkeletalMeshAttributes::DefaultSkinWeightProfileName);
	}

	// Get set of bone indices used in the target mesh
	TMap<FName, FBoneIndexType> UsedBoneNames;
	const FReferenceSkeleton& TargetRefSkeleton = ClothComponent->GetClothAsset()->GetRefSkeleton();

	const TMap<FName, TUniquePtr<FDynamicMeshVertexSkinWeightsAttribute>>& WeightLayers = TargetDynamicMesh.Attributes()->GetSkinWeightsAttributes();
	for (const TPair<FName, TUniquePtr<FDynamicMeshVertexSkinWeightsAttribute>>& LayerPair : WeightLayers)
	{
		const FDynamicMeshVertexSkinWeightsAttribute* Layer = LayerPair.Value.Get();

		for (int VertexID = 0; VertexID < TargetDynamicMesh.MaxVertexID(); ++VertexID)
		{
			if (TargetDynamicMesh.IsVertex(VertexID))
			{
				FBoneWeights Data;
				Layer->GetValue(VertexID, Data);
				for (FBoneWeight Wt : Data)
				{
					const FBoneIndexType BoneIndex = Wt.GetBoneIndex();
					const FName& BoneName = TargetRefSkeleton.GetRawRefBoneInfo()[BoneIndex].Name;
					UsedBoneNames.Add(BoneName, BoneIndex);
				}
			}
		}
	}

	// Update list of bone names in the Properties panel
	UsedBoneNames.ValueSort([](int16 A, int16 B) { return A < B; });
	UsedBoneNames.GetKeys(ToolProperties->BoneNameList);

	// Update the preview mesh
	PreviewMesh->UpdatePreview(&TargetDynamicMesh);
	PreviewMesh->SetMaterial(ToolSetupUtil::GetDefaultSculptMaterial(GetToolManager()));
	PreviewMesh->SetOverrideRenderMaterial(ToolSetupUtil::GetSelectionMaterial(GetToolManager()));

	UpdatePreviewMeshColor();

	PreviewMesh->SetTransform(TargetToWorld);
	PreviewMesh->SetVisible(true);

	UE::ToolTarget::HideSourceObject(Target);
}


void UClothTransferSkinWeightsTool::UpdateSourceMeshRender()
{
	if (ToolProperties && SourceComponent)
	{
		SourceComponent->SetVisibility(!ToolProperties->bHideSourceMesh);
	}
}

void UClothTransferSkinWeightsTool::TransferWeights()
{
	using namespace UE::AnimationCore;
	using namespace UE::Geometry;
	
	//TODO: for now, assume we are always transfering from LOD 0, but make this a parameter in the future...
	constexpr int32 SourceLODIdx = 0; 
	
	// User hasn't specified the source mesh in the UI
	if (ToolProperties->SourceMesh == nullptr) 
	{
		//TODO: Display error message
		return;
	}

	// Convert source Skeletal Mesh to Dynamic Mesh
	FDynamicMesh3 SourceDynamicMesh;
	USkeletalMesh* FromSkeletalMeshAsset = ToolProperties->SourceMesh;
	ClothTransferSkinWeightsToolHelpers::SkeletalMeshToDynamicMesh(FromSkeletalMeshAsset, SourceLODIdx, SourceDynamicMesh);
	FTransformSRT3d SourceToWorld; //TODO: Allows the user to set this value or infer it from an editor
	MeshTransforms::ApplyTransform(SourceDynamicMesh, SourceToWorld, true);

	UChaosClothAsset* TargetClothAsset = ClothComponent->GetClothAsset(); 

	// Compute bone index mappings
	TMap<FBoneIndexType, FName> SourceIndexToBone;
	TMap<FName, FBoneIndexType> TargetBoneToIndex;
	ClothTransferSkinWeightsToolHelpers::GetBoneMaps(static_cast<USkinnedAsset*>(FromSkeletalMeshAsset), static_cast<USkinnedAsset*>(TargetClothAsset), SourceIndexToBone, TargetBoneToIndex);
	
	// Setup bone weight transfer operator
	FTransferBoneWeights TransferBoneWeights(&SourceDynamicMesh, FSkeletalMeshAttributes::DefaultSkinWeightProfileName); 
	TransferBoneWeights.SourceIndexToBone = &SourceIndexToBone;
	TransferBoneWeights.TargetBoneToIndex = &TargetBoneToIndex;
	if (TransferBoneWeights.Validate() != EOperationValidationResult::Ok)
	{
		//TODO: Display error message
		return;
	}

	UE::Chaos::ClothAsset::FClothAdapter ClothAdapter(TargetClothAsset->GetClothCollection());
    FTransformSRT3d TargetToWorld; //TODO: Allows the user to set this value or infer it from an editor

	// Iterate over the LODs and transfer the bone weights from the source Skeletal mesh to the Cloth asset
	for (int TargetLODIdx = 0; TargetLODIdx < ClothAdapter.GetNumLods(); ++TargetLODIdx) 
	{
		UE::Chaos::ClothAsset::FClothLodAdapter ClothLodAdapter = ClothAdapter.GetLod(TargetLODIdx);

		// Cloth collection data arrays we are writing to
		TArrayView<int32> NumBoneInfluences = ClothLodAdapter.GetPatternsSimNumBoneInfluences();
		TArrayView<TArray<int32>> SimBoneIndices = ClothLodAdapter.GetPatternsSimBoneIndices();
		TArrayView<TArray<float>> SimBoneWeights = ClothLodAdapter.GetPatternsSimBoneWeights();

		const TArrayView<FVector3f> SimPositions =  ClothLodAdapter.GetPatternsSimRestPosition();
		
		checkSlow(SimPositions.Num() == SimBoneIndices.Num());
		
		const int32 NumVert = ClothLodAdapter.GetPatternsNumSimVertices();
		constexpr bool bUseParallel = true; 

		// Iterate over each vertex and write the data from FBoneWeights into cloth collection managed arrays
		ParallelFor(NumVert, [&](int32 VertexID)
		{
			const FVector3f Pos = SimPositions[VertexID];
			const FVector3d PosD = FVector3d((double)Pos[0], (double)Pos[1], (double)Pos[2]);
			
			UE::AnimationCore::FBoneWeights BoneWeights;
			TransferBoneWeights.Compute(PosD, TargetToWorld, BoneWeights);
			
			const int32 NumBones = BoneWeights.Num();
			
			NumBoneInfluences[VertexID] = NumBones;
			SimBoneIndices[VertexID].SetNum(NumBones);
			SimBoneWeights[VertexID].SetNum(NumBones);

			for (int BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx) 
			{
				SimBoneIndices[VertexID][BoneIdx] = BoneWeights[BoneIdx].GetBoneIndex();
				SimBoneWeights[VertexID][BoneIdx] = BoneWeights[BoneIdx].GetWeight();
			}

		}, bUseParallel ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
	}
}


void UClothTransferSkinWeightsTool::OnTick(float DeltaTime)
{
	if (PendingAction != EClothTransferSkinWeightsToolActions::NoAction)
	{
		if (PendingAction == EClothTransferSkinWeightsToolActions::Transfer)
		{
			TransferWeights();
		}
		PendingAction = EClothTransferSkinWeightsToolActions::NoAction;
	}
}


void UClothTransferSkinWeightsTool::RequestAction(EClothTransferSkinWeightsToolActions ActionType)
{
	if (PendingAction != EClothTransferSkinWeightsToolActions::NoAction)
	{
		return;
	}
	PendingAction = ActionType;
}

#undef LOCTEXT_NAMESPACE
