// Copyright Epic Games, Inc. All Rights Reserved.

#include "ActorBrowsingMode.h"
#include "SceneOutlinerFilters.h"
#include "SceneOutlinerModule.h"
#include "SceneOutlinerMenuContext.h"
#include "ActorHierarchy.h"
#include "SceneOutlinerDelegates.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "UnrealEdGlobals.h"
#include "ToolMenus.h"
#include "Engine/Selection.h"
#include "Editor/UnrealEdEngine.h"
#include "HAL/PlatformApplicationMisc.h"
#include "SceneOutlinerDragDrop.h"
#include "EditorActorFolders.h"
#include "ActorEditorUtils.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "Logging/MessageLog.h"
#include "SSocketChooser.h"
#include "ActorFolderPickingMode.h"
#include "ActorTreeItem.h"
#include "ActorFolderTreeItem.h"
#include "WorldTreeItem.h"
#include "ComponentTreeItem.h"
#include "ActorBrowsingModeSettings.h"
#include "ScopedTransaction.h"
#include "Foundation/FoundationActor.h"
#include "Foundation/FoundationSubsystem.h"
#include "Foundation/FoundationEditorInstanceActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogActorBrowser, Log, All);

#define LOCTEXT_NAMESPACE "SceneOutliner_ActorBrowsingMode"

