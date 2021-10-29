// Copyright Epic Games, Inc. All Rights Reserved.

#include "IKRigDefinition.h"
#include "Engine/SkeletalMesh.h"

#define LOCTEXT_NAMESPACE	"IKRigDefinition"

FBoneChain* FRetargetDefinition::GetEditableBoneChainByName(FName ChainName)
{
	for (FBoneChain& Chain : BoneChains)
	{
		if (Chain.ChainName == ChainName)
		{
			return &Chain;
		}
	}
	
	return nullptr;
}


#if WITH_EDITOR
void UIKRigDefinition::PostEditUndo()
{
	Super::PostEditUndo();
	IKRigEditUndo.Broadcast();
}
#endif

const FBoneChain* UIKRigDefinition::GetRetargetChainByName(FName ChainName) const
{
	for (const FBoneChain& Chain : RetargetDefinition.BoneChains)
	{
		if (Chain.ChainName == ChainName)
		{
			return &Chain;
		}
	}
	
	return nullptr;
}

void UIKRigDefinition::SetPreviewMesh(USkeletalMesh* PreviewMesh, bool bMarkAsDirty)
{	
	PreviewSkeletalMesh = PreviewMesh;
}

USkeletalMesh* UIKRigDefinition::GetPreviewMesh() const
{
	return PreviewSkeletalMesh.Get();
}

#undef LOCTEXT_NAMESPACE
