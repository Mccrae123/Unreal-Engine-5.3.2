// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Microsoft.Win32;
using EpicGames.Core;
using UnrealBuildBase;
using Microsoft.Extensions.Logging;

namespace UnrealBuildTool
{
	class LinuxToolChain : ClangToolChain
	{
		/** Flavor of the current build (target triplet)*/
		string Architecture;

		/** Whether the compiler is set up to produce PIE executables by default */
		bool bSuppressPIE = false;

		/** Pass --gdb-index option to linker to generate .gdb_index section. */
		protected bool bGdbIndexSection = true;

		protected bool bPreprocessDepends = false;

		/** Allows you to override the maximum binary size allowed to be passed to objcopy.exe when cross building on Windows. */
		/** Max value is 2GB, due to bat file limitation */
		protected UInt64 MaxBinarySizeOverrideForObjcopy = 0;

		/** Platform SDK to use */
		protected LinuxPlatformSDK PlatformSDK;

		/** Toolchain information to print during the build. */
		protected string? ToolchainInfo;

		public LinuxToolChain(string InArchitecture, LinuxPlatformSDK InSDK, ClangToolChainOptions InOptions, ILogger InLogger)
			: this(UnrealTargetPlatform.Linux, InArchitecture, InSDK, InOptions, InLogger)
		{
			MultiArchRoot = PlatformSDK.GetSDKLocation();
			BaseLinuxPath = PlatformSDK.GetBaseLinuxPathForArchitecture(InArchitecture);

			bool bForceUseSystemCompiler = PlatformSDK.ForceUseSystemCompiler();
			bool bHasValidCompiler = false;

			if (bForceUseSystemCompiler)
			{
				// Validate the system toolchain.
				BaseLinuxPath = "";
				MultiArchRoot = "";

				ToolchainInfo = "system toolchain";

				// use native linux toolchain
				ClangPath = LinuxCommon.WhichClang(Logger);
				LlvmArPath = LinuxCommon.Which("llvm-ar", Logger);
				ObjcopyPath = LinuxCommon.Which("llvm-objcopy", Logger);

				// When compiling on Linux, use a faster way to relink circularly dependent libraries.
				// Race condition between actions linking to the .so and action overwriting it is avoided thanks to inodes
				bUseFixdeps = false;

				bIsCrossCompiling = false;

				bHasValidCompiler = DetermineCompilerVersion();
			}
			else
			{
				if (String.IsNullOrEmpty(BaseLinuxPath))
				{
					throw new BuildException("LINUX_MULTIARCH_ROOT environment variable is not set; cannot instantiate Linux toolchain");
				}
				if (String.IsNullOrEmpty(MultiArchRoot))
				{
					MultiArchRoot = BaseLinuxPath;
					Logger.LogInformation("Using LINUX_ROOT (deprecated, consider LINUX_MULTIARCH_ROOT)");
				}

				BaseLinuxPath = BaseLinuxPath.Replace("\"", "").Replace('\\', '/');
				ToolchainInfo = String.Format("toolchain located at '{0}'", BaseLinuxPath);

				// set up the path to our toolchain
				ClangPath = Path.Combine(BaseLinuxPath, @"bin", "clang++" + BuildHostPlatform.Current.BinarySuffix);
				LlvmArPath = Path.Combine(Path.Combine(BaseLinuxPath, String.Format("bin/{0}", "llvm-ar" + BuildHostPlatform.Current.BinarySuffix)));
				ObjcopyPath = Path.Combine(Path.Combine(BaseLinuxPath, String.Format("bin/{0}", "llvm-objcopy" + BuildHostPlatform.Current.BinarySuffix)));

				// When cross-compiling on Windows, use old FixDeps. It is slow, but it does not have timing issues
				bUseFixdeps = BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64;

				if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Linux)
				{
					Environment.SetEnvironmentVariable("LC_ALL", "C");
				}

				bIsCrossCompiling = true;

				bHasValidCompiler = DetermineCompilerVersion();
			}

			if (!bHasValidCompiler)
			{
				throw new BuildException("Could not determine version of the compiler, not registering Linux toolchain.");
			}

			CheckDefaultCompilerSettings();

			// refuse to use compilers that we know won't work

			// prevent unknown clangs since the build is likely to fail on too old or too new compilers
			if ((ClangVersionMajor * 10 + ClangVersionMinor) > 130 || (ClangVersionMajor * 10 + ClangVersionMinor) < 100)
			{
				throw new BuildException(
					string.Format("This version of the Unreal Engine can only be compiled with clang 13.0, 12.0, 11.0, and 10.0. clang {0} may not build it - please use a different version.",
						ClangVersionString)
					);
			}
		}

		public LinuxToolChain(UnrealTargetPlatform InPlatform, string InArchitecture, LinuxPlatformSDK InSDK, ClangToolChainOptions InOptions, ILogger InLogger)
			: base(InOptions, InLogger)
		{
			Architecture = InArchitecture;
			PlatformSDK = InSDK;

			// these are supplied by the engine and do not change depending on the circumstances
			DumpSymsPath = Path.Combine(Unreal.EngineDirectory.FullName, "Binaries", "Linux", "dump_syms" + BuildHostPlatform.Current.BinarySuffix);
			BreakpadEncoderPath = Path.Combine(Unreal.EngineDirectory.FullName, "Binaries", "Linux", "BreakpadSymbolEncoder" + BuildHostPlatform.Current.BinarySuffix);
		}

		public override void SetUpGlobalEnvironment(ReadOnlyTargetRules Target)
		{
			base.SetUpGlobalEnvironment(Target);

			bPreprocessDepends = Target.LinuxPlatform.bPreprocessDepends;
		}

		protected virtual bool CrossCompiling()
		{
			return bIsCrossCompiling;
		}


		/// <summary>
		/// Splits compiler version string into numerical components, leaving unchanged if not known
		/// </summary>
		private void DetermineCompilerMajMinPatchFromVersionString()
		{
			string[] Parts = ClangVersionString!.Split('.');
			if (Parts.Length >= 1)
			{
				ClangVersionMajor = Convert.ToInt32(Parts[0]);
			}
			if (Parts.Length >= 2)
			{
				ClangVersionMinor = Convert.ToInt32(Parts[1]);
			}
			if (Parts.Length >= 3)
			{
				ClangVersionPatch = Convert.ToInt32(Parts[2]);
			}
		}

		protected internal virtual string GetDumpEncodeDebugCommand(LinkEnvironment LinkEnvironment, FileItem OutputFile)
		{
			bool bUseCmdExe = BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64;
			string DumpCommand = bUseCmdExe ? "\"{0}\" \"{1}\" \"{2}\" 2>NUL" : "\"{0}\" -c -o \"{2}\" \"{1}\"";
			FileItem EncodedBinarySymbolsFile = FileItem.GetItemByPath(Path.Combine(LinkEnvironment.OutputDirectory!.FullName, OutputFile.Location.GetFileNameWithoutExtension() + ".sym"));
			FileItem SymbolsFile = FileItem.GetItemByPath(Path.Combine(LinkEnvironment.LocalShadowDirectory!.FullName, OutputFile.Location.GetFileName() + ".psym"));
			FileItem StrippedFile = FileItem.GetItemByPath(Path.Combine(LinkEnvironment.LocalShadowDirectory.FullName, OutputFile.Location.GetFileName() + "_nodebug"));
			FileItem DebugFile = FileItem.GetItemByPath(Path.Combine(LinkEnvironment.OutputDirectory.FullName, OutputFile.Location.GetFileNameWithoutExtension() + ".debug"));

			if (Options.HasFlag(ClangToolChainOptions.PreservePSYM))
			{
				SymbolsFile = FileItem.GetItemByPath(Path.Combine(LinkEnvironment.OutputDirectory.FullName, OutputFile.Location.GetFileNameWithoutExtension() + ".psym"));
			}

			StringWriter Out = new StringWriter();
			Out.NewLine = bUseCmdExe ? "\r\n" : "\n";

			// dump_syms
			Out.WriteLine(DumpCommand,
				DumpSymsPath,
				OutputFile.AbsolutePath,
				SymbolsFile.AbsolutePath
			);

			// encode breakpad symbols
			Out.WriteLine("\"{0}\" \"{1}\" \"{2}\"",
				BreakpadEncoderPath,
				SymbolsFile.AbsolutePath,
				EncodedBinarySymbolsFile.AbsolutePath
			);

			if (!Options.HasFlag(ClangToolChainOptions.DisableSplitDebugInfoWithObjCopy) && LinkEnvironment.bCreateDebugInfo)
			{
				if (MaxBinarySizeOverrideForObjcopy > 0 && bUseCmdExe)
				{
					Out.WriteLine("for /F \"tokens=*\" %%F in (\"{0}\") DO set size=%%~zF",
						OutputFile.AbsolutePath
					);

					Out.WriteLine("if %size% LSS {0} (", MaxBinarySizeOverrideForObjcopy);
				}

				// objcopy stripped file
				Out.WriteLine("\"{0}\" --strip-all \"{1}\" \"{2}\"",
					ObjcopyPath!,
					OutputFile.AbsolutePath,
					StrippedFile.AbsolutePath
				);

				// objcopy debug file
				Out.WriteLine("\"{0}\" --only-keep-debug \"{1}\" \"{2}\"",
					ObjcopyPath!,
					OutputFile.AbsolutePath,
					DebugFile.AbsolutePath
				);

				// objcopy link debug file to final so
				Out.WriteLine("\"{0}\" --add-gnu-debuglink=\"{1}\" \"{2}\" \"{3}.temp\"",
					ObjcopyPath!,
					DebugFile.AbsolutePath,
					StrippedFile.AbsolutePath,
					OutputFile.AbsolutePath
				);

				if (bUseCmdExe)
				{
					// Only move the temp final elf file once its done being linked by objcopy
					Out.WriteLine("move /Y \"{0}.temp\" \"{1}\"",
						OutputFile.AbsolutePath,
						OutputFile.AbsolutePath
					);

					if (MaxBinarySizeOverrideForObjcopy > 0)
					{
						// If we have an override size, then we need to create a dummy file if that size is exceeded
						Out.WriteLine(") ELSE (");
						Out.WriteLine("echo DummyDebug >> \"{0}\"", DebugFile.AbsolutePath);
						Out.WriteLine(")");
					}
				}
				else
				{
					// Only move the temp final elf file once its done being linked by objcopy
					Out.WriteLine("mv \"{0}.temp\" \"{1}\"",
						OutputFile.AbsolutePath,
						OutputFile.AbsolutePath
					);

					// Change the debug file to normal permissions. It was taking on the +x rights from the output file
					Out.WriteLine("chmod 644 \"{0}\"",
						DebugFile.AbsolutePath
					);
				}
			}
			else
			{
				// If we have disabled objcopy then we need to create a dummy debug file
				Out.WriteLine("echo DummyDebug >> \"{0}\"",
					DebugFile.AbsolutePath
				);
			}

			return Out.ToString();
		}

		/// <summary>
		/// Queries compiler for the version
		/// </summary>
		protected bool DetermineCompilerVersion()
		{
			ClangVersionString = null;
			ClangVersionMajor = -1;
			ClangVersionMinor = -1;
			ClangVersionPatch = -1;

			using (Process Proc = new Process())
			{
				Proc.StartInfo.UseShellExecute = false;
				Proc.StartInfo.CreateNoWindow = true;
				Proc.StartInfo.RedirectStandardOutput = true;
				Proc.StartInfo.RedirectStandardError = true;

				if (!String.IsNullOrEmpty(ClangPath))
				{
					Proc.StartInfo.FileName = ClangPath;
					Proc.StartInfo.Arguments = " --version";

					Proc.Start();
					Proc.WaitForExit();

					if (Proc.ExitCode == 0)
					{
						// read just the first string
						string? VersionString = Proc.StandardOutput.ReadLine();

						if (VersionString != null)
						{
							Regex VersionPattern = new Regex("version \\d+(\\.\\d+)+");
							Match VersionMatch = VersionPattern.Match(VersionString);

							// version match will be like "version 3.3", so remove the "version"
							if (VersionMatch.Value.StartsWith("version "))
							{
								ClangVersionString = VersionMatch.Value.Replace("version ", "");

								DetermineCompilerMajMinPatchFromVersionString();
							}
						}
					}
				}
				else
				{
					// icl?
				}
			}

			return !String.IsNullOrEmpty(ClangVersionString);
		}

		/// <summary>
		/// Checks default compiler settings
		/// </summary>
		private void CheckDefaultCompilerSettings()
		{
			using (Process Proc = new Process())
			{
				Proc.StartInfo.UseShellExecute = false;
				Proc.StartInfo.CreateNoWindow = true;
				Proc.StartInfo.RedirectStandardOutput = true;
				Proc.StartInfo.RedirectStandardError = true;
				Proc.StartInfo.RedirectStandardInput = true;

				if (!String.IsNullOrEmpty(ClangPath) && File.Exists(ClangPath))
				{
					Proc.StartInfo.FileName = ClangPath;
					Proc.StartInfo.Arguments = " -E -dM -";

					Proc.Start();
					Proc.StandardInput.Close();

					for (; ; )
					{
						string? CompilerDefine = Proc.StandardOutput.ReadLine();
						if (string.IsNullOrEmpty(CompilerDefine))
						{
							Proc.WaitForExit();
							break;
						}

						if (CompilerDefine.Contains("__PIE__") || CompilerDefine.Contains("__pie__"))
						{
							bSuppressPIE = true;
						}
					}
				}
				else
				{
					// other compilers aren't implemented atm
				}
			}
		}

		/// <summary>
		/// Architecture-specific compiler switches
		/// </summary>
		static string ArchitectureSpecificSwitches(string Architecture)
		{
			string Result = "";

			if (Architecture.StartsWith("arm") || Architecture.StartsWith("aarch64"))
			{
				Result += "-fsigned-char";
			}

			return Result;
		}

		static string ArchitectureSpecificDefines(string Architecture)
		{
			string Result = "";

			if (Architecture.StartsWith("x86_64") || Architecture.StartsWith("aarch64"))
			{
				Result += "-D_LINUX64";
			}

			return Result;
		}

		private static bool ShouldUseLibcxx(string Architecture)
		{
			// set UE_LINUX_USE_LIBCXX to either 0 or 1. If unset, defaults to 1.
			string? UseLibcxxEnvVarOverride = Environment.GetEnvironmentVariable("UE_LINUX_USE_LIBCXX");
			if (string.IsNullOrEmpty(UseLibcxxEnvVarOverride) || UseLibcxxEnvVarOverride == "1")
			{
				// at the moment ARM32 libc++ remains missing
				return Architecture.StartsWith("x86_64") || Architecture.StartsWith("aarch64");
			}
			return false;
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_WarningsAndErrors(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_WarningsAndErrors(CompileEnvironment, Arguments);

			//Arguments.Add("-Wunreachable-code");            // additional warning not normally included in Wall: warns if there is code that will never be executed - not helpful due to bIsGCC and similar

			Arguments.Add("-Wno-undefined-bool-conversion"); // hides checking if 'this' pointer is null
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_Global(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_Global(CompileEnvironment, Arguments);

			// build up the commandline common to C and C++

			if (ShouldUseLibcxx(CompileEnvironment.Architecture))
			{
				Arguments.Add("-nostdinc++");
				Arguments.Add("-I" + "ThirdParty/Unix/LibCxx/include/");
				Arguments.Add("-I" + "ThirdParty/Unix/LibCxx/include/c++/v1");
			}

			// ASan
			if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer))
			{
				// Force using the ANSI allocator if ASan is enabled
				Arguments.Add("-fsanitize=address -DFORCE_ANSI_ALLOCATOR=1");
			}

			// TSan
			if (Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer))
			{
				// Force using the ANSI allocator if TSan is enabled
				Arguments.Add("-fsanitize=thread -DFORCE_ANSI_ALLOCATOR=1");
			}

