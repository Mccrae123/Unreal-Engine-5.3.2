// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Framework/Commands/Commands.h"

/**
 * Defines commands for the Remote Control API which enables most functionality of the Remote Control Panel.
 */
class FRemoteControlCommands : public TCommands<FRemoteControlCommands>
{
public:

	FRemoteControlCommands();

	//~ BEGIN : TCommands<> Implementation(s)

	virtual void RegisterCommands() override;

	//~ END : TCommands<> Implementation(s)

	/**
	 * Holds the information about UI Command that toggles edit mode in RC Panel.
	 */
	TSharedPtr<FUICommandInfo> ToggleEditMode;

	/**
	 * Holds the information about UI Command that finds the actively edited preset in the Content Browser.
	 */
	TSharedPtr<FUICommandInfo> FindPresetInContentBrowser;

	/**
	 * Holds the information about UI Command that saves the actively edited preset.
	 */
	TSharedPtr<FUICommandInfo> SavePreset;

	/**
	 * Holds the information about UI Command that brings up a panel that facilitates exposing functions using RC API.
	 */
	TSharedPtr<FUICommandInfo> ToggleExposeFunctions;
	
	/**
	 * Holds the information about UI Command that brings up a panel which holds the active protocol mappings.
	 */
	TSharedPtr<FUICommandInfo> ToggleProtocolMappings;
	
	/**
	 * Holds the information about UI Command that brings up a panel which enables the RC Logical Behaviour.
	 */
	TSharedPtr<FUICommandInfo> ToggleLogicEditor;

	/**
	 * Holds the information about UI Command that deletes currently selected group/exposed entity.
	 */
	TSharedPtr< FUICommandInfo > DeleteEntity;

	/**
	 * Holds the information about UI Command that  renames selected group/exposed entity.
	 */
	TSharedPtr< FUICommandInfo > RenameEntity;
};
