// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ISceneOutlinerMode.h"

namespace SceneOutliner
{
	/** Functor which can be used to get weak actor pointers from a selection */
	struct FWeakActorSelector
	{
		bool operator()(const TWeakPtr<ISceneOutlinerTreeItem>& Item, TWeakObjectPtr<AActor>& DataOut) const;
	};

	/** Functor which can be used to get actors from a selection including component parents */
	struct FActorSelector
	{
		bool operator()(const TWeakPtr<ISceneOutlinerTreeItem>& Item, AActor*& ActorPtrOut) const;
	};
}

class SCENEOUTLINER_API FActorMode : public ISceneOutlinerMode
{
public:
	struct EItemSortOrder
	{
		enum Type { World = 0, Folder = 10, Actor = 20 };
	};
	
	FActorMode(SSceneOutliner* InSceneOutliner, bool bHideComponents, TWeakObjectPtr<UWorld> InSpecifiedWorldToDisplay = nullptr);
	virtual ~FActorMode();

	virtual void Rebuild() override;

	void BuildWorldPickerMenu(FMenuBuilder& MenuBuilder);

	virtual void SynchronizeSelection() override { SynchronizeActorSelection(); }

	virtual void OnFilterTextChanged(const FText& InFilterText) override;

	virtual int32 GetTypeSortPriority(const ISceneOutlinerTreeItem& Item) const override;
private:
	/** Called when the user selects a world in the world picker menu */
	void OnSelectWorld(TWeakObjectPtr<UWorld> World);
private:
	/* Private Helpers */

	void ChooseRepresentingWorld();
	bool IsWorldChecked(TWeakObjectPtr<UWorld> World) const;
protected:
	void SynchronizeActorSelection();
	bool IsActorDisplayable(const AActor* InActor) const;

	virtual TUniquePtr<ISceneOutlinerHierarchy> CreateHierarchy() override;
protected:
	// Should the hide components filter be enabled
	bool bHideComponents;
	/** The world which we are currently representing */
	TWeakObjectPtr<UWorld> RepresentingWorld;
	/** The world which the user manually selected */
	TWeakObjectPtr<UWorld> UserChosenWorld;

	/** If this mode was created to display a specific world, don't allow it to be reassigned */
	const TWeakObjectPtr<UWorld> SpecifiedWorldToDisplay;
};
