// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGElement.h"
#include "PCGSettings.h"

#include "Data/PCGPointData.h"
#include "PCGPoint.h"

#include "Math/RandomStream.h"
#include "Templates/SubclassOf.h"

#include "PCGExecuteBlueprint.generated.h"

class UWorld;

#if WITH_EDITOR
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPCGBlueprintChanged, UPCGBlueprintElement*);

namespace PCGBlueprintHelper
{
	TSet<TObjectPtr<UObject>> GetDataDependencies(UPCGBlueprintElement* InElement);
}
#endif // WITH_EDITOR

UCLASS(Abstract, BlueprintType, Blueprintable, hidecategories = (Object))
class PCG_API UPCGBlueprintElement : public UObject
{
	GENERATED_BODY()

public:
	// ~Begin UObject interface
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	// ~End UObject interface

	UFUNCTION(BlueprintNativeEvent, Category = Execution)
	void ExecuteWithContext(UPARAM(ref)FPCGContext& InContext, const FPCGDataCollection& Input, FPCGDataCollection& Output);

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = Execution)
	void Execute(const FPCGDataCollection& Input, FPCGDataCollection& Output);

	UFUNCTION(BlueprintImplementableEvent, Category = Execution)
	bool PointLoopBody(const FPCGContext& InContext, const UPCGPointData* InData, const FPCGPoint& InPoint, FPCGPoint& OutPoint, UPCGMetadata* OutMetadata) const;

	UFUNCTION(BlueprintImplementableEvent, Category = Execution)
	TArray<FPCGPoint> MultiPointLoopBody(const FPCGContext& InContext, const UPCGPointData* InData, const FPCGPoint& InPoint, UPCGMetadata* OutMetadata) const;

	UFUNCTION(BlueprintImplementableEvent, Category = Execution)
	bool PointPairLoopBody(const FPCGContext& InContext, const UPCGPointData* InA, const UPCGPointData* InB, const FPCGPoint& InPointA, const FPCGPoint& InPointB, FPCGPoint& OutPoint, UPCGMetadata* OutMetadata) const;

	UFUNCTION(BlueprintImplementableEvent, Category = Execution)
	bool IterationLoopBody(const FPCGContext& InContext, int64 Iteration, const UPCGSpatialData* InA, const UPCGSpatialData* InB, FPCGPoint& OutPoint, UPCGMetadata* OutMetadata) const;

	/** Calls the LoopBody function on all points */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = Execution, meta = (HideSelfPin = "true"))
	void LoopOnPoints(UPARAM(ref) FPCGContext& InContext, const UPCGPointData* InData, UPCGPointData*& OutData, UPCGPointData* OptionalOutData = nullptr) const;

	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = Execution, meta = (HideSelfPin = "true"))
	void MultiLoopOnPoints(UPARAM(ref) FPCGContext& InContext, const UPCGPointData* InData, UPCGPointData*& OutData, UPCGPointData* OptionalOutData = nullptr) const;

	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = Execution, meta = (HideSelfPin = "true"))
	void LoopOnPointPairs(UPARAM(ref) FPCGContext& InContext, const UPCGPointData* InA, const UPCGPointData* InB, UPCGPointData*& OutData, UPCGPointData* OptionalOutData = nullptr) const;

	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = Execution, meta = (HideSelfPin = "true"))
	void LoopNTimes(UPARAM(ref) FPCGContext& InContext, int64 NumIterations, UPCGPointData*& OutData, const UPCGSpatialData* InA = nullptr, const UPCGSpatialData* InB = nullptr, UPCGPointData* OptionalOutData = nullptr) const;

	/** Override for the default node name */
	UFUNCTION(BlueprintNativeEvent, Category = Graph)
	FName NodeTitleOverride() const;

	UFUNCTION(BlueprintNativeEvent, Category = Graph)
	FLinearColor NodeColorOverride() const;

	UFUNCTION(BlueprintNativeEvent, Category = Graph)
	EPCGSettingsType NodeTypeOverride() const;

	UFUNCTION(BlueprintCallable, Category = "Input & Output")
	TSet<FName> InputLabels() const;

	UFUNCTION(BlueprintCallable, Category = "Input & Output")
	TSet<FName> OutputLabels() const;

	/** Gets the seed from the associated settings & source component */
	UFUNCTION(BlueprintCallable, Category = "PCG|Random")
	int GetSeed(UPARAM(ref) FPCGContext& InContext) const;

	/** Creates a random stream from the settings & source component */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "PCG|Random")
	FRandomStream GetRandomStream(UPARAM(ref) FPCGContext& InContext) const;

	/** Called after object creation to setup the object callbacks */
	void Initialize();

#if WITH_EDITOR
	// ~Begin UObject interface
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	// ~End UObject interface

	/** Used for filtering */
	static FString GetParentClassName();
