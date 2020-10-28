// Copyright Epic Games, Inc. All Rights Reserved.

#include "DataLayerHierarchy.h"
#include "DataLayerMode.h"
#include "DataLayerActorTreeItem.h"
#include "DataLayerTreeItem.h"
#include "SDataLayerBrowser.h"
#include "DataLayerMode.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/DataLayer/DataLayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"

TUniquePtr<FDataLayerHierarchy> FDataLayerHierarchy::Create(FDataLayerMode* Mode, const TWeakObjectPtr<UWorld>& World)
{
	FDataLayerHierarchy* Hierarchy = new FDataLayerHierarchy(Mode, World);

	GEngine->OnLevelActorAdded().AddRaw(Hierarchy, &FDataLayerHierarchy::OnLevelActorAdded);
	GEngine->OnLevelActorDeleted().AddRaw(Hierarchy, &FDataLayerHierarchy::OnLevelActorDeleted);
	GEngine->OnLevelActorListChanged().AddRaw(Hierarchy, &FDataLayerHierarchy::OnLevelActorListChanged);
	UDataLayerEditorSubsystem::Get()->OnDataLayerChanged().AddRaw(Hierarchy, &FDataLayerHierarchy::OnDataLayerChanged);
	UDataLayerEditorSubsystem::Get()->OnActorDataLayersChanged().AddRaw(Hierarchy, &FDataLayerHierarchy::OnActorDataLayersChanged);
	Mode->GetDataLayerBrowser()->OnModeChanged().AddRaw(Hierarchy, &FDataLayerHierarchy::OnDataLayerBrowserModeChanged);

	FWorldDelegates::LevelAddedToWorld.AddRaw(Hierarchy, &FDataLayerHierarchy::OnLevelAdded);
	FWorldDelegates::LevelRemovedFromWorld.AddRaw(Hierarchy, &FDataLayerHierarchy::OnLevelRemoved);

	return TUniquePtr<FDataLayerHierarchy>(Hierarchy);
}

FDataLayerHierarchy::FDataLayerHierarchy(FDataLayerMode* Mode, const TWeakObjectPtr<UWorld>& World)
	: ISceneOutlinerHierarchy(Mode)
	, RepresentingWorld(World)
{
}

FDataLayerHierarchy::~FDataLayerHierarchy()
{
	if (GEngine)
	{
		GEngine->OnLevelActorAdded().RemoveAll(this);
		GEngine->OnLevelActorDeleted().RemoveAll(this);
		GEngine->OnLevelActorListChanged().RemoveAll(this);
	}

	UDataLayerEditorSubsystem::Get()->OnDataLayerChanged().RemoveAll(this);
	UDataLayerEditorSubsystem::Get()->OnActorDataLayersChanged().RemoveAll(this);

	if (SDataLayerBrowser* DataLayerBrowser = GetDataLayerMode() ? GetDataLayerMode()->GetDataLayerBrowser() : nullptr)
	{
		DataLayerBrowser->OnModeChanged().RemoveAll(this);
	}

	FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
	FWorldDelegates::LevelRemovedFromWorld.RemoveAll(this);
}

FDataLayerMode* FDataLayerHierarchy::GetDataLayerMode() const
{
	return (FDataLayerMode*)Mode;
}

void FDataLayerHierarchy::CreateItems(TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	const AWorldDataLayers* WorldDataLayers = AWorldDataLayers::Get(RepresentingWorld.Get());
	if (!WorldDataLayers)
	{
		return;
	}

	WorldDataLayers->ForEachDataLayer([this, &OutItems](UDataLayer* DataLayer)
		{
			if (FSceneOutlinerTreeItemPtr DataLayerItem = Mode->CreateItemFor<FDataLayerTreeItem>(DataLayer))
			{
				OutItems.Add(DataLayerItem);
			}
			return true;
		});

	if (GetDataLayerMode()->GetDataLayerBrowser()->GetMode() == EDataLayerBrowserMode::DataLayerContents)
	{
		for (AActor* Actor : FActorRange(RepresentingWorld.Get()))
		{
			if (Actor->HasDataLayers())
			{
				for (const UDataLayer* DataLayer : Actor->GetDataLayerObjects())
				{
					if (FSceneOutlinerTreeItemPtr DataLayerActorItem = Mode->CreateItemFor<FDataLayerActorTreeItem>(FDataLayerActorTreeItemData(Actor, const_cast<UDataLayer*>(DataLayer))))
					{
						OutItems.Add(DataLayerActorItem);
					}
				}
			}
		}
	}
}

