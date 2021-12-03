// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenSceneDirectLighting.cpp
=============================================================================*/

#include "LumenSceneLighting.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "VolumeLighting.h"
#include "DistanceFieldLightingShared.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"
#include "VolumetricCloudRendering.h"
#include "LumenTracingUtils.h"

int32 GLumenDirectLighting = 1;
FAutoConsoleVariableRef CVarLumenDirectLighting(
	TEXT("r.LumenScene.DirectLighting"),
	GLumenDirectLighting,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

int32 GLumenDirectLightingForceForceShadowMaps = 0;
FAutoConsoleVariableRef CVarLumenDirectLightingForceShadowMaps(
	TEXT("r.LumenScene.DirectLighting.ForceShadowMaps"),
	GLumenDirectLightingForceForceShadowMaps,
	TEXT("Use shadow maps for all lights casting shadows."),
	ECVF_RenderThreadSafe
);

int32 GLumenDirectLightingForceOffscreenShadowing = 0;
FAutoConsoleVariableRef CVarLumenDirectLightingForceOffscreenShadowing(
	TEXT("r.LumenScene.DirectLighting.ForceOffscreenShadowing"),
	GLumenDirectLightingForceOffscreenShadowing,
	TEXT("Use offscreen shadowing for all lights casting shadows."),
	ECVF_RenderThreadSafe
);

int32 GLumenDirectLightingOffscreenShadowingTraceMeshSDFs = 1;
FAutoConsoleVariableRef CVarLumenDirectLightingOffscreenShadowingTraceMeshSDFs(
	TEXT("r.LumenScene.DirectLighting.OffscreenShadowing.TraceMeshSDFs"),
	GLumenDirectLightingOffscreenShadowingTraceMeshSDFs,
	TEXT("Whether to trace against Mesh Signed Distance Fields for offscreen shadowing, or to trace against the lower resolution Global SDF."),
	ECVF_RenderThreadSafe
);

int32 GLumenDirectLightingMaxLightsPerTile = 8;
FAutoConsoleVariableRef CVarLumenDirectLightinggMaxLightsPerTile(
	TEXT("r.LumenScene.DirectLighting.MaxLightsPerTile"),
	GLumenDirectLightingMaxLightsPerTile,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GOffscreenShadowingMaxTraceDistance = 15000;
FAutoConsoleVariableRef CVarOffscreenShadowingMaxTraceDistance(
	TEXT("r.LumenScene.DirectLighting.OffscreenShadowingMaxTraceDistance"),
	GOffscreenShadowingMaxTraceDistance,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GOffscreenShadowingTraceStepFactor = 5;
FAutoConsoleVariableRef CVarOffscreenShadowingTraceStepFactor(
	TEXT("r.LumenScene.DirectLighting.OffscreenShadowingTraceStepFactor"),
	GOffscreenShadowingTraceStepFactor,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GOffscreenShadowingSDFSurfaceBiasScale = 6;
FAutoConsoleVariableRef CVarOffscreenShadowingSDFSurfaceBiasScale(
	TEXT("r.LumenScene.DirectLighting.OffscreenShadowingSDFSurfaceBiasScale"),
	GOffscreenShadowingSDFSurfaceBiasScale,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GShadowingSurfaceBias = 2;
FAutoConsoleVariableRef CVarShadowingSurfaceBias(
	TEXT("r.LumenScene.DirectLighting.ShadowingSurfaceBias"),
	GShadowingSurfaceBias,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GShadowingSlopeScaledSurfaceBias = 4.0f;
FAutoConsoleVariableRef CVarShadowingSlopeScaledSurfaceBias(
	TEXT("r.LumenScene.DirectLighting.ShadowingSlopeScaledSurfaceBias"),
	GShadowingSlopeScaledSurfaceBias,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

int32 GLumenDirectLightingCloudTransmittance = 1;
FAutoConsoleVariableRef CVarLumenDirectLightingCloudTransmittance(
	TEXT("r.LumenScene.DirectLighting.CloudTransmittance"),
	GLumenDirectLightingCloudTransmittance,
	TEXT("Whether to sample cloud shadows when avaible."),
	ECVF_RenderThreadSafe
);

int32 GLumenDirectLightingVirtualShadowMap = 1;
FAutoConsoleVariableRef CVarLumenDirectLightingVirtualShadowMap(
	TEXT("r.LumenScene.DirectLighting.VirtualShadowMap"),
	GLumenDirectLightingVirtualShadowMap,
	TEXT("Whether to sample virtual shadow when avaible."),
	ECVF_RenderThreadSafe
);

float GLumenDirectLightingVirtualShadowMapBias = 7.0f;
FAutoConsoleVariableRef CVarLumenDirectLightingVirtualShadowMapBias(
	TEXT("r.LumenScene.DirectLighting.VirtualShadowMapBias"),
	GLumenDirectLightingVirtualShadowMapBias,
	TEXT("Bias for sampling virtual shadow maps."),
	ECVF_RenderThreadSafe
);

bool Lumen::UseVirtualShadowMaps()
{
	return GLumenDirectLightingVirtualShadowMap != 0;
}

float Lumen::GetSurfaceCacheOffscreenShadowingMaxTraceDistance()
{
	return FMath::Max(GOffscreenShadowingMaxTraceDistance, 0.0f);
}

class FLumenGatheredLight
{
public:
	FLumenGatheredLight(const FLightSceneInfo* InLightSceneInfo, uint32 InLightIndex)
	{
		LightIndex = InLightIndex;
		LightSceneInfo = InLightSceneInfo;
		bHasShadows = InLightSceneInfo->Proxy->CastsDynamicShadow();

		Type = ELumenLightType::MAX;
		const ELightComponentType LightType = (ELightComponentType)InLightSceneInfo->Proxy->GetLightType();
		switch (LightType)
		{
			case LightType_Directional: Type = ELumenLightType::Directional;	break;
			case LightType_Point:		Type = ELumenLightType::Point;			break;
			case LightType_Spot:		Type = ELumenLightType::Spot;			break;
			case LightType_Rect:		Type = ELumenLightType::Rect;			break;
		}

		FSceneRenderer::GetLightNameForDrawEvent(InLightSceneInfo->Proxy, Name);
	}

	const FLightSceneInfo* LightSceneInfo = nullptr;
	uint32 LightIndex = 0;
	ELumenLightType Type = ELumenLightType::MAX;
	bool bHasShadows = false;
	FString Name;
};

BEGIN_SHADER_PARAMETER_STRUCT(FLumenLightTileScatterParameters, )
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(DispatchIndirectArgs, ERHIAccess::IndirectArgs)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileAllocator)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, LightTiles)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileOffsetsPerLight)
END_SHADER_PARAMETER_STRUCT()

class FRasterizeToLightTilesVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRasterizeToLightTilesVS);
	SHADER_USE_PARAMETER_STRUCT(FRasterizeToLightTilesVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenLightTileScatterParameters, LightTileScatterParameters)
		SHADER_PARAMETER(uint32, LightIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRasterizeToLightTilesVS,"/Engine/Private/Lumen/LumenSceneDirectLightingCulling.usf","RasterizeToLightTilesVS",SF_Vertex);

class FBuildLightTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildLightTilesCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildLightTilesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RDG_BUFFER_ACCESS(IndirectArgBuffer, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FLumenPackedLight>, LumenPackedLights)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWLightTileAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWLightTiles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWLightTileAllocatorPerLight)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CardPageIndexAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CardPageIndexData)
		SHADER_PARAMETER(uint32, MaxLightsPerTile)
		SHADER_PARAMETER(uint32, NumLights)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	static int32 GetGroupSize()
	{
		return 8;
	}
};

IMPLEMENT_GLOBAL_SHADER(FBuildLightTilesCS, "/Engine/Private/Lumen/LumenSceneDirectLightingCulling.usf", "BuildLightTilesCS", SF_Compute);

class FComputeLightTileOffsetsPerLightCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeLightTileOffsetsPerLightCS)
	SHADER_USE_PARAMETER_STRUCT(FComputeLightTileOffsetsPerLightCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWLightTileOffsetsPerLight)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileAllocatorPerLight)
		SHADER_PARAMETER(uint32, NumLights)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static uint32 GetGroupSize()
	{
		return 64;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FComputeLightTileOffsetsPerLightCS, "/Engine/Private/Lumen/LumenSceneDirectLightingCulling.usf", "ComputeLightTileOffsetsPerLightCS", SF_Compute);

class FCompactLightTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompactLightTilesCS);
	SHADER_USE_PARAMETER_STRUCT(FCompactLightTilesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RDG_BUFFER_ACCESS(IndirectArgBuffer, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWCompactedLightTiles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWLightTileAllocatorPerLight)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, LightTiles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileOffsetsPerLight)
		SHADER_PARAMETER(uint32, NumLights)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	static int32 GetGroupSize()
	{
		return 64;
	}
};

IMPLEMENT_GLOBAL_SHADER(FCompactLightTilesCS, "/Engine/Private/Lumen/LumenSceneDirectLightingCulling.usf", "CompactLightTilesCS", SF_Compute);

class FInitializeLightTileIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FInitializeLightTileIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FInitializeLightTileIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDispatchLightTilesIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDrawTilesPerLightIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDispatchTilesPerLightIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightTileAllocatorPerLight)
		SHADER_PARAMETER(uint32, VertexCountPerInstanceIndirect)
		SHADER_PARAMETER(uint32, NumLights)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static uint32 GetGroupSize()
	{
		return 64;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FInitializeLightTileIndirectArgsCS, "/Engine/Private/Lumen/LumenSceneDirectLightingCulling.usf", "InitializeLightTileIndirectArgsCS", SF_Compute)

BEGIN_SHADER_PARAMETER_STRUCT(FClearLumenCardsParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FRasterizeToCardsVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FClearLumenCardsPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void ClearLumenSceneDirectLighting(
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder,
	const FLumenSceneData& LumenSceneData,
	const FLumenCardTracingInputs& TracingInputs,
	const FLumenCardUpdateContext& CardUpdateContext)
{
	FClearLumenCardsParameters* PassParameters = GraphBuilder.AllocParameters<FClearLumenCardsParameters>();

	PassParameters->RenderTargets[0] = FRenderTargetBinding(TracingInputs.DirectLightingAtlas, ERenderTargetLoadAction::ENoAction);
	PassParameters->VS.LumenCardScene = TracingInputs.LumenCardSceneUniformBuffer;
	PassParameters->VS.DrawIndirectArgs = CardUpdateContext.DrawCardPageIndicesIndirectArgs;
	PassParameters->VS.CardPageIndexAllocator = GraphBuilder.CreateSRV(CardUpdateContext.CardPageIndexAllocator);
	PassParameters->VS.CardPageIndexData = GraphBuilder.CreateSRV(CardUpdateContext.CardPageIndexData);
	PassParameters->VS.IndirectLightingAtlasSize = LumenSceneData.GetRadiosityAtlasSize();
	PassParameters->PS.View = View.ViewUniformBuffer;
	PassParameters->PS.LumenCardScene = TracingInputs.LumenCardSceneUniformBuffer;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("ClearDirectLighting"),
		PassParameters,
		ERDGPassFlags::Raster,
		[ViewportSize = LumenSceneData.GetPhysicalAtlasSize(), PassParameters, GlobalShaderMap = View.ShaderMap](FRHICommandList& RHICmdList)
	{
		FClearLumenCardsPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FClearLumenCardsPS::FNumTargets>(1);
		auto PixelShader = GlobalShaderMap->GetShader<FClearLumenCardsPS>(PermutationVector);

		auto VertexShader = GlobalShaderMap->GetShader<FRasterizeToCardsVS>();

		DrawQuadsToAtlas(
			ViewportSize,
			VertexShader,
			PixelShader,
			PassParameters,
			GlobalShaderMap,
			TStaticBlendState<>::GetRHI(),
			RHICmdList,
			[](FRHICommandList& RHICmdList, TShaderRefBase<FClearLumenCardsPS, FShaderMapPointerTable> Shader, FRHIPixelShader* ShaderRHI, const typename FClearLumenCardsPS::FParameters& Parameters) {},
			PassParameters->VS.DrawIndirectArgs,
			0);
	});
}

void Lumen::SetDirectLightingDeferredLightUniformBuffer(
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	TUniformBufferBinding<FDeferredLightUniformStruct>& UniformBuffer)
{
	FDeferredLightUniformStruct DeferredLightUniforms = GetDeferredLightParameters(View, *LightSceneInfo);
	if (LightSceneInfo->Proxy->IsInverseSquared())
	{
		DeferredLightUniforms.LightParameters.FalloffExponent = 0;
	}
	DeferredLightUniforms.LightParameters.Color *= LightSceneInfo->Proxy->GetIndirectLightingScale();

	UniformBuffer = CreateUniformBufferImmediate(DeferredLightUniforms, UniformBuffer_SingleDraw);
}

BEGIN_SHADER_PARAMETER_STRUCT(FLightFunctionParameters, )
	SHADER_PARAMETER(FVector4f, LightFunctionParameters)
	SHADER_PARAMETER(FMatrix44f, LightFunctionWorldToLight)
	SHADER_PARAMETER(FVector3f, LightFunctionParameters2)
END_SHADER_PARAMETER_STRUCT()

