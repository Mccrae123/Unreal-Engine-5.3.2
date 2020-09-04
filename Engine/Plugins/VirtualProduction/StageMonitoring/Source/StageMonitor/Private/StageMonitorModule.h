// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IStageMonitorModule.h"

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FAutoConsoleCommand;
class FStageMonitor;

DECLARE_LOG_CATEGORY_EXTERN(LogStageMonitor, Log, All);


class FStageMonitorModule : public IStageMonitorModule
{
public:	
	virtual ~FStageMonitorModule() = default;
	
	//~ Begin IModuleInterface interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~End IModuleInterface

	//~Begin IStageMonitorModule interface
	virtual IStageMonitor& GetStageMonitor() override;
	//~End IStageMonitorModule interface

private:

	/** Create StageMonitor when engine is fully initialized */
	void OnEngineLoopInitComplete();

	/** Starts monitor */
	void StartMonitor();

	/** Stops monitor */
	void StopMonitor();

protected:
	
	/** Single instance of the StageMonitor */
	TUniquePtr<FStageMonitor> StageMonitor;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	TUniquePtr<FAutoConsoleCommand> CommandStart;
	TUniquePtr<FAutoConsoleCommand> CommandStop;
#endif
};
