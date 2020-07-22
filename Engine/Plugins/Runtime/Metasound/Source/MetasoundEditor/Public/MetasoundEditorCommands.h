// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "EditorStyleSet.h"

namespace Metasound
{
	namespace Editor
	{
		class FEditorCommands : public TCommands<FEditorCommands>
		{
		public:
			/** Constructor */
			FEditorCommands()
				: TCommands<FEditorCommands>("MetasoundEditor", NSLOCTEXT("Contexts", "MetasoundEditor", "Metasound Graph Editor"), NAME_None, "MetasoundStyle")
			{
			}

			/** Plays the Metasound */
			TSharedPtr<FUICommandInfo> Play;

			/** Stops the currently playing Metasound */
			TSharedPtr<FUICommandInfo> Stop;

			/** Compiles the Metasound */
			TSharedPtr<FUICommandInfo> Compile;

			/** Plays stops the currently playing Metasound */
			TSharedPtr<FUICommandInfo> TogglePlayback;

			/** Selects the Metasound in the content browser. If referencing Metasound nodes are selected, selects referenced assets instead. */
			TSharedPtr<FUICommandInfo> BrowserSync;

			/** Breaks the node input/output link */
			TSharedPtr<FUICommandInfo> BreakLink;

			/** Adds an input to the node */
			TSharedPtr<FUICommandInfo> AddInput;

			/** Removes an input from the node */
			TSharedPtr<FUICommandInfo> DeleteInput;

			/** Initialize commands */
			virtual void RegisterCommands() override;
		};
	} // namespace Editor
} // namespace Metasound