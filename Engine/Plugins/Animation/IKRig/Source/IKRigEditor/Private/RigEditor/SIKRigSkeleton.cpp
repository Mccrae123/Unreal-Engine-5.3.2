// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigEditor/SIKRigSkeleton.h"

#include "IKRigSolver.h"
#include "Engine/SkeletalMesh.h"
#include "IPersonaToolkit.h"
#include "SKismetInspector.h"
#include "Dialogs/Dialogs.h"
#include "SEditorHeaderButton.h"
#include "RigEditor/IKRigEditorController.h"
#include "RigEditor/IKRigEditorStyle.h"
#include "RigEditor/IKRigSkeletonCommands.h"
#include "RigEditor/IKRigToolkit.h"
#include "RigEditor/SIKRigSolverStack.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Commands/UICommandList.h"

#define LOCTEXT_NAMESPACE "SIKRigSkeleton"

FIKRigTreeElement::FIKRigTreeElement(const FName& InKey, IKRigTreeElementType InType)
{
	Key = InKey;
	ElementType = InType;
}

TSharedRef<ITableRow> FIKRigTreeElement::MakeTreeRowWidget(
	TSharedRef<FIKRigEditorController> InEditorController,
	const TSharedRef<STableViewBase>& InOwnerTable,
	TSharedRef<FIKRigTreeElement> InRigTreeElement,
	TSharedRef<FUICommandList> InCommandList,
	TSharedPtr<SIKRigSkeleton> InSkeleton)
{
	return SNew(SIKRigSkeletonItem, InEditorController, InOwnerTable, InRigTreeElement, InCommandList, InSkeleton);
}

void FIKRigTreeElement::RequestRename()
{
	OnRenameRequested.ExecuteIfBound();
}

void SIKRigSkeletonItem::Construct(
	const FArguments& InArgs,
	TSharedRef<FIKRigEditorController> InEditorController,
    const TSharedRef<STableViewBase>& OwnerTable,
    TSharedRef<FIKRigTreeElement> InRigTreeElement,
    TSharedRef<FUICommandList> InCommandList,
    TSharedPtr<SIKRigSkeleton> InSkeleton)
{
	WeakRigTreeElement = InRigTreeElement;
	EditorController = InEditorController;

	// is this element affected by the selected solver?
	bool bIsConnectedToSelectedSolver;
	const int32 SelectedSolver = InEditorController->GetSelectedSolverIndex();
	if (SelectedSolver == INDEX_NONE)
	{
		bIsConnectedToSelectedSolver = InEditorController->IsElementConnectedToAnySolver(InRigTreeElement);
	}
	else
	{
		bIsConnectedToSelectedSolver = InEditorController->IsElementConnectedToSolver(InRigTreeElement, SelectedSolver);
	}

	// determine text style
	FTextBlockStyle NormalText = FIKRigEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("IKRig.Tree.NormalText");
	FTextBlockStyle ItalicText = FIKRigEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>("IKRig.Tree.ItalicText");
	FSlateFontInfo TextFont;
	FSlateColor TextColor;
	if (bIsConnectedToSelectedSolver)
	{
		// elements connected to the selected solver are green
		TextFont = ItalicText.Font;
		TextColor = NormalText.ColorAndOpacity;
	}
	else
	{
		TextFont = NormalText.Font;
		TextColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.5f);
	}

	// determine which icon to use for tree element
	const FSlateBrush* Brush = FAppStyle::Get().GetBrush("SkeletonTree.Bone");
	switch(InRigTreeElement->ElementType)
	{
		case IKRigTreeElementType::BONE:
			if (!InEditorController->IsElementExcludedBone(InRigTreeElement))
			{
				Brush = FAppStyle::Get().GetBrush("SkeletonTree.Bone");
			}
			else
			{
				Brush = FAppStyle::Get().GetBrush("SkeletonTree.BoneNonWeighted");
			}
			break;
		case IKRigTreeElementType::BONE_SETTINGS:
			Brush = FIKRigEditorStyle::Get().GetBrush("IKRig.Tree.BoneWithSettings");
			break;
		case IKRigTreeElementType::GOAL:
			Brush = FIKRigEditorStyle::Get().GetBrush("IKRig.Tree.Goal");
			break;
		case IKRigTreeElementType::EFFECTOR:
			Brush = FIKRigEditorStyle::Get().GetBrush("IKRig.Tree.Effector");
			break;
		default:
			checkNoEntry();
	}
	 
	TSharedPtr<SHorizontalBox> HorizontalBox;
	STableRow<TSharedPtr<FIKRigTreeElement>>::Construct(
        STableRow<TSharedPtr<FIKRigTreeElement>>::FArguments()
        .ShowWires(true)
        .OnDragDetected(InSkeleton.Get(), &SIKRigSkeleton::OnDragDetected)
        .OnCanAcceptDrop(InSkeleton.Get(), &SIKRigSkeleton::OnCanAcceptDrop)
        .OnAcceptDrop(InSkeleton.Get(), &SIKRigSkeleton::OnAcceptDrop)
        .Content()
        [
            SAssignNew(HorizontalBox, SHorizontalBox)
            + SHorizontalBox::Slot()
            .MaxWidth(18)
            .FillWidth(1.0)
            .HAlign(HAlign_Left)
            .VAlign(VAlign_Center)
            [
                SNew(SImage)
                .Image(Brush)
            ]
        ], OwnerTable);
	
	if (InRigTreeElement->ElementType == IKRigTreeElementType::BONE)
	{
		HorizontalBox->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
			SNew(STextBlock)
			.Text(this, &SIKRigSkeletonItem::GetName)
			.Font(TextFont)
			.ColorAndOpacity(TextColor)
        ];

		if (InEditorController->AssetController->GetRetargetRoot() == InRigTreeElement->Key)
		{	
			HorizontalBox->AddSlot()
			.AutoWidth()
			.HAlign(HAlign_Left)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RetargetRootLabel", " (Retarget Root)"))
				.Font(ItalicText.Font)
				.ColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 0.5f))
			];
		}
	}
	else
	{
		TSharedPtr< SInlineEditableTextBlock > InlineWidget;
		HorizontalBox->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
	        SAssignNew(InlineWidget, SInlineEditableTextBlock)
		    .Text(this, &SIKRigSkeletonItem::GetName)
		    .Font(TextFont)
			.ColorAndOpacity(TextColor)
		    .OnTextCommitted(this, &SIKRigSkeletonItem::OnNameCommitted)
		    .MultiLine(false)
        ];
		InRigTreeElement->OnRenameRequested.BindSP(InlineWidget.Get(), &SInlineEditableTextBlock::EnterEditingMode);
	}
}

