// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UIFramework : ModuleRules
{
	public UIFramework(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"FieldNotification",
				"ModelViewViewModel",
				"SlateCore",
				"Slate",
				"UMG",
				"Verse"
			}
		);

		PublicIncludePathModuleNames.AddRange(
			new string[] {
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"NetCore",
			}
		);

		SetupIrisSupport(Target);
	}
}