class FLumenCardDirectLightingPS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FLumenCardDirectLightingPS, Material);

	FLumenCardDirectLightingPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) 
		: FMaterialShader(Initializer) 
	{ 
		Bindings.BindForLegacyShaderParameters( 
			this, 
			Initializer.PermutationId, 
			Initializer.ParameterMap,
			*FParameters::FTypeInfo::GetStructMetadata(), 
			// Don't require full bindings, we use FMaterialShader::SetParameters
			false); 
	} 

	FLumenCardDirectLightingPS() {}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_STRUCT_REF(FDeferredLightUniformStruct, DeferredLightUniforms)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVolumeShadowingShaderParameters, VolumeShadowingShaderParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLightFunctionParameters, LightFunctionParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLightCloudTransmittanceParameters, LightCloudTransmittanceParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShadowMaskTiles)
		SHADER_PARAMETER(uint32, UseIESProfile)
		SHADER_PARAMETER_TEXTURE(Texture2D,IESTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,IESTextureSampler)
	END_SHADER_PARAMETER_STRUCT()

	class FShadowMask : SHADER_PERMUTATION_BOOL("SHADOW_MASK");
	class FLightFunction : SHADER_PERMUTATION_BOOL("LIGHT_FUNCTION");
	class FCloudTransmittance : SHADER_PERMUTATION_BOOL("USE_CLOUD_TRANSMITTANCE");
	class FLightType : SHADER_PERMUTATION_ENUM_CLASS("LIGHT_TYPE", ELumenLightType);
	using FPermutationDomain = TShaderPermutationDomain<FLightType, FShadowMask, FLightFunction, FCloudTransmittance>;

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		if (!PermutationVector.Get<FShadowMask>())
		{
			PermutationVector.Set<FCloudTransmittance>(false);
		}

		if (PermutationVector.Get<FLightType>() != ELumenLightType::Directional)
		{
			PermutationVector.Set<FCloudTransmittance>(false);
		}

		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		return Parameters.MaterialParameters.MaterialDomain == EMaterialDomain::MD_LightFunction && DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	FORCENOINLINE static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) 
	{
		FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(,FLumenCardDirectLightingPS, TEXT("/Engine/Private/Lumen/LumenSceneDirectLighting.usf"), TEXT("LumenCardDirectLightingPS"), SF_Pixel);

class FLumenDirectLightingSampleShadowMapCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenDirectLightingSampleShadowMapCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenDirectLightingSampleShadowMapCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RDG_BUFFER_ACCESS(IndirectArgBuffer, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWShadowMaskTiles)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenLightTileScatterParameters, LightTileScatterParameters)
		SHADER_PARAMETER(uint32, CardScatterInstanceIndex)
		SHADER_PARAMETER(uint32, LightIndex)
		SHADER_PARAMETER(uint32, DummyZeroForFixingShaderCompilerBug)
		SHADER_PARAMETER_STRUCT_REF(FForwardLightData, ForwardLightData)
		SHADER_PARAMETER_STRUCT_REF(FDeferredLightUniformStruct, DeferredLightUniforms)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVirtualShadowMapSamplingParameters, VirtualShadowMapSamplingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVolumeShadowingShaderParameters, VolumeShadowingShaderParameters)
		SHADER_PARAMETER(float, StepFactor)
		SHADER_PARAMETER(float, TanLightSourceAngle)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(float, SurfaceBias)
		SHADER_PARAMETER(float, SlopeScaledSurfaceBias)
		SHADER_PARAMETER(float, VirtualShadowMapSurfaceBias)
		SHADER_PARAMETER(int32, VirtualShadowMapId)
		SHADER_PARAMETER(uint32, SampleDenseShadowMap)
		SHADER_PARAMETER(uint32, ForceShadowMaps)
		SHADER_PARAMETER(uint32, ForceOffscreenShadowing)
	END_SHADER_PARAMETER_STRUCT()

	class FDynamicallyShadowed : SHADER_PERMUTATION_BOOL("DYNAMICALLY_SHADOWED");
	class FVirtualShadowMap : SHADER_PERMUTATION_BOOL("VIRTUAL_SHADOW_MAP");
	class FDenseShadowMap : SHADER_PERMUTATION_BOOL("DENSE_SHADOW_MAP");
	class FLightType : SHADER_PERMUTATION_ENUM_CLASS("LIGHT_TYPE", ELumenLightType);
	using FPermutationDomain = TShaderPermutationDomain<FLightType, FDynamicallyShadowed, FVirtualShadowMap, FDenseShadowMap>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
		FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
	}

	static int32 GetGroupSize()
	{
		return 8;
	}
};

IMPLEMENT_GLOBAL_SHADER(FLumenDirectLightingSampleShadowMapCS, "/Engine/Private/Lumen/LumenSceneDirectLightingShadowMask.usf", "LumenSceneDirectLightingSampleShadowMapCS", SF_Compute);

class FLumenSceneDirectLightingTraceDistanceFieldShadowsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenSceneDirectLightingTraceDistanceFieldShadowsCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenSceneDirectLightingTraceDistanceFieldShadowsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RDG_BUFFER_ACCESS(IndirectArgBuffer, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWShadowMaskTiles)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenLightTileScatterParameters, LightTileScatterParameters)
		SHADER_PARAMETER(uint32, LightIndex)
		SHADER_PARAMETER(uint32, DummyZeroForFixingShaderCompilerBug)
		SHADER_PARAMETER_STRUCT_REF(FDeferredLightUniformStruct, DeferredLightUniforms)
		SHADER_PARAMETER_STRUCT_INCLUDE(FDistanceFieldObjectBufferParameters, ObjectBufferParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FDistanceFieldCulledObjectBufferParameters, CulledObjectBufferParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLightTileIntersectionParameters, LightTileIntersectionParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FDistanceFieldAtlasParameters, DistanceFieldAtlasParameters)
		SHADER_PARAMETER(FMatrix44f, WorldToShadow)
		SHADER_PARAMETER(float, TwoSidedMeshDistanceBias)
		SHADER_PARAMETER(float, StepFactor)
		SHADER_PARAMETER(float, TanLightSourceAngle)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(float, SurfaceBias)
		SHADER_PARAMETER(float, SlopeScaledSurfaceBias)
		SHADER_PARAMETER(float, SDFSurfaceBiasScale)
	END_SHADER_PARAMETER_STRUCT()

	class FTraceMeshSDFs : SHADER_PERMUTATION_BOOL("OFFSCREEN_SHADOWING_TRACE_MESH_SDF");
	class FLightType : SHADER_PERMUTATION_ENUM_CLASS("LIGHT_TYPE", ELumenLightType);
	using FPermutationDomain = TShaderPermutationDomain<FLightType, FTraceMeshSDFs>;

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	FORCENOINLINE static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) 
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	static int32 GetGroupSize()
	{
		return 8;
	}
};

