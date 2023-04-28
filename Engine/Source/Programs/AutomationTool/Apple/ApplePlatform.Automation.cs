﻿// Copyright Epic Games, Inc. All Rights Reserved.
using System;
using System.Linq;
using System.Text;
using System.IO;
using AutomationTool;
using UnrealBuildTool;
using EpicGames.Core;
using UnrealBuildBase;
using System.Diagnostics;
using System.Collections.Generic;
using Microsoft.Extensions.Logging;

public abstract class ApplePlatform : Platform
{
	public ApplePlatform(UnrealTargetPlatform TargetPlatform)
		: base(TargetPlatform)
	{
	}

	#region SDK

	public override bool GetSDKInstallCommand(out string Command, out string Params, ref bool bRequiresPrivilegeElevation, ref bool bCreateWindow, ITurnkeyContext TurnkeyContext)
	{
		Command = "";
		Params = "";

		if (UnrealBuildTool.BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Mac)
		{
			TurnkeyContext.Log("Moving your original Xcode application from /Applications to the Trash, and unzipping the new version into /Applications");

			// put current Xcode in the trash, and unzip a new one. Xcode in the dock will have to be fixed up tho!
			Command = "osascript";
			Params =
				" -e \"try\"" +
				" -e   \"tell application \\\"Finder\\\" to delete POSIX file \\\"/Applications/Xcode.app\\\"\"" +
				" -e \"end try\"" +
				" -e \"do shell script \\\"cd /Applications; xip --expand $(CopyOutputPath);\\\"\"" +
				" -e \"try\"" +
				" -e   \"do shell script \\\"xcode-select -s /Applications/Xcode.app; xcode-select --install; xcodebuild -license accept; xcodebuild -runFirstLaunch\\\" with administrator privileges\"" +
				" -e \"end try\"";
		}
		else if (UnrealBuildTool.BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64)
		{

			TurnkeyContext.Log("Uninstalling old iTunes and preparing the new one to be installed.");

			Command = "$(EngineDir)/Extras/iTunes/Install_iTunes.bat";
			Params = "$(CopyOutputPath)";
		}
		return true;
	}

	public override bool OnSDKInstallComplete(int ExitCode, ITurnkeyContext TurnkeyContext, DeviceInfo Device)
	{
		if (Device == null)
		{
			if (ExitCode == 0)
			{
				if (UnrealBuildTool.BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Mac)
				{
					TurnkeyContext.PauseForUser("If you had Xcode in your Dock, you will need to remove it and add the new one (even though it was in the same location). macOS follows the move to the Trash for the Dock icon");
				}
			}
		}

		return ExitCode == 0;
	}

	#endregion


	public override void PostStagingFileCopy(ProjectParams Params, DeploymentContext SC)
	{
		if (MacExports.UseModernXcode(Params.RawProjectPath))
		{
			foreach (TargetReceipt Target in SC.StageTargets.Select(x => x.Receipt))
			{
				// fiddle with some envvars to redirect the .app into root of Staging directory, without going under any build subdirectories
				string ExtraOptions = $"SYMROOT=\"{SC.StageDirectory.ParentDirectory}\" EFFECTIVE_PLATFORM_NAME={SC.StageDirectory.GetDirectoryName()}";
				MacExports.BuildWithModernXcode(SC.RawProjectPath, Target.Platform, Target.Configuration, Target.TargetName, bArchiveForDistro: false, Logger, ExtraOptions);
			}
		}
	}

