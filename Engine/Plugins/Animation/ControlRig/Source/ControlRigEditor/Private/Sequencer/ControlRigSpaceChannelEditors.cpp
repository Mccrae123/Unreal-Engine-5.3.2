// Copyright Epic Games, Inc. All Rights Reserved.

#include "ControlRigSpaceChannelEditors.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSequenceEditor.h"
#include "MovieSceneEventUtils.h"
#include "Channels/MovieSceneChannelHandle.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "ISequencerChannelInterface.h"
#include "Widgets/SNullWidget.h"
#include "IKeyArea.h"
#include "ISequencer.h"
#include "SequencerSettings.h"
#include "MovieSceneCommonHelpers.h"
#include "GameFramework/Actor.h"
#include "EditorStyleSet.h"
#include "Styling/CoreStyle.h"
#include "KeyDrawParams.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/DoubleChannelCurveModel.h"
#include "Channels/FloatChannelCurveModel.h"
#include "Channels/IntegerChannelCurveModel.h"
#include "Channels/BoolChannelCurveModel.h"
#include "PropertyCustomizationHelpers.h"
#include "MovieSceneObjectBindingIDCustomization.h"
#include "MovieSceneObjectBindingIDPicker.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Framework/Application/MenuStack.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor/SceneOutliner/Private/SSocketChooser.h"
#include "EntitySystem/MovieSceneDecompositionQuery.h"
#include "EntitySystem/Interrogation/MovieSceneInterrogationLinker.h"
#include "EntitySystem/Interrogation/MovieSceneInterrogatedPropertyInstantiator.h"
#include "Systems/MovieScenePropertyInstantiator.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "MovieSceneSpawnableAnnotation.h"
#include "ISequencerModule.h"
#include "MovieSceneTracksComponentTypes.h"
#include "SRigSpacePickerWidget.h"
#include "Tools/ControlRigSnapper.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "MovieScene.h"
#include "Sequencer/MovieSceneControlRigSpaceChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "MovieSceneObjectBindingID.h"
#include "ControlRig.h"
#include "IControlRigObjectBinding.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ControlRigEditMode"


static FKeyHandle SequencerOpenSpaceSwitchDialog(UControlRig* ControlRig, TArray<FRigElementKey> SelectedControls,ISequencer* Sequencer, FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey, FFrameNumber Time)
{
	FKeyHandle Handle = FKeyHandle::Invalid();
	if (ControlRig == nullptr || Sequencer == nullptr)
	{
		return Handle;
	}

	TSharedRef<SRigSpacePickerWidget> PickerWidget =
	SNew(SRigSpacePickerWidget)
	.Hierarchy(ControlRig->GetHierarchy())
	.Controls(SelectedControls)
	.Title(LOCTEXT("PickSpace", "Pick Space"))
	.AllowDelete(false)
	.AllowReorder(false)
	.AllowAdd(false)
	.GetControlCustomization_Lambda([ControlRig](URigHierarchy*, const FRigElementKey& InControlKey)
	{
		return ControlRig->GetControlCustomization(InControlKey);
	})
	.OnActiveSpaceChanged_Lambda([&Handle,ControlRig,Sequencer,Channel,SectionToKey,Time,SelectedControls](URigHierarchy* RigHierarchy, const FRigElementKey& ControlKey, const FRigElementKey& SpaceKey)
	{
			
		Handle = FControlRigSpaceChannelHelpers::SequencerKeyControlRigSpaceChannel(ControlRig, Sequencer, Channel, SectionToKey, Time, RigHierarchy, ControlKey, SpaceKey);

	})
	.OnSpaceListChanged_Lambda([SelectedControls, ControlRig](URigHierarchy* InHierarchy, const FRigElementKey& InControlKey, const TArray<FRigElementKey>& InSpaceList)
	{
		check(SelectedControls.Contains(InControlKey));
				
		// update the settings in the control element
		if (FRigControlElement* ControlElement = InHierarchy->Find<FRigControlElement>(InControlKey))
		{
			FScopedTransaction Transaction(LOCTEXT("ControlChangeAvailableSpaces", "Edit Available Spaces"));

			InHierarchy->Modify();

			FRigControlElementCustomization ControlCustomization = *ControlRig->GetControlCustomization(InControlKey);
			ControlCustomization.AvailableSpaces = InSpaceList;
			ControlCustomization.RemovedSpaces.Reset();

			// remember  the elements which are in the asset's available list but removed by the user
			for (const FRigElementKey& AvailableSpace : ControlElement->Settings.Customization.AvailableSpaces)
			{
				if (!ControlCustomization.AvailableSpaces.Contains(AvailableSpace))
				{
					ControlCustomization.RemovedSpaces.Add(AvailableSpace);
				}
			}

			ControlRig->SetControlCustomization(InControlKey, ControlCustomization);
			InHierarchy->Notify(ERigHierarchyNotification::ControlSettingChanged, ControlElement);
		}

	});
	// todo: implement GetAdditionalSpacesDelegate to pull spaces from sequencer

	FReply Reply = PickerWidget->OpenDialog(true);
	if (Reply.IsEventHandled())
	{
		return Handle;
	}
	return FKeyHandle::Invalid();
}

