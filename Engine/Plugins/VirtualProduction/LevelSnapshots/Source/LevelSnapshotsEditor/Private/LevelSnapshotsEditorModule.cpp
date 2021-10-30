// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelSnapshotsEditorModule.h"

#include "Data/Filters/NegatableFilter.h"
#include "Data/LevelSnapshotsEditorData.h"
#include "LevelSnapshotsEditorCommands.h"
#include "LevelSnapshotsEditorStyle.h"
#include "Settings/LevelSnapshotsEditorProjectSettings.h"
#include "Settings/LevelSnapshotsEditorDataManagementSettings.h"
#include "Views/SLevelSnapshotsEditor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetTypeActions/AssetTypeActions_LevelSnapshot.h"
#include "IAssetTools.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "Util/TakeSnapshotUtil.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FLevelSnapshotsEditorModule"

namespace LevelSnapshotsEditor
{
	const FName LevelSnapshotsTabName("LevelSnapshots");
}

FLevelSnapshotsEditorModule& FLevelSnapshotsEditorModule::Get()
{
	return FModuleManager::GetModuleChecked<FLevelSnapshotsEditorModule>("LevelSnapshotsEditor");
}

void FLevelSnapshotsEditorModule::OpenLevelSnapshotsSettings()
{
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "Level Snapshots");
}

void FLevelSnapshotsEditorModule::StartupModule()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	AssetTools.RegisterAssetTypeActions(MakeShared<FAssetTypeActions_LevelSnapshot>());

	FLevelSnapshotsEditorStyle::Initialize();
	FLevelSnapshotsEditorCommands::Register();
	
	RegisterTabSpawner();
}

void FLevelSnapshotsEditorModule::ShutdownModule()
{
	UToolMenus::UnregisterOwner(this);
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);

	FLevelSnapshotsEditorStyle::Shutdown();

	if (UObjectInitialized())
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UNegatableFilter::StaticClass()->GetFName());

		UToolMenus::Get()->RemoveSection("LevelEditor.LevelEditorToolBar.User", "LevelSnapshots");
	}

	UnregisterTabSpawner();
	FLevelSnapshotsEditorCommands::Unregister();
	
	// Unregister project settings
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");
	{
		SettingsModule.UnregisterSettings("Project", "Plugins", "Level Snapshots");
		SettingsModule.UnregisterSettings("Project", "Plugins", "Level Snapshots Data Management");
	}
}

bool FLevelSnapshotsEditorModule::GetUseCreationForm() const
{
	if (ensureMsgf(ProjectSettingsObjectPtr.IsValid(), 
		TEXT("ProjectSettingsObjectPtr was not valid. Returning false for bUseCreationForm. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		return ProjectSettingsObjectPtr.Get()->bUseCreationForm;
	}
	
	return false;
}

void FLevelSnapshotsEditorModule::SetUseCreationForm(bool bInUseCreationForm)
{
	if (ensureMsgf(ProjectSettingsObjectPtr.IsValid(),
		TEXT("ProjectSettingsObjectPtr was not valid. Returning false for bUseCreationForm. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		ProjectSettingsObjectPtr.Get()->bUseCreationForm = bInUseCreationForm;
	}
}

void FLevelSnapshotsEditorModule::RegisterTabSpawner()
{
	FTabSpawnerEntry& TabSpawnerEntry = FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LevelSnapshotsEditor::LevelSnapshotsTabName, FOnSpawnTab::CreateRaw(this, &FLevelSnapshotsEditorModule::SpawnLevelSnapshotsTab))
			.SetDisplayName(NSLOCTEXT("LevelSnapshots", "LevelSnapshotsTabTitle", "Level Snapshots"))
			.SetTooltipText(NSLOCTEXT("LevelSnapshots", "LevelSnapshotsTooltipText", "Open the Level Snapshots tab"))
			.SetIcon(FSlateIcon(FLevelSnapshotsEditorStyle::GetStyleSetName(), "LevelSnapshots.ToolbarButton", "LevelSnapshots.ToolbarButton.Small")
			);
	TabSpawnerEntry.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorVirtualProductionCategory());
}

void FLevelSnapshotsEditorModule::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LevelSnapshotsEditor::LevelSnapshotsTabName);
}

