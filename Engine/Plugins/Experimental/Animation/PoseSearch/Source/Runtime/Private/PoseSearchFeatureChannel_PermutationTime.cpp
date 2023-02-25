// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearchFeatureChannel_PermutationTime.h"
#include "PoseSearch/PoseSearchAssetIndexer.h"
#include "PoseSearch/PoseSearchContext.h"
#include "PoseSearch/PoseSearchSchema.h"

void UPoseSearchFeatureChannel_PermutationTime::Finalize(UPoseSearchSchema* Schema)
{
	ChannelDataOffset = Schema->SchemaCardinality;
	ChannelCardinality = 1;
	Schema->SchemaCardinality += ChannelCardinality;
}

void UPoseSearchFeatureChannel_PermutationTime::BuildQuery(UE::PoseSearch::FSearchContext& SearchContext, FPoseSearchFeatureVectorBuilder& InOutQuery) const
{
	using namespace UE::PoseSearch;
	FFeatureVectorHelper::EncodeFloat(InOutQuery.EditValues(), ChannelDataOffset, SearchContext.DesiredPermutationTimeOffset);
}

#if WITH_EDITOR
void UPoseSearchFeatureChannel_PermutationTime::FillWeights(TArray<float>& Weights) const
{
	for (int32 i = 0; i < ChannelCardinality; ++i)
	{
		Weights[ChannelDataOffset + i] = Weight;
	}
}

void UPoseSearchFeatureChannel_PermutationTime::IndexAsset(UE::PoseSearch::FAssetIndexer& Indexer) const
{
	using namespace UE::PoseSearch;

	const float PermutationTimeOffset = Indexer.CalculatePermutationTimeOffset();
	for (int32 SampleIdx = Indexer.GetBeginSampleIdx(); SampleIdx != Indexer.GetEndSampleIdx(); ++SampleIdx)
	{
		FFeatureVectorHelper::EncodeFloat(Indexer.GetPoseVector(SampleIdx), ChannelDataOffset, PermutationTimeOffset);
	}
}

FString UPoseSearchFeatureChannel_PermutationTime::GetLabel() const
{
	TStringBuilder<256> Label;
	if (const UPoseSearchFeatureChannel* OuterChannel = Cast<UPoseSearchFeatureChannel>(GetOuter()))
	{
		Label.Append(OuterChannel->GetLabel());
		Label.Append(TEXT("_"));
	}

	Label.Append(TEXT("PermTime"));
	return Label.ToString();
}
#endif