// Copyright Epic Games, Inc. All Rights Reserved.

#include "CameraCalibrationSettings.h"

#include "SphericalLensDistortionModelHandler.h"

UCameraCalibrationSettings::UCameraCalibrationSettings()
{
	DefaultDisplacementMaterials.Add(USphericalLensDistortionModelHandler::StaticClass(), TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/CameraCalibration/Materials/M_SphericalDistortionDisplacementMap.M_SphericalDistortionDisplacementMap"))));
	DefaultDistortionMaterials.Add(USphericalLensDistortionModelHandler::StaticClass(), TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/CameraCalibration/Materials/M_DistortionPostProcess.M_DistortionPostProcess"))));
}

FName UCameraCalibrationSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

#if WITH_EDITOR
FText UCameraCalibrationSettings::GetSectionText() const
{
	return NSLOCTEXT("CameraCalibrationPlugin", "CameraCalibrationSettingsSection", "Camera Calibration");
}

#endif

ULensFile* UCameraCalibrationSettings::GetStartupLensFile() const
{
	return StartupLensFile.LoadSynchronous();
}

UMaterialInterface* UCameraCalibrationSettings::GetDefaultDisplacementMaterial(const TSubclassOf<ULensDistortionModelHandlerBase>& InModelHandler) const
{
	const TSoftObjectPtr<UMaterialInterface>* DisplacementMaterial = DefaultDisplacementMaterials.Find(InModelHandler);
	if (!DisplacementMaterial)
	{
		return nullptr;
	}
	return DisplacementMaterial->LoadSynchronous();
}

UMaterialInterface* UCameraCalibrationSettings::GetDefaultDistortionMaterial(const TSubclassOf<ULensDistortionModelHandlerBase>& InModelHandler) const
{
	const TSoftObjectPtr<UMaterialInterface>* DistortionMaterial = DefaultDistortionMaterials.Find(InModelHandler);
	if (!DistortionMaterial)
	{
		return nullptr;
	}
	return DistortionMaterial->LoadSynchronous();
}

FName UCameraCalibrationEditorSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

#if WITH_EDITOR
FText UCameraCalibrationEditorSettings::GetSectionText() const
{
	return NSLOCTEXT("CameraCalibrationEditorPlugin", "CameraCalibrationEditorSettingsSection", "Camera Calibration");
}
#endif

ULensFile* UCameraCalibrationEditorSettings::GetUserLensFile() const
{
#if WITH_EDITOR
	return UserLensFile.LoadSynchronous();
#else
	return nullptr;
#endif
}

void UCameraCalibrationEditorSettings::SetUserLensFile(ULensFile* InLensFile)
{
#if WITH_EDITOR
	UserLensFile = InLensFile;
	SaveConfig();
#endif
}
