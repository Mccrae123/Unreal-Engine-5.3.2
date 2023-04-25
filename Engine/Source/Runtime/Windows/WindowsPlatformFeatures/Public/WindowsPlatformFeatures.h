// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PlatformFeatures.h"

class WINDOWSPLATFORMFEATURES_API FWindowsPlatformFeaturesModule : public IPlatformFeaturesModule
{
public:

	/** Creates a new instance of the audio device implemented by the module. */
	FWindowsPlatformFeaturesModule();

	virtual IVideoRecordingSystem* GetVideoRecordingSystem() override;
	virtual class ISaveGameSystem* GetSaveGameSystem() override;

private:
	void StartupModule() override;

};


