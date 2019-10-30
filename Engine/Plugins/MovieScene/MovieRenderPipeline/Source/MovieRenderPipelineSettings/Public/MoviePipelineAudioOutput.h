// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MoviePipelineOutput.h"
#include "MoviePipelineAudioOutput.generated.h"

UCLASS(BlueprintType, meta=(MovieRenderPipelineDisplayName="Audio"))
class MOVIERENDERPIPELINESETTINGS_API UMoviePipelineAudioOutput : public UMoviePipelineOutput
{
	GENERATED_BODY()
public:
	UMoviePipelineAudioOutput()
		: bExportWavFile(true)
		, CachedExpectedDuration(0.f)
		, PrevRenderEveryTickValue(false)
	{
	}

	// virtual FText GetDisplayText() const override { return NSLOCTEXT("MovieRenderPipeline", "AudioSettingDisplayName", "Audio"); }
	virtual void OnInitializedForPipelineImpl(UMoviePipeline* InPipeline) override;
	virtual void OnPipelineFinishedImpl() override;
	virtual void OnPostTickImpl() override;
	virtual void OnFrameProductionStartImpl() override;
	virtual void OnShotFinishedImpl(const FMoviePipelineShotCache& Shot) override;
public:
	/** If true audio will be written to a separate wave file in addition to being embedded into any supported containers */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movie Render Pipeline")
	bool bExportWavFile;

private:
	float CachedExpectedDuration;
	int32 PrevRenderEveryTickValue;
	FString PrevAudioDevicePlatform;
	float PrevUnfocusedAudioMultiplier;
};