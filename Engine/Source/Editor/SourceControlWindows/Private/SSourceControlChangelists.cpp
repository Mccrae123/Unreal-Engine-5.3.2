// Copyright Epic Games, Inc. All Rights Reserved.

#include "SSourceControlChangelists.h"

#include "EditorStyleSet.h"

#include "Algo/Transform.h"

#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SScrollBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

#include "ISourceControlProvider.h"
#include "ISourceControlModule.h"
#include "SourceControlOperations.h"
#include "ToolMenus.h"
#include "Widgets/Images/SLayeredImage.h"
#include "SSourceControlDescription.h"
#include "SourceControlWindows.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "AssetRegistry/AssetRegistryModule.h"


#define LOCTEXT_NAMESPACE "SourceControlChangelist"

//////////////////////////////
static TSharedRef<SWidget> GetSCCFileWidget(FSourceControlStateRef InFileState, bool bIsShelvedFile = false)
{
	const FSlateBrush* IconBrush = FEditorStyle::GetBrush("ContentBrowser.ColumnViewAssetIcon");

	// Make icon overlays (eg, SCC and dirty status) a reasonable size in relation to the icon size (note: it is assumed this icon is square)
	const float ICON_SCALING_FACTOR = 0.7f;
	const float IconOverlaySize = IconBrush->ImageSize.X * ICON_SCALING_FACTOR;

	return SNew(SOverlay)
		// The actual icon
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(IconBrush)
			.ColorAndOpacity_Lambda([bIsShelvedFile]() -> FSlateColor {
					return FSlateColor(bIsShelvedFile ? FColor::Yellow : FColor::White);
				})
		]
		// Source control state
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		[
			SNew(SBox)
			.WidthOverride(IconOverlaySize)
			.HeightOverride(IconOverlaySize)
			[
				SNew(SLayeredImage, InFileState->GetIcon())
			]
		];
}

struct FSCCFileDragDropOp : public FDragDropOperation
{
	DRAG_DROP_OPERATOR_TYPE(FSCCFileDragDropOp, FDragDropOperation);

	using FDragDropOperation::Construct;

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
	{
		return GetSCCFileWidget(Files[0]);
	}

	TArray<FSourceControlStateRef> Files;
};

//////////////////////////////

struct FChangelistTreeItem : public IChangelistTreeItem
{
	FChangelistTreeItem(FSourceControlChangelistStateRef InChangelistState)
		: ChangelistState(InChangelistState)
	{
		Type = IChangelistTreeItem::Changelist;
	}

	FText GetDisplayText() const
	{
		return ChangelistState->GetDisplayText();
	}

	FText GetDescriptionText() const
	{
		return ChangelistState->GetDescriptionText();
	}

	FSourceControlChangelistStateRef ChangelistState;
};

struct FShelvedChangelistTreeItem : public IChangelistTreeItem
{
	FShelvedChangelistTreeItem()
	{
		Type = IChangelistTreeItem::ShelvedChangelist;
	}

	FText GetDisplayText() const
	{
		return LOCTEXT("SourceControl_ShelvedFiles", "Shelved Items");
	}
};

bool GetAssetData(const FString& InPackageName, const FString& InFileName, TArray<FAssetData>& OutAssets)
{
	OutAssets.Reset();

	// Try the registry first
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().GetAssetsByPackageName(*InPackageName, OutAssets);

	if (OutAssets.Num() > 0)
	{
		return true;
	}

	// Filter on improbable file extensions
	EPackageExtension PackageExtension = FPackagePath::ParseExtension(InFileName);

	if (PackageExtension == EPackageExtension::Unspecified ||
		PackageExtension == EPackageExtension::Custom)
	{
		return false;
	}

	// If nothing was done, try to get the data explicitly
	TArray<FAssetData*> LoadedAssets;
	AssetRegistryModule.Get().LoadPackageRegistryData(InFileName, LoadedAssets);

	for (FAssetData* AssetData : LoadedAssets)
	{
		OutAssets.Add(MoveTemp(*AssetData));
		delete AssetData;
	}

	LoadedAssets.Reset();

	return OutAssets.Num() > 0;
}

struct FFileTreeItem : public IChangelistTreeItem
{
	FFileTreeItem(FSourceControlStateRef InFileState)
		: FileState(InFileState)
	{
		Type = IChangelistTreeItem::File;

		// Initialize asset data first
		FString Filename = FileState->GetFilename();
		FString AssetPackageName;

		if (FPackageName::TryConvertFilenameToLongPackageName(Filename, AssetPackageName))
		{
			::GetAssetData(AssetPackageName, Filename, Assets);
		}

		// Initialize display-related members
		FString AssetName = LOCTEXT("SourceControl_DefaultAssetName", "None").ToString();
		FString AssetType = LOCTEXT("SourceControl_DefaultAssetType", "Unknown").ToString();
		FString AssetPath = Filename;
		FColor AssetColor = FColor(		// Copied from ContentBrowserCLR.cpp
			127 + FColor::Red.R / 2,	// Desaturate the colors a bit (GB colors were too.. much)
			127 + FColor::Red.G / 2,
			127 + FColor::Red.B / 2,
			200); // Opacity

		if(Assets.Num() > 0)
		{
			AssetPath = Assets[0].ObjectPath.ToString();

			// Strip asset name from object path
			int32 LastDot = -1;
			if (AssetPath.FindLastChar('.', LastDot))
			{
				AssetPath.LeftInline(LastDot);
			}

			// Find name, asset type & color only if there is exactly one asset
			if (Assets.Num() == 1)
			{
				static FName NAME_ActorLabel(TEXT("ActorLabel"));
				if (Assets[0].FindTag(NAME_ActorLabel))
				{
					Assets[0].GetTagValue(NAME_ActorLabel, AssetName);
				}
				else
				{
					AssetName = Assets[0].AssetName.ToString();
				}

				AssetType = Assets[0].AssetClass.ToString();

				const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				const TSharedPtr<IAssetTypeActions> AssetTypeActions = AssetToolsModule.Get().GetAssetTypeActionsForClass(Assets[0].GetClass()).Pin();
				if (AssetTypeActions.IsValid())
				{
					AssetColor = AssetTypeActions->GetTypeColor();
				}
				else
				{
					AssetColor = FColor::White;
				}
			}
			else
			{
				AssetType = LOCTEXT("SourceCOntrol_ManyAssetType", "Multiple Assets").ToString();
				AssetColor = FColor::White;
			}
		}

		DisplayName = FText::FromString(AssetName);
		DisplayPath = FText::FromString(AssetPath);
		DisplayType = FText::FromString(AssetType);
		DisplayColor = AssetColor;
	}

