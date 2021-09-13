// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeometryScript/SceneUtilityFunctions.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "DynamicMesh/MeshTransforms.h"

#include "Components/StaticMeshComponent.h"
#include "Components/DynamicMeshComponent.h"


#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UGeometryScriptLibrary_SceneUtilityFunctions"


UDynamicMesh* UGeometryScriptLibrary_SceneUtilityFunctions::CopyMeshFromComponent(
	USceneComponent* Component,
	UDynamicMesh* ToDynamicMesh,
	FGeometryScriptCopyMeshFromComponentOptions Options,
	bool bTransformToWorld,
	FTransform& LocalToWorld,
	TEnumAsByte<EGeometryScriptOutcomePins>& Outcome,
	UGeometryScriptDebug* Debug)
{
	Outcome = EGeometryScriptOutcomePins::Failure;

	if (Cast<UStaticMeshComponent>(Component) != nullptr)
	{
		UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
		LocalToWorld = StaticMeshComponent->GetComponentTransform();
		UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
		if (StaticMesh)
		{
			FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
			AssetOptions.bApplyBuildSettings = (Options.bWantNormals || Options.bWantTangents);
			AssetOptions.bRequestTangents = Options.bWantTangents;
			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
				StaticMesh, ToDynamicMesh, AssetOptions, Options.RequestedLOD, Outcome, Debug);	// will set Outcome pin
		}
		else
		{
			UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("CopyMeshFromComponent_MissingStaticMesh", "CopyMeshFromComponent: StaticMeshComponent has a null StaticMesh"));
		}
	}
	else if (Cast<UDynamicMeshComponent>(Component) != nullptr)
	{
		UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component);
		LocalToWorld = DynamicMeshComponent->GetComponentTransform();
		UDynamicMesh* CopyDynamicMesh = DynamicMeshComponent->GetDynamicMesh();
		if (CopyDynamicMesh)
		{
			CopyDynamicMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
			{
				ToDynamicMesh->SetMesh(Mesh);
			});
			Outcome = EGeometryScriptOutcomePins::Success;
		}
		else
		{
			UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("CopyMeshFromComponent_MissingDynamicMesh", "CopyMeshFromComponent: DynamicMeshComponent has a null DynamicMesh"));
		}
	}

	// transform mesh to world
	if (Outcome == EGeometryScriptOutcomePins::Success && bTransformToWorld)
	{
		ToDynamicMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
		{
			MeshTransforms::ApplyTransform(EditMesh, (UE::Geometry::FTransform3d)LocalToWorld);

		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);	
	}

	return ToDynamicMesh;
}


#undef LOCTEXT_NAMESPACE