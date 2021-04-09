// Copyright Epic Games, Inc. All Rights Reserved.

#include "DisplayClusterViewportConfiguration.h"

#include "DisplayClusterConfigurationStrings.h"
#include "DisplayClusterViewportConfigurationBase.h"
#include "DisplayClusterViewportConfigurationICVFX.h"

#include "DisplayClusterRootActor.h"

#include "Render/Viewport/DisplayClusterViewport.h"
#include "Render/Viewport/DisplayClusterViewportManager.h"
#include "Render/Viewport/DisplayClusterViewportManager_PostProcess.h"

#include "Render/Viewport/RenderFrame/DisplayClusterRenderFrameSettings.h"

#include "DisplayClusterConfigurationTypes.h"
#include "DisplayClusterConfigurationTypes_Viewport.h"

#include "Render/IPDisplayClusterRenderManager.h"
#include "Misc/DisplayClusterGlobals.h"

///////////////////////////////////////////////////////////////////
// FDisplayClusterViewportConfiguration
///////////////////////////////////////////////////////////////////

void FDisplayClusterViewportConfiguration::SetRootActor(ADisplayClusterRootActor* InRootActorPtr)
{
	check(IsInGameThread());
	check(InRootActorPtr);

	// Update root actor reference:
	RootActorRef.ResetSceneActor();
	RootActorRef.SetSceneActor(InRootActorPtr);
}

ADisplayClusterRootActor* FDisplayClusterViewportConfiguration::GetRootActor() const
{
	AActor* ActorPtr = RootActorRef.GetOrFindSceneActor();
	if (ActorPtr)
	{
		return static_cast<ADisplayClusterRootActor*>(ActorPtr);
	}

	return nullptr;
}

bool FDisplayClusterViewportConfiguration::UpdateConfiguration(EDisplayClusterRenderFrameMode InRenderMode, const FString& InClusterNodeId)
{
	check(InRenderMode != EDisplayClusterRenderFrameMode::PreviewMono)

	ADisplayClusterRootActor* RootActor = GetRootActor();
	if (RootActor)
	{
		const UDisplayClusterConfigurationData* ConfigurationData = RootActor->GetConfigData();
		if (ConfigurationData)
		{
			TArray<FString> RenderNodes;
			RenderNodes.Add(InClusterNodeId);

			ImplUpdateConfiguration(RenderNodes, *RootActor, *ConfigurationData);

			ImplUpdateConfiguration_PostProcess(InClusterNodeId, *ConfigurationData);

			// Set current rendering mode
			RenderFrameSettings.RenderMode = InRenderMode;

			return true;
		}
	}

	return false;
}

void FDisplayClusterViewportConfiguration::ImplUpdateConfiguration(const TArray<FString>& InClusterNodeIds, ADisplayClusterRootActor& RootActor, const UDisplayClusterConfigurationData& ConfigurationData)
{
	// Update and create Base viewports
	FDisplayClusterViewportConfigurationBase BaseViewports(ViewportManager, RootActor, ConfigurationData);
	BaseViewports.Update(InClusterNodeIds);

	// Update ICBFX internal viewports and resources
	FDisplayClusterViewportConfigurationICVFX ConfigurationICVFX(ViewportManager, RootActor, ConfigurationData);
	ConfigurationICVFX.Update();

	// Hide root actor components for all viewports
	TSet<FPrimitiveComponentId> RootActorHidePrimitivesList;
	if (RootActor.GetHiddenInGamePrimitives(RootActorHidePrimitivesList))
	{
		for (FDisplayClusterViewport* ViewportIt : ViewportManager.ImplGetViewports())
		{
			ViewportIt->VisibilitySettings.SetRootActorHideList(RootActorHidePrimitivesList);
		}
	}

	ImplUpdateRenderFrameConfiguration(*(RootActor.GetRenderFrameSettings()));
}

