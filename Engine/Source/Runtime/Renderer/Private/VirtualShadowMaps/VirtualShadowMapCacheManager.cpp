// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	VirtualShadowMap.h:
=============================================================================*/
#include "VirtualShadowMapCacheManager.h"
#include "RendererModule.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "ScenePrivate.h"
#include "HAL/FileManager.h"

#include "PrimitiveSceneInfo.h"

static TAutoConsoleVariable<int32> CVarAccumulateStats(
	TEXT("r.Shadow.v.AccumulateStats"),
	0,
	TEXT("AccumulateStats"),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<int32> CVarCacheVirtualSMs(
	TEXT("r.Shadow.v.Cache"),
	0,
	TEXT("Turn on to enable caching"),
	ECVF_RenderThreadSafe
);


void FVirtualShadowMapCacheEntry::UpdateClipmap(
	int32 VirtualShadowMapId,
	const FMatrix &WorldToLight,
	FIntPoint PageSpaceLocation,
	float GlobalDepth)
{
	// Swap previous frame data over.
	PrevVirtualShadowMapId = CurrentVirtualShadowMapId;
	PrevPageSpaceLocation = CurrentPageSpaceLocation;
	PrevShadowMapGlobalDepth = CurrentShadowMapGlobalDepth;
	
	if (WorldToLight != ClipmapCacheValidKey.WorldToLight)
	{
		PrevVirtualShadowMapId = INDEX_NONE;
		ClipmapCacheValidKey.WorldToLight = WorldToLight;
	}

	CurrentVirtualShadowMapId = VirtualShadowMapId;
	CurrentPageSpaceLocation = PageSpaceLocation;
	CurrentShadowMapGlobalDepth = GlobalDepth;
}

void FVirtualShadowMapCacheEntry::Update(int32 VirtualShadowMapId, const FWholeSceneProjectedShadowInitializer &InCacheValidKey)
{
	// Swap previous frame data over.
	PrevPageSpaceLocation = CurrentPageSpaceLocation;
	PrevVirtualShadowMapId = CurrentVirtualShadowMapId;
	PrevShadowMapGlobalDepth = CurrentShadowMapGlobalDepth;

	// Check cache validity based of shadow setup
	if (!CacheValidKey.IsCachedShadowValid(InCacheValidKey))
	{
		PrevVirtualShadowMapId = INDEX_NONE;
		//UE_LOG(LogRenderer, Display, TEXT("Invalidated!"));
	}

	CacheValidKey = InCacheValidKey;

	CurrentVirtualShadowMapId = VirtualShadowMapId;
	PrevPageSpaceLocation = CurrentPageSpaceLocation = FIntPoint(0, 0);
	PrevShadowMapGlobalDepth = CurrentShadowMapGlobalDepth = 0.0f;
}


TSharedPtr<FVirtualShadowMapCacheEntry> FVirtualShadowMapArrayCacheManager::FindCreateCacheEntry(int32 LightSceneId, int32 CascadeIndex)
{
	if (CVarCacheVirtualSMs.GetValueOnRenderThread() == 0)
	{
		return nullptr;
	}

	const FIntPoint Key(LightSceneId, CascadeIndex);

	if (TSharedPtr<FVirtualShadowMapCacheEntry> *VirtualShadowMapCacheEntry = CacheEntries.Find(Key))
	{
		return *VirtualShadowMapCacheEntry;
	}

	// Add to current frame / active set.
	TSharedPtr<FVirtualShadowMapCacheEntry> &NewVirtualShadowMapCacheEntry = CacheEntries.Add(Key);

	// Copy data if available
	if (TSharedPtr<FVirtualShadowMapCacheEntry> *PrevVirtualShadowMapCacheEntry = PrevCacheEntries.Find(Key))
	{
		NewVirtualShadowMapCacheEntry = *PrevVirtualShadowMapCacheEntry;
	}
	else
	{
		NewVirtualShadowMapCacheEntry = TSharedPtr<FVirtualShadowMapCacheEntry>(new FVirtualShadowMapCacheEntry);
	}

	// return entry
	return NewVirtualShadowMapCacheEntry;
}


class FVirtualSmCopyStatsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVirtualSmCopyStatsCS);
	SHADER_USE_PARAMETER_STRUCT(FVirtualSmCopyStatsCS, FGlobalShader)