void SIKRigSkeletonItem::OnNameCommitted(const FText& InText, ETextCommit::Type InCommitType) const
{
	check(WeakRigTreeElement.IsValid());

	if (!(InCommitType == ETextCommit::OnEnter || InCommitType == ETextCommit::OnUserMovedFocus))
	{
		return; // make sure user actually intends to commit a name change
	}

	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return; 
	}
	
	const FName OldName = WeakRigTreeElement.Pin()->Key;
	const FName PotentialNewName = FName(InText.ToString());
	const FName NewName = Controller->AssetController->RenameGoal(OldName, PotentialNewName);
	if (NewName != NAME_None)
	{
		WeakRigTreeElement.Pin()->Key = NewName;
	}

	Controller->SkeletonView->RefreshTreeView();
}

FText SIKRigSkeletonItem::GetName() const
{
	return (FText::FromName(WeakRigTreeElement.Pin()->Key));
}

TSharedRef<FIKRigSkeletonDragDropOp> FIKRigSkeletonDragDropOp::New(TWeakPtr<FIKRigTreeElement> InElement)
{
	TSharedRef<FIKRigSkeletonDragDropOp> Operation = MakeShared<FIKRigSkeletonDragDropOp>();
	Operation->Element = InElement;
	Operation->Construct();
	return Operation;
}

TSharedPtr<SWidget> FIKRigSkeletonDragDropOp::GetDefaultDecorator() const
{
	return SNew(SBorder)
        .Visibility(EVisibility::Visible)
        .BorderImage(FEditorStyle::GetBrush("Menu.Background"))
        [
            SNew(STextBlock)
            .Text(FText::FromString(Element.Pin()->Key.ToString()))
        ];
}

void SIKRigSkeleton::Construct(const FArguments& InArgs, TSharedRef<FIKRigEditorController> InEditorController)
{
	EditorController = InEditorController;
	EditorController.Pin()->SkeletonView = SharedThis(this);
	CommandList = MakeShared<FUICommandList>();
	BindCommands();
	
	ChildSlot
    [
        SNew(SVerticalBox)

        +SVerticalBox::Slot()
        .Padding(0.0f, 0.0f)
        [
            SNew(SBorder)
            .Padding(2.0f)
            .BorderImage(FEditorStyle::GetBrush("SCSEditor.TreePanel"))
            [
                SAssignNew(TreeView, SIKRigSkeletonTreeView)
                .TreeItemsSource(&RootElements)
                .SelectionMode(ESelectionMode::Multi)
                .OnGenerateRow(this, &SIKRigSkeleton::MakeTableRowWidget)
                .OnGetChildren(this, &SIKRigSkeleton::HandleGetChildrenForTree)
                .OnSelectionChanged(this, &SIKRigSkeleton::OnSelectionChanged)
                .OnContextMenuOpening(this, &SIKRigSkeleton::CreateContextMenu)
                .OnMouseButtonClick(this, &SIKRigSkeleton::OnItemClicked)
                .OnMouseButtonDoubleClick(this, &SIKRigSkeleton::OnItemDoubleClicked)
                .OnSetExpansionRecursive(this, &SIKRigSkeleton::OnSetExpansionRecursive)
                .HighlightParentNodesForSelection(false)
                .ItemHeight(24)
            ]
        ]
    ];

	const bool IsInitialSetup = true;
	RefreshTreeView(IsInitialSetup);
}

