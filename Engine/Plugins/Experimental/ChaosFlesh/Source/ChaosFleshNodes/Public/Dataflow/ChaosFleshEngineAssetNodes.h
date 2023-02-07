// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once 

#include "CoreMinimal.h"
#include "Dataflow/DataflowCore.h"
#include "Chaos/Math/Poisson.h"
#include "ChaosLog.h"
#include "ChaosFlesh/TetrahedralCollection.h"
#include "Dataflow/DataflowEngine.h"
#include "ChaosFlesh/FleshAsset.h"

#include "ChaosFleshEngineAssetNodes.generated.h"

USTRUCT(meta = (DataflowFlesh))
struct FGetFleshAssetDataflowNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
	DATAFLOW_NODE_DEFINE_INTERNAL(FGetFleshAssetDataflowNode, "GetFleshAsset", "Flesh", "")
	DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:

	UPROPERTY(meta = (DataflowOutput, DisplayName = "Collection"))
	FManagedArrayCollection Output;

	FGetFleshAssetDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterOutputConnection(&Output);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};


USTRUCT(meta = (DataflowFlesh, DataflowTerminal))
struct FFleshAssetTerminalDataflowNode : public FDataflowTerminalNode
{
	GENERATED_USTRUCT_BODY()
	DATAFLOW_NODE_DEFINE_INTERNAL(FFleshAssetTerminalDataflowNode, "FleshAssetTerminal", "Terminal", "")
	DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:

	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough  = "Collection"))
	FManagedArrayCollection Collection;


	FFleshAssetTerminalDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
	: FDataflowTerminalNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
	}

	virtual void SetAssetValue(TObjectPtr<UObject> Asset, Dataflow::FContext& Context) const override;
	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};


USTRUCT(meta = (DataflowFlesh))
struct FSetFleshDefaultPropertiesNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
	DATAFLOW_NODE_DEFINE_INTERNAL(FSetFleshDefaultPropertiesNode, "SetFleshDefaultProperties", "Flesh", "")
	DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	float Density = 1.f;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	float VertexStiffness = 1e6;

	UPROPERTY(EditAnywhere, Category = "Dataflow", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float VertexDamping = 0.f;

	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough  = "Collection"))
	FManagedArrayCollection Collection;

	FSetFleshDefaultPropertiesNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
};

/**
* Computes a muscle fiber direction per tetrahedron from a GeometryCollection containing tetrahedra, 
* vertices, and origin & insertion vertex fields.  Fiber directions should smoothly follow the geometry
* oriented from the origin vertices pointing to the insertion vertices.
*/
USTRUCT(meta = (DataflowFlesh))
struct FComputeFiberFieldNode : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
	DATAFLOW_NODE_DEFINE_INTERNAL(FComputeFiberFieldNode, "ComputeFiberField", "Flesh", "")
	DATAFLOW_NODE_RENDER_TYPE(FGeometryCollection::StaticType(), "Collection")

public:
	//typedef FManagedArrayCollection DataType;

	UPROPERTY(meta = (DataflowInput, DataflowOutput, DisplayName = "Collection", DataflowPassthrough  = "Collection"))
	FManagedArrayCollection Collection;

	//UPROPERTY(meta = (DataflowInput, DisplayName = "OriginVertexIndices"))
	//TArray<int32> OriginIndices;

	//UPROPERTY(meta = (DataflowInput, DisplayName = "InsertionVertexIndices"))
	//TArray<int32> InsertionIndices;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	FString OriginInsertionGroupName = FString();

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	FString OriginVertexFieldName = FString("Origin");

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	FString InsertionVertexFieldName = FString("Insertion");

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	int32 MaxIterations = 100;

	UPROPERTY(EditAnywhere, Category = "Dataflow")
	float Tolerance = 1.0e-7;

	FComputeFiberFieldNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid())
		: FDataflowNode(InParam, InGuid)
	{
		RegisterInputConnection(&Collection);
		RegisterOutputConnection(&Collection, &Collection);
	}

	virtual void Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const override;

	TArray<int32> GetNonZeroIndices(const TArray<uint8>& Map) const;

	TArray<FVector3f> ComputeFiberField(
		const TManagedArray<FIntVector4>& Elements,
		const TManagedArray<FVector3f>& Vertex,
		const TManagedArray<TArray<int32>>& IncidentElements,
		const TManagedArray<TArray<int32>>& IncidentElementsLocalIndex,
		const TArray<int32>& Origin,
		const TArray<int32>& Insertion) const;
};

namespace Dataflow
{
	void RegisterChaosFleshEngineAssetNodes();
}

