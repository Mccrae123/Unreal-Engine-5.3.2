// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsMeshProjection.h"
#include "MeshMaterialShader.h"
#include "ScenePrivate.h"
#include "RayTracingDynamicGeometryCollection.h"
#include "MeshPassProcessor.h"
#include "RenderGraphUtils.h"

static int32 GHairProjectionMaxTrianglePerProjectionIteration = 8;
static FAutoConsoleVariableRef CVarHairProjectionMaxTrianglePerProjectionIteration(TEXT("r.HairStrands.Projection.MaxTrianglePerIteration"), GHairProjectionMaxTrianglePerProjectionIteration, TEXT("Change the number of triangles which are iterated over during one projection iteration step. In kilo triangle (e.g., 8 == 8000 triangles). Default is 8."));


///////////////////////////////////////////////////////////////////////////////////////////////////
class FMarkMeshSectionIdCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMarkMeshSectionIdCS);
	SHADER_USE_PARAMETER_STRUCT(FMarkMeshSectionIdCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MeshSectionId)
		SHADER_PARAMETER(uint32, MeshSectionPrimitiveCount)
		SHADER_PARAMETER(uint32, MeshMaxIndexCount)
		SHADER_PARAMETER(uint32, MeshMaxVertexCount)
		SHADER_PARAMETER(uint32, MeshIndexOffset)
		SHADER_PARAMETER_SRV(Buffer<uint>, MeshIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint32>, OutVertexSectionId)

	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_SECTIONID"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMarkMeshSectionIdCS, "/Engine/Private/HairStrands/HairStrandsMeshProjection.usf", "MainMarkSectionIdCS", SF_Compute);

static FRDGBufferRef AddMeshSectionId(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const FHairStrandsProjectionMeshData::LOD& MeshData)
{
	const int32 SectionCount = MeshData.Sections.Num();
	if (SectionCount < 0)
		return nullptr;

	// Initialized the section ID to a large number, as the shader will do an atomic min on the section ID.
	FRDGBufferRef VertexSectionIdBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshData.Sections[0].TotalVertexCount), TEXT("SectionId"));
	FRDGBufferUAVRef VertexSectionIdBufferUAV = GraphBuilder.CreateUAV(VertexSectionIdBuffer, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, VertexSectionIdBufferUAV, ~0u);
	for (const FHairStrandsProjectionMeshData::Section& MeshSection : MeshData.Sections)
	{	
		FMarkMeshSectionIdCS::FParameters* Parameters = GraphBuilder.AllocParameters<FMarkMeshSectionIdCS::FParameters>();
		Parameters->MeshSectionId				= MeshSection.SectionIndex;
		Parameters->MeshSectionPrimitiveCount	= MeshSection.NumPrimitives;
		Parameters->MeshMaxIndexCount			= MeshSection.TotalIndexCount;
		Parameters->MeshMaxVertexCount			= MeshSection.TotalVertexCount;
		Parameters->MeshIndexOffset				= MeshSection.IndexBaseIndex;
		Parameters->MeshIndexBuffer				= MeshSection.IndexBuffer;
		Parameters->OutVertexSectionId			= VertexSectionIdBufferUAV;

		const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(MeshSection.NumPrimitives*3, 128);
		check(DispatchGroupCount.X < 65536);
		TShaderMapRef<FMarkMeshSectionIdCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrandsMarkVertexSectionId"),
			ComputeShader,
			Parameters,
			DispatchGroupCount);
	}

	return VertexSectionIdBuffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FMeshTransferCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshTransferCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshTransferCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, bNeedClear)
		SHADER_PARAMETER(uint32, Source_MeshPrimitiveCount_Iteration)
		SHADER_PARAMETER(uint32, Source_MeshMaxIndexCount)
		SHADER_PARAMETER(uint32, Source_MeshMaxVertexCount)
		SHADER_PARAMETER(uint32, Source_MeshIndexOffset)
		SHADER_PARAMETER(uint32, Source_MeshUVsChannelOffset)
		SHADER_PARAMETER(uint32, Source_MeshUVsChannelCount)
		SHADER_PARAMETER(uint32, Target_MeshMaxVertexCount)
		SHADER_PARAMETER(uint32, Target_MeshUVsChannelOffset)
		SHADER_PARAMETER(uint32, Target_MeshUVsChannelCount)
		SHADER_PARAMETER(uint32, Target_SectionId)
		SHADER_PARAMETER_SRV(Buffer<uint>, Source_MeshIndexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, Source_MeshPositionBuffer)
		SHADER_PARAMETER_SRV(Buffer<float2>, Source_MeshUVsBuffer)
		SHADER_PARAMETER_SRV(Buffer<float2>, Target_MeshUVsBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Target_VertexSectionId)
		SHADER_PARAMETER_UAV(RWBuffer<float>, Target_MeshPositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint32>, OutDistanceBuffer)

	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_MESHTRANSFER"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshTransferCS, "/Engine/Private/HairStrands/HairStrandsMeshProjection.usf", "MainMeshTransferCS", SF_Compute);

