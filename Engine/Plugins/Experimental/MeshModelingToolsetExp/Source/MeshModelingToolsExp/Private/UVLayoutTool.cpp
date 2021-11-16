// Copyright Epic Games, Inc. All Rights Reserved.

#include "UVLayoutTool.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"

#include "ToolSetupUtil.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "BaseBehaviors/MultiClickSequenceInputBehavior.h"
#include "Selection/SelectClickedAction.h"

#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "ParameterizationOps/UVLayoutOp.h"
#include "Properties/UVLayoutProperties.h"

#include "TargetInterfaces/MaterialProvider.h"
#include "TargetInterfaces/MeshDescriptionCommitter.h"
#include "TargetInterfaces/MeshDescriptionProvider.h"
#include "TargetInterfaces/PrimitiveComponentBackedTarget.h"
#include "ToolTargetManager.h"

#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UUVLayoutTool"

/*
 * ToolBuilder
 */


const FToolTargetTypeRequirements& UUVLayoutToolBuilder::GetTargetRequirements() const
{
	static FToolTargetTypeRequirements TypeRequirements({
		UMaterialProvider::StaticClass(),
		UMeshDescriptionCommitter::StaticClass(),
		UMeshDescriptionProvider::StaticClass(),
		UPrimitiveComponentBackedTarget::StaticClass()
		});
	return TypeRequirements;
}

bool UUVLayoutToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return SceneState.TargetManager->CountSelectedAndTargetable(SceneState, GetTargetRequirements()) >= 1;
}

UInteractiveTool* UUVLayoutToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UUVLayoutTool* NewTool = NewObject<UUVLayoutTool>(SceneState.ToolManager);

	TArray<TObjectPtr<UToolTarget>> Targets = SceneState.TargetManager->BuildAllSelectedTargetable(SceneState, GetTargetRequirements());
	NewTool->SetTargets(MoveTemp(Targets));
	NewTool->SetWorld(SceneState.World, SceneState.GizmoManager);

	return NewTool;
}



/*
 * Tool
 */

UUVLayoutTool::UUVLayoutTool()
{
}

void UUVLayoutTool::SetWorld(UWorld* World, UInteractiveGizmoManager* GizmoManagerIn)
{
	this->TargetWorld = World;
}

void UUVLayoutTool::Setup()
{
	UInteractiveTool::Setup();

	// hide input StaticMeshComponent
	for (int32 ComponentIdx = 0; ComponentIdx < Targets.Num(); ComponentIdx++)
	{
		TargetComponentInterface(ComponentIdx)->SetOwnerVisibility(false);
	}

	// if we only have one object, add ability to set UV channel
	if (Targets.Num() == 1)
	{
		UVChannelProperties = NewObject<UMeshUVChannelProperties>(this);
		UVChannelProperties->RestoreProperties(this);
		UVChannelProperties->Initialize(TargetMeshProviderInterface(0)->GetMeshDescription(), false);
		UVChannelProperties->ValidateSelection(true);
		AddToolPropertySource(UVChannelProperties);
		UVChannelProperties->WatchProperty(UVChannelProperties->UVChannel, [this](const FString& NewValue)
		{
			MaterialSettings->UVChannel = UVChannelProperties->GetSelectedChannelIndex(true);
			UpdateVisualization();
		});
	}

	BasicProperties = NewObject<UUVLayoutProperties>(this);
	BasicProperties->RestoreProperties(this);
	AddToolPropertySource(BasicProperties);

	MaterialSettings = NewObject<UExistingMeshMaterialProperties>(this);
	MaterialSettings->RestoreProperties(this);
	AddToolPropertySource(MaterialSettings);

	// if we only have one object, add optional UV layout view
	if (Targets.Num() == 1)
	{
		UVLayoutView = NewObject<UUVLayoutPreview>(this);
		UVLayoutView->CreateInWorld(TargetWorld);

		FComponentMaterialSet MaterialSet;
		TargetMaterialInterface(0)->GetMaterialSet(MaterialSet);
		UVLayoutView->SetSourceMaterials(MaterialSet);

		UVLayoutView->SetSourceWorldPosition(
			TargetComponentInterface(0)->GetOwnerActor()->GetTransform(),
			TargetComponentInterface(0)->GetOwnerActor()->GetComponentsBoundingBox());

		UVLayoutView->Settings->RestoreProperties(this);
		AddToolPropertySource(UVLayoutView->Settings);
	}

	UpdateVisualization();

	SetToolDisplayName(LOCTEXT("ToolName", "UV Layout"));
	GetToolManager()->DisplayMessage(LOCTEXT("OnStartUVLayoutTool", "Transform/Rotate/Scale existing UV Charts using various strategies"),
		EToolMessageLevel::UserNotification);
}


