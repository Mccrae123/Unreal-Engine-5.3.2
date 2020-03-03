// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Containers/ArrayView.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/Package.h"
#include "Templates/UniquePtr.h"
#include "TickableEditorObject.h"
#include "IPlatformFileSandboxWrapper.h"
#include "INetworkFileSystemModule.h"
#include "CookOnTheFlyServer.generated.h"


class FAssetRegistryGenerator;
class FAsyncIODelete;
struct FPackageNameCache;
struct FPropertyChangedEvent;
class FReferenceCollector;
class FSavePackageContext;
class ITargetPlatform;
class IAssetRegistry;
class IPlugin;

enum class ECookInitializationFlags
{
	None =										0x00000000, // No flags
	//unused =									0x00000001, 
	Iterative =									0x00000002, // use iterative cooking (previous cooks will not be cleaned unless detected out of date, experimental)
	SkipEditorContent =							0x00000004, // do not cook any content in the content\editor directory
	Unversioned =								0x00000008, // save the cooked packages without a version number
	AutoTick =									0x00000010, // enable ticking (only works in the editor)
	AsyncSave =									0x00000020, // save packages async
	// unused =									0x00000040,
	IncludeServerMaps =							0x00000080, // should we include the server maps when cooking
	UseSerializationForPackageDependencies =	0x00000100, // should we use the serialization code path for generating package dependencies (old method will be deprecated)
	BuildDDCInBackground =						0x00000200, // build ddc content in background while the editor is running (only valid for modes which are in editor IsCookingInEditor())
	GeneratedAssetRegistry =					0x00000400, // have we generated asset registry yet
	OutputVerboseCookerWarnings =				0x00000800, // output additional cooker warnings about content issues
	EnablePartialGC =							0x00001000, // mark up with an object flag objects which are in packages which we are about to use or in the middle of using, this means we can gc more often but only gc stuff which we have finished with
	TestCook =									0x00002000, // test the cooker garbage collection process and cooking (cooker will never end just keep testing).
	//unused =									0x00004000,
	LogDebugInfo =								0x00008000, // enables additional debug log information
	IterateSharedBuild =						0x00010000, // iterate from a build in the SharedIterativeBuild directory 
	IgnoreIniSettingsOutOfDate =				0x00020000, // if the inisettings say the cook is out of date keep using the previously cooked build
	IgnoreScriptPackagesOutOfDate =				0x00040000, // for incremental cooking, ignore script package changes
};
ENUM_CLASS_FLAGS(ECookInitializationFlags);

enum class ECookByTheBookOptions
{
	None =								0x00000000, // no flags
	CookAll	=							0x00000001, // cook all maps and content in the content directory
	MapsOnly =							0x00000002, // cook only maps
	NoDevContent =						0x00000004, // don't include dev content
	ForceDisableCompressed =			0x00000010, // force compression to be disabled even if the cooker was initialized with it enabled
	ForceEnableCompressed =				0x00000020, // force compression to be on even if the cooker was initialized with it disabled
	ForceDisableSaveGlobalShaders =		0x00000040, // force global shaders to not be saved (used if cooking multiple times for the same platform and we know we are up todate)
	NoGameAlwaysCookPackages =			0x00000080, // don't include the packages specified by the game in the cook (this cook will probably be missing content unless you know what you are doing)
	NoAlwaysCookMaps =					0x00000100, // don't include always cook maps (this cook will probably be missing content unless you know what you are doing)
	NoDefaultMaps =						0x00000200, // don't include default cook maps (this cook will probably be missing content unless you know what you are doing)
	NoSlatePackages =					0x00000400, // don't include slate content (this cook will probably be missing content unless you know what you are doing)
	NoInputPackages =					0x00000800, // don't include slate content (this cook will probably be missing content unless you know what you are doing)
	DisableUnsolicitedPackages =		0x00001000, // don't cook any packages which aren't in the files to cook list (this is really dangerious as if you request a file it will not cook all it's dependencies automatically)
	FullLoadAndSave =					0x00002000, // Load all packages into memory and save them all at once in one tick for speed reasons. This requires a lot of RAM for large games.
	PackageStore =						0x00004000, // Cook package header information into a global package store
};
ENUM_CLASS_FLAGS(ECookByTheBookOptions);


UENUM()
namespace ECookMode
{
	enum Type
	{
		/** Default mode, handles requests from network. */
		CookOnTheFly,
		/** Cook on the side. */
		CookOnTheFlyFromTheEditor,
		/** Precook all resources while in the editor. */
		CookByTheBookFromTheEditor,
		/** Cooking by the book (not in the editor). */
		CookByTheBook,
	};
}

UENUM()
enum class ECookTickFlags : uint8
{
	None =									0x00000000, /* no flags */
	MarkupInUsePackages =					0x00000001, /** Markup packages for partial gc */
	HideProgressDisplay =					0x00000002, /** Hides the progress report */
};
ENUM_CLASS_FLAGS(ECookTickFlags);

DECLARE_LOG_CATEGORY_EXTERN(LogCook, Log, All);

namespace UE
{
namespace Cook
{
	struct FCookerTimer;
	class FExternalRequests;
	struct FPackageData;
	struct FPackageDatas;
	struct FPackageTracker;
	struct FPendingCookedPlatformData;
	struct FPlatformManager;
	struct FTickStackData;
}
}

UCLASS()
class UNREALED_API UCookOnTheFlyServer : public UObject, public FTickableEditorObject, public FExec
{
	GENERATED_BODY()