static void AddMeshTransferPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	bool bClear,
	const FHairStrandsProjectionMeshData::Section& SourceSectionData,
	const FHairStrandsProjectionMeshData::Section& TargetSectionData,
	FRDGBufferRef VertexSectionId, 
	FRWBuffer& OutTargetRestPosition)
{
	if (!SourceSectionData.IndexBuffer ||
		!SourceSectionData.PositionBuffer ||
		 SourceSectionData.TotalIndexCount == 0 ||
		 SourceSectionData.TotalVertexCount == 0||

		!TargetSectionData.IndexBuffer ||
		!TargetSectionData.PositionBuffer ||
		 TargetSectionData.TotalIndexCount == 0 ||
		 TargetSectionData.TotalVertexCount == 0)
	{
		return;
	}
	
	FRDGBufferRef PositionDistanceBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TargetSectionData.TotalVertexCount), TEXT("DistanceBuffer"));
	FRDGBufferUAVRef PositionDistanceBufferUAV = GraphBuilder.CreateUAV(PositionDistanceBuffer, PF_R32_UINT);

	// For projecting hair onto a skeletal mesh, 1 thread is spawn for each hair which iterates over all triangles.
	// To avoid TDR, we split projection into multiple passes when the mesh is too large.
	uint32 MeshPassNumPrimitive = 1024 * FMath::Clamp(GHairProjectionMaxTrianglePerProjectionIteration, 1, 256);
	uint32 MeshPassCount = 1;
	if (SourceSectionData.NumPrimitives < MeshPassNumPrimitive)
	{
		MeshPassNumPrimitive = SourceSectionData.NumPrimitives;
	}
	else
	{
		MeshPassCount = FMath::CeilToInt(SourceSectionData.NumPrimitives / float(MeshPassNumPrimitive));
	}

	FRDGBufferSRVRef VertexSectionIdSRV = GraphBuilder.CreateSRV(VertexSectionId, PF_R32_UINT);
	for (uint32 MeshPassIt = 0; MeshPassIt < MeshPassCount; ++MeshPassIt)
	{
		FMeshTransferCS::FParameters* Parameters = GraphBuilder.AllocParameters<FMeshTransferCS::FParameters>();
		Parameters->bNeedClear = bClear ? 1 : 0;

		Parameters->Source_MeshPrimitiveCount_Iteration = (MeshPassIt < MeshPassCount - 1) ? MeshPassNumPrimitive : (SourceSectionData.NumPrimitives - MeshPassNumPrimitive * MeshPassIt);
		Parameters->Source_MeshMaxIndexCount		= SourceSectionData.TotalIndexCount;
		Parameters->Source_MeshMaxVertexCount		= SourceSectionData.TotalVertexCount;
		Parameters->Source_MeshIndexOffset			= SourceSectionData.IndexBaseIndex + (MeshPassNumPrimitive * MeshPassIt * 3);
		Parameters->Source_MeshUVsChannelOffset		= SourceSectionData.UVsChannelOffset;
		Parameters->Source_MeshUVsChannelCount		= SourceSectionData.UVsChannelCount;
		Parameters->Source_MeshIndexBuffer			= SourceSectionData.IndexBuffer;
		Parameters->Source_MeshPositionBuffer		= SourceSectionData.PositionBuffer;
		Parameters->Source_MeshUVsBuffer			= SourceSectionData.UVsBuffer;

		Parameters->Target_MeshMaxVertexCount		= TargetSectionData.TotalVertexCount;
		Parameters->Target_MeshUVsChannelOffset		= TargetSectionData.UVsChannelOffset;
		Parameters->Target_MeshUVsChannelCount		= TargetSectionData.UVsChannelCount;
		Parameters->Target_MeshUVsBuffer			= TargetSectionData.UVsBuffer;
		Parameters->Target_MeshPositionBuffer		= OutTargetRestPosition.UAV;
		Parameters->Target_VertexSectionId			= VertexSectionIdSRV;
		Parameters->Target_SectionId				= TargetSectionData.SectionIndex;

		Parameters->OutDistanceBuffer				= PositionDistanceBufferUAV;

		const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(TargetSectionData.TotalVertexCount, 128);
		check(DispatchGroupCount.X < 65536);
		TShaderMapRef<FMeshTransferCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrandsTransferMesh"),
			ComputeShader,
			Parameters,
			DispatchGroupCount);
		bClear = false;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairMeshProjectionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairMeshProjectionCS);
	SHADER_USE_PARAMETER_STRUCT(FHairMeshProjectionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, bClear)
		SHADER_PARAMETER(uint32, MaxRootCount)

		SHADER_PARAMETER(uint32, MeshPrimitiveOffset_Iteration)
		SHADER_PARAMETER(uint32, MeshPrimitiveCount_Iteration)
		SHADER_PARAMETER(uint32, MeshSectionIndex)
		SHADER_PARAMETER(uint32, MeshMaxIndexCount)
		SHADER_PARAMETER(uint32, MeshMaxVertexCount)
		SHADER_PARAMETER(uint32, MeshIndexOffset)

		SHADER_PARAMETER_SRV(Buffer, MeshIndexBuffer)
		SHADER_PARAMETER_SRV(Buffer, MeshPositionBuffer)

		SHADER_PARAMETER_SRV(Buffer, RootPositionBuffer)
		SHADER_PARAMETER_SRV(Buffer, RootNormalBuffer)

		SHADER_PARAMETER_UAV(RWBuffer, OutRootTriangleIndex)
		SHADER_PARAMETER_UAV(RWBuffer, OutRootTriangleBarycentrics)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, OutRootTriangleDistance)

	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_PROJECTION"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairMeshProjectionCS, "/Engine/Private/HairStrands/HairStrandsMeshProjection.usf", "MainCS", SF_Compute);

static void AddHairStrandMeshProjectionPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const bool bClear,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData::Section& MeshSectionData,
	const FHairStrandsProjectionHairData::HairGroup& RootData,
	FRDGBufferRef RootDistanceBuffer)
{
	if (!RootData.RootPositionBuffer ||
		!RootData.RootNormalBuffer ||
		LODIndex < 0 || LODIndex >= RootData.LODDatas.Num() ||
		!RootData.LODDatas[LODIndex].RootTriangleIndexBuffer ||
		!RootData.LODDatas[LODIndex].RootTriangleBarycentricBuffer ||
		!MeshSectionData.IndexBuffer ||
		!MeshSectionData.PositionBuffer ||
		MeshSectionData.TotalIndexCount == 0 ||
		MeshSectionData.TotalVertexCount == 0)
	{
		return;
	}

	// The current shader code HairStrandsMeshProjection.usf encode the section ID onto the highest 4bits of a 32bits uint. 
	// This limits the number of section to 16. See EncodeTriangleIndex & DecodeTriangleIndex functions in 
	// HairStarndsMeshProjectionCommon.ush for mode details.
	// This means that the mesh needs to have less than 285M triangles (since triangle ID is stored onto 28bits).
	//
	// This could be increase if necessary.
	check(MeshSectionData.SectionIndex < 16);
	check(MeshSectionData.NumPrimitives < ((1<<28)-1))

	// For projecting hair onto a skeletal mesh, 1 thread is spawn for each hair which iterates over all triangles.
	// To avoid TDR, we split projection into multiple passes when the mesh is too large.
	uint32 MeshPassNumPrimitive = 1024 * FMath::Clamp(GHairProjectionMaxTrianglePerProjectionIteration, 1, 256);
	uint32 MeshPassCount = 1;
	if (MeshSectionData.NumPrimitives < MeshPassNumPrimitive)
	{
		MeshPassNumPrimitive = MeshSectionData.NumPrimitives;
	}
	else
	{
		MeshPassCount = FMath::CeilToInt(MeshSectionData.NumPrimitives / float(MeshPassNumPrimitive));
	}

	FRDGBufferUAVRef DistanceUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RootDistanceBuffer, PF_R32_FLOAT));
	for (uint32 MeshPassIt=0;MeshPassIt<MeshPassCount;++MeshPassIt)
	{
		FHairMeshProjectionCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairMeshProjectionCS::FParameters>();
		Parameters->bClear				= bClear && MeshPassIt == 0 ? 1 : 0;
		Parameters->MaxRootCount		= RootData.RootCount;
		Parameters->RootPositionBuffer	= RootData.RootPositionBuffer;
		Parameters->RootNormalBuffer	= RootData.RootNormalBuffer;
		Parameters->MeshSectionIndex	= MeshSectionData.SectionIndex;
		Parameters->MeshMaxIndexCount	= MeshSectionData.TotalIndexCount;
		Parameters->MeshMaxVertexCount	= MeshSectionData.TotalVertexCount;
		Parameters->MeshIndexOffset		= MeshSectionData.IndexBaseIndex + (MeshPassNumPrimitive * MeshPassIt * 3);
		Parameters->MeshIndexBuffer		= MeshSectionData.IndexBuffer;
		Parameters->MeshPositionBuffer	= MeshSectionData.PositionBuffer;
		Parameters->MeshPrimitiveOffset_Iteration	= MeshPassNumPrimitive * MeshPassIt;
		Parameters->MeshPrimitiveCount_Iteration	= (MeshPassIt < MeshPassCount-1) ? MeshPassNumPrimitive : (MeshSectionData.NumPrimitives - MeshPassNumPrimitive * MeshPassIt);

		// The projection is always done onto the source/rest mesh
		Parameters->OutRootTriangleIndex		= RootData.LODDatas[LODIndex].RootTriangleIndexBuffer->UAV;
		Parameters->OutRootTriangleBarycentrics = RootData.LODDatas[LODIndex].RootTriangleBarycentricBuffer->UAV;
		Parameters->OutRootTriangleDistance		= DistanceUAV;

		const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(RootData.RootCount, 128);
		check(DispatchGroupCount.X < 65536);
		TShaderMapRef<FHairMeshProjectionCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrandsMeshProjection"),
			ComputeShader,
			Parameters,
			DispatchGroupCount);
	}
}

