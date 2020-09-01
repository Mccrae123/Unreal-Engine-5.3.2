// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	HairRendering.h: Hair rendering implementation.
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "Shader.h"
#include "HairStrandsUtils.h"
#include "HairStrandsCluster.h"
#include "HairStrandsClusters.h"
#include "HairStrandsLUT.h"
#include "HairStrandsDeepShadow.h"
#include "HairStrandsVoxelization.h"
#include "HairStrandsVisibility.h"
#include "HairStrandsTransmittance.h"
#include "HairStrandsEnvironment.h"
#include "HairStrandsComposition.h"
#include "HairStrandsDebug.h"
#include "HairStrandsInterface.h"

/// Hold all the hair strands data
struct FHairStrandsRenderingData
{
	FHairStrandsVisibilityViews HairVisibilityViews;
	FHairStrandsMacroGroupViews MacroGroupsPerViews;
	FHairStrandsDebugData DebugData;
};

void RenderHairPrePass(
	FRHICommandListImmediate& RHICmdList,
	FScene* Scene,
	TArray<FViewInfo>& Views,
	FHairStrandsRenderingData& OutHairDatas);

void RenderHairBasePass(
	FRHICommandListImmediate& RHICmdList,
	FScene* Scene,
	FSceneRenderTargets& SceneContext,
	TArray<FViewInfo>& Views,
	FHairStrandsRenderingData& OutHairDatas);

void RunHairStrandsBookmark(
	FRHICommandListImmediate& RHICmdList, 
	EHairStrandsBookmark Bookmark, 
	FHairStrandsBookmarkParameters& Parameters);

FHairStrandsBookmarkParameters CreateHairStrandsBookmarkParameters(FViewInfo& View);