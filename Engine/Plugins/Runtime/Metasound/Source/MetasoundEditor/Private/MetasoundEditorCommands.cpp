// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorCommands.h"

#define LOCTEXT_NAMESPACE "MetasoundEditorCommands"

namespace Metasound
{
	namespace Editor
	{
		void FEditorCommands::RegisterCommands()
		{
			UI_COMMAND(Play, "Play", "Plays (or restarts) the Metasound", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(Stop, "Stop", "Stops Metasound (If currently playing)", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(TogglePlayback, "Toggle Playback", "Plays the Metasound or stops the currently playing Metasound", EUserInterfaceActionType::Button, FInputChord(EKeys::SpaceBar));

			UI_COMMAND(Import, "Import", "Imports Metasound from Json", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(Export, "Export", "Exports Metasound to Json", EUserInterfaceActionType::Button, FInputChord());

			UI_COMMAND(BrowserSync, "Browse", "Selects the Metasound in the content browser. If referencing Metasound nodes are selected, selects referenced assets instead.", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(AddInput, "Add Input", "Adds an input to the node", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(DeleteInput, "Delete Input", "Removes an input from the node", EUserInterfaceActionType::Button, FInputChord());

			UI_COMMAND(EditMetasoundSettings, "Settings", "Edit Metasound Settings", EUserInterfaceActionType::Button, FInputChord());
		}
	} // namespace Editor
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
