// Copyright Epic Games, Inc. All Rights Reserved.

#include "SPoseSearchDatabaseAssetList.h"
#include "PoseSearchDatabaseViewModel.h"

#include "PoseSearch/PoseSearch.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"

#include "DragAndDrop/AssetDragDropOp.h"
#include "Misc/FeedbackContext.h"
#include "AssetSelection.h"

#include "Widgets/Text/SRichTextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "SPositiveActionButton.h"

#define LOCTEXT_NAMESPACE "PoseSearchDatabaseAssetList"

namespace UE::PoseSearch
{
	FDatabaseAssetTreeNode::FDatabaseAssetTreeNode(
		int32 InSourceAssetIdx,
		ESearchIndexAssetType InSourceAssetType,
		const TSharedRef<FDatabaseViewModel>& InEditorViewModel) 
		: SourceAssetIdx(InSourceAssetIdx)
		, SourceAssetType(InSourceAssetType)
		, EditorViewModel(InEditorViewModel)
	{ }

	TSharedRef<ITableRow> FDatabaseAssetTreeNode::MakeTreeRowWidget(
		const TSharedRef<STableViewBase>& InOwnerTable,
		TSharedRef<FDatabaseAssetTreeNode> InDatabaseAssetNode,
		TSharedRef<FUICommandList> InCommandList,
		TSharedPtr<SDatabaseAssetTree> InHierarchy)
	{
		return SNew(
			SDatabaseAssetListItem, 
			EditorViewModel.Pin().ToSharedRef(), 
			InOwnerTable, 
			InDatabaseAssetNode, 
			InCommandList, 
			InHierarchy);
	}

	void SDatabaseAssetListItem::Construct(
		const FArguments& InArgs,
		const TSharedRef<FDatabaseViewModel>& InEditorViewModel,
		const TSharedRef<STableViewBase>& OwnerTable,
		TSharedRef<FDatabaseAssetTreeNode> InAssetTreeNode,
		TSharedRef<FUICommandList> InCommandList,
		TSharedPtr<SDatabaseAssetTree> InHierarchy)
	{
		WeakAssetTreeNode = InAssetTreeNode;
		EditorViewModel = InEditorViewModel;
		SkeletonView = InHierarchy;

		if (InAssetTreeNode->SourceAssetType == ESearchIndexAssetType::Invalid)
		{
			ConstructGroupItem(OwnerTable);
		}
		else
		{
			ConstructAssetItem(OwnerTable);
		}
	}

	void SDatabaseAssetListItem::ConstructGroupItem(const TSharedRef<STableViewBase>& OwnerTable)
	{
		STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::ChildSlot
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			GenerateItemWidget()
		];

		STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::ConstructInternal(
			STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::FArguments()
			.Style(FEditorStyle::Get(), "DetailsView.TreeView.TableRow")
			.OnCanAcceptDrop(SkeletonView.Pin().Get(), &SDatabaseAssetTree::OnCanAcceptDrop)
			.OnAcceptDrop(SkeletonView.Pin().Get(), &SDatabaseAssetTree::OnAcceptDrop)
			.ShowSelection(true),
			OwnerTable);
	}

	void SDatabaseAssetListItem::ConstructAssetItem(const TSharedRef<STableViewBase>& OwnerTable)
	{
		STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::Construct(
			STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::FArguments()
			.Style(&FEditorStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row"))
			.OnCanAcceptDrop(SkeletonView.Pin().Get(), &SDatabaseAssetTree::OnCanAcceptDrop)
			.OnAcceptDrop(SkeletonView.Pin().Get(), &SDatabaseAssetTree::OnAcceptDrop)
			.ShowWires(false)
			.Content()
			[
				GenerateItemWidget()
			], OwnerTable);
	}

	void SDatabaseAssetListItem::OnAddSequence()
	{
		TSharedPtr<FDatabaseAssetTreeNode> NodePtr = WeakAssetTreeNode.Pin();
		const int32 GroupIdx = NodePtr->Parent.IsValid() ? NodePtr->Parent->SourceAssetIdx : NodePtr->SourceAssetIdx;

		EditorViewModel.Pin()->AddSequenceToDatabase(nullptr, GroupIdx);

		SkeletonView.Pin()->RefreshTreeView(false);
	}

	void SDatabaseAssetListItem::OnAddBlendSpace()
	{
		TSharedPtr<FDatabaseAssetTreeNode> NodePtr = WeakAssetTreeNode.Pin();
		const int32 GroupIdx = NodePtr->Parent.IsValid() ? NodePtr->Parent->SourceAssetIdx : NodePtr->SourceAssetIdx;

		EditorViewModel.Pin()->AddBlendSpaceToDatabase(nullptr, GroupIdx);

		SkeletonView.Pin()->RefreshTreeView(false);
	}

	FText SDatabaseAssetListItem::GetName() const
	{
		FText Name = LOCTEXT("None", "None");

		TSharedPtr<FDatabaseAssetTreeNode> Node = WeakAssetTreeNode.Pin();
		TSharedPtr<FDatabaseViewModel> ViewModel = EditorViewModel.Pin();
		UPoseSearchDatabase* Database = ViewModel->GetPoseSearchDatabase();

		if (Database)
		{
			switch (Node->SourceAssetType)
			{
			case ESearchIndexAssetType::Sequence:
			{
				const UAnimSequence* Sequence = Database->Sequences[Node->SourceAssetIdx].Sequence;
				if (Sequence)
				{
					Name = FText::FromString(Sequence->GetName());
				}
				break;
			}
			case ESearchIndexAssetType::BlendSpace:
			{
				const UBlendSpace* BlendSpace = Database->BlendSpaces[Node->SourceAssetIdx].BlendSpace;
				if (BlendSpace)
				{
					Name = FText::FromString(BlendSpace->GetName());
				}
				break;
			}
			default:
			{
				if (Node->SourceAssetIdx == INDEX_NONE)
				{
					Name = LOCTEXT("Default", "Default");
				}
				else
				{
					FPoseSearchDatabaseGroup& Group = Database->Groups[Node->SourceAssetIdx];
					if (Group.Tag.IsValid())
					{
						Name = FText::FromName(Group.Tag.GetTagName());
					}
				}
			}
			}
		}

		return Name;
	}

	TSharedRef<SWidget> SDatabaseAssetListItem::GenerateItemWidget()
	{
		TSharedPtr<FDatabaseAssetTreeNode> Node = WeakAssetTreeNode.Pin();

		TSharedPtr<SWidget> ItemWidget;

		if (Node->SourceAssetType == ESearchIndexAssetType::Invalid)
		{
			// it's a group
			SAssignNew(ItemWidget, SBorder)
			.BorderImage(this, &SDatabaseAssetListItem::GetGroupBackgroundImage)
			.Padding(FMargin(3.0f, 5.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.Padding(5.0f)
				.AutoWidth()
				[
					SNew(SExpanderArrow, STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::SharedThis(this))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SRichTextBlock)
					.Text(this, &SDatabaseAssetListItem::GetName)
					.TransformPolicy(ETextTransformPolicy::ToUpper)
					.DecoratorStyleSet(&FEditorStyle::Get())
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(2, 0, 0, 0)
				[
					GenerateAddButtonWidget()
				]
			];
		}
		else
		{
			// it's an asset (sequence or blendspace)
			SAssignNew(ItemWidget, SHorizontalBox)
			+ SHorizontalBox::Slot()
			.MaxWidth(18)
			.AutoWidth()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::Get().GetBrush("Icons.Star"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SDatabaseAssetListItem::GetName)
			];
		}

		return ItemWidget.ToSharedRef();
	}

	TSharedRef<SWidget> SDatabaseAssetListItem::GenerateAddButtonWidget()
	{
		FMenuBuilder AddOptions(true, nullptr);

		AddOptions.AddMenuEntry(
			LOCTEXT("AddSequence", "Add Sequence"),
			LOCTEXT("AddSequenceTooltip", "Add new sequence to this group"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetListItem::OnAddSequence)),
			NAME_None,
			EUserInterfaceActionType::Button);

		AddOptions.AddMenuEntry(
			LOCTEXT("AddBlendSpace", "Add Blend Space"),
			LOCTEXT("AddBlendSpaceTooltip", "Add new blend space to this group"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetListItem::OnAddBlendSpace)),
			NAME_None,
			EUserInterfaceActionType::Button);

		TSharedPtr<SComboButton> AddButton;
		SAssignNew(AddButton, SComboButton)
		.ContentPadding(0)
		.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
		.HasDownArrow(false)
		.ButtonContent()
		[
			SNew(SImage)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Image(FAppStyle::Get().GetBrush("Icons.PlusCircle"))
		]
		.MenuContent()
		[
			AddOptions.MakeWidget()
		];

		return AddButton.ToSharedRef();
	}


	const FSlateBrush* SDatabaseAssetListItem::GetGroupBackgroundImage() const
	{
		if (STableRow<TSharedPtr<FDatabaseAssetTreeNode>>::IsHovered())
		{
			return FAppStyle::Get().GetBrush("Brushes.Secondary");
		}
		else
		{
			return FAppStyle::Get().GetBrush("Brushes.Header");
		}
	}

	SDatabaseAssetTree::~SDatabaseAssetTree()
	{
	}

	void SDatabaseAssetTree::Construct(
		const FArguments& InArgs, 
		TSharedRef<FDatabaseViewModel> InEditorViewModel)
	{
		EditorViewModel = InEditorViewModel;
		CommandList = MakeShared<FUICommandList>();

		ChildSlot
		[
			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 4, 0)
				[
					SNew(SPositiveActionButton)
					.Icon(FAppStyle::Get().GetBrush("Icons.Plus"))
					.Text(LOCTEXT("AddNew", "Add"))
					.ToolTipText(LOCTEXT("AddNewToolTip", "Add a new Sequence, Blend Space or Group"))
					.OnGetMenuContent(this, &SDatabaseAssetTree::CreateAddNewMenuWidget)
				]
			]
			+SVerticalBox::Slot()
			.Padding(0.0f, 0.0f)
			[
				SNew(SBorder)
				.Padding(2.0f)
				.BorderImage(FEditorStyle::GetBrush("SCSEditor.TreePanel"))
				[
					SAssignNew(TreeView, STreeView<TSharedPtr<FDatabaseAssetTreeNode>>)
					.TreeItemsSource(&RootNodes)
					.SelectionMode(ESelectionMode::Multi)
					.OnGenerateRow(this, &SDatabaseAssetTree::MakeTableRowWidget)
					.OnGetChildren(this, &SDatabaseAssetTree::HandleGetChildrenForTree)
					.OnContextMenuOpening(this, &SDatabaseAssetTree::CreateContextMenu)
					.HighlightParentNodesForSelection(false)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FDatabaseAssetTreeNode> Item, ESelectInfo::Type Type)
						{
							TArray<TSharedPtr<FDatabaseAssetTreeNode>> SelectedItems = TreeView->GetSelectedItems();
							OnSelectionChanged.Broadcast(SelectedItems, Type);
						})
					.ItemHeight(24)
				]
			]
		];

		RefreshTreeView(true);
	}

	FReply SDatabaseAssetTree::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
	{
		FReply Reply = FReply::Unhandled();

		TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();

		const bool bValidOperation =
			Operation.IsValid() &&
			(Operation->IsOfType<FExternalDragOperation>() || Operation->IsOfType<FAssetDragDropOp>());
		if (bValidOperation)
		{
			Reply = AssetUtil::CanHandleAssetDrag(DragDropEvent);

			if (!Reply.IsEventHandled())
			{
				if (Operation->IsOfType<FAssetDragDropOp>())
				{
					const TSharedPtr<FAssetDragDropOp> AssetDragDropOp = StaticCastSharedPtr<FAssetDragDropOp>(Operation);

					for (const FAssetData& AssetData : AssetDragDropOp->GetAssets())
					{
						if (UClass* AssetClass = AssetData.GetClass())
						{
							if (AssetClass->IsChildOf(UAnimSequence::StaticClass()) ||
								AssetClass->IsChildOf(UBlendSpace::StaticClass()))
							{
								Reply = FReply::Handled();
								break;
							}
						}
					}
				}
			}
		}

		return Reply;
	}

	FReply SDatabaseAssetTree::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
	{
		return OnAcceptDrop(DragDropEvent, EItemDropZone::OntoItem, nullptr);
	}

	void SDatabaseAssetTree::RefreshTreeView(bool bIsInitialSetup, bool bRecoverSelection)
	{
		const TSharedPtr<FDatabaseViewModel> ViewModel = EditorViewModel.Pin();
		if (!ViewModel.IsValid())
		{
			return;
		}

		const TSharedRef<FDatabaseViewModel> ViewModelRef = ViewModel.ToSharedRef();

		RootNodes.Reset();
		AllNodes.Reset();

		const UPoseSearchDatabase* Database = ViewModel->GetPoseSearchDatabase();
		if (!IsValid(Database))
		{
			TreeView->RequestTreeRefresh();
			return;
		}

		// store selection so we can recover it afterwards (if possible)
		TArray<TSharedPtr<FDatabaseAssetTreeNode>> PreviouslySelectedNodes = TreeView->GetSelectedItems();

		// create all group nodes
		for (int32 GroupIdx = 0; GroupIdx < Database->Groups.Num(); ++GroupIdx)
		{
			TSharedPtr<FDatabaseAssetTreeNode> GroupNode = MakeShared<FDatabaseAssetTreeNode>(
				GroupIdx,
				ESearchIndexAssetType::Invalid,
				ViewModelRef);
			AllNodes.Add(GroupNode);
			RootNodes.Add(GroupNode);
		}
		TSharedPtr<FDatabaseAssetTreeNode> DefaultGroupNode = MakeShared<FDatabaseAssetTreeNode>(
			INDEX_NONE,
			ESearchIndexAssetType::Invalid,
			ViewModelRef);
		AllNodes.Add(DefaultGroupNode);
		RootNodes.Add(DefaultGroupNode);

		const int32 DefaultGroupIdx = RootNodes.Num() - 1;

		auto CreateAssetNode = [this, ViewModelRef](int32 AssetIdx, ESearchIndexAssetType AssetType, int32 GroupIdx)
		{
			TSharedPtr<FDatabaseAssetTreeNode> SequenceGroupNode = MakeShared<FDatabaseAssetTreeNode>(
				AssetIdx,
				AssetType,
				ViewModelRef);
			TSharedPtr<FDatabaseAssetTreeNode>& ParentGroupNode = RootNodes[GroupIdx];
			SequenceGroupNode->Parent = ParentGroupNode;
			ParentGroupNode->Children.Add(SequenceGroupNode);
			AllNodes.Add(SequenceGroupNode);
		};

		// create all sequence nodes
		for (int32 SequenceIdx = 0; SequenceIdx < Database->Sequences.Num(); ++SequenceIdx)
		{
			const FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[SequenceIdx];

			int32 NumGroups = 0;
			for (const FGameplayTag& GroupTag : DbSequence.GroupTags)
			{
				const int32 GroupIdx = Database->Groups.IndexOfByPredicate(
					[GroupTag](const FPoseSearchDatabaseGroup& Group)
				{
					return Group.Tag == GroupTag;
				});

				if (GroupIdx != INDEX_NONE)
				{
					CreateAssetNode(SequenceIdx, ESearchIndexAssetType::Sequence, GroupIdx);
					++NumGroups;
				}
			}

			if (NumGroups == 0)
			{
				CreateAssetNode(SequenceIdx, ESearchIndexAssetType::Sequence, DefaultGroupIdx);
			}
		}

		// create all blendspace nodes
		for (int32 BlendSpaceIdx = 0; BlendSpaceIdx < Database->BlendSpaces.Num(); ++BlendSpaceIdx)
		{
			const FPoseSearchDatabaseBlendSpace& DbBlendSpace = Database->BlendSpaces[BlendSpaceIdx];

			int32 NumGroups = 0;
			for (const FGameplayTag& GroupTag : DbBlendSpace.GroupTags)
			{
				const int32 GroupIdx = Database->Groups.IndexOfByPredicate(
					[GroupTag](const FPoseSearchDatabaseGroup& Group)
				{
					return Group.Tag == GroupTag;
				});

				if (GroupIdx != INDEX_NONE)
				{
					CreateAssetNode(BlendSpaceIdx, ESearchIndexAssetType::BlendSpace, GroupIdx);
					++NumGroups;
				}
			}

			if (NumGroups == 0)
			{
				CreateAssetNode(BlendSpaceIdx, ESearchIndexAssetType::BlendSpace, DefaultGroupIdx);
			}
		}

		TreeView->RequestTreeRefresh();

		for (TSharedPtr<FDatabaseAssetTreeNode>& RootNode : RootNodes)
		{
			TreeView->SetItemExpansion(RootNode, true);
		}

		if (bRecoverSelection)
		{
			RecoverSelection(PreviouslySelectedNodes);
		}
	}

	TSharedRef<ITableRow> SDatabaseAssetTree::MakeTableRowWidget(
		TSharedPtr<FDatabaseAssetTreeNode> InItem,
		const TSharedRef<STableViewBase>& OwnerTable)
	{
		return InItem->MakeTreeRowWidget(OwnerTable, InItem.ToSharedRef(), CommandList.ToSharedRef(), SharedThis(this));
	}

	void SDatabaseAssetTree::HandleGetChildrenForTree(
		TSharedPtr<FDatabaseAssetTreeNode> InNode,
		TArray<TSharedPtr<FDatabaseAssetTreeNode>>& OutChildren)
	{
		OutChildren = InNode.Get()->Children;
	}

	TOptional<EItemDropZone> SDatabaseAssetTree::OnCanAcceptDrop(
		const FDragDropEvent& DragDropEvent,
		EItemDropZone DropZone,
		TSharedPtr<FDatabaseAssetTreeNode> TargetItem)
	{
		TOptional<EItemDropZone> ReturnedDropZone;

		TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();

		const bool bValidOperation = Operation.IsValid() && Operation->IsOfType<FAssetDragDropOp>();
		if (bValidOperation)
		{
			const TSharedPtr<FAssetDragDropOp> AssetDragDropOp = StaticCastSharedPtr<FAssetDragDropOp>(Operation);

			for (const FAssetData& AssetData : AssetDragDropOp->GetAssets())
			{
				if (UClass* AssetClass = AssetData.GetClass())
				{
					if (AssetClass->IsChildOf(UAnimSequence::StaticClass()) ||
						AssetClass->IsChildOf(UBlendSpace::StaticClass()))
					{
						ReturnedDropZone = EItemDropZone::OntoItem;
						break;
					}
				}
			}
		}

		return ReturnedDropZone;
	}

	FReply SDatabaseAssetTree::OnAcceptDrop(
		const FDragDropEvent& DragDropEvent,
		EItemDropZone DropZone,
		TSharedPtr<FDatabaseAssetTreeNode> TargetItem)
	{
		TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();

		const bool bValidOperation = Operation.IsValid() && Operation->IsOfType<FAssetDragDropOp>();
		if (!bValidOperation)
		{
			return FReply::Unhandled();
		}

		TSharedPtr<FDatabaseViewModel> ViewModel = EditorViewModel.Pin();
		if (!ViewModel.IsValid())
		{
			return FReply::Unhandled();
		}

		TArray<FAssetData> DroppedAssetData = AssetUtil::ExtractAssetDataFromDrag(Operation);
		const int32 NumAssets = DroppedAssetData.Num();

		int32 AddedAssets = 0;
		if (NumAssets > 0)
		{
			GWarn->BeginSlowTask(LOCTEXT("LoadingAssets", "Loading Asset(s)"), true);
			for (int32 DroppedAssetIdx = 0; DroppedAssetIdx < NumAssets; ++DroppedAssetIdx)
			{
				const FAssetData& AssetData = DroppedAssetData[DroppedAssetIdx];

				if (!AssetData.IsAssetLoaded())
				{
					GWarn->StatusUpdate(
						DroppedAssetIdx,
						NumAssets,
						FText::Format(
							LOCTEXT("LoadingAsset", "Loading Asset {0}"),
							FText::FromName(AssetData.AssetName)));
				}

				UClass* AssetClass = AssetData.GetClass();
				UObject* Asset = AssetData.GetAsset();

				if (AssetClass->IsChildOf(UAnimSequence::StaticClass()))
				{
					const int32 GroupIdx = FindGroupIndex(TargetItem);
					ViewModel->AddSequenceToDatabase(Cast<UAnimSequence>(Asset), GroupIdx);
					++AddedAssets;
				}
				else if (AssetClass->IsChildOf(UBlendSpace::StaticClass()))
				{
					const int32 GroupIdx = FindGroupIndex(TargetItem);
					ViewModel->AddBlendSpaceToDatabase(Cast<UBlendSpace>(Asset), GroupIdx);
					++AddedAssets;
				}
			}

			GWarn->EndSlowTask();
		}

		if (AddedAssets == 0)
		{
			return FReply::Unhandled();
		}

		RefreshTreeView(false);
		return FReply::Handled();
	}

	int32 SDatabaseAssetTree::FindGroupIndex(TSharedPtr<FDatabaseAssetTreeNode> TargetItem)
	{
		if (!TargetItem.IsValid())
		{
			return INDEX_NONE;
		}

		if (TargetItem->SourceAssetType == ESearchIndexAssetType::Invalid)
		{
			return TargetItem->SourceAssetIdx;
		}

		check(TargetItem->Parent.IsValid() && TargetItem->Parent->SourceAssetType == ESearchIndexAssetType::Invalid);
		return TargetItem->Parent->SourceAssetIdx;
	}

	TSharedRef<SWidget> SDatabaseAssetTree::CreateAddNewMenuWidget()
	{
		FMenuBuilder AddOptions(true, nullptr);

		AddOptions.BeginSection("AddOptions", LOCTEXT("AssetAddOptions", "Assets"));
		AddOptions.AddMenuEntry(
			LOCTEXT("AddSequence", "Sequence"),
			LOCTEXT("AddSequenceTooltip", "Add new sequence to the default group"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnAddSequence)),
			NAME_None,
			EUserInterfaceActionType::Button);

		AddOptions.AddMenuEntry(
			LOCTEXT("AddBlendSpace", "Blend Space"),
			LOCTEXT("AddBlendSpaceTooltip", "Add new blend space to the default group"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnAddBlendSpace)),
			NAME_None,
			EUserInterfaceActionType::Button);
		AddOptions.EndSection();

		AddOptions.BeginSection("AddOptions", LOCTEXT("GroupAddOptions", "Groups"));
		AddOptions.AddMenuEntry(
			LOCTEXT("AddBlendSpace", "Group"),
			LOCTEXT("AddBlendSpaceTooltip", "Add new group"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnAddGroup)),
			NAME_None,
			EUserInterfaceActionType::Button);
		AddOptions.EndSection();

		return AddOptions.MakeWidget();
	}

	TSharedPtr<SWidget> SDatabaseAssetTree::CreateContextMenu()
	{
		const bool CloseAfterSelection = true;
		FMenuBuilder MenuBuilder(CloseAfterSelection, CommandList);

		const TArray<TSharedPtr<FDatabaseAssetTreeNode>> SelectedNodes = TreeView->GetSelectedItems();
		if (!SelectedNodes.IsEmpty())
		{
			TSharedPtr<FDatabaseAssetTreeNode> SelectedNode = SelectedNodes[0];

			if (SelectedNode->SourceAssetType != ESearchIndexAssetType::Invalid)
			{
				const int32 GroupIdx = SelectedNode->Parent->SourceAssetIdx;
				if (GroupIdx == INDEX_NONE)
				{
					MenuBuilder.AddMenuEntry(
						LOCTEXT("DeleteAsset", "Delete Asset"),
						LOCTEXT("DeleteAssetTooltip", "Delete asset from database"),
						FSlateIcon(),
						FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnDeleteAsset, SelectedNode)),
						NAME_None,
						EUserInterfaceActionType::Button);
				}
				else
				{
					MenuBuilder.AddMenuEntry(
						LOCTEXT("RemoveFromGroup", "Remove From Group"),
						LOCTEXT("RemoveFromGroupTooltip", "Remove asset from group"),
						FSlateIcon(),
						FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnRemoveFromGroup, SelectedNode)),
						NAME_None,
						EUserInterfaceActionType::Button);
				}
			}
			else if (SelectedNode->SourceAssetIdx != INDEX_NONE)
			{
				// a group that's not default
				MenuBuilder.AddMenuEntry(
					LOCTEXT("DeleteGroup", "Delete Group"),
					LOCTEXT("DeleteGroupTooltip", "Delete group and remove it from the sequences in it"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateSP(this, &SDatabaseAssetTree::OnDeleteGroup, SelectedNode)),
					NAME_None,
					EUserInterfaceActionType::Button);
			}

		}

		return MenuBuilder.MakeWidget();
	}

	void SDatabaseAssetTree::OnAddGroup()
	{
		EditorViewModel.Pin()->AddGroupToDatabase();
		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::OnAddSequence()
	{
		EditorViewModel.Pin()->AddSequenceToDatabase(nullptr, INDEX_NONE);
		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::OnAddBlendSpace()
	{
		EditorViewModel.Pin()->AddBlendSpaceToDatabase(nullptr, INDEX_NONE);
		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::OnDeleteAsset(TSharedPtr<FDatabaseAssetTreeNode> Node)
	{
		if (Node->SourceAssetType == ESearchIndexAssetType::Sequence)
		{
			EditorViewModel.Pin()->DeleteSequenceFromDatabase(Node->SourceAssetIdx);
		}
		else if (Node->SourceAssetType == ESearchIndexAssetType::BlendSpace)
		{
			EditorViewModel.Pin()->DeleteBlendSpaceFromDatabase(Node->SourceAssetIdx);
		}
		else
		{
			checkNoEntry();
		}

		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::OnRemoveFromGroup(TSharedPtr<FDatabaseAssetTreeNode> Node)
	{
		check(Node->Parent->SourceAssetType == ESearchIndexAssetType::Invalid);
		const int32 GroupIdx = Node->Parent->SourceAssetIdx;

		if (Node->SourceAssetType == ESearchIndexAssetType::Sequence)
		{
			EditorViewModel.Pin()->RemoveSequenceFromGroup(Node->SourceAssetIdx, GroupIdx);
		}
		else if (Node->SourceAssetType == ESearchIndexAssetType::BlendSpace)
		{
			EditorViewModel.Pin()->RemoveBlendSpaceFromGroup(Node->SourceAssetIdx, GroupIdx);
		}
		else
		{
			checkNoEntry();
		}

		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::OnDeleteGroup(TSharedPtr<FDatabaseAssetTreeNode> Node)
	{
		check(Node->SourceAssetType == ESearchIndexAssetType::Invalid);
		const int32 GroupIdx = Node->SourceAssetIdx;
		EditorViewModel.Pin()->DeleteGroup(GroupIdx);

		RefreshTreeView(false);
	}

	void SDatabaseAssetTree::RegisterOnSelectionChanged(const FOnSelectionChanged& Delegate)
	{
		OnSelectionChanged.Add(Delegate);
	}

	void SDatabaseAssetTree::UnregisterOnSelectionChanged(void* Unregister)
	{
		OnSelectionChanged.RemoveAll(Unregister);
	}

	void SDatabaseAssetTree::RecoverSelection(const TArray<TSharedPtr<FDatabaseAssetTreeNode>>& PreviouslySelectedNodes)
	{
		TArray<TSharedPtr<FDatabaseAssetTreeNode>> NewSelectedNodes;

		for (const TSharedPtr<FDatabaseAssetTreeNode>& Node : AllNodes)
		{
			bool bFoundNode = PreviouslySelectedNodes.ContainsByPredicate(
				[Node](const TSharedPtr<FDatabaseAssetTreeNode>& PrevSelectedNode)
			{
				return
					PrevSelectedNode->SourceAssetType == Node->SourceAssetType &&
					PrevSelectedNode->SourceAssetIdx == Node->SourceAssetIdx;
			});

			if (bFoundNode)
			{
				NewSelectedNodes.Add(Node);
			}
		}

		TreeView->SetItemSelection(NewSelectedNodes, true, ESelectInfo::Direct);
	}
}

#undef LOCTEXT_NAMESPACE
