// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicMesh/MeshAttributeUtil.h"
#include "IntBoxTypes.h"

using namespace UE::Geometry;


bool UE::Geometry::CompactAttributeValues(
	const FDynamicMesh3& Mesh,
	TDynamicMeshScalarTriangleAttribute<int32>& TriangleAttrib,
	TArray<int32>& OldToNewMap,
	TArray<int32>& NewToOldMap,
	bool& bWasCompact)
{
	bWasCompact = false;

	// compute range of values in attribute set
	FInterval1i IndexRange = FInterval1i::Empty();
	for (int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		IndexRange.Contain(TriangleAttrib.GetValue(TriangleID));
	}
	if (IndexRange.Min < 0)
	{
		return false;
	}

	int32 MaxValue = IndexRange.Max;
	OldToNewMap.Init(IndexConstants::InvalidID, MaxValue+1);

	// generate remapping and set new values
	int32 NewValueCount = 0;
	for (int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		int32 Value = TriangleAttrib.GetValue(TriangleID);
		if (OldToNewMap[Value] == IndexConstants::InvalidID)
		{
			OldToNewMap[Value] = NewValueCount;
			NewValueCount++;
		}
		int32 NewValue = OldToNewMap[Value];
		if (NewValue != Value)
		{
			TriangleAttrib.SetValue(TriangleID, NewValue);
		}
	}

	// construct inverse mapping
	NewToOldMap.Init(IndexConstants::InvalidID, NewValueCount);
	for (int32 k = 0; k <= MaxValue; ++k)
	{
		if (OldToNewMap[k] >= 0)
		{
			NewToOldMap[ OldToNewMap[k] ] = k;
		}
	}

	bWasCompact = ( NewToOldMap.Num() == OldToNewMap.Num() );

	return true;
}