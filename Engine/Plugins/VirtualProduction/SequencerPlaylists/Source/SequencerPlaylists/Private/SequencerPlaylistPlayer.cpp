// Copyright Epic Games, Inc. All Rights Reserved.

#include "SequencerPlaylistPlayer.h"
#include "ISequencer.h"
#include "Misc/QualifiedFrameTime.h"
#include "MovieSceneFwd.h"
#include "SequencerPlaylist.h"
#include "SequencerPlaylistItem.h"
#include "SequencerPlaylistsLog.h"
#include "SequencerPlaylistsModule.h"

#include "ILevelSequenceEditorToolkit.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Recorder/TakeRecorder.h"
#include "ScopedTransaction.h"
#include "TakePreset.h"
#include "TakeRecorderSettings.h"

#define LOCTEXT_NAMESPACE "SequencerPlaylists"

USequencerPlaylistPlayer::USequencerPlaylistPlayer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		UTakeRecorder::OnRecordingInitialized().AddUObject(this, &USequencerPlaylistPlayer::OnTakeRecorderInitialized);
		if (UTakeRecorder* ExistingRecorder = UTakeRecorder::GetActiveRecorder())
		{
			OnTakeRecorderInitialized(ExistingRecorder);
		}
	}
}


void USequencerPlaylistPlayer::BeginDestroy()
{
	Super::BeginDestroy();

	UTakeRecorder::OnRecordingInitialized().RemoveAll(this);

	if (UTakeRecorder* BoundRecorder = WeakRecorder.Get())
	{
		BoundRecorder->OnRecordingStarted().RemoveAll(this);
		BoundRecorder->OnRecordingStopped().RemoveAll(this);
	}
}


bool USequencerPlaylistPlayer::PlayItem(USequencerPlaylistItem* Item)
{
	if (!Item)
	{
		return false;
	}

	EnterUnboundedPlayIfNotRecording();

	FScopedTransaction Transaction(FText::Format(LOCTEXT("PlayItemTransaction", "Trigger playback of {0}"), Item->GetDisplayName()));
	return GetCheckedItemPlayer(Item)->Play(Item);
}


bool USequencerPlaylistPlayer::StopItem(USequencerPlaylistItem* Item)
{
	if (!Item)
	{
		return false;
	}

	FScopedTransaction Transaction(FText::Format(LOCTEXT("StopItemTransaction", "Stop playback of {0}"), Item->GetDisplayName()));
	return GetCheckedItemPlayer(Item)->Stop(Item);
}


bool USequencerPlaylistPlayer::ResetItem(USequencerPlaylistItem* Item)
{
	if (!Item)
	{
		return false;
	}

	FScopedTransaction Transaction(FText::Format(LOCTEXT("ResetItemTransaction", "Reset playback of {0}"), Item->GetDisplayName()));
	return GetCheckedItemPlayer(Item)->Reset(Item);
}

namespace UE::Private::PlaylistPlayer
{

TOptional<TRange<double>> ComputeNewRange(TSharedPtr<ISequencer>& Sequencer)
{
	FAnimatedRange Range = Sequencer->GetViewRange();
	UMovieSceneSequence* Sequence  = Sequencer->GetRootMovieSceneSequence();
	if (Sequence)
	{
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (MovieScene)
		{
			FFrameRate FrameRate = MovieScene->GetTickResolution();
			FQualifiedFrameTime GlobalTime = Sequencer->GetGlobalTime();
			FFrameTime CurrentFrameTime = GlobalTime.ConvertTo(FrameRate);
			double CurrentTimeSeconds = FrameRate.AsSeconds(CurrentFrameTime) + 0.5f;
			CurrentTimeSeconds = CurrentTimeSeconds > Range.GetUpperBoundValue() ? CurrentTimeSeconds : Range.GetUpperBoundValue();
			TRange<double> NewRange(Range.GetLowerBoundValue(), CurrentTimeSeconds);
			return NewRange;
		}
	}
	return {};
}

void AdjustMovieSceneRangeForPlay(TSharedPtr<ISequencer>& Sequencer)
{
	check(Sequencer);

	TOptional<TRange<double>> NewRange = ComputeNewRange(Sequencer);
	if (NewRange)
	{
		Sequencer->SetViewRange(*NewRange, EViewRangeInterpolation::Immediate);
		Sequencer->SetClampRange(Sequencer->GetViewRange());
	}
}

FFrameTime GetFrameTime(UMovieScene* MovieScene, FQualifiedFrameTime GlobalTime)
{
	FFrameRate FrameRate = MovieScene->GetTickResolution();
	return GlobalTime.ConvertTo(FrameRate);
}

void SetInfinitePlayRange(TSharedPtr<ISequencer>& Sequencer)
{
	UMovieSceneSequence* Sequence = Sequencer->GetRootMovieSceneSequence();
	UMovieScene* MovieScene = Sequence->GetMovieScene();

	TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
	// Set infinite playback range when starting recording. Playback range will be clamped to the bounds of the sections at the completion of the recording
	MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Range.GetLowerBoundValue(), TNumericLimits<int32>::Max() - 1), false);
}

