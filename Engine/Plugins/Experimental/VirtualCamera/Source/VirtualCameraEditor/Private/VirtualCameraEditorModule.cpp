// Copyright Epic Games, Inc. All Rights Reserved.

#include "VirtualCameraEditorModule.h"
#include "VirtualCameraEditorStyle.h"

#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Modules/ModuleManager.h"
#include "VirtualCameraTab.h"
#include "VirtualCameraUserSettings.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"


#define LOCTEXT_NAMESPACE "FVirtualCameraEditorModule"


class FVirtualCameraEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FVirtualCameraEditorStyle::Register();
		const IWorkspaceMenuStructure& MenuStructure = WorkspaceMenu::GetMenuStructure();
		TSharedRef<FWorkspaceItem> DeveloperToolsGroup = MenuStructure.GetDeveloperToolsMiscCategory();
		SVirtualCameraTab::RegisterNomadTabSpawner(DeveloperToolsGroup);

		RegisterSettings();
	}

	virtual void ShutdownModule() override
	{
		UnregisterSettings();

		if (!IsEngineExitRequested() && UObjectInitialized())
		{
			FVirtualCameraEditorStyle::Unregister();
			SVirtualCameraTab::UnregisterNomadTabSpawner();
		}
	}

	void RegisterSettings()
	{
		ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
		if (SettingsModule != nullptr)
		{
			ISettingsSectionPtr SettingsSection = SettingsModule->RegisterSettings("Project", "Plugins", "VirtualCamera",
				LOCTEXT("VirtualCameraUserSettingsName", "Virtual Camera"),
				LOCTEXT("VirtualCameraUserSettingsDescription", "Configure the Virtual Camera settings."),
				GetMutableDefault<UVirtualCameraUserSettings>());
		}
	}

	void UnregisterSettings()
	{
		ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
		if (SettingsModule != nullptr)
		{
			SettingsModule->UnregisterSettings("Project", "Plugins", "VirtualCamera");
		}
	}
};

IMPLEMENT_MODULE(FVirtualCameraEditorModule, VirtualCameraEditor)

#undef LOCTEXT_NAMESPACE