			// UBSan
			if (Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer))
			{
				Arguments.Add("-fsanitize=undefined -fno-sanitize=vptr");
			}

			// MSan
			if (Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
			{
				// Force using the ANSI allocator if MSan is enabled
				// -fsanitize-memory-track-origins adds a 1.5x-2.5x slow down ontop of MSan normal amount of overhead
				// -fsanitize-memory-track-origins=1 is faster but collects only allocation points but not intermediate stores
				Arguments.Add("-fsanitize=memory -fsanitize-memory-track-origins -DFORCE_ANSI_ALLOCATOR=1");
			}

			if (CompilerVersionGreaterOrEqual(12, 0, 0))
			{
				Arguments.Add("-fbinutils-version=2.36");
			}

			if (!CompileEnvironment.Architecture.StartsWith("x86_64"))
			{
				Arguments.Add("-funwind-tables");               // generate unwind tables as they are needed for backtrace (on x86(64) they are generated implicitly)
			}

			Arguments.Add(ArchitectureSpecificSwitches(CompileEnvironment.Architecture));

			Arguments.Add("-fno-math-errno");               // do not assume that math ops have side effects

			Arguments.Add(GetRTTIFlag(CompileEnvironment)); // flag for run-time type info

			if (CompileEnvironment.Architecture.StartsWith("x86_64"))
			{
				Arguments.Add("-mssse3"); // enable ssse3 by default for x86. This is default on for MSVC so lets reflect that here
			}

			if (CompileEnvironment.bHideSymbolsByDefault)
			{
				Arguments.Add("-fvisibility-ms-compat");
				Arguments.Add("-fvisibility-inlines-hidden");
			}

			// Profile Guided Optimization (PGO) and Link Time Optimization (LTO)
			// Whether we actually can enable that is checked in CanUseAdvancedLinkerFeatures() earlier
			if (CompileEnvironment.bPGOOptimize)
			{
				Log.TraceInformationOnce("Enabling Profile Guided Optimization (PGO). Linking will take a while.");
				Arguments.Add(string.Format(" -fprofile-instr-use=\"{0}\"", Path.Combine(CompileEnvironment.PGODirectory!, CompileEnvironment.PGOFilenamePrefix!)));
			}
			else if (CompileEnvironment.bPGOProfile)
			{
				Log.TraceInformationOnce("Enabling Profile Guided Instrumentation (PGI). Linking will take a while.");
				Arguments.Add("-fprofile-generate");
			}

			// Unlike on other platforms, allow LTO be specified independently of PGO
			// Whether we actually can enable that is checked in CanUseAdvancedLinkerFeatures() earlier
			if (CompileEnvironment.bAllowLTCG)
			{
				if ((Options & ClangToolChainOptions.EnableThinLTO) != 0)
				{
					Arguments.Add("-flto=thin");
				}
				else
				{
					Arguments.Add("-flto");
				}
			}

			//Arguments.Add("-DOPERATOR_NEW_INLINE=FORCENOINLINE");

			bool bRetainFramePointers = CompileEnvironment.bRetainFramePointers
				|| Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer) || Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer)
				|| CompileEnvironment.Configuration == CppConfiguration.Debug;

			if (CompileEnvironment.Configuration == CppConfiguration.Shipping)
			{
				if (!bRetainFramePointers)
				{
					Arguments.Add("-fomit-frame-pointer");
				}
			}
			// switches to help debugging
			else if (CompileEnvironment.Configuration == CppConfiguration.Debug)
			{
				Arguments.Add("-fno-inline");                   // disable inlining for better debuggability (e.g. callstacks, "skip file" in gdb)
				Arguments.Add("-fstack-protector");             // detect stack smashing
																//Arguments.Add("-fsanitize=address");            // detect address based errors (support properly and link to libasan)
			}

			if (bRetainFramePointers)
			{
				Arguments.Add("-fno-optimize-sibling-calls -fno-omit-frame-pointer");
			}

			// debug info
			// bCreateDebugInfo is normally set for all configurations, including Shipping - this is needed to enable callstack in Shipping builds (proper resolution: UEPLAT-205, separate files with debug info)
			if (CompileEnvironment.bCreateDebugInfo)
			{
				Arguments.Add("-gdwarf-4");

				if (bGdbIndexSection)
				{
					// Generate .debug_pubnames and .debug_pubtypes sections in a format suitable for conversion into a
					// GDB index. This option is only useful with a linker that can produce GDB index version 7.
					Arguments.Add("-ggnu-pubnames");
				}

				if (Options.HasFlag(ClangToolChainOptions.TuneDebugInfoForLLDB))
				{
					Arguments.Add("-glldb");
				}
			}

			// optimization level
			if (!CompileEnvironment.bOptimizeCode)
			{
				Arguments.Add("-O0");
			}
			else
			{
				// Don't over optimise if using Address/MemorySanitizer or you'll get false positive errors due to erroneous optimisation of necessary Address/MemorySanitizer instrumentation.
				if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer) || Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
				{
					Arguments.Add("-O1 -g");

					// This enables __asan_default_options() in UnixCommonStartup.h which disables the leak detector
					Arguments.Add("-DDISABLE_ASAN_LEAK_DETECTOR=1");
				}
				else if (Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer))
				{
					Arguments.Add("-O1 -g");
				}
				else
				{
					Arguments.Add("-O3");
				}
			}

			if (!CompileEnvironment.bUseInlining)
			{
				Arguments.Add("-fno-inline-functions");
			}

			if (CompileEnvironment.bIsBuildingDLL)
			{
				Arguments.Add("-fPIC");
				// Use local-dynamic TLS model. This generates less efficient runtime code for __thread variables, but avoids problems of running into
				// glibc/ld.so limit (DTV_SURPLUS) for number of dlopen()'ed DSOs with static TLS (see e.g. https://www.cygwin.com/ml/libc-help/2013-11/msg00033.html)
				Arguments.Add("-ftls-model=local-dynamic");
			}
			else
			{
				Arguments.Add("-ffunction-sections");
				Arguments.Add("-fdata-sections");
			}

			if (CompileEnvironment.bEnableExceptions)
			{
				Arguments.Add("-fexceptions");
				Arguments.Add("-DPLATFORM_EXCEPTIONS_DISABLED=0");
			}
			else
			{
				Arguments.Add("-fno-exceptions");               // no exceptions
				Arguments.Add("-DPLATFORM_EXCEPTIONS_DISABLED=1");
			}

			if (bSuppressPIE && !CompileEnvironment.bIsBuildingDLL)
			{
				Arguments.Add("-fno-PIE");
			}

			if (PlatformSDK.bVerboseCompiler)
			{
				Arguments.Add("-v");                            // for better error diagnosis
			}

			Arguments.Add(ArchitectureSpecificDefines(CompileEnvironment.Architecture));
			if (CrossCompiling())
			{
				if (!String.IsNullOrEmpty(CompileEnvironment.Architecture))
				{
					Arguments.Add(String.Format("-target {0}", CompileEnvironment.Architecture));        // Set target triple
				}
				Arguments.Add(String.Format("--sysroot=\"{0}\"", BaseLinuxPath));
			}
		}

		/// <inheritdoc/>
		protected override string EscapePreprocessorDefinition(string Definition)
		{
			string[] SplitData = Definition.Split('=');
			string? Key = SplitData.ElementAtOrDefault(0);
			string? Value = SplitData.ElementAtOrDefault(1);

			if (string.IsNullOrEmpty(Key)) { return ""; }
			if (!string.IsNullOrEmpty(Value))
			{
				if (!Value.StartsWith("\"") && (Value.Contains(" ") || Value.Contains("$")))
				{
					Value = Value.Trim('\"');       // trim any leading or trailing quotes
					Value = "\"" + Value + "\"";    // ensure wrap string with double quotes
				}

				// replace double quotes to escaped double quotes if exists
				Value = Value.Replace("\"", "\\\"");
			}

			return Value == null
				? string.Format("{0}", Key)
				: string.Format("{0}={1}", Key, Value);
		}

		protected virtual string GetLinkArguments(LinkEnvironment LinkEnvironment)
		{
			string Result = "";

			Result += (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64) ? " -fuse-ld=lld.exe" : " -fuse-ld=lld";

			// debugging symbols
			// Applying to all configurations @FIXME: temporary hack for FN to enable callstack in Shipping builds (proper resolution: UEPLAT-205)
			Result += " -rdynamic";   // needed for backtrace_symbols()...

			if (LinkEnvironment.bIsBuildingDLL)
			{
				Result += " -shared";
			}
			else
			{
				// ignore unresolved symbols in shared libs
				Result += string.Format(" -Wl,--unresolved-symbols=ignore-in-shared-libs");
			}

			if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
			{
				Result += " -g";

				if (Options.HasFlag(ClangToolChainOptions.EnableSharedSanitizer))
				{
					Result += " -shared-libsan";
				}

				if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer))
				{
					Result += " -fsanitize=address";
				}
				else if (Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer))
				{
					Result += " -fsanitize=thread";
				}
				else if (Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer))
				{
					Result += " -fsanitize=undefined";
				}
				else if (Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
				{
					// -fsanitize-memory-track-origins adds a 1.5x-2.5x slow ontop of MSan normal amount of overhead
					// -fsanitize-memory-track-origins=1 is faster but collects only allocation points but not intermediate stores
					Result += " -fsanitize=memory -fsanitize-memory-track-origins";
				}

				if (CrossCompiling())
				{
					Result += string.Format(" -Wl,-rpath=\"{0}/lib/clang/{1}.{2}.{3}/lib/linux\"",
							BaseLinuxPath, ClangVersionMajor, ClangVersionMinor, ClangVersionPatch);
				}
			}

			if (LinkEnvironment.bCreateDebugInfo && bGdbIndexSection)
			{
				// Generate .gdb_index section. On my machine, this cuts symbol loading time (breaking at main) from 45
				// seconds to 17 seconds (with gdb v8.3.1).
				Result += " -Wl,--gdb-index";
			}

			// RPATH for third party libs
			Result += " -Wl,-rpath=${ORIGIN}";
			Result += " -Wl,-rpath-link=${ORIGIN}";
			Result += " -Wl,-rpath=${ORIGIN}/..";   // for modules that are in sub-folders of the main Engine/Binary/Linux folder
			if (LinkEnvironment.Architecture.StartsWith("x86_64"))
			{
				Result += " -Wl,-rpath=${ORIGIN}/../../../Engine/Binaries/ThirdParty/Qualcomm/Linux";
			}
			else
			{
				// x86_64 is now using updated ICU that doesn't need extra .so
				Result += " -Wl,-rpath=${ORIGIN}/../../../Engine/Binaries/ThirdParty/ICU/icu4c-53_1/Unix/" + LinkEnvironment.Architecture;
			}

			Result += " -Wl,-rpath=${ORIGIN}/../../../Engine/Binaries/ThirdParty/OpenVR/OpenVRv1_5_17/linux64";

			// @FIXME: Workaround for generating RPATHs for launching on devices UE-54136
			Result += " -Wl,-rpath=${ORIGIN}/../../../Engine/Binaries/ThirdParty/PhysX3/Unix/x86_64-unknown-linux-gnu";
			Result += " -Wl,-rpath=${ORIGIN}/../../../Engine/Binaries/ThirdParty/Intel/Embree/Embree2140/Linux/x86_64-unknown-linux-gnu/lib";

			// Some OS ship ld with new ELF dynamic tags, which use DT_RUNPATH vs DT_RPATH. Since DT_RUNPATH do not propagate to dlopen()ed DSOs,
			// this breaks the editor on such systems. See https://kenai.com/projects/maxine/lists/users/archive/2011-01/message/12 for details
			Result += " -Wl,--disable-new-dtags";

			// This severely improves runtime linker performance. Without using FixDeps the impact on link time is not as big.
			Result += " -Wl,--as-needed";

			// Additionally speeds up editor startup by 1-2s
			Result += " -Wl,--hash-style=gnu";

			// This apparently can help LLDB speed up symbol lookups
			Result += " -Wl,--build-id";
			if (!LinkEnvironment.bIsBuildingDLL)
			{
				Result += " -Wl,--gc-sections";

				if (bSuppressPIE)
				{
					Result += " -Wl,-no-pie";
				}
			}

			// Profile Guided Optimization (PGO) and Link Time Optimization (LTO)
			// Whether we actually can enable that is checked in CanUseAdvancedLinkerFeatures() earlier
			if (LinkEnvironment.bPGOOptimize)
			{
				//
				// Clang emits a warning for each compiled function that doesn't have a matching entry in the profile data.
				// This can happen when the profile data is older than the binaries we're compiling.
				//
				// Disable this warning. It's far too verbose.
				//
				Result += " -Wno-backend-plugin";

				Log.TraceInformationOnce("Enabling Profile Guided Optimization (PGO). Linking will take a while.");
				Result += string.Format(" -fprofile-instr-use=\"{0}\"", Path.Combine(LinkEnvironment.PGODirectory!, LinkEnvironment.PGOFilenamePrefix!));
			}
			else if (LinkEnvironment.bPGOProfile)
			{
				Log.TraceInformationOnce("Enabling Profile Guided Instrumentation (PGI). Linking will take a while.");
				Result += " -fprofile-generate";
			}

			// whether we actually can do that is checked in CanUseAdvancedLinkerFeatures() earlier
			if (LinkEnvironment.bAllowLTCG)
			{
				if ((Options & ClangToolChainOptions.EnableThinLTO) != 0)
				{
					Result += String.Format(" -flto=thin -Wl,--thinlto-jobs={0}", Utils.GetPhysicalProcessorCount());
				}
				else
				{
					Result += " -flto";
				}
			}

			if (CrossCompiling())
			{
				Result += String.Format(" -target {0}", LinkEnvironment.Architecture);        // Set target triple
				string SysRootPath = BaseLinuxPath!.TrimEnd(new char[] { '\\', '/' });
				Result += String.Format(" \"--sysroot={0}\"", SysRootPath);

				// Linking with the toolchain on linux appears to not search usr/
				if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Linux)
				{
					Result += String.Format(" -B\"{0}/usr/lib/\"", SysRootPath);
					Result += String.Format(" -B\"{0}/usr/lib64/\"", SysRootPath);
					Result += String.Format(" -L\"{0}/usr/lib/\"", SysRootPath);
					Result += String.Format(" -L\"{0}/usr/lib64/\"", SysRootPath);
				}
			}

			return Result;
		}

		string GetArchiveArguments(LinkEnvironment LinkEnvironment)
		{
			return " rcs";
		}

		// cache the location of NDK tools
		protected bool bIsCrossCompiling;
		protected string? BaseLinuxPath;
		protected string? ClangPath;
		protected string? LlvmArPath;
		protected string? ObjcopyPath;
		protected string? DumpSymsPath;
		protected string? BreakpadEncoderPath;
		protected string? MultiArchRoot;

		/// <summary>
		/// Whether to use old, slower way to relink circularly dependent libraries.
		/// It makes sense to use it when cross-compiling on Windows due to race conditions between actions reading and modifying the libs.
		/// </summary>
		private bool bUseFixdeps = false;

		/// <summary>
		/// Track which scripts need to be deleted before appending to
		/// </summary>
		private bool bHasWipedFixDepsScript = false;

		/// <summary>
		/// Holds all the binaries for a particular target (except maybe the executable itself).
		/// </summary>
		private static List<FileItem> AllBinaries = new List<FileItem>();

		/// <summary>
		/// Tracks that information about used C++ library is only printed once
		/// </summary>
		private bool bHasPrintedBuildDetails = false;

		protected void PrintBuildDetails(CppCompileEnvironment CompileEnvironment, ILogger Logger)
		{
			Logger.LogInformation("------- Build details --------");
			Logger.LogInformation("Using {ToolchainInfo}.", ToolchainInfo);
			Logger.LogInformation("Using clang ({ClangPath}) version '{ClangVersionString}' (string), {ClangVersionMajor} (major), {ClangVersionMinor} (minor), {ClangVersionPatch} (patch)",
				ClangPath, ClangVersionString, ClangVersionMajor, ClangVersionMinor, ClangVersionPatch);

			// inform the user which C++ library the engine is going to be compiled against - important for compatibility with third party code that uses STL
			Logger.LogInformation("Using {Lib} standard C++ library.", ShouldUseLibcxx(CompileEnvironment.Architecture) ? "bundled libc++" : "compiler default (most likely libstdc++)");
			Logger.LogInformation("Using lld linker");
			Logger.LogInformation("Using llvm-ar ({LlvmAr})", LlvmArPath);

			if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer) ||
				Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
			{
				string SanitizerInfo = "Building with:";
				string StaticOrShared = Options.HasFlag(ClangToolChainOptions.EnableSharedSanitizer) ? " dynamically" : " statically";

				SanitizerInfo += Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer) ? StaticOrShared + " linked AddressSanitizer" : "";
				SanitizerInfo += Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer) ? StaticOrShared + " linked ThreadSanitizer" : "";
				SanitizerInfo += Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer) ? StaticOrShared + " linked UndefinedBehaviorSanitizer" : "";
				SanitizerInfo += Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer) ? StaticOrShared + " linked MemorySanitizer" : "";

				Logger.LogInformation("{SanitizerInfo}", SanitizerInfo);
			}

			// Also print other once-per-build information
			if (bUseFixdeps)
			{
				Logger.LogInformation("Using old way to relink circularly dependent libraries (with a FixDeps step).");
			}
			else
			{
				Logger.LogInformation("Using fast way to relink  circularly dependent libraries (no FixDeps).");
			}

			if (CompileEnvironment.bPGOOptimize)
			{
				Logger.LogInformation("Using PGO (profile guided optimization).");
				Logger.LogInformation("  Directory for PGO data files='{CompileEnvironmentPGODirectory}'", CompileEnvironment.PGODirectory);
				Logger.LogInformation("  Prefix for PGO data files='{CompileEnvironmentPGOFilenamePrefix}'", CompileEnvironment.PGOFilenamePrefix);
			}

			if (CompileEnvironment.bPGOProfile)
			{
				Logger.LogInformation("Using PGI (profile guided instrumentation).");
			}

			if (CompileEnvironment.bAllowLTCG)
			{
				Logger.LogInformation("Using LTO (link-time optimization).");
			}

			if (bSuppressPIE)
			{
				Logger.LogInformation("Compiler is set up to generate position independent executables by default, but we're suppressing it.");
			}
			Logger.LogInformation("------------------------------");
		}

		public override CPPOutput CompileCPPFiles(CppCompileEnvironment CompileEnvironment, List<FileItem> InputFiles, DirectoryReference OutputDir, string ModuleName, IActionGraphBuilder Graph)
		{
			List<string> GlobalArguments = new();
			GetCompileArguments_Global(CompileEnvironment, GlobalArguments);
			List<string> PCHArguments = new();

			//var BuildPlatform = UEBuildPlatform.GetBuildPlatform(CompileEnvironment.Platform);

			if (!bHasPrintedBuildDetails)
			{
				PrintBuildDetails(CompileEnvironment, Logger);

				bHasPrintedBuildDetails = true;
			}

			if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Include)
			{
				// Validate PCH inputs by content if mtime check fails
				if (CompilerVersionGreaterOrEqual(11, 0, 0))
				{
					PCHArguments.Add("-fpch-validate-input-files-content");
				}
				PCHArguments.Add(GetForceIncludeFileArgument(CompileEnvironment.PrecompiledHeaderIncludeFilename!));
			}

			// Create a compile action for each source file.
			CPPOutput Result = new CPPOutput();
			foreach (FileItem SourceFile in InputFiles)
			{
				Action CompileAction = Graph.CreateAction(ActionType.Compile);
				CompileAction.PrerequisiteItems.AddRange(CompileEnvironment.ForceIncludeFiles);
				CompileAction.PrerequisiteItems.AddRange(CompileEnvironment.AdditionalPrerequisites);

				List<string> FileArguments = new();
				string Extension = Path.GetExtension(SourceFile.AbsolutePath).ToUpperInvariant();

				// Add C or C++ specific compiler arguments.
				if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Create)
				{
					GetCompileArguments_PCH(CompileEnvironment, FileArguments);
				}
				else if (Extension == ".C")
				{
					// Compile the file as C code.
					GetCompileArguments_C(CompileEnvironment, FileArguments);
				}
				else if (Extension == ".MM")
				{
					// Compile the file as Objective-C++ code.
					GetCompileArguments_MM(CompileEnvironment, FileArguments);
					FileArguments.Add(GetRTTIFlag(CompileEnvironment));
				}
				else if (Extension == ".M")
				{
					// Compile the file as Objective-C code.
					GetCompileArguments_M(CompileEnvironment, FileArguments);
				}
				else
				{
					GetCompileArguments_CPP(CompileEnvironment, FileArguments);

					// only use PCH for .cpp files
					FileArguments.AddRange(PCHArguments);
				}

				GetCompileArguments_ForceInclude(CompileEnvironment, FileArguments);

				// Add the C++ source file and its included files to the prerequisite item list.
				CompileAction.PrerequisiteItems.Add(SourceFile);

				if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Create)
				{
					// Add the precompiled header file to the produced item list.
					FileItem PrecompiledHeaderFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".gch"));

					CompileAction.ProducedItems.Add(PrecompiledHeaderFile);
					Result.PrecompiledHeaderFile = PrecompiledHeaderFile;

					// Add the parameters needed to compile the precompiled header file to the command-line.
					FileArguments.Add(string.Format("-o \"{0}\"", NormalizeCommandLinePath(PrecompiledHeaderFile)));
				}
				else
				{
					if (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.Include)
					{
						CompileAction.PrerequisiteItems.Add(CompileEnvironment.PrecompiledHeaderFile!);
					}

					// Add the object file to the produced item list.
					FileItem ObjectFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".o"));
					CompileAction.ProducedItems.Add(ObjectFile);
					Result.ObjectFiles.Add(ObjectFile);

					FileArguments.Add(string.Format("-o \"{0}\"", NormalizeCommandLinePath(ObjectFile)));
				}

				// Add the source file path to the command-line.
				FileArguments.Add(string.Format("\"{0}\"", NormalizeCommandLinePath(SourceFile)));

				// Generate the timing info
				if (CompileEnvironment.bPrintTimingInfo)
				{
					FileItem TraceFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".json"));
					FileArguments.Add("-ftime-trace");
					CompileAction.ProducedItems.Add(TraceFile);
				}

				// Generate the included header dependency list
				if (!bPreprocessDepends && CompileEnvironment.bGenerateDependenciesFile)
				{
					FileItem DependencyListFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".d"));
					FileArguments.Add(string.Format("-MD -MF\"{0}\"", NormalizeCommandLinePath(DependencyListFile)));
					CompileAction.DependencyListFile = DependencyListFile;
					CompileAction.ProducedItems.Add(DependencyListFile);
				}

				CompileAction.WorkingDirectory = Unreal.EngineSourceDirectory;
				CompileAction.CommandPath = new FileReference(ClangPath!);

				List<string> ResponseFileContents = new();
				ResponseFileContents.AddRange(GlobalArguments);
				ResponseFileContents.AddRange(FileArguments);
				ResponseFileContents.Add(CompileEnvironment.AdditionalArguments);
				// all response lines should have / instead of \, but we cannot just bulk-replace it here since some \ are used to escape quotes, e.g. Definitions.Add("FOO=TEXT(\"Bar\")");


				Debug.Assert(CompileAction.ProducedItems.Count > 0);

				FileReference CompilerResponseFileName = CompileAction.ProducedItems[0].Location + ".rsp";
				FileItem CompilerResponseFileItem = Graph.CreateIntermediateTextFile(CompilerResponseFileName, ResponseFileContents);

				CompileAction.CommandArguments = string.Format(" @\"{0}\"", NormalizeCommandLinePath(CompilerResponseFileName));
				CompileAction.PrerequisiteItems.Add(CompilerResponseFileItem);
				CompileAction.CommandDescription = "Compile";
				CompileAction.CommandVersion = ClangVersionString!;
				CompileAction.StatusDescription = Path.GetFileName(SourceFile.AbsolutePath);
				CompileAction.bIsGCCCompiler = true;

				// Don't farm out creation of pre-compiled headers as it is the critical path task.
				CompileAction.bCanExecuteRemotely =
					CompileEnvironment.PrecompiledHeaderAction != PrecompiledHeaderAction.Create ||
					CompileEnvironment.bAllowRemotelyCompiledPCHs;

				if (bPreprocessDepends && CompileEnvironment.bGenerateDependenciesFile)
				{
					Action PrepassAction = Graph.CreateAction(ActionType.Compile);
					PrepassAction.PrerequisiteItems.AddRange(CompileAction.PrerequisiteItems);
					PrepassAction.PrerequisiteItems.Remove(CompilerResponseFileItem);
					PrepassAction.CommandDescription = "Preprocess Depends";
					PrepassAction.StatusDescription = CompileAction.StatusDescription;
					PrepassAction.bIsGCCCompiler = true;
					PrepassAction.bCanExecuteRemotely = false;
					PrepassAction.bShouldOutputStatusDescription = true;
					PrepassAction.CommandPath = CompileAction.CommandPath;
					PrepassAction.CommandVersion = CompileAction.CommandVersion;
					PrepassAction.WorkingDirectory = CompileAction.WorkingDirectory;

					List<string> PreprocessGlobalArguments = new(GlobalArguments);
					List<string> PreprocessFileArguments = new(FileArguments);
					PreprocessGlobalArguments.Remove("-c");

					FileItem DependencyListFile = FileItem.GetItemByFileReference(FileReference.Combine(OutputDir, Path.GetFileName(SourceFile.AbsolutePath) + ".d"));
					PreprocessFileArguments.Add(string.Format("-M -MF\"{0}\"", NormalizeCommandLinePath(DependencyListFile)));
					PrepassAction.DependencyListFile = DependencyListFile;
					PrepassAction.ProducedItems.Add(DependencyListFile);

					PreprocessFileArguments.Remove("-ftime-trace");
					PreprocessFileArguments.Remove(string.Format(" -o \"{0}\"", NormalizeCommandLinePath(CompileAction.ProducedItems.First())));

					PrepassAction.DeleteItems.AddRange(PrepassAction.ProducedItems);

					// Gets the target file so we can get the correct output path.
					FileItem PreprocessTargetFile = PrepassAction.ProducedItems[0];

					// Creates the path to the response file using the name of the output file and creates its contents.
					FileReference PreprocessResponseFileName = new FileReference(PreprocessTargetFile.AbsolutePath + ".rsp");
					List<string> PreprocessResponseFileContents = new();
					PreprocessResponseFileContents.AddRange(PreprocessGlobalArguments);
					PreprocessResponseFileContents.AddRange(PreprocessFileArguments);
					PreprocessResponseFileContents.Add(CompileEnvironment.AdditionalArguments);

					// Adds the response file to the compiler input.
					FileItem PreprocessResponseFileItem = Graph.CreateIntermediateTextFile(PreprocessResponseFileName, PreprocessResponseFileContents);
					PrepassAction.PrerequisiteItems.Add(PreprocessResponseFileItem);

					PrepassAction.CommandArguments = string.Format("@\"{0}\"", PreprocessResponseFileItem);
					CompileAction.DependencyListFile = DependencyListFile;
					CompileAction.PrerequisiteItems.Add(DependencyListFile);
				}
			}

			return Result;
		}

		/// <summary>
		/// Creates an action to archive all the .o files into single .a file
		/// </summary>
		public FileItem CreateArchiveAndIndex(LinkEnvironment LinkEnvironment, IActionGraphBuilder Graph, ILogger Logger)
		{
			Debugger.Launch();
			// Create an archive action
			Action ArchiveAction = Graph.CreateAction(ActionType.Link);
			ArchiveAction.WorkingDirectory = Unreal.EngineSourceDirectory;
			ArchiveAction.CommandPath = new FileReference(LlvmArPath!);

			// this will produce a final library
			ArchiveAction.bProducesImportLibrary = true;

			// Add the output file as a production of the link action.
			FileItem OutputFile = FileItem.GetItemByFileReference(LinkEnvironment.OutputFilePath);
			ArchiveAction.ProducedItems.Add(OutputFile);
			ArchiveAction.CommandDescription = "Archive";
			ArchiveAction.StatusDescription = Path.GetFileName(OutputFile.AbsolutePath);
			ArchiveAction.CommandArguments += string.Format("{1} \"{2}\"", GetArchiveArguments(LinkEnvironment), OutputFile.AbsolutePath);

			// Add the input files to a response file, and pass the response file on the command-line.
			List<string> InputFileNames = new List<string>();
			foreach (FileItem InputFile in LinkEnvironment.InputFiles)
			{
				string InputAbsolutePath = InputFile.AbsolutePath.Replace("\\", "/");
				InputFileNames.Add(string.Format("\"{0}\"", InputAbsolutePath));
				ArchiveAction.PrerequisiteItems.Add(InputFile);
			}

			// this won't stomp linker's response (which is not used when compiling static libraries)
			FileReference ResponsePath = GetResponseFileName(LinkEnvironment, OutputFile);
			if (!ProjectFileGenerator.bGenerateProjectFiles)
			{
				FileItem ResponseFileItem = Graph.CreateIntermediateTextFile(ResponsePath, InputFileNames);
				ArchiveAction.PrerequisiteItems.Add(ResponseFileItem);
			}
			ArchiveAction.CommandArguments += string.Format(" @\"{0}\"", ResponsePath.FullName);

			// Add the additional arguments specified by the environment.
			ArchiveAction.CommandArguments += LinkEnvironment.AdditionalArguments;
			ArchiveAction.CommandArguments = ArchiveAction.CommandArguments.Replace("\\", "/");

			if (BuildHostPlatform.Current.ShellType == ShellType.Sh)
			{
				ArchiveAction.CommandArguments += "'";
			}
			else
			{
				ArchiveAction.CommandArguments += "\"";
			}

			// Only execute linking on the local PC.
			ArchiveAction.bCanExecuteRemotely = false;

			return OutputFile;
		}

		public FileItem? FixDependencies(LinkEnvironment LinkEnvironment, FileItem Executable, IActionGraphBuilder Graph, ILogger Logger)
		{
			if (bUseFixdeps)
			{
				Logger.LogDebug("Adding postlink step");

				bool bUseCmdExe = BuildHostPlatform.Current.ShellType == ShellType.Cmd;
				FileReference ShellBinary = BuildHostPlatform.Current.Shell;
				string ExecuteSwitch = bUseCmdExe ? " /C" : ""; // avoid -c so scripts don't need +x
				string ScriptName = bUseCmdExe ? "FixDependencies.bat" : "FixDependencies.sh";

				FileItem FixDepsScript = FileItem.GetItemByFileReference(FileReference.Combine(LinkEnvironment.LocalShadowDirectory!, ScriptName));

				// if we never generated one we did not have a circular depends that needed fixing up
				if (!FixDepsScript.Exists)
				{
					return null;
				}

				Action PostLinkAction = Graph.CreateAction(ActionType.Link);
				PostLinkAction.WorkingDirectory = Unreal.EngineSourceDirectory;
				PostLinkAction.CommandPath = ShellBinary;
				PostLinkAction.StatusDescription = string.Format("{0}", Path.GetFileName(Executable.AbsolutePath));
				PostLinkAction.CommandDescription = "FixDeps";
				PostLinkAction.bCanExecuteRemotely = false;
				PostLinkAction.CommandArguments = ExecuteSwitch;

				PostLinkAction.CommandArguments += bUseCmdExe ? " \"" : " -c '";

				FileItem OutputFile = FileItem.GetItemByFileReference(FileReference.Combine(LinkEnvironment.LocalShadowDirectory!, Path.GetFileNameWithoutExtension(Executable.AbsolutePath) + ".link"));

				// Make sure we don't run this script until the all executables and shared libraries
				// have been built.
				PostLinkAction.PrerequisiteItems.Add(Executable);
				foreach (FileItem Dependency in AllBinaries)
				{
					PostLinkAction.PrerequisiteItems.Add(Dependency);
				}

				PostLinkAction.CommandArguments += ShellBinary + ExecuteSwitch + " \"" + FixDepsScript.AbsolutePath + "\" && ";

				// output file should not be empty or it will be rebuilt next time
				string Touch = bUseCmdExe ? "echo \"Dummy\" >> \"{0}\" && copy /b \"{0}\" +,," : "echo \"Dummy\" >> \"{0}\"";

				PostLinkAction.CommandArguments += String.Format(Touch, OutputFile.AbsolutePath);
				PostLinkAction.CommandArguments += bUseCmdExe ? "\"" : "'";

				System.Console.WriteLine("{0} {1}", PostLinkAction.CommandPath, PostLinkAction.CommandArguments);

				PostLinkAction.ProducedItems.Add(OutputFile);
				return OutputFile;
			}
			else
			{
				return null;
			}
		}

		// allow sub-platforms to modify the name of the output file
		protected virtual FileItem GetLinkOutputFile(LinkEnvironment LinkEnvironment)
		{
			return FileItem.GetItemByFileReference(LinkEnvironment.OutputFilePath);
		}


		public override FileItem LinkFiles(LinkEnvironment LinkEnvironment, bool bBuildImportLibraryOnly, IActionGraphBuilder Graph)
		{
			Debug.Assert(!bBuildImportLibraryOnly);

			List<string> RPaths = new List<string>();

			if (LinkEnvironment.bIsBuildingLibrary || bBuildImportLibraryOnly)
			{
				return CreateArchiveAndIndex(LinkEnvironment, Graph, Logger);
			}

			// Create an action that invokes the linker.
			Action LinkAction = Graph.CreateAction(ActionType.Link);
			LinkAction.WorkingDirectory = Unreal.EngineSourceDirectory;

			string LinkCommandString;
			LinkCommandString = "\"" + ClangPath + "\"";

			// Get link arguments.
			LinkCommandString += GetLinkArguments(LinkEnvironment);

			// Tell the action that we're building an import library here and it should conditionally be
			// ignored as a prerequisite for other actions
			LinkAction.bProducesImportLibrary = LinkEnvironment.bIsBuildingDLL;

			// Add the output file as a production of the link action.
			FileItem OutputFile = GetLinkOutputFile(LinkEnvironment);
			LinkAction.ProducedItems.Add(OutputFile);
			// LTO/PGO can take a lot of time, make it clear for the user
			if (LinkEnvironment.bPGOProfile)
			{
				LinkAction.CommandDescription = "Link-PGI";
			}
			else if (LinkEnvironment.bPGOOptimize)
			{
				LinkAction.CommandDescription = "Link-PGO";
			}
			else if (LinkEnvironment.bAllowLTCG)
			{
				LinkAction.CommandDescription = "Link-LTO";
			}
			else
			{
				LinkAction.CommandDescription = "Link";
			}
			// because the logic choosing between lld and ld is somewhat messy atm (lld fails to link .DSO due to bugs), make the name of the linker clear
			LinkAction.CommandDescription += (LinkCommandString.Contains("-fuse-ld=lld")) ? " (lld)" : " (ld)";
			LinkAction.CommandVersion = ClangVersionString!;
			LinkAction.StatusDescription = Path.GetFileName(OutputFile.AbsolutePath);

			// Add the output file to the command-line.
			LinkCommandString += string.Format(" -o \"{0}\"", OutputFile.AbsolutePath);

			// Add the input files to a response file, and pass the response file on the command-line.
			List<string> ResponseLines = new List<string>();
			foreach (FileItem InputFile in LinkEnvironment.InputFiles)
			{
				ResponseLines.Add(string.Format("\"{0}\"", InputFile.AbsolutePath.Replace("\\", "/")));
				LinkAction.PrerequisiteItems.Add(InputFile);
			}

			if (LinkEnvironment.bIsBuildingDLL)
			{
				ResponseLines.Add(string.Format(" -soname=\"{0}\"", OutputFile.Location.GetFileName()));
			}

			// Start with the configured LibraryPaths and also add paths to any libraries that
			// we depend on (libraries that we've build ourselves).
			List<DirectoryReference> AllLibraryPaths = LinkEnvironment.SystemLibraryPaths;

			IEnumerable<string> AdditionalLibraries = Enumerable.Concat(LinkEnvironment.SystemLibraries, LinkEnvironment.Libraries.Select(x => x.FullName));
			foreach (string AdditionalLibrary in AdditionalLibraries)
			{
				string PathToLib = Path.GetDirectoryName(AdditionalLibrary)!;
				if (!String.IsNullOrEmpty(PathToLib))
				{
					// make path absolute, because FixDependencies script may be executed in a different directory
					DirectoryReference AbsolutePathToLib = new DirectoryReference(PathToLib);
					if (!AllLibraryPaths.Contains(AbsolutePathToLib))
					{
						AllLibraryPaths.Add(AbsolutePathToLib);
					}
				}

				if ((AdditionalLibrary.Contains("Plugins") || AdditionalLibrary.Contains("Binaries/ThirdParty") || AdditionalLibrary.Contains("Binaries\\ThirdParty")) && Path.GetDirectoryName(AdditionalLibrary) != Path.GetDirectoryName(OutputFile.AbsolutePath))
				{
					string RelativePath = new FileReference(AdditionalLibrary).Directory.MakeRelativeTo(OutputFile.Location.Directory);

					if (LinkEnvironment.bIsBuildingDLL)
					{
						// Remove the root Unreal.RootDirectory from the RuntimeLibaryPath
						string AdditionalLibraryRootPath = new FileReference(AdditionalLibrary).Directory.MakeRelativeTo(Unreal.RootDirectory);

						// Figure out how many dirs we need to go back
						string RelativeRootPath = Unreal.RootDirectory.MakeRelativeTo(OutputFile.Location.Directory);

						// Combine the two together ie. number of ../ + the path after the root
						RelativePath = Path.Combine(RelativeRootPath, AdditionalLibraryRootPath);
					}

					// On Windows, MakeRelativeTo can silently fail if the engine and the project are located on different drives
					if (CrossCompiling() && RelativePath.StartsWith(Unreal.RootDirectory.FullName))
					{
						// do not replace directly, but take care to avoid potential double slashes or missed slashes
						string PathFromRootDir = RelativePath.Replace(Unreal.RootDirectory.FullName, "");
						// Path.Combine doesn't combine these properly
						RelativePath = ((PathFromRootDir.StartsWith("\\") || PathFromRootDir.StartsWith("/")) ? "..\\..\\.." : "..\\..\\..\\") + PathFromRootDir;
					}

					if (!RPaths.Contains(RelativePath))
					{
						RPaths.Add(RelativePath);
						ResponseLines.Add(string.Format(" -rpath=\"${{ORIGIN}}/{0}\"", RelativePath.Replace('\\', '/')));
					}
				}
			}

			foreach (string RuntimeLibaryPath in LinkEnvironment.RuntimeLibraryPaths)
			{
				string RelativePath = RuntimeLibaryPath;

				if (!RelativePath.StartsWith("$"))
				{
					if (LinkEnvironment.bIsBuildingDLL)
					{
						// Remove the root Unreal.RootDirectory from the RuntimeLibaryPath
						string RuntimeLibraryRootPath = new DirectoryReference(RuntimeLibaryPath).MakeRelativeTo(Unreal.RootDirectory);

						// Figure out how many dirs we need to go back
						string RelativeRootPath = Unreal.RootDirectory.MakeRelativeTo(OutputFile.Location.Directory);

						// Combine the two together ie. number of ../ + the path after the root
						RelativePath = Path.Combine(RelativeRootPath, RuntimeLibraryRootPath);
					}
					else
					{
						string RelativeRootPath = new DirectoryReference(RuntimeLibaryPath).MakeRelativeTo(Unreal.RootDirectory);

						// We're assuming that the binary will be placed according to our ProjectName/Binaries/Platform scheme
						RelativePath = Path.Combine("..", "..", "..", RelativeRootPath);
					}
				}

				// On Windows, MakeRelativeTo can silently fail if the engine and the project are located on different drives
				if (CrossCompiling() && RelativePath.StartsWith(Unreal.RootDirectory.FullName))
				{
					// do not replace directly, but take care to avoid potential double slashes or missed slashes
					string PathFromRootDir = RelativePath.Replace(Unreal.RootDirectory.FullName, "");
					// Path.Combine doesn't combine these properly
					RelativePath = ((PathFromRootDir.StartsWith("\\") || PathFromRootDir.StartsWith("/")) ? "..\\..\\.." : "..\\..\\..\\") + PathFromRootDir;
				}

				if (!RPaths.Contains(RelativePath))
				{
					RPaths.Add(RelativePath);
					ResponseLines.Add(string.Format(" -rpath=\"${{ORIGIN}}/{0}\"", RelativePath.Replace('\\', '/')));
				}
			}

			ResponseLines.Add(string.Format(" -rpath-link=\"{0}\"", Path.GetDirectoryName(OutputFile.AbsolutePath)));

			// Add the library paths to the argument list.
			foreach (DirectoryReference LibraryPath in AllLibraryPaths)
			{
				// use absolute paths because of FixDependencies script again
				ResponseLines.Add(string.Format(" -L\"{0}\"", LibraryPath.FullName.Replace('\\', '/')));
			}

			List<string> EngineAndGameLibrariesLinkFlags = new List<string>();
			List<FileItem> EngineAndGameLibrariesFiles = new List<FileItem>();

			// Pre-2.25 ld has symbol resolution problems when .so are mixed with .a in a single --start-group/--end-group
			// when linking with --as-needed.
			// Move external libraries to a separate --start-group/--end-group to fix it (and also make groups smaller and faster to link).
			// See https://github.com/EpicGames/UnrealEngine/pull/2778 and https://github.com/EpicGames/UnrealEngine/pull/2793 for discussion
			string ExternalLibraries = "";

			// add libraries in a library group
			ResponseLines.Add(string.Format(" --start-group"));

			foreach (string AdditionalLibrary in AdditionalLibraries)
			{
				if (String.IsNullOrEmpty(Path.GetDirectoryName(AdditionalLibrary)))
				{
					// library was passed just like "jemalloc", turn it into -ljemalloc
					ExternalLibraries += string.Format(" -l{0}", AdditionalLibrary);
				}
				else if (Path.GetExtension(AdditionalLibrary) == ".a")
				{
					// static library passed in, pass it along but make path absolute, because FixDependencies script may be executed in a different directory
					string AbsoluteAdditionalLibrary = Path.GetFullPath(AdditionalLibrary);
					if (AbsoluteAdditionalLibrary.Contains(" "))
					{
						AbsoluteAdditionalLibrary = string.Format("\"{0}\"", AbsoluteAdditionalLibrary);
					}
					AbsoluteAdditionalLibrary = AbsoluteAdditionalLibrary.Replace('\\', '/');

					// libcrypto/libssl contain number of functions that are being used in different DSOs. FIXME: generalize?
					if (LinkEnvironment.bIsBuildingDLL && (AbsoluteAdditionalLibrary.Contains("libcrypto") || AbsoluteAdditionalLibrary.Contains("libssl")))
					{
						ResponseLines.Add(" --whole-archive " + AbsoluteAdditionalLibrary + " --no-whole-archive");
					}
					else
					{
						ResponseLines.Add(" " + AbsoluteAdditionalLibrary);
					}

					LinkAction.PrerequisiteItems.Add(FileItem.GetItemByPath(AdditionalLibrary));
				}
				else
				{
					// Skip over full-pathed library dependencies when building DLLs to avoid circular
					// dependencies.
					FileItem LibraryDependency = FileItem.GetItemByPath(AdditionalLibrary);

					string LibName = Path.GetFileNameWithoutExtension(AdditionalLibrary);
					if (LibName.StartsWith("lib"))
					{
						// Remove lib prefix
						LibName = LibName.Remove(0, 3);
					}
					else if (LibraryDependency.Exists)
					{
						// The library exists, but it is not prefixed with "lib", so force the
						// linker to find it without that prefix by prepending a colon to
						// the file name.
						LibName = string.Format(":{0}", LibraryDependency.Name);
					}
					string LibLinkFlag = string.Format(" -l{0}", LibName);

					if (LinkEnvironment.bIsBuildingDLL && LinkEnvironment.bIsCrossReferenced)
					{
						// We are building a cross referenced DLL so we can't actually include
						// dependencies at this point. Instead we add it to the list of
						// libraries to be used in the FixDependencies step.
						EngineAndGameLibrariesLinkFlags.Add(LibLinkFlag);
						EngineAndGameLibrariesFiles.Add(LibraryDependency);
						// it is important to add this exactly to the same place where the missing libraries would have been, it will be replaced later
						if (!ExternalLibraries.Contains("--allow-shlib-undefined"))
						{
							ExternalLibraries += string.Format(" -Wl,--allow-shlib-undefined");
						}
					}
					else
					{
						LinkAction.PrerequisiteItems.Add(LibraryDependency);
						ExternalLibraries += LibLinkFlag;
					}
				}
			}
			ResponseLines.Add(" --end-group");

			FileReference ResponseFileName = GetResponseFileName(LinkEnvironment, OutputFile);
			FileItem ResponseFileItem = Graph.CreateIntermediateTextFile(ResponseFileName, ResponseLines);

			LinkCommandString += string.Format(" -Wl,@\"{0}\"", ResponseFileName);
			LinkAction.PrerequisiteItems.Add(ResponseFileItem);

			LinkCommandString += " -Wl,--start-group";
			LinkCommandString += ExternalLibraries;

			// make unresolved symbols an error, unless a) building a cross-referenced DSO  b) we opted out
			if ((!LinkEnvironment.bIsBuildingDLL || !LinkEnvironment.bIsCrossReferenced) && !LinkEnvironment.bIgnoreUnresolvedSymbols)
			{
				// This will make the linker report undefined symbols the current module, but ignore in the dependent DSOs.
				// It is tempting, but may not be possible to change that report-all - due to circular dependencies between our libs.
				LinkCommandString += " -Wl,--unresolved-symbols=ignore-in-shared-libs";
			}
			LinkCommandString += " -Wl,--end-group";

			LinkCommandString += " -lrt"; // needed for clock_gettime()
			LinkCommandString += " -lm"; // math

			if (ShouldUseLibcxx(LinkEnvironment.Architecture))
			{
				// libc++ and its abi lib
				LinkCommandString += " -nodefaultlibs";
				LinkCommandString += " -L" + "ThirdParty/Unix/LibCxx/lib/Unix/" + LinkEnvironment.Architecture + "/";
				LinkCommandString += " " + "ThirdParty/Unix/LibCxx/lib/Unix/" + LinkEnvironment.Architecture + "/libc++.a";
				LinkCommandString += " " + "ThirdParty/Unix/LibCxx/lib/Unix/" + LinkEnvironment.Architecture + "/libc++abi.a";
				LinkCommandString += " -lm";
				LinkCommandString += " -lc";
				LinkCommandString += " -lpthread"; // pthread_mutex_trylock is missing from libc stubs
				LinkCommandString += " -lgcc_s";
				LinkCommandString += " -lgcc";
			}

			// these can be helpful for understanding the order of libraries or library search directories
			if (PlatformSDK.bVerboseLinker)
			{
				LinkCommandString += " -Wl,--verbose";
				LinkCommandString += " -Wl,--trace";
				LinkCommandString += " -v";
			}

			// Add the additional arguments specified by the environment.
			LinkCommandString += LinkEnvironment.AdditionalArguments;
			LinkCommandString = LinkCommandString.Replace("\\\\", "/");
			LinkCommandString = LinkCommandString.Replace("\\", "/");

			bool bUseCmdExe = BuildHostPlatform.Current.ShellType == ShellType.Cmd;
			FileReference ShellBinary = BuildHostPlatform.Current.Shell;
			string ExecuteSwitch = bUseCmdExe ? " /C" : ""; // avoid -c so scripts don't need +x

			// Linux has issues with scripts and parameter expansion from curely brakets
			if (!bUseCmdExe)
			{
				LinkCommandString = LinkCommandString.Replace("{", "'{");
				LinkCommandString = LinkCommandString.Replace("}", "}'");
				LinkCommandString = LinkCommandString.Replace("$'{", "'${");    // fixing $'{ORIGIN}' to be '${ORIGIN}'
			}

			string LinkScriptName = string.Format((bUseCmdExe ? "Link-{0}.link.bat" : "Link-{0}.link.sh"), OutputFile.Location.GetFileName());
			string LinkScriptFullPath = Path.Combine(LinkEnvironment.LocalShadowDirectory!.FullName, LinkScriptName);
			Logger.LogDebug("Creating link script: {LinkScriptFullPath}", LinkScriptFullPath);
			Directory.CreateDirectory(Path.GetDirectoryName(LinkScriptFullPath)!);
			using (StreamWriter LinkWriter = File.CreateText(LinkScriptFullPath))
			{
				if (bUseCmdExe)
				{
					LinkWriter.NewLine = "\r\n";
					LinkWriter.WriteLine("@echo off");
					LinkWriter.WriteLine("rem Automatically generated by UnrealBuildTool");
					LinkWriter.WriteLine("rem *DO NOT EDIT*");
					LinkWriter.WriteLine();
					LinkWriter.WriteLine("set Retries=0");
					LinkWriter.WriteLine(":linkloop");
					LinkWriter.WriteLine("if %Retries% GEQ 10 goto failedtorelink");
					LinkWriter.WriteLine(LinkCommandString);
					LinkWriter.WriteLine("if %errorlevel% neq 0 goto sleepandretry");
					LinkWriter.WriteLine(GetDumpEncodeDebugCommand(LinkEnvironment, OutputFile));
					LinkWriter.WriteLine("exit 0");
					LinkWriter.WriteLine(":sleepandretry");
					LinkWriter.WriteLine("ping 127.0.0.1 -n 1 -w 5000 >NUL 2>NUL");     // timeout complains about lack of redirection
					LinkWriter.WriteLine("set /a Retries+=1");
					LinkWriter.WriteLine("goto linkloop");
					LinkWriter.WriteLine(":failedtorelink");
					LinkWriter.WriteLine("echo Failed to link {0} after %Retries% retries", OutputFile.AbsolutePath);
					LinkWriter.WriteLine("exit 1");
				}
				else
				{
					LinkWriter.NewLine = "\n";
					LinkWriter.WriteLine("#!/bin/sh");
					LinkWriter.WriteLine("# Automatically generated by UnrealBuildTool");
					LinkWriter.WriteLine("# *DO NOT EDIT*");
					LinkWriter.WriteLine();
					LinkWriter.WriteLine("set -o errexit");
					LinkWriter.WriteLine(LinkCommandString);
					LinkWriter.WriteLine(GetDumpEncodeDebugCommand(LinkEnvironment, OutputFile));
				}
			};

			LinkAction.CommandPath = ShellBinary;

			// This must maintain the quotes around the LinkScriptFullPath
			LinkAction.CommandArguments = ExecuteSwitch + " \"" + LinkScriptFullPath + "\"";

			// prepare a linker script
			FileReference LinkerScriptPath = FileReference.Combine(LinkEnvironment.LocalShadowDirectory, "remove-sym.ldscript");
			if (!DirectoryReference.Exists(LinkEnvironment.LocalShadowDirectory))
			{
				DirectoryReference.CreateDirectory(LinkEnvironment.LocalShadowDirectory);
			}
			if (FileReference.Exists(LinkerScriptPath))
			{
				FileReference.Delete(LinkerScriptPath);
			}

			// Only execute linking on the local PC.
			LinkAction.bCanExecuteRemotely = false;

			// Prepare a script that will run later, once all shared libraries and the executable
			// are created. This script will be called by action created in FixDependencies()
			if (LinkEnvironment.bIsCrossReferenced && LinkEnvironment.bIsBuildingDLL)
			{
				if (bUseFixdeps)
				{
					string ScriptName = bUseCmdExe ? "FixDependencies.bat" : "FixDependencies.sh";

					string FixDepsScriptPath = Path.Combine(LinkEnvironment.LocalShadowDirectory.FullName, ScriptName);
					if (!bHasWipedFixDepsScript)
					{
						bHasWipedFixDepsScript = true;
						Logger.LogDebug("Creating script: {FixDepsScriptPath}", FixDepsScriptPath);
						Directory.CreateDirectory(Path.GetDirectoryName(FixDepsScriptPath)!);
						using (StreamWriter Writer = File.CreateText(FixDepsScriptPath))
						{
							if (bUseCmdExe)
							{
								Writer.NewLine = "\r\n";
								Writer.WriteLine("@echo off");
								Writer.WriteLine("rem Automatically generated by UnrealBuildTool");
								Writer.WriteLine("rem *DO NOT EDIT*");
								Writer.WriteLine();
							}
							else
							{
								Writer.NewLine = "\n";
								Writer.WriteLine("#!/bin/sh");
								Writer.WriteLine("# Automatically generated by UnrealBuildTool");
								Writer.WriteLine("# *DO NOT EDIT*");
								Writer.WriteLine();
								Writer.WriteLine("set -o errexit");
							}
						}
					}

					StreamWriter FixDepsScript = File.AppendText(FixDepsScriptPath);
					FixDepsScript.NewLine = bUseCmdExe ? "\r\n" : "\n";

					string EngineAndGameLibrariesString = "";
					foreach (string Library in EngineAndGameLibrariesLinkFlags)
					{
						EngineAndGameLibrariesString += Library;
					}

					FixDepsScript.WriteLine("echo Fixing {0}", Path.GetFileName(OutputFile.AbsolutePath));
					if (!bUseCmdExe)
					{
						FixDepsScript.WriteLine("TIMESTAMP=`stat --format %y \"{0}\"`", OutputFile.AbsolutePath);
					}
					string FixDepsLine = LinkCommandString;
					string Replace = "-Wl,--allow-shlib-undefined";

					FixDepsLine = FixDepsLine.Replace(Replace, EngineAndGameLibrariesString);
					string OutputFileForwardSlashes = OutputFile.AbsolutePath.Replace("\\", "/");
					FixDepsLine = FixDepsLine.Replace(OutputFileForwardSlashes, OutputFileForwardSlashes + ".fixed");
					FixDepsLine = FixDepsLine.Replace("$", "\\$");
					FixDepsScript.WriteLine(FixDepsLine);
					if (bUseCmdExe)
					{
						FixDepsScript.WriteLine("move /Y \"{0}.fixed\" \"{0}\"", OutputFile.AbsolutePath);
					}
					else
					{
						FixDepsScript.WriteLine("mv \"{0}.fixed\" \"{0}\"", OutputFile.AbsolutePath);
						FixDepsScript.WriteLine("touch -d \"$TIMESTAMP\" \"{0}\"", OutputFile.AbsolutePath);
						FixDepsScript.WriteLine();
					}
					FixDepsScript.Close();
				}
				else
				{
					// Create the action to relink the library. This actions does not overwrite the source file so it can be executed in parallel
					Action RelinkAction = Graph.CreateAction(ActionType.Link);
					RelinkAction.WorkingDirectory = LinkAction.WorkingDirectory;
					RelinkAction.StatusDescription = LinkAction.StatusDescription;
					RelinkAction.CommandDescription = "Relink";
					RelinkAction.bCanExecuteRemotely = false;
					RelinkAction.ProducedItems.Clear();
					RelinkAction.PrerequisiteItems = new List<FileItem>(LinkAction.PrerequisiteItems);
					foreach (FileItem Dependency in EngineAndGameLibrariesFiles)
					{
						RelinkAction.PrerequisiteItems.Add(Dependency);
					}
					RelinkAction.PrerequisiteItems.Add(OutputFile); // also depend on the first link action's output

					string LinkOutputFileForwardSlashes = OutputFile.AbsolutePath.Replace("\\", "/");
					string RelinkedFileForwardSlashes = Path.Combine(LinkEnvironment.LocalShadowDirectory.FullName, OutputFile.Location.GetFileName()) + ".relinked";

					// cannot use the real product because we need to maintain the timestamp on it
					FileReference RelinkActionDummyProductRef = FileReference.Combine(LinkEnvironment.LocalShadowDirectory, LinkEnvironment.OutputFilePath.GetFileNameWithoutExtension() + ".relinked_action_ran");
					RelinkAction.ProducedItems.Add(FileItem.GetItemByFileReference(RelinkActionDummyProductRef));

					string EngineAndGameLibrariesString = "";
					foreach (string Library in EngineAndGameLibrariesLinkFlags)
					{
						EngineAndGameLibrariesString += Library;
					}

					// create the relinking step
					string RelinkScriptName = string.Format((bUseCmdExe ? "Relink-{0}.bat" : "Relink-{0}.sh"), OutputFile.Location.GetFileName());
					string RelinkScriptFullPath = Path.Combine(LinkEnvironment.LocalShadowDirectory.FullName, RelinkScriptName);

					Logger.LogDebug("Creating script: {RelinkScriptFullPath}", RelinkScriptFullPath);
					Directory.CreateDirectory(Path.GetDirectoryName(RelinkScriptFullPath)!);
					using (StreamWriter RelinkWriter = File.CreateText(RelinkScriptFullPath))
					{
						string RelinkInvocation = LinkCommandString;
						string Replace = "-Wl,--allow-shlib-undefined";
						RelinkInvocation = RelinkInvocation.Replace(Replace, EngineAndGameLibrariesString);

						// should be the same as RelinkedFileRef
						RelinkInvocation = RelinkInvocation.Replace(LinkOutputFileForwardSlashes, RelinkedFileForwardSlashes);
						RelinkInvocation = RelinkInvocation.Replace("$", "\\$");

						if (bUseCmdExe)
						{
							RelinkWriter.WriteLine("@echo off");
							RelinkWriter.WriteLine("rem Automatically generated by UnrealBuildTool");
							RelinkWriter.WriteLine("rem *DO NOT EDIT*");
							RelinkWriter.WriteLine();
							RelinkWriter.WriteLine("set Retries=0");
							RelinkWriter.WriteLine(":relinkloop");
							RelinkWriter.WriteLine("if %Retries% GEQ 10 goto failedtorelink");
							RelinkWriter.WriteLine(RelinkInvocation);
							RelinkWriter.WriteLine("if %errorlevel% neq 0 goto sleepandretry");
							RelinkWriter.WriteLine("copy /B \"{0}\" \"{1}.temp\" >NUL 2>NUL", RelinkedFileForwardSlashes, OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("if %errorlevel% neq 0 goto sleepandretry");
							RelinkWriter.WriteLine("move /Y \"{0}.temp\" \"{1}\" >NUL 2>NUL", OutputFile.AbsolutePath, OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("if %errorlevel% neq 0 goto sleepandretry");
							RelinkWriter.WriteLine(GetDumpEncodeDebugCommand(LinkEnvironment, OutputFile));
							RelinkWriter.WriteLine("echo \"Dummy\" >> \"{0}\" && copy /b \"{0}\" +,,", RelinkActionDummyProductRef.FullName);
							RelinkWriter.WriteLine("echo Relinked {0} successfully after %Retries% retries", OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("exit 0");
							RelinkWriter.WriteLine(":sleepandretry");
							RelinkWriter.WriteLine("ping 127.0.0.1 -n 1 -w 5000 >NUL 2>NUL");     // timeout complains about lack of redirection
							RelinkWriter.WriteLine("set /a Retries+=1");
							RelinkWriter.WriteLine("goto relinkloop");
							RelinkWriter.WriteLine(":failedtorelink");
							RelinkWriter.WriteLine("echo Failed to relink {0} after %Retries% retries", OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("exit 1");
						}
						else
						{
							RelinkWriter.NewLine = "\n";
							RelinkWriter.WriteLine("#!/bin/sh");
							RelinkWriter.WriteLine("# Automatically generated by UnrealBuildTool");
							RelinkWriter.WriteLine("# *DO NOT EDIT*");
							RelinkWriter.WriteLine();
							RelinkWriter.WriteLine("set -o errexit");
							RelinkWriter.WriteLine(RelinkInvocation);
							RelinkWriter.WriteLine("TIMESTAMP=`stat --format %y \"{0}\"`", OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("cp \"{0}\" \"{1}.temp\"", RelinkedFileForwardSlashes, OutputFile.AbsolutePath);
							RelinkWriter.WriteLine("mv \"{0}.temp\" \"{1}\"", OutputFile.AbsolutePath, OutputFile.AbsolutePath);
							RelinkWriter.WriteLine(GetDumpEncodeDebugCommand(LinkEnvironment, OutputFile));
							RelinkWriter.WriteLine("touch -d \"$TIMESTAMP\" \"{0}\"", OutputFile.AbsolutePath);
							RelinkWriter.WriteLine();
							RelinkWriter.WriteLine("echo \"Dummy\" >> \"{0}\"", RelinkActionDummyProductRef.FullName);
						}
					}

					RelinkAction.CommandPath = ShellBinary;
					RelinkAction.CommandArguments = ExecuteSwitch + " \"" + RelinkScriptFullPath + "\"";
				}
			}
			return OutputFile;
		}

		public override void SetupBundleDependencies(List<UEBuildBinary> Binaries, string GameName)
		{
			if (bUseFixdeps)
			{
				foreach (UEBuildBinary Binary in Binaries)
				{
					AllBinaries.Add(FileItem.GetItemByFileReference(Binary.OutputFilePath));
				}
			}
		}

		public override ICollection<FileItem> PostBuild(FileItem Executable, LinkEnvironment BinaryLinkEnvironment, IActionGraphBuilder Graph)
		{
			ICollection<FileItem> OutputFiles = base.PostBuild(Executable, BinaryLinkEnvironment, Graph);

			if (bUseFixdeps)
			{
				if (BinaryLinkEnvironment.bIsBuildingDLL || BinaryLinkEnvironment.bIsBuildingLibrary)
				{
					return OutputFiles;
				}

				FileItem? FixDepsOutputFile = FixDependencies(BinaryLinkEnvironment, Executable, Graph, Logger);
				if (FixDepsOutputFile != null)
				{
					OutputFiles.Add(FixDepsOutputFile);
				}
			}
			else
			{
				// make build product of cross-referenced DSOs to be *.relinked_action_ran, so the relinking steps are executed
				if (BinaryLinkEnvironment.bIsBuildingDLL && BinaryLinkEnvironment.bIsCrossReferenced)
				{
					FileReference RelinkedMapRef = FileReference.Combine(BinaryLinkEnvironment.LocalShadowDirectory!, BinaryLinkEnvironment.OutputFilePath.GetFileNameWithoutExtension() + ".relinked_action_ran");
					OutputFiles.Add(FileItem.GetItemByFileReference(RelinkedMapRef));
				}
			}
			return OutputFiles;
		}

		public void StripSymbols(FileReference SourceFile, FileReference TargetFile, ILogger Logger)
		{
			if (SourceFile != TargetFile)
			{
				// Strip command only works in place so we need to copy original if target is different
				File.Copy(SourceFile.FullName, TargetFile.FullName, true);
			}

			ProcessStartInfo StartInfo = new ProcessStartInfo();
			StartInfo.FileName = ObjcopyPath!;
			StartInfo.Arguments = "--strip-debug \"" + TargetFile.FullName + "\"";
			StartInfo.UseShellExecute = false;
			StartInfo.CreateNoWindow = true;
			Utils.RunLocalProcessAndLogOutput(StartInfo, Logger);
		}
	}
}