UActorBrowsingModeSettings::UActorBrowsingModeSettings(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{ }

using FActorFilter = TSceneOutlinerPredicateFilter<FActorTreeItem>;

FActorBrowsingMode::FActorBrowsingMode(SSceneOutliner* InSceneOutliner, TWeakObjectPtr<UWorld> InSpecifiedWorldToDisplay)
	: FActorModeInteractive(FActorModeParams(InSceneOutliner, InSpecifiedWorldToDisplay, true, false))
	, FilteredActorCount(0)
{
	// Capture selection changes of bones from mesh selection in fracture tools
	FSceneOutlinerDelegates::Get().OnComponentsUpdated.AddRaw(this, &FActorBrowsingMode::OnComponentsUpdated);

	GEngine->OnLevelActorDeleted().AddRaw(this, &FActorBrowsingMode::OnLevelActorDeleted);

	FEditorDelegates::OnEditCutActorsBegin.AddRaw(this, &FActorBrowsingMode::OnEditCutActorsBegin);
	FEditorDelegates::OnEditCutActorsEnd.AddRaw(this, &FActorBrowsingMode::OnEditCutActorsEnd);
	FEditorDelegates::OnEditCopyActorsBegin.AddRaw(this, &FActorBrowsingMode::OnEditCopyActorsBegin);
	FEditorDelegates::OnEditCopyActorsEnd.AddRaw(this, &FActorBrowsingMode::OnEditCopyActorsEnd);
	FEditorDelegates::OnEditPasteActorsBegin.AddRaw(this, &FActorBrowsingMode::OnEditPasteActorsBegin);
	FEditorDelegates::OnEditPasteActorsEnd.AddRaw(this, &FActorBrowsingMode::OnEditPasteActorsEnd);
	FEditorDelegates::OnDuplicateActorsBegin.AddRaw(this, &FActorBrowsingMode::OnDuplicateActorsBegin);
	FEditorDelegates::OnDuplicateActorsEnd.AddRaw(this, &FActorBrowsingMode::OnDuplicateActorsEnd);
	FEditorDelegates::OnDeleteActorsBegin.AddRaw(this, &FActorBrowsingMode::OnDeleteActorsBegin);
	FEditorDelegates::OnDeleteActorsEnd.AddRaw(this, &FActorBrowsingMode::OnDeleteActorsEnd);

	UActorBrowsingModeSettings* SharedSettings = GetMutableDefault<UActorBrowsingModeSettings>();
	// Get the OutlinerModule to register FilterInfos with the FilterInfoMap
	FSceneOutlinerFilterInfo ShowOnlySelectedActorsInfo(LOCTEXT("ToggleShowOnlySelected", "Only Selected"), LOCTEXT("ToggleShowOnlySelectedToolTip", "When enabled, only displays actors that are currently selected."), SharedSettings->bShowOnlySelectedActors, FCreateSceneOutlinerFilter::CreateStatic(&FActorBrowsingMode::CreateShowOnlySelectedActorsFilter));
	ShowOnlySelectedActorsInfo.OnToggle().AddLambda([this](bool bIsActive)
		{
			UActorBrowsingModeSettings* Settings = GetMutableDefault<UActorBrowsingModeSettings>();
			Settings->bShowOnlySelectedActors = bIsActive;
			Settings->PostEditChange();
		});
	FilterInfoMap.Add(TEXT("ShowOnlySelectedActors"), ShowOnlySelectedActorsInfo);

	FSceneOutlinerFilterInfo HideTemporaryActorsInfo(LOCTEXT("ToggleHideTemporaryActors", "Hide Temporary Actors"), LOCTEXT("ToggleHideTemporaryActorsToolTip", "When enabled, hides temporary/run-time Actors."), SharedSettings->bHideTemporaryActors, FCreateSceneOutlinerFilter::CreateStatic(&FActorBrowsingMode::CreateHideTemporaryActorsFilter));
	HideTemporaryActorsInfo.OnToggle().AddLambda([this](bool bIsActive)
		{
			UActorBrowsingModeSettings* Settings = GetMutableDefault<UActorBrowsingModeSettings>();
			Settings->bHideTemporaryActors = bIsActive;
			Settings->PostEditChange();
		});
	FilterInfoMap.Add(TEXT("HideTemporaryActors"), HideTemporaryActorsInfo);

	FSceneOutlinerFilterInfo OnlyCurrentLevelInfo(LOCTEXT("ToggleShowOnlyCurrentLevel", "Only in Current Level"), LOCTEXT("ToggleShowOnlyCurrentLevelToolTip", "When enabled, only shows Actors that are in the Current Level."), SharedSettings->bShowOnlyActorsInCurrentLevel, FCreateSceneOutlinerFilter::CreateStatic(&FActorBrowsingMode::CreateIsInCurrentLevelFilter));
	OnlyCurrentLevelInfo.OnToggle().AddLambda([this](bool bIsActive)
		{
			UActorBrowsingModeSettings* Settings = GetMutableDefault<UActorBrowsingModeSettings>();
			Settings->bShowOnlyActorsInCurrentLevel = bIsActive;
			Settings->PostEditChange();
		});
	FilterInfoMap.Add(TEXT("ShowOnlyCurrentLevel"), OnlyCurrentLevelInfo);

	bHideComponents = SharedSettings->bHideActorComponents;
	FSceneOutlinerFilterInfo HideComponentsInfo(LOCTEXT("ToggleHideActorComponents", "Hide Actor Components"), LOCTEXT("ToggleHideActorComponentsToolTip", "When enabled, hides components belonging to actors."), SharedSettings->bHideActorComponents, FCreateSceneOutlinerFilter::CreateStatic(&FActorBrowsingMode::CreateHideComponentsFilter));
	HideComponentsInfo.OnToggle().AddLambda([this](bool bIsActive)
		{
			UActorBrowsingModeSettings* Settings = GetMutableDefault<UActorBrowsingModeSettings>();
			Settings->bHideActorComponents = bHideComponents = bIsActive;
			Settings->PostEditChange();

			if (auto ActorHierarchy = StaticCast<FActorHierarchy*>(Hierarchy.Get()))
			{
				ActorHierarchy->SetShowingComponents(!bIsActive);
			}
		});

	FilterInfoMap.Add(TEXT("HideComponentsFilter"), HideComponentsInfo);

	FSceneOutlinerFilterInfo HideFoundationsInfo(LOCTEXT("ToggleHideFoundations", "Hide Foundations"), LOCTEXT("ToggleHideFoundationsToolTip", "When enabled, hides all foundation content."), SharedSettings->bHideFoundationHierarchy, FCreateSceneOutlinerFilter::CreateStatic(&FActorBrowsingMode::CreateHideFoundationsFilter));
	HideFoundationsInfo.OnToggle().AddLambda([this](bool bIsActive)
		{
			UActorBrowsingModeSettings* Settings = GetMutableDefault<UActorBrowsingModeSettings>();
			Settings->bHideFoundationHierarchy = bHideFoundationHierarchy = bIsActive;
			Settings->PostEditChange();

			if (auto ActorHierarchy = StaticCast<FActorHierarchy*>(Hierarchy.Get()))
			{
				ActorHierarchy->SetShowingFoundations(!bIsActive);
			}
		});
	FilterInfoMap.Add(TEXT("HideFoundationsFilter"), HideFoundationsInfo);

	// Add a filter which sets the interactive mode of foundation items and their children
	SceneOutliner->AddFilter(MakeShared<FActorFilter>(FActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* Actor) {return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass, FActorTreeItem::FFilterPredicate::CreateLambda([this](const AActor* Actor)
		{
			if (!bHideFoundationHierarchy)
			{
				if (const UFoundationSubsystem* FoundationSubsystem = RepresentingWorld->GetSubsystem<UFoundationSubsystem>())
				{
					// if actor has a valid parent and the parent is not being edited,
					// then the actor should not be selectable.
					if (const AFoundationActor* ParentFoundation = FoundationSubsystem->GetParentFoundation(Actor))
					{
						if (!FoundationSubsystem->IsEditingFoundation(ParentFoundation))
						{
							return false;
						}
					}
				}
			}
			return true;
		})));

	Rebuild();
}

FActorBrowsingMode::~FActorBrowsingMode()
{
	FSceneOutlinerDelegates::Get().OnComponentsUpdated.RemoveAll(this);

	GEngine->OnLevelActorDeleted().RemoveAll(this);
		
	FEditorDelegates::OnEditCutActorsBegin.RemoveAll(this);
	FEditorDelegates::OnEditCutActorsEnd.RemoveAll(this);
	FEditorDelegates::OnEditCopyActorsBegin.RemoveAll(this);
	FEditorDelegates::OnEditCopyActorsEnd.RemoveAll(this);
	FEditorDelegates::OnEditPasteActorsBegin.RemoveAll(this);
	FEditorDelegates::OnEditPasteActorsEnd.RemoveAll(this);
	FEditorDelegates::OnDuplicateActorsBegin.RemoveAll(this);
	FEditorDelegates::OnDuplicateActorsEnd.RemoveAll(this);
	FEditorDelegates::OnDeleteActorsBegin.RemoveAll(this);
	FEditorDelegates::OnDeleteActorsEnd.RemoveAll(this);
}

void FActorBrowsingMode::Rebuild()
{
	FActorMode::Rebuild();

	FilteredActorCount = 0;
	ApplicableActors.Empty();
}

FText FActorBrowsingMode::GetStatusText() const 
{
	if (!RepresentingWorld.IsValid())
	{
		return FText();
	}

	const int32 TotalActorCount = ApplicableActors.Num();
	const int32 SelectedActorCount = SceneOutliner->GetSelection().Num<FActorTreeItem>();

	if (!SceneOutliner->IsTextFilterActive())
	{
		if (SelectedActorCount == 0) //-V547
		{
			return FText::Format(LOCTEXT("ShowingAllActorsFmt", "{0} actors"), FText::AsNumber(FilteredActorCount));
		}
		else
		{
			return FText::Format(LOCTEXT("ShowingAllActorsSelectedFmt", "{0} actors ({1} selected)"), FText::AsNumber(FilteredActorCount), FText::AsNumber(SelectedActorCount));
		}
	}
	else if (SceneOutliner->IsTextFilterActive() && FilteredActorCount == 0)
	{
		return FText::Format(LOCTEXT("ShowingNoActorsFmt", "No matching actors ({0} total)"), FText::AsNumber(TotalActorCount));
	}
	else if (SelectedActorCount != 0) //-V547
	{
		return FText::Format(LOCTEXT("ShowingOnlySomeActorsSelectedFmt", "Showing {0} of {1} actors ({2} selected)"), FText::AsNumber(FilteredActorCount), FText::AsNumber(TotalActorCount), FText::AsNumber(SelectedActorCount));
	}
	else
	{
		return FText::Format(LOCTEXT("ShowingOnlySomeActorsFmt", "Showing {0} of {1} actors"), FText::AsNumber(FilteredActorCount), FText::AsNumber(TotalActorCount));
	}
}

FSlateColor FActorBrowsingMode::GetStatusTextColor() const
{
	if (!SceneOutliner->IsTextFilterActive())
	{
		return FSlateColor::UseForeground();
	}
	else if (FilteredActorCount == 0)
	{
		return FAppStyle::Get().GetSlateColor("Colors.AccentRed");
	}
	else
	{
		return FAppStyle::Get().GetSlateColor("Colors.AccentGreen");
	}
}

void FActorBrowsingMode::CreateViewContent(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.BeginSection("AssetThumbnails", LOCTEXT("ShowWorldHeading", "World"));
	{
		MenuBuilder.AddSubMenu(
			LOCTEXT("ChooseWorldSubMenu", "Choose World"),
			LOCTEXT("ChooseWorldSubMenuToolTip", "Choose the world to display in the outliner."),
			FNewMenuDelegate::CreateRaw(this, &FActorMode::BuildWorldPickerMenu)
		);
	}
	MenuBuilder.EndSection();
}

TSharedRef<FSceneOutlinerFilter> FActorBrowsingMode::CreateShowOnlySelectedActorsFilter()
{
	auto IsActorSelected = [](const AActor* InActor)
	{
		return InActor && InActor->IsSelected();
	};
	return MakeShareable(new FActorFilter(FActorTreeItem::FFilterPredicate::CreateStatic(IsActorSelected), FSceneOutlinerFilter::EDefaultBehaviour::Fail, FActorTreeItem::FFilterPredicate::CreateStatic(IsActorSelected)));
}

TSharedRef<FSceneOutlinerFilter> FActorBrowsingMode::CreateHideTemporaryActorsFilter()
{
	return MakeShareable(new FActorFilter(FActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* InActor)
		{
			return ((InActor->GetWorld() && InActor->GetWorld()->WorldType != EWorldType::PIE) || GEditor->ObjectsThatExistInEditorWorld.Get(InActor)) && !InActor->HasAnyFlags(EObjectFlags::RF_Transient);
		}), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FActorBrowsingMode::CreateIsInCurrentLevelFilter()
{
	return MakeShareable(new FActorFilter(FActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* InActor)
		{
			if (InActor->GetWorld())
			{
				return InActor->GetLevel() == InActor->GetWorld()->GetCurrentLevel();
			}

			return false;
		}), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FActorBrowsingMode::CreateHideComponentsFilter()
{
	return MakeShared<TSceneOutlinerPredicateFilter<FComponentTreeItem>>(TSceneOutlinerPredicateFilter<FComponentTreeItem>(
		FComponentTreeItem::FFilterPredicate::CreateStatic([](const UActorComponent*) { return false; }), 
		FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FActorBrowsingMode::CreateHideFoundationsFilter()
{
	return MakeShareable(new FActorFilter(FActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* Actor)
		{
			// Check if actor belongs to a foundation
			if (const UFoundationSubsystem* FoundationSubsystem = Actor->GetWorld()->GetSubsystem<UFoundationSubsystem>())
			{
				if (const AFoundationActor* ParentFoundation = FoundationSubsystem->GetParentFoundation(Actor))
				{
					if (!FoundationSubsystem->IsEditingFoundation(ParentFoundation))
					{
						return false;
					}
				}
			}
			// Or if the actor itself is a foundation editor instance
			return Cast<AFoundationEditorInstanceActor>(Actor) == nullptr;
		}), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

static const FName DefaultContextBaseMenuName("SceneOutliner.DefaultContextMenuBase");
static const FName DefaultContextMenuName("SceneOutliner.DefaultContextMenu");

void FActorBrowsingMode::RegisterContextMenu()
{
	UToolMenus* ToolMenus = UToolMenus::Get();

	if (!ToolMenus->IsMenuRegistered(DefaultContextBaseMenuName))
	{
		UToolMenu* Menu = ToolMenus->RegisterMenu(DefaultContextBaseMenuName);

		Menu->AddDynamicSection("DynamicSection1", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				USceneOutlinerMenuContext* Context = InMenu->FindContext<USceneOutlinerMenuContext>();
				if (!Context || !Context->SceneOutliner.IsValid())
				{
					return;
				}

				SSceneOutliner* SceneOutliner = Context->SceneOutliner.Pin().Get();
				if (Context->bShowParentTree)
				{
					if (Context->NumSelectedItems == 0)
					{
						InMenu->FindOrAddSection("Section").AddMenuEntry(
							"CreateFolder",
							LOCTEXT("CreateFolder", "Create Folder"),
							FText(),
							FSlateIcon(FEditorStyle::GetStyleSetName(), "SceneOutliner.NewFolderIcon"),
							FUIAction(FExecuteAction::CreateSP(SceneOutliner, &SSceneOutliner::CreateFolder)));
					}
					else
					{
						if (Context->NumSelectedItems == 1)
						{
							SceneOutliner->GetTree().GetSelectedItems()[0]->GenerateContextMenu(InMenu, *SceneOutliner);
						}

						// If we've only got folders selected, show the selection and edit sub menus
						if (Context->NumSelectedItems > 0 && Context->NumSelectedFolders == Context->NumSelectedItems)
						{
							InMenu->FindOrAddSection("Section").AddSubMenu(
								"SelectSubMenu",
								LOCTEXT("SelectSubmenu", "Select"),
								LOCTEXT("SelectSubmenu_Tooltip", "Select the contents of the current selection"),
								FNewToolMenuDelegate::CreateSP(SceneOutliner, &SSceneOutliner::FillSelectionSubMenu));
						}
					}
				}
			}));

		Menu->AddDynamicSection("DynamicMainSection", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				// We always create a section here, even if there is no parent so that clients can still extend the menu
				FToolMenuSection& Section = InMenu->AddSection("MainSection");

				if (USceneOutlinerMenuContext* Context = InMenu->FindContext<USceneOutlinerMenuContext>())
				{
					// Don't add any of these menu items if we're not showing the parent tree
					// Can't move worlds or level blueprints
					if (Context->bShowParentTree && Context->NumSelectedItems > 0 && Context->NumWorldsSelected == 0 && Context->SceneOutliner.IsValid())
					{
						Section.AddSubMenu(
							"MoveActorsTo",
							LOCTEXT("MoveActorsTo", "Move To"),
							LOCTEXT("MoveActorsTo_Tooltip", "Move selection to another folder"),
							FNewToolMenuDelegate::CreateSP(Context->SceneOutliner.Pin().Get(), &SSceneOutliner::FillFoldersSubMenu));
					}
				}
			}));
	}

	if (!ToolMenus->IsMenuRegistered(DefaultContextMenuName))
	{
		ToolMenus->RegisterMenu(DefaultContextMenuName, DefaultContextBaseMenuName);
	}
}

TSharedPtr<SWidget> FActorBrowsingMode::BuildContextMenu()
{
	RegisterContextMenu();

	FSceneOutlinerItemSelection ItemSelection(SceneOutliner->GetSelection());

	USceneOutlinerMenuContext* ContextObject = NewObject<USceneOutlinerMenuContext>();
	ContextObject->SceneOutliner = StaticCastSharedRef<SSceneOutliner>(SceneOutliner->AsShared());
	ContextObject->bShowParentTree = SceneOutliner->GetSharedData().bShowParentTree;
	ContextObject->NumSelectedItems = ItemSelection.Num();
	ContextObject->NumSelectedFolders = ItemSelection.Num<FFolderTreeItem>();
	ContextObject->NumWorldsSelected = ItemSelection.Num<FWorldTreeItem>();
	FToolMenuContext Context(ContextObject);

	FName MenuName = DefaultContextMenuName;
	SceneOutliner->GetSharedData().ModifyContextMenu.ExecuteIfBound(MenuName, Context);

	// Build up the menu for a selection
	UToolMenus* ToolMenus = UToolMenus::Get();
	UToolMenu* Menu = ToolMenus->GenerateMenu(MenuName, Context);

	for (const FToolMenuSection& Section : Menu->Sections)
	{
		if (Section.Blocks.Num() > 0)
		{
			return ToolMenus->GenerateWidget(Menu);
		}
	}

	return nullptr;
}

TSharedPtr<SWidget> FActorBrowsingMode::CreateContextMenu()
{
	TArray<AActor*> SelectedActors;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(SelectedActors);

	// Make sure that no components are selected
	if (GEditor->GetSelectedComponentCount() > 0)
	{
		// We want to be able to undo to regain the previous component selection
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealEd", "ClickingOnActorsContextMenu", "Clicking on Actors (context menu)"));
		USelection* ComponentSelection = GEditor->GetSelectedComponents();
		ComponentSelection->Modify(false);
		ComponentSelection->DeselectAll();

		GUnrealEd->UpdatePivotLocationForSelection();
		GEditor->RedrawLevelEditingViewports(false);
	}

	return BuildContextMenu();
}

void FActorBrowsingMode::OnItemAdded(FSceneOutlinerTreeItemPtr Item)
{
	if (const FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>())
	{
		if (!Item->Flags.bIsFilteredOut)
		{
			++FilteredActorCount;

			// Synchronize selection
			if (GEditor->GetSelectedActors()->IsSelected(ActorItem->Actor.Get()))
			{
				SceneOutliner->SetItemSelection(Item, true);
			}
		}
	}
	else if (FActorFolderTreeItem* FolderItem = Item->CastTo<FActorFolderTreeItem>())
	{
		if (FolderItem->World.IsValid())
		{
			if (FActorFolderProps* Props = FActorFolders::Get().GetFolderProperties(*FolderItem->World, FolderItem->Path))
			{
				FolderItem->Flags.bIsExpanded = Props->bIsExpanded;
			}
		}
	}
}

void FActorBrowsingMode::OnItemRemoved(FSceneOutlinerTreeItemPtr Item)
{
	if (const FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>())
	{
		if (!ActorItem->Flags.bIsFilteredOut)
		{
			--FilteredActorCount;
		}
	}
}

void FActorBrowsingMode::OnComponentsUpdated()
{
	SceneOutliner->FullRefresh();
}

void FActorBrowsingMode::OnLevelActorDeleted(AActor* Actor)
{
	ApplicableActors.Remove(Actor);
}

void FActorBrowsingMode::OnItemSelectionChanged(FSceneOutlinerTreeItemPtr TreeItem, ESelectInfo::Type SelectionType, const FSceneOutlinerItemSelection& Selection)
{
	TArray<AActor*> SelectedActors = Selection.GetData<AActor*>(SceneOutliner::FActorSelector());

	bool bChanged = false;
	bool bAnyInPIE = false;
	for (auto* Actor : SelectedActors)
	{
		if (!bAnyInPIE && Actor && Actor->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
		{
			bAnyInPIE = true;
		}
		if (!GEditor->GetSelectedActors()->IsSelected(Actor))
		{
			bChanged = true;
			break;
		}
	}

	for (FSelectionIterator SelectionIt(*GEditor->GetSelectedActors()); SelectionIt && !bChanged; ++SelectionIt)
	{
		const AActor* Actor = CastChecked< AActor >(*SelectionIt);
		if (!bAnyInPIE && Actor->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
		{
			bAnyInPIE = true;
		}
		if (!SelectedActors.Contains(Actor))
		{
			// Actor has been deselected
			bChanged = true;

			// If actor was a group actor, remove its members from the ActorsToSelect list
			const AGroupActor* DeselectedGroupActor = Cast<AGroupActor>(Actor);
			if (DeselectedGroupActor)
			{
				TArray<AActor*> GroupActors;
				DeselectedGroupActor->GetGroupActors(GroupActors);

				for (auto* GroupActor : GroupActors)
				{
					SelectedActors.Remove(GroupActor);
				}

			}
		}
	}

	// If there's a discrepancy, update the selected actors to reflect this list.
	if (bChanged)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UnrealEd", "ClickingOnActors", "Clicking on Actors"), !bAnyInPIE);
		GEditor->GetSelectedActors()->Modify();

		// Clear the selection.
		GEditor->SelectNone(false, true, true);

		// We'll batch selection changes instead by using BeginBatchSelectOperation()
		GEditor->GetSelectedActors()->BeginBatchSelectOperation();

		const bool bShouldSelect = true;
		const bool bNotifyAfterSelect = false;
		const bool bSelectEvenIfHidden = true;	// @todo outliner: Is this actually OK?
		for (auto* Actor : SelectedActors)
		{
			UE_LOG(LogActorBrowser, Verbose, TEXT("Clicking on Actor (world outliner): %s (%s)"), *Actor->GetClass()->GetName(), *Actor->GetActorLabel());
			GEditor->SelectActor(Actor, bShouldSelect, bNotifyAfterSelect, bSelectEvenIfHidden);
		}

		// Commit selection changes
		GEditor->GetSelectedActors()->EndBatchSelectOperation(/*bNotify*/false);

		// Fire selection changed event
		GEditor->NoteSelectionChange();
	}

	SceneOutliner->RefreshSelection();
}

void FActorBrowsingMode::OnItemDoubleClick(FSceneOutlinerTreeItemPtr Item)
{
	if (const FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>())
	{
		AActor* Actor = ActorItem->Actor.Get();
		check(Actor);

		AFoundationActor* FoundationActor = Cast<AFoundationActor>(Actor);
		if (FoundationActor && FSlateApplication::Get().GetModifierKeys().IsAltDown())
		{
			if (FoundationActor->CanEdit())
			{
				FoundationActor->Edit();
			}
			else if (FoundationActor->CanCommit())
			{
				FoundationActor->Commit();
			}
		}
		else if (Item->CanInteract())
		{
			FSceneOutlinerItemSelection Selection(SceneOutliner->GetSelection());
			if (Selection.Has<FActorTreeItem>())
			{
				const bool bActiveViewportOnly = false;
				GEditor->MoveViewportCamerasToActor(Selection.GetData<AActor*>(SceneOutliner::FActorSelector()), bActiveViewportOnly);
			}
		}
		else
		{
			const bool bActiveViewportOnly = false;
			GEditor->MoveViewportCamerasToActor(*Actor, bActiveViewportOnly);
		}
	}
	else if (Item->IsA<FFolderTreeItem>())
	{
		SceneOutliner->SetItemExpansion(Item, !SceneOutliner->IsItemExpanded(Item));
	}
}

void FActorBrowsingMode::OnFilterTextCommited(FSceneOutlinerItemSelection& Selection, ETextCommit::Type CommitType)
{
	// Start batching selection changes
	GEditor->GetSelectedActors()->BeginBatchSelectOperation();

	// Select actors (and only the actors) that match the filter text
	const bool bNoteSelectionChange = false;
	const bool bDeselectBSPSurfs = false;
	const bool WarnAboutManyActors = true;
	GEditor->SelectNone(bNoteSelectionChange, bDeselectBSPSurfs, WarnAboutManyActors);
	for (AActor* Actor : Selection.GetData<AActor*>(SceneOutliner::FActorSelector()))
	{
		const bool bShouldSelect = true;
		const bool bSelectEvenIfHidden = false;
		GEditor->SelectActor(Actor, bShouldSelect, bNoteSelectionChange, bSelectEvenIfHidden);
	}

	// Commit selection changes
	GEditor->GetSelectedActors()->EndBatchSelectOperation(/*bNotify*/false);

	// Fire selection changed event
	GEditor->NoteSelectionChange();

	// Set keyboard focus to the SceneOutliner, so the user can perform keyboard commands that interact
	// with selected actors (such as Delete, to delete selected actors.)
	SceneOutliner->SetKeyboardFocus();
}

void FActorBrowsingMode::OnItemPassesFilters(const ISceneOutlinerTreeItem& Item)
{
	if (const FActorTreeItem* ActorItem = Item.CastTo<FActorTreeItem>())
	{
		ApplicableActors.Add(ActorItem->Actor);
	}
}

FReply FActorBrowsingMode::OnKeyDown(const FKeyEvent& InKeyEvent)
{
	const FSceneOutlinerItemSelection& Selection = SceneOutliner->GetSelection();

	// Rename key: Rename selected actors (not rebindable, because it doesn't make much sense to bind.)
	if (InKeyEvent.GetKey() == EKeys::F2)
	{
		if (Selection.Num() == 1)
		{
			FSceneOutlinerTreeItemPtr ItemToRename = Selection.SelectedItems[0].Pin();

			if (ItemToRename.IsValid() && CanRenameItem(*ItemToRename) && ItemToRename->CanInteract())
			{
				SceneOutliner->SetPendingRenameItem(ItemToRename);
				SceneOutliner->ScrollItemIntoView(ItemToRename);
			}

			return FReply::Handled();
		}
	}

	// F5 forces a full refresh
	else if (InKeyEvent.GetKey() == EKeys::F5)
	{
		SceneOutliner->FullRefresh();
		return FReply::Handled();
	}

	// Delete key: Delete selected actors (not rebindable, because it doesn't make much sense to bind.)
	// Use Delete and Backspace instead of Platform_Delete because the LevelEditor default Edit Delete is bound to both 
	else if (InKeyEvent.GetKey() == EKeys::Delete || InKeyEvent.GetKey() == EKeys::BackSpace)
	{
		if (SceneOutliner->GetSharedData().CustomDelete.IsBound())
		{
			SceneOutliner->GetSharedData().CustomDelete.Execute(Selection.SelectedItems);
		}
		else
		{
			if (RepresentingWorld.IsValid())
			{
				GUnrealEd->Exec(RepresentingWorld.Get(), TEXT("DELETE"));
			}
		}
		return FReply::Handled();
			
	}
	return FReply::Unhandled();
}

bool FActorBrowsingMode::CanRename() const
{
	const FSceneOutlinerItemSelection ItemSelection = SceneOutliner->GetSelection();
	const uint32 NumberOfFolders = ItemSelection.Num<FFolderTreeItem>();
	return (NumberOfFolders == 1 && NumberOfFolders == ItemSelection.Num());
}

bool FActorBrowsingMode::CanRenameItem(const ISceneOutlinerTreeItem& Item) const
{
	// Can only rename actor and folder items when in actor browsing mode
	return (Item.IsValid() && (Item.IsA<FActorTreeItem>() || Item.IsA<FFolderTreeItem>()));
}

bool FActorBrowsingMode::CanCut() const
{
	const FSceneOutlinerItemSelection ItemSelection = SceneOutliner->GetSelection();
	const uint32 NumberOfFolders = ItemSelection.Num<FFolderTreeItem>();
	return (NumberOfFolders > 0 && NumberOfFolders == ItemSelection.Num());
}

bool FActorBrowsingMode::CanCopy() const
{
	const FSceneOutlinerItemSelection ItemSelection = SceneOutliner->GetSelection();
	const uint32 NumberOfFolders = ItemSelection.Num<FFolderTreeItem>();
	return (NumberOfFolders > 0 && NumberOfFolders == ItemSelection.Num());
}

bool FActorBrowsingMode::CanPaste() const
{
	return CanPasteFoldersOnlyFromClipboard();
}

bool FActorBrowsingMode::CanPasteFoldersOnlyFromClipboard() const
{
	// Intentionally not checking if the level is locked/hidden here, as it's better feedback for the user if they attempt to paste
	// and get the message explaining why it's failed, than just not having the option available to them.
	FString PasteString;
	FPlatformApplicationMisc::ClipboardPaste(PasteString);
	return PasteString.StartsWith("BEGIN FOLDERLIST");
}

TSharedPtr<FDragDropOperation> FActorBrowsingMode::CreateDragDropOperation(const TArray<FSceneOutlinerTreeItemPtr>& InTreeItems) const
{
	FSceneOutlinerDragDropPayload DraggedObjects(InTreeItems);

	TSharedPtr<FSceneOutlinerDragDropOp> OutlinerOp = MakeShareable(new FSceneOutlinerDragDropOp());

	if (DraggedObjects.Has<FActorTreeItem>())
	{
		TSharedPtr<FActorDragDropOp> ActorOperation = MakeShareable(new FActorDragDropOp);
		ActorOperation->Init(DraggedObjects.GetData<TWeakObjectPtr<AActor>>(SceneOutliner::FWeakActorSelector()));
		OutlinerOp->AddSubOp(ActorOperation);
	}

	if (DraggedObjects.Has<FFolderTreeItem>())
	{
		TSharedPtr<FFolderDragDropOp> FolderOperation = MakeShareable(new FFolderDragDropOp);
		FolderOperation->Init(DraggedObjects.GetData<FName>(SceneOutliner::FFolderPathSelector()));
		OutlinerOp->AddSubOp(FolderOperation);
	}
	OutlinerOp->Construct();
	return OutlinerOp;
}
	
bool FActorBrowsingMode::ParseDragDrop(FSceneOutlinerDragDropPayload& OutPayload, const FDragDropOperation& Operation) const
{
	if (Operation.IsOfType<FSceneOutlinerDragDropOp>())
	{
		const auto& OutlinerOp = static_cast<const FSceneOutlinerDragDropOp&>(Operation);
		if (const auto& FolderOp = OutlinerOp.GetSubOp<FFolderDragDropOp>())
		{
			for (const auto& Folder : FolderOp->Folders)
			{
				OutPayload.DraggedItems.Add(SceneOutliner->GetTreeItem(Folder));
			}
		}
		if (const auto& ActorOp = OutlinerOp.GetSubOp<FActorDragDropOp>())
		{
			for (const auto& Actor : ActorOp->Actors)
			{
				OutPayload.DraggedItems.Add(SceneOutliner->GetTreeItem(Actor.Get()));
			}
		}
		return true;
	}
	else if (Operation.IsOfType<FActorDragDropOp>())
	{
		for (const TWeakObjectPtr<AActor>& Actor : static_cast<const FActorDragDropOp&>(Operation).Actors)
		{
			OutPayload.DraggedItems.Add(SceneOutliner->GetTreeItem(Actor.Get()));
		}
		return true;
	}
		
	return false;
}

FSceneOutlinerDragValidationInfo FActorBrowsingMode::ValidateDrop(const ISceneOutlinerTreeItem& DropTarget, const FSceneOutlinerDragDropPayload& Payload) const
{
	if (const FActorTreeItem* ActorItem = DropTarget.CastTo<FActorTreeItem>())
	{
		if (Payload.Has<FFolderTreeItem>())
		{
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("FoldersOnActorError", "Cannot attach folders to actors"));
		}

		const AActor* ActorTarget = ActorItem->Actor.Get();

		if (!ActorTarget || !Payload.Has<FActorTreeItem>())
		{
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, FText());
		}

		const AFoundationActor* FoundationTarget = Cast<AFoundationActor>(ActorTarget);
		const UFoundationSubsystem* FoundationSubsystem = RepresentingWorld->GetSubsystem<UFoundationSubsystem>();

		if (FoundationTarget)
		{
			check(FoundationSubsystem);
			if (!FoundationTarget->IsEditing())
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("Error_AttachToClosedFoundation", "Cannot attach to foundation which is not being edited"));
			}
		}

		FText AttachErrorMsg;
		bool bCanAttach = true;
		bool bDraggedOntoAttachmentParent = true;
		const auto& DragActors = Payload.GetData<TWeakObjectPtr<AActor>>(SceneOutliner::FWeakActorSelector());
		for (const auto& DragActorPtr : DragActors)
		{
			AActor* DragActor = DragActorPtr.Get();
			if (DragActor)
			{
				if (bCanAttach)
				{
					if (FoundationSubsystem)
					{
						// Either all actors must be in a foundation or none of them
						if (const AFoundationActor* ParentFoundation = FoundationSubsystem->GetParentFoundation(DragActor))
						{
							if (!ParentFoundation->IsEditing())
							{
								return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("Error_RemoveEditingFoundation", "Cannot detach from a foundation which is not being edited"));
							}
						}

						if (!FoundationSubsystem->CanMoveActorToLevel(DragActor))
						{
							AttachErrorMsg = LOCTEXT("Error_MoveActorToFoundation", "Cannot move foundation while it or its children are being edited");
							bCanAttach = bDraggedOntoAttachmentParent = false;
							break;
						}
					}

					if (DragActor->IsChildActor())
					{
						AttachErrorMsg = FText::Format(LOCTEXT("Error_AttachChildActor", "Cannot move {0} as it is a child actor."), FText::FromString(DragActor->GetActorLabel()));
						bCanAttach = bDraggedOntoAttachmentParent = false;
						break;
					}
					if (!FoundationTarget && !GEditor->CanParentActors(ActorTarget, DragActor, &AttachErrorMsg))
					{
						bCanAttach = false;
					}
				}

				if (DragActor->GetAttachParentActor() != ActorTarget)
				{
					bDraggedOntoAttachmentParent = false;
				}
			}
		}

		const FText ActorLabel = FText::FromString(ActorTarget->GetActorLabel());
		if (bDraggedOntoAttachmentParent)
		{
			if (DragActors.Num() == 1)
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleDetach, ActorLabel);
			}
			else
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleMultipleDetach, ActorLabel);
			}
		}
		else if (bCanAttach)
		{
			if (DragActors.Num() == 1)
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleAttach, ActorLabel);
			}
			else
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleMultipleAttach, ActorLabel);
			}
		}
		else
		{
			if (DragActors.Num() == 1)
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, AttachErrorMsg);
			}
			else
			{
				const FText ReasonText = FText::Format(LOCTEXT("DropOntoText", "{0}. {1}"), ActorLabel, AttachErrorMsg);
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleMultipleAttach, ReasonText);
			}
		}
	}
	else if (DropTarget.IsA<FFolderTreeItem>() || DropTarget.IsA<FWorldTreeItem>())
	{
		const FFolderTreeItem* FolderItem = DropTarget.CastTo<FFolderTreeItem>();
		// World items are treated as folders with path = none
		const FName& DestinationPath = FolderItem ? FolderItem->Path : NAME_None;
		if (Payload.Has<FFolderTreeItem>())
		{
			// Iterate over all the folders that have been dragged
			for (FName DraggedFolder : Payload.GetData<FName>(SceneOutliner::FFolderPathSelector()))
			{
				const FName Leaf = SceneOutliner::GetFolderLeafName(DraggedFolder);
				const FName Parent = SceneOutliner::GetParentPath(DraggedFolder);

				if (Parent == DestinationPath)
				{
					FFormatNamedArguments Args;
					Args.Add(TEXT("SourceName"), FText::FromName(Leaf));

					FText Text;
					if (DestinationPath.IsNone())
					{
						Text = FText::Format(LOCTEXT("FolderAlreadyAssignedRoot", "{SourceName} is already assigned to root"), Args);
					}
					else
					{
						Args.Add(TEXT("DestPath"), FText::FromName(DestinationPath));
						Text = FText::Format(LOCTEXT("FolderAlreadyAssigned", "{SourceName} is already assigned to {DestPath}"), Args);
					}

					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, Text);
				}

				const FString DragFolderPath = DraggedFolder.ToString();
				const FString LeafName = Leaf.ToString();
				const FString DstFolderPath = DestinationPath.IsNone() ? FString() : DestinationPath.ToString();
				const FString NewPath = DstFolderPath / LeafName;

				if (FActorFolders::Get().GetFolderProperties(*RepresentingWorld, FName(*NewPath)))
				{
					// The folder already exists
					FFormatNamedArguments Args;
					Args.Add(TEXT("DragName"), FText::FromString(LeafName));
					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric,
						FText::Format(LOCTEXT("FolderAlreadyExistsRoot", "A folder called \"{DragName}\" already exists at this level"), Args));
				}
				else if (DragFolderPath == DstFolderPath || DstFolderPath.StartsWith(DragFolderPath + "/"))
				{
					// Cannot drag as a child of itself
					FFormatNamedArguments Args;
					Args.Add(TEXT("FolderPath"), FText::FromName(DraggedFolder));
					return FSceneOutlinerDragValidationInfo(
						ESceneOutlinerDropCompatibility::IncompatibleGeneric,
						FText::Format(LOCTEXT("ChildOfItself", "Cannot move \"{FolderPath}\" to be a child of itself"), Args));
				}
			}
		}

		if (Payload.Has<FActorTreeItem>())
		{
			const UFoundationSubsystem* FoundationSubsystem = RepresentingWorld->GetSubsystem<UFoundationSubsystem>();
			// Iterate over all the actors that have been dragged
			for (const TWeakObjectPtr<AActor>& WeakActor : Payload.GetData<TWeakObjectPtr<AActor>>(SceneOutliner::FWeakActorSelector()))
			{
				const AActor* Actor = WeakActor.Get();

				bool bActorContainedInFoundation = false;
				if (FoundationSubsystem)
				{
					if (const AFoundationActor* ParentFoundation = FoundationSubsystem->GetParentFoundation(Actor))
					{
						if (!ParentFoundation->IsEditing())
						{
							return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("Error_RemoveEditingFoundation", "Cannot detach from a foundation which is not being edited"));
						}
						bActorContainedInFoundation = true;
					}

					if (const AFoundationActor* FoundationActor = Cast<AFoundationActor>(Actor))
					{
						if (!FoundationSubsystem->CanMoveActorToLevel(FoundationActor))
						{
							return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("Error_RemoveEditingFoundation", "Cannot detach a foundation which is currently being edited"));
						}
					}
				}
				
				if (Actor->IsChildActor())
				{
					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, FText::Format(LOCTEXT("Error_AttachChildActor", "Cannot move {0} as it is a child actor."), FText::FromString(Actor->GetActorLabel())));
				}
				else if (Actor->GetFolderPath() == DestinationPath && !Actor->GetAttachParentActor() && !bActorContainedInFoundation)
				{
					FFormatNamedArguments Args;
					Args.Add(TEXT("SourceName"), FText::FromString(Actor->GetActorLabel()));

					FText Text;
					if (DestinationPath.IsNone())
					{
						Text = FText::Format(LOCTEXT("FolderAlreadyAssignedRoot", "{SourceName} is already assigned to root"), Args);
					}
					else
					{
						Args.Add(TEXT("DestPath"), FText::FromName(DestinationPath));
						Text = FText::Format(LOCTEXT("FolderAlreadyAssigned", "{SourceName} is already assigned to {DestPath}"), Args);
					}

					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, Text);
				}
			}
		}

		// Everything else is a valid operation
		if (DestinationPath.IsNone())
		{
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleGeneric, LOCTEXT("MoveToRoot", "Move to root"));
		}
		else
		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("DestPath"), FText::FromName(DestinationPath));
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::CompatibleGeneric, FText::Format(LOCTEXT("MoveInto", "Move into \"{DestPath}\""), Args));
		}
	}
	else if (DropTarget.IsA<FComponentTreeItem>())
	{
		// we don't allow drag and drop on components for now
		return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, FText());
	}
	return FSceneOutlinerDragValidationInfo::Invalid();
}

