// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Enumeration;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.UHT.Parsers;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Types;
using Microsoft.Extensions.Logging;

namespace EpicGames.UHT.Utils
{

	/// <summary>
	/// To support the testing framework, source files can be containing in other source files.  
	/// A source fragment represents this possibility.
	/// </summary>
	public struct UhtSourceFragment
	{

		/// <summary>
		/// When not null, this source comes from another source file
		/// </summary>
		public UhtSourceFile? SourceFile { get; set; }

		/// <summary>
		/// The file path of the source
		/// </summary>
		public string FilePath { get; set; }

		/// <summary>
		/// The line number of the fragment in the containing source file.
		/// </summary>
		public int LineNumber { get; set; }

		/// <summary>
		/// Data of the source file
		/// </summary>
		public StringView Data { get; set; }
	}

	/// <summary>
	/// Implementation of the export factory
	/// </summary>
	class UhtExportFactory : IUhtExportFactory
	{
		public struct Output
		{
			public string _filePath;
			public string _tempFilePath;
			public bool _saved;
		}

		/// <summary>
		/// UHT session
		/// </summary>
		private readonly UhtSession _sessionInternal;

		/// <summary>
		/// Module associated with the plugin
		/// </summary>
		private readonly UHTManifest.Module? _pluginModuleInternal;

		/// <summary>
		/// Limiter for the number of files being saved to the reference directory.
		/// The OS can get swamped on high core systems
		/// </summary>
		private static readonly Semaphore s_writeRefSemaphore = new Semaphore(32, 32);

		/// <summary>
		/// Requesting exporter
		/// </summary>
		public readonly UhtExporter Exporter;

		/// <summary>
		/// UHT Session
		/// </summary>
		public UhtSession Session => this._sessionInternal;

		/// <summary>
		/// Plugin module
		/// </summary>
		public UHTManifest.Module? PluginModule => this._pluginModuleInternal;

		/// <summary>
		/// Collection of error from mismatches with the reference files
		/// </summary>
		public Dictionary<string, bool> ReferenceErrorMessages { get; } = new Dictionary<string, bool>();

		/// <summary>
		/// List of export outputs
		/// </summary>
		public List<Output> Outputs { get; } = new List<Output>();

		/// <summary>
		/// Directory for the reference output
		/// </summary>
		public string ReferenceDirectory { get; set; } = String.Empty;

		/// <summary>
		/// Directory for the verify output
		/// </summary>
		public string VerifyDirectory { get; set; } = String.Empty;

		/// <summary>
		/// Collection of external dependencies
		/// </summary>
		public HashSet<string> ExternalDependencies { get; } = new HashSet<string>();

		/// <summary>
		/// Create a new instance of the export factory
		/// </summary>
		/// <param name="session">UHT session</param>
		/// <param name="pluginModule">Plugin module</param>
		/// <param name="exporter">Exporter being run</param>
		public UhtExportFactory(UhtSession session, UHTManifest.Module? pluginModule, UhtExporter exporter)
		{
			this._sessionInternal = session;
			this._pluginModuleInternal = pluginModule;
			this.Exporter = exporter;
			if (this.Session.ReferenceMode != UhtReferenceMode.None)
			{
				this.ReferenceDirectory = Path.Combine(this.Session.ReferenceDirectory, this.Exporter.Name);
				this.VerifyDirectory = Path.Combine(this.Session.VerifyDirectory, this.Exporter.Name);
				Directory.CreateDirectory(this.Session.ReferenceMode == UhtReferenceMode.Reference ? this.ReferenceDirectory : this.VerifyDirectory);
			}
		}

		/// <summary>
		/// Commit the contents of the string builder as the output.
		/// If you have a string builder, use this method so that a 
		/// temporary buffer can be used.
		/// </summary>
		/// <param name="filePath">Destination file path</param>
		/// <param name="builder">Source for the content</param>
		public void CommitOutput(string filePath, StringBuilder builder)
		{
			using (UhtBorrowBuffer borrowBuffer = new UhtBorrowBuffer(builder))
			{
				string tempFilePath = filePath + ".tmp";
				SaveIfChanged(filePath, tempFilePath, new StringView(borrowBuffer.Buffer.Memory));
			}
		}

		/// <summary>
		/// Commit the value of the string as the output
		/// </summary>
		/// <param name="filePath">Destination file path</param>
		/// <param name="output">Output to commit</param>
		public void CommitOutput(string filePath, StringView output)
		{
			string tempFilePath = filePath + ".tmp";
			SaveIfChanged(filePath, tempFilePath, output);
		}

