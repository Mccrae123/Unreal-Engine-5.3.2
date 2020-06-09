// Copyright Epic Games, Inc. All Rights Reserved.

#include "LightmapRenderer.h"
#include "GPULightmassModule.h"
#include "GPULightmassCommon.h"
#include "SceneRendering.h"
#include "Scene/Scene.h"
#include "Scene/StaticMesh.h"
#include "LightmapGBuffer.h"
#include "LightmapRayTracing.h"
#include "ClearQuad.h"
#include "RayTracing/RayTracingMaterialHitShaders.h"
#include "Async/ParallelFor.h"
#include "Async/Async.h"
#include "Rendering/SkyLightImportanceSampling.h"
#include "LightmapPreviewVirtualTexture.h"
#include "RHIGPUReadback.h"
#include "LightmapStorage.h"
#include "LevelEditorViewport.h"
#include "Editor.h"
#include "CanvasTypes.h"

int32 GGPULightmassSamplesPerTexel = 512;
static FAutoConsoleVariableRef CVarGPULightmassSamplesPerTexel(
	TEXT("r.GPULightmass.SamplesPerTexel"),
	GGPULightmassSamplesPerTexel,
	TEXT("\n"),
	ECVF_Default
);

int32 GGPULightmassShadowSamplesPerTexel = 512; // 512 samples to reach good image plane stratification. Shadow samples are 100x faster than path samples
static FAutoConsoleVariableRef CVarGPULightmassShadowSamplesPerTexel(
	TEXT("r.GPULightmass.ShadowSamplesPerTexel"),
	GGPULightmassShadowSamplesPerTexel,
	TEXT("\n"),
	ECVF_Default
);

int32 GGPULightmassShowProgressBars = 1;
static FAutoConsoleVariableRef CVarGPULightmassShowProgressBars(
	TEXT("r.GPULightmass.ShowProgressBars"),
	GGPULightmassShowProgressBars,
	TEXT("\n"),
	ECVF_Default
	);

int32 GGPULightmassUseIrradianceCaching = 0; 
static FAutoConsoleVariableRef CVarGPULightmassUseIrradianceCaching(
	TEXT("r.GPULightmass.IrradianceCaching"),
	GGPULightmassUseIrradianceCaching,
	TEXT("\n"),
	ECVF_Default
);

int32 GGPULightmassVisualizeIrradianceCache = 0;
static FAutoConsoleVariableRef CVarGPULightmassVisualizeIrradianceCache(
	TEXT("r.GPULightmass.IrradianceCaching.Visualize"),
	GGPULightmassVisualizeIrradianceCache,
	TEXT("\n"),
	ECVF_Default
);

int32 GGPULightmassUseFirstBounceRayGuiding = 0;
static FAutoConsoleVariableRef CVarGPULightmassUseFirstBounceRayGuiding(
	TEXT("r.GPULightmass.FirstBounceRayGuiding"),
	GGPULightmassUseFirstBounceRayGuiding,
	TEXT("\n"),
	ECVF_Default
);

int32 GGPULightmassFirstBounceRayGuidingTrialSamples = 128;
static FAutoConsoleVariableRef CVarGPULightmassFirstBounceRayGuidingTrialSamples(
	TEXT("r.GPULightmass.FirstBounceRayGuiding.TrialSamples"),
	GGPULightmassFirstBounceRayGuidingTrialSamples,
	TEXT("\n"),
	ECVF_Default
);

class FCopyConvergedLightmapTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCopyConvergedLightmapTilesCS)
	SHADER_USE_PARAMETER_STRUCT(FCopyConvergedLightmapTilesCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;// ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("GPreviewLightmapPhysicalTileSize"), GPreviewLightmapPhysicalTileSize);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumBatchedTiles)
		SHADER_PARAMETER(uint32, StagingPoolSizeX)
		SHADER_PARAMETER_SRV(StructuredBuffer<FGPUTileDescription>, BatchedTiles)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, IrradianceAndSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SHDirectionality)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SHCorrectionAndStationarySkyLightBentNormal)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ShadowMask)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ShadowMaskSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, StagingHQLayer0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, StagingHQLayer1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, StagingShadowMask)
	END_SHADER_PARAMETER_STRUCT()
};

class FUploadConvergedLightmapTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FUploadConvergedLightmapTilesCS)
	SHADER_USE_PARAMETER_STRUCT(FUploadConvergedLightmapTilesCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;// ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("GPreviewLightmapPhysicalTileSize"), GPreviewLightmapPhysicalTileSize);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumBatchedTiles)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SrcTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DstTexture)
		SHADER_PARAMETER_SRV(StructuredBuffer<int2>, SrcTilePositions)
		SHADER_PARAMETER_SRV(StructuredBuffer<int2>, DstTilePositions)
	END_SHADER_PARAMETER_STRUCT()
};

class FSelectiveLightmapOutputCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSelectiveLightmapOutputCS)
	SHADER_USE_PARAMETER_STRUCT(FSelectiveLightmapOutputCS, FGlobalShader)

	class FOutputLayerDim : SHADER_PERMUTATION_INT("DIM_OUTPUT_LAYER", 3);
	class FDrawProgressBars : SHADER_PERMUTATION_BOOL("DRAW_PROGRESS_BARS");
	using FPermutationDomain = TShaderPermutationDomain<FOutputLayerDim, FDrawProgressBars>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;// ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("GPreviewLightmapPhysicalTileSize"), GPreviewLightmapPhysicalTileSize);
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumBatchedTiles)
		SHADER_PARAMETER(int32, NumTotalSamples)
		SHADER_PARAMETER(int32, NumRayGuidingTrialSamples)
		SHADER_PARAMETER_SRV(StructuredBuffer<FGPUTileDescription>, BatchedTiles)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTileAtlas)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, IrradianceAndSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SHDirectionality)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ShadowMask)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ShadowMaskSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SHCorrectionAndStationarySkyLightBentNormal)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FCopyConvergedLightmapTilesCS, "/Plugin/GPULightmass/Private/LightmapBufferClear.usf", "CopyConvergedLightmapTilesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUploadConvergedLightmapTilesCS, "/Plugin/GPULightmass/Private/LightmapBufferClear.usf", "UploadConvergedLightmapTilesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSelectiveLightmapOutputCS, "/Plugin/GPULightmass/Private/LightmapOutput.usf", "SelectiveLightmapOutputCS", SF_Compute);

struct FGPUTileDescription
{
	FIntPoint LightmapSize;
	FIntPoint VirtualTilePosition;
	FIntPoint WorkingSetPosition;
	FIntPoint ScratchPosition;
	FIntPoint OutputLayer0Position;
	FIntPoint OutputLayer1Position;
	FIntPoint OutputLayer2Position;
	int32 FrameIndex;
	int32 RenderPassIndex;
};

struct FGPUBatchedTileRequests
{
	FStructuredBufferRHIRef BatchedTilesBuffer;
	FShaderResourceViewRHIRef BatchedTilesSRV;
	TResourceArray<FGPUTileDescription> BatchedTilesDesc;
};

#if RHI_RAYTRACING
FPathTracingLightData SetupPathTracingLightParameters(const GPULightmass::FLightSceneRenderState& LightScene)
{
	FPathTracingLightData LightParameters;

	LightParameters.Count = 0;

	// Prepend SkyLight to light buffer
	// WARNING: Until ray payload encodes Light data buffer, the execution depends on this ordering!
	uint32 SkyLightIndex = 0;
	LightParameters.Type[SkyLightIndex] = 0;
	LightParameters.Color[SkyLightIndex] = FVector(1.0);
	LightParameters.Mobility[SkyLightIndex] = (LightScene.SkyLight.IsSet() && LightScene.SkyLight->bStationary) ? 1 : 0;
	uint32 Transmission = 1;
	uint8 LightingChannelMask = 0b111;
	LightParameters.Flags[SkyLightIndex] = Transmission & 0x01;
	LightParameters.Flags[SkyLightIndex] |= (LightingChannelMask & 0x7) << 1;
	LightParameters.Count++;

	uint32 MaxLightCount = RAY_TRACING_LIGHT_COUNT_MAXIMUM;

	for (auto Light : LightScene.DirectionalLights.Elements)
	{
		if (LightParameters.Count < MaxLightCount)
		{
			LightParameters.Type[LightParameters.Count] = 2;
			LightParameters.Normal[LightParameters.Count] = -Light.Direction;
			LightParameters.Color[LightParameters.Count] = FVector(Light.Color);
			LightParameters.Attenuation[LightParameters.Count] = 1.0;
			LightParameters.Mobility[LightParameters.Count] = Light.bStationary ? 1 : 0;

			LightParameters.Flags[LightParameters.Count] = Transmission & 0x01;
			LightParameters.Flags[LightParameters.Count] |= (LightingChannelMask & 0x7) << 1;

			LightParameters.Count++;
		}
	}

	for (auto Light : LightScene.PointLights.Elements)
	{
		if (LightParameters.Count < MaxLightCount)
		{
			LightParameters.Type[LightParameters.Count] = 1;
			LightParameters.Position[LightParameters.Count] = Light.Position;
			LightParameters.Color[LightParameters.Count] = FVector(Light.Color) / (4.0 * PI);
			LightParameters.Dimensions[LightParameters.Count] = FVector(0.0, 0.0, Light.SourceRadius);
			LightParameters.Attenuation[LightParameters.Count] = Light.AttenuationRadius;
			LightParameters.Mobility[LightParameters.Count] = Light.bStationary ? 1 : 0;

			LightParameters.Flags[LightParameters.Count] = Transmission & 0x01;
			LightParameters.Flags[LightParameters.Count] |= (LightingChannelMask & 0x7) << 1;

			LightParameters.Count++;
		}
	}

	for (auto Light : LightScene.SpotLights.Elements)
	{
		if (LightParameters.Count < MaxLightCount)
		{
			LightParameters.Type[LightParameters.Count] = 4;
			LightParameters.Position[LightParameters.Count] = Light.Position;
			LightParameters.Normal[LightParameters.Count] = Light.Direction;
			LightParameters.Color[LightParameters.Count] = 4.0 * PI * FVector(Light.Color);
			LightParameters.Dimensions[LightParameters.Count] = FVector(Light.SpotAngles, Light.SourceRadius);
			LightParameters.Attenuation[LightParameters.Count] = Light.AttenuationRadius;
			LightParameters.Mobility[LightParameters.Count] = Light.bStationary ? 1 : 0;

			LightParameters.Flags[LightParameters.Count] = Transmission & 0x01;
			LightParameters.Flags[LightParameters.Count] |= (LightingChannelMask & 0x7) << 1;

			LightParameters.Count++;
		}
	}

	for (auto Light : LightScene.RectLights.Elements)
	{
		if (LightParameters.Count < MaxLightCount)
		{
			LightParameters.Type[LightParameters.Count] = 3;
			LightParameters.Position[LightParameters.Count] = Light.Position;
			LightParameters.Normal[LightParameters.Count] = Light.Direction;
			LightParameters.dPdu[LightParameters.Count] = FVector::CrossProduct(Light.Tangent, -Light.Direction);
			LightParameters.dPdv[LightParameters.Count] = Light.Tangent;

			FLinearColor LightColor = Light.Color;
			LightColor /= 0.5f * Light.SourceWidth * Light.SourceHeight;
			LightParameters.Color[LightParameters.Count] = FVector(LightColor);

			LightParameters.Dimensions[LightParameters.Count] = FVector(Light.SourceWidth, Light.SourceHeight, 0.0f);
			LightParameters.Attenuation[LightParameters.Count] = Light.AttenuationRadius;
			LightParameters.RectLightBarnCosAngle[LightParameters.Count] = FMath::Cos(FMath::DegreesToRadians(Light.BarnDoorAngle));
			LightParameters.RectLightBarnLength[LightParameters.Count] = Light.BarnDoorLength;

			LightParameters.Mobility[LightParameters.Count] = Light.bStationary ? 1 : 0;

			LightParameters.Flags[LightParameters.Count] = Transmission & 0x01;
			LightParameters.Flags[LightParameters.Count] |= (LightingChannelMask & 0x7) << 1;

			LightParameters.Count++;
		}
	}

	return LightParameters;
}

FSkyLightData SetupSkyLightParameters(const GPULightmass::FLightSceneRenderState& LightScene)
{
	FSkyLightData SkyLightData;
	// Check if parameters should be set based on if the sky light's texture has been processed and if its mip tree has been built yet
	if (LightScene.SkyLight.IsSet())
	{
		check(LightScene.SkyLight->ProcessedTexture);
		check(LightScene.SkyLight->ImportanceSamplingData->bIsValid);

		SkyLightData.SamplesPerPixel = 1;
		SkyLightData.SamplingStopLevel = 0;
		SkyLightData.MaxRayDistance = 1.0e7;
		SkyLightData.MaxNormalBias = 0.1f;
		SkyLightData.MaxShadowThickness = 1.0e3;

		SkyLightData.Color = FVector(LightScene.SkyLight->Color);
		SkyLightData.Texture = LightScene.SkyLight->ProcessedTexture;
		SkyLightData.TextureDimensions = FIntVector(LightScene.SkyLight->TextureDimensions.X, LightScene.SkyLight->TextureDimensions.Y, 1);
		SkyLightData.TextureSampler = LightScene.SkyLight->ProcessedTextureSampler;
		SkyLightData.MipDimensions = LightScene.SkyLight->ImportanceSamplingData->MipDimensions;

		SkyLightData.MipTreePosX = LightScene.SkyLight->ImportanceSamplingData->MipTreePosX.SRV;
		SkyLightData.MipTreeNegX = LightScene.SkyLight->ImportanceSamplingData->MipTreeNegX.SRV;
		SkyLightData.MipTreePosY = LightScene.SkyLight->ImportanceSamplingData->MipTreePosY.SRV;
		SkyLightData.MipTreeNegY = LightScene.SkyLight->ImportanceSamplingData->MipTreeNegY.SRV;
		SkyLightData.MipTreePosZ = LightScene.SkyLight->ImportanceSamplingData->MipTreePosZ.SRV;
		SkyLightData.MipTreeNegZ = LightScene.SkyLight->ImportanceSamplingData->MipTreeNegZ.SRV;

		SkyLightData.MipTreePdfPosX = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfPosX.SRV;
		SkyLightData.MipTreePdfNegX = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfNegX.SRV;
		SkyLightData.MipTreePdfPosY = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfPosY.SRV;
		SkyLightData.MipTreePdfNegY = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfNegY.SRV;
		SkyLightData.MipTreePdfPosZ = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfPosZ.SRV;
		SkyLightData.MipTreePdfNegZ = LightScene.SkyLight->ImportanceSamplingData->MipTreePdfNegZ.SRV;
		SkyLightData.SolidAnglePdf = LightScene.SkyLight->ImportanceSamplingData->SolidAnglePdf.SRV;
	}
	else
	{
		SkyLightData.SamplesPerPixel = -1;
		SkyLightData.SamplingStopLevel = 0;
		SkyLightData.MaxRayDistance = 0.0f;
		SkyLightData.MaxNormalBias = 0.0f;
		SkyLightData.MaxShadowThickness = 0.0f;

		SkyLightData.Color = FVector(0.0);
		SkyLightData.Texture = GBlackTextureCube->TextureRHI;
		SkyLightData.TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		SkyLightData.MipDimensions = FIntVector(0);

		SkyLightData.MipTreePosX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreeNegX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePosY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreeNegY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePosZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreeNegZ = GBlackTextureWithSRV->ShaderResourceViewRHI;

		SkyLightData.MipTreePdfPosX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePdfNegX = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePdfPosY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePdfNegY = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePdfPosZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.MipTreePdfNegZ = GBlackTextureWithSRV->ShaderResourceViewRHI;
		SkyLightData.SolidAnglePdf = GBlackTextureWithSRV->ShaderResourceViewRHI;
	}

	return SkyLightData;
}
#endif
namespace GPULightmass
{

FLightmapRenderer::FLightmapRenderer(FSceneRenderState* InScene)
	: Scene(InScene)
	, LightmapTilePoolGPU(FIntPoint(40, 40))
{
	bUseFirstBounceRayGuiding = GGPULightmassUseFirstBounceRayGuiding == 1;
	if (bUseFirstBounceRayGuiding)
	{
		NumFirstBounceRayGuidingTrialSamples = GGPULightmassFirstBounceRayGuidingTrialSamples;
	}

	if (!bUseFirstBounceRayGuiding)
	{
		LightmapTilePoolGPU.Initialize(
			{
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // IrradianceAndSampleCount
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // SHDirectionality
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // ShadowMask
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // ShadowMaskSampleCount
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // SHCorrectionAndStationarySkyLightBentNormal
			});
	}
	else
	{
		LightmapTilePoolGPU.Initialize(
			{
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // IrradianceAndSampleCount
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // SHDirectionality
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // ShadowMask
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // ShadowMaskSampleCount
				{ PF_A32B32G32R32F, FIntPoint(GPreviewLightmapPhysicalTileSize) }, // SHCorrectionAndStationarySkyLightBentNormal
				{ PF_R32_UINT, FIntPoint(128) }, // RayGuidingLuminance
				{ PF_R32_UINT, FIntPoint(128) }, // RayGuidingSampleCount
				{ PF_R32_FLOAT, FIntPoint(128) }, // RayGuidingCDFX
				{ PF_R32_FLOAT, FIntPoint(32) }, // RayGuidingCDFY
			});
	}
}

void FLightmapRenderer::AddRequest(FLightmapTileRequest TileRequest)
{
	PendingTileRequests.Add(TileRequest);
}

void FCachedRayTracingSceneData::SetupViewUniformBufferFromSceneRenderState(FSceneRenderState& Scene)
{
	TResourceArray<FPrimitiveSceneShaderData> PrimitiveSceneData;
	TResourceArray<FLightmapSceneShaderData> LightmapSceneData;

	PrimitiveSceneData.AddZeroed(Scene.StaticMeshInstanceRenderStates.Elements.Num());

	TArray<int32> LightmapSceneDataStartOffsets;
	LightmapSceneDataStartOffsets.AddZeroed(Scene.StaticMeshInstanceRenderStates.Elements.Num());

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ComputePrefixSum);