void FActorBrowsingMode::OnDrop(ISceneOutlinerTreeItem& DropTarget, const FSceneOutlinerDragDropPayload& Payload, const FSceneOutlinerDragValidationInfo& ValidationInfo) const
{
	if (const FActorTreeItem* ActorItem = DropTarget.CastTo<FActorTreeItem>())
	{
		AActor* DropActor = ActorItem->Actor.Get();
		if (!DropActor)
		{
			return;
		}

		FMessageLog EditorErrors("EditorErrors");
		EditorErrors.NewPage(LOCTEXT("ActorAttachmentsPageLabel", "Actor attachment"));

		if (ValidationInfo.CompatibilityType == ESceneOutlinerDropCompatibility::CompatibleMultipleDetach || ValidationInfo.CompatibilityType == ESceneOutlinerDropCompatibility::CompatibleDetach)
		{
			const FScopedTransaction Transaction(LOCTEXT("UndoAction_DetachActors", "Detach actors"));

			TArray<TWeakObjectPtr<AActor>> DraggedActors = Payload.GetData<TWeakObjectPtr<AActor>>(SceneOutliner::FWeakActorSelector());
			for (const auto& WeakActor : DraggedActors)
			{
				if (auto* DragActor = WeakActor.Get())
				{
					// Detach from parent
					USceneComponent* RootComp = DragActor->GetRootComponent();
					if (RootComp && RootComp->GetAttachParent())
					{
						AActor* OldParent = RootComp->GetAttachParent()->GetOwner();
						OldParent->Modify();
						RootComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

						DragActor->SetFolderPath_Recursively(OldParent->GetFolderPath());
					}
				}
			}
		}
		else if (ValidationInfo.CompatibilityType == ESceneOutlinerDropCompatibility::CompatibleMultipleAttach || ValidationInfo.CompatibilityType == ESceneOutlinerDropCompatibility::CompatibleAttach)
		{
			// Show socket chooser if we have sockets to select

			if (AFoundationActor* TargetFoundation = Cast<AFoundationActor>(DropActor))
			{
				// Actors inside foundations cannot have folder paths
				TArray<AActor*> DraggedActors = Payload.GetData<AActor*>(SceneOutliner::FActorSelector());
				for (auto& Actor : DraggedActors)
				{
					Actor->SetFolderPath_Recursively(FName());
				}

				UFoundationSubsystem* FoundationSubsystem = RepresentingWorld->GetSubsystem<UFoundationSubsystem>();
				check(FoundationSubsystem);

				check(TargetFoundation->IsEditing());
				const FScopedTransaction Transaction(LOCTEXT("UndoAction_MoveActorsToFoundation", "Move actors to foundation"));

				FoundationSubsystem->MoveActorsTo(TargetFoundation, DraggedActors);
			}
			else
			{
				auto PerformAttachment = [](FName SocketName, TWeakObjectPtr<AActor> Parent, const TArray<TWeakObjectPtr<AActor>> NewAttachments)
				{
					AActor* ParentActor = Parent.Get();
					if (ParentActor)
					{
						// modify parent and child
						const FScopedTransaction Transaction(LOCTEXT("UndoAction_PerformAttachment", "Attach actors"));

						// Attach each child
						bool bAttached = false;
						for (auto& Child : NewAttachments)
						{
							AActor* ChildActor = Child.Get();
							if (GEditor->CanParentActors(ParentActor, ChildActor))
							{
								GEditor->ParentActors(ParentActor, ChildActor, SocketName);

								ChildActor->SetFolderPath_Recursively(ParentActor->GetFolderPath());
							}
						}
					}
				};

				TArray<TWeakObjectPtr<AActor>> DraggedActors = Payload.GetData<TWeakObjectPtr<AActor>>(SceneOutliner::FWeakActorSelector());
				//@TODO: Should create a menu for each component that contains sockets, or have some form of disambiguation within the menu (like a fully qualified path)
				// Instead, we currently only display the sockets on the root component
				USceneComponent* Component = DropActor->GetRootComponent();
				if ((Component != NULL) && (Component->HasAnySockets()))
				{
					// Create the popup
					FSlateApplication::Get().PushMenu(
						SceneOutliner->AsShared(),
						FWidgetPath(),
						SNew(SSocketChooserPopup)
						.SceneComponent(Component)
						.OnSocketChosen_Lambda(PerformAttachment, DropActor, MoveTemp(DraggedActors)),
						FSlateApplication::Get().GetCursorPos(),
						FPopupTransitionEffect(FPopupTransitionEffect::TypeInPopup)
					);
				}
				else
				{
					PerformAttachment(NAME_None, DropActor, MoveTemp(DraggedActors));
				}
			}
			
		}
		// Report errors
		EditorErrors.Notify(NSLOCTEXT("ActorAttachmentError", "AttachmentsFailed", "Attachments Failed!"));
	}
	else if (DropTarget.IsA<FFolderTreeItem>() || DropTarget.IsA<FWorldTreeItem>())
	{
		const FFolderTreeItem* FolderItem = DropTarget.CastTo<FFolderTreeItem>();
		// If the cast fails, the item must be a WorldTreeItem and we set the path to None to reflect this
		const FName& DestinationPath = FolderItem ? FolderItem->Path : FName(NAME_None);

		const FScopedTransaction Transaction(LOCTEXT("MoveOutlinerItems", "Move World Outliner Items"));

		auto MoveToDestination = [&DestinationPath](FFolderTreeItem& Item)
		{
			Item.MoveTo(DestinationPath);
		};
		Payload.ForEachItem<FFolderTreeItem>(MoveToDestination);

		// Set the folder path on all the dragged actors, and detach any that need to be moved
		if (Payload.Has<FActorTreeItem>())
		{
			TSet<const AActor*> ParentActors;
			TSet<const AActor*> ChildActors;

			Payload.ForEachItem<FActorTreeItem>([&DestinationPath, &ParentActors, &ChildActors](const FActorTreeItem& ActorItem)
				{
					AActor* Actor = ActorItem.Actor.Get();
					if (Actor)
					{
						// First mark this object as a parent, then set its children's path
						ParentActors.Add(Actor);
						Actor->SetFolderPath(DestinationPath);

						FActorEditorUtils::TraverseActorTree_ParentFirst(Actor, [&](AActor* InActor) {
							ChildActors.Add(InActor);
							InActor->SetFolderPath(DestinationPath);
							return true;
							}, false);
					}
				});

			// Detach parent actors
			for (const AActor* Parent : ParentActors)
			{
				auto* RootComp = Parent->GetRootComponent();

				// We don't detach if it's a child of another that's been dragged
				if (RootComp && RootComp->GetAttachParent() && !ChildActors.Contains(Parent))
				{
					if (AActor* OldParentActor = RootComp->GetAttachParent()->GetOwner())
					{
						OldParentActor->Modify();
					}
					RootComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
				}
			}

			const UFoundationSubsystem* FoundationSubsystem = RepresentingWorld->GetSubsystem<UFoundationSubsystem>();
			check(FoundationSubsystem);
			// Since we are moving to a folder (or root), we must be moving into the persistent level.
			ULevel* DestinationLevel = RepresentingWorld->PersistentLevel;
			check(DestinationLevel);

			TArray<AActor*> ActorsToMove;
			Payload.ForEachItem<FActorTreeItem>([FoundationSubsystem, &ActorsToMove](const FActorTreeItem& ActorItem)
				{
					AActor* Actor = ActorItem.Actor.Get();
					if (const AFoundationActor* ParentFoundation = FoundationSubsystem->GetParentFoundation(Actor))
					{
						check(ParentFoundation->IsEditing());
						ActorsToMove.Add(Actor);
					}
				});

			TArray<AActor*> DraggedActors = Payload.GetData<AActor*>(SceneOutliner::FActorSelector());
			FoundationSubsystem->MoveActorsToLevel(ActorsToMove, DestinationLevel);
		}
	}
}

