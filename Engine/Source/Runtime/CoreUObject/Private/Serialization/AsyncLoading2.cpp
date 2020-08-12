// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	UnAsyncLoading.cpp: Unreal async loading code.
=============================================================================*/

#include "Serialization/AsyncLoading2.h"
#include "Serialization/AsyncPackageLoader.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Stats/StatsMisc.h"
#include "Misc/CoreStats.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/StringBuilder.h"
#include "UObject/ObjectResource.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/NameBatchSerialization.h"
#include "Serialization/DeferredMessageLog.h"
#include "UObject/UObjectThreadContext.h"
#include "Misc/Paths.h"
#include "Misc/ExclusiveLoadPackageTimeTracker.h"
#include "ProfilingDebugging/LoadTimeTracker.h"
#include "HAL/ThreadHeartBeat.h"
#include "HAL/ExceptionHandling.h"
#include "UObject/UObjectHash.h"
#include "Templates/Casts.h"
#include "Templates/UniquePtr.h"
#include "Serialization/BufferReader.h"
#include "Async/TaskGraphInterfaces.h"
#include "Blueprint/BlueprintSupport.h"
#include "HAL/LowLevelMemTracker.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "UObject/UObjectArchetypeInternal.h"
#include "UObject/GarbageCollectionInternal.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Serialization/LoadTimeTracePrivate.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Serialization/AsyncPackage.h"
#include "Serialization/UnversionedPropertySerialization.h"
#include "Serialization/Zenaphore.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectRedirector.h"
#include "Serialization/BulkData.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/MemoryReader.h"
#include "UObject/UObjectClusters.h"
#include "UObject/LinkerInstancingContext.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Async/Async.h"
#include "HAL/LowLevelMemStats.h"
#include "HAL/IPlatformFileOpenLogWrapper.h"

#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
PRAGMA_DISABLE_OPTIMIZATION
#endif

FArchive& operator<<(FArchive& Ar, FMappedName& MappedName)
{
	Ar << MappedName.Index << MappedName.Number;

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FContainerHeader& ContainerHeader)
{
	Ar << ContainerHeader.ContainerId;
	Ar << ContainerHeader.PackageCount;
	Ar << ContainerHeader.Names;
	Ar << ContainerHeader.NameHashes;
	Ar << ContainerHeader.PackageIds;
	Ar << ContainerHeader.StoreEntries;
	Ar << ContainerHeader.CulturePackageMap;
	Ar << ContainerHeader.PackageRedirects;

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FExportBundleEntry& ExportBundleEntry)
{
	Ar << ExportBundleEntry.LocalExportIndex;
	Ar << ExportBundleEntry.CommandType;

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FExportBundleHeader& ExportBundleHeader)
{
	Ar << ExportBundleHeader.FirstEntryIndex;
	Ar << ExportBundleHeader.EntryCount;

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FScriptObjectEntry& ScriptObjectEntry)
{
	Ar << ScriptObjectEntry.ObjectName.Index << ScriptObjectEntry.ObjectName.Number;
	Ar << ScriptObjectEntry.GlobalIndex;
	Ar << ScriptObjectEntry.OuterIndex;
	Ar << ScriptObjectEntry.CDOClassIndex;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FExportMapEntry& ExportMapEntry)
{
	Ar << ExportMapEntry.CookedSerialOffset;
	Ar << ExportMapEntry.CookedSerialSize;
	Ar << ExportMapEntry.ObjectName;
	Ar << ExportMapEntry.OuterIndex;
	Ar << ExportMapEntry.ClassIndex;
	Ar << ExportMapEntry.SuperIndex;
	Ar << ExportMapEntry.TemplateIndex;
	Ar << ExportMapEntry.GlobalImportIndex;

	uint32 ObjectFlags = uint32(ExportMapEntry.ObjectFlags);
	Ar << ObjectFlags;
	
	if (Ar.IsLoading())
	{
		ExportMapEntry.ObjectFlags = EObjectFlags(ObjectFlags);
	}

	uint8 FilterFlags = uint8(ExportMapEntry.FilterFlags);
	Ar << FilterFlags;

	if (Ar.IsLoading())
	{
		ExportMapEntry.FilterFlags = EExportFilterFlags(FilterFlags);
	}

	Ar.Serialize(&ExportMapEntry.Pad, sizeof(ExportMapEntry.Pad));

	return Ar;
}

uint64 FPackageObjectIndex::GenerateImportHashFromObjectPath(const FStringView& ObjectPath)
{
	TArray<TCHAR, TInlineAllocator<FName::StringBufferSize>> FullImportPath;
	const int32 Len = ObjectPath.Len();
	FullImportPath.AddUninitialized(Len);
	for (int32 I = 0; I < Len; ++I)
	{
		if (ObjectPath[I] == TEXT('.') || ObjectPath[I] == TEXT(':'))
		{
			FullImportPath[I] = TEXT('/');
		}
		else
		{
			FullImportPath[I] = TChar<TCHAR>::ToLower(ObjectPath[I]);
		}
	}
	uint64 Hash = CityHash64(reinterpret_cast<const char*>(FullImportPath.GetData()), Len * sizeof(TCHAR));
	Hash &= ~(3ull << 62ull);
	return Hash;
}

void FindAllRuntimeScriptPackages(TArray<UPackage*>& OutPackages)
{
	OutPackages.Empty(256);
	ForEachObjectOfClass(UPackage::StaticClass(), [&OutPackages](UObject* InPackageObj)
	{
		UPackage* Package = CastChecked<UPackage>(InPackageObj);
		if (Package->HasAnyPackageFlags(PKG_CompiledIn) && !Package->HasAnyPackageFlags(PKG_EditorOnly))
		{
			TCHAR Buffer[FName::StringBufferSize];
			if (FStringView(Buffer, Package->GetFName().ToString(Buffer)).StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
			{
				OutPackages.Add(Package);
			}
		}
	}, /*bIncludeDerivedClasses*/false);
}

#if WITH_ASYNCLOADING2

#ifndef ALT2_VERIFY_ASYNC_FLAGS
#define ALT2_VERIFY_ASYNC_FLAGS DO_CHECK
#endif

#ifndef ALT2_VERIFY_RECURSIVE_LOADS
#define ALT2_VERIFY_RECURSIVE_LOADS DO_CHECK
#endif

#ifndef ALT2_LOG_VERBOSE
#define ALT2_LOG_VERBOSE DO_CHECK
#endif

static TSet<FPackageId> GAsyncLoading2_DebugPackageIds;
static FString GAsyncLoading2_DebugPackageNamesString;
static TSet<FPackageId> GAsyncLoading2_VerbosePackageIds;
static FString GAsyncLoading2_VerbosePackageNamesString;
#if !UE_BUILD_SHIPPING
static void ParsePackageNames(const FString& PackageNamesString, TSet<FPackageId>& PackageIds)
{
	TArray<FString> Args;
	const TCHAR* Delimiters[] = { TEXT(","), TEXT(" ") };
	PackageNamesString.ParseIntoArray(Args, Delimiters, UE_ARRAY_COUNT(Delimiters), true);
	PackageIds.Empty(Args.Num());
	for (const FString& PackageName : Args)
	{
		PackageIds.Add(FPackageId::FromName(FName(*PackageName)));
	}
}
static FAutoConsoleVariableRef CVar_DebugPackageNames(
	TEXT("s.DebugPackageNames"),
	GAsyncLoading2_DebugPackageNamesString,
	TEXT("Add debug breaks for all listed package names, also automatically added to s.VerbosePackageNames."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable)
	{
		ParsePackageNames(Variable->GetString(), GAsyncLoading2_DebugPackageIds);
		ParsePackageNames(Variable->GetString(), GAsyncLoading2_VerbosePackageIds);
	}),
	ECVF_Default);
static FAutoConsoleVariableRef CVar_VerbosePackageNames(
	TEXT("s.VerbosePackageNames"),
	GAsyncLoading2_VerbosePackageNamesString,
	TEXT("Restrict verbose logging to listed package names."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable)
	{
		ParsePackageNames(Variable->GetString(), GAsyncLoading2_VerbosePackageIds);
	}),
	ECVF_Default);
#endif

#define UE_ASYNC_PACKAGE_DEBUG(PackageDesc) \
if ((GAsyncLoading2_VerbosePackageIds.Num() > 0) && \
	(GAsyncLoading2_DebugPackageIds.Contains((PackageDesc).CustomPackageId) || \
	 GAsyncLoading2_DebugPackageIds.Contains((PackageDesc).DiskPackageId))) \
{ \
	UE_DEBUG_BREAK(); \
}

// The ELogVerbosity::VerbosityMask is used to silence PVS,
// using constexpr gave the same warning, and the disable comment can can't be used in a macro: //-V501 
// warning V501: There are identical sub-expressions 'ELogVerbosity::Verbose' to the left and to the right of the '<' operator.
#define UE_ASYNC_PACKAGE_LOG(Verbosity, PackageDesc, LogDesc, Format, ...) \
if (GAsyncLoading2_VerbosePackageIds.Num() == 0 || \
	(ELogVerbosity::Type(ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) < ELogVerbosity::Verbose) || \
	GAsyncLoading2_VerbosePackageIds.Contains((PackageDesc).CustomPackageId) || \
	GAsyncLoading2_VerbosePackageIds.Contains((PackageDesc).DiskPackageId)) \
{ \
	if (!(PackageDesc).CustomPackageName.IsNone()) \
	{ \
		UE_LOG(LogStreaming, Verbosity, LogDesc TEXT(": %s (0x%llX) %s (0x%llX) - ") Format, \
			*(PackageDesc).CustomPackageName.ToString(), \
			(PackageDesc).CustomPackageId.ValueForDebugging(), \
			*(PackageDesc).DiskPackageName.ToString(), \
			(PackageDesc).DiskPackageId.ValueForDebugging(), \
			##__VA_ARGS__); \
	} \
	else \
	{ \
		UE_LOG(LogStreaming, Verbosity, LogDesc TEXT(": %s (0x%llX) - ") Format, \
			*(PackageDesc).DiskPackageName.ToString(), \
			(PackageDesc).DiskPackageId.ValueForDebugging(), \
			##__VA_ARGS__); \
	} \
}

#define UE_ASYNC_PACKAGE_CLOG(Condition, Verbosity, PackageDesc, LogDesc, Format, ...) \
if ((Condition)) \
{ \
	UE_ASYNC_PACKAGE_LOG(Verbosity, PackageDesc, LogDesc, Format, ##__VA_ARGS__); \
}

#if ALT2_LOG_VERBOSE
#define UE_ASYNC_PACKAGE_LOG_VERBOSE(Verbosity, PackageDesc, LogDesc, Format, ...) \
	UE_ASYNC_PACKAGE_LOG(Verbosity, PackageDesc, LogDesc, Format, ##__VA_ARGS__)
#define UE_ASYNC_PACKAGE_CLOG_VERBOSE(Condition, Verbosity, PackageDesc, LogDesc, Format, ...) \
	UE_ASYNC_PACKAGE_CLOG(Condition, Verbosity, PackageDesc, LogDesc, Format, ##__VA_ARGS__)
#else
#define UE_ASYNC_PACKAGE_LOG_VERBOSE(Verbosity, PackageDesc, LogDesc, Format, ...)
#define UE_ASYNC_PACKAGE_CLOG_VERBOSE(Condition, Verbosity, PackageDesc, LogDesc, Format, ...)
#endif

TRACE_DECLARE_INT_COUNTER(PendingBundleIoRequests, TEXT("AsyncLoading/PendingBundleIoRequests"));

struct FAsyncPackage2;
class FAsyncLoadingThread2;

class FSimpleArchive final
	: public FArchive
{
public:
	FSimpleArchive(const uint8* BufferPtr, uint64 BufferSize)
	{
		ActiveFPLB->OriginalFastPathLoadBuffer = BufferPtr;
		ActiveFPLB->StartFastPathLoadBuffer = BufferPtr;
		ActiveFPLB->EndFastPathLoadBuffer = BufferPtr + BufferSize;
	}

	int64 TotalSize() override
	{
		return ActiveFPLB->EndFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer;
	}

	int64 Tell() override
	{
		return ActiveFPLB->StartFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer;
	}

	void Seek(int64 Position) override
	{
		ActiveFPLB->StartFastPathLoadBuffer = ActiveFPLB->OriginalFastPathLoadBuffer + Position;
		check(ActiveFPLB->StartFastPathLoadBuffer <= ActiveFPLB->EndFastPathLoadBuffer);
	}

	void Serialize(void* Data, int64 Length) override
	{
		if (!Length || IsError())
		{
			return;
		}
		check(ActiveFPLB->StartFastPathLoadBuffer + Length <= ActiveFPLB->EndFastPathLoadBuffer);
		FMemory::Memcpy(Data, ActiveFPLB->StartFastPathLoadBuffer, Length);
		ActiveFPLB->StartFastPathLoadBuffer += Length;
	}
};

struct FExportObject
{
	UObject* Object = nullptr;
	bool bFiltered = false;
	bool bExportLoadFailed = false;
};

struct FAsyncPackageDesc2
{
	// A unique request id for each external call to LoadPackage
	int32 RequestID;
	// The package store entry with meta data about the actual disk package
	const FPackageStoreEntry* StoreEntry;
	// The disk package id corresponding to the StoreEntry
	// It is used by the loader for io chunks and to handle ref tracking of loaded packages and import objects.
	FPackageId DiskPackageId; 
	// The custom package id is only set for temp packages with a valid but "fake" CustomPackageName,
	// if set, it will be used as key when tracking active async packages in AsyncPackageLookup
	FPackageId CustomPackageId;
	// The disk package name from the LoadPackage call, or none for imported packages
	// up until the package summary has been serialized
	FName DiskPackageName;
	// The custom package name from the LoadPackage call is only used for temp packages,
	// if set, it will be used as the runtime UPackage name
	FName CustomPackageName;
	// Set from the package summary,
	FName SourcePackageName;
	/** Delegate called on completion of loading. This delegate can only be created and consumed on the game thread */
	TUniquePtr<FLoadPackageAsyncDelegate> PackageLoadedDelegate;

	FAsyncPackageDesc2(
		int32 InRequestID,
		FPackageId InPackageIdToLoad,
		const FPackageStoreEntry* InStoreEntry,
		FName InDiskPackageName = FName(),
		FPackageId InPackageId = FPackageId(),
		FName InCustomName = FName(),
		TUniquePtr<FLoadPackageAsyncDelegate>&& InCompletionDelegate = TUniquePtr<FLoadPackageAsyncDelegate>())
		: RequestID(InRequestID)
		, StoreEntry(InStoreEntry)
		, DiskPackageId(InPackageIdToLoad)
		, CustomPackageId(InPackageId)
		, DiskPackageName(InDiskPackageName)
		, CustomPackageName(InCustomName)
		, PackageLoadedDelegate(MoveTemp(InCompletionDelegate))
	{
	}

	/** This constructor does not modify the package loaded delegate as this is not safe outside the game thread */
	FAsyncPackageDesc2(const FAsyncPackageDesc2& OldPackage)
		: RequestID(OldPackage.RequestID)
		, StoreEntry(OldPackage.StoreEntry)
		, DiskPackageId(OldPackage.DiskPackageId)
		, CustomPackageId(OldPackage.CustomPackageId)
		, DiskPackageName(OldPackage.DiskPackageName)
		, CustomPackageName(OldPackage.CustomPackageName)
	{
	}

	/** This constructor will explicitly copy the package loaded delegate and invalidate the old one */
	FAsyncPackageDesc2(const FAsyncPackageDesc2& OldPackage, TUniquePtr<FLoadPackageAsyncDelegate>&& InPackageLoadedDelegate)
		: FAsyncPackageDesc2(OldPackage)
	{
		PackageLoadedDelegate = MoveTemp(InPackageLoadedDelegate);
	}

	void SetDiskPackageName(FName SerializedDiskPackageName, FName SerializedSourcePackageName = FName())
	{
		check(DiskPackageName.IsNone() || DiskPackageName == SerializedDiskPackageName);
		check(SourcePackageName.IsNone());
		DiskPackageName = SerializedDiskPackageName;
		SourcePackageName = SerializedSourcePackageName;
	}

	bool IsTrackingPublicExports() const
	{
		return CustomPackageName.IsNone();
	}

	/**
	 * The UPackage name is used by the engine and game code for in-memory and network communication.
	 */
	FName GetUPackageName() const
	{
		if (!CustomPackageName.IsNone())
		{
			// temp packages
			return CustomPackageName;
		}
		else if(!SourcePackageName.IsNone())
		{
			// localized packages
			return SourcePackageName;
		}
		// normal packages
		return DiskPackageName;
	}

	/**
	 * The AsyncPackage id is used by the loader as a key in AsyncPackageLookup to track active load requests,
	 * which in turn is used for looking up packages for setting up serialized arcs (mostly post load dependencies).
	 */
	FORCEINLINE FPackageId GetAsyncPackageId() const
	{
		return CustomPackageId.IsValid() ? CustomPackageId : DiskPackageId;
	}

#if DO_GUARD_SLOW
	~FAsyncPackageDesc2()
	{
		checkSlow(!PackageLoadedDelegate.IsValid() || IsInGameThread());
	}
#endif
};

using FExportObjects = TArray<FExportObject>;

class FNameMap
{
public:
	void LoadGlobal(FIoDispatcher& IoDispatcher)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LoadGlobalNameMap);

		check(NameEntries.Num() == 0);

		FIoChunkId NamesId = CreateIoChunkId(0, 0, EIoChunkType::LoaderGlobalNames);
		FIoChunkId HashesId = CreateIoChunkId(0, 0, EIoChunkType::LoaderGlobalNameHashes);

		FIoBatch Batch = IoDispatcher.NewBatch();
		FIoRequest NameRequest = Batch.Read(NamesId, FIoReadOptions());
		FIoRequest HashRequest = Batch.Read(HashesId, FIoReadOptions());
		Batch.Issue(IoDispatcherPriority_High);

		ReserveNameBatch(	IoDispatcher.GetSizeForChunk(NamesId).ValueOrDie(),
							IoDispatcher.GetSizeForChunk(HashesId).ValueOrDie());

		Batch.Wait();

		FIoBuffer NameBuffer = NameRequest.GetResult().ConsumeValueOrDie();
		FIoBuffer HashBuffer = HashRequest.GetResult().ConsumeValueOrDie();

		Load(MakeArrayView(NameBuffer.Data(), NameBuffer.DataSize()), MakeArrayView(HashBuffer.Data(), HashBuffer.DataSize()), FMappedName::EType::Global);

		IoDispatcher.FreeBatch(Batch);
	}

	int32 Num() const
	{
		return NameEntries.Num();
	}

	void Load(TArrayView<const uint8> NameBuffer, TArrayView<const uint8> HashBuffer, FMappedName::EType InNameMapType)
	{
		LoadNameBatch(NameEntries, NameBuffer, HashBuffer);
		NameMapType = InNameMapType;
	}

	FName GetName(const FMappedName& MappedName) const
	{
		check(MappedName.GetType() == NameMapType);
		check(MappedName.GetIndex() < uint32(NameEntries.Num()));
		FNameEntryId NameEntry = NameEntries[MappedName.GetIndex()];
		return FName::CreateFromDisplayId(NameEntry, MappedName.GetNumber());
	}

	bool TryGetName(const FMappedName& MappedName, FName& OutName) const
	{
		check(MappedName.GetType() == NameMapType);
		uint32 Index = MappedName.GetIndex();
		if (Index < uint32(NameEntries.Num()))
		{
			FNameEntryId NameEntry = NameEntries[MappedName.GetIndex()];
			OutName = FName::CreateFromDisplayId(NameEntry, MappedName.GetNumber());
			return true;
		}
		return false;
	}

	FMinimalName GetMinimalName(const FMappedName& MappedName) const
	{
		check(MappedName.GetType() == NameMapType);
		check(MappedName.GetIndex() < uint32(NameEntries.Num()));
		FNameEntryId NameEntry = NameEntries[MappedName.GetIndex()];
		return FMinimalName(NameEntry, MappedName.GetNumber());
	}

private:
	TArray<FNameEntryId> NameEntries;
	FMappedName::EType NameMapType = FMappedName::EType::Global;
};

struct FPublicExport
{
	UObject* Object = nullptr;
	FPackageId PackageId; // for fast clear of package load status during GC
};

struct FGlobalImportStore
{
	TMap<FPackageObjectIndex, UObject*> ScriptObjects;
	TMap<FPackageObjectIndex, FPublicExport> PublicExportObjects;
	TMap<int32, FPackageObjectIndex> ObjectIndexToPublicExport;
	// Temporary initial load data
	TArray<FScriptObjectEntry> ScriptObjectEntries;
	TMap<FPackageObjectIndex, FScriptObjectEntry*> ScriptObjectEntriesMap;

	FGlobalImportStore()
	{
		PublicExportObjects.Reserve(32768);
		ObjectIndexToPublicExport.Reserve(32768);
	}

	FPackageId RemovePublicExport(UObject* InObject)
	{
		FPackageId PackageId;
		int32 ObjectIndex = GUObjectArray.ObjectToIndex(InObject);
		FPackageObjectIndex GlobalIndex;
		if (ObjectIndexToPublicExport.RemoveAndCopyValue(ObjectIndex, GlobalIndex))
		{
			FPublicExport PublicExport;
			bool bSuccess = PublicExportObjects.RemoveAndCopyValue(GlobalIndex, PublicExport);
			checkf(bSuccess, TEXT("Missing entry in ImportStore for object %s with id 0x%llX"), *InObject->GetPathName(), GlobalIndex.Value());
			checkf(PublicExport.Object == InObject, TEXT("Mismatch in ImportStore for %s with id 0x%llX"), *InObject->GetPathName(), GlobalIndex.Value());
			PackageId = PublicExport.PackageId;
		}
		return PackageId;
	}

	inline UObject* GetPublicExportObject(FPackageObjectIndex GlobalIndex)
	{
		check(GlobalIndex.IsPackageImport());
		UObject* Object = nullptr;
		FPublicExport* PublicExport = PublicExportObjects.Find(GlobalIndex);
		if (PublicExport)
		{
			Object = PublicExport->Object;
			checkf(Object && !Object->IsUnreachable(), TEXT("%s"), Object ? *Object->GetFullName() : TEXT("null"));
		}
		return Object;
	}

	UObject* FindScriptImportObjectFromIndex(FPackageObjectIndex ScriptImportIndex);

	inline UObject* FindOrGetImportObject(FPackageObjectIndex GlobalIndex)
	{
		check(GlobalIndex.IsImport());
		UObject* Object = nullptr;
		if (GlobalIndex.IsScriptImport())
		{
			if (GIsInitialLoad)
			{
				Object = FindScriptImportObjectFromIndex(GlobalIndex);
			}
			else
			{
				Object = ScriptObjects.FindRef(GlobalIndex);
			}
		}
		else
		{
			Object = GetPublicExportObject(GlobalIndex);
		}
		return Object;
	}

	void StoreGlobalObject(FPackageId PackageId, FPackageObjectIndex GlobalIndex, UObject* Object)
	{
		check(GlobalIndex.IsPackageImport());
		int32 ObjectIndex = GUObjectArray.ObjectToIndex(Object);
		PublicExportObjects.Add(GlobalIndex, {Object, PackageId});
		ObjectIndexToPublicExport.Add(ObjectIndex, GlobalIndex);
	}

	void FindAllScriptObjects();
};

class FLoadedPackageRef
{
	UPackage* Package = nullptr;
	int32 RefCount = 0;
	bool bIsLoaded = false;
	bool bIsMissing = false;

public:
	inline int32 GetRefCount() const
	{
		return RefCount;
	}

	inline bool AddRef()
	{
		++RefCount;
		// is this the first reference to an already fully loaded package?
		return RefCount == 1 && bIsLoaded;
	}

	inline bool ReleaseRef()
	{
		check(RefCount > 0);
		--RefCount;
#if DO_CHECK
		check(bIsLoaded || bIsMissing);
		if (bIsLoaded)
		{
			check(!bIsMissing);
		}
		if (bIsMissing)
		{
			check(!bIsLoaded);
		}
#endif
		// is this the last reference to a fully loaded package?
		return RefCount == 0 && bIsLoaded;
	}

	inline UPackage* GetPackage() const
	{
#if DO_CHECK
		if (Package)
		{
			check(!bIsMissing);
			check(!Package->IsUnreachable());
		}
		else
		{
			check(!bIsLoaded);
		}
#endif
		return Package;
	}

	inline void SetPackage(UPackage* InPackage)
	{
		check(!bIsLoaded);
		check(!bIsMissing);
		check(!Package);
		Package = InPackage;
	}

	inline bool AreAllPublicExportsLoaded() const
	{
		return bIsLoaded;
	}

	inline void SetAllPublicExportsLoaded()
	{
		check(!bIsMissing);
		check(Package);
		bIsMissing = false;
		bIsLoaded = true;
	}

	inline void ClearAllPublicExportsLoaded()
	{
		check(!bIsMissing);
		check(Package);
		bIsMissing = false;
		bIsLoaded = false;
	}

	inline bool IsMissingPackage() const
	{
		return bIsMissing;
	}

	inline void SetIsMissingPackage()
	{
		check(!bIsLoaded);
		check(!Package);
		bIsMissing = true;
		bIsLoaded = false;
	}

	inline void ClearIsMissingPackage()
	{
		check(!bIsLoaded);
		check(!Package);
		bIsMissing = false;
		bIsLoaded = false;
	}
};

class FLoadedPackageStore
{
private:
	// Packages in active loading or completely loaded packages, with Desc.DiskPackageName as key.
	// Does not track temp packages with custom UPackage names, since they are never imorted by other packages.
	TMap<FPackageId, FLoadedPackageRef> Packages;

public:
	FLoadedPackageStore()
	{
		Packages.Reserve(32768);
	}

	int32 NumTracked() const
	{
		return Packages.Num();
	}

	inline FLoadedPackageRef* FindPackageRef(FPackageId PackageId)
	{
		return Packages.Find(PackageId);
	}

	inline FLoadedPackageRef& GetPackageRef(FPackageId PackageId)
	{
		return Packages.FindOrAdd(PackageId);
	}

	inline bool Remove(FPackageId PackageId)
	{
#if DO_CHECK
		FLoadedPackageRef* Ref = Packages.Find(PackageId);
		check(!Ref || Ref->GetRefCount() == 0);
#endif
		return Packages.Remove(PackageId) > 0;
	}

#if ALT2_VERIFY_ASYNC_FLAGS
	void VerifyLoadedPackages()
	{
		for (TPair<FPackageId, FLoadedPackageRef>& Pair : Packages)
		{
			FPackageId& PackageId = Pair.Key;
			FLoadedPackageRef& Ref = Pair.Value;
			ensureMsgf(Ref.GetRefCount() == 0,
				TEXT("PackageId '0x%llX' with ref count %d should not have a ref count now")
				TEXT(", or this check is incorrectly reached during active loading."),
				PackageId.Value(),
				Ref.GetRefCount());
		}
	}
#endif
};

class FPackageStore
{
public:
	FPackageStore(FIoDispatcher& InIoDispatcher, FNameMap& InGlobalNameMap)
		: IoDispatcher(InIoDispatcher)
		, GlobalNameMap(InGlobalNameMap) { }
	
	struct FLoadedContainer
	{
		TUniquePtr<FNameMap> ContainerNameMap;
		TArray<uint8> StoreEntries; //FPackageStoreEntry[PackageCount];
		uint32 PackageCount;
		int32 Order = 0;
		bool bValid = false;
	};

	FIoDispatcher& IoDispatcher;
	FNameMap& GlobalNameMap;
	TMap<FIoContainerId, TUniquePtr<FLoadedContainer>> LoadedContainers;

	FString CurrentCulture;

	FCriticalSection PackageNameMapsCritical;

	TMap<FPackageId, FPackageStoreEntry*> StoreEntriesMap;
	TMap<FPackageId, FPackageId> RedirectsPackageMap;
	int32 NextCustomPackageIndex = 0;

	FGlobalImportStore ImportStore;
	FLoadedPackageStore LoadedPackageStore;
	int32 ScriptArcsCount = 0;

public:
	void Initialize()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(InitializePackageStore);

		CurrentCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
		FParse::Value(FCommandLine::Get(), TEXT("CULTURE="), CurrentCulture);

		FPackageName::DoesPackageExistOverride().BindLambda([this](FName PackageName)
		{
			FPackageId PackageId = FPackageId::FromName(PackageName);
			FScopeLock Lock(&PackageNameMapsCritical);
			return StoreEntriesMap.Contains(PackageId);
		});
	}

	void SetupInitialLoadData()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SetupInitialLoadData);

		FIoBuffer InitialLoadIoBuffer;
		FEvent* InitialLoadEvent = FPlatformProcess::GetSynchEventFromPool();

		IoDispatcher.ReadWithCallback(
			CreateIoChunkId(0, 0, EIoChunkType::LoaderInitialLoadMeta),
			FIoReadOptions(),
			IoDispatcherPriority_High,
			[InitialLoadEvent, &InitialLoadIoBuffer](TIoStatusOr<FIoBuffer> Result)
			{
				InitialLoadIoBuffer = Result.ConsumeValueOrDie();
				InitialLoadEvent->Trigger();
			});

		InitialLoadEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(InitialLoadEvent);

		FLargeMemoryReader InitialLoadArchive(InitialLoadIoBuffer.Data(), InitialLoadIoBuffer.DataSize());
		int32 NumScriptObjects = 0;
		InitialLoadArchive << NumScriptObjects;
		ImportStore.ScriptObjectEntries = MakeArrayView(reinterpret_cast<const FScriptObjectEntry*>(InitialLoadIoBuffer.Data() + InitialLoadArchive.Tell()), NumScriptObjects);

		ImportStore.ScriptObjectEntriesMap.Reserve(ImportStore.ScriptObjectEntries.Num());
		for (FScriptObjectEntry& ScriptObjectEntry : ImportStore.ScriptObjectEntries)
		{
			const FMappedName& MappedName = FMappedName::FromMinimalName(ScriptObjectEntry.ObjectName);
			check(MappedName.IsGlobal());
			ScriptObjectEntry.ObjectName = GlobalNameMap.GetMinimalName(MappedName);

			ImportStore.ScriptObjectEntriesMap.Add(ScriptObjectEntry.GlobalIndex, &ScriptObjectEntry);
		}
	}

	void LoadContainers(TArrayView<const FIoDispatcherMountedContainer> Containers)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LoadContainers);

		int32 ContainersToLoad = 0;

		for (const FIoDispatcherMountedContainer& Container : Containers)
		{
			if (Container.ContainerId.IsValid())
			{
				++ContainersToLoad;
			}
		}

		if (!ContainersToLoad)
		{
			return;
		}

		TAtomic<int32> Remaining(ContainersToLoad);

		FEvent* Event = FPlatformProcess::GetSynchEventFromPool();

		for (const FIoDispatcherMountedContainer& Container : Containers)
		{
			const FIoContainerId& ContainerId = Container.ContainerId;
			if (!ContainerId.IsValid())
			{
				continue;
			}

			TUniquePtr<FLoadedContainer>& LoadedContainerPtr = LoadedContainers.FindOrAdd(ContainerId);
			if (!LoadedContainerPtr)
			{
				LoadedContainerPtr.Reset(new FLoadedContainer);
			}
			FLoadedContainer& LoadedContainer = *LoadedContainerPtr;
			if (LoadedContainer.bValid && LoadedContainer.Order >= Container.Environment.GetOrder())
			{
				UE_LOG(LogStreaming, Log, TEXT("Skipping loading mounted container ID '0x%llX', already loaded with higher order"), ContainerId.Value());
				if (--Remaining == 0)
				{
					Event->Trigger();
				}
				continue;
			}

			UE_LOG(LogStreaming, Log, TEXT("Loading mounted container ID '0x%llX'"), ContainerId.Value());
			LoadedContainer.bValid = true;
			LoadedContainer.Order = Container.Environment.GetOrder();

			FIoChunkId HeaderChunkId = CreateIoChunkId(ContainerId.Value(), 0, EIoChunkType::ContainerHeader);
			IoDispatcher.ReadWithCallback(HeaderChunkId, FIoReadOptions(), IoDispatcherPriority_High, [this, &Remaining, Event, &LoadedContainer](TIoStatusOr<FIoBuffer> Result)
			{
				// Execution method Thread will run the async block synchronously when multithreading is NOT supported
				const EAsyncExecution ExecutionMethod = FPlatformProcess::SupportsMultithreading() ? EAsyncExecution::TaskGraph : EAsyncExecution::Thread;

				Async(ExecutionMethod, [this, &Remaining, Event, IoBuffer = Result.ConsumeValueOrDie(), &LoadedContainer]()
				{
					LLM_SCOPE(ELLMTag::AsyncLoading);

					FMemoryReaderView Ar(MakeArrayView(IoBuffer.Data(), IoBuffer.DataSize()));

					FContainerHeader ContainerHeader;
					Ar << ContainerHeader;

					const bool bHasContainerLocalNameMap = ContainerHeader.Names.Num() > 0;
					if (bHasContainerLocalNameMap)
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(LoadContainerNameMap);
						LoadedContainer.ContainerNameMap.Reset(new FNameMap());
						LoadedContainer.ContainerNameMap->Load(ContainerHeader.Names, ContainerHeader.NameHashes, FMappedName::EType::Container);
					}

					LoadedContainer.PackageCount = ContainerHeader.PackageCount;
					LoadedContainer.StoreEntries = MoveTemp(ContainerHeader.StoreEntries);
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(AddPackages);
						FScopeLock Lock(&PackageNameMapsCritical);

						TArrayView<FPackageStoreEntry> StoreEntries(reinterpret_cast<FPackageStoreEntry*>(LoadedContainer.StoreEntries.GetData()), LoadedContainer.PackageCount);

						int32 Index = 0;
						StoreEntriesMap.Reserve(StoreEntriesMap.Num() + LoadedContainer.PackageCount);
						for (FPackageStoreEntry& ContainerEntry : StoreEntries)
						{
							const FPackageId& PackageId = ContainerHeader.PackageIds[Index];

							FPackageStoreEntry*& GlobalEntry = StoreEntriesMap.FindOrAdd(PackageId);
							if (!GlobalEntry)
							{
								GlobalEntry = &ContainerEntry;
							}
							++Index;
						}

						{
							TRACE_CPUPROFILER_EVENT_SCOPE(LoadPackageStoreLocalization);
							FSourceToLocalizedPackageIdMap* LocalizedPackages = ContainerHeader.CulturePackageMap.Find(CurrentCulture);
							if (LocalizedPackages)
							{
								for (auto& Pair : *LocalizedPackages)
								{
									const FPackageId& SourceId = Pair.Key;
									const FPackageId& LocalizedId = Pair.Value;
									RedirectsPackageMap.Emplace(SourceId, LocalizedId);
								}
							}
						}

						{
							TRACE_CPUPROFILER_EVENT_SCOPE(LoadPackageStoreRedirects);
							for (const TPair<FPackageId, FPackageId>& Redirect : ContainerHeader.PackageRedirects)
							{
								RedirectsPackageMap.Emplace(Redirect.Key, Redirect.Value);
							}
						}
					}

					if (--Remaining == 0)
					{
						Event->Trigger();
					}
				});
			});
		}

		Event->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Event);

		ApplyRedirects(RedirectsPackageMap);
	}

	void OnContainerMounted(const FIoDispatcherMountedContainer& Container)
	{
		LLM_SCOPE(ELLMTag::AsyncLoading);
		LoadContainers(MakeArrayView(&Container, 1));
	}

	void ApplyRedirects(const TMap<FPackageId, FPackageId>& Redirects)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ApplyRedirects);

		FScopeLock Lock(&PackageNameMapsCritical);

		if (Redirects.Num() == 0)
		{
			return;
		}

		for (auto It = Redirects.CreateConstIterator(); It; ++It)
		{
			const FPackageId& SourceId = It.Key();
			const FPackageId& RedirectId = It.Value();
			check(RedirectId.IsValid());
			FPackageStoreEntry* RedirectEntry = StoreEntriesMap.FindRef(RedirectId);
			check(RedirectEntry);
			FPackageStoreEntry*& PackageEntry = StoreEntriesMap.FindOrAdd(SourceId);
			if (RedirectEntry && PackageEntry)
			{
				PackageEntry = RedirectEntry;
			}
		}

		for (auto It = StoreEntriesMap.CreateIterator(); It; ++It)
		{
			FPackageStoreEntry* StoreEntry = It.Value();

			for (FPackageId& ImportedPackageId : StoreEntry->ImportedPackages)
			{
				if (const FPackageId* RedirectId = Redirects.Find(ImportedPackageId))
				{
					ImportedPackageId = *RedirectId;
				}
			}
		}
	}

	void FinalizeInitialLoad()
	{
		ImportStore.FindAllScriptObjects();

		UE_LOG(LogStreaming, Display, TEXT("AsyncLoading2 - InitialLoad Finalized: %d script object entries in %.2f KB"),
			ImportStore.ScriptObjects.Num(), (float)ImportStore.ScriptObjects.GetAllocatedSize() / 1024.f);
	}

	inline FGlobalImportStore& GetGlobalImportStore()
	{
		return ImportStore;
	}

	void RemovePackage(UPackage* Package)
	{
		check(IsGarbageCollecting());
		FPackageId PackageId = Package->GetPackageId();
		if (!LoadedPackageStore.Remove(PackageId))
		{
			FPackageId* RedirectedId = RedirectsPackageMap.Find(PackageId);
			if (RedirectedId)
			{
				LoadedPackageStore.Remove(*RedirectedId);
			}
		}
	}

	void RemovePublicExport(UObject* Object)
	{
		FPackageId PackageId = ImportStore.RemovePublicExport(Object);
		if (PackageId.IsValid())
		{
			FLoadedPackageRef* PackageRef = LoadedPackageStore.FindPackageRef(PackageId);
			if (PackageRef)
			{
				PackageRef->ClearAllPublicExportsLoaded();
			}
		}
	}

	inline const FPackageStoreEntry* FindStoreEntry(FPackageId PackageId)
	{
		FScopeLock Lock(&PackageNameMapsCritical);
		FPackageStoreEntry* Entry = StoreEntriesMap.FindRef(PackageId);
		return Entry;
	}
};

