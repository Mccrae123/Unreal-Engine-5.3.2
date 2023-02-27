// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Animation/AnimNodeMessages.h"
#include "BonePose.h"
#include "Containers/RingBuffer.h"
#include "UObject/ObjectKey.h"

class USkeleton;
class UWorld;

namespace UE::PoseSearch
{

struct FSearchResult;
typedef uint16 FComponentSpaceTransformIndex;
typedef TPair<FBoneIndexType, FComponentSpaceTransformIndex> FBoneToTransformPair;
typedef TMap<FBoneIndexType, FComponentSpaceTransformIndex> FBoneToTransformMap;

struct POSESEARCH_API IPoseHistory
{
	virtual ~IPoseHistory() {}
	virtual float GetSampleTimeInterval() const = 0;
	virtual bool GetComponentSpaceTransformAtTime(float Time, FBoneIndexType BoneIndexType, FTransform& OutBoneTransform, bool bExtrapolate = true) const = 0;
	virtual void GetRootTransformAtTime(float Time, FTransform& OutRootTransform, bool bExtrapolate = true) const = 0;
};

struct FPoseHistoryEntry
{
	FTransform RootTransform;
	TArray<FTransform> ComponentSpaceTransforms;
	float Time = 0.f;

	void Update(float InTime, FCSPose<FCompactPose>& ComponentSpacePose, const FTransform& ComponentTransform, const FBoneToTransformMap& BoneToTransformMap);
};

typedef TRingBuffer<FPoseHistoryEntry> FPoseHistoryEntries;
typedef TArray<FPoseHistoryEntry> FPoseHistoryFutureEntries;

struct FPoseHistory : public IPoseHistory
{
	void Init(int32 InNumPoses, float InTimeHorizon, const TArray<FBoneIndexType>& RequiredBones);
	void Update(float SecondsElapsed, FCSPose<FCompactPose>& ComponentSpacePose, const FTransform& ComponentTransform);
	float GetTimeHorizon() const { return TimeHorizon; }
	void DebugDraw(const UWorld* World, const USkeleton* Skeleton) const;
	const FBoneToTransformMap& GetBoneToTransformMap() const { return BoneToTransformMap; }
	const FPoseHistoryEntries& GetEntries() const { return Entries; }

	// IPoseHistory interface
	virtual float GetSampleTimeInterval() const override;
	virtual bool GetComponentSpaceTransformAtTime(float Time, FBoneIndexType BoneIndexType, FTransform& OutBoneTransform, bool bExtrapolate = true) const override;
	virtual void GetRootTransformAtTime(float Time, FTransform& OutRootTransform, bool bExtrapolate = true) const override;
	// End of IPoseHistory interface

private:
	FBoneToTransformMap BoneToTransformMap;
	FPoseHistoryEntries Entries;
	float TimeHorizon = 0.f;
};

struct FExtendedPoseHistory : public IPoseHistory
{
	void Init(const FPoseHistory* InPoseHistory);

	// IPoseHistory interface
	virtual float GetSampleTimeInterval() const override;
	virtual bool GetComponentSpaceTransformAtTime(float Time, FBoneIndexType BoneIndexType, FTransform& OutBoneTransform, bool bExtrapolate = true) const override;
	virtual void GetRootTransformAtTime(float Time, FTransform& OutRootTransform, bool bExtrapolate = true) const override;
	// End of IPoseHistory interface

	void ResetFuturePoses();
	void AddFuturePose(float SecondsInTheFuture, FCSPose<FCompactPose>& ComponentSpacePose, const FTransform& ComponentTransform);
	void DebugDraw(const UWorld* World, const USkeleton* Skeleton) const;

private:
	const FPoseHistory* PoseHistory = nullptr;
	FPoseHistoryFutureEntries FutureEntries;
};

class IPoseHistoryProvider : public UE::Anim::IGraphMessage
{
	DECLARE_ANIMGRAPH_MESSAGE(IPoseHistoryProvider);
public:
	virtual const IPoseHistory& GetPoseHistory() const = 0;
};

struct FHistoricalPoseIndex
{
	bool operator==(const FHistoricalPoseIndex& Index) const
	{
		return PoseIndex == Index.PoseIndex && DatabaseKey == Index.DatabaseKey;
	}

	friend FORCEINLINE uint32 GetTypeHash(const FHistoricalPoseIndex& Index)
	{
		return HashCombineFast(::GetTypeHash(Index.PoseIndex), GetTypeHash(Index.DatabaseKey));
	}

	int32 PoseIndex = INDEX_NONE;
	FObjectKey DatabaseKey;
};

struct FPoseIndicesHistory
{
	void Update(const FSearchResult& SearchResult, float DeltaTime, float MaxTime);
	void Reset() { IndexToTime.Reset(); }
	TMap<FHistoricalPoseIndex, float> IndexToTime;
};

} // namespace UE::PoseSearch


