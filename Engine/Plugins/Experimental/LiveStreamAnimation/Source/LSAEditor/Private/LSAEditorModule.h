// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"

class FLSAEditorModule : public IModuleInterface
{
public:

	FLSAEditorModule() = default;
	virtual ~FLSAEditorModule() = default;

	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(GetModuleName());
	}

protected:

	static FName GetModuleName()
	{
		static FName ModuleName = FName(TEXT("LSAEditor"));
		return ModuleName;
	}
};