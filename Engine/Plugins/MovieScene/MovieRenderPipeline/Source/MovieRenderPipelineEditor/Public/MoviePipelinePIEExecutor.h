// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MoviePipelineLinearExecutor.h"
#include "MoviePipelinePIEExecutor.generated.h"

class UMoviePipeline;

/**
* This is the implementation responsible for executing the rendering of
* multiple movie pipelines in the currently running Editor process. This
* involves launching a Play in Editor session for each Movie Pipeline to
* process.
*/
UCLASS(Blueprintable)
class MOVIERENDERPIPELINEEDITOR_API UMoviePipelinePIEExecutor : public UMoviePipelineLinearExecutorBase
{
	GENERATED_BODY()
	
public:
	UMoviePipelinePIEExecutor()
		: UMoviePipelineLinearExecutorBase()
	{
	}

protected:
	virtual void Start(UMovieRenderPipelineConfig* InConfig, const int32 InConfigIndex, const int32 InNumConfigs) override;

private:
	/** Called when PIE finishes booting up and it is safe for us to spawn an object into that world. */
	void OnPIEStartupFinished(bool);
	/** Called when the instance of the pipeline in the PIE world has finished. */
	void OnPIEMoviePipelineFinished(UMoviePipeline* InMoviePipeline);
	/** Called a short period of time after OnPIEMoviePipelineFinished to allow Editor the time to fully close PIE before we make a new request. */
	void DelayedFinishNotification();

	/** Gets the title for the Window. Used to show progress in the title. */
	FText GetWindowTitle(const int32 InConfigIndex, const int32 InNumConfigs) const;


private:
	/** Instance of the Pipeline that exists in the world that is currently processing (if any) */
	UPROPERTY(Transient)
	UMoviePipeline* ActiveMoviePipeline;

	/** Instance of the Config we're supposed to be working on */
	UPROPERTY(Transient)
	UMovieRenderPipelineConfig* ActiveConfig;
};