void StopPlaybackAndAdjustTime(TSharedPtr<ISequencer>& Sequencer)
{
	check(Sequencer);

	Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Stopped);
	UMovieSceneSequence* Sequence  = Sequencer->GetRootMovieSceneSequence();
	if (Sequence)
	{
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (MovieScene)
		{
			FFrameTime CurrentFrameTime = GetFrameTime(MovieScene, Sequencer->GetGlobalTime());
			TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
			// Set the playback range back to a closed interval.
			//
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Range.GetLowerBoundValue(), CurrentFrameTime.GetFrame()), false);
		}
	}
}

}

void USequencerPlaylistPlayer::Tick(float DeltaTime)
{
	TSharedPtr<ISequencer> Sequencer = GetSequencer();

	if (Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Paused)
	{
		return;
	}

	if (Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Stopped)
	{
		PlaylistTicker = {};
		UE::Private::PlaylistPlayer::StopPlaybackAndAdjustTime(Sequencer);
		return;
	}
	// Handle a tick
	UE::Private::PlaylistPlayer::AdjustMovieSceneRangeForPlay(Sequencer);
}

void USequencerPlaylistPlayer::EnterUnboundedPlayIfNotRecording()
{
	TSharedPtr<ISequencer> Sequencer = GetSequencer();
	UTakeRecorder* Recorder = UTakeRecorder::GetActiveRecorder();

	const bool bInRecorder = Recorder && Recorder->GetState() != ETakeRecorderState::Stopped;
	if (!PlaylistTicker || !bInRecorder)
	{
		PlaylistTicker = MakeUnique<FTickablePlaylist>(this);
	}

	if (PlaylistTicker && Sequencer->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
	{
		UE::Private::PlaylistPlayer::SetInfinitePlayRange(Sequencer);
		Sequencer->SetPlaybackStatus(EMovieScenePlayerStatus::Playing);

		// Tick once to set our playback range.
		Tick(0.0);
	}
}

bool USequencerPlaylistPlayer::PlayAll()
{
	if (!ensure(Playlist) || !Playlist->Items.Num())
	{
		return false;
	}

	EnterUnboundedPlayIfNotRecording();

	bool bResult = true;

	FScopedTransaction Transaction(LOCTEXT("PlayAllTransaction", "Trigger playback of all items"));
	for (USequencerPlaylistItem* Item : Playlist->Items)
	{
		bResult &= GetCheckedItemPlayer(Item)->Play(Item);
	}

	return bResult;
}


bool USequencerPlaylistPlayer::StopAll()
{
	if (!ensure(Playlist) || !Playlist->Items.Num())
	{
		return false;
	}

	UTakeRecorder* Recorder = UTakeRecorder::GetActiveRecorder();

	TSharedPtr<ISequencer> Sequencer = GetSequencer();
	const bool bInRecorder = Recorder && Recorder->GetState() != ETakeRecorderState::Stopped;
	if (!bInRecorder && Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing)
	{
		UE::Private::PlaylistPlayer::StopPlaybackAndAdjustTime(Sequencer);
	}

	bool bResult = true;

	FScopedTransaction Transaction(LOCTEXT("StopAllTransaction", "Stop playback of all items"));
	for (USequencerPlaylistItem* Item : Playlist->Items)
	{
		bResult &= GetCheckedItemPlayer(Item)->Stop(Item);
	}

	PlaylistTicker = {};
	return bResult;
}


bool USequencerPlaylistPlayer::ResetAll()
{
	if (!ensure(Playlist) || !Playlist->Items.Num())
	{
		return false;
	}

	bool bResult = true;

	FScopedTransaction Transaction(LOCTEXT("ResetAllTransaction", "Reset playback of all items"));
	for (USequencerPlaylistItem* Item : Playlist->Items)
	{
		bResult &= GetCheckedItemPlayer(Item)->Reset(Item);
	}

	return bResult;
}


TSharedPtr<ISequencer> USequencerPlaylistPlayer::GetSequencer()
{
	if (TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin())
	{
		return Sequencer;
	}

	ULevelSequence* RootSequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!RootSequence)
	{
		UTakePreset* Preset = UTakePreset::AllocateTransientPreset(GetDefault<UTakeRecorderUserSettings>()->LastOpenedPreset.Get());

		FScopedTransaction Transaction(LOCTEXT("CreateEmptyTake", "Create Empty Playlist Sequence"));

		Preset->Modify();
		Preset->CreateLevelSequence();

		RootSequence = Preset->GetLevelSequence();
	}

	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(RootSequence);
	IAssetEditorInstance* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(RootSequence, false);
	ILevelSequenceEditorToolkit* LevelSequenceEditor = static_cast<ILevelSequenceEditorToolkit*>(AssetEditor);

	TSharedPtr<ISequencer> Sequencer = LevelSequenceEditor ? LevelSequenceEditor->GetSequencer() : nullptr;
	if (!Sequencer)
	{
		UE_LOG(LogSequencerPlaylists, Error, TEXT("USequencerPlaylistPlayer::GetSequencer: Unable to open Sequencer for asset"));
	}
	else
	{
		Sequencer->OnCloseEvent().AddWeakLambda(this, [this](TSharedRef<ISequencer>) {
			// Existing item players invalidated by their sequencer going away.
			ItemPlayersByType.Empty();
		});
	}
	WeakSequencer = Sequencer;
	return Sequencer;
}


