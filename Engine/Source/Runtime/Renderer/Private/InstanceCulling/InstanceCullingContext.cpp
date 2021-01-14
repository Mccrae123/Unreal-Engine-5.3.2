// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstanceCulling/InstanceCullingContext.h"
#include "CoreMinimal.h"
#include "RHI.h"
#include "RendererModule.h"
#include "ShaderParameterMacros.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "InstanceCulling/InstanceCullingManager.h"

#define ENABLE_DETERMINISTIC_INSTANCE_CULLING 1

void FInstanceCullingContext::BeginCullingCommand(EPrimitiveType BatchType, uint32 BaseVertexIndex, uint32 FirstIndex, uint32 NumPrimitives)
{
#if defined(GPUCULL_TODO)
	if (ensure(BatchType == PT_TriangleList || BatchType == PT_LineList || BatchType == PT_PointList || BatchType == PT_QuadList))
	{
		// default to PT_TriangleList
		int32 NumVerticesOrIndices = NumPrimitives * 3;
		switch (BatchType)
		{
		case PT_QuadList:
			NumVerticesOrIndices = NumPrimitives * 4;
			break;
		case PT_LineList:
			NumVerticesOrIndices = NumPrimitives * 2;
			break;
		case PT_PointList:
			NumVerticesOrIndices = NumPrimitives;
			break;
		default:
			break;
		}
		FPrimCullingCommand& CullingCommand = CullingCommands.AddDefaulted_GetRef();
		CullingCommand.BaseVertexIndex = BaseVertexIndex;
		CullingCommand.FirstIndex = FirstIndex;
		CullingCommand.NumVerticesOrIndices = NumVerticesOrIndices;
		CullingCommand.FirstPrimitiveIdOffset = PrimitiveIds.Num();
		CullingCommand.FirstInstanceRunOffset = InstanceRuns.Num();
	}
#endif
}

void FInstanceCullingContext::AddPrimitiveToCullingCommand(int32 ScenePrimitiveId)
{
#if defined(GPUCULL_TODO)
	PrimitiveIds.Add(ScenePrimitiveId);
#endif
}

void FInstanceCullingContext::AddInstanceRunToCullingCommand(int32 ScenePrimitiveId, const uint32* Runs, uint32 NumRuns)
{
#if defined(GPUCULL_TODO)
	//InstanceRuns.AddDefaulted(NumRuns);
	for (uint32 Index = 0; Index < NumRuns; ++Index)
	{
		InstanceRuns.Add(FInstanceRun{ Runs[Index * 2], Runs[Index * 2 + 1], ScenePrimitiveId });
	}
#endif
}

int32 FInstanceCullingContext::AllocateArgsSlotRange(uint32 NumSlots)
{
	//CullingCommands.SetNumZeroed(NumSlots);
	return 0;
}

#if defined(GPUCULL_TODO)

#if ENABLE_DETERMINISTIC_INSTANCE_CULLING

class FComputeInstanceIdOutputSizeCs : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeInstanceIdOutputSizeCs);
	SHADER_USE_PARAMETER_STRUCT(FComputeInstanceIdOutputSizeCs, FGlobalShader)
public:
	class FCullInstancesDim : SHADER_PERMUTATION_BOOL("CULL_INSTANCES");
	using FPermutationDomain = TShaderPermutationDomain<FCullInstancesDim>;

	static constexpr int32 NumThreadsPerGroup = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("INDIRECT_ARGS_NUM_WORDS"), FInstanceCullingContext::IndirectArgsNumWords);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
		OutEnvironment.SetDefine(TEXT("NANITE_MULTI_VIEW"), 1);
		OutEnvironment.SetDefine(TEXT("ENABLE_DETERMINISTIC_INSTANCE_CULLING"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUSceneInstanceSceneData)
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(uint32, InstanceDataSOAStride)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FPrimCullingCommand >, PrimitiveCullingCommands)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< int32 >, PrimitiveIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FInstanceRun >, InstanceRuns)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, VisibleInstanceFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, ViewIds)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputOffsetBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, InstanceCountsOut)

		SHADER_PARAMETER(int32, NumPrimitiveIds)
		SHADER_PARAMETER(int32, NumInstanceRuns)
		SHADER_PARAMETER(int32, NumCommands)
		SHADER_PARAMETER(uint32, NumInstanceFlagWords)
		SHADER_PARAMETER(int32, NumViewIds)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FComputeInstanceIdOutputSizeCs, "/Engine/Private/InstanceCulling/BuildInstanceDrawCommands.usf", "ComputeInstanceIdOutputSize", SF_Compute);

