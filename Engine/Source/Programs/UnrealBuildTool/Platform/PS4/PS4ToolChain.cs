// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;
using System.Linq;
using Tools.DotNETCommon;

namespace UnrealBuildTool
{
	/// <summary>
	/// Option flags for the PS4 tool chain
	/// </summary>
	[Flags]
	enum PS4ToolChainOptions
	{
		/// <summary>
		/// No custom options
		/// </summary>
		None = 0,

		/// <summary>
		/// Enable the address sanitizer (ASan)
		/// </summary>
		EnableAddressSanitizer = 1,

		/// <summary>
		/// Enable the undefined behavior sanitizer (UBSan)
		/// </summary>
		EnableUndefinedBehaviorSanitizer = 2,

		/// <summary>
		/// Enables link time optimization (LTO). Link times will significantly increase.
		/// </summary>
		EnableLinkTimeOptimization = 4,
	}

	class PS4ToolChain : UEToolChain
	{
		PS4ToolChainOptions Options;
		bool bASanEnabled => Options.HasFlag(PS4ToolChainOptions.EnableAddressSanitizer);
		bool bUBSanEnabled => Options.HasFlag(PS4ToolChainOptions.EnableUndefinedBehaviorSanitizer);
		bool bLTOEnabled => Options.HasFlag(PS4ToolChainOptions.EnableLinkTimeOptimization);

		// cache the location of SDK tools
		string ClangPath;
		string SnarlPath;

		public PS4ToolChain(PS4ToolChainOptions InOptions)
			: base(CppPlatform.PS4)
		{
			Options = InOptions;

			// Make sure the SDK is installed
			string BaseSDKPath = Environment.GetEnvironmentVariable("SCE_ORBIS_SDK_DIR");
			if (String.IsNullOrEmpty(BaseSDKPath))
			{
				throw new BuildException("PS4 SDK is not installed");
			}

			BaseSDKPath = BaseSDKPath.Replace("\"", "");
			if (String.IsNullOrEmpty(BaseSDKPath))
			{
				throw new BuildException("PS4 SDK is not installed");
			}

			ClangPath = Path.Combine(BaseSDKPath, "host_tools/bin/orbis-clang.exe");
			SnarlPath = Path.Combine(BaseSDKPath, "host_tools/bin/orbis-snarl.exe");
		}