		int32 ConservativeLightmapEntriesNum = 0;

		for (int32 InstanceIndex = 0; InstanceIndex < Scene.StaticMeshInstanceRenderStates.Elements.Num(); InstanceIndex++)
		{
			FStaticMeshInstanceRenderState& Instance = Scene.StaticMeshInstanceRenderStates.Elements[InstanceIndex];
			LightmapSceneDataStartOffsets[InstanceIndex] = ConservativeLightmapEntriesNum;
			ConservativeLightmapEntriesNum += Instance.LODLightmapRenderStates.Num();
		}

		LightmapSceneData.AddZeroed(ConservativeLightmapEntriesNum);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SetupGPUScene);

		ParallelFor(Scene.StaticMeshInstanceRenderStates.Elements.Num(),
			[
				this,
				&Scene,
				&PrimitiveSceneData,
				&LightmapSceneData,
				&LightmapSceneDataStartOffsets
			](int32 InstanceIndex = 0)
			{
				FStaticMeshInstanceRenderState& Instance = Scene.StaticMeshInstanceRenderStates.Elements[InstanceIndex];

				FPrimitiveUniformShaderParameters PrimitiveUniformShaderParameters = Instance.PrimitiveUniformShaderParameters;
				PrimitiveUniformShaderParameters.LightmapDataIndex = LightmapSceneDataStartOffsets[InstanceIndex];
				PrimitiveSceneData[InstanceIndex] = FPrimitiveSceneShaderData(PrimitiveUniformShaderParameters);

				for (int32 LODIndex = 0; LODIndex < Instance.LODLightmapRenderStates.Num(); LODIndex++)
				{
					FPrecomputedLightingUniformParameters LightmapParams;
					GetDefaultPrecomputedLightingParameters(LightmapParams);

					if (Instance.LODLightmapRenderStates[LODIndex].IsValid())
					{
						LightmapParams.LightmapVTPackedPageTableUniform[0] = Instance.LODLightmapRenderStates[LODIndex]->LightmapVTPackedPageTableUniform[0];
						for (uint32 LayerIndex = 0u; LayerIndex < 5u; ++LayerIndex)
						{
							LightmapParams.LightmapVTPackedUniform[LayerIndex] = Instance.LODLightmapRenderStates[LODIndex]->LightmapVTPackedUniform[LayerIndex];
						}

						LightmapParams.LightMapCoordinateScaleBias = Instance.LODLightmapRenderStates[LODIndex]->LightmapCoordinateScaleBias;
					}

					LightmapSceneData[LightmapSceneDataStartOffsets[InstanceIndex] + LODIndex] = FLightmapSceneShaderData(LightmapParams);
				}
			});
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SetupViewBuffers);

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PrimitiveSceneData);

			FRHIResourceCreateInfo CreateInfo(&PrimitiveSceneData);
			if (PrimitiveSceneData.GetResourceDataSize() == 0)
			{
				PrimitiveSceneData.Add(FPrimitiveSceneShaderData(GetIdentityPrimitiveParameters()));
			}

			PrimitiveSceneDataBufferRHI = RHICreateStructuredBuffer(sizeof(FVector4), PrimitiveSceneData.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
			PrimitiveSceneDataBufferSRV = RHICreateShaderResourceView(PrimitiveSceneDataBufferRHI);
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(LightmapSceneData);

			FRHIResourceCreateInfo CreateInfo(&LightmapSceneData);
			if (LightmapSceneData.GetResourceDataSize() == 0)
			{
				LightmapSceneData.Add(FLightmapSceneShaderData());
			}

			LightmapSceneDataBufferRHI = RHICreateStructuredBuffer(sizeof(FVector4), LightmapSceneData.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
			LightmapSceneDataBufferSRV = RHICreateShaderResourceView(LightmapSceneDataBufferRHI);
		}

		FViewUniformShaderParameters ViewUniformBufferParameters;
		CachedViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	}
}

void FCachedRayTracingSceneData::SetupFromSceneRenderState(FSceneRenderState& Scene)
{
#if RHI_RAYTRACING
	FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

	{
		RayTracingGeometryInstances.Reserve(Scene.StaticMeshInstanceRenderStates.Elements.Num());

		for (int32 StaticMeshIndex = 0; StaticMeshIndex < Scene.StaticMeshInstanceRenderStates.Elements.Num();  StaticMeshIndex++)
		{
			FStaticMeshInstanceRenderState& Instance = Scene.StaticMeshInstanceRenderStates.Elements[StaticMeshIndex];

			TArray<FMeshBatch> MeshBatches = Instance.GetMeshBatchesForGBufferRendering(0);

			bool bAllSegmentsUnlit = true;
			bool bAllSegmentsOpaque = true;

			for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
			{
				const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
				const FMaterial& Material = MeshBatches[SegmentIndex].MaterialRenderProxy->GetMaterialWithFallback(GMaxRHIFeatureLevel, FallbackMaterialRenderProxyPtr);

				bAllSegmentsUnlit &= Material.GetShadingModels().HasOnlyShadingModel(MSM_Unlit) || !MeshBatches[SegmentIndex].CastShadow;
				bAllSegmentsOpaque &= Material.GetBlendMode() == EBlendMode::BLEND_Opaque;
			}

			if (!bAllSegmentsUnlit)
			{
				int32 InstanceIndex = RayTracingGeometryInstances.AddDefaulted(1);
				FRayTracingGeometryInstance& RayTracingInstance = RayTracingGeometryInstances[InstanceIndex];
				RayTracingInstance.GeometryRHI = Instance.RenderData->LODResources[0].RayTracingGeometry.RayTracingGeometryRHI;
				RayTracingInstance.Transforms.Add(Instance.LocalToWorld);
				RayTracingInstance.NumTransforms = 1;
				RayTracingInstance.UserData.Add((uint32)StaticMeshIndex);
				RayTracingInstance.Mask = 0xFF;
				RayTracingInstance.bForceOpaque = bAllSegmentsOpaque;

				for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
				{
					FFullyCachedRayTracingMeshCommandContext CommandContext(MeshCommandStorage, VisibleRayTracingMeshCommands, SegmentIndex, InstanceIndex);
					FMeshPassProcessorRenderState PassDrawRenderState(CachedViewUniformBuffer, CachedViewUniformBuffer);
					FLightmapRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, PassDrawRenderState);

					RayTracingMeshProcessor.AddMeshBatch(MeshBatches[SegmentIndex], 1, nullptr);
				}
			}
		}

		RayTracingGeometryInstances.Reserve(Scene.InstanceGroupRenderStates.Elements.Num());

		{
			for (int32 InstanceGroupIndex = 0; InstanceGroupIndex < Scene.InstanceGroupRenderStates.Elements.Num(); InstanceGroupIndex++)
			{
				FInstanceGroupRenderState& InstanceGroup = Scene.InstanceGroupRenderStates.Elements[InstanceGroupIndex];

				TArray<FMeshBatch> MeshBatches = InstanceGroup.GetMeshBatchesForGBufferRendering(0, FTileVirtualCoordinates{});

				bool bAllSegmentsUnlit = true;
				bool bAllSegmentsOpaque = true;

				for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
				{
					const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
					const FMaterial& Material = MeshBatches[SegmentIndex].MaterialRenderProxy->GetMaterialWithFallback(GMaxRHIFeatureLevel, FallbackMaterialRenderProxyPtr);

					bAllSegmentsUnlit &= Material.GetShadingModels().HasOnlyShadingModel(MSM_Unlit) || !MeshBatches[SegmentIndex].CastShadow;
					bAllSegmentsOpaque &= Material.GetBlendMode() == EBlendMode::BLEND_Opaque;
				}

				if (!bAllSegmentsUnlit)
				{
					int32 InstanceIndex = RayTracingGeometryInstances.AddDefaulted(1);
					FRayTracingGeometryInstance& RayTracingInstance = RayTracingGeometryInstances[InstanceIndex];
					RayTracingInstance.GeometryRHI = InstanceGroup.ComponentUObject->GetStaticMesh()->RenderData->LODResources[0].RayTracingGeometry.RayTracingGeometryRHI;

					RayTracingInstance.Transforms.AddZeroed(InstanceGroup.InstancedRenderData->PerInstanceRenderData->InstanceBuffer.GetNumInstances());

					for (int32 InstanceIdx = 0; InstanceIdx < (int32)InstanceGroup.InstancedRenderData->PerInstanceRenderData->InstanceBuffer.GetNumInstances(); InstanceIdx++)
					{
						FMatrix Transform;
						InstanceGroup.InstancedRenderData->PerInstanceRenderData->InstanceBuffer.GetInstanceTransform(InstanceIdx, Transform);
						Transform.M[3][3] = 1.0f;
						FMatrix InstanceTransform = Transform * InstanceGroup.LocalToWorld;

						RayTracingInstance.Transforms[InstanceIdx] = InstanceTransform;
					}

					RayTracingInstance.NumTransforms = RayTracingInstance.Transforms.Num();

					RayTracingInstance.UserData.Add((uint32)(Scene.StaticMeshInstanceRenderStates.Elements.Num() + InstanceGroupIndex));
					RayTracingInstance.Mask = 0xFF;
					RayTracingInstance.bForceOpaque = bAllSegmentsOpaque;

					for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
					{
						FFullyCachedRayTracingMeshCommandContext CommandContext(MeshCommandStorage, VisibleRayTracingMeshCommands, SegmentIndex, InstanceIndex);
						FMeshPassProcessorRenderState PassDrawRenderState(CachedViewUniformBuffer, CachedViewUniformBuffer);
						FLightmapRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, PassDrawRenderState);

						RayTracingMeshProcessor.AddMeshBatch(MeshBatches[SegmentIndex], 1, nullptr);
					}
				}
			}
		}
	}
#else // RHI_RAYTRACING
	checkNoEntry();
#endif // RHI_RAYTRACING
}

void FSceneRenderState::SetupRayTracingScene()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SetupRayTracingScene);

	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

	if (!CachedRayTracingScene.IsValid())
	{
		CachedRayTracingScene = MakeUnique<FCachedRayTracingSceneData>();
		CachedRayTracingScene->SetupViewUniformBufferFromSceneRenderState(*this);
		CachedRayTracingScene->SetupFromSceneRenderState(*this);
		
		CalculateDistributionPrefixSumForAllLightmaps();
	}

#if 0 // Debug: verify cached ray tracing scene has up-to-date shader bindings
	TUniquePtr<FCachedRayTracingSceneData> VerificationRayTracingScene = MakeUnique<FCachedRayTracingSceneData>();
	VerificationRayTracingScene->CachedViewUniformBuffer = CachedRayTracingScene->CachedViewUniformBuffer;
	VerificationRayTracingScene->SetupFromSceneRenderState(*this);

	check(CachedRayTracingScene->VisibleRayTracingMeshCommands.Num() == VerificationRayTracingScene->VisibleRayTracingMeshCommands.Num());
	check(CachedRayTracingScene->MeshCommandStorage.Num() == VerificationRayTracingScene->MeshCommandStorage.Num());

	for (int32 CommandIndex = 0; CommandIndex < CachedRayTracingScene->VisibleRayTracingMeshCommands.Num(); CommandIndex++)
	{
		const FVisibleRayTracingMeshCommand& VisibleMeshCommand = CachedRayTracingScene->VisibleRayTracingMeshCommands[CommandIndex];
		const FRayTracingMeshCommand& MeshCommand = *VisibleMeshCommand.RayTracingMeshCommand;
		const FRayTracingMeshCommand& VerificationMeshCommand = *VerificationRayTracingScene->VisibleRayTracingMeshCommands[CommandIndex].RayTracingMeshCommand;
		check(MeshCommand.ShaderBindings.GetDynamicInstancingHash() == VerificationMeshCommand.ShaderBindings.GetDynamicInstancingHash());
		MeshCommand.ShaderBindings.MatchesForDynamicInstancing(VerificationMeshCommand.ShaderBindings);
	}