FSceneOutlinerTreeItemPtr FDataLayerHierarchy::FindParent(const ISceneOutlinerTreeItem& Item, const TMap<FSceneOutlinerTreeItemID, FSceneOutlinerTreeItemPtr>& Items) const
{
	if (Item.IsA<FDataLayerTreeItem>())
	{
		return nullptr;
	}
	else if (const FDataLayerActorTreeItem* DataLayerActorTreeItem = Item.CastTo<FDataLayerActorTreeItem>())
	{
		if (const UDataLayer* DataLayer = DataLayerActorTreeItem->GetDataLayer())
		{
			if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(DataLayer))
			{
				return *ParentItem;
			}
		}
	}
	return nullptr;
}

FSceneOutlinerTreeItemPtr FDataLayerHierarchy::CreateParentItem(const FSceneOutlinerTreeItemPtr& Item) const
{
	if (Item->IsA<FDataLayerTreeItem>())
	{
		return nullptr;
	}
	else if (FDataLayerActorTreeItem* DataLayerActorTreeItem = Item->CastTo<FDataLayerActorTreeItem>())
	{
		if (UDataLayer* DataLayer = DataLayerActorTreeItem->GetDataLayer())
		{
			return Mode->CreateItemFor<FDataLayerTreeItem>(DataLayer);
		}
	}
	return nullptr;
}

void FDataLayerHierarchy::OnLevelActorAdded(AActor* InActor)
{
	if (InActor && RepresentingWorld.Get() == InActor->GetWorld())
	{
		if (InActor->HasDataLayers())
		{
			FSceneOutlinerHierarchyChangedData EventData;
			EventData.Type = FSceneOutlinerHierarchyChangedData::Added;

			for (const UDataLayer* DataLayer : InActor->GetDataLayerObjects())
			{
				EventData.Item = Mode->CreateItemFor<FDataLayerActorTreeItem>(FDataLayerActorTreeItemData(InActor, const_cast<UDataLayer*>(DataLayer)));
				HierarchyChangedEvent.Broadcast(EventData);
			}
		}
	}
}

void FDataLayerHierarchy::OnActorDataLayersChanged(const TWeakObjectPtr<AActor>& InActor)
{
	AActor* Actor = InActor.Get();
	if (Actor && RepresentingWorld.Get() == Actor->GetWorld())
	{
		FullRefreshEvent();
	}
}

void FDataLayerHierarchy::OnDataLayerChanged(const EDataLayerAction Action, const TWeakObjectPtr<const UDataLayer>& ChangedDataLayer, const FName& ChangedProperty)
{
	const UDataLayer* DataLayer = ChangedDataLayer.Get();
	if ((DataLayer && (RepresentingWorld.Get() == DataLayer->GetWorld())) || (Action == EDataLayerAction::Delete) || (Action == EDataLayerAction::Reset))
	{
		FullRefreshEvent();
	}
}

void FDataLayerHierarchy::OnDataLayerBrowserModeChanged(EDataLayerBrowserMode InMode)
{
	FullRefreshEvent();
}

void FDataLayerHierarchy::OnLevelActorDeleted(AActor* InActor)
{
	if (RepresentingWorld.Get() == InActor->GetWorld())
	{
		if (InActor->HasDataLayers())
		{
			FSceneOutlinerHierarchyChangedData EventData;
			EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;

			for (const UDataLayer* DataLayer : InActor->GetDataLayerObjects())
			{
				EventData.ItemID = FDataLayerActorTreeItem::ComputeTreeItemID(InActor, DataLayer);
				HierarchyChangedEvent.Broadcast(EventData);
			}
		}
	}
}

void FDataLayerHierarchy::OnLevelActorListChanged()
{
	FullRefreshEvent();
}

void FDataLayerHierarchy::OnLevelAdded(ULevel* InLevel, UWorld* InWorld)
{
	if (RepresentingWorld.Get() == InWorld)
	{
		FullRefreshEvent();
	}
}

void FDataLayerHierarchy::OnLevelRemoved(ULevel* InLevel, UWorld* InWorld)
{
	if (RepresentingWorld.Get() == InWorld)
	{
		FullRefreshEvent();
	}
}

void FDataLayerHierarchy::FullRefreshEvent()
{
	FSceneOutlinerHierarchyChangedData EventData;
	EventData.Type = FSceneOutlinerHierarchyChangedData::FullRefresh;
	HierarchyChangedEvent.Broadcast(EventData);
}