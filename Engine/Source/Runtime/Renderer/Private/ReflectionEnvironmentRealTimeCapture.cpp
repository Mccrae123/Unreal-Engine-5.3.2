// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Functionality for capturing and pre-filtering a sky env map in real time.
=============================================================================*/

#include "ReflectionEnvironmentCapture.h"
#include "ClearQuad.h"
#include "MeshPassProcessor.h"
#include "PrimitiveSceneProxy.h"
#include "MeshPassProcessor.inl"
#include "ScenePrivate.h"
#include "SkyPassRendering.h"
#include "RenderGraphUtils.h"
#include "VolumetricCloudRendering.h"
#include "VolumetricCloudProxy.h"
#include "FogRendering.h"
#include "GPUScene.h"

#if WITH_EDITOR
#include "CanvasTypes.h"
#include "RenderTargetTemp.h"
#endif

extern float GReflectionCaptureNearPlane;


DECLARE_GPU_STAT(CaptureConvolveSkyEnvMap);


static TAutoConsoleVariable<int32> CVarRealTimeReflectionCaptureTimeSlicing(
	TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"), 1,
	TEXT("When enabled, the real-time sky light capture and convolutions will by distributed over several frames to lower the per-frame cost."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRealTimeReflectionCaptureShadowFromOpaque(
	TEXT("r.SkyLight.RealTimeReflectionCapture.ShadowFromOpaque"), 0,
	TEXT("Opaque meshes cast shadow from directional lights when enabled."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRealTimeReflectionCaptureDepthBuffer(
	TEXT("r.SkyLight.RealTimeReflectionCapture.DepthBuffer"), 1,
	TEXT("When enabled, the real-time sky light capture will have a depth buffer, this is for multiple meshes to be cover each other correctly. The height fog wil lalso be applied according to the depth buffer."),
	ECVF_RenderThreadSafe);



class FDownsampleCubeFaceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDownsampleCubeFaceCS);
	SHADER_USE_PARAMETER_STRUCT(FDownsampleCubeFaceCS, FGlobalShader);

	static const uint32 ThreadGroupSize = 8;

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MipIndex)
		SHADER_PARAMETER(uint32, NumMips)
		SHADER_PARAMETER(int32, CubeFace)
		SHADER_PARAMETER(int32, FaceThreadGroupSize)
		SHADER_PARAMETER(FIntPoint, ValidDispatchCoord)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, SourceCubemapTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SourceCubemapSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutTextureMipColor)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("USE_COMPUTE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FDownsampleCubeFaceCS, "/Engine/Private/ReflectionEnvironmentShaders.usf", "DownsampleCS", SF_Compute);


class FConvolveSpecularFaceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConvolveSpecularFaceCS);
	SHADER_USE_PARAMETER_STRUCT(FConvolveSpecularFaceCS, FGlobalShader);

	static const uint32 ThreadGroupSize = 8;

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MipIndex)
		SHADER_PARAMETER(uint32, NumMips)
		SHADER_PARAMETER(int32, CubeFace)
		SHADER_PARAMETER(int32, FaceThreadGroupSize)
		SHADER_PARAMETER(FIntPoint, ValidDispatchCoord)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, SourceCubemapTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SourceCubemapSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutTextureMipColor)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("USE_COMPUTE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FConvolveSpecularFaceCS, "/Engine/Private/ReflectionEnvironmentShaders.usf", "FilterCS", SF_Compute);


class FComputeSkyEnvMapDiffuseIrradianceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComputeSkyEnvMapDiffuseIrradianceCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeSkyEnvMapDiffuseIrradianceCS, FGlobalShader);

	// 8*8=64 threads in a group.
	// Each thread uses 4*7*RGB sh float => 84 bytes shared group memory. 
	// 64 * 84 = 5376 bytes which fits dx11 16KB shared memory limitation. 6144 with vector alignement in shared memory and it still fits
	// Low occupancy on a single CU.
	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 8;

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, SourceCubemapTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SourceCubemapSampler)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, OutIrradianceEnvMapSH)
		SHADER_PARAMETER(float, UniformSampleSolidAngle)
		SHADER_PARAMETER(uint32, MipIndex)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("SHADER_DIFFUSE_TO_SH"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FComputeSkyEnvMapDiffuseIrradianceCS, "/Engine/Private/ReflectionEnvironmentShaders.usf", "ComputeSkyEnvMapDiffuseIrradianceCS", SF_Compute);



class FApplyLowerHemisphereColor : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FApplyLowerHemisphereColor);
	SHADER_USE_PARAMETER_STRUCT(FApplyLowerHemisphereColor, FGlobalShader);

	static const uint32 ThreadGroupSize = 8;

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FLinearColor, LowerHemisphereSolidColor)
		SHADER_PARAMETER(FIntPoint, ValidDispatchCoord)
		SHADER_PARAMETER(int32, FaceThreadGroupSize)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutTextureMipColor)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("USE_COMPUTE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FApplyLowerHemisphereColor, "/Engine/Private/ReflectionEnvironmentShaders.usf", "ApplyLowerHemisphereColorCS", SF_Compute);


class FRenderRealTimeReflectionHeightFogVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderRealTimeReflectionHeightFogVS);
	SHADER_USE_PARAMETER_STRUCT(FRenderRealTimeReflectionHeightFogVS, FGlobalShader);;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("REALTIME_REFLECTION_HEIGHT_FOG"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRenderRealTimeReflectionHeightFogVS, "/Engine/Private/ReflectionEnvironmentShaders.usf", "RenderRealTimeReflectionHeightFogVS", SF_Vertex);


class FRenderRealTimeReflectionHeightFogPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRenderRealTimeReflectionHeightFogPS);
	SHADER_USE_PARAMETER_STRUCT(FRenderRealTimeReflectionHeightFogPS, FGlobalShader);

	class FDepthTexture : SHADER_PERMUTATION_BOOL("PERMUTATION_DEPTHTEXTURE");
	using FPermutationDomain = TShaderPermutationDomain<FDepthTexture>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FFogUniformParameters, FogStruct)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("REALTIME_REFLECTION_HEIGHT_FOG"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRenderRealTimeReflectionHeightFogPS, "/Engine/Private/ReflectionEnvironmentShaders.usf", "RenderRealTimeReflectionHeightFogPS", SF_Pixel);


