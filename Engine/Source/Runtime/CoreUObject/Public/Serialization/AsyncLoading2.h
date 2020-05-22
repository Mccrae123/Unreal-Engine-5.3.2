// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	AsyncLoading2.h: Unreal async loading #2 definitions.
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectResource.h"
#include "UObject/PackageId.h"
#include "Serialization/Archive.h"
#include "IO/IoContainerId.h"

class FArchive;
class IAsyncPackageLoader;
class FIoDispatcher;
class IEDLBootNotificationManager;

using FSourceToLocalizedPackageIdMap = TMap<FPackageId, FPackageId>;
using FCulturePackageMap = TMap<FString, FSourceToLocalizedPackageIdMap>;

class FMappedName
{
	static constexpr uint32 InvalidIndex = ~uint32(0);
	static constexpr uint32 IndexBits = 31u;
	static constexpr uint32 IndexMask = (1u << IndexBits) - 1u;
	static constexpr uint32 TypeMask = ~IndexMask;
	static constexpr uint32 TypeShift = IndexBits;

public:
	enum class EType { Container, Global };

	inline FMappedName() = default;

	static inline FMappedName Create(const uint32 InIndex, const uint32 InNumber, EType InType)
	{
		check(InIndex <= MAX_int32);
		return FMappedName((uint32(InType) << TypeShift) | InIndex, InNumber);
	}

	static inline FMappedName FromMinimalName(const FMinimalName& MinimalName)
	{
		return *reinterpret_cast<const FMappedName*>(&MinimalName);
	}

	static inline bool IsResolvedToMinimalName(const FMinimalName& MinimalName)
	{
		// Not completely safe, relies on that no FName will have its Index and Number equal to Max_uint32
		const FMappedName MappedName = FromMinimalName(MinimalName);
		return MappedName.IsValid();
	}

	static inline FName SafeMinimalNameToName(const FMinimalName& MinimalName)
	{
		return IsResolvedToMinimalName(MinimalName) ? MinimalNameToName(MinimalName) : NAME_None;
	}

	inline FMinimalName ToUnresolvedMinimalName() const
	{
		return *reinterpret_cast<const FMinimalName*>(this);
	}

	inline bool IsValid() const
	{
		return Index != InvalidIndex && Number != InvalidIndex;
	}

	inline EType GetType() const
	{
		return static_cast<EType>(uint32((Index & TypeMask) >> TypeShift));
	}

	inline bool IsGlobal() const
	{
		return ((Index & TypeMask) >> TypeShift) != 0;
	}

	inline uint32 GetIndex() const
	{
		return Index & IndexMask;
	}

	inline uint32 GetNumber() const
	{
		return Number;
	}

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FMappedName& MappedName);

private:
	inline FMappedName(const uint32 InIndex, const uint32 InNumber)
		: Index(InIndex)
		, Number(InNumber) { }

	uint32 Index = InvalidIndex;
	uint32 Number = InvalidIndex;
};

struct FContainerHeader
{
	FIoContainerId ContainerId;
	TArray<uint8> Names;
	TArray<uint8> NameHashes;
	TArray<FPackageId> PackageIds;
	TArray<FMappedName> PackageNames;

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FContainerHeader& ContainerHeader);
};

class FPackageObjectIndex
{
	static constexpr uint32 IndexBits = 30;
	static constexpr uint32 IndexMask = (1 << IndexBits) - 1;
	static constexpr uint32 TypeMask = ~IndexMask;
	static constexpr uint32 TypeShift = IndexBits;

	uint32 TypeAndIndex = Null << TypeShift;

public:
	enum Type
	{
		ScriptImport,
		PackageImport,
		ImportTypeCount,
		Export = ImportTypeCount,
		Null,
		TypeCount = Null,
	};
	static_assert((TypeCount - 1) <= (TypeMask >> TypeShift), "FPackageObjectIndex: Too many index types for TypeMask");

	FPackageObjectIndex() = default;
	inline explicit FPackageObjectIndex(Type InType, int32 InIndex) : TypeAndIndex((InType << TypeShift) | InIndex) {}

	inline bool IsNull() const
	{
		return (TypeAndIndex & TypeMask) == (Null << TypeShift);
	}

	inline bool IsExport() const
	{
		return (TypeAndIndex & TypeMask) == (Export << TypeShift);
	}

	inline bool IsImport() const
	{
		return IsScriptImport() || IsPackageImport();
	}

	inline bool IsScriptImport() const
	{
		return (TypeAndIndex & TypeMask) == (ScriptImport << TypeShift);
	}

	inline bool IsPackageImport() const
	{
		return (TypeAndIndex & TypeMask) == (PackageImport << TypeShift);
	}

	inline uint32 ToExport() const
	{
		check(IsExport());
		return TypeAndIndex & IndexMask;
	}

