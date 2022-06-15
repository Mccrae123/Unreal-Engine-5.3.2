// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearch/PoseSearchLibrary.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimNode_Inertialization.h"
#include "Animation/AnimNode_SequencePlayer.h"
#include "Animation/AnimRootMotionProvider.h"
#include "Animation/BlendSpace.h"
#include "PoseSearch/AnimNode_MotionMatching.h"
#include "Trace/PoseSearchTraceLogger.h"
#include "MotionTrajectoryLibrary.h"

#define LOCTEXT_NAMESPACE "PoseSearchLibrary"

namespace UE::PoseSearch {

static void ComputeDatabaseSequenceFilter(
	const UPoseSearchDatabase* Database, 
	const FGameplayTagQuery* Query, 
	TArray<bool>& OutDbSequenceFilter)
{
	OutDbSequenceFilter.SetNum(Database->Sequences.Num());

	if (Query)
	{
		for (int SeqIdx = 0; SeqIdx < Database->Sequences.Num(); ++SeqIdx)
		{
			OutDbSequenceFilter[SeqIdx] = Query->Matches(Database->Sequences[SeqIdx].GroupTags);
		}
	}
	else
	{
		for (int SeqIdx = 0; SeqIdx < Database->Sequences.Num(); ++SeqIdx)
		{
			OutDbSequenceFilter[SeqIdx] = true;
		}
	}
}

static void ComputeDatabaseBlendSpaceFilter(
	const UPoseSearchDatabase* Database,
	const FGameplayTagQuery* Query,
	TArray<bool>& OutDbBlendSpaceFilter)
{
	OutDbBlendSpaceFilter.SetNum(Database->BlendSpaces.Num());

	if (Query)
	{
		for (int BlendSpaceIdx = 0; BlendSpaceIdx < Database->BlendSpaces.Num(); ++BlendSpaceIdx)
		{
			OutDbBlendSpaceFilter[BlendSpaceIdx] = Query->Matches(Database->BlendSpaces[BlendSpaceIdx].GroupTags);
		}
	}
	else
	{
		for (int BlendSpaceIdx = 0; BlendSpaceIdx < Database->BlendSpaces.Num(); ++BlendSpaceIdx)
		{
			OutDbBlendSpaceFilter[BlendSpaceIdx] = true;
		}
	}
}

} // namespace UE::PoseSearch


//////////////////////////////////////////////////////////////////////////
// FMotionMatchingState

void FMotionMatchingState::Reset()
{
	CurrentSearchResult.Reset();
	AssetPlayerTime = 0.0f;
	// Set the elapsed time to INFINITY to trigger a search right away
	ElapsedPoseJumpTime = INFINITY;
}

void FMotionMatchingState::AdjustAssetTime(float AssetTime)
{
	CurrentSearchResult.Update(AssetTime);
	AssetPlayerTime = CurrentSearchResult.AssetTime;
}

