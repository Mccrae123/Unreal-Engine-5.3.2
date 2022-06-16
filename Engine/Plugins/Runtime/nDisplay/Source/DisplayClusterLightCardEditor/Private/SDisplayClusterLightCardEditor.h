// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

#include "DisplayClusterLightCardEditorProxyType.h"

class FTabManager;
class FLayoutExtender;
class FSpawnTabArgs;
class FToolBarBuilder;
class FUICommandList;
class SDockTab;
class SDisplayClusterLightCardList;
class SDisplayClusterLightCardTemplateList;
class SDisplayClusterLightCardEditorViewport;
class ADisplayClusterRootActor;
class ADisplayClusterLightCardActor;
class UDisplayClusterLightCardTemplate;

/** A panel that can be spawned in a tab that contains all the UI elements that make up the 2D light cards editor */
class SDisplayClusterLightCardEditor : public SCompoundWidget
{
public:
	/** The name of the tab that the light card editor lives in */
	static const FName TabName;

	/** Registers the light card editor with the global tab manager and adds it to the operator panel's extension tab stack */
	static void RegisterTabSpawner();

	/** Unregisters the light card editor from the global tab manager */
	static void UnregisterTabSpawner();

	/** Registers the light card editor tab with the operator panel using a layout extension */
	static void RegisterLayoutExtension(FLayoutExtender& InExtender);

	/** Creates a tab with the light card editor inside */
	static TSharedRef<SDockTab> SpawnInTab(const FSpawnTabArgs& SpawnTabArgs);

	static void ExtendToolbar(FToolBarBuilder& ToolbarBuilder);


	SLATE_BEGIN_ARGS(SDisplayClusterLightCardEditor)
	{}
	SLATE_END_ARGS()

	~SDisplayClusterLightCardEditor();

	void Construct(const FArguments& args, const TSharedRef<SDockTab>& MajorTabOwner, const TSharedPtr<SWindow>& WindowOwner);

	/** The current active root actor for this light card editor */
	TWeakObjectPtr<ADisplayClusterRootActor> GetActiveRootActor() const { return ActiveRootActor; }

	/** Selects the specified light cards in the light card list and details panel */
	void SelectLightCards(const TArray<ADisplayClusterLightCardActor*>& LightCardsToSelect);

	/** Gets the light cards that are selected in the light card list */
	void GetSelectedLightCards(TArray<ADisplayClusterLightCardActor*>& OutSelectedLightCards);

	/** Selects the light card proxies that correspond to the specified light cards */
	void SelectLightCardProxies(const TArray<ADisplayClusterLightCardActor*>& LightCardsToSelect);

	/** Places the given light card in the middle of the viewport */
	void CenterLightCardInView(ADisplayClusterLightCardActor& LightCard);

	/** Spawns a new light card and adds it to the root actor */
	ADisplayClusterLightCardActor* SpawnLightCard();

	/** Spawns a new light card from a light card template */
	ADisplayClusterLightCardActor* SpawnLightCardFromTemplate(const UDisplayClusterLightCardTemplate* InTemplate, ULevel* InLevel, bool bIsPreview);

	/** Adds a new light card to the root actor and centers it in the viewport */
	void AddNewLightCard();

	/** Select an existing Light Card from a menu */
	void AddExistingLightCard();

	/** Adds the given Light Card to the root actor */
	void AddLightCardsToActor(TArray<ADisplayClusterLightCardActor*> LightCards);

	/** If a Light Card can currently be added */
	bool CanAddLightCard() const;

	/** Copies any selected light cards to the clipboard, and then deletes them */
	void CutLightCards();

	/** Determines if there are selected light cards that can be cut */
	bool CanCutLightCards();

	/** Copies any selected light cards to the clipboard */
	void CopyLightCards();

	/** Determines if there are selected light cards that can be copied */
	bool CanCopyLightCards() const;

	/** Pastes any light cards in the clipboard to the current root actor */
	void PasteLightCards(bool bOffsetLightCardPosition);

	/** Determines if there are any light cards that can be pasted from the clipboard */
	bool CanPasteLightCards() const;

	/** Copies any selected light cards to the clipboard and then pastes them */
	void DuplicateLightCards();

	/** Determines if there are selected light cards that can be duplicated */
	bool CanDuplicateLightCards() const;

	/**
	 * Remove the light card from the actor
	 *@param bDeleteLightCardActor Delete the actor from the level
	 */
	void RemoveLightCards(bool bDeleteLightCardActor);

	/** If the selected Light Card can be removed */
	bool CanRemoveLightCards() const;

	/** Creates a template of the selected light card */
	void CreateLightCardTemplate();

	/** Determines if there are selected light cards that can have templates created */
	bool CanCreateLightCardTemplate() const;

private:
	/** Raised when the active Display cluster root actor has been changed in the operator panel */
	void OnActiveRootActorChanged(ADisplayClusterRootActor* NewRootActor);

	/** Creates the widget used to show the list of light cards associated with the active root actor */
	TSharedRef<SWidget> CreateLightCardListWidget();

	/** Create the widget for selecting light card templates */
	TSharedRef<SWidget> CreateLightCardTemplateWidget();
	
	/** Create the 3d viewport widget */
	TSharedRef<SWidget> CreateViewportWidget();

	/** Creates the editor's command list and binds commands to it */
	void BindCommands();

	/** Refresh all preview actors */
	void RefreshPreviewActors(EDisplayClusterLightCardEditorProxyType ProxyType = EDisplayClusterLightCardEditorProxyType::All);

	/**
	 * Check if an object is managed by us
	 * @param InObject The object to compare
	 * @param OutProxyType The type of the object
	 * @return True if our object, false if not
	 */
	bool IsOurObject(UObject* InObject, EDisplayClusterLightCardEditorProxyType& OutProxyType) const;
	
	/** Bind delegates to when a BP compiles */
	void BindCompileDelegates();

	/** Remove compile delegates from a BP */
	void RemoveCompileDelegates();

	/** When a property on the actor has changed */
	void OnActorPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent);

	/** Raised when the user deletes an actor from the level */
	void OnLevelActorDeleted(AActor* Actor);

	/** Raised when a supported blueprint is compiled */
	void OnBlueprintCompiled(UBlueprint* Blueprint);

	/** Raised when any object is transacted */
	void OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionObjectEvent);

	/** Raised when the light card list has added or removed a card */
	void OnLightCardListChanged();
	
private:
	TSharedPtr<FTabManager> TabManager;
	
	/** The light card list widget */
	TSharedPtr<SDisplayClusterLightCardList> LightCardList;

	/** The light card template list */
	TSharedPtr<SDisplayClusterLightCardTemplateList> LightCardTemplateList;
	
	/** The 3d viewport */
	TSharedPtr<SDisplayClusterLightCardEditorViewport> ViewportView;

	/** The command list for editor commands */
	TSharedPtr<FUICommandList> CommandList;

	/** Stores the mouse position when the context menu was opened */
	TOptional<FIntPoint> CachedContextMenuMousePos;

	/** A reference to the root actor that is currently being operated on */
	TWeakObjectPtr<ADisplayClusterRootActor> ActiveRootActor;

	/** Delegate handle for the OnActiveRootActorChanged delegate */
	FDelegateHandle ActiveRootActorChangedHandle;

	/** Delegate handle for when an object is transacted */
	FDelegateHandle OnObjectTransactedHandle;
};