void SIKRigSkeleton::SetSelectedGoalsFromViewport(const TArray<FName>& GoalNames)
{
	if (GoalNames.IsEmpty())
	{
		TreeView->ClearSelection();
		return;
	}
	
	for (const TSharedPtr<FIKRigTreeElement>& Item : AllElements)
	{
		if (GoalNames.Contains(Item->Key))
		{
			TreeView->SetSelection(Item, ESelectInfo::Direct);
		}
	}
}

void SIKRigSkeleton::GetSelectedBoneChains(TArray<FIKRigSkeletonChain>& OutChains)
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return; 
	}

	// get selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBoneItems;
	GetSelectedBones(SelectedBoneItems);

	const FIKRigSkeleton& Skeleton = Controller->AssetController->GetIKRigSkeleton();

	// get selected bone indices
	TArray<int32> SelectedBones;
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBoneItems)
	{
		const FName BoneName = BoneItem.Get()->Key;
		const int32 BoneIndex = Skeleton.GetBoneIndexFromName(BoneName);
		SelectedBones.Add(BoneIndex);
	}
	
	return Skeleton.GetChainsInList(SelectedBones, OutChains);
}

void SIKRigSkeleton::BindCommands()
{
	const FIKRigSkeletonCommands& Commands = FIKRigSkeletonCommands::Get();

	CommandList->MapAction(Commands.NewGoal,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleNewGoal),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanAddNewGoal));
	
	CommandList->MapAction(Commands.DeleteGoal,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleDeleteGoal),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanDeleteGoal));

	CommandList->MapAction(Commands.ConnectGoalToSolvers,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleConnectGoalToSolver),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanConnectGoalToSolvers));

	CommandList->MapAction(Commands.DisconnectGoalFromSolvers,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleDisconnectGoalFromSolver),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanDisconnectGoalFromSolvers));

	CommandList->MapAction(Commands.SetRootBoneOnSolvers,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleSetRootBoneOnSolvers),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanSetRootBoneOnSolvers));

	CommandList->MapAction(Commands.AddBoneSettings,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleAddBoneSettings),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanAddBoneSettings));

	CommandList->MapAction(Commands.RemoveBoneSettings,
        FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleRemoveBoneSettings),
        FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanRemoveBoneSettings));

	CommandList->MapAction(Commands.ExcludeBone,
		FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleExcludeBone),
		FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanExcludeBone));

	CommandList->MapAction(Commands.IncludeBone,
		FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleIncludeBone),
		FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanIncludeBone));

	CommandList->MapAction(Commands.NewRetargetChain,
		FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleNewRetargetChain),
		FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanAddNewRetargetChain));

	CommandList->MapAction(Commands.SetRetargetRoot,
		FExecuteAction::CreateSP(this, &SIKRigSkeleton::HandleSetRetargetRoot),
		FCanExecuteAction::CreateSP(this, &SIKRigSkeleton::CanSetRetargetRoot));
}

void SIKRigSkeleton::FillContextMenu(FMenuBuilder& MenuBuilder)
{
	const FIKRigSkeletonCommands& Actions = FIKRigSkeletonCommands::Get();

	const TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	if (SelectedItems.IsEmpty())
	{
		return;
	}

	MenuBuilder.BeginSection("AddRemoveGoals", LOCTEXT("AddRemoveGoalOperations", "Goals"));
	MenuBuilder.AddMenuEntry(Actions.NewGoal);
	MenuBuilder.AddMenuEntry(Actions.DeleteGoal);
	MenuBuilder.EndSection();
	
	MenuBuilder.BeginSection("ConnectGoals", LOCTEXT("ConnectGoalOperations", "Connect Goals To Solvers"));
	MenuBuilder.AddMenuEntry(Actions.ConnectGoalToSolvers);
	MenuBuilder.AddMenuEntry(Actions.DisconnectGoalFromSolvers);
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("BoneSettings", LOCTEXT("BoneSettingsOperations", "Bone Settings"));
	MenuBuilder.AddMenuEntry(Actions.AddBoneSettings);
	MenuBuilder.AddMenuEntry(Actions.RemoveBoneSettings);
	MenuBuilder.AddMenuEntry(Actions.SetRootBoneOnSolvers);
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("IncludeExclude", LOCTEXT("IncludeExcludeOperations", "Exclude Bones"));
	MenuBuilder.AddMenuEntry(Actions.ExcludeBone);
	MenuBuilder.AddMenuEntry(Actions.IncludeBone);
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("Retargeting", LOCTEXT("RetargetingOperations", "Retargeting"));
	MenuBuilder.AddMenuEntry(Actions.NewRetargetChain);
	MenuBuilder.AddMenuEntry(Actions.SetRetargetRoot);
	MenuBuilder.EndSection();
}