	FText GetDisplayPath() const
	{
		return DisplayPath;
	}

	FText GetDisplayName() const
	{
		return DisplayName;
	}

	FText GetDisplayType() const
	{
		return DisplayType;
	}

	FSlateColor GetDisplayColor() const
	{
		return FSlateColor(DisplayColor);
	}

	const TArray<FAssetData>& GetAssetData() const
	{
		return Assets;
	}

public:
	FSourceControlStateRef FileState;

private:
	TArray<FAssetData> Assets;

	FText DisplayPath;
	FText DisplayName;
	FText DisplayType;
	FColor DisplayColor;
};

struct FShelvedFileTreeItem : public IChangelistTreeItem
{
	FShelvedFileTreeItem(FSourceControlStateRef InFileState)
		: FileState(InFileState)
	{
		Type = IChangelistTreeItem::ShelvedFile;
	}

	FText GetDisplayName() const
	{
		return FText::FromString(FileState->GetFilename());
	}

	FSourceControlStateRef FileState;
};

SSourceControlChangelistsWidget::SSourceControlChangelistsWidget()
{
}

void SSourceControlChangelistsWidget::Construct(const FArguments& InArgs)
{
	// Register delegates
	ISourceControlModule& SCCModule = ISourceControlModule::Get();
	SCCModule.RegisterProviderChanged(FSourceControlProviderChanged::FDelegate::CreateSP(this, &SSourceControlChangelistsWidget::OnSourceControlProviderChanged));
	SourceControlStateChangedDelegateHandle = SCCModule.GetProvider().RegisterSourceControlStateChanged_Handle(FSourceControlStateChanged::FDelegate::CreateSP(this, &SSourceControlChangelistsWidget::OnSourceControlStateChanged));

	TreeView = CreateTreeviewWidget();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(4)
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					MakeToolBar()
				]
			]
		]
		+ SVerticalBox::Slot()
		[
			SNew(SScrollBorder, TreeView.ToSharedRef())
			.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda([]()->EVisibility { return ISourceControlModule::Get().IsEnabled() ? EVisibility::Visible : EVisibility::Hidden; })))
			[
				TreeView.ToSharedRef()
			]
		]
	];

	bShouldRefresh = true;
}

TSharedRef<SWidget> SSourceControlChangelistsWidget::MakeToolBar()
{
	FSlimHorizontalToolBarBuilder ToolBarBuilder(nullptr, FMultiBoxCustomization::None);

	ToolBarBuilder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateLambda([this]() {
				RequestRefresh();
				})),
		NAME_None,
		LOCTEXT("SourceControl_RefreshButton", "Refresh"),
		LOCTEXT("SourceControl_RefreshButton_Tooltip", "Refreshes changelists from source control provider."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Refresh"));

	return ToolBarBuilder.MakeWidget();
}

void SSourceControlChangelistsWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	if (bShouldRefresh)
	{
		if (ISourceControlModule::Get().IsEnabled())
		{
			RequestRefresh();
			bShouldRefresh = false;
		}
		else
		{
			// No provider available, clear changelist tree
			ClearChangelistsTree();
		}
	}
}

void SSourceControlChangelistsWidget::RequestRefresh()
{
	if (ISourceControlModule::Get().IsEnabled())
	{
		TSharedRef<FUpdatePendingChangelistsStatus, ESPMode::ThreadSafe> UpdatePendingChangelistsOperation = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
		UpdatePendingChangelistsOperation->SetUpdateAllChangelists(true);
		UpdatePendingChangelistsOperation->SetUpdateFilesStates(true);
		UpdatePendingChangelistsOperation->SetUpdateShelvedFilesStates(true);

		ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
		SourceControlProvider.Execute(UpdatePendingChangelistsOperation, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SSourceControlChangelistsWidget::OnChangelistsStatusUpdated));
	}
	else
	{
		// No provider available, clear changelist tree
		ClearChangelistsTree();
	}
}

void SSourceControlChangelistsWidget::ClearChangelistsTree()
{
	if (!ChangelistsNodes.IsEmpty())
	{
		ChangelistsNodes.Empty();
		TreeView->RequestTreeRefresh();
	}
}