bool FMotionMatchingState::CanAdvance(float DeltaTime, bool& bOutAdvanceToFollowUpAsset, UE::PoseSearch::FSearchResult& OutFollowUpAsset) const
{
	bOutAdvanceToFollowUpAsset = false;
	OutFollowUpAsset = UE::PoseSearch::FSearchResult();

	if (!CurrentSearchResult.IsValid())
	{
		return false;
	}

	const FPoseSearchIndexAsset* SearchIndexAsset = GetCurrentSearchIndexAsset();

	if (SearchIndexAsset->Type == ESearchIndexAssetType::Sequence)
	{
		const FPoseSearchDatabaseSequence& DbSequence = 
			CurrentSearchResult.Database->GetSequenceSourceAsset(SearchIndexAsset);
		const float AssetLength = DbSequence.Sequence->GetPlayLength();

		float SteppedTime = AssetPlayerTime;
		ETypeAdvanceAnim AdvanceType = FAnimationRuntime::AdvanceTime(
			DbSequence.Sequence->bLoop,
			DeltaTime,
			SteppedTime,
			AssetLength);

		if (AdvanceType != ETAA_Finished)
		{
			return SearchIndexAsset->SamplingInterval.Contains(SteppedTime);
		}
		else
		{
			// check if there's a follow-up that can be used
			int32 FollowUpDbSequenceIdx = CurrentSearchResult.Database->Sequences.IndexOfByPredicate(
				[&](const FPoseSearchDatabaseSequence& Entry)
				{
					return Entry.Sequence == DbSequence.FollowUpSequence;
				});

			int32 FollowUpSearchIndexAssetIdx = CurrentSearchResult.Database->GetSearchIndex()->Assets.IndexOfByPredicate(
				[&](const FPoseSearchIndexAsset& Entry)
				{
					const bool bIsMatch =
						Entry.SourceAssetIdx == FollowUpDbSequenceIdx &&
						Entry.bMirrored == SearchIndexAsset->bMirrored &&
						Entry.SamplingInterval.Contains(0.0f);
					return bIsMatch;
				});

			if (FollowUpSearchIndexAssetIdx != INDEX_NONE)
			{
				bOutAdvanceToFollowUpAsset = true;

				const FPoseSearchIndexAsset* FollowUpSearchIndexAsset =
					&CurrentSearchResult.Database->GetSearchIndex()->Assets[FollowUpSearchIndexAssetIdx];

				// Follow up asset time will start slightly before the beginning of the sequence as 
				// this is essentially what the matching time in the corresponding main sequence is.
				// Here we are assuming that the tick will advance the asset player timer into the 
				// valid region
				const float FollowUpAssetTime = AssetPlayerTime - AssetLength;

				// There is no correspoding pose index when we switch due to what is mentioned above
				// so for now we just take whatever pose index is associated with the first frame.
				OutFollowUpAsset.PoseIdx = CurrentSearchResult.Database->GetPoseIndexFromTime(FollowUpSearchIndexAsset->SamplingInterval.Min, FollowUpSearchIndexAsset);
				OutFollowUpAsset.SearchIndexAsset = FollowUpSearchIndexAsset;
				OutFollowUpAsset.AssetTime = FollowUpAssetTime;
				return true;
			}
		}
	}
	else if (SearchIndexAsset->Type == ESearchIndexAssetType::BlendSpace)
	{
		const FPoseSearchDatabaseBlendSpace& DbBlendSpace = 
			CurrentSearchResult.Database->GetBlendSpaceSourceAsset(SearchIndexAsset);

		TArray<FBlendSampleData> BlendSamples;
		int32 TriangulationIndex = 0;
		DbBlendSpace.BlendSpace->GetSamplesFromBlendInput(SearchIndexAsset->BlendParameters, BlendSamples, TriangulationIndex, true);

		float PlayLength = DbBlendSpace.BlendSpace->GetAnimationLengthFromSampleData(BlendSamples);

		// Asset player time for blendspaces is normalized [0, 1] so we need to convert 
		// to a real time before we advance it
		float RealTime = AssetPlayerTime * PlayLength;
		float SteppedTime = RealTime;
		ETypeAdvanceAnim AdvanceType = FAnimationRuntime::AdvanceTime(
			DbBlendSpace.BlendSpace->bLoop,
			DeltaTime,
			SteppedTime,
			PlayLength);

		if (AdvanceType != ETAA_Finished)
		{
			return SearchIndexAsset->SamplingInterval.Contains(SteppedTime);
		}
	}
	else
	{
		checkNoEntry();
	}

	return false;
}

static void RequestInertialBlend(const FAnimationUpdateContext& Context, float BlendTime)
{
	// Use inertial blending to smooth over the transition
	// It would be cool in the future to adjust the blend time by amount of dissimilarity, but we'll need a standardized distance metric first.
	if (BlendTime > 0.0f)
	{
		UE::Anim::IInertializationRequester* InertializationRequester = Context.GetMessage<UE::Anim::IInertializationRequester>();
		if (InertializationRequester)
		{
			InertializationRequester->RequestInertialization(BlendTime);
		}
	}
}

void FMotionMatchingState::JumpToPose(const FAnimationUpdateContext& Context, const FMotionMatchingSettings& Settings, const UE::PoseSearch::FSearchResult& Result)
{
	// Remember which pose and sequence we're playing from the database
	CurrentSearchResult = Result;

	ElapsedPoseJumpTime = 0.0f;
	AssetPlayerTime = Result.AssetTime;

	const float JumpBlendTime = ComputeJumpBlendTime(Result, Settings);
	RequestInertialBlend(Context, JumpBlendTime);
	Flags |= EMotionMatchingFlags::JumpedToPose;
}

