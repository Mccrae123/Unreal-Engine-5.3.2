// Copyright Epic Games, Inc. All Rights Reserved.

#include "HeterogeneousVolumes.h"

#include "PixelShaderUtils.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstance.h"
#include "RayTracingInstanceBufferUtil.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneManagement.h"

class FRenderSingleScatteringWithLiveShadingCS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FRenderSingleScatteringWithLiveShadingCS, MeshMaterial);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Light data
		SHADER_PARAMETER(int, bApplyEmission)
		SHADER_PARAMETER(int, bApplyDirectLighting)
		SHADER_PARAMETER(int, bApplyShadowTransmittance)
		SHADER_PARAMETER(int, LightType)
		SHADER_PARAMETER_STRUCT_REF(FDeferredLightUniformStruct, DeferredLight)

		// Shadow data
		SHADER_PARAMETER(float, ShadowStepFactor)

		// Object data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)
		SHADER_PARAMETER(FVector3f, LocalBoundsOrigin)
		SHADER_PARAMETER(FVector3f, LocalBoundsExtent)
		SHADER_PARAMETER(int32, PrimitiveId)

		// Ray data
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(float, StepSize)
		SHADER_PARAMETER(int, MaxStepCount)
		SHADER_PARAMETER(int, bJitter)

		// Dispatch data
		SHADER_PARAMETER(FIntVector, GroupCount)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWLightingTexture)
		//SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<Volumes::FDebugOutput>, RWDebugOutputBuffer)
		END_SHADER_PARAMETER_STRUCT()

		FRenderSingleScatteringWithLiveShadingCS() = default;

	FRenderSingleScatteringWithLiveShadingCS(
		const FMeshMaterialShaderType::CompiledShaderInitializerType& Initializer
	)
		: FMeshMaterialShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(
			this,
			Initializer.PermutationId,
			Initializer.ParameterMap,
			*FParameters::FTypeInfo::GetStructMetadata(),
			// Don't require full bindings, we use FMaterialShader::SetParameters
			false
		);
	}

	static bool ShouldCompilePermutation(
		const FMaterialShaderPermutationParameters& Parameters
	)
	{
		return DoesPlatformSupportHeterogeneousVolumes(Parameters.Platform)
			&& (Parameters.MaterialParameters.MaterialDomain == MD_Volume)
			&& Parameters.MaterialParameters.bIsUsedWithNiagaraMeshParticles;
	}

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static void ModifyCompilationEnvironment(
		const FMaterialShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment
	)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());

		// This shader takes a very long time to compile with FXC, so we pre-compile it with DXC first and then forward the optimized HLSL to FXC.
		OutEnvironment.CompilerFlags.Add(CFLAG_PrecompileWithDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);

		OutEnvironment.SetDefine(TEXT("GET_PRIMITIVE_DATA_OVERRIDE"), 1);
	}

	void SetParameters(
		FRHIComputeCommandList& RHICmdList,
		FRHIComputeShader* ShaderRHI,
		const FViewInfo& View,
		const FMaterialRenderProxy* MaterialProxy,
		const FMaterial& Material
	)
	{
		FMaterialShader::SetViewParameters(RHICmdList, ShaderRHI, View, View.ViewUniformBuffer);
		FMaterialShader::SetParameters(RHICmdList, ShaderRHI, MaterialProxy, Material, View);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FRenderSingleScatteringWithLiveShadingCS, TEXT("/Engine/Private/HeterogeneousVolumes/HeterogeneousVolumesLiveShadingPipeline.usf"), TEXT("RenderSingleScatteringWithLiveShadingCS"), SF_Compute);

