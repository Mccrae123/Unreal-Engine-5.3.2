// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "ClothingSimulationFactoryNv.h"

#if WITH_NVCLOTH
#include "ClothPhysicalMeshData.h"  // For EWeightMapTargetCommon
#include "ClothingSimulationNv.h"
#include "ClothingSimulationInteractorNv.h"
#endif

IClothingSimulation* UClothingSimulationFactoryNv::CreateSimulation()
{
#if WITH_NVCLOTH
	return new FClothingSimulationNv();
#else
	return nullptr;
#endif
}

void UClothingSimulationFactoryNv::DestroySimulation(IClothingSimulation* InSimulation)
{
#if WITH_NVCLOTH
	delete InSimulation;
#endif
}

bool UClothingSimulationFactoryNv::SupportsAsset(UClothingAssetBase* InAsset)
{
#if WITH_NVCLOTH
	return true;
#else
	return false;
#endif
}

bool UClothingSimulationFactoryNv::SupportsRuntimeInteraction()
{
	return true;
}

UClothingSimulationInteractor* UClothingSimulationFactoryNv::CreateInteractor()
{
#if WITH_NVCLOTH
	return NewObject<UClothingSimulationInteractorNv>(GetTransientPackage());
#else
	return nullptr;
#endif
}

TSubclassOf<UClothConfigBase> UClothingSimulationFactoryNv::GetClothConfigClass() const
{
#if WITH_NVCLOTH
	return TSubclassOf<UClothConfigBase>(UClothConfigNv::StaticClass());
#else
	return nullptr;
#endif
}

const UEnum* UClothingSimulationFactoryNv::GetWeightMapTargetEnum() const
{
#if WITH_NVCLOTH
	return StaticEnum<EWeightMapTargetCommon>();
#else
	return nullptr;
#endif
}