void UpdateMotionMatchingState(
	const FAnimationUpdateContext& Context,
	const UPoseSearchSearchableAsset* Searchable,
	const FGameplayTagQuery* DatabaseTagQuery,
	const FGameplayTagContainer* ActiveTagsContainer,
	const FTrajectorySampleRange& Trajectory,
	const FMotionMatchingSettings& Settings,
	FMotionMatchingState& InOutMotionMatchingState
)
{
	using namespace UE::PoseSearch;

	if (!Searchable)
	{
		Context.LogMessage(
			EMessageSeverity::Error, 
			LOCTEXT("NoSearchable", "No searchable asset provided for motion matching."));
		return;
	}

	const float DeltaTime = Context.GetDeltaTime();

	// Reset State Flags
	InOutMotionMatchingState.Flags = EMotionMatchingFlags::None;

	// Record Current Pose Index for Debugger
	const FSearchResult LastResult = InOutMotionMatchingState.CurrentSearchResult;

	// Check if we can advance. Includes the case where we can advance but only by switching to a follow up asset.
	bool bAdvanceToFollowUpAsset = false;
	FSearchResult FollowUpAsset;
	bool bCanAdvance = InOutMotionMatchingState.CanAdvance(Context.GetDeltaTime(), bAdvanceToFollowUpAsset, FollowUpAsset);

	// If we can't advance or enough time has elapsed since the last pose jump then search
	if (!bCanAdvance || (InOutMotionMatchingState.ElapsedPoseJumpTime >= Settings.SearchThrottleTime))
	{
		// Build the search context
		FPoseSearchContext SearchContext;
		SearchContext.DatabaseTagQuery = DatabaseTagQuery;
		SearchContext.ActiveTagsContainer = ActiveTagsContainer;
		SearchContext.Trajectory = &Trajectory;
		SearchContext.OwningComponent = Context.AnimInstanceProxy->GetSkelMeshComponent();
		SearchContext.BoneContainer = &Context.AnimInstanceProxy->GetRequiredBones();

#if WITH_EDITORONLY_DATA
		SearchContext.DebugDrawParams.SearchCostHistoryBruteForce = &InOutMotionMatchingState.SearchCostHistoryBruteForce;
		SearchContext.DebugDrawParams.SearchCostHistoryKDTree = &InOutMotionMatchingState.SearchCostHistoryKDTree;
#endif

		SearchContext.CurrentResult = InOutMotionMatchingState.CurrentSearchResult;

		IPoseHistoryProvider* PoseHistoryProvider = Context.GetMessage<IPoseHistoryProvider>();
		if (PoseHistoryProvider)
		{
			SearchContext.History = &PoseHistoryProvider->GetPoseHistory();
		}

		if (const FPoseSearchIndexAsset* CurrentIndexAsset = InOutMotionMatchingState.GetCurrentSearchIndexAsset())
		{
			SearchContext.QueryMirrorRequest =
				CurrentIndexAsset->bMirrored ?
				EPoseSearchBooleanRequest::TrueValue :
				EPoseSearchBooleanRequest::FalseValue;
		}

		// Search the database for the nearest match to the updated query vector
		FSearchResult SearchResult = Searchable->Search(SearchContext);

		if (SearchResult.IsValid())
		{
			// If the result is valid and we couldn't advance we should always jump to the search result
			if (!bCanAdvance)
			{
				InOutMotionMatchingState.JumpToPose(Context, Settings, SearchResult);
			}
			// Otherwise we need to check if the result is a good improvement over the current pose
			else
			{
				// Consider the search result better if it is more similar to the query than the current pose we're playing back from the database
				check(SearchResult.PoseCost.GetDissimilarity() >= 0.0f);
				bool bBetterPose = true;
				if (SearchResult.ContinuityPoseCost.IsValid())
				{
					if ((SearchResult.ContinuityPoseCost.GetTotalCost() <= SearchResult.PoseCost.GetTotalCost()) || 
						(SearchResult.ContinuityPoseCost.GetDissimilarity() <= SearchResult.PoseCost.GetDissimilarity()))
					{
						bBetterPose = false;
					}
					else
					{
						checkSlow(
							SearchResult.ContinuityPoseCost.GetDissimilarity() > 0.0f && 
							 SearchResult.ContinuityPoseCost.GetDissimilarity() > SearchResult.PoseCost.GetDissimilarity());
						const float RelativeSimilarityGain = -1.0f * 
							(SearchResult.PoseCost.GetDissimilarity() - SearchResult.ContinuityPoseCost.GetDissimilarity()) / 
							SearchResult.ContinuityPoseCost.GetDissimilarity();
						bBetterPose = RelativeSimilarityGain >= Settings.MinPercentImprovement / 100.0f;
					}
				}

				// Ignore the candidate poses from the same anim when they are too near to the current pose
				bool bNearbyPose = false;
				const FPoseSearchIndexAsset* StateSearchIndexAsset = InOutMotionMatchingState.GetCurrentSearchIndexAsset();
				if (StateSearchIndexAsset == SearchResult.SearchIndexAsset)
				{
					// We need to check in terms of PoseIdx rather than AssetTime because
					// for blendspaces, AssetTime is not in seconds, but in the normalized range 
					// [0, 1] so comparing to `PoseJumpThresholdTime` will not make sense		
					bNearbyPose = 
						FMath::Abs(InOutMotionMatchingState.CurrentSearchResult.PoseIdx - SearchResult.PoseIdx) *
						SearchResult.Database->Schema->SamplingInterval < Settings.PoseJumpThresholdTime;

					// Handle looping anims when checking for the pose being too close
					if (!bNearbyPose && SearchResult.Database->IsSourceAssetLooping(StateSearchIndexAsset))
					{
						const float Time =
							FMath::Abs(
								StateSearchIndexAsset->NumPoses -
								InOutMotionMatchingState.CurrentSearchResult.PoseIdx -
								SearchResult.PoseIdx) *
							SearchResult.Database->Schema->SamplingInterval;
						bNearbyPose = Time < Settings.PoseJumpThresholdTime;
					}
				}

				// Jump to candidate pose if there was a better option
				if (bBetterPose && !bNearbyPose)
				{
					InOutMotionMatchingState.JumpToPose(Context, Settings, SearchResult);
				}
			}
		}
	}

	// If we didn't search or it didn't find a pose to jump to, and we can 
	// advance but only with the follow up asset, jump to that. Otherwise we 
	// are advancing as normal, and nothing needs to be done.
	if (!(InOutMotionMatchingState.Flags & EMotionMatchingFlags::JumpedToPose)
		&& bCanAdvance
		&& bAdvanceToFollowUpAsset)
	{
		InOutMotionMatchingState.JumpToPose(Context, Settings, FollowUpAsset);
		InOutMotionMatchingState.Flags |= EMotionMatchingFlags::JumpedToFollowUp;
	}

	// Tick elapsed pose jump timer
	if (!(InOutMotionMatchingState.Flags & EMotionMatchingFlags::JumpedToPose))
	{
		InOutMotionMatchingState.ElapsedPoseJumpTime += DeltaTime;
	}

	// Record debugger details
#if UE_POSE_SEARCH_TRACE_ENABLED
	if (InOutMotionMatchingState.CurrentSearchResult.IsValid())
	{
		float SimLinearVelocity, SimAngularVelocity, AnimLinearVelocity, AnimAngularVelocity;

		if (DeltaTime > SMALL_NUMBER)
		{
			// simulation

			int32 FirstIdx = 0;
			const FTrajectorySample PrevSample = FTrajectorySampleRange::IterSampleTrajectory(
				Trajectory.Samples,
				ETrajectorySampleDomain::Time,
				-DeltaTime, FirstIdx);

			const FTrajectorySample CurrSample = FTrajectorySampleRange::IterSampleTrajectory(
				Trajectory.Samples,
				ETrajectorySampleDomain::Time,
				0.0f, FirstIdx);

			const FTransform SimDelta = CurrSample.Transform.GetRelativeTransform(PrevSample.Transform);

			SimLinearVelocity = SimDelta.GetTranslation().Size() / DeltaTime;
			SimAngularVelocity = FMath::RadiansToDegrees(SimDelta.GetRotation().GetAngle()) / DeltaTime;

			// animation

			const FTransform AnimDelta = InOutMotionMatchingState.RootMotionTransformDelta;

			AnimLinearVelocity = AnimDelta.GetTranslation().Size() / DeltaTime;
			AnimAngularVelocity = FMath::RadiansToDegrees(AnimDelta.GetRotation().GetAngle()) / DeltaTime;
		}
		else
		{
			SimLinearVelocity = 0.0f;
			SimAngularVelocity = 0.0f;
			AnimLinearVelocity = 0.0f;
			AnimAngularVelocity = 0.0f;
		}

		TArray<bool> DatabaseSequenceFilter;
		ComputeDatabaseSequenceFilter(
			InOutMotionMatchingState.CurrentSearchResult.Database.Get(), 
			DatabaseTagQuery, 
			DatabaseSequenceFilter);

		TArray<bool> DatabaseBlendSpaceFilter;
		ComputeDatabaseBlendSpaceFilter(
			InOutMotionMatchingState.CurrentSearchResult.Database.Get(),
			DatabaseTagQuery, 
			DatabaseBlendSpaceFilter);

		FTraceMotionMatchingState TraceState;
		if (EnumHasAnyFlags(InOutMotionMatchingState.Flags, EMotionMatchingFlags::JumpedToFollowUp))
		{
			TraceState.Flags |= FTraceMotionMatchingState::EFlags::FollowupAnimation;
		}

		TraceState.ElapsedPoseJumpTime = InOutMotionMatchingState.ElapsedPoseJumpTime;
		// @TODO: Change this to only be the previous query, not persistently updated (i.e. if throttled)?
		TraceState.QueryVector = InOutMotionMatchingState.CurrentSearchResult.ComposedQuery.GetValues();
		TraceState.QueryVectorNormalized = InOutMotionMatchingState.CurrentSearchResult.ComposedQuery.GetNormalizedValues();
		TraceState.DbPoseIdx = InOutMotionMatchingState.CurrentSearchResult.PoseIdx;
		TraceState.DatabaseId = FObjectTrace::GetObjectId(InOutMotionMatchingState.CurrentSearchResult.Database.Get());
		TraceState.ContinuingPoseIdx = LastResult.PoseIdx;

		TraceState.AssetPlayerTime = InOutMotionMatchingState.AssetPlayerTime;
		TraceState.DeltaTime = DeltaTime;
		TraceState.SimLinearVelocity = SimLinearVelocity;
		TraceState.SimAngularVelocity = SimAngularVelocity;
		TraceState.AnimLinearVelocity = AnimLinearVelocity;
		TraceState.AnimAngularVelocity = AnimAngularVelocity;
		TraceState.DatabaseSequenceFilter = DatabaseSequenceFilter;
		TraceState.DatabaseBlendSpaceFilter = DatabaseBlendSpaceFilter;
		UE_TRACE_POSE_SEARCH_MOTION_MATCHING_STATE(Context, TraceState)
	}
#endif
}

const FPoseSearchIndexAsset* FMotionMatchingState::GetCurrentSearchIndexAsset() const
{
	if (CurrentSearchResult.IsValid())
	{
		return CurrentSearchResult.SearchIndexAsset;
	}

	return nullptr;
}

float FMotionMatchingState::ComputeJumpBlendTime(
	const UE::PoseSearch::FSearchResult& Result, 
	const FMotionMatchingSettings& Settings
) const
{
	const FPoseSearchIndexAsset* SearchIndexAsset = GetCurrentSearchIndexAsset();

	// Use alternate blend time when changing between mirrored and unmirrored
	float JumpBlendTime = Settings.BlendTime;
	if ((SearchIndexAsset != nullptr) && (Settings.MirrorChangeBlendTime > 0.0f))
	{
		if (Result.SearchIndexAsset->bMirrored != SearchIndexAsset->bMirrored)
		{
			JumpBlendTime = Settings.MirrorChangeBlendTime;
		}
	}

	return JumpBlendTime;
}

EPoseSearchPostSearchStatus UPoseSearchPostProcessor_Bias::PostProcess_Implementation(FPoseSearchCost& InOutCost) const
{
	InOutCost.SetDissimilarity(Multiplier * InOutCost.GetDissimilarity());
	InOutCost.SetCostAddend(Addend + InOutCost.GetCostAddend());

	return EPoseSearchPostSearchStatus::Continue;
}

#undef LOCTEXT_NAMESPACE