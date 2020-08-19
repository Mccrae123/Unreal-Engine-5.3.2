// Copyright Epic Games, Inc. All Rights Reserved.
/*=============================================================================
	VirtualShadowMapArray.h:
=============================================================================*/
#pragma once

#include "../Nanite/NaniteRender.h"

struct FSortedLightSetSceneInfo;
class FViewInfo;
class FProjectedShadowInfo;
class FVisibleLightInfo;
class FVirtualShadowMapCacheEntry;
class FVirtualShadowMapArrayCacheManager;
struct FSortedLightSetSceneInfo;

// TODO: does this exist?
constexpr uint32 ILog2Const(uint32 n)
{
	return (n > 1) ? 1 + ILog2Const(n / 2) : 0;
}


class FVirtualShadowMap
{
public:
	// PageSize * Level0DimPagesXY defines the virtual address space, e.g., 128x128 = 16k

	// 32x512 = 16k
	//static constexpr uint32 PageSize = 32U;
	//static constexpr uint32 Level0DimPagesXY = 512U;

	// 128x128 = 16k
	static constexpr uint32 PageSize = 128U;
	static constexpr uint32 Level0DimPagesXY = 128U;

	// With 128x128 pages, a 4k texture holds 1024 physical pages
	static constexpr uint32 PhysicalPagePoolTexureSizeX = 4096U;
	static constexpr uint32 PhysicalPagePoolTexureSizeY = 4096U;

	static constexpr uint32 PageSizeMask = PageSize - 1U;
	static constexpr uint32 Log2PageSize = ILog2Const(PageSize);
	static constexpr uint32 Log2Level0DimPagesXY = ILog2Const(Level0DimPagesXY);
	static constexpr uint32 MaxMipLevels = Log2Level0DimPagesXY + 1U;

	static constexpr uint32 VirtualMaxResolutionXY = Level0DimPagesXY * PageSize;
	
	static constexpr uint32 PhysicalPageAddressBits = 16U;
	static constexpr uint32 MaxPhysicalTextureDimPages = 1U << PhysicalPageAddressBits;
	static constexpr uint32 MaxPhysicalTextureDimTexels = MaxPhysicalTextureDimPages * PageSize;

	static constexpr uint32 RasterWindowPages = 4u;
	
	
	// Something large (we're using ints at the moment...)
	// TODO: Fix this when tweaking data sizes of page table entries to e.g., 2x8 bits
	static constexpr uint32 InvalidPhysicalPageAddress = 65535U; 

	FVirtualShadowMap(uint32 InID) : ID(InID)
	{
	}

	uint32 ID = INDEX_NONE;
	TSharedPtr<FVirtualShadowMapCacheEntry> VirtualShadowMapCacheEntry;
};

// Useful data for both the page mapping shader and the projection shader
// as well as cached shadow maps
struct FVirtualShadowMapProjectionShaderData
{
	/**
	 * Transform from shadow-pre-translated world space to shadow view space, example use: (WorldSpacePos + ShadowPreViewTranslation) * TranslatedWorldToShadowViewMatrix
	 * TODO: Why don't we call it a rotation and store in a 3x3? Does it ever have translation in?
	 */
	FMatrix TranslatedWorldToShadowViewMatrix;
	FMatrix ShadowViewToClipMatrix;
	FMatrix TranslatedWorldToShadowUvNormalMatrix;
	/**
	 * Translation from world space to shadow space (add before transform by TranslatedWorldToShadowViewMatrix).
	 */
	FVector4 ShadowPreViewTranslation;
	uint32 VirtualShadowMapId;
	
	// These could be per-light (first/count), but convenient here and not much overhead
	int32 ClipmapLevel = 0;					// "Absolute" level, can be negative
	int32 ClipmapLevelCount = 0;
	float ClipmapResolutionLodBias = 0.0f;

	float MinSceneDepth = -HALF_WORLD_MAX;
	float MaxSceneDepth =  HALF_WORLD_MAX;

	// Seems the FMatrix forces 16-byte alignment
	float Padding[2];
};
static_assert((sizeof(FVirtualShadowMapProjectionShaderData) % 16) == 0, "FVirtualShadowMapProjectionShaderData size should be a multiple of 16-bytes for alignment.");

FVirtualShadowMapProjectionShaderData GetVirtualShadowMapProjectionShaderData(const FProjectedShadowInfo* ShadowInfo);

FMatrix CalcTranslatedWorldToShadowUvNormalMatrix(const FMatrix& TranslatedWorldToShadowView, const FMatrix& ViewToClip);

BEGIN_SHADER_PARAMETER_STRUCT(FVirtualShadowMapCommonParameters, )
	SHADER_PARAMETER_ARRAY(uint32, LevelOffsets, [FVirtualShadowMap::MaxMipLevels])
	SHADER_PARAMETER_ARRAY(uint32, HPageFlagLevelOffsets, [FVirtualShadowMap::MaxMipLevels])
	SHADER_PARAMETER(uint32, PageTableSize)
	SHADER_PARAMETER(uint32, HPageTableSize)
	SHADER_PARAMETER(uint32, NumShadowMaps)

	SHADER_PARAMETER(uint32, MaxPhysicalPages)
	// use to map linear index to x,y page coord
	SHADER_PARAMETER(uint32, PhysicalPageRowMask)
	SHADER_PARAMETER(uint32, PhysicalPageRowShift)