void RenderSingleScatteringWithLiveShading(
	FRDGBuilder& GraphBuilder,
	// Scene data
	const FScene* Scene,
	const FViewInfo& View,
	const FSceneTextures& SceneTextures,
	// Light data
	bool bApplyEmission,
	bool bApplyDirectLighting,
	bool bApplyShadowTransmittance,
	uint32 LightType,
	const FLightSceneInfo* LightSceneInfo,
	// Object data
	const FPrimitiveSceneProxy* PrimitiveSceneProxy,
	const FMaterialRenderProxy* MaterialRenderProxy,
	const int32 PrimitiveId,
	const FBoxSphereBounds LocalBoxSphereBounds,
	// Output
	FRDGTextureRef& HeterogeneousVolumeTexture
)
{
	const FMaterial& Material = MaterialRenderProxy->GetMaterialWithFallback(View.GetFeatureLevel(), MaterialRenderProxy);
	if (Material.GetMaterialDomain() != MD_Volume)
	{
		return;
	}

	uint32 GroupCountX = FMath::DivideAndRoundUp(View.ViewRect.Size().X, FRenderSingleScatteringWithLiveShadingCS::GetThreadGroupSize2D());
	uint32 GroupCountY = FMath::DivideAndRoundUp(View.ViewRect.Size().Y, FRenderSingleScatteringWithLiveShadingCS::GetThreadGroupSize2D());
	FIntVector GroupCount = FIntVector(GroupCountX, GroupCountY, 1);

#if 0
	FRDGBufferRef DebugOutputBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(Volumes::FDebugOutput), GroupCountX * GroupCountY * FRenderSingleScatteringWithLiveShadingCS::GetThreadGroupSize2D()),
		TEXT("Lumen.Reflections.TraceDataPacked"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugOutputBuffer), 0);
#endif

	FRenderSingleScatteringWithLiveShadingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRenderSingleScatteringWithLiveShadingCS::FParameters>();
	{
		// Scene data
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder, SceneTextures);

		// Light data
		FDeferredLightUniformStruct DeferredLightUniform;
		PassParameters->bApplyEmission = bApplyEmission;
		PassParameters->bApplyDirectLighting = bApplyDirectLighting;
		PassParameters->bApplyShadowTransmittance = bApplyShadowTransmittance;
		if (PassParameters->bApplyDirectLighting && (LightSceneInfo != nullptr))
		{
			DeferredLightUniform = GetDeferredLightParameters(View, *LightSceneInfo);
		}
		PassParameters->DeferredLight = CreateUniformBufferImmediate(DeferredLightUniform, UniformBuffer_SingleDraw);
		PassParameters->LightType = LightType;
		PassParameters->ShadowStepFactor = HeterogeneousVolumes::GetShadowStepFactor();

		// Object data
		FMatrix44f LocalToWorld = FMatrix44f(PrimitiveSceneProxy->GetLocalToWorld());
		PassParameters->LocalToWorld = LocalToWorld;
		PassParameters->WorldToLocal = LocalToWorld.Inverse();
		PassParameters->LocalBoundsOrigin = FVector3f(LocalBoxSphereBounds.Origin);
		PassParameters->LocalBoundsExtent = FVector3f(LocalBoxSphereBounds.BoxExtent);
		PassParameters->PrimitiveId = PrimitiveId;

		// Ray data
		PassParameters->MaxTraceDistance = HeterogeneousVolumes::GetMaxTraceDistance();
		PassParameters->StepSize = HeterogeneousVolumes::GetStepSize();
		PassParameters->MaxStepCount = HeterogeneousVolumes::GetMaxStepCount();
		PassParameters->bJitter = HeterogeneousVolumes::ShouldJitter();

		// Dispatch data
		PassParameters->GroupCount = GroupCount;

		// Output
		PassParameters->RWLightingTexture = GraphBuilder.CreateUAV(HeterogeneousVolumeTexture);
		//PassParameters->RWDebugOutputBuffer = GraphBuilder.CreateUAV(DebugOutputBuffer);
	}

	FString LightName = TEXT("none");
	if (LightSceneInfo != nullptr)
	{
		FSceneRenderer::GetLightNameForDrawEvent(LightSceneInfo->Proxy, LightName);
	}

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RenderSingleScatteringWithLiveShadingCS (Light = %s)", *LightName),
		PassParameters,
		ERDGPassFlags::Compute,
		// Why is scene explicitly copied??
		[PassParameters, LocalScene = Scene, &View, MaterialRenderProxy, &Material, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FRenderSingleScatteringWithLiveShadingCS::FPermutationDomain PermutationVector;
			TShaderRef<FRenderSingleScatteringWithLiveShadingCS> ComputeShader = Material.GetShader<FRenderSingleScatteringWithLiveShadingCS>(&FLocalVertexFactory::StaticType, PermutationVector, false);

			if (!ComputeShader.IsNull())
			{
				FMeshPassProcessorRenderState DrawRenderState;

				FMeshMaterialShaderElementData ShaderElementData;
				ShaderElementData.FadeUniformBuffer = GDistanceCullFadedInUniformBuffer.GetUniformBufferRHI();
				ShaderElementData.DitherUniformBuffer = GDitherFadedInUniformBuffer.GetUniformBufferRHI();

				FMeshProcessorShaders PassShaders;
				PassShaders.ComputeShader = ComputeShader;

				FMeshDrawShaderBindings ShaderBindings;
				{
					ShaderBindings.Initialize(PassShaders);

					int32 DataOffset = 0;
					FMeshDrawSingleShaderBindings SingleShaderBindings = ShaderBindings.GetSingleShaderBindings(SF_Compute, DataOffset);
					ComputeShader->GetShaderBindings(LocalScene, LocalScene->GetFeatureLevel(), nullptr, *MaterialRenderProxy, Material, DrawRenderState, ShaderElementData, SingleShaderBindings);

					ShaderBindings.Finalize(&PassShaders);
				}
				ShaderBindings.SetOnCommandList(RHICmdList, ComputeShader.GetComputeShader());

				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
			}
		}
	);
}

