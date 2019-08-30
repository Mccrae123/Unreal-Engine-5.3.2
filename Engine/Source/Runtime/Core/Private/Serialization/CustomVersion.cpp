// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	CustomVersion.cpp: Unreal custom versioning system.
=============================================================================*/

#include "Serialization/CustomVersion.h"
#include "Serialization/StructuredArchiveFromArchive.h"
#include "Algo/Sort.h"
#include "Containers/Map.h"

namespace
{
	static const FGuid UnusedCustomVersionKey(0, 0, 0, 0xF99D40C1);

	static const FCustomVersion& GetUnusedCustomVersion()
	{
		static const FCustomVersion UnusedCustomVersion(UnusedCustomVersionKey, 0, TEXT("Unused custom version"));
		return UnusedCustomVersion;
	}
	
	struct FEnumCustomVersion_DEPRECATED
	{
		uint32 Tag;
		int32  Version;

		FCustomVersion ToCustomVersion() const
		{
			// We'll invent a GUID from three zeroes and the original tag
			return FCustomVersion(FGuid(0, 0, 0, Tag), Version, *FString::Printf(TEXT("EnumTag%u"), Tag));
		}
	};

	void operator<<(FStructuredArchive::FSlot Slot, FEnumCustomVersion_DEPRECATED& Version)
	{
		// Serialize keys
		FStructuredArchive::FRecord Record = Slot.EnterRecord();
		Record << NAMED_ITEM("Tag", Version.Tag);
		Record << NAMED_ITEM("Version", Version.Version);
	}

	FArchive& operator<<(FArchive& Ar, FEnumCustomVersion_DEPRECATED& Version)
	{
		FStructuredArchiveFromArchive(Ar).GetSlot() << Version;
		return Ar;
	}

	struct FGuidCustomVersion_DEPRECATED
	{
		FGuid Key;
		int32 Version;
		FString FriendlyName;

		FCustomVersion ToCustomVersion() const
		{
			// We'll invent a GUID from three zeroes and the original tag
			return FCustomVersion(Key, Version, *FriendlyName);
		}
	};

	void operator<<(FStructuredArchive::FSlot Slot, FGuidCustomVersion_DEPRECATED& Version)
	{
		FStructuredArchive::FRecord Record = Slot.EnterRecord();
		Record << NAMED_ITEM("Key", Version.Key);
		Record << NAMED_ITEM("Version", Version.Version);
		Record << NAMED_ITEM("FriendlyName", Version.FriendlyName);
	}

	FArchive& operator<<(FArchive& Ar, FGuidCustomVersion_DEPRECATED& Version)
	{
		FStructuredArchiveFromArchive(Ar).GetSlot() << Version;
		return Ar;
	}
}

/** Defer FName creation and allocations from static FCustomVersionRegistrations that may never be needed */
class FStaticCustomVersionRegistry
{
public:
	static const FCustomVersionContainer& Get()
	{
		FStaticCustomVersionRegistry& StaticVersions = GetInstance();
		StaticVersions.RegisterQueue();
		return StaticVersions.Registered;
	}

	static void Register(FGuid Key, int32 Version, const TCHAR* Name)
	{
		RegistrationQueue& Queue = GetInstance().Queue;
		check(Queue.Find(Key) == nullptr);
		Queue.Add(Key, { Version, Name });
	}

	static void Unregister(FGuid Key)
	{
		 GetInstance().UnregisterImpl(Key);
	}

private:
	struct FPendingRegistration
	{
		int32 Version;
		const TCHAR* FriendlyName;
	};
	typedef TMap<FGuid, FPendingRegistration, TInlineSetAllocator<64>> RegistrationQueue;

	FCustomVersionContainer Registered;
	RegistrationQueue Queue;

	static FStaticCustomVersionRegistry& GetInstance()
	{
		static FStaticCustomVersionRegistry Singleton;
		return Singleton;
	};

	void RegisterQueue()
	{
		if (Queue.Num())
		{
			for (const TPair<FGuid, FPendingRegistration>& Queued : Queue)
			{
				// Check if this tag hasn't already been registered
				if (FCustomVersion* ExistingRegistration = Registered.Versions.FindByKey(Queued.Key))
				{
					// We don't allow the registration details to change across registrations - this code path only exists to support hotreload

					// If you hit this then you've probably either:
					// * Changed registration details during hotreload.
					// * Accidentally copy-and-pasted an FCustomVersionRegistration object.
					ensureMsgf(
						ExistingRegistration->Version == Queued.Value.Version && ExistingRegistration->GetFriendlyName() == Queued.Value.FriendlyName,
						TEXT("Custom version registrations cannot change between hotreloads - \"%s\" version %d is being reregistered as \"%s\" version %d"),
						*ExistingRegistration->GetFriendlyName().ToString(),
						ExistingRegistration->Version,
						Queued.Value.FriendlyName,
						Queued.Value.Version
					);

					++ExistingRegistration->ReferenceCount;
				}
				else
				{
					Registered.Versions.Add(FCustomVersion(Queued.Key, Queued.Value.Version, Queued.Value.FriendlyName));
				}
			}

			Queue.Empty();
		}
	}

