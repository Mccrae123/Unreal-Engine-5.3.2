// Copyright Epic Games, Inc. All Rights Reserved.

#include "DataLayerMode.h"
#include "SSceneOutliner.h"
#include "SDataLayerBrowser.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "DataLayer/DataLayerOutlinerDeleteButtonColumn.h"
#include "WorldPartition/DataLayer/DataLayerSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/WorldPartitionEditorPerProjectUserSettings.h"
#include "DataLayer/SDataLayerOutliner.h"
#include "DataLayerHierarchy.h"
#include "DataLayerActorTreeItem.h"
#include "DataLayerTreeItem.h"
#include "WorldDataLayersTreeItem.h"
#include "WorldTreeItem.h"
#include "LevelInstance/LevelInstanceEditorInstanceActor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "ActorDescTreeItem.h"
#include "ISceneOutlinerHierarchy.h"
#include "SceneOutlinerMenuContext.h"
#include "ScopedTransaction.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "DragAndDrop/FolderDragDropOp.h"
#include "ActorMode.h"
#include "DataLayersActorDescTreeItem.h"
#include "EditorActorFolders.h"
#include "Algo/Transform.h"
#include "Algo/AnyOf.h"
#include "ToolMenus.h"
#include "Selection.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "DataLayer"

using FDataLayerFilter = TSceneOutlinerPredicateFilter<FDataLayerTreeItem>;
using FDataLayerActorFilter = TSceneOutlinerPredicateFilter<FDataLayerActorTreeItem>;
using FActorDescFilter = TSceneOutlinerPredicateFilter<FActorDescTreeItem>;

FDataLayerModeParams::FDataLayerModeParams(SSceneOutliner* InSceneOutliner, SDataLayerBrowser* InDataLayerBrowser, const TWeakObjectPtr<UWorld>& InSpecifiedWorldToDisplay, FOnSceneOutlinerItemPicked InOnItemPicked)
: SpecifiedWorldToDisplay(InSpecifiedWorldToDisplay)
, DataLayerBrowser(InDataLayerBrowser)
, SceneOutliner(InSceneOutliner)
, OnItemPicked(InOnItemPicked)
{}

FDataLayerMode::FDataLayerMode(const FDataLayerModeParams& Params)
	: ISceneOutlinerMode(Params.SceneOutliner)
	, OnItemPicked(Params.OnItemPicked)
	, DataLayerBrowser(Params.DataLayerBrowser)
	, SpecifiedWorldToDisplay(Params.SpecifiedWorldToDisplay)
	, FilteredDataLayerCount(0)
{
	USelection::SelectionChangedEvent.AddRaw(this, &FDataLayerMode::OnLevelSelectionChanged);
	USelection::SelectObjectEvent.AddRaw(this, &FDataLayerMode::OnLevelSelectionChanged);

	UWorldPartitionEditorPerProjectUserSettings* SharedSettings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
	bHideEditorDataLayers = SharedSettings->bHideEditorDataLayers;
	bHideRuntimeDataLayers = SharedSettings->bHideRuntimeDataLayers;
	bHideDataLayerActors = SharedSettings->bHideDataLayerActors;
	bHideUnloadedActors = SharedSettings->bHideUnloadedActors;
	bShowOnlySelectedActors = SharedSettings->bShowOnlySelectedActors;
	bHighlightSelectedDataLayers = SharedSettings->bHighlightSelectedDataLayers;
	bHideLevelInstanceContent = SharedSettings->bHideLevelInstanceContent;

	FSceneOutlinerFilterInfo ShowOnlySelectedActorsInfo(LOCTEXT("ToggleShowOnlySelected", "Only Selected"), LOCTEXT("ToggleShowOnlySelectedToolTip", "When enabled, only displays actors that are currently selected."), bShowOnlySelectedActors, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateShowOnlySelectedActorsFilter));
	ShowOnlySelectedActorsInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bShowOnlySelectedActors = bShowOnlySelectedActors = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowOnlySelectedActors(bIsActive);
		}

		RefreshSelection();
	});
	FilterInfoMap.Add(TEXT("ShowOnlySelectedActors"), ShowOnlySelectedActorsInfo);

	FSceneOutlinerFilterInfo HideEditorDataLayersInfo(LOCTEXT("ToggleHideEditorDataLayers", "Hide Editor Data Layers"), LOCTEXT("ToggleHideEditorDataLayersToolTip", "When enabled, hides Editor Data Layers."), bHideEditorDataLayers, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateHideEditorDataLayersFilter));
	HideEditorDataLayersInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bHideEditorDataLayers = bHideEditorDataLayers = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowEditorDataLayers(!bIsActive);
		}
	});
	FilterInfoMap.Add(TEXT("HideEditorDataLayersFilter"), HideEditorDataLayersInfo);

	FSceneOutlinerFilterInfo HideRuntimeDataLayersInfo(LOCTEXT("ToggleHideRuntimeDataLayers", "Hide Runtime Data Layers"), LOCTEXT("ToggleHideRuntimeDataLayersToolTip", "When enabled, hides Runtime Data Layers."), bHideRuntimeDataLayers, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateHideRuntimeDataLayersFilter));
	HideRuntimeDataLayersInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bHideRuntimeDataLayers = bHideRuntimeDataLayers = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowRuntimeDataLayers(!bIsActive);
		}
	});
	FilterInfoMap.Add(TEXT("HideRuntimeDataLayersFilter"), HideRuntimeDataLayersInfo);

	FSceneOutlinerFilterInfo HideDataLayerActorsInfo(LOCTEXT("ToggleHideDataLayerActors", "Hide Actors"), LOCTEXT("ToggleHideDataLayerActorsToolTip", "When enabled, hides Data Layer Actors."), bHideDataLayerActors, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateHideDataLayerActorsFilter));
	HideDataLayerActorsInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bHideDataLayerActors = bHideDataLayerActors = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowDataLayerActors(!bIsActive);
		}
	});
	FilterInfoMap.Add(TEXT("HideDataLayerActorsFilter"), HideDataLayerActorsInfo);

	FSceneOutlinerFilterInfo HideUnloadedActorsInfo(LOCTEXT("ToggleHideUnloadedActors", "Hide Unloaded Actors"), LOCTEXT("ToggleHideUnloadedActorsToolTip", "When enabled, hides all unloaded world partition actors."), bHideUnloadedActors, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateHideUnloadedActorsFilter));
	HideUnloadedActorsInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bHideUnloadedActors = bHideUnloadedActors = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowUnloadedActors(!bIsActive);
		}
	});
	FilterInfoMap.Add(TEXT("HideUnloadedActorsFilter"), HideUnloadedActorsInfo);

	SceneOutliner->AddFilter(MakeShared<TSceneOutlinerPredicateFilter<FDataLayerActorTreeItem>>(FDataLayerActorTreeItem::FFilterPredicate::CreateLambda([this](const AActor* Actor, const UDataLayerInstance* DataLayer)
	{
		return FActorMode::IsActorDisplayable(SceneOutliner, Actor);
	}), FSceneOutlinerFilter::EDefaultBehaviour::Pass));

	FSceneOutlinerFilterInfo HideLevelInstancesInfo(LOCTEXT("ToggleHideLevelInstanceContent", "Hide Level Instance Content"), LOCTEXT("ToggleHideLevelInstanceContentToolTip", "When enabled, hides all level instance content."), bHideLevelInstanceContent, FCreateSceneOutlinerFilter::CreateStatic(&FDataLayerMode::CreateHideLevelInstancesFilter));
	HideLevelInstancesInfo.OnToggle().AddLambda([this](bool bIsActive)
	{
		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->bHideLevelInstanceContent = bHideLevelInstanceContent = bIsActive;
		Settings->PostEditChange();

		if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
		{
			DataLayerHierarchy->SetShowLevelInstanceContent(!bIsActive);
		}
	});
	FilterInfoMap.Add(TEXT("HideLevelInstancesFilter"), HideLevelInstancesInfo);

	// Add a filter which sets the interactive mode of LevelInstance items and their children
	SceneOutliner->AddFilter(MakeShared<FDataLayerActorFilter>(FDataLayerActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* Actor, const UDataLayerInstance* DataLayer) {return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass, FDataLayerActorTreeItem::FFilterPredicate::CreateLambda([this](const AActor* Actor, const UDataLayerInstance* DataLayer)
	{
		if (!bHideLevelInstanceContent)
		{
			if (const ULevelInstanceSubsystem* LevelInstanceSubsystem = UWorld::GetSubsystem<ULevelInstanceSubsystem>(RepresentingWorld.Get()))
			{
				const ILevelInstanceInterface* ActorAsLevelInstance = Cast<ILevelInstanceInterface>(Actor);
				const ILevelInstanceInterface* ActorParentLevelInstance = LevelInstanceSubsystem->GetParentLevelInstance(Actor);
				if (!LevelInstanceSubsystem->IsEditingLevelInstance(ActorAsLevelInstance) &&
					!LevelInstanceSubsystem->IsEditingLevelInstance(ActorParentLevelInstance))
				{
					return false;
				}
			}
		}
		return true;
	})));

	DataLayerEditorSubsystem = UDataLayerEditorSubsystem::Get();
	Rebuild();
	const_cast<FSharedSceneOutlinerData&>(SceneOutliner->GetSharedData()).CustomDelete = FCustomSceneOutlinerDeleteDelegate::CreateRaw(this, &FDataLayerMode::DeleteItems);
}

