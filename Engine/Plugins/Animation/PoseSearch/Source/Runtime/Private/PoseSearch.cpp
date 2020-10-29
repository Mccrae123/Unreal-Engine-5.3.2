// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearch/PoseSearch.h"

#include "Async/ParallelFor.h"
#include "Features/IModularFeatures.h"
#include "DrawDebugHelpers.h"
#include "Animation/AnimPoseSearchProvider.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "PoseSearch/AnimNode_PoseSearchHistoryCollector.h"


namespace UE { namespace PoseSearch {

//////////////////////////////////////////////////////////////////////////
// FFeatureTypeTraits

struct FFeatureTypeTraits
{
	EPoseSearchFeatureType Type = EPoseSearchFeatureType::Invalid;
	uint32 NumFloats = 0;
};

// Could upgrade to class objects in the future with value reader/writer functions
static constexpr FFeatureTypeTraits FeatureTypeTraits[] =
{
	{ EPoseSearchFeatureType::Position, 3 },
	{ EPoseSearchFeatureType::Rotation, 6 },
	{ EPoseSearchFeatureType::LinearVelocity, 3 },
	{ EPoseSearchFeatureType::AngularVelocity, 3 },
};

FFeatureTypeTraits GetFeatureTypeTraits(EPoseSearchFeatureType Type)
{
	// Could allow external registration to a TSet of traits in the future
	// For now just use a simple local array
	for (const FFeatureTypeTraits& Traits : FeatureTypeTraits)
	{
		if (Traits.Type == Type)
		{
			return Traits;
		}
	}

	return FFeatureTypeTraits();
}

}} // namespace UE::PoseSearch


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureDesc

bool FPoseSearchFeatureDesc::operator==(const FPoseSearchFeatureDesc& Other) const
{
	return
		(SchemaBoneIdx == Other.SchemaBoneIdx) &&
		(SubsampleIdx == Other.SubsampleIdx) &&
		(Type == Other.Type) &&
		(Domain == Other.Domain);
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureVectorLayout

void FPoseSearchFeatureVectorLayout::Init()
{
	uint32 FloatCount = 0;

	for (FPoseSearchFeatureDesc& Element : Features)
	{
		Element.ValueOffset = FloatCount;
		FloatCount += UE::PoseSearch::GetFeatureTypeTraits(Element.Type).NumFloats;
	}

	NumFloats = FloatCount;
}

void FPoseSearchFeatureVectorLayout::Reset()
{
	Features.Reset();
	NumFloats = 0;
}

bool FPoseSearchFeatureVectorLayout::IsValid() const
{
	return NumFloats != 0.0f;
}


//////////////////////////////////////////////////////////////////////////
// UPoseSearchSchema

void UPoseSearchSchema::PreSave(const class ITargetPlatform* TargetPlatform)
{
	// Initialize references to obtain bone indices
	for (FBoneReference& BoneRef : Bones)
	{
		BoneRef.Initialize(Skeleton);
	}

	// Fill out bone index array and sort by bone index
	BoneIndices.SetNum(Bones.Num());
	for (int32 Index = 0; Index != Bones.Num(); ++Index)
	{
		BoneIndices[Index] = Bones[Index].BoneIndex;
	}
	BoneIndices.Sort();

	// Build separate index array with parent indices guaranteed to be present
	BoneIndicesWithParents = BoneIndices;
	if (Skeleton)
	{
		FAnimationRuntime::EnsureParentsPresent(BoneIndicesWithParents, Skeleton->GetReferenceSkeleton());
	}

	BoneIndices = BoneIndicesWithParents;

	PoseSampleOffsets.Sort(TLess<>());
	TrajectorySampleOffsets.Sort(TLess<>());
	TrajectoryDistanceOffsets.Sort(TLess<>());

	GenerateLayout();

	Super::PreSave(TargetPlatform);
}

bool UPoseSearchSchema::IsValid() const
{
	return (Skeleton != nullptr) && Layout.IsValid();
}

void UPoseSearchSchema::GenerateLayout()
{
	Layout.Reset();

	for (int32 TrajectoryTimeSubsampleIdx = 0; TrajectoryTimeSubsampleIdx != TrajectorySampleOffsets.Num(); ++TrajectoryTimeSubsampleIdx)
	{
		FPoseSearchFeatureDesc Element;
		Element.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		Element.SubsampleIdx = TrajectoryTimeSubsampleIdx;
		Element.Domain = EPoseSearchFeatureDomain::Time;

		if (bUseTrajectoryPositions)
		{
			Element.Type = EPoseSearchFeatureType::Position;
			Layout.Features.Add(Element);
		}

		if (bUseTrajectoryVelocities)
		{
			Element.Type = EPoseSearchFeatureType::LinearVelocity;
			Layout.Features.Add(Element);
		}
	}

 	for (int32 TrajectoryDistSubsampleIdx = 0; TrajectoryDistSubsampleIdx != TrajectoryDistanceOffsets.Num(); ++TrajectoryDistSubsampleIdx)
 	{
 		FPoseSearchFeatureDesc Element;
 		Element.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
 		Element.SubsampleIdx = TrajectoryDistSubsampleIdx;
 		Element.Domain = EPoseSearchFeatureDomain::Distance;

		if (bUseTrajectoryPositions)
		{
			Element.Type = EPoseSearchFeatureType::Position;
			Layout.Features.Add(Element);
		}

		if (bUseTrajectoryVelocities)
		{
			Element.Type = EPoseSearchFeatureType::LinearVelocity;
			Layout.Features.Add(Element);
		}
 	}

	for (int32 PoseSubsampleIdx = 0; PoseSubsampleIdx != PoseSampleOffsets.Num(); ++PoseSubsampleIdx)
	{
		FPoseSearchFeatureDesc Element;
		Element.SubsampleIdx = PoseSubsampleIdx;
		Element.Domain = EPoseSearchFeatureDomain::Time;

		for (int32 SchemaBoneIdx = 0; SchemaBoneIdx != BoneIndices.Num(); ++SchemaBoneIdx)
		{
			Element.SchemaBoneIdx = SchemaBoneIdx;
			if (bUseBonePositions)
			{
				Element.Type = EPoseSearchFeatureType::Position;
				Layout.Features.Add(Element);
			}

			if (bUseBoneVelocities)
			{
				Element.Type = EPoseSearchFeatureType::LinearVelocity;
				Layout.Features.Add(Element);
			}
		}
	}

	Layout.Init();
}

//////////////////////////////////////////////////////////////////////////
// FPoseSearchIndex

bool FPoseSearchIndex::IsValid() const
{
	bool bSchemaValid = Schema && Schema->IsValid();
	bool bSearchIndexValid = bSchemaValid && (NumPoses * Schema->Layout.NumFloats == Values.Num());

	return bSearchIndexValid;
}


//////////////////////////////////////////////////////////////////////////
// UPoseSearchSequenceMetaData

void UPoseSearchSequenceMetaData::PreSave(const class ITargetPlatform* TargetPlatform)
{
	if (Schema && SamplingRange.Size() > 0.0f)
	{
		UObject* Outer = GetOuter();
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(Outer))
		{
			UE::PoseSearch::BuildIndex(Sequence, this);
		}
	}

	Super::PreSave(TargetPlatform);
}

bool UPoseSearchSequenceMetaData::IsValidForIndexing() const
{
	return Schema && Schema->IsValid() && (SamplingRange.Size() > 0.0f);
}

bool UPoseSearchSequenceMetaData::IsValidForSearch() const
{
	return IsValidForIndexing() && SearchIndex.IsValid();
}


//////////////////////////////////////////////////////////////////////////
// UPoseSearchDatabase

const FPoseSearchDatabaseSequence* UPoseSearchDatabase::FindSequenceByPoseIdx(int32 PoseIdx) const
{
	for (const FPoseSearchDatabaseSequence& Sequence : Sequences)
	{
		if (PoseIdx >= Sequence.FirstPoseIdx && PoseIdx < Sequence.FirstPoseIdx + Sequence.NumPoses)
		{
			return &Sequence;
		}
	}

	return nullptr;
}

bool UPoseSearchDatabase::IsValidForIndexing() const
{
	return Schema && Schema->IsValid() && !Sequences.IsEmpty();
}

bool UPoseSearchDatabase::IsValidForSearch() const
{
	return IsValidForIndexing() && SearchIndex.IsValid();
}

void UPoseSearchDatabase::PreSave(const class ITargetPlatform* TargetPlatform)
{
	if (IsValidForIndexing())
	{
		UE::PoseSearch::BuildIndex(this);
	}

	Super::PreSave(TargetPlatform);
}


namespace UE { namespace PoseSearch {

//////////////////////////////////////////////////////////////////////////
// FFeatureVectorBuilder

void FFeatureVectorBuilder::Init(const FPoseSearchFeatureVectorLayout* InLayout, TArrayView<float> Buffer)
{
	check(InLayout);
	check(Buffer.Num() == InLayout->NumFloats);
	Layout = InLayout;
	Values = Buffer;
	ResetFeatures();
}

void FFeatureVectorBuilder::ResetFeatures()
{
	NumFeaturesAdded = 0;
	FeaturesAdded.Init(false, Layout->Features.Num());
}

void FFeatureVectorBuilder::SetTransform(FPoseSearchFeatureDesc Element, const FTransform& Transform)
{
	SetPosition(Element, Transform.GetTranslation());
	SetRotation(Element, Transform.GetRotation());
}

void FFeatureVectorBuilder::SetTransformDerivative(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	SetLinearVelocity(Element, Transform, PrevTransform, DeltaTime);
	SetAngularVelocity(Element, Transform, PrevTransform, DeltaTime);
}

void FFeatureVectorBuilder::SetPosition(FPoseSearchFeatureDesc Element, const FVector& Position)
{
	Element.Type = EPoseSearchFeatureType::Position;
	SetVector(Element, Position);
}

void FFeatureVectorBuilder::SetRotation(FPoseSearchFeatureDesc Element, const FQuat& Rotation)
{
	Element.Type = EPoseSearchFeatureType::Rotation;
	int32 ElementIndex = Layout->Features.Find(Element);
	if (ElementIndex >= 0)
	{
		FVector X = Rotation.GetAxisX();
		FVector Y = Rotation.GetAxisY();

		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = X.X;
		Values[FoundElement.ValueOffset + 1] = X.Y;
		Values[FoundElement.ValueOffset + 2] = X.Z;
		Values[FoundElement.ValueOffset + 3] = Y.X;
		Values[FoundElement.ValueOffset + 4] = Y.Y;
		Values[FoundElement.ValueOffset + 5] = Y.Z;

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

void FFeatureVectorBuilder::SetLinearVelocity(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	Element.Type = EPoseSearchFeatureType::LinearVelocity;
	FVector LinearVelocity = (Transform.GetTranslation() - PrevTransform.GetTranslation()) / DeltaTime;
	SetVector(Element, LinearVelocity);
}

void FFeatureVectorBuilder::SetAngularVelocity(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	Element.Type = EPoseSearchFeatureType::AngularVelocity;
	int32 ElementIndex = Layout->Features.Find(Element);
	if (ElementIndex >= 0)
	{
		FQuat Q0 = PrevTransform.GetRotation();
		FQuat Q1 = Transform.GetRotation();
		Q1.EnforceShortestArcWith(Q0);

		// Given angular velocity vector w, quaternion differentiation can be represented as
		//   dq/dt = (w * q)/2
		// Solve for w
		//   w = 2 * dq/dt * q^-1
		// And let dq/dt be expressed as the finite difference
		//   dq/dt = (q(t+h) - q(t)) / h
		FQuat DQDt = (Q1 - Q0) / DeltaTime;
		FQuat QInv = Q0.Inverse();
		FQuat W = (DQDt * QInv) * 2.0f;

		FVector AngularVelocity(W.X, W.Y, W.Z);

		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = AngularVelocity[0];
		Values[FoundElement.ValueOffset + 1] = AngularVelocity[1];
		Values[FoundElement.ValueOffset + 2] = AngularVelocity[2];

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

void FFeatureVectorBuilder::SetVector(FPoseSearchFeatureDesc Element, const FVector& Vector)
{
	int32 ElementIndex = Layout->Features.Find(Element);
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = Vector[0];
		Values[FoundElement.ValueOffset + 1] = Vector[1];
		Values[FoundElement.ValueOffset + 2] = Vector[2];

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

bool FFeatureVectorBuilder::SetPoseFeatures(const UPoseSearchSchema* Schema, FPoseHistory* History)
{
	check(Schema && Schema->IsValid());
	check(History);

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;

	for (int32 SubsampleIdx = 0; SubsampleIdx != Schema->PoseSampleOffsets.Num(); ++SubsampleIdx)
	{
		Feature.SubsampleIdx = SubsampleIdx;

		int32 Offset = Schema->PoseSampleOffsets[SubsampleIdx];
		float TimeDelta = -Offset * (1.0f /  Schema->SampleRate);

		if (!History->Sample(TimeDelta, Schema->Skeleton->GetReferenceSkeleton(), Schema->BoneIndicesWithParents))
		{
			return false;
		}

		TArrayView<const FTransform> ComponentPose = History->GetComponentPoseSample();
		TArrayView<const FTransform> ComponentPrevPose = History->GetPrevComponentPoseSample();
		for (int32 SchemaBoneIdx = 0; SchemaBoneIdx != Schema->BoneIndices.Num(); ++SchemaBoneIdx)
		{
			Feature.SchemaBoneIdx = SchemaBoneIdx;

			int32 SkeletonBoneIndex = Schema->BoneIndices[SchemaBoneIdx]; // @@@ need to resolve bone indices at runtime, hold in FPoseHistory?
			const FTransform& Transform = ComponentPose[SkeletonBoneIndex];
			const FTransform& PrevTransform = ComponentPrevPose[SkeletonBoneIndex];
			SetTransform(Feature, Transform);
			SetTransformDerivative(Feature, Transform, PrevTransform, History->GetSampleInterval());
		}
	}

	return true;
}

bool FFeatureVectorBuilder::IsComplete() const
{
	return NumFeaturesAdded == Layout->Features.Num();
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureVectorReader

void FFeatureVectorReader::Init(const FPoseSearchFeatureVectorLayout* InLayout)
{
	check(InLayout);
	Layout = InLayout;
}

void FFeatureVectorReader::SetValues(TArrayView<const float> InValues)
{
	check(Layout);
	check(Layout->NumFloats == InValues.Num());
	Values = InValues;
}

bool FFeatureVectorReader::IsValid() const
{
	return Layout && (Layout->NumFloats == Values.Num());
}

bool FFeatureVectorReader::GetTransform(FPoseSearchFeatureDesc Element, FTransform* OutTransform) const
{
	FVector Position;
	bool bResult = GetPosition(Element, &Position);

	FQuat Rotation;
	bResult |= GetRotation(Element, &Rotation);

	OutTransform->SetComponents(Rotation, Position, FVector::OneVector);
	return bResult;
}

bool FFeatureVectorReader::GetPosition(FPoseSearchFeatureDesc Element, FVector* OutPosition) const
{
	Element.Type = EPoseSearchFeatureType::Position;
	return GetVector(Element, OutPosition);
}

bool FFeatureVectorReader::GetRotation(FPoseSearchFeatureDesc Element, FQuat* OutRotation) const
{
	Element.Type = EPoseSearchFeatureType::Rotation;
	int32 ElementIndex = IsValid() ? Layout->Features.Find(Element) : -1;
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		FVector X;
		FVector Y;

		X.X = Values[FoundElement.ValueOffset + 0];
		X.Y = Values[FoundElement.ValueOffset + 1];
		X.Z = Values[FoundElement.ValueOffset + 2];
		Y.X = Values[FoundElement.ValueOffset + 3];
		Y.Y = Values[FoundElement.ValueOffset + 4];
		Y.Z = Values[FoundElement.ValueOffset + 5];

		FVector Z = FVector::CrossProduct(X, Y);

		FMatrix M(FMatrix::Identity);
		M.SetColumn(0, X);
		M.SetColumn(1, Y);
		M.SetColumn(2, Z);

		*OutRotation = FQuat(M);
		return true;
	}

	*OutRotation = FQuat::Identity;
	return false;
}

bool FFeatureVectorReader::GetLinearVelocity(FPoseSearchFeatureDesc Element, FVector* OutLinearVelocity) const
{
	Element.Type = EPoseSearchFeatureType::LinearVelocity;
	return GetVector(Element, OutLinearVelocity);
}

bool FFeatureVectorReader::GetAngularVelocity(FPoseSearchFeatureDesc Element, FVector* OutAngularVelocity) const
{
	Element.Type = EPoseSearchFeatureType::AngularVelocity;
	return GetVector(Element, OutAngularVelocity);
}

bool FFeatureVectorReader::GetVector(FPoseSearchFeatureDesc Element, FVector* OutVector) const
{
	int32 ElementIndex = IsValid() ? Layout->Features.Find(Element) : -1;
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		FVector V;
		V.X = Values[FoundElement.ValueOffset + 0];
		V.Y = Values[FoundElement.ValueOffset + 1];
		V.Z = Values[FoundElement.ValueOffset + 2];
		*OutVector = V;
		return true;
	}

	*OutVector = FVector::ZeroVector;
	return false;
}


//////////////////////////////////////////////////////////////////////////
// FPoseHistory

/**
* Fills skeleton transforms with evaluated compact pose transforms.
* Bones that weren't evaluated are filled with the bone's reference pose.
*/
static void CopyCompactToSkeletonPose(const FCompactPose& Pose, TArray<FTransform>& OutLocalTransforms)
{
	const FBoneContainer& BoneContainer = Pose.GetBoneContainer();
	const FReferenceSkeleton& RefSkeleton = BoneContainer.GetReferenceSkeleton();
	TArrayView<const FTransform> RefSkeletonTransforms = MakeArrayView(RefSkeleton.GetRefBonePose());

	const int32 NumSkeletonBones = BoneContainer.GetNumBones();
	OutLocalTransforms.SetNum(NumSkeletonBones);

	for (auto SkeletonBoneIdx = FSkeletonPoseBoneIndex(0); SkeletonBoneIdx != NumSkeletonBones; ++SkeletonBoneIdx)
	{
		FCompactPoseBoneIndex CompactBoneIdx = BoneContainer.GetCompactPoseIndexFromSkeletonIndex(SkeletonBoneIdx.GetInt());
		OutLocalTransforms[SkeletonBoneIdx.GetInt()] = CompactBoneIdx.IsValid() ? Pose[CompactBoneIdx] : RefSkeletonTransforms[SkeletonBoneIdx.GetInt()];
	}
}

void FPoseHistory::Init(int32 InNumPoses, float InTimeHorizon)
{
	int32 Capacity = FMath::RoundUpToPowerOfTwo(InNumPoses);

	if ((Queue.GetCapacity() == Capacity) && (TimeHorizon == InTimeHorizon))
	{
		return;
	}

	Poses.SetNum(Capacity);
	Knots.SetNum(Capacity);
	Queue.Init(Capacity);
	TimeHorizon = InTimeHorizon;
}

void FPoseHistory::Init(const FPoseHistory& History)
{
	Poses = History.Poses;
	Knots = History.Knots;
	Queue = History.Queue;
	TimeHorizon = History.TimeHorizon;
}

bool FPoseHistory::SampleLocalPose(float SecondsAgo, const FReferenceSkeleton& RefSkeleton, const TArray<FBoneIndexType>& RequiredBones, TArray<FTransform>& LocalPose)
{
	// Find the lower bound knot
	uint32 NextIndex = MAX_uint32;
	int32 NextOffset = 1;
	for (; NextOffset < (int32)Queue.Num(); ++NextOffset)
	{
		uint32 TestIndex = Queue.GetOffsetFromBack(NextOffset);
		if (Knots[TestIndex] >= SecondsAgo)
		{
			NextIndex = TestIndex;
			break;
		}
	}

	if (NextIndex == MAX_uint32)
	{
		return false;
	}

	// Get the previous knot
	int32 PrevOffset = NextOffset - 1;
	uint32 PrevIndex = Queue.GetOffsetFromBack(PrevOffset);

	// Compute alpha between previous and next knots
	float Alpha = FMath::GetMappedRangeValueUnclamped(
		FVector2D(Knots[PrevIndex], Knots[NextIndex]),
		FVector2D(0.0f, 1.0f),
		SecondsAgo);

	TArray<FTransform>& PrevPose = Poses[PrevIndex].LocalTransforms;
	TArray<FTransform>& NextPose = Poses[NextIndex].LocalTransforms;

	// We may not have accumulated enough poses yet
	if (PrevPose.Num() != NextPose.Num())
	{
		return false;
	}

	if (RequiredBones.Num() > PrevPose.Num())
	{
		return false;
	}

	// Lerp between poses by alpha to produce output local pose at requested sample time
	LocalPose = PrevPose;
	FAnimationRuntime::LerpBoneTransforms(
		LocalPose,
		NextPose,
		Alpha,
		RequiredBones);

	return true;
}

bool FPoseHistory::Sample(float SecondsAgo, const FReferenceSkeleton& RefSkeleton, const TArray<FBoneIndexType>& RequiredBones)
{
	// Compute local space pose at requested time
	bool bSampled = SampleLocalPose(SecondsAgo, RefSkeleton, RequiredBones, SampledLocalPose);

	// Compute local space pose one sample interval in the past
	bSampled = bSampled && SampleLocalPose(SecondsAgo + GetSampleInterval(), RefSkeleton, RequiredBones, SampledPrevLocalPose);

	// Convert local to component space
	if (bSampled)
	{
		FAnimationRuntime::FillUpComponentSpaceTransforms(RefSkeleton, SampledLocalPose, SampledComponentPose);
		FAnimationRuntime::FillUpComponentSpaceTransforms(RefSkeleton, SampledPrevLocalPose, SampledPrevComponentPose);
	}

	return bSampled;
}

void FPoseHistory::Update(float SecondsElapsed, const FCompactPose& Pose)
{
	// Age our elapsed times
	for (int32 Offset = 0; Offset != (int32)Queue.Num(); ++Offset)
	{
		uint32 Index = Queue.GetOffsetFromFront(Offset);
		Knots[Index] += SecondsElapsed;
	}

	if (!Queue.IsFull())
	{
		// Consume every pose until the queue is full
		Queue.PushBack();
	}
	else
	{
		// Exercise pose retention policy. We must guarantee there is always one additional knot
		// at or beyond the desired time horizon H so we can fulfill sample requests at t=H. We also
		// want to evenly distribute knots across the entire history buffer so we only push additional
		// poses when enough time has elapsed.

		const float SampleInterval = GetSampleInterval();

		uint32 SecondOldest = Queue.GetOffsetFromFront(1);
		bool bCanEvictOldest = Knots[SecondOldest] >= TimeHorizon;

		uint32 SecondNewest = Queue.GetOffsetFromBack(1);
		bool bShouldPushNewest = Knots[SecondNewest] >= SampleInterval;

		if (bCanEvictOldest && bShouldPushNewest)
		{
			Queue.PopFront();
			Queue.PushBack();
		}
	}

	// Regardless of the retention policy, we always update the most recent pose
	uint32 Newest = Queue.GetOffsetFromBack(0);
	Knots[Newest] = 0.0f;
	CopyCompactToSkeletonPose(Pose, Poses[Newest].LocalTransforms);
}

float FPoseHistory::GetSampleInterval() const
{
	return TimeHorizon / Queue.GetCapacity();
}


//////////////////////////////////////////////////////////////////////////
// FIndexer

class FSequenceIndexer
{
public:
	struct Result
	{
		int32 NumIndexedPoses;
		TArrayView<const float> Values;
	};
	Result Process(const UPoseSearchSchema* Schema, const UAnimSequence* Sequence, FFloatInterval SamplingRange);
	Result GetResult() const;

private:
	void SampleBegin(int32 SampleIdx);
	void SampleEnd();
	void ExtractPoses(const UAnimSequence* Sequence);
	void ExtractRootMotion(const UAnimSequence* Sequence);
	void AddPoseFeatures(int32 SampleIdx);
	void AddTrajectoryTimeFeatures(int32 SampleIdx);
	void AddTrajectoryDistanceFeatures(int32 SampleIdx);

	struct FSampleContext
	{
		TArray<FTransform> ComponentSpacePose;		// Indexed by SampleIdx * NumBones + SchemaBoneIdx
		TArray<FTransform> LocalRootMotion;			// Indexed by SampleIdx
		TArray<FTransform> AccumulatedRootMotion;	// Indexed by SampleIdx
		TArray<float> AccumulatedRootDistance;		// Indexed by SampleIdx

		int32 TotalSamples = 0;
		int32 FirstIndexedSample = 0;
		int32 LastIndexedSample = 0;
		int32 NumIndexedSamples = 0;
		int32 NumBones = 0;

		void Reset();
		void Reserve();
	};

	const UPoseSearchSchema* Schema = nullptr;

	TArray<float> Values;
	
	FFeatureVectorBuilder Builder;
	FSampleContext Context;

	float DeltaTime = 0.0f; //@@@ move to Schema?
};

void FSequenceIndexer::FSampleContext::Reset()
{
	TotalSamples = 0;
	FirstIndexedSample = 0;
	LastIndexedSample = 0;
	NumIndexedSamples = 0;
	NumBones = 0;

	ComponentSpacePose.Reset(0);
	LocalRootMotion.Reset(0);
	AccumulatedRootMotion.Reset(0);
	AccumulatedRootDistance.Reset(0);
}

void FSequenceIndexer::FSampleContext::Reserve()
{
	ComponentSpacePose.Reserve(NumBones * TotalSamples);
	LocalRootMotion.Reserve(TotalSamples);
	AccumulatedRootMotion.Reserve(TotalSamples);
	AccumulatedRootDistance.Reserve(TotalSamples);
}

FSequenceIndexer::Result FSequenceIndexer::Process(const UPoseSearchSchema* InSchema, const UAnimSequence* Sequence, FFloatInterval SamplingRange)
{
	check(InSchema);
	check(Sequence);

	Schema = InSchema;

	USkeleton* Skeleton = Sequence->GetSkeleton();
	check(Skeleton);
	check(Skeleton->IsCompatible(Schema->Skeleton));

	const float BeginTime = SamplingRange.Min;
	const float EndTime = FMath::Min(Sequence->GetPlayLength(), SamplingRange.Max);

	DeltaTime = 1.0f / Schema->SampleRate;

	Context.Reset();
	Context.NumBones = Schema->BoneIndices.Num();
	Context.TotalSamples = FMath::FloorToInt(Sequence->GetPlayLength() * Schema->SampleRate);
	Context.FirstIndexedSample = FMath::FloorToInt(BeginTime * Schema->SampleRate);
	Context.LastIndexedSample = FMath::Max(0, FMath::FloorToInt(EndTime * Schema->SampleRate) - 1);
	Context.NumIndexedSamples = Context.LastIndexedSample - Context.FirstIndexedSample + 1;
	Context.Reserve();

	Values.SetNumZeroed(Schema->Layout.NumFloats * Context.NumIndexedSamples);

	ExtractPoses(Sequence);
	ExtractRootMotion(Sequence);

	for (int32 SampleIdx = Context.FirstIndexedSample; SampleIdx <= Context.LastIndexedSample; ++SampleIdx)
	{
		SampleBegin(SampleIdx);

		AddPoseFeatures(SampleIdx);
		AddTrajectoryTimeFeatures(SampleIdx);
		AddTrajectoryDistanceFeatures(SampleIdx);

		SampleEnd();
	}

	return GetResult();
}

FSequenceIndexer::Result FSequenceIndexer::GetResult() const
{
	Result Result;
	Result.NumIndexedPoses = Context.NumIndexedSamples;
	Result.Values = Values;
	return Result;
}

void FSequenceIndexer::SampleBegin(int32 SampleIdx)
{
	int32 FirstValueIdx = (SampleIdx - Context.FirstIndexedSample) * Schema->Layout.NumFloats;
	TArrayView<float> FeatureVectorValues = MakeArrayView(&Values[FirstValueIdx], Schema->Layout.NumFloats);
	Builder.Init(&Schema->Layout, FeatureVectorValues);
}

void FSequenceIndexer::SampleEnd()
{
	check(Builder.IsComplete());
}

void FSequenceIndexer::ExtractPoses(const UAnimSequence* Sequence)
{
	USkeleton* Skeleton = Sequence->GetSkeleton();
	FBoneContainer BoneContainer;
	BoneContainer.InitializeTo(Schema->BoneIndicesWithParents, FCurveEvaluationOption(false), *Skeleton);

	FCompactPose Pose;
	Pose.SetBoneContainer(&BoneContainer);
	FCSPose<FCompactPose> ComponentSpacePose;

	FBlendedCurve UnusedCurve;
	FStackCustomAttributes UnusedAttributes;

	FAnimExtractContext ExtractionCtx;
	// ExtractionCtx.PoseCurves is intentionally left empty
	// ExtractionCtx.BonesRequired is unused by UAnimSequence::GetAnimationPose
	ExtractionCtx.bExtractRootMotion = true;

	FAnimationPoseData AnimPoseData(Pose, UnusedCurve, UnusedAttributes);
	for (int32 SampleIdx = 0; SampleIdx != Context.TotalSamples; ++SampleIdx)
	{
		const float CurrentTime = SampleIdx * DeltaTime;

		ExtractionCtx.CurrentTime = CurrentTime;
		Sequence->GetAnimationPose(AnimPoseData, ExtractionCtx);
		ComponentSpacePose.InitPose(Pose);

		for (int32 BoneIndex : Schema->BoneIndices)
		{
			FCompactPoseBoneIndex CompactBoneIndex = BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(BoneIndex));
			const FTransform& Transform = ComponentSpacePose.GetComponentSpaceTransform(CompactBoneIndex);
			Context.ComponentSpacePose.Add(Transform);
		}
	}
}

void FSequenceIndexer::ExtractRootMotion(const UAnimSequence* Sequence)
{
	double AccumulatedRootDistance = 0.0;
	FTransform AccumulatedRootMotion = FTransform::Identity;
	for (int32 SampleIdx = 0; SampleIdx != Context.TotalSamples; ++SampleIdx)
	{
		const float CurrentTime = SampleIdx * DeltaTime;

		FTransform LocalRootMotion = Sequence->ExtractRootMotion(CurrentTime, DeltaTime, false /*!allowLooping*/);
		Context.LocalRootMotion.Add(LocalRootMotion);

		AccumulatedRootMotion = LocalRootMotion * AccumulatedRootMotion;
		AccumulatedRootDistance += LocalRootMotion.GetTranslation().Size();
		Context.AccumulatedRootMotion.Add(AccumulatedRootMotion);
		Context.AccumulatedRootDistance.Add((float)AccumulatedRootDistance);
	}
}

void FSequenceIndexer::AddPoseFeatures(int32 SampleIdx)
{
	FPoseSearchFeatureDesc CurrentElement;
	CurrentElement.Domain = EPoseSearchFeatureDomain::Time;

	FTransform SampleSpaceOrigin = Context.AccumulatedRootMotion[SampleIdx];
	
	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != Schema->PoseSampleOffsets.Num(); ++SchemaSubsampleIdx)
	{
		CurrentElement.SubsampleIdx = SchemaSubsampleIdx;

		const int32 SampleOffset = Schema->PoseSampleOffsets[SchemaSubsampleIdx];
		const int32 SubsampleIdx = FMath::Clamp(SampleIdx + SampleOffset, 0, Context.AccumulatedRootMotion.Num() - 1);

		FTransform SubsampleRoot = Context.AccumulatedRootMotion[SubsampleIdx];
		SubsampleRoot.SetToRelativeTransform(SampleSpaceOrigin);

		for (int32 SchemaBoneIndex = 0; SchemaBoneIndex != Context.NumBones; ++SchemaBoneIndex)
		{
			CurrentElement.SchemaBoneIdx = SchemaBoneIndex;

			int32 BoneSampleIdx = Context.NumBones * (SampleIdx + SampleOffset) + SchemaBoneIndex;
			int32 BonePrevSampleIdx = Context.NumBones * (SampleIdx - 1 + SampleOffset) + SchemaBoneIndex;
			
			// @@@Add extrapolation. Clamp for now
			BoneSampleIdx = FMath::Clamp(BoneSampleIdx, 0, Context.ComponentSpacePose.Num() - 1);
			BonePrevSampleIdx = FMath::Clamp(BonePrevSampleIdx, 0, Context.ComponentSpacePose.Num() - 1);

			FTransform BoneInSampleSpace = Context.ComponentSpacePose[BoneSampleIdx] * SubsampleRoot;
			FTransform BonePrevInSampleSpace = Context.ComponentSpacePose[BonePrevSampleIdx] * SubsampleRoot;

			Builder.SetTransform(CurrentElement, BoneInSampleSpace);
			Builder.SetTransformDerivative(CurrentElement, BoneInSampleSpace, BonePrevInSampleSpace, DeltaTime);
		}
	}
}

void FSequenceIndexer::AddTrajectoryTimeFeatures(int32 SampleIdx)
{
	FPoseSearchFeatureDesc CurrentElement;
	CurrentElement.Domain = EPoseSearchFeatureDomain::Time;
	CurrentElement.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	FTransform SampleSpaceOrigin = Context.AccumulatedRootMotion[SampleIdx];

	for (int32 SubsampleIdx = 0; SubsampleIdx != Schema->TrajectorySampleOffsets.Num(); ++SubsampleIdx)
	{
		CurrentElement.SubsampleIdx = SubsampleIdx;

		int32 RootMotionIdx = SampleIdx + Schema->TrajectorySampleOffsets[SubsampleIdx];
		int32 RootMotionPrevIdx = RootMotionIdx - 1;

		// @@@ Add extrapolation. Clamp for now
		RootMotionIdx = FMath::Clamp(RootMotionIdx, 0, Context.AccumulatedRootMotion.Num() - 1);
		RootMotionPrevIdx = FMath::Clamp(RootMotionPrevIdx, 0, Context.AccumulatedRootMotion.Num() - 1);

		FTransform SubsampleRoot = Context.AccumulatedRootMotion[RootMotionIdx];
		SubsampleRoot.SetToRelativeTransform(SampleSpaceOrigin);

		FTransform SubsamplePrevRoot = Context.AccumulatedRootMotion[RootMotionPrevIdx];
		SubsamplePrevRoot.SetToRelativeTransform(SampleSpaceOrigin);

		Builder.SetTransform(CurrentElement, SubsampleRoot);
		Builder.SetTransformDerivative(CurrentElement, SubsampleRoot, SubsamplePrevRoot, DeltaTime);
	}
}

void FSequenceIndexer::AddTrajectoryDistanceFeatures(int32 SampleIdx)
{
	FPoseSearchFeatureDesc CurrentElement;
	CurrentElement.Domain = EPoseSearchFeatureDomain::Distance;
	CurrentElement.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	TArrayView<const float> AccumulatedRootDistances = Context.AccumulatedRootDistance;

	FTransform SampleSpaceOrigin = Context.AccumulatedRootMotion[SampleIdx];

	for (int32 SubsampleIdx = 0; SubsampleIdx != Schema->TrajectoryDistanceOffsets.Num(); ++SubsampleIdx)
	{
		CurrentElement.SubsampleIdx = SubsampleIdx;

		const float TrajectoryDistance = Schema->TrajectoryDistanceOffsets[SubsampleIdx];
		const float SampleAccumulatedRootDistance = TrajectoryDistance + AccumulatedRootDistances[SampleIdx];

		int32 LowerBoundSampleIdx = Algo::LowerBound(AccumulatedRootDistances, SampleAccumulatedRootDistance);

		// @@@ Add extrapolation. Clamp for now
		int32 PrevSampleIdx = FMath::Clamp(LowerBoundSampleIdx - 1, 0, AccumulatedRootDistances.Num() - 1);
		int32 NextSampleIdx = FMath::Clamp(LowerBoundSampleIdx, 0, AccumulatedRootDistances.Num() - 1);

		const float PrevSampleDistance = AccumulatedRootDistances[PrevSampleIdx];
		const float NextSampleDistance = AccumulatedRootDistances[NextSampleIdx];

		FTransform PrevRootInSampleSpace = Context.AccumulatedRootMotion[PrevSampleIdx];
		PrevRootInSampleSpace.SetToRelativeTransform(SampleSpaceOrigin);

		FTransform NextRootInSampleSpace = Context.AccumulatedRootMotion[NextSampleIdx];
		NextRootInSampleSpace.SetToRelativeTransform(SampleSpaceOrigin);
		
		float Alpha = FMath::GetRangePct(PrevSampleDistance, NextSampleDistance, SampleAccumulatedRootDistance);
		FTransform BlendedRootInSampleSpace;
		BlendedRootInSampleSpace.Blend(PrevRootInSampleSpace, NextRootInSampleSpace, Alpha);

		Builder.SetTransform(CurrentElement, BlendedRootInSampleSpace);
	}
}


//////////////////////////////////////////////////////////////////////////
// PoseSearch API

static void DrawFeatureVector(const FDebugDrawParams& DrawParams, const FFeatureVectorReader& Reader, const FLinearColor& Color1, const FLinearColor& Color2)
{
	const float LifeTime = DrawParams.DefaultLifeTime;
	const uint8 DepthPriority = ESceneDepthPriorityGroup::SDPG_Foreground + 2;

	FPoseSearchFeatureDesc Element;
	Element.Domain = EPoseSearchFeatureDomain::Time;

	const int32 NumSubsamples = DrawParams.SearchIndex->Schema->PoseSampleOffsets.Num();
	const int32 NumBones = DrawParams.SearchIndex->Schema->BoneIndices.Num();

	if ((NumSubsamples * NumBones) == 0)
	{
		return;
	}

	for (int32 SubsampleIdx = 0; SubsampleIdx != NumSubsamples; ++SubsampleIdx)
	{
		Element.SubsampleIdx = SubsampleIdx;
		float Lerp = (SubsampleIdx + 1.0f) / NumSubsamples;
		FColor Color = FLinearColor::LerpUsingHSV(Color1, Color2, Lerp).ToFColor(true);

		FTransform Adjust;
		Adjust.SetTranslation(FVector((SubsampleIdx - 2.0f) * 5.0f, 0, 0));
		Adjust *= DrawParams.ComponentTransform;

		Element.SchemaBoneIdx = 0;
		FVector BonePosPrev;
		Reader.GetPosition(Element, &BonePosPrev);
		BonePosPrev = Adjust.TransformPosition(BonePosPrev);


		FVector BoneVel;
		Reader.GetLinearVelocity(Element, &BoneVel);
		BoneVel *= 0.1f;
		DrawDebugDirectionalArrow(DrawParams.World, BonePosPrev, BonePosPrev + BoneVel, 5.0f, FColor::Red, false, LifeTime, DepthPriority, 0.0f);
		
		for (int32 SchemaBoneIdx = 1; SchemaBoneIdx != NumBones; ++SchemaBoneIdx)
		{
			Element.SchemaBoneIdx = SchemaBoneIdx;

			FVector BonePosNext;
			Reader.GetPosition(Element, &BonePosNext);
			BonePosNext = Adjust.TransformPosition(BonePosNext);

			DrawDebugPoint(DrawParams.World, BonePosNext, 1.0f, Color, false, LifeTime, DepthPriority);

			Reader.GetLinearVelocity(Element, &BoneVel);
			BoneVel *= 0.1f;
			DrawDebugDirectionalArrow(DrawParams.World, BonePosNext, BonePosNext + BoneVel, 5.0f, FColor::Red, false, LifeTime, DepthPriority, 0.0f);

			bool bIsChildOfPrev = DrawParams.SearchIndex->Schema->Skeleton->GetReferenceSkeleton().BoneIsChildOf(
				DrawParams.SearchIndex->Schema->BoneIndices[SchemaBoneIdx],
				DrawParams.SearchIndex->Schema->BoneIndices[SchemaBoneIdx-1]);

			if (bIsChildOfPrev)
			{
				DrawDebugLine(DrawParams.World, BonePosPrev, BonePosNext, Color, false, LifeTime, DepthPriority);
			}
			BonePosPrev = BonePosNext;
		}
	}
}

static void DrawSearchIndex(const FDebugDrawParams& DrawParams)
{
	if (!DrawParams.CanDraw())
	{
		return;
	}

	FFeatureVectorReader Reader;
	Reader.Init(&DrawParams.SearchIndex->Schema->Layout);

	int32 LastPoseIdx = DrawParams.SearchIndex->NumPoses;
	int32 StartPoseIdx = 0;
	if (!(DrawParams.Flags & EDebugDrawFlags::DrawSearchIndex))
	{
		StartPoseIdx = DrawParams.HighlightPoseIdx;
		LastPoseIdx = StartPoseIdx + 1;
	}

	if (StartPoseIdx < 0)
	{
		return;
	}

	for (int32 PoseIdx = StartPoseIdx; PoseIdx != LastPoseIdx; ++PoseIdx)
	{
		FLinearColor Color1;
		FLinearColor Color2;
		if (PoseIdx == DrawParams.HighlightPoseIdx)
		{
			Color1 = Color2 = FLinearColor::Yellow;
		}
		else
		{
			float Lerp = (float)(PoseIdx + 1) / DrawParams.SearchIndex->NumPoses;
			Color1 = FLinearColor(FColor::Cyan);
			Color2 = FLinearColor(FColor::Blue);
		}

		// @@@ make a search index helper for pose slicing
		int32 ValueOffset = PoseIdx * DrawParams.SearchIndex->Schema->Layout.NumFloats;
		TArrayView<const float> Values = MakeArrayView(&DrawParams.SearchIndex->Values[ValueOffset], DrawParams.SearchIndex->Schema->Layout.NumFloats);
		Reader.SetValues(Values);
		
		DrawFeatureVector(DrawParams, Reader, Color1, Color2);
	}
}

static void DrawQuery(const FDebugDrawParams& DrawParams)
{
	if (!DrawParams.CanDraw())
	{
		return;
	}

	FFeatureVectorReader Reader;
	Reader.Init(&DrawParams.SearchIndex->Schema->Layout);
	Reader.SetValues(DrawParams.Query);
	DrawFeatureVector(DrawParams, Reader, FLinearColor(FColor::Magenta), FLinearColor(FColor::Purple));
}

void Draw(const FDebugDrawParams& DebugDrawParams)
{
	if (DebugDrawParams.CanDraw())
	{
		if (EnumHasAnyFlags(DebugDrawParams.Flags, EDebugDrawFlags::DrawQuery))
		{
			DrawQuery(DebugDrawParams);
		}

		if (EnumHasAnyFlags(DebugDrawParams.Flags, EDebugDrawFlags::DrawSearchIndex | EDebugDrawFlags::DrawBest))
		{
			DrawSearchIndex(DebugDrawParams);
		}
	}
}

bool BuildIndex(const UAnimSequence* Sequence, UPoseSearchSequenceMetaData* SequenceMetaData)
{
	check(Sequence);
	check(SequenceMetaData);

	if (!SequenceMetaData->IsValidForIndexing())
	{
		return false;
	}

	USkeleton* SeqSkeleton = Sequence->GetSkeleton();
	if (!SeqSkeleton || !SeqSkeleton->IsCompatible(SequenceMetaData->Schema->Skeleton))
	{
		return false;
	}

	FSequenceIndexer Indexer;
	FSequenceIndexer::Result Result = Indexer.Process(SequenceMetaData->Schema, Sequence, SequenceMetaData->SamplingRange);

	SequenceMetaData->SearchIndex.Values = Result.Values;
	SequenceMetaData->SearchIndex.NumPoses = Result.NumIndexedPoses;
	SequenceMetaData->SearchIndex.Schema = SequenceMetaData->Schema;
	return true;
}

bool BuildIndex(UPoseSearchDatabase* Database)
{
	check(Database);

	if (!Database->IsValidForIndexing())
	{
		return false;
	}

	for (const FPoseSearchDatabaseSequence& DbSequence : Database->Sequences)
	{
		USkeleton* SeqSkeleton = DbSequence.Sequence->GetSkeleton();
		if (!SeqSkeleton || !SeqSkeleton->IsCompatible(Database->Schema->Skeleton))
		{
			return false;
		}
	}

	// Prepare animation indexing tasks
	TArray<FSequenceIndexer> Indexers;
	Indexers.SetNum(Database->Sequences.Num());

	auto IndexerTask = [&Database, &Indexers](int32 SequenceIdx)
	{
		const FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[SequenceIdx];
		FSequenceIndexer& Indexer = Indexers[SequenceIdx];
		Indexer.Process(Database->Schema, DbSequence.Sequence, DbSequence.SamplingRange);
	};

	// Index animations independently
	ParallelFor(Database->Sequences.Num(), IndexerTask);

	// Write index info to sequence and count up total poses and storage required
	int32 TotalPoses = 0;
	int32 TotalFloats = 0;
	for (int32 SequenceIdx = 0; SequenceIdx != Database->Sequences.Num(); ++SequenceIdx)
	{
		FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[SequenceIdx];
		FSequenceIndexer::Result Result = Indexers[SequenceIdx].GetResult();
		DbSequence.NumPoses = Result.NumIndexedPoses;
		DbSequence.FirstPoseIdx = TotalPoses;
		TotalPoses += Result.NumIndexedPoses;
		TotalFloats += Result.Values.Num();
	}

	// Join animation data into a single search index
	Database->SearchIndex.Values.Reset(TotalFloats);
	for (const FSequenceIndexer& Indexer : Indexers)
	{
		FSequenceIndexer::Result Result = Indexer.GetResult();
		Database->SearchIndex.Values.Append(Result.Values.GetData(), Result.Values.Num());
	}

	Database->SearchIndex.NumPoses = TotalPoses;
	Database->SearchIndex.Schema = Database->Schema;
	return true;
}

bool BuildQuery(const UPoseSearchSchema* Schema, FPoseHistory* History, TArrayView<float> Query)
{
	check(Schema);
	check(History);

	FFeatureVectorBuilder Builder;
	Builder.Init(&Schema->Layout, Query);

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time; // @@@ making assumption here, need to refer to schema

	for (int32 SubsampleIdx = 0; SubsampleIdx != Schema->PoseSampleOffsets.Num(); ++SubsampleIdx)
	{
		Feature.SubsampleIdx = SubsampleIdx;

		int32 Offset = Schema->PoseSampleOffsets[SubsampleIdx];
		float TimeDelta = -Offset * (1.0f /  Schema->SampleRate);

		if (!History->Sample(TimeDelta, Schema->Skeleton->GetReferenceSkeleton(), Schema->BoneIndicesWithParents))
		{
			break;
		}

		TArrayView<const FTransform> ComponentPose = History->GetComponentPoseSample();
		TArrayView<const FTransform> ComponentPrevPose = History->GetPrevComponentPoseSample();
		for (int32 SchemaBoneIdx = 0; SchemaBoneIdx != Schema->BoneIndices.Num(); ++SchemaBoneIdx)
		{
			Feature.SchemaBoneIdx = SchemaBoneIdx;

			int32 SkeletonBoneIndex = Schema->BoneIndices[SchemaBoneIdx]; // @@@ need to resolve bone indices at runtime, hold in FPoseHistory?
			const FTransform& Transform = ComponentPose[SkeletonBoneIndex];
			const FTransform& PrevTransform = ComponentPrevPose[SkeletonBoneIndex];
			Builder.SetTransform(Feature, Transform);
			Builder.SetTransformDerivative(Feature, Transform, PrevTransform, History->GetSampleInterval());
		}
	}

	return Builder.IsComplete();
}

static FSearchResult Search(const FPoseSearchIndex& SearchIndex, TArrayView<const float> Query)
{
	FSearchResult Result;

	if (!ensure(SearchIndex.IsValid()))
	{
		return Result;
	}

	if(!ensure(Query.Num() == SearchIndex.Schema->Layout.NumFloats))
	{
		return Result;
	}

	float BestPoseDissimilarity = MAX_flt;
	int32 BestPoseIdx = INDEX_NONE;

	for (int32 PoseIdx = 0; PoseIdx != SearchIndex.NumPoses; ++PoseIdx)
	{
		const int32 FeatureValueOffset = PoseIdx * SearchIndex.Schema->Layout.NumFloats;

		float PoseDissimilarity = 0.0f;
		for (int32 ValueIdx = 0; ValueIdx != SearchIndex.Schema->Layout.NumFloats; ++ValueIdx)
		{
			PoseDissimilarity += FMath::Square(Query[ValueIdx] - SearchIndex.Values[ValueIdx + FeatureValueOffset]);
		}

		if (PoseDissimilarity < BestPoseDissimilarity)
		{
			BestPoseDissimilarity = PoseDissimilarity;
			BestPoseIdx = PoseIdx;
		}
	}

	ensure(BestPoseIdx != INDEX_NONE);

	Result.Dissimilarity = BestPoseDissimilarity;
	Result.PoseIdx = BestPoseIdx;
	// Result.TimeOffsetSeconds is set by caller

	return Result;
}

FSearchResult Search(const UPoseSearchSequenceMetaData* Sequence, TArrayView<const float> Query, FDebugDrawParams DebugDrawParams)
{
	FSearchResult Result;

	if (!ensure(Sequence && Sequence->IsValidForSearch()))
	{
		return Result;
	}

	const FPoseSearchIndex& SearchIndex = Sequence->SearchIndex;

	Result = Search(SearchIndex, Query);
	if (!Result.IsValid())
	{
		return Result;
	}

	const float SampleDelta = 1.0f / SearchIndex.Schema->SampleRate;

	Result.TimeOffsetSeconds = SampleDelta * Result.PoseIdx + Sequence->SamplingRange.Min;

	// Do debug visualization
	DebugDrawParams.SearchIndex = &SearchIndex;
	DebugDrawParams.Query = Query;
	DebugDrawParams.HighlightPoseIdx = Result.PoseIdx;
	Draw(DebugDrawParams);

	return Result;
}

FDbSearchResult Search(const UPoseSearchDatabase* Database, TArrayView<const float> Query, FDebugDrawParams DebugDrawParams)
{
	if (!ensure(Database && Database->IsValidForSearch()))
	{
		return FDbSearchResult();
	}

	const FPoseSearchIndex& SearchIndex = Database->SearchIndex;

	FDbSearchResult Result = Search(SearchIndex, Query);
	if (!Result.IsValid())
	{
		return FDbSearchResult();
	}

	const FPoseSearchDatabaseSequence* DbSequence = Database->FindSequenceByPoseIdx(Result.PoseIdx);
	if (!ensure(DbSequence))
	{
		return FDbSearchResult();
	}

	Result.DbSequence = DbSequence;

	const float SampleDelta = 1.0f / SearchIndex.Schema->SampleRate;
	Result.TimeOffsetSeconds = SampleDelta * (DbSequence->FirstPoseIdx - Result.PoseIdx) + DbSequence->SamplingRange.Min;

	// Do debug visualization
	DebugDrawParams.SearchIndex = &SearchIndex;
	DebugDrawParams.Query = Query;
	DebugDrawParams.HighlightPoseIdx = Result.PoseIdx;
	Draw(DebugDrawParams);

	return Result;
}

FSearchResult Search(const FAnimationBaseContext& GraphContext, const UAnimSequenceBase* Sequence)
{
	FSearchResult Result;

	const UPoseSearchSequenceMetaData* MetaData = Sequence->FindMetaDataByClass<UPoseSearchSequenceMetaData>();
	if (!MetaData || !MetaData->IsValidForSearch())
	{
		return Result;
	}

	FAnimNode_PoseSearchHistoryCollector* HistoryNode = GraphContext.GetAncestor<FAnimNode_PoseSearchHistoryCollector>();
	if (!HistoryNode)
	{
		return Result;
	}

	TArrayView<const float> Query = HistoryNode->BuildQuery(MetaData->Schema);

	Result = Search(MetaData, Query);
	return Result;
}


//////////////////////////////////////////////////////////////////////////
// FModule

class FModule : public IModuleInterface, public UE::Anim::IPoseSearchProvider
{
public: // IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

public: // IPoseSearchProvider
	virtual UE::Anim::IPoseSearchProvider::FSearchResult Search(const FAnimationBaseContext& GraphContext, const UAnimSequenceBase* Sequence) override;
};

void FModule::StartupModule()
{
	IModularFeatures::Get().RegisterModularFeature(UE::Anim::IPoseSearchProvider::ModularFeatureName, this);
}

void FModule::ShutdownModule()
{
	IModularFeatures::Get().UnregisterModularFeature(UE::Anim::IPoseSearchProvider::ModularFeatureName, this);
}

UE::Anim::IPoseSearchProvider::FSearchResult FModule::Search(const FAnimationBaseContext& GraphContext, const UAnimSequenceBase* Sequence)
{
	::UE::PoseSearch::FSearchResult Result = ::UE::PoseSearch::Search(GraphContext, Sequence);

	UE::Anim::IPoseSearchProvider::FSearchResult ProviderResult;
	ProviderResult.Dissimilarity = Result.Dissimilarity;
	ProviderResult.PoseIdx = Result.PoseIdx;
	ProviderResult.TimeOffsetSeconds = Result.TimeOffsetSeconds;
	return ProviderResult;
}

}} // namespace UE::PoseSearch

IMPLEMENT_MODULE(UE::PoseSearch::FModule, PoseSearch)