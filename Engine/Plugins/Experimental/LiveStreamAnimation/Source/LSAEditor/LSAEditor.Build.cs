// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class LSAEditor : ModuleRules
	{
		public LSAEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"LiveStreamAnimation",
					"Core",
					"CoreUObject",
					"UnrealEd",
					"AssetTools",
					"Engine",
					"PropertyEditor",
					"SlateCore",
					"Slate",
					"InputCore",
					"EditorStyle",
				}
			);
		}
	}
}