IMPLEMENT_GLOBAL_SHADER(FLumenSceneDirectLightingTraceDistanceFieldShadowsCS, "/Engine/Private/Lumen/LumenSceneDirectLightingShadowMask.usf", "LumenSceneDirectLightingTraceDistanceFieldShadowsCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FLumenCardDirectLighting, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FRasterizeToLightTilesVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardDirectLightingPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void SetupLightFunctionParameters(const FLightSceneInfo* LightSceneInfo, float ShadowFadeFraction, FLightFunctionParameters& OutParameters)
{
	const bool bIsSpotLight = LightSceneInfo->Proxy->GetLightType() == LightType_Spot;
	const bool bIsPointLight = LightSceneInfo->Proxy->GetLightType() == LightType_Point;
	const float TanOuterAngle = bIsSpotLight ? FMath::Tan(LightSceneInfo->Proxy->GetOuterConeAngle()) : 1.0f;

	OutParameters.LightFunctionParameters = FVector4f(TanOuterAngle, ShadowFadeFraction, bIsSpotLight ? 1.0f : 0.0f, bIsPointLight ? 1.0f : 0.0f);

	const FVector Scale = LightSceneInfo->Proxy->GetLightFunctionScale();
	// Switch x and z so that z of the user specified scale affects the distance along the light direction
	const FVector InverseScale = FVector( 1.f / Scale.Z, 1.f / Scale.Y, 1.f / Scale.X );
	const FMatrix WorldToLight = LightSceneInfo->Proxy->GetWorldToLight() * FScaleMatrix(FVector(InverseScale));	

	OutParameters.LightFunctionWorldToLight = WorldToLight;

	const float PreviewShadowsMask = 0.0f;
	OutParameters.LightFunctionParameters2 = FVector(
		LightSceneInfo->Proxy->GetLightFunctionFadeDistance(),
		LightSceneInfo->Proxy->GetLightFunctionDisabledBrightness(),
		PreviewShadowsMask);
}

void SetupMeshSDFShadowInitializer(
	const FLightSceneInfo* LightSceneInfo,
	const FBox& LumenSceneBounds, 
	FSphere& OutShadowBounds,
	FWholeSceneProjectedShadowInitializer& OutInitializer)
{	
	FSphere Bounds;

	{
		// Get the 8 corners of the cascade's camera frustum, in world space
		FVector CascadeFrustumVerts[8];
		const FVector LumenSceneCenter = LumenSceneBounds.GetCenter();
		const FVector LumenSceneExtent = LumenSceneBounds.GetExtent();
		CascadeFrustumVerts[0] = LumenSceneCenter + FVector(LumenSceneExtent.X, LumenSceneExtent.Y, LumenSceneExtent.Z);  
		CascadeFrustumVerts[1] = LumenSceneCenter + FVector(LumenSceneExtent.X, LumenSceneExtent.Y, -LumenSceneExtent.Z); 
		CascadeFrustumVerts[2] = LumenSceneCenter + FVector(LumenSceneExtent.X, -LumenSceneExtent.Y, LumenSceneExtent.Z);    
		CascadeFrustumVerts[3] = LumenSceneCenter + FVector(LumenSceneExtent.X, -LumenSceneExtent.Y, -LumenSceneExtent.Z);  
		CascadeFrustumVerts[4] = LumenSceneCenter + FVector(-LumenSceneExtent.X, LumenSceneExtent.Y, LumenSceneExtent.Z);     
		CascadeFrustumVerts[5] = LumenSceneCenter + FVector(-LumenSceneExtent.X, LumenSceneExtent.Y, -LumenSceneExtent.Z);   
		CascadeFrustumVerts[6] = LumenSceneCenter + FVector(-LumenSceneExtent.X, -LumenSceneExtent.Y, LumenSceneExtent.Z);      
		CascadeFrustumVerts[7] = LumenSceneCenter + FVector(-LumenSceneExtent.X, -LumenSceneExtent.Y, -LumenSceneExtent.Z);   

		Bounds = FSphere(LumenSceneCenter, 0);
		for (int32 Index = 0; Index < 8; Index++)
		{
			Bounds.W = FMath::Max(Bounds.W, FVector::DistSquared(CascadeFrustumVerts[Index], Bounds.Center));
		}

		Bounds.W = FMath::Max(FMath::Sqrt(Bounds.W), 1.0f);

		ComputeShadowCullingVolume(true, CascadeFrustumVerts, -LightSceneInfo->Proxy->GetDirection(), OutInitializer.CascadeSettings.ShadowBoundsAccurate, OutInitializer.CascadeSettings.NearFrustumPlane, OutInitializer.CascadeSettings.FarFrustumPlane);
	}

	OutInitializer.CascadeSettings.ShadowSplitIndex = 0;

	const float ShadowExtent = Bounds.W / FMath::Sqrt(3.0f);
	const FBoxSphereBounds SubjectBounds(Bounds.Center, FVector(ShadowExtent, ShadowExtent, ShadowExtent), Bounds.W);
	OutInitializer.PreShadowTranslation = -Bounds.Center;
	OutInitializer.WorldToLight = FInverseRotationMatrix(LightSceneInfo->Proxy->GetDirection().GetSafeNormal().Rotation());
	OutInitializer.Scales = FVector2D(1.0f / Bounds.W, 1.0f / Bounds.W);
	OutInitializer.SubjectBounds = FBoxSphereBounds(FVector::ZeroVector, SubjectBounds.BoxExtent, SubjectBounds.SphereRadius);
	OutInitializer.WAxis = FVector4f(0, 0, 0, 1);
	OutInitializer.MinLightW = FMath::Min<float>(-HALF_WORLD_MAX, -SubjectBounds.SphereRadius);
	const float MaxLightW = SubjectBounds.SphereRadius;
	OutInitializer.MaxDistanceToCastInLightW = MaxLightW - OutInitializer.MinLightW;
	OutInitializer.bRayTracedDistanceField = true;
	OutInitializer.CascadeSettings.bFarShadowCascade = false;

	const float SplitNear = -Bounds.W;
	const float SplitFar = Bounds.W;

	OutInitializer.CascadeSettings.SplitFarFadeRegion = 0.0f;
	OutInitializer.CascadeSettings.SplitNearFadeRegion = 0.0f;
	OutInitializer.CascadeSettings.SplitFar = SplitFar;
	OutInitializer.CascadeSettings.SplitNear = SplitNear;
	OutInitializer.CascadeSettings.FadePlaneOffset = SplitFar;
	OutInitializer.CascadeSettings.FadePlaneLength = 0;
	OutInitializer.CascadeSettings.CascadeBiasDistribution = 0;
	OutInitializer.CascadeSettings.ShadowSplitIndex = 0;

	OutShadowBounds = Bounds;
}

void CullMeshSDFsForLightCards(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	const FDistanceFieldObjectBufferParameters& ObjectBufferParameters,
	FMatrix& WorldToMeshSDFShadowValue,
	FDistanceFieldCulledObjectBufferParameters& CulledObjectBufferParameters,
	FLightTileIntersectionParameters& LightTileIntersectionParameters)
{
	const FVector LumenSceneViewOrigin = GetLumenSceneViewOrigin(View, GetNumLumenVoxelClipmaps() - 1);
	const FVector LumenSceneExtent = FVector(ComputeMaxCardUpdateDistanceFromCamera());
	const FBox LumenSceneBounds(LumenSceneViewOrigin - LumenSceneExtent, LumenSceneViewOrigin + LumenSceneExtent);

	FSphere MeshSDFShadowBounds;
	FWholeSceneProjectedShadowInitializer MeshSDFShadowInitializer;
	SetupMeshSDFShadowInitializer(LightSceneInfo, LumenSceneBounds, MeshSDFShadowBounds, MeshSDFShadowInitializer);

	const FMatrix FaceMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(0, 1, 0, 0),
		FPlane(-1, 0, 0, 0),
		FPlane(0, 0, 0, 1));

	const FMatrix TranslatedWorldToView = MeshSDFShadowInitializer.WorldToLight * FaceMatrix;

	float MaxSubjectZ = TranslatedWorldToView.TransformPosition(MeshSDFShadowInitializer.SubjectBounds.Origin).Z + MeshSDFShadowInitializer.SubjectBounds.SphereRadius;
	MaxSubjectZ = FMath::Min(MaxSubjectZ, MeshSDFShadowInitializer.MaxDistanceToCastInLightW);
	const float MinSubjectZ = FMath::Max(MaxSubjectZ - MeshSDFShadowInitializer.SubjectBounds.SphereRadius * 2, MeshSDFShadowInitializer.MinLightW);

	const FMatrix ScaleMatrix = FScaleMatrix( FVector( MeshSDFShadowInitializer.Scales.X, MeshSDFShadowInitializer.Scales.Y, 1.0f ) );
	const FMatrix ViewToClip = ScaleMatrix * FShadowProjectionMatrix(MinSubjectZ, MaxSubjectZ, MeshSDFShadowInitializer.WAxis);
	const FMatrix SubjectAndReceiverMatrix = TranslatedWorldToView * ViewToClip;

	int32 NumPlanes = MeshSDFShadowInitializer.CascadeSettings.ShadowBoundsAccurate.Planes.Num();
	const FPlane* PlaneData = MeshSDFShadowInitializer.CascadeSettings.ShadowBoundsAccurate.Planes.GetData();
	FVector4f LocalLightShadowBoundingSphereValue(0, 0, 0, 0);

	WorldToMeshSDFShadowValue = FTranslationMatrix(MeshSDFShadowInitializer.PreShadowTranslation) * SubjectAndReceiverMatrix;

	CullDistanceFieldObjectsForLight(
		GraphBuilder,
		View,
		LightSceneInfo->Proxy,
		DFPT_SignedDistanceField,
		WorldToMeshSDFShadowValue,
		NumPlanes,
		PlaneData,
		LocalLightShadowBoundingSphereValue,
		MeshSDFShadowBounds.W,
		ObjectBufferParameters,
		CulledObjectBufferParameters,
		LightTileIntersectionParameters);
}

FLumenShadowSetup GetShadowForLumenDirectLighting(const FViewInfo& View, FVisibleLightInfo& VisibleLightInfo)
{
	FLumenShadowSetup ShadowSetup;
	ShadowSetup.VirtualShadowMapId = Lumen::UseVirtualShadowMaps() ? VisibleLightInfo.GetVirtualShadowMapId(&View) : INDEX_NONE;
	ShadowSetup.DenseShadowMap = nullptr;

	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];
		if (ProjectedShadowInfo->bIncludeInScreenSpaceShadowMask 
			&& ProjectedShadowInfo->bWholeSceneShadow 
			&& !ProjectedShadowInfo->bRayTracedDistanceField)
		{
			if (ProjectedShadowInfo->bAllocated)
			{
				ShadowSetup.DenseShadowMap = ProjectedShadowInfo;
			}
		}
	}

	return ShadowSetup;
}