TSharedRef<SDockTab> FLevelSnapshotsEditorModule::SpawnLevelSnapshotsTab(const FSpawnTabArgs& SpawnTabArgs)
{
	const TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(NomadTab);

	TSharedRef<SLevelSnapshotsEditor> SnapshotsEditor = SNew(SLevelSnapshotsEditor, AllocateTransientPreset(), DockTab, SpawnTabArgs.GetOwnerWindow());
	WeakSnapshotEditor = SnapshotsEditor;
	DockTab->SetContent(SnapshotsEditor);

	return DockTab;
}

ULevelSnapshotsEditorData* FLevelSnapshotsEditorModule::AllocateTransientPreset()
{
	ULevelSnapshotsEditorData* ExistingPreset = FindObject<ULevelSnapshotsEditorData>(nullptr, TEXT("/Temp/LevelSnapshots/PendingSnapshots.PendingSnapshots"));
	if (ExistingPreset)
	{
		return ExistingPreset;
	}
	
	UPackage* NewPackage = CreatePackage(TEXT("/Temp/LevelSnapshots/PendingSnapshots"));
	NewPackage->SetFlags(RF_Transient);
	NewPackage->AddToRoot();

	return NewObject<ULevelSnapshotsEditorData>(NewPackage, TEXT("PendingSnapshots"), RF_Transient | RF_Transactional | RF_Standalone);
}

void FLevelSnapshotsEditorModule::OpenLevelSnapshotsDialogWithAssetSelected(const FAssetData& InAssetData)
{
	OpenSnapshotsEditor();
	if (WeakSnapshotEditor.IsValid())
	{
		WeakSnapshotEditor.Pin()->OpenLevelSnapshotsDialogWithAssetSelected(InAssetData);
	}
}

void FLevelSnapshotsEditorModule::OpenSnapshotsEditor()
{
	FGlobalTabmanager::Get()->TryInvokeTab(LevelSnapshotsEditor::LevelSnapshotsTabName);
}

bool FLevelSnapshotsEditorModule::RegisterProjectSettings()
{
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");
	{
		// User Project Settings
		ProjectSettingsSectionPtr = SettingsModule.RegisterSettings("Project", "Plugins", "Level Snapshots",
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsSettingsCategoryDisplayName", "Level Snapshots"),
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsSettingsDescription", "Configure the Level Snapshots user settings"),
			GetMutableDefault<ULevelSnapshotsEditorProjectSettings>());

		if (ProjectSettingsSectionPtr.IsValid() && ProjectSettingsSectionPtr->GetSettingsObject().IsValid())
		{
			ProjectSettingsObjectPtr = Cast<ULevelSnapshotsEditorProjectSettings>(ProjectSettingsSectionPtr->GetSettingsObject());
			ProjectSettingsSectionPtr->OnModified().BindRaw(this, &FLevelSnapshotsEditorModule::HandleModifiedProjectSettings);
		}

		// Data Management Project Settings
		DataMangementSettingsSectionPtr = SettingsModule.RegisterSettings("Project", "Plugins", "Level Snapshots Data Management",
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsDataManagementSettingsCategoryDisplayName", "Level Snapshots Data Management"),
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsDataManagementSettingsDescription", "Configure the Level Snapshots path and data settings"),
			GetMutableDefault<ULevelSnapshotsEditorDataManagementSettings>());

		if (DataMangementSettingsSectionPtr.IsValid() && DataMangementSettingsSectionPtr->GetSettingsObject().IsValid())
		{
			DataMangementSettingsObjectPtr = Cast<ULevelSnapshotsEditorDataManagementSettings>(DataMangementSettingsSectionPtr->GetSettingsObject());
			DataMangementSettingsSectionPtr->OnModified().BindRaw(this, &FLevelSnapshotsEditorModule::HandleModifiedProjectSettings);
		}
	}

	return ProjectSettingsObjectPtr.IsValid();
}

