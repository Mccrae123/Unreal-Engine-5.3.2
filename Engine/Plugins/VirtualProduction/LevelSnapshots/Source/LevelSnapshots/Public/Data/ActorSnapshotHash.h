// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/SecureHash.h"
#include "ActorSnapshotHash.generated.h"

USTRUCT()
struct LEVELSNAPSHOTS_API FActorSnapshotHash
{
	GENERATED_BODY()

	/**
	 * How many micro seconds it took to compute the actor CRC32 during saving. Used when loading actors.
	 * If the hash time is excessively high, it is more efficient simply to load the actor. Configured in project settings.
	 */
	UPROPERTY()
	double MicroSecondsForCrc;

	/** How many micro seconds it took to compute the MD5 hash. */
	UPROPERTY()
	double MicroSecondsForMD5;
	
	/** How many bytes of data were in the data we used for computing hash. Used to avoid computing hash. */
	UPROPERTY()
	int32 Crc32DataLength;

	/** Crc32 hash of actor when it was snapshot. Used to check for changes without loading actor. */
	UPROPERTY()
	uint32 Crc32;

	FMD5Hash MD5;

	bool Serialize(FArchive& Archive)
	{
		Archive << MicroSecondsForCrc;
		Archive << MicroSecondsForMD5;
		Archive << Crc32DataLength;
		Archive << Crc32;
		Archive << MD5;
		return true;
	}
};

template<>
struct TStructOpsTypeTraits<FActorSnapshotHash> : public TStructOpsTypeTraitsBase2<FActorSnapshotHash>
{
	enum 
	{ 
		WithSerialize = true
	};
};