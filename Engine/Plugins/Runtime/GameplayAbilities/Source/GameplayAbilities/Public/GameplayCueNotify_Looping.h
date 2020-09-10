// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Instanced.h"
#include "GameplayCueNotifyTypes.h"
#include "GameplayCueNotify_Looping.generated.h"


/**
 * AGameplayCueNotify_Looping
 *
 *	This is an instanced gameplay cue notify for continuous looping effects.
 *	The game is responsible for defining the start/stop by adding/removing the gameplay cue.
 */
UCLASS(Blueprintable, notplaceable, Category = "GameplayCueNotify", Meta = (ShowWorldContextPin, DisplayName = "GCN Looping", ShortTooltip = "A GameplayCueNotify that has a duration that is driven by the game."))
class AGameplayCueNotify_Looping : public AGameplayCueNotify_Instanced
{
	GENERATED_BODY()

public:

	AGameplayCueNotify_Looping();

protected:

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual bool Recycle() override;

	virtual bool OnActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool WhileActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;

	UFUNCTION(BlueprintImplementableEvent)
	void OnApplication(AActor* Target, const FGameplayCueParameters& Parameters, const FGameplayCueNotify_SpawnResult& SpawnResults) const;

	UFUNCTION(BlueprintImplementableEvent)
	void OnLoopingStart(AActor* Target, const FGameplayCueParameters& Parameters, const FGameplayCueNotify_SpawnResult& SpawnResults) const;

	UFUNCTION(BlueprintImplementableEvent)
	void OnRecurring(AActor* Target, const FGameplayCueParameters& Parameters, const FGameplayCueNotify_SpawnResult& SpawnResults) const;

	UFUNCTION(BlueprintImplementableEvent)
	void OnRemoval(AActor* Target, const FGameplayCueParameters& Parameters, const FGameplayCueNotify_SpawnResult& SpawnResults) const;

	void RemoveLoopingEffects();

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(TArray<FText>& ValidationErrors) override;
#endif // #if WITH_EDITOR

protected:

	// Default condition to check before spawning anything.  Applies for all spawns unless overridden.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Defaults")
	FGameplayCueNotify_SpawnCondition DefaultSpawnCondition;

	// Default placement rules.  Applies for all spawns unless overridden.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Defaults")
	FGameplayCueNotify_PlacementInfo DefaultPlacementInfo;

	// List of effects to spawn on application.  These should not be looping effects!
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Application Effects (On Active)")
	FGameplayCueNotify_BurstEffects ApplicationEffects;

	// Results of spawned application effects.
	UPROPERTY(BlueprintReadOnly, Category = "Application Effects (On Active)")
	FGameplayCueNotify_SpawnResult ApplicationSpawnResults;

	// List of effects to spawn on loop start.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Looping Effects (While Active)")
	FGameplayCueNotify_LoopingEffects LoopingEffects;

	// Results of spawned looping effects.
	UPROPERTY(BlueprintReadOnly, Category = "Looping Effects (While Active)")
	FGameplayCueNotify_SpawnResult LoopingSpawnResults;

	// List of effects to spawn for a recurring gameplay effect (e.g. each time a DOT ticks).  These should not be looping effects!
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recurring Effects (On Execute)")
	FGameplayCueNotify_BurstEffects RecurringEffects;

	// Results of spawned recurring effects.
	UPROPERTY(BlueprintReadOnly, Category = "Recurring Effects (On Execute)")
	FGameplayCueNotify_SpawnResult RecurringSpawnResults;

	// List of effects to spawn on removal.  These should not be looping effects!
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Removal Effects (On Remove)")
	FGameplayCueNotify_BurstEffects RemovalEffects;

	// Results of spawned removal effects.
	UPROPERTY(BlueprintReadOnly, Category = "Removal Effects (On Remove)")
	FGameplayCueNotify_SpawnResult RemovalSpawnResults;

	bool bLoopingEffectsRemoved;
};