		/// <summary>
		/// Create a task to export two files
		/// </summary>
		/// <param name="prereqs">Tasks that must be completed prior to this task running</param>
		/// <param name="action">Action to be invoked to generate the output</param>
		/// <returns>Task object or null if the task was immediately executed.</returns>
		public Task? CreateTask(List<Task?>? prereqs, UhtExportTaskDelegate action)
		{
			if (this.Session.GoWide)
			{
				Task[]? prereqTasks = prereqs?.Where(x => x != null).Cast<Task>().ToArray();
				if (prereqTasks != null && prereqTasks.Length > 0)
				{
					return Task.Factory.ContinueWhenAll(prereqTasks, (Task[] tasks) => { action(this); });
				}
				else
				{
					return Task.Factory.StartNew(() => { action(this); }, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
				}
			}
			else
			{
				action(this);
				return null;
			}
		}

		/// <summary>
		/// Create a task to export two files
		/// </summary>
		/// <param name="action">Action to be invoked to generate the output</param>
		/// <returns>Task object or null if the task was immediately executed.</returns>
		public Task? CreateTask(UhtExportTaskDelegate action)
		{
			return CreateTask(null, action);
		}

		/// <summary>
		/// Given a header file, generate the output file name.
		/// </summary>
		/// <param name="headerFile">Header file</param>
		/// <param name="suffix">Suffix/extension to be added to the file name.</param>
		/// <returns>Resulting file name</returns>
		public string MakePath(UhtHeaderFile headerFile, string suffix)
		{
			return MakePath(headerFile.Package.Module, headerFile.FileNameWithoutExtension, suffix);
		}

		/// <summary>
		/// Given a package file, generate the output file name
		/// </summary>
		/// <param name="package">Package file</param>
		/// <param name="suffix">Suffix/extension to be added to the file name.</param>
		/// <returns>Resulting file name</returns>
		public string MakePath(UhtPackage package, string suffix)
		{
			return MakePath(package.Module, package.ShortName, suffix);
		}

		/// <summary>
		/// Make a path for an output based on the package output directory.
		/// </summary>
		/// <param name="fileName">Name of the file</param>
		/// <param name="extension">Extension to add to the file</param>
		/// <returns>Output file path</returns>
		public string MakePath(string fileName, string extension)
		{
			if (this.PluginModule == null)
			{
				throw new UhtIceException("MakePath with just a filename and extension can not be called from non-plugin exporters");
			}
			return MakePath(this.PluginModule, fileName, extension);
		}

		/// <summary>
		/// Add an external dependency to the given file path
		/// </summary>
		/// <param name="filePath">External dependency to add</param>
		public void AddExternalDependency(string filePath)
		{
			this.ExternalDependencies.Add(filePath);
		}

		private string MakePath(UHTManifest.Module module, string fileName, string suffix)
		{
			if (PluginModule != null)
			{
				module = PluginModule;
			}
			return Path.Combine(module.OutputDirectory, fileName) + suffix;
		}

		/// <summary>
		/// Helper method to test to see if the output has changed.
		/// </summary>
		/// <param name="filePath">Name of the output file</param>
		/// <param name="tempFilePath">Name of the temporary file</param>
		/// <param name="exported">Exported contents of the file</param>
		internal void SaveIfChanged(string filePath, string tempFilePath, StringView exported)
		{

			ReadOnlySpan<char> exportedSpan = exported.Span;

			if (this.Session.ReferenceMode != UhtReferenceMode.None)
			{
				string fileName = Path.GetFileName(filePath);

				// Writing billions of files to the same directory causes issues.  Use ourselves to throttle reference writes
				try
				{
					UhtExportFactory.s_writeRefSemaphore.WaitOne();
					{
						string outPath = Path.Combine(this.Session.ReferenceMode == UhtReferenceMode.Reference ? this.ReferenceDirectory : this.VerifyDirectory, fileName);
						if (!this.Session.WriteSource(outPath, exported.Span))
						{
							new UhtSimpleFileMessageSite(this.Session, outPath).LogWarning($"Unable to write reference file {outPath}");
						}
					}
				}
				finally
				{
					UhtExportFactory.s_writeRefSemaphore.Release();
				}

				// If we are verifying, read the existing file and check the contents
				if (this.Session.ReferenceMode == UhtReferenceMode.Verify)
				{
					string message = String.Empty;
					string refPath = Path.Combine(this.ReferenceDirectory, fileName);
					UhtBuffer? existingRef = this.Session.ReadSourceToBuffer(refPath);
					if (existingRef != null)
					{
						ReadOnlySpan<char> existingSpan = existingRef.Memory.Span;
						if (existingSpan.CompareTo(exportedSpan, StringComparison.Ordinal) != 0)
						{
							message = $"********************************* {fileName} has changed.";
						}
						UhtBuffer.Return(existingRef);
					}
					else
					{
						message = $"********************************* {fileName} appears to be a new generated file.";
					}

					if (!String.IsNullOrEmpty(message))
					{
						Log.TraceInformation(message);
						lock (this.ReferenceErrorMessages)
						{
							this.ReferenceErrorMessages.Add(message, true);
						}
					}
				}
			}

			// Check to see if the contents have changed
			UhtBuffer? original = this.Session.ReadSourceToBuffer(filePath);
			bool save = original == null;
			if (original != null)
			{
				ReadOnlySpan<char> originalSpan = original.Memory.Span;
				if (originalSpan.CompareTo(exportedSpan, StringComparison.Ordinal) != 0)
				{
					if (this.Session.FailIfGeneratedCodeChanges)
					{
						string conflictPath = filePath + ".conflict";
						if (!this.Session.WriteSource(conflictPath, exported.Span))
						{
							new UhtSimpleFileMessageSite(this.Session, filePath).LogError($"Changes to generated code are not allowed - conflicts written to '{conflictPath}'");
						}
					}
					save = true;
				}
				UhtBuffer.Return(original);
			}

			// If changed of the original didn't exist, then save the new version
			if (save && !this.Session.NoOutput)
			{
				if (!this.Session.WriteSource(tempFilePath, exported.Span))
				{
					new UhtSimpleFileMessageSite(this.Session, filePath).LogWarning($"Failed to save export file: '{tempFilePath}'");
				}
			}
			else
			{
				save = false;
			}

			// Add this to the list of outputs
			lock (this.Outputs)
			{
				this.Outputs.Add(new Output { _filePath = filePath, _tempFilePath = tempFilePath, _saved = save });
			}
		}

		/// <summary>
		/// Run the output exporter
		/// </summary>
		public void Run()
		{

			// Invoke the exported via the delegate
			this.Exporter.Delegate(this);

			// If outputs were exported
			if (this.Outputs.Count > 0)
			{

				// These outputs are used to cull old outputs from the directories
				Dictionary<string, HashSet<string>> outputsByDirectory = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
				List<UhtExportFactory.Output> saves = new List<UhtExportFactory.Output>();

				// Collect information about the outputs
				foreach (UhtExportFactory.Output output in this.Outputs)
				{

					// Add this output to the list of expected outputs by directory
					string? fileDirectory = Path.GetDirectoryName(output._filePath);
					if (fileDirectory != null)
					{
						HashSet<string>? files;
						if (!outputsByDirectory.TryGetValue(fileDirectory, out files))
						{
							files = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
							outputsByDirectory.Add(fileDirectory, files);
						}
						files.Add(Path.GetFileName(output._filePath));
					}

					// Add the save task
					if (output._saved)
					{
						saves.Add(output);
					}
				}

				// Perform the renames
				if (this.Session.GoWide)
				{
					Parallel.ForEach(saves, (UhtExportFactory.Output output) =>
					{
						RenameSource(output);
					});
				}
				else
				{
					foreach (UhtExportFactory.Output output in saves)
					{
						RenameSource(output);
					}
				}

				// Perform the culling of the output directories
				if (this.Session.CullOutput && !this.Session.NoOutput &&
					(this.Exporter.CppFilters != null || this.Exporter.HeaderFilters != null || this.Exporter.OtherFilters != null))
				{
					if (this.Session.GoWide)
					{
						Parallel.ForEach(outputsByDirectory, (KeyValuePair<string, HashSet<string>> kvp) =>
						{
							CullOutputDirectory(kvp.Key, kvp.Value);
						});
					}
					else
					{
						foreach (KeyValuePair<string, HashSet<string>> kvp in outputsByDirectory)
						{
							CullOutputDirectory(kvp.Key, kvp.Value);
						}
					}
				}
			}
		}

		/// <summary>
		/// Given an output, rename the output file from the temporary file name to the final file name.
		/// If there exists a current final file, it will be replaced.
		/// </summary>
		/// <param name="output">The output file to rename</param>
		private void RenameSource(UhtExportFactory.Output output)
		{
			this.Session.RenameSource(output._tempFilePath, output._filePath);
		}

		/// <summary>
		/// Given a directory and a list of known files, delete any unknown file that matches of the supplied filters
		/// </summary>
		/// <param name="outputDirectory">Output directory to scan</param>
		/// <param name="knownOutputs">Collection of known output files not to be deleted</param>
		private void CullOutputDirectory(string outputDirectory, HashSet<string> knownOutputs)
		{
			foreach (string filePath in Directory.EnumerateFiles(outputDirectory))
			{
				string fileName = Path.GetFileName(filePath);
				if (knownOutputs.Contains(fileName))
				{
					continue;
				}

				if (IsFilterMatch(fileName, this.Exporter.CppFilters) ||
					IsFilterMatch(fileName, this.Exporter.HeaderFilters) ||
					IsFilterMatch(fileName, this.Exporter.OtherFilters))
				{
					try
					{
						File.Delete(Path.Combine(outputDirectory, filePath));
					}
					catch (Exception)
					{
					}
				}
			}
		}

		/// <summary>
		/// Test to see if the given filename (without directory), matches one of the given filters
		/// </summary>
		/// <param name="fileName">File name to test</param>
		/// <param name="filters">List of wildcard filters</param>
		/// <returns>True if there is a match</returns>
		private static bool IsFilterMatch(string fileName, IEnumerable<string> filters)
		{
			foreach (string filter in filters)
			{
				if (FileSystemName.MatchesSimpleExpression(filter, fileName, true))
				{
					return true;
				}
			}
			return false;
		}
	}

	/// <summary>
	/// UHT supports the exporting of two reference output directories for testing.  The reference version can be used to test
	/// modification to UHT and verify there are no output changes or just expected changes.
	/// </summary>
	public enum UhtReferenceMode
	{
		/// <summary>
		/// Do not export any reference output files
		/// </summary>
		None,

		/// <summary>
		/// Export the reference copy
		/// </summary>
		Reference,

		/// <summary>
		/// Export the verify copy and compare to the reference copy
		/// </summary>
		Verify,
	};

	/// <summary>
	/// Session object that represents a UHT run
	/// </summary>
	public class UhtSession : IUhtMessageSite, IUhtMessageSession
	{

		/// <summary>
		/// Helper class for returning a sequence of auto-incrementing indices
		/// </summary>
		private class TypeCounter
		{

			/// <summary>
			/// Current number of types
			/// </summary>
			private int _count = 0;

			/// <summary>
			/// Get the next type index
			/// </summary>
			/// <returns>Index starting at zero</returns>
			public int GetNext()
			{
				return Interlocked.Increment(ref this._count) - 1;
			}

			/// <summary>
			/// The number of times GetNext was called.
			/// </summary>
			public int Count => Interlocked.Add(ref this._count, 0) + 1;
		}

		/// <summary>
		/// Pair that represents a specific value for an enumeration
		/// </summary>
		private struct EnumAndValue
		{
			public UhtEnum Enum { get; set; }
			public long Value { get; set; }
		}

		/// <summary>
		/// Collection of reserved names
		/// </summary>
		private static readonly HashSet<string> s_reservedNames = new HashSet<string> { "none" };

		#region Configurable settings

		/// <summary>
		/// Interface used to read/write files
		/// </summary>
		public IUhtFileManager? FileManager { get; set; }

		/// <summary>
		/// Location of the engine code
		/// </summary>
		public string? EngineDirectory { get; set; }

		/// <summary>
		/// If set, the name of the project file.
		/// </summary>
		public string? ProjectFile { get; set; }

		/// <summary>
		/// Optional location of the project
		/// </summary>
		public string? ProjectDirectory { get; set; }

		/// <summary>
		/// Root directory for the engine.  This is usually just EngineDirectory without the Engine directory.
		/// </summary>
		public string? RootDirectory { get; set; }

		/// <summary>
		/// Directory to store the reference output
		/// </summary>
		public string ReferenceDirectory { get; set; } = String.Empty;

		/// <summary>
		/// Directory to store the verification output
		/// </summary>
		public string VerifyDirectory { get; set; } = String.Empty;

		/// <summary>
		/// Mode for generating and/or testing reference output
		/// </summary>
		public UhtReferenceMode ReferenceMode { get; set; } = UhtReferenceMode.None;

		/// <summary>
		/// If true, warnings are considered to be errors
		/// </summary>
		public bool WarningsAsErrors { get; set; } = false;

		/// <summary>
		/// If true, include relative file paths in the log file
		/// </summary>
		public bool RelativePathInLog { get; set; } = false;

		/// <summary>
		/// If true, use concurrent tasks to run UHT
		/// </summary>
		public bool GoWide { get; set; } = true;

		/// <summary>
		/// If any output file mismatches existing outputs, an error will be generated
		/// </summary>
		public bool FailIfGeneratedCodeChanges { get; set; } = false;

		/// <summary>
		/// If true, no output files will be saved
		/// </summary>
		public bool NoOutput { get; set; } = false;

		/// <summary>
		/// If true, cull the output for any extra files
		/// </summary>
		public bool CullOutput { get; set; } = true;

		/// <summary>
		/// If true, include extra output in code generation
		/// </summary>
		public bool IncludeDebugOutput { get; set; } = false;