void ProjectHairStrandsOntoMesh(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData& ProjectionMeshData,
	FHairStrandsProjectionHairData::HairGroup& ProjectionHairData)
{
	if (LODIndex < 0 || LODIndex >= ProjectionHairData.LODDatas.Num())
		return;

	FRDGBufferRef RootDistanceBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), ProjectionHairData.RootCount), TEXT("HairStrandsTriangleDistance"));

	bool ClearDistance = true;
	for (const FHairStrandsProjectionMeshData::Section& MeshSection : ProjectionMeshData.LODs[LODIndex].Sections)
	{
		check(ProjectionHairData.LODDatas[LODIndex].LODIndex == LODIndex);
		AddHairStrandMeshProjectionPass(GraphBuilder, ShaderMap, ClearDistance, LODIndex, MeshSection, ProjectionHairData, RootDistanceBuffer);
		ProjectionHairData.LODDatas[LODIndex].bIsValid = true;
		ClearDistance = false;
	}
}

void TransferMesh(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData& SourceMeshData,
	const FHairStrandsProjectionMeshData& TargetMeshData,
	FRWBuffer& OutPositionBuffer)
{
	if (LODIndex < 0)
		return;

	// LODs are transfered using the LOD0 of the source mesh, as the LOD count can mismatch between source and target meshes.
	const int32 SourceLODIndex = 0;
	const int32 TargetLODIndex = LODIndex;

	// Assume that the section 0 contains the head section, which is where the hair/facial hair should be projected on
	const uint32 SourceSectionIndex = 0;
	const uint32 TargetSectionIndex = 0;

	const int32 SectionCount = TargetMeshData.LODs[TargetLODIndex].Sections.Num();
	if (SectionCount < 0)
		return;

	FRDGBufferRef VertexSectionId = AddMeshSectionId(GraphBuilder, ShaderMap, TargetMeshData.LODs[TargetLODIndex]);
	const FHairStrandsProjectionMeshData::Section& SourceMeshSection = SourceMeshData.LODs[SourceLODIndex].Sections[SourceSectionIndex];
	const FHairStrandsProjectionMeshData::Section& TargetMeshSection = TargetMeshData.LODs[TargetLODIndex].Sections[TargetSectionIndex];
	AddMeshTransferPass(GraphBuilder, ShaderMap, true, SourceMeshSection, TargetMeshSection, VertexSectionId, OutPositionBuffer);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
#define SECTION_ARRAY_COUNT 16
class FHairUpdateMeshTriangleCS : public FGlobalShader
{
public:
	const static uint32 SectionArrayCount = 16;
private:
	DECLARE_GLOBAL_SHADER(FHairUpdateMeshTriangleCS);
	SHADER_USE_PARAMETER_STRUCT(FHairUpdateMeshTriangleCS, FGlobalShader);

	class FUpdateUVs : SHADER_PERMUTATION_INT("PERMUTATION_WITHUV", 2);
	using FPermutationDomain = TShaderPermutationDomain<FUpdateUVs>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxRootCount)
		SHADER_PARAMETER(uint32, MaxSectionCount)
		
		SHADER_PARAMETER_ARRAY(uint32, MeshSectionIndex, [SectionArrayCount])
		SHADER_PARAMETER_ARRAY(uint32, MeshMaxIndexCount, [SectionArrayCount])
		SHADER_PARAMETER_ARRAY(uint32, MeshMaxVertexCount, [SectionArrayCount])
		SHADER_PARAMETER_ARRAY(uint32, MeshIndexOffset, [SectionArrayCount])
		SHADER_PARAMETER_ARRAY(uint32, MeshUVsChannelOffset, [SectionArrayCount])
		SHADER_PARAMETER_ARRAY(uint32, MeshUVsChannelCount, [SectionArrayCount])
		SHADER_PARAMETER_SRV(Buffer, MeshIndexBuffer)
		SHADER_PARAMETER_SRV(Buffer, MeshPositionBuffer)
		SHADER_PARAMETER_SRV(Buffer, MeshUVsBuffer)

		SHADER_PARAMETER_SRV(Buffer, RootTriangleIndex)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutRootTrianglePosition0)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutRootTrianglePosition1)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutRootTrianglePosition2)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("MAX_SECTION_COUNT"), SectionArrayCount);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairUpdateMeshTriangleCS, "/Engine/Private/HairStrands/HairStrandsMeshUpdate.usf", "MainCS", SF_Compute);

