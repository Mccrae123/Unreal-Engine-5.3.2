// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "EngineDefines.h"
#include "GameFramework/Info.h"
#include "Misc/Guid.h"
#include "RenderResource.h"

#include "VolumetricCloudComponent.generated.h"


class FVolumetricCloudSceneProxy;


/**
 * A component that represents a participating media material around a planet, e.g. clouds.
 */
UCLASS(ClassGroup = Rendering, collapsecategories, hidecategories = (Object, Mobility, Activation, "Components|Activation"), editinlinenew, meta = (BlueprintSpawnableComponent), MinimalAPI)
class UVolumetricCloudComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

	~UVolumetricCloudComponent();

	/** The altitude at which the cloud layer starts. (kilometers above the ground) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Layer", meta = (UIMin = 0.0f, UIMax = 20.0f, ClampMin = 0.0f, SliderExponent = 2.0))
	float LayerBottomAltitude;

	/** The altitude at which the cloud layer ends. (kilometers above the ground) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Layer", meta = (UIMin = 0.1f, UIMax = 20.0f, ClampMin = 0.1f, SliderExponent = 2.0))
	float LayerHeight;

	/** The maximum distance of the volumetric surface before which we will accept to start tracing. (kilometers) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Layer", meta = (UIMin = 100.0f, UIMax = 500.0f, ClampMin = 10.0f, SliderExponent = 2.0))
	float TracingStartMaxDistance;

	/** The maximum distance that will be traced inside the cloud layer. (kilometers) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Layer", meta = (UIMin = 10.0f, UIMax = 500.0f, ClampMin = 10.0f, SliderExponent = 2.0))
	float TracingMaxDistance;

	/** The planet radius used when there is not SkyAtmosphere component present in the scene. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Planet", meta = (UIMin = 100.0f, UIMax = 7000.0f, ClampMin = 100.0f, ClampMax = 10000.0f))
	float PlanetRadius;

	/** 
	 * The ground albedo used to light the cloud from below with respect to the sun light and sky atmosphere. 
	 * This is only used by the cloud material when the 'Volumetric Advanced' node have GroundContribution enabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, interp, Category = "Planet", meta = (HideAlphaChannel))
	FColor GroundAlbedo;

	/** The material describing the cloud volume. It must be a Volume domain material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Material")
	UMaterialInterface* Material;

	/** Wether to apply atmosphere transmittance per sample, instead of using the light global transmittance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Tracing")
	uint32 bUsePerSampleAtmosphericLightTransmittance : 1; 
	// bUsePerSampleAtmosphericLightTransmittance is there on the cloud component and not on the light because otherwise we would need optimisation permutations of the cloud shader.
	// And this for the two atmospheric lights ON or OFF. Keeping it simple for now because this changes the look of the cloud, so it is an art/look decision.

	/** Occlude the sky light contribution at the bottom of the cloud layer. This is a fast appoximation to sky lighting being occluded by cloud without having ot trace rays or sample AO texture. Ignored if the cloud material explicitely sets the ambient occlusion value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Tracing", meta = (UIMin = 0.0f, UIMax = 1.0f, ClampMin = 0.0f, ClampMax = 1.0f))
	float SkyLightCloudBottomOcclusion;

	/**
	 * Scale the view tracing sample count. Quality level scalability CVARs affect the maximum range.
	 * The sample count resolution is still clamped according to scalability setting to 'r.VolumetricCloud.ViewRaySampleCountMax'.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cloud Tracing", AdvancedDisplay, meta = (UIMin = "0.25", UIMax = "8", ClampMin = "0.25", SliderExponent = 3.0))
	float ViewSampleCountScale;
	/**
	 * Scale the view tracing sample count. Quality level scalability CVARs affect the maximum range.
	 * The sample count resolution is still clamped according to scalability setting to 'r.VolumetricCloud.ReflectionRaySampleMaxCount'.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cloud Tracing", AdvancedDisplay, meta = (UIMin = "0.25", UIMax = "8", ClampMin = "0.25", SliderExponent = 3.0))
	float ReflectionSampleCountScale;

	/**
	 * Scale the shadow tracing sample count. Quality level scalability CVARs affect the maximum range.
	 * The sample count resolution is still clamped according to scalability setting to 'r.VolumetricCloud.Shadow.ViewRaySampleMaxCount'.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cloud Tracing", AdvancedDisplay, meta = (UIMin = "0.25", UIMax = "8", ClampMin = "0.25", SliderExponent = 3.0))
	float ShadowViewSampleCountScale;
	/**
	 * Scale the shadow tracing sample count. Quality level scalability CVARs affect the maximum range.
	 * The sample count resolution is still clamped according to scalability setting to 'r.VolumetricCloud.Shadow.ReflectionRaySampleMaxCount'.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cloud Tracing", AdvancedDisplay, meta = (UIMin = "0.25", UIMax = "8", ClampMin = "0.25", SliderExponent = 3.0))
	float ShadowReflectionSampleCountScale;

	/**
	 * The shadow tracing distance in kilometers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cloud Tracing", AdvancedDisplay, meta = (UIMin = "0.1", UIMax = "50", ClampMin = "0.01", SliderExponent = 3.0))
	float ShadowTracingDistance;

protected:
	//~ Begin UActorComponent Interface.
	virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	virtual void DestroyRenderState_Concurrent() override;
	//~ End UActorComponent Interface.

public:

	//~ Begin UObject Interface. 
	virtual void PostInterpChange(FProperty* PropertyThatChanged) override;
	virtual void Serialize(FArchive& Ar) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif // WITH_EDITOR
	//~ End UObject Interface

	//~ Begin UActorComponent Interface.
#if WITH_EDITOR
	virtual void CheckForErrors() override;
#endif // WITH_EDITOR
	//~ End UActorComponent Interface.

private:

	FVolumetricCloudSceneProxy* VolumetricCloudSceneProxy;

};


/**
 * A placeable actor that represents a participating media material around a planet, e.g. clouds.
 * @see TODO address to the documentation.
 */
UCLASS(showcategories = (Movement, Rendering, "Utilities|Transformation", "Input|MouseInput", "Input|TouchInput"), ClassGroup = Fog, hidecategories = (Info, Object, Input), MinimalAPI)
class AVolumetricCloud : public AInfo
{
	GENERATED_UCLASS_BODY()

private:

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = Atmosphere, meta = (AllowPrivateAccess = "true"))
	class UVolumetricCloudComponent* VolumetricCloudComponent;

public:

};