FKeyHandle AddOrUpdateKey(FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey, FFrameNumber Time, ISequencer& Sequencer, const FGuid& InObjectBindingID, FTrackInstancePropertyBindings* PropertyBindings)
{
	FKeyHandle Handle = FKeyHandle::Invalid();
	if (UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(SectionToKey))
	{
		if (UControlRig* ControlRig = Section->GetControlRig())
		{
			FName ControlName = Section->FindControlNameFromSpaceChannel(Channel);
			if (ControlName != NAME_None)
			{
				if (FRigControlElement* Control = ControlRig->FindControl(ControlName))
				{
					TArray<FRigElementKey> Controls;
					FRigElementKey ControlKey = Control->GetKey();
					Controls.Add(ControlKey);
					FMovieSceneControlRigSpaceBaseKey ExistingValue, Value;
					using namespace UE::MovieScene;
					EvaluateChannel(Channel, Time, ExistingValue);
					Value = ExistingValue;
					URigHierarchy* RigHierarchy = ControlRig->GetHierarchy();
					Handle = SequencerOpenSpaceSwitchDialog(ControlRig, Controls, &Sequencer, Channel, SectionToKey, Time);
				}
			}
		}
	}
	return Handle;
}

bool CanCreateKeyEditor(const FMovieSceneControlRigSpaceChannel* Channel)
{
	return false; //mz todoo maybe change
}
TSharedRef<SWidget> CreateKeyEditor(const TMovieSceneChannelHandle<FMovieSceneControlRigSpaceChannel>& Channel, UMovieSceneSection* Section, const FGuid& InObjectBindingID, TWeakPtr<FTrackInstancePropertyBindings> PropertyBindings, TWeakPtr<ISequencer> InSequencer)
{
	return SNullWidget::NullWidget;
}

/*******************************************************************
*
* FControlRigSpaceChannelHelpers
*
**********************************************************************/


FSpaceChannelAndSection FControlRigSpaceChannelHelpers::FindSpaceChannelAndSectionForControl(UControlRig* ControlRig, FName ControlName, ISequencer* Sequencer, bool bCreateIfNeeded)
{
	FSpaceChannelAndSection SpaceChannelAndSection;
	SpaceChannelAndSection.SpaceChannel = nullptr;
	SpaceChannelAndSection.SectionToKey = nullptr;
	if (ControlRig == nullptr || Sequencer == nullptr)
	{
		return SpaceChannelAndSection;
	}
	if (TSharedPtr<IControlRigObjectBinding> ObjectBinding = ControlRig->GetObjectBinding())
	{
		USceneComponent* Component = Cast<USceneComponent>(ObjectBinding->GetBoundObject());
		if (!Component)
		{
			return SpaceChannelAndSection;
		}
		const bool bCreateHandleIfMissing = false;
		FName CreatedFolderName = NAME_None;
		FGuid ObjectHandle = Sequencer->GetHandleToObject(Component, bCreateHandleIfMissing);
		if (!ObjectHandle.IsValid())
		{
			UObject* ActorObject = Component->GetOwner();
			ObjectHandle = Sequencer->GetHandleToObject(ActorObject, bCreateHandleIfMissing);
			if (!ObjectHandle.IsValid())
			{
				return SpaceChannelAndSection;
			}
		}
		bool bCreateTrack = false;
		UMovieScene* MovieScene = Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene();
		if (!MovieScene)
		{
			return SpaceChannelAndSection;
		}
		if (FMovieSceneBinding* Binding = MovieScene->FindBinding(ObjectHandle))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (UMovieSceneControlRigParameterTrack* ControlRigParameterTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
				{
					if (ControlRigParameterTrack->GetControlRig() == ControlRig)
					{
						UMovieSceneControlRigParameterSection* ActiveSection = Cast<UMovieSceneControlRigParameterSection>(ControlRigParameterTrack->GetSectionToKey());
						if (ActiveSection)
						{
							ActiveSection->Modify();
							ControlRig->Modify();
							SpaceChannelAndSection.SectionToKey = ActiveSection;
							FSpaceControlNameAndChannel* NameAndChannel = ActiveSection->GetSpaceChannel(ControlName);
							if (NameAndChannel)
							{
								SpaceChannelAndSection.SpaceChannel = &NameAndChannel->SpaceCurve;
							}
							else if (bCreateIfNeeded)
							{
								ActiveSection->AddSpaceChannel(ControlName, true /*ReconstructChannelProxy*/);
								NameAndChannel = ActiveSection->GetSpaceChannel(ControlName);
								if (NameAndChannel)
								{
									SpaceChannelAndSection.SpaceChannel = &NameAndChannel->SpaceCurve;
								}
							}
						}
					}
				}
			}
		}
	}
	return SpaceChannelAndSection;
}

