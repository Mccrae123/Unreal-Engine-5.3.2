// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGCommon.h"
#include "PCGData.h"
#include "PCGElement.h"
#include "PCGDebug.h"
#include "Tests/Determinism/PCGDeterminismSettings.h"

#include "PCGSettings.generated.h"

class UPCGGraph;
class UPCGNode;
class UPCGSettings;

typedef TMap<FName, TSet<TWeakObjectPtr<const UPCGSettings>>> FPCGTagToSettingsMap;

UENUM()
enum class EPCGSettingsExecutionMode : uint8
{
	Enabled,
	Debug,
	Isolated,
	Disabled
};

UENUM()
enum class EPCGSettingsType : uint8
{
	InputOutput,
	Spatial,
	Density,
	Blueprint,
	Metadata,
	Filter,
	Sampler,
	Spawner,
	Subgraph,
	Debug,
	Generic,
	Param
};

#if WITH_EDITOR
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPCGSettingsChanged, UPCGSettings*, EPCGChangeType);
#endif

UCLASS(Abstract)
class PCG_API UPCGSettingsInterface : public UPCGData
{
	GENERATED_BODY()

public:
	virtual UPCGSettings* GetSettings() PURE_VIRTUAL(UPCGSettingsInterface::GetSettings, return nullptr;);
	virtual const UPCGSettings* GetSettings() const PURE_VIRTUAL(UPCGSettingsInterface::GetSettings, return nullptr;);

	bool IsInstance() const;

#if WITH_EDITOR
	FOnPCGSettingsChanged OnSettingsChangedDelegate;
#endif

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Debug)
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Debug)
	bool bDebug = false;

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Debug, meta = (ShowOnlyInnerProperties))
	FPCGDebugVisualizationSettings DebugSettings;
#endif
};

/**
* Base class for settings-as-data in the PCG framework
*/
UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGSettings : public UPCGSettingsInterface
{
	GENERATED_BODY()

public:
	// ~Begin UPCGData interface
	virtual EPCGDataType GetDataType() const override { return EPCGDataType::Settings | Super::GetDataType(); }
	// ~End UPCGData interface

	// ~Begin UPCGSettingsInterface interface
	virtual UPCGSettings* GetSettings() { return this; }
	virtual const UPCGSettings* GetSettings() const { return this; }
	// ~End UPCGSettingsInterface interface

	//~Begin UObject interface
	virtual void PostLoad() override;
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostSaveRoot(FObjectPostSaveRootContext ObjectSaveContext) override;
	//~End UObject interface

	// TODO: check if we need this to be virtual, we don't really need if we're always caching
	/*virtual*/ FPCGElementPtr GetElement() const;
	virtual UPCGNode* CreateNode() const;

	virtual TArray<FPCGPinProperties> InputPinProperties() const;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const;

	bool operator==(const UPCGSettings& Other) const;
	uint32 GetCrc32() const;
	bool UseSeed() const { return bUseSeed; }

#if WITH_EDITOR
	virtual void ApplyDeprecation(UPCGNode* InOutNode) {}

	virtual FName GetDefaultNodeName() const { return NAME_None; }
	virtual FText GetNodeTooltipText() const { return FText::GetEmpty(); }
	virtual FLinearColor GetNodeTitleColor() const { return FLinearColor::White; }
	virtual EPCGSettingsType GetType() const { return EPCGSettingsType::Generic; }
	/** Derived classes must implement this to communicate dependencies on external actors */
	virtual void GetTrackedActorTags(FPCGTagToSettingsMap& OutTagToSettings, TArray<TObjectPtr<const UPCGGraph>>& OutVisitedGraphs) const {}
	/** Override this class to provide an UObject to jump to in case of double click on node
	 *  ie. returning a blueprint instance will open the given blueprint in its editor.
	 *  By default, it will return the underlying class, to try to jump to its header in code
     */
	virtual UObject* GetJumpTargetForDoubleClick() const;
#endif

	/** Derived classes can implement this to expose additional name information in the logs */
	virtual FName AdditionalTaskName() const { return NAME_None; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(EditCondition=bUseSeed, EditConditionHides))
	int Seed = 0xC35A9631; // random prime number

	/** TODO: Remove this - Placeholder feature until we have a nodegraph */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Tags")
	TSet<FString> FilterOnTags;

	/** TODO: Remove this - Placeholder feature until we have a nodegraph */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Tags")
	bool bPassThroughFilteredOutInputs = true;

	/** TODO: Remove this - Placeholder feature until we have a nodegraph */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Tags")
	TSet<FString> TagsAppliedOnOutput;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	EPCGSettingsExecutionMode ExecutionMode_DEPRECATED = EPCGSettingsExecutionMode::Enabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Determinism, meta = (ShowOnlyInnerProperties))
	FPCGDeterminismSettings DeterminismSettings;
