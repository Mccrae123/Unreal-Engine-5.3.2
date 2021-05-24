// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimGraphNode_SequenceEvaluator.h"
#include "ToolMenus.h"

#include "Kismet2/CompilerResultsLog.h"
#include "AnimGraphCommands.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimSequence.h"
#include "AnimGraphNode_SequenceEvaluator.h"

#include "BlueprintNodeSpawner.h"
#include "EditorCategoryUtils.h"
#include "IAnimBlueprintNodeOverrideAssetsContext.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeTemplateCache.h"

/////////////////////////////////////////////////////
// UAnimGraphNode_SequenceEvaluator

#define LOCTEXT_NAMESPACE "A3Nodes"

UAnimGraphNode_SequenceEvaluator::UAnimGraphNode_SequenceEvaluator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAnimGraphNode_SequenceEvaluator::PreloadRequiredAssets()
{
	PreloadObject(Node.GetSequence());

	Super::PreloadRequiredAssets();
}

void UAnimGraphNode_SequenceEvaluator::BakeDataDuringCompilation(class FCompilerResultsLog& MessageLog)
{
	UAnimBlueprint* AnimBlueprint = GetAnimBlueprint();
	AnimBlueprint->FindOrAddGroup(Node.GetGroupName());
}

void UAnimGraphNode_SequenceEvaluator::GetAllAnimationSequencesReferred(TArray<UAnimationAsset*>& AnimationAssets) const
{
	if(Node.GetSequence())
	{
		HandleAnimReferenceCollection(Node.Sequence, AnimationAssets);
	}
}

void UAnimGraphNode_SequenceEvaluator::ReplaceReferredAnimations(const TMap<UAnimationAsset*, UAnimationAsset*>& AnimAssetReplacementMap)
{
	HandleAnimReferenceReplacement(Node.Sequence, AnimAssetReplacementMap);
}

FText UAnimGraphNode_SequenceEvaluator::GetMenuCategory() const
{
	return FEditorCategoryUtils::GetCommonCategory(FCommonEditorCategory::Animation);
}

FText UAnimGraphNode_SequenceEvaluator::GetNodeTitleForSequence(ENodeTitleType::Type TitleType, UAnimSequenceBase* InSequence) const
{
	const FText SequenceName = FText::FromString(InSequence->GetName());

	FFormatNamedArguments Args;
	Args.Add(TEXT("SequenceName"), SequenceName);

	// FText::Format() is slow, so we cache this to save on performance
	if (InSequence->IsValidAdditive())
	{
		CachedNodeTitle.SetCachedText(FText::Format(LOCTEXT("EvaluateSequence_Additive", "Evaluate {SequenceName} (additive)"), Args), this);
	}
	else
	{
		CachedNodeTitle.SetCachedText(FText::Format(LOCTEXT("EvaluateSequence", "Evaluate {SequenceName}"), Args), this);
	}

	return CachedNodeTitle;
}

FText UAnimGraphNode_SequenceEvaluator::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	UAnimSequenceBase* Sequence = Node.GetSequence();
	if (Sequence == nullptr)
	{
		// we may have a valid variable connected or default pin value
		UEdGraphPin* SequencePin = FindPin(GET_MEMBER_NAME_STRING_CHECKED(FAnimNode_SequenceEvaluator, Sequence));
		if (SequencePin && SequencePin->LinkedTo.Num() > 0)
		{
			return LOCTEXT("EvaluateSequence_TitleVariable", "Evaluate Animation Sequence");
		}
		else if (SequencePin && SequencePin->DefaultObject != nullptr)
		{
			return GetNodeTitleForSequence(TitleType, CastChecked<UAnimSequenceBase>(SequencePin->DefaultObject));
		}
		else
		{
			return LOCTEXT("EvaluateSequence_TitleNONE", "Evaluate (None)");
		}
	}
	// @TODO: don't know enough about this node type to comfortably assert that
	//        the CacheName won't change after the node has spawned... until
	//        then, we'll leave this optimization off
	else //if (CachedNodeTitle.IsOutOfDate(this))
	{
		GetNodeTitleForSequence(TitleType, Sequence);
	}

	return CachedNodeTitle;
}

FSlateIcon UAnimGraphNode_SequenceEvaluator::GetIconAndTint(FLinearColor& OutColor) const
{
	return FSlateIcon("EditorStyle", "ClassIcon.AnimSequence");
}

void UAnimGraphNode_SequenceEvaluator::GetMenuActions(FBlueprintActionDatabaseRegistrar& InActionRegistrar) const
{
	GetMenuActionsHelper(
		InActionRegistrar,
		GetClass(),
		{ UAnimSequence::StaticClass() },
		{ },
		[](const FAssetData& InAssetData)
		{
			const FString TagValue = InAssetData.GetTagValueRef<FString>(GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType));
			if(const bool bKnownToBeAdditive = (!TagValue.IsEmpty() && !TagValue.Equals(TEXT("AAT_None"))))
			{
				return FText::Format(LOCTEXT("MenuDescFormat", "Evaluate '{0}' (additive)"), FText::FromName(InAssetData.AssetName));
			}
			else
			{
				return FText::Format(LOCTEXT("MenuDescFormat", "Evaluate '{0}'"), FText::FromName(InAssetData.AssetName));
			}
		},
		[](const FAssetData& InAssetData)
		{
			const FString TagValue = InAssetData.GetTagValueRef<FString>(GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType));
			if(const bool bKnownToBeAdditive = (!TagValue.IsEmpty() && !TagValue.Equals(TEXT("AAT_None"))))
			{
				return FText::Format(LOCTEXT("MenuDescTooltipFormat", "Evaluate (additive)\n'{0}'"), FText::FromName(InAssetData.ObjectPath));
			}
			else
			{
				return FText::Format(LOCTEXT("MenuDescTooltipFormat", "Evaluate\n'{0}'"), FText::FromName(InAssetData.ObjectPath));
			}
		},
		[](UEdGraphNode* InNewNode, bool bInIsTemplateNode, const FAssetData InAssetData)
		{
			UAnimGraphNode_AssetPlayerBase::SetupNewNode(InNewNode, bInIsTemplateNode, InAssetData);
		});
}