FKeyHandle FControlRigSpaceChannelHelpers::SequencerKeyControlRigSpaceChannel(UControlRig* ControlRig, ISequencer* Sequencer, FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey, FFrameNumber Time, URigHierarchy* RigHierarchy, const FRigElementKey& ControlKey, const FRigElementKey& SpaceKey)
{
	FKeyHandle Handle = FKeyHandle::Invalid();
	if (ControlRig == nullptr || Sequencer == nullptr || Sequencer->GetFocusedMovieSceneSequence() == nullptr || Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene() == nullptr)
	{
		return Handle;
	}

	URigHierarchy* Hierarchy = ControlRig->GetHierarchy();
	FMovieSceneControlRigSpaceBaseKey ExistingValue;
	using namespace UE::MovieScene;
	EvaluateChannel(Channel, Time, ExistingValue);
	FMovieSceneControlRigSpaceBaseKey Value = ExistingValue;

	if (SpaceKey == RigHierarchy->GetWorldSpaceSocketKey())
	{
		Value.SpaceType = EMovieSceneControlRigSpaceType::World;
	}
	else
	{
		FRigElementKey DefaultParent = RigHierarchy->GetFirstParent(ControlKey);
		if (DefaultParent == SpaceKey)
		{
			Value.SpaceType = EMovieSceneControlRigSpaceType::Parent;

		}
		else  //support all types
		{
			Value.SpaceType = EMovieSceneControlRigSpaceType::ControlRig;
			Value.ControlRigElement = SpaceKey;
		}
	}

	//we only key if the value is different.
	if (Value != ExistingValue)
	{
		TArray<FFrameNumber> Frames;
		Frames.Add(Time);
		
		TMovieSceneChannelData<FMovieSceneControlRigSpaceBaseKey> ChannelInterface = Channel->GetData();
		bool bSetPreviousKey = true;
		//if we have no keys need to set key for current space at start frame, unless setting key at start time, where then don't do previous compensation
		if (Channel->GetNumKeys() == 0 && Sequencer->GetFocusedMovieSceneSequence() && Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene())
		{
			FFrameNumber StartFrame = Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene()->GetPlaybackRange().GetLowerBoundValue();
			if (StartFrame != Time)
			{
				//find all of the times in the space after this time we now need to compensate for
				TSortedMap<FFrameNumber,FFrameNumber> ExtraFrames;
				FControlRigSpaceChannelHelpers::GetFramesInThisSpaceAfterThisTime(ControlRig, ControlKey.Name, ExistingValue,
					Channel, SectionToKey, Time, ExtraFrames);
				if (ExtraFrames.Num() > 0)
				{
					for (const TPair<FFrameNumber,FFrameNumber>& Frame : ExtraFrames)
					{
						Frames.Add(Frame.Value);
					}
				}

				FMovieSceneControlRigSpaceBaseKey Original = ExistingValue;
				ChannelInterface.AddKey(StartFrame, Forward<FMovieSceneControlRigSpaceBaseKey>(Original));

			}
			else
			{
				bSetPreviousKey = false;
			}
		}

		TArray<FTransform> ControlRigParentWorldTransforms;
		ControlRigParentWorldTransforms.SetNum(Frames.Num());
		for (FTransform& WorldTransform : ControlRigParentWorldTransforms)
		{
			WorldTransform  = FTransform::Identity;
		}
		TArray<FTransform> ControlWorldTransforms;
		FControlRigSnapper Snapper;
		Snapper.GetControlRigControlTransforms(Sequencer, ControlRig, ControlKey.Name, Frames, ControlRigParentWorldTransforms, ControlWorldTransforms);

		int32 ExistingIndex = ChannelInterface.FindKey(Time);
		if (ExistingIndex != INDEX_NONE)
		{
			Handle = ChannelInterface.GetHandle(ExistingIndex);
			using namespace UE::MovieScene;
			AssignValue(Channel, Handle, Forward<FMovieSceneControlRigSpaceBaseKey>(Value));
		}
		else
		{
			ExistingIndex = ChannelInterface.AddKey(Time, Forward<FMovieSceneControlRigSpaceBaseKey>(Value));
			Handle = ChannelInterface.GetHandle(ExistingIndex);
		}

		FRigControlModifiedContext Context;
		Context.SetKey = EControlRigSetKey::Always;
		FFrameRate TickResolution = Sequencer->GetFocusedTickResolution();

		if (bSetPreviousKey)
		{
			Context.LocalTime = TickResolution.AsSeconds(FFrameTime(Time - 1));
			ControlRig->SetControlGlobalTransform(ControlKey.Name, ControlWorldTransforms[0], true, Context);
		}
		ControlRig->GetHierarchy()->SwitchToParent(ControlKey, SpaceKey);
		int32 FramesIndex = 0;
		for (const FFrameNumber& Frame : Frames)
		{
			ControlRig->Evaluate_AnyThread();
			Context.LocalTime = TickResolution.AsSeconds(FFrameTime(Frame));
			ControlRig->SetControlGlobalTransform(ControlKey.Name, ControlWorldTransforms[FramesIndex], true, Context);
			FramesIndex++;
		}
		Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemAdded);

	}
	return Handle;

}

