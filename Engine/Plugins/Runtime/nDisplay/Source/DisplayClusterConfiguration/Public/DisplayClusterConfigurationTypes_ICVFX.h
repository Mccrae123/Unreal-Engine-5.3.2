// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"

#include "Components/ActorComponent.h"
#include "ActorLayerUtilities.h"

#include "DisplayClusterConfigurationTypes_Viewport.h"

#include "DisplayClusterConfigurationTypes_ICVFX.generated.h"

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_VisibilityList
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	TArray<FActorLayer> ActorLayers;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	TArray<AActor*> Actors;

	//@todo change link, now by names
	// Reference to RootActor components by names
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	TArray<FString> RootActorComponentNames;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_ChromakeyMarkers
{
	GENERATED_BODY()

public:
	// Allow shromakey markers rendering (Also require not empty MarkerTileRGBA)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bEnable = true;

	// (*required) This texture must by tiled in both directions. Alpha channel used to composing
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	UTexture2D* MarkerTileRGBA = nullptr;

	// Scale markers UV source
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float MarkerTileScale = 1;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float MarkerTileDistance = 0;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_OverlayAdvancedRenderSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//FDisplayClusterConfigurationICVFX_OverlayAdvancedRenderSettings();

public:
	// Allow ScreenPercentage 
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX", meta = (ClampMin = "0.05", UIMin = "0.05", ClampMax = "10.0", UIMax = "10.0"))
	float BufferRatio = 1;

	// Performance: Render to scale RTT, resolved with shader to viewport (Custom value)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float RenderTargetRatio = 1;

	// Performance, Multi-GPU: Asign GPU for viewport rendering. The Value '-1' used to default gpu mapping (EYE_LEFT and EYE_RIGHT GPU)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int GPUIndex = -1;

	// Performance, Multi-GPU: Customize GPU for stereo mode second view (EYE_RIGHT GPU)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int StereoGPUIndex = -1;

	// Performance: force monoscopic render, resolved to stereo viewport
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationViewport_StereoMode StereoMode = EDisplayClusterConfigurationViewport_StereoMode::Default;

	// Experimental: Support special frame builder mode - merge viewports to single viewfamily by group num
	// [not implemented yet]
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int RenderFamilyGroup = -1;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_ChromakeyRenderSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_ChromakeyRenderSettings();

public:
	// Debug: override the texture of the camera viewport from this chromakey RTT
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bOverrideCameraViewport = false;

	// Render actors from this layers to chromakey texture
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_VisibilityList ShowOnlyList;

	// Override viewport render from source texture
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_Override Override;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_BlurPostprocess PostprocessBlur;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_GenerateMips GenerateMips;

	// Advanced render settings
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_OverlayAdvancedRenderSettings AdvancedRenderSettings;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_ChromakeySettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//FDisplayClusterConfigurationICVFX_ChromakeySettings();

public:
	// Allow chromakey rendering (also require not empty ChromakeyLayers)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationICVFX_ChromakeySource Source = EDisplayClusterConfigurationICVFX_ChromakeySource::None;

	// Color to fill camera frame
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FLinearColor ChromakeyColor = FLinearColor::Green;

	// Settings for chromakey texture source rendering
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_ChromakeyRenderSettings ChromakeyRenderTexture;

	// Global setup for chromakey markers rendering
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_ChromakeyMarkers ChromakeyMarkers;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_LightcardRenderSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_LightcardRenderSettings();

public:
	// Debug: override the texture of the target viewport from this lightcard RTT
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bOverrideViewport = false;

	// Override viewport render from source texture
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_Override Override;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_BlurPostprocess PostprocessBlur;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_GenerateMips GenerateMips;

	// Advanced render settings
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_OverlayAdvancedRenderSettings AdvancedRenderSettings;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_LightcardSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_LightcardSettings();

public:
	// Allow lightcard rendering (also require not empty LightCardLayers)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bEnable = true;

	// Global lighcard rendering mode
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationICVFX_LightcardRenderMode Blendingmode = EDisplayClusterConfigurationICVFX_LightcardRenderMode::Under;

	// Render actors from this layers to lightcard textures
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_VisibilityList ShowOnlyList;

	// Configure global render settings for this viewports
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_LightcardRenderSettings RenderSettings;

	// OCIO Display look configuration 
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FOpenColorIODisplayConfiguration OCIO_Configuration;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_CameraFrameSize
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	FDisplayClusterConfigurationICVFX_CameraFrameSize()
		: CustomSizeValue(2560, 1440)
	{ }

public:
	// Camera frame size value source
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationICVFX_CameraFrameSizeSource Size = EDisplayClusterConfigurationICVFX_CameraFrameSizeSource::Default;

	// Frame size for this camera, used when selected "Custom size value"
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FIntPoint CustomSizeValue;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_CameraAdvancedRenderSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_CameraAdvancedRenderSettings();

public:
	// Performance: Render to scale RTT, resolved with shader to viewport (Custom value)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float RenderTargetRatio = 1;

	// Performance, Multi-GPU: Asign GPU for viewport rendering. The Value '-1' used to default gpu mapping (EYE_LEFT and EYE_RIGHT GPU)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int GPUIndex = -1;

	// Performance, Multi-GPU: Customize GPU for stereo mode second view (EYE_RIGHT GPU)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int StereoGPUIndex = -1;

	// Performance: force monoscopic render, resolved to stereo viewport
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationViewport_StereoMode StereoMode = EDisplayClusterConfigurationViewport_StereoMode::Default;

	// Experimental: Support special frame builder mode - merge viewports to single viewfamily by group num
	// [not implemented yet]
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int RenderFamilyGroup = -1;
};

USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_CameraRenderSettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_CameraRenderSettings();

public:
	// Define camera RTT texture size
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_CameraFrameSize FrameSize;

	// Camera render order, bigger value is over
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	int RenderOrder = -1;

	// Override viewport render from source texture
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_Override Override;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_BlurPostprocess PostprocessBlur;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationPostRender_GenerateMips GenerateMips;

	// Advanced render settings
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_CameraAdvancedRenderSettings AdvancedRenderSettings;
};


USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_CameraCustomChromakeySettings
	// : public UDisplayClusterConfigurationData_Base
{
	GENERATED_BODY()

public:
	//UDisplayClusterConfigurationICVFX_CameraCustomChromakeySettings();

public:
	// Allow use local settings for chromakey and markers
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bEnable = false;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_ChromakeySettings Chromakey;
};



USTRUCT()
struct DISPLAYCLUSTERCONFIGURATION_API FDisplayClusterConfigurationICVFX_CameraMotionBlur
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	EDisplayClusterConfigurationCameraMotionBlurMode MotionBlurMode = EDisplayClusterConfigurationCameraMotionBlurMode::Off;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float TranslationScale = 1.f;

	// GUI: Add ext camera refs
};


UCLASS(ClassGroup = (DisplayClusterICVFX), meta = (BlueprintSpawnableComponent))
class DISPLAYCLUSTERCONFIGURATION_API UDisplayClusterConfigurationICVFX_CameraSettings
	 : public UActorComponent
{
	GENERATED_BODY()

public:
	UDisplayClusterConfigurationICVFX_CameraSettings();

public:
	// Enable this camera
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bEnable = true;

	// Allow ScreenPercentage, for values!=1
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX", meta = (ClampMin = "0.05", UIMin = "0.05", ClampMax = "10.0", UIMax = "10.0"))
	float BufferRatio = 1;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	float FieldOfViewMultiplier = 1.0f;

	// Basic soft edges setup for incamera
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FVector  SoftEdge = FVector::ZeroVector;

	// Rotate incamera frustum on this value to fix broken lens on physic camera
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FRotator  FrustumRotation = FRotator::ZeroRotator;

	// Move incamera frustum on this value to fix broken lens on physic camera
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FVector FrustumOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_CameraMotionBlur CameraMotionBlur;

	// Configure global render settings for this viewports
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_CameraRenderSettings RenderSettings;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_CameraCustomChromakeySettings CustomChromakey;

	// OCIO Display look configuration 
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FOpenColorIODisplayConfiguration OCIO_Configuration;
};

UCLASS(ClassGroup = (DisplayClusterICVFX), meta = (BlueprintSpawnableComponent))
class  DISPLAYCLUSTERCONFIGURATION_API UDisplayClusterConfigurationICVFX_StageSettings
	 : public UActorComponent
{
	GENERATED_BODY()

public:
	UDisplayClusterConfigurationICVFX_StageSettings();

public:
	// Allow ICVFX features
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	bool bEnable = true;

	// Default incameras RTT texture size.
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FIntPoint DefaultFrameSize;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_ChromakeySettings Chromakey;

	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_LightcardSettings Lightcard;

	// Should be to add to this list all defined lightcards and chromakeys layers
	// (This allow to hide all actors from layers for icvfx render logic)
	UPROPERTY(EditAnywhere, Category = "Display Cluster ICVFX")
	FDisplayClusterConfigurationICVFX_VisibilityList HideList;
};