void SSourceControlChangelistsWidget::Refresh()
{
	if (ISourceControlModule::Get().IsEnabled())
	{
		TMap<FSourceControlChangelistStateRef, ExpandedState> ExpandedStates;
		SaveExpandedState(ExpandedStates);

		ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
		TArray<FSourceControlChangelistRef> Changelists = SourceControlProvider.GetChangelists(EStateCacheUsage::Use);

		TArray<FSourceControlChangelistStateRef> ChangelistsStates;
		SourceControlProvider.GetState(Changelists, ChangelistsStates, EStateCacheUsage::Use);

		ChangelistsNodes.Reset(ChangelistsStates.Num());

		for (FSourceControlChangelistStateRef ChangelistState : ChangelistsStates)
		{
			FChangelistTreeItemRef ChangelistTreeItem = MakeShareable(new FChangelistTreeItem(ChangelistState));

			for (FSourceControlStateRef FileRef : ChangelistState->GetFilesStates())
			{
				FChangelistTreeItemRef FileTreeItem = MakeShareable(new FFileTreeItem(FileRef));
				ChangelistTreeItem->AddChild(FileTreeItem);
			}

			if (ChangelistState->GetShelvedFilesStates().Num() > 0)
			{
				FChangelistTreeItemRef ShelvedChangelistTreeItem = MakeShareable(new FShelvedChangelistTreeItem());
				ChangelistTreeItem->AddChild(ShelvedChangelistTreeItem);

				for (FSourceControlStateRef ShelvedFileRef : ChangelistState->GetShelvedFilesStates())
				{
					FChangelistTreeItemRef ShelvedFileTreeItem = MakeShareable(new FShelvedFileTreeItem(ShelvedFileRef));
					ShelvedChangelistTreeItem->AddChild(ShelvedFileTreeItem);
				}
			}

			ChangelistsNodes.Add(ChangelistTreeItem);
		}

		RestoreExpandedState(ExpandedStates);

		TreeView->RequestTreeRefresh();
	}
	else
	{
		ClearChangelistsTree();
	}
}

void SSourceControlChangelistsWidget::OnSourceControlProviderChanged(ISourceControlProvider& OldProvider, ISourceControlProvider& NewProvider)
{
	OldProvider.UnregisterSourceControlStateChanged_Handle(SourceControlStateChangedDelegateHandle);
	SourceControlStateChangedDelegateHandle = NewProvider.RegisterSourceControlStateChanged_Handle(FSourceControlStateChanged::FDelegate::CreateSP(this, &SSourceControlChangelistsWidget::OnSourceControlStateChanged));

	bShouldRefresh = true;
}

void SSourceControlChangelistsWidget::OnSourceControlStateChanged()
{
	Refresh();
}

void SSourceControlChangelistsWidget::OnChangelistsStatusUpdated(const FSourceControlOperationRef& InOperation, ECommandResult::Type InType)
{
	Refresh();
}

void SChangelistTree::Private_SetItemSelection(FChangelistTreeItemPtr TheItem, bool bShouldBeSelected, bool bWasUserDirected)
{
	bool bAllowSelectionChange = true;

	if (bShouldBeSelected && !SelectedItems.IsEmpty())
	{
		// Prevent selecting changelists and files at the same time.
		FChangelistTreeItemPtr CurrentlySelectedItem = (*SelectedItems.begin());
		if (TheItem->GetTreeItemType() != CurrentlySelectedItem->GetTreeItemType())
		{
			bAllowSelectionChange = false;
		}
		// Prevent selecting items that don't share the same root
		else if (TheItem->GetParent() != CurrentlySelectedItem->GetParent())
		{
			bAllowSelectionChange = false;
		}
	}

	if (bAllowSelectionChange)
	{
		STreeView::Private_SetItemSelection(TheItem, bShouldBeSelected, bWasUserDirected);
	}
}

FSourceControlChangelistStatePtr SSourceControlChangelistsWidget::GetCurrentChangelistState()
{
	if (!TreeView)
	{
		return nullptr;
	}

	TArray<FChangelistTreeItemPtr> SelectedItems = TreeView->GetSelectedItems();

	if (SelectedItems.Num() != 1 || SelectedItems[0]->GetTreeItemType() != IChangelistTreeItem::Changelist)
	{
		return nullptr;
	}
	else
	{
		return StaticCastSharedPtr<FChangelistTreeItem>(SelectedItems[0])->ChangelistState;
	}
}

FSourceControlChangelistPtr SSourceControlChangelistsWidget::GetCurrentChangelist()
{
	FSourceControlChangelistStatePtr ChangelistState = GetCurrentChangelistState();
	return ChangelistState ? (FSourceControlChangelistPtr)(ChangelistState->GetChangelist()) : nullptr;
}

FSourceControlChangelistStatePtr SSourceControlChangelistsWidget::GetChangelistStateFromSelection()
{
	if (!TreeView)
	{
		return nullptr;
	}

	TArray<FChangelistTreeItemPtr> SelectedItems = TreeView->GetSelectedItems();

	if (SelectedItems.Num() == 0 || SelectedItems[0]->GetTreeItemType() == IChangelistTreeItem::Invalid)
	{
		return nullptr;
	}

	FChangelistTreeItemPtr Item = SelectedItems[0];

	while (Item && Item->GetTreeItemType() != IChangelistTreeItem::Invalid)
	{
		if (Item->GetTreeItemType() == IChangelistTreeItem::Changelist)
			return StaticCastSharedPtr<FChangelistTreeItem>(Item)->ChangelistState;
		else
			Item = Item->GetParent();
	}

	return nullptr;
}

FSourceControlChangelistPtr SSourceControlChangelistsWidget::GetChangelistFromSelection()
{
	FSourceControlChangelistStatePtr ChangelistState = GetChangelistStateFromSelection();
	return ChangelistState ? (FSourceControlChangelistPtr)(ChangelistState->GetChangelist()) : nullptr;
}

TArray<FString> SSourceControlChangelistsWidget::GetSelectedFiles()
{
	TArray<FChangelistTreeItemPtr> SelectedItems = TreeView->GetSelectedItems();

	if (SelectedItems.Num() == 0 || SelectedItems[0]->GetTreeItemType() != IChangelistTreeItem::File)
	{
		return TArray<FString>();
	}
	else
	{
		TArray<FString> Files;
		for (FChangelistTreeItemPtr Item : SelectedItems)
		{
			Files.Add(StaticCastSharedPtr<FFileTreeItem>(Item)->FileState->GetFilename());
		}

		return Files;
	}
}

