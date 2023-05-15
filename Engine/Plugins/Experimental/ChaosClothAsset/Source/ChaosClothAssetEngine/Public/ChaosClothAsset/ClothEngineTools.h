// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SharedPointer.h"

struct FManagedArrayCollection;
class FName;

namespace UE::Chaos::ClothAsset
{
	/**
	 *  Tools operating on cloth collections with Engine dependency
	 */
	struct CHAOSCLOTHASSETENGINE_API FClothEngineTools
	{
		/** Generate tether data. */
		static void GenerateTethers(const TSharedPtr<FManagedArrayCollection>& ClothCollection, const FName& WeightMap, const bool bGeodesicTethers);
	};
}  // End namespace UE::Chaos::ClothAsset