void  FControlRigSpaceChannelHelpers::SequencerSpaceChannelKeyDeleted(UControlRig* ControlRig, ISequencer* Sequencer, FName ControlName, FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneControlRigParameterSection* SectionToKey,
	FFrameNumber TimeOfDeletion)
{
	FMovieSceneControlRigSpaceBaseKey ExistingValue, PreviousValue;
	using namespace UE::MovieScene;
	EvaluateChannel(Channel, TimeOfDeletion -1, PreviousValue);
	EvaluateChannel(Channel, TimeOfDeletion, ExistingValue);
	if (ExistingValue != PreviousValue) //if they are the same no need to do anything
	{
		//find all key frames we need to compensate
		TArray<FFrameNumber> Frames;
		Frames.Add(TimeOfDeletion);
		TSortedMap<FFrameNumber, FFrameNumber> ExtraFrames;
		FControlRigSpaceChannelHelpers::GetFramesInThisSpaceAfterThisTime(ControlRig, ControlName, ExistingValue,
			Channel, SectionToKey, TimeOfDeletion, ExtraFrames);
		if (ExtraFrames.Num() > 0)
		{
			for (const TPair<FFrameNumber, FFrameNumber>& Frame : ExtraFrames)
			{
				Frames.Add(Frame.Value);
			}
		}
		TArray<FTransform> ControlRigParentWorldTransforms;
		ControlRigParentWorldTransforms.SetNum(Frames.Num());
		for (FTransform& WorldTransform : ControlRigParentWorldTransforms)
		{
			WorldTransform = FTransform::Identity;
		}
		TArray<FTransform> ControlWorldTransforms;
		FControlRigSnapper Snapper;
		Snapper.GetControlRigControlTransforms(Sequencer, ControlRig, ControlName, Frames, ControlRigParentWorldTransforms, ControlWorldTransforms);
		FRigElementKey ControlKey;
		ControlKey.Name = ControlName;
		ControlKey.Type = ERigElementType::Control;  
		URigHierarchy* RigHierarchy = ControlRig->GetHierarchy();
		switch (PreviousValue.SpaceType)
		{
		case EMovieSceneControlRigSpaceType::Parent:
			RigHierarchy->SwitchToDefaultParent(ControlKey);
			break;
		case EMovieSceneControlRigSpaceType::World:
			RigHierarchy->SwitchToWorldSpace(ControlKey);
			break;
		case EMovieSceneControlRigSpaceType::ControlRig:
			RigHierarchy->SwitchToParent(ControlKey, PreviousValue.ControlRigElement);
			break;
		}
		int32 FramesIndex = 0;
		FRigControlModifiedContext Context;
		Context.SetKey = EControlRigSetKey::Always;
		FFrameRate TickResolution = Sequencer->GetFocusedTickResolution();
		for (const FFrameNumber& Frame : Frames)
		{
			ControlRig->Evaluate_AnyThread();
			Context.LocalTime = TickResolution.AsSeconds(FFrameTime(Frame));
			ControlRig->SetControlGlobalTransform(ControlKey.Name, ControlWorldTransforms[FramesIndex++], true, Context);
		}
		//now delete any extra TimeOfDelete -1
		FControlRigSpaceChannelHelpers::DeleteTransformKeysAtThisTime(ControlRig, SectionToKey, ControlName, TimeOfDeletion - 1);
	}
}