struct FPackageImportStore
{
	FPackageStore& GlobalPackageStore;
	FGlobalImportStore& GlobalImportStore;
	const FAsyncPackageDesc2& Desc;
	TArrayView<const FPackageObjectIndex> ImportMap;

	FPackageImportStore(FPackageStore& InGlobalPackageStore, const FAsyncPackageDesc2& InDesc)
		: GlobalPackageStore(InGlobalPackageStore)
		, GlobalImportStore(GlobalPackageStore.ImportStore)
		, Desc(InDesc)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(NewPackageImportStore);
		AddPackageReferences();
	}

	~FPackageImportStore()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DeletePackageImportStore);
		check(ImportMap.Num() == 0);
		ReleasePackageReferences();
	}

	inline bool IsValidLocalImportIndex(FPackageIndex LocalIndex)
	{
		check(ImportMap.Num() > 0);
		return LocalIndex.IsImport() && LocalIndex.ToImport() < ImportMap.Num();
	}

	inline UObject* FindOrGetImportObjectFromLocalIndex(FPackageIndex LocalIndex)
	{
		check(LocalIndex.IsImport());
		check(ImportMap.Num() > 0);
		const int32 LocalImportIndex = LocalIndex.ToImport();
		check(LocalImportIndex < ImportMap.Num());
		const FPackageObjectIndex GlobalIndex = ImportMap[LocalIndex.ToImport()];
		UObject* Object = nullptr;
		if (GlobalIndex.IsImport())
		{
			Object = GlobalImportStore.FindOrGetImportObject(GlobalIndex);
		}
		else
		{
			check(GlobalIndex.IsNull());
		}
		return Object;
	}

	inline UObject* FindOrGetImportObject(FPackageObjectIndex GlobalIndex)
	{
		check(GlobalIndex.IsImport());
		return GlobalImportStore.FindOrGetImportObject(GlobalIndex);
	}

	bool GetUnresolvedCDOs(TArray<UClass*, TInlineAllocator<8>>& Classes)
	{
		for (const FPackageObjectIndex& Index : ImportMap)
		{
			if (!Index.IsScriptImport())
			{
				continue;
			}

			UObject* Object = GlobalImportStore.FindScriptImportObjectFromIndex(Index);
			if (Object)
			{
				continue;
			}

			const FScriptObjectEntry* Entry = GlobalImportStore.ScriptObjectEntriesMap.FindRef(Index);
			check(Entry);
			const FPackageObjectIndex& CDOClassIndex = Entry->CDOClassIndex;
			if (CDOClassIndex.IsScriptImport())
			{
				UObject* CDOClassObject = GlobalImportStore.FindScriptImportObjectFromIndex(CDOClassIndex);
				if (CDOClassObject)
				{
					UClass* CDOClass = static_cast<UClass*>(CDOClassObject);
					Classes.AddUnique(CDOClass);
				}
			}
		}
		return Classes.Num() > 0;
	}

	inline void StoreGlobalObject(FPackageId PackageId, FPackageObjectIndex GlobalIndex, UObject* Object)
	{
		GlobalImportStore.StoreGlobalObject(PackageId, GlobalIndex, Object);
	}

	void ClearReferences()
	{
		ReleasePackageReferences();
	}

private:
	void AddAsyncFlags(UPackage* ImportedPackage)
	{
		// const FPackageStoreEntry& ImportEntry = GlobalPackageStore.GetStoreEntry(InPackageId);
		// UObject* ImportedPackage = StaticFindObjectFast(UPackage::StaticClass(), nullptr, MinimalNameToName(ImportEntry.Name), true);
		
		if (GUObjectArray.IsDisregardForGC(ImportedPackage))
		{
			// UE_LOG(LogStreaming, Display, TEXT("Skipping AddAsyncFlags for persistent package: %s"), *ImportedPackage->GetPathName());
			return;
		}
		ForEachObjectWithOuter(ImportedPackage, [](UObject* Object)
		{
			if (Object->HasAllFlags(RF_Public | RF_WasLoaded))
			{
				checkf(!Object->HasAnyInternalFlags(EInternalObjectFlags::Async), TEXT("%s"), *Object->GetFullName());
				Object->SetInternalFlags(EInternalObjectFlags::Async);
			}
		}, /* bIncludeNestedObjects*/ true);
	}

	void ClearAsyncFlags(UPackage* ImportedPackage)
	{
		// const FPackageStoreEntry& ImportEntry = GlobalPackageStore.GetStoreEntry(InPackageId);
		// UObject* ImportedPackage = StaticFindObjectFast(UPackage::StaticClass(), nullptr, MinimalNameToName(ImportEntry.Name), true);

		if (GUObjectArray.IsDisregardForGC(ImportedPackage))
		{
			// UE_LOG(LogStreaming, Display, TEXT("Skipping ClearAsyncFlags for persistent package: %s"), *ImportedPackage->GetPathName());
			return;
		}
		ForEachObjectWithOuter(ImportedPackage, [](UObject* Object)
		{
			if (Object->HasAllFlags(RF_Public | RF_WasLoaded))
			{
				checkf(Object->HasAnyInternalFlags(EInternalObjectFlags::Async), TEXT("%s"), *Object->GetFullName());
				Object->AtomicallyClearInternalFlags(EInternalObjectFlags::Async);
			}
		}, /* bIncludeNestedObjects*/ true);
	}

	void AddPackageReferences()
	{
		for (const FPackageId& ImportedPackageId : Desc.StoreEntry->ImportedPackages)
		{
			FLoadedPackageRef& PackageRef = GlobalPackageStore.LoadedPackageStore.GetPackageRef(ImportedPackageId);
			if (PackageRef.AddRef())
			{
				AddAsyncFlags(PackageRef.GetPackage());
			}
		}
		if (Desc.IsTrackingPublicExports())
		{
			FLoadedPackageRef& PackageRef = GlobalPackageStore.LoadedPackageStore.GetPackageRef(Desc.DiskPackageId);
			if (PackageRef.AddRef())
			{
				// should only happen if someone from outside call LoadPackage with an already loaded package
				// this could be detected already in CreatePackagesFromQueue,
				// but requires:
				// - queuing up package callbacks
				// - handling request ids properly
				// - calling AddAsyncFlags(PackageId); (now this is done from create/serialize in the async package)
			}
		}
	}

	void ReleasePackageReferences()
	{
		for (const FPackageId& ImportedPackageId : Desc.StoreEntry->ImportedPackages)
		{
			FLoadedPackageRef& PackageRef = GlobalPackageStore.LoadedPackageStore.GetPackageRef(ImportedPackageId);
			if (PackageRef.ReleaseRef())
			{
				ClearAsyncFlags(PackageRef.GetPackage());
			}
		}
		if (Desc.IsTrackingPublicExports())
		{
			// clear own reference, and possible all async flags if no remaining ref count
			FLoadedPackageRef& PackageRef =	GlobalPackageStore.LoadedPackageStore.GetPackageRef(Desc.DiskPackageId);
			if (PackageRef.ReleaseRef())
			{
				ClearAsyncFlags(PackageRef.GetPackage());
			}
		}
	}
};
	
class FExportArchive final : public FArchive
{
public:
	FExportArchive(const uint8* AllExportDataPtr, const uint8* CurrentExportPtr, uint64 AllExportDataSize)
	{
		ActiveFPLB->OriginalFastPathLoadBuffer = AllExportDataPtr;
		ActiveFPLB->StartFastPathLoadBuffer = CurrentExportPtr;
		ActiveFPLB->EndFastPathLoadBuffer = AllExportDataPtr + AllExportDataSize;
	}

	void ExportBufferBegin(uint64 InExportCookedFileSerialOffset, uint64 InExportSerialSize)
	{
		CookedSerialOffset = InExportCookedFileSerialOffset;
		BufferSerialOffset = (ActiveFPLB->StartFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer);
		CookedSerialSize = InExportSerialSize;
	}

	void ExportBufferEnd()
	{
		CookedSerialOffset = 0;
		BufferSerialOffset = 0;
		CookedSerialSize = 0;
	}

	void CheckBufferPosition(const TCHAR* Text, uint64 Offset = 0)
	{
#if DO_CHECK
		const uint64 BufferPosition = (ActiveFPLB->StartFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer) + Offset;
		const bool bIsInsideExportBuffer =
			(BufferSerialOffset <= BufferPosition) && (BufferPosition <= BufferSerialOffset + CookedSerialSize);

		UE_ASYNC_PACKAGE_CLOG(
			!bIsInsideExportBuffer,
			Error, *PackageDesc, TEXT("FExportArchive::InvalidPosition"),
			TEXT("%s: Position %llu is outside of the current export buffer (%lld,%lld)."),
			Text,
			BufferPosition,
			BufferSerialOffset, BufferSerialOffset + CookedSerialSize);
#endif
	}

	void Skip(int64 InBytes)
	{
		CheckBufferPosition(TEXT("InvalidSkip"), InBytes);
		ActiveFPLB->StartFastPathLoadBuffer += InBytes;
	}

	virtual int64 TotalSize() override
	{
		return CookedHeaderSize + (ActiveFPLB->EndFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer);
	}

	virtual int64 Tell() override
	{
		int64 CookedFilePosition = (ActiveFPLB->StartFastPathLoadBuffer - ActiveFPLB->OriginalFastPathLoadBuffer);
		CookedFilePosition -= BufferSerialOffset;
		CookedFilePosition += CookedSerialOffset;
		return CookedFilePosition;
	}

	virtual void Seek(int64 Position) override
	{
		uint64 BufferPosition = (uint64)Position;
		BufferPosition -= CookedSerialOffset;
		BufferPosition += BufferSerialOffset;
		ActiveFPLB->StartFastPathLoadBuffer = ActiveFPLB->OriginalFastPathLoadBuffer + BufferPosition;
		CheckBufferPosition(TEXT("InvalidSeek"));
	}

	virtual void Serialize(void* Data, int64 Length) override
	{
		if (!Length || ArIsError)
		{
			return;
		}
		CheckBufferPosition(TEXT("InvalidSerialize"), (uint64)Length);
		FMemory::Memcpy(Data, ActiveFPLB->StartFastPathLoadBuffer, Length);
		ActiveFPLB->StartFastPathLoadBuffer += Length;
	}

	void UsingCustomVersion(const FGuid& Key) override {};
	using FArchive::operator<<; // For visibility of the overloads we don't override

	//~ Begin FArchive::FArchiveUObject Interface
	virtual FArchive& operator<<(FSoftObjectPath& Value) override { return FArchiveUObject::SerializeSoftObjectPath(*this, Value); }
	virtual FArchive& operator<<(FWeakObjectPtr& Value) override { return FArchiveUObject::SerializeWeakObjectPtr(*this, Value); }
	//~ End FArchive::FArchiveUObject Interface

	//~ Begin FArchive::FLinkerLoad Interface
	UObject* GetArchetypeFromLoader(const UObject* Obj) { return TemplateForGetArchetypeFromLoader; }

	virtual bool AttachExternalReadDependency(FExternalReadCallback& ReadCallback) override
	{
		ExternalReadDependencies->Add(ReadCallback);
		return true;
	}

	FORCENOINLINE void HandleBadExportIndex(int32 ExportIndex, UObject*& Object)
	{
		UE_ASYNC_PACKAGE_LOG(Error, *PackageDesc, TEXT("HandleBadExportIndex"),
			TEXT("Index: %d/%d"), ExportIndex, ExportCount);

		Object = nullptr;
	}

	FORCENOINLINE void HandleBadImportIndex(int32 ImportIndex, UObject*& Object)
	{
		UE_ASYNC_PACKAGE_LOG(Error, *PackageDesc, TEXT("HandleBadImportIndex"),
			TEXT("ImportIndex: %d/%d"), ImportIndex, ImportStore->ImportMap.Num());

		Object = nullptr;
	}

	virtual FArchive& operator<<( UObject*& Object ) override
	{
		FPackageIndex Index;
		FArchive& Ar = *this;
		Ar << Index;

		if (Index.IsNull())
		{
			Object = nullptr;
		}
		else if (Index.IsExport())
		{
			const int32 ExportIndex = Index.ToExport();
			if (ExportIndex < ExportCount)
			{
				Object = (*Exports)[ExportIndex].Object;

#if ALT2_LOG_VERBOSE
				const FExportMapEntry& Export = ExportMap[ExportIndex];
				FName ObjectName = NameMap->GetName(Export.ObjectName);
				UE_ASYNC_PACKAGE_CLOG_VERBOSE(!Object, VeryVerbose, *PackageDesc,
					TEXT("FExportArchive: Object"), TEXT("Export %s at index %d is null."),
					*ObjectName.ToString(), 
					ExportIndex);
#endif
			}
			else
			{
				HandleBadExportIndex(ExportIndex, Object);
			}
		}
		else
		{
			if (ImportStore->IsValidLocalImportIndex(Index))
			{
				Object = ImportStore->FindOrGetImportObjectFromLocalIndex(Index);

				UE_ASYNC_PACKAGE_CLOG_VERBOSE(!Object, Log, *PackageDesc,
					TEXT("FExportArchive: Object"), TEXT("Import index %d is null"),
					Index.ToImport());
			}
			else
			{
				HandleBadImportIndex(Index.ToImport(), Object);
			}
		}
		return *this;
	}

	inline virtual FArchive& operator<<(FLazyObjectPtr& LazyObjectPtr) override
	{
		FArchive& Ar = *this;
		FUniqueObjectGuid ID;
		Ar << ID;
		LazyObjectPtr = ID;
		return Ar;
	}

	inline virtual FArchive& operator<<(FSoftObjectPtr& Value) override
	{
		FArchive& Ar = *this;
		FSoftObjectPath ID;
		ID.Serialize(Ar);
		Value = ID;
		return Ar;
	}

	FORCENOINLINE void HandleBadNameIndex(int32 NameIndex, FName& Name)
	{
		UE_ASYNC_PACKAGE_LOG(Error, *PackageDesc, TEXT("HandleBadNameIndex"),
			TEXT("Index: %d/%d"), NameIndex, NameMap->Num());

		Name = FName();
		SetCriticalError();
	}

	inline virtual FArchive& operator<<(FName& Name) override
	{
		FArchive& Ar = *this;
		uint32 NameIndex;
		Ar << NameIndex;
		uint32 Number = 0;
		Ar << Number;

		FMappedName MappedName = FMappedName::Create(NameIndex, Number, FMappedName::EType::Package);
		if (!NameMap->TryGetName(MappedName, Name))
		{
			HandleBadNameIndex(NameIndex, Name);
		}
		return *this;
	}
	//~ End FArchive::FLinkerLoad Interface

private:
	friend FAsyncPackage2;

	UObject* TemplateForGetArchetypeFromLoader = nullptr;

	FAsyncPackageDesc2* PackageDesc = nullptr;
	FPackageImportStore* ImportStore = nullptr;
	TArray<FExternalReadCallback>* ExternalReadDependencies;
	const FNameMap* NameMap = nullptr;
	const FExportObjects* Exports = nullptr;
	const FExportMapEntry* ExportMap = nullptr;
	int32 ExportCount = 0;
	uint32 CookedHeaderSize = 0;
	uint64 CookedSerialOffset = 0;
	uint64 CookedSerialSize = 0;
	uint64 BufferSerialOffset = 0;
};

enum class EAsyncPackageLoadingState2 : uint8
{
	NewPackage,
	WaitingForSummary,
	ProcessNewImportsAndExports,
	PostLoad_Etc,
	PackageComplete,
};

class FEventLoadGraphAllocator;
struct FAsyncLoadEventSpec;
struct FAsyncLoadingThreadState2;