const FProjectedShadowInfo* GetShadowForInjectionIntoVolumetricFog(const FVisibleLightInfo & VisibleLightInfo);

void RenderDirectLightIntoLumenCards(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenCardTracingInputs& TracingInputs,
	const FEngineShowFlags& EngineShowFlags,
	TRDGUniformBufferRef<FLumenCardScene> LumenCardSceneUniformBuffer,
	const FLumenGatheredLight& Light,
	const FLumenLightTileScatterParameters& LightTileScatterParameters,
	FRDGBufferSRVRef ShadowMaskTilesSRV)
{
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;

	FLumenCardDirectLighting* PassParameters = GraphBuilder.AllocParameters<FLumenCardDirectLighting>();
	{
		PassParameters->RenderTargets[0] = FRenderTargetBinding(TracingInputs.DirectLightingAtlas, ERenderTargetLoadAction::ELoad);
		PassParameters->VS.LumenCardScene = LumenCardSceneUniformBuffer;
		PassParameters->VS.LightTileScatterParameters = LightTileScatterParameters;
		PassParameters->VS.LightIndex = Light.LightIndex;

		PassParameters->PS.View = View.ViewUniformBuffer;
		PassParameters->PS.LumenCardScene = LumenCardSceneUniformBuffer;
		Lumen::SetDirectLightingDeferredLightUniformBuffer(View, Light.LightSceneInfo, PassParameters->PS.DeferredLightUniforms);

		SetupLightFunctionParameters(Light.LightSceneInfo, 1.0f, PassParameters->PS.LightFunctionParameters);

		PassParameters->PS.ShadowMaskTiles = ShadowMaskTilesSRV;

		// IES profile
		{
			FTexture* IESTextureResource = Light.LightSceneInfo->Proxy->GetIESTextureResource();

			if (View.Family->EngineShowFlags.TexturedLightProfiles && IESTextureResource)
			{
				PassParameters->PS.UseIESProfile = 1;
				PassParameters->PS.IESTexture = IESTextureResource->TextureRHI;
			}
			else
			{
				PassParameters->PS.UseIESProfile = 0;
				PassParameters->PS.IESTexture = GWhiteTexture->TextureRHI;
			}

			PassParameters->PS.IESTextureSampler = TStaticSamplerState<SF_Bilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
		}
	}

	auto VertexShader = View.ShaderMap->GetShader<FRasterizeToLightTilesVS>();

	const FMaterialRenderProxy* LightFunctionMaterialProxy = Light.LightSceneInfo->Proxy->GetLightFunctionMaterial();
	bool bUseLightFunction = true;

	if (!LightFunctionMaterialProxy
		|| !LightFunctionMaterialProxy->GetIncompleteMaterialWithFallback(Scene->GetFeatureLevel()).IsLightFunction()
		|| !EngineShowFlags.LightFunctions)
	{
		bUseLightFunction = false;
		LightFunctionMaterialProxy = UMaterial::GetDefaultMaterial(MD_LightFunction)->GetRenderProxy();
	}

	const bool bUseCloudTransmittance = SetupLightCloudTransmittanceParameters(
		GraphBuilder,
		Scene,
		View,
		GLumenDirectLightingCloudTransmittance != 0 ? Light.LightSceneInfo : nullptr,
		PassParameters->PS.LightCloudTransmittanceParameters);

	FLumenCardDirectLightingPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FLumenCardDirectLightingPS::FLightType>(Light.Type);
	PermutationVector.Set<FLumenCardDirectLightingPS::FShadowMask>(Light.bHasShadows);
	PermutationVector.Set<FLumenCardDirectLightingPS::FLightFunction>(bUseLightFunction);
	PermutationVector.Set<FLumenCardDirectLightingPS::FCloudTransmittance>(bUseCloudTransmittance);
	PermutationVector = FLumenCardDirectLightingPS::RemapPermutation(PermutationVector);

	const FMaterial& Material = LightFunctionMaterialProxy->GetMaterialWithFallback(Scene->GetFeatureLevel(), LightFunctionMaterialProxy);
	const FMaterialShaderMap* MaterialShaderMap = Material.GetRenderingThreadShaderMap();
	auto PixelShader = MaterialShaderMap->GetShader<FLumenCardDirectLightingPS>(PermutationVector);

	ClearUnusedGraphResources(PixelShader, &PassParameters->PS);

	const uint32 DrawIndirectArgOffset = Light.LightIndex * sizeof(FRHIDrawIndirectParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("%s %s", *Light.Name),
		PassParameters,
		ERDGPassFlags::Raster,
		[MaxAtlasSize = LumenSceneData.GetPhysicalAtlasSize(), PassParameters, VertexShader, PixelShader, GlobalShaderMap = View.ShaderMap, LightFunctionMaterialProxy, &Material, &View, DrawIndirectArgOffset](FRHICommandList& RHICmdList)
		{
			DrawQuadsToAtlas(
				MaxAtlasSize,
				VertexShader,
				PixelShader,
				PassParameters,
				GlobalShaderMap,
				TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One>::GetRHI(),
				RHICmdList,
				[LightFunctionMaterialProxy, &Material, &View](FRHICommandList& RHICmdList, TShaderRefBase<FLumenCardDirectLightingPS, FShaderMapPointerTable> Shader, FRHIPixelShader* ShaderRHI, const FLumenCardDirectLightingPS::FParameters& Parameters)
				{
					Shader->SetParameters(RHICmdList, ShaderRHI, LightFunctionMaterialProxy, Material, View);
				},
				PassParameters->VS.LightTileScatterParameters.DrawIndirectArgs,
				DrawIndirectArgOffset);
		});
}