void FControlRigSpaceChannelHelpers::DeleteTransformKeysAtThisTime(UControlRig* ControlRig, UMovieSceneControlRigParameterSection* Section, FName ControlName, FFrameNumber Time)
{
	if (Section && ControlRig)
	{
		TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
		FChannelMapInfo* pChannelIndex = Section->ControlChannelMap.Find(ControlName);
		if (pChannelIndex != nullptr)
		{
			int32 ChannelIndex = pChannelIndex->ChannelIndex;

			if (FRigControlElement* ControlElement = ControlRig->FindControl(ControlName))
			{
				FMovieSceneControlRigSpaceBaseKey Value;
				switch (ControlElement->Settings.ControlType)
				{
					case ERigControlType::Position:
					case ERigControlType::Scale:
					case ERigControlType::Rotator:
					case ERigControlType::Transform:
					case ERigControlType::TransformNoScale:
					case ERigControlType::EulerTransform:
					{
						int NumChannels = 0;
						if (ControlElement->Settings.ControlType == ERigControlType::Transform ||
							ControlElement->Settings.ControlType == ERigControlType::EulerTransform)
						{
							NumChannels = 9;
						}
						else if (ControlElement->Settings.ControlType == ERigControlType::TransformNoScale)
						{
							NumChannels = 6;
						}
						else //vectors
						{
							NumChannels = 3;
						}
						for (int32 Index = 0; Index < NumChannels; ++Index)
						{
							int32 KeyIndex = 0;
							for (FFrameNumber Frame : FloatChannels[ChannelIndex]->GetData().GetTimes())
							{
								if (Frame == Time)
								{
									FloatChannels[ChannelIndex]->GetData().RemoveKey(KeyIndex);
									break;
								}
								else if (Frame > Time)
								{
									break;
								}
								++KeyIndex;
							}
							++ChannelIndex;
						}
						break;
					}
					default:
						break;

				}
			}
		}
	}
}

void FControlRigSpaceChannelHelpers::GetFramesInThisSpaceAfterThisTime(UControlRig* ControlRig, FName ControlName, FMovieSceneControlRigSpaceBaseKey CurrentValue,
	FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey,
	FFrameNumber Time, TSortedMap<FFrameNumber,FFrameNumber>& OutMoreFrames)
{
	OutMoreFrames.Reset();
	if (ControlRig && Channel && SectionToKey)
	{
		if (UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(SectionToKey))
		{
			TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			FChannelMapInfo* pChannelIndex = Section->ControlChannelMap.Find(ControlName);
			if (pChannelIndex != nullptr)
			{
				int32 ChannelIndex = pChannelIndex->ChannelIndex;

				if (FRigControlElement* ControlElement = ControlRig->FindControl(ControlName))
				{
					FMovieSceneControlRigSpaceBaseKey Value;
					switch (ControlElement->Settings.ControlType)
					{
						case ERigControlType::Position:
						case ERigControlType::Scale:
						case ERigControlType::Rotator:
						case ERigControlType::Transform:
						case ERigControlType::TransformNoScale:
						case ERigControlType::EulerTransform:
						{
							int NumChannels = 0;
							if (ControlElement->Settings.ControlType == ERigControlType::Transform ||
								ControlElement->Settings.ControlType == ERigControlType::EulerTransform)
							{
								NumChannels = 9;
							}
							else if (ControlElement->Settings.ControlType == ERigControlType::TransformNoScale)
							{
								NumChannels = 6;
							}
							else //vectors
							{
								NumChannels = 3;
							}
							for (int32 Index = 0; Index < NumChannels; ++Index)
							{
								for (FFrameNumber Frame : FloatChannels[ChannelIndex++]->GetData().GetTimes())
								{
									if (Frame > Time)
									{
										using namespace UE::MovieScene;
										EvaluateChannel(Channel, Frame, Value);
										if (CurrentValue == Value)
										{
											if (OutMoreFrames.Find(Frame) == nullptr)
											{
												OutMoreFrames.Add(Frame, Frame);
											}
										}
										else
										{
											break;
										}
									}
								}
							}
							break;
						}

					}
				}
			}
		}
	}
}


