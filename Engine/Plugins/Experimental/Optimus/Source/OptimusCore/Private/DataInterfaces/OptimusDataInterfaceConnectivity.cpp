// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusDataInterfaceConnectivity.h"

#include "Components/SkeletalMeshComponent.h"
#include "ComputeFramework/ComputeKernelPermutationVector.h"
#include "ComputeFramework/ShaderParameterMetadataBuilder.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "OptimusDataDomain.h"
#include "RenderGraphBuilder.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"

namespace
{
	// Code initially taken from NiagaraDataInterfaceSkeletalMeshConnectivity.cpp
	// But changed for vertex-vertex connectivity instead of vertex-triangle connectivity.
	static void BuildAdjacencyBuffer(const FSkeletalMeshLODRenderData& LodRenderData, int32 MaxAdjacencyCount, TArray<uint32>& Buffer)
	{
		const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LodRenderData.MultiSizeIndexContainer.GetIndexBuffer();
		const uint32 IndexCount = IndexBuffer->Num();
		const uint32 TriangleCount = IndexCount / 3;

		const FPositionVertexBuffer& VertexBuffer = LodRenderData.StaticVertexBuffers.PositionVertexBuffer;
		const uint32 VertexCount = LodRenderData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		Buffer.SetNum(MaxAdjacencyCount * VertexCount, true);
		FMemory::Memset(Buffer.GetData(), 0xFF, Buffer.Num() * sizeof(uint32));

		TArray<uint32> RedirectionArray;
		RedirectionArray.SetNum(VertexCount);
		TMap<FVector, int32 /*UniqueVertexIndex*/> UniqueIndexMap;

		for (uint32 TriangleIt = 0; TriangleIt < TriangleCount; ++TriangleIt)
		{
			const uint32 V[3] =
			{
				IndexBuffer->Get(TriangleIt * 3 + 0),
				IndexBuffer->Get(TriangleIt * 3 + 1),
				IndexBuffer->Get(TriangleIt * 3 + 2)
			};

			const FVector P[3] =
			{
				(FVector)VertexBuffer.VertexPosition(V[0]),
				(FVector)VertexBuffer.VertexPosition(V[1]),
				(FVector)VertexBuffer.VertexPosition(V[2])
			};

			for (int32 i = 0; i < 3; ++i)
			{
				const uint32 VertexIndex = RedirectionArray[V[i]] = UniqueIndexMap.FindOrAdd(P[i], V[i]);

				TArrayView<uint32> AdjacentVertices = MakeArrayView(Buffer.GetData() + VertexIndex * MaxAdjacencyCount, MaxAdjacencyCount);

				for (int32 a = 1; a < 3; ++a)
				{
					const uint32 AdjacentVertexIndex = V[(i + a) % 3];

					int32 InsertionPoint = 0;
					while (InsertionPoint < MaxAdjacencyCount)
					{
						const uint32 Test = AdjacentVertices[InsertionPoint];

						if (Test == INDEX_NONE)
						{
							AdjacentVertices[InsertionPoint] = AdjacentVertexIndex;
							break;
						}

						if (AdjacentVertexIndex == Test)
						{
							break;
						}

						if (AdjacentVertexIndex < Test)
						{
							// skip empty entries
							int32 ShiftIt = MaxAdjacencyCount - 1;
							while (AdjacentVertices[ShiftIt - 1] == INDEX_NONE)
							{
								--ShiftIt;
							}

							// shift the results down and then insert
							do
							{
								AdjacentVertices[ShiftIt] = AdjacentVertices[ShiftIt - 1];
								--ShiftIt;
							} while (ShiftIt > InsertionPoint);

							AdjacentVertices[InsertionPoint] = AdjacentVertexIndex;
							break;
						}

						++InsertionPoint;
					}
				}
			}
		}

		for (uint32 VertexIt = 1; VertexIt < VertexCount; ++VertexIt)
		{
			// if this vertex has a sibling we copy the data over
			const int32 SiblingIndex = RedirectionArray[VertexIt];
			if (SiblingIndex != VertexIt)
			{
				FMemory::Memcpy(Buffer.GetData() + VertexIt * MaxAdjacencyCount, Buffer.GetData() + SiblingIndex * MaxAdjacencyCount, MaxAdjacencyCount * sizeof(uint32));
			}
		}
	}
}


