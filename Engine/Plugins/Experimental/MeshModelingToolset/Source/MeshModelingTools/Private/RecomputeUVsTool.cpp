// Copyright Epic Games, Inc. All Rights Reserved.

#include "RecomputeUVsTool.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Polygroups/PolygroupUtil.h"
#include "FaceGroupUtil.h"
#include "ToolSetupUtil.h"
#include "ModelingToolTargetUtil.h"
#include "ParameterizationOps/RecomputeUVsOp.h"


#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "URecomputeUVsTool"


/*
 * ToolBuilder
 */

USingleSelectionMeshEditingTool* URecomputeUVsToolBuilder::CreateNewTool(const FToolBuilderState& SceneState) const
{
	URecomputeUVsTool* NewTool = NewObject<URecomputeUVsTool>(SceneState.ToolManager);
	return NewTool;
}

/*
 * Tool
 */


void URecomputeUVsTool::Setup()
{
	UInteractiveTool::Setup();

	InputMesh = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	*InputMesh = UE::ToolTarget::GetDynamicMeshCopy(Target);

	Preview = NewObject<UMeshOpPreviewWithBackgroundCompute>(this);
	Preview->Setup(this->TargetWorld, this);
	Preview->PreviewMesh->SetTangentsMode(EDynamicMeshComponentTangentsMode::AutoCalculated);
	Preview->PreviewMesh->ReplaceMesh(*InputMesh);
	Preview->ConfigureMaterials(UE::ToolTarget::GetMaterialSet(Target).Materials,
								ToolSetupUtil::GetDefaultWorkingMaterial(GetToolManager()) );
	Preview->PreviewMesh->SetTransform((FTransform)UE::ToolTarget::GetLocalToWorldTransform(Target));

	Preview->OnMeshUpdated.AddLambda([this](UMeshOpPreviewWithBackgroundCompute* Op)
	{
		MaterialSettings->UpdateMaterials();
	});

	UE::ToolTarget::HideSourceObject(Target);

	// initialize our properties

	UVChannelProperties = NewObject<UMeshUVChannelProperties>(this);
	UVChannelProperties->RestoreProperties(this);
	UVChannelProperties->Initialize(InputMesh.Get(), false);
	UVChannelProperties->ValidateSelection(true);
	UVChannelProperties->WatchProperty(UVChannelProperties->UVChannel, [this](const FString& NewValue) 
	{
		MaterialSettings->UVChannel = UVChannelProperties->GetSelectedChannelIndex(true);
	});
	AddToolPropertySource(UVChannelProperties);

	Settings = NewObject<URecomputeUVsToolProperties>(this);
	Settings->RestoreProperties(this);
	AddToolPropertySource(Settings);

	PolygroupLayerProperties = NewObject<UPolygroupLayersProperties>(this);
	PolygroupLayerProperties->RestoreProperties(this, TEXT("RecomputeUVsTool"));
	PolygroupLayerProperties->InitializeGroupLayers(InputMesh.Get());
	PolygroupLayerProperties->WatchProperty(PolygroupLayerProperties->ActiveGroupLayer, [&](FName) { OnSelectedGroupLayerChanged(); });
	AddToolPropertySource(PolygroupLayerProperties);
	UpdateActiveGroupLayer();

	MaterialSettings = NewObject<UExistingMeshMaterialProperties>(this);
	MaterialSettings->MaterialMode = ESetMeshMaterialMode::Checkerboard;
	MaterialSettings->RestoreProperties(this, TEXT("ModelingUVTools"));
	AddToolPropertySource(MaterialSettings);
	// force update
	MaterialSettings->UpdateMaterials();
	Preview->OverrideMaterial = MaterialSettings->GetActiveOverrideMaterial();

	Preview->InvalidateResult();    // start compute

	SetToolDisplayName(LOCTEXT("ToolNameLocal", "UV Unwrap"));
	GetToolManager()->DisplayMessage(
		LOCTEXT("OnStartTool_Regions", "Generate UVs for Polygroups or existing UV charts of the Mesh using various strategies."),
		EToolMessageLevel::UserNotification);
}


void URecomputeUVsTool::OnPropertyModified(UObject* PropertySet, FProperty* Property)
{
	bool bForceMaterialUpdate = false;
	if (PropertySet == Settings || PropertySet == UVChannelProperties)
	{
		// One of the UV generation properties must have changed.  Dirty the result to force a recompute
		Preview->InvalidateResult();
		bForceMaterialUpdate = true;
	}

	if (PropertySet == MaterialSettings || bForceMaterialUpdate)
	{
		MaterialSettings->UpdateMaterials();
		Preview->OverrideMaterial = MaterialSettings->GetActiveOverrideMaterial();
	}
}