class FCalcOutputOffsetsCs : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCalcOutputOffsetsCs);
	SHADER_USE_PARAMETER_STRUCT(FCalcOutputOffsetsCs, FGlobalShader)
public:
	static constexpr int32 NumThreadsPerGroup = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("INDIRECT_ARGS_NUM_WORDS"), FInstanceCullingContext::IndirectArgsNumWords);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
		OutEnvironment.SetDefine(TEXT("NANITE_MULTI_VIEW"), 1);
		OutEnvironment.SetDefine(TEXT("ENABLE_DETERMINISTIC_INSTANCE_CULLING"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InstanceIdOffsetBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputOffsetBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InstanceCounts)
		SHADER_PARAMETER(int32, NumCommands)
		SHADER_PARAMETER(int32, NumViewIds)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCalcOutputOffsetsCs, "/Engine/Private/InstanceCulling/BuildInstanceDrawCommands.usf", "CalcOutputOffsets", SF_Compute);

class FOutputInstanceIdsAtOffsetCs : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FOutputInstanceIdsAtOffsetCs);
	SHADER_USE_PARAMETER_STRUCT(FOutputInstanceIdsAtOffsetCs, FGlobalShader)
public:
	static constexpr int32 NumThreadsPerGroup = 64;

	// GPUCULL_TODO: remove once buffer is somehow unified
	class FOutputCommandIdDim : SHADER_PERMUTATION_BOOL("OUTPUT_COMMAND_IDS");
	class FCullInstancesDim : SHADER_PERMUTATION_BOOL("CULL_INSTANCES");
	using FPermutationDomain = TShaderPermutationDomain<FOutputCommandIdDim, FCullInstancesDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("INDIRECT_ARGS_NUM_WORDS"), FInstanceCullingContext::IndirectArgsNumWords);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
		OutEnvironment.SetDefine(TEXT("NANITE_MULTI_VIEW"), 1);
		OutEnvironment.SetDefine(TEXT("ENABLE_DETERMINISTIC_INSTANCE_CULLING"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InstanceIdOffsetBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InstanceCounts)

		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUSceneInstanceSceneData)
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(uint32, InstanceDataSOAStride)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FPrimCullingCommand >, PrimitiveCullingCommands)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< int32 >, PrimitiveIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FInstanceRun >, InstanceRuns)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, VisibleInstanceFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, ViewIds)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InstanceIdsBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DrawCommandIdsBufferOut)
		// Using the wrong kind of buffer for RDG...
		SHADER_PARAMETER_UAV(RWBuffer<uint>, InstanceIdsBufferLegacyOut)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DrawIndirectArgsBufferOut)
		SHADER_PARAMETER(int32, NumPrimitiveIds)
		SHADER_PARAMETER(int32, NumInstanceRuns)
		SHADER_PARAMETER(int32, NumCommands)
		SHADER_PARAMETER(uint32, NumInstanceFlagWords)
		SHADER_PARAMETER(int32, NumViewIds)

	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FOutputInstanceIdsAtOffsetCs, "/Engine/Private/InstanceCulling/BuildInstanceDrawCommands.usf", "OutputInstanceIdsAtOffset", SF_Compute);

#endif // ENABLE_DETERMINISTIC_INSTANCE_CULLING

class FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs);
	SHADER_USE_PARAMETER_STRUCT(FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs, FGlobalShader)