static void AddHairStrandUpdateMeshTrianglesPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const HairStrandsTriangleType Type,
	const FHairStrandsProjectionMeshData::LOD& MeshData,
	FHairStrandsProjectionHairData::HairGroup& RootData)
{
	if (RootData.RootCount == 0 || LODIndex < 0 || LODIndex >= RootData.LODDatas.Num())
	{
		return;
	}
	FHairStrandsProjectionHairData::LODData& LODData = RootData.LODDatas[LODIndex];
	check(LODData.LODIndex == LODIndex);

	const int32 SectionCount = MeshData.Sections.Num();
	FHairUpdateMeshTriangleCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairUpdateMeshTriangleCS::FParameters>();
	Parameters->MaxRootCount		= RootData.RootCount;
	Parameters->MaxSectionCount		= SectionCount;
	Parameters->MeshPositionBuffer	= MeshData.Sections[0].PositionBuffer;
	Parameters->MeshIndexBuffer		= MeshData.Sections[0].IndexBuffer;
	Parameters->MeshUVsBuffer		= MeshData.Sections[0].UVsBuffer;

	check(SectionCount < FHairUpdateMeshTriangleCS::SectionArrayCount);
	for (int32 SectionIt = 0; SectionIt < SectionCount; ++SectionIt)
	{
		const FHairStrandsProjectionMeshData::Section& MeshSectionData = MeshData.Sections[SectionIt];
		
		// Sanity check to insure all sections share the same underlying buffer 
		check(Parameters->MeshPositionBuffer == MeshSectionData.PositionBuffer);
		check(Parameters->MeshIndexBuffer == MeshSectionData.IndexBuffer);
		check(Parameters->MeshUVsBuffer == MeshSectionData.UVsBuffer);

		Parameters->MeshSectionIndex[SectionIt]		= MeshSectionData.SectionIndex;
		Parameters->MeshMaxIndexCount[SectionIt]	= MeshSectionData.TotalIndexCount;
		Parameters->MeshMaxVertexCount[SectionIt]	= MeshSectionData.TotalVertexCount;
		Parameters->MeshIndexOffset[SectionIt]		= MeshSectionData.IndexBaseIndex;
		Parameters->MeshUVsChannelOffset[SectionIt] = MeshSectionData.UVsChannelOffset;
		Parameters->MeshUVsChannelCount[SectionIt]	= MeshSectionData.UVsChannelCount;
	}

	Parameters->RootTriangleIndex = LODData.RootTriangleIndexBuffer->SRV;
	if (Type == HairStrandsTriangleType::RestPose)
	{
		Parameters->OutRootTrianglePosition0 = LODData.RestRootTrianglePosition0Buffer->UAV;
		Parameters->OutRootTrianglePosition1 = LODData.RestRootTrianglePosition1Buffer->UAV;
		Parameters->OutRootTrianglePosition2 = LODData.RestRootTrianglePosition2Buffer->UAV;
	}
	else if (Type == HairStrandsTriangleType::DeformedPose)
	{
		Parameters->OutRootTrianglePosition0 = LODData.DeformedRootTrianglePosition0Buffer->UAV;
		Parameters->OutRootTrianglePosition1 = LODData.DeformedRootTrianglePosition1Buffer->UAV;
		Parameters->OutRootTrianglePosition2 = LODData.DeformedRootTrianglePosition2Buffer->UAV;
		if (LODData.Status) (*LODData.Status) = FHairStrandsProjectionHairData::LODData::EStatus::Completed;
	}
	else
	{
		// error
		return;
	}

	FHairUpdateMeshTriangleCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairUpdateMeshTriangleCS::FUpdateUVs>(1);

	const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(RootData.RootCount, 128);
	check(DispatchGroupCount.X < 65536);
	TShaderMapRef<FHairUpdateMeshTriangleCS> ComputeShader(ShaderMap, PermutationVector);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrandsTriangleMeshUpdate"),
		ComputeShader,
		Parameters,
		DispatchGroupCount);
}

void UpdateHairStrandsMeshTriangles(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const HairStrandsTriangleType Type,
	const FHairStrandsProjectionMeshData::LOD& ProjectionMeshData,
	FHairStrandsProjectionHairData::HairGroup& ProjectionHairData)
{
	AddHairStrandUpdateMeshTrianglesPass(GraphBuilder, ShaderMap, LODIndex, Type, ProjectionMeshData, ProjectionHairData);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

class FHairInterpolateMeshTriangleCS : public FGlobalShader
{
private:
	DECLARE_GLOBAL_SHADER(FHairInterpolateMeshTriangleCS);
	SHADER_USE_PARAMETER_STRUCT(FHairInterpolateMeshTriangleCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxRootCount)
		SHADER_PARAMETER(uint32, MaxSampleCount)

		SHADER_PARAMETER_SRV(Buffer, RestSamplePositionsBuffer)
		SHADER_PARAMETER_SRV(Buffer, MeshSampleWeightsBuffer)

		SHADER_PARAMETER_SRV(StructuredBuffer, RestRootTrianglePosition0)
		SHADER_PARAMETER_SRV(StructuredBuffer, RestRootTrianglePosition1)
		SHADER_PARAMETER_SRV(StructuredBuffer, RestRootTrianglePosition2)

		SHADER_PARAMETER_UAV(StructuredBuffer, OutDeformedRootTrianglePosition0)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutDeformedRootTrianglePosition1)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutDeformedRootTrianglePosition2)
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairInterpolateMeshTriangleCS, "/Engine/Private/HairStrands/HairStrandsMeshInterpolate.usf", "MainCS", SF_Compute);