void FScene::ValidateSkyLightRealTimeCapture(FRHICommandListImmediate& RHICmdList, FSceneRenderer& SceneRenderer, FViewInfo& MainView)
{
#if WITH_EDITOR
	auto GetMaterialDebugName = [&](const FMaterialRenderProxy* MaterialRenderProxy)
	{
		return MaterialRenderProxy ? MaterialRenderProxy->GetMaterial(MainView.GetFeatureLevel())->GetFriendlyName() : FString(TEXT("Could not find name"));
	};

	bool bSkyMeshInMainPassExist = false;
	bool bSkyMeshInRealTimeSkyCaptureExtist = false;

	const int32 SkyRealTimeReflectionOnlyMeshBatcheCount = MainView.SkyMeshBatches.Num();
	for (int32 MeshBatchIndex = 0; MeshBatchIndex < SkyRealTimeReflectionOnlyMeshBatcheCount; ++MeshBatchIndex)
	{
		FSkyMeshBatch& SkyMeshBatch = MainView.SkyMeshBatches[MeshBatchIndex];
		bSkyMeshInMainPassExist |= SkyMeshBatch.bVisibleInMainPass;
		bSkyMeshInRealTimeSkyCaptureExtist |= SkyMeshBatch.bVisibleInRealTimeSkyCapture;
	}

	if (!bSkyMeshInMainPassExist || !bSkyMeshInRealTimeSkyCaptureExtist)
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
		const float ViewPortWidth = float(MainView.ViewRect.Width());
		const float ViewPortHeight = float(MainView.ViewRect.Height());
		FRenderTargetTemp TempRenderTarget(MainView, (const FTexture2DRHIRef&)SceneContext.GetSceneColor()->GetRenderTargetItem().TargetableTexture);
		FCanvas Canvas(&TempRenderTarget, NULL, MainView.Family->CurrentRealTime, SceneRenderer.ViewFamily.CurrentWorldTime, SceneRenderer.ViewFamily.DeltaWorldTime, MainView.GetFeatureLevel());
		FLinearColor TextColor(1.0f, 0.5f, 0.0f);

		if (MainView.bSceneHasSkyMaterial && !bSkyMeshInMainPassExist)
		{
			Canvas.DrawShadowedString(100.0f, 100.0f, TEXT("At least one mesh with a sky material is in the scene but none are rendered in main view."), GetStatsFont(), TextColor);
		}
		if (MainView.bSceneHasSkyMaterial && !bSkyMeshInRealTimeSkyCaptureExtist && SkyLight && SkyLight->bRealTimeCaptureEnabled)
		{
			Canvas.DrawShadowedString(100.0f, 110.0f, TEXT("At least one mesh with a sky material is in the scene but none are rendered in the real-time sky light reflection."), GetStatsFont(), TextColor);
		}
		Canvas.Flush_RenderThread(RHICmdList);
	}
#endif
}