void SampleShadowMap(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	TRDGUniformBufferRef<FLumenCardScene> LumenCardSceneUniformBuffer,
	TArray<FVisibleLightInfo, SceneRenderingAllocator>& VisibleLightInfos,
	const FVirtualShadowMapArray& VirtualShadowMapArray,
	const FLumenGatheredLight& Light,
	const FLumenLightTileScatterParameters& LightTileScatterParameters,
	FRDGBufferUAVRef ShadowMaskTilesUAV)
{
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
	check(Light.bHasShadows);

	FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[Light.LightSceneInfo->Id];
	FLumenShadowSetup ShadowSetup = GetShadowForLumenDirectLighting(View, VisibleLightInfo);

	const bool bUseVirtualShadowMap = ShadowSetup.VirtualShadowMapId != INDEX_NONE;
	if (!bUseVirtualShadowMap)
	{
		// Fallback to a complete shadow map
		ShadowSetup.DenseShadowMap = GetShadowForInjectionIntoVolumetricFog(VisibleLightInfo);
	}
	const bool bUseDenseShadowMap = ShadowSetup.DenseShadowMap != nullptr;

	FLumenDirectLightingSampleShadowMapCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenDirectLightingSampleShadowMapCS::FParameters>();
	{
		PassParameters->IndirectArgBuffer = LightTileScatterParameters.DispatchIndirectArgs;
		PassParameters->RWShadowMaskTiles = ShadowMaskTilesUAV;

		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->LumenCardScene = LumenCardSceneUniformBuffer;
		PassParameters->LightTileScatterParameters = LightTileScatterParameters;
		PassParameters->CardScatterInstanceIndex = 0;
		PassParameters->LightIndex = Light.LightIndex;
		PassParameters->DummyZeroForFixingShaderCompilerBug = 0;
		Lumen::SetDirectLightingDeferredLightUniformBuffer(View, Light.LightSceneInfo, PassParameters->DeferredLightUniforms);
		PassParameters->ForwardLightData = View.ForwardLightingResources->ForwardLightDataUniformBuffer;

		GetVolumeShadowingShaderParameters(
			GraphBuilder,
			View,
			Light.LightSceneInfo,
			ShadowSetup.DenseShadowMap,
			0,
			bUseDenseShadowMap,
			PassParameters->VolumeShadowingShaderParameters);
		
		PassParameters->VirtualShadowMapId = ShadowSetup.VirtualShadowMapId;
		if (bUseVirtualShadowMap)
		{
			PassParameters->VirtualShadowMapSamplingParameters = VirtualShadowMapArray.GetSamplingParameters(GraphBuilder);
		}

		PassParameters->TanLightSourceAngle = FMath::Tan(Light.LightSceneInfo->Proxy->GetLightSourceAngle());
		PassParameters->MaxTraceDistance = Lumen::GetSurfaceCacheOffscreenShadowingMaxTraceDistance();
		PassParameters->StepFactor = FMath::Clamp(GOffscreenShadowingTraceStepFactor, .1f, 10.0f);
		PassParameters->SurfaceBias = FMath::Clamp(GShadowingSurfaceBias, .01f, 100.0f);
		PassParameters->SlopeScaledSurfaceBias = FMath::Clamp(GShadowingSlopeScaledSurfaceBias, .01f, 100.0f);
		PassParameters->VirtualShadowMapSurfaceBias = FMath::Clamp(GLumenDirectLightingVirtualShadowMapBias, .01f, 100.0f);
		PassParameters->ForceOffscreenShadowing = GLumenDirectLightingForceOffscreenShadowing;
		PassParameters->ForceShadowMaps = GLumenDirectLightingForceForceShadowMaps;
	}

	FLumenDirectLightingSampleShadowMapCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FLumenDirectLightingSampleShadowMapCS::FLightType>(Light.Type);
	PermutationVector.Set<FLumenDirectLightingSampleShadowMapCS::FVirtualShadowMap>(bUseVirtualShadowMap);
	PermutationVector.Set<FLumenDirectLightingSampleShadowMapCS::FDynamicallyShadowed>(bUseDenseShadowMap);
	PermutationVector.Set<FLumenDirectLightingSampleShadowMapCS::FDenseShadowMap>(bUseDenseShadowMap);
	TShaderRef<FLumenDirectLightingSampleShadowMapCS> ComputeShader = View.ShaderMap->GetShader<FLumenDirectLightingSampleShadowMapCS>(PermutationVector);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ShadowMapPass %s", *Light.Name),
		ComputeShader,
		PassParameters,
		LightTileScatterParameters.DispatchIndirectArgs,
		Light.LightIndex * sizeof(FRHIDispatchIndirectParameters));
}

void TraceDistanceFieldShadows(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	TRDGUniformBufferRef<FLumenCardScene> LumenCardSceneUniformBuffer,
	const FLumenGatheredLight& Light,
	const FLumenLightTileScatterParameters& LightTileScatterParameters,
	FRDGBufferUAVRef ShadowMaskTilesUAV)
{
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
	check(Light.bHasShadows);

	FDistanceFieldObjectBufferParameters ObjectBufferParameters = DistanceField::SetupObjectBufferParameters(Scene->DistanceFieldSceneData);

	FLightTileIntersectionParameters LightTileIntersectionParameters;
	FDistanceFieldCulledObjectBufferParameters CulledObjectBufferParameters;
	FMatrix WorldToMeshSDFShadowValue = FMatrix::Identity;

	const bool bTraceMeshSDFs = Light.bHasShadows
		&& Light.Type == ELumenLightType::Directional
		&& DoesPlatformSupportDistanceFieldShadowing(View.GetShaderPlatform())
		&& GLumenDirectLightingOffscreenShadowingTraceMeshSDFs != 0
		&& Lumen::UseMeshSDFTracing()
		&& ObjectBufferParameters.NumSceneObjects > 0;

	if (bTraceMeshSDFs)
	{
		CullMeshSDFsForLightCards(GraphBuilder, Scene, View, Light.LightSceneInfo, ObjectBufferParameters, WorldToMeshSDFShadowValue, CulledObjectBufferParameters, LightTileIntersectionParameters);
	}

	FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::FParameters>();
	{
		PassParameters->IndirectArgBuffer = LightTileScatterParameters.DispatchIndirectArgs;
		PassParameters->RWShadowMaskTiles = ShadowMaskTilesUAV;

		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->LumenCardScene = LumenCardSceneUniformBuffer;
		PassParameters->LightTileScatterParameters = LightTileScatterParameters;
		PassParameters->LightIndex = Light.LightIndex;
		PassParameters->DummyZeroForFixingShaderCompilerBug = 0;
		Lumen::SetDirectLightingDeferredLightUniformBuffer(View, Light.LightSceneInfo, PassParameters->DeferredLightUniforms);

		PassParameters->ObjectBufferParameters = ObjectBufferParameters;
		PassParameters->CulledObjectBufferParameters = CulledObjectBufferParameters;
		PassParameters->LightTileIntersectionParameters = LightTileIntersectionParameters;

		FDistanceFieldAtlasParameters DistanceFieldAtlasParameters = DistanceField::SetupAtlasParameters(Scene->DistanceFieldSceneData);

		PassParameters->DistanceFieldAtlasParameters = DistanceFieldAtlasParameters;
		PassParameters->WorldToShadow = WorldToMeshSDFShadowValue;
		extern float GTwoSidedMeshDistanceBias;
		PassParameters->TwoSidedMeshDistanceBias = GTwoSidedMeshDistanceBias;

		PassParameters->TanLightSourceAngle = FMath::Tan(Light.LightSceneInfo->Proxy->GetLightSourceAngle());
		PassParameters->MaxTraceDistance = Lumen::GetSurfaceCacheOffscreenShadowingMaxTraceDistance();
		PassParameters->StepFactor = FMath::Clamp(GOffscreenShadowingTraceStepFactor, .1f, 10.0f);
		PassParameters->SurfaceBias = FMath::Clamp(GShadowingSurfaceBias, .01f, 100.0f);
		PassParameters->SlopeScaledSurfaceBias = FMath::Clamp(GShadowingSlopeScaledSurfaceBias, .01f, 100.0f);
		PassParameters->SDFSurfaceBiasScale = FMath::Clamp(GOffscreenShadowingSDFSurfaceBiasScale, .01f, 100.0f);
	}

	FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::FLightType>(Light.Type);
	PermutationVector.Set<FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::FTraceMeshSDFs>(bTraceMeshSDFs);
	PermutationVector = FLumenSceneDirectLightingTraceDistanceFieldShadowsCS::RemapPermutation(PermutationVector);

	TShaderRef<FLumenSceneDirectLightingTraceDistanceFieldShadowsCS> ComputeShader = View.ShaderMap->GetShader<FLumenSceneDirectLightingTraceDistanceFieldShadowsCS>(PermutationVector);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("DistanceFieldShadowPass %s", *Light.Name),
		ComputeShader,
		PassParameters,
		LightTileScatterParameters.DispatchIndirectArgs,
		Light.LightIndex * sizeof(FRHIDispatchIndirectParameters));
}

// Must match FLumenPackedLight in LumenSceneDirectLighting.ush
struct FLumenPackedLight
{
	FVector3f Position;
	float InvRadius;

	FVector3f Color;
	float FalloffExponent;

	FVector3f Direction;
	float SpecularScale;

	FVector3f Tangent;
	float SourceRadius;

	FVector2f SpotAngles;
	float SoftSourceRadius;
	float SourceLength;

	float RectLightBarnCosAngle;
	float RectLightBarnLength;
	uint32 LightType;
	uint32 VirtualShadowMapId;

	FVector4f InfluenceSphere;

	FVector3f ProxyPosition;
	float ProxyRadius;

	FVector3f ProxyDirection;
	float CosConeAngle;
	
	float SinConeAngle;
	FVector3f Padding;
};

