// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "MovieRenderPipelineSettings.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelineNewProcessExecutor.h"
#include "MoviePipeline.h"

UMovieRenderPipelineProjectSettings::UMovieRenderPipelineProjectSettings()
{
	DefaultLocalExecutor = UMoviePipelinePIEExecutor::StaticClass();
	DefaultRemoteExecutor = UMoviePipelineNewProcessExecutor::StaticClass();
	DefaultPipeline = UMoviePipeline::StaticClass();
}