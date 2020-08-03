// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sections/MovieSceneEventRepeaterSection.h"
#include "Tracks/MovieSceneEventTrack.h"

#include "EntitySystem/MovieSceneEntityBuilder.h"
#include "EntitySystem/MovieSceneEntityManager.h"
#include "EntitySystem/BuiltInComponentTypes.h"
#include "EntitySystem/MovieSceneSequenceInstance.h"
#include "Systems/MovieSceneEventSystems.h"
#include "MovieSceneTracksComponentTypes.h"

#include "Evaluation/MovieSceneEvaluationField.h"


void UMovieSceneEventRepeaterSection::ImportEntityImpl(UMovieSceneEntitySystemLinker* EntityLinker, const FEntityImportParams& Params, FImportedEntity* OutImportedEntity)
{
	using namespace UE::MovieScene;

	if (Event.Ptrs.Function == nullptr)
	{
		return;
	}

	UMovieSceneEventTrack*   EventTrack     = GetTypedOuter<UMovieSceneEventTrack>();
	const FSequenceInstance& ThisInstance   = EntityLinker->GetInstanceRegistry()->GetInstance(Params.Sequence.InstanceHandle);
	FMovieSceneContext       Context        = ThisInstance.GetContext();

	if (!GetRange().Contains(Context.GetTime().FrameNumber))
	{
		return;
	}

	UMovieSceneEventSystem* EventSystem = nullptr;

	if (EventTrack->EventPosition == EFireEventsAtPosition::AtStartOfEvaluation)
	{
		EventSystem = EntityLinker->LinkSystem<UMovieScenePreSpawnEventSystem>();
	}
	else if (EventTrack->EventPosition == EFireEventsAtPosition::AfterSpawn)
	{
		EventSystem = EntityLinker->LinkSystem<UMovieScenePostSpawnEventSystem>();
	}
	else
	{
		EventSystem = EntityLinker->LinkSystem<UMovieScenePostEvalEventSystem>();
	}

	FMovieSceneEventTriggerData TriggerData = {
		Event.Ptrs,
		Params.ObjectBindingID,
		ThisInstance.GetSequenceID(),
		Context.GetTime() * Context.GetSequenceToRootTransform()
	};

	EventSystem->AddEvent(ThisInstance.GetRootInstanceHandle(), TriggerData);

	// Mimic the structure changing in order to ensure that the instantiation phase runs
	EntityLinker->EntityManager.MimicStructureChanged();
}

bool UMovieSceneEventRepeaterSection::PopulateEvaluationFieldImpl(const TRange<FFrameNumber>& EffectiveRange, FMovieSceneEntityComponentField* OutField)
{
	OutField->OneShotEntities.Populate(EffectiveRange, this, NAME_None);
	return true;
}
