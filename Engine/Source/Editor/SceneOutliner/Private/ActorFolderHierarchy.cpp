#include "ActorFolderHierarchy.h"

#include "ISceneOutlinerMode.h"
#include "WorldTreeItem.h"
#include "ActorFolderTreeItem.h"
#include "EditorActorFolders.h"

FActorFolderHierarchy::FActorFolderHierarchy(ISceneOutlinerMode* InMode, const TWeakObjectPtr<UWorld>& World)
	: ISceneOutlinerHierarchy(InMode)
	, RepresentingWorld(World)
{
	// ActorFolderHierarchy should only be used with a mode which is showing folders
	check(Mode->ShouldShowFolders());
}

FSceneOutlinerTreeItemPtr FActorFolderHierarchy::FindParent(const ISceneOutlinerTreeItem& Item, const TMap<FSceneOutlinerTreeItemID, FSceneOutlinerTreeItemPtr>& Items) const
{
	if (Item.IsA<FWorldTreeItem>())
	{
		return nullptr;
	}
	else if (const FActorFolderTreeItem* ActorFolderItem = Item.CastTo<FActorFolderTreeItem>())
	{
		const FName ParentPath = SceneOutliner::GetParentPath(ActorFolderItem->Path);

		const FSceneOutlinerTreeItemPtr* ParentItem = nullptr;
		// If the folder has no parent path, it must be parented to the root world
		if (ParentPath.IsNone())
		{
			ParentItem = Items.Find(ActorFolderItem->World.Get());
		}
		else
		{
			ParentItem = Items.Find(SceneOutliner::GetParentPath(ActorFolderItem->Path));
		}

		if (ParentItem)
		{
			return *ParentItem;
		}
	}
	return nullptr;
}

void FActorFolderHierarchy::FindChildren(const ISceneOutlinerTreeItem& Item, const TMap<FSceneOutlinerTreeItemID, FSceneOutlinerTreeItemPtr>& Items, TArray<FSceneOutlinerTreeItemPtr>& OutChildren) const
{
	if (const FWorldTreeItem* WorldTreeItem = Item.CastTo<FWorldTreeItem>())
	{
		// Find folders which are located at the root
		for (const auto& Pair : FActorFolders::Get().GetFolderPropertiesForWorld(*WorldTreeItem->World))
		{
			if (const FSceneOutlinerTreeItemPtr* PotentialChild = Items.Find(Pair.Key))
			{
				if (const FFolderTreeItem* FolderItem = (*PotentialChild)->CastTo<FFolderTreeItem>())
				{
					if (SceneOutliner::GetParentPath(FolderItem->Path).IsNone())
					{
						OutChildren.Add(*PotentialChild);
					}
				}
			}
		}
	}
	else if (const FFolderTreeItem* FolderItem = Item.CastTo<FFolderTreeItem>())
	{
		// Search through all items and see if there is an item with a path which is a child of this one
		for (const auto& Pair : Items)
		{
			if (const FFolderTreeItem* PotentialChild = Pair.Value->CastTo<FFolderTreeItem>())
			{
				if (SceneOutliner::PathIsChildOf(PotentialChild->Path, FolderItem->Path))
				{
					OutChildren.Add(Pair.Value);
				}
			}
		}
	}
}

void FActorFolderHierarchy::CreateWorldChildren(UWorld* World, TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	for (const auto& Pair : FActorFolders::Get().GetFolderPropertiesForWorld(*World))
	{
		if (FSceneOutlinerTreeItemPtr FolderItem = Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(Pair.Key, World)))
		{
			OutItems.Add(FolderItem);
		}
	}
}

void FActorFolderHierarchy::CreateItems(TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	check(RepresentingWorld.IsValid());

	if (FSceneOutlinerTreeItemPtr WorldItem = Mode->CreateItemFor<FWorldTreeItem>(RepresentingWorld.Get()))
	{
		OutItems.Add(WorldItem);
	}

	CreateWorldChildren(RepresentingWorld.Get(), OutItems);
}

void FActorFolderHierarchy::CreateChildren(const FSceneOutlinerTreeItemPtr& Item, TArray<FSceneOutlinerTreeItemPtr>& OutChildren) const
{
	if (const FWorldTreeItem* WorldItem = Item->CastTo<FWorldTreeItem>())
	{
		check(WorldItem->World == RepresentingWorld);
		CreateWorldChildren(WorldItem->World.Get(), OutChildren);
	}
	else if (FActorFolderTreeItem* FolderItem = Item->CastTo<FActorFolderTreeItem>())
	{
		// Since no map of folder->children exist for ActorFolders, must iterate through all
		// and manually check the path to know if it a child
		for (const auto& Pair : FActorFolders::Get().GetFolderPropertiesForWorld(*(FolderItem->World)))
		{
			if (SceneOutliner::PathIsChildOf(Pair.Key, FolderItem->Path))
			{
				if (FSceneOutlinerTreeItemPtr NewFolderItem = Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(Pair.Key, FolderItem->World), true))
				{
					OutChildren.Add(NewFolderItem);
				}
			}
		}
	}
}

FSceneOutlinerTreeItemPtr FActorFolderHierarchy::CreateParentItem(const FSceneOutlinerTreeItemPtr& Item) const
{
	if (Item->IsA<FWorldTreeItem>())
	{
		return nullptr;
	}
	else if (const FActorFolderTreeItem* FolderTreeItem = Item->CastTo<FActorFolderTreeItem>())
	{
		const FName ParentPath = SceneOutliner::GetParentPath(FolderTreeItem->Path);
		if (ParentPath.IsNone())
		{
			UWorld* OwningWorld = FolderTreeItem->World.Get();
			check(OwningWorld);
			return Mode->CreateItemFor<FWorldTreeItem>(OwningWorld, true);
		}
		else
		{
			return Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(ParentPath, FolderTreeItem->World));
		}
	}
	return nullptr;
}
