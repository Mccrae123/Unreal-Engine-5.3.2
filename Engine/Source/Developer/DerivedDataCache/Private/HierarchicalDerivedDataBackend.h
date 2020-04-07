// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBackendInterface.h"
#include "ProfilingDebugging/CookStats.h"
#include "DerivedDataCacheUsageStats.h"
#include "DerivedDataBackendAsyncPutWrapper.h"
#include "Templates/UniquePtr.h"

PRAGMA_DISABLE_OPTIMIZATION

/** 
 * A backend wrapper that implements a cache hierarchy of backends. 
**/
class FHierarchicalDerivedDataBackend : public FDerivedDataBackendInterface
{
public:

	/**
	 * Constructor
	 * @param	InInnerBackends Backends to call into for actual storage of the cache, first item is the "fastest cache"
	 */
	FHierarchicalDerivedDataBackend(const TArray<FDerivedDataBackendInterface*>& InInnerBackends)
		: InnerBackends(InInnerBackends)
		, bIsWritable(false)
	{
		check(InnerBackends.Num() > 1); // if it is just one, then you don't need this wrapper
		UpdateAsyncInnerBackends();
	}

	/** Return a name for this interface */
	virtual FString GetName() const override
	{
		return TEXT("HierarchicalDerivedDataBackend");
	}

	/** Are we a remote cache? */
	virtual ESpeedClass GetSpeedClass() override
	{
		return ESpeedClass::Local;
	}

