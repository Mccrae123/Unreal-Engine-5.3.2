// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class MetasoundEngine : ModuleRules
	{
		public MetasoundEngine(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateDependencyModuleNames.AddRange
			(
				new string[]
				{
					"Core",
					"MetasoundGraphCore",
					"MetasoundGenerator",
					"AudioExtentions"
				}
			);

			PublicDependencyModuleNames.AddRange
			(
				new string[]
				{
					"CoreUObject",
					"Engine",
					"MetasoundFrontend",
					"Serialization"
				}
			);
		}
	}
}