static void AddHairStrandInterpolateMeshTrianglesPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData::LOD& MeshData,
	FHairStrandsProjectionHairData::HairGroup& RootData)
{
	if (RootData.RootCount == 0 || LODIndex < 0 || LODIndex >= RootData.LODDatas.Num())
	{
		return;
	}
	FHairStrandsProjectionHairData::LODData& LODData = RootData.LODDatas[LODIndex];
	check(LODData.LODIndex == LODIndex);

	FHairInterpolateMeshTriangleCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairInterpolateMeshTriangleCS::FParameters>();
	Parameters->MaxRootCount = RootData.RootCount;
	Parameters->MaxSampleCount = LODData.SampleCount;

	Parameters->RestRootTrianglePosition0 = LODData.RestRootTrianglePosition0Buffer->SRV;
	Parameters->RestRootTrianglePosition1 = LODData.RestRootTrianglePosition1Buffer->SRV;
	Parameters->RestRootTrianglePosition2 = LODData.RestRootTrianglePosition2Buffer->SRV;

	Parameters->OutDeformedRootTrianglePosition0 = LODData.DeformedRootTrianglePosition0Buffer->UAV;
	Parameters->OutDeformedRootTrianglePosition1 = LODData.DeformedRootTrianglePosition1Buffer->UAV;
	Parameters->OutDeformedRootTrianglePosition2 = LODData.DeformedRootTrianglePosition2Buffer->UAV;

	Parameters->MeshSampleWeightsBuffer = LODData.MeshSampleWeightsBuffer->SRV;
	Parameters->RestSamplePositionsBuffer = LODData.RestSamplePositionsBuffer->SRV;

	const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(RootData.RootCount, 128);
	check(DispatchGroupCount.X < 65536);
	TShaderMapRef<FHairInterpolateMeshTriangleCS> ComputeShader(ShaderMap);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrandsTriangleMeshInterpolate"),
		ComputeShader,
		Parameters,
		DispatchGroupCount);
}

void InterpolateHairStrandsMeshTriangles(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData::LOD& ProjectionMeshData,
	FHairStrandsProjectionHairData::HairGroup& ProjectionHairData)
{
	AddHairStrandInterpolateMeshTrianglesPass(GraphBuilder, ShaderMap, LODIndex, ProjectionMeshData, ProjectionHairData);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
class FHairInitMeshSamplesCS : public FGlobalShader
{
private:
	DECLARE_GLOBAL_SHADER(FHairInitMeshSamplesCS);
	SHADER_USE_PARAMETER_STRUCT(FHairInitMeshSamplesCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxSampleCount)
		SHADER_PARAMETER(uint32, MaxVertexCount)

		SHADER_PARAMETER_SRV(Buffer, VertexPositionsBuffer)

		SHADER_PARAMETER_SRV(Buffer, SampleIndicesBuffer)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutSamplePositionsBuffer)
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairInitMeshSamplesCS, "/Engine/Private/HairStrands/HairStrandsSamplesInit.usf", "MainCS", SF_Compute);

static void AddHairStrandInitMeshSamplesPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const HairStrandsTriangleType Type,
	const FHairStrandsProjectionMeshData::LOD& MeshData,
	FHairStrandsProjectionHairData::HairGroup& RootData)
{
	if (LODIndex < 0 || LODIndex >= RootData.LODDatas.Num())
	{
		return;
	}
	FHairStrandsProjectionHairData::LODData& LODData = RootData.LODDatas[LODIndex];
	check(LODData.LODIndex == LODIndex);

	const uint32 SectionCount = MeshData.Sections.Num();
	if (SectionCount > 0 && LODData.SampleCount > 0)
	{
		FHairInitMeshSamplesCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairInitMeshSamplesCS::FParameters>();

		Parameters->MaxVertexCount = MeshData.Sections[0].TotalVertexCount;
		Parameters->VertexPositionsBuffer = MeshData.Sections[0].PositionBuffer;

		Parameters->MaxSampleCount = LODData.SampleCount;
		Parameters->SampleIndicesBuffer = LODData.MeshSampleIndicesBuffer->SRV;
		if (Type == HairStrandsTriangleType::RestPose)
		{
			Parameters->OutSamplePositionsBuffer = LODData.RestSamplePositionsBuffer->UAV;
		}
		else if (Type == HairStrandsTriangleType::DeformedPose)
		{
			Parameters->OutSamplePositionsBuffer = LODData.DeformedSamplePositionsBuffer->UAV;
		}
		else
		{
			return;
		}

		const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(LODData.SampleCount, 128);
		check(DispatchGroupCount.X < 65536);
		TShaderMapRef<FHairInitMeshSamplesCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrandsInitMeshSamples"),
			ComputeShader,
			Parameters,
			DispatchGroupCount);
	}
}

