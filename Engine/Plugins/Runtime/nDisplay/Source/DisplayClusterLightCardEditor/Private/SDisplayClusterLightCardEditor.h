// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FTabManager;
class FLayoutExtender;
class FSpawnTabArgs;
class FToolBarBuilder;
class SDockTab;
class ADisplayClusterRootActor;

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

private:
	/** Raised when the active Display cluster root actor has been changed in the operator panel */
	void OnActiveRootActorChanged(ADisplayClusterRootActor* NewRootActor);

private:
	/** Holds the tab manager that manages the light card editor's tabs. */
	TSharedPtr<FTabManager> TabManager;

	/** A reference to the root actor that is currently being operated on */
	TWeakObjectPtr<ADisplayClusterRootActor> ActiveRootActor;

	FDelegateHandle ActiveRootActorChangedHandle;
};