		string GetCLArguments_Global(CppCompileEnvironment CompileEnvironment)
		{
			string Result = "";

			// build up the commandline common to C and C++
			Result += " -c";
			Result += " -fdiagnostics-format=msvc";
			Result += " -Wall -Werror";
			Result += " -Wdelete-non-virtual-dtor";
			Result += " -mno-omit-leaf-frame-pointer";

			Result += " -Wno-unused-variable";
			// this will hide the warnings about static functions in headers that aren't used in every single .cpp file
			Result += " -Wno-unused-function";
			// this hides the "enumeration value 'XXXXX' not handled in switch [-Wswitch]" warnings - we should maybe remove this at some point and add UE_LOG(, Fatal, ) to default cases
			Result += " -Wno-switch";
			// this hides the "use of logical '||' with constant operand" which we use with, ie GIsEditor || IsRunningCommandlet(), which we want to be valid.
			Result += " -Wno-constant-logical-operand";
			// this hides the "warning : comparison of unsigned expression < 0 is always false" type warnings due to constant comparisons, which are possible with template arguments
			Result += " -Wno-tautological-compare";
			// this hides the "private field 'XXX' is not used", because it's not always right (see FSkeletalMeshVertexBuffer::bInflucencesByteSwapped)
			Result += " -Wno-unused-private-field";
			Result += " -Wno-invalid-offsetof"; // needed to suppress warnings about using offsetof on non-POD types.
			Result += " -Wno-inconsistent-missing-override";
			Result += " -Wno-unused-local-typedef";

			// we use this feature to allow static FNames.
			Result += " -Wno-gnu-string-literal-operator-template";

			Result += " -Wno-unused-lambda-capture";

			// Force include all the requested headers
			if(CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Include)
			{
				Result += String.Format(" -include \"{0}\"", CompileEnvironment.PrecompiledHeaderIncludeFilename);
			}
			foreach(FileItem ForceIncludeFile in CompileEnvironment.ForceIncludeFiles)
			{
				Result += String.Format(" -include \"{0}\"", ForceIncludeFile.Location);
			}

			// Profile Guided Optimization (PGO) and Link Time Optimization (LTO)
			if (CompileEnvironment.bPGOOptimize)
			{
				//
				// Clang emits a warning for each compiled function that doesn't have a matching entry in the profile data.
				// This can happen when the profile data is older than the binaries we're compiling.
				//
				// Disable this warning. It's far too verbose.
				//
				Result += " -Wno-backend-plugin";

				// Always enable LTO when using PGO.
				Log.TraceInformationOnce("Enabling Profile Guided Optimization (PGO). Linking will take a while.");
				Result += string.Format(" -fprofile-instr-use=\"{0}\"", Path.Combine(CompileEnvironment.PGODirectory, CompileEnvironment.PGOFilenamePrefix));
				Result += " -flto";
			}
			else if (CompileEnvironment.bPGOProfile)
			{
				// Always enable LTO when generating PGO profile data.
				Log.TraceInformationOnce("Enabling Profile Guided Instrumentation (PGI). Linking will take a while.");
				Result += " -fprofile-generate";
				Result += " -flto";
			}
			else if (bLTOEnabled && CompileEnvironment.bAllowLTCG)
			{
				// When not using PGO, enable LTO if opted-in by the build configuration.
				Log.TraceInformationOnce("Enabling Link Time Optimization (LTO). Linking will take a while.");
				Result += " -flto";
			}
			
			if (CompileEnvironment.bEnableShadowVariableWarnings)
			{
				Result += " -Wshadow" + (CompileEnvironment.bShadowVariableWarningsAsErrors ? "" : " -Wno-error=shadow");
			}

			if (CompileEnvironment.bEnableUndefinedIdentifierWarnings)
			{
				Result += " -Wundef" + (CompileEnvironment.bUndefinedIdentifierWarningsAsErrors ? "" : " -Wno-error=undef");
			}

			// shipping builds will cause this warning with "ensure", so disable only in those case
			if (CompileEnvironment.Configuration == CppConfiguration.Shipping)
			{
				Result += " -Wno-unused-value";
			}

			// debug info
			if (CompileEnvironment.bCreateDebugInfo)
			{
				Result += " -g";
				//                Result += " -ginlined-scopes";   // debug info for inlined functions
			}

			// Set optimization options
			if(!CompileEnvironment.bOptimizeCode)
			{
				Result += " -O0";
			}
			else if (bASanEnabled || bUBSanEnabled)
			{
				// PS4 has limited flexible memory, and ASan/UBSan both greatly inflate the in-memory size of the binary.
				// Optimize for size when ASan or UBSan is enabled to avoid insufficient-flexible-memory issues when launching the binary.
				Result += " -Os";
				Result += " -ffast-math";
			}
			else
			{
				Result += " -O3";
				Result += " -ffast-math";
				Result += " -funroll-loops";
			}

			if (!CompileEnvironment.bUseInlining)
			{
				Result += " -fno-inline-functions";
			}

			if (bASanEnabled)
			{
				Result += " -fsanitize=address";
			}
			if (bUBSanEnabled)
			{
				Result += " -fsanitize=undefined";
			}

			return Result;
		}

		static string GetCompileArguments_CPP()
		{
			string Result = "";
			Result += " -x c++";
			Result += " -std=c++14";
			return Result;

		}

		static string GetCompileArguments_C()
		{
			string Result = "";
			Result += " -x c";
			return Result;
		}

		static string GetCompileArguments_PCH()
		{
			string Result = "";
			Result += " -x c++-header";
			Result += " -std=c++14";
			return Result;
		}

		static string GetLinkArguments(LinkEnvironment LinkEnvironment)
		{
			string Result = "";

			if (LinkEnvironment.bAllowASLR == false)
			{
				Result += " -Wl,--addressing=non-aslr";
				Result += " -Wl,-Map=\"" + LinkEnvironment.IntermediateDirectory + "/Symbols.map\"";
				Result += " -Wl,-sn-full-map";
			}
			return Result;
		}

		static string GetSnarlArguments(LinkEnvironment LinkEnvironment)
		{
			return "";
		}