		UCookOnTheFlyServer(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
		UCookOnTheFlyServer(FVTableHelper& Helper); // Declare the FVTableHelper constructor manually so that we can forward-declare-only TUniquePtrs in the header without getting compile error in generated cpp

public:
	struct FCookByTheBookOptions;

private:
	/** Current cook mode the cook on the fly server is running in */
	ECookMode::Type CurrentCookMode = ECookMode::CookOnTheFly;
	/** Directory to output to instead of the default should be empty in the case of DLC cooking */ 
	FString OutputDirectoryOverride;

	FCookByTheBookOptions* CookByTheBookOptions = nullptr;
	TUniquePtr<UE::Cook::FPlatformManager> PlatformManager;
	FCriticalSection RequestLock;

	//////////////////////////////////////////////////////////////////////////
	// Cook on the fly options

	/** Cook on the fly server uses the NetworkFileServer */
	TArray<class INetworkFileServer*> NetworkFileServers;
	FOnFileModifiedDelegate FileModifiedDelegate;

	//////////////////////////////////////////////////////////////////////////
	// General cook options

	/** Number of packages to load before performing a garbage collect. Set to 0 to never GC based on number of loaded packages */
	uint32 PackagesPerGC;
	/** Amount of time that is allowed to be idle before forcing a garbage collect. Set to 0 to never force GC due to idle time */
	double IdleTimeToGC;
	/** Max memory the cooker should use before forcing a gc */
	uint64 MaxMemoryAllowance;
	/** Min memory before the cooker should partial gc */
	uint64 MinMemoryBeforeGC;
	/** If we have less then this much memory free then finish current task and kick off gc */
	uint64 MinFreeMemory;
	/** Max number of packages to save before we partial gc */
	int32 MaxNumPackagesBeforePartialGC;
	/** Max number of concurrent shader jobs reducing this too low will increase cook time */
	int32 MaxConcurrentShaderJobs;
	/** Min number of free UObject indices before the cooker should partial gc */
	int32 MinFreeUObjectIndicesBeforeGC;

	ECookInitializationFlags CookFlags = ECookInitializationFlags::None;
	TUniquePtr<class FSandboxPlatformFile> SandboxFile;
	TUniquePtr<FAsyncIODelete> AsyncIODelete; // Helper for deleting the old cook directory asynchronously
	bool bIsInitializingSandbox = false; // stop recursion into callbacks when we are initializing sandbox
	bool bIsSavingPackage = false; // used to stop recursive mark package dirty functions
	bool bSaveAsyncAllowed = false; // True if and only if command line options and all other restrictions allow the use of SAVE_Async


	TMap<FName, int32> MaxAsyncCacheForType; // max number of objects of a specific type which are allowed to async cache at once
	mutable TMap<FName, int32> CurrentAsyncCacheForType; // max number of objects of a specific type which are allowed to async cache at once

	/** List of additional plugin directories to remap into the sandbox as needed */
	TArray<TSharedRef<IPlugin> > PluginsToRemap;

	//////////////////////////////////////////////////////////////////////////
	// precaching system
	//
	// this system precaches materials and textures before we have considered the object 
	// as requiring save so as to utilize the system when it's idle
	
	TArray<FWeakObjectPtr> CachedMaterialsToCacheArray;
	TArray<FWeakObjectPtr> CachedTexturesToCacheArray;
	int32 LastUpdateTick = 0;
	int32 MaxPrecacheShaderJobs = 0;
	void TickPrecacheObjectsForPlatforms(const float TimeSlice, const TArray<const ITargetPlatform*>& TargetPlatform);

	//////////////////////////////////////////////////////////////////////////

	int32 LastCookPendingCount = 0;
	int32 LastCookedPackagesCount = 0;
	double LastProgressDisplayTime = 0;

	FName ConvertCookedPathToUncookedPath(
		const FString& SandboxRootDir, const FString& RelativeRootDir,
		const FString& SandboxProjectDir, const FString& RelativeProjectDir,
		const FString& CookedPath, FString& OutUncookedPath) const;

	/** Get dependencies for package */
	const TArray<FName>& GetFullPackageDependencies(const FName& PackageName) const;
	mutable TMap<FName, TArray<FName>> CachedFullPackageDependencies;

	/** Cached copy of asset registry */
	IAssetRegistry* AssetRegistry = nullptr;

	/** Map of platform name to scl.csv files we saved out. */
	TMap<FName, TArray<FString>> OutSCLCSVPaths;

	/** List of filenames that may be out of date in the asset registry */
	TSet<FName> ModifiedAssetFilenames;

	//////////////////////////////////////////////////////////////////////////
	// iterative ini settings checking
	// growing list of ini settings which are accessed over the course of the cook

	// tmap of the Config name, Section name, Key name, to the value
	typedef TMap<FName, TMap<FName, TMap<FName, TArray<FString>>>> FIniSettingContainer;

	mutable FCriticalSection ConfigFileCS;
	mutable bool IniSettingRecurse = false;
	mutable FIniSettingContainer AccessedIniStrings;
	TArray<const FConfigFile*> OpenConfigFiles;
	TArray<FString> ConfigSettingBlacklist;
	void OnFConfigDeleted(const FConfigFile* Config);
	void OnFConfigCreated(const FConfigFile* Config);

	void ProcessAccessedIniSettings(const FConfigFile* Config, FIniSettingContainer& AccessedIniStrings) const;

	/**
	* OnTargetPlatformChangedSupportedFormats
	* called when target platform changes the return value of supports shader formats 
	* used to reset the cached cooked shaders
	*/
	void OnTargetPlatformChangedSupportedFormats(const ITargetPlatform* TargetPlatform);

	/* In CookOnTheFly, requests can add a new platform; this function conditionally initializes the requested platform.  Returns false if the given TargetPlatform is not available, else true */
	bool AddCookOnTheFlyPlatform(ITargetPlatform* TargetPlatform);
	/* Version of AddCookOnTheFlyPlatform that takes the Platform name instead of an ITargetPlatform*.  Returns the Platform if found */
	ITargetPlatform* AddCookOnTheFlyPlatform(const FString& PlatformName);
	/* Internal helper for AddCookOnTheFlyPlatform.  Initializing Platforms must be done on the tickloop thread; Platform data is read only on other threads */
	void AddCookOnTheFlyPlatformFromGameThread(ITargetPlatform* TargetPlatform);

	/* Update polled fields used by CookOnTheFly's network request handlers */
	void TickNetwork();

	/** Execute operations that need to be done after each Scheduler task, such as checking for new external requests. */
	void TickCookStatus(UE::Cook::FTickStackData& StackData);
	enum class ECookAction
	{
		Done,		// The cook is complete; no requests remain in any non-idle state
		Request,    // Process the RequestQueue
		Save,       // Process the SaveQueue
		YieldTick,  // Progress is blocked by an async result. Temporarily exit TickCookOnTheSide.
		Cancel,		// Cancel the current CookByTheBook
	};
	/** Inspect all tasks the scheduler could do and return which one it should do. */
	ECookAction DecideNextCookAction(UE::Cook::FTickStackData& StackData);
	/** Execute any existing external callbacks and push any existing external cook requests into the RequestQueue. */
	void PumpExternalRequests(const UE::Cook::FCookerTimer& CookerTimer);
	/** Load the next package in the RequestQueue and push it on to its next state. */
	void PumpRequestQueue(UE::Cook::FTickStackData& StackData);
	/** Try to save all packages in the SaveQueue until blocked or out of time. */
	void PumpSaveQueue(UE::Cook::FTickStackData& StackData);
	/** Load the given package and push it on to its next state. */
	void ProcessLoadQueuePackage(UE::Cook::FPackageData& PackageData, uint32& ResultFlags);
	/** Mark that the given PackageData failed to load and return it to idle. */
	void RejectPackageToLoad(UE::Cook::FPackageData& PackageData, const TCHAR* Reason);
	/**
	 * Inspect the given package from the PackageTracker and add it to the SaveQueue if the cooker should save it.
	 *
	 * @param bUpdatePlatforms If true, then FilterLoadedPackage is being called on all existing packages in order a change to the package filter.  Any InProgress PackageDatas will be demoted to request to update the requested platforms.
	 */
	void FilterLoadedPackage(UPackage* Package, bool bUpdatePlatforms);
	/** Check whether the package filter has change, and if so call FilterLoadedPackage again on each existing package. */
	void UpdatePackageFilter();

	/**
	 * Remove all request data about the given platform from any data in the CookOnTheFlyServer.
	 * Called in response to a platform being removed from the list of session platforms e.g. because it has not been recently used by CookOnTheFly.
	 * Does not modify Cooked platforms.
	 */
	void OnRemoveSessionPlatform(const ITargetPlatform* TargetPlatform);

	/** Entry/Exit gates for PackageData states, used to enforce state contracts and free unneeded memory. */
	void EnterIdle(UE::Cook::FPackageData& PackageData);
	void ExitIdle(UE::Cook::FPackageData& PackageData);
	void EnterInProgress(UE::Cook::FPackageData& PackageData);
	void ExitInProgress(UE::Cook::FPackageData& PackageData);
	void EnterRequest(UE::Cook::FPackageData& PackageData);
	void ExitRequest(UE::Cook::FPackageData& PackageData);
	void EnterSave(UE::Cook::FPackageData& PackageData);
	void ExitSave(UE::Cook::FPackageData& PackageData);

public:

	enum ECookOnTheSideResult
	{
		COSR_None					= 0x00000000,
		COSR_CookedMap				= 0x00000001,
		COSR_CookedPackage			= 0x00000002,
		COSR_ErrorLoadingPackage	= 0x00000004,
		COSR_RequiresGC				= 0x00000008,
		COSR_WaitingOnCache			= 0x00000010,
		COSR_MarkedUpKeepPackages	= 0x00000040
	};


	virtual ~UCookOnTheFlyServer();

	/**
	* FTickableEditorObject interface used by cook on the side
	*/
	TStatId GetStatId() const override;
	void Tick(float DeltaTime) override;
	bool IsTickable() const override;
	ECookMode::Type GetCookMode() const { return CurrentCookMode; }


	/**
	* FExec interface used in the editor
	*/
	virtual bool Exec(class UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override;

	/**
	  * UObject interface
	  */
	virtual bool IsDestructionThreadSafe() const override { return false; }

	/**
	 * Dumps cooking stats to the log
	 *  run from the exec command "Cook stats"
	 */
	void DumpStats();

	/**
	* Initialize the CookServer so that either CookOnTheFly can be called or Cook on the side can be started and ticked
	*/
	void Initialize( ECookMode::Type DesiredCookMode, ECookInitializationFlags InCookInitializationFlags, const FString& OutputDirectoryOverride = FString() );

	/**
	* Cook on the side, cooks while also running the editor...
	*
	* @param BindAnyPort					Whether to bind on any port or the default port.
	* @param TargetPlatforms				If nonempty, cooking will be prepared (generate AssetRegistry, etc) for each platform in the array
	*
	* @return true on success, false otherwise.
	*/
	bool StartNetworkFileServer( bool BindAnyPort, const TArray<ITargetPlatform*>& TargetPlatforms = TArray<ITargetPlatform*>() );

	/**
	* Broadcast our the fileserver presence on the network
	*/
	bool BroadcastFileserverPresence( const FGuid &InstanceId );
	/** 
	* Stop the network file server
	*
	*/
	void EndNetworkFileServer();


	struct FCookByTheBookStartupOptions
	{
	public:
		TArray<ITargetPlatform*> TargetPlatforms;
		TArray<FString> CookMaps;
		TArray<FString> CookDirectories;
		TArray<FString> NeverCookDirectories;
		TArray<FString> CookCultures; 
		TArray<FString> IniMapSections;
		TArray<FString> CookPackages; // list of packages we should cook, used to specify specific packages to cook
		ECookByTheBookOptions CookOptions = ECookByTheBookOptions::None;
		FString DLCName;
		FString CreateReleaseVersion;
		FString BasedOnReleaseVersion;
		bool bGenerateStreamingInstallManifests = false;
		bool bGenerateDependenciesForMaps = false;
		bool bErrorOnEngineContentUse = false; // this is a flag for dlc, will cause the cooker to error if the dlc references engine content
	};

	/**
	* Start a cook by the book session
	* Cook on the fly can't run at the same time as cook by the book
	*/
	void StartCookByTheBook( const FCookByTheBookStartupOptions& CookByTheBookStartupOptions );

	/**
	* Queue a cook by the book cancel (you might want to do this instead of calling cancel directly so that you don't have to be in the game thread when canceling
	*/
	void QueueCancelCookByTheBook();

	/**
	* Cancel the currently running cook by the book (needs to be called from the game thread)
	*/
	void CancelCookByTheBook();

	bool IsCookByTheBookRunning() const;
	/** Report whether the CookOnTheFlyServer is in a cook session, either CookByTheBook or CookOnTheFly. Used to restrict operations when cooking and reduce cputime when not cooking. */
	bool IsInSession() const;

	/**
	* Get any packages which are in memory, these were probably required to be loaded because of the current package we are cooking, so we should probably cook them also
	*/
	UE_DEPRECATED(4.26, "Unsolicited packages are now added directly to the savequeue and are not marked as unsolicited")
	TArray<UPackage*> GetUnsolicitedPackages(const TArray<const ITargetPlatform*>& TargetPlatforms) const;

	/**
	* PostLoadPackageFixup
	* after a package is loaded we might want to fix up some stuff before it gets saved
	*/
	void PostLoadPackageFixup(UPackage* Package);

	/**
	* Handles cook package requests until there are no more requests, then returns
	*
	* @param  CookFlags output of the things that might have happened in the cook on the side
	* 
	* @return returns ECookOnTheSideResult
	*/
	uint32 TickCookOnTheSide( const float TimeSlice, uint32 &CookedPackagesCount, ECookTickFlags TickFlags = ECookTickFlags::None );

	/**
	* Clear all the previously cooked data all cook requests from now on will be considered recook requests
	*/
	void ClearAllCookedData();

	/** Demote all PackageDatas in any queue back to Idle, and eliminate any pending requests. Used when canceling a cook. */
	void CancelAllQueues();


	/**
	* Clear any cached cooked platform data for a platform
	*  call ClearCachedCookedPlatformData on all UObjects
	* @param TargetPlatform platform to clear all the cached data for
	*/
	void ClearCachedCookedPlatformDataForPlatform(const ITargetPlatform* TargetPlatform);

	UE_DEPRECATED(4.25, "Use version that takes const ITargetPlatform* instead")
	void ClearCachedCookedPlatformDataForPlatform(const FName& PlatformName);


	/**
	* Clear all the previously cooked data for the platform passed in 
	* 
	* @param TargetPlatform the platform to clear the cooked packages for
	*/
	void ClearPlatformCookedData(const ITargetPlatform* TargetPlatform);

	UE_DEPRECATED(4.25, "Use version that takes const ITargetPlatform* instead")
	void ClearPlatformCookedData(const FString& PlatformName);


	/**
	* Recompile any global shader changes 
	* if any are detected then clear the cooked platform data so that they can be rebuilt
	* 
	* @return return true if shaders were recompiled
	*/
	bool RecompileChangedShaders(const TArray<const ITargetPlatform*>& TargetPlatforms);

	UE_DEPRECATED(4.25, "Use version that takes const ITargetPlatform* instead")
	bool RecompileChangedShaders(const TArray<FName>& TargetPlatformNames);


	/**
	* Force stop whatever pending cook requests are going on and clear all the cooked data
	* Note cook on the side / cook on the fly clients may not be able to recover from this if they are waiting on a cook request to complete
	*/
	void StopAndClearCookedData();

	/** 
	* Process any shader recompile requests
	*/
	void TickRecompileShaderRequests();

	bool HasRecompileShaderRequests() const;

	UE_DEPRECATED(4.26, "Use HasRemainingWork instead")
	bool HasCookRequests() const
	{
		return HasRemainingWork();
	}
	/** Return whether TickCookOnTheSide needs to take any action for the current session. If not, the session is done. Used for external managers of the cooker to know when to tick it. */
	bool HasRemainingWork() const;
	void WaitForRequests(int TimeoutMs);

	uint32 NumConnections() const;

	/**
	* Is this cooker running in the editor
	*
	* @return true if we are running in the editor
	*/
	bool IsCookingInEditor() const;

	/**
	* Is this cooker running in real time mode (where it needs to respect the timeslice) 
	* 
	* @return returns true if this cooker is running in realtime mode like in the editor
	*/
	bool IsRealtimeMode() const;

	/**
	* Helper function returns if we are in any cook by the book mode
	*
	* @return if the cook mode is a cook by the book mode
	*/
	bool IsCookByTheBookMode() const;

	bool IsUsingShaderCodeLibrary() const;

	bool IsUsingPackageStore() const;

	/**
	* Helper function returns if we are in any cook on the fly mode
	*
	* @return if the cook mode is a cook on the fly mode
	*/
	bool IsCookOnTheFlyMode() const;


	virtual void BeginDestroy() override;

	/** Returns the configured number of packages to process before GC */
	uint32 GetPackagesPerGC() const;

	/** Returns the configured number of packages to process before partial GC */
	uint32 GetPackagesPerPartialGC() const;

	/** Returns the configured amount of idle time before forcing a GC */
	double GetIdleTimeToGC() const;

	/** Returns the configured amount of memory allowed before forcing a GC */
	uint64 GetMaxMemoryAllowance() const;

	/** Mark package as keep around for the cooker (don't GC) */
	UE_DEPRECATED(4.25, "UCookOnTheFlyServer now uses FGCObject to interact with garbage collection")
	void MarkGCPackagesToKeepForCooker()
	{
	}

	bool HasExceededMaxMemory() const;

	/**
	* RequestPackage to be cooked
	*
	* @param StandardFileName FileName of the package in standard format as returned by FPaths::MakeStandardFilename
	* @param TargetPlatforms The TargetPlatforms we want this package cooked for
	* @param bForceFrontOfQueue should we put this package in the front of the cook queue (next to be processed) or at the end
	*/
	bool RequestPackage(const FName& StandardFileName, const TArrayView<const ITargetPlatform* const>& TargetPlatforms, const bool bForceFrontOfQueue);

	UE_DEPRECATED(4.25, "Use Version that takes TArray<const ITargetPlatform*> instead")
	bool RequestPackage(const FName& StandardFileName, const TArrayView<const FName>& TargetPlatformNames, const bool bForceFrontOfQueue);

	/**
	* RequestPackage to be cooked
	* This function can only be called while the cooker is in cook by the book mode
	*
	* @param StandardPackageFName name of the package in standard format as returned by FPaths::MakeStandardFilename
	* @param bForceFrontOfQueue should we put this package in the front of the cook queue (next to be processed) or at the end
	*/
	bool RequestPackage(const FName& StandardPackageFName, const bool bForceFrontOfQueue);


	/**
	* Callbacks from editor 
	*/

	void OnObjectModified( UObject *ObjectMoving );
	void OnObjectPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent);
	void OnObjectUpdated( UObject *Object );
	void OnObjectSaved( UObject *ObjectSaved );

	/**
	* Marks a package as dirty for cook
	* causes package to be recooked on next request (and all dependent packages which are currently cooked)
	*/
	void MarkPackageDirtyForCooker( UPackage *Package, bool bAllowInSession = false );

	/** Helper function for MarkPackageDirtyForCooker. Executes the MarkPackageDirtyForCooker operations that are only safe to execute from the scheduler's designated point for handling external requests. */
	void MarkPackageDirtyForCookerFromSchedulerThread(const FName& PackageName);

	/**
	* MaybeMarkPackageAsAlreadyLoaded
	* Mark the package as already loaded if we have already cooked the package for all requested target platforms
	* this hints to the objects on load that we don't need to load all our bulk data
	* 
	* @param Package to mark as not requiring reload
	*/
	void MaybeMarkPackageAsAlreadyLoaded(UPackage* Package);

	/**
	* Callbacks from UObject globals
	*/
	void PreGarbageCollect();
	void CookerAddReferencedObjects(FReferenceCollector& Ar);
	void PostGarbageCollect();

private:

	//////////////////////////////////////////////////////////////////////////
	// cook by the book specific functions
	/**
	* Collect all the files which need to be cooked for a cook by the book session
	*/
	void CollectFilesToCook(TArray<FName>& FilesInPath, 
		const TArray<FString>& CookMaps, const TArray<FString>& CookDirectories, 
		const TArray<FString>& IniMapSections, ECookByTheBookOptions FilesToCookFlags,
		const TArrayView<const ITargetPlatform* const>& TargetPlatforms);

	/**
	* AddFileToCook add file to cook list 
	*/
	void AddFileToCook( TArray<FName>& InOutFilesToCook, const FString &InFilename ) const;

	/**
	* Invokes the necessary FShaderCodeLibrary functions to start cooking the shader code library.
	*/
	void InitShaderCodeLibrary(void);
    
	/**
	* Invokes the necessary FShaderCodeLibrary functions to open a named code library.
	*/
	void OpenShaderCodeLibrary(FString const& Name);
    
	/**
	* Invokes the necessary FShaderCodeLibrary functions to save and close a named code library.
	*/
	void SaveShaderCodeLibrary(FString const& Name);

	/**
	* Calls the ShaderPipelineCacheToolsCommandlet to build a upipelinecache file from the .stablepc.csv file, if any
	*/
	void ProcessShaderCodeLibraries(const FString& LibraryName);

	/**
	* Invokes the necessary FShaderCodeLibrary functions to clean out all the temporary files.
	*/
	void CleanShaderCodeLibraries();

	/**
	* Call back from the TickCookOnTheSide when a cook by the book finishes (when started form StartCookByTheBook)
	*/
	void CookByTheBookFinished();

	/**
	* Get all the packages which are listed in asset registry passed in.  
	*
	* @param AssetRegistryPath path of the assetregistry.bin file to read
	* @param OutPackageNames out list of uncooked package filenames which were contained in the asset registry file
	* @return true if successfully read false otherwise
	*/
	bool GetAllPackageFilenamesFromAssetRegistry( const FString& AssetRegistryPath, TArray<FName>& OutPackageFilenames ) const;

	/**
	* BuildMapDependencyGraph
	* builds a map of dependencies from maps
	* 
	* @param TargetPlatform the platform we want to build a map for
	*/
	void BuildMapDependencyGraph(const ITargetPlatform* TargetPlatform);

	/**
	* WriteMapDependencyGraph
	* write a previously built map dependency graph out to the sandbox directory for a platform
	*
	* @param TargetPlatform the platform we want to save out the dependency graph for
	*/
	void WriteMapDependencyGraph(const ITargetPlatform* TargetPlatform);

	//////////////////////////////////////////////////////////////////////////
	// cook on the fly specific functions

	/**
	 * When we get a new connection from the network make sure the version is compatible 
	 *		Will terminate the connection if return false
	 * 
	 * @return return false if not compatible, true if it is
	 */
	bool HandleNetworkFileServerNewConnection( const FString& VersionInfo, const FString& PlatformName );

	void GetCookOnTheFlyUnsolicitedFiles(const ITargetPlatform* TargetPlatform, TArray<FString>& UnsolicitedFiles, const FString& Filename);

	/**
	* Cook requests for a package from network
	*  blocks until cook is complete
	* 
	* @param  Filename	requested file to cook
	* @param  PlatformName platform to cook for
	* @param  out UnsolicitedFiles return a list of files which were cooked as a result of cooking the requested package
	*/
	void HandleNetworkFileServerFileRequest( const FString& Filename, const FString& PlatformName, TArray<FString>& UnsolicitedFiles );

	/**
	* Shader recompile request from network
	*  blocks until shader recompile complete
	*
	* @param  RecompileData input params for shader compile and compiled shader output
	*/
	void HandleNetworkFileServerRecompileShaders(const struct FShaderRecompileData& RecompileData);

	/**
	 * Get the sandbox path we want the network file server to use
	 */
	FString HandleNetworkGetSandboxPath();

	/**
	 * HandleNetworkGetPrecookedList 
	 * this is used specifically for cook on the fly with shared cooked builds
	 * returns the list of files which are still valid in the pak file which was initially loaded
	 * 
	 * @param PrecookedFileList all the files which are still valid in the client pak file
	 */
	void HandleNetworkGetPrecookedList( const FString& PlatformName, TMap<FString, FDateTime>& PrecookedFileList );

	//////////////////////////////////////////////////////////////////////////
	// general functions

	/**
	 * Attempts to update the metadata for a package in an asset registry generator
	 *
	 * @param Generator The asset registry generator to update
	 * @param PackageName The name of the package to update info on
	 * @param SavePackageResult The metadata to associate with the given package name
	 */
	void UpdateAssetRegistryPackageData(FAssetRegistryGenerator* Generator, const FName& PackageName, FSavePackageResultStruct& SavePackageResult);

	/** Perform any special processing for freshly loaded packages 
	 */
	void ProcessUnsolicitedPackages();

	/**
	 * Loads a package and prepares it for cooking
	 *  this is the same as a normal load but also ensures that the sublevels are loaded if they are streaming sublevels
	 *
	 * @param BuildFilename long package name of the package to load 
	 * @param OutPackage UPackage of the package loaded, non-null on success, may be non-null on failure if the UPackage existed but had another failure
	 * @return Whether LoadPackage was completely successful and the package can be cooked
	 */
	bool LoadPackageForCooking(const FString& BuildFilename, UPackage*& OutPackage);

	/**
	* Makes sure a package is fully loaded before we save it out
	* returns true if it succeeded
	*/
	bool MakePackageFullyLoaded(UPackage* Package) const;

	/**
	* Initialize the sandbox for @param TargetPlatforms
	*/
	void InitializeSandbox(const TArrayView<const ITargetPlatform* const>& TargetPlatforms);

	/**
	* Initialize the package store
	*/
	void InitializePackageStore(const TArrayView<const ITargetPlatform* const>& TargetPlatforms);

	/**
	* Finalize the package store
	*/
	void FinalizePackageStore();

	/**
	* Empties SavePackageContexts and deletes the contents
	*/
	void ClearPackageStoreContexts();

	/**
	* Initialize platforms in @param NewTargetPlatforms
	*/
	void InitializeTargetPlatforms(const TArrayView<ITargetPlatform* const>& NewTargetPlatforms);

	/**
	* Some content plugins does not support all target platforms.
	* Build up a map of unsupported packages per platform that can be checked before saving.
	*/
	void DiscoverPlatformSpecificNeverCookPackages(
		const TArrayView<const ITargetPlatform* const>& TargetPlatforms, const TArray<FString>& UBTPlatformStrings);

	/**
	* Clean up the sandbox
	*/
	void TermSandbox();

	/**
	* GetDependentPackages
	* get package dependencies according to the asset registry
	* 
	* @param Packages List of packages to use as the root set for dependency checking
	* @param Found return value, all objects which package is dependent on
	*/
	void GetDependentPackages( const TSet<UPackage*>& Packages, TSet<FName>& Found);

	/**
	* GetDependentPackages
	* get package dependencies according to the asset registry
	*
	* @param Root set of packages to use when looking for dependencies
	* @param FoundPackages list of packages which were found
	*/
	void GetDependentPackages(const TSet<FName>& RootPackages, TSet<FName>& FoundPackages);

	/**
	* ContainsWorld
	* use the asset registry to determine if a Package contains a UWorld or ULevel object
	* 
	* @param PackageName to return if it contains the a UWorld object or a ULevel
	* @return true if the Package contains a UWorld or ULevel false otherwise
	*/
	bool ContainsMap(const FName& PackageName) const;


	/** 
	 * Returns true if this package contains a redirector, and fills in paths
	 *
	 * @param PackageName to return if it contains a redirector
	 * @param RedirectedPaths map of original to redirected object paths
	 * @return true if the Package contains a redirector false otherwise
	 */
	bool ContainsRedirector(const FName& PackageName, TMap<FName, FName>& RedirectedPaths) const;
	
	/**
	 * Calls BeginCacheForCookedPlatformData on all UObjects in the package
	 *
	 * @param PackageData the PackageData used to gather all uobjects from
	 * @return false if time slice was reached, true if all objects have had BeginCacheForCookedPlatformData called
	 */
	bool BeginPackageCacheForCookedPlatformData(UE::Cook::FPackageData& PackageData, UE::Cook::FCookerTimer& Timer);
	
	/**
	 * Returns true when all objects in package have all their cooked platform data loaded
	 *	confirms that BeginCacheForCookedPlatformData is called and will return true after all objects return IsCachedCookedPlatformDataLoaded true
	 *
	 * @param PackageData the PackageData used to gather all uobjects from
	 * @return false if time slice was reached, true if all return true for IsCachedCookedPlatformDataLoaded 
	 */
	bool FinishPackageCacheForCookedPlatformData(UE::Cook::FPackageData& PackageData, UE::Cook::FCookerTimer& Timer);

	/**
	 * Frees all the memory used to call BeginCacheForCookedPlatformData on all the objects in PackageData.
	 * If the calls were incomplete because the PackageData's save was cancelled, handles canceling them and leaving any required CancelManagers in GetPendingCookedPlatformDatas
	 */
	void ReleaseCookedPlatformData(UE::Cook::FPackageData& PackageData);

	/**
	 * Poll the GetPendingCookedPlatformDatas and release their resources when they are complete.
	 * This is done inside of PumpSaveQueue, but is also required after a cancelled cook so that references to the pending objects will be eventually dropped.
	 */
	void TickCancels();

	/**
	* GetCurrentIniVersionStrings gets the current ini version strings for compare against previous cook
	* 
	* @param IniVersionStrings return list of the important current ini version strings
	* @return false if function fails (should assume all platforms are out of date)
	*/
	bool GetCurrentIniVersionStrings( const ITargetPlatform* TargetPlatform, FIniSettingContainer& IniVersionStrings ) const;

	/**
	* GetCookedIniVersionStrings gets the ini version strings used in previous cook for specified target platform
	* 
	* @param IniVersionStrings return list of the previous cooks ini version strings
	* @return false if function fails to find the ini version strings
	*/
	bool GetCookedIniVersionStrings( const ITargetPlatform* TargetPlatform, FIniSettingContainer& IniVersionStrings, TMap<FString, FString>& AdditionalStrings ) const;


	/**
	* Convert a path to a full sandbox path
	* is effected by the cooking dlc settings
	* This function should be used instead of calling the FSandbox Sandbox->ConvertToSandboxPath functions
	*/
	FString ConvertToFullSandboxPath( const FString &FileName, bool bForWrite = false ) const;
	FString ConvertToFullSandboxPath( const FString &FileName, bool bForWrite, const FString& PlatformName ) const;

	/**
	* GetSandboxAssetRegistryFilename
	* 
	* return full path of the asset registry in the sandbox
	*/
	const FString GetSandboxAssetRegistryFilename();

	const FString GetCookedAssetRegistryFilename(const FString& PlatformName);

	/**
	* Get the sandbox root directory for that platform
	* is effected by the CookingDlc settings
	* This should be used instead of calling the Sandbox function
	*/
	FString GetSandboxDirectory( const FString& PlatformName ) const;

	/* Delete the sandbox directory (asynchronously) for the given platform in preparation for a clean cook */
	void DeleteSandboxDirectory(const FString& PlatformName);

	/* Create the delete-old-cooked-directory helper. PlatformName tells it where to create its temp directory. AsyncDeleteDirectory is DeleteSandboxDirectory internal use only and should otherwise be set to nullptr. */
	FAsyncIODelete& GetAsyncIODelete(const FString& PlatformName, const FString* AsyncDeleteDirectory = nullptr);

	/** Get the directory that should be used for AsyncDeletes based on the given platform. SandboxDirectory is DeleteSandboxDirectory internal use only and should otherwise be set to nullptr. */
	FString GetAsyncDeleteDirectory(const FString& PlatformName, const FString* SandboxDirectory = nullptr) const;

	bool IsCookingDLC() const;

	/**
	* GetBaseDirectoryForDLC
	* 
	* @return return the path to the DLC
	*/
	FString GetBaseDirectoryForDLC() const;

	FString GetContentDirecctoryForDLC() const;

	bool IsCreatingReleaseVersion();

	/**
	* Loads the cooked ini version settings maps into the Ini settings cache
	* 
	* @param TargetPlatforms to look for ini settings for
	*/
	//bool CacheIniVersionStringsMap( const ITargetPlatform* TargetPlatform ) const;

	/**
	* Checks if important ini settings have changed since last cook for each target platform 
	* 
	* @param TargetPlatforms to check if out of date
	* @param OutOfDateTargetPlatforms return list of out of date target platforms which should be cleaned
	*/
	bool IniSettingsOutOfDate( const ITargetPlatform* TargetPlatform ) const;

	/**
	* Saves ini settings which are in the memory cache to the hard drive in ini files
	*
	* @param TargetPlatforms to save
	*/
	bool SaveCurrentIniSettings( const ITargetPlatform* TargetPlatform ) const;



	/**
	* IsCookFlagSet
	* 
	* @param InCookFlag used to check against the current cook flags
	* @return true if the cook flag is set false otherwise
	*/
	bool IsCookFlagSet( const ECookInitializationFlags& InCookFlags ) const 
	{
		return (CookFlags & InCookFlags) != ECookInitializationFlags::None;
	}

	/**
	*	Cook (save) the given package
	*
	*	@param	PackageData			The PackageData to cook/save
	*	@param	SaveFlags			The flags to pass to the SavePackage function
	*	@param	bOutWasUpToDate		Upon return, if true then the cooked package was cached (up to date)
	*	@param  TargetPlatforms		Only cook for target platforms which are included in this array
	*
	*	@return	ESavePackageResult::Success if packages was cooked
	*/
	void SaveCookedPackage(UE::Cook::FPackageData& PackageData, uint32 SaveFlags, const TArrayView<const ITargetPlatform* const>& TargetPlatforms, TArray<FSavePackageResultStruct>& SavePackageResults);


	/**
	*  Save the global shader map
	*  
	*  @param	Platforms		List of platforms to make global shader maps for
	*/
	void SaveGlobalShaderMapFiles(const TArrayView<const ITargetPlatform* const>& Platforms);


	/** Create sandbox file in directory using current settings supplied */
	void CreateSandboxFile();
	/** Gets the output directory respecting any command line overrides */
	FString GetOutputDirectoryOverride() const;

	/**
	* Populate the cooked packages list from the on disk content using time stamps and dependencies to figure out if they are ok
	* delete any local content which is out of date
	* 
	* @param Platforms to process
	*/
	void PopulateCookedPackagesFromDisk(const TArrayView<const ITargetPlatform* const>& Platforms);

	/**
	* Searches the disk for all the cooked files in the sandbox path provided
	* Returns a map of the uncooked file path matches to the cooked file path for each package which exists
	*
	* @param UncookedpathToCookedPath out Map of the uncooked path matched with the cooked package which exists
	* @param SandboxRootDir root dir to search for cooked packages in
	*/
	void GetAllCookedFiles(TMap<FName, FName>& UncookedPathToCookedPath, const FString& SandboxRootDir);

	/** Loads the platform-independent asset registry for use by the cooker */
	void GenerateAssetRegistry();

	/** Construct or refresh-for-filechanges the platform-specific asset registry for the given platforms */
	void RefreshPlatformAssetRegistries(const TArrayView<const ITargetPlatform* const>& TargetPlatforms);

	/** Generates long package names for all files to be cooked */
	void GenerateLongPackageNames(TArray<FName>& FilesInPath);

	/** Return the PackageNameCache that is caching FileNames on disk for this CookOnTheFlyServer. */
	const FPackageNameCache& GetPackageNameCache() const;

	uint32 FullLoadAndSave(uint32& CookedPackageCount);

	uint32		StatLoadedPackageCount = 0;
	uint32		StatSavedPackageCount = 0;

	/** This is set to true when the decision about which packages we need to cook changes because e.g. a platform was added to the sessionplatforms. */
	bool bPackageFilterDirty = false;
	/** This is set to true when PumpSaveQueue has detected it is blocked and therefore the CookOnTheFlyServer should do work elsewhere. */
	bool bIsYieldingSave = false;
	/** Tracks whether we need to do once-per-process initializations for cookbythebook. */
	bool bHasRunCookByTheBookBefore = false;

	// These helper structs are all TUniquePtr rather than inline members so that we can keep their headers private.  See class header comments for their purpose.
	TUniquePtr<UE::Cook::FPackageTracker> PackageTracker;
	TUniquePtr<UE::Cook::FPackageDatas> PackageDatas;
	TUniquePtr<UE::Cook::FExternalRequests> ExternalRequests;

	TArray<FSavePackageContext*> SavePackageContexts;
	/** Objects that were collected during the single-threaded PreGarbageCollect callback and that should be reported as referenced in CookerAddReferencedObjects. */
	TArray<UObject*> GCKeepObjects;
	TArray<FWeakObjectPtr> SavingPackageCachedObjectsInOuter;
	UE::Cook::FPackageData* SavingPackageData = nullptr;

	// temporary -- should eliminate the need for this. Only required right now because FullLoadAndSave 
	// accesses maps directly
	friend FPackageNameCache;
	friend UE::Cook::FPackageData;
	friend UE::Cook::FPendingCookedPlatformData;
	friend UE::Cook::FPlatformManager;
};
