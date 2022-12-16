// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RayTracingInstance.cpp: Helper functions for creating a ray tracing instance.
=============================================================================*/

#include "RayTracingInstance.h"

#if RHI_RAYTRACING

#include "MaterialShared.h"
#include "RayTracing/RayTracingInstanceMask.h"
#include "MeshPassProcessor.h"


void FRayTracingInstance::BuildInstanceMaskAndFlags(ERHIFeatureLevel::Type FeatureLevel)
{
	FSceneProxyRayTracingMaskInfo MaskInfo;
	MaskInfo.MaskMode = ERayTracingViewMaskMode::RayTracing; // Deprecated function only supports RayTracing

	uint8 ExtraMask = bThinGeometry ? ComputeRayTracingInstanceMask(ERayTracingInstanceMaskType::HairStrands, MaskInfo.MaskMode) : 0;

	TArrayView<const FMeshBatch> MeshBatches = GetMaterials();
	FRayTracingMaskAndFlags MaskAndFlags = BuildRayTracingInstanceMaskAndFlags(
		MeshBatches, FeatureLevel, MaskInfo, InstanceLayer, ExtraMask);

	Mask = MaskAndFlags.Mask;
	bForceOpaque = bForceOpaque || MaskAndFlags.bForceOpaque;
	bDoubleSided = bDoubleSided || MaskAndFlags.bDoubleSided;
}

FRayTracingMaskAndFlags BuildRayTracingInstanceMaskAndFlags(TArrayView<const FMeshBatch> MeshBatches, ERHIFeatureLevel::Type FeatureLevel, ERayTracingInstanceLayer InstanceLayer, uint8 ExtraMask)
{
	FSceneProxyRayTracingMaskInfo MaskInfo;
	MaskInfo.MaskMode = ERayTracingViewMaskMode::RayTracing; // Deprecated function only supports RayTracing

	return BuildRayTracingInstanceMaskAndFlags(
		MeshBatches,
		FeatureLevel,
		MaskInfo,
		InstanceLayer,
		ExtraMask);
}	

uint8 ComputeBlendModeMask(const EBlendMode BlendMode)
{
	return BlendModeToRayTracingInstanceMask(BlendMode, ERayTracingViewMaskMode::RayTracing);
}

#endif