public:
	static constexpr int32 NumThreadsPerGroup = 64;

	// GPUCULL_TODO: remove once buffer is somehow unified
	class FOutputCommandIdDim : SHADER_PERMUTATION_BOOL("OUTPUT_COMMAND_IDS");
	using FPermutationDomain = TShaderPermutationDomain<FOutputCommandIdDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("INDIRECT_ARGS_NUM_WORDS"), FInstanceCullingContext::IndirectArgsNumWords);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
		OutEnvironment.SetDefine(TEXT("NANITE_MULTI_VIEW"), 1);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUSceneInstanceSceneData)
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(uint32, InstanceDataSOAStride)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FPrimCullingCommand >, PrimitiveCullingCommands)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< int32 >, PrimitiveIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceCullingContext::FInstanceRun >, InstanceRuns)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, VisibleInstanceFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint32 >, ViewIds)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputOffsetBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InstanceIdOffsetBufferOut)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InstanceIdsBufferOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DrawCommandIdsBufferOut)
		// Using the wrong kind of buffer for RDG...
		SHADER_PARAMETER_UAV(RWBuffer<uint>, InstanceIdsBufferLegacyOut)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InstanceIdOffsetBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DrawIndirectArgsBufferOut)
		SHADER_PARAMETER(int32, NumPrimitiveIds)
		SHADER_PARAMETER(int32, NumInstanceRuns)		
		SHADER_PARAMETER(int32, NumCommands)
		SHADER_PARAMETER(uint32, NumInstanceFlagWords)
		SHADER_PARAMETER(int32, NumViewIds)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs, "/Engine/Private/InstanceCulling/BuildInstanceDrawCommands.usf", "BuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs", SF_Compute);

void FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& GraphBuilder, const FGPUScene& GPUScene, FInstanceCullingResult& Results) const
{
	Results = FInstanceCullingResult();
	if (CullingCommands.Num() == 0)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "BuildRenderingCommands");

	// Note: use start at zero offset if there is no instance culling manager, this means each build rendering commands pass will overwrite the same ID range. Which is only ok assuming correct barriers (should be erring on this side by default).
	TArray<uint32> NullArray;
	NullArray.AddZeroed(1);
	FRDGBufferRef InstanceIdOutOffsetBufferRDG = InstanceCullingManager ? InstanceCullingManager->CullingIntermediate.InstanceIdOutOffsetBuffer : CreateStructuredBuffer(GraphBuilder, TEXT("OutputOffsetBufferOutTransient"), NullArray);
	// If there is no manager, then there is no data on culling, so set flag to skip that and ignore buffers.
	FRDGBufferRef VisibleInstanceFlagsRDG = InstanceCullingManager != nullptr ? InstanceCullingManager->CullingIntermediate.VisibleInstanceFlags : nullptr;
	const bool bCullInstances = InstanceCullingManager != nullptr;
	const int32 NumInstances = InstanceCullingManager != nullptr ? InstanceCullingManager->CullingIntermediate.NumInstances : 0;
	int32 NumInstanceFlagWords = FMath::DivideAndRoundUp(NumInstances, int32(sizeof(uint32) * 8));

