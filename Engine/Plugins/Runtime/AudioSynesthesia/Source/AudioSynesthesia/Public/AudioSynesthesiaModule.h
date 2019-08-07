// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "LoudnessNRTFactory.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAudioSynesthesia, Log, All);

namespace Audio
{
	class AUDIOSYNESTHESIA_API FAudioSynesthesiaModule : public IModuleInterface
	{
	public:

		// IModuleInterface interface
		virtual void StartupModule() override;
		virtual void ShutdownModule() override;

	private:

		FLoudnessNRTFactory LoudnessNRTFactory;
	};
}
