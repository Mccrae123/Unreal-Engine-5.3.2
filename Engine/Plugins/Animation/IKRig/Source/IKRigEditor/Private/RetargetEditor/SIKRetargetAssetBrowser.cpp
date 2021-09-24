﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "RetargetEditor/SIKRetargetAssetBrowser.h"

#include "SEditorHeaderButton.h"
#include "AnimPreviewInstance.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Animation/AnimMontage.h"
#include "Animation/PoseAsset.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargetEditorController.h"
#include "Retargeter/IKRetargeter.h"

#define LOCTEXT_NAMESPACE "IKRetargeterAssetBrowser"

void SIKRetargetAssetBrowser::Construct(
	const FArguments& InArgs,
	TSharedRef<FIKRetargetEditorController> InEditorController)
{
	EditorController = InEditorController;
	
	ChildSlot
    [
        SNew(SVerticalBox)
        
        + SVerticalBox::Slot()
		.AutoHeight()
		.Padding(5)
		[
			SNew(SEditorHeaderButton)
			.IsEnabled(this, &SIKRetargetAssetBrowser::IsExportButtonEnabled)
			.Icon(FAppStyle::Get().GetBrush("Icons.Save"))
			.Text(LOCTEXT("ExportButtonLabel", "Export Selected Animations"))
			.ToolTipText(LOCTEXT("ExportButtonToolTip", "Generate new retargeted sequence assets on target skeletal mesh (uses current retargeting configuration)."))
			.OnClicked(this, &SIKRetargetAssetBrowser::OnExportButtonClicked)
		]

		+SVerticalBox::Slot()
		[
			SAssignNew(AssetBrowserBox, SBox)
		]
    ];

	AddAssetBrowser();
}

void SIKRetargetAssetBrowser::AddAssetBrowser()
{
	FAssetPickerConfig AssetPickerConfig;
	AssetPickerConfig.Filter.ClassNames.Add(UAnimSequence::StaticClass()->GetFName());
	AssetPickerConfig.Filter.ClassNames.Add(UAnimMontage::StaticClass()->GetFName());
	AssetPickerConfig.Filter.ClassNames.Add(UPoseAsset::StaticClass()->GetFName());
	AssetPickerConfig.OnAssetDoubleClicked = FOnAssetSelected::CreateSP(this, &SIKRetargetAssetBrowser::OnAssetDoubleClicked);
	AssetPickerConfig.GetCurrentSelectionDelegates.Add(&GetCurrentSelectionDelegate);
	AssetPickerConfig.bAllowNullSelection = false;
	AssetPickerConfig.InitialAssetViewType = EAssetViewType::Column;
	AssetPickerConfig.OnShouldFilterAsset = FOnShouldFilterAsset::CreateSP(this, &SIKRetargetAssetBrowser::OnShouldFilterAsset);
	AssetPickerConfig.bShowPathInColumnView = true;
	AssetPickerConfig.bShowTypeInColumnView = false;

	// hide all asset registry columns by default (we only really want the name and path)
	TArray<UObject::FAssetRegistryTag> AssetRegistryTags;
	UAnimSequence::StaticClass()->GetDefaultObject()->GetAssetRegistryTags(AssetRegistryTags);
	for(UObject::FAssetRegistryTag& AssetRegistryTag : AssetRegistryTags)
	{
		AssetPickerConfig.HiddenColumnNames.Add(AssetRegistryTag.Name.ToString());
	}

	// Also hide the type column by default (but allow users to enable it, so don't use bShowTypeInColumnView)
	AssetPickerConfig.HiddenColumnNames.Add(TEXT("Class"));

	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	AssetBrowserBox->SetContent(ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig));
}

FReply SIKRetargetAssetBrowser::OnExportButtonClicked() const
{
	FIKRetargetEditorController* Controller = EditorController.Pin().Get();
	if (!Controller)
	{
		return FReply::Handled();
	}
	
	// assemble the data for the assets we want to batch duplicate/retarget
	FIKRetargetBatchOperationContext BatchContext;

	// add selected assets to dup/retarget
	TArray<FAssetData> SelectedAssets = GetCurrentSelectionDelegate.Execute();
	for (const FAssetData& Asset : SelectedAssets)
	{
		UE_LOG(LogTemp, Display, TEXT("Duplicating and Retargeting: %s"), *Asset.GetFullName());

		BatchContext.AssetsToRetarget.Add(Asset.GetAsset());
	}

	BatchContext.SourceMesh = Controller->GetSourceSkeletalMesh();
	BatchContext.TargetMesh = Controller->GetTargetSkeletalMesh();
	BatchContext.IKRetargetAsset = Controller->Asset;
	BatchContext.bRemapReferencedAssets = false;
	BatchContext.NameRule.Suffix = "_Retargeted";

	// actually run the retarget
	FIKRetargetBatchOperation BatchOperation;
	BatchOperation.RunRetarget(BatchContext);
	
	return FReply::Handled();
}

bool SIKRetargetAssetBrowser::IsExportButtonEnabled() const
{
	if (!EditorController.Pin().IsValid())
	{
		return false;
	}

	UIKRetargeter* CurrentRetargeter = EditorController.Pin()->GetCurrentlyRunningRetargeter();
	if (!IsValid(CurrentRetargeter))
	{
		return false;
	}

	return CurrentRetargeter->bIsLoadedAndValid;
}

void SIKRetargetAssetBrowser::OnAssetDoubleClicked(const FAssetData& AssetData)
{
	if (!AssetData.GetAsset())
	{
		return;
	}
	
	UAnimationAsset* NewAnimationAsset = Cast<UAnimationAsset>(AssetData.GetAsset());
	if (NewAnimationAsset && EditorController.Pin().IsValid())
	{
		EditorController.Pin()->PlayAnimationAsset(NewAnimationAsset);
	}
}

bool SIKRetargetAssetBrowser::OnShouldFilterAsset(const struct FAssetData& AssetData)
{
	// is this an animation asset?
	if (!AssetData.GetClass()->IsChildOf(UAnimationAsset::StaticClass()))
	{
		return true;
	}
	
	// controller setup
	FIKRetargetEditorController* Controller = EditorController.Pin().Get();
	if (!Controller)
	{
		return true;
	}

	// get source mesh
	USkeletalMesh* SourceMesh = Controller->GetSourceSkeletalMesh();
	if (!SourceMesh)
	{
		return true;
	}

	// get source skeleton
	USkeleton* DesiredSkeleton = SourceMesh->GetSkeleton();
	if (!DesiredSkeleton)
	{
		return true;
	}

	return !DesiredSkeleton->IsCompatibleSkeletonByAssetData(AssetData);
}

#undef LOCTEXT_NAMESPACE