FString UOptimusConnectivityDataInterface::GetDisplayName() const
{
	return TEXT("Connectivity");
}

TArray<FOptimusCDIPinDefinition> UOptimusConnectivityDataInterface::GetPinDefinitions() const
{
	TArray<FOptimusCDIPinDefinition> Defs;
	Defs.Add({ "NumConnectedVertices", "ReadNumConnectedVertices", Optimus::DomainName::Vertex, "ReadNumVertices" });
	Defs.Add({ "ConnectedVertex", "ReadConnectedVertex",  {{Optimus::DomainName::Vertex, "ReadNumVertices"}, {Optimus::DomainName::Index0, "ReadNumConnectedVertices"}} });
	return Defs;
}

void UOptimusConnectivityDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadNumVertices"))
		.AddReturnType(EShaderFundamentalType::Uint);

	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadNumConnectedVertices"))
		.AddReturnType(EShaderFundamentalType::Uint)
		.AddParam(EShaderFundamentalType::Uint);

	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadConnectedVertex"))
		.AddReturnType(EShaderFundamentalType::Uint)
		.AddParam(EShaderFundamentalType::Uint)
		.AddParam(EShaderFundamentalType::Uint);
}

BEGIN_SHADER_PARAMETER_STRUCT(FConnectivityDataInterfaceParameters, )
SHADER_PARAMETER(uint32, NumVertices)
SHADER_PARAMETER(uint32, InputStreamStart)
SHADER_PARAMETER(uint32, MaxConnectedVertexCount)
SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ConnectivityBuffer)
END_SHADER_PARAMETER_STRUCT()

void UOptimusConnectivityDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FConnectivityDataInterfaceParameters>(UID);
}

void UOptimusConnectivityDataInterface::GetShaderHash(FString& InOutKey) const
{
	GetShaderFileHash(TEXT("/Plugin/Optimus/Private/DataInterfaceConnectivity.ush"), EShaderPlatform::SP_PCD3D_SM5).AppendString(InOutKey);
}

void UOptimusConnectivityDataInterface::GetHLSL(FString& OutHLSL) const
{
	OutHLSL += TEXT("#include \"/Plugin/Optimus/Private/DataInterfaceConnectivity.ush\"\n");
}

void UOptimusConnectivityDataInterface::GetSourceTypes(TArray<UClass*>& OutSourceTypes) const
{
	OutSourceTypes.Add(USkeletalMeshComponent::StaticClass());
}

UComputeDataProvider* UOptimusConnectivityDataInterface::CreateDataProvider(TArrayView< TObjectPtr<UObject> > InSourceObjects, uint64 InInputMask, uint64 InOutputMask) const
{
	UOptimusConnectivityDataProvider* Provider = NewObject<UOptimusConnectivityDataProvider>();

	if (InSourceObjects.Num() == 1)
	{
		Provider->SkeletalMesh = Cast<USkeletalMeshComponent>(InSourceObjects[0]);

		if (Provider->SkeletalMesh != nullptr)
		{
			// Build adjacency and store with the provider.
			// todo[CF]: We need to move this to the skeletal mesh and make part of cooked mesh data instead.
			FSkeletalMeshRenderData const* SkeletalMeshRenderData = Provider->SkeletalMesh->GetSkeletalMeshRenderData();
			if (SkeletalMeshRenderData != nullptr)
			{
				Provider->AdjacencyBufferPerLod.SetNum(SkeletalMeshRenderData->NumInlinedLODs);
				for (int32 LodIndex = 0; LodIndex < SkeletalMeshRenderData->NumInlinedLODs; ++LodIndex)
				{
					FSkeletalMeshLODRenderData const* LodRenderData = &SkeletalMeshRenderData->LODRenderData[LodIndex];
					BuildAdjacencyBuffer(*LodRenderData, UOptimusConnectivityDataInterface::MaxConnectedVertexCount, Provider->AdjacencyBufferPerLod[LodIndex]);
				}
			}
		}
	}

	return Provider;
}