FDataLayerMode::~FDataLayerMode()
{
	UnregisterContextMenu();

	USelection::SelectionChangedEvent.RemoveAll(this);
	USelection::SelectObjectEvent.RemoveAll(this);
}

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateHideEditorDataLayersFilter()
{
	return MakeShareable(new FDataLayerFilter(FDataLayerTreeItem::FFilterPredicate::CreateStatic([](const UDataLayerInstance* DataLayerInstance) { return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateHideRuntimeDataLayersFilter()
{
	return MakeShareable(new FDataLayerFilter(FDataLayerTreeItem::FFilterPredicate::CreateStatic([](const UDataLayerInstance* DataLayerInstance) { return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateHideDataLayerActorsFilter()
{
	return MakeShareable(new FDataLayerActorFilter(FDataLayerActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* Actor, const UDataLayerInstance* DataLayerInstance) { return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateHideUnloadedActorsFilter()
{
	return MakeShareable(new FActorDescFilter(FActorDescTreeItem::FFilterPredicate::CreateStatic([](const FWorldPartitionActorDesc* ActorDesc) { return true; }), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateHideLevelInstancesFilter()
{
	return MakeShareable(new FDataLayerActorFilter(FDataLayerActorTreeItem::FFilterPredicate::CreateStatic([](const AActor* Actor, const UDataLayerInstance* DataLayerInstance)
	{
		// Check if actor belongs to a LevelInstance
		if (const ULevelInstanceSubsystem* LevelInstanceSubsystem = Actor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			if (const ILevelInstanceInterface* ParentLevelInstance = LevelInstanceSubsystem->GetParentLevelInstance(Actor))
			{
				if (!LevelInstanceSubsystem->IsEditingLevelInstance(ParentLevelInstance))
				{
					return false;
				}
			}
		}
		// Or if the actor itself is a LevelInstance editor instance
		return !Actor->IsA<ALevelInstanceEditorInstanceActor>();
	}), FSceneOutlinerFilter::EDefaultBehaviour::Pass));
}

int32 FDataLayerMode::GetTypeSortPriority(const ISceneOutlinerTreeItem& Item) const
{
	if (Item.IsA<FWorldDataLayersTreeItem>())
	{
		FWorldDataLayersTreeItem* WorldDataLayersTreeItem = (FWorldDataLayersTreeItem*)&Item;
		return static_cast<int32>(EItemSortOrder::WorldDataLayers) + WorldDataLayersTreeItem->GetSortPriority();
	}
	else if (Item.IsA<FDataLayerTreeItem>())
	{
		return static_cast<int32>(EItemSortOrder::DataLayer);
	}
	else if (Item.IsA<FDataLayerActorTreeItem>())
	{
		return static_cast<int32>(EItemSortOrder::Actor);
	}
	else if (Item.IsA<FDataLayerActorDescTreeItem>())
	{
		return static_cast<int32_t>(EItemSortOrder::Unloaded);
	}
	// Warning: using actor mode with an unsupported item type!
	check(false);
	return -1;
}

bool FDataLayerMode::CanRenameItem(const ISceneOutlinerTreeItem& Item) const 
{
	if (Item.IsValid() && (Item.IsA<FDataLayerTreeItem>()))
	{
		FDataLayerTreeItem* DataLayerTreeItem = (FDataLayerTreeItem*)&Item;
		return !DataLayerTreeItem->GetDataLayer()->IsLocked() && DataLayerTreeItem->GetDataLayer()->SupportRelabeling();
	}
	return false;
}

FText FDataLayerMode::GetStatusText() const
{
	// The number of DataLayers in the outliner before applying the text filter
	const int32 TotalDataLayerCount = ApplicableDataLayers.Num();
	const int32 SelectedDataLayerCount = SceneOutliner->GetSelection().Num<FDataLayerTreeItem>();

	if (!SceneOutliner->IsTextFilterActive())
	{
		if (SelectedDataLayerCount == 0)
		{
			return FText::Format(LOCTEXT("ShowingAllDataLayersFmt", "{0} data layers"), FText::AsNumber(FilteredDataLayerCount));
		}
		else
		{
			return FText::Format(LOCTEXT("ShowingAllDataLayersSelectedFmt", "{0} data layers ({1} selected)"), FText::AsNumber(FilteredDataLayerCount), FText::AsNumber(SelectedDataLayerCount));
		}
	}
	else if (SceneOutliner->IsTextFilterActive() && FilteredDataLayerCount == 0)
	{
		return FText::Format(LOCTEXT("ShowingNoDataLayersFmt", "No matching data layers ({0} total)"), FText::AsNumber(TotalDataLayerCount));
	}
	else if (SelectedDataLayerCount != 0)
	{
		return FText::Format(LOCTEXT("ShowingOnlySomeDataLayersSelectedFmt", "Showing {0} of {1} data layers ({2} selected)"), FText::AsNumber(FilteredDataLayerCount), FText::AsNumber(TotalDataLayerCount), FText::AsNumber(SelectedDataLayerCount));
	}
	else
	{
		return FText::Format(LOCTEXT("ShowingOnlySomeDataLayersFmt", "Showing {0} of {1} data layers"), FText::AsNumber(FilteredDataLayerCount), FText::AsNumber(TotalDataLayerCount));
	}
}

FFolder::FRootObject FDataLayerMode::GetRootObject() const
{
	return FFolder::GetWorldRootFolder(RepresentingWorld.Get()).GetRootObject();
}

SDataLayerBrowser* FDataLayerMode::GetDataLayerBrowser() const
{
	return DataLayerBrowser;
}

void FDataLayerMode::OnItemAdded(FSceneOutlinerTreeItemPtr Item)
{
	if (FDataLayerTreeItem* DataLayerItem = Item->CastTo<FDataLayerTreeItem>())
	{
		if (!Item->Flags.bIsFilteredOut)
		{
			++FilteredDataLayerCount;

			if (ShouldExpandDataLayer(DataLayerItem->GetDataLayer()))
			{
				SceneOutliner->SetItemExpansion(DataLayerItem->AsShared(), true);
			}

			if (SelectedDataLayersSet.Contains(DataLayerItem->GetDataLayer()))
			{
				SceneOutliner->AddToSelection({Item});
			}
		}
	}
	else if (const FDataLayerActorTreeItem* DataLayerActorTreeItem = Item->CastTo<FDataLayerActorTreeItem>())
	{
		if (SelectedDataLayerActors.Contains(FSelectedDataLayerActor(DataLayerActorTreeItem->GetDataLayer(), DataLayerActorTreeItem->GetActor())))
		{
			SceneOutliner->AddToSelection({Item});
		}
	}
}

void FDataLayerMode::OnItemRemoved(FSceneOutlinerTreeItemPtr Item)
{
	if (const FDataLayerTreeItem* DataLayerItem = Item->CastTo<FDataLayerTreeItem>())
	{
		if (!Item->Flags.bIsFilteredOut)
		{
			--FilteredDataLayerCount;
		}
	}
}

void FDataLayerMode::OnItemPassesFilters(const ISceneOutlinerTreeItem& Item)
{
	if (const FDataLayerTreeItem* const DataLayerItem = Item.CastTo<FDataLayerTreeItem>())
	{
		ApplicableDataLayers.Add(DataLayerItem->GetDataLayer());
	}
}

void FDataLayerMode::OnItemDoubleClick(FSceneOutlinerTreeItemPtr Item)
{
	if (const FDataLayerTreeItem* DataLayerItem = Item->CastTo<FDataLayerTreeItem>())
	{
		if (UDataLayerInstance* DataLayerInstance = DataLayerItem->GetDataLayer())
		{
			const FScopedTransaction Transaction(LOCTEXT("SelectActorsInDataLayer", "Select Actors in Data Layer"));
			GEditor->SelectNone(/*bNoteSelectionChange*/false, true);
			DataLayerEditorSubsystem->SelectActorsInDataLayer(DataLayerInstance, /*bSelect*/true, /*bNotify*/true, /*bSelectEvenIfHidden*/true);
		}
	}
	else if (FDataLayerActorTreeItem* DataLayerActorItem = Item->CastTo<FDataLayerActorTreeItem>())
	{
		if (AActor * Actor = DataLayerActorItem->GetActor())
		{
			const FScopedTransaction Transaction(LOCTEXT("ClickingOnActor", "Clicking on Actor in Data Layer"));
			GEditor->GetSelectedActors()->Modify();
			GEditor->SelectNone(/*bNoteSelectionChange*/false, true);
			GEditor->SelectActor(Actor, /*bSelected*/true, /*bNotify*/true, /*bSelectEvenIfHidden*/true);
			GEditor->NoteSelectionChange();
			GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly*/false);
		}
	}
}

void FDataLayerMode::DeleteItems(const TArray<TWeakPtr<ISceneOutlinerTreeItem>>& Items)
{
	TArray<UDataLayerInstance*> DataLayersToDelete;
	TMap<UDataLayerInstance*, TArray<AActor*>> ActorsToRemoveFromDataLayer;

	for (const TWeakPtr<ISceneOutlinerTreeItem>& Item : Items)
	{
		if (FDataLayerActorTreeItem* DataLayerActorItem = Item.Pin()->CastTo<FDataLayerActorTreeItem>())
		{
			UDataLayerInstance* DataLayerInstance = DataLayerActorItem->GetDataLayer();
			AActor* Actor = DataLayerActorItem->GetActor();
			if (DataLayerInstance && !DataLayerInstance->IsLocked() && Actor)
			{
				ActorsToRemoveFromDataLayer.FindOrAdd(DataLayerInstance).Add(Actor);
			}
		}
		else if (FDataLayerTreeItem* DataLayerItem = Item.Pin()->CastTo< FDataLayerTreeItem>())
		{
			if (UDataLayerInstance* DataLayerInstance = DataLayerItem->GetDataLayer())
			{
				if (!DataLayerInstance->IsLocked())
				{
					DataLayersToDelete.Add(DataLayerInstance);
				}
			}
		}
	}

	if (!ActorsToRemoveFromDataLayer.IsEmpty())
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveActorsFromDataLayer", "Remove Actors from Data Layer"));
		for (const auto& ItPair : ActorsToRemoveFromDataLayer)
		{
			DataLayerEditorSubsystem->RemoveActorsFromDataLayer(ItPair.Value, ItPair.Key);
		}
	}
	else if (!DataLayersToDelete.IsEmpty())
	{
		int32 PrevDeleteCount = SelectedDataLayersSet.Num();
		for (UDataLayerInstance* DataLayerToDelete : DataLayersToDelete)
		{
			SelectedDataLayersSet.Remove(DataLayerToDelete);
		}
		
		{
			const FScopedTransaction Transaction(LOCTEXT("DeleteDataLayers", "Delete Data Layers"));
			DataLayerEditorSubsystem->DeleteDataLayers(DataLayersToDelete);
		}

		if ((SelectedDataLayersSet.Num() != PrevDeleteCount) && DataLayerBrowser)
		{
			DataLayerBrowser->OnSelectionChanged(SelectedDataLayersSet);
		}
	}
}

FReply FDataLayerMode::OnKeyDown(const FKeyEvent& InKeyEvent)
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

	// Delete/BackSpace keys delete selected actors
	else if (InKeyEvent.GetKey() == EKeys::Delete || InKeyEvent.GetKey() == EKeys::BackSpace)
	{
		DeleteItems(Selection.SelectedItems);
		return FReply::Handled();

	}
	return FReply::Unhandled();
}

bool FDataLayerMode::ParseDragDrop(FSceneOutlinerDragDropPayload& OutPayload, const FDragDropOperation& Operation) const
{
	return !GetDataLayerActorPairsFromOperation(Operation).IsEmpty() || !GetActorsFromOperation(Operation, true).IsEmpty() || !GetDataLayersFromOperation(Operation, true).IsEmpty();
}

FSceneOutlinerDragValidationInfo FDataLayerMode::ValidateDrop(const ISceneOutlinerTreeItem& DropTarget, bool bMoveOperation /*= false*/) const
{
	if (const FDataLayerTreeItem* DataLayerItem = DropTarget.CastTo<FDataLayerTreeItem>())
	{
		const UDataLayerInstance* TargetDataLayer = DataLayerItem->GetDataLayer();
		if (!TargetDataLayer)
		{
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, FText());
		}

		if (TargetDataLayer->IsLocked())
		{
			const FText Text = bMoveOperation ? LOCTEXT("CantMoveActorsToLockedDataLayer", "Can't move actors to locked Data Layer") : LOCTEXT("CantAssignActorsToLockedDataLayer", "Can't assign actors to locked Data Layer");
			return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, Text);
		}

		if (!bMoveOperation)
		{
			if (GetSelectedDataLayers(SceneOutliner).Num() > 1)
			{
				if (SceneOutliner->GetTree().IsItemSelected(const_cast<ISceneOutlinerTreeItem&>(DropTarget).AsShared()))
				{
					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::Compatible, LOCTEXT("AssignToDataLayers", "Assign to Selected Data Layers"));
				}
			}
		}

		const FText Text = bMoveOperation ? FText::Format(LOCTEXT("MoveToDataLayer", "Move to Data Layer \"{0}\""), FText::FromString(TargetDataLayer->GetDataLayerShortName())) : FText::Format(LOCTEXT("AssignToDataLayer", "Assign to Data Layer \"{0}\""), FText::FromString(TargetDataLayer->GetDataLayerShortName()));
		return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::Compatible, Text);
	}

	return FSceneOutlinerDragValidationInfo::Invalid();
}

FSceneOutlinerDragValidationInfo FDataLayerMode::ValidateDrop(const ISceneOutlinerTreeItem& DropTarget, const FSceneOutlinerDragDropPayload& Payload) const
{
	TArray<AActor*> PayloadActors = GetActorsFromOperation(Payload.SourceOperation);
	if (!PayloadActors.IsEmpty()) // Adding actor(s) in data layer(s)
	{
		for (AActor* Actor : PayloadActors)
		{
			if (!DataLayerEditorSubsystem->IsActorValidForDataLayer(Actor))
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("ActorCantBeAssignedToDataLayer", "Can't assign actors to Data Layer"));
			}
		}

		return ValidateDrop(DropTarget);
	}
	else if (!GetDataLayerActorPairsFromOperation(Payload.SourceOperation).IsEmpty()) // Moving actor(s) into a Data Layer
	{
		return ValidateDrop(DropTarget, true);
	}
	else // Moving a data layer(s)
	{
		TArray<UDataLayerInstance*> PayloadDataLayers = GetDataLayersFromOperation(Payload.SourceOperation);
		if (!PayloadDataLayers.IsEmpty())
		{
			const FDataLayerTreeItem* DataLayerItem = DropTarget.CastTo<FDataLayerTreeItem>();
			const FDataLayerActorTreeItem* DataLayerActorTreeItem = DropTarget.CastTo<FDataLayerActorTreeItem>();
			const UDataLayerInstance* ParentDataLayer = DataLayerItem ? DataLayerItem->GetDataLayer() : nullptr;
			if (!ParentDataLayer && DataLayerActorTreeItem)
			{
				ParentDataLayer = DataLayerActorTreeItem->GetDataLayer();
			}

			for (UDataLayerInstance* DataLayerInstance : PayloadDataLayers)
			{
				if (DataLayerInstance->IsLocked())
				{
					return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("CantMoveLockedDataLayer", "Can't move locked Data Layer"));
				}

				if (ParentDataLayer != nullptr)
				{
					if (!DataLayerInstance->CanParent(ParentDataLayer))
					{
						if (!DataLayerInstance->IsDataLayerTypeValidToParent(ParentDataLayer->GetType()))
						{
							return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, 
								FText::Format(LOCTEXT("CantMoveToDataLayerDiffType", "Can't move a {0} Data Layer under a {1} Data Layer"), UEnum::GetDisplayValueAsText(DataLayerInstance->GetType()), UEnum::GetDisplayValueAsText(ParentDataLayer->GetType())));
						}

						if (ParentDataLayer == DataLayerInstance->GetParent() || ParentDataLayer == DataLayerInstance)
						{
							return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("CantMoveToSameDataLayer", "Can't move Data Layer to same Data Layer"));
						}
					}
				
					if (ParentDataLayer->IsLocked())
					{
						return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::IncompatibleGeneric, LOCTEXT("CantMoveDataLayerToLockedDataLayer", "Can't move Data Layer to locked Data Layer"));
					}
				}
			}

			if (ParentDataLayer != nullptr)
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::Compatible, FText::Format(LOCTEXT("MoveDataLayerToDataLayer", "Move to Data Layer \"{0}\""), FText::FromString(ParentDataLayer->GetDataLayerShortName())));
			}
			else
			{
				return FSceneOutlinerDragValidationInfo(ESceneOutlinerDropCompatibility::Compatible, LOCTEXT("MoveDataLayerToRoot", "Move to root"));
			}
		}
	}

	return FSceneOutlinerDragValidationInfo::Invalid();
}

TArray<UDataLayerInstance*> FDataLayerMode::GetDataLayersFromOperation(const FDragDropOperation& Operation, bool bOnlyFindFirst /*= false*/) const
{
	TArray<UDataLayerInstance*> OutDataLayers;
	
	auto GetDataLayers = [this, &OutDataLayers](const FDataLayerDragDropOp& DataLayerOp)
	{
		const TArray<FDataLayerDragDropOp::FDragDropInfo>& DragDropInfos = DataLayerOp.DataLayerDragDropInfos;
		for (const FDataLayerDragDropOp::FDragDropInfo& DragDropInfo : DragDropInfos)
		{
			if (UDataLayerInstance* DataLayerInstance = DataLayerEditorSubsystem->GetDataLayerInstance(DragDropInfo.DataLayerInstanceName))
			{
				OutDataLayers.AddUnique(DataLayerInstance);
			}
		}
	};

	if (Operation.IsOfType<FCompositeDragDropOp>())
	{
		const FCompositeDragDropOp& CompositeDragDropOp = StaticCast<const FCompositeDragDropOp&>(Operation);
		if (TSharedPtr<const FDataLayerDragDropOp> DataLayerDragDropOp = CompositeDragDropOp.GetSubOp<FDataLayerDragDropOp>())
		{
			GetDataLayers(*DataLayerDragDropOp);
		}
	}
	else if (Operation.IsOfType<FDataLayerDragDropOp>())
	{
		const FDataLayerDragDropOp& DataLayerDragDropOp = StaticCast<const FDataLayerDragDropOp&>(Operation);
		GetDataLayers(DataLayerDragDropOp);
	}

	return OutDataLayers;
}

TArray<FDataLayerActorMoveElement> FDataLayerMode::GetDataLayerActorPairsFromOperation(const FDragDropOperation& Operation) const
{
	TArray<FDataLayerActorMoveElement> Out;

	if (Operation.IsOfType<FDataLayerActorMoveOp>())
	{
		const FDataLayerActorMoveOp& DataLayerActorDragOp = StaticCast<const FDataLayerActorMoveOp&>(Operation);
		return DataLayerActorDragOp.DataLayerActorMoveElements;
	}

	return Out;
}

TArray<AActor*> FDataLayerMode::GetActorsFromOperation(const FDragDropOperation& Operation, bool bOnlyFindFirst /*= false*/ ) const
{
	TSet<AActor*> Actors;

	auto GetActorsFromFolderOperation = [&Actors, bOnlyFindFirst](const FFolderDragDropOp& FolderOp)
	{
		if (bOnlyFindFirst && Actors.Num())
		{
			return;
		}

		if (UWorld* World = FolderOp.World.Get())
		{
			TArray<TWeakObjectPtr<AActor>> ActorsToDrop;
			FActorFolders::GetWeakActorsFromFolders(*World, FolderOp.Folders, ActorsToDrop, FolderOp.RootObject);
			for (const auto& Actor : ActorsToDrop)
			{
				if (AActor* ActorPtr = Actor.Get())
				{
					Actors.Add(ActorPtr);
					if (bOnlyFindFirst)
					{
						break;
					}
				}
			}
		}
	};

	auto GetActorsFromActorOperation = [&Actors, bOnlyFindFirst](const FActorDragDropOp& ActorOp)
	{
		if (bOnlyFindFirst && Actors.Num())
		{
			return;
		}
		for (const auto& Actor : ActorOp.Actors)
		{
			if (AActor* ActorPtr = Actor.Get())
			{
				Actors.Add(ActorPtr);
				if (bOnlyFindFirst)
				{
					break;
				}
			}
		}
	};

	if (Operation.IsOfType<FActorDragDropOp>())
	{
		const FActorDragDropOp& ActorDragOp = StaticCast<const FActorDragDropOp&>(Operation);
		GetActorsFromActorOperation(ActorDragOp);
	}
	if (Operation.IsOfType<FFolderDragDropOp>())
	{
		const FFolderDragDropOp& FolderDragOp = StaticCast<const FFolderDragDropOp&>(Operation);
		GetActorsFromFolderOperation(FolderDragOp);
	}
	if (Operation.IsOfType<FCompositeDragDropOp>())
	{
		const FCompositeDragDropOp& CompositeDragOp = StaticCast<const FCompositeDragDropOp&>(Operation);
		if (TSharedPtr<const FActorDragDropOp> ActorSubOp = CompositeDragOp.GetSubOp<FActorDragDropOp>())
		{
			GetActorsFromActorOperation(*ActorSubOp);
		}
		if (TSharedPtr<const FFolderDragDropOp> FolderSubOp = CompositeDragOp.GetSubOp<FFolderDragDropOp>())
		{
			GetActorsFromFolderOperation(*FolderSubOp);
		}
	}
	return Actors.Array();
}

void FDataLayerMode::OnDrop(ISceneOutlinerTreeItem& DropTarget, const FSceneOutlinerDragDropPayload& Payload, const FSceneOutlinerDragValidationInfo& ValidationInfo) const
{
	const FDataLayerTreeItem* DataLayerItem = DropTarget.CastTo<FDataLayerTreeItem>();
	UDataLayerInstance* TargetDataLayer = DataLayerItem ? DataLayerItem->GetDataLayer() : nullptr;

	TArray<AActor*> ActorsToAdd = GetActorsFromOperation(Payload.SourceOperation);
	if (!ActorsToAdd.IsEmpty()) // Adding actor(s) in data layer(s)
	{
		if (SceneOutliner->GetTree().IsItemSelected(const_cast<ISceneOutlinerTreeItem&>(DropTarget).AsShared()))
		{
			TArray<UDataLayerInstance*> AllSelectedDataLayers = GetSelectedDataLayers(SceneOutliner);
			if (AllSelectedDataLayers.Num() > 1)
			{
				const FScopedTransaction Transaction(LOCTEXT("DataLayerOutlinerAddActorsToDataLayers", "Add Actors to Data Layers"));
				DataLayerEditorSubsystem->AddActorsToDataLayers(ActorsToAdd, AllSelectedDataLayers);
				return;
			}
		}

		if (TargetDataLayer)
		{
			const FScopedTransaction Transaction(LOCTEXT("DataLayerOutlinerAddActorsToDataLayer", "Add Actors to Data Layer"));
			DataLayerEditorSubsystem->AddActorsToDataLayer(ActorsToAdd, TargetDataLayer);
		}
	}
	else if (Payload.SourceOperation.IsOfType<FDataLayerActorMoveOp>()) // Moving actor(s) into a Data Layer
	{
		TArray<FDataLayerActorMoveElement> ActorsToMove = GetDataLayerActorPairsFromOperation(Payload.SourceOperation);
		if (!ActorsToMove.IsEmpty() && TargetDataLayer)
		{
			const FScopedTransaction Transaction(LOCTEXT("DataLayerOutlinerMoveActorsToDataLayer", "Move Actors to Data Layer"));
			for (const auto& DataLayerActorPair : ActorsToMove)
			{
				if (AActor* ActorPtr = DataLayerActorPair.Key.Get())
				{
					DataLayerEditorSubsystem->AddActorToDataLayer(ActorPtr, TargetDataLayer);
					DataLayerEditorSubsystem->RemoveActorFromDataLayer(ActorPtr, DataLayerActorPair.Value.Get());
				}
			}
		}

		return;
	}
	else // Moving a data layer(s)
	{
		TArray<UDataLayerInstance*> DataLayerInstances = GetDataLayersFromOperation(Payload.SourceOperation);
		SetParentDataLayer(DataLayerInstances, TargetDataLayer);
	}
}

FReply FDataLayerMode::OnDragOverItem(const FDragDropEvent& Event, const ISceneOutlinerTreeItem& Item) const
{
	TSharedPtr<FDragDropOperation> DragOperation = Event.GetOperation();
	
	if (!DragOperation.IsValid())
	{
		return FReply::Handled();
	}

	if (DragOperation->IsOfType<FDataLayerActorMoveOp>()) // Moving actor(s) into a Data Layer
	{
		DragOperation->SetCursorOverride(EMouseCursor::GrabHandClosed);
	}
	else if (DragOperation->IsOfType<FActorDragDropOp>()) // Adding actor(s) in data layer(s)
	{
		DragOperation->SetCursorOverride(EMouseCursor::Default);
	}

	return FReply::Handled();
}

void FDataLayerMode::SetParentDataLayer(const TArray<UDataLayerInstance*> DataLayerInstances, UDataLayerInstance* ParentDataLayer) const
{
	if (!DataLayerInstances.IsEmpty())
	{
		TArray<UDataLayerInstance*> ValidDataLayers;
		ValidDataLayers.Reserve(DataLayerInstances.Num());
		for (UDataLayerInstance* DataLayerInstance : DataLayerInstances)
		{
			if (DataLayerInstance->CanParent(ParentDataLayer))
			{
				ValidDataLayers.Add(DataLayerInstance);
			}
		}

		if (!ValidDataLayers.IsEmpty())
		{
			const FScopedTransaction Transaction(LOCTEXT("DataLayerOutlinerChangeDataLayersParent", "Change Data Layers Parent"));
			for (UDataLayerInstance* DataLayerInstance : ValidDataLayers)
			{
				DataLayerEditorSubsystem->SetParentDataLayer(DataLayerInstance, ParentDataLayer);
			}
		}
	}
}

struct FWeakDataLayerActorSelector
{
	bool operator()(const TWeakPtr<ISceneOutlinerTreeItem>& Item, TWeakObjectPtr<AActor>& DataOut) const
	{
		if (TSharedPtr<ISceneOutlinerTreeItem> ItemPtr = Item.Pin())
		{
			if (FDataLayerActorTreeItem* TypedItem = ItemPtr->CastTo<FDataLayerActorTreeItem>())
			{
				if (TypedItem->IsValid())
				{
					DataOut = TypedItem->Actor;
					return true;
				}
			}
		}
		return false;
	}
};

struct FDataLayerActorPairSelector
{
	bool operator()(const TWeakPtr<ISceneOutlinerTreeItem>& Item, FDataLayerActorMoveElement& DataOut) const
	{
		if (TSharedPtr<ISceneOutlinerTreeItem> ItemPtr = Item.Pin())
		{
			if (FDataLayerActorTreeItem* TypedItem = ItemPtr->CastTo<FDataLayerActorTreeItem>())
			{
				if (TypedItem->IsValid())
				{
					DataOut = FDataLayerActorMoveElement(TypedItem->Actor.Get(), TypedItem->GetDataLayer());
					return true;
				}
			}
		}
		return false;
	}
};


struct FWeakDataLayerSelector
{
	bool operator()(const TWeakPtr<ISceneOutlinerTreeItem>& Item, TWeakObjectPtr<UDataLayerInstance>& DataOut) const
	{
		if (TSharedPtr<ISceneOutlinerTreeItem> ItemPtr = Item.Pin())
		{
			if (FDataLayerTreeItem* TypedItem = ItemPtr->CastTo<FDataLayerTreeItem>())
			{
				if (TypedItem->IsValid())
				{
					DataOut = TypedItem->GetDataLayer();
					return true;
				}
			}
		}
		return false;
	}
};

TSharedPtr<FDragDropOperation> FDataLayerMode::CreateDragDropOperation(const FPointerEvent& MouseEvent, const TArray<FSceneOutlinerTreeItemPtr>& InTreeItems) const
{
	FSceneOutlinerDragDropPayload DraggedObjects(InTreeItems);

	if (DraggedObjects.Has<FDataLayerTreeItem>())
	{
		TArray<TWeakObjectPtr<UDataLayerInstance>> DataLayerInstances = DraggedObjects.GetData<TWeakObjectPtr<UDataLayerInstance>>(FWeakDataLayerSelector());
		if (DataLayerInstances.FindByPredicate([&](const TWeakObjectPtr<UDataLayerInstance>& DataLayer) { return DataLayer.IsValid() && DataLayer->IsLocked(); }))
		{
			return TSharedPtr<FDragDropOperation>();
		}
	}

	auto GetDataLayerOperation = [&DraggedObjects]()
	{
		TSharedPtr<FDataLayerDragDropOp> DataLayerOperation = MakeShareable(new FDataLayerDragDropOp);
		TArray<TWeakObjectPtr<UDataLayerInstance>> DataLayers = DraggedObjects.GetData<TWeakObjectPtr<UDataLayerInstance>>(FWeakDataLayerSelector());
		for (const auto& DataLayerInstance : DataLayers)
		{
			if (DataLayerInstance.IsValid())
			{
				DataLayerOperation->DataLayerDragDropInfos.Emplace(DataLayerInstance.Get());
			}
		}
		DataLayerOperation->Construct();
		return DataLayerOperation;
	};

	auto GetActorOperation = [&DraggedObjects]()
	{
		TSharedPtr<FActorDragDropOp> ActorOperation = MakeShareable(new FActorDragDropOp);
		ActorOperation->Init(DraggedObjects.GetData<TWeakObjectPtr<AActor>>(FWeakDataLayerActorSelector()));
		ActorOperation->Construct();
		return ActorOperation;
	};

	auto GetActorDataLayerOperation = [&DraggedObjects]()
	{
		TSharedPtr<FDataLayerActorMoveOp> DataLayerActorOperation = MakeShareable(new FDataLayerActorMoveOp);
		DataLayerActorOperation->DataLayerActorMoveElements = DraggedObjects.GetData<FDataLayerActorMoveElement>(FDataLayerActorPairSelector());
		DataLayerActorOperation->Construct();
		return DataLayerActorOperation;
	};

	if (DraggedObjects.Has<FDataLayerTreeItem>() && !DraggedObjects.Has<FDataLayerActorTreeItem>())
	{
		return GetDataLayerOperation();
	}
	else if (!DraggedObjects.Has<FDataLayerTreeItem>() && DraggedObjects.Has<FDataLayerActorTreeItem>())
	{	
		if (MouseEvent.IsLeftAltDown())
		{
			return GetActorDataLayerOperation();
		}
		else
		{
			return GetActorOperation();
		}
	}
	else
	{
		TSharedPtr<FSceneOutlinerDragDropOp> OutlinerOp = MakeShareable(new FSceneOutlinerDragDropOp());

		if (DraggedObjects.Has<FDataLayerActorTreeItem>())
		{
			OutlinerOp->AddSubOp(GetActorOperation());
		}

		if (DraggedObjects.Has<FDataLayerTreeItem>())
		{
			OutlinerOp->AddSubOp(GetDataLayerOperation());
		}

		OutlinerOp->Construct();
		return OutlinerOp;
	}
}

static const FName DefaultContextBaseMenuName("DataLayerOutliner.DefaultContextMenuBase");
static const FName DefaultContextMenuName("DataLayerOutliner.DefaultContextMenu");

TArray<UDataLayerInstance*> FDataLayerMode::GetSelectedDataLayers(SSceneOutliner* InSceneOutliner) const
{
	FSceneOutlinerItemSelection ItemSelection(InSceneOutliner->GetSelection());
	TArray<FDataLayerTreeItem*> SelectedDataLayerItems;
	ItemSelection.Get<FDataLayerTreeItem>(SelectedDataLayerItems);
	TArray<UDataLayerInstance*> ValidSelectedDataLayers;
	Algo::TransformIf(SelectedDataLayerItems, ValidSelectedDataLayers, [](const auto Item) { return Item && Item->GetDataLayer(); }, [](const auto Item) { return Item->GetDataLayer(); });
	return MoveTemp(ValidSelectedDataLayers);
}

void FDataLayerMode::CreateDataLayerPicker(UToolMenu* InMenu, FOnDataLayerPicked OnDataLayerPicked, bool bInShowRoot /*= false*/)
{
	if (bInShowRoot)
	{
		FToolMenuSection& Section = InMenu->AddSection("DataLayers", LOCTEXT("DataLayers", "Data Layers"));
		Section.AddMenuEntry("Root", LOCTEXT("Root", "<Root>"), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([=]() { OnDataLayerPicked.ExecuteIfBound(nullptr); })));
	}
	
	FToolMenuSection& Section = InMenu->AddSection(FName(), LOCTEXT("ExistingDataLayers", "Existing Data Layers:"));
	TSharedRef<SWidget> DataLayerPickerWidget = FDataLayerPickingMode::CreateDataLayerPickerWidget(OnDataLayerPicked);
	Section.AddEntry(FToolMenuEntry::InitWidget("DataLayerPickerWidget", DataLayerPickerWidget, FText::GetEmpty(), false));
}

