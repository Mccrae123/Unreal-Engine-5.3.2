// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using UnrealBuildTool;
using AutomationTool;
using Tools.DotNETCommon;

namespace Turnkey.Commands
{
	class ListPlatforms : TurnkeyCommand
	{
		protected override void Execute(string[] CommandOptions)
		{
			TurnkeyUtils.Log("");
			TurnkeyUtils.Log("Valid Platforms:");
			string PlatformString = TurnkeyUtils.ParseParamValue("Platform", null, CommandOptions);

			foreach (UnrealTargetPlatform TargetPlatform in UnrealTargetPlatform.GetValidPlatforms())
			{
				// HACK UNTIUL WIN32 IS GONE COMPLETELY
				if (TargetPlatform == UnrealTargetPlatform.Win32 || TargetPlatform == UnrealTargetPlatform.XboxOne)
				{
					continue;
				}

				if (PlatformString != null && UnrealTargetPlatform.Parse(PlatformString) != TargetPlatform)
				{
					continue;
				}

				Platform Platform = Platform.Platforms[new TargetPlatformDescriptor(TargetPlatform)];
				UEBuildPlatformSDK SDK = UEBuildPlatformSDK.GetSDKForPlatform(TargetPlatform.ToString());

				string ManualSDKVersion, AutoSDKVersion;
				string MinAllowedVersion, MaxAllowedVersion;
				SDK.GetInstalledVersions(out ManualSDKVersion, out AutoSDKVersion);
				SDK.GetValidVersionRange(out MinAllowedVersion, out MaxAllowedVersion);
				
				string AllowedSoftware = Platform.GetAllowedSoftwareVersions();
				bool bIsAutoSdkValid = SDK.IsVersionValid(AutoSDKVersion, bForAutoSDK:true);
				bool bIsManualSdkValid = SDK.IsVersionValid(ManualSDKVersion, bForAutoSDK:false);
				TurnkeyUtils.Log("  Platform: {0}", TargetPlatform.ToString());
				TurnkeyUtils.Log("  Installed Manual Sdk: {0}", ManualSDKVersion);
				TurnkeyUtils.Log("  Installed Auto Sdk: {0}", AutoSDKVersion);
				TurnkeyUtils.Log("  Allowed Sdk Range: {0}-{1}", MinAllowedVersion, MaxAllowedVersion);
				TurnkeyUtils.Log("  Valid Manual SDK Installed? {0}", bIsManualSdkValid);
				TurnkeyUtils.Log("  Valid Auto SDK Installed? {0}", bIsAutoSdkValid);

				// look for available sdks
				List<FileSource> MatchingFullSdks = TurnkeyManifest.FilterDiscoveredFileSources(TargetPlatform, FileSource.SourceType.Full);
				if (MatchingFullSdks == null || MatchingFullSdks.Count == 0)
				{
					TurnkeyUtils.Log("    NO MATCHING FULL SDK FOUND!");
				}
				else
				{
					TurnkeyUtils.Log("    Possible Full Sdks that could be installed:");

					foreach (FileSource Sdk in MatchingFullSdks)
					{
						TurnkeyUtils.Log(Sdk.ToString(4));
					}
				}

				TurnkeyUtils.Log("  Allowed Device Software Version(s): {0}", AllowedSoftware);
				TurnkeyUtils.Log("  Devices: ");

				DeviceInfo[] Devices = Platform.GetDevices();
				if (Devices == null || Devices.Length == 0)
				{
					TurnkeyUtils.Log("    NO DEVICES FOUND!");
				}
				else
				{
					foreach (DeviceInfo Device in Devices)
					{
						bool bIsSoftwareValid = TurnkeyUtils.IsValueValid(Device.SoftwareVersion, AllowedSoftware, Platform);

						TurnkeyUtils.Log("    Name: {0}{1}", Device.Name, Device.bIsDefault ? "*" : "");
						TurnkeyUtils.Log("      Id: {0}", Device.Id);
						TurnkeyUtils.Log("      Type: {0}", Device.Type);
						TurnkeyUtils.Log("      Installed Software Version: {0}", Device.SoftwareVersion);
						TurnkeyUtils.Log("      Valid Software Installed?: {0}", bIsSoftwareValid);

						if (!bIsSoftwareValid)
						{
							// look for available flash
							List<FileSource> MatchingFlashSdks = TurnkeyManifest.FilterDiscoveredFileSources(TargetPlatform, FileSource.SourceType.Flash).FindAll(x => x.IsVersionValid(TargetPlatform, Device));
							if (MatchingFlashSdks == null || MatchingFlashSdks.Count == 0)
							{
								TurnkeyUtils.Log("      NO MATCHING FLASH SDK FOUND!");
							}
							else
							{
								TurnkeyUtils.Log("      Possible Flash Sdks that could be installed:");

								foreach (FileSource Sdk in MatchingFlashSdks)
								{
									TurnkeyUtils.Log(Sdk.ToString(6));
								}
							}
						}
					}
				}
				TurnkeyUtils.Log("");
			}

		}
	}
}
