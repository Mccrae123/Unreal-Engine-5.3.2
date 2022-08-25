// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGGraphCache.h"
#include "PCGComponent.h"
#include "PCGSettings.h"

#include "Misc/ScopeRWLock.h"
#include "Algo/AnyOf.h"
#include "GameFramework/Actor.h"

namespace PCGGraphCacheConstants
{
	constexpr int32 NullComponentSeed = 0;
	constexpr int32 NullSettingsCrc32 = 0;
}

FPCGGraphCacheEntry::FPCGGraphCacheEntry(const FPCGDataCollection& InInput, const UPCGSettings* InSettings, const UPCGComponent* InComponent, const FPCGDataCollection& InOutput, TWeakObjectPtr<UObject> InOwner, FPCGRootSet& OutRootSet)
	: Input(InInput)
	, Output(InOutput)
{
	// Note: we don't need to root the settings since they'll be owned by the subsystem
	Settings = InSettings ? Cast<UPCGSettings>(StaticDuplicateObject(InSettings, InOwner.Get())) : nullptr;
	SettingsCrc32 = InSettings ? InSettings->GetCrc32() : PCGGraphCacheConstants::NullSettingsCrc32;
	ComponentSeed = InComponent ? InComponent->Seed : PCGGraphCacheConstants::NullComponentSeed;

	Input.AddToRootSet(OutRootSet);
	Output.AddToRootSet(OutRootSet);
}

bool FPCGGraphCacheEntry::Matches(const FPCGDataCollection& InInput, int32 InSettingsCrc32, int32 InComponentSeed) const
{
	return (SettingsCrc32 == InSettingsCrc32) && (Input == InInput) && (ComponentSeed == InComponentSeed);
}

FPCGGraphCache::FPCGGraphCache(TWeakObjectPtr<UObject> InOwner)
	: Owner(InOwner)
{
}

FPCGGraphCache::~FPCGGraphCache()
{
	ClearCache();
}

bool FPCGGraphCache::GetFromCache(const IPCGElement* InElement, const FPCGDataCollection& InInput, const UPCGSettings* InSettings, const UPCGComponent* InComponent, FPCGDataCollection& OutOutput) const
{
	if (!Owner.IsValid())
	{
		return false;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGGraphCache::GetFromCache);
	FReadScopeLock ScopedReadLock(CacheLock);

	if (const FPCGGraphCacheEntries* Entries = CacheData.Find(InElement))
	{
		int32 InSettingsCrc32 = (InSettings ? InSettings->GetCrc32() : PCGGraphCacheConstants::NullSettingsCrc32);
		int32 InComponentSeed = (InComponent ? InComponent->Seed : PCGGraphCacheConstants::NullComponentSeed);

		for (const FPCGGraphCacheEntry& Entry : *Entries)
		{
			if (Entry.Matches(InInput, InSettingsCrc32, InComponentSeed))
			{
				OutOutput = Entry.Output;
				return true;
			}
		}

		return false;
	}

	return false;
}

void FPCGGraphCache::StoreInCache(const IPCGElement* InElement, const FPCGDataCollection& InInput, const UPCGSettings* InSettings, const UPCGComponent* InComponent, const FPCGDataCollection& InOutput)
{
	if (!Owner.IsValid())
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGGraphCache::StoreInCache);
	FWriteScopeLock ScopedWriteLock(CacheLock);

	FPCGGraphCacheEntries* Entries = CacheData.Find(InElement);
	if(!Entries)
	{
		Entries = &(CacheData.Add(InElement));
	}

	Entries->Emplace(InInput, InSettings, InComponent, InOutput, Owner, RootSet);
}

void FPCGGraphCache::ClearCache()
{
	FWriteScopeLock ScopedWriteLock(CacheLock);

	// Remove all entries
	CacheData.Reset();

	// Unroot all previously rooted data
	RootSet.Clear();
}

#if WITH_EDITOR
void FPCGGraphCache::CleanFromCache(const IPCGElement* InElement)
{
	if (!InElement)
	{
		return;
	}

	FWriteScopeLock ScopeWriteLock(CacheLock);
	FPCGGraphCacheEntries* Entries = CacheData.Find(InElement);
	if (Entries)
	{
		for (FPCGGraphCacheEntry& Entry : *Entries)
		{
			Entry.Input.RemoveFromRootSet(RootSet);
			Entry.Output.RemoveFromRootSet(RootSet);

			if (Entry.Settings)
			{
				RootSet.Remove(Entry.Settings);
			}
		}
	}

	// Finally, remove all entries matching that element
	CacheData.Remove(InElement);
}
#endif // WITH_EDITOR