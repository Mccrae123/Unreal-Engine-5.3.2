// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "BlueNoise.h"
#include "LumenRadianceCache.h"
#include "SceneTextureParameters.h"
#include "IndirectLightRendering.h"
#include "ScreenSpaceRayTracing.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "LumenSceneUtils.h"

extern int32 GLumenScreenProbeGatherNumMips;
extern int32 GLumenScreenProbeSpatialFilterScatter;

namespace LumenScreenProbeGather 
{
	extern int32 GetTracingOctahedronResolution();
	extern bool UseImportanceSampling();
	extern bool UseProbeSpatialFilter();
	extern bool UseRadianceCache(const FViewInfo& View);
}

// Must match SetupAdaptiveProbeIndirectArgsCS in usf
enum class EScreenProbeIndirectArgs
{
	GroupPerProbe,
	ThreadPerProbe,
	ThreadPerTrace,
	ThreadPerGather,
	ThreadPerGatherWithBorder,
	Max
};

BEGIN_SHADER_PARAMETER_STRUCT(FScreenProbeImportanceSamplingParameters, )
	SHADER_PARAMETER(uint32, MaxImportanceSamplingOctahedronResolution)
	SHADER_PARAMETER(uint32, ScreenProbeBRDFOctahedronResolution)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, StructuredImportanceSampledRayInfosForTracing)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint2>, StructuredImportanceSampledRayCoordForComposite)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FOctahedralSolidAngleParameters, )
	SHADER_PARAMETER(float, InvOctahedralSolidAngleTextureResolutionSq)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, OctahedralSolidAngleTexture)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FScreenProbeParameters, )
	SHADER_PARAMETER(uint32, ScreenProbeTracingOctahedronResolution)
	SHADER_PARAMETER(uint32, ScreenProbeGatherOctahedronResolution)
	SHADER_PARAMETER(uint32, ScreenProbeGatherOctahedronResolutionWithBorder)
	SHADER_PARAMETER(uint32, ScreenProbeDownsampleFactor)
	SHADER_PARAMETER(FIntPoint, ScreenProbeViewSize)
	SHADER_PARAMETER(FIntPoint, ScreenProbeAtlasViewSize)
	SHADER_PARAMETER(FIntPoint, ScreenProbeAtlasBufferSize)
	SHADER_PARAMETER(FIntPoint, ScreenProbeTraceBufferSize)
	SHADER_PARAMETER(FIntPoint, ScreenProbeGatherBufferSize)
	SHADER_PARAMETER(float, ScreenProbeGatherMaxMip)
	SHADER_PARAMETER(uint32, AdaptiveScreenTileSampleResolution)
	SHADER_PARAMETER(uint32, NumUniformScreenProbes)
	SHADER_PARAMETER(uint32, MaxNumAdaptiveProbes)

	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, NumAdaptiveScreenProbes)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, AdaptiveScreenProbeData)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenTileAdaptiveProbeHeader)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenTileAdaptiveProbeIndices)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceRadiance)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceHit)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DownsampledDepth)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWTraceRadiance)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWTraceHit)

	SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeImportanceSamplingParameters, ImportanceSampling)
	SHADER_PARAMETER_STRUCT_INCLUDE(FOctahedralSolidAngleParameters, OctahedralSolidAngleParameters)
	SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)

	SHADER_PARAMETER_RDG_BUFFER(Buffer<uint>, ProbeIndirectArgs)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FScreenProbeGatherParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenProbeRadiance)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ScreenProbeRadianceWithBorder)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float3>, ScreenProbeRadianceSHAmbient)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float3>, ScreenProbeRadianceSHDirectional)
END_SHADER_PARAMETER_STRUCT()

extern void GenerateImportanceSamplingRays(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const LumenRadianceCache::FRadianceCacheParameters& RadianceCacheParameters,
	FScreenProbeParameters& ScreenProbeParameters);

extern void TraceScreenProbes(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	bool bEnableSSGI,
	bool bTraceCards,
	const FSceneTextureParameters& SceneTextures,
	const ScreenSpaceRayTracing::FPrevSceneColorMip& PrevSceneColor,
	const FLumenCardTracingInputs& TracingInputs,
	const LumenRadianceCache::FRadianceCacheParameters& RadianceCacheParameters,
	FScreenProbeParameters& ScreenProbeParameters,
	FLumenMeshSDFGridParameters& MeshSDFGridParameters);

extern void FilterScreenProbes(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FScreenProbeParameters& ScreenProbeParameters,
	FScreenProbeGatherParameters& GatherParameters);

BEGIN_SHADER_PARAMETER_STRUCT(FScreenSpaceBentNormalParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float3>, ScreenBentNormal)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float3>, ScreenDiffuseLighting)
	SHADER_PARAMETER(uint32, UseScreenBentNormal)
END_SHADER_PARAMETER_STRUCT()

extern FScreenSpaceBentNormalParameters ComputeScreenSpaceBentNormal(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FScreenProbeParameters& ScreenProbeParameters);