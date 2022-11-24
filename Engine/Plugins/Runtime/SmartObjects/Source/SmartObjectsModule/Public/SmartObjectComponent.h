// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/SceneComponent.h"
#include "SmartObjectTypes.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectComponent.generated.h"

class UAbilitySystemComponent;
struct FSmartObjectRuntime;

UCLASS(Blueprintable, ClassGroup = Gameplay, meta = (BlueprintSpawnableComponent), config = Game, HideCategories = (Activation, AssetUserData, Collision, Cooking, HLOD, Lighting, LOD, Mobile, Mobility, Navigation, Physics, RayTracing, Rendering, Tags, TextureStreaming))
class SMARTOBJECTSMODULE_API USmartObjectComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnSmartObjectChanged, const USmartObjectComponent& /*Instance*/);

	explicit USmartObjectComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	FBox GetSmartObjectBounds() const;

	const USmartObjectDefinition* GetDefinition() const { return DefinitionAsset; }
	void SetDefinition(USmartObjectDefinition* Definition) { DefinitionAsset = Definition; }

	bool GetCanBePartOfCollection() const { return bCanBePartOfCollection; }

	FSmartObjectHandle GetRegisteredHandle() const { return RegisteredHandle; }
	void SetRegisteredHandle(const FSmartObjectHandle Value) { RegisteredHandle = Value; }

	void OnRuntimeInstanceCreated(FSmartObjectRuntime& RuntimeInstance);
	void OnRuntimeInstanceDestroyed();
	void OnRuntimeInstanceBound(FSmartObjectRuntime& RuntimeInstance);
	void OnRuntimeInstanceUnbound(FSmartObjectRuntime& RuntimeInstance);

#if WITH_EDITORONLY_DATA
	static FOnSmartObjectChanged& GetOnSmartObjectChanged() { return OnSmartObjectChanged; }
#endif // WITH_EDITORONLY_DATA

protected:
	friend struct FSmartObjectComponentInstanceData;
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void PostInitProperties() override;

#if WITH_EDITOR
	virtual void PostEditUndo() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif // WITH_EDITOR

	void RegisterToSubsystem();

	void BindTagsDelegates(FSmartObjectRuntime& RuntimeInstance, UAbilitySystemComponent& AbilitySystemComponent);
	void UnbindComponentTagsDelegate();
	void UnbindRuntimeInstanceTagsDelegate(FSmartObjectRuntime& RuntimeInstance);

	UPROPERTY(EditAnywhere, Category = SmartObject, BlueprintReadWrite)
	TObjectPtr<USmartObjectDefinition> DefinitionAsset;

	/** RegisteredHandle != FSmartObjectHandle::Invalid when registered into a collection by SmartObjectSubsystem */
	UPROPERTY(Transient, VisibleAnywhere, Category = SmartObject)
	FSmartObjectHandle RegisteredHandle;

	FDelegateHandle OnComponentTagsModifiedHandle;
	bool bInstanceTagsDelegateBound = false;

	/** 
	 * Controls whether a given SmartObject can be aggregated in SmartObjectPersistentCollections. SOs in collections
	 * can be queried and reasoned about even while the actual Actor and its components are not streamed in.
	 * By default SmartObjects are not placed in collections and are active only as long as the owner-actor remains
	 * loaded and active (i.e. not streamed out).
	 */
	UPROPERTY(config, EditAnywhere, Category = SmartObject, AdvancedDisplay)
	bool bCanBePartOfCollection = false;

#if WITH_EDITORONLY_DATA
	static FOnSmartObjectChanged OnSmartObjectChanged;
#endif // WITH_EDITORONLY_DATA
};


/** Used to store SmartObjectComponent data during RerunConstructionScripts */
USTRUCT()
struct FSmartObjectComponentInstanceData : public FActorComponentInstanceData
{
	GENERATED_BODY()

public:
	FSmartObjectComponentInstanceData() = default;

	explicit FSmartObjectComponentInstanceData(const USmartObjectComponent* SourceComponent, USmartObjectDefinition* Asset)
		: FActorComponentInstanceData(SourceComponent)
		, DefinitionAsset(Asset)
	{}

	USmartObjectDefinition* GetDefinitionAsset() const { return DefinitionAsset; }

protected:
	virtual bool ContainsData() const override;
	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override;

	UPROPERTY()
	TObjectPtr<USmartObjectDefinition> DefinitionAsset = nullptr;
};