void SIKRigSkeleton::HandleNewGoal()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return; 
	}

	// get names of selected bones and default goal names for them
	TArray<FName> GoalNames;
	TArray<FName> BoneNames;
	const TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView.Get()->GetSelectedItems();
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		if (Item->ElementType != IKRigTreeElementType::BONE)
		{
			continue; // can only add goals to bones
		}

		// build default name for the new goal
		const FName BoneName = Item->Key;
		const FName NewGoalName = FName(BoneName.ToString() + "_Goal");
		
		GoalNames.Add(NewGoalName);
		BoneNames.Add(BoneName);
	}
	
	// add new goals
	Controller->AddNewGoals(GoalNames, BoneNames);
}

bool SIKRigSkeleton::CanAddNewGoal()
{
	// is anything selected?
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	if (SelectedItems.IsEmpty())
	{
		return false;
	}

	// can only add goals to selected bones
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		if (Item->ElementType != IKRigTreeElementType::BONE)
		{
			return false;
		}
	}

	return true;
}

void SIKRigSkeleton::HandleDeleteGoal()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return; 
	}

	TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	TSharedPtr<FIKRigTreeElement> ParentOfDeletedGoal;
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		if (Item->ElementType == IKRigTreeElementType::GOAL)
		{
			Controller->DeleteGoal(Item->Key);
		}
		else if (Item->ElementType == IKRigTreeElementType::EFFECTOR)
		{
			Controller->AssetController->DisconnectGoalFromSolver(Item->EffectorGoalName, Item->EffectorSolverIndex);
		}
	}

	Controller->ShowEmptyDetails();
	// update all views
	Controller->RefreshAllViews();
}
bool SIKRigSkeleton::CanDeleteGoal() const
{
	// is anything selected?
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	if (SelectedItems.IsEmpty())
	{
		return false;
	}

	// are all selected items goals or effectors?
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		const bool bIsGoal = Item->ElementType == IKRigTreeElementType::GOAL;
		const bool bIsEffector = Item->ElementType == IKRigTreeElementType::EFFECTOR;
		if (!(bIsGoal || bIsEffector))
		{
			return false;
		}
	}

	return true;
}

void SIKRigSkeleton::HandleConnectGoalToSolver()
{
	const bool bConnect = true; //connect
	ConnectSelectedGoalsToSelectedSolvers(bConnect);
}

void SIKRigSkeleton::HandleDisconnectGoalFromSolver()
{
	const bool bConnect = false; //disconnect
	ConnectSelectedGoalsToSelectedSolvers(bConnect);
}

bool SIKRigSkeleton::CanConnectGoalToSolvers() const
{
	const bool bCountOnlyConnected = false;
	const int32 NumDisconnected = GetNumSelectedGoalToSolverConnections(bCountOnlyConnected);
	return NumDisconnected > 0;
}

bool SIKRigSkeleton::CanDisconnectGoalFromSolvers() const
{
	const bool bCountOnlyConnected = true;
	const int32 NumConnected = GetNumSelectedGoalToSolverConnections(bCountOnlyConnected);
	return NumConnected > 0;
}

void SIKRigSkeleton::ConnectSelectedGoalsToSelectedSolvers(bool bConnect)
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return; 
	}
	
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedGoals;
	GetSelectedGoals(SelectedGoals);
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);

	UIKRigController* AssetController = Controller->AssetController;
	for (const TSharedPtr<FIKRigTreeElement>& GoalElement : SelectedGoals)
	{
		const FName GoalName = GoalElement->Key;
		const int32 GoalIndex = AssetController->GetGoalIndex(GoalName);
		check(GoalIndex != INDEX_NONE);
		const UIKRigEffectorGoal& EffectorGoal = *AssetController->GetGoal(GoalIndex);
		for (const TSharedPtr<FSolverStackElement>& SolverElement : SelectedSolvers)
		{
			if (bConnect)
			{
				AssetController->ConnectGoalToSolver(EffectorGoal, SolverElement->IndexInStack);	
			}
			else
			{
				AssetController->DisconnectGoalFromSolver(EffectorGoal.GoalName, SolverElement->IndexInStack);	
			}
		}
	}

	// add/remove new effector under goal in skeleton view
	RefreshTreeView();
}

int32 SIKRigSkeleton::GetNumSelectedGoalToSolverConnections(bool bCountOnlyConnected) const
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return 0;
	}
	
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedGoals;
	GetSelectedGoals(SelectedGoals);
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);

	int32 NumMatched = 0;
	for (const TSharedPtr<FIKRigTreeElement>& Goal : SelectedGoals)
	{
		for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
		{
			const bool bIsConnected = Controller->AssetController->IsGoalConnectedToSolver(Goal->Key, Solver->IndexInStack);
			if (bIsConnected == bCountOnlyConnected)
			{
				++NumMatched;
			}
		}
	}

	return NumMatched;
}

