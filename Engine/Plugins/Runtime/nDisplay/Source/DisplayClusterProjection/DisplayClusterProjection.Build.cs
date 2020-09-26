// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class DisplayClusterProjection : ModuleRules
{
	public DisplayClusterProjection(ReadOnlyTargetRules ROTargetRules) : base(ROTargetRules)
	{
		PrivateIncludePaths.AddRange(
			new string[] {
				"../../../../../Engine/Source/Runtime/Windows/D3D11RHI/Private",
				"../../../../../Engine/Source/Runtime/Windows/D3D11RHI/Private/Windows",
			});

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Engine",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"ApplicationCore",
				"D3D11RHI",
				"DisplayCluster",
				"Composure",
				"HeadMountedDisplay",
				"MPCDI",
				"Projects",
				"RenderCore",
				"RHI",
			});

		if (Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAftermath");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelMetricsDiscovery");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelExtensionsFramework");

		AddThirdPartyDependencies(ROTargetRules);
	}

	public void AddThirdPartyDependencies(ReadOnlyTargetRules ROTargetRules)
	{
		string ThirdPartyPath = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// EasyBlend
			PrivateIncludePaths.Add(Path.Combine(ThirdPartyPath, "EasyBlend", "Include"));
			RuntimeDependencies.Add(Path.Combine(ThirdPartyPath, "EasyBlend", "DLL", "mplEasyBlendSDKDX1164.dll"));

			// Domeprojection
			PrivateIncludePaths.Add(Path.Combine(ThirdPartyPath, "Domeprojection", "Include"));
			RuntimeDependencies.Add(Path.Combine(ThirdPartyPath, "Domeprojection", "DLL", "dpLib.dll"));
			RuntimeDependencies.Add(Path.Combine(ThirdPartyPath, "Domeprojection", "DLL", "WibuCm64.dll"));
		}
	}
}
