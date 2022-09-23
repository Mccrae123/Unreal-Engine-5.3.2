// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using UnrealBuildTool;

public class ORTDefault : ModuleRules
{
    public ORTDefault(ReadOnlyTargetRules Target) : base(Target)
    {
		Type = ModuleType.External;
		// Win64
		if (Target.Platform == UnrealTargetPlatform.Win64 || 
			Target.Platform == UnrealTargetPlatform.Mac ||
			Target.Platform == UnrealTargetPlatform.Linux)
		{
			// PublicSystemIncludePaths
			// string IncPath = Path.Combine(ModuleDirectory, "include/");
			// PublicSystemIncludePaths.Add(IncPath);

			// PublicSystemIncludePaths
			PublicIncludePaths.AddRange(
				new string[] {
					System.IO.Path.Combine(ModuleDirectory, "include/"),
					System.IO.Path.Combine(ModuleDirectory, "include/onnxruntime"),
					System.IO.Path.Combine(ModuleDirectory, "include/onnxruntime/core/session")
				}
			);

			// PublicAdditionalLibraries
			string PlatformDir = Target.Platform.ToString();
			string LibDirPath = Path.Combine(ModuleDirectory, "lib", PlatformDir);


			string[] LibFileNames;
			if(Target.Platform == UnrealTargetPlatform.Win64)
			{
				LibFileNames = new string[] {
					"onnxruntime",
					"onnxruntime_providers_cuda",
					"onnxruntime_providers_shared",
					"custom_op_library",
					"test_execution_provider"
				};
			}
			else if(Target.Platform == UnrealTargetPlatform.Linux)
			{
				LibFileNames = new string[] {
					"onnxruntime",
					"onnxruntime_providers_shared",
					"custom_op_library",
					"test_execution_provider"
				};
			}
			else if(Target.Platform == UnrealTargetPlatform.Mac)
			{
				LibFileNames = new string[] {
					"onnxruntime",
					"custom_op_library"
				};
			}
			else 
			{
				LibFileNames = new string[] {};
			}

			string BinaryThirdPartyDirPath = Path.GetFullPath(Path.Combine(ModuleDirectory, "bin", PlatformDir));
			
			foreach (string LibFileName in LibFileNames)
			{

				if(Target.Platform == UnrealTargetPlatform.Win64)
				{
					PublicAdditionalLibraries.Add(Path.Combine(LibDirPath, LibFileName + ".lib"));
					
					// PublicDelayLoadDLLs
					string DLLFileName = LibFileName + ".dll";
					PublicDelayLoadDLLs.Add(DLLFileName);

					// RuntimeDependencies
					string DLLFullPath = Path.Combine(BinaryThirdPartyDirPath, DLLFileName);
					RuntimeDependencies.Add(DLLFullPath);
				} 
				else if(Target.Platform == UnrealTargetPlatform.Linux)
				{
					// PublicDelayLoadDLLs
					string DLLFileName = "lib" + LibFileName + ".so";
				}
				else if(Target.Platform == UnrealTargetPlatform.Mac)
				{
					// PublicDelayLoadDLLs
					string CurrentLibPath = Path.Combine(BinaryThirdPartyDirPath, "lib" + LibFileName + ".dylib");
					PublicAdditionalLibraries.Add(CurrentLibPath);

				}
				else
				{
					
				}


			}

			// PublicDefinitions
			PublicDefinitions.Add("ONNXRUNTIME_USE_DLLS");
			PublicDefinitions.Add("WITH_ONNXRUNTIME");
			PublicDefinitions.Add("ORTDEFAULT_PLATFORM_PATH=bin/" + PlatformDir);
			PublicDefinitions.Add("ORTDEFAULT_PLATFORM_BIN_PATH=" + BinaryThirdPartyDirPath.Replace('\\', '/'));
			
			if(Target.Platform == UnrealTargetPlatform.Win64)
			{
				PublicDefinitions.Add("ONNXRUNTIME_DLL_NAME=" + "onnxruntime.dll");
			}
			else if(Target.Platform == UnrealTargetPlatform.Linux)
			{
				PublicDefinitions.Add("ONNXRUNTIME_DLL_NAME=" + "libonnxruntime.so");
			}
			else if(Target.Platform == UnrealTargetPlatform.Mac)
			{
				PublicDefinitions.Add("ONNXRUNTIME_DLL_NAME=" + "libonnxruntime.dylib");
			}

			PublicDefinitions.Add("ORT_API_MANUAL_INIT");
		}
	}
}
