// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "WorldPartition/WorldPartitionActorDescFactory.h"

class FWorldPartitionActorDesc;
class UHLODSubsystem;

class ENGINE_API FHLODActorDescFactory : public FWorldPartitionActorDescFactory
{
public:
#if WITH_EDITOR
	virtual FWorldPartitionActorDesc* CreateInstance(const FWorldPartitionActorDescInitData& ActorDescInitData) override;
	virtual FWorldPartitionActorDesc* CreateInstance(AActor* InActor) override;
#endif
};