#endif

#if WITH_EDITORONLY_DATA
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = AssetInfo, AssetRegistrySearchable)
	bool bExposeToLibrary = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = AssetInfo, AssetRegistrySearchable)
	FText Category;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = AssetInfo, AssetRegistrySearchable)
	FText Description;
#endif

protected:
	virtual FPCGElementPtr CreateElement() const PURE_VIRTUAL(UPCGSettings::CreateElement, return nullptr;);

	/** An additional custom version number that external system users can use to track versions. This version will be serialized into the asset and will be provided by UserDataVersion after load. */
	virtual FGuid GetUserCustomVersionGuid() { return FGuid(); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual bool IsStructuralProperty(const FName& InPropertyName) const { return false; }

	/** Method that can be called to dirty the cache data from this settings objects if the operator== does not allow to detect changes */
	void DirtyCache();
#endif

	// By default, settings won't use a seed. Set this bool at true in the child ctor to allow edition and use it.
	UPROPERTY(VisibleAnywhere, Transient, Category = Settings, meta = (EditCondition = false, EditConditionHides))
	bool bUseSeed = false;

	/** Methods to remove boilerplate code across settings */
	TArray<FPCGPinProperties> DefaultPointOutputPinProperties() const;

#if WITH_EDITOR
protected:
	/** The version number of the data after load and after any data migration. */
	int32 DataVersion = -1;

	/** If a custom version guid was provided through GetUserCustomVersionGuid(), this field will hold the version number after load and after any data migration. */
	int32 UserDataVersion = -1;
#endif

private:
	mutable FPCGElementPtr CachedElement;
	mutable FCriticalSection CacheLock;
};

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGSettingsInstance : public UPCGSettingsInterface
{
	GENERATED_BODY()

public:
	// ~Begin UPCGSettingsInterface
	virtual UPCGSettings* GetSettings() { return Settings.Get(); }
	virtual const UPCGSettings* GetSettings() const { return Settings.Get(); }
	// ~End UPCGSettingsInterface

	// ~Begin UObject interface
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	// ~End UObject interface

	void SetSettings(UPCGSettings* InSettings);

#if WITH_EDITOR
	void OnSettingsChanged(UPCGSettings* InSettings, EPCGChangeType ChangeType);
#endif

#if WITH_EDITORONLY_DATA
	UPROPERTY(Transient, VisibleAnywhere, Category = Instance)
	TObjectPtr<UPCGSettings> OriginalSettings = nullptr; // Transient just for display
#endif

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Instance, meta = (EditInline))
	TObjectPtr<UPCGSettings> Settings = nullptr;
};

/** Trivial / Pass-through settings used for input/output nodes */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCG_API UPCGTrivialSettings : public UPCGSettings
{
	GENERATED_BODY()

protected:
	//~UPCGSettings implementation
	virtual FPCGElementPtr CreateElement() const override;
};

class PCG_API FPCGTrivialElement : public FSimplePCGElement
{
public:
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return true; }

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool IsPassthrough() const override { return true; }
};