FName FActorBrowsingMode::CreateNewFolder()
{
	const FScopedTransaction Transaction(LOCTEXT("UndoAction_CreateFolder", "Create Folder"));

	const FName NewFolderName = FActorFolders::Get().GetDefaultFolderNameForSelection(*RepresentingWorld);
	FActorFolders::Get().CreateFolderContainingSelection(*RepresentingWorld, NewFolderName);

	return NewFolderName;
}

FName FActorBrowsingMode::CreateFolder(const FName& ParentPath, const FName& LeafName)
{
	const FName NewPath = FActorFolders::Get().GetFolderName(*RepresentingWorld, ParentPath, LeafName);
	FActorFolders::Get().CreateFolder(*RepresentingWorld, NewPath);
	return NewPath;
}

bool FActorBrowsingMode::ReparentItemToFolder(const FName& FolderPath, const FSceneOutlinerTreeItemPtr& Item)
{
	if (FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>())
	{
		ActorItem->Actor->SetFolderPath_Recursively(FolderPath);
		return true;
	}
	return false;
}

void FActorBrowsingMode::SelectFoldersDescendants(const TArray<FFolderTreeItem*>& FolderItems, bool bSelectImmediateChildrenOnly)
{
	// Ensure that all folder descendants are expanded
	TFunctionRef<void(const FSceneOutlinerTreeItemPtr&)> RecursiveExpand = [&](const FSceneOutlinerTreeItemPtr&  Item)
	{
		SceneOutliner->SetItemExpansion(Item, true);
			
		if (!bSelectImmediateChildrenOnly)
		{
			for (const TWeakPtr<ISceneOutlinerTreeItem> Child : Item->GetChildren())
			{
				RecursiveExpand(Child.Pin());
			}
		}
	};

	// Selects all actor descendants of a folder
	TFunctionRef<void(const FSceneOutlinerTreeItemPtr&)> RecursiveActorSelect = [&](const FSceneOutlinerTreeItemPtr& Item)
	{
		// If the current item is an actor, ensure to select it as well
		if (FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>())
		{
			if (AActor* Actor = ActorItem->Actor.Get())
			{
				GEditor->SelectActor(Actor, true, false);
			}
		}
		// Select all children
		for (const TWeakPtr<ISceneOutlinerTreeItem> Child : Item->GetChildren())
		{
			if (FActorTreeItem* ActorItem = Child.Pin()->CastTo<FActorTreeItem>())
			{
				if (AActor* Actor = ActorItem->Actor.Get())
				{
					GEditor->SelectActor(Actor, true, false);
				}
			}
			else if (FFolderTreeItem* FolderItem = Child.Pin()->CastTo<FFolderTreeItem>())
			{
				SceneOutliner->SetItemSelection(FolderItem->AsShared(), true);
			}

			if (!bSelectImmediateChildrenOnly)
			{
				for (const TWeakPtr<ISceneOutlinerTreeItem>& Grandchild : Child.Pin()->GetChildren())
				{
					RecursiveActorSelect(Grandchild.Pin());
				}
			}
		}
	};

	// Expand everything before beginning selection
	for (FFolderTreeItem* Folder : FolderItems)
	{
		RecursiveExpand(Folder->AsShared());
	}

	// batch selection
	GEditor->GetSelectedActors()->BeginBatchSelectOperation();

	for (FFolderTreeItem* Folder : FolderItems)
	{
		RecursiveActorSelect(Folder->AsShared());
	}

	GEditor->GetSelectedActors()->EndBatchSelectOperation(/*bNotify*/false);
	GEditor->NoteSelectionChange();
}