/** [EDL] Event Load Node */
class FEventLoadNode2
{
public:
	FEventLoadNode2(const FAsyncLoadEventSpec* InSpec, FAsyncPackage2* InPackage, int32 InImportOrExportIndex);
	void DependsOn(FEventLoadNode2* Other);
	void AddBarrier();
	void AddBarrier(int32 Count);
	void ReleaseBarrier();
	void Execute(FAsyncLoadingThreadState2& ThreadState);

	int32 GetBarrierCount()
	{
		return BarrierCount.Load();
	}

	bool IsDone()
	{
		return !!bDone.Load();
	}

private:
	void ProcessDependencies(FAsyncLoadingThreadState2& ThreadState);
	void Fire();

	union
	{
		FEventLoadNode2* SingleDependent;
		FEventLoadNode2** MultipleDependents;
	};
	uint32 DependenciesCount = 0;
	uint32 DependenciesCapacity = 0;
	TAtomic<int32> BarrierCount { 0 };
	TAtomic<uint8> DependencyWriterCount { 0 };
	TAtomic<uint8> bDone { 0 };
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	TAtomic<uint8> bFired { 0 };
#endif

	const FAsyncLoadEventSpec* Spec;
	FAsyncPackage2* Package;
	int32 ImportOrExportIndex;
};

class FAsyncLoadEventGraphAllocator
{
public:
	FEventLoadNode2* AllocNodes(uint32 Count)
	{
		//TRACE_CPUPROFILER_EVENT_SCOPE(AllocNodes);
		SIZE_T Size = Count * sizeof(FEventLoadNode2);
		TotalNodeCount += Count;
		TotalAllocated += Size;
		return reinterpret_cast<FEventLoadNode2*>(FMemory::Malloc(Size));
	}

	void FreeNodes(FEventLoadNode2* Nodes, uint32 Count)
	{
		//TRACE_CPUPROFILER_EVENT_SCOPE(FreeNodes);
		FMemory::Free(Nodes);
		SIZE_T Size = Count * sizeof(FEventLoadNode2);
		TotalAllocated -= Size;
		TotalNodeCount -= Count;
	}

	FEventLoadNode2** AllocArcs(uint32 Count)
	{
		//TRACE_CPUPROFILER_EVENT_SCOPE(AllocArcs);
		SIZE_T Size = Count * sizeof(FEventLoadNode2*);
		TotalArcCount += Count;
		TotalAllocated += Size;
		return reinterpret_cast<FEventLoadNode2**>(FMemory::Malloc(Size));
	}

	void FreeArcs(FEventLoadNode2** Arcs, uint32 Count)
	{
		//TRACE_CPUPROFILER_EVENT_SCOPE(FreeArcs);
		FMemory::Free(Arcs);
		SIZE_T Size = Count * sizeof(FEventLoadNode2*);
		TotalAllocated -= Size;
		TotalArcCount -= Count;
	}

	TAtomic<int64> TotalNodeCount { 0 };
	TAtomic<int64> TotalArcCount { 0 };
	TAtomic<int64> TotalAllocated { 0 };
};

class FAsyncLoadEventQueue2
{
public:
	FAsyncLoadEventQueue2();
	~FAsyncLoadEventQueue2();

	void SetZenaphore(FZenaphore* InZenaphore)
	{
		Zenaphore = InZenaphore;
	}

	bool PopAndExecute(FAsyncLoadingThreadState2& ThreadState);
	void Push(FEventLoadNode2* Node);

private:
	FZenaphore* Zenaphore = nullptr;
	TAtomic<uint64> Head { 0 };
	TAtomic<uint64> Tail { 0 };
	TAtomic<FEventLoadNode2*> Entries[524288];
};

struct FAsyncLoadEventSpec
{
	typedef EAsyncPackageState::Type(*FAsyncLoadEventFunc)(FAsyncPackage2*, int32);
	FAsyncLoadEventFunc Func = nullptr;
	FAsyncLoadEventQueue2* EventQueue = nullptr;
	bool bExecuteImmediately = false;
};

struct FAsyncLoadingThreadState2
	: public FTlsAutoCleanup
{
	static FAsyncLoadingThreadState2* Create(FAsyncLoadEventGraphAllocator& GraphAllocator, FIoDispatcher& IoDispatcher)
	{
		check(TlsSlot != 0);
		check(!FPlatformTLS::GetTlsValue(TlsSlot));
		FAsyncLoadingThreadState2* State = new FAsyncLoadingThreadState2(GraphAllocator, IoDispatcher);
		State->Register();
		FPlatformTLS::SetTlsValue(TlsSlot, State);
		return State;
	}

	static FAsyncLoadingThreadState2* Get()
	{
		check(TlsSlot != 0);
		return static_cast<FAsyncLoadingThreadState2*>(FPlatformTLS::GetTlsValue(TlsSlot));
	}

	FAsyncLoadingThreadState2(FAsyncLoadEventGraphAllocator& InGraphAllocator, FIoDispatcher& InIoDispatcher)
		: GraphAllocator(InGraphAllocator)
	{

	}

	bool HasDeferredFrees() const
	{
		return DeferredFreeArcs.Num() > 0;
	}

	void ProcessDeferredFrees()
	{
		if (DeferredFreeArcs.Num() > 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ProcessDeferredFrees);
			for (TTuple<FEventLoadNode2**, uint32>& DeferredFreeArc : DeferredFreeArcs)
			{
				GraphAllocator.FreeArcs(DeferredFreeArc.Get<0>(), DeferredFreeArc.Get<1>());
			}
			DeferredFreeArcs.Reset();
		}
	}

	void SetTimeLimit(bool bInUseTimeLimit, double InTimeLimit)
	{
		bUseTimeLimit = bInUseTimeLimit;
		TimeLimit = InTimeLimit;
		StartTime = FPlatformTime::Seconds();
	}

	bool IsTimeLimitExceeded(const TCHAR* InLastTypeOfWorkPerformed = nullptr, UObject* InLastObjectWorkWasPerformedOn = nullptr)
	{
		bool bTimeLimitExceeded = false;

		if (bUseTimeLimit)
		{
			double CurrentTime = FPlatformTime::Seconds();
			bTimeLimitExceeded = CurrentTime - StartTime > TimeLimit;

			if (bTimeLimitExceeded && GWarnIfTimeLimitExceeded)
			{
				IsTimeLimitExceededPrint(StartTime, CurrentTime, LastTestTime, TimeLimit, InLastTypeOfWorkPerformed, InLastObjectWorkWasPerformedOn);
			}

			LastTestTime = CurrentTime;
		}

		if (!bTimeLimitExceeded)
		{
			bTimeLimitExceeded = IsGarbageCollectionWaiting();
			UE_CLOG(bTimeLimitExceeded, LogStreaming, Verbose, TEXT("Timing out async loading due to Garbage Collection request"));
		}

		return bTimeLimitExceeded;
	}

	bool UseTimeLimit()
	{
		return bUseTimeLimit;
	}

	FAsyncLoadEventGraphAllocator& GraphAllocator;
	TArray<TTuple<FEventLoadNode2**, uint32>> DeferredFreeArcs;
	TArray<FEventLoadNode2*> NodesToFire;
	FEventLoadNode2* CurrentEventNode = nullptr;
	bool bShouldFireNodes = true;
	bool bUseTimeLimit = false;
	double TimeLimit = 0.0;
	double StartTime = 0.0;
	double LastTestTime = -1.0;
	static uint32 TlsSlot;
};

uint32 FAsyncLoadingThreadState2::TlsSlot;

/**
 * Event node.
 */
enum EEventLoadNode2 : uint8
{
	Package_ProcessSummary,
	Package_ExportsSerialized,
	Package_NumPhases,

	ExportBundle_Process = 0,
	ExportBundle_PostLoad,
	ExportBundle_DeferredPostLoad,
	ExportBundle_NumPhases,
};

/**
* Structure containing intermediate data required for async loading of all exports of a package.
*/

struct FAsyncPackage2
{
	friend struct FScopedAsyncPackageEvent2;
	friend class FAsyncLoadingThread2;

	FAsyncPackage2(const FAsyncPackageDesc2& InDesc,
		FAsyncLoadingThread2& InAsyncLoadingThread,
		FAsyncLoadEventGraphAllocator& InGraphAllocator,
		const FAsyncLoadEventSpec* EventSpecs);
	virtual ~FAsyncPackage2();


	bool bCompleted = false;

	void AddRef()
	{
		++RefCount;
	}

	void ReleaseRef();

	void ClearImportedPackages();

	/** Marks a specific request as complete */
	void MarkRequestIDsAsComplete();

	/**
	 * @return Estimated load completion percentage.
	 */
	FORCEINLINE float GetLoadPercentage() const
	{
		return LoadPercentage;
	}

	/**
	 * @return Time load begun. This is NOT the time the load was requested in the case of other pending requests.
	 */
	double GetLoadStartTime() const;

	void AddCompletionCallback(TUniquePtr<FLoadPackageAsyncDelegate>&& Callback);

	FORCEINLINE UPackage* GetLinkerRoot() const
	{
		return LinkerRoot;
	}

	/** Returns true if the package has finished loading. */
	FORCEINLINE bool HasFinishedLoading() const
	{
		return bLoadHasFinished;
	}

	/** Returns true if loading has failed */
	FORCEINLINE bool HasLoadFailed() const
	{
		return bLoadHasFailed;
	}

	/** Adds new request ID to the existing package */
	void AddRequestID(int32 Id);

	/**
	* Cancel loading this package.
	*/
	void Cancel();

	void AddConstructedObject(UObject* Object, bool bSubObjectThatAlreadyExists)
	{
		if (bSubObjectThatAlreadyExists)
		{
			ConstructedObjects.AddUnique(Object);
		}
		else
		{
			checkf(!ConstructedObjects.Contains(Object), TEXT("%s"), *Object->GetFullName());
			ConstructedObjects.Add(Object);
		}
	}

	void PinObjectForGC(UObject* Object, bool bIsNewObject)
	{
		if (bIsNewObject && !IsInGameThread())
		{
			checkf(Object->HasAnyInternalFlags(EInternalObjectFlags::Async), TEXT("%s"), *Object->GetFullName());
		}
		else
		{
			Object->SetInternalFlags(EInternalObjectFlags::Async);
		}
	}

	void ClearConstructedObjects();

	/** Returns the UPackage wrapped by this, if it is valid */
	UPackage* GetLoadedPackage();

	/** Checks if all dependencies (imported packages) of this package have been fully loaded */
	bool AreAllDependenciesFullyLoaded(TSet<FPackageId>& VisitedPackages);

	/** Returs true if this package loaded objects that can create GC clusters */
	bool HasClusterObjects() const
	{
		return bHasClusterObjects;
	}

	/** Creates GC clusters from loaded objects */
	EAsyncPackageState::Type CreateClusters();

	void ImportPackagesRecursive();
	void StartLoading();

private:

	/** Checks if all dependencies (imported packages) of this package have been fully loaded */
	bool AreAllDependenciesFullyLoadedInternal(FAsyncPackage2* Package, TSet<FPackageId>& VisitedPackages, FPackageId& OutPackageId);

	TAtomic<int32> RefCount{ 0 };

	/** Basic information associated with this package */
	FAsyncPackageDesc2 Desc;
	/** Package which is going to have its exports and imports loaded									*/
	UPackage*				LinkerRoot;
	/** Call backs called when we finished loading this package											*/
	using FCompletionCallback = TUniquePtr<FLoadPackageAsyncDelegate>;
	TArray<FCompletionCallback, TInlineAllocator<2>> CompletionCallbacks;
	/** Current bundle entry index in the current export bundle */
	int32						ExportBundleEntryIndex = 0;
	/** Current index into ExternalReadDependencies array used to spread wating for external reads over several frames			*/
	int32						ExternalReadIndex = 0;
	/** Current index into DeferredClusterObjects array used to spread routing CreateClusters over several frames			*/
	int32						DeferredClusterIndex;
	/** True if any export can be a cluster root */
	bool						bHasClusterObjects;
	/** True if our load has failed */
	bool						bLoadHasFailed;
	/** True if our load has finished */
	bool						bLoadHasFinished;
	/** True if this package was created by this async package */
	bool						bCreatedLinkerRoot;
	/** Time load begun. This is NOT the time the load was requested in the case of pending requests.	*/
	double						LoadStartTime;
	/** Estimated load percentage.																		*/
	float						LoadPercentage;

	/** List of all request handles */
	TArray<int32, TInlineAllocator<2>> RequestIDs;
	/** Number of times we recursed to load this package. */
	int32 ReentryCount;
	TArray<FAsyncPackage2*> ImportedAsyncPackages;
	/** List of ConstructedObjects = Exports + UPackage + ObjectsCreatedFromExports */
	TArray<UObject*> ConstructedObjects;
	/** Cached async loading thread object this package was created by */
	FAsyncLoadingThread2& AsyncLoadingThread;
	FAsyncLoadEventGraphAllocator& GraphAllocator;

	FEventLoadNode2* PackageNodes = nullptr;
	FEventLoadNode2* ExportBundleNodes = nullptr;
	uint32 ExportBundleNodeCount = 0;

	FIoBuffer IoBuffer;
	const uint8* CurrentExportDataPtr = nullptr;
	const uint8* AllExportDataPtr = nullptr;
	uint64 ExportBundlesSize = 0;
	uint32 CookedHeaderSize = 0;
	uint32 LoadOrder = 0;

	TArray<FExternalReadCallback> ExternalReadDependencies;
	int32 ExportCount = 0;
	const FExportMapEntry* ExportMap = nullptr;
	FExportObjects Exports;
	FPackageImportStore ImportStore;
	FNameMap NameMap;

	int32 ExportBundleCount = 0;
	uint64 ExportBundlesMetaSize = 0;
	uint8* ExportBundlesMetaMemory = nullptr;
	const FExportBundleHeader* ExportBundleHeaders = nullptr;
	const FExportBundleEntry* ExportBundleEntries = nullptr;
public:

	FAsyncLoadingThread2& GetAsyncLoadingThread()
	{
		return AsyncLoadingThread;
	}

	FAsyncLoadEventGraphAllocator& GetGraphAllocator()
	{
		return GraphAllocator;
	}

	/** [EDL] Begin Event driven loader specific stuff */

	EAsyncPackageLoadingState2 AsyncPackageLoadingState;

	bool bHasImportedPackagesRecursive = false;

	bool bAllExportsSerialized;
	bool bAllExportsDeferredPostLoaded;

	static EAsyncPackageState::Type Event_ProcessExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex);
	static EAsyncPackageState::Type Event_ProcessPackageSummary(FAsyncPackage2* Package, int32);
	static EAsyncPackageState::Type Event_ExportsDone(FAsyncPackage2* Package, int32);
	static EAsyncPackageState::Type Event_PostLoadExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex);
	static EAsyncPackageState::Type Event_DeferredPostLoadExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex);
	
	void EventDrivenCreateExport(int32 LocalExportIndex);
	bool EventDrivenSerializeExport(int32 LocalExportIndex, FExportArchive& Ar);

	UObject* EventDrivenIndexToObject(FPackageObjectIndex Index, bool bCheckSerialized);
	template<class T>
	T* CastEventDrivenIndexToObject(FPackageObjectIndex Index, bool bCheckSerialized)
	{
		UObject* Result = EventDrivenIndexToObject(Index, bCheckSerialized);
		if (!Result)
		{
			return nullptr;
		}
		return CastChecked<T>(Result);
	}

	FEventLoadNode2* GetPackageNode(EEventLoadNode2 Phase);
	FEventLoadNode2* GetExportBundleNode(EEventLoadNode2 Phase, uint32 ExportBundleIndex);
	FEventLoadNode2* GetNode(int32 NodeIndex);

	/** [EDL] End Event driven loader specific stuff */

	void CallCompletionCallbacks(EAsyncLoadingResult::Type LoadingResult);

private:
	void CreateNodes(const FAsyncLoadEventSpec* EventSpecs);
	void SetupSerializedArcs(const uint8* GraphData, uint64 GraphDataSize);
	void SetupScriptDependencies();

	/**
	 * Begin async loading process. Simulates parts of BeginLoad.
	 *
	 * Objects created during BeginAsyncLoad and EndAsyncLoad will have EInternalObjectFlags::AsyncLoading set
	 */
	void BeginAsyncLoad();
	/**
	 * End async loading process. Simulates parts of EndLoad(). FinishObjects
	 * simulates some further parts once we're fully done loading the package.
	 */
	void EndAsyncLoad();
	/**
	 * Create UPackage
	 *
	 * @return true
	 */
	void CreateUPackage(const FPackageSummary* PackageSummary);

	/**
	 * Finish up objects and state, which means clearing the EInternalObjectFlags::AsyncLoading flag on newly created ones
	 *
	 * @return true
	 */
	EAsyncPackageState::Type FinishObjects();

	/**
	 * Finalizes external dependencies till time limit is exceeded
	 *
	 * @return Complete if all dependencies are finished, TimeOut otherwise
	 */
	enum EExternalReadAction { ExternalReadAction_Poll, ExternalReadAction_Wait };
	EAsyncPackageState::Type ProcessExternalReads(EExternalReadAction Action);

	/**
	* Updates load percentage stat
	*/
	void UpdateLoadPercentage();

public:

	/** Serialization context for this package */
	FUObjectSerializeContext* GetSerializeContext();
};

struct FScopedAsyncPackageEvent2
{
	/** Current scope package */
	FAsyncPackage2* Package;
	/** Outer scope package */
	FAsyncPackage2* PreviousPackage;

	FScopedAsyncPackageEvent2(FAsyncPackage2* InPackage);
	~FScopedAsyncPackageEvent2();
};

class FAsyncLoadingThreadWorker : private FRunnable
{
public:
	FAsyncLoadingThreadWorker(FAsyncLoadEventGraphAllocator& InGraphAllocator, FAsyncLoadEventQueue2& InEventQueue, FIoDispatcher& InIoDispatcher, FZenaphore& InZenaphore, TAtomic<int32>& InActiveWorkersCount)
		: Zenaphore(InZenaphore)
		, EventQueue(InEventQueue)
		, GraphAllocator(InGraphAllocator)
		, IoDispatcher(InIoDispatcher)
		, ActiveWorkersCount(InActiveWorkersCount)
	{
	}

	void StartThread();
	
	void StopThread()
	{
		bStopRequested = true;
		bSuspendRequested = true;
		Zenaphore.NotifyAll();
	}
	
	void SuspendThread()
	{
		bSuspendRequested = true;
		Zenaphore.NotifyAll();
	}
	
	void ResumeThread()
	{
		bSuspendRequested = false;
	}
	
	int32 GetThreadId() const
	{
		return ThreadId;
	}

private:
	virtual bool Init() override { return true; }
	virtual uint32 Run() override;
	virtual void Stop() override {};

	FZenaphore& Zenaphore;
	FAsyncLoadEventQueue2& EventQueue;
	FAsyncLoadEventGraphAllocator& GraphAllocator;
	FIoDispatcher& IoDispatcher;
	TAtomic<int32>& ActiveWorkersCount;
	FRunnableThread* Thread = nullptr;
	TAtomic<bool> bStopRequested { false };
	TAtomic<bool> bSuspendRequested { false };
	int32 ThreadId = 0;
};