void UUVLayoutTool::UpdateNumPreviews()
{
	int32 CurrentNumPreview = Previews.Num();
	int32 TargetNumPreview = Targets.Num();
	if (TargetNumPreview < CurrentNumPreview)
	{
		for (int32 PreviewIdx = CurrentNumPreview - 1; PreviewIdx >= TargetNumPreview; PreviewIdx--)
		{
			Previews[PreviewIdx]->Cancel();
		}
		Previews.SetNum(TargetNumPreview);
		OriginalDynamicMeshes.SetNum(TargetNumPreview);
		Factories.SetNum(TargetNumPreview);
	}
	else
	{
		OriginalDynamicMeshes.SetNum(TargetNumPreview);
		Factories.SetNum(TargetNumPreview);
		for (int32 PreviewIdx = CurrentNumPreview; PreviewIdx < TargetNumPreview; PreviewIdx++)
		{
			OriginalDynamicMeshes[PreviewIdx] = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(TargetMeshProviderInterface(PreviewIdx)->GetMeshDescription(), *OriginalDynamicMeshes[PreviewIdx]);

			Factories[PreviewIdx]= NewObject<UUVLayoutOperatorFactory>();
			Factories[PreviewIdx]->OriginalMesh = OriginalDynamicMeshes[PreviewIdx];
			Factories[PreviewIdx]->Settings = BasicProperties;
			Factories[PreviewIdx]->TargetTransform = TargetComponentInterface(PreviewIdx)->GetWorldTransform();
			Factories[PreviewIdx]->GetSelectedUVChannel = [this]() { return GetSelectedUVChannel(); };

			UMeshOpPreviewWithBackgroundCompute* Preview = Previews.Add_GetRef(NewObject<UMeshOpPreviewWithBackgroundCompute>(Factories[PreviewIdx], "Preview"));
			Preview->Setup(this->TargetWorld, Factories[PreviewIdx]);
			ToolSetupUtil::ApplyRenderingConfigurationToPreview(Preview->PreviewMesh, Targets[PreviewIdx]); 

			FComponentMaterialSet MaterialSet;
			TargetMaterialInterface(PreviewIdx)->GetMaterialSet(MaterialSet);
			Preview->ConfigureMaterials(MaterialSet.Materials,
				ToolSetupUtil::GetDefaultWorkingMaterial(GetToolManager())
			);
			Preview->PreviewMesh->UpdatePreview(OriginalDynamicMeshes[PreviewIdx].Get());
			Preview->PreviewMesh->SetTransform(TargetComponentInterface(PreviewIdx)->GetWorldTransform());

			Preview->OnMeshUpdated.AddLambda([this](UMeshOpPreviewWithBackgroundCompute* Compute)
			{
				OnPreviewMeshUpdated(Compute);
			});

			Preview->SetVisibility(true);
		}
	}
}


void UUVLayoutTool::Shutdown(EToolShutdownType ShutdownType)
{
	if (UVLayoutView)
	{
		UVLayoutView->Settings->SaveProperties(this);
		UVLayoutView->Disconnect();
	}

	BasicProperties->SaveProperties(this);
	MaterialSettings->SaveProperties(this);

	// Restore (unhide) the source meshes
	for (int32 ComponentIdx = 0; ComponentIdx < Targets.Num(); ComponentIdx++)
	{
		TargetComponentInterface(ComponentIdx)->SetOwnerVisibility(true);
	}

	TArray<FDynamicMeshOpResult> Results;
	for (UMeshOpPreviewWithBackgroundCompute* Preview : Previews)
	{
		Results.Emplace(Preview->Shutdown());
	}
	if (ShutdownType == EToolShutdownType::Accept)
	{
		GenerateAsset(Results);
	}
	for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
	{
		Factories[TargetIndex] = nullptr;
	}
}