#endif

	FSceneViewFamily ViewFamily(FSceneViewFamily::ConstructionValues(
		nullptr,
		nullptr,
		FEngineShowFlags(ESFIM_Game))
		.SetWorldTimes(0, 0, 0)
		.SetGammaCorrection(1.0f));

	const FIntRect ViewRect(FIntPoint(0, 0), FIntPoint(GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize));

	// make a temporary view
	FSceneViewInitOptions ViewInitOptions;
	ViewInitOptions.ViewFamily = &ViewFamily;
	ViewInitOptions.SetViewRectangle(ViewRect);
	ViewInitOptions.ViewOrigin = FVector::ZeroVector;
	ViewInitOptions.ViewRotationMatrix = FMatrix::Identity;
	ViewInitOptions.ProjectionMatrix = FCanvas::CalcBaseTransform2D(GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize);
	ViewInitOptions.BackgroundColor = FLinearColor::Black;
	ViewInitOptions.OverlayColor = FLinearColor::White;

	ReferenceView = MakeUnique<FViewInfo>(ViewInitOptions);
	FViewInfo& View = *ReferenceView;
	View.ViewRect = View.UnscaledViewRect;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SetupViewBuffers);

		View.PrimitiveSceneDataOverrideSRV = CachedRayTracingScene->PrimitiveSceneDataBufferSRV;
		View.LightmapSceneDataOverrideSRV = CachedRayTracingScene->LightmapSceneDataBufferSRV;

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SetupUniformBufferParameters);

			// Expanded version of View.InitRHIResources() - need to do SetupSkyIrradianceEnvironmentMapConstants manually because the estimation of skylight is dependent on GetSkySHDiffuse
			View.CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();

			FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(FRHICommandListExecutor::GetImmediateCommandList());

			FBox UnusedVolumeBounds[TVC_MAX];
			View.SetupUniformBufferParameters(
				SceneContext,
				UnusedVolumeBounds,
				TVC_MAX,
				*View.CachedViewUniformShaderParameters);

			if (LightSceneRenderState.SkyLight.IsSet())
			{
				View.CachedViewUniformShaderParameters->SkyIrradianceEnvironmentMap = LightSceneRenderState.SkyLight->SkyIrradianceEnvironmentMap.SRV;
			}
			else
			{
				View.CachedViewUniformShaderParameters->SkyIrradianceEnvironmentMap = GIdentityPrimitiveBuffer.SkyIrradianceEnvironmentMapSRV;
			}

			View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*View.CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);

			CachedRayTracingScene->CachedViewUniformBuffer.UpdateUniformBufferImmediate(*View.CachedViewUniformShaderParameters);
		}
	}

#if RHI_RAYTRACING

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RayTracingScene);

		SCOPED_DRAW_EVENTF(RHICmdList, GPULightmassUpdateRayTracingScene, TEXT("GPULightmass UpdateRayTracingScene %d Instances"), StaticMeshInstanceRenderStates.Elements.Num());

		TArray<FRayTracingGeometryInstance> RayTracingGeometryInstances;
		RayTracingGeometryInstances.Append(CachedRayTracingScene->RayTracingGeometryInstances);

		int32 LandscapeStartOffset = RayTracingGeometryInstances.Num();
		for (FLandscapeRenderState& Landscape : LandscapeRenderStates.Elements)
		{
			for (int32 SubY = 0; SubY < Landscape.NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < Landscape.NumSubsections; SubX++)
				{
					RayTracingGeometryInstances.AddDefaulted(1);
				}
			}
		}

		FMemMark Mark(FMemStack::Get());

		FRayTracingMeshCommandOneFrameArray VisibleRayTracingMeshCommands;
		FDynamicRayTracingMeshCommandStorage DynamicRayTracingMeshCommandStorage;

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Landscapes);

			int32 NumLandscapeInstances = 0;

			for (FLandscapeRenderState& Landscape : LandscapeRenderStates.Elements)
			{
				for (int32 SubY = 0; SubY < Landscape.NumSubsections; SubY++)
				{
					for (int32 SubX = 0; SubX < Landscape.NumSubsections; SubX++)
					{
						const int8 SubSectionIdx = SubX + SubY * Landscape.NumSubsections;
						uint32 NumPrimitives = FMath::Square(Landscape.SubsectionSizeVerts - 1) * 2;

						int32 InstanceIndex = LandscapeStartOffset + NumLandscapeInstances;
						NumLandscapeInstances++;

						FRayTracingGeometryInstance& RayTracingInstance = RayTracingGeometryInstances[InstanceIndex];
						RayTracingInstance.GeometryRHI = Landscape.SectionRayTracingStates[SubSectionIdx]->Geometry.RayTracingGeometryRHI;
						RayTracingInstance.Transforms.Add(FMatrix::Identity);
						RayTracingInstance.NumTransforms = 1;
						RayTracingInstance.UserData.Add((uint32)InstanceIndex);
						RayTracingInstance.Mask = 0xFF;

						TArray<FMeshBatch> MeshBatches = Landscape.GetMeshBatchesForGBufferRendering(0);

						FLandscapeBatchElementParams& BatchElementParams = *(FLandscapeBatchElementParams*)MeshBatches[0].Elements[0].UserData;
						BatchElementParams.LandscapeVertexFactoryMVFUniformBuffer = Landscape.SectionRayTracingStates[SubSectionIdx]->UniformBuffer;

						MeshBatches[0].Elements[0].IndexBuffer = Landscape.SharedBuffers->ZeroOffsetIndexBuffers[0];
						MeshBatches[0].Elements[0].FirstIndex = 0;
						MeshBatches[0].Elements[0].NumPrimitives = NumPrimitives;
						MeshBatches[0].Elements[0].MinVertexIndex = 0;
						MeshBatches[0].Elements[0].MaxVertexIndex = 0;

						bool bAllSegmentsUnlit = true;
						bool bAllSegmentsOpaque = true;

						for (int32 SegmentIndex = 0; SegmentIndex < MeshBatches.Num(); SegmentIndex++)
						{
							FDynamicRayTracingMeshCommandContext CommandContext(DynamicRayTracingMeshCommandStorage, VisibleRayTracingMeshCommands, SegmentIndex, InstanceIndex);
							FMeshPassProcessorRenderState PassDrawRenderState(View.ViewUniformBuffer, View.ViewUniformBuffer);
							FLightmapRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, PassDrawRenderState);

							RayTracingMeshProcessor.AddMeshBatch(MeshBatches[SegmentIndex], 1, nullptr);

							const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
							const FMaterial& Material = MeshBatches[SegmentIndex].MaterialRenderProxy->GetMaterialWithFallback(GMaxRHIFeatureLevel, FallbackMaterialRenderProxyPtr);

							bAllSegmentsUnlit &= Material.GetShadingModels().HasOnlyShadingModel(MSM_Unlit) || !MeshBatches[SegmentIndex].CastShadow;
							bAllSegmentsOpaque &= Material.GetBlendMode() == EBlendMode::BLEND_Opaque;
						}

						if (bAllSegmentsUnlit)
						{
							RayTracingInstance.Mask = 0;
						}

						RayTracingInstance.bForceOpaque = bAllSegmentsOpaque;
					}
				}
			}
		}

		FRayTracingSceneInitializer Initializer;
		Initializer.Instances = RayTracingGeometryInstances;
		Initializer.ShaderSlotsPerGeometrySegment = RAY_TRACING_NUM_SHADER_SLOTS;
		if (IsRayTracingEnabled())
		{
			SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());

			RayTracingScene = RHICreateRayTracingScene(Initializer);
			RHICmdList.BuildAccelerationStructure(RayTracingScene);

			FRayTracingPipelineStateInitializer PSOInitializer;

			PSOInitializer.MaxPayloadSizeInBytes = 60;
			PSOInitializer.bAllowHitGroupIndexing = true;

			TArray<FRHIRayTracingShader*> RayGenShaderTable;
			FLightmapPathTracingRGS::FPermutationDomain PermutationVector;

			PermutationVector.Set<FLightmapPathTracingRGS::FUseFirstBounceRayGuiding>(LightmapRenderer->bUseFirstBounceRayGuiding);

			PermutationVector.Set<FLightmapPathTracingRGS::FUseIrradianceCaching>(false);
			PermutationVector.Set<FLightmapPathTracingRGS::FVisualizeIrradianceCache>(false);
			RayGenShaderTable.Add(GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FLightmapPathTracingRGS>(FLightmapPathTracingRGS::RemapPermutation(PermutationVector)).GetRayTracingShader());
			PermutationVector.Set<FLightmapPathTracingRGS::FUseIrradianceCaching>(true);
			PermutationVector.Set<FLightmapPathTracingRGS::FVisualizeIrradianceCache>(false);
			RayGenShaderTable.Add(GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FLightmapPathTracingRGS>(FLightmapPathTracingRGS::RemapPermutation(PermutationVector)).GetRayTracingShader());
			PermutationVector.Set<FLightmapPathTracingRGS::FUseIrradianceCaching>(true);
			PermutationVector.Set<FLightmapPathTracingRGS::FVisualizeIrradianceCache>(true);
			RayGenShaderTable.Add(GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FLightmapPathTracingRGS>(FLightmapPathTracingRGS::RemapPermutation(PermutationVector)).GetRayTracingShader());
			RayGenShaderTable.Add(GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FStationaryLightShadowTracingRGS>().GetRayTracingShader());
			RayGenShaderTable.Add(GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FVolumetricLightmapPathTracingRGS>().GetRayTracingShader());
			PSOInitializer.SetRayGenShaderTable(RayGenShaderTable);

			auto DefaultClosestHitShader = GetGlobalShaderMap(ERHIFeatureLevel::SM5)->GetShader<FOpaqueShadowHitGroup>().GetRayTracingShader();
			TArray<FRHIRayTracingShader*> RayTracingMaterialLibrary;
			FShaderMapResource::GetRayTracingMaterialLibrary(RayTracingMaterialLibrary, DefaultClosestHitShader);

			PSOInitializer.SetHitGroupTable(RayTracingMaterialLibrary);

			RayTracingPipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, PSOInitializer);

			TUniquePtr<FRayTracingLocalShaderBindingWriter> BindingWriter = MakeUnique<FRayTracingLocalShaderBindingWriter>();

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(SetRayTracingShaderBindings);

				for (const FVisibleRayTracingMeshCommand VisibleMeshCommand : CachedRayTracingScene->VisibleRayTracingMeshCommands)
				{
					const FRayTracingMeshCommand& MeshCommand = *VisibleMeshCommand.RayTracingMeshCommand;

					MeshCommand.ShaderBindings.SetRayTracingShaderBindingsForHitGroup(BindingWriter.Get(),
						VisibleMeshCommand.InstanceIndex,
						MeshCommand.GeometrySegmentIndex,
						MeshCommand.MaterialShaderIndex,
						RAY_TRACING_SHADER_SLOT_MATERIAL);

					MeshCommand.ShaderBindings.SetRayTracingShaderBindingsForHitGroup(BindingWriter.Get(),
						VisibleMeshCommand.InstanceIndex,
						MeshCommand.GeometrySegmentIndex,
						MeshCommand.MaterialShaderIndex,
						RAY_TRACING_SHADER_SLOT_SHADOW);
				}

				for (const FVisibleRayTracingMeshCommand VisibleMeshCommand : VisibleRayTracingMeshCommands)
				{
					const FRayTracingMeshCommand& MeshCommand = *VisibleMeshCommand.RayTracingMeshCommand;

					MeshCommand.ShaderBindings.SetRayTracingShaderBindingsForHitGroup(BindingWriter.Get(),
						VisibleMeshCommand.InstanceIndex,
						MeshCommand.GeometrySegmentIndex,
						MeshCommand.MaterialShaderIndex,
						RAY_TRACING_SHADER_SLOT_MATERIAL);

					MeshCommand.ShaderBindings.SetRayTracingShaderBindingsForHitGroup(BindingWriter.Get(),
						VisibleMeshCommand.InstanceIndex,
						MeshCommand.GeometrySegmentIndex,
						MeshCommand.MaterialShaderIndex,
						RAY_TRACING_SHADER_SLOT_SHADOW);
				}

				{
					// Data is kept alive at the high level and explicitly deleted on RHI timeline,
					// so we can avoid copying parameters to the command list and simply pass raw pointers around.
					const bool bCopyDataToInlineStorage = false;
					BindingWriter->Commit(
						RHICmdList,
						RayTracingScene,
						RayTracingPipelineState,
						bCopyDataToInlineStorage);
				}

				// Move the ray tracing binding container ownership to the command list, so that memory will be
				// released on the RHI thread timeline, after the commands that reference it are processed.
				// TUniquePtr<> auto destructs when exiting the lambda
				RHICmdList.EnqueueLambda([BindingWriter = MoveTemp(BindingWriter)](FRHICommandListImmediate&) {});
			}
		}
	}
#endif
}

void FSceneRenderState::DestroyRayTracingScene()
{
	ReferenceView.Reset();

#if RHI_RAYTRACING
	if (IsRayTracingEnabled() && RayTracingScene.IsValid())
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
		RHICmdList.ClearRayTracingBindings(RayTracingScene);

		RayTracingScene.SafeRelease();
	}
#endif
}

void FSceneRenderState::CalculateDistributionPrefixSumForAllLightmaps()
{
	uint32 PrefixSum = 0;

	for (FLightmapRenderState& Lightmap : LightmapRenderStates.Elements)
	{
		Lightmap.DistributionPrefixSum = PrefixSum;
		PrefixSum += Lightmap.GetNumTilesAcrossAllMipmapLevels();
	}
}