void InitHairStrandsMeshSamples(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const HairStrandsTriangleType Type,
	const FHairStrandsProjectionMeshData::LOD& ProjectionMeshData,
	FHairStrandsProjectionHairData::HairGroup& ProjectionHairData)
{
	AddHairStrandInitMeshSamplesPass(GraphBuilder, ShaderMap, LODIndex, Type, ProjectionMeshData, ProjectionHairData);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
class FHairUpdateMeshSamplesCS : public FGlobalShader
{
private:
	DECLARE_GLOBAL_SHADER(FHairUpdateMeshSamplesCS);
	SHADER_USE_PARAMETER_STRUCT(FHairUpdateMeshSamplesCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxSampleCount)

		SHADER_PARAMETER_SRV(Buffer, SampleIndicesBuffer)
		SHADER_PARAMETER_SRV(Buffer, InterpolationWeightsBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer, SampleRestPositionsBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer, SampleDeformedPositionsBuffer)
		SHADER_PARAMETER_UAV(StructuredBuffer, OutSampleDeformationsBuffer)
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairUpdateMeshSamplesCS, "/Engine/Private/HairStrands/HairStrandsSamplesUpdate.usf", "MainCS", SF_Compute);

static void AddHairStrandUpdateMeshSamplesPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData::LOD& MeshData,
	FHairStrandsProjectionHairData::HairGroup& RootData)
{
	if (LODIndex < 0 || LODIndex >= RootData.LODDatas.Num())
	{
		return;
	}
	FHairStrandsProjectionHairData::LODData& LODData = RootData.LODDatas[LODIndex];
	check(LODData.LODIndex == LODIndex);

	const uint32 SectionCount = MeshData.Sections.Num();
	if (SectionCount > 0 && LODData.SampleCount > 0)
	{
		FHairUpdateMeshSamplesCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairUpdateMeshSamplesCS::FParameters>();

		Parameters->MaxSampleCount = LODData.SampleCount;
		Parameters->SampleIndicesBuffer = LODData.MeshSampleIndicesBuffer->SRV;
		Parameters->InterpolationWeightsBuffer = LODData.MeshInterpolationWeightsBuffer->SRV;
		Parameters->SampleRestPositionsBuffer = LODData.RestSamplePositionsBuffer->SRV;
		Parameters->SampleDeformedPositionsBuffer = LODData.DeformedSamplePositionsBuffer->SRV;
		Parameters->OutSampleDeformationsBuffer = LODData.MeshSampleWeightsBuffer->UAV;

		const FIntVector DispatchGroupCount = FComputeShaderUtils::GetGroupCount(LODData.SampleCount, 128);
		check(DispatchGroupCount.X < 65536);
		TShaderMapRef<FHairUpdateMeshSamplesCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrandsUpdateMeshSamples"),
			ComputeShader,
			Parameters,
			DispatchGroupCount);
	}
}

void UpdateHairStrandsMeshSamples(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const int32 LODIndex,
	const FHairStrandsProjectionMeshData::LOD& ProjectionMeshData,
	FHairStrandsProjectionHairData::HairGroup& ProjectionHairData)
{
	AddHairStrandUpdateMeshSamplesPass(GraphBuilder, ShaderMap, LODIndex, ProjectionMeshData, ProjectionHairData);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// Generate follicle mask texture
BEGIN_SHADER_PARAMETER_STRUCT(FHairFollicleMaskParameters, )
	SHADER_PARAMETER(FVector2D, OutputResolution)
	SHADER_PARAMETER(uint32, MaxRootCount)
	SHADER_PARAMETER(uint32, Channel)
	SHADER_PARAMETER(uint32, KernelSizeInPixels)

	SHADER_PARAMETER_SRV(Buffer, TrianglePosition0Buffer)
	SHADER_PARAMETER_SRV(Buffer, TrianglePosition1Buffer)
	SHADER_PARAMETER_SRV(Buffer, TrianglePosition2Buffer)
	SHADER_PARAMETER_SRV(Buffer, RootBarycentricBuffer)

	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FHairFollicleMask : public FGlobalShader
{
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_FOLLICLE_MASK"), 1);
	}

	FHairFollicleMask() = default;
	FHairFollicleMask(const CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}
};

class FHairFollicleMaskVS : public FHairFollicleMask
{
	DECLARE_GLOBAL_SHADER(FHairFollicleMaskVS);
	SHADER_USE_PARAMETER_STRUCT(FHairFollicleMaskVS, FHairFollicleMask);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairFollicleMaskParameters, Pass)
	END_SHADER_PARAMETER_STRUCT()
};

class FHairFollicleMaskPS : public FHairFollicleMask
{
	DECLARE_GLOBAL_SHADER(FHairFollicleMaskPS);
	SHADER_USE_PARAMETER_STRUCT(FHairFollicleMaskPS, FHairFollicleMask);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairFollicleMaskParameters, Pass)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHairFollicleMaskPS, "/Engine/Private/HairStrands/HairStrandsFollicleMask.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FHairFollicleMaskVS, "/Engine/Private/HairStrands/HairStrandsFollicleMask.usf", "MainVS", SF_Vertex);