void FScene::AllocateAndCaptureFrameSkyEnvMap(
	FRHICommandListImmediate& RHICmdList, FSceneRenderer& SceneRenderer, FViewInfo& MainView, 
	bool bShouldRenderSkyAtmosphere, bool bShouldRenderVolumetricCloud)
{
	check(SkyLight && SkyLight->bRealTimeCaptureEnabled && !SkyLight->bHasStaticLighting);

	SCOPED_DRAW_EVENT(RHICmdList, CaptureConvolveSkyEnvMap);
	SCOPED_GPU_STAT(RHICmdList, CaptureConvolveSkyEnvMap);

	const uint32 CubeWidth = SkyLight->CaptureCubeMapResolution;
	const uint32 CubeMipCount = FMath::CeilLogTwo(CubeWidth) + 1;

	// Make a snapshot we are going to use for the 6 cubemap faces and set it up.
	// Note: cube view is not meant to be sent to lambdas because we only create a single one. You should only send the ViewUniformBuffer around.
	FViewInfo& CubeView = *MainView.CreateSnapshot();
	CubeView.FOV = 90.0f;
	// Note: We cannot override exposure because sky input texture are using exposure

	// DYNAMIC PRIMITIVES - We empty the CubeView dynamic primitive list to make sure UploadDynamicPrimitiveShaderDataForViewInternal is going through the cheap fast path only updating unfirm buffer.
	// This means we cannot render procedurally animated meshes into the real-time sky capture as of today.
	CubeView.DynamicPrimitiveShaderData.Empty();

	// Other view data clean up
	CubeView.StereoPass = eSSP_FULL;
	CubeView.DrawDynamicFlags = EDrawDynamicFlags::ForceLowestLOD;
	CubeView.MaterialTextureMipBias = 0;

	FViewMatrices::FMinimalInitializer SceneCubeViewInitOptions;
	SceneCubeViewInitOptions.ConstrainedViewRect = FIntRect(FIntPoint(0, 0), FIntPoint(CubeWidth, CubeWidth));

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
	FBox VolumeBounds[TVC_MAX];
	CubeView.CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();
	CubeView.SetupUniformBufferParameters(
		SceneContext,
		VolumeBounds,
		TVC_MAX,
		*CubeView.CachedViewUniformShaderParameters);

	const FMatrix CubeProjectionMatrix = GetCubeProjectionMatrix(CubeView.FOV * 0.5f, (float)CubeWidth, GReflectionCaptureNearPlane);
	CubeView.UpdateProjectionMatrix(CubeProjectionMatrix);

	FPooledRenderTargetDesc SkyCubeTexDesc = FPooledRenderTargetDesc::CreateCubemapDesc(CubeWidth, 
		PF_FloatR11G11B10, FClearValueBinding::Black, TexCreate_TargetArraySlicesIndependently,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable, false, 1, CubeMipCount, false);

	const bool bTimeSlicedRealTimeCapture = CVarRealTimeReflectionCaptureTimeSlicing.GetValueOnRenderThread() > 0;

	const bool CubeResolutionInvalidated = ConvolvedSkyRenderTarget.IsValid() && ConvolvedSkyRenderTarget->GetDesc().GetSize().X != CubeWidth;
	if (!ConvolvedSkyRenderTarget.IsValid() || CubeResolutionInvalidated)
	{
		GRenderTargetPool.FindFreeElement(RHICmdList, SkyCubeTexDesc, ConvolvedSkyRenderTarget, TEXT("ConvolvedSkyRenderTarget"), true, ERenderTargetTransience::NonTransient);
		GRenderTargetPool.FindFreeElement(RHICmdList, SkyCubeTexDesc, CapturedSkyRenderTarget, TEXT("CapturedSkyRenderTarget"), true, ERenderTargetTransience::NonTransient);
	}
	if (bTimeSlicedRealTimeCapture && (!ProcessedSkyRenderTarget.IsValid() || CubeResolutionInvalidated))
	{
		GRenderTargetPool.FindFreeElement(RHICmdList, SkyCubeTexDesc, ProcessedSkyRenderTarget, TEXT("CapturedSkyRenderTarget"), true, ERenderTargetTransience::NonTransient);
	}


	auto RenderCubeFaces_SkyCloud = [&](bool bExecuteSky, bool bExecuteCloud, TRefCountPtr<IPooledRenderTarget> SkyRenderTarget)
	{
		FScene* Scene = MainView.Family->Scene->GetRenderScene();

		if (bShouldRenderSkyAtmosphere)
		{
			FRDGBuilder GraphBuilder(RHICmdList);// , RDG_EVENT_NAME("CaptureConvolveSkyEnvMap"));

			FSkyAtmosphereRenderSceneInfo& SkyInfo = *GetSkyAtmosphereSceneInfo();
			const FSkyAtmosphereSceneProxy& SkyAtmosphereSceneProxy = SkyInfo.GetSkyAtmosphereSceneProxy();

			FRDGTextureRef SkyCubeTexture = GraphBuilder.RegisterExternalTexture(SkyRenderTarget, TEXT("SkyRenderTarget"));
			FRDGTextureRef BlackDummy2dTex = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
			FRDGTextureRef BlackDummy3dTex = GraphBuilder.RegisterExternalTexture(GSystemTextures.VolumetricBlackDummy);

			FSkyAtmosphereRenderContext SkyRC;

			// Global data constant between faces
			const FAtmosphereSetup& AtmosphereSetup = SkyAtmosphereSceneProxy.GetAtmosphereSetup();
			SkyRC.bFastSky = false;
			SkyRC.bFastAerialPerspective = false;
			SkyRC.bFastAerialPerspectiveDepthTest = false;
			SkyRC.bSecondAtmosphereLightEnabled = IsSecondAtmosphereLightEnabled();

			const bool CaptureShadowFromOpaque = CVarRealTimeReflectionCaptureShadowFromOpaque.GetValueOnRenderThread() > 0;

			// Enable opaque shadow on sky if needed
			SkyRC.bShouldSampleOpaqueShadow = false;
			if (CaptureShadowFromOpaque)
			{
				SkyAtmosphereLightShadowData LightShadowData;
				SkyRC.bShouldSampleOpaqueShadow = ShouldSkySampleAtmosphereLightsOpaqueShadow(*Scene, SceneRenderer.VisibleLightInfos, LightShadowData);
				GetSkyAtmosphereLightsUniformBuffers(SkyRC.LightShadowShaderParams0UniformBuffer, SkyRC.LightShadowShaderParams1UniformBuffer,
					LightShadowData, CubeView, SkyRC.bShouldSampleOpaqueShadow, UniformBuffer_SingleDraw);
			}

			SkyRC.bUseDepthBoundTestIfPossible = false;
			SkyRC.bForceRayMarching = true;				// We do not have any valid view LUT
			SkyRC.bDepthReadDisabled = true;
			SkyRC.bDisableBlending = true;

			SkyRC.TransmittanceLut = GraphBuilder.RegisterExternalTexture(SkyInfo.GetTransmittanceLutTexture());
			SkyRC.MultiScatteredLuminanceLut = GraphBuilder.RegisterExternalTexture(SkyInfo.GetMultiScatteredLuminanceLutTexture());

			FCloudRenderContext CloudRC;
			if (bShouldRenderVolumetricCloud)
			{
				FVolumetricCloudRenderSceneInfo& CloudInfo = *GetVolumetricCloudSceneInfo();
				FVolumetricCloudSceneProxy& CloudSceneProxy = CloudInfo.GetVolumetricCloudSceneProxy();

				if (CloudSceneProxy.GetCloudVolumeMaterial())
				{
					FMaterialRenderProxy* CloudVolumeMaterialProxy = CloudSceneProxy.GetCloudVolumeMaterial()->GetRenderProxy();
					CloudRC.CloudInfo = &CloudInfo;
					CloudRC.CloudVolumeMaterialProxy = CloudVolumeMaterialProxy;
					CloudRC.SceneDepthZ = GSystemTextures.MaxFP16Depth;

					CloudRC.MainView = &CubeView; /// This is only accessing data that is not changing between view oerientation. Such data are accessed from the ViewUniformBuffer. See CubeView comment above.

					CloudRC.bShouldViewRenderVolumetricRenderTarget = false;
					CloudRC.bIsReflectionRendering = true;
					CloudRC.bIsSkyRealTimeReflectionRendering = true;
					CloudRC.bSecondAtmosphereLightEnabled = IsSecondAtmosphereLightEnabled();

					CloudRC.bSkipAtmosphericLightShadowmap = !CaptureShadowFromOpaque;
					if (CaptureShadowFromOpaque)
					{
						FLightSceneInfo* AtmosphericLight0Info = Scene->AtmosphereLights[0];
						FLightSceneProxy* AtmosphericLight0 = AtmosphericLight0Info ? AtmosphericLight0Info->Proxy : nullptr;
						const FProjectedShadowInfo* ProjectedShadowInfo0 = nullptr;
						if (AtmosphericLight0Info)
						{
							ProjectedShadowInfo0 = GetLastCascadeShadowInfo(AtmosphericLight0, SceneRenderer.VisibleLightInfos[AtmosphericLight0Info->Id]);
						}

						// Get the main view shadow info for the cloud shadows in refelction.
						if (!CloudRC.bSkipAtmosphericLightShadowmap && AtmosphericLight0 && ProjectedShadowInfo0)
						{
							SetVolumeShadowingShaderParameters(CloudRC.LightShadowShaderParams0, MainView, AtmosphericLight0Info, ProjectedShadowInfo0, INDEX_NONE);
						}
						else
						{
							SetVolumeShadowingDefaultShaderParameters(CloudRC.LightShadowShaderParams0);
						}
					}
					else
					{
						SetVolumeShadowingDefaultShaderParameters(CloudRC.LightShadowShaderParams0);
					}
				}
				else
				{
					bShouldRenderVolumetricCloud = false; // Disable cloud rendering
				}
			}


			for (int32 CubeFace = 0; CubeFace < CubeFace_MAX; CubeFace++)
			{
				SkyRC.RenderTargets[0] = FRenderTargetBinding(SkyCubeTexture, ERenderTargetLoadAction::ENoAction, 0, CubeFace);

				const FMatrix CubeViewRotationMatrix = CalcCubeFaceViewRotationMatrix((ECubeFace)CubeFace);

				SceneCubeViewInitOptions.ViewRotationMatrix = CubeViewRotationMatrix;
				SceneCubeViewInitOptions.ViewOrigin = SkyLight->CapturePosition;
				SceneCubeViewInitOptions.ProjectionMatrix = CubeProjectionMatrix;
				FViewMatrices CubeViewMatrices = FViewMatrices(SceneCubeViewInitOptions);
				CubeView.SetupCommonViewUniformBufferParameters(
					*CubeView.CachedViewUniformShaderParameters,
					FIntPoint(CubeWidth, CubeWidth),
					1,
					FIntRect(FIntPoint(0, 0), FIntPoint(CubeWidth, CubeWidth)),
					CubeViewMatrices,
					CubeViewMatrices);

				// Notify the fact that we render a reflection, e.g. remove sun disk.
				CubeView.CachedViewUniformShaderParameters->RenderingReflectionCaptureMask = 1.0f;
				// Notify the fact that we render a reflection, e.g. use special exposure.
				CubeView.CachedViewUniformShaderParameters->RealTimeReflectionCapture = 1.0f;

				// We have rendered a sky dome with identity rotation at the SkyLight position for the capture.
				if (MainView.bSceneHasSkyMaterial)
				{
					// Setup a constant referential for each of the faces of the dynamic reflection capture.
					// This is to have the FastSkyViewLUT match the one generated specifically for the capture point of view.
					const FVector SkyViewLutReferentialForward = FVector(1.0f, 0.0f, 0.0f);
					const FVector SkyViewLutReferentialRight = FVector(0.0f, 0.0f, -1.0f);
					AtmosphereSetup.ComputeViewData(SkyLight->CapturePosition, SkyViewLutReferentialForward, SkyViewLutReferentialRight,
						CubeView.CachedViewUniformShaderParameters->SkyWorldCameraOrigin, CubeView.CachedViewUniformShaderParameters->SkyPlanetCenterAndViewHeight, 
						CubeView.CachedViewUniformShaderParameters->SkyViewLutReferential);

					CubeView.CachedViewUniformShaderParameters->SkyViewLutTexture = RealTimeReflectionCaptureSkyAtmosphereViewLutTexture->GetRenderTargetItem().ShaderResourceTexture;
				}
				else
				{
					// Else if there is no sky material, we assume that no material is sampling the FastSkyViewLUT texture in the sky light reflection (bFastSky=bFastAerialPerspective=false).
					// But, we still need to udpate the sky parameters on the view according to the sky light capture position
					const FVector SkyViewLutReferentialForward = FVector(1.0f, 0.0f, 0.0f);
					const FVector SkyViewLutReferentialRight = FVector(0.0f, 0.0f, -1.0f);
					AtmosphereSetup.ComputeViewData(SkyLight->CapturePosition, SkyViewLutReferentialForward, SkyViewLutReferentialRight,
						CubeView.CachedViewUniformShaderParameters->SkyWorldCameraOrigin, CubeView.CachedViewUniformShaderParameters->SkyPlanetCenterAndViewHeight,
						CubeView.CachedViewUniformShaderParameters->SkyViewLutReferential);
				}

				if (MainView.bSceneHasSkyMaterial || HasVolumetricCloud())
				{
					CubeView.CachedViewUniformShaderParameters->CameraAerialPerspectiveVolume = RealTimeReflectionCaptureCamera360APLutTexture->GetRenderTargetItem().ShaderResourceTexture;
				}
				// Else we do nothing as we assume the MainView one will not be used

				TUniformBufferRef<FViewUniformShaderParameters> CubeViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*CubeView.CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
				CubeView.ViewUniformBuffer = CubeViewUniformBuffer;
				if (CubeView.bSceneHasSkyMaterial)
				{
					// DYNAMIC PRIMITIVES - This will hit the fast path not updating the GPU scene, but only setting the GPUSCene resources on the view uniform buffer.
					UploadDynamicPrimitiveShaderDataForView(RHICmdList, *this, CubeView);
				}

				SkyRC.ViewUniformBuffer = CubeViewUniformBuffer;
				SkyRC.ViewMatrices = &CubeViewMatrices;

				SkyRC.SkyAtmosphereViewLutTexture = BlackDummy2dTex;
				SkyRC.SkyAtmosphereCameraAerialPerspectiveVolume = BlackDummy3dTex;

				SkyRC.Viewport = FIntRect(FIntPoint(0, 0), FIntPoint(CubeWidth, CubeWidth));
				SkyRC.bLightDiskEnabled = false;
				SkyRC.bRenderSkyPixel = true;
				SkyRC.AerialPerspectiveStartDepthInCm = 0.01f;
				SkyRC.NearClippingDistance = 0.01f;
				SkyRC.FeatureLevel = FeatureLevel;

				//SkyRC.LightShadowShaderParams0UniformBuffer = nullptr;
				//SkyRC.LightShadowShaderParams1UniformBuffer = nullptr;

				SkyRC.bShouldSampleCloudShadow = HasVolumetricCloud() && (MainView.VolumetricCloudShadowMap[0].IsValid() || MainView.VolumetricCloudShadowMap[1].IsValid());
				SkyRC.VolumetricCloudShadowMap[0] = GraphBuilder.RegisterExternalTexture(SkyRC.bShouldSampleCloudShadow && MainView.VolumetricCloudShadowMap[0].IsValid() ? MainView.VolumetricCloudShadowMap[0] : GSystemTextures.BlackDummy);
				SkyRC.VolumetricCloudShadowMap[1] = GraphBuilder.RegisterExternalTexture(SkyRC.bShouldSampleCloudShadow && MainView.VolumetricCloudShadowMap[1].IsValid() ? MainView.VolumetricCloudShadowMap[1] : GSystemTextures.BlackDummy);

				SkyRC.bShouldSampleCloudSkyAO = HasVolumetricCloud() && MainView.VolumetricCloudSkyAO.IsValid();
				SkyRC.VolumetricCloudSkyAO = GraphBuilder.RegisterExternalTexture(SkyRC.bShouldSampleCloudSkyAO ? MainView.VolumetricCloudSkyAO : GSystemTextures.BlackDummy);

				const bool bUseDepthBuffer = CVarRealTimeReflectionCaptureDepthBuffer.GetValueOnRenderThread() > 0;
				FRDGTextureRef CubeDepthTexture = nullptr;

				if (bExecuteSky)
				{
					if (MainView.bSceneHasSkyMaterial)
					{
						FRenderTargetParameters* RenderTargetPassParameter = GraphBuilder.AllocParameters<FRenderTargetParameters>();
						RenderTargetPassParameter->RenderTargets = SkyRC.RenderTargets;

						// Setup the depth buffer
						if (bUseDepthBuffer)
						{
							FRDGTextureDesc CubeDepthTextureDesc = FRDGTextureDesc::Create2DDesc(FIntPoint(CubeWidth, CubeWidth), PF_DepthStencil, SceneContext.GetDefaultDepthClear(), TexCreate_None, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead, false);
							CubeDepthTexture = GraphBuilder.CreateTexture(CubeDepthTextureDesc, TEXT("CubeDepthTexture"));
							RenderTargetPassParameter->RenderTargets.DepthStencil = FDepthStencilBinding(CubeDepthTexture, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilNop);
						}

						GraphBuilder.AddPass(
							RDG_EVENT_NAME("CaptureSkyMeshReflection"),
							RenderTargetPassParameter,
							ERDGPassFlags::Raster,
							[&MainView, CubeViewUniformBuffer, bUseDepthBuffer](FRHICommandListImmediate& RHICmdList)
							{
								DrawDynamicMeshPass(MainView, RHICmdList,
									[&MainView, &CubeViewUniformBuffer, bUseDepthBuffer](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
									{
										FScene* Scene = MainView.Family->Scene->GetRenderScene();

										FMeshPassProcessorRenderState DrawRenderState(CubeViewUniformBuffer, Scene->UniformBuffers.OpaqueBasePassUniformBuffer);
										DrawRenderState.SetInstancedViewUniformBuffer(Scene->UniformBuffers.InstancedViewUniformBuffer);

										FExclusiveDepthStencil::Type BasePassDepthStencilAccess_Sky = bUseDepthBuffer ? FExclusiveDepthStencil::Type(Scene->DefaultBasePassDepthStencilAccess | FExclusiveDepthStencil::DepthWrite)
											: FExclusiveDepthStencil::Type(Scene->DefaultBasePassDepthStencilAccess & ~FExclusiveDepthStencil::DepthWrite);
										SetupBasePassState(BasePassDepthStencilAccess_Sky, false, DrawRenderState);

										FSkyPassMeshProcessor PassMeshProcessor(Scene, nullptr, DrawRenderState, DynamicMeshPassContext);
										const int32 SkyRealTimeReflectionOnlyMeshBatcheCount = MainView.SkyMeshBatches.Num();
										for (int32 MeshBatchIndex = 0; MeshBatchIndex < SkyRealTimeReflectionOnlyMeshBatcheCount; ++MeshBatchIndex)
										{
											FSkyMeshBatch& SkyMeshBatch = MainView.SkyMeshBatches[MeshBatchIndex];
											if (!SkyMeshBatch.bVisibleInRealTimeSkyCapture)
											{
												continue;
											}

											const FMeshBatch* MeshBatch = SkyMeshBatch.Mesh;
											const FPrimitiveSceneProxy* PrimitiveSceneProxy = SkyMeshBatch.Proxy;
											const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();

											const uint64 DefaultBatchElementMask = ~0ull;
											PassMeshProcessor.AddMeshBatch(*MeshBatch, DefaultBatchElementMask, PrimitiveSceneProxy);
										}
									});
							});
					}
					else
					{
						SceneRenderer.RenderSkyAtmosphereInternal(GraphBuilder, SkyRC);
					}

					// Also render the height fog as part of the sky render pass when time slicing is enabled.
					if (Scene && Scene->ExponentialFogs.Num() > 0)
					{
						FRenderRealTimeReflectionHeightFogVS::FPermutationDomain VsPermutationVector;
						TShaderMapRef<FRenderRealTimeReflectionHeightFogVS> VertexShader(GetGlobalShaderMap(SkyRC.FeatureLevel), VsPermutationVector);

						FRenderRealTimeReflectionHeightFogPS::FPermutationDomain PsPermutationVector;
						PsPermutationVector.Set<FRenderRealTimeReflectionHeightFogPS::FDepthTexture>(CubeDepthTexture != nullptr);
						TShaderMapRef<FRenderRealTimeReflectionHeightFogPS> PixelShader(GetGlobalShaderMap(SkyRC.FeatureLevel), PsPermutationVector);

						FRenderRealTimeReflectionHeightFogPS::FParameters* PsPassParameters = GraphBuilder.AllocParameters<FRenderRealTimeReflectionHeightFogPS::FParameters>();
						PsPassParameters->ViewUniformBuffer = CubeViewUniformBuffer;
						PsPassParameters->RenderTargets = SkyRC.RenderTargets;
						PsPassParameters->DepthTexture = CubeDepthTexture != nullptr ? CubeDepthTexture : BlackDummy2dTex;

						FFogUniformParameters FogUniformParameters;
						SetupFogUniformParameters(CubeView, FogUniformParameters);
						PsPassParameters->FogStruct = TUniformBufferRef<FFogUniformParameters>::CreateUniformBufferImmediate(FogUniformParameters, UniformBuffer_SingleDraw);

						ClearUnusedGraphResources(PixelShader, PsPassParameters);

						// Render height fog at an infinite distance since real time reflections does not have a depth buffer for now.
						// Volumetric fog is not supported in such reflections.
						GraphBuilder.AddPass(
							RDG_EVENT_NAME("DistantHeightFog"),
							PsPassParameters,
							ERDGPassFlags::Raster,
							[PsPassParameters, VertexShader, PixelShader, CubeWidth](FRHICommandList& RHICmdListLambda)
						{
							RHICmdListLambda.SetViewport(0.0f, 0.0f, 0.0f, CubeWidth, CubeWidth, 1.0f);

							FGraphicsPipelineStateInitializer GraphicsPSOInit;
							RHICmdListLambda.ApplyCachedRenderTargets(GraphicsPSOInit);

							GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_Zero, BF_One>::GetRHI();
							GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
							GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
							GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
							GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
							GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
							GraphicsPSOInit.PrimitiveType = PT_TriangleList;
							SetGraphicsPipelineState(RHICmdListLambda, GraphicsPSOInit);

							FRenderRealTimeReflectionHeightFogVS::FParameters VsPassParameters;
							VsPassParameters.ViewUniformBuffer = PsPassParameters->ViewUniformBuffer;
							SetShaderParameters(RHICmdListLambda, VertexShader, VertexShader.GetVertexShader(), VsPassParameters);
							SetShaderParameters(RHICmdListLambda, PixelShader, PixelShader.GetPixelShader(), *PsPassParameters);

							RHICmdListLambda.DrawPrimitive(0, 1, 1);
						});
					}
				}

				if (bShouldRenderVolumetricCloud && bExecuteCloud)
				{
					CloudRC.ViewUniformBuffer = CubeViewUniformBuffer;

					CloudRC.RenderTargets[0] = SkyRC.RenderTargets[0];
					//	CloudRC.RenderTargets[1] = Null target will skip export

					FCloudShadowAOData CloudShadowAOData;
					GetCloudShadowAOData(GetVolumetricCloudSceneInfo(), CubeView, GraphBuilder, CloudShadowAOData);
					CloudRC.VolumetricCloudShadowTexture[0] = CloudShadowAOData.VolumetricCloudShadowMap[0];
					CloudRC.VolumetricCloudShadowTexture[1] = CloudShadowAOData.VolumetricCloudShadowMap[1];

					SceneRenderer.RenderVolumetricCloudsInternal(GraphBuilder, CloudRC);
				}
			}

			// Render lower hemisphere color
			if (SkyLight->bLowerHemisphereIsSolidColor)
			{
				FApplyLowerHemisphereColor::FPermutationDomain PermutationVector;
				TShaderMapRef<FApplyLowerHemisphereColor> ComputeShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);

				const uint32 MipIndex = 0;
				const uint32 Mip0Resolution = SkyCubeTexture->Desc.GetSize().X;
				FApplyLowerHemisphereColor::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyLowerHemisphereColor::FParameters>();
				PassParameters->ValidDispatchCoord = FIntPoint(Mip0Resolution, Mip0Resolution);
				PassParameters->LowerHemisphereSolidColor = SkyLight->LowerHemisphereColor;
				PassParameters->OutTextureMipColor = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SkyCubeTexture, MipIndex));

				FIntVector NumGroups = FIntVector::DivideAndRoundUp(FIntVector(Mip0Resolution, Mip0Resolution, 1), FIntVector(FApplyLowerHemisphereColor::ThreadGroupSize, FApplyLowerHemisphereColor::ThreadGroupSize, 1));

				// The groupd size per face with padding
				PassParameters->FaceThreadGroupSize = NumGroups.X * FConvolveSpecularFaceCS::ThreadGroupSize;

				// We are going to dispatch once for all faces 
				NumGroups.X *= 6;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ApplyLowerHemisphereColor"), ComputeShader, PassParameters, NumGroups);
			}

			GraphBuilder.Execute();
		//	GraphBuilder.QueueTextureExtraction(SkyCubeTexture, &SkyRenderTarget); // Not needed because SkyRenderTarget is not transient
		}
		else
		{
			FRDGBuilder GraphBuilder(RHICmdList);// , RDG_EVENT_NAME("ClearSkyRenderTarget"));
			FRDGTextureRef SkyCubeTexture = GraphBuilder.RegisterExternalTexture(SkyRenderTarget, TEXT("SkyRenderTarget"));

			for (int32 CubeFace = 0; CubeFace < CubeFace_MAX; CubeFace++)
			{
				FRenderTargetParameters* Parameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
				Parameters->RenderTargets[0] = FRenderTargetBinding(SkyCubeTexture, ERenderTargetLoadAction::ENoAction, 0, CubeFace);

				FLinearColor ClearColor = FLinearColor::Black;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("ClearSkyRenderTarget"),
					Parameters,
					ERDGPassFlags::Raster,
					[Parameters, ClearColor](FRHICommandList& RHICmdList)
					{
						DrawClearQuad(RHICmdList, ClearColor);
					});
			}
			GraphBuilder.Execute();
			//	GraphBuilder.QueueTextureExtraction(SkyCubeTexture, &SkyRenderTarget); // Not needed because SkyRenderTarget is not transient
		}
	};



	auto RenderCubeFaces_GenCubeMips = [&](uint32 CubeMipStart, uint32 CubeMipEnd, TRefCountPtr<IPooledRenderTarget> SkyRenderTarget)
	{
		check(CubeMipStart > 0);	// Never write to mip0 as it has just been redered into

		FRDGBuilder GraphBuilder(RHICmdList);// , RDG_EVENT_NAME("GenerateMipChain"));
		FRDGTextureRef SkyCubeTexture = GraphBuilder.RegisterExternalTexture(SkyRenderTarget, TEXT("SkyRenderTarget"));

		FDownsampleCubeFaceCS::FPermutationDomain PermutationVector;
		TShaderMapRef<FDownsampleCubeFaceCS> ComputeShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);

		for (uint32 MipIndex = CubeMipStart; MipIndex <= CubeMipEnd; MipIndex++)
		{
			const uint32 MipResolution = 1 << (CubeMipCount - MipIndex - 1);
			FRDGTextureSRVRef SkyCubeTextureSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(SkyCubeTexture, MipIndex - 1)); // slice/face selection is useless so remove from CreateForMipLevel

			FDownsampleCubeFaceCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDownsampleCubeFaceCS::FParameters>();
			PassParameters->MipIndex = MipIndex;
			PassParameters->NumMips = CubeMipCount;
			PassParameters->CubeFace = 0; // unused
			PassParameters->ValidDispatchCoord = FIntPoint(MipResolution, MipResolution);
			PassParameters->SourceCubemapSampler = TStaticSamplerState<SF_Point>::GetRHI();

			PassParameters->SourceCubemapTexture = SkyCubeTextureSRV;
			PassParameters->OutTextureMipColor = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SkyCubeTexture, MipIndex));

			FIntVector NumGroups = FIntVector::DivideAndRoundUp(FIntVector(MipResolution, MipResolution, 1), FIntVector(FDownsampleCubeFaceCS::ThreadGroupSize, FDownsampleCubeFaceCS::ThreadGroupSize, 1));

			// The groupd size per face with padding
			PassParameters->FaceThreadGroupSize = NumGroups.X * FDownsampleCubeFaceCS::ThreadGroupSize;

			// We are going to dispatch once for all faces 
			NumGroups.X *= 6;

			// Dispatch with GenerateMips: reading from a slice through SRV and writing into lower mip through UAV.
			ClearUnusedGraphResources(ComputeShader, PassParameters);
			GraphBuilder.AddPass(
				Forward<FRDGEventName>(RDG_EVENT_NAME("MipGen")),
				PassParameters,
				ERDGPassFlags::Compute | ERDGPassFlags::GenerateMips,
			[PassParameters, ComputeShader, NumGroups](FRHICommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, NumGroups);
			});
		}

		GraphBuilder.Execute();

		FSceneRenderTargetItem& SkyRenderTargetItem = SkyRenderTarget->GetRenderTargetItem();
		RHICmdList.CopyToResolveTarget(SkyRenderTargetItem.TargetableTexture, SkyRenderTargetItem.ShaderResourceTexture, FResolveParams());
	};



	auto RenderCubeFaces_SpecularConvolution = [&](uint32 CubeMipStart, uint32 CubeMipEnd, TRefCountPtr<IPooledRenderTarget> DstRenderTarget, TRefCountPtr<IPooledRenderTarget> SrcRenderTarget)
	{
		FRDGBuilder GraphBuilder(RHICmdList);// , RDG_EVENT_NAME("ConvolveSpecular"));
		FRDGTextureRef RDGSrcRenderTarget = GraphBuilder.RegisterExternalTexture(SrcRenderTarget, TEXT("CapturedSkyRenderTarget"));
		FRDGTextureRef RDGDstRenderTarget = GraphBuilder.RegisterExternalTexture(DstRenderTarget, TEXT("CapturedSkyRenderTarget"));

		FRDGTextureSRVRef RDGSrcRenderTargetSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RDGSrcRenderTarget));

		FDownsampleCubeFaceCS::FPermutationDomain PermutationVector;
		TShaderMapRef<FConvolveSpecularFaceCS> ComputeShader(GetGlobalShaderMap(FeatureLevel), PermutationVector);
		for (uint32 MipIndex = CubeMipStart; MipIndex <= CubeMipEnd; MipIndex++)
		{
			const uint32 MipResolution = 1 << (CubeMipCount - MipIndex - 1);

			FConvolveSpecularFaceCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FConvolveSpecularFaceCS::FParameters>();
			PassParameters->MipIndex = MipIndex;
			PassParameters->NumMips = CubeMipCount;
			PassParameters->CubeFace = 0; // unused
			PassParameters->ValidDispatchCoord = FIntPoint(MipResolution, MipResolution);
			PassParameters->SourceCubemapSampler = TStaticSamplerState<SF_Point>::GetRHI();

			PassParameters->SourceCubemapTexture = RDGSrcRenderTargetSRV;
			PassParameters->OutTextureMipColor = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RDGDstRenderTarget, MipIndex));

			FIntVector NumGroups = FIntVector::DivideAndRoundUp(FIntVector(MipResolution, MipResolution, 1), FIntVector(FConvolveSpecularFaceCS::ThreadGroupSize, FConvolveSpecularFaceCS::ThreadGroupSize, 1));

			// The groupd size per face with padding
			PassParameters->FaceThreadGroupSize = NumGroups.X * FConvolveSpecularFaceCS::ThreadGroupSize;

			// We are going to dispatch once for all faces 
			NumGroups.X *= 6;

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Convolve"), ComputeShader, PassParameters, NumGroups);
		}

		GraphBuilder.Execute();

		FSceneRenderTargetItem& DstRenderTargetItem = DstRenderTarget->GetRenderTargetItem();
		RHICmdList.CopyToResolveTarget(DstRenderTargetItem.TargetableTexture, DstRenderTargetItem.ShaderResourceTexture, FResolveParams());
	};



	auto RenderCubeFaces_DiffuseIrradiance = [&](TRefCountPtr<IPooledRenderTarget> SourceCubemap)
	{
		// ComputeDiffuseIrradiance using N uniform samples
		{
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, EResourceTransitionPipeline::EGfxToCompute, SkyIrradianceEnvironmentMap.UAV);

			FRDGBuilder GraphBuilder(RHICmdList);// , RDG_EVENT_NAME("ComputeDiffuseIrradiance"));

			FRDGTextureRef SourceCubemapTexture = GraphBuilder.RegisterExternalTexture(SourceCubemap, TEXT("SourceCubemap"));
			FRDGTextureSRVRef SourceCubemapTextureSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SourceCubemapTexture));

			TShaderMapRef<FComputeSkyEnvMapDiffuseIrradianceCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));

			const float SampleCount = FComputeSkyEnvMapDiffuseIrradianceCS::ThreadGroupSizeX * FComputeSkyEnvMapDiffuseIrradianceCS::ThreadGroupSizeY;
			const float UniformSampleSolidAngle = 4.0f * PI / SampleCount; // uniform distribution

			FComputeSkyEnvMapDiffuseIrradianceCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeSkyEnvMapDiffuseIrradianceCS::FParameters>();
			PassParameters->SourceCubemapSampler = TStaticSamplerState<SF_Point>::GetRHI();
			PassParameters->SourceCubemapTexture = SourceCubemapTextureSRV;
			PassParameters->OutIrradianceEnvMapSH = SkyIrradianceEnvironmentMap.UAV;
			PassParameters->UniformSampleSolidAngle = UniformSampleSolidAngle;

			// For 64 uniform samples on the unit sphere, we roughly have 10 samples per face.
			// Considering mip generation and bilinear sampling, we can assume 10 samples is enough to integrate 10*4=40 texels.
			// With that, we target integration of 16*16 face.
			const uint32 Log2_16 = 4; // FMath::Log2(16.0f)
			PassParameters->MipIndex = uint32(FMath::Log2(float(CapturedSkyRenderTarget->GetDesc().GetSize().X))) - Log2_16;

			const FIntVector NumGroups = FIntVector(1, 1, 1);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ComputeSkyEnvMapDiffuseIrradianceCS"), ComputeShader, PassParameters, NumGroups);
			GraphBuilder.Execute();

			// This buffer is now going to be read for rendering.
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToGfx, SkyIrradianceEnvironmentMap.UAV);
		}
	};

	const uint32 LastMipLevel = CubeMipCount - 1;

	if (!bTimeSlicedRealTimeCapture || bRealTimeSlicedReflectionCaptureFirstFrame)
	{
		// Generate a full cube map in a single frame for the first frame.
		// Perf number are for a 128x128x6 a cubemap on PS4 with sky and cloud and default settings

		// 0.60ms (0.12ms for faces with the most clouds)
		RenderCubeFaces_SkyCloud(true, true, CapturedSkyRenderTarget);

		// 0.05ms
		RenderCubeFaces_GenCubeMips(1, LastMipLevel, CapturedSkyRenderTarget);

		// 0.80ms total (0.30ms for mip0, 0.20ms for mip1+2, 0.30ms for remaining mips)
		RenderCubeFaces_SpecularConvolution(0, LastMipLevel, ConvolvedSkyRenderTarget, CapturedSkyRenderTarget);

		// 0.015ms
		RenderCubeFaces_DiffuseIrradiance(ConvolvedSkyRenderTarget);

		// Reset Scene time slicing state if time slicing is disabled
		if (!bTimeSlicedRealTimeCapture)
		{
			bRealTimeSlicedReflectionCaptureFirstFrame = true;
			RealTimeSlicedReflectionCaptureState = 0;
		}
		else
		{
			bRealTimeSlicedReflectionCaptureFirstFrame = false;
		}
	}
	else
	{
		// Each frame we capture the sky and work in ProcessedSkyRenderTarget to generate the specular convolution.
		// Once done, we copy the result into ConvolvedSkyRenderTarget and generate the sky irradiance SH from there.

		if (RealTimeSlicedReflectionCaptureState == 0)
		{
			RenderCubeFaces_SkyCloud(true, false, CapturedSkyRenderTarget);
		}
		else if (RealTimeSlicedReflectionCaptureState == 1)
		{
			RenderCubeFaces_SkyCloud(false, true, CapturedSkyRenderTarget);
		}
		else if (RealTimeSlicedReflectionCaptureState == 2)
		{
			RenderCubeFaces_GenCubeMips(1, LastMipLevel, CapturedSkyRenderTarget);
		}
		else if (RealTimeSlicedReflectionCaptureState == 3)
		{
			RenderCubeFaces_SpecularConvolution(0, 0, ProcessedSkyRenderTarget, CapturedSkyRenderTarget);
		}
		else if (RealTimeSlicedReflectionCaptureState == 4)
		{
			if (LastMipLevel >= 2)
			{
				RenderCubeFaces_SpecularConvolution(1, 2, ProcessedSkyRenderTarget, CapturedSkyRenderTarget);
			}
			else if (LastMipLevel >= 1)
			{
				RenderCubeFaces_SpecularConvolution(1, 1, ProcessedSkyRenderTarget, CapturedSkyRenderTarget);
			}
		}
		else if (RealTimeSlicedReflectionCaptureState == 5)
		{
			if (LastMipLevel >= 3)
			{
				RenderCubeFaces_SpecularConvolution(3, LastMipLevel, ProcessedSkyRenderTarget, CapturedSkyRenderTarget);
			}
		}
		else if (RealTimeSlicedReflectionCaptureState == 6)
		{
			// Copy last result to the texture bound when rendering reflection. This is 0.065ms on PS4 for a 128x128x6 cubemap.
			FRHICopyTextureInfo CopyInfo;
			CopyInfo.NumMips = ProcessedSkyRenderTarget->GetDesc().NumMips;
			CopyInfo.NumSlices = 6;

			FRHITexture* ConvolvedSkyTexture = ConvolvedSkyRenderTarget->GetRenderTargetItem().ShaderResourceTexture;

			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, ConvolvedSkyTexture);
			RHICmdList.CopyTexture(ProcessedSkyRenderTarget->GetRenderTargetItem().ShaderResourceTexture, ConvolvedSkyTexture, CopyInfo);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, ConvolvedSkyTexture);

			// Update the sky irradiance SH buffer.
			RenderCubeFaces_DiffuseIrradiance(ConvolvedSkyRenderTarget);
		}

		RealTimeSlicedReflectionCaptureState++;
		RealTimeSlicedReflectionCaptureState = RealTimeSlicedReflectionCaptureState == 7 ? 0 : RealTimeSlicedReflectionCaptureState;
	}
}