UWorld* FDataLayerMode::GetOwningWorld() const
{
	UWorld* World = RepresentingWorld.Get();
	return World ? World->PersistentLevel->GetWorld() : nullptr;
}

AWorldDataLayers* FDataLayerMode::GetOwningWorldAWorldDataLayers() const
{
	UWorld* OwningWorld = GetOwningWorld();
	return OwningWorld ? OwningWorld->GetWorldDataLayers() : nullptr;
}

void FDataLayerMode::RegisterContextMenu()
{
	UToolMenus* ToolMenus = UToolMenus::Get();

	if (!ToolMenus->IsMenuRegistered(DefaultContextBaseMenuName))
	{
		UToolMenu* Menu = ToolMenus->RegisterMenu(DefaultContextBaseMenuName);

		Menu->AddDynamicSection("DataLayerDynamicSection", FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InMenu)
		{
			USceneOutlinerMenuContext* Context = InMenu->FindContext<USceneOutlinerMenuContext>();
			if (!Context || !Context->SceneOutliner.IsValid())
			{
				return;
			}

			SSceneOutliner* SceneOutliner = Context->SceneOutliner.Pin().Get();
			TArray<UDataLayerInstance*> SelectedDataLayers = GetSelectedDataLayers(SceneOutliner);
			const bool bSelectedDataLayersContainsLocked = !!SelectedDataLayers.FindByPredicate([](const UDataLayerInstance* DataLayer) { return DataLayer->IsLocked(); });

			bool bHasActorEditorContextDataLayers = false;			
			TArray<const UDataLayerInstance*> AllDataLayers;
			if (const UDataLayerSubsystem* DataLayerSubsystem = UWorld::GetSubsystem<UDataLayerSubsystem>(GetOwningWorld()))
			{
				DataLayerSubsystem->ForEachDataLayer([&AllDataLayers](UDataLayerInstance* DataLayerInstance)
				{
					AllDataLayers.Add(DataLayerInstance);
					return true;
				});

				bHasActorEditorContextDataLayers = !DataLayerSubsystem->GetActorEditorContextDataLayers().IsEmpty();
			}

			{
				FToolMenuSection& Section = InMenu->AddSection("DataLayers", LOCTEXT("DataLayers", "Data Layers"));
				
				auto CreateNewDataLayer = [this, SceneOutliner](UDataLayerInstance* InParentDataLayer = nullptr, const TSet<const UDataLayerAsset*>& InDataLayerAssets = {})
				{
					TSet<const UDataLayerAsset*> DataLayerAssets = InDataLayerAssets;
					DataLayerAssets.Remove(nullptr);

					if (DataLayerAssets.IsEmpty())
					{
						if (UDataLayerAsset* DataLayerAsset = PromptDataLayerAssetSelection())
						{
							DataLayerAssets.Add(DataLayerAsset);
						}
					}

					for (const UDataLayerAsset* DataLayerAsset : DataLayerAssets)
					{
						const FScopedTransaction Transaction(LOCTEXT("CreateNewDataLayer", "Create New Data Layer"));
						SelectedDataLayersSet.Empty();
						SelectedDataLayerActors.Empty();

						FDataLayerCreationParameters CreationParams;
						CreationParams.DataLayerAsset = const_cast<UDataLayerAsset*>(DataLayerAsset);
						CreationParams.ParentDataLayer = InParentDataLayer;
						CreationParams.WorlDataLayers = InParentDataLayer ? InParentDataLayer->GetOuterAWorldDataLayers() : GetOwningWorldAWorldDataLayers();
						if (UDataLayerInstance* NewDataLayerInstance = DataLayerEditorSubsystem->CreateDataLayerInstance(CreationParams))
						{
							SelectedDataLayersSet.Add(NewDataLayerInstance);
							// Select it and open a rename when it gets refreshed
							SceneOutliner->OnItemAdded(NewDataLayerInstance, SceneOutliner::ENewItemAction::Select | SceneOutliner::ENewItemAction::Rename);
						}
					}
				};

				// If selection contains readonly DataLayerInstances
				if (Algo::AnyOf(SelectedDataLayersSet, [](const TWeakObjectPtr<const UDataLayerInstance>& DataLayerInstance) { return DataLayerInstance.IsValid() && DataLayerInstance->IsReadOnly(); }))
				{
					// Create a menu entry that allows to creating DataLayerInstance in the world using selection referenced DataLayer Assets
					TSet<const UDataLayerAsset*> ExistingDataLayerAssets;
					if (AWorldDataLayers* WorldDataLayer = GetOwningWorldAWorldDataLayers())
					{
						WorldDataLayer->ForEachDataLayer([&ExistingDataLayerAssets](UDataLayerInstance* DataLayerInstance)
						{
							UDataLayerInstanceWithAsset* DataLayerWithAsset = Cast<UDataLayerInstanceWithAsset>(DataLayerInstance);
							if (const UDataLayerAsset* DataLayerAsset = DataLayerWithAsset ? DataLayerWithAsset->GetAsset() : nullptr)
							{
								ExistingDataLayerAssets.Add(DataLayerAsset);
							}
							return true;
						});
					}

					TSet<const UDataLayerAsset*> DataLayerAssetsToImport;
					for (const TWeakObjectPtr<const UDataLayerInstance>& DataLayerInstance : SelectedDataLayersSet)
					{
						if (DataLayerInstance.IsValid() && DataLayerInstance->IsReadOnly())
						{
							const UDataLayerInstanceWithAsset* DataLayerWithAsset = Cast<const UDataLayerInstanceWithAsset>(DataLayerInstance);
							const UDataLayerAsset* DataLayerAsset = DataLayerWithAsset ? DataLayerWithAsset->GetAsset() : nullptr;
							if (DataLayerAsset && !ExistingDataLayerAssets.Contains(DataLayerAsset))
							{
								DataLayerAssetsToImport.Add(DataLayerAsset);
							}
						}
					}

					if (!DataLayerAssetsToImport.IsEmpty())
					{
						Section.AddMenuEntry("ImportDataLayers", LOCTEXT("ImportDataLayers", "Import Data Layer(s)"), FText(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([this, DataLayerAssetsToImport, CreateNewDataLayer]()
								{
									const FScopedTransaction Transaction(LOCTEXT("ImportDataLayersTransaction", "Import Data Layers"));
									CreateNewDataLayer(nullptr, DataLayerAssetsToImport);
								})));
					}

					// Readonly selected Data Layer Instances will only show the option to import Data Layers
					return;
				}
				
				Section.AddMenuEntry("CreateNewDataLayer", LOCTEXT("CreateNewDataLayer", "Create New Data Layer"), FText(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, CreateNewDataLayer]() { CreateNewDataLayer(); })));

				UDataLayerInstance* ParentDataLayer = (SelectedDataLayersSet.Num() == 1) ? const_cast<UDataLayerInstance*>(SelectedDataLayersSet.CreateIterator()->Get()) : nullptr;
				if (ParentDataLayer)
				{
					Section.AddMenuEntry("CreateNewDataLayerUnderDataLayer", FText::Format(LOCTEXT("CreateNewDataLayerUnderDataLayer", "Create New Data Layer under \"{0}\""), FText::FromString(ParentDataLayer->GetDataLayerShortName())), FText(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, CreateNewDataLayer, ParentDataLayer]() { CreateNewDataLayer(ParentDataLayer); })));
				}

				Section.AddMenuEntry("AddSelectedActorsToNewDataLayer", LOCTEXT("AddSelectedActorsToNewDataLayer", "Add Selected Actors to New Data Layer"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this]() {
							if (UDataLayerAsset* DataLayerAsset = PromptDataLayerAssetSelection())
							{
								const FScopedTransaction Transaction(LOCTEXT("AddSelectedActorsToNewDataLayer", "Add Selected Actors to New Data Layer"));
								FDataLayerCreationParameters CreationParams;
								CreationParams.DataLayerAsset = DataLayerAsset;
								CreationParams.WorlDataLayers = GetOwningWorldAWorldDataLayers();
								if (UDataLayerInstance* NewDataLayerInstance = DataLayerEditorSubsystem->CreateDataLayerInstance(CreationParams))
								{
									DataLayerEditorSubsystem->AddSelectedActorsToDataLayer(NewDataLayerInstance);
								}
							}}),
						FCanExecuteAction::CreateLambda([] { return GEditor->GetSelectedActorCount() > 0; })
					));

				Section.AddMenuEntry("AddSelectedActorsToSelectedDataLayers", LOCTEXT("AddSelectedActorsToSelectedDataLayersMenu", "Add Selected Actors to Selected Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("AddSelectedActorsToSelectedDataLayers", "Add Selected Actors to Selected Data Layers"));
								DataLayerEditorSubsystem->AddSelectedActorsToDataLayers(SelectedDataLayers);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers,bSelectedDataLayersContainsLocked] { return !SelectedDataLayers.IsEmpty() && GEditor->GetSelectedActorCount() > 0 && !bSelectedDataLayersContainsLocked; })
					));

				if (!SelectedDataLayerActors.IsEmpty())
				{
					Section.AddSubMenu("AddSelectedActorsTo", LOCTEXT("AddSelectedActorsTo", "Add Selected Actors To"), FText(),
						FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InSubMenu)
						{
							CreateDataLayerPicker(InSubMenu, FOnDataLayerPicked::CreateLambda([this](UDataLayerInstance* TargetDataLayer)
							{
								check(TargetDataLayer);
								TArray<AActor*> Actors;
								Algo::TransformIf(SelectedDataLayerActors, Actors, [](const FSelectedDataLayerActor& InActor) { return InActor.Value.Get(); }, [](const FSelectedDataLayerActor& InActor) { return const_cast<AActor*>(InActor.Value.Get()); });
								if (!Actors.IsEmpty())
								{
									const FScopedTransaction Transaction(LOCTEXT("AddSelectedActorsToDataLayer", "Add Selected Actors to Selected Data Layer"));
									DataLayerEditorSubsystem->AddActorsToDataLayers(Actors, { TargetDataLayer });
								}
							}));
						}));
				}
				if (!SelectedDataLayers.IsEmpty() && !bSelectedDataLayersContainsLocked)
				{
					Section.AddSubMenu("MoveSelectedDataLayersTo", LOCTEXT("MoveSelectedDataLayersTo", "Move Data Layers To"), FText(),
						FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InSubMenu)
						{
							CreateDataLayerPicker(InSubMenu, FOnDataLayerPicked::CreateLambda([this](UDataLayerInstance* TargetDataLayer)
							{
								TArray<UDataLayerInstance*> DataLayers;
								for (TWeakObjectPtr<const UDataLayerInstance>& DataLayer : SelectedDataLayersSet)
								{
									if (DataLayer.IsValid() && !DataLayer->IsLocked() && DataLayer != TargetDataLayer)
									{
										DataLayers.Add(const_cast<UDataLayerInstance*>(DataLayer.Get()));
									}
								}
								SetParentDataLayer(DataLayers, TargetDataLayer);
							}), /*bShowRoot*/true);
						}));
				}

				Section.AddSeparator("SectionsSeparator");

				Section.AddMenuEntry("RemoveSelectedActorsFromSelectedDataLayers", LOCTEXT("RemoveSelectedActorsFromSelectedDataLayersMenu", "Remove Selected Actors from Selected Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("RemoveSelectedActorsFromSelectedDataLayers_DataLayerMode", "Remove Selected Actors from Selected Data Layers"));
								DataLayerEditorSubsystem->RemoveSelectedActorsFromDataLayers(SelectedDataLayers);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers,bSelectedDataLayersContainsLocked] { return !SelectedDataLayers.IsEmpty() && GEditor->GetSelectedActorCount() > 0 && !bSelectedDataLayersContainsLocked; })
					));

				Section.AddMenuEntry("DeleteSelectedDataLayers", LOCTEXT("DeleteSelectedDataLayers", "Delete Selected Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("DeleteSelectedDataLayers", "Delete Selected Data Layers"));
								DataLayerEditorSubsystem->DeleteDataLayers(SelectedDataLayers);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers,bSelectedDataLayersContainsLocked] { return !SelectedDataLayers.IsEmpty() && !bSelectedDataLayersContainsLocked; })
					));

				Section.AddMenuEntry("RenameSelectedDataLayer", LOCTEXT("RenameSelectedDataLayer", "Rename Selected Data Layer"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SceneOutliner,SelectedDataLayers]() {
							if (SelectedDataLayers.Num() == 1)
							{
								FSceneOutlinerTreeItemPtr ItemToRename = SceneOutliner->GetTreeItem(SelectedDataLayers[0]);
								if (ItemToRename.IsValid() && CanRenameItem(*ItemToRename) && ItemToRename->CanInteract())
								{
									SceneOutliner->SetPendingRenameItem(ItemToRename);
									SceneOutliner->ScrollItemIntoView(ItemToRename);
								}
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers] { return (SelectedDataLayers.Num() == 1) && !SelectedDataLayers[0]->IsLocked(); })
					));

				Section.AddSeparator("SectionsSeparator");
			}

			{
				FToolMenuSection& Section = InMenu->AddSection("DataLayerSelection", LOCTEXT("DataLayerSelection", "Selection"));

				Section.AddMenuEntry("SelectActorsInDataLayers", LOCTEXT("SelectActorsInDataLayers", "Select Actors in Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("SelectActorsInDataLayers", "Select Actors in Data Layers"));
								GEditor->SelectNone(/*bNoteSelectionChange*/false, /*bDeselectBSPSurfs*/true);
								DataLayerEditorSubsystem->SelectActorsInDataLayers(SelectedDataLayers, /*bSelect*/true, /*bNotify*/true, /*bSelectEvenIfHidden*/true);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers] { return !SelectedDataLayers.IsEmpty(); })
					));

				Section.AddMenuEntry("AppendActorsToSelection", LOCTEXT("AppendActorsToSelection", "Append Actors in Data Layer to Selection"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("AppendActorsToSelection", "Append Actors in Data Layer to Selection"));
								DataLayerEditorSubsystem->SelectActorsInDataLayers(SelectedDataLayers, /*bSelect*/true, /*bNotify*/true, /*bSelectEvenIfHidden*/true);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers,bSelectedDataLayersContainsLocked] { return !SelectedDataLayers.IsEmpty() && !bSelectedDataLayersContainsLocked; })
					));

				Section.AddMenuEntry("DeselectActors", LOCTEXT("DeselectActors", "Deselect Actors in Data Layer"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("DeselectActors", "Deselect Actors in Data Layer"));
								DataLayerEditorSubsystem->SelectActorsInDataLayers(SelectedDataLayers, /*bSelect*/false, /*bNotifySelectActors*/true);
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers] { return !SelectedDataLayers.IsEmpty(); })
					));
			}

			{
				FToolMenuSection& Section = InMenu->AddSection("DataLayerExpansion", LOCTEXT("DataLayerExpansion", "Expansion"));

				Section.AddMenuEntry("CollapseAllDataLayers", LOCTEXT("CollapseAllDataLayers", "Collapse All Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([SceneOutliner,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								GEditor->SelectNone(/*bNoteSelectionChange*/false, /*bDeselectBSPSurfs*/true);
								SceneOutliner->CollapseAll();
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers] { return !SelectedDataLayers.IsEmpty(); })
					));

				Section.AddMenuEntry("ExpandAllDataLayers", LOCTEXT("ExpandAllDataLayers", "Expand All Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([SceneOutliner,SelectedDataLayers]() {
							check(!SelectedDataLayers.IsEmpty());
							{
								GEditor->SelectNone(/*bNoteSelectionChange*/false, /*bDeselectBSPSurfs*/true);
								SceneOutliner->ExpandAll();
							}}),
						FCanExecuteAction::CreateLambda([SelectedDataLayers] { return !SelectedDataLayers.IsEmpty(); })
					));
			}

			{
				FToolMenuSection& Section = InMenu->AddSection("DataLayerActorEditorContext", LOCTEXT("DataLayerActorEditorContext", "Actor Editor Context"));

				Section.AddMenuEntry("MakeCurrentDataLayers", LOCTEXT("MakeCurrentDataLayers", "Make Current Data Layer(s)"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this, SceneOutliner, SelectedDataLayers]() 
						{
							const FScopedTransaction Transaction(LOCTEXT("MakeCurrentDataLayers", "Make Current Data Layer(s)"));
							for (TWeakObjectPtr<const UDataLayerInstance>& DataLayer : SelectedDataLayersSet)
							{
								if (DataLayer.IsValid() && !DataLayer->IsLocked())
								{
									DataLayerEditorSubsystem->AddToActorEditorContext(const_cast<UDataLayerInstance*>(DataLayer.Get()));
								}
							}
						}),
						FCanExecuteAction::CreateLambda([this] { return Algo::AnyOf(SelectedDataLayersSet, [](const TWeakObjectPtr<const UDataLayerInstance>& DataLayer) { return DataLayer.IsValid() && !DataLayer->IsLocked() && !DataLayer->IsInActorEditorContext(); }); })
					));

				Section.AddMenuEntry("RemoveCurrentDataLayers", LOCTEXT("RemoveCurrentDataLayers", "Remove Current Data Layer(s)"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this, SceneOutliner, SelectedDataLayers]()
						{
							const FScopedTransaction Transaction(LOCTEXT("RemoveCurrentDataLayers", "Remove Current Data Layer(s)"));
							for (TWeakObjectPtr<const UDataLayerInstance>& DataLayer : SelectedDataLayersSet)
							{
								if (DataLayer.IsValid() && !DataLayer->IsLocked())
								{
									DataLayerEditorSubsystem->RemoveFromActorEditorContext(const_cast<UDataLayerInstance*>(DataLayer.Get()));
								}
							}
						}),
						FCanExecuteAction::CreateLambda([this] { return Algo::AnyOf(SelectedDataLayersSet, [](const TWeakObjectPtr<const UDataLayerInstance>& DataLayer) { return DataLayer.IsValid() && !DataLayer->IsLocked() && DataLayer->IsInActorEditorContext(); }); })
					));

				Section.AddMenuEntry("ClearCurrentDataLayers", LOCTEXT("ClearCurrentDataLayers", "Clear Current Data Layers"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this, AllDataLayers]() 
						{
							check(!AllDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("ClearCurrentDataLayers", "Clear Current Data Layers"));
								for (const UDataLayerInstance* DataLayer : AllDataLayers)
								{
									DataLayerEditorSubsystem->RemoveFromActorEditorContext(const_cast<UDataLayerInstance*>(DataLayer));
								}
							}}),
						FCanExecuteAction::CreateLambda([bHasActorEditorContextDataLayers] { return bHasActorEditorContextDataLayers; })
					));
			}

			{
				FToolMenuSection& Section = InMenu->AddSection("DataLayerVisibility", LOCTEXT("DataLayerVisibility", "Visibility"));

				Section.AddMenuEntry("MakeAllDataLayersVisible", LOCTEXT("MakeAllDataLayersVisible", "Make All Data Layers Visible"), FText(), FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([this,AllDataLayers]() {
							check(!AllDataLayers.IsEmpty());
							{
								const FScopedTransaction Transaction(LOCTEXT("MakeAllDataLayersVisible", "Make All Data Layers Visible"));
								DataLayerEditorSubsystem->MakeAllDataLayersVisible();
							}}),
						FCanExecuteAction::CreateLambda([AllDataLayers] { return !AllDataLayers.IsEmpty(); })
					));
			}
		}));
	}

	if (!ToolMenus->IsMenuRegistered(DefaultContextMenuName))
	{
		ToolMenus->RegisterMenu(DefaultContextMenuName, DefaultContextBaseMenuName);
	}
}