void FLightmapRenderer::Finalize(FRHICommandListImmediate& RHICmdList)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLightmapRenderer::Finalize);

	if (PendingTileRequests.Num() == 0)
	{
		return;
	}

	FMemMark Mark(FMemStack::Get());

	// Upload & copy converged tiles directly
	{
		TArray<FLightmapTileRequest> TileUploadRequests = PendingTileRequests.FilterByPredicate([CurrentRevision = CurrentRevision](const FLightmapTileRequest& Tile) { return Tile.RenderState->DoesTileHaveValidCPUData(Tile.VirtualCoordinates, CurrentRevision); });

		if (TileUploadRequests.Num() > 0)
		{
			SCOPED_DRAW_EVENTF(RHICmdList, GPULightmassUploadConvergedTiles, TEXT("GPULightmass UploadConvergedTiles %d tiles"), TileUploadRequests.Num());

			int32 NewSize = FMath::CeilToInt(FMath::Sqrt(TileUploadRequests.Num()));
			if (!UploadTilePoolGPU.IsValid() || UploadTilePoolGPU->SizeInTiles.X < NewSize)
			{
				UploadTilePoolGPU = MakeUnique<FLightmapTilePoolGPU>(3, FIntPoint(NewSize, NewSize), FIntPoint(GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize));
				UE_LOG(LogGPULightmass, Log, TEXT("Resizing GPULightmass upload tile pool to (%d, %d) %dx%d"), NewSize, NewSize, NewSize * GPreviewLightmapPhysicalTileSize, NewSize * GPreviewLightmapPhysicalTileSize);
			}

			{
				uint32 DstRowPitch;
				FLinearColor* Texture[3];
				Texture[0] = (FLinearColor*)RHICmdList.LockTexture2D(UploadTilePoolGPU->PooledRenderTargets[0]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, RLM_WriteOnly, DstRowPitch, false);
				Texture[1] = (FLinearColor*)RHICmdList.LockTexture2D(UploadTilePoolGPU->PooledRenderTargets[1]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, RLM_WriteOnly, DstRowPitch, false);
				Texture[2] = (FLinearColor*)RHICmdList.LockTexture2D(UploadTilePoolGPU->PooledRenderTargets[2]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, RLM_WriteOnly, DstRowPitch, false);

				ParallelFor(TileUploadRequests.Num(), [&](int32 TileIndex)
				{
					FIntPoint SrcTilePosition(TileUploadRequests[TileIndex].VirtualCoordinates.Position);
					FIntPoint DstTilePosition(TileIndex % UploadTilePoolGPU->SizeInTiles.X, TileIndex / UploadTilePoolGPU->SizeInTiles.X);

					const int32 SrcRowPitchInPixels = TileUploadRequests[TileIndex].RenderState->GetPaddedPhysicalSize().X;
					const int32 DstRowPitchInPixels = DstRowPitch / sizeof(FLinearColor);

					for (int32 Y = 0; Y < GPreviewLightmapPhysicalTileSize; Y++)
					{
						for (int32 X = 0; X < GPreviewLightmapPhysicalTileSize; X++)
						{
							FIntPoint SrcPixelPosition = SrcTilePosition * GPreviewLightmapPhysicalTileSize + FIntPoint(X, Y);
							FIntPoint DstPixelPosition = DstTilePosition * GPreviewLightmapPhysicalTileSize + FIntPoint(X, Y);

							int32 SrcLinearIndex = SrcPixelPosition.Y * SrcRowPitchInPixels + SrcPixelPosition.X;
							int32 DstLinearIndex = DstPixelPosition.Y * DstRowPitchInPixels + DstPixelPosition.X;

							Texture[0][DstLinearIndex] = TileUploadRequests[TileIndex].RenderState->CPUTextureData[0][SrcLinearIndex];
							Texture[1][DstLinearIndex] = TileUploadRequests[TileIndex].RenderState->CPUTextureData[1][SrcLinearIndex];
							Texture[2][DstLinearIndex] = TileUploadRequests[TileIndex].RenderState->CPUTextureData[2][SrcLinearIndex];
						}
					}
				});

				RHICmdList.UnlockTexture2D(UploadTilePoolGPU->PooledRenderTargets[0]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, false);
				RHICmdList.UnlockTexture2D(UploadTilePoolGPU->PooledRenderTargets[1]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, false);
				RHICmdList.UnlockTexture2D(UploadTilePoolGPU->PooledRenderTargets[2]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), 0, false);
			}

			FGPUBatchedTileRequests GPUBatchedTileRequests;

			{
				for (const FLightmapTileRequest& Tile : TileUploadRequests)
				{
					FGPUTileDescription TileDesc;
					TileDesc.LightmapSize = Tile.RenderState->GetSize();
					TileDesc.VirtualTilePosition = Tile.VirtualCoordinates.Position * GPreviewLightmapVirtualTileSize;
					TileDesc.WorkingSetPosition = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * GPreviewLightmapPhysicalTileSize;
					TileDesc.ScratchPosition = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer0Position = Tile.OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer1Position = Tile.OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer2Position = Tile.OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize;
					TileDesc.FrameIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision;
					TileDesc.RenderPassIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex;
					GPUBatchedTileRequests.BatchedTilesDesc.Add(TileDesc);
				}

				FRHIResourceCreateInfo CreateInfo;
				CreateInfo.ResourceArray = &GPUBatchedTileRequests.BatchedTilesDesc;

				GPUBatchedTileRequests.BatchedTilesBuffer = RHICreateStructuredBuffer(sizeof(FGPUTileDescription), GPUBatchedTileRequests.BatchedTilesDesc.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
				GPUBatchedTileRequests.BatchedTilesSRV = RHICreateShaderResourceView(GPUBatchedTileRequests.BatchedTilesBuffer);
			}

			IPooledRenderTarget* OutputRenderTargets[3] = { nullptr, nullptr, nullptr };

			for (auto& Tile : TileUploadRequests)
			{
				for (int32 RenderTargetIndex = 0; RenderTargetIndex < 3; RenderTargetIndex++)
				{
					if (Tile.OutputRenderTargets[RenderTargetIndex] != nullptr)
					{
						if (OutputRenderTargets[RenderTargetIndex] == nullptr)
						{
							OutputRenderTargets[RenderTargetIndex] = Tile.OutputRenderTargets[RenderTargetIndex];
						}
						else
						{
							ensure(OutputRenderTargets[RenderTargetIndex] == Tile.OutputRenderTargets[RenderTargetIndex]);
						}
					}
				}
			}

			FIntPoint DispatchResolution;
			DispatchResolution.X = GPreviewLightmapPhysicalTileSize * GPUBatchedTileRequests.BatchedTilesDesc.Num();
			DispatchResolution.Y = GPreviewLightmapPhysicalTileSize;

			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef StagingHQLayer0 = GraphBuilder.RegisterExternalTexture(UploadTilePoolGPU->PooledRenderTargets[0], TEXT("StagingHQLayer0"));
			FRDGTextureRef StagingHQLayer1 = GraphBuilder.RegisterExternalTexture(UploadTilePoolGPU->PooledRenderTargets[1], TEXT("StagingHQLayer1"));
			FRDGTextureRef StagingShadowMask = GraphBuilder.RegisterExternalTexture(UploadTilePoolGPU->PooledRenderTargets[2], TEXT("StagingShadowMask"));

			FStructuredBufferRHIRef SrcTilePositionsBuffer;
			FShaderResourceViewRHIRef SrcTilePositionsSRV;
			FStructuredBufferRHIRef DstTilePositionsBuffer;
			FShaderResourceViewRHIRef DstTilePositionsSRV;

			if (OutputRenderTargets[0] != nullptr)
			{
				{
					TResourceArray<FIntPoint> SrcTilePositions;
					TResourceArray<FIntPoint> DstTilePositions;

					for (int32 TileIndex = 0; TileIndex < TileUploadRequests.Num(); TileIndex++)
					{
						SrcTilePositions.Add(FIntPoint(TileIndex % UploadTilePoolGPU->SizeInTiles.X, TileIndex / UploadTilePoolGPU->SizeInTiles.X) * GPreviewLightmapPhysicalTileSize);
						DstTilePositions.Add(TileUploadRequests[TileIndex].OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&SrcTilePositions);
						SrcTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), SrcTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						SrcTilePositionsSRV = RHICreateShaderResourceView(SrcTilePositionsBuffer);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&DstTilePositions);
						DstTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), DstTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						DstTilePositionsSRV = RHICreateShaderResourceView(DstTilePositionsBuffer);
					}
				}

				{
					FRDGTextureRef RenderTargetTileAtlas0 = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[0], TEXT("GPULightmassRenderTargetTileAtlas0"));

					FUploadConvergedLightmapTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUploadConvergedLightmapTilesCS::FParameters>();

					PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
					PassParameters->SrcTexture = GraphBuilder.CreateUAV(StagingHQLayer0);
					PassParameters->DstTexture = GraphBuilder.CreateUAV(RenderTargetTileAtlas0);
					PassParameters->SrcTilePositions = SrcTilePositionsSRV;
					PassParameters->DstTilePositions = DstTilePositionsSRV;

					TShaderMapRef<FUploadConvergedLightmapTilesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("UploadConvergedLightmapTiles"),
						ComputeShader,
						PassParameters,
						FComputeShaderUtils::GetGroupCount(DispatchResolution, FComputeShaderUtils::kGolden2DGroupSize));
				}
			}

			if (OutputRenderTargets[1] != nullptr)
			{
				{
					TResourceArray<FIntPoint> SrcTilePositions;
					TResourceArray<FIntPoint> DstTilePositions;

					for (int32 TileIndex = 0; TileIndex < TileUploadRequests.Num(); TileIndex++)
					{
						SrcTilePositions.Add(FIntPoint(TileIndex % UploadTilePoolGPU->SizeInTiles.X, TileIndex / UploadTilePoolGPU->SizeInTiles.X) * GPreviewLightmapPhysicalTileSize);
						DstTilePositions.Add(TileUploadRequests[TileIndex].OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&SrcTilePositions);
						SrcTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), SrcTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						SrcTilePositionsSRV = RHICreateShaderResourceView(SrcTilePositionsBuffer);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&DstTilePositions);
						DstTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), DstTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						DstTilePositionsSRV = RHICreateShaderResourceView(DstTilePositionsBuffer);
					}
				}

				{
					FRDGTextureRef RenderTargetTileAtlas1 = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[1], TEXT("GPULightmassRenderTargetTileAtlas1"));

					FUploadConvergedLightmapTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUploadConvergedLightmapTilesCS::FParameters>();

					PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
					PassParameters->SrcTexture = GraphBuilder.CreateUAV(StagingHQLayer1);
					PassParameters->DstTexture = GraphBuilder.CreateUAV(RenderTargetTileAtlas1);
					PassParameters->SrcTilePositions = SrcTilePositionsSRV;
					PassParameters->DstTilePositions = DstTilePositionsSRV;

					TShaderMapRef<FUploadConvergedLightmapTilesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("UploadConvergedLightmapTiles"),
						ComputeShader,
						PassParameters,
						FComputeShaderUtils::GetGroupCount(DispatchResolution, FComputeShaderUtils::kGolden2DGroupSize));
				}
			}

			if (OutputRenderTargets[2] != nullptr)
			{
				{
					TResourceArray<FIntPoint> SrcTilePositions;
					TResourceArray<FIntPoint> DstTilePositions;

					for (int32 TileIndex = 0; TileIndex < TileUploadRequests.Num(); TileIndex++)
					{
						SrcTilePositions.Add(FIntPoint(TileIndex % UploadTilePoolGPU->SizeInTiles.X, TileIndex / UploadTilePoolGPU->SizeInTiles.X) * GPreviewLightmapPhysicalTileSize);
						DstTilePositions.Add(TileUploadRequests[TileIndex].OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&SrcTilePositions);
						SrcTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), SrcTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						SrcTilePositionsSRV = RHICreateShaderResourceView(SrcTilePositionsBuffer);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&DstTilePositions);
						DstTilePositionsBuffer = RHICreateStructuredBuffer(sizeof(FIntPoint), DstTilePositions.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						DstTilePositionsSRV = RHICreateShaderResourceView(DstTilePositionsBuffer);
					}
				}

				{
					FRDGTextureRef RenderTargetTileAtlas2 = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[2], TEXT("GPULightmassRenderTargetTileAtlas1"));

					FUploadConvergedLightmapTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUploadConvergedLightmapTilesCS::FParameters>();

					PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
					PassParameters->SrcTexture = GraphBuilder.CreateUAV(StagingShadowMask);
					PassParameters->DstTexture = GraphBuilder.CreateUAV(RenderTargetTileAtlas2);
					PassParameters->SrcTilePositions = SrcTilePositionsSRV;
					PassParameters->DstTilePositions = DstTilePositionsSRV;

					TShaderMapRef<FUploadConvergedLightmapTilesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("UploadConvergedLightmapTiles"),
						ComputeShader,
						PassParameters,
						FComputeShaderUtils::GetGroupCount(DispatchResolution, FComputeShaderUtils::kGolden2DGroupSize));
				}
			}

			GraphBuilder.Execute();
		}

		// Drop these converged requests, critical so that we won't perform readback repeatedly
		PendingTileRequests = PendingTileRequests.FilterByPredicate([CurrentRevision = CurrentRevision](const FLightmapTileRequest& Tile) { return !Tile.RenderState->DoesTileHaveValidCPUData(Tile.VirtualCoordinates, CurrentRevision); });
	}

	PendingTileRequests = PendingTileRequests.FilterByPredicate([](const FLightmapTileRequest& Tile) { return !Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).bHasReadbackInFlight; });

	if (!bInsideBackgroundTick)
	{
		TArray<FLightmapTileRequest> RoundRobinFilteredRequests;
		if (PendingTileRequests.Num() > (128 * (int32)GNumExplicitGPUsForRendering))
		{
			int32 RoundRobinDivisor = PendingTileRequests.Num() / (128 * (int32)GNumExplicitGPUsForRendering);

			for (int32 Index = 0; Index < PendingTileRequests.Num(); Index++)
			{
				if (Index % RoundRobinDivisor == FrameNumber % RoundRobinDivisor)
				{
					RoundRobinFilteredRequests.Add(PendingTileRequests[Index]);
				}
			}

			PendingTileRequests = RoundRobinFilteredRequests;
		}
	}

	// Alloc for tiles that need work
	{
		// Find which tiles are already resident
		TArray<FVirtualTile> TilesToQuery;
		for (auto& Tile : PendingTileRequests)
		{
			checkSlow(TilesToQuery.Find(FVirtualTile{ Tile.RenderState, Tile.VirtualCoordinates.MipLevel, (int32)Tile.VirtualCoordinates.GetVirtualAddress() }) == INDEX_NONE);
			TilesToQuery.Add(FVirtualTile{ Tile.RenderState, Tile.VirtualCoordinates.MipLevel, (int32)Tile.VirtualCoordinates.GetVirtualAddress() });
		}
		TArray<uint32> TileAddressIfResident;
		LightmapTilePoolGPU.QueryResidency(TilesToQuery, TileAddressIfResident);

		// We lock tiles that are resident and requested for current frame so that they won't be evicted by the following AllocAndLock
		TArray<FVirtualTile> NonResidentTilesToAllocate;
		TArray<int32> NonResidentTileRequestIndices;
		TArray<int32> ResidentTilesToLock;
		for (int32 TileIndex = 0; TileIndex < TileAddressIfResident.Num(); TileIndex++)
		{
			if (TileAddressIfResident[TileIndex] == ~0u)
			{
				NonResidentTilesToAllocate.Add(TilesToQuery[TileIndex]);
				NonResidentTileRequestIndices.Add(TileIndex);
			}
			else
			{
				ResidentTilesToLock.Add(TileAddressIfResident[TileIndex]);
				PendingTileRequests[TileIndex].TileAddressInWorkingSet = TileAddressIfResident[TileIndex];
			}
		}

		LightmapTilePoolGPU.Lock(ResidentTilesToLock);

		{
			TArray<int32> SuccessfullyAllocatedTiles;
			LightmapTilePoolGPU.AllocAndLock(NonResidentTilesToAllocate.Num(), SuccessfullyAllocatedTiles);

			// Map successfully allocated tiles, potentially evict some resident tiles to the lower cache tiers
			TArray<FVirtualTile> TilesToMap;
			for (int32 TileIndex = 0; TileIndex < SuccessfullyAllocatedTiles.Num(); TileIndex++)
			{
				TilesToMap.Add(NonResidentTilesToAllocate[TileIndex]);

				auto& Tile = PendingTileRequests[NonResidentTileRequestIndices[TileIndex]];
				Tile.TileAddressInWorkingSet = SuccessfullyAllocatedTiles[TileIndex];
				Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision = -1;
				Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex = 0;
			}

			// Till this point there might still be tiles with ~0u (which have failed allocation), they will be dropped later

			TArray<FVirtualTile> TilesEvicted;
			LightmapTilePoolGPU.Map(TilesToMap, SuccessfullyAllocatedTiles, TilesEvicted);

			// Invalidate evicted tiles' state as they can't be read back anymore
			// TODO: save to CPU and reload when appropriate
			for (const FVirtualTile& Tile : TilesEvicted)
			{
				if (Tile.RenderState.IsValid())
				{
					Tile.RenderState->RetrieveTileState(FTileVirtualCoordinates(Tile.VirtualAddress, Tile.MipLevel)).Revision = -1;
					Tile.RenderState->RetrieveTileState(FTileVirtualCoordinates(Tile.VirtualAddress, Tile.MipLevel)).RenderPassIndex = 0;
				}
			}

			LightmapTilePoolGPU.MakeAvailable(SuccessfullyAllocatedTiles, FrameNumber);
		}

		LightmapTilePoolGPU.MakeAvailable(ResidentTilesToLock, FrameNumber);

		{
			bool bScratchAllocationSucceeded = false;

			while (!bScratchAllocationSucceeded)
			{
				if (ScratchTilePoolGPU.IsValid())
				{
					TArray<int32> SuccessfullyAllocatedTiles;
					ScratchTilePoolGPU->AllocAndLock(TilesToQuery.Num(), SuccessfullyAllocatedTiles);

					if (SuccessfullyAllocatedTiles.Num() == TilesToQuery.Num())
					{
						for (int32 TileIndex = 0; TileIndex < SuccessfullyAllocatedTiles.Num(); TileIndex++)
						{
							auto& Tile = PendingTileRequests[TileIndex];
							Tile.TileAddressInScratch = SuccessfullyAllocatedTiles[TileIndex];
						}

						bScratchAllocationSucceeded = true;
					}

					ScratchTilePoolGPU->MakeAvailable(SuccessfullyAllocatedTiles, FrameNumber);
				}

				if (!bScratchAllocationSucceeded)
				{
					if (ScratchTilePoolGPU.IsValid() && ScratchTilePoolGPU->SizeInTiles.X >= 64)
					{
						// If we have reached our limit, don't retry and drop the requests.
						// Till this point there might still be tiles with ~0u (which have failed allocation), they will be dropped later
						break;
					}

					int32 NewSize = FMath::Min(FMath::CeilToInt(FMath::Sqrt(TilesToQuery.Num())), 64);
					ScratchTilePoolGPU = MakeUnique<FLightmapTilePoolGPU>(3, FIntPoint(NewSize, NewSize), FIntPoint(GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize));
					UE_LOG(LogGPULightmass, Log, TEXT("Resizing GPULightmass scratch tile pool to (%d, %d) %dx%d"), NewSize, NewSize, NewSize * GPreviewLightmapPhysicalTileSize, NewSize * GPreviewLightmapPhysicalTileSize);
				}
			}
		}

		// Drop requests that have failed allocation
		PendingTileRequests = PendingTileRequests.FilterByPredicate([](const FLightmapTileRequest& TileRequest) { return TileRequest.TileAddressInWorkingSet != ~0u && TileRequest.TileAddressInScratch != ~0u; });
	}

	// If all tiles have failed allocation (unlikely but possible), return immediately
	if (PendingTileRequests.Num() == 0)
	{
		return;
	}

	Scene->SetupRayTracingScene();

	SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::GPU0());

	IPooledRenderTarget* OutputRenderTargets[3] = { nullptr, nullptr, nullptr };

	for (auto& Tile : PendingTileRequests)
	{
		for (int32 RenderTargetIndex = 0; RenderTargetIndex < 3; RenderTargetIndex++)
		{
			if (Tile.OutputRenderTargets[RenderTargetIndex] != nullptr)
			{
				if (OutputRenderTargets[RenderTargetIndex] == nullptr)
				{
					OutputRenderTargets[RenderTargetIndex] = Tile.OutputRenderTargets[RenderTargetIndex];
				}
				else
				{
					ensure(OutputRenderTargets[RenderTargetIndex] == Tile.OutputRenderTargets[RenderTargetIndex]);
				}
			}
		}
	}

	// Perform deferred invalidation
	{
		// Clear working set pools
		for (int PoolLayerIndex = 0; PoolLayerIndex < LightmapTilePoolGPU.PooledRenderTargets.Num(); PoolLayerIndex++)
		{
			SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
			FRHIRenderPassInfo RPInfo(LightmapTilePoolGPU.PooledRenderTargets[PoolLayerIndex]->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::DontLoad_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearLightmapTilePoolGPU"));
			for (auto& Tile : PendingTileRequests)
			{
				if (Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision != CurrentRevision)
				{
					RHICmdList.SetViewport(
						LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet).X * LightmapTilePoolGPU.LayerFormatAndTileSize[PoolLayerIndex].TileSize.X,
						LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet).Y * LightmapTilePoolGPU.LayerFormatAndTileSize[PoolLayerIndex].TileSize.Y,
						0.0f,
						(LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet).X + 1) * LightmapTilePoolGPU.LayerFormatAndTileSize[PoolLayerIndex].TileSize.X,
						(LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet).Y + 1) * LightmapTilePoolGPU.LayerFormatAndTileSize[PoolLayerIndex].TileSize.Y,
						1.0f);
					DrawClearQuad(RHICmdList, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
				}
			}
			RHICmdList.EndRenderPass();
		}

		for (auto& Tile : PendingTileRequests)
		{
			if (Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision != CurrentRevision)
			{
				{
					// Reset GI sample states
					Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex = 0;
				}

				{
					// Clear stationary light sample states
					Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantDirectionalLightSampleCount.Empty();
					Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantPointLightSampleCount.Empty();
					Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantSpotLightSampleCount.Empty();
					Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantRectLightSampleCount.Empty();

					for (FDirectionalLightRenderState& DirectionalLight : Scene->LightSceneRenderState.DirectionalLights.Elements)
					{
						if (DirectionalLight.bStationary)
						{
							Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantDirectionalLightSampleCount.Add(FDirectionalLightRenderStateRef(DirectionalLight, Scene->LightSceneRenderState.DirectionalLights), 0);
						}
					}

					for (FPointLightRenderStateRef& PointLight : Tile.RenderState->RelevantPointLights)
					{
						check(PointLight->bStationary);

						Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantPointLightSampleCount.Add(PointLight, 0);
					}

					for (FSpotLightRenderStateRef& SpotLight : Tile.RenderState->RelevantSpotLights)
					{
						check(SpotLight->bStationary);

						Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantSpotLightSampleCount.Add(SpotLight, 0);
					}

					for (FRectLightRenderStateRef& RectLight : Tile.RenderState->RelevantRectLights)
					{
						check(RectLight->bStationary);

						Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantRectLightSampleCount.Add(RectLight, 0);
					}
				}

				{
					// Last step: set invalidation state to 'valid'
					Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision = CurrentRevision;
				}
			}
		}
	}

	bool bLastFewFramesIdle = !GCurrentLevelEditingViewportClient || !GCurrentLevelEditingViewportClient->IsRealtime();
	int32 NumSamplesPerFrame = (bInsideBackgroundTick && bLastFewFramesIdle) ? 8 : 1;

	{
		TArray<FLightmapTileRequest> PendingGITileRequests = PendingTileRequests.FilterByPredicate([](const FLightmapTileRequest& Tile) { return !Tile.RenderState->IsTileGIConverged(Tile.VirtualCoordinates); });

		// Render GI
		for (int32 SampleIndex = 0; SampleIndex < NumSamplesPerFrame; SampleIndex++)
		{
			FMemMark PerSampleMark(FMemStack::Get());

			{
				if (PendingGITileRequests.Num() > 0)
				{
					for (int ScratchLayerIndex = 0; ScratchLayerIndex < 3; ScratchLayerIndex++)
					{
						SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());

						FRHIRenderPassInfo RPInfo(ScratchTilePoolGPU->PooledRenderTargets[ScratchLayerIndex]->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::DontLoad_Store);
						RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearScratchTillPoolGPU"));

						for (auto& Tile : PendingGITileRequests)
						{
							RHICmdList.SetViewport(
								ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).X * GPreviewLightmapPhysicalTileSize,
								ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).Y * GPreviewLightmapPhysicalTileSize,
								0.0f,
								(ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).X + 1) * GPreviewLightmapPhysicalTileSize,
								(ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).Y + 1) * GPreviewLightmapPhysicalTileSize,
								1.0f);
							DrawClearQuad(RHICmdList, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
						}
						RHICmdList.EndRenderPass();
					}

					{
						for (uint32 GPUIndex = 0; GPUIndex < GNumExplicitGPUsForRendering; GPUIndex++)
						{
							SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::FromIndex(GPUIndex));

							FRHIRenderPassInfo RPInfo(FRHIRenderPassInfo::NoRenderTargets);
							RHICmdList.BeginRenderPass(RPInfo, TEXT("LightmapGBuffer"));

							for (const FLightmapTileRequest& Tile : PendingGITileRequests)
							{							
								if (Tile.RenderState->IsTileGIConverged(Tile.VirtualCoordinates)) continue;
								uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
								if (AssignedGPUIndex != GPUIndex) continue;

								RHICmdList.SetViewport(0, 0, 0.0f, GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize, 1.0f);

								float ScaleX = Tile.RenderState->GetPaddedSizeInTiles().X * GPreviewLightmapVirtualTileSize * 1.0f / (1 << Tile.VirtualCoordinates.MipLevel) / GPreviewLightmapPhysicalTileSize;
								float ScaleY = Tile.RenderState->GetPaddedSizeInTiles().Y * GPreviewLightmapVirtualTileSize * 1.0f / (1 << Tile.VirtualCoordinates.MipLevel) / GPreviewLightmapPhysicalTileSize;
								float BiasX = (1.0f * (-Tile.VirtualCoordinates.Position.X * GPreviewLightmapVirtualTileSize) - (-GPreviewLightmapTileBorderSize)) / GPreviewLightmapPhysicalTileSize;
								float BiasY = (1.0f * (-Tile.VirtualCoordinates.Position.Y * GPreviewLightmapVirtualTileSize) - (-GPreviewLightmapTileBorderSize)) / GPreviewLightmapPhysicalTileSize;

								FVector4 VirtualTexturePhysicalTileCoordinateScaleAndBias = FVector4(ScaleX, ScaleY, BiasX, BiasY);

								FLightmapGBufferParams LightmapGBufferParameters{};
								LightmapGBufferParameters.RenderPassIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex;
								LightmapGBufferParameters.VirtualTexturePhysicalTileCoordinateScaleAndBias = VirtualTexturePhysicalTileCoordinateScaleAndBias;
								LightmapGBufferParameters.ScratchTilePoolOffset = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
								LightmapGBufferParameters.ScratchTilePoolLayer0 = ScratchTilePoolGPU->PooledRenderTargets[0]->GetRenderTargetItem().UAV;
								LightmapGBufferParameters.ScratchTilePoolLayer1 = ScratchTilePoolGPU->PooledRenderTargets[1]->GetRenderTargetItem().UAV;
								LightmapGBufferParameters.ScratchTilePoolLayer2 = ScratchTilePoolGPU->PooledRenderTargets[2]->GetRenderTargetItem().UAV;
								FLightmapGBufferUniformBufferRef PassUniformBuffer = FLightmapGBufferUniformBufferRef::CreateUniformBufferImmediate(LightmapGBufferParameters, UniformBuffer_SingleDraw);

								TArray<FMeshBatch> MeshBatches = Tile.RenderState->GeometryInstanceRef.GetMeshBatchesForGBufferRendering(Tile.VirtualCoordinates);

								for (auto& MeshBatch : MeshBatches)
								{
									FMeshBatchElement& Element = MeshBatch.Elements[0];

									Element.DynamicPrimitiveShaderDataIndex = Tile.RenderState->GeometryInstanceRef.GetElementId();
								}

								DrawDynamicMeshPass(
									*Scene->ReferenceView, RHICmdList,
									[View = Scene->ReferenceView.Get(), PassUniformBuffer, MeshBatches](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
								{
									FLightmapGBufferMeshProcessor MeshProcessor(nullptr, View, DynamicMeshPassContext, PassUniformBuffer);

									for (auto& MeshBatch : MeshBatches)
									{
										MeshProcessor.AddMeshBatch(MeshBatch, ~0ull, nullptr);
									}
								});

								GPrimitiveIdVertexBufferPool.DiscardAll();
							}

							RHICmdList.EndRenderPass();
						}
					}

