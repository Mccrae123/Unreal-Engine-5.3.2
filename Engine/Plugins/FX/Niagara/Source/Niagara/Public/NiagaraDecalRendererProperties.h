// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraCommon.h"
#include "NiagaraDataSetAccessor.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraDecalRendererProperties.generated.h"

class UMaterialInterface;
class FNiagaraEmitterInstance;
class SWidget;

UCLASS(editinlinenew, MinimalAPI, meta = (DisplayName = "Decal Renderer"))
class UNiagaraDecalRendererProperties : public UNiagaraRendererProperties
{
public:
	GENERATED_BODY()

	UNiagaraDecalRendererProperties();

	//UObject Interface
	virtual void PostLoad() override;
	virtual void PostInitProperties() override;
#if WITH_EDITORONLY_DATA
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif// WITH_EDITORONLY_DATA
	//UObject Interface END

	static void InitCDOPropertiesAfterModuleStartup();

	//~ UNiagaraRendererProperties interface
	virtual FNiagaraRenderer* CreateEmitterRenderer(ERHIFeatureLevel::Type FeatureLevel, const FNiagaraEmitterInstance* Emitter, const FNiagaraSystemInstanceController& InController) override;
	virtual class FNiagaraBoundsCalculator* CreateBoundsCalculator() override;
	virtual void GetUsedMaterials(const FNiagaraEmitterInstance* InEmitter, TArray<UMaterialInterface*>& OutMaterials) const override;
	virtual bool IsSimTargetSupported(ENiagaraSimTarget InSimTarget) const override { return InSimTarget == ENiagaraSimTarget::CPUSim; };
#if WITH_EDITORONLY_DATA
	virtual const TArray<FNiagaraVariable>& GetOptionalAttributes() override;
	virtual void GetRendererWidgets(const FNiagaraEmitterInstance* InEmitter, TArray<TSharedPtr<SWidget>>& OutWidgets, TSharedPtr<FAssetThumbnailPool> InThumbnailPool) const override;
	virtual void GetRendererTooltipWidgets(const FNiagaraEmitterInstance* InEmitter, TArray<TSharedPtr<SWidget>>& OutWidgets, TSharedPtr<FAssetThumbnailPool> InThumbnailPool) const override;
	virtual void GetRendererFeedback(const FVersionedNiagaraEmitter& InEmitter, TArray<FText>& OutErrors, TArray<FText>& OutWarnings, TArray<FText>& OutInfo) const override;
#endif // WITH_EDITORONLY_DATA
	virtual void CacheFromCompiledData(const FNiagaraDataSetCompiledData* CompiledData) override;
	virtual void UpdateSourceModeDerivates(ENiagaraRendererSourceDataMode InSourceMode, bool bFromPropertyEdit = false) override;
	virtual ENiagaraRendererSourceDataMode GetCurrentSourceMode() const override { return SourceMode; }
	virtual bool PopulateRequiredBindings(FNiagaraParameterStore& InParameterStore) override;
	//UNiagaraRendererProperties Interface END

	UMaterialInterface* GetMaterial(const FNiagaraEmitterInstance* InEmitter) const;

	static const FQuat4f GetDefaultOrientation() { return FRotator3f(-90.0f, 0.0f, 90.0f).Quaternion(); }
	static const FVector3f GetDefaultDecalSize() { return FVector3f(50.0f, 50.0f, 50.0f); }
	static const float GetDefaultDecalFade() { return 1.0f; }
	static const FNiagaraBool GetDefaultDecalVisible() { return FNiagaraBool(true); }

	/** What material to use for the decal. */
	UPROPERTY(EditAnywhere, Category = "Decal Rendering")
	TObjectPtr<UMaterialInterface> Material;

	/** Binding to material. */
	UPROPERTY(EditAnywhere, Category = "Decal Rendering")
	FNiagaraParameterBinding MaterialParameterBinding;

	/** Whether or not to draw a single element for the Emitter or to draw the particles.*/
	UPROPERTY(EditAnywhere, Category = "Decal Rendering")
	ENiagaraRendererSourceDataMode SourceMode = ENiagaraRendererSourceDataMode::Particles;

	/** If a render visibility tag is present, particles whose tag matches this value will be visible in this renderer. */
	UPROPERTY(EditAnywhere, Category = "Decal Rendering")
	int32 RendererVisibility = 0;

	/** When the decal is smaller than this screen size fade out the decal, can be used to reduce the amount of small decals drawn. */
	UPROPERTY(EditAnywhere, Category = "Decal Rendering")
	float DecalScreenSizeFade = 0.f;

	/** Position binding for the decals, should be center of the decal */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding PositionBinding;

	/** Orientation binding for the decal. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding DecalOrientationBinding;

	/** Size binding for the decal. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding DecalSizeBinding;

	/** Fade binding for the decal, value can be queried using the Decal Lifetime Opacity material node. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding DecalFadeBinding;

	/** Color binding for the decal, value can be queried using the Decal Color material node. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding DecalColorBinding;

	/** Should the decal be visibile or not, works in conjunction with RendererVisibilityTagBinding to determine visibility. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding DecalVisibleBinding;

	/** Visibility tag binding, when valid the returned values is compated with RendererVisibility. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding RendererVisibilityTagBinding;

	FNiagaraDataSetAccessor<FNiagaraPosition>	PositionDataSetAccessor;
	FNiagaraDataSetAccessor<FQuat4f>			DecalOrientationDataSetAccessor;
	FNiagaraDataSetAccessor<FVector3f>			DecalSizeDataSetAccessor;
	FNiagaraDataSetAccessor<float>				DecalFadeDataSetAccessor;
	FNiagaraDataSetAccessor<FLinearColor>		DecalColorDataSetAccessor;
	FNiagaraDataSetAccessor<FNiagaraBool>		DecalVisibleAccessor;
	FNiagaraDataSetAccessor<int32>				RendererVisibilityTagAccessor;
};
