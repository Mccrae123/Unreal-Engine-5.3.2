// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MoviePipelineRenderPass.h"
#include "MovieRenderPipelineDataTypes.h"
#include "SceneTypes.h"
#include "SceneView.h"
#include "UObject/GCObject.h"
#include "MoviePipelineDeferredPasses.generated.h"

class UTextureRenderTarget2D;
struct FImageOverlappedAccumulator;
class FSceneViewFamily;
class FSceneView;

UCLASS(BlueprintType)
class MOVIERENDERPIPELINERENDERPASSES_API UMoviePipelineDeferredPassBase : public UMoviePipelineRenderPass
{
	GENERATED_BODY()

protected:
	
	// UMoviePipelineRenderPass API
	virtual void GetRequiredEnginePassesImpl(TSet<FMoviePipelinePassIdentifier>& RequiredEnginePasses) override;
	virtual void GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses) override;
	virtual void SetupImpl(TArray<TSharedPtr<MoviePipeline::FMoviePipelineEnginePass>>& InEnginePasses, const MoviePipeline::FMoviePipelineRenderPassInitSettings& InPassInitSettings) override;
	// ~UMovieRenderPassAPI

	virtual FText GetDisplayText() const override { return NSLOCTEXT("MovieRenderPipeline", "DeferredBasePassSettingDisplayName", "Deferred Rendering"); }


	void OnBackbufferSampleReady(TArray<FLinearColor> InPixelData, FMoviePipelineRenderPassMetrics InSampleState);
	void OnSetupView(FSceneViewFamily& InViewFamily, FSceneView& InView, const FMoviePipelineRenderPassMetrics& InSampleState);

private:
	/** List of passes by name that we should output. */
	TArray<FString> DesiredOutputPasses;

	bool bAccumulateAlpha;

	/** One accumulator per sample source being saved. */
	TArray<TSharedPtr<FImageOverlappedAccumulator, ESPMode::ThreadSafe>> ImageTileAccumulator;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FMoviePipelineSampleReady, TArray<FLinearColor>, FMoviePipelineRenderPassMetrics);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FMoviePipelineSetupView, FSceneViewFamily&, FSceneView&, const FMoviePipelineRenderPassMetrics&);

namespace MoviePipeline
{
	struct FDeferredRenderEnginePass : public FMoviePipelineEnginePass, FGCObject
	{
		FDeferredRenderEnginePass()
			: FMoviePipelineEnginePass(FMoviePipelinePassIdentifier(TEXT("MainDeferredPass")))
		{
		}

		virtual void Setup(TWeakObjectPtr<UMoviePipeline> InOwningPipeline, const FMoviePipelineRenderPassInitSettings& InInitSettings) override;
		virtual void RenderSample_GameThread(const FMoviePipelineRenderPassMetrics& InSampleState) override;
		virtual void Teardown() override;

		// FGCObject Interface
		virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
		// ~FGCObject Interface

		FMoviePipelineSampleReady BackbufferReadyDelegate;
		FMoviePipelineSetupView SetupViewDelegate;

	protected:
		FSceneView* CalcSceneView(FSceneViewFamily* ViewFamily, const FMoviePipelineRenderPassMetrics& InSampleState);

	protected:
		FSceneViewStateReference ViewState;

		TWeakObjectPtr<UTextureRenderTarget2D> TileRenderTarget;
	};
}