void FDataLayerMode::UnregisterContextMenu()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	ToolMenus->RemoveMenu(DefaultContextBaseMenuName);
	ToolMenus->RemoveMenu(DefaultContextMenuName);
}

TSharedPtr<SWidget> FDataLayerMode::CreateContextMenu()
{
	RegisterContextMenu();

	FSceneOutlinerItemSelection ItemSelection(SceneOutliner->GetSelection());

	USceneOutlinerMenuContext* ContextObject = NewObject<USceneOutlinerMenuContext>();
	ContextObject->SceneOutliner = StaticCastSharedRef<SSceneOutliner>(SceneOutliner->AsShared());
	ContextObject->bShowParentTree = SceneOutliner->GetSharedData().bShowParentTree;
	ContextObject->NumSelectedItems = ItemSelection.Num();
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

void FDataLayerMode::CreateViewContent(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ToggleHighlightSelectedDataLayers", "Highlight Selected"),
		LOCTEXT("ToggleHighlightSelectedDataLayersToolTip", "When enabled, highlights Data Layers containing actors that are currently selected."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([this]()
			{
				UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
				bHighlightSelectedDataLayers = !bHighlightSelectedDataLayers;
				Settings->bHighlightSelectedDataLayers = bHighlightSelectedDataLayers;
				Settings->PostEditChange();

				if (auto DataLayerHierarchy = StaticCast<FDataLayerHierarchy*>(Hierarchy.Get()))
				{
					DataLayerHierarchy->SetHighlightSelectedDataLayers(bHighlightSelectedDataLayers);
				}

				SceneOutliner->FullRefresh();
			}), 
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this]() { return bHighlightSelectedDataLayers; })
		),
		NAME_None,
		EUserInterfaceActionType::ToggleButton
	);

	MenuBuilder.BeginSection("AssetThumbnails", LOCTEXT("ShowAdvancedHeading", "Advanced"));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ToggleAllowRuntimeDataLayerEditing", "Allow Runtime Data Layer Editing"), 
		LOCTEXT("ToggleAllowRuntimeDataLayerEditingToolTip", "When enabled, allows editing of Runtime Data Layers."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([this]()
			{
				if (AWorldDataLayers* WorldDataLayers = RepresentingWorld.IsValid() ? RepresentingWorld->GetWorldDataLayers() : nullptr)
				{
					const FScopedTransaction Transaction(LOCTEXT("ToggleAllowRuntimeDataLayerEditingTransaction", "Toggle Allow Runtime Data Layer Editing"));
					WorldDataLayers->SetAllowRuntimeDataLayerEditing(!WorldDataLayers->GetAllowRuntimeDataLayerEditing());
				}
				SceneOutliner->FullRefresh();
			}),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this]()
			{
				const AWorldDataLayers* WorldDataLayers = RepresentingWorld.IsValid() ? RepresentingWorld->GetWorldDataLayers() : nullptr;
				return WorldDataLayers ? WorldDataLayers->GetAllowRuntimeDataLayerEditing() : true;
			})
		),
		NAME_None,
		EUserInterfaceActionType::ToggleButton
	);

	TArray<UDataLayerInstance*> AllDataLayers;
	if (const UDataLayerSubsystem* DataLayerSubsystem = UWorld::GetSubsystem<UDataLayerSubsystem>(GetOwningWorld()))
	{
		DataLayerSubsystem->ForEachDataLayer([&AllDataLayers](UDataLayerInstance* DataLayer)
		{
			AllDataLayers.Add(DataLayer);
			return true;
		});
	}

	MenuBuilder.AddMenuEntry(
		LOCTEXT("ResetDataLayerUserSettings", "Reset User Settings"),
		LOCTEXT("ResetDataLayerUserSettingsToolTip", "Resets Data Layers User Settings to their initial values."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([this]()
			{
				const FScopedTransaction Transaction(LOCTEXT("ResetDataLayerUserSettings", "Reset User Settings"));
				DataLayerEditorSubsystem->ResetUserSettings();
			})
		)
	);

	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("AssetThumbnails", LOCTEXT("ShowWorldHeading", "World"));
	MenuBuilder.AddSubMenu(
		LOCTEXT("ChooseWorldSubMenu", "Choose World"),
		LOCTEXT("ChooseWorldSubMenuToolTip", "Choose the world to display in the outliner."),
		FNewMenuDelegate::CreateRaw(this, &FDataLayerMode::BuildWorldPickerMenu)
	);
	MenuBuilder.EndSection();
}

