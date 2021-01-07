// Copyright Epic Games, Inc. All Rights Reserved.

#if RHI_RAYTRACING

#include "RHIDefinitions.h"
#include "RenderCore.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"
#include "DeferredShadingRenderer.h"
#include "PipelineStateCache.h"
#include "ShaderCompilerCore.h"

class FLumenHardwareRayTracingMaterialCHS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenHardwareRayTracingMaterialCHS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenHardwareRayTracingMaterialCHS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform) && DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		//OutEnvironment.SetDefine(TEXT("UE_RAY_TRACING_LIGHTWEIGHT_CLOSEST_HIT_SHADER"), 1);
	}

	using FParameters = FEmptyShaderParameters;
};

IMPLEMENT_GLOBAL_SHADER(FLumenHardwareRayTracingMaterialCHS, "/Engine/Private/Lumen/LumenHardwareRayTracingMaterials.usf", "LumenHardwareRayTracingMaterialCHS", SF_RayHitGroup);

class FLumenHardwareRayTracingMaterialMS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenHardwareRayTracingMaterialMS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenHardwareRayTracingMaterialMS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform) && DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	using FParameters = FEmptyShaderParameters;
};

IMPLEMENT_GLOBAL_SHADER(FLumenHardwareRayTracingMaterialMS, "/Engine/Private/Lumen/LumenHardwareRayTracingMaterials.usf", "LumenHardwareRayTracingMaterialMS", SF_RayMiss);

FRayTracingPipelineState* FDeferredShadingSceneRenderer::BindLumenHardwareRayTracingMaterialPipeline(FRHICommandList& RHICmdList, const FViewInfo& View, const TArrayView<FRHIRayTracingShader*>& RayGenShaderTable)
{
	SCOPE_CYCLE_COUNTER(STAT_BindRayTracingPipeline);

	FRayTracingPipelineStateInitializer Initializer;

	Initializer.SetRayGenShaderTable(RayGenShaderTable);

	Initializer.MaxPayloadSizeInBytes = 8; // sizeof FLumenMinimalPayload

	// Get the ray tracing materials
	auto ClosestHitShader = View.ShaderMap->GetShader<FLumenHardwareRayTracingMaterialCHS>();
	FRHIRayTracingShader* HitShaderTable[] = { ClosestHitShader.GetRayTracingShader() };
	Initializer.SetHitGroupTable(HitShaderTable);

	auto MissShader = View.ShaderMap->GetShader<FLumenHardwareRayTracingMaterialMS>();
	FRHIRayTracingShader* MissShaderTable[] = { MissShader.GetRayTracingShader() };
	Initializer.SetMissShaderTable(MissShaderTable);

	FRayTracingPipelineState* PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, Initializer);

	const FViewInfo& ReferenceView = Views[0];
	const int32 NumTotalBindings = ReferenceView.VisibleRayTracingMeshCommands.Num();

	const uint32 MergedBindingsSize = sizeof(FRayTracingLocalShaderBindings) * NumTotalBindings;
	FRayTracingLocalShaderBindings* Bindings = (FRayTracingLocalShaderBindings*)(RHICmdList.Bypass()
		? FMemStack::Get().Alloc(MergedBindingsSize, alignof(FRayTracingLocalShaderBindings))
		: RHICmdList.Alloc(MergedBindingsSize, alignof(FRayTracingLocalShaderBindings)));

	uint32 BindingIndex = 0;
	for (const FVisibleRayTracingMeshCommand VisibleMeshCommand : ReferenceView.VisibleRayTracingMeshCommands)
	{
		const FRayTracingMeshCommand& MeshCommand = *VisibleMeshCommand.RayTracingMeshCommand;

		FRayTracingLocalShaderBindings Binding = {};
		Binding.InstanceIndex = VisibleMeshCommand.InstanceIndex;
		Binding.SegmentIndex = MeshCommand.GeometrySegmentIndex;
		Binding.UserData = MeshCommand.MaterialShaderIndex;

		Bindings[BindingIndex] = Binding;
		BindingIndex++;
	}

	const bool bCopyDataToInlineStorage = false; // Storage is already allocated from RHICmdList, no extra copy necessary
	RHICmdList.SetRayTracingHitGroups(
		View.RayTracingScene.RayTracingSceneRHI,
		PipelineState,
		NumTotalBindings, Bindings,
		bCopyDataToInlineStorage);

	return PipelineState;
}

#endif // RHI_RAYTRACING