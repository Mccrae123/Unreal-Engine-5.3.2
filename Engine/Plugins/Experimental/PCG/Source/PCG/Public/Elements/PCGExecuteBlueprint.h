// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGElement.h"
#include "PCGSettings.h"

#include "Templates/SubclassOf.h"

#include "PCGExecuteBlueprint.generated.h"

class UWorld;

#if WITH_EDITOR
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPCGBlueprintChanged, UPCGBlueprintElement*);

namespace PCGBlueprintHelper
{
	void GatherDependencies(UObject* Object, TSet<TObjectPtr<UObject>>& OutDependencies);
	void GatherDependencies(FProperty* Property, const void* InContainer, TSet<TObjectPtr<UObject>>& OutDependencies);
	TSet<TObjectPtr<UObject>> GetDataDependencies(UPCGBlueprintElement* InElement);
}
#endif // WITH_EDITOR

UCLASS(Abstract, BlueprintType, Blueprintable, hidecategories = (Object))
class UPCGBlueprintElement : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = Execution)
	void Execute(const FPCGDataCollection& Input, FPCGDataCollection& Output) const;

#if WITH_EDITOR
	// ~Begin UObject interface
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	// ~End UObject interface
#endif

	/** Needed to be able to call certain blueprint functions */
	virtual UWorld* GetWorld() const override;

#if WITH_EDITOR
	FOnPCGBlueprintChanged OnBlueprintChangedDelegate;
#endif
};

UCLASS(BlueprintType, ClassGroup = (Procedural))
class UPCGBlueprintSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	friend class FPCGExecuteBlueprintElement;

	// ~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("BlueprintNode")); }
	virtual TArray<FName> GetTrackedActorTags() const override;
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	// ~End UPCGSettings interface

public:
	// ~Begin UObject interface
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	// ~End UObject interface
#endif

	UFUNCTION(BlueprintCallable, Category = Settings)
	void SetElementType(TSubclassOf<UPCGBlueprintElement> InElementType);

protected:
	UPROPERTY()
	TSubclassOf<UPCGBlueprintElement> BlueprintElement_DEPRECATED;

	UPROPERTY(BlueprintSetter = SetElementType, EditAnywhere, Category = Template)
	TSubclassOf<UPCGBlueprintElement> BlueprintElementType;

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Instanced, Category = Settings, meta = (ShowOnlyInnerProperties))
	TObjectPtr<UPCGBlueprintElement> BlueprintElementInstance;

#if WITH_EDITORONLY_DATA
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	TArray<FName> TrackedActorTags;
#endif

protected:
#if WITH_EDITOR
	void OnBlueprintChanged(UBlueprint* InBlueprint);
	void OnDependencyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	void OnBlueprintElementChanged(UPCGBlueprintElement* InElement);
#endif

	void RefreshBlueprintElement();
	void SetupBlueprintEvent();
	void TeardownBlueprintEvent();
	void SetupBlueprintElementEvent();
	void TeardownBlueprintElementEvent();

#if WITH_EDITORONLY_DATA
	TSet<TObjectPtr<UObject>> DataDependencies;
#endif
};

class FPCGExecuteBlueprintElement : public FSimpleTypedPCGElement<UPCGBlueprintSettings>
{
public:
	virtual bool Execute(FPCGContextPtr Context) const override;
};