public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, InStatsBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer< uint >, AccumulatedStatsBufferOut)
		SHADER_PARAMETER(uint32, NumStats)
	END_SHADER_PARAMETER_STRUCT()
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("MAX_STAT_FRAMES"), FVirtualShadowMapArrayCacheManager::MaxStatFrames);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVirtualSmCopyStatsCS, "/Engine/Private/VirtualShadowMaps/CopyStats.usf", "CopyStatsCS", SF_Compute);

void FVirtualShadowMapArrayCacheManager::ExtractFrameData(FVirtualShadowMapArray &VirtualShadowMapArray, FRDGBuilder& GraphBuilder)
{
	if (VirtualShadowMapArray.IsAllocated()
		&& CVarCacheVirtualSMs.GetValueOnRenderThread() != 0)
	{
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.PageTableRDG, &PrevPageTable);
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.PageFlagsRDG, &PrevPageFlags);
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.HPageFlagsRDG, &PrevHPageFlags);
		
		GraphBuilder.QueueTextureExtraction(VirtualShadowMapArray.PhysicalPagePoolRDG, &PrevPhysicalPagePool);
#if ENABLE_NON_NANITE_VSM
		GraphBuilder.QueueTextureExtraction(VirtualShadowMapArray.PhysicalPagePoolHw, &PrevPhysicalPagePoolHw);