class FAsyncLoadingThread2 final
	: public FRunnable
	, public IAsyncPackageLoader
{
	friend struct FAsyncPackage2;
public:
	FAsyncLoadingThread2(FIoDispatcher& IoDispatcher);
	virtual ~FAsyncLoadingThread2();

private:
	/** Thread to run the worker FRunnable on */
	FRunnableThread* Thread;
	TAtomic<bool> bStopRequested { false };
	TAtomic<bool> bSuspendRequested { false };
	TArray<FAsyncLoadingThreadWorker> Workers;
	TAtomic<int32> ActiveWorkersCount { 0 };
	bool bWorkersSuspended = false;

	/** [ASYNC/GAME THREAD] true if the async thread is actually started. We don't start it until after we boot because the boot process on the game thread can create objects that are also being created by the loader */
	bool bThreadStarted = false;

	bool bLazyInitializedFromLoadPackage = false;

#if ALT2_VERIFY_RECURSIVE_LOADS
	int32 LoadRecursionLevel = 0;
#endif

#if !UE_BUILD_SHIPPING
	FPlatformFileOpenLog* FileOpenLogWrapper = nullptr;
#endif

	/** [ASYNC/GAME THREAD] Event used to signal loading should be cancelled */
	FEvent* CancelLoadingEvent;
	/** [ASYNC/GAME THREAD] Event used to signal that the async loading thread should be suspended */
	FEvent* ThreadSuspendedEvent;
	/** [ASYNC/GAME THREAD] Event used to signal that the async loading thread has resumed */
	FEvent* ThreadResumedEvent;
	/** [ASYNC/GAME THREAD] List of queued packages to stream */
	TArray<FAsyncPackageDesc2*> QueuedPackages;
	/** [ASYNC/GAME THREAD] Package queue critical section */
	FCriticalSection QueueCritical;
	TArray<FAsyncPackage2*> LoadedPackagesToProcess;
	/** [GAME THREAD] Game thread CompletedPackages list */
	TArray<FAsyncPackage2*> CompletedPackages;
	/** [ASYNC/GAME THREAD] Packages to be deleted from async thread */
	TQueue<FAsyncPackage2*, EQueueMode::Spsc> DeferredDeletePackages;
	
	struct FQueuedFailedPackageCallback
	{
		FName PackageName;
		TUniquePtr<FLoadPackageAsyncDelegate> Callback;
	};
	TArray<FQueuedFailedPackageCallback> QueuedFailedPackageCallbacks;

	FCriticalSection AsyncPackagesCritical;
	/** Packages in active loading with GetAsyncPackageId() as key */
	TMap<FPackageId, FAsyncPackage2*> AsyncPackageLookup;

	TQueue<FAsyncPackage2*, EQueueMode::Mpsc> ExternalReadQueue;
	FThreadSafeCounter WaitingForIoBundleCounter;
	
	/** List of all pending package requests */
	TSet<int32> PendingRequests;
	/** Synchronization object for PendingRequests list */
	FCriticalSection PendingRequestsCritical;

	/** [ASYNC/GAME THREAD] Number of package load requests in the async loading queue */
	TAtomic<uint32> QueuedPackagesCounter { 0 };
	/** [ASYNC/GAME THREAD] Number of packages being loaded on the async thread and post loaded on the game thread */
	FThreadSafeCounter ExistingAsyncPackagesCounter;

	FThreadSafeCounter AsyncThreadReady;

	/** When cancelling async loading: list of package requests to cancel */
	TArray<FAsyncPackageDesc2*> QueuedPackagesToCancel;
	/** When cancelling async loading: list of packages to cancel */
	TSet<FAsyncPackage2*> PackagesToCancel;

	/** Async loading thread ID */
	uint32 AsyncLoadingThreadID;

	FThreadSafeCounter PackageRequestID;

	/** I/O Dispatcher */
	FIoDispatcher& IoDispatcher;

	FNameMap GlobalNameMap;
	FPackageStore GlobalPackageStore;

	/** Initial load pending CDOs */
	TMap<UClass*, TArray<FEventLoadNode2*>> PendingCDOs;

	struct FBundleIoRequest
	{
		bool operator<(const FBundleIoRequest& Other) const
		{
			return Package->LoadOrder < Other.Package->LoadOrder;
		}

		FAsyncPackage2* Package;
	};
	TArray<FBundleIoRequest> WaitingIoRequests;
	uint64 PendingBundleIoRequestsTotalSize = 0;

public:

	//~ Begin FRunnable Interface.
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	//~ End FRunnable Interface

	/** Start the async loading thread */
	virtual void StartThread() override;

	/** [EDL] Event queue */
	FZenaphore AltZenaphore;
	TArray<FZenaphore> WorkerZenaphores;
	FAsyncLoadEventGraphAllocator GraphAllocator;
	FAsyncLoadEventQueue2 EventQueue;
	FAsyncLoadEventQueue2 MainThreadEventQueue;
	TArray<FAsyncLoadEventQueue2*> AltEventQueues;
	TArray<FAsyncLoadEventSpec> EventSpecs;

	/** True if multithreaded async loading is currently being used. */
	inline virtual bool IsMultithreaded() override
	{
		return bThreadStarted;
	}

	/** Sets the current state of async loading */
	void EnterAsyncLoadingTick()
	{
		AsyncLoadingTickCounter++;
	}

	void LeaveAsyncLoadingTick()
	{
		AsyncLoadingTickCounter--;
		check(AsyncLoadingTickCounter >= 0);
	}

	/** Gets the current state of async loading */
	bool GetIsInAsyncLoadingTick() const
	{
		return !!AsyncLoadingTickCounter;
	}

	/** Returns true if packages are currently being loaded on the async thread */
	inline virtual bool IsAsyncLoadingPackages() override
	{
		FPlatformMisc::MemoryBarrier();
		return QueuedPackagesCounter != 0 || ExistingAsyncPackagesCounter.GetValue() != 0 || !DeferredDeletePackages.IsEmpty();
	}

	/** Returns true this codes runs on the async loading thread */
	virtual bool IsInAsyncLoadThread() override
	{
		if (IsMultithreaded())
		{
			// We still need to report we're in async loading thread even if 
			// we're on game thread but inside of async loading code (PostLoad mostly)
			// to make it behave exactly like the non-threaded version
			uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
			if (CurrentThreadId == AsyncLoadingThreadID ||
				(IsInGameThread() && GetIsInAsyncLoadingTick()))
			{
				return true;
			}
			else
			{
				for (const FAsyncLoadingThreadWorker& Worker : Workers)
				{
					if (CurrentThreadId == Worker.GetThreadId())
					{
						return true;
					}
				}
			}
			return false;
		}
		else
		{
			return IsInGameThread() && GetIsInAsyncLoadingTick();
		}
	}

	/** Returns true if async loading is suspended */
	inline virtual bool IsAsyncLoadingSuspended() override
	{
		return bSuspendRequested;
	}

	virtual void NotifyConstructedDuringAsyncLoading(UObject* Object, bool bSubObjectThatAlreadyExists) override;

	virtual void NotifyUnreachableObjects(const TArrayView<FUObjectItem*>& UnreachableObjects) override;

	virtual void FireCompletedCompiledInImport(void* AsyncPackage, FPackageIndex Import) override {}

	/**
	* [ASYNC THREAD] Finds an existing async package in the AsyncPackages by its name.
	*
	* @param PackageName async package name.
	* @return Pointer to the package or nullptr if not found
	*/
	FORCEINLINE FAsyncPackage2* FindAsyncPackage(const FName& PackageName)
	{
		// TRACE_CPUPROFILER_EVENT_SCOPE(FindAsyncPackage);
		FPackageId PackageId = FPackageId::FromName(PackageName);
		if (PackageId.IsValid())
		{
			FScopeLock LockAsyncPackages(&AsyncPackagesCritical);
			//checkSlow(IsInAsyncLoadThread());
			return AsyncPackageLookup.FindRef(PackageId);
		}
		return nullptr;
	}

	FORCEINLINE FAsyncPackage2* GetAsyncPackage(const FPackageId& PackageId)
	{
		// TRACE_CPUPROFILER_EVENT_SCOPE(GetAsyncPackage);
		FScopeLock LockAsyncPackages(&AsyncPackagesCritical);
		return AsyncPackageLookup.FindRef(PackageId);
	}

	/**
	* [ASYNC THREAD] Inserts package to queue according to priority.
	*
	* @param PackageName - async package name.
	* @param InsertMode - Insert mode, describing how we insert this package into the request list
	*/
	void InsertPackage(FAsyncPackage2* Package, bool bReinsert = false);

	FAsyncPackage2* FindOrInsertPackage(FAsyncPackageDesc2* InDesc, bool& bInserted);

	/**
	* [ASYNC/GAME THREAD] Queues a package for streaming.
	*
	* @param Package package descriptor.
	*/
	void QueuePackage(FAsyncPackageDesc2& Package);

	/**
	* [ASYNC* THREAD] Loads all packages
	*
	* @param OutPackagesProcessed Number of packages processed in this call.
	* @return The current state of async loading
	*/
	EAsyncPackageState::Type ProcessAsyncLoadingFromGameThread(int32& OutPackagesProcessed);

	/**
	* [GAME THREAD] Ticks game thread side of async loading.
	*
	* @param bUseTimeLimit True if time limit should be used [time-slicing].
	* @param bUseFullTimeLimit True if full time limit should be used [time-slicing].
	* @param TimeLimit Maximum amount of time that can be spent in this call [time-slicing].
	* @param FlushTree Package dependency tree to be flushed
	* @return The current state of async loading
	*/
	EAsyncPackageState::Type TickAsyncLoadingFromGameThread(bool bUseTimeLimit, bool bUseFullTimeLimit, float TimeLimit, int32 FlushRequestID = INDEX_NONE);

	/**
	* [ASYNC THREAD] Main thread loop
	*
	* @param bUseTimeLimit True if time limit should be used [time-slicing].
	* @param bUseFullTimeLimit True if full time limit should be used [time-slicing].
	* @param TimeLimit Maximum amount of time that can be spent in this call [time-slicing].
	* @param FlushTree Package dependency tree to be flushed
	*/
	EAsyncPackageState::Type TickAsyncThreadFromGameThread(bool& bDidSomething);

	/** Initializes async loading thread */
	virtual void InitializeLoading() override;

	virtual void ShutdownLoading() override;

	virtual int32 LoadPackage(
		const FString& InPackageName,
		const FGuid* InGuid,
		const TCHAR* InPackageToLoadFrom,
		FLoadPackageAsyncDelegate InCompletionDelegate,
		EPackageFlags InPackageFlags,
		int32 InPIEInstanceID,
		int32 InPackagePriority,
		const FLinkerInstancingContext* InstancingContext = nullptr) override;

	EAsyncPackageState::Type ProcessLoadingFromGameThread(bool bUseTimeLimit, bool bUseFullTimeLimit, float TimeLimit);

	inline virtual EAsyncPackageState::Type ProcessLoading(bool bUseTimeLimit, bool bUseFullTimeLimit, float TimeLimit) override
	{
		return ProcessLoadingFromGameThread(bUseTimeLimit, bUseFullTimeLimit, TimeLimit);
	}

	EAsyncPackageState::Type ProcessLoadingUntilCompleteFromGameThread(TFunctionRef<bool()> CompletionPredicate, float TimeLimit);

	inline virtual EAsyncPackageState::Type ProcessLoadingUntilComplete(TFunctionRef<bool()> CompletionPredicate, float TimeLimit) override
	{
		return ProcessLoadingUntilCompleteFromGameThread(CompletionPredicate, TimeLimit);
	}

	virtual void CancelLoading() override;

	virtual void SuspendLoading() override;

	virtual void ResumeLoading() override;

	virtual void FlushLoading(int32 PackageId) override;

	virtual int32 GetNumQueuedPackages() override
	{
		return QueuedPackagesCounter;
	}

	virtual int32 GetNumAsyncPackages() override
	{
		return ExistingAsyncPackagesCounter.GetValue();
	}

	/**
	 * [GAME THREAD] Gets the load percentage of the specified package
	 * @param PackageName Name of the package to return async load percentage for
	 * @return Percentage (0-100) of the async package load or -1 of package has not been found
	 */
	virtual float GetAsyncLoadPercentage(const FName& PackageName) override;

	/**
	 * [ASYNC/GAME THREAD] Checks if a request ID already is added to the loading queue
	 */
	bool ContainsRequestID(int32 RequestID)
	{
		FScopeLock Lock(&PendingRequestsCritical);
		return PendingRequests.Contains(RequestID);
	}

	/**
	 * [ASYNC/GAME THREAD] Adds a request ID to the list of pending requests
	 */
	void AddPendingRequest(int32 RequestID)
	{
		FScopeLock Lock(&PendingRequestsCritical);
		PendingRequests.Add(RequestID);
	}

	/**
	 * [ASYNC/GAME THREAD] Removes a request ID from the list of pending requests
	 */
	void RemovePendingRequests(TArray<int32, TInlineAllocator<2>>& RequestIDs)
	{
		FScopeLock Lock(&PendingRequestsCritical);
		for (int32 ID : RequestIDs)
		{
			PendingRequests.Remove(ID);
			TRACE_LOADTIME_END_REQUEST(ID);
		}
	}

	void AddPendingCDOs(FAsyncPackage2* Package, TArray<UClass*, TInlineAllocator<8>>& Classes)
	{
		FEventLoadNode2* FirstBundleNode = Package->GetExportBundleNode(ExportBundle_Process, 0);
		FirstBundleNode->AddBarrier(Classes.Num());
		for (UClass* Class : Classes)
		{
			PendingCDOs.FindOrAdd(Class).Add(FirstBundleNode);
		}
	}

private:

	void SuspendWorkers();
	void ResumeWorkers();

	void LazyInitializeFromLoadPackage();
	void FinalizeInitialLoad();

	bool ProcessPendingCDOs()
	{
		if (PendingCDOs.Num() > 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ProcessPendingCDOs);

			auto It = PendingCDOs.CreateIterator();
			UClass* Class = It.Key();
			TArray<FEventLoadNode2*> Nodes = MoveTemp(It.Value());
			It.RemoveCurrent();

			UE_LOG(LogStreaming, Verbose, TEXT("ProcessPendingCDOs: Creating CDO for %s. %d entries remaining."), *Class->GetFullName(), PendingCDOs.Num());
			UObject* CDO = Class->GetDefaultObject();

			ensureMsgf(CDO, TEXT("Failed to create CDO for %s"), *Class->GetFullName());
			UE_LOG(LogStreaming, Verbose, TEXT("ProcessPendingCDOs: Created CDO for %s."), *Class->GetFullName());

			for (FEventLoadNode2* Node : Nodes)
			{
				Node->ReleaseBarrier();
			}
			return true;
		}
		return false;
	}

	/**
	* [GAME THREAD] Performs game-thread specific operations on loaded packages (not-thread-safe PostLoad, callbacks)
	*
	* @param bUseTimeLimit True if time limit should be used [time-slicing].
	* @param bUseFullTimeLimit True if full time limit should be used [time-slicing].
	* @param TimeLimit Maximum amount of time that can be spent in this call [time-slicing].
	* @param FlushTree Package dependency tree to be flushed
	* @return The current state of async loading
	*/
	EAsyncPackageState::Type ProcessLoadedPackagesFromGameThread(bool& bDidSomething, int32 FlushRequestID = INDEX_NONE);

	bool CreateAsyncPackagesFromQueue();
	void AddBundleIoRequest(FAsyncPackage2* Package);
	void BundleIoRequestCompleted(FAsyncPackage2* Package);
	void StartBundleIoRequests();

	FAsyncPackage2* CreateAsyncPackage(const FAsyncPackageDesc2& Desc)
	{
		UE_ASYNC_PACKAGE_DEBUG(Desc);
		checkf(Desc.StoreEntry, TEXT("No package store entry for package %s"), *Desc.DiskPackageName.ToString());
		return new FAsyncPackage2(Desc, *this, GraphAllocator, EventSpecs.GetData());
	}

	/** Number of times we re-entered the async loading tick, mostly used by singlethreaded ticking. Debug purposes only. */
	int32 AsyncLoadingTickCounter;
};

/**
 * Updates FUObjectThreadContext with the current package when processing it.
 * FUObjectThreadContext::AsyncPackage is used by NotifyConstructedDuringAsyncLoading.
 */
struct FAsyncPackageScope2
{
	/** Outer scope package */
	void* PreviousPackage;
	/** Cached ThreadContext so we don't have to access it again */
	FUObjectThreadContext& ThreadContext;

	FAsyncPackageScope2(void* InPackage)
		: ThreadContext(FUObjectThreadContext::Get())
	{
		PreviousPackage = ThreadContext.AsyncPackage;
		ThreadContext.AsyncPackage = InPackage;
	}
	~FAsyncPackageScope2()
	{
		ThreadContext.AsyncPackage = PreviousPackage;
	}
};

/** Just like TGuardValue for FAsyncLoadingThread::AsyncLoadingTickCounter but only works for the game thread */
struct FAsyncLoadingTickScope2
{
	FAsyncLoadingThread2& AsyncLoadingThread;
	bool bNeedsToLeaveAsyncTick;

	FAsyncLoadingTickScope2(FAsyncLoadingThread2& InAsyncLoadingThread)
		: AsyncLoadingThread(InAsyncLoadingThread)
		, bNeedsToLeaveAsyncTick(false)
	{
		if (IsInGameThread())
		{
			AsyncLoadingThread.EnterAsyncLoadingTick();
			bNeedsToLeaveAsyncTick = true;
		}
	}
	~FAsyncLoadingTickScope2()
	{
		if (bNeedsToLeaveAsyncTick)
		{
			AsyncLoadingThread.LeaveAsyncLoadingTick();
		}
	}
};

void FAsyncLoadingThread2::InitializeLoading()
{
#if !UE_BUILD_SHIPPING
	{
		FString DebugPackageNamesString;
		FParse::Value(FCommandLine::Get(), TEXT("-s.DebugPackageNames="), DebugPackageNamesString);
		ParsePackageNames(DebugPackageNamesString, GAsyncLoading2_DebugPackageIds);
		FString VerbosePackageNamesString;
		FParse::Value(FCommandLine::Get(), TEXT("-s.VerbosePackageNames="), VerbosePackageNamesString);
		ParsePackageNames(VerbosePackageNamesString, GAsyncLoading2_VerbosePackageIds);
		ParsePackageNames(DebugPackageNamesString, GAsyncLoading2_VerbosePackageIds);
	}

	FileOpenLogWrapper = (FPlatformFileOpenLog*)(FPlatformFileManager::Get().FindPlatformFile(FPlatformFileOpenLog::GetTypeName()));
#endif

#if USE_NEW_BULKDATA
	FBulkDataBase::SetIoDispatcher(&IoDispatcher);
#endif

	GlobalPackageStore.Initialize();

	AsyncThreadReady.Increment();

	UE_LOG(LogStreaming, Display, TEXT("AsyncLoading2 - Initialized"));
}

void FAsyncLoadingThread2::QueuePackage(FAsyncPackageDesc2& Package)
{
	//TRACE_CPUPROFILER_EVENT_SCOPE(QueuePackage);
	UE_ASYNC_PACKAGE_DEBUG(Package);
	checkf(Package.StoreEntry, TEXT("No package store entry for package %s"), *Package.DiskPackageName.ToString());
	{
		FScopeLock QueueLock(&QueueCritical);
		++QueuedPackagesCounter;
		QueuedPackages.Add(new FAsyncPackageDesc2(Package, MoveTemp(Package.PackageLoadedDelegate)));
	}
	AltZenaphore.NotifyOne();
}

FAsyncPackage2* FAsyncLoadingThread2::FindOrInsertPackage(FAsyncPackageDesc2* Desc, bool& bInserted)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindOrInsertPackage);
	FAsyncPackage2* Package = nullptr;
	bInserted = false;
	{
		FScopeLock LockAsyncPackages(&AsyncPackagesCritical);
		Package = AsyncPackageLookup.FindRef(Desc->GetAsyncPackageId());
		if (!Package)
		{
			Package = CreateAsyncPackage(*Desc);
			checkf(Package, TEXT("Failed to create async package %s"), *Desc->DiskPackageName.ToString());
			Package->AddRef();
			ExistingAsyncPackagesCounter.Increment();
			AsyncPackageLookup.Add(Desc->GetAsyncPackageId(), Package);
			bInserted = true;
		}
		else if (Desc->RequestID > 0)
		{
			Package->AddRequestID(Desc->RequestID);
		}
		if (Desc->PackageLoadedDelegate.IsValid())
		{
			Package->AddCompletionCallback(MoveTemp(Desc->PackageLoadedDelegate));
		}
	}
	return Package;
}

bool FAsyncLoadingThread2::CreateAsyncPackagesFromQueue()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CreateAsyncPackagesFromQueue);
	
	FAsyncLoadingThreadState2& ThreadState = *FAsyncLoadingThreadState2::Get();
	bool bPackagesCreated = false;
	const int32 TimeSliceGranularity = ThreadState.UseTimeLimit() ? 4 : MAX_int32;
	TArray<FAsyncPackageDesc2*> QueueCopy;

	do
	{
		{
			QueueCopy.Reset();
			FScopeLock QueueLock(&QueueCritical);

			const int32 NumPackagesToCopy = FMath::Min(TimeSliceGranularity, QueuedPackages.Num());
			if (NumPackagesToCopy > 0)
			{
				QueueCopy.Append(QueuedPackages.GetData(), NumPackagesToCopy);
				QueuedPackages.RemoveAt(0, NumPackagesToCopy, false);
			}
			else
			{
				break;
			}
		}

		for (FAsyncPackageDesc2* PackageDesc : QueueCopy)
		{
			bool bInserted;
			FAsyncPackage2* Package = FindOrInsertPackage(PackageDesc, bInserted);
			checkf(Package, TEXT("Failed to find or insert imported package %s"), *PackageDesc->DiskPackageName.ToString());

			if (bInserted)
			{
				UE_ASYNC_PACKAGE_LOG(Verbose, *PackageDesc, TEXT("CreateAsyncPackages: AddPackage"),
					TEXT("Start loading package."));
			}
			else
			{
				UE_ASYNC_PACKAGE_LOG_VERBOSE(Verbose, *PackageDesc, TEXT("CreateAsyncPackages: UpdatePackage"),
					TEXT("Package is alreay being loaded."));
			}

			--QueuedPackagesCounter;
			if (Package)
			{
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(ImportPackages);
					Package->ImportPackagesRecursive();
				}

				if (bInserted)
				{
					Package->StartLoading();
				}

				StartBundleIoRequests();
			}
			delete PackageDesc;
		}

		bPackagesCreated |= QueueCopy.Num() > 0;
	} while (!ThreadState.IsTimeLimitExceeded(TEXT("CreateAsyncPackagesFromQueue")));

	return bPackagesCreated;
}

void FAsyncLoadingThread2::AddBundleIoRequest(FAsyncPackage2* Package)
{
	WaitingForIoBundleCounter.Increment();
	WaitingIoRequests.HeapPush({ Package });
}

void FAsyncLoadingThread2::BundleIoRequestCompleted(FAsyncPackage2* Package)
{
	check(PendingBundleIoRequestsTotalSize >= Package->ExportBundlesSize)
	PendingBundleIoRequestsTotalSize -= Package->ExportBundlesSize;
	if (WaitingIoRequests.Num())
	{
		StartBundleIoRequests();
	}
}

void FAsyncLoadingThread2::StartBundleIoRequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StartBundleIoRequests);
	constexpr uint64 MaxPendingRequestsSize = 256 << 20;
	while (WaitingIoRequests.Num())
	{
		FBundleIoRequest& BundleIoRequest = WaitingIoRequests.HeapTop();
		FAsyncPackage2* Package = BundleIoRequest.Package;
		if (PendingBundleIoRequestsTotalSize > 0 && PendingBundleIoRequestsTotalSize + Package->ExportBundlesSize > MaxPendingRequestsSize)
		{
			break;
		}
		PendingBundleIoRequestsTotalSize += Package->ExportBundlesSize;
		WaitingIoRequests.HeapPop(BundleIoRequest, false);

		FIoReadOptions ReadOptions;
		IoDispatcher.ReadWithCallback(CreateIoChunkId(Package->Desc.DiskPackageId.Value(), 0, EIoChunkType::ExportBundleData),
			ReadOptions,
			IoDispatcherPriority_Medium,
			[Package](TIoStatusOr<FIoBuffer> Result)
		{
			if (Result.IsOk())
			{
				Package->IoBuffer = Result.ConsumeValueOrDie();
			}
			else
			{
				UE_ASYNC_PACKAGE_LOG(Error, Package->Desc, TEXT("StartBundleIoRequests: FailedRead"),
					TEXT("Failed reading chunk for package: %s"), *Result.Status().ToString());
				Package->bLoadHasFailed = true;
			}
			Package->GetPackageNode(EEventLoadNode2::Package_ProcessSummary)->ReleaseBarrier();
			Package->AsyncLoadingThread.WaitingForIoBundleCounter.Decrement();
		});
		TRACE_COUNTER_DECREMENT(PendingBundleIoRequests);
	}
}

FEventLoadNode2::FEventLoadNode2(const FAsyncLoadEventSpec* InSpec, FAsyncPackage2* InPackage, int32 InImportOrExportIndex)
	: Spec(InSpec)
	, Package(InPackage)
	, ImportOrExportIndex(InImportOrExportIndex)
{
	check(Spec);
	check(Package);
}

void FEventLoadNode2::DependsOn(FEventLoadNode2* Other)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DependsOn);
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	check(!bDone);
	check(!bFired);
#endif
	uint8 Expected = 0;
	while (!Other->DependencyWriterCount.CompareExchange(Expected, 1))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DependsOnContested);
		check(Expected == 1);
		Expected = 0;
	}
	if (!Other->bDone.Load())
	{
		++BarrierCount;
		if (Other->DependenciesCount == 0)
		{
			Other->SingleDependent = this;
			Other->DependenciesCount = 1;
		}
		else
		{
			if (Other->DependenciesCount == 1)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(DependsOnAlloc);
				FEventLoadNode2* FirstDependency = Other->SingleDependent;
				uint32 NewDependenciesCapacity = 4;
				Other->DependenciesCapacity = NewDependenciesCapacity;
				Other->MultipleDependents = Package->GetGraphAllocator().AllocArcs(NewDependenciesCapacity);
				Other->MultipleDependents[0] = FirstDependency;
			}
			else if (Other->DependenciesCount == Other->DependenciesCapacity)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(DependsOnRealloc);
				FEventLoadNode2** OriginalDependents = Other->MultipleDependents;
				uint32 OldDependenciesCapcity = Other->DependenciesCapacity;
				SIZE_T OldDependenciesSize = OldDependenciesCapcity * sizeof(FEventLoadNode2*);
				uint32 NewDependenciesCapacity = OldDependenciesCapcity * 2;
				Other->DependenciesCapacity = NewDependenciesCapacity;
				Other->MultipleDependents = Package->GetGraphAllocator().AllocArcs(NewDependenciesCapacity);
				FMemory::Memcpy(Other->MultipleDependents, OriginalDependents, OldDependenciesSize);
				Package->GetGraphAllocator().FreeArcs(OriginalDependents, OldDependenciesCapcity);
			}
			Other->MultipleDependents[Other->DependenciesCount++] = this;
		}
	}
	Other->DependencyWriterCount.Store(0);
}

void FEventLoadNode2::AddBarrier()
{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	check(!bDone);
	check(!bFired);
#endif
	++BarrierCount;
}

void FEventLoadNode2::AddBarrier(int32 Count)
{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	check(!bDone);
	check(!bFired);
#endif
	BarrierCount += Count;
}

void FEventLoadNode2::ReleaseBarrier()
{
	check(BarrierCount > 0);
	if (--BarrierCount == 0)
	{
		Fire();
	}
}

void FEventLoadNode2::Fire()
{
	//TRACE_CPUPROFILER_EVENT_SCOPE(Fire);

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	bFired.Store(1);
#endif

	FAsyncLoadingThreadState2* ThreadState = FAsyncLoadingThreadState2::Get();
	if (Spec->bExecuteImmediately && ThreadState && !ThreadState->CurrentEventNode)
	{
		Execute(*ThreadState);
	}
	else
	{
		Spec->EventQueue->Push(this);
	}
}

void FEventLoadNode2::Execute(FAsyncLoadingThreadState2& ThreadState)
{
	//TRACE_CPUPROFILER_EVENT_SCOPE(ExecuteEvent);
	check(BarrierCount.Load() == 0);
	check(!ThreadState.CurrentEventNode || ThreadState.CurrentEventNode == this);

	ThreadState.CurrentEventNode = this;
	EAsyncPackageState::Type State = Spec->Func(Package, ImportOrExportIndex);
	if (State == EAsyncPackageState::Complete)
	{
		ThreadState.CurrentEventNode = nullptr;
		bDone.Store(1);
		ProcessDependencies(ThreadState);
	}
}

void FEventLoadNode2::ProcessDependencies(FAsyncLoadingThreadState2& ThreadState)
{
	//TRACE_CPUPROFILER_EVENT_SCOPE(ProcessDependencies);
	if (DependencyWriterCount.Load() != 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ConcurrentWriter);
		do
		{
			FPlatformProcess::Sleep(0);
		} while (DependencyWriterCount.Load() != 0);
	}

	if (DependenciesCount == 1)
	{
		check(SingleDependent->BarrierCount > 0);
		if (--SingleDependent->BarrierCount == 0)
		{
			ThreadState.NodesToFire.Push(SingleDependent);
		}
	}
	else if (DependenciesCount != 0)
	{
		FEventLoadNode2** Current = MultipleDependents;
		FEventLoadNode2** End = MultipleDependents + DependenciesCount;
		for (; Current < End; ++Current)
		{
			FEventLoadNode2* Dependent = *Current;
			check(Dependent->BarrierCount > 0);
			if (--Dependent->BarrierCount == 0)
			{
				ThreadState.NodesToFire.Push(Dependent);
			}
		}
		ThreadState.DeferredFreeArcs.Add(MakeTuple(MultipleDependents, DependenciesCapacity));
	}
	if (ThreadState.bShouldFireNodes)
	{
		ThreadState.bShouldFireNodes = false;
		while (ThreadState.NodesToFire.Num())
		{
			ThreadState.NodesToFire.Pop(false)->Fire();
		}
		ThreadState.bShouldFireNodes = true;
	}
}

FAsyncLoadEventQueue2::FAsyncLoadEventQueue2()
{
	FMemory::Memzero(Entries, sizeof(Entries));
}

FAsyncLoadEventQueue2::~FAsyncLoadEventQueue2()
{
}

void FAsyncLoadEventQueue2::Push(FEventLoadNode2* Node)
{
	uint64 LocalHead = Head.IncrementExchange();
	FEventLoadNode2* Expected = nullptr;
	if (!Entries[LocalHead % UE_ARRAY_COUNT(Entries)].CompareExchange(Expected, Node))
	{
		*(volatile int*)0 = 0; // queue is full: TODO
	}
	if (Zenaphore)
	{
		Zenaphore->NotifyOne();
	}
}

bool FAsyncLoadEventQueue2::PopAndExecute(FAsyncLoadingThreadState2& ThreadState)
{
	if (ThreadState.CurrentEventNode)
	{
		check(!ThreadState.CurrentEventNode->IsDone());
		ThreadState.CurrentEventNode->Execute(ThreadState);
		return true;
	}

	FEventLoadNode2* Node = nullptr;
	{
		uint64 LocalHead = Head.Load();
		uint64 LocalTail = Tail.Load();
		for (;;)
		{
			if (LocalTail >= LocalHead)
			{
				break;
			}
			if (Tail.CompareExchange(LocalTail, LocalTail + 1))
			{
				while (!Node)
				{
					Node = Entries[LocalTail % UE_ARRAY_COUNT(Entries)].Exchange(nullptr);
				}
				break;
			}
		}
	}

	if (Node)
	{
		//TRACE_CPUPROFILER_EVENT_SCOPE(Execute);
		Node->Execute(ThreadState);
		return true;
	}
	else
	{
		return false;
	}
}

FScopedAsyncPackageEvent2::FScopedAsyncPackageEvent2(FAsyncPackage2* InPackage)
	:Package(InPackage)
{
	check(Package);

	// Update the thread context with the current package. This is used by NotifyConstructedDuringAsyncLoading.
	FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();
	PreviousPackage = static_cast<FAsyncPackage2*>(ThreadContext.AsyncPackage);
	ThreadContext.AsyncPackage = Package;

	Package->BeginAsyncLoad();
}

FScopedAsyncPackageEvent2::~FScopedAsyncPackageEvent2()
{
	Package->EndAsyncLoad();

	// Restore the package from the outer scope
	FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();
	ThreadContext.AsyncPackage = PreviousPackage;
}

void FAsyncLoadingThreadWorker::StartThread()
{
	LLM_SCOPE(ELLMTag::AsyncLoading);
	Trace::ThreadGroupBegin(TEXT("AsyncLoading"));
	Thread = FRunnableThread::Create(this, TEXT("FAsyncLoadingThreadWorker"), 0, TPri_Normal);
	ThreadId = Thread->GetThreadID();
	Trace::ThreadGroupEnd();
}