static void AddFollicleMaskPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const bool bNeedClear,
	const uint32 KernelSizeInPixels,
	const uint32 Channel,
	const uint32 LODIndex,
	const FHairStrandsProjectionHairData::HairGroup& HairData,
	FRDGTextureRef OutTexture)
{
	if (LODIndex >= uint32(HairData.LODDatas.Num()) || HairData.RootCount == 0)
		return;

	const FHairStrandsProjectionHairData::LODData& LODData = HairData.LODDatas[LODIndex];
	if (!LODData.RootTriangleBarycentricBuffer ||
		!LODData.RestRootTrianglePosition0Buffer ||
		!LODData.RestRootTrianglePosition1Buffer ||
		!LODData.RestRootTrianglePosition2Buffer)
		return;

	const FIntPoint OutputResolution = OutTexture->Desc.Extent;
	FHairFollicleMaskParameters* Parameters = GraphBuilder.AllocParameters<FHairFollicleMaskParameters>();
	Parameters->TrianglePosition0Buffer = LODData.RestRootTrianglePosition0Buffer->SRV;
	Parameters->TrianglePosition1Buffer = LODData.RestRootTrianglePosition1Buffer->SRV;
	Parameters->TrianglePosition2Buffer = LODData.RestRootTrianglePosition2Buffer->SRV;
	Parameters->RootBarycentricBuffer = LODData.RootTriangleBarycentricBuffer->SRV;
	Parameters->OutputResolution = OutputResolution;
	Parameters->MaxRootCount = HairData.RootCount;
	Parameters->Channel = FMath::Min(Channel, 3u);
	Parameters->KernelSizeInPixels = FMath::Clamp(KernelSizeInPixels, 2u, 200u);
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutTexture, bNeedClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, 0);

	TShaderMapRef<FHairFollicleMaskVS> VertexShader(ShaderMap);
	TShaderMapRef<FHairFollicleMaskPS> PixelShader(ShaderMap);
	FHairFollicleMaskVS::FParameters ParametersVS;
	FHairFollicleMaskPS::FParameters ParametersPS;
	ParametersVS.Pass = *Parameters;
	ParametersPS.Pass = *Parameters;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrandsFollicleMask"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, ParametersVS, ParametersPS, VertexShader, PixelShader, OutputResolution](FRHICommandList& RHICmdList)
	{

		RHICmdList.SetViewport(0, 0, 0.0f, OutputResolution.X, OutputResolution.Y, 1.0f);

		// Apply additive blending pipeline state.
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Max, BF_SourceColor, BF_DestColor, BO_Max, BF_SourceAlpha, BF_DestAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), ParametersVS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), ParametersPS);

		// Emit an instanced quad draw call on the order of the number of pixels on the screen.	
		RHICmdList.DrawPrimitive(0, Parameters->MaxRootCount, 1);
	});
}

void GenerateFolliculeMask(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const FIntPoint Resolution,
	const uint32 MipCount,
	const uint32 KernelSizeInPixels,
	const uint32 Channel,
	const int32 LODIndex,
	const FHairStrandsProjectionHairData& HairData, 
	FRDGTextureRef& OutTexture)
{
	const FLinearColor ClearColor(0.0f, 0.f, 0.f, 0.f);

	bool bClear = OutTexture == nullptr;
	if (OutTexture == nullptr)
	{
		FRDGTextureDesc OutputDesc;
		OutputDesc.ClearValue = FClearValueBinding(ClearColor);
		OutputDesc.Extent.X = Resolution.X;
		OutputDesc.Extent.Y = Resolution.Y;
		OutputDesc.Depth = 0;
		OutputDesc.Format = PF_R8G8B8A8;
		OutputDesc.NumMips = MipCount;
		OutputDesc.Flags = 0;
		OutputDesc.TargetableFlags = TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV;
		OutTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("FollicleMask"));
	}

	for (const FHairStrandsProjectionHairData::HairGroup& HairGroup : HairData.HairGroups)
	{
		AddFollicleMaskPass(GraphBuilder, ShaderMap, bClear, KernelSizeInPixels, Channel, LODIndex, HairGroup, OutTexture);
		bClear = false;
	}
}

class FGenerateMipCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateMipCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateMipCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(uint32, SourceMip)
		SHADER_PARAMETER(uint32, TargetMip)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_GENERATE_MIPS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGenerateMipCS, "/Engine/Private/HairStrands/HairStrandsFollicleMask.usf", "MainCS", SF_Compute);

void AddComputeMipsPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	FRDGTextureRef& OutTexture)
{
	check(OutTexture->Desc.Extent.X == OutTexture->Desc.Extent.Y);
	const uint32 Resolution = OutTexture->Desc.Extent.X;
	const uint32 MipCount = OutTexture->Desc.NumMips;
	for (uint32 MipIt = 0; MipIt < MipCount - 1; ++MipIt)
	{
		const uint32 SourceMipIndex = MipIt;
		const uint32 TargetMipIndex = MipIt + 1;
		const uint32 TargetResolution = Resolution << TargetMipIndex;

		FGenerateMipCS::FParameters* Parameters = GraphBuilder.AllocParameters<FGenerateMipCS::FParameters>();
		Parameters->InTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(OutTexture, SourceMipIndex));
		Parameters->OutTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutTexture, TargetMipIndex));
		Parameters->Resolution = Resolution;
		Parameters->SourceMip = SourceMipIndex;
		Parameters->TargetMip = TargetMipIndex;
		Parameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		TShaderMapRef<FGenerateMipCS> ComputeShader(ShaderMap);
		ClearUnusedGraphResources(ComputeShader, Parameters);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HairStrandsComputeVoxelMip"),
			Parameters,
			ERDGPassFlags::Compute | ERDGPassFlags::GenerateMips,
			[Parameters, ComputeShader, TargetResolution](FRHICommandList& RHICmdList)
		{
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntPoint(TargetResolution, TargetResolution), FIntPoint(8, 8));
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *Parameters, GroupCount);
		});
	}
}