void SIKRigSkeleton::HandleSetRootBoneOnSolvers()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}

    // get name of selected root bone
    TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	const FName RootBoneName = SelectedBones[0]->Key;

	// apply to all selected solvers (ignored on solvers that don't accept a root bone)
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	int32 SolverToShow = 0;
	for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
	{
		AssetController->SetRootBone(RootBoneName, Solver->IndexInStack);
		SolverToShow = Solver->IndexInStack;
	}

	// show solver that had it's root bone updated
	Controller->ShowDetailsForSolver(SolverToShow);
	
	// show new icon when bone has settings applied
	RefreshTreeView();
}

bool SIKRigSkeleton::CanSetRootBoneOnSolvers()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return false;
	}
	
	// must have at least 1 bone selected
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	if (SelectedBones.Num() != 1)
	{
		return false;
	}

	// must have at least 1 solver selected that accepts root bones
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
	{
		if (AssetController->GetSolver(Solver->IndexInStack)->CanSetRootBone())
		{
			return true;
		}
	}
	
	return false;
}

void SIKRigSkeleton::HandleAddBoneSettings()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// get selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	
	// add settings for bone on all selected solvers (ignored if already present)
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	FName BoneNameForSettings;
	int32 SolverIndex = INDEX_NONE;
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
		{
			AssetController->AddBoneSetting(BoneItem->Key, Solver->IndexInStack);
			BoneNameForSettings = BoneItem->Key;
			SolverIndex = Solver->IndexInStack;
        }
	}

	Controller->ShowDetailsForBoneSettings(BoneNameForSettings, SolverIndex);

	// show new icon when bone has settings applied
	RefreshTreeView();
}

bool SIKRigSkeleton::CanAddBoneSettings()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return false;
	}
	
    // must have at least 1 bone selected
    TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	if (SelectedBones.IsEmpty())
	{
		return false;
	}

	// must have at least 1 solver selected that does not already have a bone setting for the selected bones
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
		{
			if (AssetController->CanAddBoneSetting(BoneItem->Key, Solver->IndexInStack))
			{
				return true;
			}
        }
	}
	
	return false;
}

void SIKRigSkeleton::HandleRemoveBoneSettings()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// get selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	
	// add settings for bone on all selected solvers (ignored if already present)
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	FName BoneToShowInDetailsView;
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
		{
			AssetController->RemoveBoneSetting(BoneItem->Key, Solver->IndexInStack);
			BoneToShowInDetailsView = BoneItem->Key;
		}
	}

	Controller->ShowDetailsForBone(BoneToShowInDetailsView);
	
	// show new icon when bone has settings applied
	RefreshTreeView();
}

bool SIKRigSkeleton::CanRemoveBoneSettings()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return false;
	}
	
    // must have at least 1 bone selected
    TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	if (SelectedBones.IsEmpty())
	{
		return false;
	}

	// must have at least 1 solver selected that has a bone setting for 1 of the selected bones
	UIKRigController* AssetController = Controller->AssetController;
	TArray<TSharedPtr<FSolverStackElement>> SelectedSolvers;
	Controller->GetSelectedSolvers(SelectedSolvers);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		for (const TSharedPtr<FSolverStackElement>& Solver : SelectedSolvers)
		{
			if (AssetController->CanRemoveBoneSetting(BoneItem->Key, Solver->IndexInStack))
			{
				return true;
			}
		}
	}
	
	return false;
}

void SIKRigSkeleton::HandleExcludeBone()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// exclude selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		Controller->AssetController->SetBoneExcluded(BoneItem->Key, true);
	}

	// show greyed out bone name after being excluded
	RefreshTreeView();
}

bool SIKRigSkeleton::CanExcludeBone()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return false;
	}
	
	// must have at least 1 bone selected that is INCLUDED
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		if (!Controller->AssetController->GetBoneExcluded(BoneItem->Key))
		{
			return true;
		}
	}

	return false;
}

void SIKRigSkeleton::HandleIncludeBone()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// exclude selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		Controller->AssetController->SetBoneExcluded(BoneItem->Key, false);
	}

	// show normal bone name after being included
	RefreshTreeView();
}

bool SIKRigSkeleton::CanIncludeBone()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return false;
	}
	
	// must have at least 1 bone selected that is EXCLUDED
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	for (const TSharedPtr<FIKRigTreeElement>& BoneItem : SelectedBones)
	{
		if (Controller->AssetController->GetBoneExcluded(BoneItem->Key))
		{
			return true;
		}
	}

	return false;
}

void SIKRigSkeleton::HandleNewRetargetChain()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	TArray<FIKRigSkeletonChain> BoneChains;
	GetSelectedBoneChains(BoneChains);
	for (const FIKRigSkeletonChain& BoneChain : BoneChains)
	{
		Controller->AddNewRetargetChain(BoneChain.StartBone, BoneChain.StartBone, BoneChain.EndBone);
	}
	
	Controller->RefreshAllViews();
}

bool SIKRigSkeleton::CanAddNewRetargetChain()
{
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	return !SelectedBones.IsEmpty();
}