FCreateSceneOutlinerMode FActorBrowsingMode::CreateFolderPickerMode() const
{
	auto MoveSelectionTo = [this](const FSceneOutlinerTreeItemRef& NewParent)
	{
		if (NewParent->IsA<FWorldTreeItem>())
		{
			SceneOutliner->MoveSelectionTo(FName());
		}
		else if (FFolderTreeItem* FolderItem = NewParent->CastTo<FFolderTreeItem>())
		{
			SceneOutliner->MoveSelectionTo(FolderItem->Path);
		}
	};

	return FCreateSceneOutlinerMode::CreateLambda([this, MoveSelectionTo](SSceneOutliner* Outliner)
		{
			return new FActorFolderPickingMode(Outliner, FOnSceneOutlinerItemPicked::CreateLambda(MoveSelectionTo));
		});
}

void FActorBrowsingMode::OnDuplicateSelected()
{
	GUnrealEd->Exec(RepresentingWorld.Get(), TEXT("DUPLICATE"));
}

void FActorBrowsingMode::OnEditCutActorsBegin()
{
	// Only a callback in actor browsing mode
	SceneOutliner->CopyFoldersBegin();
	SceneOutliner->DeleteFoldersBegin();
}

void FActorBrowsingMode::OnEditCutActorsEnd()
{
	// Only a callback in actor browsing mode
	SceneOutliner->CopyFoldersEnd();
	SceneOutliner->DeleteFoldersEnd();
}