void FControlRigSpaceChannelHelpers::SequencerBakeControlInSpace(UControlRig* ControlRig, ISequencer* Sequencer, FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey,
	TArray<FFrameNumber> Frames, URigHierarchy* RigHierarchy, const FRigElementKey& ControlKey, FRigSpacePickerBakeSettings Settings)
{
	if (ControlRig && Sequencer && Channel && SectionToKey && Frames.Num() > 0 && RigHierarchy
		&& Sequencer->GetFocusedMovieSceneSequence() && Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene())
	{
		TArray<FTransform> ControlRigParentWorldTransforms;
		ControlRigParentWorldTransforms.SetNum(Frames.Num());
		for (FTransform& Transform : ControlRigParentWorldTransforms)
		{
			Transform = FTransform::Identity;
		}
		//Store transforms
		FControlRigSnapper Snapper;
		TArray<FTransform> ControlWorldTransforms;
		Snapper.GetControlRigControlTransforms(Sequencer, ControlRig, ControlKey.Name, Frames, ControlRigParentWorldTransforms, ControlWorldTransforms);

		//Find all space keys in range and delete them since it will get replaced with new space when we components.
		FFrameNumber StartFrame = Frames[0];
		FFrameNumber EndFrame = Frames[Frames.Num() - 1];
		TArray<FFrameNumber> Keys;
		TArray < FKeyHandle> KeyHandles;
		TRange<FFrameNumber> Range(StartFrame, EndFrame);
		Channel->GetKeys(Range, &Keys, &KeyHandles);
		Channel->DeleteKeys(KeyHandles);
		URigHierarchy* Hierarchy = ControlRig->GetHierarchy();

		//now find space at start and end see if different than the new space if so we need to compensate
		FMovieSceneControlRigSpaceBaseKey StartFrameValue, EndFrameValue;
		using namespace UE::MovieScene;
		EvaluateChannel(Channel, StartFrame, StartFrameValue);
		EvaluateChannel(Channel, EndFrame, EndFrameValue);

		FMovieSceneControlRigSpaceBaseKey Value;
		if (Settings.TargetSpace == RigHierarchy->GetWorldSpaceSocketKey())
		{
			Value.SpaceType = EMovieSceneControlRigSpaceType::World;
		}
		else
		{
			FRigElementKey DefaultParent = RigHierarchy->GetFirstParent(ControlKey);
			if (DefaultParent == Settings.TargetSpace)
			{
				Value.SpaceType = EMovieSceneControlRigSpaceType::Parent;
			}
			else
			{
				Value.SpaceType = EMovieSceneControlRigSpaceType::ControlRig;
				Value.ControlRigElement = Settings.TargetSpace;
			}
		}
		const bool bCompensateStart = StartFrameValue != Value;
		TRange<FFrameNumber> PlaybackRange = Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene()->GetPlaybackRange();
		const bool bCompensateEnd = (EndFrameValue != Value && PlaybackRange.GetUpperBoundValue() != EndFrame);

		//if compensate at the start we need to set the channel key as the new value
		if (bCompensateStart)
		{
			TMovieSceneChannelData<FMovieSceneControlRigSpaceBaseKey> ChannelInterface = Channel->GetData();
			ChannelInterface.AddKey(StartFrame, Forward<FMovieSceneControlRigSpaceBaseKey>(Value));

		}
		//if we compensate at the end we change the last frame to frame -1(tick), and then later set the space to the other one and 
		if (bCompensateEnd)
		{
			Frames[Frames.Num() - 1] = Frames[Frames.Num() - 1] - 1;
		}
		//now set all of the key values
		FRigControlModifiedContext Context;
		Context.SetKey = EControlRigSetKey::Always;
		FFrameRate TickResolution = Sequencer->GetFocusedTickResolution();

		RigHierarchy->SwitchToParent(ControlKey, Settings.TargetSpace);
		ControlRig->Evaluate_AnyThread();

		for (int32 Index = 0; Index < Frames.Num(); ++Index)
		{
			const FTransform GlobalTransform = ControlWorldTransforms[Index];
			const FFrameNumber Frame = Frames[Index];
			Context.LocalTime = TickResolution.AsSeconds(FFrameTime(Frame));
			ControlRig->SetControlGlobalTransform(ControlKey.Name, GlobalTransform, true, Context);
		}

		//if end compensated set the space that was active previously and set the compensated global value
		if (bCompensateEnd)
		{
			//EndFrameValue to SpaceKey todoo move to function
			switch (EndFrameValue.SpaceType)
			{
			case EMovieSceneControlRigSpaceType::Parent:
				RigHierarchy->SwitchToDefaultParent(ControlKey);
				break;
			case EMovieSceneControlRigSpaceType::World:
				RigHierarchy->SwitchToWorldSpace(ControlKey);
				break;
			case EMovieSceneControlRigSpaceType::ControlRig:
				RigHierarchy->SwitchToParent(ControlKey, EndFrameValue.ControlRigElement);
				break;
			}
			ControlRig->Evaluate_AnyThread();

			TMovieSceneChannelData<FMovieSceneControlRigSpaceBaseKey> ChannelInterface = Channel->GetData();
			ChannelInterface.AddKey(EndFrame, Forward<FMovieSceneControlRigSpaceBaseKey>(EndFrameValue));
			const FTransform GlobalTransform = ControlWorldTransforms[Frames.Num() - 1];
			Context.LocalTime = TickResolution.AsSeconds(FFrameTime(EndFrame));
			ControlRig->SetControlGlobalTransform(ControlKey.Name, GlobalTransform, true, Context);
		}
		if (UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(SectionToKey))
		{ 		
			// Fix any Rotation Channels
			Section->FixRotationWinding(ControlKey.Name, Frames[0], Frames[Frames.Num() - 1]);
			// Then reduce
			if (Settings.bReduceKeys)
			{
				TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
				FChannelMapInfo* pChannelIndex = Section->ControlChannelMap.Find(ControlKey.Name);
				if (pChannelIndex != nullptr)
				{
					int32 ChannelIndex = pChannelIndex->ChannelIndex;

					if (FRigControlElement* ControlElement = ControlRig->FindControl(ControlKey.Name))
					{
						FKeyDataOptimizationParams Params;
						Params.bAutoSetInterpolation = true;
						Params.Tolerance = Settings.Tolerance;
						Params.Range = TRange <FFrameNumber>(Frames[0], Frames[Frames.Num() - 1]);

						switch (ControlElement->Settings.ControlType)
						{
						case ERigControlType::Position:
						case ERigControlType::Scale:
						case ERigControlType::Rotator:
						{
							FloatChannels[ChannelIndex]->Optimize(Params);
							FloatChannels[ChannelIndex + 1]->Optimize(Params);
							FloatChannels[ChannelIndex + 2]->Optimize(Params);
							break;
						}

						case ERigControlType::Transform:
						case ERigControlType::TransformNoScale:
						case ERigControlType::EulerTransform:
						{

							FloatChannels[ChannelIndex]->Optimize(Params);
							FloatChannels[ChannelIndex + 1]->Optimize(Params);
							FloatChannels[ChannelIndex + 2]->Optimize(Params);
							FloatChannels[ChannelIndex + 3]->Optimize(Params);
							FloatChannels[ChannelIndex + 4]->Optimize(Params);
							FloatChannels[ChannelIndex + 5]->Optimize(Params);

							if (ControlElement->Settings.ControlType == ERigControlType::Transform ||
								ControlElement->Settings.ControlType == ERigControlType::EulerTransform)
							{
								FloatChannels[ChannelIndex + 6]->Optimize(Params);
								FloatChannels[ChannelIndex + 7]->Optimize(Params);
								FloatChannels[ChannelIndex + 8]->Optimize(Params);
							}
							break;

						}
						}
					}
				}
			}
			
		}
		Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemAdded); //may have added channel
	}
}

