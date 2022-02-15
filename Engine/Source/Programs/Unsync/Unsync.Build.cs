// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Unsync : ModuleRules
{
	public Unsync(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		bUseUnity = false;
		bEnableExceptions = true;

		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "ThirdParty"));

		PrivateDefinitions.Add("BLAKE3_NO_SSE2=1");
		PrivateDefinitions.Add("BLAKE3_NO_AVX512=1");

		PrivateDefinitions.Add("UNSYNC_USE_TLS=1");
		PrivateDefinitions.Add("UNSYNC_USE_DEBUG_HEAP=1");

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDefinitions.Add("UNSYNC_PLATFORM_WINDOWS=1");
			PrivateDefinitions.Add("UNSYNC_USE_CONCRT=1");
			PrivateDefinitions.Add("UNSYNC_PLATFORM_UNIX=0");
			PrivateDefinitions.Add("NOMINMAX=1");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.Linux)
		{
			PrivateDefinitions.Add("UNSYNC_PLATFORM_WINDOWS=0");
			PrivateDefinitions.Add("UNSYNC_USE_CONCRT=0");
			PrivateDefinitions.Add("UNSYNC_PLATFORM_UNIX=1");
		}

		if (!IsVcPackageSupported)
		{
			return;
		}

		// AddVcPackage(string PackageName, bool AddInclude, params string[] Libraries)
		AddVcPackage("cli11", true, new string[] {});
		AddVcPackage("fmt", true, "fmt");
		AddVcPackage("http-parser", true, "http_parser");
		AddVcPackage("libressl", true, new string [] { "crypto", "ssl", "tls" });
		AddVcPackage("zstd", true, "zstd_static");
	}
}