TArray<FString> SSourceControlChangelistsWidget::GetSelectedShelvedFiles()
{
	TArray<FString> ShelvedFiles;
	TArray<FChangelistTreeItemPtr> SelectedItems = TreeView->GetSelectedItems();
	
	if (SelectedItems.Num() > 0)
	{
		if (SelectedItems[0]->GetTreeItemType() == IChangelistTreeItem::ShelvedChangelist)
		{
			check(SelectedItems.Num() == 1);
			const TArray<FChangelistTreeItemPtr>& ShelvedChildren = SelectedItems[0]->GetChildren();
			for (FChangelistTreeItemPtr Item : ShelvedChildren)
			{
				ShelvedFiles.Add(StaticCastSharedPtr<FShelvedFileTreeItem>(Item)->FileState->GetFilename());
			}
		}
		else if (SelectedItems[0]->GetTreeItemType() == IChangelistTreeItem::ShelvedFile)
		{
			for (FChangelistTreeItemPtr Item : SelectedItems)
			{
				ShelvedFiles.Add(StaticCastSharedPtr<FShelvedFileTreeItem>(Item)->FileState->GetFilename());
			}
		}
	}

	return ShelvedFiles;
}

void SSourceControlChangelistsWidget::OnNewChangelist()
{
	FText ChangelistDescription;
	bool bOk = GetChangelistDescription(
		nullptr,
		LOCTEXT("SourceControl.Changelist.New.Title", "New Changelist..."),
		LOCTEXT("SourceControl.Changelist.New.Label", "Enter a description for the changelist:"),
		ChangelistDescription);

	if (!bOk)
	{
		return;
	}

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	auto NewChangelistOperation = ISourceControlOperation::Create<FNewChangelist>();
	NewChangelistOperation->SetDescription(ChangelistDescription);

	SourceControlProvider.Execute(NewChangelistOperation);
}

void SSourceControlChangelistsWidget::OnDeleteChangelist()
{
	if (GetCurrentChangelist() == nullptr)
	{
		return;
	}

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	SourceControlProvider.Execute(ISourceControlOperation::Create<FDeleteChangelist>(), GetCurrentChangelist());
}

bool SSourceControlChangelistsWidget::CanDeleteChangelist()
{
	FSourceControlChangelistStatePtr Changelist = GetCurrentChangelistState();
	return Changelist != nullptr && Changelist->GetFilesStates().Num() == 0 && Changelist->GetShelvedFilesStates().Num() == 0;
}

void SSourceControlChangelistsWidget::OnEditChangelist()
{
	FSourceControlChangelistStatePtr ChangelistState = GetCurrentChangelistState();

	if(ChangelistState == nullptr)
	{
		return;
	}

	FText NewChangelistDescription = ChangelistState->GetDescriptionText();

	bool bOk = GetChangelistDescription(
		nullptr,
		LOCTEXT("SourceControl.Changelist.New.Title", "Edit Changelist..."),
		LOCTEXT("SourceControl.Changelist.New.Label", "Enter a new description for the changelist:"),
		NewChangelistDescription);

	if (!bOk)
	{
		return;
	}

	auto EditChangelistOperation = ISourceControlOperation::Create<FEditChangelist>();
	EditChangelistOperation->SetDescription(NewChangelistDescription);

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	SourceControlProvider.Execute(EditChangelistOperation, ChangelistState->GetChangelist());
}

void SSourceControlChangelistsWidget::OnRevertUnchanged()
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();

	auto RevertUnchangedOperation = ISourceControlOperation::Create<FRevertUnchanged>();
	SourceControlProvider.Execute(RevertUnchangedOperation, GetChangelistFromSelection(), GetSelectedFiles());
}

void SSourceControlChangelistsWidget::OnRevert()
{
	FText DialogText;
	FText DialogTitle;

	const bool bApplyOnChangelist = (GetCurrentChangelist() != nullptr);

	if (bApplyOnChangelist)
	{
		DialogText = LOCTEXT("SourceControl_ConfirmRevertChangelist", "Are you sure you want to revert this changelist?");
		DialogTitle = LOCTEXT("SourceControl_ConfirmRevertChangelist_Title", "Confirm changelist revert");
	}
	else
	{
		DialogText = LOCTEXT("SourceControl_ConfirmRevertFiles", "Are you sure you want to revert the selected files?");
		DialogTitle = LOCTEXT("SourceControl_ConfirmReverFiles_Title", "Confirm files revert");
	}
	
	EAppReturnType::Type UserConfirmation = FMessageDialog::Open(EAppMsgType::OkCancel, EAppReturnType::Ok, DialogText, &DialogTitle);

	if (UserConfirmation != EAppReturnType::Ok)
	{
		return;
	}

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	auto RevertOperation = ISourceControlOperation::Create<FRevert>();
	SourceControlProvider.Execute(RevertOperation, GetChangelistFromSelection(), GetSelectedFiles());
}

void SSourceControlChangelistsWidget::OnShelve()
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	auto ShelveOperation = ISourceControlOperation::Create<FShelve>();
	SourceControlProvider.Execute(ShelveOperation, GetChangelistFromSelection(), GetSelectedFiles());
}

void SSourceControlChangelistsWidget::OnUnshelve()
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	auto UnshelveOperation = ISourceControlOperation::Create<FUnshelve>();
	SourceControlProvider.Execute(UnshelveOperation, GetChangelistFromSelection(), GetSelectedShelvedFiles());
}