void RenderWithLiveShading(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	const FScene* Scene,
	const FViewInfo& View,
	// Object data
	const FPrimitiveSceneProxy* PrimitiveSceneProxy,
	const FMaterialRenderProxy* MaterialRenderProxy,
	const int32 PrimitiveId,
	const FBoxSphereBounds LocalBoxSphereBounds,
	// Output
	FRDGTextureRef& HeterogeneousVolumeRadiance
)
{
	// Light culling
	TArray<FLightSceneInfoCompact, TInlineAllocator<64>> LightSceneInfoCompact;
	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		if (LightIt->AffectsPrimitive(PrimitiveSceneProxy->GetBounds(), PrimitiveSceneProxy))
		{
			LightSceneInfoCompact.Add(*LightIt);
		}
	}

	// Light loop:
	int32 NumPasses = FMath::Max(LightSceneInfoCompact.Num(), 1);
	for (int32 PassIndex = 0; PassIndex < NumPasses; ++PassIndex)
	{
		bool bApplyEmission = PassIndex == 0;
		bool bApplyDirectLighting = !LightSceneInfoCompact.IsEmpty();
		bool bApplyShadowTransmittance = false;

		uint32 LightType = 0;
		FLightSceneInfo* LightSceneInfo = nullptr;
		if (bApplyDirectLighting)
		{
			LightType = LightSceneInfoCompact[PassIndex].LightType;
			LightSceneInfo = LightSceneInfoCompact[PassIndex].LightSceneInfo;
			check(LightSceneInfo != nullptr);

			bApplyDirectLighting = (LightSceneInfo != nullptr);
			bApplyShadowTransmittance = LightSceneInfo->Proxy->CastsVolumetricShadow();
		}

		RenderSingleScatteringWithLiveShading(
			GraphBuilder,
			// Scene data
			Scene,
			View,
			SceneTextures,
			// Light data
			bApplyEmission,
			bApplyDirectLighting,
			bApplyShadowTransmittance,
			LightType,
			LightSceneInfo,
			// Object data
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			PrimitiveId,
			LocalBoxSphereBounds,
			// Output
			HeterogeneousVolumeRadiance
		);
	}
}