	void UnregisterImpl(FGuid Key)
	{
		if (Queue.Remove(Key) == 0)
		{
			const int32 KeyIndex = Registered.Versions.IndexOfByKey(Key);

			// Ensure this tag has been registered
			check(KeyIndex != INDEX_NONE);

			FCustomVersion* FoundKey = &Registered.Versions[KeyIndex];

			--FoundKey->ReferenceCount;
			if (FoundKey->ReferenceCount == 0)
			{
				Registered.Versions.RemoveAtSwap(KeyIndex);
			}
		}
	}
};

const FName FCustomVersion::GetFriendlyName() const
{
	if (FriendlyName == NAME_None)
	{
		FriendlyName = FCustomVersionContainer::GetRegistered().GetFriendlyName(Key);
	}
	return FriendlyName;
}

const FCustomVersionContainer& FCustomVersionContainer::GetRegistered()
{
	return FStaticCustomVersionRegistry::Get();
}

void FCustomVersionContainer::Empty()
{
	Versions.Empty();
}

void FCustomVersionContainer::SortByKey()
{
	Algo::SortBy(Versions, &FCustomVersion::Key);
}

FString FCustomVersionContainer::ToString(const FString& Indent) const
{
	FString VersionsAsString;
	for (const FCustomVersion& SomeVersion : Versions)
	{
		VersionsAsString += Indent;
		VersionsAsString += FString::Printf(TEXT("Key=%s  Version=%d  Friendly Name=%s \n"), *SomeVersion.Key.ToString(), SomeVersion.Version, *SomeVersion.GetFriendlyName().ToString() );
	}

	return VersionsAsString;
}

FArchive& operator<<(FArchive& Ar, FCustomVersion& Version)
{
	FStructuredArchiveFromArchive(Ar).GetSlot() << Version;
	return Ar;
}

void operator<<(FStructuredArchive::FSlot Slot, FCustomVersion& Version)
{
	FStructuredArchive::FRecord Record = Slot.EnterRecord();
	Record << NAMED_ITEM("Key", Version.Key);
	Record << NAMED_ITEM("Version", Version.Version);
}

void FCustomVersionContainer::Serialize(FArchive& Ar, ECustomVersionSerializationFormat::Type Format)
{
	Serialize(FStructuredArchiveFromArchive(Ar).GetSlot(), Format);
}

void FCustomVersionContainer::Serialize(FStructuredArchive::FSlot Slot, ECustomVersionSerializationFormat::Type Format)
{
	switch (Format)
	{
	default: check(false);

	case ECustomVersionSerializationFormat::Enums:
	{
		// We should only ever be loading enums.  They should never be saved - they only exist for backward compatibility.
		check(Slot.GetUnderlyingArchive().IsLoading());

		TArray<FEnumCustomVersion_DEPRECATED> OldTags;
		Slot << OldTags;

		Versions.Empty(OldTags.Num());
		for (auto It = OldTags.CreateConstIterator(); It; ++It)
		{
			Versions.Add(It->ToCustomVersion());
		}
	}
	break;

	case ECustomVersionSerializationFormat::Guids:
	{
		// We should only ever be loading old versions.  They should never be saved - they only exist for backward compatibility.
		check(Slot.GetUnderlyingArchive().IsLoading());

		TArray<FGuidCustomVersion_DEPRECATED> VersionArray;
		Slot << VersionArray;
		Versions.Empty(VersionArray.Num());
		for (FGuidCustomVersion_DEPRECATED& OldVer : VersionArray)
		{
			Versions.Add(OldVer.ToCustomVersion());
		}
	}
	break;

	case ECustomVersionSerializationFormat::Optimized:
	{
		Slot << Versions;
	}
	break;
	}
}

const FCustomVersion* FCustomVersionContainer::GetVersion(FGuid Key) const
{
	// A testing tag was written out to a few archives during testing so we need to
	// handle the existence of it to ensure that those archives can still be loaded.
	if (Key == UnusedCustomVersionKey)
	{
		return &GetUnusedCustomVersion();
	}

	return Versions.FindByKey(Key);
}

const FName FCustomVersionContainer::GetFriendlyName(FGuid Key) const
{
	FName FriendlyName = NAME_Name;
	const FCustomVersion* CustomVersion = GetVersion(Key);
	if (CustomVersion)
	{
		FriendlyName = CustomVersion->FriendlyName;
	}
	return FriendlyName;
}

void FCustomVersionContainer::SetVersion(FGuid CustomKey, int32 Version, FName FriendlyName)
{
	if (CustomKey == UnusedCustomVersionKey)
	{
		return;
	}

	if (FCustomVersion* Found = Versions.FindByKey(CustomKey))
	{
		Found->Version      = Version;
		Found->FriendlyName = FriendlyName;
	}
	else
	{
		Versions.Add(FCustomVersion(CustomKey, Version, FriendlyName));
	}
}

void FCustomVersionRegistration::QueueRegistration(FGuid Key, int32 Version, const TCHAR* Name)
{
	FStaticCustomVersionRegistry::Register(Key, Version, Name);
}

FCustomVersionRegistration::~FCustomVersionRegistration()
{
	FStaticCustomVersionRegistry::Unregister(Key);
}