void FControlRigSpaceChannelHelpers::HandleSpaceKeyTimeChanged(UControlRig* ControlRig, FName ControlName, FMovieSceneControlRigSpaceChannel* Channel, UMovieSceneSection* SectionToKey,
	FFrameNumber CurrentFrame, FFrameNumber NextFrame)
{
	if (ControlRig && Channel && SectionToKey && (CurrentFrame != NextFrame))
	{
		if (UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(SectionToKey))
		{
			TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			FChannelMapInfo* pChannelIndex = Section->ControlChannelMap.Find(ControlName);
			if (pChannelIndex != nullptr)
			{
				int32 ChannelIndex = pChannelIndex->ChannelIndex;
				FFrameNumber Delta = NextFrame - CurrentFrame;
				if (FRigControlElement* ControlElement = ControlRig->FindControl(ControlName))
				{
					FMovieSceneControlRigSpaceBaseKey Value;
					switch (ControlElement->Settings.ControlType)
					{
						case ERigControlType::Position:
						case ERigControlType::Scale:
						case ERigControlType::Rotator:
						case ERigControlType::Transform:
						case ERigControlType::TransformNoScale:
						case ERigControlType::EulerTransform:
						{
							int NumChannels = 0;
							if (ControlElement->Settings.ControlType == ERigControlType::Transform ||
								ControlElement->Settings.ControlType == ERigControlType::EulerTransform)
							{
								NumChannels = 9;
							}
							else if (ControlElement->Settings.ControlType == ERigControlType::TransformNoScale)
							{
								NumChannels = 6;
							}
							else //vectors
							{
								NumChannels = 3;
							}
							for (int32 Index = 0; Index < NumChannels; ++Index)
							{
								FMovieSceneFloatChannel* FloatChannel = FloatChannels[ChannelIndex++];
								if (Delta > 0) //if we are moving keys positively in time we start from end frames and move them so we can use indices
								{
									for (int32 KeyIndex = FloatChannel->GetData().GetTimes().Num() - 1; KeyIndex >= 0; --KeyIndex)
									{
										const FFrameNumber Frame = FloatChannel->GetData().GetTimes()[KeyIndex];
										FFrameNumber Diff = Frame - CurrentFrame;
										FFrameNumber AbsDiff = Diff < 0 ? -Diff : Diff;
										if (AbsDiff <= 1)
										{
											FFrameNumber NewKeyTime = Frame + Delta;
											FloatChannel->GetData().MoveKey(KeyIndex, NewKeyTime);
										}
									}
								}
								else
								{
									for (int32 KeyIndex = 0; KeyIndex < FloatChannel->GetData().GetTimes().Num(); ++KeyIndex)
									{
										const FFrameNumber Frame = FloatChannel->GetData().GetTimes()[KeyIndex];
										FFrameNumber Diff = Frame - CurrentFrame;
										FFrameNumber AbsDiff = Diff < 0 ? -Diff : Diff;
										if (AbsDiff <= 1)
										{
											FFrameNumber NewKeyTime = Frame + Delta;
											FloatChannel->GetData().MoveKey(KeyIndex, NewKeyTime);
										}
									}
								}
							}
							break;
						}
						default:
							break;
					}
				}
			}
		}
	}
}