#if ENABLE_DETERMINISTIC_INSTANCE_CULLING
	// 1. Compute output sizes for all commands
	FRDGBufferRef InstanceCountsRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector2), CullingCommands.Num()), TEXT("InstanceCounts"));
	FRDGBufferRef InstanceIdOffsetBufferRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CullingCommands.Num()), TEXT("InstanceIdOffsetBuffer"));
	FRDGBufferRef DrawIndirectArgsRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(IndirectArgsNumWords * CullingCommands.Num()), TEXT("DrawIndirectArgsBuffer"));

	{
		FComputeInstanceIdOutputSizeCs::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeInstanceIdOutputSizeCs::FParameters>();

		PassParameters->InstanceCountsOut = GraphBuilder.CreateUAV(InstanceCountsRDG);

		// Because the view uniforms are not set up by the time this runs
		// PassParameters->View = View.ViewUniformBuffer;
		// Set up global GPU-scene data instead...
		PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
		PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
		PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;
		// Upload data etc
		PassParameters->PrimitiveCullingCommands = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveCullingCommands"), CullingCommands));

		PassParameters->PrimitiveIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveIds"), PrimitiveIds));
		PassParameters->InstanceRuns = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRuns"), InstanceRuns));

		PassParameters->OutputOffsetBufferOut = GraphBuilder.CreateUAV(InstanceIdOutOffsetBufferRDG);

		PassParameters->ViewIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("ViewIds"), ViewIds));
		PassParameters->NumViewIds = ViewIds.Num();
		PassParameters->NumPrimitiveIds = PrimitiveIds.Num();
		PassParameters->NumInstanceRuns = InstanceRuns.Num();
		PassParameters->NumCommands = CullingCommands.Num();
		PassParameters->VisibleInstanceFlags = VisibleInstanceFlagsRDG ? GraphBuilder.CreateSRV(VisibleInstanceFlagsRDG) : nullptr;

		PassParameters->NumInstanceFlagWords = uint32(NumInstanceFlagWords);

		FComputeInstanceIdOutputSizeCs::FPermutationDomain PermutationVector;
		PermutationVector.Set<FComputeInstanceIdOutputSizeCs::FCullInstancesDim>(bCullInstances);
		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FComputeInstanceIdOutputSizeCs>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ComputeInstanceIdOutputSize"),
			ComputeShader,
			PassParameters,
			FIntVector(CullingCommands.Num(), 1, 1)
		);
	}
	// 2. Allocate output slots for each command
	{
		FCalcOutputOffsetsCs::FParameters* PassParameters = GraphBuilder.AllocParameters<FCalcOutputOffsetsCs::FParameters>();

		PassParameters->InstanceCounts = GraphBuilder.CreateSRV(InstanceCountsRDG);
		PassParameters->OutputOffsetBufferOut = GraphBuilder.CreateUAV(InstanceIdOutOffsetBufferRDG);
		PassParameters->InstanceIdOffsetBufferOut = GraphBuilder.CreateUAV(InstanceIdOffsetBufferRDG, PF_R32_UINT);
		PassParameters->NumViewIds = ViewIds.Num();
		PassParameters->NumCommands = CullingCommands.Num();

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FCalcOutputOffsetsCs>();

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CalcOutputOffsets"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1)
		);
	}
	// 3. Populate the output buffers
	{
		FOutputInstanceIdsAtOffsetCs::FParameters* PassParameters = GraphBuilder.AllocParameters<FOutputInstanceIdsAtOffsetCs::FParameters>();

		PassParameters->InstanceCounts = GraphBuilder.CreateSRV(InstanceCountsRDG);
		PassParameters->InstanceIdOffsetBuffer = GraphBuilder.CreateSRV(InstanceIdOffsetBufferRDG, PF_R32_UINT);


		// Because the view uniforms are not set up by the time this runs
		// PassParameters->View = View.ViewUniformBuffer;
		// Set up global GPU-scene data instead...
		PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
		PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
		PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;
		// Upload data etc
		PassParameters->PrimitiveCullingCommands = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveCullingCommands"), CullingCommands));

		PassParameters->PrimitiveIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveIds"), PrimitiveIds));
		PassParameters->InstanceRuns = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRuns"), InstanceRuns));

		PassParameters->ViewIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("ViewIds"), ViewIds));
		PassParameters->NumViewIds = ViewIds.Num();

		// TODO: Remove this when everything is properly RDG'd
		AddPass(GraphBuilder, [&GraphBuilder](FRHICommandList& RHICmdList)
		{
			RHICmdList.Transition(FRHITransitionInfo(GInstanceCullingManagerResources.GetInstancesIdBufferUav(), ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		});

		//PassParameters->InstanceIdsBufferOut = GraphBuilder.CreateUAV(InstanceIdsBufferRDG, PF_R32_UINT);
		// TODO: Access resources through manager rather than global
		PassParameters->InstanceIdsBufferLegacyOut = GInstanceCullingManagerResources.GetInstancesIdBufferUav();
		PassParameters->DrawIndirectArgsBufferOut = GraphBuilder.CreateUAV(DrawIndirectArgsRDG, PF_R32_UINT);
		PassParameters->NumPrimitiveIds = PrimitiveIds.Num();
		PassParameters->NumInstanceRuns = InstanceRuns.Num();
		PassParameters->NumCommands = CullingCommands.Num();
		PassParameters->VisibleInstanceFlags = VisibleInstanceFlagsRDG ? GraphBuilder.CreateSRV(VisibleInstanceFlagsRDG) : nullptr;
		PassParameters->NumInstanceFlagWords = uint32(NumInstanceFlagWords);

		FOutputInstanceIdsAtOffsetCs::FPermutationDomain PermutationVector;
		// NOTE: this also switches between legacy buffer and RDG for Id output
		PermutationVector.Set<FOutputInstanceIdsAtOffsetCs::FOutputCommandIdDim>(0);
		PermutationVector.Set<FOutputInstanceIdsAtOffsetCs::FCullInstancesDim>(bCullInstances);
		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FOutputInstanceIdsAtOffsetCs>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CalcOutputOffsets"),
			ComputeShader,
			PassParameters,
			FIntVector(CullingCommands.Num(), 1, 1)
		);
	}


