// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "LumenSceneUtils.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "SceneTextureParameters.h"
#include "IndirectLightRendering.h"

// Actual screen-probe requirements..
#include "LumenRadianceCache.h"
#include "LumenScreenProbeGather.h"

#if RHI_RAYTRACING
#include "RayTracing/RayTracingDeferredMaterials.h"
#include "RayTracing/RaytracingOptions.h"
#include "RayTracing/RayTracingLighting.h"
#include "LumenHardwareRayTracingCommon.h"

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracing(
	TEXT("r.Lumen.Visualize.HardwareRayTracing"),
	0,
	TEXT("Enables visualization of hardware ray tracing (Default = 0)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingLightingMode(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.LightingMode"),
	0,
	TEXT("Determines the lighting mode (Default = 0)\n")
	TEXT("0: interpolate final lighting from the surface cache\n")
	TEXT("1: evaluate material, and interpolate irradiance and indirect irradiance from the surface cache\n")
	TEXT("2: evaluate material and direct lighting, and interpolate indirect irradiance from the surface cache"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingNormalMode(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.NormalMode"),
	0,
	TEXT("Determines the tracing normal (Default = 0)\n")
	TEXT("0: SDF normal\n")
	TEXT("1: Geometry normal"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingDeferredMaterial(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.DeferredMaterial"),
	1,
	TEXT("Enables deferred material pipeline (Default = 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenVisualizeHardwareRayTracingDeferredMaterialTileSize(
	TEXT("r.Lumen.Visualize.HardwareRayTracing.DeferredMaterial.TileDimension"),
	64,
	TEXT("Determines the tile dimension for material sorting (Default = 64)"),
	ECVF_RenderThreadSafe
);

#endif // RHI_RAYTRACING

namespace Lumen
{
	EHardwareRayTracingLightingMode GetVisualizeHardwareRayTracingLightingMode()
	{
#if RHI_RAYTRACING
		return EHardwareRayTracingLightingMode(CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread());
#else
		return EHardwareRayTracingLightingMode::LightingFromSurfaceCache;
#endif
	}

	bool ShouldVisualizeHardwareRayTracing()
	{
		bool bVisualize = false;
#if RHI_RAYTRACING
		bVisualize = (CVarLumenVisualizeHardwareRayTracing.GetValueOnRenderThread() != 0) && IsRayTracingEnabled();
#endif
		return bVisualize;
	}
}

#if RHI_RAYTRACING

class FLumenVisualizeHardwareRayTracingRGS : public FLumenHardwareRayTracingRGS
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenVisualizeHardwareRayTracingRGS, FLumenHardwareRayTracingRGS)

	class FDeferredMaterialModeDim : SHADER_PERMUTATION_BOOL("DIM_DEFERRED_MATERIAL_MODE");
	class FLightingModeDim : SHADER_PERMUTATION_INT("DIM_LIGHTING_MODE", 3);
	using FPermutationDomain = TShaderPermutationDomain<FDeferredMaterialModeDim, FLightingModeDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingRGS::FSharedParameters, SharedParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FDeferredMaterialPayload>, DeferredMaterialBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadiance)
		SHADER_PARAMETER(int, NormalMode)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingRGS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "LumenVisualizeHardwareRayTracingRGS", SF_RayGen);

class FLumenVisualizeHardwareRayTracingDeferredMaterialRGS : public FLumenHardwareRayTracingDeferredMaterialRGS
{
	DECLARE_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingDeferredMaterialRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FLumenVisualizeHardwareRayTracingDeferredMaterialRGS, FLumenHardwareRayTracingDeferredMaterialRGS)

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingDeferredMaterialRGS::FDeferredMaterialParameters, DeferredMaterialParameters)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FLumenVisualizeHardwareRayTracingDeferredMaterialRGS, "/Engine/Private/Lumen/LumenVisualizeHardwareRayTracing.usf", "LumenVisualizeHardwareRayTracingDeferredMaterialRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualize(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Shading pass
	{
		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FDeferredMaterialModeDim>(CVarLumenVisualizeHardwareRayTracingDeferredMaterial.GetValueOnRenderThread() != 0);
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FLightingModeDim>(CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread());
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualizeDeferredMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Tracing pass
	{
		FLumenVisualizeHardwareRayTracingDeferredMaterialRGS::FPermutationDomain PermutationVector;
		TShaderRef<FLumenVisualizeHardwareRayTracingDeferredMaterialRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingDeferredMaterialRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingVisualizeLumenMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Fixed-function lighting version
	Lumen::EHardwareRayTracingLightingMode LightingMode = static_cast<Lumen::EHardwareRayTracingLightingMode>(CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread());
	if (Lumen::ShouldVisualizeHardwareRayTracing() && LightingMode == Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache)
	{
		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FDeferredMaterialModeDim>(0);
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FLightingModeDim>(0);
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

#endif // RHI_RAYTRACING

void VisualizeHardwareRayTracing(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FSceneTextureParameters& SceneTextures,
	const FViewInfo& View,
	const FLumenCardTracingInputs& TracingInputs,
	const FLumenMeshSDFGridParameters& MeshSDFGridParameters,
	FLumenIndirectTracingParameters& IndirectTracingParameters,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	FRDGTextureRef SceneColor
)
#if RHI_RAYTRACING
{
	FIntPoint RayTracingResolution = View.ViewRect.Size();

	int TileSize = CVarLumenVisualizeHardwareRayTracingDeferredMaterialTileSize.GetValueOnRenderThread();
	FIntPoint DeferredMaterialBufferResolution = RayTracingResolution;
	DeferredMaterialBufferResolution = FIntPoint::DivideAndRoundUp(DeferredMaterialBufferResolution, TileSize) * TileSize;
	int DeferredMaterialBufferNumElements = DeferredMaterialBufferResolution.X * DeferredMaterialBufferResolution.Y;
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDeferredMaterialPayload), DeferredMaterialBufferNumElements);
	FRDGBufferRef DeferredMaterialBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("LumenVisualizeHardwareRayTracingDeferredMaterialBuffer"));

	// Trace to get material-id
	Lumen::EHardwareRayTracingLightingMode LightingMode = static_cast<Lumen::EHardwareRayTracingLightingMode>(CVarLumenVisualizeHardwareRayTracingLightingMode.GetValueOnRenderThread());
	bool bUseDeferredMaterial = CVarLumenVisualizeHardwareRayTracingDeferredMaterial.GetValueOnRenderThread() != 0;
	bUseDeferredMaterial &= LightingMode != Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache;

	if (bUseDeferredMaterial)
	{
		FLumenVisualizeHardwareRayTracingDeferredMaterialRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeHardwareRayTracingDeferredMaterialRGS::FParameters>();
		SetLumenHardwareRayTracingSharedParameters(
			GraphBuilder,
			SceneTextures,
			View,
			TracingInputs,
			MeshSDFGridParameters,
			&PassParameters->DeferredMaterialParameters.SharedParameters);

		// Output..
		PassParameters->DeferredMaterialParameters.RWDeferredMaterialBuffer = GraphBuilder.CreateUAV(DeferredMaterialBuffer);
		PassParameters->DeferredMaterialParameters.DeferredMaterialBufferResolution = DeferredMaterialBufferResolution;
		PassParameters->DeferredMaterialParameters.TileSize = TileSize;

		// Permutation settings
		FLumenVisualizeHardwareRayTracingDeferredMaterialRGS::FPermutationDomain PermutationVector;
		TShaderRef<FLumenVisualizeHardwareRayTracingDeferredMaterialRGS> RayGenerationShader =
			View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingDeferredMaterialRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("LumenVisualizeHardwareRayTracingDeferredMaterial %ux%u", DeferredMaterialBufferResolution.X, DeferredMaterialBufferResolution.Y),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DeferredMaterialBufferResolution](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				RHICmdList.RayTraceDispatch(View.RayTracingMaterialGatherPipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DeferredMaterialBufferResolution.X, DeferredMaterialBufferResolution.Y);
			}
		);

		// Sort by material-id
		const uint32 SortSize = 5; // 4096 elements
		SortDeferredMaterials(GraphBuilder, View, SortSize, DeferredMaterialBufferNumElements, DeferredMaterialBuffer);
	}

	// Re-trace and shade
	{
		// TODO: indent code block after review
		FLumenVisualizeHardwareRayTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenVisualizeHardwareRayTracingRGS::FParameters>();

		SetLumenHardwareRayTracingSharedParameters(
			GraphBuilder,
			SceneTextures,
			View,
			TracingInputs,
			MeshSDFGridParameters,
			&PassParameters->SharedParameters);
		PassParameters->DeferredMaterialBuffer = GraphBuilder.CreateSRV(DeferredMaterialBuffer);

		// Constants!
		PassParameters->NormalMode = CVarLumenVisualizeHardwareRayTracingNormalMode.GetValueOnRenderThread();

		// Output..
		PassParameters->RWRadiance = GraphBuilder.CreateUAV(SceneColor);

		FLumenVisualizeHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FDeferredMaterialModeDim>(bUseDeferredMaterial);
		PermutationVector.Set<FLumenVisualizeHardwareRayTracingRGS::FLightingModeDim>(static_cast<int>(LightingMode));
		TShaderRef<FLumenVisualizeHardwareRayTracingRGS> RayGenerationShader =
			View.ShaderMap->GetShader<FLumenVisualizeHardwareRayTracingRGS>(PermutationVector);
		ClearUnusedGraphResources(RayGenerationShader, PassParameters);

		FIntPoint DispatchResolution = RayTracingResolution;
		if (bUseDeferredMaterial)
		{
			DispatchResolution = FIntPoint(DeferredMaterialBufferNumElements, 1);
		}
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VisualizeHardwareRayTracing %ux%u LightingMode=%s", DispatchResolution.X, DispatchResolution.Y, Lumen::GetRayTracedLightingModeName((Lumen::EHardwareRayTracingLightingMode)LightingMode)),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, &View, RayGenerationShader, DispatchResolution, LightingMode](FRHICommandList& RHICmdList)
			{
				FRayTracingShaderBindingsWriter GlobalResources;
				SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

				FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
				FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;
				if (LightingMode == Lumen::EHardwareRayTracingLightingMode::LightingFromSurfaceCache)
				{
					Pipeline = View.LumenHardwareRayTracingMaterialPipeline;
				}
				RHICmdList.RayTraceDispatch(Pipeline, RayGenerationShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, DispatchResolution.X, DispatchResolution.Y);
			}
		);
	}
}
#else
{
	unimplemented();
}
#endif