uint32 FAsyncLoadingThreadWorker::Run()
{
	LLM_SCOPE(ELLMTag::AsyncLoading);

	FPlatformProcess::SetThreadAffinityMask(FPlatformAffinity::GetAsyncLoadingThreadMask());
	FMemory::SetupTLSCachesOnCurrentThread();

	FAsyncLoadingThreadState2::Create(GraphAllocator, IoDispatcher);

	FZenaphoreWaiter Waiter(Zenaphore, TEXT("WaitForEvents"));

	FAsyncLoadingThreadState2& ThreadState = *FAsyncLoadingThreadState2::Get();

	bool bSuspended = false;
	while (!bStopRequested)
	{
		if (bSuspended)
		{
			if (!bSuspendRequested.Load(EMemoryOrder::SequentiallyConsistent))
			{
				bSuspended = false;
			}
			else
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		else
		{
			bool bDidSomething = false;
			{
				FGCScopeGuard GCGuard;
				TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
				++ActiveWorkersCount;
				do
				{
					bDidSomething = EventQueue.PopAndExecute(ThreadState);
					
					if (bSuspendRequested.Load(EMemoryOrder::Relaxed))
					{
						bSuspended = true;
						bDidSomething = true;
						break;
					}
				} while (bDidSomething);
				--ActiveWorkersCount;
			}
			if (!bDidSomething)
			{
				ThreadState.ProcessDeferredFrees();
				Waiter.Wait();
			}
		}
	}
	return 0;
}

FUObjectSerializeContext* FAsyncPackage2::GetSerializeContext()
{
	return FUObjectThreadContext::Get().GetSerializeContext();
}

void FAsyncPackage2::SetupSerializedArcs(const uint8* GraphData, uint64 GraphDataSize)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SetupSerializedArcs);

	FSimpleArchive GraphArchive(GraphData, GraphDataSize);
	int32 ImportedPackagesCount;
	GraphArchive << ImportedPackagesCount;
	for (int32 ImportedPackageIndex = 0; ImportedPackageIndex < ImportedPackagesCount; ++ImportedPackageIndex)
	{
		FPackageId ImportedPackageId;
		int32 ExternalArcCount;
		GraphArchive << ImportedPackageId;
		GraphArchive << ExternalArcCount;

		FAsyncPackage2* ImportedPackage = AsyncLoadingThread.GetAsyncPackage(ImportedPackageId);
		for (int32 ExternalArcIndex = 0; ExternalArcIndex < ExternalArcCount; ++ExternalArcIndex)
		{
			int32 FromExportBundleIndex;
			int32 ToExportBundleIndex;
			GraphArchive << FromExportBundleIndex;
			GraphArchive << ToExportBundleIndex;
			if (ImportedPackage)
			{
				FromExportBundleIndex = FromExportBundleIndex == MAX_uint32
					? ImportedPackage->ExportBundleCount - 1
					: FromExportBundleIndex;

				check(FromExportBundleIndex < ImportedPackage->ExportBundleCount);
				check(ToExportBundleIndex < ExportBundleCount);
				uint32 FromNodeIndexBase = FromExportBundleIndex * EEventLoadNode2::ExportBundle_NumPhases;
				uint32 ToNodeIndexBase = ToExportBundleIndex * EEventLoadNode2::ExportBundle_NumPhases;
				for (int32 Phase = 0; Phase < EEventLoadNode2::ExportBundle_NumPhases; ++Phase)
				{
					uint32 ToNodeIndex = ToNodeIndexBase + Phase;
					check(ToNodeIndex < ExportBundleNodeCount);
					uint32 FromNodeIndex = FromNodeIndexBase + Phase;
					check(FromNodeIndex < ImportedPackage->ExportBundleNodeCount);
					ExportBundleNodes[ToNodeIndex].DependsOn(ImportedPackage->ExportBundleNodes + FromNodeIndex);
				}
			}
		}
	}
}

void FAsyncPackage2::SetupScriptDependencies()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SetupScriptDependencies);

	// UObjectLoadAllCompiledInDefaultProperties is creating CDOs from a flat list.
	// During initial laod, if a CDO called LoadObject for this package it may depend on other CDOs later in the list.
	// Then collect them here, and wait for them to be created before allowing this package to proceed.
	TArray<UClass*, TInlineAllocator<8>> UnresolvedCDOs;
	if (ImportStore.GetUnresolvedCDOs(UnresolvedCDOs))
	{
		AsyncLoadingThread.AddPendingCDOs(this, UnresolvedCDOs);
	}
}

static UObject* GFindExistingScriptImport(FPackageObjectIndex GlobalImportIndex,
	TMap<FPackageObjectIndex, UObject*>& ScriptObjects,
	const TMap<FPackageObjectIndex, FScriptObjectEntry*>& ScriptObjectEntriesMap)
{
	UObject** Object = &ScriptObjects.FindOrAdd(GlobalImportIndex);
	if (!*Object)
	{
		const FScriptObjectEntry* Entry = ScriptObjectEntriesMap.FindRef(GlobalImportIndex);
		check(Entry);
		if (Entry->OuterIndex.IsNull())
		{
			*Object = StaticFindObjectFast(UPackage::StaticClass(), nullptr, MinimalNameToName(Entry->ObjectName), true);
		}
		else
		{
			UObject* Outer = GFindExistingScriptImport(Entry->OuterIndex, ScriptObjects, ScriptObjectEntriesMap);
			Object = &ScriptObjects.FindChecked(GlobalImportIndex);
			if (Outer)
			{
				*Object = StaticFindObjectFast(UObject::StaticClass(), Outer, MinimalNameToName(Entry->ObjectName), false, true);
			}
		}
	}
	return *Object;
}

UObject* FGlobalImportStore::FindScriptImportObjectFromIndex(FPackageObjectIndex GlobalImportIndex)
{
	check(ScriptObjectEntries.Num() > 0);
	return GFindExistingScriptImport(GlobalImportIndex, ScriptObjects, ScriptObjectEntriesMap);
}

void FGlobalImportStore::FindAllScriptObjects()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindAllScriptObjects);
	TStringBuilder<FName::StringBufferSize> Name;
	TArray<UPackage*> ScriptPackages;
	TArray<UObject*> Objects;
	FindAllRuntimeScriptPackages(ScriptPackages);

	for (UPackage* Package : ScriptPackages)
	{
		Objects.Reset();
		GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects*/true);
		for (UObject* Object : Objects)
		{
			if (Object->HasAnyFlags(RF_Public))
			{
				Name.Reset();
				Object->GetPathName(nullptr, Name);
				FPackageObjectIndex GlobalImportIndex = FPackageObjectIndex::FromScriptPath(Name);
				ScriptObjects.Add(GlobalImportIndex, Object);
			}
		}
	}

	ScriptObjectEntriesMap.Empty();
	ScriptObjectEntries.Empty();
	ScriptObjects.Shrink();
}

void FAsyncPackage2::ImportPackagesRecursive()
{
	if (bHasImportedPackagesRecursive)
	{
		return;
	}
	bHasImportedPackagesRecursive = true;

	const int32 ImportedPackageCount = Desc.StoreEntry->ImportedPackages.Num();
	if (!ImportedPackageCount)
	{
		return;
	}

	FPackageStore& GlobalPackageStore = AsyncLoadingThread.GlobalPackageStore;
	for (const FPackageId& ImportedPackageId : Desc.StoreEntry->ImportedPackages)
	{
		FLoadedPackageRef& PackageRef = GlobalPackageStore.LoadedPackageStore.GetPackageRef(ImportedPackageId);
		if (PackageRef.AreAllPublicExportsLoaded())
		{
			continue;
		}

		const FPackageStoreEntry* ImportedPackageEntry = GlobalPackageStore.FindStoreEntry(ImportedPackageId);

		if (!ImportedPackageEntry)
		{
			UE_ASYNC_PACKAGE_LOG(Warning, Desc, TEXT("ImportPackages: SkipPackage"),
				TEXT("Skipping non mounted imported package with id '0x%llX'"), ImportedPackageId.Value());
			PackageRef.SetIsMissingPackage();
			continue;
		}
		else if (PackageRef.IsMissingPackage())
		{
			PackageRef.ClearIsMissingPackage();
		}

		FAsyncPackageDesc2 PackageDesc(INDEX_NONE, ImportedPackageId, ImportedPackageEntry);
		bool bInserted;
		FAsyncPackage2* ImportedPackage = AsyncLoadingThread.FindOrInsertPackage(&PackageDesc, bInserted);

		checkf(ImportedPackage, TEXT("Failed to find or insert imported package with id '0x%llX'"), ImportedPackageId.Value());
		TRACE_LOADTIME_ASYNC_PACKAGE_IMPORT_DEPENDENCY(this, ImportedPackage);

		if (bInserted)
		{
			UE_ASYNC_PACKAGE_LOG(Verbose, PackageDesc, TEXT("ImportPackages: AddPackage"),
				TEXT("Start loading imported package."));
		}
		else
		{
			UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, PackageDesc, TEXT("ImportPackages: UpdatePackage"),
				TEXT("Imported package is already being loaded."));
		}
		ImportedPackage->AddRef();
		ImportedAsyncPackages.Reserve(ImportedPackageCount);
		ImportedAsyncPackages.Add(ImportedPackage);
		if (bInserted)
		{
			ImportedPackage->ImportPackagesRecursive();
			ImportedPackage->StartLoading();
		}
	}
	UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("ImportPackages: ImportsDone"),
		TEXT("All imported packages are now being loaded."));
}

void FAsyncPackage2::StartLoading()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(StartLoading);
	TRACE_LOADTIME_BEGIN_LOAD_ASYNC_PACKAGE(this);
	check(AsyncPackageLoadingState == EAsyncPackageLoadingState2::NewPackage);
	AsyncPackageLoadingState = EAsyncPackageLoadingState2::WaitingForSummary;

	LoadStartTime = FPlatformTime::Seconds();

	AsyncLoadingThread.AddBundleIoRequest(this);
}

EAsyncPackageState::Type FAsyncPackage2::Event_ProcessPackageSummary(FAsyncPackage2* Package, int32)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Event_ProcessPackageSummary);
	UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

	FScopedAsyncPackageEvent2 Scope(Package);

	if (!Package->bLoadHasFailed)
	{
		check(Package->AsyncPackageLoadingState == EAsyncPackageLoadingState2::WaitingForSummary);
		check(Package->ExportBundleEntryIndex == 0);

		const uint8* PackageSummaryData = Package->IoBuffer.Data();
		const FPackageSummary* PackageSummary = reinterpret_cast<const FPackageSummary*>(PackageSummaryData);
		const uint8* GraphData = PackageSummaryData + PackageSummary->GraphDataOffset;
		const uint64 PackageSummarySize = GraphData + PackageSummary->GraphDataSize - PackageSummaryData;

		if (PackageSummary->NameMapNamesSize)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(LoadPackageNameMap);
			const uint8* NameMapNamesData = PackageSummaryData + PackageSummary->NameMapNamesOffset;
			const uint8* NameMapHashesData = PackageSummaryData + PackageSummary->NameMapHashesOffset;
			Package->NameMap.Load(
				TArrayView<const uint8>(NameMapNamesData, PackageSummary->NameMapNamesSize),
				TArrayView<const uint8>(NameMapHashesData, PackageSummary->NameMapHashesSize),
				FMappedName::EType::Package);
		}

		{
			FName PackageName = Package->NameMap.GetName(PackageSummary->Name);
			if (PackageSummary->SourceName != PackageSummary->Name)
			{
				FName SourcePackageName = Package->NameMap.GetName(PackageSummary->SourceName);
				Package->Desc.SetDiskPackageName(PackageName, SourcePackageName);
			}
			else
			{
				Package->Desc.SetDiskPackageName(PackageName);
			}
		}

		Package->CookedHeaderSize = PackageSummary->CookedHeaderSize;
		Package->ImportStore.ImportMap = TArrayView<const FPackageObjectIndex>(
				reinterpret_cast<const FPackageObjectIndex*>(PackageSummaryData + PackageSummary->ImportMapOffset),
				(PackageSummary->ExportMapOffset - PackageSummary->ImportMapOffset) / sizeof(FPackageObjectIndex));
		Package->ExportMap = reinterpret_cast<const FExportMapEntry*>(PackageSummaryData + PackageSummary->ExportMapOffset);
		
		FMemory::Memcpy(Package->ExportBundlesMetaMemory, PackageSummaryData + PackageSummary->ExportBundlesOffset, Package->ExportBundlesMetaSize);

		Package->CreateUPackage(PackageSummary);
		Package->SetupSerializedArcs(GraphData, PackageSummary->GraphDataSize);

		Package->AllExportDataPtr = PackageSummaryData + PackageSummarySize;
		Package->CurrentExportDataPtr = Package->AllExportDataPtr;

		TRACE_LOADTIME_PACKAGE_SUMMARY(Package, PackageSummarySize, Package->ImportStore.ImportMap.Num(), Package->ExportCount);
	}
	Package->AsyncPackageLoadingState = EAsyncPackageLoadingState2::ProcessNewImportsAndExports;

	if (GIsInitialLoad)
	{
		Package->SetupScriptDependencies();
	}
	Package->GetExportBundleNode(ExportBundle_Process, 0)->ReleaseBarrier();

	return EAsyncPackageState::Complete;
}

EAsyncPackageState::Type FAsyncPackage2::Event_ProcessExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Event_ProcessExportBundle);
	UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

	FScopedAsyncPackageEvent2 Scope(Package);

	auto FilterExport = [](const EExportFilterFlags FilterFlags) -> bool
	{
#if UE_SERVER
		return !!(static_cast<uint32>(FilterFlags) & static_cast<uint32>(EExportFilterFlags::NotForServer));
#elif !WITH_SERVER_CODE
		return !!(static_cast<uint32>(FilterFlags) & static_cast<uint32>(EExportFilterFlags::NotForClient));
#else
		static const bool bIsDedicatedServer = !GIsClient && GIsServer;
		static const bool bIsClientOnly = GIsClient && !GIsServer;

		if (bIsDedicatedServer && static_cast<uint32>(FilterFlags) & static_cast<uint32>(EExportFilterFlags::NotForServer))
		{
			return true;
		}

		if (bIsClientOnly && static_cast<uint32>(FilterFlags) & static_cast<uint32>(EExportFilterFlags::NotForClient))
		{
			return true;
		}

		return false;
#endif
	};

	check(ExportBundleIndex < Package->ExportBundleCount);
	
	if (!Package->bLoadHasFailed)
	{
		check(Package->AsyncPackageLoadingState == EAsyncPackageLoadingState2::ProcessNewImportsAndExports);

		const uint64 AllExportDataSize = Package->IoBuffer.DataSize() - (Package->AllExportDataPtr - Package->IoBuffer.Data());
		FExportArchive Ar(Package->AllExportDataPtr, Package->CurrentExportDataPtr, AllExportDataSize);
		{
			Ar.SetUE4Ver(Package->LinkerRoot->LinkerPackageVersion);
			Ar.SetLicenseeUE4Ver(Package->LinkerRoot->LinkerLicenseeVersion);
			// Ar.SetEngineVer(Summary.SavedByEngineVersion); // very old versioning scheme
			// Ar.SetCustomVersions(LinkerRoot->LinkerCustomVersion); // only if not cooking with -unversioned
			Ar.SetUseUnversionedPropertySerialization((Package->LinkerRoot->GetPackageFlags() & PKG_UnversionedProperties) != 0);
			Ar.SetIsLoading(true);
			Ar.SetIsPersistent(true);
			if (Package->LinkerRoot->GetPackageFlags() & PKG_FilterEditorOnly)
			{
				Ar.SetFilterEditorOnly(true);
			}
			Ar.ArAllowLazyLoading = true;

			// FExportArchive special fields
			Ar.CookedHeaderSize = Package->CookedHeaderSize;
			Ar.PackageDesc = &Package->Desc;
			Ar.NameMap = &Package->NameMap;
			Ar.ImportStore = &Package->ImportStore;
			Ar.Exports = &Package->Exports;
			Ar.ExportMap = Package->ExportMap;
			Ar.ExportCount = Package->ExportCount;
			Ar.ExternalReadDependencies = &Package->ExternalReadDependencies;
		}
		const FExportBundleHeader* ExportBundle = Package->ExportBundleHeaders + ExportBundleIndex;
		const FExportBundleEntry* BundleEntries = Package->ExportBundleEntries + ExportBundle->FirstEntryIndex;
		const FExportBundleEntry* BundleEntry = BundleEntries + Package->ExportBundleEntryIndex;
		const FExportBundleEntry* BundleEntryEnd = BundleEntries + ExportBundle->EntryCount;
		check(BundleEntry <= BundleEntryEnd);
		while (BundleEntry < BundleEntryEnd)
		{
			if (FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("Event_ProcessExportBundle")))
			{
				return EAsyncPackageState::TimeOut;
			}
			const FExportMapEntry& ExportMapEntry = Package->ExportMap[BundleEntry->LocalExportIndex];
			FExportObject& Export = Package->Exports[BundleEntry->LocalExportIndex];
			Export.bFiltered = FilterExport(ExportMapEntry.FilterFlags);

			if (BundleEntry->CommandType == FExportBundleEntry::ExportCommandType_Create)
			{
				Package->EventDrivenCreateExport(BundleEntry->LocalExportIndex);
			}
			else
			{
				check(BundleEntry->CommandType == FExportBundleEntry::ExportCommandType_Serialize);

				const uint64 CookedSerialSize = ExportMapEntry.CookedSerialSize;
				UObject* Object = Export.Object;

				check(Package->CurrentExportDataPtr + CookedSerialSize <= Package->IoBuffer.Data() + Package->IoBuffer.DataSize());
				check(Object || Export.bFiltered || Export.bExportLoadFailed);

				Ar.ExportBufferBegin(ExportMapEntry.CookedSerialOffset, ExportMapEntry.CookedSerialSize);

				const int64 Pos = Ar.Tell();
				checkf(CookedSerialSize <= uint64(Ar.TotalSize() - Pos),
					TEXT("Package %s: Expected read size: %llu - Remaining archive size: %llu"),
					*Package->Desc.DiskPackageName.ToString(), CookedSerialSize, uint64(Ar.TotalSize() - Pos));

				const bool bSerialized = Package->EventDrivenSerializeExport(BundleEntry->LocalExportIndex, Ar);
				if (!bSerialized)
				{
					Ar.Skip(CookedSerialSize);
				}
				checkf(CookedSerialSize == uint64(Ar.Tell() - Pos),
					TEXT("Package %s: Expected read size: %llu - Actual read size: %llu"),
					*Package->Desc.DiskPackageName.ToString(), CookedSerialSize, uint64(Ar.Tell() - Pos));

				Ar.ExportBufferEnd();

				check((Object && !Object->HasAnyFlags(RF_NeedLoad)) || Export.bFiltered || Export.bExportLoadFailed);

				Package->CurrentExportDataPtr += CookedSerialSize;
			}
			++BundleEntry;
			++Package->ExportBundleEntryIndex;
		}
	}
	
	Package->ExportBundleEntryIndex = 0;

	if (ExportBundleIndex + 1 < Package->ExportBundleCount)
	{
		Package->GetExportBundleNode(ExportBundle_Process, ExportBundleIndex + 1)->ReleaseBarrier();
	}
	else
	{
		check(Package->AsyncPackageLoadingState == EAsyncPackageLoadingState2::ProcessNewImportsAndExports);
		Package->ImportStore.ImportMap = TArrayView<const FPackageObjectIndex>();
		Package->bAllExportsSerialized = true;
		Package->IoBuffer = FIoBuffer();
		Package->AsyncPackageLoadingState = EAsyncPackageLoadingState2::PostLoad_Etc;

		if (Package->ExternalReadDependencies.Num() == 0)
		{
			Package->GetNode(Package_ExportsSerialized)->ReleaseBarrier();
		}
		else
		{
			Package->AsyncLoadingThread.ExternalReadQueue.Enqueue(Package);
		}
	}

	if (ExportBundleIndex == 0)
	{
		Package->AsyncLoadingThread.BundleIoRequestCompleted(Package);
	}

	return EAsyncPackageState::Complete;
}

UObject* FAsyncPackage2::EventDrivenIndexToObject(FPackageObjectIndex Index, bool bCheckSerialized)
{
	UObject* Result = nullptr;
	if (Index.IsNull())
	{
		return Result;
	}
	if (Index.IsExport())
	{
		Result = Exports[Index.ToExport()].Object;
	}
	else if (Index.IsImport())
	{
		Result = ImportStore.FindOrGetImportObject(Index);
		UE_CLOG(!Result, LogStreaming, Warning, TEXT("Missing %s import 0x%llX for package %s"),
			Index.IsScriptImport() ? TEXT("script") : TEXT("package"),
			Index.Value(),
			*Desc.DiskPackageName.ToString());
	}
#if DO_CHECK
	if (bCheckSerialized && !IsFullyLoadedObj(Result))
	{
		/*FEventLoadNode2* MyDependentNode = GetExportNode(EEventLoadNode2::Export_Serialize, Index.ToExport());
		if (!Result)
		{
			UE_LOG(LogStreaming, Error, TEXT("Missing Dependency, request for %s but it hasn't been created yet."), *Linker->GetPathName(Index));
		}
		else if (!MyDependentNode || MyDependentNode->GetBarrierCount() > 0)
		{
			UE_LOG(LogStreaming, Fatal, TEXT("Missing Dependency, request for %s but it was still waiting for serialization."), *Linker->GetPathName(Index));
		}
		else
		{
			UE_LOG(LogStreaming, Fatal, TEXT("Missing Dependency, request for %s but it was still has RF_NeedLoad."), *Linker->GetPathName(Index));
		}*/
		UE_LOG(LogStreaming, Warning, TEXT("Missing Dependency"));
	}
	if (Result)
	{
		UE_CLOG(Result->HasAnyInternalFlags(EInternalObjectFlags::Unreachable), LogStreaming, Fatal, TEXT("Returning an object  (%s) from EventDrivenIndexToObject that is unreachable."), *Result->GetFullName());
	}
#endif
	return Result;
}


void FAsyncPackage2::EventDrivenCreateExport(int32 LocalExportIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CreateExport);

	const FExportMapEntry& Export = ExportMap[LocalExportIndex];
	FExportObject& ExportObject = Exports[LocalExportIndex];
	UObject*& Object = ExportObject.Object;
	check(!Object);

	TRACE_LOADTIME_CREATE_EXPORT_SCOPE(this, &Object);

	FName ObjectName;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ObjectNameFixup);
		ObjectName = NameMap.GetName(Export.ObjectName);
	}

	if (ExportObject.bFiltered | ExportObject.bExportLoadFailed)
	{
		if (ExportObject.bExportLoadFailed)
		{
			UE_ASYNC_PACKAGE_LOG(Warning, Desc, TEXT("CreateExport"), TEXT("Skipped failed export %s"), *ObjectName.ToString());
		}
		else
		{
			UE_ASYNC_PACKAGE_LOG_VERBOSE(Verbose, Desc, TEXT("CreateExport"), TEXT("Skipped filtered export %s"), *ObjectName.ToString());
		}
		return;
	}

	LLM_SCOPED_TAG_WITH_OBJECT_IN_SET(GetLinkerRoot(), ELLMTagSet::Assets);
	// LLM_SCOPED_TAG_WITH_OBJECT_IN_SET((Export.DynamicType == FObjectExport::EDynamicType::DynamicType) ? UDynamicClass::StaticClass() : 
	// 	CastEventDrivenIndexToObject<UClass>(Export.ClassIndex, false), ELLMTagSet::AssetClasses);

	bool bIsCompleteyLoaded = false;
	UClass* LoadClass = Export.ClassIndex.IsNull() ? UClass::StaticClass() : CastEventDrivenIndexToObject<UClass>(Export.ClassIndex, true);
	UObject* ThisParent = Export.OuterIndex.IsNull() ? LinkerRoot : EventDrivenIndexToObject(Export.OuterIndex, false);

	if (!LoadClass)
	{
		UE_ASYNC_PACKAGE_LOG(Error, Desc, TEXT("CreateExport"), TEXT("Could not find class object for %s"), *ObjectName.ToString());
		ExportObject.bExportLoadFailed = true;
		return;
	}
	if (!ThisParent)
	{
		UE_ASYNC_PACKAGE_LOG(Error, Desc, TEXT("CreateExport"), TEXT("Could not find outer object for %s"), *ObjectName.ToString());
		ExportObject.bExportLoadFailed = true;
		return;
	}
	check(!dynamic_cast<UObjectRedirector*>(ThisParent));

	// Try to find existing object first as we cannot in-place replace objects, could have been created by other export in this package
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FindExport);
		Object = StaticFindObjectFastInternal(NULL, ThisParent, ObjectName, true);
	}

	const bool bIsNewObject = !Object;

	// Object is found in memory.
	if (Object)
	{
		// If this object was allocated but never loaded (components created by a constructor, CDOs, etc) make sure it gets loaded
		// Do this for all subobjects created in the native constructor.
		const EObjectFlags ObjectFlags = Object->GetFlags();
		bIsCompleteyLoaded = !!(ObjectFlags & RF_LoadCompleted);
		if (!bIsCompleteyLoaded)
		{
			check(!(ObjectFlags & (RF_NeedLoad | RF_WasLoaded))); // If export exist but is not completed, it is expected to have been created from a native constructor and not from EventDrivenCreateExport, but who knows...?
			if (ObjectFlags & RF_ClassDefaultObject)
			{
				// never call PostLoadSubobjects on class default objects, this matches the behavior of the old linker where
				// StaticAllocateObject prevents setting of RF_NeedPostLoad and RF_NeedPostLoadSubobjects, but FLinkerLoad::Preload
				// assigns RF_NeedPostLoad for blueprint CDOs:
				Object->SetFlags(RF_NeedLoad | RF_NeedPostLoad | RF_WasLoaded);
			}
			else
			{
				Object->SetFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WasLoaded);
			}
		}
	}
	else
	{
		// Find the Archetype object for the one we are loading.
		check(!Export.TemplateIndex.IsNull());
		UObject* Template = EventDrivenIndexToObject(Export.TemplateIndex, true);
		if (!Template)
		{
			UE_ASYNC_PACKAGE_LOG(Error, Desc, TEXT("CreateExport"), TEXT("Could not find template object for %s"), *ObjectName.ToString());
			ExportObject.bExportLoadFailed = true;
			return;
		}
		// we also need to ensure that the template has set up any instances
		Template->ConditionalPostLoadSubobjects();

		check(!GVerifyObjectReferencesOnly); // not supported with the event driven loader
		// Create the export object, marking it with the appropriate flags to
		// indicate that the object's data still needs to be loaded.
		EObjectFlags ObjectLoadFlags = Export.ObjectFlags;
		ObjectLoadFlags = EObjectFlags(ObjectLoadFlags | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WasLoaded);

		// If we are about to create a CDO, we need to ensure that all parent sub-objects are loaded
		// to get default value initialization to work.
#if DO_CHECK
		if ((ObjectLoadFlags & RF_ClassDefaultObject) != 0)
		{
			UClass* SuperClass = LoadClass->GetSuperClass();
			UObject* SuperCDO = SuperClass ? SuperClass->GetDefaultObject() : nullptr;
			check(!SuperCDO || Template == SuperCDO); // the template for a CDO is the CDO of the super
			if (SuperClass && !SuperClass->IsNative())
			{
				check(SuperCDO);
				if (SuperClass->HasAnyFlags(RF_NeedLoad))
				{
					UE_LOG(LogStreaming, Fatal, TEXT("Super %s had RF_NeedLoad while creating %s"), *SuperClass->GetFullName(), *ObjectName.ToString());
					return;
				}
				if (SuperCDO->HasAnyFlags(RF_NeedLoad))
				{
					UE_LOG(LogStreaming, Fatal, TEXT("Super CDO %s had RF_NeedLoad while creating %s"), *SuperCDO->GetFullName(), *ObjectName.ToString());
					return;
				}
				TArray<UObject*> SuperSubObjects;
				GetObjectsWithOuter(SuperCDO, SuperSubObjects, /*bIncludeNestedObjects=*/ false, /*ExclusionFlags=*/ RF_NoFlags, /*InternalExclusionFlags=*/ EInternalObjectFlags::Native);

				for (UObject* SubObject : SuperSubObjects)
				{
					if (SubObject->HasAnyFlags(RF_NeedLoad))
					{
						UE_LOG(LogStreaming, Fatal, TEXT("Super CDO subobject %s had RF_NeedLoad while creating %s"), *SubObject->GetFullName(), *ObjectName.ToString());
						return;
					}
				}
			}
			else
			{
				check(Template->IsA(LoadClass));
			}
		}
#endif
		checkf(!LoadClass->HasAnyFlags(RF_NeedLoad),
			TEXT("LoadClass %s had RF_NeedLoad while creating %s"), *LoadClass->GetFullName(), *ObjectName.ToString());
		checkf(!(LoadClass->GetDefaultObject() && LoadClass->GetDefaultObject()->HasAnyFlags(RF_NeedLoad)), 
			TEXT("Class CDO %s had RF_NeedLoad while creating %s"), *LoadClass->GetDefaultObject()->GetFullName(), *ObjectName.ToString());
		checkf(!Template->HasAnyFlags(RF_NeedLoad),
			TEXT("Template %s had RF_NeedLoad while creating %s"), *Template->GetFullName(), *ObjectName.ToString());

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(ConstructObject);
			Object = StaticConstructObject_Internal
				(
				 LoadClass,
				 ThisParent,
				 ObjectName,
				 ObjectLoadFlags,
				 EInternalObjectFlags::None,
				 Template,
				 false,
				 nullptr,
				 true
				);
		}

		if (GIsInitialLoad || GUObjectArray.IsOpenForDisregardForGC())
		{
			Object->AddToRoot();
		}

		check(Object->GetClass() == LoadClass);
		check(Object->GetFName() == ObjectName);
	}

	check(Object);
	PinObjectForGC(Object, bIsNewObject);

	if (Desc.IsTrackingPublicExports() && !Export.GlobalImportIndex.IsNull())
	{
		check(Object->HasAnyFlags(RF_Public));
		ImportStore.StoreGlobalObject(Desc.DiskPackageId, Export.GlobalImportIndex, Object);

		UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("CreateExport"),
			TEXT("Created public export %s. Tracked as 0x%llX"), *Object->GetPathName(), Export.GlobalImportIndex.Value());
	}
	else
	{
		UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("CreateExport"), TEXT("Created %s export %s. Not tracked."),
			Object->HasAnyFlags(RF_Public) ? TEXT("public") : TEXT("private"), *Object->GetPathName());
	}
}