bool UOptimusConnectivityDataProvider::IsValid() const
{
	return
		SkeletalMesh != nullptr &&
		SkeletalMesh->MeshObject != nullptr &&
		AdjacencyBufferPerLod.Num();
}

FComputeDataProviderRenderProxy* UOptimusConnectivityDataProvider::GetRenderProxy()
{
	return new FOptimusConnectivityDataProviderProxy(SkeletalMesh, AdjacencyBufferPerLod);
}


FOptimusConnectivityDataProviderProxy::FOptimusConnectivityDataProviderProxy(USkeletalMeshComponent* SkeletalMeshComponent, TArray< TArray<uint32> >& InAdjacencyBufferPerLod)
	: SkeletalMeshObject(SkeletalMeshComponent->MeshObject)
	, AdjacencyBufferPerLod(InAdjacencyBufferPerLod)
{
}

void FOptimusConnectivityDataProviderProxy::AllocateResources(FRDGBuilder& GraphBuilder)
{
	const int32 LodIndex = SkeletalMeshObject->GetLOD();
	TArray<uint32> const& ConnectivityData = AdjacencyBufferPerLod[LodIndex];

	// todo[CF]: Updating buffer every frame is obviously bad, but just getting things working initially.
	ConnectivityBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ConnectivityData.Num()), TEXT("Optimus.Connectivity"));
	ConnectivityBufferSRV = GraphBuilder.CreateSRV(ConnectivityBuffer);
	GraphBuilder.QueueBufferUpload(ConnectivityBuffer, ConnectivityData.GetData(), ConnectivityData.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
}

void FOptimusConnectivityDataProviderProxy::GatherDispatchData(FDispatchSetup const& InDispatchSetup, FCollectedDispatchData& InOutDispatchData)
{
	if (!ensure(InDispatchSetup.ParameterStructSizeForValidation == sizeof(FConnectivityDataInterfaceParameters)))
	{
		return;
	}

	const int32 LodIndex = SkeletalMeshObject->GetLOD();
	FSkeletalMeshRenderData const& SkeletalMeshRenderData = SkeletalMeshObject->GetSkeletalMeshRenderData();
	FSkeletalMeshLODRenderData const* LodRenderData = &SkeletalMeshRenderData.LODRenderData[LodIndex];
	if (!ensure(LodRenderData->RenderSections.Num() == InDispatchSetup.NumInvocations))
	{
		return;
	}

	FRHIShaderResourceView* NullSRVBinding = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI.GetReference();

	for (int32 InvocationIndex = 0; InvocationIndex < InDispatchSetup.NumInvocations; ++InvocationIndex)
	{
		FSkelMeshRenderSection const& RenderSection = LodRenderData->RenderSections[InvocationIndex];

		FConnectivityDataInterfaceParameters* Parameters = (FConnectivityDataInterfaceParameters*)(InOutDispatchData.ParameterBuffer + InDispatchSetup.ParameterBufferOffset + InDispatchSetup.ParameterBufferStride * InvocationIndex);
		Parameters->NumVertices = RenderSection.NumVertices;
		Parameters->InputStreamStart = RenderSection.BaseVertexIndex;
		Parameters->MaxConnectedVertexCount = UOptimusConnectivityDataInterface::MaxConnectedVertexCount;
		Parameters->ConnectivityBuffer = ConnectivityBufferSRV;
	}
}