#if RHI_RAYTRACING
					if (IsRayTracingEnabled())
					{
						for (uint32 GPUIndex = 0; GPUIndex < GNumExplicitGPUsForRendering; GPUIndex++)
						{
							FGPUBatchedTileRequests GPUBatchedTileRequests;

							for (const FLightmapTileRequest& Tile : PendingGITileRequests)
							{
								uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
								if (AssignedGPUIndex != GPUIndex) continue;

								FGPUTileDescription TileDesc;
								TileDesc.LightmapSize = Tile.RenderState->GetSize();
								TileDesc.VirtualTilePosition = Tile.VirtualCoordinates.Position * GPreviewLightmapVirtualTileSize;
								TileDesc.WorkingSetPosition = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * GPreviewLightmapPhysicalTileSize;
								TileDesc.ScratchPosition = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer0Position = Tile.OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer1Position = Tile.OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer2Position = Tile.OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize;
								TileDesc.FrameIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision;
								TileDesc.RenderPassIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex;
								if (!Tile.RenderState->IsTileGIConverged(Tile.VirtualCoordinates))
								{
									Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex++;

									if (/*Tile.VirtualCoordinates.MipLevel == 0 && */SampleIndex == 0)
									{
										if (!bInsideBackgroundTick)
										{
											Mip0WorkDoneLastFrame++;
										}
									}

									GPUBatchedTileRequests.BatchedTilesDesc.Add(TileDesc);
								}
							}

							if (GPUBatchedTileRequests.BatchedTilesDesc.Num() > 0)
							{
								FRHIResourceCreateInfo CreateInfo;
								CreateInfo.ResourceArray = &GPUBatchedTileRequests.BatchedTilesDesc;

								GPUBatchedTileRequests.BatchedTilesBuffer = RHICreateStructuredBuffer(sizeof(FGPUTileDescription), GPUBatchedTileRequests.BatchedTilesDesc.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
								GPUBatchedTileRequests.BatchedTilesSRV = RHICreateShaderResourceView(GPUBatchedTileRequests.BatchedTilesBuffer);
							}

							SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::FromIndex(GPUIndex));

							if (GPUBatchedTileRequests.BatchedTilesDesc.Num() > 0)
							{
								FRDGBuilder GraphBuilder(RHICmdList);

								FRDGTextureRef GBufferWorldPosition = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[0], TEXT("GBufferWorldPosition"));
								FRDGTextureRef GBufferWorldNormal = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[1], TEXT("GBufferWorldNormal"));
								FRDGTextureRef GBufferShadingNormal = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[2], TEXT("GBufferShadingNormal"));
								FRDGTextureRef IrradianceAndSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[0], TEXT("IrradianceAndSampleCount"));
								FRDGTextureRef SHDirectionality = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[1], TEXT("SHDirectionality"));
								FRDGTextureRef SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[4], TEXT("SHCorrectionAndStationarySkyLightBentNormal"));

								FRDGTextureRef RayGuidingLuminance = nullptr;
								FRDGTextureRef RayGuidingSampleCount = nullptr;
								FRDGTextureRef RayGuidingCDFX = nullptr;
								FRDGTextureRef RayGuidingCDFY = nullptr;

								if (bUseFirstBounceRayGuiding)
								{
									RayGuidingLuminance = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[5], TEXT("RayGuidingLuminance"));
									RayGuidingSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[6], TEXT("RayGuidingSampleCount"));
									RayGuidingCDFX = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[7], TEXT("RayGuidingCDFX"));
									RayGuidingCDFY = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[8], TEXT("RayGuidingCDFY"));
								}

								// These two buffers must have lifetime extended beyond GraphBuilder.Execute()
								TUniformBufferRef<FPathTracingLightData> LightDataUniformBuffer;
								TUniformBufferRef<FSkyLightData> SkyLightDataUniformBuffer;

								FIntPoint RayTracingResolution;
								RayTracingResolution.X = GPreviewLightmapPhysicalTileSize * GPUBatchedTileRequests.BatchedTilesDesc.Num();
								RayTracingResolution.Y = GPreviewLightmapPhysicalTileSize;

								// Path Tracing GI
								{
									{
										FLightmapPathTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLightmapPathTracingRGS::FParameters>();
										PassParameters->LastInvalidationFrame = LastInvalidationFrame;
										PassParameters->NumTotalSamples = GGPULightmassSamplesPerTexel;
										PassParameters->TLAS = Scene->RayTracingScene->GetShaderResourceView();
										PassParameters->GBufferWorldPosition = GraphBuilder.CreateUAV(GBufferWorldPosition);
										PassParameters->GBufferWorldNormal = GraphBuilder.CreateUAV(GBufferWorldNormal);
										PassParameters->GBufferShadingNormal = GraphBuilder.CreateUAV(GBufferShadingNormal);
										PassParameters->IrradianceAndSampleCount = GraphBuilder.CreateUAV(IrradianceAndSampleCount);
										PassParameters->SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.CreateUAV(SHCorrectionAndStationarySkyLightBentNormal);
										PassParameters->SHDirectionality = GraphBuilder.CreateUAV(SHDirectionality);

										if (bUseFirstBounceRayGuiding)
										{
											PassParameters->RayGuidingLuminance = GraphBuilder.CreateUAV(RayGuidingLuminance);
											PassParameters->RayGuidingSampleCount = GraphBuilder.CreateUAV(RayGuidingSampleCount);
											PassParameters->RayGuidingCDFX = GraphBuilder.CreateUAV(RayGuidingCDFX);
											PassParameters->RayGuidingCDFY = GraphBuilder.CreateUAV(RayGuidingCDFY);
											PassParameters->NumRayGuidingTrialSamples = NumFirstBounceRayGuidingTrialSamples;
										}

										PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
										PassParameters->ViewUniformBuffer = Scene->ReferenceView->ViewUniformBuffer;
										PassParameters->IrradianceCachingParameters = Scene->IrradianceCache->IrradianceCachingParametersUniformBuffer;

										{
											LightDataUniformBuffer = CreateUniformBufferImmediate(SetupPathTracingLightParameters(Scene->LightSceneRenderState), EUniformBufferUsage::UniformBuffer_SingleFrame);
											PassParameters->LightParameters = LightDataUniformBuffer;
										}

										{
											SkyLightDataUniformBuffer = CreateUniformBufferImmediate(SetupSkyLightParameters(Scene->LightSceneRenderState), EUniformBufferUsage::UniformBuffer_SingleFrame);
											PassParameters->SkyLight = SkyLightDataUniformBuffer;
										}

										FLightmapPathTracingRGS::FPermutationDomain PermutationVector;
										PermutationVector.Set<FLightmapPathTracingRGS::FUseFirstBounceRayGuiding>(bUseFirstBounceRayGuiding);
										PermutationVector.Set<FLightmapPathTracingRGS::FUseIrradianceCaching>(GGPULightmassUseIrradianceCaching == 1);
										PermutationVector.Set<FLightmapPathTracingRGS::FVisualizeIrradianceCache>(GGPULightmassVisualizeIrradianceCache == 1);
										auto RayGenerationShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FLightmapPathTracingRGS>(FLightmapPathTracingRGS::RemapPermutation(PermutationVector));
										ClearUnusedGraphResources(RayGenerationShader, PassParameters);

										GraphBuilder.AddPass(
											RDG_EVENT_NAME("LightmapPathTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
											PassParameters,
											ERDGPassFlags::Compute,
											[PassParameters, this, RayTracingScene = Scene->RayTracingScene, PipelineState = Scene->RayTracingPipelineState, RayGenerationShader, RayTracingResolution, GPUIndex](FRHICommandList& RHICmdList)
										{
											FRayTracingShaderBindingsWriter GlobalResources;
											SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

											check(RHICmdList.GetGPUMask().HasSingleIndex());

											RHICmdList.RayTraceDispatch(PipelineState, RayGenerationShader.GetRayTracingShader(), RayTracingScene, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
										});
									}

									if (bUseFirstBounceRayGuiding)
									{
										FFirstBounceRayGuidingCDFBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFirstBounceRayGuidingCDFBuildCS::FParameters>();

										PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
										PassParameters->RayGuidingLuminance = GraphBuilder.CreateUAV(RayGuidingLuminance);
										PassParameters->RayGuidingSampleCount = GraphBuilder.CreateUAV(RayGuidingSampleCount);
										PassParameters->RayGuidingCDFX = GraphBuilder.CreateUAV(RayGuidingCDFX);
										PassParameters->RayGuidingCDFY = GraphBuilder.CreateUAV(RayGuidingCDFY);
										PassParameters->NumRayGuidingTrialSamples = NumFirstBounceRayGuidingTrialSamples;

										TShaderMapRef<FFirstBounceRayGuidingCDFBuildCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
										FComputeShaderUtils::AddPass(
											GraphBuilder,
											RDG_EVENT_NAME("FirstBounceRayGuidingCDFBuild"),
											ComputeShader,
											PassParameters,
											FIntVector(GPUBatchedTileRequests.BatchedTilesDesc.Num() * 256, 1, 1));
									}
								}

								GraphBuilder.Execute();
							}
						}
					}
