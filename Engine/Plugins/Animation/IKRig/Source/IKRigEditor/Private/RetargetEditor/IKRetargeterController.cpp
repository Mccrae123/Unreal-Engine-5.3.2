﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "RetargetEditor/IKRetargeterController.h"

#include "ScopedTransaction.h"
#include "Algo/LevenshteinDistance.h"
#include "Retargeter/IKRetargeter.h"

#define LOCTEXT_NAMESPACE "IKRetargeterController"


UIKRetargeterController* UIKRetargeterController::GetController(UIKRetargeter* InRetargeterAsset)
{
	if (!InRetargeterAsset)
	{
		return nullptr;
	}

	if (!InRetargeterAsset->Controller)
	{
		UIKRetargeterController* Controller = NewObject<UIKRetargeterController>();
		Controller->Asset = InRetargeterAsset;
		InRetargeterAsset->Controller = Controller;
	}

	UIKRetargeterController* Controller = Cast<UIKRetargeterController>(InRetargeterAsset->Controller);
	// clean the asset before editing
	Controller->CleanChainMapping();
	Controller->CleanPoseList();
	
	return Controller;
}

UIKRetargeter* UIKRetargeterController::GetAsset() const
{
	return Asset;
}

FName UIKRetargeterController::GetSourceRootBone() const
{
	return Asset->SourceIKRigAsset ? Asset->SourceIKRigAsset->GetRetargetRoot() : FName("None");
}

FName UIKRetargeterController::GetTargetRootBone() const
{
	return Asset->TargetIKRigAsset ? Asset->TargetIKRigAsset->GetRetargetRoot() : FName("None");
}

void UIKRetargeterController::GetTargetChainNames(TArray<FName>& OutNames) const
{
	if (Asset->TargetIKRigAsset)
	{
		const TArray<FBoneChain>& Chains = Asset->TargetIKRigAsset->GetRetargetChains();
		for (const FBoneChain& Chain : Chains)
		{
			OutNames.Add(Chain.ChainName);
		}
	}
}

void UIKRetargeterController::GetSourceChainNames(TArray<FName>& OutNames) const
{
	if (Asset->SourceIKRigAsset)
	{
		const TArray<FBoneChain>& Chains = Asset->SourceIKRigAsset->GetRetargetChains();
		for (const FBoneChain& Chain : Chains)
		{
			OutNames.Add(Chain.ChainName);
		}
	}
}

void UIKRetargeterController::CleanChainMapping()
{
	if (!Asset->TargetIKRigAsset)
	{
		// don't clean chain mappings, in case user is replacing with IK Rig asset that has some valid mappings
		return;
	}
	
	TArray<FName> TargetChainNames;
	GetTargetChainNames(TargetChainNames);

	// remove all target chains that are no longer in the target IK rig asset
	TArray<FName> TargetChainsToRemove;
	for (FRetargetChainMap& ChainMap : Asset->ChainMapping)
	{
		if (!TargetChainNames.Contains(ChainMap.TargetChain))
		{
			TargetChainsToRemove.Add(ChainMap.TargetChain);
		}
	}
	for (FName TargetChainToRemove : TargetChainsToRemove)
	{
		Asset->ChainMapping.RemoveAll([&TargetChainToRemove](FRetargetChainMap& Element)
		{
			return Element.TargetChain == TargetChainToRemove;
		});
	}

	// add a mapping for each chain that is in the target IK rig (if it doesn't have one already)
	for (FName TargetChainName : TargetChainNames)
	{
		const bool HasChain = Asset->ChainMapping.ContainsByPredicate([&TargetChainName](FRetargetChainMap& Element)
		{
			return Element.TargetChain == TargetChainName;
		});
		
		if (!HasChain)
		{
			Asset->ChainMapping.Add(FRetargetChainMap(TargetChainName));
		}
	}

	TArray<FName> SourceChainNames;
	GetSourceChainNames(SourceChainNames);
	
	// reset any sources that are no longer present to "None"
	for (FRetargetChainMap& ChainMap : Asset->ChainMapping)
	{
		if (!SourceChainNames.Contains(ChainMap.SourceChain))
		{
			ChainMap.SourceChain = NAME_None;
		}
	}

	// enforce the same chain order as the target IK rig
	Asset->ChainMapping.Sort([this](const FRetargetChainMap& A, const FRetargetChainMap& B)
	{
		const TArray<FBoneChain>& BoneChains = Asset->TargetIKRigAsset->GetRetargetChains();
		
		const int32 IndexA = BoneChains.IndexOfByPredicate([&A](const FBoneChain& Chain)
		{
			return A.TargetChain == Chain.ChainName;
		});

		const int32 IndexB = BoneChains.IndexOfByPredicate([&B](const FBoneChain& Chain)
		{
			return B.TargetChain == Chain.ChainName;
		});
 
		return IndexA < IndexB;
	});

	BroadcastNeedsReinitialized();
}

