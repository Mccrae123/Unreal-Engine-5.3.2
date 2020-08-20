// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	VolumetricCloudRendering.cpp
=============================================================================*/

#include "VolumetricCloudRendering.h"
#include "Components/VolumetricCloudComponent.h"
#include "VolumetricCloudProxy.h"
#include "DeferredShadingRenderer.h"
#include "PixelShaderUtils.h"
#include "RenderCore/Public/RenderGraphUtils.h"
#include "ScenePrivate.h"
#include "MeshPassProcessor.inl"
#include "StaticMeshResources.h"
#include "SkyAtmosphereRendering.h"
#include "VolumeLighting.h"
#include "DynamicPrimitiveDrawing.h"
#include "GpuDebugRendering.h"
#include "CanvasTypes.h"
#include "RenderTargetTemp.h"
#include "VolumetricRenderTarget.h"
#include "BlueNoise.h"
#include "FogRendering.h"
#include "SkyAtmosphereRendering.h"


//PRAGMA_DISABLE_OPTIMIZATION


////////////////////////////////////////////////////////////////////////// Cloud rendering and tracing

// The runtime ON/OFF toggle
static TAutoConsoleVariable<int32> CVarVolumetricCloud(
	TEXT("r.VolumetricCloud"), 1,
	TEXT("VolumetricCloud components are rendered when this is not 0, otherwise ignored."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVolumetricCloudDistanceToSampleMaxCount(
	TEXT("r.VolumetricCloud.DistanceToSampleMaxCount"), 15.0f,
	TEXT("The number of ray marching samples will span 0 to SampleCountMax from 0 to DistanceToSampleCountMax (kilometers). After that it is capped at SampleCountMax."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarVolumetricCloudViewRaySampleMaxCount(
	TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"), 768,
	TEXT("The maximum number of samples taken while ray marching view primary rays."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarVolumetricCloudReflectionRaySampleMaxCount(
	TEXT("r.VolumetricCloud.ReflectionRaySampleMaxCount"), 80,
	TEXT("The maximum number of samples taken while ray marching primary rays in reflections."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudOpaqueIntersectionMode(
	TEXT("r.VolumetricCloud.OpaqueIntersectionMode"), 2,
	TEXT("0: no intersection with opaque. 1: trace up to the far distance and interesect during composition (sharp transition, single layer). 2: trace up to the depth buffer and take into account HZB: softer but can have artefact at edges when flying in the cloud layer."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudHighQualityAerialPerspective(
	TEXT("r.VolumetricCloud.HighQualityAerialPerspective"), 0,
	TEXT("True if we want to trace the aerial perspective per pixel on cloud instead of using the aerial persepctive texture. Only possible to do when r.VolumetricRenderTarget=1."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudHzbCulling(
	TEXT("r.VolumetricCloud.HzbCulling"), 1,
	TEXT("True if we want the HZB to be use in order to not trace behind opaque surfaces. Should be 0 when r.VolumetricRenderTarget.Mode is 2."),
	ECVF_Scalability);

////////////////////////////////////////////////////////////////////////// Shadow tracing

static TAutoConsoleVariable<float> CVarVolumetricCloudShadowViewRaySampleMaxCount(
	TEXT("r.VolumetricCloud.Shadow.ViewRaySampleMaxCount"), 80,
	TEXT("The maximum number of samples taken while ray marching shadow rays."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarVolumetricCloudShadowReflectionRaySampleMaxCount(
	TEXT("r.VolumetricCloud.Shadow.ReflectionRaySampleMaxCount"), 24,
	TEXT("The maximum number of samples taken while ray marching shadow rays in reflections."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudShadowSampleAtmosphericLightShadowmap(
	TEXT("r.VolumetricCloud.Shadow.SampleAtmosphericLightShadowmap"), 1,
	TEXT("Enable the sampling of atmospheric lights shadow map in order to produce volumetric shadows."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

////////////////////////////////////////////////////////////////////////// Cloud SKY AO

static TAutoConsoleVariable<int32> CVarVolumetricCloudSkyAO(
	TEXT("r.VolumetricCloud.SkyAO"), 1,
	TEXT("The resolution of the texture storting occlusion information for the lighting coming from the ground."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudSkyAODebug(
	TEXT("r.VolumetricCloud.SkyAO.Debug"), 0,
	TEXT("Print information to debug the cloud sky ao map."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarVolumetricCloudSkyAOSnapLength(
	TEXT("r.VolumetricCloud.SkyAO.SnapLength"), 20.0f,
	TEXT("Snapping size in kilometers of the cloud SkyAO texture position to avoid flickering."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudSkyAOMaxResolution(
	TEXT("r.VolumetricCloud.SkyAO.MaxResolution"), 2048,
	TEXT("The maximum resolution of the texture storing ambiant occlusion information for the environment lighting coming from sky light."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudSkyAOTraceSampleCount(
	TEXT("r.VolumetricCloud.SkyAO.TraceSampleCount"), 10,
	TEXT("The number of sample taken to evaluate ground lighting occlusion."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudSkyAOFiltering(
	TEXT("r.VolumetricCloud.SkyAO.Filtering"), 1,
	TEXT("Enable / disable the sky AO dilation/smoothing filter."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

////////////////////////////////////////////////////////////////////////// Cloud shadow map

static TAutoConsoleVariable<int32> CVarVolumetricCloudShadowMap(
	TEXT("r.VolumetricCloud.ShadowMap"), 1,
	TEXT("Enable / disable the shadow map."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudShadowMapDebug(
	TEXT("r.VolumetricCloud.ShadowMap.Debug"), 0,
	TEXT("Print information to debug the cloud shadow map."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarVolumetricCloudShadowMapSnapLength(
	TEXT("r.VolumetricCloud.ShadowMap.SnapLength"), 20.0f,
	TEXT("Snapping size in kilometers of the cloud shadowmap position to avoid flickering."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudShadowMapMaxResolution(
	TEXT("r.VolumetricCloud.ShadowMap.MaxResolution"), 2048,
	TEXT("The maximum resolution of the cloud shadow map."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarVolumetricCloudShadowFiltering(
	TEXT("r.VolumetricCloud.ShadowMap.Filtering"), 1,
	TEXT("Enable / disable the shadow map dilation/smoothing filter."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

////////////////////////////////////////////////////////////////////////// Lighting component controls

static TAutoConsoleVariable<int32> CVarVolumetricCloudEnableAerialPerspectiveSampling(
	TEXT("r.VolumetricCloud.EnableAerialPerspectiveSampling"), 1,
	TEXT("Enable/Disable the aerial perspective contribution on clouds."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVolumetricCloudEnableDistantSkyLightSampling(
	TEXT("r.VolumetricCloud.EnableDistantSkyLightSampling"), 1,
	TEXT("Enable/Disable the distant sky light contribution on clouds."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVolumetricCloudEnableAtmosphericLightsSampling(
	TEXT("r.VolumetricCloud.EnableAtmosphericLightsSampling"), 1,
	TEXT("Enable/Disable the atmospheric lights contribution on clouds."),
	ECVF_RenderThreadSafe);

////////////////////////////////////////////////////////////////////////// 

static TAutoConsoleVariable<int32> CVarVolumetricCloudDebugSampleCountMode(
	TEXT("r.VolumetricCloud.Debug.SampleCountMode"), 0,
	TEXT("Debug mode for per trace sample count."));

////////////////////////////////////////////////////////////////////////// 


static bool ShouldPipelineCompileVolumetricCloudShader(EShaderPlatform ShaderPlatform)
{
	// Requires SM5 or ES3_1 (GL/Vulkan) for compute shaders and volume textures support.
	return RHISupportsComputeShaders(ShaderPlatform);
}

bool ShouldRenderVolumetricCloud(const FScene* Scene, const FEngineShowFlags& EngineShowFlags)
{
	if (Scene && Scene->HasVolumetricCloud() ) //&& EngineShowFlags.VolumetricCloud) TODO apply 10810454 for clouds
	{
		const FVolumetricCloudRenderSceneInfo* VolumetricCloud = Scene->GetVolumetricCloudSceneInfo();
		check(VolumetricCloud);

		const bool bShadersCompiled = ShouldPipelineCompileVolumetricCloudShader(Scene->GetShaderPlatform());

		FLightSceneInfo* AtmosphericLight0 = Scene->AtmosphereLights[0];
		return bShadersCompiled && CVarVolumetricCloud.GetValueOnRenderThread() > 0 && AtmosphericLight0!=nullptr;
	}
	return false;
}

static bool ShouldRenderCloudShadowmap(const FLightSceneProxy* AtmosphericLight)
{
	return CVarVolumetricCloudShadowMap.GetValueOnRenderThread() > 0 && AtmosphericLight && AtmosphericLight->GetCastCloudShadows();
}

//////////////////////////////////////////////////////////////////////////

static float GetVolumetricCloudShadowmapStrength(const FLightSceneProxy* AtmosphericLight)
{
	if (AtmosphericLight)
	{
		return AtmosphericLight->GetCloudShadowStrength();
	}
	return 1.0f;
}

static int32 GetVolumetricCloudShadowMapResolution(const FLightSceneProxy* AtmosphericLight)
{
	if (AtmosphericLight)
	{
		return FMath::Min( int32(512.0f * float(AtmosphericLight->GetCloudShadowMapResolutionScale())), CVarVolumetricCloudShadowMapMaxResolution.GetValueOnAnyThread());
	}
	return 32;
}

static float GetVolumetricCloudShadowMapExtentKm(const FLightSceneProxy* AtmosphericLight)
{
	if (AtmosphericLight)
	{
		return AtmosphericLight->GetCloudShadowExtent();
	}
	return 1.0f;
}

static int32 GetVolumetricCloudReceiveAtmosphericLightShadowmap(const FLightSceneProxy* AtmosphericLight)
{
	if (AtmosphericLight)
	{
		return AtmosphericLight->GetCastShadowsOnClouds();
	}
	return 1.0f;
}

static FLinearColor GetVolumetricCloudScatteredLuminanceScale(const FLightSceneProxy* AtmosphericLight)
{
	if (AtmosphericLight)
	{
		return AtmosphericLight->GetCloudScatteredLuminanceScale();
	}
	return FLinearColor::White;
}

static bool ShouldRenderCloudSkyAO(const FSkyLightSceneProxy* SkyLight)
{
	return CVarVolumetricCloudSkyAO.GetValueOnRenderThread() > 0 && SkyLight && SkyLight->bCloudAmbientOcclusion;
}

static float GetVolumetricCloudSkyAOStrength(const FSkyLightSceneProxy* SkyLight)
{
	if (SkyLight)
	{
		return SkyLight->CloudAmbientOcclusionStrength;
	}
	return 1.0f;
}

static int32 GetVolumetricCloudSkyAOResolution(const FSkyLightSceneProxy* SkyLight)
{
	if (SkyLight)
	{
		return FMath::Min(int32(512.0f * float(SkyLight->CloudAmbientOcclusionMapResolutionScale)), CVarVolumetricCloudShadowMapMaxResolution.GetValueOnAnyThread());
	}
	return 32;
}

static float GetVolumetricCloudSkyAOExtentKm(const FSkyLightSceneProxy* SkyLight)
{
	if (SkyLight)
	{
		return SkyLight->CloudAmbientOcclusionExtent;
	}
	return 1.0f;
}

static float GetVolumetricCloudSkyAOApertureScale(const FSkyLightSceneProxy* SkyLight)
{
	if (SkyLight)
	{
		return SkyLight->CloudAmbientOcclusionApertureScale;
	}
	return 1.0f;
}

static bool ShouldUsePerSampleAtmosphereTransmittance(const FScene* Scene, const FViewInfo* InViewIfDynamicMeshCommand)
{
	return Scene->VolumetricCloud && Scene->VolumetricCloud->GetVolumetricCloudSceneProxy().bUsePerSampleAtmosphericLightTransmittance &&
		Scene->HasSkyAtmosphere() && ShouldRenderSkyAtmosphere(Scene, InViewIfDynamicMeshCommand->Family->EngineShowFlags);
}


//////////////////////////////////////////////////////////////////////////


void GetCloudShadowAOData(FVolumetricCloudRenderSceneInfo* CloudInfo, FViewInfo& View, FRDGBuilder& GraphBuilder, FCloudShadowAOData& OutData)
{
	// We pick up the texture if they exists, the decision has been mande to render them before already.
	OutData.bShouldSampleCloudShadow = CloudInfo && (View.VolumetricCloudShadowMap[0].IsValid() || View.VolumetricCloudShadowMap[1].IsValid());
	OutData.VolumetricCloudShadowMap[0] = GraphBuilder.RegisterExternalTexture(OutData.bShouldSampleCloudShadow && View.VolumetricCloudShadowMap[0].IsValid() ? View.VolumetricCloudShadowMap[0] : GSystemTextures.BlackDummy);
	OutData.VolumetricCloudShadowMap[1] = GraphBuilder.RegisterExternalTexture(OutData.bShouldSampleCloudShadow && View.VolumetricCloudShadowMap[1].IsValid() ? View.VolumetricCloudShadowMap[1] : GSystemTextures.BlackDummy);

	OutData.bShouldSampleCloudSkyAO = CloudInfo && View.VolumetricCloudSkyAO.IsValid();
	OutData.VolumetricCloudSkyAO = GraphBuilder.RegisterExternalTexture(OutData.bShouldSampleCloudSkyAO ? View.VolumetricCloudSkyAO : GSystemTextures.BlackDummy);
}


/*=============================================================================
	FVolumetricCloudRenderSceneInfo implementation.
=============================================================================*/


FVolumetricCloudRenderSceneInfo::FVolumetricCloudRenderSceneInfo(FVolumetricCloudSceneProxy& VolumetricCloudSceneProxyIn)
	:VolumetricCloudSceneProxy(VolumetricCloudSceneProxyIn)
{
}

FVolumetricCloudRenderSceneInfo::~FVolumetricCloudRenderSceneInfo()
{
}



/*=============================================================================
	FScene functions
=============================================================================*/



void FScene::AddVolumetricCloud(FVolumetricCloudSceneProxy* VolumetricCloudSceneProxy)
{
	check(VolumetricCloudSceneProxy);
	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(FAddVolumetricCloudCommand)(
		[Scene, VolumetricCloudSceneProxy](FRHICommandListImmediate& RHICmdList)
		{
			check(!Scene->VolumetricCloudStack.Contains(VolumetricCloudSceneProxy));
			Scene->VolumetricCloudStack.Push(VolumetricCloudSceneProxy);

			VolumetricCloudSceneProxy->RenderSceneInfo = new FVolumetricCloudRenderSceneInfo(*VolumetricCloudSceneProxy);

			// Use the most recently enabled VolumetricCloud
			Scene->VolumetricCloud = VolumetricCloudSceneProxy->RenderSceneInfo;
		} );
}

void FScene::RemoveVolumetricCloud(FVolumetricCloudSceneProxy* VolumetricCloudSceneProxy)
{
	check(VolumetricCloudSceneProxy);
	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(FRemoveVolumetricCloudCommand)(
		[Scene, VolumetricCloudSceneProxy](FRHICommandListImmediate& RHICmdList)
		{
			delete VolumetricCloudSceneProxy->RenderSceneInfo;
			Scene->VolumetricCloudStack.RemoveSingle(VolumetricCloudSceneProxy);

			if (Scene->VolumetricCloudStack.Num() > 0)
			{
				// Use the most recently enabled VolumetricCloud
				Scene->VolumetricCloud = Scene->VolumetricCloudStack.Last()->RenderSceneInfo;
			}
			else
			{
				Scene->VolumetricCloud = nullptr;
			}
		} );
}



/*=============================================================================
	VolumetricCloud rendering functions
=============================================================================*/



DECLARE_GPU_STAT(VolumetricCloud);
DECLARE_GPU_STAT(VolumetricCloudShadow);



FORCEINLINE bool IsVolumetricCloudMaterialSupported(const EShaderPlatform Platform)
{
	return GetMaxSupportedFeatureLevel(Platform) >= ERHIFeatureLevel::SM5;
}


FORCEINLINE bool IsMaterialCompatibleWithVolumetricCloud(const FMaterialShaderParameters& Material, const EShaderPlatform Platform)
{
	return IsVolumetricCloudMaterialSupported(Platform) && Material.MaterialDomain == MD_Volume;
}



//////////////////////////////////////////////////////////////////////////

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FRenderVolumetricCloudGlobalParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FVolumetricCloudCommonShaderParameters, VolumetricCloud)
	SHADER_PARAMETER_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_TEXTURE(Texture2D<float3>, CloudShadowTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, CloudBilinearTextureSampler)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVolumeShadowingShaderParametersGlobal0, Light0Shadow)
//	SHADER_PARAMETER_STRUCT(FBlueNoise, BlueNoise)
	SHADER_PARAMETER(FUintVector4, SubSetCoordToFullResolutionScaleBias)
	SHADER_PARAMETER(uint32, NoiseFrameIndexModPattern)
	SHADER_PARAMETER(int32, OpaqueIntersectionMode)
	SHADER_PARAMETER(uint32, VolumetricRenderTargetMode)
	SHADER_PARAMETER(uint32, SampleCountDebugMode)
	SHADER_PARAMETER(uint32, IsReflectionRendering)
	SHADER_PARAMETER(uint32, HasValidHZB)
	SHADER_PARAMETER(uint32, ClampRayTToDepthBufferPostHZB)
	SHADER_PARAMETER(uint32, TraceShadowmap)
	SHADER_PARAMETER(FVector, HZBUvFactor)
	SHADER_PARAMETER(FVector4, HZBSize)
	SHADER_PARAMETER_TEXTURE(Texture2D<float>, HZBTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, HZBSampler)
	SHADER_PARAMETER(FVector4, OutputSizeInvSize)
	SHADER_PARAMETER(int32, EnableAerialPerspectiveSampling)
	SHADER_PARAMETER(int32, EnableDistantSkyLightSampling)
	SHADER_PARAMETER(int32, EnableAtmosphericLightsSampling)
	SHADER_PARAMETER(int32, EnableHeightFog)
	SHADER_PARAMETER_STRUCT_INCLUDE(FFogUniformParameters, FogStruct)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FRenderVolumetricCloudGlobalParameters, "RenderVolumetricCloudParameters");

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVolumetricCloudCommonGlobalShaderParameters, "VolumetricCloudCommonParameters");

// When calling this, you still need to setup Light0Shadow yourself.
void SetupDefaultRenderVolumetricCloudGlobalParameters(FRenderVolumetricCloudGlobalParameters& VolumetricCloudParams, FVolumetricCloudRenderSceneInfo& CloudInfo, FViewInfo& ViewInfo)
{
	TRefCountPtr<IPooledRenderTarget> BlackDummy = GSystemTextures.BlackDummy;
	VolumetricCloudParams.VolumetricCloud = CloudInfo.GetVolumetricCloudCommonShaderParameters();
	VolumetricCloudParams.SceneDepthTexture = BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
	VolumetricCloudParams.CloudShadowTexture = BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
	VolumetricCloudParams.CloudBilinearTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	// Light0Shadow
/*#if RHI_RAYTRACING
	InitializeBlueNoise(VolumetricCloudParams.BlueNoise);
#else
	// Blue noise texture is undified for some configuration so replace by other noise for now.
	VolumetricCloudParams.BlueNoise.Dimensions = FIntVector(16, 16, 4); // 16 is the size of the tile, so 4 dimension for the 64x64 HighFrequencyNoiseTexture.
	VolumetricCloudParams.BlueNoise.Texture = GEngine->HighFrequencyNoiseTexture->Resource->TextureRHI;
#endif*/
	VolumetricCloudParams.SubSetCoordToFullResolutionScaleBias = FUintVector4(1, 1, 0, 0);
	VolumetricCloudParams.NoiseFrameIndexModPattern = 0;
	VolumetricCloudParams.VolumetricRenderTargetMode = ViewInfo.ViewState ? ViewInfo.ViewState->VolumetricCloudRenderTarget.GetMode() : 0;
	VolumetricCloudParams.SampleCountDebugMode = FMath::Clamp(CVarVolumetricCloudDebugSampleCountMode.GetValueOnAnyThread(), 0, 5);

	VolumetricCloudParams.HasValidHZB = 0;
	VolumetricCloudParams.ClampRayTToDepthBufferPostHZB = 0;
	VolumetricCloudParams.HZBTexture = BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
	VolumetricCloudParams.HZBSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	VolumetricCloudParams.EnableHeightFog = ViewInfo.Family->Scene->HasAnyExponentialHeightFog() && ShouldRenderFog(*ViewInfo.Family);
	SetupFogUniformParameters(ViewInfo, VolumetricCloudParams.FogStruct);
}

static void SetupRenderVolumetricCloudGlobalParametersHZB(const FViewInfo& ViewInfo, FRenderVolumetricCloudGlobalParameters& ShaderParameters)
{
	ShaderParameters.HasValidHZB = (ViewInfo.HZB.IsValid() && CVarVolumetricCloudHzbCulling.GetValueOnAnyThread() > 0) ? 1 : 0;

	ShaderParameters.HZBTexture = (ShaderParameters.HasValidHZB ? ViewInfo.HZB : GSystemTextures.BlackDummy)->GetRenderTargetItem().ShaderResourceTexture;
	ShaderParameters.HZBSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	const float kHZBTestMaxMipmap = 9.0f;
	const float HZBMipmapCounts = FMath::Log2(FMath::Max(ViewInfo.HZBMipmap0Size.X, ViewInfo.HZBMipmap0Size.Y));
	const FVector HZBUvFactor(
		float(ViewInfo.ViewRect.Width()) / float(2 * ViewInfo.HZBMipmap0Size.X),
		float(ViewInfo.ViewRect.Height()) / float(2 * ViewInfo.HZBMipmap0Size.Y),
		FMath::Max(HZBMipmapCounts - kHZBTestMaxMipmap, 0.0f)
	);
	const FVector4 HZBSize(
		ViewInfo.HZBMipmap0Size.X,
		ViewInfo.HZBMipmap0Size.Y,
		1.0f / float(ViewInfo.HZBMipmap0Size.X),
		1.0f / float(ViewInfo.HZBMipmap0Size.Y)
	);
	ShaderParameters.HZBUvFactor = HZBUvFactor;
	ShaderParameters.HZBSize = HZBSize;
}

//////////////////////////////////////////////////////////////////////////

class FRenderVolumetricCloudVS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FRenderVolumetricCloudVS, MeshMaterial);

public:

	FRenderVolumetricCloudVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FRenderVolumetricCloudGlobalParameters::StaticStructMetadata.GetShaderVariableName());
	}

	FRenderVolumetricCloudVS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const bool bIsCompatible = IsMaterialCompatibleWithVolumetricCloud(Parameters.MaterialParameters, Parameters.Platform);
		return bIsCompatible;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_MAINVS"), TEXT("1"));
	}

private:
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FRenderVolumetricCloudVS, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainVS"), SF_Vertex);

//////////////////////////////////////////////////////////////////////////

enum EVolumetricCloudRenderViewPsPermutations
{
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight0,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight0,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight0,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight0,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight1,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight1,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight1,
	VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1,
	VolumetricCloudRenderViewPsCount
};

BEGIN_SHADER_PARAMETER_STRUCT(FRenderVolumetricCloudRenderViewParametersPS, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CloudShadowTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

template<EVolumetricCloudRenderViewPsPermutations Permutation>
class FRenderVolumetricCloudRenderViewPs : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FRenderVolumetricCloudRenderViewPs, MeshMaterial);

public:

	FRenderVolumetricCloudRenderViewPs(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FRenderVolumetricCloudGlobalParameters::StaticStructMetadata.GetShaderVariableName());
	}

	FRenderVolumetricCloudRenderViewPs() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const bool bIsCompatible = IsMaterialCompatibleWithVolumetricCloud(Parameters.MaterialParameters, Parameters.Platform);
		return bIsCompatible;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_RENDERVIEW_PS"), TEXT("1"));
		OutEnvironment.SetDefine(TEXT("CLOUD_LAYER_PIXEL_SHADER"), TEXT("1"));

		// Force texture fetches to not use automatic mip generation because the pixel shader is using a dynamic loop to evaluate the material multiple times.
		OutEnvironment.SetDefine(TEXT("USE_FORCE_TEXTURE_MIP"), TEXT("1"));
		
		const bool bUseAtmosphereTransmittance =Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight0 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight0 ||
												Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight1 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1;
		OutEnvironment.SetDefine(TEXT("CLOUD_PER_SAMPLE_ATMOSPHERE_TRANSMITTANCE"), bUseAtmosphereTransmittance ? TEXT("1") : TEXT("0"));

		const bool bSampleLightShadowmap =	Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight0 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight0 ||
											Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight1 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1;
		OutEnvironment.SetDefine(TEXT("CLOUD_SAMPLE_ATMOSPHERIC_LIGHT_SHADOWMAP"), bSampleLightShadowmap ? TEXT("1") : TEXT("0"));

		const bool bSampleSecondLight =	Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight1 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight1 ||
										Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight1 || Permutation == VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1;
		OutEnvironment.SetDefine(TEXT("CLOUD_SAMPLE_SECOND_LIGHT"), bSampleSecondLight ? TEXT("1") : TEXT("0"));
	}

private:
};

IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight0>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight0>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight0>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight0>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight1>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight1>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight1>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1>, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);



//////////////////////////////////////////////////////////////////////////

class FSingleTriangleMeshVertexBuffer : public FRenderResource
{
public:
	FStaticMeshVertexBuffers Buffers;

	FSingleTriangleMeshVertexBuffer()
	{
		TArray<FDynamicMeshVertex> Vertices;

		// Vertex position constructed in the shader
		Vertices.Add(FDynamicMeshVertex(FVector(0.0f, 0.0f, 0.0f)));
		Vertices.Add(FDynamicMeshVertex(FVector(0.0f, 0.0f, 0.0f)));
		Vertices.Add(FDynamicMeshVertex(FVector(0.0f, 0.0f, 0.0f)));

		Buffers.PositionVertexBuffer.Init(Vertices.Num());
		Buffers.StaticMeshVertexBuffer.Init(Vertices.Num(), 1);

		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			const FDynamicMeshVertex& Vertex = Vertices[i];

			Buffers.PositionVertexBuffer.VertexPosition(i) = Vertex.Position;
			Buffers.StaticMeshVertexBuffer.SetVertexTangents(i, Vertex.TangentX.ToFVector(), Vertex.GetTangentY(), Vertex.TangentZ.ToFVector());
			Buffers.StaticMeshVertexBuffer.SetVertexUV(i, 0, Vertex.TextureCoordinate[0]);
		}
	}

	virtual void InitRHI() override
	{
		Buffers.PositionVertexBuffer.InitResource();
		Buffers.StaticMeshVertexBuffer.InitResource();
	}

	virtual void ReleaseRHI() override
	{
		Buffers.PositionVertexBuffer.ReleaseResource();
		Buffers.StaticMeshVertexBuffer.ReleaseResource();
	}
};

static TGlobalResource<FSingleTriangleMeshVertexBuffer> GSingleTriangleMeshVertexBuffer;

class FSingleTriangleMeshVertexFactory : public FLocalVertexFactory
{
public:
	FSingleTriangleMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FLocalVertexFactory(InFeatureLevel, "FSingleTriangleMeshVertexFactory")
	{}

	~FSingleTriangleMeshVertexFactory()
	{
		ReleaseResource();
	}

	virtual void InitRHI() override
	{
		FSingleTriangleMeshVertexBuffer* VertexBuffer = &GSingleTriangleMeshVertexBuffer;
		FLocalVertexFactory::FDataType NewData;
		VertexBuffer->Buffers.PositionVertexBuffer.BindPositionVertexBuffer(this, NewData);
		VertexBuffer->Buffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(this, NewData);
		VertexBuffer->Buffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(this, NewData);
		VertexBuffer->Buffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(this, NewData, 0);
		FColorVertexBuffer::BindDefaultColorVertexBuffer(this, NewData, FColorVertexBuffer::NullBindStride::ZeroForDefaultBufferBind);
		// Don't call SetData(), because that ends up calling UpdateRHI(), and if the resource has already been initialized
		// (e.g. when switching the feature level in the editor), that calls InitRHI(), resulting in an infinite loop.
		Data = NewData;
		FLocalVertexFactory::InitRHI();
	}

	bool HasIncompatibleFeatureLevel(ERHIFeatureLevel::Type InFeatureLevel)
	{
		return InFeatureLevel != GetFeatureLevel();
	}
};

static FSingleTriangleMeshVertexFactory* GSingleTriangleMeshVertexFactory = NULL;

static void GetSingleTriangleMeshBatch(FMeshBatch& LocalSingleTriangleMesh, const FMaterialRenderProxy* CloudVolumeMaterialProxy, const ERHIFeatureLevel::Type FeatureLevel)
{
	if (!GSingleTriangleMeshVertexFactory || GSingleTriangleMeshVertexFactory->HasIncompatibleFeatureLevel(FeatureLevel))
	{
		if (GSingleTriangleMeshVertexFactory)
		{
			GSingleTriangleMeshVertexFactory->ReleaseResource();
			delete GSingleTriangleMeshVertexFactory;
		}
		GSingleTriangleMeshVertexFactory = new FSingleTriangleMeshVertexFactory(FeatureLevel);
		GSingleTriangleMeshVertexBuffer.UpdateRHI();
		GSingleTriangleMeshVertexFactory->InitResource();
	}
	LocalSingleTriangleMesh.VertexFactory = GSingleTriangleMeshVertexFactory;
	LocalSingleTriangleMesh.MaterialRenderProxy = CloudVolumeMaterialProxy;
	LocalSingleTriangleMesh.Elements[0].IndexBuffer = nullptr;
	LocalSingleTriangleMesh.Elements[0].FirstIndex = 0;
	LocalSingleTriangleMesh.Elements[0].NumPrimitives = 1;
	LocalSingleTriangleMesh.Elements[0].MinVertexIndex = 0;
	LocalSingleTriangleMesh.Elements[0].MaxVertexIndex = 2;

	LocalSingleTriangleMesh.Elements[0].PrimitiveUniformBuffer = nullptr;
	LocalSingleTriangleMesh.Elements[0].PrimitiveIdMode = PrimID_ForceZero;
}



//////////////////////////////////////////////////////////////////////////



class FVolumetricCloudRenderViewMeshProcessor : public FMeshPassProcessor
{
public:
	FVolumetricCloudRenderViewMeshProcessor(const FScene* Scene, const FViewInfo* InViewIfDynamicMeshCommand, 
		TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer, bool bShouldViewRenderVolumetricRenderTarget, bool bSkipAtmosphericLightShadowmap, bool bSecondAtmosphereLightEnabled,
		FMeshPassDrawListContext* InDrawListContext, TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> VolumetricCloudParmsUB)
		: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
		, bVolumetricCloudPerSampleAtmosphereTransmittance(ShouldUsePerSampleAtmosphereTransmittance(Scene, InViewIfDynamicMeshCommand))
		, bVolumetricCloudSampleLightShadowmap(!bSkipAtmosphericLightShadowmap && CVarVolumetricCloudShadowSampleAtmosphericLightShadowmap.GetValueOnAnyThread() > 0)
		, bVolumetricCloudSecondLight(bSecondAtmosphereLightEnabled)
	{
		PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
		PassDrawRenderState.SetPassUniformBuffer(VolumetricCloudParmsUB);

		PassDrawRenderState.SetViewUniformBuffer(ViewUniformBuffer);

		if (bShouldViewRenderVolumetricRenderTarget)
		{ 
			// No blending as we only render clouds in that render target today. Avoids clearing for now.
			PassDrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
		}
		else
		{
			// When volumetric render target is not enabled globally or for some views, e.g. reflection captures.
			PassDrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_Zero, BF_SourceAlpha>::GetRHI());
		}
	}

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final
	{
		// Determine the mesh's material and blend mode.
		const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
		const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, FallbackMaterialRenderProxyPtr);

		if (Material.GetMaterialDomain() != MD_Volume)
		{
			// Skip in this case. This can happens when the material is compiled and a fallback is provided.
			return;
		}

		const ERasterizerFillMode MeshFillMode = FM_Solid;
		const ERasterizerCullMode MeshCullMode = CM_None;
		const FMaterialRenderProxy& MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? *FallbackMaterialRenderProxyPtr : *MeshBatch.MaterialRenderProxy;

		if (bVolumetricCloudSecondLight)
		{
			if (bVolumetricCloudSampleLightShadowmap)
			{
				if (bVolumetricCloudPerSampleAtmosphereTransmittance)
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight1>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
				else
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight1>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
			}
			else
			{
				if (bVolumetricCloudPerSampleAtmosphereTransmittance)
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight1>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
				else
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight1>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
			}
		}
		else
		{
			if (bVolumetricCloudSampleLightShadowmap)
			{
				if (bVolumetricCloudPerSampleAtmosphereTransmittance)
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow1_SecondLight0>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
				else
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow1_SecondLight0>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
			}
			else
			{
				if (bVolumetricCloudPerSampleAtmosphereTransmittance)
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance1_SampleShadow0_SecondLight0>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
				else
				{
					TemplatedProcess<FRenderVolumetricCloudRenderViewPs<VolumetricCloudRenderViewPs_PerSampleAtmosphereTransmittance0_SampleShadow0_SecondLight0>>(
						MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
				}
			}
		}
	}

private:

	template<class RenderVolumetricCloudRenderViewPsType>
	void TemplatedProcess(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		int32 StaticMeshId,
		ERasterizerFillMode MeshFillMode,
		ERasterizerCullMode MeshCullMode)
	{
		FMeshMaterialShaderElementData EmptyShaderElementData;
		EmptyShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

		const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

		TMeshProcessorShaders< FRenderVolumetricCloudVS, FMeshMaterialShader, FMeshMaterialShader, RenderVolumetricCloudRenderViewPsType> PassShaders;
		PassShaders.PixelShader = MaterialResource.GetShader<RenderVolumetricCloudRenderViewPsType>(VertexFactory->GetType());
		PassShaders.VertexShader = MaterialResource.GetShader<FRenderVolumetricCloudVS>(VertexFactory->GetType());
		const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(PassShaders.VertexShader, PassShaders.PixelShader);
		BuildMeshDrawCommands(
			MeshBatch,
			BatchElementMask,
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			MaterialResource,
			PassDrawRenderState,
			PassShaders,
			MeshFillMode,
			MeshCullMode,
			SortKey,
			EMeshPassFeatures::Default,
			EmptyShaderElementData);
	}

	FMeshPassProcessorRenderState PassDrawRenderState;
	bool bVolumetricCloudPerSampleAtmosphereTransmittance;
	bool bVolumetricCloudSampleLightShadowmap;
	bool bVolumetricCloudSecondLight;
};



//////////////////////////////////////////////////////////////////////////

BEGIN_SHADER_PARAMETER_STRUCT(FVolumetricCloudShadowParametersPS, )
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FVolumetricCloudShadowPS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FVolumetricCloudShadowPS, MeshMaterial);

public:

	FVolumetricCloudShadowPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FRenderVolumetricCloudGlobalParameters::StaticStructMetadata.GetShaderVariableName());
	}

	FVolumetricCloudShadowPS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const bool bIsCompatible = IsMaterialCompatibleWithVolumetricCloud(Parameters.MaterialParameters, Parameters.Platform);
		return bIsCompatible;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_SHADOW_PS"), TEXT("1"));
		OutEnvironment.SetDefine(TEXT("CLOUD_LAYER_PIXEL_SHADER"), TEXT("1"));

		// Force texture fetches to not use automatic mip generation because the pixel shader is using a dynamic loop to evaluate the material multiple times.
		OutEnvironment.SetDefine(TEXT("USE_FORCE_TEXTURE_MIP"), TEXT("1"));
	}

private:
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FVolumetricCloudShadowPS, TEXT("/Engine/Private/VolumetricCloud.usf"), TEXT("MainPS"), SF_Pixel);



class FVolumetricCloudRenderShadowMeshProcessor : public FMeshPassProcessor
{
public:
	FVolumetricCloudRenderShadowMeshProcessor(const FScene* Scene, const FViewInfo* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext, TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> VolumetricCloudParmsUB)
		: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	{
		PassDrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
		PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
		PassDrawRenderState.SetViewUniformBuffer(Scene->UniformBuffers.ViewUniformBuffer);
		PassDrawRenderState.SetPassUniformBuffer(VolumetricCloudParmsUB);
	}

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final
	{
		// Determine the mesh's material and blend mode.
		const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
		const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, FallbackMaterialRenderProxyPtr);

		check(Material.GetMaterialDomain() == MD_Volume);

		const ERasterizerFillMode MeshFillMode = FM_Solid;
		const ERasterizerCullMode MeshCullMode = CM_None;
		const FMaterialRenderProxy& MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? *FallbackMaterialRenderProxyPtr : *MeshBatch.MaterialRenderProxy;
		Process(MeshBatch, BatchElementMask, PrimitiveSceneProxy, MaterialRenderProxy, Material, StaticMeshId, MeshFillMode, MeshCullMode);
	}

private:

	void Process(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		int32 StaticMeshId,
		ERasterizerFillMode MeshFillMode,
		ERasterizerCullMode MeshCullMode)
	{
		FMeshMaterialShaderElementData EmptyShaderElementData;
		EmptyShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

		const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

		TMeshProcessorShaders< FRenderVolumetricCloudVS, FMeshMaterialShader, FMeshMaterialShader,
			FVolumetricCloudShadowPS> PassShaders;
		PassShaders.PixelShader = MaterialResource.GetShader<FVolumetricCloudShadowPS>(VertexFactory->GetType());
		PassShaders.VertexShader = MaterialResource.GetShader<FRenderVolumetricCloudVS>(VertexFactory->GetType());
		const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(PassShaders.VertexShader, PassShaders.PixelShader);
		BuildMeshDrawCommands(
			MeshBatch,
			BatchElementMask,
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			MaterialResource,
			PassDrawRenderState,
			PassShaders,
			MeshFillMode,
			MeshCullMode,
			SortKey,
			EMeshPassFeatures::Default,
			EmptyShaderElementData);
	}

	FMeshPassProcessorRenderState PassDrawRenderState;
};



//////////////////////////////////////////////////////////////////////////

class FDrawDebugCloudShadowCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawDebugCloudShadowCS);
	SHADER_USE_PARAMETER_STRUCT(FDrawDebugCloudShadowCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderDrawDebugParameters, ShaderDrawParameters)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CloudTracedTexture)
		SHADER_PARAMETER(FVector4, CloudTextureSizeInvSize)
		SHADER_PARAMETER(FVector, CloudTraceDirection)
		SHADER_PARAMETER(FMatrix, CloudWorldToLightClipMatrixInv)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsVolumetricCloudMaterialSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_DEBUG_SHADOW_CS"), TEXT("1"));
	}
};

IMPLEMENT_GLOBAL_SHADER(FDrawDebugCloudShadowCS, "/Engine/Private/VolumetricCloud.usf", "MainDrawDebugShadowCS", SF_Compute);



//////////////////////////////////////////////////////////////////////////

class FCloudShadowFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCloudShadowFilterCS);
	SHADER_USE_PARAMETER_STRUCT(FCloudShadowFilterCS, FGlobalShader);

	class FFilterSkyAO : SHADER_PERMUTATION_BOOL("PERMUTATION_SKYAO");
	using FPermutationDomain = TShaderPermutationDomain<FFilterSkyAO>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CloudShadowTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCloudShadowTexture)
		SHADER_PARAMETER(FVector4, CloudTextureSizeInvSize)
		SHADER_PARAMETER(FVector4, CloudTextureTexelWorldSizeInvSize)
		SHADER_PARAMETER(float, CloudLayerStartHeight)
		SHADER_PARAMETER(float, CloudSkyAOApertureScaleAdd)
		SHADER_PARAMETER(float, CloudSkyAOApertureScaleMul)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsVolumetricCloudMaterialSupported(Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_SHADOW_FILTER_CS"), TEXT("1"));
	}
};

IMPLEMENT_GLOBAL_SHADER(FCloudShadowFilterCS, "/Engine/Private/VolumetricCloud.usf", "MainShadowFilterCS", SF_Compute);



//////////////////////////////////////////////////////////////////////////



void FSceneRenderer::InitVolumetricCloudsForViews(FRHICommandListImmediate& RHICmdList)
{
	if (Scene)
	{
		check(ShouldRenderVolumetricCloud(Scene, ViewFamily.EngineShowFlags)); // This should not be called if we should not render SkyAtmosphere

		check(Scene->GetVolumetricCloudSceneInfo());
		const FSkyAtmosphereRenderSceneInfo* SkyInfo = Scene->GetSkyAtmosphereSceneInfo();
		FVolumetricCloudRenderSceneInfo& CloudInfo = *Scene->GetVolumetricCloudSceneInfo();
		const FVolumetricCloudSceneProxy& CloudProxy = CloudInfo.GetVolumetricCloudSceneProxy();
		FLightSceneProxy* AtmosphericLight0 = Scene->AtmosphereLights[0] ? Scene->AtmosphereLights[0]->Proxy : nullptr;
		FLightSceneProxy* AtmosphericLight1 = Scene->AtmosphereLights[1] ? Scene->AtmosphereLights[1]->Proxy : nullptr;
		FSkyLightSceneProxy* SkyLight = Scene->SkyLight;
		const float KilometersToCentimeters = 100000.0f;
		const float CentimetersToKilometers = 1.0f / KilometersToCentimeters;
		const float KilometersToMeters = 1000.0f;
		const float MetersToKilometers = 1.0f / KilometersToMeters;

		// Initialise the cloud common parameters
		{
			FVolumetricCloudCommonShaderParameters& CloudGlobalShaderParams = CloudInfo.GetVolumetricCloudCommonShaderParameters();
			float PlanetRadiusKm = CloudProxy.PlanetRadiusKm;
			if (SkyInfo)
			{
				const FAtmosphereSetup& AtmosphereSetup = SkyInfo->GetSkyAtmosphereSceneProxy().GetAtmosphereSetup();
				PlanetRadiusKm = AtmosphereSetup.BottomRadiusKm;
				CloudGlobalShaderParams.CloudLayerCenterKm = AtmosphereSetup.PlanetCenterKm;
			}
			else
			{
				CloudGlobalShaderParams.CloudLayerCenterKm = FVector(0.0f, 0.0f, -PlanetRadiusKm);
			}
			CloudGlobalShaderParams.PlanetRadiusKm = PlanetRadiusKm;
			CloudGlobalShaderParams.BottomRadiusKm = PlanetRadiusKm + CloudProxy.LayerBottomAltitudeKm;
			CloudGlobalShaderParams.TopRadiusKm = CloudGlobalShaderParams.BottomRadiusKm + CloudProxy.LayerHeightKm;
			CloudGlobalShaderParams.GroundAlbedo = FLinearColor(CloudProxy.GroundAlbedo);
			CloudGlobalShaderParams.SkyLightCloudBottomVisibility = 1.0f - CloudProxy.SkyLightCloudBottomOcclusion;

			CloudGlobalShaderParams.TracingStartMaxDistance = KilometersToCentimeters * CloudProxy.TracingStartMaxDistance;
			CloudGlobalShaderParams.TracingMaxDistance		= KilometersToCentimeters * CloudProxy.TracingMaxDistance;

			const float BaseViewRaySampleCount = 96.0f;
			const float BaseShadowRaySampleCount = 10.0f;
			CloudGlobalShaderParams.SampleCountMax		= FMath::Max(2.0f, FMath::Min(BaseViewRaySampleCount   * CloudProxy.ViewSampleCountScale,       CVarVolumetricCloudViewRaySampleMaxCount.GetValueOnAnyThread()));
			CloudGlobalShaderParams.ShadowSampleCountMax= FMath::Max(2.0f, FMath::Min(BaseShadowRaySampleCount * CloudProxy.ShadowViewSampleCountScale, CVarVolumetricCloudShadowViewRaySampleMaxCount.GetValueOnAnyThread()));
			CloudGlobalShaderParams.ShadowTracingMaxDistance = KilometersToCentimeters * FMath::Max(0.1f, CloudProxy.ShadowTracingDistance);
			CloudGlobalShaderParams.InvDistanceToSampleCountMax = 1.0f / FMath::Max(1.0f, KilometersToCentimeters * CVarVolumetricCloudDistanceToSampleMaxCount.GetValueOnAnyThread());


			auto PrepareCloudShadowMapLightData = [&](FLightSceneProxy* AtmosphericLight, int LightIndex)
			{
				const float CloudShadowmapResolution = float(GetVolumetricCloudShadowMapResolution(AtmosphericLight));
				const float CloudShadowmapResolutionInv = 1.0f / CloudShadowmapResolution;
				CloudGlobalShaderParams.CloudShadowmapSizeInvSize[LightIndex] = FVector4(CloudShadowmapResolution, CloudShadowmapResolution, CloudShadowmapResolutionInv, CloudShadowmapResolutionInv);
				CloudGlobalShaderParams.CloudShadowmapStrength[LightIndex] = GetVolumetricCloudShadowmapStrength(AtmosphericLight);
				CloudGlobalShaderParams.AtmosphericLightCloudScatteredLuminanceScale[LightIndex] = GetVolumetricCloudScatteredLuminanceScale(AtmosphericLight);

				// Setup cloud shadow constants
				if (AtmosphericLight)
				{
					const FVector AtmopshericLight0Direction = AtmosphericLight->GetDirection();
					const FVector UpVector = FMath::Abs(FVector::DotProduct(AtmopshericLight0Direction, FVector::UpVector)) > 0.99f ? FVector::ForwardVector : FVector::UpVector;

					const float SphereRadius = GetVolumetricCloudShadowMapExtentKm(AtmosphericLight) * KilometersToCentimeters;
					const float SphereDiameter = SphereRadius * 2.0f;
					const float NearPlane = 0.0f;
					const float FarPlane = SphereDiameter;
					const float ZScale = 1.0f / (FarPlane - NearPlane);
					const float ZOffset = -NearPlane;

					// TODO Make it work for all views
					FVector LookAtPosition = FVector::ZeroVector;
					FVector PlanetToCameraNormUp = FVector::UpVector;
					if (Views.Num() > 0)
					{
						FViewInfo& View = Views[0];

						// Look at position is positioned on the planet surface under the camera.
						LookAtPosition = (View.ViewMatrices.GetViewOrigin() - (CloudGlobalShaderParams.CloudLayerCenterKm * KilometersToCentimeters));
						LookAtPosition.Normalize();
						PlanetToCameraNormUp = LookAtPosition;
						LookAtPosition = (CloudGlobalShaderParams.CloudLayerCenterKm + LookAtPosition * PlanetRadiusKm) * KilometersToCentimeters;
						// Light position is positioned away from the look at position in the light direction according to the shadowmap radius.
						const FVector LightPosition = LookAtPosition - AtmopshericLight0Direction * SphereRadius;

						float WorldSizeSnap = CVarVolumetricCloudShadowMapSnapLength.GetValueOnAnyThread() * KilometersToCentimeters;
						LookAtPosition.X = (FMath::FloorToFloat((LookAtPosition.X + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap; // offset by 0.5 to not snap around origin
						LookAtPosition.Y = (FMath::FloorToFloat((LookAtPosition.Y + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap;
						LookAtPosition.Z = (FMath::FloorToFloat((LookAtPosition.Z + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap;
					}

					const FVector LightPosition = LookAtPosition - AtmopshericLight0Direction * SphereRadius;
					FReversedZOrthoMatrix ShadowProjectionMatrix(SphereDiameter, SphereDiameter, ZScale, ZOffset);
					FLookAtMatrix ShadowViewMatrix(LightPosition, LookAtPosition, UpVector);
					CloudGlobalShaderParams.CloudShadowmapWorldToLightClipMatrix[LightIndex] = ShadowViewMatrix * ShadowProjectionMatrix;
					CloudGlobalShaderParams.CloudShadowmapWorldToLightClipMatrixInv[LightIndex] = CloudGlobalShaderParams.CloudShadowmapWorldToLightClipMatrix[LightIndex].InverseFast();
					CloudGlobalShaderParams.CloudShadowmapLight0Dir[LightIndex] = AtmopshericLight0Direction;
					CloudGlobalShaderParams.CloudShadowmapFarDepthKm[LightIndex] = FarPlane * CentimetersToKilometers;

					// More samples when the sun is at the horizon: a lot more distance to travel and less pixel covered so trying to keep the same cost and quality.
					CloudGlobalShaderParams.CloudShadowmapSampleClount[LightIndex] = 16.0f + 32.0f * FMath::Clamp(0.2f / FMath::Abs(FVector::DotProduct(PlanetToCameraNormUp, AtmopshericLight0Direction)) - 1.0f, 0.0f, 1.0f);
				}
				else
				{
					CloudGlobalShaderParams.CloudShadowmapWorldToLightClipMatrix[LightIndex] = FMatrix::Identity;
					CloudGlobalShaderParams.CloudShadowmapWorldToLightClipMatrixInv[LightIndex] = FMatrix::Identity;
					CloudGlobalShaderParams.CloudShadowmapFarDepthKm[LightIndex] = 1.0f;
					CloudGlobalShaderParams.CloudShadowmapSampleClount[LightIndex] = 0.0f;
				}
			};
			PrepareCloudShadowMapLightData(AtmosphericLight0, 0);
			PrepareCloudShadowMapLightData(AtmosphericLight1, 1);

			// Setup cloud SkyAO constants
			{
				const float CloudSkyAOResolution = float(GetVolumetricCloudSkyAOResolution(SkyLight));
				const float CloudSkyAOResolutionInv = 1.0f / CloudSkyAOResolution;
				CloudGlobalShaderParams.CloudSkyAOSizeInvSize = FVector4(CloudSkyAOResolution, CloudSkyAOResolution, CloudSkyAOResolutionInv, CloudSkyAOResolutionInv);
				CloudGlobalShaderParams.CloudSkyAOStrength = GetVolumetricCloudSkyAOStrength(SkyLight);

				const float WorldSizeSnap = CVarVolumetricCloudSkyAOSnapLength.GetValueOnAnyThread() * KilometersToCentimeters;
				const float SphereDiameter = GetVolumetricCloudSkyAOExtentKm(SkyLight) * KilometersToCentimeters * 2.0f;
				const float VolumeDepthRange = (CloudProxy.LayerBottomAltitudeKm + CloudProxy.LayerHeightKm) * KilometersToCentimeters + WorldSizeSnap;
				const float NearPlane = 0.0f;
				const float FarPlane = 2.0f * VolumeDepthRange;
				const float ZScale = 1.0f / (FarPlane - NearPlane);
				const float ZOffset = -NearPlane;

				// TODO Make it work for all views
				FVector LookAtPosition = FVector::ZeroVector;
				if (Views.Num() > 0)
				{
					FViewInfo& View = Views[0];

					// Look at position is positioned on the planet surface under the camera.
					LookAtPosition = (View.ViewMatrices.GetViewOrigin() - (CloudGlobalShaderParams.CloudLayerCenterKm * KilometersToCentimeters));
					LookAtPosition.Normalize();
					LookAtPosition = (CloudGlobalShaderParams.CloudLayerCenterKm + LookAtPosition * PlanetRadiusKm) * KilometersToCentimeters;

					// Snap the texture projection
					LookAtPosition.X = (FMath::FloorToFloat((LookAtPosition.X + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap; // offset by 0.5 to not snap around origin
					LookAtPosition.Y = (FMath::FloorToFloat((LookAtPosition.Y + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap;
					LookAtPosition.Z = (FMath::FloorToFloat((LookAtPosition.Z + 0.5f * WorldSizeSnap) / WorldSizeSnap)) * WorldSizeSnap;
				}

				// Trace direction is towards the ground
				FVector TraceDirection =  CloudGlobalShaderParams.CloudLayerCenterKm * KilometersToCentimeters - LookAtPosition;
				TraceDirection.Normalize();

				const FVector UpVector = FVector::ForwardVector; //FMath::Abs(FVector::DotProduct(-TraceDirection, FVector::RightVector)) > 0.99f ? FVector::ForwardVector : FVector::RightVector;
				const FVector LightPosition = LookAtPosition - TraceDirection * VolumeDepthRange;
				FReversedZOrthoMatrix ShadowProjectionMatrix(SphereDiameter, SphereDiameter, ZScale, ZOffset);
				FLookAtMatrix ShadowViewMatrix(LightPosition, LookAtPosition, UpVector);
				CloudGlobalShaderParams.CloudSkyAOWorldToLightClipMatrix = ShadowViewMatrix * ShadowProjectionMatrix;
				CloudGlobalShaderParams.CloudSkyAOWorldToLightClipMatrixInv = CloudGlobalShaderParams.CloudSkyAOWorldToLightClipMatrix.InverseFast();
				CloudGlobalShaderParams.CloudSkyAOTrace0Dir = TraceDirection;
				CloudGlobalShaderParams.CloudSkyAOFarDepthKm = FarPlane * CentimetersToKilometers;

				// More samples when the sun is at the horizon: a lot more distance to travel and less pixel covered so trying to keep the same cost and quality.
				CloudGlobalShaderParams.CloudSkyAOSampleClount = CVarVolumetricCloudSkyAOTraceSampleCount.GetValueOnAnyThread();
			}

			FVolumetricCloudCommonGlobalShaderParameters CloudGlobalShaderParamsUB;
			CloudGlobalShaderParamsUB.VolumetricCloudCommonParams = CloudGlobalShaderParams;
			CloudInfo.GetVolumetricCloudCommonShaderParametersUB() = TUniformBufferRef<FVolumetricCloudCommonGlobalShaderParameters>::CreateUniformBufferImmediate(CloudGlobalShaderParamsUB, UniformBuffer_SingleFrame);
		}



		if (CloudProxy.GetCloudVolumeMaterial())
		{
			FMaterialRenderProxy* CloudVolumeMaterialProxy = CloudProxy.GetCloudVolumeMaterial()->GetRenderProxy();
			if (CloudVolumeMaterialProxy->GetMaterial(ViewFamily.GetFeatureLevel())->GetMaterialDomain() == MD_Volume)
			{
				SCOPED_DRAW_EVENT(RHICmdList, VolumetricCloudShadow);
				SCOPED_GPU_STAT(RHICmdList, VolumetricCloudShadow);

				FRDGBuilder GraphBuilder(RHICmdList);

				FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
				TRefCountPtr<IPooledRenderTarget> BlackDummy = GSystemTextures.BlackDummy;
				FRDGTextureRef BlackDummyRDG = GraphBuilder.RegisterExternalTexture(BlackDummy);

				for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
				{
					FViewInfo& ViewInfo = Views[ViewIndex];
					FVector ViewOrigin = ViewInfo.ViewMatrices.GetViewOrigin();

					FVolumeShadowingShaderParametersGlobal0 LightShadowShaderParams0;
					SetVolumeShadowingDefaultShaderParameters(LightShadowShaderParams0);

					FRenderVolumetricCloudGlobalParameters VolumetricCloudParams;
					VolumetricCloudParams.Light0Shadow = LightShadowShaderParams0;
					SetupDefaultRenderVolumetricCloudGlobalParameters(VolumetricCloudParams, CloudInfo, ViewInfo);

					auto TraceCloudTexture = [&](FRDGTextureRef CloudTextureTracedOutput, bool bSkyAOPass,
						TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> TraceVolumetricCloudParamsUB)
					{
						FVolumetricCloudShadowParametersPS* CloudShadowParameters = GraphBuilder.AllocParameters<FVolumetricCloudShadowParametersPS>();
						CloudShadowParameters->RenderTargets[0] = FRenderTargetBinding(CloudTextureTracedOutput, ERenderTargetLoadAction::ENoAction);

						GraphBuilder.AddPass(
							bSkyAOPass ? RDG_EVENT_NAME("CloudSkyAO") : RDG_EVENT_NAME("CloudShadow"),
							CloudShadowParameters,
							ERDGPassFlags::Raster,
							[CloudShadowParameters, Scene = Scene, &ViewInfo, &CloudVolumeMaterialProxy, TraceVolumetricCloudParamsUB](FRHICommandListImmediate& RHICmdList)
							{
								DrawDynamicMeshPass(ViewInfo, RHICmdList,
									[&ViewInfo, &CloudVolumeMaterialProxy, &RHICmdList, &TraceVolumetricCloudParamsUB](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
									{
										FVolumetricCloudRenderShadowMeshProcessor PassMeshProcessor(
											ViewInfo.Family->Scene->GetRenderScene(), &ViewInfo,
											DynamicMeshPassContext, TraceVolumetricCloudParamsUB);

										FMeshBatch LocalSingleTriangleMesh;
										GetSingleTriangleMeshBatch(LocalSingleTriangleMesh, CloudVolumeMaterialProxy, ViewInfo.GetFeatureLevel());

										const FPrimitiveSceneProxy* PrimitiveSceneProxy = nullptr;
										const uint64 DefaultBatchElementMask = ~0ull;
										PassMeshProcessor.AddMeshBatch(LocalSingleTriangleMesh, DefaultBatchElementMask, PrimitiveSceneProxy);
									});
							});
					};

					const float CloudLayerStartHeight = CloudProxy.LayerBottomAltitudeKm * KilometersToCentimeters;

					auto FilterTracedCloudTexture = [&](FRDGTextureRef* TracedCloudTextureOutput, FVector4 TracedTextureSizeInvSize, FVector4 CloudAOTextureTexelWorldSizeInvSize, bool bSkyAOPass)
					{
						FRDGTextureRef CloudShadowTexture2 = GraphBuilder.CreateTexture(
							FRDGTextureDesc::Create2DDesc(FIntPoint(TracedTextureSizeInvSize.X, TracedTextureSizeInvSize.Y), PF_FloatR11G11B10,
								FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false, 1), bSkyAOPass ? TEXT("CloudSkyAOTexture2") : TEXT("CloudShadowTexture2"));

						FCloudShadowFilterCS::FPermutationDomain Permutation;
						Permutation.Set<FCloudShadowFilterCS::FFilterSkyAO>(bSkyAOPass);
						TShaderMapRef<FCloudShadowFilterCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), Permutation);

						FCloudShadowFilterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCloudShadowFilterCS::FParameters>();
						Parameters->BilinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
						Parameters->CloudShadowTexture = *TracedCloudTextureOutput;
						Parameters->CloudTextureSizeInvSize = TracedTextureSizeInvSize;
						Parameters->CloudTextureTexelWorldSizeInvSize = CloudAOTextureTexelWorldSizeInvSize;
						Parameters->CloudLayerStartHeight = CloudLayerStartHeight;
						Parameters->CloudSkyAOApertureScaleMul = GetVolumetricCloudSkyAOApertureScale(SkyLight);
						Parameters->CloudSkyAOApertureScaleAdd = 1.0f - Parameters->CloudSkyAOApertureScaleMul;
						Parameters->OutCloudShadowTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(CloudShadowTexture2));

						const FIntVector CloudShadowTextureSize = FIntVector(TracedTextureSizeInvSize.X, TracedTextureSizeInvSize.Y, 1);
						const FIntVector DispatchCount = DispatchCount.DivideAndRoundUp(FIntVector(CloudShadowTextureSize.X, CloudShadowTextureSize.Y, 1), FIntVector(8, 8, 1));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CloudDataFilter"), ComputeShader, Parameters, DispatchCount);

						*TracedCloudTextureOutput = CloudShadowTexture2;
					};

					// Render Cloud SKY AO
					if (ShouldRenderCloudSkyAO(SkyLight))
					{
						const uint32 VolumetricCloudSkyAOResolution = GetVolumetricCloudSkyAOResolution(SkyLight);
						FRDGTextureRef CloudSkyAOTexture = GraphBuilder.CreateTexture(
							FRDGTextureDesc::Create2DDesc(FIntPoint(VolumetricCloudSkyAOResolution, VolumetricCloudSkyAOResolution), PF_FloatR11G11B10,
								FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, 1), TEXT("CloudSkyAOTexture"));

						VolumetricCloudParams.TraceShadowmap = 0;
						TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> TraceVolumetricCloudSkyAOParamsUB = TUniformBufferRef<FRenderVolumetricCloudGlobalParameters>::CreateUniformBufferImmediate(VolumetricCloudParams, UniformBuffer_SingleFrame);
						TraceCloudTexture(CloudSkyAOTexture, true, TraceVolumetricCloudSkyAOParamsUB);

						if (CVarVolumetricCloudSkyAOFiltering.GetValueOnAnyThread() > 0)
						{
							const float CloudAOTextureTexelWorldSize = GetVolumetricCloudSkyAOExtentKm(SkyLight) * KilometersToCentimeters * VolumetricCloudParams.VolumetricCloud.CloudSkyAOSizeInvSize.Z;
							const FVector4 CloudAOTextureTexelWorldSizeInvSize = FVector4(CloudAOTextureTexelWorldSize, CloudAOTextureTexelWorldSize, 1.0f / CloudAOTextureTexelWorldSize, 1.0f / CloudAOTextureTexelWorldSize);

							FilterTracedCloudTexture(&CloudSkyAOTexture, VolumetricCloudParams.VolumetricCloud.CloudSkyAOSizeInvSize, CloudAOTextureTexelWorldSizeInvSize, true);
						}

						GraphBuilder.QueueTextureExtraction(CloudSkyAOTexture, &ViewInfo.VolumetricCloudSkyAO);
					}


					// Render atmospheric lights shadow maps
					auto GenerateCloudTexture = [&](FLightSceneProxy* AtmosphericLight, int LightIndex)
					{
						if (ShouldRenderCloudShadowmap(AtmosphericLight))
						{
							const uint32 VolumetricCloudShadowMapResolution = GetVolumetricCloudShadowMapResolution(AtmosphericLight);
							FRDGTextureRef CloudShadowTexture = GraphBuilder.CreateTexture(
								FRDGTextureDesc::Create2DDesc(FIntPoint(VolumetricCloudShadowMapResolution, VolumetricCloudShadowMapResolution), PF_FloatR11G11B10,
									FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, 1), TEXT("CloudShadowTexture"));

							VolumetricCloudParams.TraceShadowmap = 1 + LightIndex;
							TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> TraceVolumetricCloudShadowParamsUB = TUniformBufferRef<FRenderVolumetricCloudGlobalParameters>::CreateUniformBufferImmediate(VolumetricCloudParams, UniformBuffer_SingleFrame);
							TraceCloudTexture(CloudShadowTexture, false, TraceVolumetricCloudShadowParamsUB);

							if (CVarVolumetricCloudShadowFiltering.GetValueOnAnyThread() > 0)
							{
								const float CloudShadowTextureTexelWorldSize = GetVolumetricCloudShadowMapExtentKm(AtmosphericLight) * KilometersToCentimeters * VolumetricCloudParams.VolumetricCloud.CloudShadowmapSizeInvSize[LightIndex].Z;
								const FVector4 CloudShadowTextureTexelWorldSizeInvSize = FVector4(CloudShadowTextureTexelWorldSize, CloudShadowTextureTexelWorldSize, 1.0f / CloudShadowTextureTexelWorldSize, 1.0f / CloudShadowTextureTexelWorldSize);

								FilterTracedCloudTexture(&CloudShadowTexture, VolumetricCloudParams.VolumetricCloud.CloudShadowmapSizeInvSize[LightIndex], CloudShadowTextureTexelWorldSizeInvSize, false);
							}

							GraphBuilder.QueueTextureExtraction(CloudShadowTexture, &ViewInfo.VolumetricCloudShadowMap[LightIndex]);
						}
					};
					GenerateCloudTexture(AtmosphericLight0, 0);
					GenerateCloudTexture(AtmosphericLight1, 1);
				}

				GraphBuilder.Execute();
			}
		}
	}
}

FCloudRenderContext::FCloudRenderContext()
{
	SubSetCoordToFullResolutionScaleBias = FUintVector4(1, 1, 0, 0);
	NoiseFrameIndexModPattern = 0;

	bIsReflectionRendering = false;
	bIsSkyRealTimeReflectionRendering = false;
	bSkipAtmosphericLightShadowmap = false;

	bSkipAerialPerspective = false;
}

void FSceneRenderer::RenderVolumetricCloudsInternal(FRDGBuilder& GraphBuilder, FCloudRenderContext& CloudRC)
{
	FRenderVolumetricCloudRenderViewParametersPS* RenderViewPassParameters = GraphBuilder.AllocParameters<FRenderVolumetricCloudRenderViewParametersPS>();
	RenderViewPassParameters->RenderTargets = CloudRC.RenderTargets;
	RenderViewPassParameters->CloudShadowTexture = CloudRC.VolumetricCloudShadowTexture[0];	// only for experimental path sampling the texture to evaluate shadows

	FRDGTexture* RT0 = CloudRC.RenderTargets.Output[0].GetTexture();
	FVector4 OutputSizeInvSize = FVector4(float(RT0->Desc.Extent.X), float(RT0->Desc.Extent.Y), 1.0f/float(RT0->Desc.Extent.X), 1.0f/float(RT0->Desc.Extent.Y));

	// Copy parameters to lambda
	check(CloudRC.MainView);
	check(CloudRC.CloudInfo);
	check(CloudRC.CloudVolumeMaterialProxy);
	FViewInfo& MainView = *CloudRC.MainView;
	FVolumetricCloudRenderSceneInfo& CloudInfo = *CloudRC.CloudInfo;
	FMaterialRenderProxy* CloudVolumeMaterialProxy = CloudRC.CloudVolumeMaterialProxy;
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = CloudRC.ViewUniformBuffer;
	const bool bShouldViewRenderVolumetricRenderTarget = CloudRC.bShouldViewRenderVolumetricRenderTarget;
	const bool bIsReflectionRendering = CloudRC.bIsReflectionRendering;
	const bool bIsSkyRealTimeReflectionRendering = CloudRC.bIsSkyRealTimeReflectionRendering;
	const bool bSkipAtmosphericLightShadowmap = CloudRC.bSkipAtmosphericLightShadowmap;
	const bool bSecondAtmosphereLightEnabled = CloudRC.bSecondAtmosphereLightEnabled;

	FUintVector4 SubSetCoordToFullResolutionScaleBias = CloudRC.SubSetCoordToFullResolutionScaleBias;
	uint32 NoiseFrameIndexModPattern = CloudRC.NoiseFrameIndexModPattern;
	TRefCountPtr<IPooledRenderTarget> SceneDepthZ = CloudRC.SceneDepthZ;
	FVolumeShadowingShaderParametersGlobal0 LightShadowShaderParams0 = CloudRC.LightShadowShaderParams0;
	bool bSkipAerialPerspective = CloudRC.bSkipAerialPerspective;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CloudView"),
		RenderViewPassParameters,
		ERDGPassFlags::Raster,
		[RenderViewPassParameters, Scene = Scene, &MainView, ViewUniformBuffer, 
		bShouldViewRenderVolumetricRenderTarget, CloudVolumeMaterialProxy, bIsReflectionRendering, bIsSkyRealTimeReflectionRendering, bSkipAtmosphericLightShadowmap, bSecondAtmosphereLightEnabled,
		&CloudInfo, SceneDepthZ, LightShadowShaderParams0, SubSetCoordToFullResolutionScaleBias, NoiseFrameIndexModPattern, OutputSizeInvSize, bSkipAerialPerspective](FRHICommandListImmediate& RHICmdList)
		{
			int32 VolumetricCloudOpaqueIntersectionMode = CVarVolumetricCloudOpaqueIntersectionMode.GetValueOnAnyThread();

			FRenderVolumetricCloudGlobalParameters VolumetricCloudParams;
			SetupDefaultRenderVolumetricCloudGlobalParameters(VolumetricCloudParams, CloudInfo, MainView);
			VolumetricCloudParams.SceneDepthTexture = SceneDepthZ->GetRenderTargetItem().ShaderResourceTexture;
			VolumetricCloudParams.Light0Shadow = LightShadowShaderParams0;
			VolumetricCloudParams.CloudShadowTexture = RenderViewPassParameters->CloudShadowTexture->GetPooledRenderTarget()->GetRenderTargetItem().ShaderResourceTexture;
			VolumetricCloudParams.SubSetCoordToFullResolutionScaleBias = SubSetCoordToFullResolutionScaleBias;
			VolumetricCloudParams.NoiseFrameIndexModPattern = NoiseFrameIndexModPattern;
			VolumetricCloudParams.OpaqueIntersectionMode = bShouldViewRenderVolumetricRenderTarget ? VolumetricCloudOpaqueIntersectionMode : (VolumetricCloudOpaqueIntersectionMode > 0 ? 2 : 0);	// When tracing per pixel and not in the volumetric render target, we can alway intersect with depth
			VolumetricCloudParams.IsReflectionRendering = bIsReflectionRendering ? 1 : 0;

			if (bIsReflectionRendering)
			{
				const float BaseReflectionRaySampleCount = 10.0f;
				const float BaseReflectionShadowRaySampleCount = 3.0f;
				VolumetricCloudParams.VolumetricCloud.SampleCountMax = FMath::Max(2.0f, FMath::Min(BaseReflectionRaySampleCount * CloudInfo.GetVolumetricCloudSceneProxy().ReflectionSampleCountScale, CVarVolumetricCloudReflectionRaySampleMaxCount.GetValueOnAnyThread()));
				VolumetricCloudParams.VolumetricCloud.ShadowSampleCountMax = FMath::Max(2.0f, FMath::Min(BaseReflectionShadowRaySampleCount * CloudInfo.GetVolumetricCloudSceneProxy().ShadowReflectionSampleCountScale, CVarVolumetricCloudShadowReflectionRaySampleMaxCount.GetValueOnAnyThread()));
			}

			VolumetricCloudParams.EnableAerialPerspectiveSampling = bSkipAerialPerspective ? 0 : 1;
			VolumetricCloudParams.EnableDistantSkyLightSampling   = CVarVolumetricCloudEnableDistantSkyLightSampling.GetValueOnAnyThread() > 0 ? 1 : 0;
			VolumetricCloudParams.EnableAtmosphericLightsSampling = CVarVolumetricCloudEnableAtmosphericLightsSampling.GetValueOnAnyThread() > 0 ? 1 : 0;

			VolumetricCloudParams.OutputSizeInvSize = OutputSizeInvSize;
			SetupRenderVolumetricCloudGlobalParametersHZB(MainView, VolumetricCloudParams);

			if (bIsSkyRealTimeReflectionRendering)
			{
				VolumetricCloudParams.FogStruct.ApplyVolumetricFog = 0;		// No valid camera froxel volume available.
				VolumetricCloudParams.OpaqueIntersectionMode = 0;			// No depth buffer is available
				VolumetricCloudParams.HasValidHZB = 0;						// No valid HZB is available
			}

			VolumetricCloudParams.ClampRayTToDepthBufferPostHZB = bShouldViewRenderVolumetricRenderTarget ? 0 : 1;

			TUniformBufferRef<FRenderVolumetricCloudGlobalParameters> VolumetricCloudRenderViewParamsUB = TUniformBufferRef<FRenderVolumetricCloudGlobalParameters>::CreateUniformBufferImmediate(VolumetricCloudParams, UniformBuffer_SingleFrame);

			DrawDynamicMeshPass(MainView, RHICmdList,
				[&MainView, ViewUniformBuffer, bShouldViewRenderVolumetricRenderTarget, bSkipAtmosphericLightShadowmap, bSecondAtmosphereLightEnabled,
				CloudVolumeMaterialProxy, &RHICmdList, &VolumetricCloudRenderViewParamsUB](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
				{
					FVolumetricCloudRenderViewMeshProcessor PassMeshProcessor(
						MainView.Family->Scene->GetRenderScene(), &MainView, ViewUniformBuffer, bShouldViewRenderVolumetricRenderTarget,
						bSkipAtmosphericLightShadowmap, bSecondAtmosphereLightEnabled, DynamicMeshPassContext, VolumetricCloudRenderViewParamsUB);

					FMeshBatch LocalSingleTriangleMesh;
					GetSingleTriangleMeshBatch(LocalSingleTriangleMesh, CloudVolumeMaterialProxy, MainView.GetFeatureLevel());

					const FPrimitiveSceneProxy* PrimitiveSceneProxy = nullptr;
					const uint64 DefaultBatchElementMask = ~0ull;
					PassMeshProcessor.AddMeshBatch(LocalSingleTriangleMesh, DefaultBatchElementMask, PrimitiveSceneProxy);
				});
		});
}

void FSceneRenderer::RenderVolumetricCloud(FRHICommandListImmediate& RHICmdList, bool bSkipVolumetricRenderTarget, bool bSkipPerPixelTracing)
{
	check(ShouldRenderVolumetricCloud(Scene, ViewFamily.EngineShowFlags)); // This should not be called if we should not render SkyAtmosphere

	FVolumetricCloudRenderSceneInfo& CloudInfo = *Scene->GetVolumetricCloudSceneInfo();
	FVolumetricCloudSceneProxy& CloudSceneProxy = CloudInfo.GetVolumetricCloudSceneProxy();

	FLightSceneInfo* AtmosphericLight0Info = Scene->AtmosphereLights[0];
	FLightSceneProxy* AtmosphericLight0 = AtmosphericLight0Info ? AtmosphericLight0Info->Proxy : nullptr;
	FSkyLightSceneProxy* SkyLight = Scene->SkyLight;

	if (CloudSceneProxy.GetCloudVolumeMaterial())
	{
		FMaterialRenderProxy* CloudVolumeMaterialProxy = CloudSceneProxy.GetCloudVolumeMaterial()->GetRenderProxy();
		if (CloudVolumeMaterialProxy->GetMaterial(ViewFamily.GetFeatureLevel())->GetMaterialDomain() == MD_Volume)
		{
			SCOPED_DRAW_EVENT(RHICmdList, VolumetricCloud);
			SCOPED_GPU_STAT(RHICmdList, VolumetricCloud);

			FRDGBuilder GraphBuilder(RHICmdList);

			FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

			TRefCountPtr<IPooledRenderTarget> SceneDepthZ = SceneContext.SceneDepthZ;
			TRefCountPtr<IPooledRenderTarget> BlackDummy = GSystemTextures.BlackDummy;
			FRDGTextureRef BlackDummyRDG = GraphBuilder.RegisterExternalTexture(BlackDummy);

			FCloudRenderContext CloudRC;
			CloudRC.CloudInfo = &CloudInfo;
			CloudRC.CloudVolumeMaterialProxy= CloudVolumeMaterialProxy;
			CloudRC.SceneDepthZ = SceneDepthZ;
			CloudRC.bSkipAtmosphericLightShadowmap = !GetVolumetricCloudReceiveAtmosphericLightShadowmap(AtmosphericLight0);
			CloudRC.bSecondAtmosphereLightEnabled = Scene->IsSecondAtmosphereLightEnabled();

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				FViewInfo& ViewInfo = Views[ViewIndex];

				CloudRC.MainView = &ViewInfo;

				bool bShouldViewRenderVolumetricCloudRenderTarget = ShouldViewRenderVolumetricCloudRenderTarget(ViewInfo); // not used by reflection captures for instance
				if ((bShouldViewRenderVolumetricCloudRenderTarget && bSkipVolumetricRenderTarget) || (!bShouldViewRenderVolumetricCloudRenderTarget && bSkipPerPixelTracing))
				{
					continue;
				}
				CloudRC.bShouldViewRenderVolumetricRenderTarget = bShouldViewRenderVolumetricCloudRenderTarget;
				CloudRC.ViewUniformBuffer = bShouldViewRenderVolumetricCloudRenderTarget ? ViewInfo.VolumetricRenderTargetViewUniformBuffer : ViewInfo.ViewUniformBuffer;

				const bool bEnableAerialPerspectiveSampling = CVarVolumetricCloudEnableAerialPerspectiveSampling.GetValueOnAnyThread() > 0;
				const bool bShouldUseHighQualityAerialPerspective = bEnableAerialPerspectiveSampling && Scene->HasSkyAtmosphere() && CVarVolumetricCloudHighQualityAerialPerspective.GetValueOnAnyThread() > 0 && !CloudRC.bIsReflectionRendering;
				CloudRC.bSkipAerialPerspective = !bEnableAerialPerspectiveSampling || bShouldUseHighQualityAerialPerspective; // Skip AP on clouds if we are going to trace it separately in a second pass
				CloudRC.bIsReflectionRendering = ViewInfo.bIsReflectionCapture;

				FRDGTextureRef IntermediateRT = nullptr;
				FRDGTextureRef DestinationRT = nullptr;
				FRDGTextureRef DestinationRTDepth = nullptr;
				CloudRC.SubSetCoordToFullResolutionScaleBias = FUintVector4(1, 1, 0, 0);
				CloudRC.NoiseFrameIndexModPattern = ViewInfo.CachedViewUniformShaderParameters->StateFrameIndexMod8;
				if (bShouldViewRenderVolumetricCloudRenderTarget)
				{
					FVolumetricRenderTargetViewStateData& VRT = ViewInfo.ViewState->VolumetricCloudRenderTarget;
					DestinationRT = VRT.GetOrCreateVolumetricTracingRT(GraphBuilder);
					DestinationRTDepth = VRT.GetOrCreateVolumetricTracingRTDepth(GraphBuilder);

					if (bShouldUseHighQualityAerialPerspective)
					{
						FIntPoint IntermadiateTargetResolution = FIntPoint(DestinationRT->Desc.GetSize().X, DestinationRT->Desc.GetSize().Y);
						IntermediateRT = GraphBuilder.CreateTexture(
								FRDGTextureDesc::Create2DDesc(IntermadiateTargetResolution, PF_FloatRGBA, FClearValueBinding(FLinearColor(63000.0f, 63000.0f, 63000.0f, 63000.0f)),
									TexCreate_None, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, 1), TEXT("RGBCloudIntermediate"));
					}

					// No action because we only need to render volumetric clouds so we do not blend in that render target.
					// When we have more elements rendered in that target later, we can clear it to default and blend.
					CloudRC.RenderTargets[0] = FRenderTargetBinding(bShouldUseHighQualityAerialPerspective ? IntermediateRT : DestinationRT, ERenderTargetLoadAction::ENoAction);
					CloudRC.RenderTargets[1] = FRenderTargetBinding(DestinationRTDepth, ERenderTargetLoadAction::ENoAction);
					CloudRC.SubSetCoordToFullResolutionScaleBias = VRT.GetTracingToFullResResolutionScaleBias();
					CloudRC.NoiseFrameIndexModPattern = VRT.GetNoiseFrameIndexModPattern();
				}
				else
				{
					DestinationRT = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));
					const FIntVector RtSize = SceneContext.GetSceneColor()->GetDesc().GetSize();

					if (bShouldUseHighQualityAerialPerspective)
					{
						FIntPoint IntermadiateTargetResolution = FIntPoint(RtSize.X, RtSize.Y);
						IntermediateRT = GraphBuilder.CreateTexture(
							FRDGTextureDesc::Create2DDesc(IntermadiateTargetResolution, PF_FloatRGBA, FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)),
								TexCreate_None, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, 1), TEXT("RGBCloudIntermediate"));
					}

					DestinationRTDepth = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DDesc(FIntPoint(RtSize.X, RtSize.Y), PF_R16F, FClearValueBinding::Black,
						TexCreate_None, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, 1), TEXT("DummyDepth"));
					CloudRC.RenderTargets[0] = FRenderTargetBinding(bShouldUseHighQualityAerialPerspective ? IntermediateRT : DestinationRT, bShouldUseHighQualityAerialPerspective ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);
					CloudRC.RenderTargets[1] = FRenderTargetBinding(DestinationRTDepth, bShouldUseHighQualityAerialPerspective ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ENoAction);
				}



				const FProjectedShadowInfo* ProjectedShadowInfo0 = nullptr;
				if (AtmosphericLight0Info)
				{
					ProjectedShadowInfo0 = GetLastCascadeShadowInfo(AtmosphericLight0, VisibleLightInfos[AtmosphericLight0Info->Id]);
				}
				if (!CloudRC.bSkipAtmosphericLightShadowmap && AtmosphericLight0 && ProjectedShadowInfo0)
				{
					SetVolumeShadowingShaderParameters(CloudRC.LightShadowShaderParams0, ViewInfo, AtmosphericLight0Info, ProjectedShadowInfo0, INDEX_NONE);
				}
				else
				{
					SetVolumeShadowingDefaultShaderParameters(CloudRC.LightShadowShaderParams0);
				}
				// Cannot nest a global buffer into another one and we are limited to only one PassUniformBuffer on PassDrawRenderState.
				//TUniformBufferRef<FVolumeShadowingShaderParametersGlobal0> LightShadowShaderParams0UniformBuffer = TUniformBufferRef<FVolumeShadowingShaderParametersGlobal0>::CreateUniformBufferImmediate(LightShadowShaderParams0, UniformBuffer_SingleFrame);

				FCloudShadowAOData CloudShadowAOData;
				GetCloudShadowAOData(&CloudInfo, ViewInfo, GraphBuilder, CloudShadowAOData);
				CloudRC.VolumetricCloudShadowTexture[0] = CloudShadowAOData.VolumetricCloudShadowMap[0];
				CloudRC.VolumetricCloudShadowTexture[1] = CloudShadowAOData.VolumetricCloudShadowMap[1];

				RenderVolumetricCloudsInternal(GraphBuilder, CloudRC);

				// Render high quality sky light shaft on clouds.
				if (bShouldUseHighQualityAerialPerspective)
				{
					FSkyAtmosphereRenderSceneInfo& SkyInfo = *Scene->GetSkyAtmosphereSceneInfo();
					const FSkyAtmosphereSceneProxy& SkyAtmosphereSceneProxy = SkyInfo.GetSkyAtmosphereSceneProxy();
					const FAtmosphereSetup& AtmosphereSetup = SkyAtmosphereSceneProxy.GetAtmosphereSetup();

					FSkyAtmosphereRenderContext SkyRC;
					SkyRC.bFastSky = false;
					SkyRC.bFastAerialPerspective = false;
					SkyRC.bFastAerialPerspectiveDepthTest = false;
					SkyRC.bSecondAtmosphereLightEnabled = Scene->IsSecondAtmosphereLightEnabled();

					SkyAtmosphereLightShadowData LightShadowData;
					SkyRC.bShouldSampleOpaqueShadow = ShouldSkySampleAtmosphereLightsOpaqueShadow(*Scene, VisibleLightInfos, LightShadowData);
					SkyRC.bUseDepthBoundTestIfPossible = false;
					SkyRC.bForceRayMarching = true;				// We do not have any valid view LUT
					SkyRC.bDepthReadDisabled = true;
					SkyRC.bDisableBlending = bShouldViewRenderVolumetricCloudRenderTarget ? true : false;

					SkyRC.TransmittanceLut = GraphBuilder.RegisterExternalTexture(SkyInfo.GetTransmittanceLutTexture());
					SkyRC.MultiScatteredLuminanceLut = GraphBuilder.RegisterExternalTexture(SkyInfo.GetMultiScatteredLuminanceLutTexture());

					// Select the AerialPersepctiveOnCloud mode and set required parameters.
					SkyRC.bAPOnCloudMode = true;
					SkyRC.VolumetricCloudDepthTexture = DestinationRTDepth;
					SkyRC.InputCloudLuminanceTransmittanceTexture = IntermediateRT;
					SkyRC.RenderTargets[0] = FRenderTargetBinding(DestinationRT, ERenderTargetLoadAction::ENoAction);

					SkyRC.ViewMatrices = &ViewInfo.ViewMatrices;
					SkyRC.ViewUniformBuffer = bShouldViewRenderVolumetricCloudRenderTarget ? ViewInfo.VolumetricRenderTargetViewUniformBuffer : ViewInfo.ViewUniformBuffer;

					SkyRC.Viewport = ViewInfo.ViewRect;
					SkyRC.bLightDiskEnabled = !ViewInfo.bIsReflectionCapture;
					SkyRC.AerialPerspectiveStartDepthInCm = GetValidAerialPerspectiveStartDepthInCm(ViewInfo, SkyAtmosphereSceneProxy);
					SkyRC.NearClippingDistance = ViewInfo.NearClippingDistance;
					SkyRC.FeatureLevel = ViewInfo.FeatureLevel;

					SkyRC.bRenderSkyPixel = false;

					if (ViewInfo.SkyAtmosphereViewLutTexture && ViewInfo.SkyAtmosphereCameraAerialPerspectiveVolume)
					{
						SkyRC.SkyAtmosphereViewLutTexture = GraphBuilder.RegisterExternalTexture(ViewInfo.SkyAtmosphereViewLutTexture);
						SkyRC.SkyAtmosphereCameraAerialPerspectiveVolume = GraphBuilder.RegisterExternalTexture(ViewInfo.SkyAtmosphereCameraAerialPerspectiveVolume);
					}
					else
					{
						SkyRC.SkyAtmosphereViewLutTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
						SkyRC.SkyAtmosphereCameraAerialPerspectiveVolume = GSystemTextures.GetVolumetricBlackDummy(GraphBuilder);
					}

					GetSkyAtmosphereLightsUniformBuffers(SkyRC.LightShadowShaderParams0UniformBuffer, SkyRC.LightShadowShaderParams1UniformBuffer,
						LightShadowData, ViewInfo, SkyRC.bShouldSampleOpaqueShadow, UniformBuffer_SingleDraw);

					SkyRC.bShouldSampleCloudShadow = CloudShadowAOData.bShouldSampleCloudShadow;
					SkyRC.VolumetricCloudShadowMap[0] = CloudShadowAOData.VolumetricCloudShadowMap[0];
					SkyRC.VolumetricCloudShadowMap[1] = CloudShadowAOData.VolumetricCloudShadowMap[1];
					SkyRC.bShouldSampleCloudSkyAO = CloudShadowAOData.bShouldSampleCloudSkyAO;
					SkyRC.VolumetricCloudSkyAO = CloudShadowAOData.VolumetricCloudSkyAO;

					RenderSkyAtmosphereInternal(GraphBuilder, SkyRC);
				}

				if (bShouldViewRenderVolumetricCloudRenderTarget)
				{
					ViewInfo.ViewState->VolumetricCloudRenderTarget.ExtractToVolumetricTracingRT(GraphBuilder, DestinationRT);
					ViewInfo.ViewState->VolumetricCloudRenderTarget.ExtractToVolumetricTracingRTDepth(GraphBuilder, DestinationRTDepth);
				}



				const bool DebugCloudShadowMap = CVarVolumetricCloudShadowMapDebug.GetValueOnRenderThread() && ShouldRenderCloudShadowmap(AtmosphericLight0);
				const bool DebugCloudSkyAO = CVarVolumetricCloudSkyAODebug.GetValueOnRenderThread() && ShouldRenderCloudSkyAO(SkyLight);
				if (DebugCloudShadowMap || DebugCloudSkyAO)
				{
					FViewElementPDI ShadowFrustumPDI(&ViewInfo, nullptr, nullptr);

					FRenderVolumetricCloudGlobalParameters VolumetricCloudParams;
					SetupDefaultRenderVolumetricCloudGlobalParameters(VolumetricCloudParams, CloudInfo, ViewInfo);

					auto DebugCloudTexture = [&](FDrawDebugCloudShadowCS::FParameters* Parameters)
					{
						if (ShaderDrawDebug::IsShaderDrawDebugEnabled(ViewInfo))
						{
							FDrawDebugCloudShadowCS::FPermutationDomain Permutation;
							TShaderMapRef<FDrawDebugCloudShadowCS> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5), Permutation);

							ShaderDrawDebug::SetParameters(GraphBuilder, ViewInfo.ShaderDrawData, Parameters->ShaderDrawParameters);

							const FIntVector CloudShadowTextureSize = Parameters->CloudTracedTexture->Desc.GetSize();
							const FIntVector DispatchCount = DispatchCount.DivideAndRoundUp(FIntVector(CloudShadowTextureSize.X, CloudShadowTextureSize.Y, 1), FIntVector(8, 8, 1));
							FComputeShaderUtils::AddPass( GraphBuilder, RDG_EVENT_NAME("DrawDebugCloudShadow"), ComputeShader, Parameters, DispatchCount);
						}
					};

					if (DebugCloudShadowMap)
					{
						const int DebugLightIndex = 0;	// only debug atmospehric light 0 for now
						{
							const float ViewPortWidth = float(ViewInfo.ViewRect.Width());
							const float ViewPortHeight = float(ViewInfo.ViewRect.Height());
							FRenderTargetTemp TempRenderTarget(ViewInfo, (const FTexture2DRHIRef&)SceneContext.GetSceneColor()->GetRenderTargetItem().TargetableTexture);
							FCanvas Canvas(&TempRenderTarget, NULL, ViewInfo.Family->CurrentRealTime, ViewFamily.CurrentWorldTime, ViewFamily.DeltaWorldTime, ViewInfo.GetFeatureLevel());
							FLinearColor TextColor(1.0f, 0.5f, 0.0f);
							FString Text = FString::Printf(TEXT("Shadow Sample Count = %.1f"), VolumetricCloudParams.VolumetricCloud.CloudShadowmapSampleClount[DebugLightIndex]);
							Canvas.DrawShadowedString(0.05f, ViewPortHeight * 0.4f, *Text, GetStatsFont(), TextColor);
							Canvas.Flush_RenderThread(RHICmdList);
						}

						DrawFrustumWireframe(&ShadowFrustumPDI, VolumetricCloudParams.VolumetricCloud.CloudShadowmapWorldToLightClipMatrixInv[DebugLightIndex], FColor::Orange, 0);
						FDrawDebugCloudShadowCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDrawDebugCloudShadowCS::FParameters>();
						Parameters->CloudTracedTexture = CloudRC.VolumetricCloudShadowTexture[DebugLightIndex];
						Parameters->CloudTextureSizeInvSize = VolumetricCloudParams.VolumetricCloud.CloudShadowmapSizeInvSize[DebugLightIndex];
						Parameters->CloudTraceDirection = VolumetricCloudParams.VolumetricCloud.CloudShadowmapLight0Dir[DebugLightIndex];
						Parameters->CloudWorldToLightClipMatrixInv = VolumetricCloudParams.VolumetricCloud.CloudShadowmapWorldToLightClipMatrixInv[DebugLightIndex];
						DebugCloudTexture(Parameters);
					}

					if (DebugCloudSkyAO)
					{
						DrawFrustumWireframe(&ShadowFrustumPDI, VolumetricCloudParams.VolumetricCloud.CloudSkyAOWorldToLightClipMatrixInv, FColor::Blue, 0);
						FDrawDebugCloudShadowCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDrawDebugCloudShadowCS::FParameters>();
						Parameters->CloudTracedTexture = GraphBuilder.RegisterExternalTexture(ViewInfo.VolumetricCloudSkyAO);
						Parameters->CloudTextureSizeInvSize = VolumetricCloudParams.VolumetricCloud.CloudSkyAOSizeInvSize;
						Parameters->CloudTraceDirection = VolumetricCloudParams.VolumetricCloud.CloudSkyAOTrace0Dir;
						Parameters->CloudWorldToLightClipMatrixInv = VolumetricCloudParams.VolumetricCloud.CloudSkyAOWorldToLightClipMatrixInv;
						DebugCloudTexture(Parameters);
					}
				}
			}

			GraphBuilder.Execute();
		}
	}

}