void FDataLayerMode::BuildWorldPickerMenu(FMenuBuilder& MenuBuilder)
{
	MenuBuilder.BeginSection("Worlds", LOCTEXT("WorldsHeading", "Worlds"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("AutoWorld", "Auto"),
			LOCTEXT("AutoWorldToolTip", "Automatically pick the world to display based on context."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FDataLayerMode::OnSelectWorld, TWeakObjectPtr<UWorld>()),
				FCanExecuteAction(),
				FIsActionChecked::CreateRaw(this, &FDataLayerMode::IsWorldChecked, TWeakObjectPtr<UWorld>())
			),
			NAME_None,
			EUserInterfaceActionType::RadioButton
		);

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && (World->WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Editor))
			{
				MenuBuilder.AddMenuEntry(
					SceneOutliner::GetWorldDescription(World),
					LOCTEXT("ChooseWorldToolTip", "Display actors for this world."),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateRaw(this, &FDataLayerMode::OnSelectWorld, MakeWeakObjectPtr(World)),
						FCanExecuteAction(),
						FIsActionChecked::CreateRaw(this, &FDataLayerMode::IsWorldChecked, MakeWeakObjectPtr(World))
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);
			}
		}
	}
	MenuBuilder.EndSection();
}

void FDataLayerMode::OnSelectWorld(TWeakObjectPtr<UWorld> World)
{
	UserChosenWorld = World;
	SceneOutliner->FullRefresh();
}

