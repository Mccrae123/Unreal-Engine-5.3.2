// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class NaniteDisplacedMesh : ModuleRules
	{
		public NaniteDisplacedMesh(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateIncludePaths.Add("NaniteDisplacedMesh/Private");
			PublicIncludePaths.Add(ModuleDirectory + "/Public");

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"ImageCore",
					"CoreUObject",
					"Engine",
					"ImageWrapper",
					"MediaAssets",
					"RenderCore",
					"RHI",
					"TimeManagement",
					"SlateCore",
				}
			);
		}
	}
}
