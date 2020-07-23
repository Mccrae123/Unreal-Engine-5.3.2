// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class NetworkPredictionExtras : ModuleRules
	{
		public NetworkPredictionExtras(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicIncludePaths.AddRange(
				new string[] {
                    ModuleDirectory + "/Public",
				}
				);

			PrivateIncludePaths.AddRange(
				new string[] {
                    "NetworkPredictionExtras/Private",
				}
				);

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"NetworkPrediction",
					"Core",
                    "CoreUObject",
                    "Engine",
                    "RenderCore",
					"InputCore",
					"PhysicsCore",
					"Chaos",
					"ChaosSolvers",
					"Engine"
				}
				);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"TraceLog",
					"Engine"
                }
				);

			DynamicallyLoadedModuleNames.AddRange(
				new string[]
				{
				}
				);

		}
	}
}
