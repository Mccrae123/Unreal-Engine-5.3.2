// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Embree3 : ModuleRules
{
	public Embree3(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// EMBREE
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string SDKDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree3122/Win64/";

			PublicIncludePaths.Add(SDKDir + "include");
			PublicAdditionalLibraries.Add(SDKDir + "lib/embree3.lib");
			PublicAdditionalLibraries.Add(SDKDir + "lib/tbb.lib");
			RuntimeDependencies.Add("$(TargetOutputDir)/embree3.dll", SDKDir + "lib/embree3.dll");
			RuntimeDependencies.Add("$(TargetOutputDir)/tbb12.dll", SDKDir + "lib/tbb12.dll");
			PublicDefinitions.Add("USE_EMBREE=1");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			//@todo - fix for Mac
			PublicDefinitions.Add("USE_EMBREE=0");
			/*

			string SDKDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree3122/MacOSX/";

			PublicIncludePaths.Add(SDKDir + "include");
			PublicAdditionalLibraries.Add(SDKDir + "lib/libembree3.3.dylib");
			PublicAdditionalLibraries.Add(SDKDir + "lib/libtbb.12.1.dylib");
			RuntimeDependencies.Add("$(TargetOutputDir)/libembree3.3.dylib", SDKDir + "lib/libembree3.3.dylib");
			RuntimeDependencies.Add("$(TargetOutputDir)/libtbb.12.1.dylib", SDKDir + "lib/libtbb.12.1.dylib");
			PublicDefinitions.Add("USE_EMBREE=1");
*/

		}
		else if (Target.IsInPlatformGroup(UnrealPlatformGroup.Unix) && Target.Architecture.StartsWith("x86_64")) // no support for arm64 yet
		{
			string IntelEmbreeLibs = Target.UEThirdPartyBinariesDirectory + "Intel/Embree/Embree3122";
			string IncludeDir = Target.UEThirdPartySourceDirectory + "Intel/Embree/Embree3122/Linux/x86_64-unknown-linux-gnu";
			string SDKDir = Path.Combine(IntelEmbreeLibs, "Linux/x86_64-unknown-linux-gnu/lib");

			PublicIncludePaths.Add(Path.Combine(IncludeDir, "include"));
			PublicAdditionalLibraries.Add(Path.Combine(SDKDir, "libembree.so"));
			RuntimeDependencies.Add(Path.Combine(SDKDir, "libembree.so"));
			RuntimeDependencies.Add(Path.Combine(SDKDir, "libembree.so.2"));
			PublicDefinitions.Add("USE_EMBREE=1");
		}
		else
		{
			PublicDefinitions.Add("USE_EMBREE=0");
		}
	}
}
