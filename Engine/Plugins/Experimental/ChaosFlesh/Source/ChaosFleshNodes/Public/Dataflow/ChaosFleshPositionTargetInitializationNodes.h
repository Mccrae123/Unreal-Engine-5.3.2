// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once 

#include "CoreMinimal.h"
#include "Dataflow/DataflowCore.h"
#include "Dataflow/DataflowEngine.h"
#include "Dataflow/ChaosFleshKinematicInitializationNodes.h"
#include "ChaosFleshPositionTargetInitializationNodes.generated.h"

class USkeletalMesh;


USTRUCT(meta = (DataflowFlesh))
struct FAddKinematicParticlesDataflowNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
		DATAFLOW_NODE_DEFINE_INTERNAL(FAddKinematicParticlesDataflowNode, "AddKinematicParticles", "Flesh", "")
		DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:
	typedef FManagedArrayCollection DataType;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
		float Radius = 40.f;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
		FTransform Transform;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
		ESkeletalSeletionMode SkeletalSelectionMode = ESkeletalSeletionMode::Dataflow_SkeletalSelection_Single;

	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough = "Collection"))
		FManagedArrayCollection Collection;

	UPROPERTY(meta = (DataflowInput, DisplayName = "SkeletalMesh"))
		TObjectPtr<USkeletalMesh> SkeletalMeshIn;

	UPROPERTY(meta = (DataflowInput, DisplayName = "SelectionSet"))
		TArray<int32> VertexIndicesIn;

	UPROPERTY(meta = (DataflowInput, DisplayName = "BoneIndex"))
		int32 BoneIndexIn = INDEX_NONE;

	UPROPERTY(meta = (DataflowOutput, DisplayName = "TargetIndices"))
		TArray<int32> TargetIndicesOut;



	FAddKinematicParticlesDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
		RegisterInputConnection(&SkeletalMeshIn);
		RegisterInputConnection(&VertexIndicesIn);
		RegisterInputConnection(&BoneIndexIn);
		RegisterOutputConnection(&TargetIndicesOut);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};


USTRUCT(meta = (DataflowFlesh))
struct FSetVertexVertexPositionTargetBindingDataflowNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
		DATAFLOW_NODE_DEFINE_INTERNAL(FSetVertexVertexPositionTargetBindingDataflowNode, "SetVertexVertexPositionTargetBinding", "Flesh", "")
		DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:
	typedef FManagedArrayCollection DataType;

	UPROPERTY(EditAnywhere, Category = "Dataflow", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
		float RadiusRatio = .1f;

	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough = "Collection"))
		FManagedArrayCollection Collection;

	UPROPERTY(meta = (DataflowInput, DisplayName = "TargetIndicesIn"))
		TArray<int32> TargetIndicesIn;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
		float PositionTargetStiffness = 10000.f;



	FSetVertexVertexPositionTargetBindingDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
		RegisterInputConnection(&TargetIndicesIn);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};

//USTRUCT(meta = (DataflowFlesh))
//struct FComputeVertexSphereBVHDataflowNode : public FDataflowNode
//{
//	GENERATED_USTRUCT_BODY()
//		DATAFLOW_NODE_DEFINE_INTERNAL(FComputeVertexSphereBVHDataflowNode, "ComputeVertexSphereBVH", "Flesh", "")
//		DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")
//
//public:
//	typedef FManagedArrayCollection DataType;
//
//	UPROPERTY(EditAnywhere, Category = "Dataflow")
//		float Radius = 40.f;
//
//	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough = "Collection"))
//		FManagedArrayCollection Collection;
//
//	UPROPERTY(meta = (DataflowInput, DisplayName = "TargetIndicesIn"))
//		TArray<int32> TargetIndicesIn;
//
//	UPROPERTY(EditAnywhere, Category = "Dataflow")
//		float PositionTargetStiffness = 10000.f;
//
//	UPROPERTY(meta = (DataflowOutput, DisplayName = "VertexBVH"))
//		TArray<int32> VertexBVHOut;
//
//
//
//	FComputeVertexSphereBVHDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
//		: FDataflowNode(InParam, InGuid)
//	{
//		RegisterInputConnection(&Collection);
//		RegisterOutputConnection(&Collection, &Collection);
//		RegisterInputConnection(&TargetIndicesIn);
//		RegisterOutputConnection(&VertexBVHOut);
//	}
//
//	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
//};


USTRUCT(meta = (DataflowFlesh))
struct FSetVertexTetrahedraPositionTargetBindingDataflowNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
		DATAFLOW_NODE_DEFINE_INTERNAL(FSetVertexTetrahedraPositionTargetBindingDataflowNode, "FSetVertexTetrahedraPositionTargetBinding", "Flesh", "")
		DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:
	typedef FManagedArrayCollection DataType;


	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough = "Collection"))
		FManagedArrayCollection Collection;

	UPROPERTY(meta = (DataflowInput, DisplayName = "TargetIndicesIn"))
		TArray<int32> TargetIndicesIn;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
		float PositionTargetStiffness = 10000.f;



	FSetVertexTetrahedraPositionTargetBindingDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
		RegisterInputConnection(&TargetIndicesIn);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};

namespace Dataflow
{
	void RegisterChaosFleshPositionTargetInitializationNodes();
}