		public override CPPOutput CompileCPPFiles(CppCompileEnvironment CompileEnvironment, List<FileItem> InputFiles, DirectoryReference OutputDir, string ModuleName, ActionGraph ActionGraph)
		{
			string Arguments = GetCLArguments_Global(CompileEnvironment);
			string PCHArguments = "";

			// Add include paths to the argument list.
			foreach (DirectoryReference IncludePath in CompileEnvironment.IncludePaths.UserIncludePaths)
			{
				Arguments += string.Format(" -I\"{0}\"", IncludePath);
			}
			foreach (DirectoryReference IncludePath in CompileEnvironment.IncludePaths.SystemIncludePaths)
			{
				Arguments += string.Format(" -I\"{0}\"", IncludePath);
			}

			// Add preprocessor definitions to the argument list.
			foreach (string Definition in CompileEnvironment.Definitions)
			{
				string DefinitionArgument = Definition.Contains("\"") ? Definition.Replace("\"", "\\\"") : Definition;
				Arguments += string.Format(" -D \"{0}\"", DefinitionArgument);
			}

			Arguments += string.Format(" -D \"__PS4__\"");

			// Create a compile action for each source file.
			CPPOutput Result = new CPPOutput();
			foreach (FileItem SourceFile in InputFiles)
			{
				Action CompileAction = ActionGraph.Add(ActionType.Compile);
				CompileAction.PrerequisiteItems.AddRange(CompileEnvironment.ForceIncludeFiles);

				string FileArguments = "";
				bool bIsPlainCFile = Path.GetExtension(SourceFile.AbsolutePath).ToUpperInvariant() == ".C";

				// Add C or C++ specific compiler arguments.
				if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Create)
				{
					FileArguments += GetCompileArguments_PCH();
				}
				else if (bIsPlainCFile)
				{
					FileArguments += GetCompileArguments_C();
				}
				else
				{
					FileArguments += GetCompileArguments_CPP();


					// only use PCH for .cpp files
					FileArguments += PCHArguments;
				}

				// Add the C++ source file and its included files to the prerequisite item list.
				AddPrerequisiteSourceFile(CompileEnvironment, SourceFile, CompileAction.PrerequisiteItems);

				if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Create)
				{
					// Add the precompiled header file to the produced item list.
					FileItem PrecompiledHeaderFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".gch"));

					CompileAction.ProducedItems.Add(PrecompiledHeaderFile);
					Result.PrecompiledHeaderFile = PrecompiledHeaderFile;