FRDGBufferRef CreateLumenLightDataBuffer(FRDGBuilder& GraphBuilder, const FViewInfo& View, const TArray<FLumenGatheredLight, TInlineAllocator<64>>& GatheredLights)
{
	TArray<FLumenPackedLight, TInlineAllocator<16>> PackedLightData;
	PackedLightData.SetNum(FMath::RoundUpToPowerOfTwo(FMath::Max(GatheredLights.Num(), 16)));

	for (int32 LightIndex = 0; LightIndex < GatheredLights.Num(); ++LightIndex)
	{
		const FLightSceneInfo* LightSceneInfo = GatheredLights[LightIndex].LightSceneInfo;
		const FSphere LightBounds = LightSceneInfo->Proxy->GetBoundingSphere();

		FLightShaderParameters ShaderParameters;
		LightSceneInfo->Proxy->GetLightShaderParameters(ShaderParameters);

		if (LightSceneInfo->Proxy->IsInverseSquared())
		{
			ShaderParameters.FalloffExponent = 0;
		}
		ShaderParameters.Color *= LightSceneInfo->Proxy->GetIndirectLightingScale();

		FLumenPackedLight& LightData = PackedLightData[LightIndex];
		LightData.Position = ShaderParameters.Position;
		LightData.InvRadius = ShaderParameters.InvRadius;

		LightData.Color = ShaderParameters.Color;
		LightData.FalloffExponent = ShaderParameters.FalloffExponent;

		LightData.Direction = ShaderParameters.Direction;
		LightData.SpecularScale = ShaderParameters.SpecularScale;

		LightData.Tangent = ShaderParameters.Tangent;
		LightData.SourceRadius = ShaderParameters.SourceRadius;

		LightData.SpotAngles = ShaderParameters.SpotAngles;
		LightData.SoftSourceRadius = ShaderParameters.SoftSourceRadius;
		LightData.SourceLength = ShaderParameters.SourceLength;

		LightData.RectLightBarnCosAngle = ShaderParameters.RectLightBarnCosAngle;
		LightData.RectLightBarnLength = ShaderParameters.RectLightBarnLength;
		LightData.LightType = LightSceneInfo->Proxy->GetLightType();
		LightData.VirtualShadowMapId = 0;

		LightData.InfluenceSphere = FVector4f(LightBounds.Center, LightBounds.W);

		LightData.ProxyPosition = FVector3f(LightSceneInfo->Proxy->GetPosition());
		LightData.ProxyRadius = LightSceneInfo->Proxy->GetRadius();

		LightData.ProxyDirection = LightSceneInfo->Proxy->GetDirection();
		LightData.CosConeAngle = FMath::Cos(LightSceneInfo->Proxy->GetOuterConeAngle());

		LightData.SinConeAngle = FMath::Sin(LightSceneInfo->Proxy->GetOuterConeAngle());
		LightData.Padding = FVector3f(0.0f, 0.0f, 0.0f);
	}

	FRDGBufferRef LightDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("Lumen.DirectLighting.Lights"), PackedLightData);
	return LightDataBuffer;
}