TSharedPtr<ISequencer> USequencerPlaylistPlayer::GetValidatedSequencer()
{
	TSharedPtr<ISequencer> Sequencer = GetSequencer();
	if (!Sequencer)
	{
		return nullptr;
	}

	ULevelSequence* RootSequence = Cast<ULevelSequence>(Sequencer->GetRootMovieSceneSequence());
	if (!RootSequence)
	{
		UE_LOG(LogSequencerPlaylists, Error, TEXT("USequencerPlaylistPlayer::GetValidatedSequencer: Unable to get root sequence"));
		return nullptr;
	}

	UMovieScene* RootScene = RootSequence->GetMovieScene();
	if (!RootScene)
	{
		// TODO: Seems like this may not be possible?
		UE_LOG(LogSequencerPlaylists, Error, TEXT("USequencerPlaylistPlayer::GetValidatedSequencer: Unable to get root scene"));
		return nullptr;
	}

	return Sequencer;
}


void USequencerPlaylistPlayer::OnTakeRecorderInitialized(UTakeRecorder* InRecorder)
{
	if (InRecorder)
	{
		if (UTakeRecorder* PrevRecorder = WeakRecorder.Get())
		{
			PrevRecorder->OnRecordingStarted().RemoveAll(this);
			PrevRecorder->OnRecordingStopped().RemoveAll(this);
		}

		InRecorder->OnRecordingStarted().AddUObject(this, &USequencerPlaylistPlayer::OnTakeRecorderStarted);
		InRecorder->OnRecordingStopped().AddUObject(this, &USequencerPlaylistPlayer::OnTakeRecorderStopped);
		WeakRecorder = InRecorder;
	}
}


void USequencerPlaylistPlayer::OnTakeRecorderStarted(UTakeRecorder* InRecorder)
{
	if (!ensure(Playlist))
	{
		return;
	}

	if (TSharedPtr<ISequencer> Sequencer = GetValidatedSequencer())
	{
		for (USequencerPlaylistItem* Item : Playlist->Items)
		{
			if (Item->bHoldAtFirstFrame)
			{
				GetCheckedItemPlayer(Item)->AddHold(Item);
			}
		}
	}
}


void USequencerPlaylistPlayer::OnTakeRecorderStopped(UTakeRecorder* InRecorder)
{
	if (!ensure(Playlist))
	{
		return;
	}

	// FIXME: Any sequences not already stopped end up a few frames too long; pass in explicit end frame?
	if (TSharedPtr<ISequencer> Sequencer = GetValidatedSequencer())
	{
		for (USequencerPlaylistItem* Item : Playlist->Items)
		{
			GetCheckedItemPlayer(Item)->Stop(Item);
		}
	}
}


TSharedPtr<ISequencerPlaylistItemPlayer> USequencerPlaylistPlayer::GetCheckedItemPlayer(USequencerPlaylistItem* Item)
{
	check(Item);

	TSubclassOf<USequencerPlaylistItem> ItemClass = Item->GetClass();
	if (TSharedRef<ISequencerPlaylistItemPlayer>* ExistingPlayer = ItemPlayersByType.Find(ItemClass))
	{
		return *ExistingPlayer;
	}

	TSharedPtr<ISequencer> Sequencer = GetValidatedSequencer();
	check(Sequencer.IsValid());

	TSharedPtr<ISequencerPlaylistItemPlayer> NewPlayer =
		static_cast<FSequencerPlaylistsModule&>(FSequencerPlaylistsModule::Get()).CreateItemPlayerForClass(ItemClass, Sequencer.ToSharedRef());
	check(NewPlayer);

	ItemPlayersByType.Add(ItemClass, NewPlayer.ToSharedRef());
	return NewPlayer;
}


#undef LOCTEXT_NAMESPACE