void UIKRetargeterController::CleanPoseList()
{
	// enforce the existence of a default pose
	const bool HasDefaultPose = Asset->RetargetPoses.Contains(Asset->DefaultPoseName);
	if (!HasDefaultPose)
	{
		Asset->RetargetPoses.Emplace(Asset->DefaultPoseName);
	}
	
	// use default pose unless set to something else
	if (Asset->CurrentRetargetPose == NAME_None)
	{
		Asset->CurrentRetargetPose = Asset->DefaultPoseName;
	}

	// remove all bone offsets that are no longer part of the target skeleton
	if (Asset->TargetIKRigAsset)
	{
		const TArray<FName> AllowedBoneNames = Asset->TargetIKRigAsset->Skeleton.BoneNames;
		for (TTuple<FName, FIKRetargetPose>& Pose : Asset->RetargetPoses)
		{
			// find bone offsets no longer in target skeleton
			TArray<FName> BonesToRemove;
			for (TTuple<FName, FQuat>& BoneOffset : Pose.Value.BoneRotationOffsets)
			{
				if (!AllowedBoneNames.Contains(BoneOffset.Key))
				{
					BonesToRemove.Add(BoneOffset.Key);
				}
			}
			
			// remove bone offsets
			for (const FName& BoneToRemove : BonesToRemove)
			{
				Pose.Value.BoneRotationOffsets.Remove(BoneToRemove);
			}
		}
	}

	BroadcastNeedsReinitialized();
}

void UIKRetargeterController::AutoMapChains()
{
	TArray<FName> SourceChainNames;
	GetSourceChainNames(SourceChainNames);
	
	// auto-map any chains that have no value using a fuzzy string search
	for (FRetargetChainMap& ChainMap : Asset->ChainMapping)
	{
		if (ChainMap.SourceChain != NAME_None)
		{
			continue; // already set by user
		}

		// find "best match" automatically as a convenience for the user
		FString TargetNameLowerCase = ChainMap.TargetChain.ToString().ToLower();
		float HighestScore = 0.2f;
		int32 HighestScoreIndex = -1;
		for (int32 ChainIndex=0; ChainIndex<SourceChainNames.Num(); ++ChainIndex)
		{
			FString SourceNameLowerCase = SourceChainNames[ChainIndex].ToString().ToLower();
			float WorstCase = TargetNameLowerCase.Len() + SourceNameLowerCase.Len();
			WorstCase = WorstCase < 1.0f ? 1.0f : WorstCase;
			const float Score = 1.0f - (Algo::LevenshteinDistance(TargetNameLowerCase, SourceNameLowerCase) / WorstCase);
			if (Score > HighestScore)
			{
				HighestScore = Score;
				HighestScoreIndex = ChainIndex;
			}
		}

		// apply source if any decent matches were found
		if (SourceChainNames.IsValidIndex(HighestScoreIndex))
		{
			ChainMap.SourceChain = SourceChainNames[HighestScoreIndex];
		}
	}

	// force update with latest mapping
	BroadcastNeedsReinitialized();
}

void UIKRetargeterController::OnRetargetChainRenamed(UIKRigDefinition* IKRig, FName OldChainName, FName NewChainName) const
{
	const bool bIsSourceRig = IKRig == Asset->SourceIKRigAsset;
	check(bIsSourceRig || IKRig == Asset->TargetIKRigAsset)
	for (FRetargetChainMap& ChainMap : Asset->ChainMapping)
	{
		FName& ChainNameToUpdate = bIsSourceRig ? ChainMap.SourceChain : ChainMap.TargetChain;
		if (ChainNameToUpdate == OldChainName)
		{
			ChainNameToUpdate = NewChainName;
			BroadcastNeedsReinitialized();
			return;
		}
	}
}