void SIKRigSkeleton::HandleSetRetargetRoot()
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// get selected bones
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);

	// must have at least 1 bone selected
	if (SelectedBones.IsEmpty())
	{
		return;
	}

	// set the first selected bone as the retarget root
	Controller->AssetController->SetRetargetRoot(SelectedBones[0]->Key);

	// show root bone after being set
	Controller->RefreshAllViews();
}

bool SIKRigSkeleton::CanSetRetargetRoot()
{
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedBones;
	GetSelectedBones(SelectedBones);
	return !SelectedBones.IsEmpty();
}

void SIKRigSkeleton::GetSelectedBones(TArray<TSharedPtr<FIKRigTreeElement>>& OutBoneItems)
{
	const TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		if (Item->ElementType == IKRigTreeElementType::BONE)
		{
			OutBoneItems.Add(Item);
        }
	}
}

void SIKRigSkeleton::GetSelectedGoals(TArray<TSharedPtr<FIKRigTreeElement>>& OutSelectedGoals) const
{
	OutSelectedGoals.Reset();
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
	for (const TSharedPtr<FIKRigTreeElement>& Item : SelectedItems)
	{
		if (Item->ElementType == IKRigTreeElementType::GOAL)
		{
			OutSelectedGoals.Add(Item);
		}
	}
}

void SIKRigSkeleton::HandleRenameElement() const
{
	TArray<TSharedPtr<FIKRigTreeElement>> SelectedGoals;
	GetSelectedGoals(SelectedGoals);
	if (SelectedGoals.Num() != 1)
	{
		return;
	}
	
	SelectedGoals[0]->RequestRename();
}

void SIKRigSkeleton::RefreshTreeView(bool IsInitialSetup)
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// save expansion state
	TreeView->SaveAndClearSparseItemInfos();

	// reset all tree items
	RootElements.Reset();
	AllElements.Reset();

	// validate we have a skeleton to load
	UIKRigController* AssetController = Controller->AssetController;
	const FIKRigSkeleton& Skeleton = AssetController->GetIKRigSkeleton();
	if (Skeleton.BoneNames.IsEmpty())
	{
		TreeView->RequestTreeRefresh();
		return;
	}

	// get all goals
	TArray<UIKRigEffectorGoal*> Goals = AssetController->GetAllGoals();
	// get all solvers
	const TArray<UIKRigSolver*>& Solvers = AssetController->GetSolverArray();
	// record bone element indices
	TMap<FName, int32> BoneTreeElementIndices;

	// create all bone elements
	for (const FName BoneName : Skeleton.BoneNames)
	{
		// create "Bone" tree element for this bone
		TSharedPtr<FIKRigTreeElement> BoneElement = MakeShared<FIKRigTreeElement>(BoneName, IKRigTreeElementType::BONE);
		const int32 BoneElementIndex = AllElements.Add(BoneElement);
		BoneTreeElementIndices.Add(BoneName, BoneElementIndex);

		// create all "Bone Setting" tree elements for this bone
		for (int32 SolverIndex=0; SolverIndex<Solvers.Num(); ++SolverIndex)
		{
			if (Solvers[SolverIndex]->GetBoneSetting(BoneName))
			{
				const FName DisplayName = FName("Bone Settings for: " + Solvers[SolverIndex]->GetName());
				TSharedPtr<FIKRigTreeElement> SettingsItem = MakeShared<FIKRigTreeElement>(DisplayName, IKRigTreeElementType::BONE_SETTINGS);
				SettingsItem->BoneSettingBoneName = BoneName;
				SettingsItem->BoneSettingsSolverIndex = SolverIndex;
				AllElements.Add(SettingsItem);
				// store hierarchy pointers for item
				BoneElement->Children.Add(SettingsItem);
				SettingsItem->Parent = BoneElement;
			}
		}

		// create all "Goal" and "Effector" tree elements for this bone
		for (const UIKRigEffectorGoal* Goal : Goals)
		{
			if (Goal->BoneName != BoneName)
			{
				continue;
			}
			
			// make new element for goal
			TSharedPtr<FIKRigTreeElement> GoalItem = MakeShared<FIKRigTreeElement>(Goal->GoalName, IKRigTreeElementType::GOAL);
			AllElements.Add(GoalItem);

			// store hierarchy pointers for goal
			BoneElement->Children.Add(GoalItem);
			GoalItem->Parent = BoneElement;

			// add all effectors connected to this goal
			for (int32 SolverIndex=0; SolverIndex<Solvers.Num(); ++SolverIndex)
			{
				if (UObject* Effector = AssetController->GetEffectorForGoal(Goal->GoalName, SolverIndex))
				{
					// make new element for effector
					const FText EffectorPrefix = LOCTEXT("EffectorPrefix", "Effector for");
					const FName DisplayName = FName(EffectorPrefix.ToString() + ": " + Solvers[SolverIndex]->GetName());
					TSharedPtr<FIKRigTreeElement> EffectorItem = MakeShared<FIKRigTreeElement>(DisplayName, IKRigTreeElementType::EFFECTOR);
					EffectorItem->EffectorSolverIndex = SolverIndex;
					EffectorItem->EffectorGoalName = Goal->GoalName;
					AllElements.Add(EffectorItem);
					EffectorItem->Parent = GoalItem;
					GoalItem->Children.Add(EffectorItem);
				}
			}
		}
	}

	// store children/parent pointers on all bone elements
	for (int32 BoneIndex=0; BoneIndex<Skeleton.BoneNames.Num(); ++BoneIndex)
	{
		const FName BoneName = Skeleton.BoneNames[BoneIndex];
		const TSharedPtr<FIKRigTreeElement> BoneTreeElement = AllElements[BoneTreeElementIndices[BoneName]];
		const int32 ParentIndex = Skeleton.ParentIndices[BoneIndex];
		if (ParentIndex < 0)
		{
			// store the root element
			RootElements.Add(BoneTreeElement);
			// has no parent, so skip storing parent pointer
			continue;
		}

		// get parent tree element
		const FName ParentBoneName = Skeleton.BoneNames[ParentIndex];
		const TSharedPtr<FIKRigTreeElement> ParentBoneTreeElement = AllElements[BoneTreeElementIndices[ParentBoneName]];
		// store pointer to child on parent
		ParentBoneTreeElement->Children.Add(BoneTreeElement);
		// store pointer to parent on child
		BoneTreeElement->Parent = ParentBoneTreeElement;
	}

	// restore expansion state
	for (const TSharedPtr<FIKRigTreeElement>& Element : AllElements)
	{
		TreeView->RestoreSparseItemInfos(Element);
	}

	// expand all elements upon the initial construction of the tree
	if (IsInitialSetup)
	{
		for (TSharedPtr<FIKRigTreeElement> RootElement : RootElements)
		{
			SetExpansionRecursive(RootElement, false, true);
		}
	}
	
	TreeView->RequestTreeRefresh();
}