void FControlRigSpaceChannelHelpers::CompensateIfNeeded(UControlRig* ControlRig, ISequencer* Sequencer, UMovieSceneControlRigParameterSection* Section, FName ControlName, TOptional<FFrameNumber>& OptionalTime)
{
	
	//we need to check all controls for 1) space and 2) previous frame and if so we automatically compensate.
	TArray<FRigControlElement*> Controls = ControlRig->GetHierarchy()->GetControls();
	bool bDidIt = false;
	for(FRigControlElement* Control: Controls)
	{ 
		if(Control)// ac && Control->GetName() != ControlName)
		{ 
			//only if we have a channel
			if (FSpaceControlNameAndChannel* Channel = Section->GetSpaceChannel(Control->GetName()))
			{
				TArray<FFrameNumber> AllFrames; 
				if (OptionalTime.IsSet())
				{
					FFrameNumber Time = OptionalTime.GetValue();
					AllFrames.Add(Time);
				}
				else
				{
					AllFrames = Channel->SpaceCurve.GetData().GetTimes();
				}
				if (AllFrames.Num() > 0)
				{
					for (FFrameNumber& Time : AllFrames)
					{
						FMovieSceneControlRigSpaceBaseKey ExistingValue, PreviousValue;
						using namespace UE::MovieScene;
						EvaluateChannel(&(Channel->SpaceCurve), Time - 1, PreviousValue);
						EvaluateChannel(&(Channel->SpaceCurve), Time, ExistingValue);
						if (ExistingValue != PreviousValue) //if they are the same no need to do anything
						{
							//find global value at curren time
							TArray<FFrameNumber> Frames;
							Frames.Add(Time);
							TArray<FTransform> ControlRigParentWorldTransforms;
							ControlRigParentWorldTransforms.SetNum(Frames.Num());
							for (FTransform& WorldTransform : ControlRigParentWorldTransforms)
							{
								WorldTransform = FTransform::Identity;
							}
							TArray<FTransform> ControlWorldTransforms;
							FControlRigSnapper Snapper;
							Snapper.GetControlRigControlTransforms(Sequencer, ControlRig, Control->GetName(), Frames, ControlRigParentWorldTransforms, ControlWorldTransforms);
							FRigElementKey ControlKey;
							ControlKey.Name = Control->GetName();
							ControlKey.Type = ERigElementType::Control;
							//set space to previous space value that's different.
							URigHierarchy* RigHierarchy = ControlRig->GetHierarchy();
							switch (PreviousValue.SpaceType)
							{
							case EMovieSceneControlRigSpaceType::Parent:
								RigHierarchy->SwitchToDefaultParent(ControlKey);
								break;
							case EMovieSceneControlRigSpaceType::World:
								RigHierarchy->SwitchToWorldSpace(ControlKey);
								break;
							case EMovieSceneControlRigSpaceType::ControlRig:
								RigHierarchy->SwitchToParent(ControlKey, PreviousValue.ControlRigElement);
								break;
							}
							//now set time -1 frame value
							FRigControlModifiedContext Context;
							Context.SetKey = EControlRigSetKey::Always;
							FFrameRate TickResolution = Sequencer->GetFocusedTickResolution();
							ControlRig->Evaluate_AnyThread();
							Context.LocalTime = TickResolution.AsSeconds(FFrameTime(Time - 1));
							ControlRig->SetControlGlobalTransform(ControlKey.Name, ControlWorldTransforms[0], true, Context);
							bDidIt = true;
						}
					}
				}
			}
		}
	}
	if (bDidIt == true)
	{
		Sequencer->ForceEvaluate();
	}
}

#undef LOCTEXT_NAMESPACE
