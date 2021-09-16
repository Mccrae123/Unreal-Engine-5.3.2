// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeometryScript/MeshMaterialFunctions.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"

#include "ExplicitUseGeometryMathTypes.h"		// using UE::Geometry::(math types)
using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UGeometryScriptLibrary_MeshMaterialFunctions"


template<typename ReturnType> 
ReturnType SimpleMeshMaterialQuery(UDynamicMesh* Mesh, bool& bHasMaterials, ReturnType DefaultValue, 
	TFunctionRef<ReturnType(const FDynamicMesh3& Mesh, const FDynamicMeshMaterialAttribute& MaterialIDs)> QueryFunc)
{
	bHasMaterials = false;
	ReturnType RetVal = DefaultValue;
	if (Mesh)
	{
		Mesh->ProcessMesh([&](const FDynamicMesh3& ReadMesh)
		{
			if (ReadMesh.HasAttributes() && ReadMesh.Attributes()->HasMaterialID() )
			{
				const FDynamicMeshMaterialAttribute* MaterialIDs = ReadMesh.Attributes()->GetMaterialID();
				if (MaterialIDs != nullptr)
				{
					bHasMaterials = true;
					RetVal = QueryFunc(ReadMesh, *MaterialIDs);
				}
			}
		});
	}
	return RetVal;
}


void SimpleMeshMaterialEdit(UDynamicMesh* Mesh, bool bEnableIfMissing, bool& bHasMaterialIDs,
	TFunctionRef<void(FDynamicMesh3& Mesh, FDynamicMeshMaterialAttribute& MaterialIDs)> EditFunc)
{
	bHasMaterialIDs = false;
	if (Mesh)
	{
		Mesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			if (EditMesh.HasAttributes() == false)
			{
				if (bEnableIfMissing)
				{
					EditMesh.EnableAttributes();
				}
				else
				{
					return;
				}
			}
			if (EditMesh.Attributes()->HasMaterialID() == false)
			{
				if (bEnableIfMissing)
				{
					EditMesh.Attributes()->EnableMaterialID();
				}
				else
				{
					return;
				}
			}
			FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
			if (ensure(MaterialIDs != nullptr))
			{
				bHasMaterialIDs = true;
				EditFunc(EditMesh, *MaterialIDs);
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
}




UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(
	UDynamicMesh* TargetMesh,
	UGeometryScriptDebug* Debug)
{
	if (TargetMesh == nullptr)
	{
		UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("EnableMaterialIDs_InvalidInput", "EnableMaterialIDs: TargetMesh is Null"));
		return TargetMesh;
	}

	bool bHasMaterialIDs;
	SimpleMeshMaterialEdit(TargetMesh, true, bHasMaterialIDs, [](FDynamicMesh3& Mesh, FDynamicMeshMaterialAttribute& MaterialIDs) {});

	return TargetMesh;

}



UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::ClearMaterialIDs(
	UDynamicMesh* TargetMesh,
	UGeometryScriptDebug* Debug)
{
	if (TargetMesh == nullptr)
	{
		UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("ClearMaterialIDs_InvalidInput", "ClearMaterialIDs: TargetMesh is Null"));
		return TargetMesh;
	}

	bool bHasMaterialIDs;
	SimpleMeshMaterialEdit(TargetMesh, true, bHasMaterialIDs, 
		[](FDynamicMesh3& Mesh, FDynamicMeshMaterialAttribute& MaterialIDs) 
	{
		for (int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			MaterialIDs.SetValue(TriangleID, 0);
		}
	});

	return TargetMesh;
}



UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::RemapMaterialIDs(
	UDynamicMesh* TargetMesh,
	int FromMaterialID,
	int ToMaterialID,
	UGeometryScriptDebug* Debug)
{
	if (TargetMesh == nullptr)
	{
		UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("RemapMaterialIDs_InvalidInput", "RemapMaterialIDs: TargetMesh is Null"));
		return TargetMesh;
	}

	bool bHasMaterialIDs;
	SimpleMeshMaterialEdit(TargetMesh, true, bHasMaterialIDs, 
		[&](FDynamicMesh3& Mesh, FDynamicMeshMaterialAttribute& MaterialIDs) 
	{
		for (int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			int32 CurID = MaterialIDs.GetValue(TriangleID);
			if (CurID == FromMaterialID)
			{
				MaterialIDs.SetValue(TriangleID, ToMaterialID);
			}
		}
	});

	return TargetMesh;
}


int UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID( UDynamicMesh* TargetMesh, bool& bHasMaterialIDs )
{
	return SimpleMeshMaterialQuery<int32>(TargetMesh, bHasMaterialIDs, 0, [&](const FDynamicMesh3& Mesh, const FDynamicMeshMaterialAttribute& MaterialIDs) {
		int32 MaxID = 0;
		for (int TriangleID : Mesh.TriangleIndicesItr())
		{
			MaxID = FMath::Max(MaxID, MaterialIDs.GetValue(TriangleID));
		}
		return MaxID;
	});
}



int32 UGeometryScriptLibrary_MeshMaterialFunctions::GetTriangleMaterialID( 
	UDynamicMesh* TargetMesh, 
	int TriangleID, 
	bool& bIsValidTriangle)
{
	bool bHasMaterials = false;
	return SimpleMeshMaterialQuery<int32>(TargetMesh, bHasMaterials, 0, [&](const FDynamicMesh3& Mesh, const FDynamicMeshMaterialAttribute& MaterialIDs) {
		bIsValidTriangle = Mesh.IsTriangle(TriangleID);
		return (bIsValidTriangle) ? MaterialIDs.GetValue(TriangleID) : 0;
	});
}


UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::GetAllTriangleMaterialIDs(UDynamicMesh* TargetMesh, UPARAM(ref) TArray<int>& MaterialIDs, bool& bHasMaterialIDs)
{
	MaterialIDs.Reset();
	bHasMaterialIDs = false;
	SimpleMeshMaterialQuery<int32>(TargetMesh, bHasMaterialIDs, 0, [&](const FDynamicMesh3& Mesh, const FDynamicMeshMaterialAttribute& MaterialIDAttrib) {
		int32 MaxTriangleID = Mesh.MaxTriangleID();
		for (int32 TriangleID = 0; TriangleID < Mesh.MaxTriangleID(); ++TriangleID)
		{
			int32 MaterialID = Mesh.IsTriangle(TriangleID) ? MaterialIDAttrib.GetValue(TriangleID) : -1;
			MaterialIDs.Add(MaterialID);
		}
		return 0;
	});
	return TargetMesh;
}



UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::SetTriangleMaterialID( 
	UDynamicMesh* TargetMesh, 
	int TriangleID, 
	int MaterialID,
	bool& bIsValidTriangle,
	bool bDeferChangeNotifications)
{
	bIsValidTriangle = false;
	if (TargetMesh)
	{
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			if (EditMesh.IsTriangle(TriangleID) && EditMesh.HasAttributes() && EditMesh.Attributes()->HasMaterialID() )
			{
				FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
				if (MaterialIDs != nullptr)
				{
					bIsValidTriangle = true;
					MaterialIDs->SetValue(TriangleID, MaterialID);
				}
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, bDeferChangeNotifications);
	}
	return TargetMesh;	
}



UDynamicMesh* UGeometryScriptLibrary_MeshMaterialFunctions::SetAllTriangleMaterialIDs(
	UDynamicMesh* TargetMesh,
	const TArray<int32>& TriangleMaterialIDs,
	bool bDeferChangeNotifications,
	UGeometryScriptDebug* Debug)
{
	if (TargetMesh == nullptr)
	{
		UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("SetAllTriangleMaterialIDs_InvalidInput", "SetAllTriangleMaterialIDs: TargetMesh is Null"));
		return TargetMesh;
	}

	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (TriangleMaterialIDs.Num() < EditMesh.MaxTriangleID())
		{
			UE::Geometry::AppendError(Debug, EGeometryScriptErrorType::InvalidInputs, LOCTEXT("SetAllTriangleMaterialIDs_IncorrectCount", "SetAllTriangleMaterialIDs: size of provided TriangleMaterialIDs is smaller than MaxTriangleID of Mesh"));
		}
		else
		{
			if (EditMesh.HasAttributes() == false)
			{
				EditMesh.EnableAttributes();
			}
			if (EditMesh.Attributes()->HasMaterialID() == false)
			{
				EditMesh.Attributes()->EnableMaterialID();
			}
			FDynamicMeshMaterialAttribute* MaterialIDs = EditMesh.Attributes()->GetMaterialID();
			for (int32 TriangleID : EditMesh.TriangleIndicesItr())
			{
				MaterialIDs->SetValue(TriangleID, TriangleMaterialIDs[TriangleID]);
			}
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, bDeferChangeNotifications);

	return TargetMesh;
}




#undef LOCTEXT_NAMESPACE