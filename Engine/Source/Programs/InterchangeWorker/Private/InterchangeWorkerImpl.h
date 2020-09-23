// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "InterchangeWorker.h"

#include "InterchangeCommands.h"
#include "InterchangeDispatcherNetworking.h"

#include "HAL/PlatformTime.h"


struct FFileStatData;
struct FImportParameters;

class FInterchangeWorkerImpl
{
public:
	FInterchangeWorkerImpl(int32 InServerPID, int32 InServerPort, FString& InResultFolder);
	bool Run();

private:
	void InitiatePing();
	void ProcessCommand(const UE::Interchange::FPingCommand& PingCommand);
	void ProcessCommand(const UE::Interchange::FBackPingCommand& BackPingCommand);
	void ProcessCommand(const UE::Interchange::FRunTaskCommand& TerminateCommand);

	UE::Interchange::ETaskState LoadFbxFile(const UE::Interchange::FJsonLoadSourceCmd& LoadSourceCommand, FString& OutJSonResult, TArray<FString>& OutJSonMessages) const;

private:
	UE::Interchange::FNetworkClientNode NetworkInterface;
	UE::Interchange::FCommandQueue CommandIO;

	int32 ServerPID;
	int32 ServerPort;
	uint64 PingStartCycle;
	FString ResultFolder;
};
