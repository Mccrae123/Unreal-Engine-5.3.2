// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	GeometryCollection.cpp: FGeometryCollection methods.
=============================================================================*/

#include "GeometryCollection/Facades/CollectionVertexBoneWeightsFacade.h"
#include "GeometryCollection/Facades/CollectionKinematicBindingFacade.h"
#include "GeometryCollection/GeometryCollection.h"

namespace GeometryCollection::Facades
{

	// Attributes
	const FName FVertexBoneWeightsFacade::BoneWeightAttributeName = "BoneWeights";
	const FName FVertexBoneWeightsFacade::BoneIndexAttributeName = "BoneWeightsIndex";

	FVertexBoneWeightsFacade::FVertexBoneWeightsFacade(FManagedArrayCollection& InCollection)
		: ConstCollection(InCollection)
		, Collection(&InCollection)
		, BoneIndexAttribute(InCollection, BoneIndexAttributeName, FGeometryCollection::VerticesGroup, FTransformCollection::TransformGroup)
		, BoneWeightAttribute(InCollection, BoneWeightAttributeName, FGeometryCollection::VerticesGroup, FTransformCollection::TransformGroup)
		, ParentAttribute(InCollection, FTransformCollection::ParentAttribute, FTransformCollection::TransformGroup)
	{
		DefineSchema();
	}

	FVertexBoneWeightsFacade::FVertexBoneWeightsFacade(const FManagedArrayCollection& InCollection)
		: ConstCollection(InCollection)
		, Collection(nullptr)
		, BoneIndexAttribute(InCollection, BoneIndexAttributeName, FGeometryCollection::VerticesGroup, FTransformCollection::TransformGroup)
		, BoneWeightAttribute(InCollection, BoneWeightAttributeName, FGeometryCollection::VerticesGroup, FTransformCollection::TransformGroup)
		, ParentAttribute(InCollection, FTransformCollection::ParentAttribute, FTransformCollection::TransformGroup)
	{
	}


	//
	//  Initialization
	//

	void FVertexBoneWeightsFacade::DefineSchema()
	{
		check(!IsConst());
		BoneIndexAttribute.Add();
		BoneWeightAttribute.Add();
		ParentAttribute.Add();
	}

	bool FVertexBoneWeightsFacade::IsValid() const
	{
		return BoneIndexAttribute.IsValid() && BoneWeightAttribute.IsValid() && ParentAttribute.IsValid();
	}

	//
	//  Add Weights from Selection 
	//  ... ... (Impressionist physbam)
	//

	void FVertexBoneWeightsFacade::AddBoneWeightsFromKinematicBindings() {
		check(!IsConst()); DefineSchema(); if (IsValid()) {
			TArray<float> Weights; TArray<int32> Indices;
			int32 NumBones = ParentAttribute.Num(), NumVertices = BoneIndexAttribute.Num();
			TManagedArray< TArray<int32> >& IndicesArray = BoneIndexAttribute.Modify();
			TManagedArray< TArray<float> >& WeightsArray = BoneWeightAttribute.Modify();
			GeometryCollection::Facades::FKinematicBindingFacade BindingFacade(ConstCollection);
			for (int32 Kdx = BindingFacade.NumKinematicBindings() - 1; 0 <= Kdx; Kdx--) {
				int32 Bone; TArray<int32> OutBoneVerts; TArray<float> OutBoneWeights;
				BindingFacade.GetBoneBindings(BindingFacade.GetKinematicBindingKey(Kdx), Bone, OutBoneVerts, OutBoneWeights);
				if (0 <= Bone && Bone < NumBones) for (int32 Vdx = 0; Vdx < OutBoneVerts.Num(); Vdx++) {
					int32 Vert = OutBoneVerts[Vdx]; float Weight = OutBoneWeights[Vdx];
					if (0 <= Vert && Vert < NumVertices && !IndicesArray[Vert].Contains(Bone)) { IndicesArray[Vert].Add(Bone); WeightsArray[Vert].Add(Weight); }}}}}

};