bool FDataLayerMode::IsWorldChecked(TWeakObjectPtr<UWorld> World) const
{
	return (UserChosenWorld == World) || (World.IsExplicitlyNull() && !UserChosenWorld.IsValid());
}

TUniquePtr<ISceneOutlinerHierarchy> FDataLayerMode::CreateHierarchy()
{
	TUniquePtr<FDataLayerHierarchy> DataLayerHierarchy = FDataLayerHierarchy::Create(this, RepresentingWorld);
	DataLayerHierarchy->SetShowEditorDataLayers(!bHideEditorDataLayers);
	DataLayerHierarchy->SetShowRuntimeDataLayers(!bHideRuntimeDataLayers);
	DataLayerHierarchy->SetShowDataLayerActors(!bHideDataLayerActors);
	DataLayerHierarchy->SetShowUnloadedActors(!bHideUnloadedActors);
	DataLayerHierarchy->SetShowOnlySelectedActors(bShowOnlySelectedActors);
	DataLayerHierarchy->SetHighlightSelectedDataLayers(bHighlightSelectedDataLayers);
	DataLayerHierarchy->SetShowLevelInstanceContent(!bHideLevelInstanceContent);
	return DataLayerHierarchy;
}

void FDataLayerMode::OnItemSelectionChanged(FSceneOutlinerTreeItemPtr TreeItem, ESelectInfo::Type SelectionType, const FSceneOutlinerItemSelection& Selection)
{
	SelectedDataLayersSet.Empty();
	SelectedDataLayerActors.Empty();
	Selection.ForEachItem<FDataLayerTreeItem>([this](const FDataLayerTreeItem& Item) { SelectedDataLayersSet.Add(Item.GetDataLayer()); });
	Selection.ForEachItem<FDataLayerActorTreeItem>([this](const FDataLayerActorTreeItem& Item) { SelectedDataLayerActors.Add(FSelectedDataLayerActor(Item.GetDataLayer(), Item.GetActor())); });
	if (DataLayerBrowser)
	{
		DataLayerBrowser->OnSelectionChanged(SelectedDataLayersSet);
	}

	if (OnItemPicked.IsBound())
	{
		auto SelectedItems = SceneOutliner->GetSelectedItems();
		if (SelectedItems.Num() > 0)
		{
			auto FirstItem = SelectedItems[0];
			if (FirstItem->CanInteract())
			{
				OnItemPicked.ExecuteIfBound(FirstItem.ToSharedRef());
			}
		}
	}
}

