// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameFramework/Actor.h"
#include "Misc/SecureHash.h"
#include "Templates/Function.h"

#include "USDLevelSequenceHelper.h"
#include "USDListener.h"
#include "USDMemory.h"
#include "USDPrimTwin.h"

#if USE_USD_SDK

#include "USDIncludesStart.h"

#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"

#include "USDIncludesEnd.h"

#endif // #if USE_USD_SDK

#include "USDStageActor.generated.h"

class ALevelSequenceActor;
class IMeshBuilderModule;
class ULevelSequence;
class UMaterial;
class UUsdAsset;
enum class EUsdPurpose : int32;
struct FMeshDescription;
struct FUsdSchemaTranslationContext;

UENUM()
enum class EUsdInitialLoadSet
{
	LoadAll,
	LoadNone
};

UCLASS( MinimalAPI )
class AUsdStageActor : public AActor
{
	GENERATED_BODY()

	friend struct FUsdStageActorImpl;
	friend class FUsdLevelSequenceHelperImpl;

public:
	UPROPERTY(EditAnywhere, Category = "USD", meta = (FilePathFilter = "usd files (*.usd; *.usda; *.usdc)|*.usd; *.usda; *.usdc"))
	FFilePath RootLayer;

	UPROPERTY(EditAnywhere, Category = "USD")
	EUsdInitialLoadSet InitialLoadSet;

	/* Only load prims with these specific purposes from the USD file */
	UPROPERTY(EditAnywhere, Category = "USD", meta = (Bitmask, BitmaskEnum=EUsdPurpose))
	int32 PurposesToLoad;

	/* Quickly toggle visibility of prims with specific purposes in the level based on component tags */
	UPROPERTY(EditAnywhere, Category = "USD", meta = (Bitmask, BitmaskEnum=EUsdPurpose))
	int32 PurposeVisibility;

	UFUNCTION(BlueprintCallable, Category = "USD", meta = (CallInEditor = "true"))
	float GetTime() const { return Time; }

	UFUNCTION(BlueprintCallable, Category = "USD", meta = (CallInEditor = "true"))
	void SetTime(float InTime);

private:
	UPROPERTY(Category = UsdStageActor, VisibleAnywhere, BlueprintReadOnly, meta = (ExposeFunctionCategories = "Mesh,Rendering,Physics,Components|StaticMesh", AllowPrivateAccess = "true"))
	class USceneComponent* SceneComponent;

	/* TimeCode to evaluate the USD stage at */
	UPROPERTY(EditAnywhere, Category = "USD")
	float Time;

	UPROPERTY(EditAnywhere, Category = "USD")
	float StartTimeCode;

	UPROPERTY(EditAnywhere, Category = "USD")
	float EndTimeCode;

	UPROPERTY(VisibleAnywhere, Category = "USD")
	float TimeCodesPerSecond;

	UPROPERTY(VisibleAnywhere, Category = "USD", Transient)
	ULevelSequence* LevelSequence;

	UPROPERTY(Transient)
	TMap<FString, ULevelSequence*> SubLayerLevelSequencesByIdentifier;

public:
	DECLARE_EVENT_OneParam( AUsdStageActor, FOnActorLoaded, AUsdStageActor* );
	USDSTAGE_API static FOnActorLoaded OnActorLoaded;

	DECLARE_EVENT( AUsdStageActor, FOnStageActorEvent );
	FOnStageActorEvent OnStageChanged;
	FOnStageActorEvent OnActorDestroyed;

	DECLARE_EVENT_TwoParams( AUsdStageActor, FOnPrimChanged, const FString&, bool );
	FOnPrimChanged OnPrimChanged;

	DECLARE_MULTICAST_DELEGATE(FOnUsdStageTimeChanged);
	FOnUsdStageTimeChanged OnTimeChanged;

public:
	AUsdStageActor();
	virtual ~AUsdStageActor();

	USDSTAGE_API void Reset() override;
	void Refresh() const;
	void ReloadAnimations();

public:
	virtual void PostEditChangeProperty( FPropertyChangedEvent& PropertyChangedEvent ) override;
	virtual void PostTransacted(const FTransactionObjectEvent& TransactionEvent) override;

private:
	void Clear();
	void OpenUsdStage();
	void LoadUsdStage();

	void RefreshVisibilityBasedOnPurpose();

	void OnUsdPrimTwinDestroyed( const FUsdPrimTwin& UsdPrimTwin );

	void OnPrimObjectPropertyChanged( UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent );
	bool HasAutorithyOverStage() const;

private:
	FUsdPrimTwin RootUsdTwin;

	TWeakObjectPtr< ALevelSequenceActor > LevelSequenceActor;

	TMultiMap< FString, FDelegateHandle > PrimDelegates;

	TSet< FString > PrimsToAnimate;

	TMap< UObject*, FString > ObjectsToWatch;

private:
	/** Hash based assets cache */
	UPROPERTY( NonPIEDuplicateTransient )
	TMap< FString, UObject* > AssetsCache;

	/** Map of USD Prim Paths to UE assets */
	UPROPERTY( NonPIEDuplicateTransient )
	TMap< FString, UObject* > PrimPathsToAssets;

#if USE_USD_SDK
public:
	USDSTAGE_API const pxr::UsdStageRefPtr& GetUsdStage();

	FUsdListener& GetUsdListener() { return UsdListener; }
	const FUsdListener& GetUsdListener() const { return UsdListener; }

	FUsdPrimTwin* GetOrCreatePrimTwin( const pxr::SdfPath& UsdPrimPath );
	FUsdPrimTwin* ExpandPrim( const pxr::UsdPrim& Prim, FUsdSchemaTranslationContext& TranslationContext );
	void UpdatePrim( const pxr::SdfPath& UsdPrimPath, bool bResync, FUsdSchemaTranslationContext& TranslationContext );

protected:
	void LoadAssets( FUsdSchemaTranslationContext& TranslationContext, const pxr::UsdPrim& StartPrim );
	void AnimatePrims();

private:
	TUsdStore< pxr::UsdStageRefPtr > UsdStageStore;
	FUsdListener UsdListener;
#endif // #if USE_USD_SDK

	FUsdLevelSequenceHelper LevelSequenceHelper;
};