void URecomputeUVsTool::Shutdown(EToolShutdownType ShutdownType)
{
	UVChannelProperties->SaveProperties(this);
	Settings->SaveProperties(this);
	PolygroupLayerProperties->RestoreProperties(this, TEXT("RecomputeUVsTool"));
	MaterialSettings->SaveProperties(this, TEXT("ModelingUVTools"));

	UE::ToolTarget::ShowSourceObject(Target);

	FDynamicMeshOpResult Result = Preview->Shutdown();
	if (ShutdownType == EToolShutdownType::Accept)
	{
		GetToolManager()->BeginUndoTransaction(LOCTEXT("RecomputeUVs", "Recompute UVs"));
		FDynamicMesh3* NewDynamicMesh = Result.Mesh.Get();
		if (ensure(NewDynamicMesh))
		{
			UE::ToolTarget::CommitDynamicMeshUVUpdate(Target, NewDynamicMesh);
		}
		GetToolManager()->EndUndoTransaction();
	}

}

void URecomputeUVsTool::OnTick(float DeltaTime)
{
	Preview->Tick(DeltaTime);
}

bool URecomputeUVsTool::CanAccept() const
{
	return Super::CanAccept() && Preview->HaveValidResult();
}

TUniquePtr<FDynamicMeshOperator> URecomputeUVsTool::MakeNewOperator()
{
	FAxisAlignedBox3d MeshBounds = Preview->PreviewMesh->GetMesh()->GetBounds();
	TUniquePtr<FRecomputeUVsOp> RecomputeUVsOp = MakeUnique<FRecomputeUVsOp>();
	RecomputeUVsOp->InputMesh = InputMesh;
	RecomputeUVsOp->InputGroups = ActiveGroupSet;
	RecomputeUVsOp->UVLayer = UVChannelProperties->GetSelectedChannelIndex(true);
	
	RecomputeUVsOp->IslandMode = (UE::Geometry::ERecomputeUVsIslandMode)(int)Settings->IslandMode;
	RecomputeUVsOp->UnwrapType = (UE::Geometry::ERecomputeUVsUnwrapType)(int)Settings->UnwrapType;

	RecomputeUVsOp->bPackUVs = Settings->bAutoPack;
	if (Settings->bAutoPack)
	{
		RecomputeUVsOp->PackingTextureResolution = Settings->TextureResolution;
		RecomputeUVsOp->bNormalizeAreas = false;
		RecomputeUVsOp->AreaScaling = 1.0;
	}
	else
	{
		switch (Settings->UVScaleMode)
		{
		case ERecomputeUVsToolUVScaleMode::NoScaling:
			RecomputeUVsOp->bNormalizeAreas = false;
			RecomputeUVsOp->AreaScaling = 1.0;
			break;
		case ERecomputeUVsToolUVScaleMode::NormalizeToBounds:
			RecomputeUVsOp->bNormalizeAreas = true;
			RecomputeUVsOp->AreaScaling = Settings->UVScale / MeshBounds.MaxDim();
			break;
		case ERecomputeUVsToolUVScaleMode::NormalizeToWorld:
			RecomputeUVsOp->bNormalizeAreas = true;
			RecomputeUVsOp->AreaScaling = Settings->UVScale;
			break;
		}
	}


	RecomputeUVsOp->SetTransform(UE::ToolTarget::GetLocalToWorldTransform(Target));

	return RecomputeUVsOp;
}



void URecomputeUVsTool::OnSelectedGroupLayerChanged()
{
	UpdateActiveGroupLayer();
	Preview->InvalidateResult();
}


void URecomputeUVsTool::UpdateActiveGroupLayer()
{
	if (PolygroupLayerProperties->HasSelectedPolygroup() == false)
	{
		ActiveGroupSet = MakeShared<UE::Geometry::FPolygroupSet, ESPMode::ThreadSafe>(InputMesh.Get());
	}
	else
	{
		FName SelectedName = PolygroupLayerProperties->ActiveGroupLayer;
		FDynamicMeshPolygroupAttribute* FoundAttrib = UE::Geometry::FindPolygroupLayerByName(*InputMesh, SelectedName);
		ensureMsgf(FoundAttrib, TEXT("Selected Attribute Not Found! Falling back to Default group layer."));
		ActiveGroupSet = MakeShared<UE::Geometry::FPolygroupSet, ESPMode::ThreadSafe>(InputMesh.Get(), FoundAttrib);
	}

}



#undef LOCTEXT_NAMESPACE
