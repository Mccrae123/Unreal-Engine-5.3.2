// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "VirtualCameraUserSettings.generated.h"

/**
 * Virtual Camera User Settings
 */
UCLASS(config=VirtualCamera, BlueprintType)
class VIRTUALCAMERA_API UVirtualCameraUserSettings : public UObject
{
	GENERATED_BODY()

public:

	
	/** Controls interpolation speed when smoothing when changing focus distance. This is used to set the value of FocusSmoothingInterpSpeed in the Virtual camera CineCamera component */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (ClampMin = "1.0", ClampMax = "50.0", DisplayName = "Focus Interpolation Speed"))
	float FocusInterpSpeed = 8.0f;

	/** Controls how fast the camera moves when using joysticks */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Joysticks Speed"))
	float JoysticksSpeed = 0.5f;

	/** Whether the map is displayed using grayscale or full color */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (DisplayName = "Display Map In Grayscale"))
	bool bIsMapGrayscale = true;
	
	/** Whether to change camera lens and fstop when teleporting to a screenshot to those with which the screenshot was taken */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (DisplayName = "Override Camera Settings On Teleporting To Screenshot"))
	bool bOverrideCameraSettingsOnTeleportToScreenshot = true;

	/** Stores the filmback preset name selected by the user */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (DisplayName = "Virtual Camera Filmback"))
	FString VirtualCameraFilmback;

	/** Whether to display film leader when recording a take */
	UPROPERTY(EditAnywhere, config, Category = "VirtualCamera", meta = (DisplayName = "Display Film Leader"))
	bool bDisplayFilmLeader = true;

	/** Get FocusInterpSpeed variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	float GetFocusInterpSpeed();

	/** Set FocusInterpSpeed variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetFocusInterpSpeed(const float InFocusInterpSpeed);

	/** Get JoysticksSpeed variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	float GetJoysticksSpeed();

	/** Set JoysticksSpeed variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetJoysticksSpeed(const float InJoysticksSpeed);

	/** Get bIsMapGrayscale variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	bool IsMapGrayscle();

	/** Set bIsMapGrayscale variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetIsMapGrayscle(const bool bInIsMapGrayscle);

	/** Get bOverrideCameraSettingsOnTeleportToScreenshot variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	bool GetShouldOverrideCameraSettingsOnTeleport();

	/** Set bOverrideCameraSettingsOnTeleportToScreenshot variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetShouldOverrideCameraSettingsOnTeleport(const bool bInOverrideCameraSettings);

	/** Get VirtualCameraFilmback variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	FString GetSavedVirtualCameraFilmbackPresetName();

	/** Set VirtualCameraFilmback variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetSavedVirtualCameraFilmbackPresetName(const FString& InFilmback);

	/** Get bDisplayFilmLeader variable */
	UFUNCTION(BlueprintPure, Category = "VirtualCamera")
	bool GetShouldDisplayFilmLeader();

	/** Set bDisplayFilmLeader variable */
	UFUNCTION(BlueprintCallable, Category = "VirtualCamera")
	void SetShouldDisplayFilmLeader(const bool bInDisplayFilmLeader);

};