void SSourceControlChangelistsWidget::OnDeleteShelvedFiles()
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	auto DeleteShelvedOperation = ISourceControlOperation::Create<FDeleteShelved>();
	SourceControlProvider.Execute(DeleteShelvedOperation, GetChangelistFromSelection(), GetSelectedShelvedFiles());
}

void SSourceControlChangelistsWidget::OnSubmitChangelist()
{
	FSourceControlChangelistPtr Changelist = GetCurrentChangelist();
	
	if (!Changelist)
	{
		return;
	}

	const FText DialogText = LOCTEXT("SourceControl_ConfirmSubmit", "Are you sure you want to submit this changelist?");
	const FText DialogTitle = LOCTEXT("SourceControl_ConfirmSubmit_Title", "Confirm changelist submit");

	EAppReturnType::Type UserConfirmation = FMessageDialog::Open(EAppMsgType::OkCancel, EAppReturnType::Ok, DialogText, & DialogTitle);

	if (UserConfirmation == EAppReturnType::Ok)
	{
		ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
		auto SubmitChangelistOperation = ISourceControlOperation::Create<FCheckIn>();

		SourceControlProvider.Execute(SubmitChangelistOperation, Changelist);
		Refresh();
	}
}

bool SSourceControlChangelistsWidget::CanSubmitChangelist()
{
	FSourceControlChangelistStatePtr Changelist = GetCurrentChangelistState();
	return Changelist != nullptr && Changelist->GetFilesStates().Num() >= 0 && Changelist->GetShelvedFilesStates().Num() == 0;
}

void SSourceControlChangelistsWidget::OnLocateFile()
{ 
	TArray<FAssetData> AssetsToSync;
	TArray<FChangelistTreeItemPtr> SelectedItems = TreeView->GetSelectedItems();

	for (const FChangelistTreeItemPtr& SelectedItem : SelectedItems)
	{
		if (SelectedItem->GetTreeItemType() == IChangelistTreeItem::File)
		{
			AssetsToSync.Append(StaticCastSharedPtr<FFileTreeItem>(SelectedItem)->GetAssetData());
		}
	}

	if (AssetsToSync.Num() > 0)
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync, true);
	}
}

bool SSourceControlChangelistsWidget::CanLocateFile()
{
	return GetSelectedFiles().Num() > 0;
}

void SSourceControlChangelistsWidget::OnShowHistory()
{
	TArray<FString> SelectedFiles = GetSelectedFiles();
	if (SelectedFiles.Num() > 0)
	{
		FSourceControlWindows::DisplayRevisionHistory(SelectedFiles);
	}
}

void SSourceControlChangelistsWidget::OnDiffAgainstDepot()
{
	TArray<FString> SelectedFiles = GetSelectedFiles();
	if (SelectedFiles.Num() > 0)
	{
		FSourceControlWindows::DiffAgainstWorkspace(SelectedFiles[0]);
	} 
}

bool SSourceControlChangelistsWidget::CanDiffAgainstDepot()
{
	return GetSelectedFiles().Num() == 1;
}

void SSourceControlChangelistsWidget::OnDiffAgainstWorkspace()
{

}

bool SSourceControlChangelistsWidget::CanDiffAgainstWorkspace()
{
	return GetSelectedShelvedFiles().Num() == 1;
}

TSharedPtr<SWidget> SSourceControlChangelistsWidget::OnOpenContextMenu()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	static const FName MenuName = "SourceControl.ChangelistContextMenu";
	if (!ToolMenus->IsMenuRegistered(MenuName))
	{
		ToolMenus->RegisterMenu(MenuName);
	}

	// Build up the menu for a selection
	FToolMenuContext Context;
	UToolMenu* Menu = ToolMenus->GenerateMenu(MenuName, Context);
		
	bool bHasSelectedChangelist = (GetCurrentChangelist() != nullptr);
	bool bHasSelectedFiles = (GetSelectedFiles().Num() > 0);
	bool bHasSelectedShelvedFiles = (GetSelectedShelvedFiles().Num() > 0);
	bool bHasEmptySelection = (!bHasSelectedChangelist && !bHasSelectedFiles && !bHasSelectedShelvedFiles);

	FToolMenuSection& Section = Menu->AddSection("Source Control");
	
	// This should appear only on change lists
	if (bHasSelectedChangelist)
	{
		Section.AddMenuEntry("SubmitChangelist", LOCTEXT("SourceControl_SubmitChangelist", "Submit Changelist"), LOCTEXT("SourceControl_SubmitChangeslit_Tooltip", "Submits a changelist"), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnSubmitChangelist),
				FCanExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::CanSubmitChangelist)));
	}

	// This can appear on both files & changelist
	if (bHasSelectedChangelist || bHasSelectedFiles)
	{
		Section.AddMenuEntry("RevertUnchanged", LOCTEXT("SourceControl_RevertUnchanged", "Revert Unchanged"), LOCTEXT("SourceControl_Revert_Unchanged_Tooltip", "Reverts unchanged files & changelists"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnRevertUnchanged)));

		Section.AddMenuEntry("Revert", LOCTEXT("SourceControl_Revert", "Revert Files"), LOCTEXT("SourceControl_Revert_Tooltip", "Reverts all files in the changelist or from the selection"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnRevert)));
	}

	if(bHasSelectedFiles || bHasSelectedShelvedFiles || (bHasSelectedChangelist && (GetCurrentChangelistState()->GetFilesStates().Num() > 0 || GetCurrentChangelistState()->GetShelvedFilesStates().Num() > 0)))
	{
		Section.AddSeparator("Shelve");
	}

	if(bHasSelectedFiles || (bHasSelectedChangelist && GetCurrentChangelistState()->GetFilesStates().Num() > 0))
	{
		Section.AddMenuEntry("Shelve", LOCTEXT("SourceControl_Shelve", "Shelve Files"), LOCTEXT("SourceControl_Shelve_Tooltip", "Shelves the changelist or the selected files"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnShelve)));
	}

	if (bHasSelectedShelvedFiles || (bHasSelectedChangelist && GetCurrentChangelistState()->GetShelvedFilesStates().Num() > 0))
	{
		Section.AddMenuEntry("Unshelve", LOCTEXT("SourceControl_Unshelve", "Unshelve Files"), LOCTEXT("SourceControl_Unshelve_Tooltip", "Unshelve selected files or changelist"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnUnshelve)));

		Section.AddMenuEntry("DeleteShelved", LOCTEXT("SourceControl_DeleteShelved", "Delete Shelved Files"), LOCTEXT("SourceControl_DeleteShelved_Tooltip", "Delete selected shelved files or all from changelist"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnDeleteShelvedFiles)));
	}

	// Shelved files-only operations
	if (bHasSelectedShelvedFiles)
	{
		// Diff against workspace
		Section.AddMenuEntry("DiffAgainstWorkspace", LOCTEXT("SourceControl_DiffAgainstWorkspace", "Diff Against Workspace Files"), LOCTEXT("SourceControl_DiffAgainstWorkspace_Tooltip", "Diff shelved file against the (local) workspace file"), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnDiffAgainstWorkspace),
				FCanExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::CanDiffAgainstWorkspace)));
	}

	if (bHasEmptySelection || bHasSelectedChangelist)
	{
		Section.AddSeparator("Changelists");
	}

	// This should appear only if we have no selection
	if (bHasEmptySelection)
	{
		Section.AddMenuEntry("NewChangelist", LOCTEXT("SourceControl_NewChangelist", "New Changelist"), LOCTEXT("SourceControl_NewChangelist_Tooltip", "Creates an empty changelist"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnNewChangelist)));
	}

	if (bHasSelectedChangelist)
	{
		Section.AddMenuEntry("EditChangelist", LOCTEXT("SourceControl_EditChangelist", "Edit Changelist"), LOCTEXT("SourceControl_Edit_Changelist_Tooltip", "Edit a changelist description"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnEditChangelist)));

		Section.AddMenuEntry("DeleteChangelist", LOCTEXT("SourceControl_DeleteChangelist", "Delete Empty Changelist"), LOCTEXT("SourceControl_Delete_Changelist_Tooltip", "Deletes an empty changelist"), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnDeleteChangelist),
				FCanExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::CanDeleteChangelist)));
	}

	// Files-only operations
	if(bHasSelectedFiles)
	{
		Section.AddSeparator("Files");

		Section.AddMenuEntry("Locate File", LOCTEXT("SourceControl_LocateFile", "Locate File"), LOCTEXT("SourceControl_LocateFile_Tooltip", "Locate File in Project..."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnLocateFile),
				FCanExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::CanLocateFile)));

		Section.AddMenuEntry("Show History", LOCTEXT("SourceControl_ShowHistory", "Show History"), LOCTEXT("SourceControl_ShowHistory_ToolTip", "Show File History From Selection..."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnShowHistory)));

		Section.AddMenuEntry("Diff Against Local Version", LOCTEXT("SourceControl_DiffAgainstDepot", "Diff Against Depot"), LOCTEXT("SourceControl_DiffAgainstLocal_Tooltip", "Diff local file against depot revision."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::OnDiffAgainstDepot),
				FCanExecuteAction::CreateSP(this, &SSourceControlChangelistsWidget::CanDiffAgainstDepot)));
	}

	return ToolMenus->GenerateWidget(Menu);
}

TSharedRef<SChangelistTree> SSourceControlChangelistsWidget::CreateTreeviewWidget()
{
	return SAssignNew(TreeView, SChangelistTree)
		.ItemHeight(24.0f)
		.TreeItemsSource(&ChangelistsNodes)
		.OnGenerateRow(this, &SSourceControlChangelistsWidget::OnGenerateRow)
		.OnGetChildren(this, &SSourceControlChangelistsWidget::OnGetChildren)
		.SelectionMode(ESelectionMode::Multi)
		.OnContextMenuOpening(this, &SSourceControlChangelistsWidget::OnOpenContextMenu)
		.HeaderRow
		(
			SNew(SHeaderRow)
			+ SHeaderRow::Column("Change")
			.DefaultLabel(LOCTEXT("Change", "Change"))
			.FillWidth(0.2f)
			+ SHeaderRow::Column("Description")
			.DefaultLabel(LOCTEXT("Description", "Description"))
			.FillWidth(0.6f)
			+ SHeaderRow::Column("Type")
			.DefaultLabel(LOCTEXT("Type", "Type"))
			.FillWidth(0.2f)
		);
}


class SChangelistTableRow : public SMultiColumnTableRow<FChangelistTreeItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SChangelistTableRow)
		: _TreeItemToVisualize()
	{}
	SLATE_ARGUMENT(FChangelistTreeItemPtr, TreeItemToVisualize)
	SLATE_END_ARGS()

public:
	/**
	* Construct child widgets that comprise this widget.
	*
	* @param InArgs Declaration from which to construct this widget.
	*/
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		TreeItem = static_cast<FChangelistTreeItem*>(InArgs._TreeItemToVisualize.Get());

		auto Args = FSuperRowType::FArguments();
		SMultiColumnTableRow<FChangelistTreeItemPtr>::Construct(Args, InOwner);
	}

	// SMultiColumnTableRow overrides
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (ColumnName == TEXT("Change"))
		{
			const FSlateBrush* IconBrush = FEditorStyle::GetBrush("SourceControl.Changelist");

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SExpanderArrow, SharedThis(this))
					]

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(IconBrush)
					]

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(2.0f, 0.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SChangelistTableRow::GetChangelistText)
					];
		}
		else if (ColumnName == TEXT("Description"))
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SChangelistTableRow::GetChangelistDescriptionText)
				];
		}
		else
		{
			return SNullWidget::NullWidget;
		}
	}

	FText GetChangelistText() const
	{
		return TreeItem->GetDisplayText();
	}

	FText GetChangelistDescriptionText() const
	{
		FString DescriptionString = TreeItem->GetDescriptionText().ToString();
		DescriptionString.ReplaceInline(TEXT("\n"), TEXT(" "));
		DescriptionString.TrimEndInline();
		return FText::FromString(DescriptionString);
	}