void UIKRetargeterController::SetSourceChainForTargetChain(FName TargetChain, FName SourceChainToMapTo)
{
	FRetargetChainMap* ChainMap = GetChainMap(TargetChain);
	check(ChainMap)
	ChainMap->SourceChain = SourceChainToMapTo;
	BroadcastNeedsReinitialized();
}

FName UIKRetargeterController::GetSourceChainForTargetChain(FName TargetChain)
{
	FRetargetChainMap* ChainMap = GetChainMap(TargetChain);
	check(ChainMap)
	return ChainMap->SourceChain;
}

const TArray<FRetargetChainMap>& UIKRetargeterController::GetChainMappings()
{
	return Asset->ChainMapping;
}

USkeleton* UIKRetargeterController::GetSourceSkeletonAsset() const
{
	if (!Asset->SourceIKRigAsset)
	{
		return nullptr;
	}

	if (!Asset->SourceIKRigAsset->PreviewSkeletalMesh)
	{
		return nullptr;
	}

	return Asset->SourceIKRigAsset->PreviewSkeletalMesh->GetSkeleton();
}

void UIKRetargeterController::AddRetargetPose(FName NewPoseName) const
{
	if (Asset->RetargetPoses.Contains(NewPoseName))
	{
		return;
	}

	Asset->RetargetPoses.Add(NewPoseName);
	Asset->CurrentRetargetPose = NewPoseName;

	BroadcastNeedsReinitialized();
}

void UIKRetargeterController::RemoveRetargetPose(FName PoseToRemove) const
{
	if (PoseToRemove == Asset->DefaultPoseName)
	{
		return; // cannot remove default pose
	}

	if (!Asset->RetargetPoses.Contains(PoseToRemove))
	{
		return; // cannot remove pose that doesn't exist
	}

	Asset->RetargetPoses.Remove(PoseToRemove);

	// did we remove the currently used pose?
	if (Asset->CurrentRetargetPose == PoseToRemove)
	{
		Asset->CurrentRetargetPose = UIKRetargeter::DefaultPoseName;
	}

	BroadcastNeedsReinitialized();
}

void UIKRetargeterController::ResetRetargetPose(FName PoseToReset) const
{
	if (!Asset->RetargetPoses.Contains(PoseToReset))
	{
		return; // cannot reset pose that doesn't exist
	}

	Asset->RetargetPoses[PoseToReset].BoneRotationOffsets.Reset();
	Asset->RetargetPoses[PoseToReset].RootTranslationOffset = FVector::ZeroVector;
	
	BroadcastNeedsReinitialized();
}

FName UIKRetargeterController::GetCurrentRetargetPoseName() const
{
	return GetAsset()->CurrentRetargetPose;
}

void UIKRetargeterController::SetCurrentRetargetPose(FName CurrentPose) const
{
	check(Asset->RetargetPoses.Contains(CurrentPose));
	Asset->CurrentRetargetPose = CurrentPose;
	BroadcastNeedsReinitialized();
}

const TMap<FName, FIKRetargetPose>& UIKRetargeterController::GetRetargetPoses()
{
	return GetAsset()->RetargetPoses;
}

void UIKRetargeterController::AddRotationOffsetToRetargetPoseBone(FName BoneName, FQuat RotationOffset) const
{
	Asset->RetargetPoses[Asset->CurrentRetargetPose].AddRotationDeltaToBone(BoneName, RotationOffset);
}

void UIKRetargeterController::AddTranslationOffsetToRetargetRootBone(FVector TranslationOffset) const
{
	Asset->RetargetPoses[Asset->CurrentRetargetPose].AddTranslationDeltaToRoot(TranslationOffset);
}

void UIKRetargeterController::SetEditRetargetPoseMode(bool bEditPoseMode) const
{
	GetAsset()->bEditRetargetPoseMode = bEditPoseMode;
	if (!bEditPoseMode)
	{
		// must reinitialize after editing the retarget pose
		BroadcastNeedsReinitialized();
	}
}

bool UIKRetargeterController::GetEditRetargetPoseMode() const
{
	return GetAsset()->bEditRetargetPoseMode;
}

FRetargetChainMap* UIKRetargeterController::GetChainMap(const FName& TargetChainName) const
{
	for (FRetargetChainMap& ChainMap : Asset->ChainMapping)
	{
		if (ChainMap.TargetChain == TargetChainName)
		{
			return &ChainMap;
		}
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE
