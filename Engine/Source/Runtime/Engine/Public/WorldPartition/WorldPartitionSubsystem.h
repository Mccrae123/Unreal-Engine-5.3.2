// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "Templates/SubclassOf.h"
#include "Tickable.h"
#include "WorldPartitionSubsystem.generated.h"

class UWorldPartition;
class UWorldPartitionEditorCell;
class FWorldPartitionActorDescFactory;
class FWorldPartitionActorDesc;

/**
 * UWorldPartitionSubsystem
 */

UCLASS()
class ENGINE_API UWorldPartitionSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UWorldPartitionSubsystem();

	bool IsEnabled() const;

	//~ Begin USubsystem Interface.
	virtual void Deinitialize() override;
	//~ End USubsystem Interface.

	//~ Begin UWorldSubsystem Interface.
	virtual void PostInitialize() override;
	virtual void UpdateStreamingState() override;
	//~ End UWorldSubsystem Interface.

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaSeconds) override;
	virtual bool IsTickableInEditor() const override;
	virtual UWorld* GetTickableGameObjectWorld() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual TStatId GetStatId() const override;
	//~End FTickableGameObject

#if WITH_EDITOR
	TArray<const FWorldPartitionActorDesc*> GetIntersectingActorDescs(const FBox& Box, TSubclassOf<AActor> ActorClass) const;

	void UpdateActorDesc(AActor* Actor);

	void RegisterActorDescFactory(TSubclassOf<AActor> Class, FWorldPartitionActorDescFactory* Factory);

	FBox GetWorldBounds();
#endif

	void ToggleDrawRuntimeHash2D();

private:
	UWorldPartition* GetMainWorldPartition();
	const UWorldPartition* GetMainWorldPartition() const;
	void RegisterWorldPartition(UWorldPartition* WorldPartition);
	void UnregisterWorldPartition(UWorldPartition* WorldPartition);
	void DrawRuntimeHash2D(class UCanvas* Canvas, class APlayerController* PC);
	friend class UWorldPartition;

	UPROPERTY()
	TArray<UWorldPartition*> RegisteredWorldPartitions;

	FDelegateHandle	DrawRuntimeHash2DHandle;
};