bool FLevelSnapshotsEditorModule::HandleModifiedProjectSettings()
{
	if (ensureMsgf(DataMangementSettingsObjectPtr.IsValid(),
		TEXT("ProjectSettingsObjectPtr was not valid. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		DataMangementSettingsObjectPtr->ValidateRootLevelSnapshotSaveDirAsGameContentRelative();
		DataMangementSettingsObjectPtr->SanitizeAllProjectSettingsPaths(true);
		
		DataMangementSettingsObjectPtr.Get()->SaveConfig();
	}
	
	return true;
}

void FLevelSnapshotsEditorModule::RegisterEditorToolbar()
{
	if (IsRunningGame())
	{
		return;
	}
	
	MapEditorToolbarActions();

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
	FToolMenuSection& Section = Menu->FindOrAddSection("LevelSnapshots");

	FToolMenuEntry LevelSnapshotsButtonEntry = FToolMenuEntry::InitToolBarButton(
		"TakeSnapshotAction",
		FUIAction(FExecuteAction::CreateStatic(&SnapshotEditor::TakeSnapshotWithOptionalForm)),
		NSLOCTEXT("LevelSnapshots", "LevelSnapshots", "Level Snapshots"), // Set Text under image
		NSLOCTEXT("LevelSnapshots", "LevelSnapshotsToolbarButtonTooltip", "Take snapshot with optional form"), //  Set tooltip
		FSlateIcon(FLevelSnapshotsEditorStyle::GetStyleSetName(), "LevelSnapshots.ToolbarButton", "LevelSnapshots.ToolbarButton.Small") // Set image
	);
	LevelSnapshotsButtonEntry.SetCommandList(EditorToolbarButtonCommandList);

	 FToolMenuEntry LevelSnapshotsComboEntry = FToolMenuEntry::InitComboButton(
		"LevelSnapshotsMenu",
		FUIAction(),
		FOnGetContent::CreateRaw(this, &FLevelSnapshotsEditorModule::FillEditorToolbarComboButtonMenuOptions, EditorToolbarButtonCommandList),
		NSLOCTEXT("LevelSnapshots", "LevelSnapshotsOptions_Label", "Level Snapshots Options"), // Set text seen when the Level Editor Toolbar is truncated and the flyout is clicked
		NSLOCTEXT("LevelSnapshots", "LevelSnapshotsToolbarComboButtonTooltip", "Open Level Snapshots Options"), //  Set tooltip
		FSlateIcon(),
		true //bInSimpleComboBox
	);

	Section.AddEntry(LevelSnapshotsButtonEntry);
	Section.AddEntry(LevelSnapshotsComboEntry);
}

void FLevelSnapshotsEditorModule::MapEditorToolbarActions()
{
	EditorToolbarButtonCommandList = MakeShared<FUICommandList>();

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().UseCreationFormToggle,
		FUIAction(
			FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::ToggleUseCreationForm),
			FCanExecuteAction(),
			FIsActionChecked::CreateRaw(this, &FLevelSnapshotsEditorModule::GetUseCreationForm)
		)
	);

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorToolbarButton,
		FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::OpenSnapshotsEditor)
	);

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().LevelSnapshotsSettings,
		FExecuteAction::CreateStatic(&FLevelSnapshotsEditorModule::OpenLevelSnapshotsSettings)
	);
}

TSharedRef<SWidget> FLevelSnapshotsEditorModule::FillEditorToolbarComboButtonMenuOptions(TSharedPtr<class FUICommandList> Commands)
{
	// Create FMenuBuilder instance for the commands we created
	FMenuBuilder MenuBuilder(true, Commands);

	// Then use it to add entries to the submenu of the combo button
	MenuBuilder.BeginSection("Creation", NSLOCTEXT("LevelSnapshots", "Creation", "Creation"));
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().UseCreationFormToggle);
	MenuBuilder.EndSection();
	MenuBuilder.AddMenuSeparator();
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorToolbarButton);
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().LevelSnapshotsSettings);

	// Create the widget so it can be attached to the combo button
	return MenuBuilder.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FLevelSnapshotsEditorModule, LevelSnapshotsEditor)