					// Add the parameters needed to compile the precompiled header file to the command-line.
					FileArguments += string.Format(" -o \"{0}\"", PrecompiledHeaderFile.AbsolutePath, false);
				}
				else
				{
					if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Include)
					{
						CompileAction.bIsUsingPCH = true;
						CompileAction.PrerequisiteItems.Add(CompileEnvironment.PrecompiledHeaderFile);
					}

					// Add the object file to the produced item list.
					FileItem ObjectFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".o"));
					CompileAction.ProducedItems.Add(ObjectFile);
					Result.ObjectFiles.Add(ObjectFile);

					FileArguments += string.Format(" -o \"{0}\"", ObjectFile.AbsolutePath, false);
				}

				// Add the source file path to the command-line.
				FileArguments += string.Format(" \"{0}\"", SourceFile.AbsolutePath);

				// Gets the target file so we can get the correct output path.
				FileItem TargetFile = CompileAction.ProducedItems[0];

				// Creates the path to the response file using the name of the output file and creates its contents.
				FileReference ResponseFileName = new FileReference(TargetFile.AbsolutePath + ".response");
				string ResponseFileContents = Arguments + FileArguments + CompileEnvironment.AdditionalArguments;
				ResponseFileContents = (ActionThread.ExpandEnvironmentVariables(ResponseFileContents));
				List<string> InputFileNames = new List<string>();
				InputFileNames.Add(string.Format("{0}", ResponseFileContents));
				// Adds the response file to the compiler input.
				FileItem ResponseFileItem = FileItem.CreateIntermediateTextFile(ResponseFileName, InputFileNames);
				CompileAction.CommandArguments += string.Format("@\"{0}\"", ResponseFileName);
				CompileAction.PrerequisiteItems.Add(ResponseFileItem);

				CompileAction.WorkingDirectory = UnrealBuildTool.EngineSourceDirectory.FullName;
				CompileAction.CommandPath = ClangPath;
				CompileAction.StatusDescription = string.Format("{0}", Path.GetFileName(SourceFile.AbsolutePath));

				// VC++ always outputs the source file name being compiled, so we don't need to emit this ourselves
				CompileAction.bShouldOutputStatusDescription = true;

				// Don't farm out creation of precompiled headers as it is the critical path task.
				CompileAction.bCanExecuteRemotely =
					CompileEnvironment.PrecompiledHeaderAction != PrecompiledHeaderAction.Create ||
					CompileEnvironment.bAllowRemotelyCompiledPCHs;
			}

			return Result;
		}

		public override FileItem LinkFiles(LinkEnvironment LinkEnvironment, bool bBuildImportLibraryOnly, ActionGraph ActionGraph)
		{
			// Create an action that invokes the linker.
			Action LinkAction = ActionGraph.Add(ActionType.Link);
			LinkAction.WorkingDirectory = UnrealBuildTool.EngineSourceDirectory.FullName;
			LinkAction.CommandPath = LinkEnvironment.bIsBuildingLibrary ? SnarlPath : ClangPath;

			// Get link arguments.
			LinkAction.CommandArguments = LinkEnvironment.bIsBuildingLibrary ? GetSnarlArguments(LinkEnvironment) : GetLinkArguments(LinkEnvironment);


			// Add the output file as a production of the link action.
			FileItem OutputFile = FileItem.GetItemByFileReference(LinkEnvironment.OutputFilePath);
			LinkAction.ProducedItems.Add(OutputFile);
			LinkAction.StatusDescription = string.Format("{0}", Path.GetFileName(OutputFile.AbsolutePath));

			if (LinkEnvironment.bIsBuildingLibrary)
			{
				// work around SNARL behavior change.  From 1.6 -> 1.7 behavior changed from 'replace' to 'append'.
				// so delete so we don't append new .o's on top of old ones.
				LinkAction.DeleteItems.Add(OutputFile);

				// Add the output file to the command-line.
				LinkAction.CommandArguments += string.Format(" \"{0}\"", OutputFile.AbsolutePath);
			}
			else
			{
				// Add the output file to the command-line.
				LinkAction.CommandArguments += string.Format(" -o \"{0}\"", OutputFile.AbsolutePath);

				if (LinkEnvironment.Configuration != CppConfiguration.Shipping)
				{
					// Map file is also a produced item of the link
					FileReference MapFilename = FileReference.Combine(LinkEnvironment.IntermediateDirectory, "Symbols.map");
					FileItem MapOutputFile = FileItem.GetItemByFileReference(MapFilename);
					LinkAction.ProducedItems.Add(MapOutputFile);
				}
			}

			// Add the input files to a response file, and pass the response file on the command-line.
			List<string> InputFileNames = new List<string>();
			foreach (FileItem InputFile in LinkEnvironment.InputFiles)
			{
				// work around a bug with orbis-snarl.  Quoting strings inside the response file will break.  Surprisingly,
				// unquoted strings with spaces still work inside the response file.
				if (LinkEnvironment.bIsBuildingLibrary)
				{
					InputFileNames.Add(string.Format("{0}", InputFile.AbsolutePath));
				}
				else
				{
					InputFileNames.Add(string.Format("\"{0}\"", InputFile.AbsolutePath));//.Replace("\\", "/")));
				}
				LinkAction.PrerequisiteItems.Add(InputFile);
			}

			// Create a response file for the linker 
			FileReference ResponseFileName = GetResponseFileName(LinkEnvironment, OutputFile);
			FileItem ResponseFileItem = FileItem.CreateIntermediateTextFile(ResponseFileName, InputFileNames);

			LinkAction.CommandArguments += string.Format(" @\"{0}\"", ResponseFileName);
			LinkAction.PrerequisiteItems.Add(ResponseFileItem);

			// libs don't link in other libs
			if (!LinkEnvironment.bIsBuildingLibrary)
			{
				// Add the library paths to the argument list.
				foreach (DirectoryReference LibraryPath in LinkEnvironment.LibraryPaths)
				{
					LinkAction.CommandArguments += string.Format(" -L\"{0}\"", LibraryPath);
				}

				// add libraries in a library group
				LinkAction.CommandArguments += string.Format(" -Wl,--start-group");
				foreach (string AdditionalLibrary in LinkEnvironment.AdditionalLibraries)
				{
					if (String.IsNullOrEmpty(Path.GetDirectoryName(AdditionalLibrary)))
					{
						LinkAction.CommandArguments += string.Format(" \"-l{0}\"", AdditionalLibrary);
					}
					else
					{
						// full pathed libs are compiled by us, so we depend on linking them
						LinkAction.CommandArguments += string.Format(" \"{0}\"", AdditionalLibrary);
						LinkAction.PrerequisiteItems.Add(FileItem.GetItemByPath(AdditionalLibrary));
					}
				}
				LinkAction.CommandArguments += string.Format(" -Wl,--end-group");
			}

			// Add the additional arguments specified by the environment.
			LinkAction.CommandArguments += LinkEnvironment.AdditionalArguments;
			LinkAction.CommandArguments.Replace("\\", "/");

			// Only execute linking on the local PC.
			LinkAction.bCanExecuteRemotely = false;

			return OutputFile;
		}

		public override void ModifyBuildProducts(ReadOnlyTargetRules Target, UEBuildBinary Binary, List<string> Libraries, List<UEBuildBundleResource> BundleResources, Dictionary<FileReference, BuildProductType> BuildProducts)
		{
			if (Binary.Type == UEBuildBinaryType.Executable
				&& Target.Configuration == UnrealTargetConfiguration.Development)
			{
				// Tool takes a path and prefix - e.g. c:\foo\test- - and will write out c:\foo\test-symbols.bin, test-symbolnames.bin etc
				string OutputFilenameBase = System.IO.Path.Combine(Binary.OutputFilePath.Directory.FullName, @"..\..\Build\PS4\Symbols", Target.Configuration.ToString() + "-").ToLower();

				BuildProducts.Add(new FileReference(OutputFilenameBase + "symbols.bin"), BuildProductType.SymbolFile);
				BuildProducts.Add(new FileReference(OutputFilenameBase + "symbolnames.bin"), BuildProductType.SymbolFile);
				BuildProducts.Add(new FileReference(OutputFilenameBase + "symbolmetadata.txt"), BuildProductType.SymbolFile);
			}
		}

		private ICollection<FileItem> GenerateSymbols(FileItem Executable, LinkEnvironment LinkEnvironment, ActionGraph ActionGraph)
		{
			// Parse map file and output function names in SymbolNames.bin and 
			// Function Address, Function Size and Function Name offset into Symbols.bin

			string InputMapFilename = LinkEnvironment.IntermediateDirectory+ "\\Symbols.map";

			string ConfigName = LinkEnvironment.IntermediateDirectory.GetDirectoryName();

			// Tool takes a path and prefix - e.g. c:\foo\test- - and will write out c:\foo\test-symbols.bin, test-symbolnames.bin etc
			string OutputFilenameBase = System.IO.Path.Combine(LinkEnvironment.OutputFilePath.Directory.FullName, @"..\..\Build\PS4\Symbols", ConfigName + "-").ToLower();

			string SelfPath = LinkEnvironment.OutputFilePath.FullName;
			
			// Symbol tool generates three files used for lookups
			FileItem OutputSymbols = FileItem.GetItemByPath(OutputFilenameBase + "symbols.bin");
			FileItem OutputSymbolNames = FileItem.GetItemByPath(OutputFilenameBase + "symbolnames.bin");
			FileItem OutputSymbolMetaData = FileItem.GetItemByPath(OutputFilenameBase + "symbolmetadata.txt");

			Action PostBuildAction = ActionGraph.Add(ActionType.Link);
			PostBuildAction.WorkingDirectory = UnrealBuildTool.EngineSourceDirectory.FullName;
			PostBuildAction.CommandPath = Path.Combine(Directory.GetCurrentDirectory(), "../Binaries/DotNET/PS4/PS4SymbolTool.exe");

			// Can pass in -verbose for more info
			PostBuildAction.CommandArguments = string.Format("generate -map=\"{0}\" -self=\"{1}\" -output=\"{2}\"", InputMapFilename, SelfPath, OutputFilenameBase);

			// We depend on the .exe file generated by the linker
			PostBuildAction.PrerequisiteItems.Add(Executable);

			PostBuildAction.ProducedItems.AddRange(new List<FileItem>() { OutputSymbols, OutputSymbolNames, OutputSymbolMetaData });
			PostBuildAction.bPrintDebugInfo = true;
			PostBuildAction.StatusDescription = "PS4SymbolTools";

			return PostBuildAction.ProducedItems;
		}


		public override ICollection<FileItem> PostBuild(FileItem Executable, LinkEnvironment BinaryLinkEnvironment, ActionGraph ActionGraph)
		{
			List<FileItem> OutputFiles = new List<FileItem>();

			// No symbols in shipping due to ASLR
			if (BinaryLinkEnvironment.bIsBuildingLibrary == false 
				&& BinaryLinkEnvironment.Configuration != CppConfiguration.Shipping)
			{
				OutputFiles.AddRange(GenerateSymbols(Executable, BinaryLinkEnvironment, ActionGraph));
			}

			return OutputFiles;
		}
    }
	
}
