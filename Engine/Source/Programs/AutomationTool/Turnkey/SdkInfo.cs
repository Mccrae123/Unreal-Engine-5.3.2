// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Text;
using System.Xml.Serialization;
using System.IO;
using Tools.DotNETCommon;
using UnrealBuildTool;
using AutomationTool;
using System.Linq;

namespace Turnkey
{
	static class SdkUtils
	{
		[Flags]
		public enum LocalAvailability
		{
			None = 0,
			AutoSdk_VariableExists = 1,
			AutoSdk_ValidVersionExists = 2,
			AutoSdk_InvalidVersionExists = 4,
			// InstalledSdk_BuildOnlyWasInstalled = 8,
			InstalledSdk_ValidVersionExists = 16,
			InstalledSdk_InvalidVersionExists = 32,
			Platform_ValidHostPrerequisites = 64,
			Platform_InvalidHostPrerequisites = 128,
		}

		static public LocalAvailability GetLocalAvailability(AutomationTool.Platform AutomationPlatform, bool bAllowUpdatingPrerequisites)
		{
			LocalAvailability Result = LocalAvailability.None;

			// no need to retrieve anything if we aren't allowing updates
			CopyProviderRetriever Retriever = new CopyProviderRetriever();

			if (AutomationPlatform.UpdateHostPrerequisites(TurnkeyUtils.CommandUtilHelper, Retriever, !bAllowUpdatingPrerequisites))
			{
				Result |= LocalAvailability.Platform_ValidHostPrerequisites;
			}
			else
			{
				Result |= LocalAvailability.Platform_InvalidHostPrerequisites;
			}

			UEBuildPlatformSDK SDK = UEBuildPlatformSDK.GetSDKForPlatform(AutomationPlatform.PlatformType.ToString());

			// if we have the variable at all, 
			string AutoSdkVar = Environment.GetEnvironmentVariable("UE_SDKS_ROOT");
			if (AutoSdkVar != null)
			{
				Result |= LocalAvailability.AutoSdk_VariableExists;

				// get platform subdirectory
				string AutoSubdir = string.Format("Host{0}/{1}", HostPlatform.Current.HostEditorPlatform.ToString(), SDK.GetAutoSDKPlatformName());
				DirectoryInfo PlatformDir = new DirectoryInfo(Path.Combine(AutoSdkVar, AutoSubdir));
				if (PlatformDir.Exists)
				{
					bool bValidVersionFound = false;
					foreach (DirectoryInfo Dir in PlatformDir.EnumerateDirectories())
					{
						// check for valid version number (auto SDK must match version exactly)
						if (string.Compare(Dir.Name, SDK.GetDesiredVersion()) == 0)
						{
							// make sure it actually has buits in it
							if (File.Exists(Path.Combine(Dir.FullName, "setup.bat")))
							{
								bValidVersionFound = true;
								break;
							}
						}
					}

					// if we had a platform directory, but no valid versions, note that
					Result |= bValidVersionFound ? LocalAvailability.AutoSdk_ValidVersionExists : LocalAvailability.AutoSdk_InvalidVersionExists;
				}
			}

			// if anything is installed, this will return a value
			string InstalledVersion = SDK.GetInstalledVersion();
			if (!string.IsNullOrEmpty(InstalledVersion))
			{
				if (SDK.IsVersionValid(InstalledVersion, bForAutoSDK: false))
				{
					Result |= LocalAvailability.InstalledSdk_ValidVersionExists;
				}
				else
				{
					Result |= LocalAvailability.InstalledSdk_InvalidVersionExists;
				}
			}


			return Result;
		}


		public static bool SetupAutoSdk(FileSource Source, UnrealTargetPlatform Platform, bool bUnattended)
		{
			bool bAttemptAutoSdkSetup = false;
			bool bSetupEnvVarAfterInstall = false;
			if (Environment.GetEnvironmentVariable("UE_SDKS_ROOT") != null)
			{
				bAttemptAutoSdkSetup = true;
			}
			else
			{
				if (!bUnattended)
				{
					// @todo turnkey - have studio settings 
					string Response = TurnkeyUtils.ReadInput("AutoSdks are not setup, but your studio has support. Would you like to set it up now? [Y/n]", "Y");
					if (string.Compare(Response, "Y", true) == 0)
					{
						bAttemptAutoSdkSetup = true;
						bSetupEnvVarAfterInstall = true;
					}
				}
			}

			if (bAttemptAutoSdkSetup)
			{
				AutomationTool.Platform AutomationPlatform = AutomationTool.Platform.GetPlatform(Platform);

				TurnkeyUtils.Log("{0}: AutoSdk is setup on this computer, will look for available AutoSdk to download", Platform);

				// make sure this is unset so that we can know if it worked or not after install
				TurnkeyUtils.ClearVariable("CopyOutputPath");

				// now download it (AutoSdks don't "install") on download
				// @todo turnkey: handle errors, handle p4 going to wrong location, handle one Sdk for multiple platforms
				string CopyOperation = Source.GetCopySourceOperation();
				if (CopyOperation == null)
				{
					return false;
				}

				// download the AutoSDK
				string DownloadedRoot = CopyProvider.ExecuteCopy(CopyOperation);
				if (DownloadedRoot == null)
				{
					return false;
				}

				if (bSetupEnvVarAfterInstall)
				{
					// failed to install, nothing we can do
					if (string.IsNullOrEmpty(DownloadedRoot))
					{
						TurnkeyUtils.ExitCode = AutomationTool.ExitCode.Error_SDKNotFound;
						return false;
					}

					// walk up to one above Host* directory
					DirectoryInfo AutoSdkSearch;
					if (Directory.Exists(DownloadedRoot))
					{
						AutoSdkSearch = new DirectoryInfo(DownloadedRoot);
					}
					else
					{
						AutoSdkSearch = new FileInfo(DownloadedRoot).Directory;
					}
					while (AutoSdkSearch.Name != "Host" + HostPlatform.Current.HostEditorPlatform.ToString())
					{
						AutoSdkSearch = AutoSdkSearch.Parent;
					}

					// now go one up to the parent of Host
					AutoSdkSearch = AutoSdkSearch.Parent;

					string AutoSdkDir = AutoSdkSearch.FullName;
					if (!bUnattended)
					{
						string Response = TurnkeyUtils.ReadInput("Enter directory for root of AutoSdks. Use detected value, or enter another:", AutoSdkSearch.FullName);
						if (string.IsNullOrEmpty(Response))
						{
							return false;
						}
					}

					// set the env var, globally
					TurnkeyUtils.StartTrackingExternalEnvVarChanges();
					Environment.SetEnvironmentVariable("UE_SDKS_ROOT", AutoSdkDir);
					Environment.SetEnvironmentVariable("UE_SDKS_ROOT", AutoSdkDir, EnvironmentVariableTarget.User);
					TurnkeyUtils.EndTrackingExternalEnvVarChanges();

				}

				// and now activate it in case we need it this run
				TurnkeyUtils.Log("Re-activating AutoSDK '{0}'...", Source.DisplayName);

				UEBuildPlatformSDK.GetSDKForPlatform(Platform.ToString()).ReactivateAutoSDK();

				return true;
			}

			return false;
		}
	}
}