void FDeferredShadingSceneRenderer::RenderDirectLightingForLumenScene(
	FRDGBuilder& GraphBuilder,
	const FLumenCardTracingInputs& TracingInputs,
	FGlobalShaderMap* GlobalShaderMap,
	const FLumenCardUpdateContext& CardUpdateContext)
{
	LLM_SCOPE_BYTAG(Lumen);

	if (GLumenDirectLighting)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "DirectLighting");
		QUICK_SCOPE_CYCLE_COUNTER(RenderDirectLightingForLumenScene);

		const FViewInfo& View = Views[0];
		FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;

		TRDGUniformBufferRef<FLumenCardScene> LumenCardSceneUniformBuffer = TracingInputs.LumenCardSceneUniformBuffer;

		ClearLumenSceneDirectLighting(
			View,
			GraphBuilder,
			LumenSceneData,
			TracingInputs,
			CardUpdateContext);

		TArray<FLumenGatheredLight, TInlineAllocator<64>> GatheredLights;

		for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
		{
			const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
			const FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

			if (LightSceneInfo->ShouldRenderLightViewIndependent()
				&& LightSceneInfo->ShouldRenderLight(View, true)
				&& LightSceneInfo->Proxy->GetIndirectLightingScale() > 0.0f)
			{
				const FLumenGatheredLight GatheredLight(LightSceneInfo, /*LightIndex*/ GatheredLights.Num());
				GatheredLights.Add(GatheredLight);
			}
		}

		FRDGBufferRef LumenPackedLights = CreateLumenLightDataBuffer(GraphBuilder, View, GatheredLights);

		const uint32 MaxLightTilesTilesX = FMath::DivideAndRoundUp<uint32>(CardUpdateContext.UpdateAtlasSize.X, Lumen::CardTileSize);
		const uint32 MaxLightTilesTilesY = FMath::DivideAndRoundUp<uint32>(CardUpdateContext.UpdateAtlasSize.Y, Lumen::CardTileSize);
		const uint32 MaxLightTiles = MaxLightTilesTilesX * MaxLightTilesTilesY;
		const uint32 NumLightsRoundedUp = FMath::RoundUpToPowerOfTwo(FMath::Max(GatheredLights.Num(), 1));
		const uint32 MaxLightsPerTile = FMath::Max(GLumenDirectLightingMaxLightsPerTile, 1);
		const uint32 MaxCulledCardTiles = MaxLightsPerTile * MaxLightTiles;

		// 2 bits per shadow mask texel
		const uint32 ShadowMaskTilesSize = FMath::Max(4 * MaxCulledCardTiles, 1024u);
		FRDGBufferRef ShadowMaskTiles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ShadowMaskTilesSize), TEXT("Lumen.DirectLighting.ShadowMaskTiles"));

		FRDGBufferRef LightTileAllocator = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("Lumen.DirectLighting.LightTileAllocator"));
		FRDGBufferRef LightTiles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(2 * sizeof(uint32), MaxCulledCardTiles), TEXT("Lumen.DirectLighting.LightTiles"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LightTileAllocator), 0);

		FRDGBufferRef LightTileAllocatorPerLight = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumLightsRoundedUp), TEXT("Lumen.DirectLighting.LightTileAllocatorPerLight"));
		FRDGBufferRef LightTileOffsetsPerLight = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumLightsRoundedUp), TEXT("Lumen.DirectLighting.LightTileOffsetsPerLight"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LightTileAllocatorPerLight), 0);

		// Build a list of light tiles for future processing
		{
			FBuildLightTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBuildLightTilesCS::FParameters>();
			PassParameters->IndirectArgBuffer = CardUpdateContext.DispatchCardPageIndicesIndirectArgs;
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->LumenCardScene = LumenCardSceneUniformBuffer;
			PassParameters->LumenPackedLights = GraphBuilder.CreateSRV(LumenPackedLights);
			PassParameters->RWLightTileAllocator = GraphBuilder.CreateUAV(LightTileAllocator);
			PassParameters->RWLightTiles = GraphBuilder.CreateUAV(LightTiles);
			PassParameters->RWLightTileAllocatorPerLight = GraphBuilder.CreateUAV(LightTileAllocatorPerLight);
			PassParameters->CardPageIndexAllocator = GraphBuilder.CreateSRV(CardUpdateContext.CardPageIndexAllocator);
			PassParameters->CardPageIndexData = GraphBuilder.CreateSRV(CardUpdateContext.CardPageIndexData);
			PassParameters->MaxLightsPerTile = MaxLightsPerTile;
			PassParameters->NumLights = GatheredLights.Num();
			auto ComputeShader = View.ShaderMap->GetShader<FBuildLightTilesCS>();

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("BuildLightTiles"),
				ComputeShader,
				PassParameters,
				CardUpdateContext.DispatchCardPageIndicesIndirectArgs,
				FLumenCardUpdateContext::EIndirectArgOffset::ThreadPerTile);
		}

		// Compute prefix sum for card tile array
		{
			FComputeLightTileOffsetsPerLightCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeLightTileOffsetsPerLightCS::FParameters>();
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->RWLightTileOffsetsPerLight = GraphBuilder.CreateUAV(LightTileOffsetsPerLight);
			PassParameters->LightTileAllocatorPerLight = GraphBuilder.CreateSRV(LightTileAllocatorPerLight);
			PassParameters->NumLights = GatheredLights.Num();

			auto ComputeShader = View.ShaderMap->GetShader<FComputeLightTileOffsetsPerLightCS>();

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ComputeLightTileOffsetsPerLight"),
				ComputeShader,
				PassParameters,
				FIntVector(1, 1, 1));
		}

		enum class EDispatchTilesIndirectArgOffset
		{
			GroupPerTile = 0 * sizeof(FRHIDispatchIndirectParameters),
			ThreadPerTile = 0 * sizeof(FRHIDispatchIndirectParameters),
			MAX = 2,
		};

		// Initialize indirect args for culled tiles
		FRDGBufferRef DispatchLightTilesIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>((int)EDispatchTilesIndirectArgOffset::MAX), TEXT("Lumen.DirectLighting.DispatchLightTilesIndirectArgs"));
		FRDGBufferRef DrawTilesPerLightIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(NumLightsRoundedUp), TEXT("Lumen.DirectLighting.DrawTilesPerLightIndirectArgs"));
		FRDGBufferRef DispatchTilesPerLightIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(NumLightsRoundedUp), TEXT("Lumen.DirectLighting.DispatchTilesPerLightIndirectArgs"));
		{
			FInitializeLightTileIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FInitializeLightTileIndirectArgsCS::FParameters>();
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->RWDispatchLightTilesIndirectArgs = GraphBuilder.CreateUAV(DispatchLightTilesIndirectArgs);
			PassParameters->RWDrawTilesPerLightIndirectArgs = GraphBuilder.CreateUAV(DrawTilesPerLightIndirectArgs);
			PassParameters->RWDispatchTilesPerLightIndirectArgs = GraphBuilder.CreateUAV(DispatchTilesPerLightIndirectArgs);
			PassParameters->LightTileAllocator = GraphBuilder.CreateSRV(LightTileAllocator);
			PassParameters->LightTileAllocatorPerLight = GraphBuilder.CreateSRV(LightTileAllocatorPerLight);
			PassParameters->VertexCountPerInstanceIndirect = GRHISupportsRectTopology ? 3 : 6;
			PassParameters->NumLights = GatheredLights.Num();

			auto ComputeShader = View.ShaderMap->GetShader<FInitializeLightTileIndirectArgsCS>();

			const FIntVector GroupSize = FComputeShaderUtils::GetGroupCount(GatheredLights.Num(), FInitializeLightTileIndirectArgsCS::GetGroupSize());

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("InitializeLightTileIndirectArgs"),
				ComputeShader,
				PassParameters,
				GroupSize);
		}

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LightTileAllocatorPerLight), 0);

		// Compact card tile array
		{
			FRDGBufferRef CompactedLightTiles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(2 * sizeof(uint32), MaxCulledCardTiles), TEXT("Lumen.DirectLighting.CompactedLightTiles"));

			FCompactLightTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCompactLightTilesCS::FParameters>();
			PassParameters->IndirectArgBuffer = DispatchLightTilesIndirectArgs;
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->RWCompactedLightTiles = GraphBuilder.CreateUAV(CompactedLightTiles);
			PassParameters->RWLightTileAllocatorPerLight = GraphBuilder.CreateUAV(LightTileAllocatorPerLight);
			PassParameters->LightTileAllocator = GraphBuilder.CreateSRV(LightTileAllocator);
			PassParameters->LightTiles = GraphBuilder.CreateSRV(LightTiles);
			PassParameters->LightTileOffsetsPerLight = GraphBuilder.CreateSRV(LightTileOffsetsPerLight);
			PassParameters->NumLights = GatheredLights.Num();

			auto ComputeShader = View.ShaderMap->GetShader<FCompactLightTilesCS>();

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CompactLightTiles"),
				ComputeShader,
				PassParameters,
				DispatchLightTilesIndirectArgs,
				(int)EDispatchTilesIndirectArgOffset::ThreadPerTile);

			LightTiles = CompactedLightTiles;
		}

		FLumenLightTileScatterParameters LightTileScatterParameters;
		LightTileScatterParameters.DrawIndirectArgs = DrawTilesPerLightIndirectArgs;
		LightTileScatterParameters.DispatchIndirectArgs = DispatchTilesPerLightIndirectArgs;
		LightTileScatterParameters.LightTileAllocator = GraphBuilder.CreateSRV(LightTileAllocator);
		LightTileScatterParameters.LightTiles = GraphBuilder.CreateSRV(LightTiles);
		LightTileScatterParameters.LightTileOffsetsPerLight = GraphBuilder.CreateSRV(LightTileOffsetsPerLight);

		// Apply shadow map
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Shadow map");

			FRDGBufferUAVRef ShadowMaskTilesUAV = GraphBuilder.CreateUAV(ShadowMaskTiles, ERDGUnorderedAccessViewFlags::SkipBarrier);

			for (int32 LightIndex = 0; LightIndex < GatheredLights.Num(); ++LightIndex)
			{
				const FLumenGatheredLight& GatheredLight = GatheredLights[LightIndex];
				if (GatheredLight.bHasShadows)
				{
					SampleShadowMap(
						GraphBuilder,
						Scene,
						View,
						LumenCardSceneUniformBuffer,
						VisibleLightInfos,
						VirtualShadowMapArray,
						GatheredLight,
						LightTileScatterParameters,
						ShadowMaskTilesUAV);
				}
			}
		}

		// Offscreen shadowing
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Offscreen shadows");

			FRDGBufferUAVRef ShadowMaskTilesUAV = GraphBuilder.CreateUAV(ShadowMaskTiles, ERDGUnorderedAccessViewFlags::SkipBarrier);

			if (Lumen::UseHardwareRayTracedDirectLighting())
			{
				TraceLumenHardwareRayTracedDirectLightingShadows(
					GraphBuilder,
					Scene,
					View,
					TracingInputs,
					DispatchLightTilesIndirectArgs,
					LightTileAllocator,
					LightTiles,
					LumenPackedLights,
					ShadowMaskTilesUAV);
			}
			else
			{
				for (int32 LightIndex = 0; LightIndex < GatheredLights.Num(); ++LightIndex)
				{
					const FLumenGatheredLight& GatheredLight = GatheredLights[LightIndex];
					if (GatheredLight.bHasShadows)
					{
						TraceDistanceFieldShadows(
							GraphBuilder,
							Scene,
							View,
							LumenCardSceneUniformBuffer,
							GatheredLight,
							LightTileScatterParameters,
							ShadowMaskTilesUAV);
					}
				}
			}
		}

		// Apply lights
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Lights");
				
			FRDGBufferSRVRef ShadowMaskTilesSRV = GraphBuilder.CreateSRV(ShadowMaskTiles);

			for (int32 LightIndex = 0; LightIndex < GatheredLights.Num(); ++LightIndex)
			{
				RenderDirectLightIntoLumenCards(
					GraphBuilder,
					Scene,
					View,
					TracingInputs,
					ViewFamily.EngineShowFlags,
					LumenCardSceneUniformBuffer,
					GatheredLights[LightIndex],
					LightTileScatterParameters,
					ShadowMaskTilesSRV);
			}
		}

		// Update Final Lighting
		Lumen::CombineLumenSceneLighting(
			Scene,
			View,
			GraphBuilder,
			TracingInputs,
			CardUpdateContext);
	}
}