void FActorBrowsingMode::OnEditCopyActorsBegin()
{
	// Only a callback in actor browsing mode
	SceneOutliner->CopyFoldersBegin();
}

void FActorBrowsingMode::OnEditCopyActorsEnd()
{
	// Only a callback in actor browsing mode
	SceneOutliner->CopyFoldersEnd();
}

void FActorBrowsingMode::OnEditPasteActorsBegin()
{
	// Only a callback in actor browsing mode
	const TArray<FName> FolderPaths = SceneOutliner->GetClipboardPasteFolders();
	SceneOutliner->PasteFoldersBegin(FolderPaths);
}

void FActorBrowsingMode::OnEditPasteActorsEnd()
{
	// Only a callback in actor browsing mode
	SceneOutliner->PasteFoldersEnd();
}

void FActorBrowsingMode::OnDuplicateActorsBegin()
{
	// Only a callback in actor browsing mode
	const TArray<FName> SelectedFolderPaths = SceneOutliner->GetSelection().GetData<FName>(SceneOutliner::FFolderPathSelector());
	SceneOutliner->PasteFoldersBegin(SelectedFolderPaths);
}

void FActorBrowsingMode::OnDuplicateActorsEnd()
{
	// Only a callback in actor browsing mode
	SceneOutliner->PasteFoldersEnd();
}

void FActorBrowsingMode::OnDeleteActorsBegin()
{
	SceneOutliner->DeleteFoldersBegin();
}

void FActorBrowsingMode::OnDeleteActorsEnd()
{
	SceneOutliner->DeleteFoldersEnd();
}

#undef LOCTEXT_NAMESPACE