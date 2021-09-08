// Copyright Epic Games, Inc. All Rights Reserved.

#include "UVEditorParameterizeMeshTool.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"
#include "ToolSetupUtil.h"
#include "ModelingToolTargetUtil.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "FaceGroupUtil.h"
#include "ParameterizationOps/ParameterizeMeshOp.h"
#include "Properties/ParameterizeMeshProperties.h"
#include "ToolTargets/UVEditorToolMeshInput.h"
#include "UVToolContextObjects.h"
#include "ContextObjectStore.h"

#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UParameterizeMeshTool"


// Tool builder
// TODO: Could consider sharing some of the tool builder boilerplate for UV editor tools in a common base class.

const FToolTargetTypeRequirements& UUVEditorParameterizeMeshToolBuilder::GetTargetRequirements() const
{
	static FToolTargetTypeRequirements TypeRequirements(UUVEditorToolMeshInput::StaticClass());
	return TypeRequirements;
}

bool UUVEditorParameterizeMeshToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return Targets && Targets->Num() >= 1;
}

UInteractiveTool* UUVEditorParameterizeMeshToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UUVEditorParameterizeMeshTool* NewTool = NewObject<UUVEditorParameterizeMeshTool>(SceneState.ToolManager);
	NewTool->SetTargets(*Targets);

	return NewTool;
}


/*
 * Tool
 */


void UUVEditorParameterizeMeshTool::Setup()
{
	check(Targets.Num() >= 1);

	UInteractiveTool::Setup();

	// initialize our properties
	Settings = NewObject<UParameterizeMeshToolProperties>(this);
	Settings->RestoreProperties(this);
	AddToolPropertySource(Settings);
	Settings->WatchProperty(Settings->Method, [&](EParameterizeMeshUVMethod) { OnMethodTypeChanged(); });

	UVAtlasProperties = NewObject<UParameterizeMeshToolUVAtlasProperties>(this);
	UVAtlasProperties->RestoreProperties(this);
	AddToolPropertySource(UVAtlasProperties);
	SetToolPropertySourceEnabled(UVAtlasProperties, true);

	XAtlasProperties = NewObject<UParameterizeMeshToolXAtlasProperties>(this);
	XAtlasProperties->RestoreProperties(this);
	AddToolPropertySource(XAtlasProperties);
	SetToolPropertySourceEnabled(XAtlasProperties, true);
	
	PatchBuilderProperties = NewObject<UParameterizeMeshToolPatchBuilderProperties>(this);
	PatchBuilderProperties->RestoreProperties(this);
	AddToolPropertySource(PatchBuilderProperties);
	SetToolPropertySourceEnabled(PatchBuilderProperties, true);

	MaterialSettings = NewObject<UExistingMeshMaterialProperties>(this);
	MaterialSettings->MaterialMode = ESetMeshMaterialMode::Checkerboard;
	MaterialSettings->RestoreProperties(this, TEXT("ModelingUVTools"));
	MaterialSettings->UVChannel = 0; // We only ever have one UV layer for the applied preview.
	AddToolPropertySource(MaterialSettings);
	// force update
	MaterialSettings->UpdateMaterials();
	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		Target->AppliedPreview->OverrideMaterial = MaterialSettings->GetActiveOverrideMaterial();
	}

	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		UParameterizeMeshOperatorFactory* OpFactory = NewObject<UParameterizeMeshOperatorFactory>();
		OpFactory->TargetTransform = Target->AppliedPreview->PreviewMesh->GetTransform();
		OpFactory->Settings = Settings;
		OpFactory->UVAtlasProperties = UVAtlasProperties;
		OpFactory->XAtlasProperties = XAtlasProperties;
		OpFactory->PatchBuilderProperties = PatchBuilderProperties;
		OpFactory->OriginalMesh = Target->AppliedCanonical;
		OpFactory->GetSelectedUVChannel = [Target]() { return Target->UVLayerIndex; };

		Target->AppliedPreview->ChangeOpFactory(OpFactory);
		Target->AppliedPreview->OnMeshUpdated.AddWeakLambda(this, [Target](UMeshOpPreviewWithBackgroundCompute* Preview) {
			Target->UpdateUnwrapPreviewFromAppliedPreview();
			});

		Target->AppliedPreview->InvalidateResult();
	}

	SetToolDisplayName(LOCTEXT("ToolNameGlobal", "AutoUV"));
	GetToolManager()->DisplayMessage(
		LOCTEXT("OnStartTool_Global", "Automatically partition the selected Mesh into UV islands, flatten, and pack into a single UV chart"),
		EToolMessageLevel::UserNotification);
}

void UUVEditorParameterizeMeshTool::OnPropertyModified(UObject* PropertySet, FProperty* Property)
{
	if (PropertySet != MaterialSettings)
	{
		for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
		{
			Target->AppliedPreview->InvalidateResult();
		}
	}

	MaterialSettings->UpdateMaterials();
	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		Target->AppliedPreview->OverrideMaterial = MaterialSettings->GetActiveOverrideMaterial();
	}
}


void UUVEditorParameterizeMeshTool::OnMethodTypeChanged()
{
	SetToolPropertySourceEnabled(UVAtlasProperties, Settings->Method == EParameterizeMeshUVMethod::UVAtlas);
	SetToolPropertySourceEnabled(XAtlasProperties, Settings->Method == EParameterizeMeshUVMethod::XAtlas);
	SetToolPropertySourceEnabled(PatchBuilderProperties, Settings->Method == EParameterizeMeshUVMethod::PatchBuilder);

	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		Target->AppliedPreview->InvalidateResult();
	}
}


void UUVEditorParameterizeMeshTool::Shutdown(EToolShutdownType ShutdownType)
{
	Settings->SaveProperties(this);
	MaterialSettings->SaveProperties(this, TEXT("ModelingUVTools"));
	UVAtlasProperties->SaveProperties(this);
	XAtlasProperties->SaveProperties(this);
	PatchBuilderProperties->SaveProperties(this);

	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		Target->AppliedPreview->OnMeshUpdated.RemoveAll(this);
		Target->AppliedPreview->ClearOpFactory();
		Target->AppliedPreview->OverrideMaterial = nullptr;
	}

	if (ShutdownType == EToolShutdownType::Accept)
	{
		UUVToolEmitChangeAPI* ChangeAPI = GetToolManager()->GetContextObjectStore()->FindContext<UUVToolEmitChangeAPI>();

		for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
		{
			// Set things up for undo. 
			FDynamicMeshChangeTracker ChangeTracker(Target->UnwrapCanonical.Get());
			ChangeTracker.BeginChange();

			for (int32 Tid : Target->UnwrapCanonical->TriangleIndicesItr())
			{
				ChangeTracker.SaveTriangle(Tid, true);
			}

			Target->UpdateCanonicalFromPreviews();

			ChangeAPI->EmitToolIndependentUnwrapCanonicalChange(Target, ChangeTracker.EndChange(), LOCTEXT("ApplyParameterizeMeshTool", "Auto UV Tool"));
		}
	}
	else
	{
		// Reset the inputs
		for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
		{
			Target->UpdatePreviewsFromCanonical();
		}
	}

	Settings = nullptr;
	Targets.Empty();
}

void UUVEditorParameterizeMeshTool::OnTick(float DeltaTime)
{
}

bool UUVEditorParameterizeMeshTool::CanAccept() const
{
	for (TObjectPtr<UUVEditorToolMeshInput> Target : Targets)
	{
		if (!Target->AppliedPreview->HaveValidResult())
		{
			return false;
		}
	}
	return true;
}


#undef LOCTEXT_NAMESPACE
