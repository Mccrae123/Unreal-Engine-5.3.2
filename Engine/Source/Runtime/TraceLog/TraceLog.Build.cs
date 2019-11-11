// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

public class TraceLog : ModuleRules
{
	public TraceLog(ReadOnlyTargetRules Target) : base(Target)
	{
		bRequiresImplementModule = false;
		PublicIncludePathModuleNames.Add("Core");
		PrivateDefinitions.Add("LZ4_DISABLE_DEPRECATE_WARNINGS");
    }
}