protected:
	//~ Begin STableRow Interface.
	virtual FReply OnDrop(const FGeometry& InGeometry, const FDragDropEvent& InDragDropEvent) override
	{
		TSharedPtr<FSCCFileDragDropOp> Operation = InDragDropEvent.GetOperationAs<FSCCFileDragDropOp>();
		if (Operation.IsValid())
		{
			FSourceControlChangelistPtr Changelist = TreeItem->ChangelistState->GetChangelist();
			check(Changelist.IsValid());

			TArray<FString> Files;
			Algo::Transform(Operation->Files, Files, [](const FSourceControlStateRef& State) { return State->GetFilename(); });

			ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
			SourceControlProvider.Execute(ISourceControlOperation::Create<FMoveToChangelist>(), Changelist, Files);
		}

		return FReply::Handled();
	}
	//~ End STableRow Interface.

private:
	/** The info about the widget that we are visualizing. */
	FChangelistTreeItem* TreeItem;
};

class SFileTableRow : public SMultiColumnTableRow<FChangelistTreeItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SFileTableRow)
		: _TreeItemToVisualize()
	{}
	SLATE_ARGUMENT(FChangelistTreeItemPtr, TreeItemToVisualize)
	SLATE_EVENT(FOnDragDetected, OnDragDetected)
	SLATE_END_ARGS()

public:
	/**
	* Construct child widgets that comprise this widget.
	*
	* @param InArgs Declaration from which to construct this widget.
	*/
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		TreeItem = static_cast<FFileTreeItem*>(InArgs._TreeItemToVisualize.Get());

		auto Args = FSuperRowType::FArguments()
			.OnDragDetected(InArgs._OnDragDetected)
			.ShowSelection(true);
		FSuperRowType::Construct(Args, InOwner);
	}

	// SMultiColumnTableRow overrides
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (ColumnName == TEXT("Change")) // eq. to name
		{
			return SNew(SHorizontalBox)

			// Icon
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(40, 0, 4, 0)
				[
					GetSCCFileWidget(TreeItem->FileState)
				]

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SFileTableRow::GetDisplayName)
				];
		}
		else if (ColumnName == TEXT("Description")) // eq. to path
		{
			return SNew(STextBlock)
				.Text(this, &SFileTableRow::GetDisplayPath);
		}
		else if (ColumnName == TEXT("Type"))
		{
			return SNew(STextBlock)
				.Text(this, &SFileTableRow::GetDisplayType)
				.ColorAndOpacity(this, &SFileTableRow::GetDisplayColor);
		}
		else
		{
			return SNullWidget::NullWidget;
		}
	}

	FText GetDisplayName() const
	{
		return TreeItem->GetDisplayName();
	}

	FText GetDisplayPath() const
	{
		return TreeItem->GetDisplayPath();
	}

	FText GetDisplayType() const
	{
		return TreeItem->GetDisplayType();
	}

	FSlateColor GetDisplayColor() const
	{
		return TreeItem->GetDisplayColor();
	}

protected:
	//~ Begin STableRow Interface.
	virtual void OnDragEnter(FGeometry const& InGeometry, FDragDropEvent const& InDragDropEvent) override
	{
		TSharedPtr<FDragDropOperation> DragOperation = InDragDropEvent.GetOperation();
		DragOperation->SetCursorOverride(EMouseCursor::SlashedCircle);
	}

	virtual void OnDragLeave(FDragDropEvent const& InDragDropEvent) override
	{
		TSharedPtr<FDragDropOperation> DragOperation = InDragDropEvent.GetOperation();
		DragOperation->SetCursorOverride(EMouseCursor::None);
	}
	//~ End STableRow Interface.

private:
	/** The info about the widget that we are visualizing. */
	FFileTreeItem* TreeItem;
};

class SShelvedChangelistTableRow : public SMultiColumnTableRow<FChangelistTreeItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SShelvedChangelistTableRow)
		: _TreeItemToVisualize()
	{}
	SLATE_ARGUMENT(FChangelistTreeItemPtr, TreeItemToVisualize)
		SLATE_END_ARGS()

public:
	/**
	* Construct child widgets that comprise this widget.
	*
	* @param InArgs Declaration from which to construct this widget.
	*/
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		TreeItem = static_cast<FShelvedChangelistTreeItem*>(InArgs._TreeItemToVisualize.Get());

		auto Args = FSuperRowType::FArguments();
		SMultiColumnTableRow<FChangelistTreeItemPtr>::Construct(Args, InOwner);
	}

	// SMultiColumnTableRow overrides
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (ColumnName == TEXT("Change"))
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(5, 0, 4, 0)
					[
						SNew(SExpanderArrow, SharedThis(this))
					]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(5, 0, 0, 0)
					[
						SNew(SImage)
						.Image(FEditorStyle::GetBrush("SourceControl.Changelist"))
					]
				+ SHorizontalBox::Slot()
					.Padding(2.0f, 0.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SShelvedChangelistTableRow::GetText)
					];
		}
		else
		{
			return SNullWidget::NullWidget;
		}
	}

protected:
	FText GetText() const
	{
		return TreeItem->GetDisplayText();
	}

