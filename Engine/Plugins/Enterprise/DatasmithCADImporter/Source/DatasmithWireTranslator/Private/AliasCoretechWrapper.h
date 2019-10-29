// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#ifdef CAD_LIBRARY
#include "CoreMinimal.h"

#include "CTSession.h"

// Additional scale factor required when tessellating NURBS as Alias has extremely small geometry,
// originally tessellating to triangles with area in the order of 10^-5, failing the
// FourSquaredTriangleArea > SMALL_NUMBER test in DatasmithMeshHelper.cpp::IsMeshValid

class AlDagNode;
class AlShell;
class AlSurface;
class AlTrimBoundary;
class AlTrimCurve;
class AlTrimRegion;
class AlCurve;


typedef double AlMatrix4x4[4][4];

using namespace CADLibrary;

class FAliasCoretechWrapper : public CTSession
{
public:
	/**
	 * Make sure CT is initialized, and a main object is ready.
	 * Handle input file unit and an output unit
	 * @param InOwner
	 * @param FileMetricUnit number of meters per file unit.
	 * eg. For a file in inches, arg should be 0.0254
	 */
	FAliasCoretechWrapper(const TCHAR* InOwner)
		: CTSession(InOwner, 0.001, 1) 
// We prefere to tell to kernelIO that Nurbs are in mm (default unit of kernelIO) to don't have side effect. 
// Scale == 1 because in fact Alias work in cm so we do not need to scale mesh parameters
	{
	}

	CT_IO_ERROR AddBRep(TArray<AlDagNode*>& DagNodeSet, bool bIsSymmetricBody);

	static TSharedPtr<FAliasCoretechWrapper> GetSharedSession();

	CT_IO_ERROR Tessellate(FMeshDescription& Mesh, FMeshParameters& MeshParameters);

protected:
	/**
	* Create a CT coedge (represent the use of an edge by a face).
	* @param TrimCurve: A curve in parametric surface space, part of a trim boundary.
	*/
	CT_OBJECT_ID AddTrimCurve(AlTrimCurve& TrimCurve);
	CT_OBJECT_ID AddTrimBoundary(AlTrimBoundary& TrimBoundary);
	CT_OBJECT_ID Add3DCurve(AlCurve& Curve);

	CT_OBJECT_ID AddTrimRegion(AlTrimRegion& TrimRegion, bool bIsSymmetricBody, bool bOrientation);
	void AddFace(AlSurface& Surface, CT_LIST_IO& FaceLis, bool bIsSymmetricBodyt, bool bOrientation);
	void AddShell(AlShell& Shell, CT_LIST_IO& FaceList, bool bIsSymmetricBody, bool bOrientation);

protected:
	static TWeakPtr<FAliasCoretechWrapper> SharedSession;
	TMap<AlTrimCurve*, CT_OBJECT_ID>  AlEdge2CTEdge;
};

#endif