#else // !ENABLE_DETERMINISTIC_INSTANCE_CULLING
	FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FParameters* PassParameters = GraphBuilder.AllocParameters<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FParameters>();

	// Because the view uniforms are not set up by the time this runs
	// PassParameters->View = View.ViewUniformBuffer;
	// Set up global GPU-scene data instead...
	PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
	PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
	PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;
	// Upload data etc
	PassParameters->PrimitiveCullingCommands = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveCullingCommands"), CullingCommands));

	PassParameters->PrimitiveIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveIds"), PrimitiveIds));
	PassParameters->InstanceRuns = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRuns"), InstanceRuns));

	FRDGBufferRef InstanceIdOutOffsetBufferRDG = Intermediate.InstanceIdOutOffsetBuffer;
	FRDGBufferRef VisibleInstanceFlagsRDG = Intermediate.VisibleInstanceFlags;

	PassParameters->OutputOffsetBufferOut = GraphBuilder.CreateUAV(InstanceIdOutOffsetBufferRDG);

	FRDGBufferRef DrawIndirectArgsRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(IndirectArgsNumWords * CullingCommands.Num()), TEXT("DrawIndirectArgsBuffer"));
	// not using structured buffer as we want/have toget at it as a vertex buffer 
	//FRDGBufferRef InstanceIdsBufferRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PrimitiveIds.Num() * FInstanceCullingManager::MaxAverageInstanceFactor), TEXT("InstanceIdsBuffer"));
	FRDGBufferRef InstanceIdOffsetBufferRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CullingCommands.Num()), TEXT("InstanceIdOffsetBuffer"));

	PassParameters->ViewIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("ViewIds"), ViewIds));
	PassParameters->NumViewIds = ViewIds.Num();

	// TODO: Remove this when everything is properly RDG'd
	AddPass(GraphBuilder, [&GraphBuilder](FRHICommandList& RHICmdList)
	{
		RHICmdList.Transition(FRHITransitionInfo(GInstanceCullingManagerResources.GetInstancesIdBufferUav(), ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	});

	//PassParameters->InstanceIdsBufferOut = GraphBuilder.CreateUAV(InstanceIdsBufferRDG, PF_R32_UINT);
	// TODO: Access resources through manager rather than global
	PassParameters->InstanceIdsBufferLegacyOut = GInstanceCullingManagerResources.GetInstancesIdBufferUav();
	PassParameters->DrawIndirectArgsBufferOut = GraphBuilder.CreateUAV(DrawIndirectArgsRDG, PF_R32_UINT);
	PassParameters->InstanceIdOffsetBufferOut = GraphBuilder.CreateUAV(InstanceIdOffsetBufferRDG, PF_R32_UINT);
	PassParameters->NumPrimitiveIds = PrimitiveIds.Num();
	PassParameters->NumInstanceRuns = InstanceRuns.Num();
	PassParameters->NumCommands = CullingCommands.Num();
	PassParameters->VisibleInstanceFlags = VisibleInstanceFlagsRDG ? GraphBuilder.CreateSRV(VisibleInstanceFlagsRDG) : nullptr;

	PassParameters->NumInstanceFlagWords = uint32(NumInstanceFlagWords);

	FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FPermutationDomain PermutationVector;
	// NOTE: this also switches between legacy buffer and RDG for Id output
	PermutationVector.Set<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FOutputCommandIdDim>(0);
	PermutationVector.Set<FComputeInstanceIdOutputSizeCs::FCullInstancesDim>(bCullInstances);

	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs>(PermutationVector);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("BuildInstanceIdBufferAndCommandsFromPrimitiveIds"),
		ComputeShader,
		PassParameters,
		FIntVector(CullingCommands.Num(), 1, 1)
	);
#endif// ENABLE_DETERMINISTIC_INSTANCE_CULLING

	Results.DrawIndirectArgsBuffer = DrawIndirectArgsRDG;
	//ConvertToExternalBuffer(GraphBuilder, DrawIndirectArgsRDG, Results.DrawIndirectArgsBuffer);
	//GraphBuilder.QueueBufferExtraction(InstanceIdsBufferRDG, &Results.InstanceIdsBuffer);
	Results.InstanceIdOffsetBuffer = InstanceIdOffsetBufferRDG;
	//ConvertToExternalBuffer(GraphBuilder, InstanceIdOffsetBufferRDG, Results.InstanceIdOffsetBuffer);
	//GraphBuilder.Transition(FRHITransitionInfo(GInstanceCullingManagerResources.GetInstancesIdBufferUav(), ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

	// TODO: Remove this when everything is properly RDG'd
	AddPass(GraphBuilder, [&GraphBuilder](FRHICommandList& RHICmdList)
	{
		RHICmdList.Transition(FRHITransitionInfo(GInstanceCullingManagerResources.GetInstancesIdBufferUav(), ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
		RHICmdList.Transition(FRHITransitionInfo(GInstanceCullingManagerResources.GetPageInfoBufferUav(), ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	});
}

void FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& GraphBuilder, FGPUScene& GPUScene, FInstanceCullingRdgParams& Params) const
{
	if (CullingCommands.Num() == 0)
	{
		return;
	}
	RDG_EVENT_SCOPE(GraphBuilder, "BuildRenderingCommands");

	const FInstanceCullingIntermediate& Intermediate = InstanceCullingManager->CullingIntermediate;

	FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FParameters* PassParameters = GraphBuilder.AllocParameters<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FParameters>();
	// Because the view uniforms are not set up by the time this runs
	// PassParameters->View = View.ViewUniformBuffer;
	// Set up global GPU-scene data instead...
	PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
	PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
	PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;
	// Upload data etc
	PassParameters->PrimitiveCullingCommands = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveCullingCommands"), CullingCommands));

	PassParameters->PrimitiveIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveIds"), PrimitiveIds));
	PassParameters->InstanceRuns = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRuns"), InstanceRuns));


	FRDGBufferRef InstanceIdOutOffsetBufferRDG = Intermediate.InstanceIdOutOffsetBuffer;
	FRDGBufferRef VisibleInstanceFlagsRDG = Intermediate.VisibleInstanceFlags;

	// Create and initialize if not allocated
	if (Params.InstanceIdWriteOffsetBuffer == nullptr)
	{
		TArray<uint32> NullArray;
		NullArray.AddZeroed(1);
		Params.InstanceIdWriteOffsetBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("InstanceIdWriteOffsetBuffer"), NullArray);
	}

	PassParameters->OutputOffsetBufferOut = GraphBuilder.CreateUAV(Params.InstanceIdWriteOffsetBuffer);

	Params.DrawIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(IndirectArgsNumWords * CullingCommands.Num()), TEXT("DrawIndirectArgsBuffer"));
	// not using structured buffer as we want/have to get at it as a vertex buffer 
	//FRDGBufferRef InstanceIdsBufferRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PrimitiveIds.Num() * FInstanceCullingManager::MaxAverageInstanceFactor), TEXT("InstanceIdsBuffer"));
	Params.InstanceIdStartOffsetBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), CullingCommands.Num()), TEXT("InstanceIdOffsetBuffer"));

	PassParameters->ViewIds = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("ViewIds"), ViewIds));
	PassParameters->NumViewIds = ViewIds.Num();


	//PassParameters->InstanceIdsBufferOut = GraphBuilder.CreateUAV(InstanceIdsBufferRDG, PF_R32_UINT);
	// TODO: Access resources through manager rather than global
	PassParameters->DrawIndirectArgsBufferOut = GraphBuilder.CreateUAV(Params.DrawIndirectArgs, PF_R32_UINT);
	PassParameters->InstanceIdOffsetBufferOut = GraphBuilder.CreateUAV(Params.InstanceIdStartOffsetBuffer, PF_R32_UINT);
	PassParameters->NumPrimitiveIds = PrimitiveIds.Num();
	PassParameters->NumInstanceRuns = InstanceRuns.Num();
	PassParameters->NumCommands = CullingCommands.Num();
	PassParameters->VisibleInstanceFlags = GraphBuilder.CreateSRV(VisibleInstanceFlagsRDG);

	if (Params.InstanceIdsBuffer == nullptr)
	{
		// TODO: we could compute the max instance count from the MDCs.
		const int32 InstanceIdBufferSize = CullingCommands.Num() * FInstanceCullingManager::MaxAverageInstanceFactor * 64;
		Params.InstanceIdsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), InstanceIdBufferSize), TEXT("InstanceIdsBuffer"));;
		Params.DrawCommandIdsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), InstanceIdBufferSize), TEXT("DrawCommandIdsBuffer"));;

	}
	
	PassParameters->InstanceIdsBufferOut = GraphBuilder.CreateUAV(Params.InstanceIdsBuffer, PF_R32_UINT);
	PassParameters->DrawCommandIdsBufferOut = GraphBuilder.CreateUAV(Params.DrawCommandIdsBuffer, PF_R32_UINT);

	int32 NumInstanceFlagWords = FMath::DivideAndRoundUp(Intermediate.NumInstances, int32(sizeof(uint32) * 8));
	PassParameters->NumInstanceFlagWords = uint32(NumInstanceFlagWords);


	FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FPermutationDomain PermutationVector;
	// NOTE: this also switches between legacy buffer and RDG for Id output
	PermutationVector.Set<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs::FOutputCommandIdDim>(1);
	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FBuildInstanceIdBufferAndCommandsFromPrimitiveIdsCs>(PermutationVector);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("BuildInstanceIdBufferAndCommandsFromPrimitiveIds"),
		ComputeShader,
		PassParameters,
		FIntVector(CullingCommands.Num(), 1, 1)
	);
}

