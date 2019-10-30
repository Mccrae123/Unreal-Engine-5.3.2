// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/Object.h"
#include "MovieRenderPipelineDataTypes.h"
#include "MoviePipelineSetting.h"
#include "ImageWriteStream.h"
#include "MoviePipelineRenderPass.generated.h"

UCLASS(Blueprintable, Abstract)
class MOVIERENDERPIPELINECORE_API UMoviePipelineRenderPass : public UMoviePipelineSetting
{
	GENERATED_BODY()
public:
	void Setup(const FMoviePipelineRenderPassInitSettings& InInitSettings, TSharedRef<FImagePixelPipe, ESPMode::ThreadSafe> InOutputPipe)
	{
		InitSettings = InInitSettings;
		SetupImpl(InInitSettings, InOutputPipe);
	}

	void CaptureFrame(const FMoviePipelineRenderPassMetrics& OutputFrameMetrics)
	{
		CaptureFrameImpl(OutputFrameMetrics);
	}


	void GetFrameData(MoviePipeline::FOutputFrameData& OutFrameData)
	{
		GetFrameDataImpl(OutFrameData);
	}

	void Teardown()
	{
		TeardownImpl();
	}

	void GatherOutputPasses(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses)
	{
		GatherOutputPassesImpl(ExpectedRenderPasses);
	}

protected:
	virtual void SetupImpl(const FMoviePipelineRenderPassInitSettings& InInitSettings, TSharedRef<FImagePixelPipe, ESPMode::ThreadSafe> InOutputPipe) {  }

	virtual void CaptureFrameImpl(const FMoviePipelineRenderPassMetrics& OutputFrameMetrics) { }

	virtual void GetFrameDataImpl(MoviePipeline::FOutputFrameData& OutFrameData) { }

	virtual void TeardownImpl() {}

	virtual void GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses) {}
		
protected:
	UPROPERTY()
	FMoviePipelineRenderPassInitSettings InitSettings;
};