// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelBlendMeshesTool.h"
#include "CompositionOps/VoxelBlendMeshesOp.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"

#include "ToolSetupUtil.h"

#include "Selection/ToolSelectionUtil.h"

#include "DynamicMesh3.h"
#include "MeshTransforms.h"

#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"

#include "InteractiveGizmoManager.h"

#include "BaseGizmos/GizmoComponents.h"
#include "BaseGizmos/TransformGizmo.h"

#include "AssetGenerationUtil.h"

#include "CompositionOps/VoxelBlendMeshesOp.h"


#define LOCTEXT_NAMESPACE "UVoxelBlendMeshesTool"


/*
 * Tool
 */

void UVoxelBlendMeshesTool::SetupProperties()
{
	Super::SetupProperties();
	BlendProperties = NewObject<UVoxelBlendMeshesToolProperties>(this);
	BlendProperties->RestoreProperties(this);
	AddToolPropertySource(BlendProperties);
}


void UVoxelBlendMeshesTool::SaveProperties()
{
	Super::SaveProperties();
	BlendProperties->SaveProperties(this);
}


TUniquePtr<FDynamicMeshOperator> UVoxelBlendMeshesTool::MakeNewOperator()
{
	TUniquePtr<FVoxelBlendMeshesOp> Op = MakeUnique<FVoxelBlendMeshesOp>();

	Op->Transforms.SetNum(ComponentTargets.Num());
	Op->Meshes.SetNum(ComponentTargets.Num());
	for (int Idx = 0; Idx < ComponentTargets.Num(); Idx++)
	{
		Op->Meshes[Idx] = OriginalDynamicMeshes[Idx];
		Op->Transforms[Idx] = TransformProxies[Idx]->GetTransform();
	}

	Op->BlendFalloff = BlendProperties->BlendFalloff;
	Op->BlendPower = BlendProperties->BlendPower;
	Op->bSolidifyInput = BlendProperties->bSolidifyInput;
	Op->bRemoveInternalsAfterSolidify = BlendProperties->bRemoveInternalsAfterSolidify;
	Op->OffsetSolidifySurface = BlendProperties->OffsetSolidifySurface;

	VoxProperties->SetPropertiesOnOp(*Op);

	return Op;
}


FString UVoxelBlendMeshesTool::GetCreatedAssetName() const
{
	return TEXT("Blended");
}

FText UVoxelBlendMeshesTool::GetActionName() const
{
	return LOCTEXT("VoxelBlendMeshes", "Voxel Blend");
}







#undef LOCTEXT_NAMESPACE
