// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakInterfacePtr.h"
#include "GameFramework/Actor.h"
#include "LandscapeBlueprintBrush.h"
#include "Containers/Map.h"
#include "WaterBrushActorInterface.h"
#include "WaterLandscapeBrush.generated.h"

class AWaterBody;
class AWaterBodyIsland;

UCLASS(Blueprintable, hidecategories = (Replication, Input, LOD, Actor, Cooking, Rendering))
class AWaterLandscapeBrush : public ALandscapeBlueprintBrush
{
	GENERATED_BODY()

public:
	AWaterLandscapeBrush(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void PostLoad() override;
	virtual void PostInitProperties() override;
	virtual void BeginDestroy() override;
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	UFUNCTION(BlueprintCallable, Category = "Water", meta = (DeterminesOutputType = "WaterBodyClass", DynamicOutputParam = "OutWaterBodies"))
	void GetWaterBodies(TSubclassOf<AWaterBody> WaterBodyClass, TArray<AWaterBody*>& OutWaterBodies) const;

	UFUNCTION(BlueprintCallable, Category = "Water", meta = (DeterminesOutputType = "WaterBodyIslandClass", DynamicOutputParam = "OutWaterBodyIslands"))
	void GetWaterBodyIslands(TSubclassOf<AWaterBodyIsland> WaterBodyIslandClass, TArray<AWaterBodyIsland*>& OutWaterBodyIslands) const;

	UFUNCTION(BlueprintCallable, Category = "Water")
	void GetActorsAffectingLandscape(TArray<TScriptInterface<IWaterBrushActorInterface>>& OutWaterBrushActors) const;

	const TArray<TWeakInterfacePtr<IWaterBrushActorInterface>>& GetActorsAffectingLandscape() const { return ActorsAffectingLandscape; }

	UFUNCTION(BlueprintNativeEvent, meta = (CallInEditor = "true"))
	void BlueprintWaterBodiesChanged();
	virtual void BlueprintWaterBodiesChanged_Native() {}

	UFUNCTION(BlueprintNativeEvent, meta = (CallInEditor = "true"))
	void BlueprintWaterBodyChanged(AActor* Actor);
	virtual void BlueprintWaterBodyChanged_Native(AActor* Actor) {}

	UFUNCTION(BlueprintCallable, Category = "Cache", meta = (DeprecatedFunction, DeprecationMessage="Use SetActorCache instead"))
	void SetWaterBodyCache(AWaterBody* WaterBody, UObject* Cache);
	
	UFUNCTION(BlueprintCallable, Category = "Cache", meta = (DeterminesOutputType = "CacheClass", DeprecatedFunction, DeprecationMessage = "Use GetActorCache instead"))
	UObject* GetWaterBodyCache(AWaterBody* WaterBody, TSubclassOf<UObject> CacheClass) const;

	UFUNCTION(BlueprintCallable, Category = "Cache", meta = (DeprecatedFunction, DeprecationMessage = "Use ClearActorCache instead"))
	void ClearWaterBodyCache(AWaterBody* WaterBody);

	UFUNCTION(BlueprintCallable, Category = "Cache")
	void SetActorCache(AActor* InActor, UObject* InCache);

	UFUNCTION(BlueprintCallable, Category = "Cache", meta = (DeterminesOutputType = "CacheClass"))
	UObject* GetActorCache(AActor* InActor, TSubclassOf<UObject> CacheClass) const;

	UFUNCTION(BlueprintCallable, Category = "Cache")
	void ClearActorCache(AActor* InActor);

	UFUNCTION(BlueprintNativeEvent, Category = "Cache", meta = (CallInEditor = "true"))
	void BlueprintGetRenderTargets(UTextureRenderTarget2D* InHeightRenderTarget, UTextureRenderTarget2D*& OutVelocityRenderTarget);
	virtual void BlueprintGetRenderTargets_Native(UTextureRenderTarget2D* InHeightRenderTarget, UTextureRenderTarget2D*& OutVelocityRenderTarget) {}

	UFUNCTION(BlueprintNativeEvent, Category = "Cache", meta = (CallInEditor = "true"))
	void BlueprintOnRenderTargetTexturesUpdated(UTexture2D* VelocityTexture);
	virtual void BlueprintOnRenderTargetTexturesUpdated_Native(UTexture2D* VelocityTexture) {}

	UFUNCTION(BlueprintCallable, Category = "Cache")
	void ForceWaterTextureUpdate();

	void SetTargetLandscape(ALandscape* InTargetLandscape);
		
	virtual void SetOwningLandscape(ALandscape* InOwningLandscape) override;

	virtual void GetRenderDependencies(TSet<UTexture2D *>& OutTextures) override;

	void ForceUpdate();

private:
	template<class T>
	friend class FAddActorsOfType;

	template<class T>
	friend class FGetActorsOfType;

	void AddActorInternal(AActor* Actor, const UWorld* ThisWorld, UObject* InCache, bool bTriggerEvent, bool bModify);
	void RemoveActorInternal(AActor* Actor);
	void UpdateActors(bool bTriggerEvents = true);
	void UpdateAffectedWeightmaps();
	void ClearActors();
	bool IsActorAffectingLandscape(AActor* Actor) const;

	void OnFullHeightmapRenderDone(UTextureRenderTarget2D* HeightmapRenderTarget);
	void OnWaterBrushActorChanged(const IWaterBrushActorInterface::FWaterBrushActorChangedEventParams& InParams);
	void OnActorChanged(AActor* Actor, bool bWeightmapSettingsChanged, bool bRebuildMesh);
	void OnActorsAffectingLandscapeChanged();

	void MarkDirty();
	
	TArray<TWeakInterfacePtr<IWaterBrushActorInterface>> ActorsAffectingLandscape;
	FDelegateHandle OnLevelAddedToWorldHandle;
	FDelegateHandle OnLevelRemovedFromWorldHandle;
	FDelegateHandle OnLevelActorAddedHandle;
	FDelegateHandle OnLevelActorDeletedHandle;
	FDelegateHandle OnActorMovedHandle;

	UPROPERTY(Transient, DuplicateTransient, VisibleAnywhere, AdvancedDisplay, meta = (Category = "Debug"))
	TMap<TWeakObjectPtr<AActor>, UObject*> Cache;

	bool bRenderTargetsDirty = false;
};