void UAnimGraphNode_SequenceEvaluator::SetAnimationAsset(UAnimationAsset* Asset)
{
	if (UAnimSequenceBase* Seq =  Cast<UAnimSequence>(Asset))
	{
		Node.SetSequence(Seq);
	}
}

void UAnimGraphNode_SequenceEvaluator::OnOverrideAssets(IAnimBlueprintNodeOverrideAssetsContext& InContext) const
{
	if(InContext.GetAssets().Num() > 0)
	{
		if (UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(InContext.GetAssets()[0]))
		{
			FAnimNode_SequenceEvaluator& AnimNode = InContext.GetAnimNode<FAnimNode_SequenceEvaluator>();
			AnimNode.SetSequence(Sequence);
		}
	}
}

void UAnimGraphNode_SequenceEvaluator::ValidateAnimNodeDuringCompilation(class USkeleton* ForSkeleton, class FCompilerResultsLog& MessageLog)
{
	Super::ValidateAnimNodeDuringCompilation(ForSkeleton, MessageLog);

	UAnimSequenceBase* SequenceToCheck = Node.GetSequence();
	UEdGraphPin* SequencePin = FindPin(GET_MEMBER_NAME_STRING_CHECKED(FAnimNode_SequenceEvaluator, Sequence));
	if (SequencePin != nullptr && SequenceToCheck == nullptr)
	{
		SequenceToCheck = Cast<UAnimSequenceBase>(SequencePin->DefaultObject);
	}

	if (SequenceToCheck == nullptr)
	{
		// Check for bindings
		bool bHasBinding = false;
		if(SequencePin != nullptr)
		{
			if (FAnimGraphNodePropertyBinding* BindingPtr = PropertyBindings.Find(SequencePin->GetFName()))
			{
				bHasBinding = true;
			}
		}

		// we may have a connected node or binding
		if (SequencePin == nullptr || (SequencePin->LinkedTo.Num() == 0 && !bHasBinding))
		{
			MessageLog.Error(TEXT("@@ references an unknown sequence"), this);
		}
	}
	else
	{
		USkeleton* SeqSkeleton = SequenceToCheck->GetSkeleton();
		if (SeqSkeleton && // if anim sequence doesn't have skeleton, it might be due to anim sequence not loaded yet, @todo: wait with anim blueprint compilation until all assets are loaded?
			!ForSkeleton->IsCompatible(SeqSkeleton))
		{
			MessageLog.Error(TEXT("@@ references sequence that uses an incompatible skeleton @@"), this, SeqSkeleton);
		}
	}
}

void UAnimGraphNode_SequenceEvaluator::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	if (!Context->bIsDebugging)
	{
		// add an option to convert to a regular sequence player
		{
			FToolMenuSection& Section = Menu->AddSection("AnimGraphNodeSequenceEvaluator", NSLOCTEXT("A3Nodes", "SequenceEvaluatorHeading", "Sequence Evaluator"));
			Section.AddMenuEntry(FAnimGraphCommands::Get().OpenRelatedAsset);
			Section.AddMenuEntry(FAnimGraphCommands::Get().ConvertToSeqPlayer);
		}
	}
}

bool UAnimGraphNode_SequenceEvaluator::DoesSupportTimeForTransitionGetter() const
{
	return true;
}

UAnimationAsset* UAnimGraphNode_SequenceEvaluator::GetAnimationAsset() const 
{
	UAnimSequenceBase* Sequence = Node.GetSequence();
	UEdGraphPin* SequencePin = FindPin(GET_MEMBER_NAME_STRING_CHECKED(FAnimNode_SequenceEvaluator, Sequence));
	if (SequencePin != nullptr && Sequence == nullptr)
	{
		Sequence = Cast<UAnimSequenceBase>(SequencePin->DefaultObject);
	}

	return Sequence;
}

const TCHAR* UAnimGraphNode_SequenceEvaluator::GetTimePropertyName() const 
{
	return TEXT("ExplicitTime");
}

UScriptStruct* UAnimGraphNode_SequenceEvaluator::GetTimePropertyStruct() const 
{
	return FAnimNode_SequenceEvaluator::StaticStruct();
}

EAnimAssetHandlerType UAnimGraphNode_SequenceEvaluator::SupportsAssetClass(const UClass* AssetClass) const
{
	if (AssetClass->IsChildOf(UAnimSequence::StaticClass()) || AssetClass->IsChildOf(UAnimComposite::StaticClass()))
	{
		return EAnimAssetHandlerType::Supported;
	}
	else
	{
		return EAnimAssetHandlerType::NotSupported;
	}
}

#undef LOCTEXT_NAMESPACE