TSharedRef<ITableRow> SIKRigSkeleton::MakeTableRowWidget(
	TSharedPtr<FIKRigTreeElement> InItem,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return InItem->MakeTreeRowWidget(EditorController.Pin().ToSharedRef(), OwnerTable, InItem.ToSharedRef(), CommandList.ToSharedRef(), SharedThis(this));
}

void SIKRigSkeleton::HandleGetChildrenForTree(
	TSharedPtr<FIKRigTreeElement> InItem,
	TArray<TSharedPtr<FIKRigTreeElement>>& OutChildren)
{
	OutChildren = InItem.Get()->Children;
}

void SIKRigSkeleton::OnSelectionChanged(TSharedPtr<FIKRigTreeElement> Selection, ESelectInfo::Type SelectInfo)
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// gate any selection changes NOT made by user clicking mouse
	if (SelectInfo == ESelectInfo::OnMouseClick)
	{
		TArray<TSharedPtr<FIKRigTreeElement>> SelectedGoals;
		GetSelectedGoals(SelectedGoals);
		TArray<FName> SelectedGoalNames;
		for (const TSharedPtr<FIKRigTreeElement>& Goal : SelectedGoals)
		{
			SelectedGoalNames.Add(Goal->Key);
		}
		Controller->HandleGoalsSelectedInTreeView(SelectedGoalNames);
	}
}

TSharedPtr<SWidget> SIKRigSkeleton::CreateContextMenu()
{
	const bool CloseAfterSelection = true;
	FMenuBuilder MenuBuilder(CloseAfterSelection, CommandList);
	FillContextMenu(MenuBuilder);
	return MenuBuilder.MakeWidget();
}

void SIKRigSkeleton::OnItemClicked(TSharedPtr<FIKRigTreeElement> InItem)
{
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return;
	}
	
	// update details view
	if (InItem->ElementType == IKRigTreeElementType::BONE)
	{
		Controller->ShowDetailsForBone(InItem->Key);
	}
	else if (InItem->ElementType == IKRigTreeElementType::GOAL)
	{
		Controller->ShowDetailsForGoal(InItem->Key);
	}
	else if (InItem->ElementType == IKRigTreeElementType::EFFECTOR)
	{
		Controller->ShowDetailsForEffector(InItem->EffectorGoalName, InItem->EffectorSolverIndex);
	}
	else if (InItem->ElementType == IKRigTreeElementType::BONE_SETTINGS)
	{
		Controller->ShowDetailsForBoneSettings(InItem->BoneSettingBoneName, InItem->BoneSettingsSolverIndex);
	}

	// to rename an item, you have to select it first, then click on it again within a time limit (slow double click)
	const bool ClickedOnSameItem = TreeView->LastSelected.Pin().Get() == InItem.Get();
	const uint32 CurrentCycles = FPlatformTime::Cycles();
	const double SecondsPassed = static_cast<double>(CurrentCycles - TreeView->LastClickCycles) * FPlatformTime::GetSecondsPerCycle();
	if (ClickedOnSameItem && SecondsPassed > 0.25f)
	{
		RegisterActiveTimer(0.f,
            FWidgetActiveTimerDelegate::CreateLambda([this](double, float) {
            HandleRenameElement();
            return EActiveTimerReturnType::Stop;
        }));
	}

	TreeView->LastClickCycles = CurrentCycles;
	TreeView->LastSelected = InItem;
}