void FDisplayClusterViewportConfiguration::ImplUpdateRenderFrameConfiguration(const UDisplayClusterConfigurationRenderFrame& InRenderFrameConfiguration)
{
	// Some frame postprocess require additional render targetable resources
	RenderFrameSettings.bShouldUseAdditionalFrameTargetableResource = ViewportManager.PostProcessManager->ShouldUseAdditionalFrameTargetableResource_PostProcess();

	// Multiply all downscale ratio inside all viewports settings for whole cluster
	RenderFrameSettings.ClusterRenderTargetRatioMult = InRenderFrameConfiguration.ClusterRenderTargetRatioMult;

	// Multiply all buffer ratios for whole cluster by this value
	RenderFrameSettings.ClusterBufferRatioMult = InRenderFrameConfiguration.ClusterBufferRatioMult;

	// Allow warpblend render
	RenderFrameSettings.bAllowWarpBlend = InRenderFrameConfiguration.bAllowWarpBlend;

	// Performance: Allow merge multiple viewports on single RTT with atlasing (required for bAllowViewFamilyMergeOptimization)
	RenderFrameSettings.bAllowRenderTargetAtlasing = InRenderFrameConfiguration.bAllowRenderTargetAtlasing;

	// Performance: Allow viewfamily merge optimization (render multiple viewports contexts within single family)
	// [not implemented yet] Experimental
	switch (InRenderFrameConfiguration.ViewFamilyMode)
	{
	case EDisplayClusterConfigurationRenderFamilyMode::AllowMergeForGroups:
		RenderFrameSettings.ViewFamilyMode = EDisplayClusterRenderFamilyMode::AllowMergeForGroups;
		break;
	case EDisplayClusterConfigurationRenderFamilyMode::AllowMergeForGroupsAndStereo:
		RenderFrameSettings.ViewFamilyMode = EDisplayClusterRenderFamilyMode::AllowMergeForGroupsAndStereo;
		break;
	case EDisplayClusterConfigurationRenderFamilyMode::MergeAnyPossible:
		RenderFrameSettings.ViewFamilyMode = EDisplayClusterRenderFamilyMode::MergeAnyPossible;
		break;
	case EDisplayClusterConfigurationRenderFamilyMode::None:
	default:
		RenderFrameSettings.ViewFamilyMode = EDisplayClusterRenderFamilyMode::None;
		break;
	}

	// Performance: Allow change global MGPU settings
	switch (InRenderFrameConfiguration.MultiGPUMode)
	{
	case EDisplayClusterConfigurationRenderMGPUMode::None:
		RenderFrameSettings.MultiGPUMode = EDisplayClusterMultiGPUMode::None;
		break;
	case EDisplayClusterConfigurationRenderMGPUMode::Optimized_DisabledLockSteps:
		RenderFrameSettings.MultiGPUMode = EDisplayClusterMultiGPUMode::Optimized_DisabledLockSteps;
		break;
	case EDisplayClusterConfigurationRenderMGPUMode::Optimized_EnabledLockSteps:
		RenderFrameSettings.MultiGPUMode = EDisplayClusterMultiGPUMode::Optimized_EnabledLockSteps;
		break;
	case EDisplayClusterConfigurationRenderMGPUMode::Enabled:
	default:
		RenderFrameSettings.MultiGPUMode = EDisplayClusterMultiGPUMode::Enabled;
		break;
	};

	// Performance: Allow to use parent ViewFamily from parent viewport 
	// (icvfx has child viewports: lightcard and chromakey with prj_view matrices copied from parent viewport. May sense to use same viewfamily?)
	// [not implemented yet] Experimental
	RenderFrameSettings.bShouldUseParentViewportRenderFamily = InRenderFrameConfiguration.bShouldUseParentViewportRenderFamily;
}

void FDisplayClusterViewportConfiguration::ImplUpdateConfiguration_PostProcess(const FString& InClusterNodeId, const UDisplayClusterConfigurationData& ConfigurationData)
{
	const UDisplayClusterConfigurationClusterNode* ClusterNode = ConfigurationData.GetClusterNode(InClusterNodeId);

	if (ClusterNode)
	{
		// Now post-process dynamic re-configuration not implemented
		static bool bInitialized = false;
		if (!bInitialized)
		{
			bInitialized = true;

			// Initialize all local postprocess operations
			TMap<FString, IPDisplayClusterRenderManager::FDisplayClusterPPInfo> Postprocess = GDisplayCluster->GetPrivateRenderMgr()->GetRegisteredPostprocessOperations();

			for (const TPair<FString, FDisplayClusterConfigurationPostprocess>& PostprocessIt : ClusterNode->Postprocess)
			{
				if (Postprocess.Contains(PostprocessIt.Value.Type))
				{
					Postprocess[PostprocessIt.Value.Type].Operation->InitializePostProcess(ViewportManager, PostprocessIt.Value.Parameters);
				}
			}
		}
	}
}

#if WITH_EDITOR
bool FDisplayClusterViewportConfiguration::UpdatePreviewConfiguration(class UDisplayClusterConfigurationViewportPreview* PreviewConfiguration)
{
	check(PreviewConfiguration);
	check(PreviewConfiguration->bEnable);

	ADisplayClusterRootActor* RootActor = GetRootActor();
	if (RootActor)
	{
		const UDisplayClusterConfigurationData* ConfigurationData = RootActor->GetConfigData();
		if (ConfigurationData)
		{
			TArray<FString> RenderNodes;
			if (PreviewConfiguration->PreviewNodeId == DisplayClusterConfigurationStrings::gui::preview::PreviewNodeAll)
			{
				// Collect all nodes from cluster
				for (const TPair<FString, UDisplayClusterConfigurationClusterNode*>& It : ConfigurationData->Cluster->Nodes)
				{
					RenderNodes.Add(It.Key);
				}
			}
			else
			{
				RenderNodes.Add(PreviewConfiguration->PreviewNodeId);
			}

			ImplUpdateConfiguration(RenderNodes, *RootActor, *ConfigurationData);

			// Downscale resources with PreviewDownscaleRatio
			RenderFrameSettings.PreviewRenderTargetRatioMult = PreviewConfiguration->PreviewRenderTargetRatioMult;
			RenderFrameSettings.RenderMode = EDisplayClusterRenderFrameMode::PreviewMono;

			return true;
		}
	}

	return false;
}
#endif