		/// <summary>
		/// If true, disable all exporters which would normally be run by default
		/// </summary>
		public bool NoDefaultExporters { get; set; } = false;

		/// <summary>
		/// If true, cache any error messages until the end of processing.  This is used by the testing
		/// harness to generate more stable console output.
		/// </summary>
		public bool CacheMessages { get; set; } = false;

		/// <summary>
		/// Collection of UHT tables
		/// </summary>
		public UhtTables? Tables { get; set; }

		/// <summary>
		/// Configuration for the session
		/// </summary>
		public IUhtConfig? Config { get; set; }
		#endregion

		/// <summary>
		/// Manifest file
		/// </summary>
		public UhtManifestFile? ManifestFile { get; set; } = null;

		/// <summary>
		/// Manifest data from the manifest file
		/// </summary>
		public UHTManifest? Manifest => this.ManifestFile?.Manifest;

		/// <summary>
		/// Collection of packages from the manifest
		/// </summary>
		public IReadOnlyList<UhtPackage> Packages => this._packagesInternal;

		/// <summary>
		/// Collection of header files from the manifest.  The header files will also appear as the children 
		/// of the packages
		/// </summary>
		public IReadOnlyList<UhtHeaderFile> HeaderFiles => this._headerFilesInternal;

		/// <summary>
		/// Collection of header files topologically sorted.  This will not be populated until after header files
		/// are parsed and resolved.
		/// </summary>
		public IReadOnlyList<UhtHeaderFile> SortedHeaderFiles => this._sortedHeaderFilesInternal;

		/// <summary>
		/// Dictionary of stripped file name to the header file
		/// </summary>
		public IReadOnlyDictionary<string, UhtHeaderFile> HeaderFileDictionary => this._headerFileDictionaryInternal;

		/// <summary>
		/// After headers are parsed, returns the UObject class.
		/// </summary>
		public UhtClass UObject
		{
			get
			{
				if (this._uobject == null)
				{
					throw new UhtIceException("UObject was not defined.");
				}
				return this._uobject;
			}
		}

		/// <summary>
		/// After headers are parsed, returns the UClass class.
		/// </summary>
		public UhtClass UClass
		{
			get
			{
				if (this._uclass == null)
				{
					throw new UhtIceException("UClass was not defined.");
				}
				return this._uclass;
			}
		}

		/// <summary>
		/// After headers are parsed, returns the UInterface class.
		/// </summary>
		public UhtClass UInterface
		{
			get
			{
				if (this._uinterface == null)
				{
					throw new UhtIceException("UInterface was not defined.");
				}
				return this._uinterface;
			}
		}

		/// <summary>
		/// After headers are parsed, returns the IInterface class.
		/// </summary>
		public UhtClass IInterface
		{
			get
			{
				if (this._iinterface == null)
				{
					throw new UhtIceException("IInterface was not defined.");
				}
				return this._iinterface;
			}
		}

		/// <summary>
		/// After headers are parsed, returns the AActor class.  Unlike such properties as "UObject", there
		/// is no requirement for AActor to be defined.  May be null.
		/// </summary>
		public UhtClass? AActor { get; set; } = null;

		/// <summary>
		/// After headers are parsed, return the INotifyFieldValueChanged interface.  There is no requirement 
		/// that this interface be defined.
		/// </summary>
		public UhtClass? INotifyFieldValueChanged { get; set; } = null;

		private readonly List<UhtPackage> _packagesInternal = new List<UhtPackage>();
		private readonly List<UhtHeaderFile> _headerFilesInternal = new List<UhtHeaderFile>();
		private readonly List<UhtHeaderFile> _sortedHeaderFilesInternal = new List<UhtHeaderFile>();
		private readonly Dictionary<string, UhtHeaderFile> _headerFileDictionaryInternal = new Dictionary<string, UhtHeaderFile>(StringComparer.OrdinalIgnoreCase);
		private long _errorCountInternal = 0;
		private long _warningCountInternal = 0;
		private readonly List<UhtMessage> _messages = new List<UhtMessage>();
		private Task? _messageTask = null;
		private UhtClass? _uobject = null;
		private UhtClass? _uclass = null;
		private UhtClass? _uinterface = null;
		private UhtClass? _iinterface = null;
		private readonly TypeCounter _typeCounterInternal = new TypeCounter();
		private readonly TypeCounter _packageTypeCountInternal = new TypeCounter();
		private readonly TypeCounter _headerFileTypeCountInternal = new TypeCounter();
		private readonly TypeCounter _objectTypeCountInternal = new TypeCounter();
		private UhtSymbolTable _sourceNameSymbolTable = new UhtSymbolTable(0);
		private UhtSymbolTable _engineNameSymbolTable = new UhtSymbolTable(0);
		private bool _symbolTablePopulated = false;
		private Task? _referenceDeleteTask = null;
		private readonly Dictionary<string, bool> _exporterStates = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
		private readonly Dictionary<string, EnumAndValue> _fullEnumValueLookup = new Dictionary<string, EnumAndValue>();
		private readonly Dictionary<string, UhtEnum> _shortEnumValueLookup = new Dictionary<string, UhtEnum>();
		private JsonDocument? _projectJson = null;

		/// <summary>
		/// The number of errors
		/// </summary>
		public long ErrorCount => Interlocked.Read(ref this._errorCountInternal);

		/// <summary>
		/// The number of warnings
		/// </summary>
		public long WarningCount => Interlocked.Read(ref this._warningCountInternal);

		/// <summary>
		/// True if any errors have occurred or warnings if warnings are to be treated as errors 
		/// </summary>
		public bool HasErrors => this.ErrorCount > 0 || (this.WarningsAsErrors && this.WarningCount > 0);

		#region IUHTMessageSession implementation
		/// <inheritdoc/>
		public IUhtMessageSession MessageSession => this;
		/// <inheritdoc/>
		public IUhtMessageSource? MessageSource => null;
		/// <inheritdoc/>
		public IUhtMessageLineNumber? MessageLineNumber => null;
		#endregion

		/// <summary>
		/// Return the index for a newly defined type
		/// </summary>
		/// <returns>New index</returns>
		public int GetNextTypeIndex()
		{
			return this._typeCounterInternal.GetNext();
		}

		/// <summary>
		/// Return the number of types that have been defined.  This includes all types.
		/// </summary>
		public int TypeCount => this._typeCounterInternal.Count;

		/// <summary>
		/// Return the index for a newly defined packaging
		/// </summary>
		/// <returns>New index</returns>
		public int GetNextHeaderFileTypeIndex()
		{
			return this._headerFileTypeCountInternal.GetNext();
		}

		/// <summary>
		/// Return the number of headers that have been defined
		/// </summary>
		public int HeaderFileTypeCount => this._headerFileTypeCountInternal.Count;

		/// <summary>
		/// Return the index for a newly defined package
		/// </summary>
		/// <returns>New index</returns>
		public int GetNextPackageTypeIndex()
		{
			return this._packageTypeCountInternal.GetNext();
		}

		/// <summary>
		/// Return the number of UPackage types that have been defined
		/// </summary>
		public int PackageTypeCount => this._packageTypeCountInternal.Count;

		/// <summary>
		/// Return the index for a newly defined object
		/// </summary>
		/// <returns>New index</returns>
		public int GetNextObjectTypeIndex()
		{
			return this._objectTypeCountInternal.GetNext();
		}

		/// <summary>
		/// Return the total number of UObject types that have been defined
		/// </summary>
		public int ObjectTypeCount => this._objectTypeCountInternal.Count;

		/// <summary>
		/// Return the collection of exporters
		/// </summary>
		public UhtExporterTable ExporterTable => this.Tables!.ExporterTable;

		/// <summary>
		/// Return the keyword table for the given table name
		/// </summary>
		/// <param name="tableName">Name of the table</param>
		/// <returns>The requested table</returns>
		public UhtKeywordTable GetKeywordTable(string tableName)
		{
			return this.Tables!.KeywordTables.Get(tableName);
		}

		/// <summary>
		/// Return the specifier table for the given table name
		/// </summary>
		/// <param name="tableName">Name of the table</param>
		/// <returns>The requested table</returns>
		public UhtSpecifierTable GetSpecifierTable(string tableName)
		{
			return this.Tables!.SpecifierTables.Get(tableName);
		}

		/// <summary>
		/// Return the specifier validator table for the given table name
		/// </summary>
		/// <param name="tableName">Name of the table</param>
		/// <returns>The requested table</returns>
		public UhtSpecifierValidatorTable GetSpecifierValidatorTable(string tableName)
		{
			return this.Tables!.SpecifierValidatorTables.Get(tableName);
		}

		/// <summary>
		/// Generate an error for the given unhandled keyword
		/// </summary>
		/// <param name="tokenReader">Token reader</param>
		/// <param name="token">Unhandled token</param>
		public void LogUnhandledKeywordError(IUhtTokenReader tokenReader, UhtToken token)
		{
			this.Tables!.KeywordTables.LogUnhandledError(tokenReader, token);
		}

