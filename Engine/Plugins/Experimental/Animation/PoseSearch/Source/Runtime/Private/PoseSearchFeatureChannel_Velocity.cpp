// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearchFeatureChannel_Velocity.h"
#include "PoseSearch/PoseSearchAssetIndexer.h"
#include "PoseSearch/PoseSearchAssetSampler.h"
#include "PoseSearch/PoseSearchContext.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchHistory.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearchFeatureChannel_Position.h"

void UPoseSearchFeatureChannel_Velocity::Finalize(UPoseSearchSchema* Schema)
{
	ChannelDataOffset = Schema->SchemaCardinality;
	ChannelCardinality = UE::PoseSearch::FFeatureVectorHelper::GetVectorCardinality(ComponentStripping);
	Schema->SchemaCardinality += ChannelCardinality;

	SchemaBoneIdx = Schema->AddBoneReference(Bone);
}

void UPoseSearchFeatureChannel_Velocity::AddDependentChannels(UPoseSearchSchema* Schema) const
{
	if (Schema->bInjectAdditionalDebugChannels)
	{
		UPoseSearchFeatureChannel_Position::FindOrAddToSchema(Schema, SampleTimeOffset, Bone.BoneName);
	}
}

void UPoseSearchFeatureChannel_Velocity::BuildQuery(UE::PoseSearch::FSearchContext& SearchContext, UE::PoseSearch::FFeatureVectorBuilder& InOutQuery) const
{
	using namespace UE::PoseSearch;

	const bool bIsCurrentResultValid = SearchContext.GetCurrentResult().IsValid() && SearchContext.GetCurrentResult().Database->Schema == InOutQuery.GetSchema();
	const bool bSkip = InputQueryPose != EInputQueryPose::UseCharacterPose && bIsCurrentResultValid;
	const bool bIsRootBone = InOutQuery.GetSchema()->IsRootBone(SchemaBoneIdx);
	if (bSkip || (!SearchContext.IsHistoryValid() && !bIsRootBone))
	{
		if (bIsCurrentResultValid)
		{
			const float LerpValue = InputQueryPose == EInputQueryPose::UseInterpolatedContinuingPose ? SearchContext.GetCurrentResult().LerpValue : 0.f;
			// @todo: we should normalize if EPoseSearchVelocityFlags::Normalize && LerpValue != 0
			FFeatureVectorHelper::EncodeVector(InOutQuery.EditValues(), ChannelDataOffset, SearchContext.GetCurrentResultPrevPoseVector(), SearchContext.GetCurrentResultPoseVector(), SearchContext.GetCurrentResultNextPoseVector(), LerpValue, false, ComponentStripping);
		}
		// else leave the InOutQuery set to zero since the SearchContext.History is invalid and it'll fail if we continue
	}
	else
	{
		// calculating the LinearVelocity for the bone indexed by SchemaBoneIdx
		FVector LinearVelocity = SearchContext.GetSampleVelocity(SampleTimeOffset, InOutQuery.GetSchema(), SchemaBoneIdx, RootSchemaBoneIdx, bUseCharacterSpaceVelocities, !bIsRootBone);
		if (bNormalize)
		{
			LinearVelocity = LinearVelocity.GetClampedToMaxSize(1.f);
		}

		FFeatureVectorHelper::EncodeVector(InOutQuery.EditValues(), ChannelDataOffset, LinearVelocity, ComponentStripping);
	}
}

#if ENABLE_DRAW_DEBUG
void UPoseSearchFeatureChannel_Velocity::DebugDraw(const UE::PoseSearch::FDebugDrawParams& DrawParams, TConstArrayView<float> PoseVector) const
{
	using namespace UE::PoseSearch;

	const FColor Color = DebugColor.ToFColor(true);
	const float LinearVelocityScale = bNormalize ? 15.f : 0.08f;

	const FVector LinearVelocity = DrawParams.GetRootTransform().TransformVector(FFeatureVectorHelper::DecodeVector(PoseVector, ChannelDataOffset, ComponentStripping));
	const FVector BoneVelDirection = LinearVelocity.GetSafeNormal();
	const FVector BonePos = DrawParams.ExtractPosition(PoseVector, SampleTimeOffset, SchemaBoneIdx);

	DrawParams.DrawLine(BonePos, BonePos + LinearVelocity * LinearVelocityScale, Color);
}
#endif // ENABLE_DRAW_DEBUG

#if WITH_EDITOR
void UPoseSearchFeatureChannel_Velocity::FillWeights(TArray<float>& Weights) const
{
	for (int32 i = 0; i < ChannelCardinality; ++i)
	{
		Weights[ChannelDataOffset + i] = Weight;
	}
}

void UPoseSearchFeatureChannel_Velocity::IndexAsset(UE::PoseSearch::FAssetIndexer& Indexer) const
{
	using namespace UE::PoseSearch;

	for (int32 SampleIdx = Indexer.GetBeginSampleIdx(); SampleIdx != Indexer.GetEndSampleIdx(); ++SampleIdx)
	{
		FVector LinearVelocity = Indexer.GetSampleVelocity(SampleTimeOffset, SampleIdx, SchemaBoneIdx, RootSchemaBoneIdx, bUseCharacterSpaceVelocities);
		if (bNormalize)
		{
			LinearVelocity = LinearVelocity.GetClampedToMaxSize(1.f);
		}
		FFeatureVectorHelper::EncodeVector(Indexer.GetPoseVector(SampleIdx), ChannelDataOffset, LinearVelocity, ComponentStripping);
	}
}

FString UPoseSearchFeatureChannel_Velocity::GetLabel() const
{
	TStringBuilder<256> Label;
	if (const UPoseSearchFeatureChannel* OuterChannel = Cast<UPoseSearchFeatureChannel>(GetOuter()))
	{
		Label.Append(OuterChannel->GetLabel());
		Label.Append(TEXT("_"));
	}

	Label.Append(TEXT("Vel"));
	if (bNormalize)
	{
		Label.Append(TEXT("Dir"));
	}

	if (ComponentStripping == EComponentStrippingVector::StripXY)
	{
		Label.Append(TEXT("_z"));
	}
	else if (ComponentStripping == EComponentStrippingVector::StripZ)
	{
		Label.Append(TEXT("_xy"));
	}

	const UPoseSearchSchema* Schema = GetSchema();
	check(Schema);
	if (!Schema->IsRootBone(SchemaBoneIdx))
	{
		Label.Append(TEXT("_"));
		Label.Append(Schema->BoneReferences[SchemaBoneIdx].BoneName.ToString());
	}

	Label.Appendf(TEXT(" %.2f"), SampleTimeOffset);
	return Label.ToString();
}
#endif