	inline uint32 ToScriptImport() const
	{
		check(IsScriptImport());
		return TypeAndIndex & IndexMask;
	}

	inline uint32 ToPackageImport() const
	{
		check(IsPackageImport());
		return TypeAndIndex & IndexMask;
	}

	inline Type GetType() const
	{
		return Type((TypeAndIndex & TypeMask) >> TypeShift);
	}

	inline int32 GetIndex() const
	{
		return TypeAndIndex & IndexMask;
	}

	inline bool operator==(FPackageObjectIndex Other) const
	{
		return TypeAndIndex == Other.TypeAndIndex;
	}

	inline bool operator!=(FPackageObjectIndex Other) const
	{
		return TypeAndIndex != Other.TypeAndIndex;
	}

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FPackageObjectIndex& Value)
	{
		Ar << Value.TypeAndIndex;
		return Ar;
	}

	inline friend uint32 GetTypeHash(const FPackageObjectIndex& Value)
	{
		return Value.TypeAndIndex;
	}
};

/**
 * Event node.
 */
enum EEventLoadNode2 : uint8
{
	Package_ProcessSummary,
	Package_ExportsSerialized,
	Package_PostLoad,
	Package_NumPhases,

	ExportBundle_Process = 0,
	ExportBundle_NumPhases,
};

/**
 * Export filter flags.
 */
enum class EExportFilterFlags : uint8
{
	None,
	NotForClient,
	NotForServer
};

/**
 * Package summary.
 */
struct FPackageSummary
{
	uint32 PackageFlags;
	uint32 CookedHeaderSize;
	uint16 NameMapIndex;
	uint16 Pad;
	int32 NameMapOffset;
	int32 ImportMapOffset;
	int32 ExportMapOffset;
	int32 ExportBundlesOffset;
	int32 GraphDataOffset;
	int32 GraphDataSize;
};

/**
 * Export bundle entry.
 */
struct FExportBundleEntry
{
	enum EExportCommandType
	{
		ExportCommandType_Create,
		ExportCommandType_Serialize
	};
	uint32 LocalExportIndex;
	uint32 CommandType;

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FExportBundleEntry& ExportBundleEntry);
};

template<typename T>
class TPackageStoreEntryCArrayView
{
	const uint32 ArrayNum = 0;
	const uint32 OffsetToDataFromThis = 0;

public:
	inline uint32 Num() const						{ return ArrayNum; }

	inline const T* Data() const					{ return (T*)((char*)this + OffsetToDataFromThis); }
	inline T* Data()								{ return (T*)((char*)this + OffsetToDataFromThis); }

	inline const T* begin() const					{ return Data(); }
	inline T* begin()								{ return Data(); }

	inline const T* end() const						{ return Data() + ArrayNum; }
	inline T* end()									{ return Data() + ArrayNum; }

	inline const T& operator[](uint32 Index) const	{ return Data()[Index]; }
	inline T& operator[](uint32 Index)				{ return Data()[Index]; }
};

struct FPackageStoreEntry
{
	uint64 ExportBundlesSize;
	FMinimalName Name;
	FPackageId SourcePackageId;
	int32 ExportCount;
	int32 ExportBundleCount;
	uint32 LoadOrder;
	TPackageStoreEntryCArrayView<FPackageId> ImportedPackages;
};

/**
 * Export bundle header
 */
struct FExportBundleHeader
{
	uint32 FirstEntryIndex;
	uint32 EntryCount;

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FExportBundleHeader& ExportBundleHeader);
};

struct FScriptObjectEntry
{
	FMinimalName ObjectName;
	FPackageObjectIndex OuterIndex;
	FPackageObjectIndex CDOClassIndex;

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FScriptObjectEntry& ScriptObjectEntry);
};

/**
 * Export map entry.
 */
struct FExportMapEntry
{
	uint64 CookedSerialOffset;
	uint64 CookedSerialSize;
	FMappedName ObjectName;
	FPackageObjectIndex OuterIndex;
	FPackageObjectIndex ClassIndex;
	FPackageObjectIndex SuperIndex;
	FPackageObjectIndex TemplateIndex;
	FPackageObjectIndex GlobalImportIndex;
	EObjectFlags ObjectFlags;
	EExportFilterFlags FilterFlags;
	uint8 Pad[7];

	COREUOBJECT_API friend FArchive& operator<<(FArchive& Ar, FExportMapEntry& ExportMapEntry);
};

#ifndef WITH_ASYNCLOADING2
#define WITH_ASYNCLOADING2 !WITH_EDITORONLY_DATA
#endif

#if WITH_ASYNCLOADING2

/**
 * Creates a new instance of the AsyncPackageLoader #2.
 *
 * @param InIoDispatcher				The I/O dispatcher.
 *
 * @return The async package loader.
 */
IAsyncPackageLoader* MakeAsyncPackageLoader2(FIoDispatcher& InIoDispatcher);

#endif