		/// <summary>
		/// Test to see if the given class name is a property
		/// </summary>
		/// <param name="name">Name of the class without the prefix</param>
		/// <returns>True if the class name is a property.  False if the class name isn't a property or isn't an engine class.</returns>
		public bool IsValidPropertyTypeName(StringView name)
		{
			return this.Tables!.EngineClassTable.IsValidPropertyTypeName(name);
		}

		/// <summary>
		/// Return the loc text default value associated with the given name
		/// </summary>
		/// <param name="name"></param>
		/// <param name="locTextDefaultValue">Loc text default value handler</param>
		/// <returns></returns>
		public bool TryGetLocTextDefaultValue(StringView name, out UhtLocTextDefaultValue locTextDefaultValue)
		{
			return this.Tables!.LocTextDefaultValueTable.TryGet(name, out locTextDefaultValue);
		}

		/// <summary>
		/// Return the default processor
		/// </summary>
		public UhtPropertyType DefaultPropertyType => this.Tables!.PropertyTypeTable.Default;

		/// <summary>
		/// Return the property type associated with the given name
		/// </summary>
		/// <param name="name"></param>
		/// <param name="propertyType">Property type if matched</param>
		/// <returns></returns>
		public bool TryGetPropertyType(StringView name, out UhtPropertyType propertyType)
		{
			return this.Tables!.PropertyTypeTable.TryGet(name, out propertyType);
		}

		/// <summary>
		/// Fetch the default sanitizer
		/// </summary>
		public UhtStructDefaultValue DefaultStructDefaultValue => this.Tables!.StructDefaultValueTable.Default;

		/// <summary>
		/// Return the structure default value associated with the given name
		/// </summary>
		/// <param name="name"></param>
		/// <param name="structDefaultValue">Structure default value handler</param>
		/// <returns></returns>
		public bool TryGetStructDefaultValue(StringView name, out UhtStructDefaultValue structDefaultValue)
		{
			return this.Tables!.StructDefaultValueTable.TryGet(name, out structDefaultValue);
		}