#if 0

void FInstanceCullingContext::CreateLegacyPassParameters(FRDGBuilder& GraphBuilder, FInstanceCullingDrawParams& InstanceCullingDrawParamsOut, int32 NumDrawCommands)
{
	FRDGBufferRef DrawIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(IndirectArgsNumWords * NumDrawCommands), TEXT("DrawIndirectArgsBuffer"));
	FRDGBufferRef InstanceIdOffsetBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumDrawCommands), TEXT("InstanceIdOffsetBuffer"));

	InstanceCullingDrawParamsOut.DrawIndirectArgsBuffer = DrawIndirectArgsBuffer;
	InstanceCullingDrawParamsOut.DrawIndirectArgsBufferAccess = DrawIndirectArgsBuffer;
	InstanceCullingDrawParamsOut.InstanceIdOffsetBuffer = InstanceIdOffsetBuffer;
	InstanceCullingDrawParamsOut.InstanceIdOffsetBufferAccess = InstanceIdOffsetBuffer;
}


void FInstanceCullingContext::BuildLegacyRenderingCommands(const FScene &Scene, FRHICommandListImmediate& RHICmdList, const FGPUScenePrimitiveCollector &DynamicPrimitiveCollector, FRHIBuffer* &InstanceIdOffsetBufferOut, FRHIBuffer* &DrawIndirectArgsBufferOut)
{
	FInstanceCullingManagerResources::FLegacyContext LegacyContext = GInstanceCullingManagerResources.GetLegacyContext();
	ensure(LegacyContext.MaxDrawCommands >= CullingCommands.Num());

	uint32 *IndirectArgs = reinterpret_cast<uint32*>(RHICmdList.LockBuffer(LegacyContext.DrawIndirectArgsBuffer, 0, LegacyContext.DrawIndirectArgsBuffer->GetSize(), RLM_WriteOnly));
	uint32 *InstanceIdOffsets = reinterpret_cast<uint32*>(RHICmdList.LockBuffer(LegacyContext.InstanceIdOffsetBuffer, 0, LegacyContext.InstanceIdOffsetBuffer->GetSize(), RLM_WriteOnly));

	FRHIBuffer* InstancesIdBuffer = GInstanceCullingManagerResources.GetInstancesIdBuffer();
	uint32* InstanceIds = reinterpret_cast<uint32*>(RHICmdList.LockBuffer(InstancesIdBuffer, 0, InstancesIdBuffer->GetSize(), RLM_WriteOnly));

	uint32 InstanceIdWriteOffset = 0;
	for (int32 CommandIndex = 0; CommandIndex < CullingCommands.Num(); ++CommandIndex)
	{
		int32 InstanceIdStartOffset = InstanceIdWriteOffset;
		const FPrimCullingCommand& Cmd = CullingCommands[CommandIndex];

		int32 InstanceRunEnd = CommandIndex < CullingCommands.Num() - 1 ? CullingCommands[CommandIndex + 1].FirstInstanceRunOffset : InstanceRuns.Num();
		for (int32 InstanceRunOffset = Cmd.FirstInstanceRunOffset; InstanceRunOffset < InstanceRunEnd; ++InstanceRunOffset)
		{
			const FInstanceRun &InstanceRun = InstanceRuns[InstanceRunOffset];
			for (uint32 InstanceId = InstanceRun.Start; InstanceId <= InstanceRun.EndInclusive; ++InstanceId)
			{
				for (int32 ViewIndex = 0; ViewIndex < ViewIds.Num(); ++ViewIndex)
				{
					InstanceIds[InstanceIdWriteOffset++] = InstanceId | (uint32(ViewIndex) << 28U);
				}
			}
		}

		const int32 NumScenePrimitives = Scene.Primitives.Num();

		int32 PrimitiveIdEnd = CommandIndex < CullingCommands.Num() - 1 ? CullingCommands[CommandIndex + 1].FirstPrimitiveIdOffset : PrimitiveIds.Num();
		for (int32 PrimitiveIdIndex = Cmd.FirstPrimitiveIdOffset; PrimitiveIdIndex < PrimitiveIdEnd; ++PrimitiveIdIndex)
		{
			int32 PrimitiveId = PrimitiveIds[PrimitiveIdIndex];

			const int32 InstanceDataOffset = PrimitiveId < NumScenePrimitives ? Scene.Primitives[PrimitiveId]->GetInstanceDataOffset() : DynamicPrimitiveCollector.GetInstanceDataOffset(PrimitiveId);
			const int32 InstanceDataEntries = PrimitiveId < NumScenePrimitives ? Scene.Primitives[PrimitiveId]->GetNumInstanceDataEntries() : 1;
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceDataEntries; ++InstanceIndex)
			{
				int32 InstanceId = InstanceDataEntries + InstanceIndex;
				for (int32 ViewIndex = 0; ViewIndex < ViewIds.Num(); ++ViewIndex)
				{
					InstanceIds[InstanceIdWriteOffset++] = InstanceId | (uint32(ViewIndex) << 28U);
				}
			}
		}

		InstanceIdOffsets[CommandIndex] = InstanceIdStartOffset;

		IndirectArgs[CommandIndex * IndirectArgsNumWords + 0] = Cmd.NumVerticesOrIndices;
		IndirectArgs[CommandIndex * IndirectArgsNumWords + 1] = InstanceIdWriteOffset - InstanceIdStartOffset;
		IndirectArgs[CommandIndex * IndirectArgsNumWords + 2] = Cmd.FirstIndex;
		IndirectArgs[CommandIndex * IndirectArgsNumWords + 3] = Cmd.BaseVertexIndex;
		IndirectArgs[CommandIndex * IndirectArgsNumWords + 4] = 0U;
	}

	RHICmdList.UnlockBuffer(LegacyContext.DrawIndirectArgsBuffer);
	RHICmdList.UnlockBuffer(LegacyContext.InstanceIdOffsetBuffer);
	RHICmdList.UnlockBuffer(GInstanceCullingManagerResources.GetInstancesIdBuffer());


	InstanceIdOffsetBufferOut = LegacyContext.InstanceIdOffsetBuffer;
	DrawIndirectArgsBufferOut = LegacyContext.DrawIndirectArgsBuffer;
}

#endif // 0

#else // GPUCULL_TODO

void FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& GraphBuilder, const FGPUScene& GPUScene, FInstanceCullingResult& Results) const
{
}

void FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& GraphBuilder, FGPUScene& GPUScene, FInstanceCullingRdgParams& Params) const
{
}

#endif // GPUCULL_TODO