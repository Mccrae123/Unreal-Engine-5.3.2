// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FDMXOutputConsoleFaderDescriptor;
class UDMXControlConsole;
class UDMXControlConsoleFaderGroup;
class UDMXControlConsolePreset;
class UDMXControlConsoleRawFader;


/** Handler to upgrade path from Control Console previous versions */
class FDMXControlConsoleEditorFromLegacyUpgradeHandler
{
public:
	/** Tries to upgrade configuration settings from previous DMXControlConsole versions */
	static void TryUpgradePathFromLegacy();

private:
	/** Creates Fader Groups based on FaderDescriptor array. Used only for compatibility with DMXControlConsole previous versions */
	static UDMXControlConsole* CreateControlConsoleFromFaderDescriptorArray(const TArray<FDMXOutputConsoleFaderDescriptor>& FaderDescriptorArray);

	/** Creates a raw fader for the given Fader Group based on a FaderDescriptor. Used only for compatibility with DMXControlConsole previous versions */
	static UDMXControlConsoleRawFader* CreateRawFaderFromFaderDescriptor(UDMXControlConsoleFaderGroup* FaderGroup, const FDMXOutputConsoleFaderDescriptor& FaderDescriptor);

	/** Called when UpgradePathControlConsolePreset asset is saved */
	static void OnUpgradePathControlConsolePresetSaved(const UDMXControlConsolePreset* ControlConsolePreset);

	/** Weak reference to the Control Console Preset created by upgrade path process */
	static TWeakObjectPtr<UDMXControlConsolePreset> UpgradePathControlConsolePreset;
};
