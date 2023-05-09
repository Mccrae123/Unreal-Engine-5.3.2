// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_STATETREE_DEBUGGER

#include "Framework/Commands/Commands.h"

/**
 * StateTree Debugger command set.
 */
class FStateTreeDebuggerCommands : public TCommands<FStateTreeDebuggerCommands>
{
public:
	FStateTreeDebuggerCommands();

	// TCommands<> overrides
	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> ToggleBreakpoint;
	TSharedPtr<FUICommandInfo> PreviousFrameWithStateChange;
	TSharedPtr<FUICommandInfo> PreviousFrameWithEvents;
	TSharedPtr<FUICommandInfo> NextFrameWithEvents;
	TSharedPtr<FUICommandInfo> NextFrameWithStateChange;
};

#endif // WITH_STATETREE_DEBUGGER