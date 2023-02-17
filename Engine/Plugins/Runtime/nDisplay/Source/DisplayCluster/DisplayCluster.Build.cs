// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DisplayCluster : ModuleRules
{
	public DisplayCluster(ReadOnlyTargetRules ROTargetRules) : base(ROTargetRules)
	{
		PublicDefinitions.Add("WITH_OCIO=0");

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"CinematicCamera",
				"Core",
				"CoreUObject",
				"DisplayClusterConfiguration",
				"DisplayClusterLightCardEditorShaders",
				"DisplayClusterLightCardExtender",
				"Engine",
				"EnhancedInput",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"HeadMountedDisplay",
				"InputCore",
				"Json",
				"JsonUtilities",
				"Networking",
				"OpenColorIO",
				"OpenCV",
				"OpenCVHelper",
				"ProceduralMeshComponent",
				"Renderer",
				"RenderCore",
				"RHI",
				"Slate",
				"SlateCore",
				"Sockets",
				"UMG"
			});

		if (Target.bBuildEditor == true)
		{
			PublicIncludePathModuleNames.Add("DisplayClusterConfigurator");

			PrivateDependencyModuleNames.Add("UnrealEd");
			PrivateDependencyModuleNames.Add("LevelEditor");
		}

		if (Target.Type == TargetType.Editor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"ConcertSyncClient",
					"DataLayerEditor"
				});
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"D3D11RHI",
					"D3D12RHI",
			});

			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11", "DX12");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAPI");
		}
	}
}