int32 UUVLayoutTool::GetSelectedUVChannel() const
{
	return UVChannelProperties ? UVChannelProperties->GetSelectedChannelIndex(true) : 0;
}


void UUVLayoutTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	GetToolManager()->GetContextQueriesAPI()->GetCurrentViewState(CameraState);

	if (UVLayoutView)
	{
		UVLayoutView->Render(RenderAPI);
	}

}

void UUVLayoutTool::OnTick(float DeltaTime)
{
	for (UMeshOpPreviewWithBackgroundCompute* Preview : Previews)
	{
		Preview->Tick(DeltaTime);
	}

	if (UVLayoutView)
	{
		UVLayoutView->OnTick(DeltaTime);
	}


}



void UUVLayoutTool::OnPropertyModified(UObject* PropertySet, FProperty* Property)
{
	if (PropertySet == BasicProperties || PropertySet == UVChannelProperties)
	{
		UpdateNumPreviews();
		for (UMeshOpPreviewWithBackgroundCompute* Preview : Previews)
		{
			Preview->InvalidateResult();
		}
	}
	else if (PropertySet == MaterialSettings)
	{
		// if we don't know what changed, or we know checker density changed, update checker material
		UpdateVisualization();
	}
}


void UUVLayoutTool::OnPreviewMeshUpdated(UMeshOpPreviewWithBackgroundCompute* Compute)
{
	if (UVLayoutView)
	{
		FDynamicMesh3 ResultMesh;
		if (Compute->GetCurrentResultCopy(ResultMesh, false) == false)
		{
			return;
		}
		UVLayoutView->UpdateUVMesh(&ResultMesh, GetSelectedUVChannel());
	}

}


void UUVLayoutTool::UpdateVisualization()
{
	MaterialSettings->UpdateMaterials();
	UpdateNumPreviews();
	for (int PreviewIdx = 0; PreviewIdx < Previews.Num(); PreviewIdx++)
	{
		UMeshOpPreviewWithBackgroundCompute* Preview = Previews[PreviewIdx];
		Preview->OverrideMaterial = MaterialSettings->GetActiveOverrideMaterial();
	}

	for (UMeshOpPreviewWithBackgroundCompute* Preview : Previews)
	{
		Preview->InvalidateResult();
	}
}

bool UUVLayoutTool::CanAccept() const
{
	for (UMeshOpPreviewWithBackgroundCompute* Preview : Previews)
	{
		if (!Preview->HaveValidResult())
		{
			return false;
		}
	}
	return Super::CanAccept();
}


void UUVLayoutTool::GenerateAsset(const TArray<FDynamicMeshOpResult>& Results)
{
	GetToolManager()->BeginUndoTransaction(LOCTEXT("UVLayoutToolTransactionName", "UV Layout Tool"));

	check(Results.Num() == Targets.Num());
	
	for (int32 ComponentIdx = 0; ComponentIdx < Targets.Num(); ComponentIdx++)
	{
		check(Results[ComponentIdx].Mesh.Get() != nullptr);
		TargetMeshCommitterInterface(ComponentIdx)->CommitMeshDescription([&Results, &ComponentIdx, this](const IMeshDescriptionCommitter::FCommitterParams& CommitParams)
		{
			FDynamicMesh3* DynamicMesh = Results[ComponentIdx].Mesh.Get();
			FMeshDescription* MeshDescription = CommitParams.MeshDescriptionOut;
			
			bool bVerticesOnly = false;
			bool bAttributesOnly = true;
			if (FDynamicMeshToMeshDescription::HaveMatchingElementCounts(DynamicMesh, MeshDescription, bVerticesOnly, bAttributesOnly))
			{
				FDynamicMeshToMeshDescription Converter;
				Converter.UpdateAttributes(DynamicMesh, *MeshDescription, false, false, true/*update uvs*/);
			}
			else
			{
				// must have been duplicate tris in the mesh description; we can't count on 1-to-1 mapping of TriangleIDs.  Just convert
				FDynamicMeshToMeshDescription Converter;
				Converter.Convert(DynamicMesh, *MeshDescription);
			}
		});
	}

	GetToolManager()->EndUndoTransaction();
}




#undef LOCTEXT_NAMESPACE