#endif //ENABLE_NON_NANITE_VSM
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.PhysicalPageMetaDataRDG, &PrevPhysicalPageMetaData);
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.DynamicCasterPageFlagsRDG, &PrevDynamicCasterPageFlags);
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.ShadowMapProjectionDataRDG, &PrevShadowMapProjectionDataBuffer);
		GraphBuilder.QueueBufferExtraction(VirtualShadowMapArray.PageRectBoundsRDG, &PrevPageRectBounds);
		// Move cache entries to previous frame, this implicitly removes any that were not used
		PrevCacheEntries = CacheEntries;
		PrevCommonParameters = VirtualShadowMapArray.CommonParameters;
	}
	else
	{
		// Drop all refs.
		PrevPageTable = TRefCountPtr<FRDGPooledBuffer>();
		PrevPageFlags = TRefCountPtr<FRDGPooledBuffer>();
		PrevHPageFlags = TRefCountPtr<FRDGPooledBuffer>();

		PrevPhysicalPagePool = TRefCountPtr<IPooledRenderTarget>();
#if ENABLE_NON_NANITE_VSM
		PrevPhysicalPagePoolHw = TRefCountPtr<IPooledRenderTarget>();
#endif //ENABLE_NON_NANITE_VSM
		PrevPhysicalPageMetaData = TRefCountPtr<FRDGPooledBuffer>();
		PrevDynamicCasterPageFlags = TRefCountPtr<FRDGPooledBuffer>();
		PrevShadowMapProjectionDataBuffer = TRefCountPtr<FRDGPooledBuffer>();
		PrevPageRectBounds = TRefCountPtr<FRDGPooledBuffer>();

		PrevCommonParameters.NumShadowMaps = 0;

		PrevCacheEntries.Empty();
	}
	CacheEntries.Reset();

	FRDGBufferRef AccumulatedStatsBufferRDG = nullptr;

	// Note: stats accumulation thing is here because it needs to persist over frames.
	if (!AccumulatedStatsBuffer.IsValid())
	{
		AccumulatedStatsBufferRDG = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(4, 1 + FVirtualShadowMapArray::NumStats * MaxStatFrames), TEXT("AccumulatedStatsBuffer"));	// TODO: Can't be a structured buffer as EnqueueCopy is only defined for vertex buffers
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AccumulatedStatsBufferRDG, PF_R32_UINT), 0);
		ConvertToExternalBuffer(GraphBuilder, AccumulatedStatsBufferRDG, AccumulatedStatsBuffer);
	}
	else
	{
		AccumulatedStatsBufferRDG = GraphBuilder.RegisterExternalBuffer(AccumulatedStatsBuffer, TEXT("AccumulatedStatsBuffer"));
	}

	if (IsAccumulatingStats())
	{
		// Initialize/clear
		if (!bAccumulatingStats)
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AccumulatedStatsBufferRDG, PF_R32_UINT), 0);
			bAccumulatingStats = true;
		}

		FVirtualSmCopyStatsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVirtualSmCopyStatsCS::FParameters>();

		PassParameters->InStatsBuffer = GraphBuilder.CreateSRV(VirtualShadowMapArray.StatsBufferRDG, PF_R32_UINT);
		PassParameters->AccumulatedStatsBufferOut = GraphBuilder.CreateUAV(AccumulatedStatsBufferRDG, PF_R32_UINT);
		PassParameters->NumStats = FVirtualShadowMapArray::NumStats;

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FVirtualSmCopyStatsCS>();

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Copy Stats"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1)
		);
	}
	else if (bAccumulatingStats)
	{
		bAccumulatingStats = false;

		GPUBufferReadback = new FRHIGPUBufferReadback(TEXT("AccumulatedStatsBuffer"));
		AddEnqueueCopyPass(GraphBuilder, GPUBufferReadback, AccumulatedStatsBufferRDG, 0u);
	}

	if (GPUBufferReadback && GPUBufferReadback->IsReady())
	{
		TArray<uint32> Tmp;
		Tmp.AddDefaulted(1 + FVirtualShadowMapArray::NumStats * MaxStatFrames);

		{
			const uint32* BufferPtr = (const uint32*)GPUBufferReadback->Lock((1 + FVirtualShadowMapArray::NumStats * MaxStatFrames) * sizeof(uint32));
			FPlatformMemory::Memcpy(Tmp.GetData(), BufferPtr, Tmp.Num() * Tmp.GetTypeSize());
			GPUBufferReadback->Unlock();

			delete GPUBufferReadback;
			GPUBufferReadback = nullptr;
		}

		FString FileName = TEXT("shadow_map_cache_stats.csv");// FString::Printf(TEXT("%s.csv"), *FileNameToUse);
		FArchive * FileToLogTo = IFileManager::Get().CreateFileWriter(*FileName, false);
		ensure(FileToLogTo);
		if (FileToLogTo)
		{
			static const FString StatNames[FVirtualShadowMapArray::NumStats] =
			{
				TEXT("Allocated"),
				TEXT("Cached"),
				TEXT("Dynamic"),
				TEXT("NumSms"),
				TEXT("RandRobin"),
			};


			// Print header
			FString StringToPrint;
			for (const FString &StatName : StatNames)
			{
				if (!StringToPrint.IsEmpty())
				{
					StringToPrint += TEXT(",");
				}

				StringToPrint += StatName;
			}

			StringToPrint += TEXT("\n");
			FileToLogTo->Serialize(TCHAR_TO_ANSI(*StringToPrint), StringToPrint.Len());

			uint32 Num = Tmp[0];
			for (uint32 Ind = 0; Ind < Num; ++Ind)
			{
				StringToPrint.Empty();

				for (uint32 StatInd = 0; StatInd < FVirtualShadowMapArray::NumStats; ++StatInd)
				{
					if (!StringToPrint.IsEmpty())
					{
						StringToPrint += TEXT(",");
					}

					StringToPrint += FString::Printf(TEXT("%d"), Tmp[1 + Ind * FVirtualShadowMapArray::NumStats + StatInd]);
				}

				StringToPrint += TEXT("\n");
				FileToLogTo->Serialize(TCHAR_TO_ANSI(*StringToPrint), StringToPrint.Len());
			}


			FileToLogTo->Close();
		}
	}
}