void FDataLayerMode::Rebuild()
{
	FilteredDataLayerCount = 0;
	ApplicableDataLayers.Empty();
	ChooseRepresentingWorld();
	Hierarchy = CreateHierarchy();

	// Hide delete actor column when it's not necessary
	const bool bShowDeleteButtonColumn = !bHideDataLayerActors && RepresentingWorld.IsValid() && !RepresentingWorld->IsPlayInEditor();
	SceneOutliner->SetColumnVisibility(FDataLayerOutlinerDeleteButtonColumn::GetID(), bShowDeleteButtonColumn);

	if (DataLayerBrowser)
	{
		DataLayerBrowser->OnSelectionChanged(SelectedDataLayersSet);
	}
}

void FDataLayerMode::ChooseRepresentingWorld()
{
	// Select a world to represent

	RepresentingWorld = nullptr;

	// If a specified world was provided, represent it
	if (SpecifiedWorldToDisplay.IsValid())
	{
		RepresentingWorld = SpecifiedWorldToDisplay.Get();
	}

	// check if the user-chosen world is valid and in the editor contexts

	if (!RepresentingWorld.IsValid() && UserChosenWorld.IsValid())
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UserChosenWorld.Get() == Context.World())
			{
				RepresentingWorld = UserChosenWorld.Get();
				break;
			}
		}
	}

	// If the user did not manually select a world, try to pick the most suitable world context
	if (!RepresentingWorld.IsValid())
	{
		// ideally we want a PIE world that is standalone or the first client
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && Context.WorldType == EWorldType::PIE)
			{
				if (World->GetNetMode() == NM_Standalone)
				{
					RepresentingWorld = World;
					break;
				}
				else if (World->GetNetMode() == NM_Client && Context.PIEInstance == 2)	// Slightly dangerous: assumes server is always PIEInstance = 1;
				{
					RepresentingWorld = World;
					break;
				}
			}
		}
	}

	if (RepresentingWorld == nullptr)
	{
		// still not world so fallback to old logic where we just prefer PIE over Editor

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE)
			{
				RepresentingWorld = Context.World();
				break;
			}
			else if (Context.WorldType == EWorldType::Editor)
			{
				RepresentingWorld = Context.World();
				if (RepresentingWorld.IsValid())
				{
					// Favor Current Level's outer world if exists
					ULevel* CurrentLevel = RepresentingWorld->GetCurrentLevel();
					if (CurrentLevel && !CurrentLevel->IsPersistentLevel() && CurrentLevel->GetWorldDataLayers())
					{
						RepresentingWorld = CurrentLevel->GetTypedOuter<UWorld>();
						check(RepresentingWorld.IsValid());
					}
				}
			}
		}
	}
}