	void UpdateAsyncInnerBackends()
	{
		bIsWritable = false;
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			if (InnerBackends[CacheIndex]->IsWritable())
			{
				bIsWritable = true;
			}
		}
		if (bIsWritable)
		{
			for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
			{
				// async puts to allow us to fill all levels without holding up the engine
				AsyncPutInnerBackends.Emplace(new FDerivedDataBackendAsyncPutWrapper(InnerBackends[CacheIndex], false));
			}
		}
	}

	/** Adds inner backend. */
	void AddInnerBackend(FDerivedDataBackendInterface* InInner) 
	{
		InnerBackends.Add(InInner);
		AsyncPutInnerBackends.Empty();
		UpdateAsyncInnerBackends();
	}

	/** Removes inner backend. */
	bool RemoveInnerBackend(FDerivedDataBackendInterface* InInner) 
	{
		int32 NumRemoved = InnerBackends.Remove(InInner);
		AsyncPutInnerBackends.Empty();
		UpdateAsyncInnerBackends();
		return NumRemoved != 0;
	}

	/** return true if this cache is writable **/
	virtual bool IsWritable() override
	{
		return bIsWritable;
	}

	/**
	 * Synchronous test for the existence of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @return				true if the data probably will be found, this can't be guaranteed because of concurrency in the backends, corruption, etc
	 */
	virtual bool CachedDataProbablyExists(const TCHAR* CacheKey) override
	{
		COOK_STAT(auto Timer = UsageStats.TimeProbablyExists());
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			if (InnerBackends[CacheIndex]->CachedDataProbablyExists(CacheKey))
			{
				COOK_STAT(Timer.AddHit(0));
				return true;
			}
		}
		return false;
	}

	/**
	 * Attempts to make sure the cached data will be available as optimally as possible. This is left up to the implementation to do
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @return				true if any steps were performed to optimize future retrieval
	 */
	virtual bool TryToPrefetch(const TCHAR* CacheKey) override
	{
		COOK_STAT(auto Timer = UsageStats.TimePrefetch());

		// Search all backends for this key. If it can be moved into a faster class then we'll do so.
		bool WorthFetching = false;

		FDerivedDataBackendInterface* LastMissedInterface = nullptr;

		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			FDerivedDataBackendInterface* Interface = InnerBackends[CacheIndex];

			if (!Interface->CachedDataProbablyExists(CacheKey) && Interface->IsWritable())
			{
				LastMissedInterface = Interface;
			}
			else
			{
				// if we have an interface that's writable and faster, lets get it
				if (LastMissedInterface && LastMissedInterface->GetSpeedClass() > Interface->GetSpeedClass())
				{
					WorthFetching = true;
				}
			}
		}
		
		// If it's remote then fetch it. We don't care about the data but we 
		// Need to read a copy from the remote store anyway to fill the caches
		if (WorthFetching)
		{
			TArray<uint8> DontCare;
			GetCachedData(CacheKey, DontCare);
			COOK_STAT(Timer.AddHit(0));
		}			

		// Return true if we did anything
		return WorthFetching;
	}

	/*
		Determine if we would cache this by asking all our inner layers
	*/
	virtual bool WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData) override
	{
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			if (InnerBackends[CacheIndex]->WouldCache(CacheKey, InData))
			{
				return true;
			}
		}

		return false;
	}

	bool ApplyDebugOptions(FBackendDebugOptions& InOptions) override
	{
		bool bSuccess = true;
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			if (!InnerBackends[CacheIndex]->ApplyDebugOptions(InOptions))
			{
				bSuccess = false;
			}
		}

		return bSuccess;
	}



	/**
	 * Synchronous retrieve of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	OutData		Buffer to receive the results, if any were found
	 * @return				true if any data was found, and in this case OutData is non-empty
	 */
	virtual bool GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData) override
	{
		COOK_STAT(auto Timer = UsageStats.TimeGet());
		for (int32 CacheIndex = 0; CacheIndex < InnerBackends.Num(); CacheIndex++)
		{
			FDerivedDataBackendInterface* GetInterface = InnerBackends[CacheIndex];

			if (GetInterface->CachedDataProbablyExists(CacheKey) && GetInterface->GetCachedData(CacheKey, OutData))
			{
				if (bIsWritable)
				{
					// fill in the higher level caches
					for (int32 PutCacheIndex = CacheIndex - 1; PutCacheIndex >= 0; PutCacheIndex--)
					{
						FDerivedDataBackendInterface* PutBackend = InnerBackends[PutCacheIndex];

						if (PutBackend->IsWritable() && PutBackend->WouldCache(CacheKey, OutData))
						{
							if (PutBackend->BackfillLowerCacheLevels() &&
								PutBackend->CachedDataProbablyExists(CacheKey))
							{
								PutBackend->RemoveCachedData(CacheKey, /*bTransient=*/ false); // it apparently failed, so lets delete what is there
								AsyncPutInnerBackends[PutCacheIndex]->PutCachedData(CacheKey, OutData, true); // we force a put here because it must have failed
								UE_LOG(LogDerivedDataCache, Verbose, TEXT("Forward-filling cache %s with: %s (%d bytes) (force=%d)"), *PutBackend->GetName(), CacheKey, OutData.Num(), true);
							}
							else
							{
								UE_LOG(LogDerivedDataCache, Verbose, TEXT("Forward-filling cache %s with: %s (%d bytes) (force=%d)"), *PutBackend->GetName(), CacheKey, OutData.Num(), false);
								PutBackend->PutCachedData(CacheKey, OutData, false);
							}
						}
					}

					if (InnerBackends[CacheIndex]->BackfillLowerCacheLevels())
					{
						// fill in the lower level caches
						for (int32 PutCacheIndex = CacheIndex + 1; PutCacheIndex < AsyncPutInnerBackends.Num(); PutCacheIndex++)
						{
							FDerivedDataBackendInterface* PutBackend = InnerBackends[PutCacheIndex];

							if (!PutBackend->IsWritable())
							{
								if (!PutBackend->BackfillLowerCacheLevels() && PutBackend->CachedDataProbablyExists(CacheKey))
								{
									break; //do not write things that are already in the read only pak file
								}
							}
							else if (PutBackend->GetSpeedClass() >= FDerivedDataBackendInterface::ESpeedClass::Fast && PutBackend->WouldCache(CacheKey, OutData))
							{								
								AsyncPutInnerBackends[PutCacheIndex]->PutCachedData(CacheKey, OutData, false); // we do not need to force a put here
								UE_LOG(LogDerivedDataCache, Verbose, TEXT("Back-filling cache %s with: %s (%d bytes) (force=%d)"), *PutBackend->GetName(), CacheKey, OutData.Num(), false);
							}
						}
					}
				}
				COOK_STAT(Timer.AddHit(OutData.Num()));
				return true;
			}
		}
		return false;
	}
	/**
	 * Asynchronous, fire-and-forget placement of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	InData		Buffer containing the data to cache, can be destroyed after the call returns, immediately
	 * @param	bPutEvenIfExists	If true, then do not attempt skip the put even if CachedDataProbablyExists returns true
	 */
	virtual void PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists) override
	{
		COOK_STAT(auto Timer = UsageStats.TimePut());
		if (!bIsWritable)
		{
			return; // no point in continuing down the chain
		}
		bool bSynchronousPutPeformed = false;  // we must do at least one synchronous put to a writable cache before we return
		for (int32 PutCacheIndex = 0; PutCacheIndex < InnerBackends.Num(); PutCacheIndex++)
		{
			if (!InnerBackends[PutCacheIndex]->IsWritable() && !InnerBackends[PutCacheIndex]->BackfillLowerCacheLevels() && InnerBackends[PutCacheIndex]->CachedDataProbablyExists(CacheKey))
			{
				break; //do not write things that are already in the read only pak file
			}
			if (InnerBackends[PutCacheIndex]->IsWritable())
			{
				COOK_STAT(Timer.AddHit(InData.Num()));
				if (!bSynchronousPutPeformed)
				{
					InnerBackends[PutCacheIndex]->PutCachedData(CacheKey, InData, bPutEvenIfExists);
					bSynchronousPutPeformed = true;
				}
				else
				{
					AsyncPutInnerBackends[PutCacheIndex]->PutCachedData(CacheKey, InData, bPutEvenIfExists);
				}
			}
		}
	}

	virtual void RemoveCachedData(const TCHAR* CacheKey, bool bTransient) override
	{
		if (!bIsWritable)
		{
			return; // no point in continuing down the chain
		}
		for (int32 PutCacheIndex = 0; PutCacheIndex < InnerBackends.Num(); PutCacheIndex++)
		{
			InnerBackends[PutCacheIndex]->RemoveCachedData(CacheKey, bTransient);
		}
	}

	virtual void GatherUsageStats(TMap<FString, FDerivedDataCacheUsageStats>& UsageStatsMap, FString&& GraphPath) override
	{
		COOK_STAT(
		{
			UsageStatsMap.Add(GraphPath + TEXT(": Hierarchical"), UsageStats);
			// All the inner backends are actually wrapped by AsyncPut backends in writable cases (most cases in practice)
			if (AsyncPutInnerBackends.Num() > 0)
			{
				int Ndx = 0;
				for (const auto& InnerBackend : AsyncPutInnerBackends)
				{
					InnerBackend->GatherUsageStats(UsageStatsMap, GraphPath + FString::Printf(TEXT(".%2d"), Ndx++));
				}
			}
			else
			{
				int Ndx = 0;
				for (auto InnerBackend : InnerBackends)
				{
					InnerBackend->GatherUsageStats(UsageStatsMap, GraphPath + FString::Printf(TEXT(".%2d"), Ndx++));
				}
			}
		});
	}



private:
	FDerivedDataCacheUsageStats UsageStats;

	/** Array of backends forming the hierarchical cache...the first element is the fastest cache. **/
	TArray<FDerivedDataBackendInterface*> InnerBackends;
	/** Each of the backends wrapped with an async put **/
	TArray<TUniquePtr<FDerivedDataBackendInterface> > AsyncPutInnerBackends;
	/** As an optimization, we check our writable status at contruction **/
	bool bIsWritable;
};

PRAGMA_ENABLE_OPTIMIZATION