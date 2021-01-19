// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MobileTranslucentRendering.cpp: translucent rendering implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "HitProxies.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRenderTargetParameters.h"
#include "SceneRendering.h"
#include "LightMapRendering.h"
#include "MaterialShaderType.h"
#include "MeshMaterialShaderType.h"
#include "MeshMaterialShader.h"
#include "BasePassRendering.h"
#include "DynamicPrimitiveDrawing.h"
#include "TranslucentRendering.h"
#include "MobileBasePassRendering.h"
#include "ScenePrivate.h"
#include "ScreenRendering.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "MeshPassProcessor.inl"
#include "ClearQuad.h"

void FMobileSceneRenderer::RenderTranslucency(FRDGBuilder& GraphBuilder, FRenderTargetBindingSlots& BasePassRenderTargets, const TArrayView<const FViewInfo> PassViews, FRDGTextureRef ScreenSpaceAO)
{
	ETranslucencyPass::Type TranslucencyPass = 
		ViewFamily.AllowTranslucencyAfterDOF() ? ETranslucencyPass::TPT_StandardTranslucency : ETranslucencyPass::TPT_AllTranslucency;
		
	if (ShouldRenderTranslucency(TranslucencyPass))
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Translucency");
		RDG_GPU_STAT_SCOPE(GraphBuilder, Translucency);

		for (int32 ViewIndex = 0; ViewIndex < PassViews.Num(); ViewIndex++)
		{
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

			const FViewInfo& View = PassViews[ViewIndex];
			if (!View.ShouldRenderView())
			{
				continue;
			}

			// GPUCULL_TODO: View.ParallelMeshDrawCommandPasses[MeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene);

			View.BeginRenderView();
			UpdateDirectionalLightUniformBuffers(GraphBuilder, View);

			auto* TranslucencyBasePassParameters = GraphBuilder.AllocParameters<FMobileBasePassParameters>();
			TranslucencyBasePassParameters->View = View.GetShaderParameters();
			TranslucencyBasePassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, ScreenSpaceAO);
			TranslucencyBasePassParameters->RenderTargets = BasePassRenderTargets;

			GraphBuilder.AddPass(RDG_EVENT_NAME("RenderTranslucencyBasePass"), TranslucencyBasePassParameters, ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
				[this, &View, TranslucencyPass](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

				if (!View.Family->UseDebugViewPS())
				{
					const EMeshPass::Type MeshPass = TranslucencyPassToMeshPass(TranslucencyPass);
					View.ParallelMeshDrawCommandPasses[MeshPass].DispatchDraw(nullptr, RHICmdList);
				}
			});
		}
	}
}

void FMobileSceneRenderer::RenderInverseOpacity(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
	View.BeginRenderView();
	UpdateDirectionalLightUniformBuffers(GraphBuilder, View);

	const FSceneTextures& SceneTextures = FSceneTextures::Get(GraphBuilder);

	auto* InverseOpacityParameters = GraphBuilder.AllocParameters<FMobileBasePassParameters>();
	InverseOpacityParameters->View = View.GetShaderParameters();
	InverseOpacityParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, SceneTextures.ScreenSpaceAO);
	InverseOpacityParameters->RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, SceneTextures.Color.Resolve, ERenderTargetLoadAction::EClear);
	InverseOpacityParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	// Opacity could fetch depth as we use exactly the same shaders as in base pass
	InverseOpacityParameters->RenderTargets.SubpassHint = ESubpassHint::DepthReadSubpass;

	GraphBuilder.AddPass(RDG_EVENT_NAME("InverseOpacityPass"), InverseOpacityParameters, ERDGPassFlags::Raster,
		[this, &View](FRHICommandListImmediate& RHICmdList)
	{
		// Mobile multi-view is not side by side stereo
		const FViewInfo& TranslucentViewport = (View.bIsMobileMultiViewEnabled) ? Views[0] : View;
		RHICmdList.SetViewport(TranslucentViewport.ViewRect.Min.X, TranslucentViewport.ViewRect.Min.Y, 0.0f, TranslucentViewport.ViewRect.Max.X, TranslucentViewport.ViewRect.Max.Y, 1.0f);

		// Default clear value for a SceneColor is (0,0,0,0), after this passs will blend inverse opacity into final render target with an 1-SrcAlpha op
		// to make this blending work untouched pixels must have alpha = 1
		DrawClearQuad(RHICmdList, FLinearColor(0, 0, 0, 1));
		// GPUCULL_TODO: View.ParallelMeshDrawCommandPasses[EMeshPass::MobileInverseOpacity].BuildRenderingCommands(GraphBuilder, Scene->GPUScene);

		RHICmdList.NextSubpass();
		if (ShouldRenderTranslucency(ETranslucencyPass::TPT_AllTranslucency) && View.ShouldRenderView())
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::MobileInverseOpacity].DispatchDraw(nullptr, RHICmdList);
			View.ParallelMeshDrawCommandPasses[EMeshPass::MobileInverseOpacity].HasAnyDraw();
		}
	});
}

// This pass is registered only when we render to scene capture, see UpdateSceneCaptureContentMobile_RenderThread()
FMeshPassProcessor* CreateMobileInverseOpacityPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
	PassDrawRenderState.SetBlendState(TStaticBlendState<CW_ALPHA, BO_Add, BF_Zero, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

	const FMobileBasePassMeshProcessor::EFlags Flags = 
		FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil | 
		FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;

	return new(FMemStack::Get()) FMobileBasePassMeshProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, ETranslucencyPass::TPT_AllTranslucency);
}