bool FDataLayerMode::ShouldExpandDataLayer(const UDataLayerInstance* DataLayer) const
{
	if (bHighlightSelectedDataLayers || bShowOnlySelectedActors)
	{
		if (DataLayer)
		{
			if ((bShowOnlySelectedActors && DataLayerEditorSubsystem->DoesDataLayerContainSelectedActors(DataLayer)) ||
				(ContainsSelectedChildDataLayer(DataLayer) && !DataLayer->GetChildren().IsEmpty()))
			{
				return true;
			}
		}
	}
	return false;
}

bool FDataLayerMode::ContainsSelectedChildDataLayer(const UDataLayerInstance* DataLayer) const
{
	if (DataLayer)
	{
		bool bFoundSelected = false;
		DataLayer->ForEachChild([this, &bFoundSelected](const UDataLayerInstance* Child)
		{
			if (DataLayerEditorSubsystem->DoesDataLayerContainSelectedActors(Child) || ContainsSelectedChildDataLayer(Child))
			{
				bFoundSelected = true;
				return false;
			}
			return true;
		});
		return bFoundSelected;
	}
	return false;
};

TSharedRef<FSceneOutlinerFilter> FDataLayerMode::CreateShowOnlySelectedActorsFilter()
{
	auto IsActorSelected = [](const AActor* InActor, const UDataLayerInstance* InDataLayer)
	{
		return InActor && InActor->IsSelected();
	};
	return MakeShareable(new FDataLayerActorFilter(FDataLayerActorTreeItem::FFilterPredicate::CreateStatic(IsActorSelected), FSceneOutlinerFilter::EDefaultBehaviour::Pass, FDataLayerActorTreeItem::FFilterPredicate::CreateStatic(IsActorSelected)));
}

UDataLayerAsset* FDataLayerMode::PromptDataLayerAssetSelection()
{
	IContentBrowserSingleton& ContentBrowserSingleton = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();

	FOpenAssetDialogConfig Config;
	Config.bAllowMultipleSelection = false;
	Config.AssetClassNames.Add(UDataLayerAsset::StaticClass()->GetClassPathName());
	Config.DefaultPath = PickDataLayerDialogPath;
	Config.DialogTitleOverride = LOCTEXT("PickDataLayerAssetDialogTitle", "Pick a Data Layer Asset");

	TArray<FAssetData> Assets = ContentBrowserSingleton.CreateModalOpenAssetDialog(Config);
	if (Assets.Num() == 1)
	{
		Assets[0].PackagePath.ToString(PickDataLayerDialogPath);
		return CastChecked<UDataLayerAsset>(Assets[0].GetAsset());
	}

	return nullptr;
}

void FDataLayerMode::SynchronizeSelection()
{
	if (!bShowOnlySelectedActors && !bHighlightSelectedDataLayers)
	{
		return;
	}

	TArray<AActor*> Actors;
	TSet<const UDataLayerInstance*> ActorDataLayersIncludingParents;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
	for (const AActor* Actor : Actors)
	{
		TArray<const UDataLayerInstance*> ActorDataLayers = Actor->GetDataLayerInstances();
		for (const UDataLayerInstance* DataLayer : ActorDataLayers)
		{
			const UDataLayerInstance* CurrentDataLayer = DataLayer;
			while (CurrentDataLayer)
			{
				bool bIsAlreadyInSet = false;
				ActorDataLayersIncludingParents.Add(CurrentDataLayer, &bIsAlreadyInSet);
				if (!bIsAlreadyInSet)
				{
					FSceneOutlinerTreeItemPtr TreeItem = SceneOutliner->GetTreeItem(CurrentDataLayer, false);
					if (TreeItem && ShouldExpandDataLayer(CurrentDataLayer))
					{
						SceneOutliner->SetItemExpansion(TreeItem, true);
					}
				}
				CurrentDataLayer = CurrentDataLayer->GetParent();
			}
		}
	}
}

void FDataLayerMode::OnLevelSelectionChanged(UObject* Obj)
{
	if (!bShowOnlySelectedActors && !bHighlightSelectedDataLayers)
	{
		return;
	}

	RefreshSelection();
}

void FDataLayerMode::RefreshSelection()
{
	SceneOutliner->FullRefresh();
	SceneOutliner->RefreshSelection();
}

//
// FDataLayerPickingMode : Lightweight version of FDataLayerMode used to show the DataLayer hierarchy and choose one.
// 

FDataLayerPickingMode::FDataLayerPickingMode(const FDataLayerModeParams& Params) 
: FDataLayerMode(Params)
{
	bHideDataLayerActors = true;
	Rebuild();
	SceneOutliner->ExpandAll();
}

TSharedRef<SWidget> FDataLayerPickingMode::CreateDataLayerPickerWidget(FOnDataLayerPicked OnDataLayerPicked)
{
	// Create mini DataLayers outliner to pick a DataLayer
	FSceneOutlinerInitializationOptions InitOptions;
	InitOptions.bShowHeaderRow = false;
	InitOptions.bShowParentTree = true;
	InitOptions.bShowCreateNewFolder = false;
	InitOptions.bFocusSearchBoxWhenOpened = true;
	InitOptions.ColumnMap.Add(FSceneOutlinerBuiltInColumnTypes::Label(), FSceneOutlinerColumnInfo(ESceneOutlinerColumnVisibility::Visible, 2));
	InitOptions.ModeFactory = FCreateSceneOutlinerMode::CreateLambda([OnDataLayerPicked](SSceneOutliner* Outliner)
	{ 
		return new FDataLayerPickingMode(FDataLayerModeParams(Outliner, nullptr, nullptr, FOnSceneOutlinerItemPicked::CreateLambda([OnDataLayerPicked](const FSceneOutlinerTreeItemRef& NewParent)
		{
			FDataLayerTreeItem* DataLayerItem = NewParent->CastTo<FDataLayerTreeItem>();
			if (UDataLayerInstance* DataLayer = DataLayerItem ? DataLayerItem->GetDataLayer() : nullptr)
			{
				OnDataLayerPicked.ExecuteIfBound(DataLayer);
			}
			FSlateApplication::Get().DismissAllMenus();
		})));
	});

	TSharedRef<SDataLayerOutliner> Outliner = SNew(SDataLayerOutliner, InitOptions).IsEnabled(FSlateApplication::Get().GetNormalExecutionAttribute());
	TSharedRef<SWidget> DataLayerPickerWidget =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.MaxHeight(400.0f)
		[
			Outliner
		];

	Outliner->ExpandAll();

	return DataLayerPickerWidget;
}

void FDataLayerPickingMode::OnItemSelectionChanged(FSceneOutlinerTreeItemPtr TreeItem, ESelectInfo::Type SelectionType, const FSceneOutlinerItemSelection& Selection)
{
	if (OnItemPicked.IsBound())
	{
		auto SelectedItems = SceneOutliner->GetSelectedItems();
		if (SelectedItems.Num() > 0)
		{
			auto FirstItem = SelectedItems[0];
			if (FirstItem->CanInteract())
			{
				if (const FDataLayerTreeItem* DataLayerItem = FirstItem->CastTo<FDataLayerTreeItem>())
				{
					UDataLayerInstance* DataLayer = DataLayerItem->GetDataLayer();
					if (DataLayer && !DataLayer->IsLocked())
					{
						OnItemPicked.ExecuteIfBound(FirstItem.ToSharedRef());
					}
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