bool FAsyncPackage2::EventDrivenSerializeExport(int32 LocalExportIndex, FExportArchive& Ar)
{
	LLM_SCOPE(ELLMTag::UObject);
	TRACE_CPUPROFILER_EVENT_SCOPE(SerializeExport);

	const FExportMapEntry& Export = ExportMap[LocalExportIndex];
	FExportObject& ExportObject = Exports[LocalExportIndex];
	UObject* Object = ExportObject.Object;
	check(Object || (ExportObject.bFiltered | ExportObject.bExportLoadFailed));

	TRACE_LOADTIME_SERIALIZE_EXPORT_SCOPE(Object, Export.CookedSerialSize);

	if ((ExportObject.bFiltered | ExportObject.bExportLoadFailed) || !(Object && Object->HasAnyFlags(RF_NeedLoad)))
	{
		if (ExportObject.bExportLoadFailed)
		{
			UE_ASYNC_PACKAGE_LOG(Warning, Desc, TEXT("SerializeExport"),
				TEXT("Skipped failed export %s"), *NameMap.GetName(Export.ObjectName).ToString());
		}
		else if (ExportObject.bFiltered)
		{
			UE_ASYNC_PACKAGE_LOG_VERBOSE(Verbose, Desc, TEXT("SerializeExport"),
				TEXT("Skipped filtered export %s"), *NameMap.GetName(Export.ObjectName).ToString());
		}
		else
		{
			UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("SerializeExport"),
				TEXT("Skipped already serialized export %s"), *NameMap.GetName(Export.ObjectName).ToString());
		}
		return false;
	}

	// If this is a struct, make sure that its parent struct is completely loaded
	if (UStruct* Struct = dynamic_cast<UStruct*>(Object))
	{
		if (!Export.SuperIndex.IsNull())
		{
			UStruct* SuperStruct = CastEventDrivenIndexToObject<UStruct>(Export.SuperIndex, true);
			if (!SuperStruct)
			{
				UE_ASYNC_PACKAGE_LOG(Error, Desc, TEXT("SerializeExport"),
					TEXT("Could not find SuperStruct object for %s"), *NameMap.GetName(Export.ObjectName).ToString());
				ExportObject.bExportLoadFailed = true;
				return false;
			}
			Struct->SetSuperStruct(SuperStruct);
			if (UClass* ClassObject = dynamic_cast<UClass*>(Object))
			{
				ClassObject->Bind();
			}
		}
	}

	LLM_SCOPED_TAG_WITH_OBJECT_IN_SET(GetLinkerRoot(), ELLMTagSet::Assets);
	// LLM_SCOPED_TAG_WITH_OBJECT_IN_SET((Export.DynamicType == FObjectExport::EDynamicType::DynamicType) ? UDynamicClass::StaticClass() :
	// 	CastEventDrivenIndexToObject<UClass>(Export.ClassIndex, false), ELLMTagSet::AssetClasses);

	// cache archetype
	// prevents GetArchetype from hitting the expensive GetArchetypeFromRequiredInfoImpl
	check(!Export.TemplateIndex.IsNull());
	UObject* Template = EventDrivenIndexToObject(Export.TemplateIndex, true);
	check(Template);
	CacheArchetypeForObject(Object, Template);

	Object->ClearFlags(RF_NeedLoad);

	FUObjectSerializeContext* LoadContext = GetSerializeContext();
	UObject* PrevSerializedObject = LoadContext->SerializedObject;
	LoadContext->SerializedObject = Object;

	Ar.TemplateForGetArchetypeFromLoader = Template;

	if (Object->HasAnyFlags(RF_ClassDefaultObject))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SerializeDefaultObject);
		Object->GetClass()->SerializeDefaultObject(Object, Ar);
	}
	else
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SerializeObject);
		Object->Serialize(Ar);
	}
	Ar.TemplateForGetArchetypeFromLoader = nullptr;

	Object->SetFlags(RF_LoadCompleted);
	LoadContext->SerializedObject = PrevSerializedObject;

#if DO_CHECK
	if (Object->HasAnyFlags(RF_ClassDefaultObject) && Object->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		check(Object->HasAllFlags(RF_NeedPostLoad | RF_WasLoaded));
		//Object->SetFlags(RF_NeedPostLoad | RF_WasLoaded);
	}
#endif

	UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("SerializeExport"), TEXT("Serialized export %s"), *Object->GetPathName());

	// push stats so that we don't overflow number of tags per thread during blocking loading
	LLM_PUSH_STATS_FOR_ASSET_TAGS();

	return true;
}

EAsyncPackageState::Type FAsyncPackage2::Event_ExportsDone(FAsyncPackage2* Package, int32)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Event_ExportsDone);
	UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

	if (Package->Desc.IsTrackingPublicExports())
	{
		FLoadedPackageRef& PackageRef =
			Package->AsyncLoadingThread.GlobalPackageStore.LoadedPackageStore.GetPackageRef((Package->Desc.DiskPackageId));
		PackageRef.SetAllPublicExportsLoaded();
	}

	Package->GetExportBundleNode(EEventLoadNode2::ExportBundle_PostLoad, 0)->ReleaseBarrier();
	return EAsyncPackageState::Complete;
}

EAsyncPackageState::Type FAsyncPackage2::Event_PostLoadExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Event_PostLoad);
	UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

	check(!Package->HasFinishedLoading());
	check(Package->ExternalReadDependencies.Num() == 0);
	
	FAsyncPackageScope2 PackageScope(Package);

	/*TSet<FAsyncPackage2*> Visited;
	TArray<FAsyncPackage2*> ProcessQueue;
	ProcessQueue.Push(Package);
	while (ProcessQueue.Num() > 0)
	{
		FAsyncPackage2* CurrentPackage = ProcessQueue.Pop();
		Visited.Add(CurrentPackage);
		if (!CurrentPackage->bAllExportsSerialized)
		{
			UE_DEBUG_BREAK();
		}
		for (const FPackageId& ImportedPackageId : CurrentPackage->StoreEntry.ImportedPackages)
		{
			FAsyncPackage2* ImportedPackage = CurrentPackage->AsyncLoadingThread.GetAsyncPackage(ImportedPackageId);
			if (ImportedPackage && !Visited.Contains(ImportedPackage))
			{
				ProcessQueue.Push(ImportedPackage);
			}
		}
	}*/
	
	check(ExportBundleIndex < Package->ExportBundleCount);

	EAsyncPackageState::Type LoadingState = EAsyncPackageState::Complete;

	if (!Package->bLoadHasFailed)
	{
		// Begin async loading, simulates BeginLoad
		Package->BeginAsyncLoad();

		SCOPED_LOADTIMER(PostLoadObjectsTime);

		FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();
		TGuardValue<bool> GuardIsRoutingPostLoad(ThreadContext.IsRoutingPostLoad, true);

		const bool bAsyncPostLoadEnabled = FAsyncLoadingThreadSettings::Get().bAsyncPostLoadEnabled;
		const bool bIsMultithreaded = Package->AsyncLoadingThread.IsMultithreaded();

		const FExportBundleHeader* ExportBundle = Package->ExportBundleHeaders + ExportBundleIndex;
		const FExportBundleEntry* BundleEntries = Package->ExportBundleEntries + ExportBundle->FirstEntryIndex;
		const FExportBundleEntry* BundleEntry = BundleEntries + Package->ExportBundleEntryIndex;
		const FExportBundleEntry* BundleEntryEnd = BundleEntries + ExportBundle->EntryCount;
		check(BundleEntry <= BundleEntryEnd);
		while (BundleEntry < BundleEntryEnd)
		{
			if (FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("Event_PostLoadExportBundle")))
			{
				LoadingState = EAsyncPackageState::TimeOut;
				break;
			}
			
			if (BundleEntry->CommandType == FExportBundleEntry::ExportCommandType_Serialize)
			{
				do
				{
					FExportObject& Export = Package->Exports[BundleEntry->LocalExportIndex];
					if (Export.bFiltered | Export.bExportLoadFailed)
					{
						break;
					}

					UObject* Object = Export.Object;
					check(Object);
					check(!Object->HasAnyFlags(RF_NeedLoad));
					if (!Object->HasAnyFlags(RF_NeedPostLoad))
					{
						break;
					}

					check(Object->IsReadyForAsyncPostLoad());
					if (!bIsMultithreaded || (bAsyncPostLoadEnabled && CanPostLoadOnAsyncLoadingThread(Object)))
					{
						ThreadContext.CurrentlyPostLoadedObjectByALT = Object;
						{
							TRACE_LOADTIME_POSTLOAD_EXPORT_SCOPE(Object);
							Object->ConditionalPostLoad();
							Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
						}
						ThreadContext.CurrentlyPostLoadedObjectByALT = nullptr;
					}
				} while (false);
			}
			++BundleEntry;
			++Package->ExportBundleEntryIndex;
		}

		// End async loading, simulates EndLoad
		Package->EndAsyncLoad();
	}
	
	if (LoadingState == EAsyncPackageState::TimeOut)
	{
		return LoadingState;
	}

	Package->ExportBundleEntryIndex = 0;

	if (ExportBundleIndex + 1 < Package->ExportBundleCount)
	{
		Package->GetExportBundleNode(ExportBundle_PostLoad, ExportBundleIndex + 1)->ReleaseBarrier();
	}
	else
	{
		// Finish objects (removing EInternalObjectFlags::AsyncLoading, dissociate imports and forced exports, 
		// call completion callback, ...
		// If the load has failed, perform completion callbacks and then quit
		LoadingState = Package->FinishObjects();

		// Mark this package as loaded if everything completed.
		Package->bLoadHasFinished = (LoadingState == EAsyncPackageState::Complete);

		if (Package->bLoadHasFinished)
		{
			check(Package->AsyncPackageLoadingState == EAsyncPackageLoadingState2::PostLoad_Etc);
			Package->AsyncPackageLoadingState = EAsyncPackageLoadingState2::PackageComplete;
		}

		if (Package->LinkerRoot && LoadingState == EAsyncPackageState::Complete)
		{
			UE_ASYNC_PACKAGE_LOG(Verbose, Package->Desc, TEXT("AsyncThread: FullyLoaded"),
				TEXT("Async loading of package is done, and UPackage is marked as fully loaded."))
				Package->LinkerRoot->MarkAsFullyLoaded();
		}

		// TODO: This doesn't seem right, this could be set to Failed above
		check(LoadingState == EAsyncPackageState::Complete);

		Package->GetExportBundleNode(ExportBundle_DeferredPostLoad, 0)->ReleaseBarrier();
	}

	return EAsyncPackageState::Complete;
}

EAsyncPackageState::Type FAsyncPackage2::Event_DeferredPostLoadExportBundle(FAsyncPackage2* Package, int32 ExportBundleIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Event_DeferredPostLoad);
	UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

	FAsyncPackageScope2 PackageScope(Package);

	check(ExportBundleIndex < Package->ExportBundleCount);
	EAsyncPackageState::Type LoadingState = EAsyncPackageState::Complete;

	if (Package->bLoadHasFailed)
	{
		FSoftObjectPath::InvalidateTag();
		FUniqueObjectGuid::InvalidateTag();
	}
	else
	{
		TGuardValue<bool> GuardIsRoutingPostLoad(PackageScope.ThreadContext.IsRoutingPostLoad, true);
		FAsyncLoadingTickScope2 InAsyncLoadingTick(Package->AsyncLoadingThread);

		const FExportBundleHeader* ExportBundle = Package->ExportBundleHeaders + ExportBundleIndex;
		const FExportBundleEntry* BundleEntries = Package->ExportBundleEntries + ExportBundle->FirstEntryIndex;
		const FExportBundleEntry* BundleEntry = BundleEntries + Package->ExportBundleEntryIndex;
		const FExportBundleEntry* BundleEntryEnd = BundleEntries + ExportBundle->EntryCount;
		check(BundleEntry <= BundleEntryEnd);
		while (BundleEntry < BundleEntryEnd)
		{
			if (Package->AsyncLoadingThread.IsAsyncLoadingSuspended() || FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("Event_DeferredPostLoadExportBundle")))
			{
				LoadingState = EAsyncPackageState::TimeOut;
				break;
			}

			if (BundleEntry->CommandType == FExportBundleEntry::ExportCommandType_Serialize)
			{
				do
				{
					FExportObject& Export = Package->Exports[BundleEntry->LocalExportIndex];
					if (Export.bFiltered | Export.bExportLoadFailed)
					{
						break;
					}

					UObject* Object = Export.Object;
					check(Object);
					check(!Object->HasAnyFlags(RF_NeedLoad));
					if (Object->HasAnyFlags(RF_NeedPostLoad))
					{
						PackageScope.ThreadContext.CurrentlyPostLoadedObjectByALT = Object;
						{
							TRACE_LOADTIME_POSTLOAD_EXPORT_SCOPE(Object);
							Object->ConditionalPostLoad();
						}
						PackageScope.ThreadContext.CurrentlyPostLoadedObjectByALT = nullptr;
					}
					Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
				} while (false);
			}
			++BundleEntry;
			++Package->ExportBundleEntryIndex;
		}
	}

	if (LoadingState == EAsyncPackageState::TimeOut)
	{
		return LoadingState;
	}

	Package->ExportBundleEntryIndex = 0;

	if (ExportBundleIndex + 1 < Package->ExportBundleCount)
	{
		Package->GetExportBundleNode(ExportBundle_DeferredPostLoad, ExportBundleIndex + 1)->ReleaseBarrier();
	}
	else
	{
		Package->bAllExportsDeferredPostLoaded = true;
		Package->AsyncLoadingThread.LoadedPackagesToProcess.Add(Package);
	}

	return EAsyncPackageState::Complete;
}

FEventLoadNode2* FAsyncPackage2::GetPackageNode(EEventLoadNode2 Phase)
{
	check(Phase < EEventLoadNode2::Package_NumPhases);
	return PackageNodes + Phase;
}

FEventLoadNode2* FAsyncPackage2::GetExportBundleNode(EEventLoadNode2 Phase, uint32 ExportBundleIndex)
{
	check(ExportBundleIndex < uint32(ExportBundleCount));
	uint32 ExportBundleNodeIndex = ExportBundleIndex * EEventLoadNode2::ExportBundle_NumPhases + Phase;
	return ExportBundleNodes + ExportBundleNodeIndex;
}

FEventLoadNode2* FAsyncPackage2::GetNode(int32 NodeIndex)
{ 
	check(uint32(NodeIndex) < EEventLoadNode2::Package_NumPhases + ExportBundleNodeCount);
	return &PackageNodes[NodeIndex];
}

#if ALT2_VERIFY_RECURSIVE_LOADS 
struct FScopedLoadRecursionVerifier
{
	int32& Level;
	FScopedLoadRecursionVerifier(int32& InLevel) : Level(InLevel)
	{
		UE_CLOG(Level > 0, LogStreaming, Error, TEXT("Entering recursive load level: %d"), Level);
		++Level;
		check(Level == 1);
	}
	~FScopedLoadRecursionVerifier()
	{
		--Level;
		UE_CLOG(Level > 0, LogStreaming, Error, TEXT("Leaving recursive load level: %d"), Level);
		check(Level == 0);
	}
};
#endif

EAsyncPackageState::Type FAsyncLoadingThread2::ProcessAsyncLoadingFromGameThread(int32& OutPackagesProcessed)
{
	SCOPED_LOADTIMER(AsyncLoadingTime);

	check(IsInGameThread());

	// If we're not multithreaded and flushing async loading, update the thread heartbeat
	const bool bNeedsHeartbeatTick = !FAsyncLoadingThread2::IsMultithreaded();
	OutPackagesProcessed = 0;

#if ALT2_VERIFY_RECURSIVE_LOADS 
	FScopedLoadRecursionVerifier LoadRecursionVerifier(this->LoadRecursionLevel);
#endif
	FAsyncLoadingTickScope2 InAsyncLoadingTick(*this);
	uint32 LoopIterations = 0;

	FAsyncLoadingThreadState2& ThreadState = *FAsyncLoadingThreadState2::Get();

	while (true)
	{
		do 
		{
			if (bNeedsHeartbeatTick && (++LoopIterations) % 32 == 31)
			{
				// Update heartbeat after 32 events
				FThreadHeartBeat::Get().HeartBeat();
			}

			if (ThreadState.IsTimeLimitExceeded(TEXT("ProcessAsyncLoadingFromGameThread")))
			{
				return EAsyncPackageState::TimeOut;
			}

			if (IsAsyncLoadingSuspended())
			{
				return EAsyncPackageState::TimeOut;
			}

			if (QueuedPackagesCounter)
			{
				CreateAsyncPackagesFromQueue();
				OutPackagesProcessed++;
				break;
			}

			bool bPopped = false;
			for (FAsyncLoadEventQueue2* Queue : AltEventQueues)
			{
				if (Queue->PopAndExecute(ThreadState))
				{
					bPopped = true;
					break;
				}
			}
			if (bPopped)
			{
				OutPackagesProcessed++;
				break;
			}

			if (!ExternalReadQueue.IsEmpty())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(ProcessExternalReads);

				FAsyncPackage2* Package = nullptr;
				ExternalReadQueue.Dequeue(Package);

				EAsyncPackageState::Type Result = Package->ProcessExternalReads(FAsyncPackage2::ExternalReadAction_Wait);
				check(Result == EAsyncPackageState::Complete);

				OutPackagesProcessed++;
				break;
			}

			ThreadState.ProcessDeferredFrees();

			if (!DeferredDeletePackages.IsEmpty())
			{
				FAsyncPackage2* Package = nullptr;
				DeferredDeletePackages.Dequeue(Package);
				TRACE_CPUPROFILER_EVENT_SCOPE(DeleteAsyncPackage);
				UE_ASYNC_PACKAGE_DEBUG(Package->Desc);
				delete Package;
				OutPackagesProcessed++;
				break;
			}

			return EAsyncPackageState::Complete;
		} while (false);
	}
	check(false);
	return EAsyncPackageState::Complete;
}

bool FAsyncPackage2::AreAllDependenciesFullyLoadedInternal(FAsyncPackage2* Package, TSet<FPackageId>& VisitedPackages, FPackageId& OutPackageId)
{
	for (const FPackageId& ImportedPackageId : Package->Desc.StoreEntry->ImportedPackages)
	{
		if (VisitedPackages.Contains(ImportedPackageId))
		{
			continue;
		}
		VisitedPackages.Add(ImportedPackageId);

		FAsyncPackage2* AsyncRoot = AsyncLoadingThread.GetAsyncPackage(ImportedPackageId);
		if (AsyncRoot)
		{
			if (!AsyncRoot->bAllExportsDeferredPostLoaded)
			{
				OutPackageId = ImportedPackageId;
				return false;
			}

			if (!AreAllDependenciesFullyLoadedInternal(AsyncRoot, VisitedPackages, OutPackageId))
			{
				return false;
			}
		}
	}
	return true;
}

bool FAsyncPackage2::AreAllDependenciesFullyLoaded(TSet<FPackageId>& VisitedPackages)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AreAllDependenciesFullyLoaded);
	VisitedPackages.Reset();
	FPackageId PackageId;
	const bool bLoaded = AreAllDependenciesFullyLoadedInternal(this, VisitedPackages, PackageId);
	if (!bLoaded)
	{
		FAsyncPackage2* AsyncRoot = AsyncLoadingThread.GetAsyncPackage(PackageId);
		UE_LOG(LogStreaming, Verbose, TEXT("AreAllDependenciesFullyLoaded: '%s' doesn't have all exports processed by DeferredPostLoad"),
			*AsyncRoot->Desc.DiskPackageName.ToString());
	}
	return bLoaded;
}

EAsyncPackageState::Type FAsyncLoadingThread2::ProcessLoadedPackagesFromGameThread(bool& bDidSomething, int32 FlushRequestID)
{
	EAsyncPackageState::Type Result = EAsyncPackageState::Complete;

	// This is for debugging purposes only. @todo remove
	volatile int32 CurrentAsyncLoadingCounter = AsyncLoadingTickCounter;

	if (IsMultithreaded() &&
		ENamedThreads::GetRenderThread() == ENamedThreads::GameThread &&
		!FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread)) // render thread tasks are actually being sent to the game thread.
	{
		// The async loading thread might have queued some render thread tasks (we don't have a render thread yet, so these are actually sent to the game thread)
		// We need to process them now before we do any postloads.
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("ProcessLoadedPackagesFromGameThread")))
		{
			return EAsyncPackageState::TimeOut;
		}
	}

	// For performance reasons this set is created here and reset inside of AreAllDependenciesFullyLoaded
	TSet<FPackageId> VisistedPackages;

	FAsyncLoadingThreadState2& ThreadState = *FAsyncLoadingThreadState2::Get();
	for (;;)
	{
		if (ThreadState.IsTimeLimitExceeded(TEXT("ProcessAsyncLoadingFromGameThread")))
		{
			Result = EAsyncPackageState::TimeOut;
			break;
		}

		bool bLocalDidSomething = false;
		bLocalDidSomething |= MainThreadEventQueue.PopAndExecute(ThreadState);

		bLocalDidSomething |= LoadedPackagesToProcess.Num() > 0;
		for (int32 PackageIndex = 0; PackageIndex < LoadedPackagesToProcess.Num() && !IsAsyncLoadingSuspended(); ++PackageIndex)
		{
			SCOPED_LOADTIMER(ProcessLoadedPackagesTime);
			FAsyncPackage2* Package = LoadedPackagesToProcess[PackageIndex];
			UE_ASYNC_PACKAGE_DEBUG(Package->Desc);

			TArray<UObject*> CDODefaultSubobjects;
			// Clear async loading flags (we still want RF_Async, but EInternalObjectFlags::AsyncLoading can be cleared)
			for (int32 FinalizeIndex = 0; FinalizeIndex < Package->ExportCount; ++FinalizeIndex)
			{
				const FExportObject& Export = Package->Exports[FinalizeIndex];
				if (Export.bFiltered | Export.bExportLoadFailed)
				{
					continue;
				}

				UObject* Object = Export.Object;

				// CDO need special handling, no matter if it's listed in DeferredFinalizeObjects or created here for DynamicClass
				UObject* CDOToHandle = nullptr;

				// Dynamic Class doesn't require/use pre-loading (or post-loading). 
				// The CDO is created at this point, because now it's safe to solve cyclic dependencies.
				if (UDynamicClass* DynamicClass = Cast<UDynamicClass>(Object))
				{
					check((DynamicClass->ClassFlags & CLASS_Constructed) != 0);

					//native blueprint 

					check(DynamicClass->HasAnyClassFlags(CLASS_TokenStreamAssembled));
					// this block should be removed entirely when and if we add the CDO to the fake export table
					CDOToHandle = DynamicClass->GetDefaultObject(false);
					UE_CLOG(!CDOToHandle, LogStreaming, Fatal, TEXT("EDL did not create the CDO for %s before it finished loading."), *DynamicClass->GetFullName());
					CDOToHandle->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
				}
				else
				{
					CDOToHandle = ((Object != nullptr) && Object->HasAnyFlags(RF_ClassDefaultObject)) ? Object : nullptr;
				}

				// Clear AsyncLoading in CDO's subobjects.
				if (CDOToHandle != nullptr)
				{
					CDOToHandle->GetDefaultSubobjects(CDODefaultSubobjects);
					for (UObject* SubObject : CDODefaultSubobjects)
					{
						if (SubObject && SubObject->HasAnyInternalFlags(EInternalObjectFlags::AsyncLoading))
						{
							SubObject->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
						}
					}
					CDODefaultSubobjects.Reset();
				}
			}

			// Mark package as having been fully loaded and update load time.
			if (Package->LinkerRoot && !Package->bLoadHasFailed)
			{
				Package->LinkerRoot->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
				Package->LinkerRoot->MarkAsFullyLoaded();
				Package->LinkerRoot->SetLoadTime(FPlatformTime::Seconds() - Package->LoadStartTime);

				if (CanCreateObjectClusters())
				{
					for (const FExportObject& Export : Package->Exports)
					{
						if (!(Export.bFiltered | Export.bExportLoadFailed) && Export.Object->CanBeClusterRoot())
						{
							Package->bHasClusterObjects = true;
							break;
						}
					}
				}
			}

			FSoftObjectPath::InvalidateTag();
			FUniqueObjectGuid::InvalidateTag();

			{
				FScopeLock LockAsyncPackages(&AsyncPackagesCritical);
				AsyncPackageLookup.Remove(Package->Desc.GetAsyncPackageId());
				Package->ClearConstructedObjects();
			}

			// Remove the package from the list before we trigger the callbacks, 
			// this is to ensure we can re-enter FlushAsyncLoading from any of the callbacks
			LoadedPackagesToProcess.RemoveAt(PackageIndex--);

			// Incremented on the Async Thread, now decrement as we're done with this package				
			const int32 NewExistingAsyncPackagesCounterValue = ExistingAsyncPackagesCounter.Decrement();

			UE_CLOG(NewExistingAsyncPackagesCounterValue < 0, LogStreaming, Fatal, TEXT("ExistingAsyncPackagesCounter is negative, this means we loaded more packages then requested so there must be a bug in async loading code."));

			TRACE_LOADTIME_END_LOAD_ASYNC_PACKAGE(Package);

			// Call external callbacks
			const EAsyncLoadingResult::Type LoadingResult = Package->HasLoadFailed() ? EAsyncLoadingResult::Failed : EAsyncLoadingResult::Succeeded;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(PackageCompletionCallbacks);
				Package->CallCompletionCallbacks(LoadingResult);
			}

			// We don't need the package anymore
			check(!Package->bCompleted);
			check(!CompletedPackages.Contains(Package));
			CompletedPackages.Add(Package);
			Package->bCompleted = true;
			Package->MarkRequestIDsAsComplete();

			UE_ASYNC_PACKAGE_LOG(Verbose, Package->Desc, TEXT("GameThread: LoadCompleted"),
				TEXT("All loading of package is done, and the async package and load request will be deleted."));
		}

		bLocalDidSomething |= QueuedFailedPackageCallbacks.Num() > 0;
		for (FQueuedFailedPackageCallback& QueuedFailedPackageCallback : QueuedFailedPackageCallbacks)
		{
			QueuedFailedPackageCallback.Callback->ExecuteIfBound(QueuedFailedPackageCallback.PackageName, nullptr, EAsyncLoadingResult::Failed);
		}
		QueuedFailedPackageCallbacks.Empty();

		bLocalDidSomething |= CompletedPackages.Num() > 0;
		for (int32 PackageIndex = 0; PackageIndex < CompletedPackages.Num(); ++PackageIndex)
		{
			FAsyncPackage2* Package = CompletedPackages[PackageIndex];
			{
				bool bSafeToDelete = false;
				if (Package->HasClusterObjects())
				{
					// This package will create GC clusters but first check if all dependencies of this package have been fully loaded
					if (Package->AreAllDependenciesFullyLoaded(VisistedPackages))
					{
						if (Package->CreateClusters() == EAsyncPackageState::Complete)
						{
							// All clusters created, it's safe to delete the package
							bSafeToDelete = true;
						}
						else
						{
							// Cluster creation timed out
							Result = EAsyncPackageState::TimeOut;
							break;
						}
					}
				}
				else
				{
					// No clusters to create so it's safe to delete
					bSafeToDelete = true;
				}

				if (bSafeToDelete)
				{
					UE_ASYNC_PACKAGE_DEBUG(Package->Desc);
					CompletedPackages.RemoveAtSwap(PackageIndex--);
					Package->ClearImportedPackages();
					Package->ReleaseRef();
				}
			}

			// push stats so that we don't overflow number of tags per thread during blocking loading
			LLM_PUSH_STATS_FOR_ASSET_TAGS();
		}
		
		if (!bLocalDidSomething)
		{
			break;
		}

		bDidSomething = true;
		
		if (FlushRequestID != INDEX_NONE && !ContainsRequestID(FlushRequestID))
		{
			// The only package we care about has finished loading, so we're good to exit
			break;
		}
	}

	if (Result == EAsyncPackageState::Complete)
	{
		// We're not done until all packages have been deleted
		Result = CompletedPackages.Num() ? EAsyncPackageState::PendingImports  : EAsyncPackageState::Complete;
	}

	return Result;
}