bool FVirtualShadowMapArrayCacheManager::IsValid()
{
	return CVarCacheVirtualSMs.GetValueOnRenderThread() != 0
		&& PrevPageTable
		&& PrevPageFlags
#if ENABLE_NON_NANITE_VSM
		&& (PrevPhysicalPagePool || PrevPhysicalPagePoolHw)
#else // !ENABLE_NON_NANITE_VSM
		&& PrevPhysicalPagePool
#endif //ENABLE_NON_NANITE_VSM
		&& PrevPhysicalPageMetaData
		&& PrevDynamicCasterPageFlags;
}


bool FVirtualShadowMapArrayCacheManager::IsAccumulatingStats()
{
	return CVarAccumulateStats.GetValueOnRenderThread() != 0;
}

void FVirtualShadowMapArrayCacheManager::ProcessRemovedPrimives(FRDGBuilder& GraphBuilder, const FGPUScene &GPUScene, const TArray<FPrimitiveSceneInfo*> &RemovedPrimitiveSceneInfos)
{
	if (CVarCacheVirtualSMs.GetValueOnRenderThread() != 0 && RemovedPrimitiveSceneInfos.Num() > 0 && PrevDynamicCasterPageFlags.IsValid())
	{
		// Note: Could filter out primitives that have no nanite here (though later this might be bad anyway, when regular geo is also rendered into virtual SMs)
		TArray<FInstanceDataRange> InstanceRangesLarge;
		TArray<FInstanceDataRange> InstanceRangesSmall;
		for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : RemovedPrimitiveSceneInfos)
		{
			if (PrimitiveSceneInfo->GetInstanceDataOffset() != INDEX_NONE)
			{
				int32 NumInstanceDataEntries = PrimitiveSceneInfo->GetNumInstanceDataEntries();
				if (NumInstanceDataEntries >= 8U)
				{
					InstanceRangesLarge.Add(FInstanceDataRange{ PrimitiveSceneInfo->GetInstanceDataOffset(), NumInstanceDataEntries });
				}
				else
				{
					InstanceRangesSmall.Add(FInstanceDataRange{ PrimitiveSceneInfo->GetInstanceDataOffset(), NumInstanceDataEntries });
				}
			}
		}
		ProcessInstanceRangeInvalidation(GraphBuilder, InstanceRangesLarge, InstanceRangesSmall, GPUScene);
	}
}


void FVirtualShadowMapArrayCacheManager::ProcessPrimitivesToUpdate(FRDGBuilder& GraphBuilder, const FScene &Scene)
{
	const FGPUScene& GPUScene = Scene.GPUScene;
	if (IsValid() && GPUScene.PrimitivesToUpdate.Num() > 0)
	{
		// TODO: As a slight CPU optimization just pass primitive ID list and use instance ranges stored in GPU scene
		TArray<FInstanceDataRange> InstanceRangesLarge;
		TArray<FInstanceDataRange> InstanceRangesSmall;
		for (const int32 PrimitiveId : GPUScene.PrimitivesToUpdate)
		{
			// Skip added ones (they dont need it, but must be marked as having moved).
			if (PrimitiveId < Scene.Primitives.Num() && (PrimitiveId >= GPUScene.AddedPrimitiveFlags.Num() || !GPUScene.AddedPrimitiveFlags[PrimitiveId]))
			{
				const FPrimitiveSceneInfo* PrimitiveSceneInfo = Scene.Primitives[PrimitiveId];
				if (PrimitiveSceneInfo->GetInstanceDataOffset() != INDEX_NONE)
				{
					int32 NumInstanceDataEntries = PrimitiveSceneInfo->GetNumInstanceDataEntries();
					if (NumInstanceDataEntries >= 8U)
					{
						InstanceRangesLarge.Add(FInstanceDataRange{ PrimitiveSceneInfo->GetInstanceDataOffset(), NumInstanceDataEntries });
					}
					else
					{
						InstanceRangesSmall.Add(FInstanceDataRange{ PrimitiveSceneInfo->GetInstanceDataOffset(), NumInstanceDataEntries });
					}
				}
			}
		}
		ProcessInstanceRangeInvalidation(GraphBuilder, InstanceRangesLarge, InstanceRangesSmall, GPUScene);
	}
}