#endif
				}
			}
		}
	}

	for (int32 SampleIndex = 0; SampleIndex < NumSamplesPerFrame; SampleIndex++)
	{
		FMemMark PerSampleMark(FMemStack::Get());

		// Render shadow mask
		{
			TArray<FLightmapTileRequest> PendingShadowTileRequestsOnAllGPUs = PendingTileRequests.FilterByPredicate([](const FLightmapTileRequest& Tile) { return !Tile.RenderState->IsTileShadowConverged(Tile.VirtualCoordinates); });

			if (PendingShadowTileRequestsOnAllGPUs.Num() > 0)
			{
				for (uint32 GPUIndex = 0; GPUIndex < GNumExplicitGPUsForRendering; GPUIndex++)
				{
					SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::FromIndex(GPUIndex));

					auto PendingShadowTileRequests = PendingShadowTileRequestsOnAllGPUs.FilterByPredicate(
						[GPUIndex](const FLightmapTileRequest& Tile)
					{
						uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
						return AssignedGPUIndex == GPUIndex;
					});

					if (PendingShadowTileRequests.Num() == 0) continue;

					FRDGBuilder GraphBuilder(RHICmdList);

					FRDGTextureRef RenderTargetTileAtlas = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[2], TEXT("GPULightmassRenderTargetTileAtlas2"));

					FRDGTextureRef GBufferWorldPosition = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[0], TEXT("GBufferWorldPosition"));
					FRDGTextureRef GBufferWorldNormal = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[1], TEXT("GBufferWorldNormal"));
					FRDGTextureRef GBufferShadingNormal = GraphBuilder.RegisterExternalTexture(ScratchTilePoolGPU->PooledRenderTargets[2], TEXT("GBufferShadingNormal"));

					FRDGTextureRef ShadowMask = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[2], TEXT("ShadowMask"));
					FRDGTextureRef ShadowMaskSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[3], TEXT("ShadowMaskSampleCount"));

					TResourceArray<int32> LightTypeArray;
					FVertexBufferRHIRef LightTypeBuffer;
					FShaderResourceViewRHIRef LightTypeSRV;

					TResourceArray<int32> ChannelIndexArray;
					FVertexBufferRHIRef ChannelIndexBuffer;
					FShaderResourceViewRHIRef ChannelIndexSRV;

					TResourceArray<int32> LightSampleIndexArray;
					FVertexBufferRHIRef LightSampleIndexBuffer;
					FShaderResourceViewRHIRef LightSampleIndexSRV;

					TResourceArray<FLightShaderConstants> LightShaderParameterArray;
					FStructuredBufferRHIRef LightShaderParameterBuffer;
					FShaderResourceViewRHIRef LightShaderParameterSRV;

					for (const FLightmapTileRequest& Tile : PendingShadowTileRequests)
					{
						// Gather all unconverged lights, then pick one based on RoundRobinIndex
						TArray<int32> UnconvergedLightTypeArray;
						TArray<int32> UnconvergedChannelIndexArray;
						TArray<int32> UnconvergedLightSampleIndexArray;
						TArray<FLightShaderConstants> UnconvergedLightShaderParameterArray;

						for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantDirectionalLightSampleCount)
						{
							if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
							{
								UnconvergedLightTypeArray.Add(0);
								UnconvergedChannelIndexArray.Add(Pair.Key->ShadowMapChannel);
								UnconvergedLightShaderParameterArray.Add(FLightShaderConstants(Pair.Key->GetLightShaderParameters()));
								UnconvergedLightSampleIndexArray.Add(Pair.Value);
							}
						}

						for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantPointLightSampleCount)
						{
							if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
							{
								UnconvergedLightTypeArray.Add(1);
								UnconvergedChannelIndexArray.Add(Pair.Key->ShadowMapChannel);
								UnconvergedLightShaderParameterArray.Add(FLightShaderConstants(Pair.Key->GetLightShaderParameters()));
								UnconvergedLightSampleIndexArray.Add(Pair.Value);
							}
						}

						for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantSpotLightSampleCount)
						{
							if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
							{
								UnconvergedLightTypeArray.Add(2);
								UnconvergedChannelIndexArray.Add(Pair.Key->ShadowMapChannel);
								UnconvergedLightShaderParameterArray.Add(FLightShaderConstants(Pair.Key->GetLightShaderParameters()));
								UnconvergedLightSampleIndexArray.Add(Pair.Value);
							}
						}

						for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantRectLightSampleCount)
						{
							if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
							{
								UnconvergedLightTypeArray.Add(3);
								UnconvergedChannelIndexArray.Add(Pair.Key->ShadowMapChannel);
								UnconvergedLightShaderParameterArray.Add(FLightShaderConstants(Pair.Key->GetLightShaderParameters()));
								UnconvergedLightSampleIndexArray.Add(Pair.Value);
							}
						}

						int32 PickedLightIndex = Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RoundRobinIndex % UnconvergedLightTypeArray.Num();

						LightTypeArray.Add(UnconvergedLightTypeArray[PickedLightIndex]);
						ChannelIndexArray.Add(UnconvergedChannelIndexArray[PickedLightIndex]);
						LightSampleIndexArray.Add(UnconvergedLightSampleIndexArray[PickedLightIndex]);
						LightShaderParameterArray.Add(UnconvergedLightShaderParameterArray[PickedLightIndex]);

						Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RoundRobinIndex++;

						{
							int32 LightIndex = 0;
							bool bFoundPickedLight = false;

							for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantDirectionalLightSampleCount)
							{
								if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
								{
									if (LightIndex == PickedLightIndex)
									{
										Pair.Value++;
										bFoundPickedLight = true;
										break;
									}
									LightIndex++;
								}
							}

							for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantPointLightSampleCount)
							{
								if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
								{
									if (LightIndex == PickedLightIndex)
									{
										Pair.Value++;
										bFoundPickedLight = true;
										break;
									}
									LightIndex++;
								}
							}

							for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantSpotLightSampleCount)
							{
								if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
								{
									if (LightIndex == PickedLightIndex)
									{
										Pair.Value++;
										bFoundPickedLight = true;
										break;
									}
									LightIndex++;
								}
							}

							for (auto& Pair : Tile.RenderState->RetrieveTileRelevantLightSampleState(Tile.VirtualCoordinates).RelevantRectLightSampleCount)
							{
								if (GGPULightmassShadowSamplesPerTexel < 0 || Pair.Value < GGPULightmassShadowSamplesPerTexel)
								{
									if (LightIndex == PickedLightIndex)
									{
										Pair.Value++;
										bFoundPickedLight = true;
										break;
									}
									LightIndex++;
								}
							}

							check(bFoundPickedLight);
						}
					}

					check(PendingShadowTileRequests.Num() == LightTypeArray.Num());

					{
						FRHIResourceCreateInfo CreateInfo(&LightTypeArray);
						LightTypeBuffer = RHICreateVertexBuffer(LightTypeArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						LightTypeSRV = RHICreateShaderResourceView(LightTypeBuffer, sizeof(int32), PF_R32_SINT);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&ChannelIndexArray);
						ChannelIndexBuffer = RHICreateVertexBuffer(ChannelIndexArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						ChannelIndexSRV = RHICreateShaderResourceView(ChannelIndexBuffer, sizeof(int32), PF_R32_SINT);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&LightSampleIndexArray);
						LightSampleIndexBuffer = RHICreateVertexBuffer(LightSampleIndexArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						LightSampleIndexSRV = RHICreateShaderResourceView(LightSampleIndexBuffer, sizeof(int32), PF_R32_SINT);
					}

					{
						FRHIResourceCreateInfo CreateInfo(&LightShaderParameterArray);
						LightShaderParameterBuffer = RHICreateStructuredBuffer(sizeof(FLightShaderConstants), LightShaderParameterArray.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
						LightShaderParameterSRV = RHICreateShaderResourceView(LightShaderParameterBuffer);
					}

					// Render GBuffer
					{
						for (int ScratchLayerIndex = 0; ScratchLayerIndex < 3; ScratchLayerIndex++)
						{
							FRHIRenderPassInfo RPInfo(ScratchTilePoolGPU->PooledRenderTargets[ScratchLayerIndex]->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::DontLoad_Store);
							RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearScratchTillPoolGPU"));

							for (auto& Tile : PendingShadowTileRequests)
							{
								RHICmdList.SetViewport(
									ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).X * GPreviewLightmapPhysicalTileSize,
									ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).Y * GPreviewLightmapPhysicalTileSize,
									0.0f,
									(ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).X + 1) * GPreviewLightmapPhysicalTileSize,
									(ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch).Y + 1) * GPreviewLightmapPhysicalTileSize,
									1.0f);
								DrawClearQuad(RHICmdList, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
							}
							RHICmdList.EndRenderPass();
						}

						FRHIRenderPassInfo RPInfo(FRHIRenderPassInfo::NoRenderTargets);
						RHICmdList.BeginRenderPass(RPInfo, TEXT("LightmapGBuffer"));

						for (int32 TileIndex = 0; TileIndex < PendingShadowTileRequests.Num(); TileIndex++)
						{
							const FLightmapTileRequest& Tile = PendingShadowTileRequests[TileIndex];

							RHICmdList.SetViewport(0, 0, 0.0f, GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize, 1.0f);

							float ScaleX = Tile.RenderState->GetPaddedSizeInTiles().X * GPreviewLightmapVirtualTileSize * 1.0f / (1 << Tile.VirtualCoordinates.MipLevel) / GPreviewLightmapPhysicalTileSize;
							float ScaleY = Tile.RenderState->GetPaddedSizeInTiles().Y * GPreviewLightmapVirtualTileSize * 1.0f / (1 << Tile.VirtualCoordinates.MipLevel) / GPreviewLightmapPhysicalTileSize;
							float BiasX = (1.0f * (-Tile.VirtualCoordinates.Position.X * GPreviewLightmapVirtualTileSize) - (-GPreviewLightmapTileBorderSize)) / GPreviewLightmapPhysicalTileSize;
							float BiasY = (1.0f * (-Tile.VirtualCoordinates.Position.Y * GPreviewLightmapVirtualTileSize) - (-GPreviewLightmapTileBorderSize)) / GPreviewLightmapPhysicalTileSize;

							FVector4 VirtualTexturePhysicalTileCoordinateScaleAndBias = FVector4(ScaleX, ScaleY, BiasX, BiasY);

							FLightmapGBufferParams LightmapGBufferParameters{};
							LightmapGBufferParameters.RenderPassIndex = LightSampleIndexArray[TileIndex];
							LightmapGBufferParameters.VirtualTexturePhysicalTileCoordinateScaleAndBias = VirtualTexturePhysicalTileCoordinateScaleAndBias;
							LightmapGBufferParameters.ScratchTilePoolOffset = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
							LightmapGBufferParameters.ScratchTilePoolLayer0 = ScratchTilePoolGPU->PooledRenderTargets[0]->GetRenderTargetItem().UAV;
							LightmapGBufferParameters.ScratchTilePoolLayer1 = ScratchTilePoolGPU->PooledRenderTargets[1]->GetRenderTargetItem().UAV;
							LightmapGBufferParameters.ScratchTilePoolLayer2 = ScratchTilePoolGPU->PooledRenderTargets[2]->GetRenderTargetItem().UAV;
							FLightmapGBufferUniformBufferRef PassUniformBuffer = FLightmapGBufferUniformBufferRef::CreateUniformBufferImmediate(LightmapGBufferParameters, UniformBuffer_SingleDraw);

							TArray<FMeshBatch> MeshBatches = Tile.RenderState->GeometryInstanceRef.GetMeshBatchesForGBufferRendering(Tile.VirtualCoordinates);

							for (auto& MeshBatch : MeshBatches)
							{
								FMeshBatchElement& Element = MeshBatch.Elements[0];

								Element.DynamicPrimitiveShaderDataIndex = Tile.RenderState->GeometryInstanceRef.GetElementId();
							}

							DrawDynamicMeshPass(
								*Scene->ReferenceView, RHICmdList,
								[View = Scene->ReferenceView.Get(), PassUniformBuffer, MeshBatches](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
							{
								FLightmapGBufferMeshProcessor MeshProcessor(nullptr, View, DynamicMeshPassContext, PassUniformBuffer);

								for (auto& MeshBatch : MeshBatches)
								{
									MeshProcessor.AddMeshBatch(MeshBatch, ~0ull, nullptr);
								}
							});

							GPrimitiveIdVertexBufferPool.DiscardAll();
						}

						RHICmdList.EndRenderPass();
					}

