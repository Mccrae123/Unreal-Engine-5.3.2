// Copyright Epic Games, Inc. All Rights Reserved.
#include "MoviePipelineInProcessExecutor.h"
#include "MoviePipeline.h"
#include "Engine/World.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineInProcessExecutorSettings.h"
#include "Kismet/KismetSystemLibrary.h"

#define LOCTEXT_NAMESPACE "MoviePipelineInProcessExecutor"
void UMoviePipelineInProcessExecutor::Start(const UMoviePipelineExecutorJob* InJob)
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UMoviePipelineInProcessExecutor::OnMapLoadFinished);

	// Force the engine into fixed timestep mode. There may be a global delay on the job that passes a fixed
	// number of frames, so we want those frames to always pass the same amount of time for determinism. 
	ULevelSequence* LevelSequence = CastChecked<ULevelSequence>(InJob->Sequence.TryLoad());
	if (LevelSequence)
	{
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(InJob->GetConfiguration()->GetEffectiveFrameRate(LevelSequence).AsInterval());
	}
}

void UMoviePipelineInProcessExecutor::OnMapLoadFinished(UWorld* NewWorld)
{
	// NewWorld can be null if a world is being destroyed.
	if (!NewWorld)
	{
		FCoreDelegates::OnBeginFrame.RemoveAll(this);
		return;
	}
	
	// Stop listening for map load until we're done and know we want to start the next config.
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	
	UMoviePipelineExecutorJob* CurrentJob = Queue->GetJobs()[CurrentPipelineIndex];

	ActiveMoviePipeline = NewObject<UMoviePipeline>(NewWorld, TargetPipelineClass);

	// We allow users to set a multi-frame delay before we actually run the Initialization function and start thinking.
	// This solves cases where there are engine systems that need to finish loading before we do anything.
	const UMoviePipelineInProcessExecutorSettings* ExecutorSettings = GetDefault<UMoviePipelineInProcessExecutorSettings>();

	// We tick each frame to update the Window Title, and kick off latent pipeling initialization.
	FCoreDelegates::OnBeginFrame.AddUObject(this, &UMoviePipelineInProcessExecutor::OnTick);
	
	// Listen for when the pipeline thinks it has finished.
	ActiveMoviePipeline->OnMoviePipelineFinished().AddUObject(this, &UMoviePipelineInProcessExecutor::OnMoviePipelineFinished);

	// Wait until we actually recieved the right map and created the pipeline before saying that we're actively rendering
	bIsRendering = true;
	
	if (ExecutorSettings->InitialDelayFrameCount == 0)
	{
		ActiveMoviePipeline->Initialize(Queue->GetJobs()[CurrentPipelineIndex]);
		RemainingInitializationFrames = -1;
	}
	else
	{
		RemainingInitializationFrames = ExecutorSettings->InitialDelayFrameCount;	
	}
}

void UMoviePipelineInProcessExecutor::OnTick()
{
	if (RemainingInitializationFrames >= 0)
	{
		if (RemainingInitializationFrames == 0)
		{
			ActiveMoviePipeline->Initialize(Queue->GetJobs()[CurrentPipelineIndex]);
		}

		RemainingInitializationFrames--;
	}

	FText WindowTitle = GetWindowTitle();
	UKismetSystemLibrary::SetWindowTitle(WindowTitle);
}

void UMoviePipelineInProcessExecutor::OnMoviePipelineFinished(UMoviePipeline* InMoviePipeline)
{
	FCoreDelegates::OnBeginFrame.RemoveAll(this);
	UMoviePipeline* MoviePipeline = ActiveMoviePipeline;

	if (ActiveMoviePipeline)
	{
		// Unsubscribe in the event that it gets called twice we don't have issues.
		ActiveMoviePipeline->OnMoviePipelineFinished().RemoveAll(this);
	}

	// Null these out now since OnIndividualPipelineFinished might invoke something that causes a GC
	// and we want them to go away with the GC.
	ActiveMoviePipeline = nullptr;

	// Now that another frame has passed and we should be OK to start another PIE session, notify our owner.
	OnIndividualPipelineFinished(MoviePipeline);
}
#undef LOCTEXT_NAMESPACE // "MoviePipelineInProcessExecutor"