	private DirectoryReference GetArchivePath(StageTarget Target, DeploymentContext SC)
	{
		DirectoryReference UserDir = new DirectoryReference(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
		DirectoryReference Library = DirectoryReference.Combine(UserDir, "Library/Developer/Xcode/Archives");

		// order date named folders (use creating data, not name, but same thing)
		List<DirectoryReference> DateDirs = DirectoryReference.EnumerateDirectories(Library).ToList();
		DateDirs.SortBy(x => Directory.GetCreationTime(x.FullName));
		DateDirs.Reverse();


		// go through each folder, starting at most recent, looking for an archive for the target
		foreach (DirectoryReference DateDir in DateDirs)
		{
			Logger.LogInformation("Looking in Xcode archive dir {0}...", DateDir);

			// find the most recent archive for this target (based on name of target, this ignores Development vs Shipping, but 
			// since Distribution is meant only for Shipping it's ok
			string Wildcard = $"{Target.Receipt.TargetName} *.xcarchive";
			List<DirectoryReference> XcArchives = DirectoryReference.EnumerateDirectories(DateDir, Wildcard).ToList();
			if (XcArchives.Count > 0)
			{
				XcArchives.SortBy(x => Directory.GetCreationTime(x.FullName));
				DirectoryReference XcArchive = XcArchives.Last();

				Logger.LogInformation("Found xcarchive dir {0}", XcArchive);
				return XcArchive;
			}
		}

		return null;
	}

	private DirectoryReference GetFinalAppPath(StageTarget Target, DeploymentContext SC)
	{
		DirectoryReference AppDir;
		// get the executable from the receipt
		FileReference Executable = Target.Receipt.BuildProducts.First(x => x.Type == BuildProductType.Executable).Path;

		// if we are in a .app, then use that
		if (Executable.FullName.Contains(".app/"))
		{
			AppDir = Executable.Directory;
			// go up until we find the .app - Mac and IOS are different, so this handles both cases
			while (AppDir.HasExtension(".app") == false)
			{
				AppDir = AppDir.ParentDirectory;
			}
		}
		// otherwise use the .app next to the executable
		else
		{
			AppDir = new DirectoryReference(Executable.FullName + ".app");
		}

		return AppDir;
	}

	public override void Package(ProjectParams Params, DeploymentContext SC, int WorkingCL)
	{
		if (MacExports.UseModernXcode(Params.RawProjectPath))
		{
			foreach (StageTarget Target in SC.StageTargets)
			{
				TargetReceipt Receipt = Target.Receipt;

				// if we we packaging for distrbution, we will create a .xcarchive which can be used to submit to app stores, or exported for other distribution methods
				// the archive will be created in the standard Archives location accessible via Xcode. Using -archive will copy it out into
				// the specified location for use as needed
				if (MacExports.BuildWithModernXcode(Params.RawProjectPath, Receipt.Platform, Receipt.Configuration, Receipt.TargetName, Params.Distribution, Logger))
				{
					Logger.LogInformation("=====================================================================================");
					if (Params.Distribution)
					{
						Logger.LogInformation("Created .xcarchive in Xcode's Library, which can be seen in Xcode's Organizer window");
						Logger.LogInformation("You may use this to validate and prepare for vvarious distribution methods");
					}
					else
					{
						Logger.LogInformation("Finalized {App} for running fully self-contained", GetFinalAppPath(Target, SC));
					}
					Logger.LogInformation("=====================================================================================");
				}
			}
		}

		// package up the program, potentially with an installer for Mac
		PrintRunTime();
	}

	public override void GetFilesToArchive(ProjectParams Params, DeploymentContext SC)
	{
		if (!MacExports.UseModernXcode(Params.RawProjectPath))
		{
			return;
		}

		Logger.LogInformation("staging targets: '{tagets}', '{configs}'", string.Join(", ", Params.ClientCookedTargets),
			string.Join(", ", SC.StageTargetConfigurations));
		foreach (StageTarget Target in SC.StageTargets)
		{
			//// copy the xcarchive and any exported files
			//string ExportMode = "Development";
			//DirectoryReference ExportPath = GetExportPath(ExportMode, SC);

			// distribution mode we want to archive the .xcarchive that was created during Package
			DirectoryReference ArchiveSource;
			if (Params.Distribution)
			{
				// find the most recent .xcarchive in the Xcode archives library
				ArchiveSource = GetArchivePath(Target, SC);

				if (ArchiveSource == null)
				{
					Logger.LogError("Unable to find a .xcarchive in Xcode's Library to archive to {ArchiveDir}", SC.ArchiveDirectory);
					return;
				}
			}
			else
			{
				ArchiveSource = GetFinalAppPath(Target, SC);
				if (!DirectoryReference.Exists(ArchiveSource))
				{
					Logger.LogError("Unable to find the expected application ({App}) for achiving", ArchiveSource);
					return;
				}
			}

			Logger.LogInformation("=====================================================================================");
			Logger.LogInformation("Copying {Type} package {ArchiveSource} to archive directory {ArchiveDir}", Params.Distribution ? "Distribution" : "Development", ArchiveSource, SC.ArchiveDirectory);
			Logger.LogInformation("=====================================================================================");

			SC.ArchiveFiles(ArchiveSource.FullName, NewPath: ArchiveSource.GetDirectoryName());
		}
	}
}
