// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/AssetUserData.h"

#include "CoreMinimal.h"
#include "LensData.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Models/LensModel.h"

#include "LensDistortionModelHandlerBase.generated.h"

USTRUCT(BlueprintType)
struct CAMERACALIBRATION_API FLensDistortionState
{
	GENERATED_BODY()

public:
	/** Generic array of distortion parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distortion")
	FDistortionInfo DistortionInfo;

	/** Normalized distance from the center of projection to the image plane */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distortion")
	FVector2D FxFy = FVector2D(1.0f, (16.0f / 9.0f));

	/** Normalized center of the image, in the range [0.0f, 1.0f] */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distortion", meta = (DisplayName = "Image Center"))
	FVector2D PrincipalPoint = FVector2D(0.5f, 0.5f);

public:
	bool operator==(const FLensDistortionState& Other) const;
	bool operator!=(const FLensDistortionState& Other) const { return !(*this == Other); }
};

/** Asset user data that can be used on Camera Actors to manage lens distortion state and utilities  */
UCLASS(Abstract)
class CAMERACALIBRATION_API ULensDistortionModelHandlerBase : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Returns true if the input model is supported by this model handler, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Distortion")
	bool IsModelSupported(const TSubclassOf<ULensModel>& ModelToSupport) const;

	/** Update the lens distortion state, recompute the overscan factor, and set all material parameters */
	UFUNCTION(BlueprintCallable, Category = "Distortion")
    void SetDistortionState(const FLensDistortionState& InNewState);

	/** Get the UV displacement map that was drawn during the last call to Update() */
	UFUNCTION(BlueprintCallable, Category = "Distortion")
	UTextureRenderTarget2D* GetUVDisplacementMap() const { return DisplacementMapRT; }

public:
	//~ Begin UObject Interface
	virtual void PostInitProperties() override;

#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(struct FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif	
	//~ End UObject Interface

	/** Get the current distortion state (the lens model and properties that mathematically represent the distortion characteristics */
	FLensDistortionState GetCurrentDistortionState() const { return CurrentState; }

	/** Get the post-process MID for the currently specified lens model */
	UMaterialInstanceDynamic* GetDistortionMID() const { return DistortionPostProcessMID; }

	/** Get the specified lens model that characterizes the distortion effect */
	const TSubclassOf<ULensModel>& GetLensModelClass() const { return LensModelClass; };

	/** Get the normalized center of projection of the image, in the range [0.0f, 1.0f] */
	FVector2D GetPrincipalPoint() const { return CurrentState.PrincipalPoint; }

	/** Get the focal length of the camera, in millimeters */
	FVector2D GetFxFy() const { return CurrentState.FxFy; }

	/** Updates overscan factor and applies to material instances */
	void SetOverscanFactor(float OverscanFactor);

	/** Returns the last overscan factor that was set */
	float GetOverscanFactor() const { return OverscanFactor; }

	/** Use the current distortion state to compute the overscan factor needed such that all distorted UVs will fall into the valid range of [0,1] */
	float ComputeOverscanFactor() const;

	/** Computes the distorted version of UndistortedUVs based on the current state */
	TArray<FVector2D> GetDistortedUVs(TConstArrayView<FVector2D> UndistortedUVs) const;

	/** Draw the displacement map associated with the current state to the DestinationTexture */
	bool DrawDisplacementMap(UTextureRenderTarget2D* DestinationTexture);

	/** Draws the current distortion state to the internal displacement map */
	void ProcessCurrentDistortion();

protected:
	/** Initialize the handler. Derived classes must set the LensModelClass that they support, if not already set */
	virtual void InitializeHandler() PURE_VIRTUAL(ULensDistortionModelHandlerBase::InitializeHandler);

	/** Use the current distortion state to compute the distortion position of an input UV coordinate */
	virtual FVector2D ComputeDistortedUV(const FVector2D& InScreenUV) const PURE_VIRTUAL(ULensDistortionModelHandlerBase::ComputeDistortedUV, return FVector2D::ZeroVector;);

	/** Create the distortion MIDs */
	virtual void InitDistortionMaterials() PURE_VIRTUAL(ULensDistortionModelHandlerBase::InitDistortionMaterials);

	/** Set the material parameters for the displacement map and distortion post-process materials */
	virtual void UpdateMaterialParameters() PURE_VIRTUAL(ULensDistortionModelHandlerBase::UpdateMaterialParameters);

	/** Convert the generic distortion parameter array into the specific structure of parameters used by the supported lens model */
	virtual void InterpretDistortionParameters() PURE_VIRTUAL(ULensDistortionModelHandlerBase::InterpretDistortionParameters);

protected:
	/** Lens Model describing how to interpret the distortion parameters */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Distortion")
	TSubclassOf<ULensModel> LensModelClass;

	/** Dynamically created post-process material instance for the currently specified lens model */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Distortion")
	UMaterialInstanceDynamic* DistortionPostProcessMID = nullptr;

	/** Current state as set by the most recent call to Update() */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Distortion", meta = (ShowOnlyInnerProperties))
	FLensDistortionState CurrentState;

	/** Computed overscan factor needed to scale the camera's FOV (read-only) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Distortion")
	float OverscanFactor = 1.0f;

	/** MID used to draw a UV distortion displacement map to the DisplacementMapRT */
	UPROPERTY(Transient)
	UMaterialInstanceDynamic* DisplacementMapMID = nullptr;

	/** Render Target representing a UV distortion displacement map */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* DisplacementMapRT = nullptr;

private:
	static constexpr uint32 DisplacementMapWidth = 256;
	static constexpr uint32 DisplacementMapHeight = 256;

	/** Tracks whether distortion state has been changed */
	bool bIsDirty = true;
};