/**
 * Compute shader to project and invalidate the rectangles of given instances.
 */
class FVirtualSmInvalidateInstancePagesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVirtualSmInvalidateInstancePagesCS);
	SHADER_USE_PARAMETER_STRUCT(FVirtualSmInvalidateInstancePagesCS, FGlobalShader)

	class FLargeSmallDim : SHADER_PERMUTATION_BOOL("PROCESS_LARGE_INSTANCE_COUNT_RANGES");
	using FPermutationDomain = TShaderPermutationDomain<FLargeSmallDim>;

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualShadowMapCommonParameters, VirtualSmCommon)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FInstanceDataRange >, InstanceRanges)
		SHADER_PARAMETER(uint32, NumRemovedItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< FVirtualShadowMapProjectionShaderData >, ShadowMapProjectionData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, PageFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint >, HPageFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer< uint4 >, PageRectBounds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer< uint >, OutDynamicCasterFlags)

		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUSceneInstanceSceneData)
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(uint32, GPUSceneFrameNumber)
		SHADER_PARAMETER(uint32, InstanceDataSOAStride)
		
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int Cs1dGroupSizeX = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
		OutEnvironment.SetDefine(TEXT("CS_1D_GROUP_SIZE_X"), Cs1dGroupSizeX);
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);

		// OutEnvironment.SetDefine(TEXT("MAX_STAT_FRAMES"), FVirtualShadowMapArrayCacheManager::MaxStatFrames);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVirtualSmInvalidateInstancePagesCS, "/Engine/Private/VirtualShadowMaps/CacheManagement.usf", "VirtualSmInvalidateInstancePagesCS", SF_Compute);