private:
	/** The info about the widget that we are visualizing. */
	FShelvedChangelistTreeItem* TreeItem;
};

class SShelvedFileTableRow : public SMultiColumnTableRow<FChangelistTreeItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SShelvedFileTableRow)
		: _TreeItemToVisualize()
	{}
	SLATE_ARGUMENT(FChangelistTreeItemPtr, TreeItemToVisualize)
		SLATE_END_ARGS()

public:
	/**
	* Construct child widgets that comprise this widget.
	*
	* @param InArgs Declaration from which to construct this widget.
	*/
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		TreeItem = static_cast<FShelvedFileTreeItem*>(InArgs._TreeItemToVisualize.Get());

		auto Args = FSuperRowType::FArguments();
		FSuperRowType::Construct(Args, InOwner);
	}

	// SMultiColumnTableRow overrides
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		const bool bIsShelvedFile = true;

		if (ColumnName == TEXT("Change")) // eq. to name
		{
			return SNew(SHorizontalBox)

				// Icon
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(60, 0, 4, 0)
				[
					GetSCCFileWidget(TreeItem->FileState, bIsShelvedFile)
				]

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SourceControl_DefaultNameForShelvedFiles", "Unavailable"))
				];
		}
		else if (ColumnName == TEXT("Description"))
		{
			return SNew(STextBlock)
				.Text(this, &SShelvedFileTableRow::GetDisplayName);
		}
		else
		{
			return SNullWidget::NullWidget;
		}
	}

	FText GetDisplayName() const
	{
		return TreeItem->GetDisplayName();
	}

private:
	/** The info about the widget that we are visualizing. */
	FShelvedFileTreeItem* TreeItem;
};

TSharedRef<ITableRow> SSourceControlChangelistsWidget::OnGenerateRow(FChangelistTreeItemPtr InTreeItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	switch (InTreeItem->GetTreeItemType())
	{
	case IChangelistTreeItem::Changelist:
		return SNew(SChangelistTableRow, OwnerTable)
			.TreeItemToVisualize(InTreeItem);

	case IChangelistTreeItem::File:
		return SNew(SFileTableRow, OwnerTable)
			.TreeItemToVisualize(InTreeItem)
			.OnDragDetected(this, &SSourceControlChangelistsWidget::OnFilesDragged);

	case IChangelistTreeItem::ShelvedChangelist:
		return SNew(SShelvedChangelistTableRow, OwnerTable)
			.TreeItemToVisualize(InTreeItem);

	case IChangelistTreeItem::ShelvedFile:
		return SNew(SShelvedFileTableRow, OwnerTable)
			.TreeItemToVisualize(InTreeItem);

	default:
		check(false);
	};

	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable);
}

FReply SSourceControlChangelistsWidget::OnFilesDragged(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton) && !TreeView->GetSelectedItems().IsEmpty())
	{
		TSharedRef<FSCCFileDragDropOp> Operation = MakeShareable(new FSCCFileDragDropOp());

		Algo::Transform(TreeView->GetSelectedItems(), Operation->Files, [](FChangelistTreeItemPtr InTreeItem) { check(InTreeItem->GetTreeItemType() == IChangelistTreeItem::File); return static_cast<FFileTreeItem*>(InTreeItem.Get())->FileState; });
		Operation->Construct();

		return FReply::Handled().BeginDragDrop(Operation);
	}

	return FReply::Unhandled();
}

void SSourceControlChangelistsWidget::OnGetChildren(FChangelistTreeItemPtr InParent, TArray<FChangelistTreeItemPtr>& OutChildren)
{
	for (auto& Child : InParent->GetChildren())
	{
		// Should never have bogus entries in this list
		check(Child.IsValid());
		OutChildren.Add(Child);
	}
}

void SSourceControlChangelistsWidget::SaveExpandedState(TMap<FSourceControlChangelistStateRef, ExpandedState>& ExpandedStates) const
{
	for (FChangelistTreeItemPtr Root : ChangelistsNodes)
	{
		if (Root->GetTreeItemType() != IChangelistTreeItem::Changelist)
		{
			continue;
		}

		bool bChangelistExpanded = TreeView->IsItemExpanded(Root);

		bool bShelveExpanded = false;
		for (FChangelistTreeItemPtr Child : Root->GetChildren())
		{
			if (Child->GetTreeItemType() == IChangelistTreeItem::ShelvedChangelist)
			{
				bShelveExpanded = TreeView->IsItemExpanded(Child);
				break;
			}
		}

		ExpandedState State;
		State.bChangelistExpanded = bChangelistExpanded;
		State.bShelveExpanded = bShelveExpanded;

		ExpandedStates.Add(StaticCastSharedPtr<FChangelistTreeItem>(Root)->ChangelistState, State);
	}
}

void SSourceControlChangelistsWidget::RestoreExpandedState(const TMap<FSourceControlChangelistStateRef, ExpandedState>& ExpandedStates)
{
	for (FChangelistTreeItemPtr Root : ChangelistsNodes)
	{
		if (Root->GetTreeItemType() != IChangelistTreeItem::Changelist)
		{
			continue;
		}

		FSourceControlChangelistStateRef ChangelistState = StaticCastSharedPtr<FChangelistTreeItem>(Root)->ChangelistState;
		const ExpandedState* State = ExpandedStates.Find(ChangelistState);

		if (!State)
		{
			continue;
		}

		TreeView->SetItemExpansion(Root, State->bChangelistExpanded);

		for (FChangelistTreeItemPtr Child : Root->GetChildren())
		{
			if (Child->GetTreeItemType() == IChangelistTreeItem::ShelvedChangelist)
			{
				TreeView->SetItemExpansion(Child, State->bShelveExpanded);
				break;
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE