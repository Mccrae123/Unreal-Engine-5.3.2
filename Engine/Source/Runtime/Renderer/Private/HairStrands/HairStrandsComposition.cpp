// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsComposition.h"
#include "Shader.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterMacros.h"
#include "RenderGraphUtils.h"
#include "PostProcessing.h"
#include "HairStrandsRendering.h"
#include "HairStrandsScatter.h"

/////////////////////////////////////////////////////////////////////////////////////////

static int32 GHairFastResolveVelocityThreshold = 1;
static FAutoConsoleVariableRef CVarHairFastResolveVelocityThreshold(TEXT("r.HairStrands.VelocityThreshold"), GHairFastResolveVelocityThreshold, TEXT("Threshold value (in pixel) above which a pixel is forced to be resolve with responsive AA (in order to avoid smearing). Default is 3."));

static int32 GHairPatchBufferDataBeforePostProcessing = 1;
static FAutoConsoleVariableRef CVarHairPatchBufferDataBeforePostProcessing(TEXT("r.HairStrands.PatchMaterialData"), GHairPatchBufferDataBeforePostProcessing, TEXT("Patch the buffer with hair material data before post processing run. (default 1)."));

class FHairVisibilityComposeSamplePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityComposeSamplePS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityComposeSamplePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCategorizationTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairVisibilityNodeOffsetAndCount)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairLightingSampleBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return Parameters.Platform == SP_PCD3D_SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_COMPOSE_SAMPLE"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityComposeSamplePS, "/Engine/Private/HairStrands/HairStrandsVisibilityComposeSubPixelPS.usf", "ComposeSamplePS", SF_Pixel);

static void AddHairVisibilityComposeSamplePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsVisibilityData& VisibilityData,
	const FRDGTextureRef& CategorizationTexture,
	FRDGTextureRef& OutColorTexture,
	FRDGTextureRef& OutDepthTexture)
{
	check(VisibilityData.SampleLightingBuffer);

	FRDGTextureRef SampleLightingBuffer = GraphBuilder.RegisterExternalTexture(VisibilityData.SampleLightingBuffer);
	FRDGTextureRef NodeCount = GraphBuilder.RegisterExternalTexture(VisibilityData.NodeCount);

	FHairVisibilityComposeSamplePS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairVisibilityComposeSamplePS::FParameters>();
	Parameters->HairSampleCount = NodeCount;
	Parameters->HairCategorizationTexture = CategorizationTexture;
	Parameters->HairVisibilityNodeOffsetAndCount = GraphBuilder.RegisterExternalTexture(VisibilityData.NodeIndex);
	Parameters->HairLightingSampleBuffer = SampleLightingBuffer;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutColorTexture, ERenderTargetLoadAction::ELoad);
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(OutDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairVisibilityComposeSamplePS> PixelShader(View.ShaderMap);
	FGlobalShaderMap* GlobalShaderMap = View.ShaderMap;
	const FIntRect Viewport = View.ViewRect;
	const FIntPoint Resolution = OutColorTexture->Desc.Extent;
	const FViewInfo* CapturedView = &View;

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrandsComposeSample"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution, CapturedView](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
//		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Max, BF_SourceAlpha, BF_DestAlpha>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		VertexShader->SetParameters(RHICmdList, CapturedView->ViewUniformBuffer);
		RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
		DrawRectangle(
			RHICmdList,
			0, 0,
			Viewport.Width(), Viewport.Height(),
			Viewport.Min.X, Viewport.Min.Y,
			Viewport.Width(), Viewport.Height(),
			Viewport.Size(),
			Resolution,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityComposeSubPixelPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityComposeSubPixelPS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityComposeSubPixelPS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SubPixelColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CategorisationTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return Parameters.Platform == SP_PCD3D_SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_SUBCOLOR"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityComposeSubPixelPS, "/Engine/Private/HairStrands/HairStrandsVisibilityComposeSubPixelPS.usf", "SubColorPS", SF_Pixel);

static void AddHairVisibilityComposeSubPixelPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FRDGTextureRef& SubPixelColorTexture,
	const FRDGTextureRef CategorisationTexture,
	FRDGTextureRef& OutColorTexture,
	FRDGTextureRef& OutDepthTexture)
{
	FHairVisibilityComposeSubPixelPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairVisibilityComposeSubPixelPS::FParameters>();
	Parameters->SubPixelColorTexture = SubPixelColorTexture;
	Parameters->CategorisationTexture = CategorisationTexture;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutColorTexture, ERenderTargetLoadAction::ELoad);
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(OutDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

	FHairVisibilityComposeSubPixelPS::FPermutationDomain PermutationVector;
	TShaderMapRef<FHairVisibilityComposeSubPixelPS> PixelShader(View.ShaderMap, PermutationVector);
	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	const FIntRect Viewport = View.ViewRect;
	const FIntPoint Resolution = OutColorTexture->Desc.Extent;
	const FViewInfo* CapturedView = &View;

	{
		ClearUnusedGraphResources(PixelShader, Parameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HairStrandsVisibilityComposeSubSPixel"),
			Parameters,
			ERDGPassFlags::Raster,
			[Parameters, VertexShader, PixelShader, Viewport, Resolution, CapturedView](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();

			// Write stencil value for partially covered pixel, in order to run responsive AA on them
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

			VertexShader->SetParameters(RHICmdList, CapturedView->ViewUniformBuffer);
			RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
			DrawRectangle(
				RHICmdList,
				0, 0,
				Viewport.Width(), Viewport.Height(),
				Viewport.Min.X, Viewport.Min.Y,
				Viewport.Width(), Viewport.Height(),
				Viewport.Size(),
				Resolution,
				VertexShader,
				EDRF_UseTriangleOptimization);
		});
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityFastResolvePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityFastResolvePS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityFastResolvePS, FGlobalShader);

	class FMSAACount : SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_MSAACOUNT", 4, 8);
	using FPermutationDomain = TShaderPermutationDomain<FMSAACount>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, VelocityThreshold)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairVisibilityVelocityTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return Parameters.Platform == SP_PCD3D_SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_FASTRESOLVE"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityFastResolvePS, "/Engine/Private/HairStrands/HairStrandsVisibilityComposeSubPixelPS.usf", "FastResolvePS", SF_Pixel);

static void AddHairVisibilityFastResolvePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FRDGTextureRef& HairVisibilityVelocityTexture,
	FRDGTextureRef& OutDepthTexture)
{
	const FIntPoint Resolution = OutDepthTexture->Desc.Extent;
	FRDGTextureRef DummyTexture;
	{
		FRDGTextureDesc Desc;
		Desc.Extent = Resolution;
		Desc.Depth = 0;
		Desc.Format = PF_R8G8B8A8;
		Desc.NumMips = 1;
		Desc.NumSamples = 1;
		Desc.Flags = TexCreate_None;
		Desc.TargetableFlags = TexCreate_RenderTargetable | TexCreate_ShaderResource;
		Desc.ClearValue = FClearValueBinding(0);
		DummyTexture = GraphBuilder.CreateTexture(Desc, TEXT("HairDummyTexture"));
	}

	FVector2D PixelVelocity(1.f / (Resolution.X * 2), 1.f / (Resolution.Y * 2));
	const float VelocityThreshold = FMath::Clamp(GHairFastResolveVelocityThreshold, 0, 512) * FMath::Min(PixelVelocity.X, PixelVelocity.Y);

	FHairVisibilityFastResolvePS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairVisibilityFastResolvePS::FParameters>();
	Parameters->HairVisibilityVelocityTexture = HairVisibilityVelocityTexture;
	Parameters->VelocityThreshold = VelocityThreshold;
	Parameters->RenderTargets[0] = FRenderTargetBinding(DummyTexture, ERenderTargetLoadAction::ENoAction);
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		OutDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthNop_StencilWrite);

	const uint32 MSAASampleCount = HairVisibilityVelocityTexture->Desc.NumSamples;
	check(MSAASampleCount == 4 || MSAASampleCount == 8);
	FHairVisibilityFastResolvePS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVisibilityFastResolvePS::FMSAACount>(MSAASampleCount == 4 ? 4 : 8);

	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairVisibilityFastResolvePS> PixelShader(View.ShaderMap, PermutationVector);
	const FGlobalShaderMap* GlobalShaderMap = View.ShaderMap;
	const FIntRect Viewport = View.ViewRect;
	
	const FViewInfo* CapturedView = &View;

	{
		ClearUnusedGraphResources(PixelShader, Parameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HairStrandsVisibilityMarkTAAFastResolve"),
			Parameters,
			ERDGPassFlags::Raster,
			[Parameters, VertexShader, PixelShader, Viewport, Resolution, CapturedView](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();	
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				false, CF_Always, 
				true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
				false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
				STENCIL_TEMPORAL_RESPONSIVE_AA_MASK, STENCIL_TEMPORAL_RESPONSIVE_AA_MASK>::GetRHI();
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

			VertexShader->SetParameters(RHICmdList, CapturedView->ViewUniformBuffer);
			RHICmdList.SetStencilRef(STENCIL_TEMPORAL_RESPONSIVE_AA_MASK);
			RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
			DrawRectangle(
				RHICmdList,
				0, 0,
				Viewport.Width(), Viewport.Height(),
				Viewport.Min.X, Viewport.Min.Y,
				Viewport.Width(), Viewport.Height(),
				Viewport.Size(),
				Resolution,
				VertexShader,
				EDRF_UseTriangleOptimization);
		});
	}
}


///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairPatchGbufferDataPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairPatchGbufferDataPS);
	SHADER_USE_PARAMETER_STRUCT(FHairPatchGbufferDataPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CategorisationTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return Parameters.Platform == SP_PCD3D_SM5; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_PATCH"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairPatchGbufferDataPS, "/Engine/Private/HairStrands/HairStrandsVisibilityComposeSubPixelPS.usf", "MainPS", SF_Pixel); 

static void AddPatchGbufferDataPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef HairCategorizationTexture,
	FRDGTextureRef OutGbufferATexture,
	FRDGTextureRef OutGbufferBTexture)
{
	const FIntPoint Resolution = OutGbufferATexture->Desc.Extent;
	FHairPatchGbufferDataPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairPatchGbufferDataPS::FParameters>();
	Parameters->CategorisationTexture = HairCategorizationTexture;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutGbufferATexture, ERenderTargetLoadAction::ELoad);
	Parameters->RenderTargets[1] = FRenderTargetBinding(OutGbufferBTexture, ERenderTargetLoadAction::ELoad);

	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairPatchGbufferDataPS> PixelShader(View.ShaderMap);
	const FIntRect Viewport = View.ViewRect;

	const FViewInfo* CapturedView = &View;

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairPatchGbufferData"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution, CapturedView](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		VertexShader->SetParameters(RHICmdList, CapturedView->ViewUniformBuffer);
		RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
		DrawRectangle(
			RHICmdList,
			0, 0,
			Viewport.Width(), Viewport.Height(),
			Viewport.Min.X, Viewport.Min.Y,
			Viewport.Width(), Viewport.Height(),
			Viewport.Size(),
			Resolution,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}
///////////////////////////////////////////////////////////////////////////////////////////////////

void RenderHairComposeSubPixel(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FViewInfo>& Views,
	const FHairStrandsDatas* HairDatas)
{
	if (!HairDatas || HairDatas->HairVisibilityViews.HairDatas.Num() == 0)
		return;

	const FHairStrandsVisibilityViews& HairVisibilityViews = HairDatas->HairVisibilityViews;

	DECLARE_GPU_STAT(HairStrandsComposeSubPixel);
	SCOPED_DRAW_EVENT(RHICmdList, HairStrandsComposeSubPixel);
	SCOPED_GPU_STAT(RHICmdList, HairStrandsComposeSubPixel);

	FSceneRenderTargets& SceneTargets = FSceneRenderTargets::Get(RHICmdList);
	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGTextureRef SceneColorSubPixelTexture = GraphBuilder.RegisterExternalTexture(SceneTargets.SceneColorSubPixel, TEXT("SceneColorSubPixelTexture"));
	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneTargets.GetSceneColor(), TEXT("SceneColorTexture"));
	FRDGTextureRef SceneColorDepth = GraphBuilder.RegisterExternalTexture(SceneTargets.SceneDepthZ, TEXT("SceneDepthTexture"));
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		if (View.Family)
		{
			if (ViewIndex < HairVisibilityViews.HairDatas.Num())
			{
				const FHairStrandsMacroGroupDatas& MacroGroupDatas = HairDatas->MacroGroupsPerViews.Views[ViewIndex];
				const FHairStrandsVisibilityData& VisibilityData = HairVisibilityViews.HairDatas[ViewIndex];

				TRefCountPtr<IPooledRenderTarget> CategorisationTexture = VisibilityData.CategorizationTexture;
				if (!CategorisationTexture)
				{
					continue; // Automatically skip for any view not rendering hair
				}
				const FRDGTextureRef RDGCategorisationTexture = CategorisationTexture ? GraphBuilder.RegisterExternalTexture(CategorisationTexture, TEXT("HairVisibilityCategorisationTexture")) : nullptr;

				AddHairDiffusionPass(
					GraphBuilder,
					View,
					VisibilityData,
					MacroGroupDatas.VirtualVoxelResources,
					SceneColorDepth,
					SceneColorSubPixelTexture,
					SceneColorTexture);

				if (VisibilityData.SampleLightingBuffer)
				{
					AddHairVisibilityComposeSamplePass(
						GraphBuilder,
						View,
						VisibilityData,
						RDGCategorisationTexture,
						SceneColorTexture,
						SceneColorDepth);
				}
				else
				{
					// #hair_todo : compose partially covered hair with transparent surface: this can be done by 
					// rendering quad(s) covering the hair at the correct depth. This will be sorted with other 
					// transparent surface, which should make the overall sorting workable
					AddHairVisibilityComposeSubPixelPass(
						GraphBuilder,
						View,
						SceneColorSubPixelTexture,
						RDGCategorisationTexture,
						SceneColorTexture,
						SceneColorDepth);
				}

				if (HairVisibilityViews.HairDatas[ViewIndex].VelocityTexture)
				{
					FRDGTextureRef RDGHairVisibilityVelocityTexture = GraphBuilder.RegisterExternalTexture(HairVisibilityViews.HairDatas[ViewIndex].VelocityTexture, TEXT("HairVisibilityVelocityTexture"));
					AddHairVisibilityFastResolvePass(
						GraphBuilder,
						View,
						RDGHairVisibilityVelocityTexture,
						SceneColorDepth);
				}

				const bool bPatchBufferData = GHairPatchBufferDataBeforePostProcessing > 0;
				if (bPatchBufferData)
				{
					FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);				
					const FRDGTextureRef GBufferATexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferA , TEXT("GBufferA"));
					const FRDGTextureRef GBufferBTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferB, TEXT("GBufferB"));
					AddPatchGbufferDataPass(
						GraphBuilder,
						View,
						RDGCategorisationTexture,
						GBufferATexture,
						GBufferBTexture);
				}
			}
		}
	}

	GraphBuilder.Execute();
}