EAsyncPackageState::Type FAsyncLoadingThread2::TickAsyncLoadingFromGameThread(bool bUseTimeLimit, bool bUseFullTimeLimit, float TimeLimit, int32 FlushRequestID)
{
	//TRACE_INT_VALUE(QueuedPackagesCounter, QueuedPackagesCounter);
	//TRACE_INT_VALUE(GraphNodeCount, GraphAllocator.TotalNodeCount);
	//TRACE_INT_VALUE(GraphArcCount, GraphAllocator.TotalArcCount);
	//TRACE_MEMORY_VALUE(GraphMemory, GraphAllocator.TotalAllocated);


	check(IsInGameThread());
	check(!IsGarbageCollecting());

	const bool bLoadingSuspended = IsAsyncLoadingSuspended();
	EAsyncPackageState::Type Result = bLoadingSuspended ? EAsyncPackageState::PendingImports : EAsyncPackageState::Complete;

	if (!bLoadingSuspended)
	{
		FAsyncLoadingThreadState2::Get()->SetTimeLimit(bUseTimeLimit, TimeLimit);

		const bool bIsMultithreaded = FAsyncLoadingThread2::IsMultithreaded();
		double TickStartTime = FPlatformTime::Seconds();

		bool bDidSomething = false;
		{
			Result = ProcessLoadedPackagesFromGameThread(bDidSomething, FlushRequestID);
			double TimeLimitUsedForProcessLoaded = FPlatformTime::Seconds() - TickStartTime;
			UE_CLOG(!GIsEditor && bUseTimeLimit && TimeLimitUsedForProcessLoaded > .1f, LogStreaming, Warning, TEXT("Took %6.2fms to ProcessLoadedPackages"), float(TimeLimitUsedForProcessLoaded) * 1000.0f);
		}

		if (!bIsMultithreaded && Result != EAsyncPackageState::TimeOut)
		{
			Result = TickAsyncThreadFromGameThread(bDidSomething);
		}

		if (Result != EAsyncPackageState::TimeOut)
		{
			// Flush deferred messages
			if (ExistingAsyncPackagesCounter.GetValue() == 0)
			{
				bDidSomething = true;
				FDeferredMessageLog::Flush();
			}

			if (GIsInitialLoad && !bDidSomething)
			{
				bDidSomething = ProcessPendingCDOs();
			}
		}

		// Call update callback once per tick on the game thread
		FCoreDelegates::OnAsyncLoadingFlushUpdate.Broadcast();
	}

	return Result;
}

FAsyncLoadingThread2::FAsyncLoadingThread2(FIoDispatcher& InIoDispatcher)
	: Thread(nullptr)
	, IoDispatcher(InIoDispatcher)
	, GlobalPackageStore(InIoDispatcher, GlobalNameMap)
{
	GEventDrivenLoaderEnabled = true;

#if LOADTIMEPROFILERTRACE_ENABLED
	FLoadTimeProfilerTracePrivate::Init();
#endif

	AltEventQueues.Add(&EventQueue);
	for (FAsyncLoadEventQueue2* Queue : AltEventQueues)
	{
		Queue->SetZenaphore(&AltZenaphore);
	}

	EventSpecs.AddDefaulted(EEventLoadNode2::Package_NumPhases + EEventLoadNode2::ExportBundle_NumPhases);
	EventSpecs[EEventLoadNode2::Package_ProcessSummary] = { &FAsyncPackage2::Event_ProcessPackageSummary, &EventQueue, false };
	EventSpecs[EEventLoadNode2::Package_ExportsSerialized] = { &FAsyncPackage2::Event_ExportsDone, &EventQueue, true };

	EventSpecs[EEventLoadNode2::Package_NumPhases + EEventLoadNode2::ExportBundle_Process] = { &FAsyncPackage2::Event_ProcessExportBundle, &EventQueue, false };
	EventSpecs[EEventLoadNode2::Package_NumPhases + EEventLoadNode2::ExportBundle_PostLoad] = { &FAsyncPackage2::Event_PostLoadExportBundle, &EventQueue, false };
	EventSpecs[EEventLoadNode2::Package_NumPhases + EEventLoadNode2::ExportBundle_DeferredPostLoad] = { &FAsyncPackage2::Event_DeferredPostLoadExportBundle, &MainThreadEventQueue, false };

	CancelLoadingEvent = FPlatformProcess::GetSynchEventFromPool();
	ThreadSuspendedEvent = FPlatformProcess::GetSynchEventFromPool();
	ThreadResumedEvent = FPlatformProcess::GetSynchEventFromPool();
	AsyncLoadingTickCounter = 0;

	FAsyncLoadingThreadState2::TlsSlot = FPlatformTLS::AllocTlsSlot();
	FAsyncLoadingThreadState2::Create(GraphAllocator, IoDispatcher);

	UE_LOG(LogStreaming, Display, TEXT("AsyncLoading2 - Created: Event Driven Loader: %s, Async Loading Thread: %s, Async Post Load: %s"),
		GEventDrivenLoaderEnabled ? TEXT("true") : TEXT("false"),
		FAsyncLoadingThreadSettings::Get().bAsyncLoadingThreadEnabled ? TEXT("true") : TEXT("false"),
		FAsyncLoadingThreadSettings::Get().bAsyncPostLoadEnabled ? TEXT("true") : TEXT("false"));
}

FAsyncLoadingThread2::~FAsyncLoadingThread2()
{
	if (Thread)
	{
		ShutdownLoading();
	}

#if USE_NEW_BULKDATA
	FBulkDataBase::SetIoDispatcher(nullptr);
#endif
}

void FAsyncLoadingThread2::ShutdownLoading()
{
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().RemoveAll(this);
	FCoreUObjectDelegates::GetPostGarbageCollect().RemoveAll(this);

	delete Thread;
	Thread = nullptr;
	FPlatformProcess::ReturnSynchEventToPool(CancelLoadingEvent);
	CancelLoadingEvent = nullptr;
	FPlatformProcess::ReturnSynchEventToPool(ThreadSuspendedEvent);
	ThreadSuspendedEvent = nullptr;
	FPlatformProcess::ReturnSynchEventToPool(ThreadResumedEvent);
	ThreadResumedEvent = nullptr;
}

void FAsyncLoadingThread2::StartThread()
{
	// Make sure the GC sync object is created before we start the thread (apparently this can happen before we call InitUObject())
	FGCCSyncObject::Create();

	if (!FAsyncLoadingThreadSettings::Get().bAsyncLoadingThreadEnabled)
	{
		FinalizeInitialLoad();
	}
	else if (!Thread)
	{
		UE_LOG(LogStreaming, Log, TEXT("Starting Async Loading Thread."));
		bThreadStarted = true;
		FPlatformMisc::MemoryBarrier();
		Trace::ThreadGroupBegin(TEXT("AsyncLoading"));
		Thread = FRunnableThread::Create(this, TEXT("FAsyncLoadingThread"), 0, TPri_Normal);
		Trace::ThreadGroupEnd();
	}

	UE_LOG(LogStreaming, Display, TEXT("AsyncLoading2 - Thread Started: %s, IsInitialLoad: %s"),
		FAsyncLoadingThreadSettings::Get().bAsyncLoadingThreadEnabled ? TEXT("true") : TEXT("false"),
		GIsInitialLoad ? TEXT("true") : TEXT("false"));
}

bool FAsyncLoadingThread2::Init()
{
	return true;
}

void FAsyncLoadingThread2::SuspendWorkers()
{
	if (bWorkersSuspended)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(SuspendWorkers);
	for (FAsyncLoadingThreadWorker& Worker : Workers)
	{
		Worker.SuspendThread();
	}
	while (ActiveWorkersCount > 0)
	{
		FPlatformProcess::SleepNoStats(0);
	}
	bWorkersSuspended = true;
}

void FAsyncLoadingThread2::ResumeWorkers()
{
	if (!bWorkersSuspended)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(ResumeWorkers);
	for (FAsyncLoadingThreadWorker& Worker : Workers)
	{
		Worker.ResumeThread();
	}
	bWorkersSuspended = false;
}

void FAsyncLoadingThread2::LazyInitializeFromLoadPackage()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LazyInitializeFromLoadPackage);
	GlobalNameMap.LoadGlobal(IoDispatcher);
	if (GIsInitialLoad)
	{
		GlobalPackageStore.SetupInitialLoadData();
	}
	GlobalPackageStore.LoadContainers(IoDispatcher.GetMountedContainers());
	IoDispatcher.OnContainerMounted().AddRaw(&GlobalPackageStore, &FPackageStore::OnContainerMounted);
}


void FAsyncLoadingThread2::FinalizeInitialLoad()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FinalizeInitialLoad);
	GlobalPackageStore.FinalizeInitialLoad();
	check(PendingCDOs.Num() == 0);
	PendingCDOs.Empty();
}

uint32 FAsyncLoadingThread2::Run()
{
	LLM_SCOPE(ELLMTag::AsyncLoading);

	AsyncLoadingThreadID = FPlatformTLS::GetCurrentThreadId();

	FAsyncLoadingThreadState2::Create(GraphAllocator, IoDispatcher);

	TRACE_LOADTIME_START_ASYNC_LOADING();

	FPlatformProcess::SetThreadAffinityMask(FPlatformAffinity::GetAsyncLoadingThreadMask());
	FMemory::SetupTLSCachesOnCurrentThread();

	FAsyncLoadingThreadState2& ThreadState = *FAsyncLoadingThreadState2::Get();
	
	FinalizeInitialLoad();

	FZenaphoreWaiter Waiter(AltZenaphore, TEXT("WaitForEvents"));
	bool bIsSuspended = false;
	while (!bStopRequested)
	{
		if (bIsSuspended)
		{
			if (!bSuspendRequested.Load(EMemoryOrder::SequentiallyConsistent) && !IsGarbageCollectionWaiting())
			{
				ThreadResumedEvent->Trigger();
				bIsSuspended = false;
				ResumeWorkers();
			}
			else
			{
				FPlatformProcess::Sleep(0.001f);
			}
		}
		else
		{
			bool bDidSomething = false;
			{
				FGCScopeGuard GCGuard;
				TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
				do
				{
					bDidSomething = false;

					if (QueuedPackagesCounter)
					{
						if (CreateAsyncPackagesFromQueue())
						{
							bDidSomething = true;
						}
					}

					bool bShouldSuspend = false;
					bool bPopped = false;
					do 
					{
						bPopped = false;
						for (FAsyncLoadEventQueue2* Queue : AltEventQueues)
						{
							if (Queue->PopAndExecute(ThreadState))
							{
								bPopped = true;
								bDidSomething = true;
							}

							if (bSuspendRequested.Load(EMemoryOrder::Relaxed) || IsGarbageCollectionWaiting())
							{
								bShouldSuspend = true;
								bPopped = false;
								break;
							}
						}
					} while (bPopped);

					if (bShouldSuspend || bSuspendRequested.Load(EMemoryOrder::Relaxed) || IsGarbageCollectionWaiting())
					{
						SuspendWorkers();
						ThreadSuspendedEvent->Trigger();
						bIsSuspended = true;
						bDidSomething = true;
						break;
					}

					{
						bool bDidExternalRead = false;
						do
						{
							bDidExternalRead = false;
							FAsyncPackage2* Package = nullptr;
							if (ExternalReadQueue.Peek(Package))
							{
								TRACE_CPUPROFILER_EVENT_SCOPE(ProcessExternalReads);

								FAsyncPackage2::EExternalReadAction Action = FAsyncPackage2::ExternalReadAction_Poll;

								EAsyncPackageState::Type Result = Package->ProcessExternalReads(Action);
								if (Result == EAsyncPackageState::Complete)
								{
									ExternalReadQueue.Pop();
									bDidExternalRead = true;
									bDidSomething = true;
								}
							}
						} while (bDidExternalRead);
					}

				} while (bDidSomething);
			}

			if (!bDidSomething)
			{
				if (ThreadState.HasDeferredFrees())
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
					ThreadState.ProcessDeferredFrees();
					bDidSomething = true;
				}

				if (!DeferredDeletePackages.IsEmpty())
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
					FAsyncPackage2* Package = nullptr;
					int32 Count = 0;
					while (++Count <= 100 && DeferredDeletePackages.Dequeue(Package))
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(DeleteAsyncPackage);
						UE_ASYNC_PACKAGE_DEBUG(Package->Desc);
						delete Package;
					}
					bDidSomething = true;
				}
			}

			if (!bDidSomething)
			{
				FAsyncPackage2* Package = nullptr;
				if (WaitingForIoBundleCounter.GetValue() > 0)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
					TRACE_CPUPROFILER_EVENT_SCOPE(WaitingForIo);
					Waiter.Wait();
				}
				else if (ExternalReadQueue.Peek(Package))
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(AsyncLoadingTime);
					TRACE_CPUPROFILER_EVENT_SCOPE(ProcessExternalReads);

					EAsyncPackageState::Type Result = Package->ProcessExternalReads(FAsyncPackage2::ExternalReadAction_Wait);
					check(Result == EAsyncPackageState::Complete);
					ExternalReadQueue.Pop();
				}
				else
				{
					Waiter.Wait();
				}
			}
		}
	}
	return 0;
}

EAsyncPackageState::Type FAsyncLoadingThread2::TickAsyncThreadFromGameThread(bool& bDidSomething)
{
	check(IsInGameThread());
	EAsyncPackageState::Type Result = EAsyncPackageState::Complete;
	
	int32 ProcessedRequests = 0;
	if (AsyncThreadReady.GetValue())
	{
		if (IsGarbageCollectionWaiting() || FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("TickAsyncThreadFromGameThread")))
		{
			Result = EAsyncPackageState::TimeOut;
		}
		else
		{
			FGCScopeGuard GCGuard;
			Result = ProcessAsyncLoadingFromGameThread(ProcessedRequests);
			bDidSomething = bDidSomething || ProcessedRequests > 0;
		}
	}

	return Result;
}

void FAsyncLoadingThread2::Stop()
{
	for (FAsyncLoadingThreadWorker& Worker : Workers)
	{
		Worker.StopThread();
	}
	bSuspendRequested = true;
	bStopRequested = true;
	AltZenaphore.NotifyAll();
}

void FAsyncLoadingThread2::CancelLoading()
{
	check(false);
	// TODO
}

void FAsyncLoadingThread2::SuspendLoading()
{
	UE_CLOG(!IsInGameThread() || IsInSlateThread(), LogStreaming, Fatal, TEXT("Async loading can only be suspended from the main thread"));
	if (!bSuspendRequested)
	{
		bSuspendRequested = true;
		if (IsMultithreaded())
		{
			TRACE_LOADTIME_SUSPEND_ASYNC_LOADING();
			AltZenaphore.NotifyAll();
			ThreadSuspendedEvent->Wait();
		}
	}
}

void FAsyncLoadingThread2::ResumeLoading()
{
	check(IsInGameThread() && !IsInSlateThread());
	if (bSuspendRequested)
	{
		bSuspendRequested = false;
		if (IsMultithreaded())
		{
			ThreadResumedEvent->Wait();
			TRACE_LOADTIME_RESUME_ASYNC_LOADING();
		}
	}
}

float FAsyncLoadingThread2::GetAsyncLoadPercentage(const FName& PackageName)
{
	float LoadPercentage = -1.0f;
	FAsyncPackage2* Package = FindAsyncPackage(PackageName);
	if (Package)
	{
		LoadPercentage = Package->GetLoadPercentage();
	}
	return LoadPercentage;
}

#if ALT2_VERIFY_ASYNC_FLAGS
static void VerifyLoadFlagsWhenFinishedLoading()
{
	const EInternalObjectFlags AsyncFlags =
		EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoading;

	const EObjectFlags LoadIntermediateFlags = 
		EObjectFlags::RF_NeedLoad | EObjectFlags::RF_WillBeLoaded |
		EObjectFlags::RF_NeedPostLoad | RF_NeedPostLoadSubobjects;

	for (int32 ObjectIndex = 0; ObjectIndex < GUObjectArray.GetObjectArrayNum(); ++ObjectIndex)
	{
		FUObjectItem* ObjectItem = &GUObjectArray.GetObjectItemArrayUnsafe()[ObjectIndex];
		if (UObject* Obj = static_cast<UObject*>(ObjectItem->Object))
		{
			const EInternalObjectFlags InternalFlags = Obj->GetInternalFlags();
			const EObjectFlags Flags = Obj->GetFlags();
			const bool bHasAnyAsyncFlags = !!(InternalFlags & AsyncFlags);
			const bool bHasAnyLoadIntermediateFlags = !!(Flags & LoadIntermediateFlags);
			const bool bWasLoaded = !!(Flags & RF_WasLoaded);
			const bool bLoadCompleted = !!(Flags & RF_LoadCompleted);

			ensureMsgf(!bHasAnyLoadIntermediateFlags,
				TEXT("Object '%s' (ObjectFlags=%X, InternalObjectFlags=%x) should not have any load flags now")
				TEXT(", or this check is incorrectly reached during active loading."),
				*Obj->GetFullName(),
				Flags,
				InternalFlags);

			if (bWasLoaded)
			{
				const bool bIsPackage = Obj->IsA(UPackage::StaticClass());

				ensureMsgf(bIsPackage || bLoadCompleted,
					TEXT("Object '%s' (ObjectFlags=%x, InternalObjectFlags=%x) is a serialized object and should be completely loaded now")
					TEXT(", or this check is incorrectly reached during active loading."),
					*Obj->GetFullName(),
					Flags,
					InternalFlags);

				ensureMsgf(!bHasAnyAsyncFlags,
					TEXT("Object '%s' (ObjectFlags=%x, InternalObjectFlags=%x) is a serialized object and should not have any async flags now")
					TEXT(", or this check is incorrectly reached during active loading."),
					*Obj->GetFullName(),
					Flags,
					InternalFlags);
			}
		}
	}
	UE_LOG(LogStreaming, Log, TEXT("Verified load flags when finished active loading."));
}
#endif

void FAsyncLoadingThread2::NotifyUnreachableObjects(const TArrayView<FUObjectItem*>& UnreachableObjects)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(NotifyUnreachableObjects);

	if (GExitPurge)
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();
	const int32 OldLoadedPackageCount = GlobalPackageStore.LoadedPackageStore.NumTracked();
	const int32 OldPublicExportCount = GlobalPackageStore.GetGlobalImportStore().PublicExportObjects.Num();
	int32 PublicExportCount = 0;
	int32 PackageCount = 0;

	for (FUObjectItem* ObjectItem : UnreachableObjects)
	{
		UObject* Object = static_cast<UObject*>(ObjectItem->Object);
		check(Object);
		if (Object->HasAllFlags(RF_WasLoaded | RF_Public))
		{
			if (Object->GetOuter())
			{
				// TRACE_CPUPROFILER_EVENT_SCOPE(PackageStoreRemovePublicExport);
				GlobalPackageStore.RemovePublicExport(Object);
				++PublicExportCount;
			}
			else
			{
				// TRACE_CPUPROFILER_EVENT_SCOPE(PackageStoreRemovePackage);
				UPackage* Package = static_cast<UPackage*>(Object);
				GlobalPackageStore.RemovePackage(Package);
				++PackageCount;
			}
		}
	}

	const int32 NewLoadedPackageCount = GlobalPackageStore.LoadedPackageStore.NumTracked();
	const int32 NewPublicExportCount = GlobalPackageStore.GetGlobalImportStore().PublicExportObjects.Num();
	const int32 RemovedLoadedPackageCount = OldLoadedPackageCount - NewLoadedPackageCount;
	const int32 RemovedPublicExportCount = OldPublicExportCount - NewPublicExportCount;

	if (RemovedLoadedPackageCount > 0 || RemovedPublicExportCount > 0)
	{
		UE_LOG(LogStreaming, Display,
			TEXT("%f ms for processing %d/%d objects in NotifyUnreachableObjects. ")
			TEXT("Removed %d/%d (%d->%d tracked) packages and %d/%d (%d->%d tracked) public exports."),
			(FPlatformTime::Seconds() - StartTime) * 1000,
			PublicExportCount + PackageCount, UnreachableObjects.Num(),
			RemovedLoadedPackageCount, PackageCount, OldLoadedPackageCount, NewLoadedPackageCount,
			RemovedPublicExportCount, PublicExportCount, OldPublicExportCount, NewPublicExportCount);
	}
	else
	{
		UE_LOG(LogStreaming, Display, TEXT("%f ms for skipping %d/%d objects in NotifyUnreachableObjects."),
			(FPlatformTime::Seconds() - StartTime) * 1000,
			PublicExportCount + PackageCount, UnreachableObjects.Num());
	}

#if ALT2_VERIFY_ASYNC_FLAGS
	if (!IsAsyncLoadingPackages())
	{
		GlobalPackageStore.LoadedPackageStore.VerifyLoadedPackages();
		VerifyLoadFlagsWhenFinishedLoading();
	}
#endif
}

/**
 * Call back into the async loading code to inform of the creation of a new object
 * @param Object		Object created
 * @param bSubObjectThatAlreadyExists	Object created as a sub-object of a loaded object
 */
void FAsyncLoadingThread2::NotifyConstructedDuringAsyncLoading(UObject* Object, bool bSubObjectThatAlreadyExists)
{
	FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();
	if (!ThreadContext.AsyncPackage)
	{
		// Something is creating objects on the async loading thread outside of the actual async loading code
		// e.g. ShaderCodeLibrary::OnExternalReadCallback doing FTaskGraphInterface::Get().WaitUntilTaskCompletes(Event);
		return;
	}

	// Mark objects created during async loading process (e.g. from within PostLoad or CreateExport) as async loaded so they 
	// cannot be found. This requires also keeping track of them so we can remove the async loading flag later one when we 
	// finished routing PostLoad to all objects.
	if (!bSubObjectThatAlreadyExists)
	{
		Object->SetInternalFlags(EInternalObjectFlags::AsyncLoading);
	}
	FAsyncPackage2* AsyncPackage2 = (FAsyncPackage2*)ThreadContext.AsyncPackage;
	AsyncPackage2->AddConstructedObject(Object, bSubObjectThatAlreadyExists);
}

/*-----------------------------------------------------------------------------
	FAsyncPackage implementation.
-----------------------------------------------------------------------------*/

/**
* Constructor
*/
FAsyncPackage2::FAsyncPackage2(
	const FAsyncPackageDesc2& InDesc,
	FAsyncLoadingThread2& InAsyncLoadingThread,
	FAsyncLoadEventGraphAllocator& InGraphAllocator,
	const FAsyncLoadEventSpec* EventSpecs)
