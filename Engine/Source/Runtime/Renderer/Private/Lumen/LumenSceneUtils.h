// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenSceneUtils.h
=============================================================================*/

#pragma once

#include "RHIDefinitions.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "RendererPrivateUtils.h"

BEGIN_SHADER_PARAMETER_STRUCT(FLumenCardScatterParameters, )
	SHADER_PARAMETER_RDG_BUFFER(Buffer<uint>, CardIndirectArgs)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, QuadAllocator)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, QuadData)
	SHADER_PARAMETER(uint32, MaxQuadsPerScatterInstance)
	SHADER_PARAMETER(uint32, TilesPerInstance)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FCullCardsShapeParameters, )
	SHADER_PARAMETER(FVector4, InfluenceSphere)
	SHADER_PARAMETER(FVector, LightPosition)
	SHADER_PARAMETER(FVector, LightDirection)
	SHADER_PARAMETER(float, LightRadius)
	SHADER_PARAMETER(float, CosConeAngle)
	SHADER_PARAMETER(float, SinConeAngle)
END_SHADER_PARAMETER_STRUCT()

enum class ECullCardsMode
{
	OperateOnCardsToRender,
	OperateOnScene,
	OperateOnSceneForceUpdateForCardsToRender,
};

enum class ECullCardsShapeType
{
	None,
	PointLight,
	SpotLight,
	RectLight
};

class FLumenCardScatterContext
{
public:
	int32 MaxQuadCount = 0;
	int32 MaxScatterInstanceCount = 0;
	int32 MaxQuadsPerScatterInstance = 0;
	int32 NumCardsToOperateOn = 0;
	ECullCardsMode CardsCullMode;

	FLumenCardScatterParameters Parameters;

	FRDGBufferUAVRef QuadAllocatorUAV = nullptr;
	FRDGBufferUAVRef QuadDataUAV = nullptr;

	void Init(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FLumenSceneData& LumenSceneData,
		const FLumenCardRenderer& LumenCardRenderer,
		ECullCardsMode InCardsCullMode,
		int32 InMaxScatterInstanceCount = 1);

	void CullCardsToShape(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FLumenSceneData& LumenSceneData,
		const FLumenCardRenderer& LumenCardRenderer,
		ECullCardsShapeType ShapeType,
		const FCullCardsShapeParameters& ShapeParameters,
		float UpdateFrequencyScale,
		int32 ScatterInstanceIndex);

	void BuildScatterIndirectArgs(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View);

	uint32 GetIndirectArgOffset(int32 ScatterInstanceIndex) const;
};

class FCullCardsToShapeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCullCardsToShapeCS);
	SHADER_USE_PARAMETER_STRUCT(FCullCardsToShapeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWQuadAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWQuadData)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER(uint32, MaxQuadsPerScatterInstance)
		SHADER_PARAMETER(uint32, ScatterInstanceIndex)
		SHADER_PARAMETER(uint32, NumVisibleCardsIndices)
		SHADER_PARAMETER(uint32, NumCardsToRenderIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VisibleCardsIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CardsToRenderIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CardsToRenderHashMap)
		SHADER_PARAMETER(uint32, FrameId)
		SHADER_PARAMETER(float, CardLightingUpdateFrequencyScale)
		SHADER_PARAMETER(uint32, CardLightingUpdateMinFrequency)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCullCardsShapeParameters, ShapeParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FOperateOnCardsMode : SHADER_PERMUTATION_INT("OPERATE_ON_CARDS_MODE", 3);
	class FShapeType : SHADER_PERMUTATION_INT("SHAPE_TYPE", 4);
	using FPermutationDomain = TShaderPermutationDomain<FOperateOnCardsMode, FShapeType>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

class FInitializeCardScatterIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FInitializeCardScatterIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FInitializeCardScatterIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCardIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, QuadAllocator)
		SHADER_PARAMETER(uint32, MaxScatterInstanceCount)
		SHADER_PARAMETER(uint32, TilesPerInstance)
	END_SHADER_PARAMETER_STRUCT()

	class FRectList : SHADER_PERMUTATION_BOOL("RECT_LIST_TOPOLOGY");
	using FPermutationDomain = TShaderPermutationDomain<FRectList>;

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

class FRasterizeToCardsVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRasterizeToCardsVS);
	SHADER_USE_PARAMETER_STRUCT(FRasterizeToCardsVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FLumenCardScene, LumenCardScene)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardScatterParameters, CardScatterParameters)
		SHADER_PARAMETER(FVector4, InfluenceSphere)
		SHADER_PARAMETER(FVector2D, CardUVSamplingOffset)
		SHADER_PARAMETER(uint32, ScatterInstanceIndex)
	END_SHADER_PARAMETER_STRUCT()

	class FClampToInfluenceSphere : SHADER_PERMUTATION_BOOL("CLAMP_TO_INFLUENCE_SPHERE");

	using FPermutationDomain = TShaderPermutationDomain<FClampToInfluenceSphere>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
};

extern TGlobalResource<FTileTexCoordVertexBuffer> GLumenTileTexCoordVertexBuffer;
extern TGlobalResource<FTileIndexBuffer> GLumenTileIndexBuffer;

extern const int32 NumLumenQuadsInBuffer;

inline bool UseRectTopologyForLumen()
{
	//@todo - debug why rects aren't working
	return false;
	//return GRHISupportsRectTopology != 0;
}

template<typename PixelShaderType, typename PassParametersType>
void DrawQuadsToAtlas(
	FIntPoint ViewportSize,
	TShaderRefBase<PixelShaderType, FShaderMapPointerTable> PixelShader,
	const PassParametersType* PassParameters,
	FGlobalShaderMap* GlobalShaderMap,
	FRHIBlendState* BlendState,
	FRHICommandListImmediate& RHICmdList)
{
	FRasterizeToCardsVS::FPermutationDomain PermutationVector;
	PermutationVector.Set< FRasterizeToCardsVS::FClampToInfluenceSphere >(false);
	auto VertexShader = GlobalShaderMap->GetShader<FRasterizeToCardsVS>(PermutationVector);

	DrawQuadsToAtlas(ViewportSize, 
		VertexShader,
		PixelShader, 
		PassParameters, 
		GlobalShaderMap, 
		BlendState, 
		RHICmdList, 
		[](FRHICommandListImmediate& RHICmdList, TShaderRefBase<PixelShaderType, FShaderMapPointerTable> Shader, FRHIPixelShader* ShaderRHI, const typename PixelShaderType::FParameters& Parameters)
	{
	});
}

template<typename PixelShaderType, typename PassParametersType, typename SetParametersLambdaType>
void DrawQuadsToAtlas(
	FIntPoint ViewportSize,
	TShaderRefBase<FRasterizeToCardsVS, FShaderMapPointerTable> VertexShader,
	TShaderRefBase<PixelShaderType, FShaderMapPointerTable> PixelShader,
	const PassParametersType* PassParameters,
	FGlobalShaderMap* GlobalShaderMap,
	FRHIBlendState* BlendState,
	FRHICommandListImmediate& RHICmdList,
	SetParametersLambdaType&& SetParametersLambda,
	uint32 CardIndirectArgOffset = 0)
{
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	RHICmdList.SetViewport(0, 0, 0.0f, ViewportSize.X, ViewportSize.Y, 1.0f);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.BlendState = BlendState;

	GraphicsPSOInit.PrimitiveType = UseRectTopologyForLumen() ? PT_RectList : PT_TriangleList;

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GTileVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);
	SetParametersLambda(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

	RHICmdList.SetStreamSource(0, GLumenTileTexCoordVertexBuffer.VertexBufferRHI, 0);

	if (UseRectTopologyForLumen())
	{
		RHICmdList.DrawPrimitiveIndirect(PassParameters->VS.CardScatterParameters.CardIndirectArgs->GetIndirectRHICallBuffer(), CardIndirectArgOffset);
	}
	else
	{
		RHICmdList.DrawIndexedPrimitiveIndirect(GLumenTileIndexBuffer.IndexBufferRHI, PassParameters->VS.CardScatterParameters.CardIndirectArgs->GetIndirectRHICallBuffer(), CardIndirectArgOffset);
	}
}

class FHemisphereDirectionSampleGenerator
{
public:
	TArray<FVector4> SampleDirections;
	float ConeHalfAngle = 0;
	int32 Seed = 0;
	int32 PowerOfTwoDivisor = 1;
	bool bFullSphere = false;
	bool bCosineDistribution = false;

	void GenerateSamples(int32 TargetNumSamples, int32 InPowerOfTwoDivisor, int32 InSeed, bool bInFullSphere = false, bool bInCosineDistribution = false);