#endif

	/** Needed to be able to call certain blueprint functions */
	virtual UWorld* GetWorld() const override;

#if !WITH_EDITOR
	void SetInstanceWorld(UWorld* World) { InstanceWorld = World; }
#endif

#if WITH_EDITOR
	FOnPCGBlueprintChanged OnBlueprintChangedDelegate;
#endif

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Settings)
	bool bCreatesArtifacts = false;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Settings)
	bool bCanBeMultithreaded = false;

#if WITH_EDITORONLY_DATA
	UPROPERTY(BlueprintGetter=InputLabels, Category = "Settings|Input & Output", meta = (DeprecatedProperty, DeprecatedMessage = "Input Pin Labels are deprecated - use Input Labels instead."))
	TSet<FName> InputPinLabels_DEPRECATED;

	UPROPERTY(BlueprintGetter=OutputLabels, Category = "Settings|Input & Output")
	TSet<FName> OutputPinLabels_DEPRECATED;
#endif

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Settings|Input & Output")
	TArray<FPCGPinProperties> CustomInputPins;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Settings|Input & Output")
	TArray<FPCGPinProperties> CustomOutputPins;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Settings|Input & Output")
	bool bHasDefaultInPin = true;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Settings|Input & Output")
	bool bHasDefaultOutPin = true;

#if WITH_EDITORONLY_DATA
	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = AssetInfo, AssetRegistrySearchable)
	bool bExposeToLibrary = false;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = AssetInfo, AssetRegistrySearchable)
	FText Category;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = AssetInfo, AssetRegistrySearchable)
	FText Description;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Settings|Advanced")
	int32 DependencyParsingDepth = 1;
#endif

protected:
#if WITH_EDITOR
	void OnDependencyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	TSet<TObjectPtr<UObject>> DataDependencies;
#endif

#if !WITH_EDITORONLY_DATA
	UWorld* InstanceWorld = nullptr;
#endif
};

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGBlueprintSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGBlueprintSettings();

	friend class FPCGExecuteBlueprintElement;

	// ~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("ExecuteBlueprint")); }
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual EPCGSettingsType GetType() const override;
	virtual void GetTrackedActorTags(FPCGTagToSettingsMap& OutTagToSettings, TArray<TObjectPtr<const UPCGGraph>>& OutVisitedGraphs) const override;
	virtual UObject* GetJumpTargetForDoubleClick() const override;
#endif

	virtual FName AdditionalTaskName() const override;
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;
	// ~End UPCGSettings interface

public:
	// ~Begin UObject interface
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	// ~End UObject interface
#endif

	UFUNCTION(BlueprintCallable, Category = "Settings|Template", meta=(DeterminesOutputType="InElementType", DynamicOutputParam = "ElementInstance"))
	void SetElementType(TSubclassOf<UPCGBlueprintElement> InElementType, UPCGBlueprintElement*& ElementInstance);

	UFUNCTION(BlueprintCallable, Category = "Settings|Template")
	TSubclassOf<UPCGBlueprintElement> GetElementType() const { return BlueprintElementType; }

#if WITH_EDITOR
	TObjectPtr<UPCGBlueprintElement> GetElementInstance() const { return BlueprintElementInstance; }
#endif

protected:
#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TSubclassOf<UPCGBlueprintElement> BlueprintElement_DEPRECATED;
#endif

	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = Template)
	TSubclassOf<UPCGBlueprintElement> BlueprintElementType;

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Instanced, Category = "Instance", meta = (ShowOnlyInnerProperties))
	TObjectPtr<UPCGBlueprintElement> BlueprintElementInstance;

#if WITH_EDITORONLY_DATA
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	TArray<FName> TrackedActorTags;

	UPROPERTY()
	bool bCreatesArtifacts_DEPRECATED = false;

	UPROPERTY()
	bool bCanBeMultithreaded_DEPRECATED = false;
#endif

protected:
#if WITH_EDITOR
	void OnBlueprintChanged(UBlueprint* InBlueprint);
	void OnBlueprintElementChanged(UPCGBlueprintElement* InElement);
#endif

	void RefreshBlueprintElement();
	void SetupBlueprintEvent();
	void TeardownBlueprintEvent();
	void SetupBlueprintElementEvent();
	void TeardownBlueprintElementEvent();
};

struct FPCGBlueprintExecutionContext : public FPCGContext
{
	virtual ~FPCGBlueprintExecutionContext();

	UPCGBlueprintElement* BlueprintElementInstance = nullptr;
};

class FPCGExecuteBlueprintElement : public IPCGElement
{
public:
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const override;
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override;

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual FPCGContext* Initialize(const FPCGDataCollection& InputData, TWeakObjectPtr<UPCGComponent> SourceComponent, const UPCGNode* Node) override;	
};
