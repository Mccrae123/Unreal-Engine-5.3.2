// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "DisplayClusterLightCardActor.generated.h"

class USceneComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class UStaticMesh;

UENUM(BlueprintType)
enum class EDisplayClusterLightCardMask : uint8
{
	Circle,
	Square,
	UseTextureAlpha,
	Polygon,
};

USTRUCT(Blueprintable)
struct FLightCardAlphaGradientSettings
{
	GENERATED_BODY()

	/** Enables/disables alpha gradient effect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	bool bEnableAlphaGradient = false;

	/** Starting alpha value in the gradient */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnableAlphaGradient"))
	float StartingAlpha = 0;

	/** Ending alpha value in the gradient */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnableAlphaGradient"))
	float EndingAlpha = 1;

	/** The angle (degrees) determines the gradient direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnableAlphaGradient"))
	float Angle = 0;
};

UCLASS(Blueprintable)
class DISPLAYCLUSTER_API ADisplayClusterLightCardActor : public AActor
{
	GENERATED_BODY()

public:

	struct PositionalParams
	{
		double DistanceFromCenter;
		double Longitude;
		double Latitude;
		double Spin;
		double Pitch;
		double Yaw;
	};

public:
	/** The rotation used to orient the plane mesh used for the light card so that its normal points radially inwards */
	static const FRotator PlaneMeshRotation;

public:
	ADisplayClusterLightCardActor(const FObjectInitializer& ObjectInitializer);

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#endif
	
	/**
	 * Gets the transform in world space of the light card component
	 * @param bIgnoreSpinYawPitch - If the light card component's spin, yaw, and pitch should be ignored when computing the transform
	 */
	FTransform GetLightCardTransform(bool bIgnoreSpinYawPitch = false) const;

	/** Gets the object oriented bounding box of the light card component */
	FBox GetLightCardBounds(bool bLocalSpace = false) const;

	/** Returns the current static mesh used by this light card */
	UStaticMesh* GetStaticMesh() const;

	/** Sets a new static mesh for the light card */
	void SetStaticMesh(UStaticMesh* InStaticMesh);

	/** Updates the Light Card transform based on its positional properties (Lat, Long, etc.) */
	void UpdateLightCardTransform();

	/** Retrieves positional parameters */
	PositionalParams GetPositionalParams();

	/** Set positional parameters */
	void SetPositionalParams(const PositionalParams& Params);

	/** Updates the card's material instance parameters */
	void UpdateLightCardMaterialInstance();

	/** Updates the polygon texture from the polygon points */
	void UpdatePolygonTexture();

public:

	/** Radius of light card polar coordinates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation")
	double DistanceFromCenter;

	/** Related to the Azimuth of light card polar coordinates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation", meta = (UIMin = 0, ClampMin = 0, UIMax = 360, ClampMax = 360))
	double Longitude;

	/** Related to the Elevation of light card polar coordinates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation", meta = (UIMin = -90, ClampMin = -90, UIMax = 90, ClampMax = 90))
	double Latitude;

	/** Roll rotation of light card around its plane axis */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation")
	double Spin;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation")
	double Pitch;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation")
	double Yaw;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orientation")
	FVector2D Scale;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	EDisplayClusterLightCardMask Mask;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	UTexture* Texture;

	/** Light card color, before any modifier is applied */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	FLinearColor Color;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (UIMin = 0, ClampMin = 0, UIMax = 10000, ClampMax = 10000))
	float Temperature;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (UIMin = -1, ClampMin = -1, UIMax = 1, ClampMax = 1))
	float Tint;

	/** 2^Exposure color value multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	float Exposure;

	/** Linear color value multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (UIMin = 0, ClampMin = 0))
	float Gain;

	/** Linear alpha multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (UIMin = 0, ClampMin = 0, UIMax = 1, ClampMax = 1))
	float Opacity;

	/** Feathers in the alpha from the edges */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (UIMin = 0, ClampMin = 0))
	float Feathering;

	/** Settings related to an alpha gradient effect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	FLightCardAlphaGradientSettings AlphaGradient;

	/** A flag that controls wether the light card's location and rotation are locked to its "owning" root actor */
	UPROPERTY()
	bool bLockToOwningRootActor = true;

	/** Used to flag this light card as a proxy of a "real" light card. Used by the LightCard Editor */
	UPROPERTY(Transient)
	bool bIsProxy = false;

	/** Polygon points when using this type of mask */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	TArray<FVector2D> Polygon;

	/** Used to flag this light card as a proxy of a "real" light card. Used by the LightCard Editor */
	UPROPERTY(Transient)
	UTexture* PolygonMask = nullptr;

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Default")
	TObjectPtr<USceneComponent> DefaultSceneRootComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Default")
	TObjectPtr<USpringArmComponent> MainSpringArmComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Default")
	TObjectPtr<USceneComponent> LightCardTransformerComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Default")
	TObjectPtr<UStaticMeshComponent> LightCardComponent;
};