	void GetSampleDirections(const FVector4*& OutDirections, int32& OutNumDirections) const
	{
		OutDirections = SampleDirections.GetData();
		OutNumDirections = SampleDirections.Num();
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FLumenCardTracingParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_REF(FReflectionUniformParameters, ReflectionStruct)
	SHADER_PARAMETER_STRUCT_REF(FLumenCardScene, LumenCardScene)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FinalLightingAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OpacityAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DilatedDepthAtlas)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D, VoxelLighting)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D, MergedVoxelLighting)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D, VoxelDistanceField)
	SHADER_PARAMETER_TEXTURE(Texture3D, CubeMapTreeLUTAtlas)
	SHADER_PARAMETER(uint32, NumClipmapLevels)
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapWorldToUVScale, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapWorldToUVBias, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapWorldCenter, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapWorldExtent, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapWorldSamplingExtent, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER_ARRAY(FVector4, ClipmapVoxelSizeAndRadius, [MaxVoxelClipmapLevels])
	SHADER_PARAMETER(uint32, NumGlobalSDFClipmaps)
END_SHADER_PARAMETER_STRUCT()

class FLumenCardTracingInputs
{
public:

	FLumenCardTracingInputs(FRDGBuilder& GraphBuilder, const FScene* Scene, const FViewInfo& View);
	void ExtractToScene(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);

	FRDGTextureRef FinalLightingAtlas;
	FRDGTextureRef OpacityAtlas;
	FRDGTextureRef DilatedDepthAtlas;
	FRDGTextureRef VoxelLighting;
	FRDGTextureRef MergedVoxelLighting;
	FRDGTextureRef VoxelDistanceField;
	FIntVector VoxelGridResolution;
	int32 NumClipmapLevels;
	int32 BVHDepth;
	TStaticArray<FVector, MaxVoxelClipmapLevels> ClipmapWorldToUVScale;
	TStaticArray<FVector, MaxVoxelClipmapLevels> ClipmapWorldToUVBias;
	TStaticArray<FVector, MaxVoxelClipmapLevels> ClipmapWorldCenter;
	TStaticArray<FVector, MaxVoxelClipmapLevels> ClipmapWorldExtent;
	TStaticArray<FVector, MaxVoxelClipmapLevels> ClipmapWorldSamplingExtent;
	TStaticArray<FVector4, MaxVoxelClipmapLevels> ClipmapVoxelSizeAndRadius;
	TUniformBufferRef<FLumenCardScene> LumenCardScene;
};

extern void GetLumenCardTracingParameters(const FViewInfo& View, const FLumenCardTracingInputs& TracingInputs, FLumenCardTracingParameters& TracingParameters, bool bShaderWillTraceCardsOnly = false);

BEGIN_SHADER_PARAMETER_STRUCT(FLumenCardFroxelGridParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CulledCardGridHeader)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CulledCardGridData)
	SHADER_PARAMETER(uint32, CardGridPixelSizeShift)
	SHADER_PARAMETER(FVector, CardGridZParams)
	SHADER_PARAMETER(FIntVector, CullGridSize)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenMeshSDFTracingParameters, )
	SHADER_PARAMETER_SRV(Buffer<uint2>, MeshSDFObjectOverlappingCardHeader)
	SHADER_PARAMETER_SRV(Buffer<uint>, MeshSDFObjectOverlappingCardData)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SceneObjectBounds)
	SHADER_PARAMETER_SRV(StructuredBuffer<float4>, SceneObjectData)
	SHADER_PARAMETER(uint32, NumSceneObjects)
	SHADER_PARAMETER_TEXTURE(Texture3D, DistanceFieldTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, DistanceFieldSampler)
	SHADER_PARAMETER(FVector, DistanceFieldAtlasTexelSize)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenMeshSDFGridParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, NumGridCulledMeshSDFObjects)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GridCulledMeshSDFObjectStartOffsetArray)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GridCulledMeshSDFObjectIndicesArray)
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenMeshSDFTracingParameters, TracingParameters)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenMeshSDFGridCompactParameters, )
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNumGridCulledMeshSDFObjects)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWGridCulledMeshSDFObjectIndicesArray)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenIndirectTracingParameters, )
	SHADER_PARAMETER(float, StepFactor)
	SHADER_PARAMETER(float, VoxelStepFactor)
	SHADER_PARAMETER(float, CardTraceEndDistanceFromCamera)
	SHADER_PARAMETER(float, DiffuseConeHalfAngle)
	SHADER_PARAMETER(float, TanDiffuseConeHalfAngle)
	SHADER_PARAMETER(float, MinSampleRadius)
	SHADER_PARAMETER(float, MinTraceDistance)
	SHADER_PARAMETER(float, MaxTraceDistance)
	SHADER_PARAMETER(float, MaxCardTraceDistance)
	SHADER_PARAMETER(float, SurfaceBias)
	SHADER_PARAMETER(float, CardInterpolateInfluenceRadius)
	SHADER_PARAMETER(float, SpecularFromDiffuseRoughnessStart)
	SHADER_PARAMETER(float, SpecularFromDiffuseRoughnessEnd)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenDiffuseTracingParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(HybridIndirectLighting::FCommonParameters, CommonDiffuseParameters)
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
	SHADER_PARAMETER(float, SampleWeight)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DownsampledDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DownsampledNormal)	
