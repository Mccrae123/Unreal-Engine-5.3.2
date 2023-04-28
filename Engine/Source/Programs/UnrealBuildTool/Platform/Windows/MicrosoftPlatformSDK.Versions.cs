// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using System.Collections.Generic;
using System.Linq;

namespace UnrealBuildTool
{
	partial class MicrosoftPlatformSDK : UEBuildPlatformSDK
	{
		/// <summary>
		/// The default Windows SDK version to be used, if installed.
		/// </summary>
		static readonly VersionNumber[] PreferredWindowsSdkVersions = new VersionNumber[]
		{
			VersionNumber.Parse("10.0.18362.0")
		};

		/// <summary>
		/// The minimum Windows SDK version to be used. If this is null then it means there is no minimum version
		/// </summary>
		static readonly VersionNumber? MinimumWindowsSDKVersion = new VersionNumber(10, 0, 18362, 0);

		/// <summary>
		/// The maximum Windows SDK version to be used. If this is null then it means "Latest"
		/// </summary>
		static readonly VersionNumber? MaximumWindowsSDKVersion = null;

		/// <summary>
		/// The default compiler version to be used, if installed. 
		/// </summary>
		static readonly VersionNumberRange[] PreferredClangVersions =
		{
			VersionNumberRange.Parse("15.0.0", "15.999"), // VS2022 17.5.x runtime requires Clang 15
			VersionNumberRange.Parse("14.0.0", "14.999"), // VS2022 17.4.x runtime requires Clang 14
			VersionNumberRange.Parse("13.0.0", "13.999"), // VS2019 16.11 runtime requires Clang 13
		};

		static readonly VersionNumber MinimumClangVersion = new VersionNumber(13, 0, 0);

		/// <summary>
		/// Ranges of tested compiler toolchains to be used, in order of preference. If multiple toolchains in a range are present, the latest version will be preferred.
		/// Note that the numbers here correspond to the installation *folders* rather than precise executable versions. 
		/// </summary>
		static readonly VersionNumberRange[] PreferredVisualCppVersions = new VersionNumberRange[]
		{
			VersionNumberRange.Parse("14.35.32215", "14.35.99999"), // VS2022 17.5.x
			VersionNumberRange.Parse("14.34.31933", "14.34.99999"), // VS2022 17.4.x
			VersionNumberRange.Parse("14.29.30133", "14.29.99999"), // VS2019 16.11.x
		};

		/// <summary>
		/// Tested compiler toolchains that should not be allowed.
		/// </summary>
		static readonly VersionNumberRange[] BannedVisualCppVersions = new VersionNumberRange[]
		{
			VersionNumberRange.Parse("14.30.0", "14.33.99999"), // VS2022 17.0.x - 17.3.x
		};

		static readonly VersionNumber MinimumVisualCppVersion = new VersionNumber(14, 29, 30133);

		/// <summary>
		/// The default compiler version to be used, if installed. 
		/// https://www.intel.com/content/www/us/en/developer/articles/tool/oneapi-standalone-components.html#dpcpp-cpp
		/// </summary>
		static readonly VersionNumberRange[] PreferredIntelOneApiVersions =
		{
			VersionNumberRange.Parse("2023.1.0", "2023.9999"),
		};

		static readonly VersionNumber MinimumIntelOneApiVersion = new VersionNumber(2023, 0, 0);

		/// <inheritdoc/>
		public override string GetMainVersion()
		{
			// preferred/main version is the top of the Preferred list - 
			return PreferredWindowsSdkVersions.First().ToString();
		}

		/// <inheritdoc/>
		protected override void GetValidVersionRange(out string MinVersion, out string MaxVersion)
		{
			MinVersion = "10.0.00000.0";
			MaxVersion = "10.9.99999.0";
		}

		/// <inheritdoc/>
		protected override void GetValidSoftwareVersionRange(out string? MinVersion, out string? MaxVersion)
		{
			MinVersion = MinimumWindowsSDKVersion?.ToString();
			MaxVersion = MaximumWindowsSDKVersion?.ToString();
		}

		/// <summary>
		/// Whether toolchain errors should be ignored. Enable to ignore banned toolchains when generating projects,
		/// as components such as the recommended toolchain can be installed by opening the generated solution via the .vsconfig file.
		/// If enabled the error will be downgraded to a warning.
		/// </summary>
		public static bool IgnoreToolchainErrors = false;
	}
}
