// Copyright Epic Games, Inc. All Rights Reserved.

#include "CookOnTheFlyMessages.h"

#if WITH_COTF

namespace UE { namespace ZenCookOnTheFly { namespace Messaging
{

FArchive& operator<<(FArchive& Ar, FCompletedPackages& CompletedPackages)
{
	Ar << CompletedPackages.CookedPackages;
	Ar << CompletedPackages.FailedPackages;
	
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FCookPackageRequest& Request)
{
	Ar << Request.PackageId;

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FCookPackageResponse& Response)
{
	Ar << Response.Status;
	if (Response.Status == EPackageStoreEntryStatus::Ok)
	{
		Ar << Response.CookedEntry;
	}

	return Ar;
}

}}} // namesapce UE::ZenCookOnTheFly::Messaging

#endif // WITH_COTF