END_SHADER_PARAMETER_STRUCT()


extern void CullLumenCardsToFroxelGrid(
	const FViewInfo& View,
	const FLumenCardTracingInputs& TracingInputs,
	float TanConeAngle, 
	float MinTraceDistance,
	float MaxTraceDistance,
	float MaxCardTraceDistance,
	float CardTraceEndDistanceFromCamera,
	int32 ScreenDownsampleFactor,
	FRDGTextureRef DownsampledDepth,
	FRDGBuilder& GraphBuilder,
	FLumenCardFroxelGridParameters& OutGridParameters);

extern void CullMeshSDFObjectsToViewGrid(
	const FViewInfo& View,
	const FScene* Scene,
	float MaxMeshSDFInfluenceRadius,
	float CardTraceEndDistanceFromCamera,
	int32 GridPixelsPerCellXY,
	int32 GridSizeZ,
	FVector ZParams,
	FRDGBuilder& GraphBuilder,
	FLumenMeshSDFGridParameters& OutGridParameters,
	FLumenMeshSDFGridCompactParameters& OutGridCompactParameters);

extern void CullMeshSDFObjectGridToGBuffer(
	const FViewInfo& View,
	const FScene* Scene,
	float MaxMeshSDFInfluenceRadius,
	float CardTraceEndDistanceFromCamera,
	const HybridIndirectLighting::FCommonParameters& CommonDiffuseParameters,
	FRDGTextureRef DownsampledDepth,
	int32 GridPixelsPerCellXY,
	int32 GridSizeZ,
	FVector ZParams,
	FRDGBuilder& GraphBuilder,
	const FLumenMeshSDFGridParameters& GridParameters,
	const FLumenMeshSDFGridCompactParameters& GridCompactParameters);

extern void CullMeshSDFObjectsToProbes(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	float MaxMeshSDFInfluenceRadius,
	float CardTraceEndDistanceFromCamera,
	const LumenProbeHierarchy::FHierarchyParameters& ProbeHierarchyParameters,
	const LumenProbeHierarchy::FEmitProbeParameters& EmitProbeParameters,
	FLumenMeshSDFGridParameters& OutGridParameters);

extern void CullForCardTracing(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	FLumenCardTracingInputs TracingInputs,
	const FLumenDiffuseTracingParameters& DiffuseTracingParameters,
	FLumenCardFroxelGridParameters& GridParameters,
	FLumenMeshSDFGridParameters& MeshSDFGridParameters);

extern void SetupLumenDiffuseTracingParameters(FLumenIndirectTracingParameters& OutParameters);
extern void SetupLumenDiffuseTracingParametersForProbe(FLumenIndirectTracingParameters& OutParameters, float DiffuseConeAngle);
extern void SetupLumenSpecularTracingParameters(FLumenIndirectTracingParameters& OutParameters);
extern bool ShouldRenderLumenReflections(const FViewInfo& View);
extern void ClearAtlasRDG(FRDGBuilder& GraphBuilder, FRDGTextureRef AtlasTexture);
extern FVector GetLumenSceneViewOrigin(const FViewInfo& View, int32 ClipmapIndex);
extern int32 GetNumLumenVoxelClipmaps();
extern void UpdateDistantScene(FScene* Scene, FViewInfo& View);

namespace Lumen
{
	enum class ETracingPermutation
	{
		Cards,
		VoxelsAfterCards,
		Voxels,
		MAX
	};

	float GetDistanceSceneNaniteLODScaleFactor();
	uint32 GetVoxelTracingMode();
	bool UseVoxelRayTracing();
	float GetMaxTraceDistance();

	void UpdateVoxelDistanceField(FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const TArray<int32, SceneRenderingAllocator>& ClipmapsToUpdate,
		FLumenCardTracingInputs& TracingInputs);
};

extern int32 GLumenFastCameraMode;
extern int32 GLumenDistantScene;