		/// <summary>
		/// Run UHT on the given manifest.  Use the bHasError property to see if process was successful.
		/// </summary>
		/// <param name="manifestFilePath">Path to the manifest file</param>
		public void Run(string manifestFilePath)
		{
			if (this.FileManager == null)
			{
				Interlocked.Increment(ref this._errorCountInternal);
				Log.Logger.LogError("No file manager supplied, aborting.");
				return;
			}

			if (this.Config == null)
			{
				Interlocked.Increment(ref this._errorCountInternal);
				Log.Logger.LogError("No configuration supplied, aborting.");
				return;
			}

			if (this.Tables == null)
			{
				Interlocked.Increment(ref this._errorCountInternal);
				Log.Logger.LogError("No parsing tables supplied, aborting.");
				return;
			}

			switch (this.ReferenceMode)
			{
				case UhtReferenceMode.None:
					break;

				case UhtReferenceMode.Reference:
					if (String.IsNullOrEmpty(this.ReferenceDirectory))
					{
						Log.Logger.LogError("WRITEREF requested but directory not set, ignoring");
						this.ReferenceMode = UhtReferenceMode.None;
					}
					break;

				case UhtReferenceMode.Verify:
					if (String.IsNullOrEmpty(this.ReferenceDirectory) || String.IsNullOrEmpty(this.VerifyDirectory))
					{
						Log.Logger.LogError("VERIFYREF requested but directories not set, ignoring");
						this.ReferenceMode = UhtReferenceMode.None;
					}
					break;
			}

			if (this.ReferenceMode != UhtReferenceMode.None)
			{
				string directoryToDelete = this.ReferenceMode == UhtReferenceMode.Reference ? this.ReferenceDirectory : this.VerifyDirectory;
				this._referenceDeleteTask = Task.Factory.StartNew(() =>
				{
					try
					{
						Directory.Delete(directoryToDelete, true);
					}
					catch (Exception)
					{ }
				}, CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
			}

			StepReadManifestFile(manifestFilePath);
			StepPrepareModules();
			StepPrepareHeaders();
			StepParseHeaders();
			StepPopulateTypeTable();
			StepResolveInvalidCheck();
			StepResolveBases();
			StepResolveProperties();
			StepResolveFinal();
			StepResolveValidate();
			StepCollectReferences();
			TopologicalSortHeaderFiles();

			// If we are deleting the reference directory, then wait for that task to complete
			if (this._referenceDeleteTask != null)
			{
				Log.Logger.LogTrace("Step - Waiting for reference output to be cleared.");
				this._referenceDeleteTask.Wait();
			}

			StepExport();
		}

		/// <summary>
		/// Try the given action regardless of any prior errors.  If an exception occurs that doesn't have the required
		/// context, use the supplied context to generate the message.
		/// </summary>
		/// <param name="messageSource">Message context for when the exception doesn't contain a context.</param>
		/// <param name="action">The lambda to be invoked</param>
		public void TryAlways(IUhtMessageSource? messageSource, Action action)
		{
			try
			{
				action();
			}
			catch (Exception e)
			{
				HandleException(messageSource, e);
			}
		}

		/// <summary>
		/// Try the given action.  If an exception occurs that doesn't have the required
		/// context, use the supplied context to generate the message.
		/// </summary>
		/// <param name="messageSource">Message context for when the exception doesn't contain a context.</param>
		/// <param name="action">The lambda to be invoked</param>
		public void Try(IUhtMessageSource? messageSource, Action action)
		{
			if (!this.HasErrors)
			{
				try
				{
					action();
				}
				catch (Exception e)
				{
					HandleException(messageSource, e);
				}
			}
		}

		/// <summary>
		/// Read the given source file
		/// </summary>
		/// <param name="filePath">Full or relative file path</param>
		/// <returns>Information about the read source</returns>
		public UhtSourceFragment ReadSource(string filePath)
		{
			if (this.FileManager!.ReadSource(filePath, out UhtSourceFragment fragment))
			{
				return fragment;
			}
			throw new UhtException($"File not found '{filePath}'");
		}

		/// <summary>
		/// Read the given source file
		/// </summary>
		/// <param name="filePath">Full or relative file path</param>
		/// <returns>Buffer containing the read data or null if not found.  The returned buffer must be returned to the cache via a call to UhtBuffer.Return</returns>
		public UhtBuffer? ReadSourceToBuffer(string filePath)
		{
			return this.FileManager!.ReadOutput(filePath);
		}

		/// <summary>
		/// Write the given contents to the file
		/// </summary>
		/// <param name="filePath">Path to write to</param>
		/// <param name="contents">Contents to write</param>
		/// <returns>True if the source was written</returns>
		internal bool WriteSource(string filePath, ReadOnlySpan<char> contents)
		{
			return this.FileManager!.WriteOutput(filePath, contents);
		}

		/// <summary>
		/// Rename the given file
		/// </summary>
		/// <param name="oldFilePath">Old file path name</param>
		/// <param name="newFilePath">New file path name</param>
		public void RenameSource(string oldFilePath, string newFilePath)
		{
			if (!this.FileManager!.RenameOutput(oldFilePath, newFilePath))
			{
				new UhtSimpleFileMessageSite(this, newFilePath).LogError($"Failed to rename exported file: '{oldFilePath}'");
			}
		}

		/// <summary>
		/// Given the name of a regular enum value, return the enum type
		/// </summary>
		/// <param name="name">Enum value</param>
		/// <returns>Associated regular enum type or null if not found or enum isn't a regular enum.</returns>
		public UhtEnum? FindRegularEnumValue(string name)
		{
			//COMPATIBILITY-TODO - See comment below on a more rebust version of the enum lookup
			//if (this.RegularEnumValueLookup.TryGetValue(name, out UhtEnum? enumObj))
			//{
			//	return enumObj;
			//}
			if (this._fullEnumValueLookup.TryGetValue(name, out EnumAndValue value))
			{
				if (value.Value != -1)
				{
					return value.Enum;
				}
			}

			if (!name.Contains("::", StringComparison.Ordinal) && this._shortEnumValueLookup.TryGetValue(name, out UhtEnum? enumObj))
			{
				return enumObj;
			}

			return null;
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">Starting point for searches</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="name">Name of the type.</param>
		/// <param name="messageSite">If supplied, then a error message will be generated if the type can not be found</param>
		/// <param name="lineNumber">Source code line number requesting the lookup.</param>
		/// <returns>The located type of null if not found</returns>
		public UhtType? FindType(UhtType? startingType, UhtFindOptions options, string name, IUhtMessageSite? messageSite = null, int lineNumber = -1)
		{
			ValidateFindOptions(options);

			UhtType? type = FindTypeInternal(startingType, options, name);
			if (type == null && messageSite != null)
			{
				FindTypeError(messageSite, lineNumber, options, name);
			}
			return type;
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">Starting point for searches</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="name">Name of the type.</param>
		/// <param name="messageSite">If supplied, then a error message will be generated if the type can not be found</param>
		/// <returns>The located type of null if not found</returns>
		public UhtType? FindType(UhtType? startingType, UhtFindOptions options, ref UhtToken name, IUhtMessageSite? messageSite = null)
		{
			ValidateFindOptions(options);

			UhtType? type = FindTypeInternal(startingType, options, name.Value.ToString());
			if (type == null && messageSite != null)
			{
				FindTypeError(messageSite, name.InputLine, options, name.Value.ToString());
			}
			return type;
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">If specified, this represents the starting type to use when searching base/owner chain for a match</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="identifiers">Enumeration of identifiers.</param>
		/// <param name="messageSite">If supplied, then a error message will be generated if the type can not be found</param>
		/// <param name="lineNumber">Source code line number requesting the lookup.</param>
		/// <returns>The located type of null if not found</returns>
		public UhtType? FindType(UhtType? startingType, UhtFindOptions options, UhtTokenList identifiers, IUhtMessageSite? messageSite = null, int lineNumber = -1)
		{
			ValidateFindOptions(options);

			if (identifiers.Next != null && identifiers.Next.Next != null)
			{
				if (messageSite != null)
				{
					messageSite.LogError(lineNumber, "UnrealHeaderTool only supports C++ identifiers of two or less identifiers");
					return null;
				}
			}

			UhtType? type;
			if (identifiers.Next != null)
			{
				type = FindTypeTwoNamesInternal(startingType, options, identifiers.Token.Value.ToString(), identifiers.Next.Token.Value.ToString());
			}
			else
			{
				type = FindTypeInternal(startingType, options, identifiers.Token.Value.ToString());
			}

			if (type == null && messageSite != null)
			{
				string fullIdentifier = identifiers.Join("::");
				FindTypeError(messageSite, lineNumber, options, fullIdentifier);
			}
			return type;
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">If specified, this represents the starting type to use when searching base/owner chain for a match</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="identifiers">Enumeration of identifiers.</param>
		/// <param name="messageSite">If supplied, then a error message will be generated if the type can not be found</param>
		/// <param name="lineNumber">Source code line number requesting the lookup.</param>
		/// <returns>The located type of null if not found</returns>
		public UhtType? FindType(UhtType? startingType, UhtFindOptions options, UhtToken[] identifiers, IUhtMessageSite? messageSite = null, int lineNumber = -1)
		{
			ValidateFindOptions(options);

			if (identifiers.Length == 0)
			{
				throw new UhtIceException("Empty identifier array");
			}
			if (identifiers.Length > 2)
			{
				if (messageSite != null)
				{
					messageSite.LogError(lineNumber, "UnrealHeaderTool only supports C++ identifiers of two or less identifiers");
					return null;
				}
			}

			UhtType? type;
			if (identifiers.Length == 0)
			{
				type = FindTypeTwoNamesInternal(startingType, options, identifiers[0].Value.ToString(), identifiers[1].Value.ToString());
			}
			else
			{
				type = FindTypeInternal(startingType, options, identifiers[0].Value.ToString());
			}

			if (type == null && messageSite != null)
			{
				string fullIdentifier = String.Join("::", identifiers);
				FindTypeError(messageSite, lineNumber, options, fullIdentifier);
			}
			return type;
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">Starting point for searches</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="firstName">First name of the type.</param>
		/// <param name="secondName">Second name used by delegates in classes and namespace enumerations</param>
		/// <returns>The located type of null if not found</returns>
		private UhtType? FindTypeTwoNamesInternal(UhtType? startingType, UhtFindOptions options, string firstName, string secondName)
		{
			// If we have two names
			if (secondName.Length > 0)
			{
				if (options.HasAnyFlags(UhtFindOptions.DelegateFunction | UhtFindOptions.Enum))
				{
					UhtFindOptions subOptions = UhtFindOptions.NoParents | (options & ~UhtFindOptions.TypesMask) | (options & UhtFindOptions.Enum);
					if (options.HasAnyFlags(UhtFindOptions.DelegateFunction))
					{
						subOptions |= UhtFindOptions.Class;
					}
					UhtType? type = FindTypeInternal(startingType, subOptions, firstName);
					if (type == null)
					{
						return null;
					}
					if (type is UhtEnum)
					{
						return type;
					}
					if (type is UhtClass)
					{
						return FindTypeInternal(startingType, UhtFindOptions.DelegateFunction | UhtFindOptions.NoParents | (options & ~UhtFindOptions.TypesMask), secondName);
					}
				}

				// We can't match anything at this point
				return null;
			}

			// Perform the lookup for just a single name
			return FindTypeInternal(startingType, options, firstName);
		}

		/// <summary>
		/// Find the given type in the type hierarchy
		/// </summary>
		/// <param name="startingType">Starting point for searches</param>
		/// <param name="options">Options controlling what is searched</param>
		/// <param name="name">Name of the type.</param>
		/// <returns>The located type of null if not found</returns>
		public UhtType? FindTypeInternal(UhtType? startingType, UhtFindOptions options, string name)
		{
			if (options.HasAnyFlags(UhtFindOptions.EngineName))
			{
				if (options.HasAnyFlags(UhtFindOptions.CaseCompare))
				{
					return this._engineNameSymbolTable.FindCasedType(startingType, options, name);
				}
				else
				{
					return this._engineNameSymbolTable.FindCaselessType(startingType, options, name);
				}
			}
			else if (options.HasAnyFlags(UhtFindOptions.SourceName))
			{
				if (options.HasAnyFlags(UhtFindOptions.CaselessCompare))
				{
					return this._sourceNameSymbolTable.FindCaselessType(startingType, options, name);
				}
				else
				{
					return this._sourceNameSymbolTable.FindCasedType(startingType, options, name);
				}
			}
			else
			{
				throw new UhtIceException("Either EngineName or SourceName must be specified in the options");
			}
		}

		/// <summary>
		/// Verify that the options are valid.  Will also check to make sure the symbol table has been populated.
		/// </summary>
		/// <param name="options">Find options</param>
		private void ValidateFindOptions(UhtFindOptions options)
		{
			if (!options.HasAnyFlags(UhtFindOptions.EngineName | UhtFindOptions.SourceName))
			{
				throw new UhtIceException("Either EngineName or SourceName must be specified in the options");
			}

			if (options.HasAnyFlags(UhtFindOptions.CaseCompare) && options.HasAnyFlags(UhtFindOptions.CaselessCompare))
			{
				throw new UhtIceException("Both CaseCompare and CaselessCompare can't be specified as FindType options");
			}

			UhtFindOptions typeOptions = options & UhtFindOptions.TypesMask;
			if (typeOptions == 0)
			{
				throw new UhtIceException("No type options specified");
			}

			if (!this._symbolTablePopulated)
			{
				throw new UhtIceException("Symbol table has not been populated, don't call FindType until headers are parsed.");
			}
		}

		/// <summary>
		/// Generate an error message for when a given symbol wasn't found.  The text will contain the list of types that the symbol must be
		/// </summary>
		/// <param name="messageSite">Destination for the message</param>
		/// <param name="lineNumber">Line number generating the error</param>
		/// <param name="options">Collection of required types</param>
		/// <param name="name">The name of the symbol</param>
		private static void FindTypeError(IUhtMessageSite messageSite, int lineNumber, UhtFindOptions options, string name)
		{
			List<string> types = new List<string>();
			if (options.HasAnyFlags(UhtFindOptions.Enum))
			{
				types.Add("'enum'");
			}
			if (options.HasAnyFlags(UhtFindOptions.ScriptStruct))
			{
				types.Add("'struct'");
			}
			if (options.HasAnyFlags(UhtFindOptions.Class))
			{
				types.Add("'class'");
			}
			if (options.HasAnyFlags(UhtFindOptions.DelegateFunction))
			{
				types.Add("'delegate'");
			}
			if (options.HasAnyFlags(UhtFindOptions.Function))
			{
				types.Add("'function'");
			}
			if (options.HasAnyFlags(UhtFindOptions.Property))
			{
				types.Add("'property'");
			}

			messageSite.LogError(lineNumber, $"Unable to find {UhtUtilities.MergeTypeNames(types, "or")} with name '{name}'");
		}

		/// <summary>
		/// Search for the given header file by just the file name
		/// </summary>
		/// <param name="name">Name to be found</param>
		/// <returns></returns>
		public UhtHeaderFile? FindHeaderFile(string name)
		{
			if (this._headerFileDictionaryInternal.TryGetValue(name, out UhtHeaderFile? headerFile))
			{
				return headerFile;
			}
			return null;
		}

		#region IUHTMessageSource implementation
		/// <summary>
		/// Add a message to the collection of output messages
		/// </summary>
		/// <param name="message">Message being added</param>
		public void AddMessage(UhtMessage message)
		{
			lock (this._messages)
			{
				this._messages.Add(message);

				// If we aren't caching messages and this is the first message,
				// start a task to flush the messages.
				if (!this.CacheMessages && this._messages.Count == 1)
				{
					this._messageTask = Task.Factory.StartNew(() => FlushMessages(), CancellationToken.None, TaskCreationOptions.None, TaskScheduler.Default);
				}
			}

			switch (message.MessageType)
			{
				case UhtMessageType.Error:
				case UhtMessageType.Ice:
					Interlocked.Increment(ref this._errorCountInternal);
					break;

				case UhtMessageType.Warning:
					Interlocked.Increment(ref this._warningCountInternal);
					break;

				case UhtMessageType.Info:
				case UhtMessageType.Trace:
					break;
			}
		}
		#endregion

		/// <summary>
		/// Log all the collected messages to the log/console.  If messages aren't being
		/// cached, then this just waits until the flush task has completed.  If messages
		/// are being cached, they are sorted by file name and line number to ensure the 
		/// output is stable.
		/// </summary>
		public void LogMessages()
		{
			if (this._messageTask != null)
			{
				this._messageTask.Wait();
			}

			foreach (UhtMessage message in FetchOrderedMessages())
			{
				LogMessage(message);
			}
		}

		/// <summary>
		/// Flush all pending messages to the logger
		/// </summary>
		private void FlushMessages()
		{
			UhtMessage[]? messageArray = null;
			lock (this._messages)
			{
				messageArray = this._messages.ToArray();
				this._messages.Clear();
			}

			foreach (UhtMessage message in messageArray)
			{
				LogMessage(message);
			}
		}

		/// <summary>
		/// Log the given message
		/// </summary>
		/// <param name="message">The message to be logged</param>
		private void LogMessage(UhtMessage message)
		{
			string formattedMessage = FormatMessage(message);
			LogLevel logLevel;
			switch (message.MessageType)
			{
				default:
				case UhtMessageType.Error:
				case UhtMessageType.Ice:
					logLevel = LogLevel.Error;
					break;

				case UhtMessageType.Warning:
					logLevel = LogLevel.Warning;
					break;

				case UhtMessageType.Info:
					logLevel = LogLevel.Information;
					break;

				case UhtMessageType.Trace:
					logLevel = LogLevel.Trace;
					break;
			}

			Log.Logger.Log(logLevel, "{FormattedMessage}", formattedMessage);
		}

		/// <summary>
		/// Return all of the messages into a list
		/// </summary>
		/// <returns>List of all the messages</returns>
		public List<string> CollectMessages()
		{
			List<string> messages = new List<string>();
			foreach (UhtMessage message in FetchOrderedMessages())
			{
				messages.Add(FormatMessage(message));
			}
			return messages;
		}

		/// <summary>
		/// Given an existing and a new instance, replace the given type in the symbol table.
		/// This is used by the property resolution system to replace properties created during
		/// the parsing phase that couldn't be resoled until after all headers are parsed.
		/// </summary>
		/// <param name="oldType"></param>
		/// <param name="newType"></param>
		public void ReplaceTypeInSymbolTable(UhtType oldType, UhtType newType)
		{
			this._sourceNameSymbolTable.Replace(oldType, newType, oldType.SourceName);
			if (oldType.EngineType.HasEngineName())
			{
				this._engineNameSymbolTable.Replace(oldType, newType, oldType.EngineName);
			}
		}

		/// <summary>
		/// Return an ordered enumeration of all messages.
		/// </summary>
		/// <returns>Enumerator</returns>
		private IOrderedEnumerable<UhtMessage> FetchOrderedMessages()
		{
			List<UhtMessage> messages = new List<UhtMessage>();
			lock (this._messages)
			{
				messages.AddRange(this._messages);
				this._messages.Clear();
			}
			return messages.OrderBy(context => context.FilePath).ThenBy(context => context.LineNumber + context.MessageSource?.MessageFragmentLineNumber);
		}

		/// <summary>
		/// Format the given message
		/// </summary>
		/// <param name="message">Message to be formatted</param>
		/// <returns>Text of the formatted message</returns>
		private string FormatMessage(UhtMessage message)
		{
			string filePath;
			string fragmentPath = "";
			int lineNumber = message.LineNumber;
			if (message.FilePath != null)
			{
				filePath = message.FilePath;
			}
			else if (message.MessageSource != null)
			{
				if (message.MessageSource.MessageIsFragment)
				{
					if (this.RelativePathInLog)
					{
						filePath = message.MessageSource.MessageFragmentFilePath;
					}
					else
					{
						filePath = message.MessageSource.MessageFragmentFullFilePath;
					}
					fragmentPath = $"[{message.MessageSource.MessageFilePath}]";
					lineNumber += message.MessageSource.MessageFragmentLineNumber;
				}
				else
				{
					if (this.RelativePathInLog)
					{
						filePath = message.MessageSource.MessageFilePath;
					}
					else
					{
						filePath = message.MessageSource.MessageFullFilePath;
					}
				}
			}
			else
			{
				filePath = "UnknownSource";
			}

			switch (message.MessageType)
			{
				case UhtMessageType.Error:
					return $"{filePath}({lineNumber}){fragmentPath}: Error: {message.Message}";
				case UhtMessageType.Warning:
					return $"{filePath}({lineNumber}){fragmentPath}: Warning: {message.Message}";
				case UhtMessageType.Info:
					return $"{filePath}({lineNumber}){fragmentPath}: Info: {message.Message}";
				case UhtMessageType.Trace:
					return $"{filePath}({lineNumber}){fragmentPath}: Trace: {message.Message}";
				default:
				case UhtMessageType.Ice:
					return $"{filePath}({lineNumber}){fragmentPath}:  Error: Internal Compiler Error - {message.Message}";
			}
		}

		/// <summary>
		/// Handle the given exception with the provided message context
		/// </summary>
		/// <param name="messageSource">Context for the exception.  Required to handled all exceptions other than UHTException</param>
		/// <param name="e">Exception being handled</param>
		private void HandleException(IUhtMessageSource? messageSource, Exception e)
		{
			switch (e)
			{
				case UhtException uhtException:
					UhtMessage message = uhtException.UhtMessage;
					if (message.MessageSource == null)
					{
						message.MessageSource = messageSource;
					}
					AddMessage(message);
					break;

				case JsonException jsonException:
					AddMessage(UhtMessage.MakeMessage(UhtMessageType.Error, messageSource, null, (int)(jsonException.LineNumber + 1 ?? 1), jsonException.Message));
					break;

				default:
					//Log.TraceInformation("{0}", E.StackTrace);
					AddMessage(UhtMessage.MakeMessage(UhtMessageType.Ice, messageSource, null, 1, $"{e.GetType()} - {e.Message}"));
					break;
			}
		}

		/// <summary>
		/// Return the normalized path converted to a full path if possible. 
		/// Code should NOT depend on a full path being returned.
		/// 
		/// In general, it is assumed that during normal UHT, all paths are already full paths.
		/// Only the test harness deals in relative paths.
		/// </summary>
		/// <param name="filePath">Path to normalize</param>
		/// <returns>Normalized path possibly converted to a full path.</returns>
		private string GetNormalizedFullFilePath(string filePath)
		{
			return this.FileManager!.GetFullFilePath(filePath).Replace('\\', '/');
		}

		private UhtHeaderFileParser? ParseHeaderFile(UhtHeaderFile headerFile)
		{
			UhtHeaderFileParser? parser = null;
			TryAlways(headerFile.MessageSource, () =>
			{
				headerFile.Read();
				parser = UhtHeaderFileParser.Parse(headerFile);
			});
			return parser;
		}

		#region Run steps
		private void StepReadManifestFile(string manifestFilePath)
		{
			this.ManifestFile = new UhtManifestFile(this, manifestFilePath);

			Try(this.ManifestFile.MessageSource, () =>
			{
				Log.Logger.LogTrace("Step - Read Manifest File");

				this.ManifestFile.Read();

				if (this.Manifest != null && this.Tables != null)
				{
					this.Tables.AddPlugins(this.Manifest.UhtPlugins);
				}
			});
		}

		private void StepPrepareModules()
		{
			if (this.ManifestFile == null || this.ManifestFile.Manifest == null)
			{
				return;
			}

			Try(this.ManifestFile.MessageSource, () =>
			{
				Log.Logger.LogTrace("Step - Prepare Modules");

				foreach (UHTManifest.Module module in this.ManifestFile.Manifest.Modules)
				{
					EPackageFlags packageFlags = EPackageFlags.ContainsScript | EPackageFlags.Compiling;

					switch (module.OverrideModuleType)
					{
						case EPackageOverrideType.None:
							switch (module.ModuleType)
							{
								case UHTModuleType.GameEditor:
								case UHTModuleType.EngineEditor:
									packageFlags |= EPackageFlags.EditorOnly;
									break;

								case UHTModuleType.GameDeveloper:
								case UHTModuleType.EngineDeveloper:
									packageFlags |= EPackageFlags.Developer;
									break;

								case UHTModuleType.GameUncooked:
								case UHTModuleType.EngineUncooked:
									packageFlags |= EPackageFlags.UncookedOnly;
									break;
							}
							break;

						case EPackageOverrideType.EditorOnly:
							packageFlags |= EPackageFlags.EditorOnly;
							break;

						case EPackageOverrideType.EngineDeveloper:
						case EPackageOverrideType.GameDeveloper:
							packageFlags |= EPackageFlags.Developer;
							break;

						case EPackageOverrideType.EngineUncookedOnly:
						case EPackageOverrideType.GameUncookedOnly:
							packageFlags |= EPackageFlags.UncookedOnly;
							break;
					}

					UhtPackage package = new UhtPackage(this, module, packageFlags);
					this._packagesInternal.Add(package);
				}
			});
		}

		private void StepPrepareHeaders(UhtPackage package, IEnumerable<string> headerFiles, UhtHeaderFileType headerFileType)
		{
			if (package.Module == null)
			{
				return;
			}

			string typeDirectory = headerFileType.ToString() + '/';
			string normalizedModuleBaseFullFilePath = GetNormalizedFullFilePath(package.Module.BaseDirectory);
			foreach (string headerFilePath in headerFiles)
			{

				// Make sure this isn't a duplicate
				string normalizedFullFilePath = GetNormalizedFullFilePath(headerFilePath);
				string fileName = Path.GetFileName(normalizedFullFilePath);
				UhtHeaderFile? existingHeaderFile;
				if (_headerFileDictionaryInternal.TryGetValue(fileName, out existingHeaderFile) && existingHeaderFile != null)
				{
					string normalizedExistingFullFilePath = GetNormalizedFullFilePath(existingHeaderFile.FilePath);
					if (!String.Equals(normalizedFullFilePath, normalizedExistingFullFilePath, StringComparison.OrdinalIgnoreCase))
					{
						IUhtMessageSite site = (IUhtMessageSite?)this.ManifestFile ?? this;
						site.LogError($"Two headers with the same name is not allowed. '{headerFilePath}' conflicts with '{existingHeaderFile.FilePath}'");
						continue;
					}
				}

				// Create the header file and add to the collections
				UhtHeaderFile headerFile = new UhtHeaderFile(package, headerFilePath);
				headerFile.HeaderFileType = headerFileType;
				_headerFilesInternal.Add(headerFile);
				_headerFileDictionaryInternal.Add(fileName, headerFile);
				package.AddChild(headerFile);

				// Save metadata for the class path, both for it's include path and relative to the module base directory
				if (normalizedFullFilePath.StartsWith(normalizedModuleBaseFullFilePath, true, null))
				{
					int stripLength = normalizedModuleBaseFullFilePath.Length;
					if (stripLength < normalizedFullFilePath.Length && normalizedFullFilePath[stripLength] == '/')
					{
						++stripLength;
					}

					headerFile.ModuleRelativeFilePath = normalizedFullFilePath.Substring(stripLength);

					if (normalizedFullFilePath.Substring(stripLength).StartsWith(typeDirectory, true, null))
					{
						stripLength += typeDirectory.Length;
					}

					headerFile.IncludeFilePath = normalizedFullFilePath.Substring(stripLength);
				}
			}
		}

		private void StepPrepareHeaders()
		{
			if (this.ManifestFile == null)
			{
				return;
			}

			Try(this.ManifestFile.MessageSource, () =>
			{
				Log.Logger.LogTrace("Step - Prepare Headers");

				foreach (UhtPackage package in this._packagesInternal)
				{
					if (package.Module != null)
					{
						StepPrepareHeaders(package, package.Module.ClassesHeaders, UhtHeaderFileType.Classes);
						StepPrepareHeaders(package, package.Module.PublicHeaders, UhtHeaderFileType.Public);
						StepPrepareHeaders(package, package.Module.InternalHeaders, UhtHeaderFileType.Internal);
						StepPrepareHeaders(package, package.Module.PrivateHeaders, UhtHeaderFileType.Private);
					}
				}

				// Locate the NoExportTypes.h file and add it to every other header file
				if (this._headerFileDictionaryInternal.TryGetValue("NoExportTypes.h", out UhtHeaderFile? noExportTypes))
				{
					foreach (UhtPackage package in this._packagesInternal)
					{
						foreach (UhtHeaderFile headerFile in package.Children)
						{
							if (headerFile != noExportTypes)
							{
								headerFile.AddReferencedHeader(noExportTypes);
							}
						}
					}
				}
			});
		}

		private void StepParseHeaders()
		{
			if (this.HasErrors)
			{
				return;
			}

			Log.Logger.LogTrace("Step - Parse Headers");

			if (this.GoWide)
			{
				Parallel.ForEach(this._headerFilesInternal, headerFile =>
				{
					ParseHeaderFile(headerFile);
				});
			}
			else
			{
				foreach (UhtHeaderFile headerFile in this._headerFilesInternal)
				{
					ParseHeaderFile(headerFile);
				}
			}
		}

		private void StepPopulateTypeTable()
		{
			Try(null, () =>
			{
				Log.Logger.LogTrace("Step - Populate symbol table");

				this._sourceNameSymbolTable = new UhtSymbolTable(this.TypeCount);
				this._engineNameSymbolTable = new UhtSymbolTable(this.TypeCount);

				PopulateSymbolTable();

				this._uobject = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "UObject");
				this._uclass = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "UClass");
				this._uinterface = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "UInterface");
				this._iinterface = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "IInterface");
				this.AActor = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "AActor");
				this.INotifyFieldValueChanged = (UhtClass?)FindType(null, UhtFindOptions.SourceName | UhtFindOptions.Class, "INotifyFieldValueChanged");
			});
		}

		private void StepResolveBases()
		{
			StepForAllHeaders(headerFile => Resolve(headerFile, UhtResolvePhase.Bases));
		}

		private void StepResolveInvalidCheck()
		{
			StepForAllHeaders(headerFile => Resolve(headerFile, UhtResolvePhase.InvalidCheck));
		}

		private void StepResolveProperties()
		{
			StepForAllHeaders(headerFile => Resolve(headerFile, UhtResolvePhase.Properties));
		}

		private void StepResolveFinal()
		{
			StepForAllHeaders(headerFile => Resolve(headerFile, UhtResolvePhase.Final));
		}

		private void StepResolveValidate()
		{
			StepForAllHeaders(headerFile => UhtType.ValidateType(headerFile, UhtValidationOptions.None));
		}

		private void StepCollectReferences()
		{
			StepForAllHeaders(headerFile =>
			{
				foreach (UhtType child in headerFile.Children)
				{
					child.CollectReferences(headerFile.References);
				}
				foreach (UhtHeaderFile refHeaderFile in headerFile.References.ReferencedHeaders)
				{
					headerFile.AddReferencedHeader(refHeaderFile);
				}
			});
		}

		private void Resolve(UhtHeaderFile headerFile, UhtResolvePhase resolvePhase)
		{
			TryAlways(headerFile.MessageSource, () =>
			{
				headerFile.Resolve(resolvePhase);
			});
		}

		private delegate void StepDelegate(UhtHeaderFile headerFile);

		private void StepForAllHeaders(StepDelegate stepDelegate)
		{
			if (this.HasErrors)
			{
				return;
			}

			if (this.GoWide)
			{
				Parallel.ForEach(this._headerFilesInternal, headerFile =>
				{
					stepDelegate(headerFile);
				});
			}
			else
			{
				foreach (UhtHeaderFile headerFile in this._headerFilesInternal)
				{
					stepDelegate(headerFile);
				}
			}
		}
		#endregion

		#region Symbol table initialization
		private void PopulateSymbolTable()
		{
			foreach (UhtHeaderFile headerFile in this._headerFilesInternal)
			{
				AddTypeToSymbolTable(headerFile);
			}
			this._symbolTablePopulated = true;
		}

		private void AddTypeToSymbolTable(UhtType type)
		{
			UhtEngineType engineExtendedType = type.EngineType;

			if (type is UhtEnum enumObj)
			{
				//COMPATIBILITY-TODO: We can get more reliable results by only adding regular enums to the table
				// and then in the lookup code in the property system to look for the '::' and just lookup
				// the raw enum name.  In UHT we only care about the enum and not the value.
				//
				// The current algorithm has issues with two cases:
				//
				//		EnumNamespaceName::EnumTypeName::Value - Where the enum type name is included with a namespace enum
				//		EnumName::Value - Where the value is defined in terms that can't be parsed.  The -1 check causes it
				//			to be kicked out.
				//if (Enum.CppForm == UhtEnumCppForm.Regular)
				//{
				//	foreach (UhtEnumValue Value in Enum.EnumValues)
				//	{
				//		this.RegularEnumValueLookup.Add(Value.Name.ToString(), Enum);
				//	}
				//}
				bool addShortNames = enumObj.CppForm == UhtEnumCppForm.Namespaced || enumObj.CppForm == UhtEnumCppForm.EnumClass;
				string checkName = $"{enumObj.SourceName}::";
				foreach (UhtEnumValue value in enumObj.EnumValues)
				{
					this._fullEnumValueLookup.Add(value.Name, new EnumAndValue { Enum = enumObj, Value = value.Value });
					if (addShortNames)
					{
						if (value.Name.StartsWith(checkName, StringComparison.Ordinal))
						{
							this._shortEnumValueLookup.TryAdd(value.Name.Substring(checkName.Length), enumObj);
						}
					}
				}
			}

			if (engineExtendedType.FindOptions() != 0)
			{
				if (engineExtendedType.MustNotBeReserved())
				{
					if (s_reservedNames.Contains(type.EngineName))
					{
						type.HeaderFile.LogError(type.LineNumber, $"{engineExtendedType.CapitalizedText()} '{type.EngineName}' uses a reserved type name.");
					}
				}

				if (engineExtendedType.HasEngineName() && engineExtendedType.MustBeUnique())
				{
					UhtType? existingType = this._engineNameSymbolTable.FindCaselessType(null, engineExtendedType.MustBeUniqueFindOptions(), type.EngineName);
					if (existingType != null)
					{
						type.HeaderFile.LogError(type.LineNumber,
							$"{engineExtendedType.CapitalizedText()} '{type.SourceName}' shares engine name '{type.EngineName}' with " +
							$"{existingType.EngineType.LowercaseText()} '{existingType.SourceName}' in {existingType.HeaderFile.FilePath}({existingType.LineNumber})");
					}
				}

				if (type.VisibleType)
				{
					this._sourceNameSymbolTable.Add(type, type.SourceName);
					if (engineExtendedType.HasEngineName())
					{
						this._engineNameSymbolTable.Add(type, type.EngineName);
					}
				}
			}

			if (engineExtendedType.AddChildrenToSymbolTable())
			{
				foreach (UhtType child in type.Children)
				{
					AddTypeToSymbolTable(child);
				}
			}
		}
		#endregion

		#region Topological sort of the header files
		private enum TopologicalState
		{
			Unmarked,
			Temporary,
			Permanent,
		}

		private void TopologicalRecursion(List<TopologicalState> states, UhtHeaderFile first, UhtHeaderFile visit)
		{
			foreach (UhtHeaderFile referenced in visit.ReferencedHeadersNoLock)
			{
				if (states[referenced.HeaderFileTypeIndex] == TopologicalState.Temporary)
				{
					first.LogError($"'{visit.FilePath}' includes/requires '{referenced.FilePath}'");
					if (first != referenced)
					{
						TopologicalRecursion(states, first, referenced);
					}
					break;
				}
			}
		}

		private UhtHeaderFile? TopologicalVisit(List<TopologicalState> states, UhtHeaderFile visit)
		{
			switch (states[visit.HeaderFileTypeIndex])
			{
				case TopologicalState.Unmarked:
					states[visit.HeaderFileTypeIndex] = TopologicalState.Temporary;
					foreach (UhtHeaderFile referenced in visit.ReferencedHeadersNoLock)
					{
						if (visit != referenced)
						{
							UhtHeaderFile? recursion = TopologicalVisit(states, referenced);
							if (recursion != null)
							{
								return recursion;
							}
						}
					}
					states[visit.HeaderFileTypeIndex] = TopologicalState.Permanent;
					this._sortedHeaderFilesInternal.Add(visit);
					return null;

				case TopologicalState.Temporary:
					return visit;

				case TopologicalState.Permanent:
					return null;

				default:
					throw new UhtIceException("Unknown topological state");
			}
		}

		private void TopologicalSortHeaderFiles()
		{
			Try(null, () =>
			{
				Log.Logger.LogTrace("Step - Topological Sort Header Files");

				// Initialize a scratch table for topological states
				this._sortedHeaderFilesInternal.Capacity = this.HeaderFileTypeCount;
				List<TopologicalState> states = new List<TopologicalState>(this.HeaderFileTypeCount);
				for (int index = 0; index < this.HeaderFileTypeCount; ++index)
				{
					states.Add(TopologicalState.Unmarked);
				}

				foreach (UhtHeaderFile headerFile in this.HeaderFiles)
				{
					if (states[headerFile.HeaderFileTypeIndex] == TopologicalState.Unmarked)
					{
						UhtHeaderFile? recursion = TopologicalVisit(states, headerFile);
						if (recursion != null)
						{
							headerFile.LogError("Circular dependency detected:");
							TopologicalRecursion(states, recursion, recursion);
							return;
						}
					}
				}
			});
		}
		#endregion

		#region Validation helpers
		private readonly HashSet<UhtScriptStruct> _scriptStructsValidForNet = new HashSet<UhtScriptStruct>();

		/// <summary>
		/// Validate that the given referenced script structure is valid for network operations.  If the structure
		/// is valid, then the result will be cached.  It not valid, errors will be generated each time the structure
		/// is referenced.
		/// </summary>
		/// <param name="referencingProperty">The property referencing a structure</param>
		/// <param name="referencedScriptStruct">The script structure being referenced</param>
		/// <returns></returns>
		public bool ValidateScriptStructOkForNet(UhtProperty referencingProperty, UhtScriptStruct referencedScriptStruct)
		{

			// Check for existing value
			lock (this._scriptStructsValidForNet)
			{
				if (this._scriptStructsValidForNet.Contains(referencedScriptStruct))
				{
					return true;
				}
			}

			bool isStructValid = true;

			// Check the super chain structure
			UhtScriptStruct? superScriptStruct = referencedScriptStruct.SuperScriptStruct;
			if (superScriptStruct != null)
			{
				if (!ValidateScriptStructOkForNet(referencingProperty, superScriptStruct))
				{
					isStructValid = false;
				}
			}

			// Check the structure properties
			foreach (UhtProperty property in referencedScriptStruct.Properties)
			{
				if (!property.ValidateStructPropertyOkForNet(referencingProperty))
				{
					isStructValid = false;
					break;
				}
			}

			// Save the results
			if (isStructValid)
			{
				lock (this._scriptStructsValidForNet)
				{
					this._scriptStructsValidForNet.Add(referencedScriptStruct);
				}
			}
			return isStructValid;
		}
		#endregion

		#region Exporting

		/// <summary>
		/// Enable/Disable an exporter.  This overrides the default state of the exporter.
		/// </summary>
		/// <param name="name">Name of the exporter</param>
		/// <param name="enabled">If true, the exporter is to be enabled</param>
		public void SetExporterStatus(string name, bool enabled)
		{
			this._exporterStates[name] = enabled;
		}

		/// <summary>
		/// Test to see if the given exporter plugin is enabled.
		/// </summary>
		/// <param name="pluginName">Name of the plugin</param>
		/// <param name="includeTargetCheck">If true, include a target check</param>
		/// <returns>True if enabled</returns>
		public bool IsPluginEnabled(string pluginName, bool includeTargetCheck)
		{
			if (this._projectJson == null && this.ProjectDirectory != null && this.ProjectFile != null)
			{
				UhtBuffer? contents = this.ReadSourceToBuffer(this.ProjectFile);
				if (contents != null)
				{
					this._projectJson = JsonDocument.Parse(contents.Memory);
					UhtBuffer.Return(contents);
				}
			}

			if (this._projectJson == null)
			{
				return false;
			}

			JsonObject rootObject = new JsonObject(this._projectJson.RootElement);
			if (rootObject.TryGetObjectArrayField("Plugins", out JsonObject[]? plugins))
			{
				foreach (JsonObject plugin in plugins)
				{
					if (!plugin.TryGetStringField("Name", out string? testPluginName) || !String.Equals(pluginName, testPluginName, StringComparison.OrdinalIgnoreCase))
					{
						continue;
					}
					if (!plugin.TryGetBoolField("Enabled", out bool enabled) || !enabled)
					{
						return false;
					}
					if (includeTargetCheck && this.Manifest != null)
					{
						if (plugin.TryGetStringArrayField("TargetAllowList", out string[]? allowList))
						{
							if (allowList.Contains(this.Manifest.TargetName, StringComparer.OrdinalIgnoreCase))
							{
								return true;
							}
						}
						if (plugin.TryGetStringArrayField("TargetDenyList", out string[]? denyList))
						{
							if (denyList.Contains(this.Manifest.TargetName, StringComparer.OrdinalIgnoreCase))
							{
								return false;
							}
						}
					}
					return true;
				}
			}
			return false;
		}

		private void StepExport()
		{
			HashSet<string> externalDependencies = new HashSet<string>();
			long totalWrittenFiles = 0;
			Try(null, () =>
			{
				Log.Logger.LogTrace("Step - Exports");

				foreach (UhtExporter exporter in this.ExporterTable)
				{
					bool run = false;
					if (!this._exporterStates.TryGetValue(exporter.Name, out run))
					{
						run = Config!.IsExporterEnabled(exporter.Name) ||
							(exporter.Options.HasAnyFlags(UhtExporterOptions.Default) && !this.NoDefaultExporters);
					}

					UHTManifest.Module? pluginModule = null;
					if (!String.IsNullOrEmpty(exporter.ModuleName))
					{
						foreach (UHTManifest.Module module in this.Manifest!.Modules)
						{
							if (String.Equals(module.Name, exporter.ModuleName, StringComparison.OrdinalIgnoreCase))
							{
								pluginModule = module;
								break;
							}
						}
						if (pluginModule == null)
						{
							Log.TraceWarning($"Exporter \"{exporter.Name}\" skipped because module \"{exporter.ModuleName}\" was not found in manifest");
							continue;
						}
					}

					if (run)
					{
						Log.TraceLog($"       Running exporter {exporter.Name}");
						UhtExportFactory factory = new UhtExportFactory(this, pluginModule, exporter);
						factory.Run();
						foreach (UhtExportFactory.Output output in factory.Outputs)
						{
							if (output._saved)
							{
								totalWrittenFiles++;
							}
						}
						foreach (string dep in factory.ExternalDependencies)
						{
							externalDependencies.Add(dep);
						}
					}
					else
					{
						Log.TraceLog($"       Exporter {exporter.Name} skipped");
					}
				}

				// Save the collected external dependencies
				if (!String.IsNullOrEmpty(this.Manifest!.ExternalDependenciesFile))
				{
					using (StreamWriter output = new StreamWriter(this.Manifest!.ExternalDependenciesFile))
					{
						foreach (string dep in externalDependencies)
						{
							output.WriteLine(dep);
						}
					}
				}
			});

			Log.TraceInformation($"Total of {totalWrittenFiles} written");
		}
		#endregion
	}
}
