// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ConsoleVariablesAsset.h"
#include "ConsoleVariablesEditorCommandInfo.h"
#include "MultiUser/ConsoleVariableSync.h"

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleInterface.h"
#include "Widgets/Docking/SDockTab.h"

struct FAssetData;

class FConsoleVariablesEditorMainPanel;
class FConsoleVariablesEditorToolkit;
class ISettingsSection;

class FConsoleVariablesEditorModule : public IModuleInterface
{
public:
	
	static FConsoleVariablesEditorModule& Get();

	//~ Begin IModuleInterface Interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface Interface

	void OpenConsoleVariablesDialogWithAssetSelected(const FAssetData& InAssetData);

	/** Find all console variables and cache their startup values */
	void QueryAndBeginTrackingConsoleVariables();

	void AddConsoleObjectCommandInfoToMasterReference(TSharedRef<FConsoleVariablesEditorCommandInfo> InCommandInfo)
	{
		ConsoleObjectsMasterReference.Add(InCommandInfo);
	}

	/** Find a tracked console variable by the command string with optional case sensitivity. */
	TWeakPtr<FConsoleVariablesEditorCommandInfo> FindCommandInfoByName(
		const FString& NameToSearch, ESearchCase::Type InSearchCase = ESearchCase::IgnoreCase);

	/**
	 *Find a tracked console variable by its console object reference.
	 *Note that some commands do not have an associated console object (such as 'stat unit') and will not be found with this method.
	 *It's normally safer to use FindCommandInfoByName() instead.
	 */
	TWeakPtr<FConsoleVariablesEditorCommandInfo> FindCommandInfoByConsoleObjectReference(
		IConsoleObject* InConsoleObjectReference);
	
	[[nodiscard]] TObjectPtr<UConsoleVariablesAsset> GetEditingAsset() const;
	void SetEditingAsset(const TObjectPtr<UConsoleVariablesAsset> InEditingAsset);

	void SendMultiUserConsoleVariableChange(const FString& InVariableName, const FString& InValueAsString);
	void OnRemoteCvarChanged(const FString InName, const FString InValue);
	
private:

	void OnFEngineLoopInitComplete();

	void RegisterMenuItem();
	void RegisterProjectSettings();

	void OnConsoleVariableChanged(IConsoleVariable* ChangedVariable);
	/** In the event a console object is unregistered, this failsafe callback will clean up the associated list item and command info object. */
	void OnDetectConsoleObjectUnregistered(FString CommandName);

	TObjectPtr<UConsoleVariablesAsset> AllocateTransientPreset();

	TSharedRef<SDockTab> SpawnMainPanelTab(const FSpawnTabArgs& Args);

	void OpenConsoleVariablesEditor();
	
	static const FName ConsoleVariablesToolkitPanelTabId;

	/* Lives for as long as the module is loaded. */
	TSharedPtr<FConsoleVariablesEditorMainPanel> MainPanel;

	// Transient preset that's being edited so we don't affect the reference asset unless we save it
	TObjectPtr<UConsoleVariablesAsset> EditingAsset = nullptr;

	/** All tracked variables and their default, startup, and current values */
	TArray<TSharedPtr<FConsoleVariablesEditorCommandInfo>> ConsoleObjectsMasterReference;
};
