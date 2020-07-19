// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Delegates/Delegate.h"

class UOptimusNodeGraph;

enum class EOptimusNodeGraphNotifyType : uint8
{
	NodeAdded,				/// A new node has been added (Subject == UOptimusNode)
	NodeRemoved,			/// A node has been removed (Subject == UOptimusNode)

	NodeLinkAdded,			/// A link between nodes has been added (Subject == UOptimusNodeLink)
	NodeLinkRemoved,		/// A link between nodes has been removed (Subject == UOptimusNodeLink)

	NodeDisplayNameChanged,	/// A node's display name has changed (Subject == UOptimusNode)
	NodePositionChanged,	/// A node's position in the graph has changed (Subject == UOptimusNode)

};

// A delegate for subscribing / reacting to graph modifications.
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOptimusNodeGraphEvent, EOptimusNodeGraphNotifyType /* type */, UOptimusNodeGraph* /* graph */, UObject* /* subject */);