void FVirtualShadowMapArrayCacheManager::ProcessInstanceRangeInvalidation(FRDGBuilder& GraphBuilder, const TArray<FInstanceDataRange>& InstanceRangesLarge, const TArray<FInstanceDataRange>& InstanceRangesSmall, const FGPUScene& GPUScene)
{
	if (InstanceRangesSmall.Num())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "ProcessInstanceRangeInvalidation [%d small-ranges]", InstanceRangesSmall.Num());

		auto RegExtCreateSrv = [&GraphBuilder](const TRefCountPtr<FRDGPooledBuffer>& Buffer, const TCHAR* Name) -> FRDGBufferSRVRef
		{
			return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffer, Name));
		};

		FVirtualSmInvalidateInstancePagesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVirtualSmInvalidateInstancePagesCS::FParameters>();

		PassParameters->VirtualSmCommon = GraphBuilder.CreateUniformBuffer(&PrevCommonParameters);
		FRDGBufferRef InstanceRangesRDG = CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRangesSmall"), InstanceRangesSmall);
		PassParameters->InstanceRanges = GraphBuilder.CreateSRV(InstanceRangesRDG);
		PassParameters->NumRemovedItems = InstanceRangesSmall.Num();
		PassParameters->ShadowMapProjectionData = RegExtCreateSrv(PrevShadowMapProjectionDataBuffer, TEXT("PrevShadowMapProjectionData"));

		PassParameters->PageFlags = RegExtCreateSrv(PrevPageFlags, TEXT("PrevPageFlags"));
		PassParameters->HPageFlags = RegExtCreateSrv(PrevHPageFlags, TEXT("PrevHPageFlags"));
		PassParameters->PageRectBounds = RegExtCreateSrv(PrevPageRectBounds, TEXT("PrevPageRectBounds"));

		FRDGBufferRef DynamicCasterFlagsRDG = GraphBuilder.RegisterExternalBuffer(PrevDynamicCasterPageFlags, TEXT("DynamicCasterFlags"));
		PassParameters->OutDynamicCasterFlags = GraphBuilder.CreateUAV(DynamicCasterFlagsRDG);

		PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
		PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
		PassParameters->GPUSceneFrameNumber = GPUScene.GetSceneFrameNumber();
		PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;

		FVirtualSmInvalidateInstancePagesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVirtualSmInvalidateInstancePagesCS::FLargeSmallDim>(0);

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FVirtualSmInvalidateInstancePagesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VirtualSmInvalidateInstancePagesCS"),
			ComputeShader,
			PassParameters,
			FIntVector(FMath::DivideAndRoundUp(InstanceRangesSmall.Num(), FVirtualSmInvalidateInstancePagesCS::Cs1dGroupSizeX), 1, 1)
		);
	}
	if (InstanceRangesLarge.Num())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "ProcessInstanceRangeInvalidation [%d large-ranges]", InstanceRangesLarge.Num());

		auto RegExtCreateSrv = [&GraphBuilder](const TRefCountPtr<FRDGPooledBuffer>& Buffer, const TCHAR* Name) -> FRDGBufferSRVRef
		{
			return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffer, Name));
		};

		FVirtualSmInvalidateInstancePagesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVirtualSmInvalidateInstancePagesCS::FParameters>();

		PassParameters->VirtualSmCommon = GraphBuilder.CreateUniformBuffer(&PrevCommonParameters);
		FRDGBufferRef InstanceRangesRDG = CreateStructuredBuffer(GraphBuilder, TEXT("InstanceRangesSmall"), InstanceRangesLarge);
		PassParameters->InstanceRanges = GraphBuilder.CreateSRV(InstanceRangesRDG);
		PassParameters->NumRemovedItems = InstanceRangesLarge.Num();
		PassParameters->ShadowMapProjectionData = RegExtCreateSrv(PrevShadowMapProjectionDataBuffer, TEXT("PrevShadowMapProjectionData"));

		PassParameters->PageFlags = RegExtCreateSrv(PrevPageFlags, TEXT("PrevPageFlags"));
		PassParameters->HPageFlags = RegExtCreateSrv(PrevHPageFlags, TEXT("PrevHPageFlags"));
		PassParameters->PageRectBounds = RegExtCreateSrv(PrevPageRectBounds, TEXT("PrevPageRectBounds"));

		FRDGBufferRef DynamicCasterFlagsRDG = GraphBuilder.RegisterExternalBuffer(PrevDynamicCasterPageFlags, TEXT("DynamicCasterFlags"));
		PassParameters->OutDynamicCasterFlags = GraphBuilder.CreateUAV(DynamicCasterFlagsRDG);

		PassParameters->GPUSceneInstanceSceneData = GPUScene.InstanceDataBuffer.SRV;
		PassParameters->GPUScenePrimitiveSceneData = GPUScene.PrimitiveBuffer.SRV;
		PassParameters->GPUSceneFrameNumber = GPUScene.GetSceneFrameNumber();
		PassParameters->InstanceDataSOAStride = GPUScene.InstanceDataSOAStride;

		FVirtualSmInvalidateInstancePagesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVirtualSmInvalidateInstancePagesCS::FLargeSmallDim>(1);

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FVirtualSmInvalidateInstancePagesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VirtualSmInvalidateInstancePagesCS"),
			ComputeShader,
			PassParameters,
			FIntVector(InstanceRangesLarge.Num(), 1, 1)
		);
	}
}