: Desc(InDesc)
, LinkerRoot(nullptr)
, DeferredClusterIndex(0)
, bHasClusterObjects(false)
, bLoadHasFailed(false)
, bLoadHasFinished(false)
, bCreatedLinkerRoot(false)
, LoadStartTime(0.0)
, LoadPercentage(0)
, AsyncLoadingThread(InAsyncLoadingThread)
, GraphAllocator(InGraphAllocator)
, ImportStore(AsyncLoadingThread.GlobalPackageStore, Desc)
, AsyncPackageLoadingState(EAsyncPackageLoadingState2::NewPackage)
, bAllExportsSerialized(false)
, bAllExportsDeferredPostLoaded(false)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(NewAsyncPackage);
	TRACE_LOADTIME_NEW_ASYNC_PACKAGE(this, Desc.DiskPackageName);
	AddRequestID(Desc.RequestID);

	ExportBundlesSize = Desc.StoreEntry->ExportBundlesSize;
	ExportBundleCount = Desc.StoreEntry->ExportBundleCount;
	LoadOrder = Desc.StoreEntry->LoadOrder;
	ExportCount = Desc.StoreEntry->ExportCount;
	Exports.AddDefaulted(ExportCount);
	ConstructedObjects.Reserve(ExportCount + 1); // +1 for UPackage

	CreateNodes(EventSpecs);

	ExportBundlesMetaSize = 
		sizeof(FExportBundleHeader) * ExportBundleCount +
		sizeof(FExportBundleEntry) * ExportCount * 2;
	
	ExportBundlesMetaMemory = reinterpret_cast<uint8*>(FMemory::Malloc(ExportBundlesMetaSize));
	ExportBundleHeaders = reinterpret_cast<const FExportBundleHeader*>(ExportBundlesMetaMemory);
	ExportBundleEntries = reinterpret_cast<const FExportBundleEntry*>(ExportBundleHeaders + ExportBundleCount);

}

void FAsyncPackage2::CreateNodes(const FAsyncLoadEventSpec* EventSpecs)
{
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CreateNodes);
		ExportBundleNodeCount = ExportBundleCount * EEventLoadNode2::ExportBundle_NumPhases;

		PackageNodes = GraphAllocator.AllocNodes(EEventLoadNode2::Package_NumPhases + ExportBundleNodeCount);
		for (int32 Phase = 0; Phase < EEventLoadNode2::Package_NumPhases; ++Phase)
		{
			new (PackageNodes + Phase) FEventLoadNode2(EventSpecs + Phase, this, -1);
		}

		FEventLoadNode2* ProcessSummaryNode = PackageNodes + EEventLoadNode2::Package_ProcessSummary;
		ProcessSummaryNode->AddBarrier();
		FEventLoadNode2* ExportsSerializedNode = PackageNodes + EEventLoadNode2::Package_ExportsSerialized;

		ExportBundleNodes = PackageNodes + EEventLoadNode2::Package_NumPhases;
		for (int32 ExportBundleIndex = 0; ExportBundleIndex < ExportBundleCount; ++ExportBundleIndex)
		{
			uint32 NodeIndex = EEventLoadNode2::ExportBundle_NumPhases * ExportBundleIndex;
			for (int32 Phase = 0; Phase < EEventLoadNode2::ExportBundle_NumPhases; ++Phase)
			{
				FEventLoadNode2* ExportBundleNode = ExportBundleNodes + NodeIndex + Phase;
				new (ExportBundleNode) FEventLoadNode2(EventSpecs + EEventLoadNode2::Package_NumPhases + Phase, this, ExportBundleIndex);
				ExportBundleNode->AddBarrier();
			}
		}
		ExportsSerializedNode->AddBarrier();
	}
}

FAsyncPackage2::~FAsyncPackage2()
{
	TRACE_LOADTIME_DESTROY_ASYNC_PACKAGE(this);
	UE_ASYNC_PACKAGE_LOG(Verbose, Desc, TEXT("AsyncThread: Deleted"), TEXT("Package deleted."));

	checkf(RefCount == 0, TEXT("RefCount is not 0 when deleting package %s"),
		*Desc.DiskPackageName.ToString());

	checkf(RequestIDs.Num() == 0, TEXT("MarkRequestIDsAsComplete() has not been called for package %s"),
		*Desc.DiskPackageName.ToString());
	
	checkf(ConstructedObjects.Num() == 0, TEXT("ClearConstructedObjects() has not been called for package %s"),
		*Desc.DiskPackageName.ToString());

	GraphAllocator.FreeNodes(PackageNodes, EEventLoadNode2::Package_NumPhases + ExportBundleNodeCount);

	FMemory::Free(ExportBundlesMetaMemory);
}

void FAsyncPackage2::ReleaseRef()
{
	check(RefCount > 0);
	if (--RefCount == 0)
	{
		FAsyncLoadingThread2& AsyncLoadingThreadLocal = AsyncLoadingThread;
		AsyncLoadingThreadLocal.DeferredDeletePackages.Enqueue(this);
		AsyncLoadingThreadLocal.AltZenaphore.NotifyOne();
	}
}

void FAsyncPackage2::ClearImportedPackages()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ClearImportedPackages);
	// TODO: Use AsyncPackage pointer from global id, namelookup entry has already been removed
#if 0
	FPackageStore& GlobalPackageStore = AsyncLoadingThread.GlobalPackageStore;
	int32 ImportedPackageCount = 0;
	int32* Imports = GlobalPackageStore.GetPackageImportedPackages(PackageId, ImportedPackageCount);
	TArray<FAsyncPackage2*> TempImportedAsyncPackages;
	for (int32 LocalImportIndex = 0; LocalImportIndex < ImportedPackageCount; ++LocalImportIndex)
	{
		int32 EntryIndex = Imports[LocalImportIndex];
		FPackageStoreEntry& Entry = GlobalPackageStore.StoreEntries[EntryIndex];
		FAsyncPackage2* ImportedAsyncPackage = AsyncLoadingThread.FindAsyncPackage(Entry.Name);
		TempImportedAsyncPackages.Add(ImportedAsyncPackage);
		if (ImportedAsyncPackage)
		{
			// ImportedAsyncPackage->ReleaseRef();
		}
	}
	ensure(TempImportedAsyncPackages == ImportedAsyncPackages);
#else
	for (FAsyncPackage2* ImportedAsyncPackage : ImportedAsyncPackages)
	{
		ImportedAsyncPackage->ReleaseRef();
	}
	ImportedAsyncPackages.Empty();
#endif
}

void FAsyncPackage2::ClearConstructedObjects()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ClearConstructedObjects);

	for (UObject* Object : ConstructedObjects)
	{
		if (Object->HasAnyFlags(RF_WasLoaded))
		{
			// exports and the upackage itself are are handled below
			continue;
		}
		Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading | EInternalObjectFlags::Async);
	}
	ConstructedObjects.Empty();

	// the async flag of all GC'able public export objects in non-temp packages are handled by FGlobalImportStore::ClearAsyncFlags
	const bool bShouldClearAsyncFlagForPublicExports = GUObjectArray.IsDisregardForGC(LinkerRoot) || !Desc.IsTrackingPublicExports();

	for (FExportObject& Export : Exports)
	{
		if (Export.bFiltered | Export.bExportLoadFailed)
		{
			continue;
		}

		UObject* Object = Export.Object;
		check(Object);
		checkf(Object->HasAnyFlags(RF_WasLoaded), TEXT("%s"), *Object->GetFullName());
		checkf(Object->HasAnyInternalFlags(EInternalObjectFlags::Async), TEXT("%s"), *Object->GetFullName());
		if (bShouldClearAsyncFlagForPublicExports || !Object->HasAnyFlags(RF_Public))
		{
			Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading | EInternalObjectFlags::Async);
		}
		else
		{
			Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
		}
	}

	LinkerRoot->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading | EInternalObjectFlags::Async);
}

void FAsyncPackage2::AddRequestID(int32 Id)
{
	if (Id > 0)
	{
		if (Desc.RequestID == INDEX_NONE)
		{
			// For debug readability
			Desc.RequestID = Id;
		}
		RequestIDs.Add(Id);
		AsyncLoadingThread.AddPendingRequest(Id);
		TRACE_LOADTIME_ASYNC_PACKAGE_REQUEST_ASSOCIATION(this, Id);
	}
}

void FAsyncPackage2::MarkRequestIDsAsComplete()
{
	AsyncLoadingThread.RemovePendingRequests(RequestIDs);
	RequestIDs.Reset();
}

/**
 * @return Time load begun. This is NOT the time the load was requested in the case of other pending requests.
 */
double FAsyncPackage2::GetLoadStartTime() const
{
	return LoadStartTime;
}

#if WITH_EDITOR 
void FAsyncPackage2::GetLoadedAssets(TArray<FWeakObjectPtr>& AssetList)
{
}
#endif

/**
 * Begin async loading process. Simulates parts of BeginLoad.
 *
 * Objects created during BeginAsyncLoad and EndAsyncLoad will have EInternalObjectFlags::AsyncLoading set
 */
void FAsyncPackage2::BeginAsyncLoad()
{
	if (IsInGameThread())
	{
		AsyncLoadingThread.EnterAsyncLoadingTick();
	}

	// this won't do much during async loading except increase the load count which causes IsLoading to return true
	FUObjectSerializeContext* LoadContext = GetSerializeContext();
	BeginLoad(LoadContext);
}

/**
 * End async loading process. Simulates parts of EndLoad(). FinishObjects 
 * simulates some further parts once we're fully done loading the package.
 */
void FAsyncPackage2::EndAsyncLoad()
{
	check(IsAsyncLoading());

	// this won't do much during async loading except decrease the load count which causes IsLoading to return false
	FUObjectSerializeContext* LoadContext = GetSerializeContext();
	EndLoad(LoadContext);

	if (IsInGameThread())
	{
		AsyncLoadingThread.LeaveAsyncLoadingTick();
	}
}

void FAsyncPackage2::CreateUPackage(const FPackageSummary* PackageSummary)
{
	check(!LinkerRoot);

	// temp packages are never stored and never found
	FLoadedPackageRef* PackageRef = nullptr;

	// Try to find existing package or create it if not already present.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UPackageFind);
		if (Desc.IsTrackingPublicExports())
		{
			PackageRef = ImportStore.GlobalPackageStore.LoadedPackageStore.FindPackageRef(Desc.DiskPackageId);
			check(PackageRef);
			LinkerRoot = PackageRef->GetPackage();
			check(LinkerRoot == FindObjectFast<UPackage>(nullptr, Desc.GetUPackageName()));
		}
		else
		{
			LinkerRoot = FindObjectFast<UPackage>(nullptr, Desc.GetUPackageName());
		}
	}
	if (!LinkerRoot)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UPackageCreate);
		LinkerRoot = NewObject<UPackage>(/*Outer*/nullptr, Desc.GetUPackageName(), RF_Public | RF_WasLoaded);
		LinkerRoot->FileName = Desc.DiskPackageName;
		LinkerRoot->SetPackageId(Desc.DiskPackageId);
		LinkerRoot->SetPackageFlagsTo(PackageSummary->PackageFlags);
		LinkerRoot->LinkerPackageVersion = GPackageFileUE4Version;
		LinkerRoot->LinkerLicenseeVersion = GPackageFileLicenseeUE4Version;
		// LinkerRoot->LinkerCustomVersion = PackageSummaryVersions; // only if (!bCustomVersionIsLatest)
		if (PackageRef)
		{
			PackageRef->SetPackage(LinkerRoot);
		}
		bCreatedLinkerRoot = true;
	}
	else
	{
		check(LinkerRoot->GetPackageId() == Desc.DiskPackageId);
		check(LinkerRoot->GetPackageFlags() == PackageSummary->PackageFlags);
		check(LinkerRoot->LinkerPackageVersion == GPackageFileUE4Version);
		check(LinkerRoot->LinkerLicenseeVersion == GPackageFileLicenseeUE4Version);
		check(LinkerRoot->HasAnyFlags(RF_WasLoaded));
	}

	PinObjectForGC(LinkerRoot, bCreatedLinkerRoot);

	if (bCreatedLinkerRoot)
	{
		UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("CreateUPackage: AddPackage"),
			TEXT("New UPackage created."));
	}
	else
	{
		UE_ASYNC_PACKAGE_LOG_VERBOSE(VeryVerbose, Desc, TEXT("CreateUPackage: UpdatePackage"),
			TEXT("Existing UPackage updated."));
	}
}

EAsyncPackageState::Type FAsyncPackage2::ProcessExternalReads(EExternalReadAction Action)
{
	double WaitTime;
	if (Action == ExternalReadAction_Poll)
	{
		WaitTime = -1.f;
	}
	else// if (Action == ExternalReadAction_Wait)
	{
		WaitTime = 0.f;
	}

	while (ExternalReadIndex < ExternalReadDependencies.Num())
	{
		FExternalReadCallback& ReadCallback = ExternalReadDependencies[ExternalReadIndex];
		if (!ReadCallback(WaitTime))
		{
			return EAsyncPackageState::TimeOut;
		}
		++ExternalReadIndex;
	}

	ExternalReadDependencies.Empty();
	GetNode(Package_ExportsSerialized)->ReleaseBarrier();
	return EAsyncPackageState::Complete;
}

EAsyncPackageState::Type FAsyncPackage2::CreateClusters()
{
	while (DeferredClusterIndex < ExportCount &&
			!AsyncLoadingThread.IsAsyncLoadingSuspended() &&
			!FAsyncLoadingThreadState2::Get()->IsTimeLimitExceeded(TEXT("CreateClusters")))
	{
		const FExportObject& Export = Exports[DeferredClusterIndex++];

		if (!(Export.bFiltered | Export.bExportLoadFailed) && Export.Object->CanBeClusterRoot())
		{
			Export.Object->CreateCluster();
		}
	}

	return DeferredClusterIndex == ExportCount ? EAsyncPackageState::Complete : EAsyncPackageState::TimeOut;
}

EAsyncPackageState::Type FAsyncPackage2::FinishObjects()
{
	SCOPED_LOADTIMER(FinishObjectsTime);

	EAsyncLoadingResult::Type LoadingResult;
	if (!bLoadHasFailed)
	{
		LoadingResult = EAsyncLoadingResult::Succeeded;
	}
	else
	{		
		// Clean up UPackage so it can't be found later
		if (LinkerRoot && !LinkerRoot->IsRooted())
		{
			if (bCreatedLinkerRoot)
			{
				LinkerRoot->ClearFlags(RF_NeedPostLoad | RF_NeedLoad | RF_NeedPostLoadSubobjects);
				LinkerRoot->MarkPendingKill();
				LinkerRoot->Rename(*MakeUniqueObjectName(GetTransientPackage(), UPackage::StaticClass()).ToString(), nullptr, REN_DontCreateRedirectors | REN_DoNotDirty | REN_ForceNoResetLoaders | REN_NonTransactional);
			}
		}

		LoadingResult = EAsyncLoadingResult::Failed;
	}

	for (UObject* Object : ConstructedObjects)
	{
		if (!Object->HasAnyFlags(RF_NeedPostLoad | RF_NeedPostLoadSubobjects))
		{
			Object->AtomicallyClearInternalFlags(EInternalObjectFlags::AsyncLoading);
		}
	}

	return EAsyncPackageState::Complete;
}

void FAsyncPackage2::CallCompletionCallbacks(EAsyncLoadingResult::Type LoadingResult)
{
	checkSlow(!IsInAsyncLoadingThread());

	UPackage* LoadedPackage = (!bLoadHasFailed) ? LinkerRoot : nullptr;
	for (FCompletionCallback& CompletionCallback : CompletionCallbacks)
	{
		CompletionCallback->ExecuteIfBound(Desc.GetUPackageName(), LoadedPackage, LoadingResult);
	}
}

UPackage* FAsyncPackage2::GetLoadedPackage()
{
	UPackage* LoadedPackage = (!bLoadHasFailed) ? LinkerRoot : nullptr;
	return LoadedPackage;
}

void FAsyncPackage2::Cancel()
{
	// Call any completion callbacks specified.
	bLoadHasFailed = true;
	const EAsyncLoadingResult::Type Result = EAsyncLoadingResult::Canceled;
	CallCompletionCallbacks(Result);

	if (LinkerRoot)
	{
		if (bCreatedLinkerRoot)
		{
			LinkerRoot->ClearFlags(RF_WasLoaded);
			LinkerRoot->bHasBeenFullyLoaded = false;
			LinkerRoot->Rename(*MakeUniqueObjectName(GetTransientPackage(), UPackage::StaticClass()).ToString(), nullptr, REN_DontCreateRedirectors | REN_DoNotDirty | REN_ForceNoResetLoaders | REN_NonTransactional);
		}
	}
}

void FAsyncPackage2::AddCompletionCallback(TUniquePtr<FLoadPackageAsyncDelegate>&& Callback)
{
	// This is to ensure that there is no one trying to subscribe to a already loaded package
	//check(!bLoadHasFinished && !bLoadHasFailed);
	CompletionCallbacks.Emplace(MoveTemp(Callback));
}

void FAsyncPackage2::UpdateLoadPercentage()
{
	// PostLoadCount is just an estimate to prevent packages to go to 100% too quickly
	// We may never reach 100% this way, but it's better than spending most of the load package time at 100%
	float NewLoadPercentage = 0.0f;
	// It's also possible that we got so many objects to PostLoad that LoadPercantage will actually drop
	LoadPercentage = FMath::Max(NewLoadPercentage, LoadPercentage);
}

int32 FAsyncLoadingThread2::LoadPackage(const FString& InName, const FGuid* InGuid, const TCHAR* InPackageToLoadFrom, FLoadPackageAsyncDelegate InCompletionDelegate, EPackageFlags InPackageFlags, int32 InPIEInstanceID, int32 InPackagePriority, const FLinkerInstancingContext*)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LoadPackage);

	if (!bLazyInitializedFromLoadPackage)
	{
		bLazyInitializedFromLoadPackage = true;
		LazyInitializeFromLoadPackage();
	}

	int32 RequestID = INDEX_NONE;

	// happy path where all inputs are actual package names
	const FName Name = FName(*InName);
	FName DiskPackageName = InPackageToLoadFrom ? FName(InPackageToLoadFrom) : Name;
	bool bHasCustomPackageName = Name != DiskPackageName;

	// Verify PackageToLoadName, or fixup to handle any input string that can be converted to a long package name.
	FPackageId DiskPackageId = FPackageId::FromName(DiskPackageName);
	const FPackageStoreEntry* StoreEntry = GlobalPackageStore.FindStoreEntry(DiskPackageId);
	if (!StoreEntry)
	{
		FString PackageNameStr = DiskPackageName.ToString();
		if (!FPackageName::IsValidLongPackageName(PackageNameStr))
		{
			FString NewPackageNameStr;
			if (FPackageName::TryConvertFilenameToLongPackageName(PackageNameStr, NewPackageNameStr))
			{
				DiskPackageName = *NewPackageNameStr;
				DiskPackageId = FPackageId::FromName(DiskPackageName);
				StoreEntry = GlobalPackageStore.FindStoreEntry(DiskPackageId);
				bHasCustomPackageName &= Name != DiskPackageName;
			}
		}
	}

	// Verify CustomPackageName, or fixup to handle any input string that can be converted to a long package name.
	// CustomPackageName must not be an existing disk package name,
	// that could cause missing or incorrect import objects for other packages.
	FName CustomPackageName;
	FPackageId CustomPackageId;
	if (bHasCustomPackageName)
	{
		FPackageId PackageId = FPackageId::FromName(Name);
		if (!GlobalPackageStore.FindStoreEntry(PackageId))
		{
			FString PackageNameStr = Name.ToString();
			if (FPackageName::IsValidLongPackageName(PackageNameStr))
			{
				CustomPackageName = Name;
				CustomPackageId = PackageId;
			}
			else
			{
				FString NewPackageNameStr;
				if (FPackageName::TryConvertFilenameToLongPackageName(PackageNameStr, NewPackageNameStr))
				{
					PackageId = FPackageId::FromName(FName(*NewPackageNameStr));
					if (!GlobalPackageStore.FindStoreEntry(PackageId))
					{
						CustomPackageName = *NewPackageNameStr;
						CustomPackageId = PackageId;
					}
				}
			}
		}
	}
	check(CustomPackageId.IsValid() == !CustomPackageName.IsNone());

	bool bCustomNameIsValid = (!bHasCustomPackageName && CustomPackageName.IsNone()) || (bHasCustomPackageName && !CustomPackageName.IsNone());
	bool bDiskPackageIdIsValid = !!StoreEntry;
	if (!bDiskPackageIdIsValid)
	{
		// While there is an active load request for (InName=/Temp/PackageABC_abc, InPackageToLoadFrom=/Game/PackageABC), then allow these requests too:
		// (InName=/Temp/PackageA_abc, InPackageToLoadFrom=/Temp/PackageABC_abc) and (InName=/Temp/PackageABC_xyz, InPackageToLoadFrom=/Temp/PackageABC_abc)
		FAsyncPackage2* Package = GetAsyncPackage(DiskPackageId);
		if (Package)
		{
			if (CustomPackageName.IsNone())
			{
				CustomPackageName = Package->Desc.CustomPackageName;
				CustomPackageId = Package->Desc.CustomPackageId;
				bHasCustomPackageName = bCustomNameIsValid = true;
			}
			DiskPackageName = Package->Desc.DiskPackageName;
			DiskPackageId = Package->Desc.DiskPackageId;
			StoreEntry = Package->Desc.StoreEntry;
			bDiskPackageIdIsValid = true;
		}
	}

	if (bDiskPackageIdIsValid && bCustomNameIsValid)
	{
		if (FCoreDelegates::OnAsyncLoadPackage.IsBound())
		{
			FCoreDelegates::OnAsyncLoadPackage.Broadcast(InName);
		}

		// Generate new request ID and add it immediately to the global request list (it needs to be there before we exit
		// this function, otherwise it would be added when the packages are being processed on the async thread).
		RequestID = PackageRequestID.Increment();
		TRACE_LOADTIME_BEGIN_REQUEST(RequestID);
		AddPendingRequest(RequestID);

		// Allocate delegate on Game Thread, it is not safe to copy delegates by value on other threads
		TUniquePtr<FLoadPackageAsyncDelegate> CompletionDelegatePtr;
		if (InCompletionDelegate.IsBound())
		{
			CompletionDelegatePtr.Reset(new FLoadPackageAsyncDelegate(InCompletionDelegate));
		}

#if !UE_BUILD_SHIPPING
		if (FileOpenLogWrapper)
		{
			FileOpenLogWrapper->AddPackageToOpenLog(*DiskPackageName.ToString());
		}
#endif

		// Add new package request
		FAsyncPackageDesc2 PackageDesc(RequestID, DiskPackageId, StoreEntry, DiskPackageName, CustomPackageId, CustomPackageName, MoveTemp(CompletionDelegatePtr));
		QueuePackage(PackageDesc);

		UE_ASYNC_PACKAGE_LOG(Verbose, PackageDesc, TEXT("LoadPackage: QueuePackage"), TEXT("Package added to pending queue."));
	}
	else
	{
		FAsyncPackageDesc2 PackageDesc(RequestID, DiskPackageId, StoreEntry, DiskPackageName, CustomPackageId, CustomPackageName);
		if (!bDiskPackageIdIsValid)
		{
			UE_ASYNC_PACKAGE_LOG(Warning, PackageDesc, TEXT("LoadPackage: SkipPackage"),
				TEXT("The package to load does not exist on disk or in the loader"));
		}
		else // if (!bCustomNameIsValid)
		{
			UE_ASYNC_PACKAGE_LOG(Warning, PackageDesc, TEXT("LoadPackage: SkipPackage"), TEXT("The custom package name is invalid"));
		}

		if (InCompletionDelegate.IsBound())
		{
			// Queue completion callback and execute at next process loaded packages call to maintain behavior compatibility with old loader
			FQueuedFailedPackageCallback& QueuedFailedPackageCallback = QueuedFailedPackageCallbacks.AddDefaulted_GetRef();
			QueuedFailedPackageCallback.PackageName = Name;
			QueuedFailedPackageCallback.Callback.Reset(new FLoadPackageAsyncDelegate(InCompletionDelegate));
		}
	}

	return RequestID;
}

EAsyncPackageState::Type FAsyncLoadingThread2::ProcessLoadingFromGameThread(bool bUseTimeLimit, bool bUseFullTimeLimit, float TimeLimit)
{
	TickAsyncLoadingFromGameThread(bUseTimeLimit, bUseFullTimeLimit, TimeLimit);
	return IsAsyncLoading() ? EAsyncPackageState::TimeOut : EAsyncPackageState::Complete;
}

void FAsyncLoadingThread2::FlushLoading(int32 RequestId)
{
	if (IsAsyncLoading())
	{
		// Flushing async loading while loading is suspend will result in infinite stall
		UE_CLOG(bSuspendRequested, LogStreaming, Fatal, TEXT("Cannot Flush Async Loading while async loading is suspended"));

		if (RequestId != INDEX_NONE && !ContainsRequestID(RequestId))
		{
			return;
		}

		FCoreDelegates::OnAsyncLoadingFlush.Broadcast();

		double StartTime = FPlatformTime::Seconds();

		// Flush async loaders by not using a time limit. Needed for e.g. garbage collection.
		{
			while (IsAsyncLoading())
			{
				EAsyncPackageState::Type Result = TickAsyncLoadingFromGameThread(false, false, 0, RequestId);
				if (RequestId != INDEX_NONE && !ContainsRequestID(RequestId))
				{
					break;
				}

				if (IsMultithreaded())
				{
					// Update the heartbeat and sleep. If we're not multithreading, the heartbeat is updated after each package has been processed
					FThreadHeartBeat::Get().HeartBeat();
					FPlatformProcess::SleepNoStats(0.0001f);
				}

				// push stats so that we don't overflow number of tags per thread during blocking loading
				LLM_PUSH_STATS_FOR_ASSET_TAGS();
			}
		}

		double EndTime = FPlatformTime::Seconds();
		double ElapsedTime = EndTime - StartTime;

		check(RequestId != INDEX_NONE || !IsAsyncLoading());
	}
}

EAsyncPackageState::Type FAsyncLoadingThread2::ProcessLoadingUntilCompleteFromGameThread(TFunctionRef<bool()> CompletionPredicate, float TimeLimit)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ProcessLoadingUntilComplete);
	if (!IsAsyncLoading())
	{
		return EAsyncPackageState::Complete;
	}

	// Flushing async loading while loading is suspend will result in infinite stall
	UE_CLOG(bSuspendRequested, LogStreaming, Fatal, TEXT("Cannot Flush Async Loading while async loading is suspended"));

	if (TimeLimit <= 0.0f)
	{
		// Set to one hour if no time limit
		TimeLimit = 60 * 60;
	}

	while (IsAsyncLoading() && TimeLimit > 0 && !CompletionPredicate())
	{
		double TickStartTime = FPlatformTime::Seconds();
		if (ProcessLoadingFromGameThread(true, true, TimeLimit) == EAsyncPackageState::Complete)
		{
			return EAsyncPackageState::Complete;
		}

		if (IsMultithreaded())
		{
			// Update the heartbeat and sleep. If we're not multithreading, the heartbeat is updated after each package has been processed
			FThreadHeartBeat::Get().HeartBeat();
			FPlatformProcess::SleepNoStats(0.0001f);
		}

		TimeLimit -= (FPlatformTime::Seconds() - TickStartTime);
	}

	return TimeLimit <= 0 ? EAsyncPackageState::TimeOut : EAsyncPackageState::Complete;
}

IAsyncPackageLoader* MakeAsyncPackageLoader2(FIoDispatcher& InIoDispatcher)
{
	return new FAsyncLoadingThread2(InIoDispatcher);
}

#endif //WITH_ASYNCLOADING2

#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
PRAGMA_ENABLE_OPTIMIZATION
#endif
