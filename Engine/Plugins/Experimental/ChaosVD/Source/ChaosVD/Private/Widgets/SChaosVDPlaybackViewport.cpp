// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/SChaosVDPlaybackViewport.h"

#include "ChaosVDPlaybackController.h"
#include "ChaosVDScene.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SChaosVDTimelineWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ChaosVisualDebugger"

SChaosVDPlaybackViewport::~SChaosVDPlaybackViewport()
{
	LevelViewportClient->Viewport = nullptr;
	LevelViewportClient.Reset();
}

TSharedPtr<FLevelEditorViewportClient> SChaosVDPlaybackViewport::CreateViewportClient() const
{
	TSharedPtr<FLevelEditorViewportClient> NewViewport = MakeShareable(new FLevelEditorViewportClient(TSharedPtr<class SLevelViewport>()));

	NewViewport->SetAllowCinematicControl(false);
	
	NewViewport->bSetListenerPosition = false;
	NewViewport->EngineShowFlags = FEngineShowFlags(ESFIM_Editor);
	NewViewport->LastEngineShowFlags = FEngineShowFlags(ESFIM_Editor);
	NewViewport->ViewportType = LVT_Perspective;
	NewViewport->bDrawAxes = true;
	NewViewport->bDisableInput = false;
	NewViewport->VisibilityDelegate.BindLambda([] {return true; });

	return NewViewport;
}

void SChaosVDPlaybackViewport::Construct(const FArguments& InArgs, const UWorld* DefaultWorld, TWeakPtr<FChaosVDPlaybackController> InPlaybackController)
{
	ensure(DefaultWorld);
	ensure(InPlaybackController.IsValid());

	LevelViewportClient = CreateViewportClient();

	ViewportWidget = SNew(SViewport)
		.RenderDirectlyToWindow(false)
		.IsEnabled(FSlateApplication::Get().GetNormalExecutionAttribute())
		.EnableGammaCorrection(false)
		.EnableBlending(false);

	SceneViewport = MakeShareable(new FSceneViewport(LevelViewportClient.Get(), ViewportWidget));

	LevelViewportClient->Viewport = SceneViewport.Get();

	ViewportWidget->SetViewportInterface(SceneViewport.ToSharedRef());
	
	// Default to the base map
	LevelViewportClient->SetReferenceToWorldContext(*GEngine->GetWorldContextFromWorld(DefaultWorld));

	ChildSlot
	[
		// 3D Viewport
		SNew(SVerticalBox)
		+SVerticalBox::Slot()
		.FillHeight(0.9f)
		[
			ViewportWidget.ToSharedRef()
		]
		// Playback controls
		// TODO: Now that the tool is In-Editor, see if we can/is worth use the Sequencer widgets
		// instead of these custom ones
		+SVerticalBox::Slot()
		.Padding(16.0f, 16.0f, 16.0f, 16.0f)
		.FillHeight(0.1f)
		[
			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Justification(ETextJustify::Center)
				.Text(LOCTEXT("PlaybackViewportWidgetPhysicsFramesLabel", "Game Frames" ))
			]
			+SVerticalBox::Slot()
			[
				SAssignNew(GameFramesTimelineWidget, SChaosVDTimelineWidget)
				.HidePlayStopButtons(false)
				.OnFrameChanged_Raw(this, &SChaosVDPlaybackViewport::OnFrameSelectionUpdated)
				.MaxFrames(0)
			]
		]	
	];

	RegisterNewController(InPlaybackController);
}

void SChaosVDPlaybackViewport::HandlePlaybackControllerDataUpdated(TWeakPtr<FChaosVDPlaybackController> InController)
{
	if (PlaybackController != InController)
	{
		RegisterNewController(InController);
	}

	const TSharedPtr<FChaosVDPlaybackController> ControllerSharedPtr = PlaybackController.Pin();
	if (ControllerSharedPtr.IsValid() && ControllerSharedPtr->IsRecordingLoaded())
	{
		if (const FChaosVDTrackInfo* TrackInfo = ControllerSharedPtr->GetTrackInfo(EChaosVDTrackType::Game, FChaosVDPlaybackController::GameTrackID))
		{
			// Max is inclusive and we use this to request as the index on the recorded frames/steps arrays so we need to -1 to the available frames/steps
			GameFramesTimelineWidget->UpdateMinMaxValue(0, TrackInfo->MaxFrames != INDEX_NONE ? TrackInfo->MaxFrames -1  : 0);
		}
	}
	else
	{
		GameFramesTimelineWidget->UpdateMinMaxValue(0, 0);
		GameFramesTimelineWidget->ResetTimeline();
	}

	LevelViewportClient->bNeedsRedraw = true;
}

void SChaosVDPlaybackViewport::HandleControllerTrackFrameUpdated(TWeakPtr<FChaosVDPlaybackController> InController, const FChaosVDTrackInfo* UpdatedTrackInfo, FGuid InstigatorGuid)
{
	if (InstigatorGuid == GetInstigatorID())
	{
		// Ignore the update if we initiated it
		return;
	}

	GameFramesTimelineWidget->SetCurrentTimelineFrame(UpdatedTrackInfo->CurrentFrame, EChaosVDSetTimelineFrameFlags::None);
}

void SChaosVDPlaybackViewport::OnPlaybackSceneUpdated()
{
	LevelViewportClient->bNeedsRedraw = true;
}

void SChaosVDPlaybackViewport::RegisterNewController(TWeakPtr<FChaosVDPlaybackController> NewController)
{
	if (PlaybackController != NewController)
	{
		if (const TSharedPtr<FChaosVDPlaybackController> CurrentPlaybackControllerPtr = PlaybackController.Pin())
		{
			if (TSharedPtr<FChaosVDScene> ScenePtr = CurrentPlaybackControllerPtr->GetControllerScene().Pin())
			{
				ScenePtr->OnSceneUpdated().RemoveAll(this);
			}
		}

		FChaosVDPlaybackControllerObserver::RegisterNewController(NewController);

		if (const TSharedPtr<FChaosVDPlaybackController> NewPlaybackControllerPtr = PlaybackController.Pin())
		{
			if (TSharedPtr<FChaosVDScene> ScenePtr = NewPlaybackControllerPtr->GetControllerScene().Pin())
			{
				ScenePtr->OnSceneUpdated().AddRaw(this, &SChaosVDPlaybackViewport::OnPlaybackSceneUpdated);
			}
		}
	}
}

void SChaosVDPlaybackViewport::OnFrameSelectionUpdated(int32 NewFrameIndex) const
{
	if (const TSharedPtr<FChaosVDPlaybackController> PlaybackControllerPtr = PlaybackController.Pin())
	{
		constexpr int32 StepNumber = 0;
		PlaybackControllerPtr->GoToTrackFrame(GetInstigatorID(), EChaosVDTrackType::Game, FChaosVDPlaybackController::GameTrackID, NewFrameIndex, StepNumber);

		LevelViewportClient->bNeedsRedraw = true;
	}
}

#undef LOCTEXT_NAMESPACE