#if RHI_RAYTRACING
					if (IsRayTracingEnabled())
					{
						FGPUBatchedTileRequests GPUBatchedTileRequests;

						{
							for (int32 TileIndex = 0; TileIndex < PendingShadowTileRequests.Num(); TileIndex++)
							{
								const FLightmapTileRequest& Tile = PendingShadowTileRequests[TileIndex];
								FGPUTileDescription TileDesc;
								TileDesc.LightmapSize = Tile.RenderState->GetSize();
								TileDesc.VirtualTilePosition = Tile.VirtualCoordinates.Position * GPreviewLightmapVirtualTileSize;
								TileDesc.WorkingSetPosition = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * GPreviewLightmapPhysicalTileSize;
								TileDesc.ScratchPosition = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer0Position = Tile.OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer1Position = Tile.OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize;
								TileDesc.OutputLayer2Position = Tile.OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize;
								TileDesc.FrameIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision;
								TileDesc.RenderPassIndex = LightSampleIndexArray[TileIndex];
								GPUBatchedTileRequests.BatchedTilesDesc.Add(TileDesc);
							}

							{
								FRHIResourceCreateInfo CreateInfo;
								CreateInfo.ResourceArray = &GPUBatchedTileRequests.BatchedTilesDesc;

								GPUBatchedTileRequests.BatchedTilesBuffer = RHICreateStructuredBuffer(sizeof(FGPUTileDescription), GPUBatchedTileRequests.BatchedTilesDesc.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
								GPUBatchedTileRequests.BatchedTilesSRV = RHICreateShaderResourceView(GPUBatchedTileRequests.BatchedTilesBuffer);
							}
						}

						FIntPoint RayTracingResolution;
						RayTracingResolution.X = GPreviewLightmapPhysicalTileSize * GPUBatchedTileRequests.BatchedTilesDesc.Num();
						RayTracingResolution.Y = GPreviewLightmapPhysicalTileSize;

						FStationaryLightShadowTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStationaryLightShadowTracingRGS::FParameters>();
						PassParameters->TLAS = Scene->RayTracingScene->GetShaderResourceView();
						PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
						PassParameters->LightTypeArray = LightTypeSRV;
						PassParameters->ChannelIndexArray = ChannelIndexSRV;
						PassParameters->LightSampleIndexArray = LightSampleIndexSRV;
						PassParameters->LightShaderParametersArray = LightShaderParameterSRV;
						PassParameters->GBufferWorldPosition = GraphBuilder.CreateUAV(GBufferWorldPosition);
						PassParameters->GBufferWorldNormal = GraphBuilder.CreateUAV(GBufferWorldNormal);
						PassParameters->GBufferShadingNormal = GraphBuilder.CreateUAV(GBufferShadingNormal);
						PassParameters->ShadowMask = GraphBuilder.CreateUAV(ShadowMask);
						PassParameters->ShadowMaskSampleCount = GraphBuilder.CreateUAV(ShadowMaskSampleCount);

						auto RayGenerationShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FStationaryLightShadowTracingRGS>();
						ClearUnusedGraphResources(RayGenerationShader, PassParameters);

						GraphBuilder.AddPass(
							RDG_EVENT_NAME("StationaryLightShadowTracing %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
							PassParameters,
							ERDGPassFlags::Compute,
							[PassParameters, this, RayTracingScene = Scene->RayTracingScene, PipelineState = Scene->RayTracingPipelineState, RayGenerationShader, RayTracingResolution](FRHICommandList& RHICmdList)
						{
							FRayTracingShaderBindingsWriter GlobalResources;
							SetShaderParameters(GlobalResources, RayGenerationShader, *PassParameters);

							RHICmdList.RayTraceDispatch(PipelineState, RayGenerationShader.GetRayTracingShader(), RayTracingScene, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
						});

					}
#endif
					GraphBuilder.Execute();
				}
			}
		}
	}

	// Pull results from other GPUs using batched transfer if realtime
	if (!bInsideBackgroundTick)
	{
		TArray<FTransferTextureParams> Params;

		for (const FLightmapTileRequest& Tile : PendingTileRequests)
		{
			uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
			if (AssignedGPUIndex != 0)
			{
				auto TransferTexture = [&](int32 RenderTargetIndex) {
					FIntRect GPURect;
					GPURect.Min = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * LightmapTilePoolGPU.LayerFormatAndTileSize[RenderTargetIndex].TileSize;
					GPURect.Max = GPURect.Min + LightmapTilePoolGPU.LayerFormatAndTileSize[RenderTargetIndex].TileSize;
					FIntVector Min(GPURect.Min.X, GPURect.Min.Y, 0);
					FIntVector Max(GPURect.Max.X, GPURect.Max.Y, 1);

					Params.Add({ LightmapTilePoolGPU.PooledRenderTargets[RenderTargetIndex]->GetRenderTargetItem().TargetableTexture->GetTexture2D(), Min, Max, AssignedGPUIndex, 0, true });
				};

				TransferTexture(0);
				TransferTexture(1);
				TransferTexture(2);
				TransferTexture(3);
				TransferTexture(4);

				if (bUseFirstBounceRayGuiding)
				{
					TransferTexture(5);
					TransferTexture(6);
					TransferTexture(7);
					TransferTexture(8);
				}
			}
		}

		RHICmdList.TransferTextures(Params);
	}

	// Output from working set to VT layers
	{
		FGPUBatchedTileRequests GPUBatchedTileRequests;

		{
			for (const FLightmapTileRequest& Tile : PendingTileRequests)
			{
				FGPUTileDescription TileDesc;
				TileDesc.LightmapSize = Tile.RenderState->GetSize();
				TileDesc.VirtualTilePosition = Tile.VirtualCoordinates.Position * GPreviewLightmapVirtualTileSize;
				TileDesc.WorkingSetPosition = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * GPreviewLightmapPhysicalTileSize;
				TileDesc.ScratchPosition = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
				TileDesc.OutputLayer0Position = Tile.OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize;
				TileDesc.OutputLayer1Position = Tile.OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize;
				TileDesc.OutputLayer2Position = Tile.OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize;
				TileDesc.FrameIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision;
				TileDesc.RenderPassIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex;
				GPUBatchedTileRequests.BatchedTilesDesc.Add(TileDesc);
			}

			FRHIResourceCreateInfo CreateInfo;
			CreateInfo.ResourceArray = &GPUBatchedTileRequests.BatchedTilesDesc;

			GPUBatchedTileRequests.BatchedTilesBuffer = RHICreateStructuredBuffer(sizeof(FGPUTileDescription), GPUBatchedTileRequests.BatchedTilesDesc.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
			GPUBatchedTileRequests.BatchedTilesSRV = RHICreateShaderResourceView(GPUBatchedTileRequests.BatchedTilesBuffer);
		}

		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef IrradianceAndSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[0], TEXT("IrradianceAndSampleCount"));
			FRDGTextureRef SHDirectionality = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[1], TEXT("SHDirectionality"));
			FRDGTextureRef ShadowMask = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[2], TEXT("ShadowMask"));
			FRDGTextureRef ShadowMaskSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[3], TEXT("ShadowMaskSampleCount"));
			FRDGTextureRef SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[4], TEXT("SHCorrectionAndStationarySkyLightBentNormal"));

			FIntPoint RayTracingResolution;
			RayTracingResolution.X = GPreviewLightmapPhysicalTileSize * GPUBatchedTileRequests.BatchedTilesDesc.Num();
			RayTracingResolution.Y = GPreviewLightmapPhysicalTileSize;

			if (OutputRenderTargets[0] != nullptr || OutputRenderTargets[1] != nullptr)
			{
				FRDGTextureRef RenderTargetTileAtlas = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[0] != nullptr ? OutputRenderTargets[0] : OutputRenderTargets[1], TEXT("GPULightmassRenderTargetTileAtlas0"));

				FSelectiveLightmapOutputCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FSelectiveLightmapOutputCS::FOutputLayerDim>(0);
				PermutationVector.Set<FSelectiveLightmapOutputCS::FDrawProgressBars>(GGPULightmassShowProgressBars == 1);

				auto Shader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FSelectiveLightmapOutputCS>(PermutationVector);

				FSelectiveLightmapOutputCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSelectiveLightmapOutputCS::FParameters>();
				PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
				PassParameters->NumTotalSamples = GGPULightmassSamplesPerTexel;
				PassParameters->NumRayGuidingTrialSamples = NumFirstBounceRayGuidingTrialSamples;
				PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
				PassParameters->OutputTileAtlas = GraphBuilder.CreateUAV(RenderTargetTileAtlas);
				PassParameters->IrradianceAndSampleCount = GraphBuilder.CreateUAV(IrradianceAndSampleCount);
				PassParameters->SHDirectionality = GraphBuilder.CreateUAV(SHDirectionality);
				PassParameters->SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.CreateUAV(SHCorrectionAndStationarySkyLightBentNormal);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("SelectiveLightmapOutput 0"),
					Shader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(RayTracingResolution, FComputeShaderUtils::kGolden2DGroupSize));
			}

			if (OutputRenderTargets[2] != nullptr)
			{
				FRDGTextureRef RenderTargetTileAtlas = GraphBuilder.RegisterExternalTexture(OutputRenderTargets[2], TEXT("GPULightmassRenderTargetTileAtlas2"));

				FSelectiveLightmapOutputCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FSelectiveLightmapOutputCS::FOutputLayerDim>(2);
				PermutationVector.Set<FSelectiveLightmapOutputCS::FDrawProgressBars>(GGPULightmassShowProgressBars == 1);

				auto Shader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FSelectiveLightmapOutputCS>(PermutationVector);

				FSelectiveLightmapOutputCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSelectiveLightmapOutputCS::FParameters>();
				PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
				PassParameters->NumTotalSamples = GGPULightmassSamplesPerTexel;
				PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
				PassParameters->OutputTileAtlas = GraphBuilder.CreateUAV(RenderTargetTileAtlas);
				PassParameters->ShadowMask = GraphBuilder.CreateUAV(ShadowMask);
				PassParameters->ShadowMaskSampleCount = GraphBuilder.CreateUAV(ShadowMaskSampleCount);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("SelectiveLightmapOutput 2"),
					Shader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(RayTracingResolution, FComputeShaderUtils::kGolden2DGroupSize));
			}

			GraphBuilder.Execute();
		}
	}

	Scene->DestroyRayTracingScene();

	// Perform readback on any potential converged tiles
	{
		auto ConvergedTileRequests = PendingTileRequests.FilterByPredicate(
			[](const FLightmapTileRequest& TileRequest)
		{
			return
				TileRequest.VirtualCoordinates.MipLevel == 0 && // Only mip 0 tiles will be saved
				TileRequest.RenderState->IsTileFullyConverged(TileRequest.VirtualCoordinates);
		}
		);

		if (ConvergedTileRequests.Num() > 0)
		{
			int32 NewSize = FMath::CeilToInt(FMath::Sqrt(ConvergedTileRequests.Num()));

			for (const FLightmapTileRequest& Tile : ConvergedTileRequests)
			{
				Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).bHasReadbackInFlight = true;
			}

			for (uint32 GPUIndex = 0; GPUIndex < GNumExplicitGPUsForRendering; GPUIndex++)
			{
				SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::FromIndex(GPUIndex));

				auto ConvergedTileRequestsOnCurrentGPU = ConvergedTileRequests.FilterByPredicate(
					[GPUIndex](const FLightmapTileRequest& Tile)
				{
					uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
					return AssignedGPUIndex == GPUIndex;
				});

				if (ConvergedTileRequestsOnCurrentGPU.Num() == 0) continue;

				FLightmapReadbackGroup LightmapReadbackGroup;
				LightmapReadbackGroup.Revision = CurrentRevision;
				LightmapReadbackGroup.GPUIndex = GPUIndex;
				LightmapReadbackGroup.ConvergedTileRequests = ConvergedTileRequestsOnCurrentGPU;
				LightmapReadbackGroup.ReadbackTilePoolGPU = MakeUnique<FLightmapTilePoolGPU>(3, FIntPoint(NewSize, NewSize), FIntPoint(GPreviewLightmapPhysicalTileSize, GPreviewLightmapPhysicalTileSize));

				FGPUBatchedTileRequests GPUBatchedTileRequests;

				for (const auto& Tile : LightmapReadbackGroup.ConvergedTileRequests)
				{
					uint32 AssignedGPUIndex = (Tile.RenderState->DistributionPrefixSum + Tile.RenderState->RetrieveTileStateIndex(Tile.VirtualCoordinates)) % GNumExplicitGPUsForRendering;
					check(AssignedGPUIndex == GPUIndex);

					FGPUTileDescription TileDesc;
					TileDesc.LightmapSize = Tile.RenderState->GetSize();
					TileDesc.VirtualTilePosition = Tile.VirtualCoordinates.Position * GPreviewLightmapVirtualTileSize;
					TileDesc.WorkingSetPosition = LightmapTilePoolGPU.GetPositionFromLinearAddress(Tile.TileAddressInWorkingSet) * GPreviewLightmapPhysicalTileSize;
					TileDesc.ScratchPosition = ScratchTilePoolGPU->GetPositionFromLinearAddress(Tile.TileAddressInScratch) * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer0Position = Tile.OutputPhysicalCoordinates[0] * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer1Position = Tile.OutputPhysicalCoordinates[1] * GPreviewLightmapPhysicalTileSize;
					TileDesc.OutputLayer2Position = Tile.OutputPhysicalCoordinates[2] * GPreviewLightmapPhysicalTileSize;
					TileDesc.FrameIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).Revision;
					TileDesc.RenderPassIndex = Tile.RenderState->RetrieveTileState(Tile.VirtualCoordinates).RenderPassIndex;
					GPUBatchedTileRequests.BatchedTilesDesc.Add(TileDesc);
				}

				FRHIResourceCreateInfo CreateInfo;
				CreateInfo.ResourceArray = &GPUBatchedTileRequests.BatchedTilesDesc;

				GPUBatchedTileRequests.BatchedTilesBuffer = RHICreateStructuredBuffer(sizeof(FGPUTileDescription), GPUBatchedTileRequests.BatchedTilesDesc.GetResourceDataSize(), BUF_Static | BUF_ShaderResource, CreateInfo);
				GPUBatchedTileRequests.BatchedTilesSRV = RHICreateShaderResourceView(GPUBatchedTileRequests.BatchedTilesBuffer);


				FIntPoint DispatchResolution;
				DispatchResolution.X = GPreviewLightmapPhysicalTileSize * GPUBatchedTileRequests.BatchedTilesDesc.Num();
				DispatchResolution.Y = GPreviewLightmapPhysicalTileSize;

				FRDGBuilder GraphBuilder(RHICmdList);

				FRDGTextureRef IrradianceAndSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[0], TEXT("IrradianceAndSampleCount"));
				FRDGTextureRef SHDirectionality = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[1], TEXT("SHDirectionality"));
				FRDGTextureRef ShadowMask = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[2], TEXT("ShadowMask"));
				FRDGTextureRef ShadowMaskSampleCount = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[3], TEXT("ShadowMaskSampleCount"));
				FRDGTextureRef SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.RegisterExternalTexture(LightmapTilePoolGPU.PooledRenderTargets[4], TEXT("SHCorrectionAndStationarySkyLightBentNormal"));

				FRDGTextureRef StagingHQLayer0 = GraphBuilder.RegisterExternalTexture(LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[0], TEXT("StagingHQLayer0"));
				FRDGTextureRef StagingHQLayer1 = GraphBuilder.RegisterExternalTexture(LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[1], TEXT("StagingHQLayer1"));
				FRDGTextureRef StagingShadowMask = GraphBuilder.RegisterExternalTexture(LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[2], TEXT("StagingShadowMask"));

				{
					FCopyConvergedLightmapTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCopyConvergedLightmapTilesCS::FParameters>();

					PassParameters->NumBatchedTiles = GPUBatchedTileRequests.BatchedTilesDesc.Num();
					PassParameters->StagingPoolSizeX = LightmapReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.X;
					PassParameters->BatchedTiles = GPUBatchedTileRequests.BatchedTilesSRV;
					PassParameters->IrradianceAndSampleCount = GraphBuilder.CreateUAV(IrradianceAndSampleCount);
					PassParameters->SHDirectionality = GraphBuilder.CreateUAV(SHDirectionality);
					PassParameters->SHCorrectionAndStationarySkyLightBentNormal = GraphBuilder.CreateUAV(SHCorrectionAndStationarySkyLightBentNormal);
					PassParameters->ShadowMask = GraphBuilder.CreateUAV(ShadowMask);
					PassParameters->ShadowMaskSampleCount = GraphBuilder.CreateUAV(ShadowMaskSampleCount);
					PassParameters->StagingHQLayer0 = GraphBuilder.CreateUAV(StagingHQLayer0);
					PassParameters->StagingHQLayer1 = GraphBuilder.CreateUAV(StagingHQLayer1);
					PassParameters->StagingShadowMask = GraphBuilder.CreateUAV(StagingShadowMask);

					TShaderMapRef<FCopyConvergedLightmapTilesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CopyConvergedLightmapTiles"),
						ComputeShader,
						PassParameters,
						FComputeShaderUtils::GetGroupCount(DispatchResolution, FComputeShaderUtils::kGolden2DGroupSize));
				}

				GraphBuilder.Execute();

				LightmapReadbackGroup.StagingHQLayer0Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("StagingHQLayer0Readback"));
				LightmapReadbackGroup.StagingHQLayer1Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("StagingHQLayer1Readback"));
				LightmapReadbackGroup.StagingShadowMaskReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("StagingShadowMaskReadback"));
				LightmapReadbackGroup.StagingHQLayer0Readback->EnqueueCopy(RHICmdList, LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[0]->GetRenderTargetItem().TargetableTexture);
				LightmapReadbackGroup.StagingHQLayer1Readback->EnqueueCopy(RHICmdList, LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[1]->GetRenderTargetItem().TargetableTexture);
				LightmapReadbackGroup.StagingShadowMaskReadback->EnqueueCopy(RHICmdList, LightmapReadbackGroup.ReadbackTilePoolGPU->PooledRenderTargets[2]->GetRenderTargetItem().TargetableTexture);

				OngoingReadbacks.Emplace(MoveTemp(LightmapReadbackGroup));
			}
		}
	}

	PendingTileRequests.Empty();

	FrameNumber++;
}

void FLightmapRenderer::BackgroundTick()
{
	TArray<FLightmapReadbackGroup> FilteredReadbackGroups;

	for (int32 Index = 0; Index < OngoingReadbacks.Num(); Index++)
	{
		FLightmapReadbackGroup& ReadbackGroup = OngoingReadbacks[Index];
		if (ReadbackGroup.Revision != CurrentRevision)
		{
			continue;
		}

		if (ReadbackGroup.StagingHQLayer0Readback->IsReady(FRHIGPUMask::FromIndex(ReadbackGroup.GPUIndex)) && ReadbackGroup.StagingHQLayer1Readback->IsReady(FRHIGPUMask::FromIndex(ReadbackGroup.GPUIndex)) && ReadbackGroup.StagingShadowMaskReadback->IsReady(FRHIGPUMask::FromIndex(ReadbackGroup.GPUIndex)))
		{
			FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
			SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::FromIndex(ReadbackGroup.GPUIndex));

			// FLinearColor is in RGBA while the GPU texture is in ABGR
			// TODO: apply swizzling in the copy compute shader if this becomes a problem
			void* LockedData[3];
			int32 RowPitchInPixels[3];
			ReadbackGroup.StagingHQLayer0Readback->LockTexture(RHICmdList, LockedData[0], RowPitchInPixels[0]); // This forces a GPU stall
			ReadbackGroup.StagingHQLayer1Readback->LockTexture(RHICmdList, LockedData[1], RowPitchInPixels[1]); // This forces a GPU stall
			ReadbackGroup.StagingShadowMaskReadback->LockTexture(RHICmdList, LockedData[2], RowPitchInPixels[2]); // This forces a GPU stall

			TArray<FLinearColor> Texture[3];
			Texture[0].AddZeroed(ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[0]);
			Texture[1].AddZeroed(ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[1]);
			Texture[2].AddZeroed(ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[2]);
			FMemory::Memcpy(Texture[0].GetData(), LockedData[0], ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[0] * sizeof(FLinearColor));
			FMemory::Memcpy(Texture[1].GetData(), LockedData[1], ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[1] * sizeof(FLinearColor));
			FMemory::Memcpy(Texture[2].GetData(), LockedData[2], ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.Y * GPreviewLightmapPhysicalTileSize * RowPitchInPixels[2] * sizeof(FLinearColor));

			ReadbackGroup.StagingHQLayer0Readback->Unlock();
			ReadbackGroup.StagingHQLayer1Readback->Unlock();
			ReadbackGroup.StagingShadowMaskReadback->Unlock();

			ParallelFor(ReadbackGroup.ConvergedTileRequests.Num(), [&](int32 TileIndex)
			{
				FIntPoint SrcTilePosition(TileIndex % ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.X, TileIndex / ReadbackGroup.ReadbackTilePoolGPU->SizeInTiles.X);
				FIntPoint DstTilePosition(ReadbackGroup.ConvergedTileRequests[TileIndex].VirtualCoordinates.Position);

				check(RowPitchInPixels[0] == RowPitchInPixels[1]);
				const int32 SrcRowPitchInPixels = RowPitchInPixels[0];
				const int32 DstRowPitchInPixels = ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->GetPaddedPhysicalSize().X;

				for (int32 Y = 0; Y < GPreviewLightmapPhysicalTileSize; Y++)
				{
					for (int32 X = 0; X < GPreviewLightmapPhysicalTileSize; X++)
					{
						FIntPoint SrcPixelPosition = SrcTilePosition * GPreviewLightmapPhysicalTileSize + FIntPoint(X, Y);
						FIntPoint DstPixelPosition = DstTilePosition * GPreviewLightmapPhysicalTileSize + FIntPoint(X, Y);

						int32 SrcLinearIndex = SrcPixelPosition.Y * SrcRowPitchInPixels + SrcPixelPosition.X;
						int32 DstLinearIndex = DstPixelPosition.Y * DstRowPitchInPixels + DstPixelPosition.X;

						ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->CPUTextureData[0][DstLinearIndex] = Texture[0][SrcLinearIndex];
						ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->CPUTextureData[1][DstLinearIndex] = Texture[1][SrcLinearIndex];
						ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->CPUTextureData[2][DstLinearIndex] = Texture[2][SrcLinearIndex];
					}
				}

				ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->RetrieveTileState(ReadbackGroup.ConvergedTileRequests[TileIndex].VirtualCoordinates).CPURevision = CurrentRevision;
				ReadbackGroup.ConvergedTileRequests[TileIndex].RenderState->RetrieveTileState(ReadbackGroup.ConvergedTileRequests[TileIndex].VirtualCoordinates).bHasReadbackInFlight = false;
			});

			continue;
		}

		FilteredReadbackGroups.Emplace(MoveTemp(ReadbackGroup));
	}

	OngoingReadbacks = MoveTemp(FilteredReadbackGroups);

	bool bLastFewFramesIdle = !GCurrentLevelEditingViewportClient || !GCurrentLevelEditingViewportClient->IsRealtime();

	if (bLastFewFramesIdle && !bWasRunningAtFullSpeed)
	{
		bWasRunningAtFullSpeed = true;
		UE_LOG(LogGPULightmass, Log, TEXT("GPULightmass is now running at full speed"));
	}

	if (!bLastFewFramesIdle && bWasRunningAtFullSpeed)
	{
		bWasRunningAtFullSpeed = false;
		UE_LOG(LogGPULightmass, Log, TEXT("GPULightmass is now throttled for realtime preview"));
	}

	const int32 NumWorkPerFrame = !bLastFewFramesIdle ? 32 : 512;

	if (Mip0WorkDoneLastFrame < NumWorkPerFrame)
	{
		int32 PoolSize = FMath::CeilToInt(FMath::Sqrt(NumWorkPerFrame * 3));

		FIntPoint TextureSize(PoolSize * GPreviewLightmapPhysicalTileSize, PoolSize * GPreviewLightmapPhysicalTileSize);

		EPixelFormat RenderTargetFormat = PF_A32B32G32R32F;

		TRefCountPtr<IPooledRenderTarget> OutputTileAtlas;

		const FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
			TextureSize,
			RenderTargetFormat,
			FClearValueBinding::None,
			TexCreate_None,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV,
			false);

		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, OutputTileAtlas, *FString::Printf(TEXT("BackgroundTilePoolGPU")));

		ensure(OutputTileAtlas.IsValid());

		int32 WorkToGenerate = NumWorkPerFrame - Mip0WorkDoneLastFrame;
		int32 WorkGenerated = 0;
		int32 TileLinearIndexInAtlas = 0;

		TArray<FString> SelectedLightmapNames;

		for (FLightmapRenderState& Lightmap : Scene->LightmapRenderStates.Elements)
		{
			bool bAnyTileSelected = false;

			for (int32 Y = 0; Y < Lightmap.GetPaddedSizeInTiles().Y; Y++)
			{
				for (int32 X = 0; X < Lightmap.GetPaddedSizeInTiles().X; X++)
				{
					FTileVirtualCoordinates VirtualCoordinates(FIntPoint(X, Y), 0);

					if (!Lightmap.DoesTileHaveValidCPUData(VirtualCoordinates, CurrentRevision) && !Lightmap.RetrieveTileState(VirtualCoordinates).bHasReadbackInFlight)
					{
						bAnyTileSelected = true;

						FVTProduceTargetLayer TargetLayers[3];
						TargetLayers[0].pPageLocation = FIntVector(TileLinearIndexInAtlas % PoolSize, TileLinearIndexInAtlas / PoolSize, 0);
						TargetLayers[0].PooledRenderTarget = OutputTileAtlas;
						TileLinearIndexInAtlas++;
						TargetLayers[1].pPageLocation = FIntVector(TileLinearIndexInAtlas % PoolSize, TileLinearIndexInAtlas / PoolSize, 0);
						TargetLayers[1].PooledRenderTarget = OutputTileAtlas;
						TileLinearIndexInAtlas++;
						TargetLayers[2].pPageLocation = FIntVector(TileLinearIndexInAtlas % PoolSize, TileLinearIndexInAtlas / PoolSize, 0);
						TargetLayers[2].PooledRenderTarget = OutputTileAtlas;
						TileLinearIndexInAtlas++;
						check(TileLinearIndexInAtlas <= PoolSize * PoolSize);

						Lightmap.LightmapPreviewVirtualTexture->ProducePageData(
							RHICmdList,
							ERHIFeatureLevel::SM5,
							EVTProducePageFlags::None,
							FVirtualTextureProducerHandle(),
							0b111,
							0,
							FMath::MortonCode2(X) | (FMath::MortonCode2(Y) << 1),
							0,
							TargetLayers);

						WorkGenerated++;

						if (WorkGenerated >= WorkToGenerate)
						{
							break;
						}
					}
				}

				if (WorkGenerated >= WorkToGenerate)
				{
					break;
				}
			}

			if (bAnyTileSelected)
			{
				SelectedLightmapNames.Add(Lightmap.Name);
			}

			if (WorkGenerated >= WorkToGenerate)
			{
				break;
			}
		}

		if (bLastFewFramesIdle && FrameNumber % 100 == 0)
		{
			FString AllNames;
			for (FString& Name : SelectedLightmapNames)
			{
				AllNames += Name.RightChop(FString(TEXT("Lightmap_")).Len()) + TEXT(" ");
			}
			UE_LOG(LogGPULightmass, Log, TEXT("Working on: %s"), *AllNames);
		}

		bInsideBackgroundTick = true;

		// Render lightmap tiles
		Finalize(RHICmdList);

		bInsideBackgroundTick = false;

		if (bLastFewFramesIdle) // Indicates that the viewport is non-realtime
		{
			// Purge resources when 'realtime' is not checked on editor viewport to avoid leak & slowing down
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		}
	}

	Mip0WorkDoneLastFrame = 0;
}

}