END_SHADER_PARAMETER_STRUCT()

class FVirtualShadowMapArray
{
public:
	FVirtualShadowMapCommonParameters CommonParameters;
	
	FVirtualShadowMapArray();
	~FVirtualShadowMapArray();

	FVirtualShadowMap *Allocate()
	{
		FVirtualShadowMap *SM = new(FMemStack::Get(), 1, 16) FVirtualShadowMap(ShadowMaps.Num());
		ShadowMaps.Add(SM);

		return SM;
	}

	FIntPoint GetPhysicalPoolSize() const
	{
		FIntPoint PhysicalPoolSize(FVirtualShadowMap::PhysicalPagePoolTexureSizeX, FVirtualShadowMap::PhysicalPagePoolTexureSizeY);
		return PhysicalPoolSize;
	}

	static void SetShaderDefines(FShaderCompilerEnvironment& OutEnvironment);

	//
	void GenerateIdentityPageTables(FRDGBuilder& GraphBuilder, uint32 MipLevel = INDEX_NONE);
	void ClearPhysicalMemory(FRDGBuilder& GraphBuilder, FRDGTextureRef& PhysicalTexture, FVirtualShadowMapArrayCacheManager *VirtualShadowMapArrayCacheManager);
	void MarkPhysicalPagesRendered(FRDGBuilder& GraphBuilder, const TArray<uint32, SceneRenderingAllocator> &VirtualShadowMapFlags);

	//
	void GeneratePageFlagsFromLightGrid(FRDGBuilder& GraphBuilder, 
		const TArray<FViewInfo> &Views, 
		const FSortedLightSetSceneInfo& SortedLights, 
		const TArray<FVisibleLightInfo, SceneRenderingAllocator> &VisibleLightInfos, 
		const TArray<Nanite::FRasterResults, TInlineAllocator<2>> &NaniteRasterResults, 
		bool bPostBasePass, 
		FVirtualShadowMapArrayCacheManager *VirtualShadowMapArrayCacheManager);

	// Draw debug info into render target 'VirtSmDebug' of screen-size, the mode is controlled by 'r.Shadow.v.DebugVisualize' (defaults to not doing aught). 
	void RenderDebugInfo(FRDGBuilder& GraphBuilder, FVirtualShadowMapArrayCacheManager *VirtualShadowMapArrayCacheManager);
	// 
	void PrintStats(FRDGBuilder& GraphBuilder, const FViewInfo& View);

	TArray<FVirtualShadowMap*, SceneRenderingAllocator> ShadowMaps;

	// Large physical texture of depth format, say 4096^2 or whatever we think is enough texels to go around
	TRefCountPtr<IPooledRenderTarget>	PhysicalPagePool;
	// Buffer that serves as the page table for all virtual shadow maps
	TRefCountPtr<FPooledRDGBuffer>		PageTable;
	
	// Buffer that stores flags (uints) marking each page that needs to be rendered and cache status, for all virtual shadow maps.
	// Flag values defined in PageAccessCommon.ush: VSM_ALLOCATED_FLAG | VSM_INVALID_FLAG
	TRefCountPtr<FPooledRDGBuffer>		PageFlags;
	// HPageFlags is a hierarchy over the PageFlags for quick query
	TRefCountPtr<FPooledRDGBuffer>		HPageFlags;

	TRefCountPtr<IPooledRenderTarget>	HZBPhysical;
	TRefCountPtr<FPooledRDGBuffer>		HZBPageTable;

	TRefCountPtr<IPooledRenderTarget> DebugVisualizationOutput;
	TRefCountPtr<IPooledRenderTarget> DebugVisualizationProjectionOutput;
	
	TRefCountPtr<FPooledRDGBuffer>		AllocatedPagesOffset;

	static constexpr uint32 NumStats = 5;
	// 0 - allocated pages
	// 1 - re-usable pages
	// 2 - Touched by dynamic
	// 3 - NumSms
	// 4 - RandRobin invalidated

	TRefCountPtr<FPooledRDGBuffer>		StatsBufferRef;

	// Allocation info for each page.
	TRefCountPtr<FPooledRDGBuffer>		CachedPageInfos;
	TRefCountPtr<FPooledRDGBuffer>		PhysicalPageMetaData;

	// Buffer that stores flags marking each page that received dynamic geo.
	TRefCountPtr<FPooledRDGBuffer>		DynamicCasterPageFlags;
	
	// uint4 buffer with one rect for each mip level in all SMs, calculated to bound committed pages
	// Used to clip the rect size of clusters during culling.
	TRefCountPtr<FPooledRDGBuffer>		PageRectBounds;


	TRefCountPtr<FPooledRDGBuffer>		ShadowMapProjectionDataBuffer;
};