void SIKRigSkeleton::OnItemDoubleClicked(TSharedPtr<FIKRigTreeElement> InItem)
{
	if (TreeView->IsItemExpanded(InItem))
	{
		SetExpansionRecursive(InItem, false, false);
	}
	else
	{
		SetExpansionRecursive(InItem, false, true);
	}
}

void SIKRigSkeleton::OnSetExpansionRecursive(TSharedPtr<FIKRigTreeElement> InItem, bool bShouldBeExpanded)
{
	SetExpansionRecursive(InItem, false, bShouldBeExpanded);
}

void SIKRigSkeleton::SetExpansionRecursive(
	TSharedPtr<FIKRigTreeElement> InElement,
	bool bTowardsParent,
    bool bShouldBeExpanded)
{
	TreeView->SetItemExpansion(InElement, bShouldBeExpanded);
    
    if (bTowardsParent)
    {
    	if (InElement->Parent.Get())
    	{
    		SetExpansionRecursive(InElement->Parent, bTowardsParent, bShouldBeExpanded);
    	}
    }
    else
    {
    	for (int32 ChildIndex = 0; ChildIndex < InElement->Children.Num(); ++ChildIndex)
    	{
    		SetExpansionRecursive(InElement->Children[ChildIndex], bTowardsParent, bShouldBeExpanded);
    	}
    }
}

FReply SIKRigSkeleton::OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView.Get()->GetSelectedItems();
	if (SelectedItems.Num() != 1)
	{
		return FReply::Unhandled();
	}

	const TSharedPtr<FIKRigTreeElement> DraggedElement = SelectedItems[0];
	if (DraggedElement->ElementType != IKRigTreeElementType::GOAL)
	{
		return FReply::Unhandled();
	}
	
	if (MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		const TSharedRef<FIKRigSkeletonDragDropOp> DragDropOp = FIKRigSkeletonDragDropOp::New(DraggedElement);
		return FReply::Handled().BeginDragDrop(DragDropOp);
	}

	return FReply::Unhandled();
}

TOptional<EItemDropZone> SIKRigSkeleton::OnCanAcceptDrop(
	const FDragDropEvent& DragDropEvent,
	EItemDropZone DropZone,
    TSharedPtr<FIKRigTreeElement> TargetItem)
{
	TOptional<EItemDropZone> ReturnedDropZone;
	
	const TSharedPtr<FIKRigSkeletonDragDropOp> DragDropOp = DragDropEvent.GetOperationAs<FIKRigSkeletonDragDropOp>();
	if (DragDropOp.IsValid())
	{
		if (TargetItem.Get()->ElementType == IKRigTreeElementType::BONE)
        {
        	ReturnedDropZone = EItemDropZone::BelowItem;	
        }
	}
	
	return ReturnedDropZone;
}

FReply SIKRigSkeleton::OnAcceptDrop(
	const FDragDropEvent& DragDropEvent,
	EItemDropZone DropZone,
    TSharedPtr<FIKRigTreeElement> TargetItem)
{
	const TSharedPtr<FIKRigSkeletonDragDropOp> DragDropOp = DragDropEvent.GetOperationAs<FIKRigSkeletonDragDropOp>();
	if (!DragDropOp.IsValid())
	{
		return FReply::Unhandled();
	}
	
	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}

	const FIKRigTreeElement& DraggedElement = *DragDropOp.Get()->Element.Pin().Get();
	UIKRigController* AssetController = Controller->AssetController;
	const bool bWasReparented = AssetController->SetGoalBone(DraggedElement.Key, TargetItem.Get()->Key);
	if (bWasReparented)
	{
		RefreshTreeView();
	}
	
	return FReply::Handled();
}

FReply SIKRigSkeleton::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	const TSharedPtr<FIKRigEditorController> Controller = EditorController.Pin();
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}

	// handle deleting selected items
	if (Key == EKeys::Delete)
	{
		TArray<TSharedPtr<FIKRigTreeElement>> SelectedItems = TreeView->GetSelectedItems();
		for (const TSharedPtr<FIKRigTreeElement>& SelectedItem : SelectedItems)
		{
			switch(SelectedItem->ElementType)
			{
				case IKRigTreeElementType::GOAL:
					Controller->DeleteGoal(SelectedItem->Key);
					break;
				case IKRigTreeElementType::EFFECTOR:
					Controller->AssetController->DisconnectGoalFromSolver(SelectedItem->EffectorGoalName, SelectedItem->EffectorSolverIndex);
					break;
				case IKRigTreeElementType::BONE_SETTINGS:
					Controller->AssetController->RemoveBoneSetting(SelectedItem->BoneSettingBoneName, SelectedItem->BoneSettingsSolverIndex);
					break;
				default:
					checkNoEntry()
			}
		}

		RefreshTreeView();
		
		return FReply::Handled();
	}
	
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE