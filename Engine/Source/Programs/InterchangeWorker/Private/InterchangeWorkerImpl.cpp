// Copyright Epic Games, Inc. All Rights Reserved.

#include "InterchangeWorkerImpl.h"

#include "InterchangeCommands.h"
#include "InterchangeDispatcherConfig.h"
#include "InterchangeDispatcherTask.h"

#if PLATFORM_WINDOWS && PLATFORM_64BITS
#include "InterchangeFbxParser.h"
#endif

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

using namespace InterchangeDispatcher;


FInterchangeWorkerImpl::FInterchangeWorkerImpl(int32 InServerPID, int32 InServerPort, FString& InResultFolder)
	: ServerPID(InServerPID)
	, ServerPort(InServerPort)
	, PingStartCycle(0)
	, ResultFolder(InResultFolder)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FPaths::NormalizeDirectoryName(ResultFolder);
	if (!PlatformFile.DirectoryExists(*ResultFolder))
	{
		PlatformFile.CreateDirectory(*ResultFolder);
	}
}

bool FInterchangeWorkerImpl::Run()
{
	UE_LOG(LogInterchangeWorker, Verbose, TEXT("connect to %d..."), ServerPort);
	bool bConnected = NetworkInterface.Connect(TEXT("Interchange Worker"), ServerPort, Config::ConnectTimeout_s);
	UE_LOG(LogInterchangeWorker, Verbose, TEXT("connected to %d %s"), ServerPort, bConnected ? TEXT("OK") : TEXT("FAIL"));
	if (bConnected)
	{
		CommandIO.SetNetworkInterface(&NetworkInterface);
	}
	else
	{
		UE_LOG(LogInterchangeWorker, Error, TEXT("Server connection failure. exit"));
		return false;
	}

	InitiatePing();

	bool bIsRunning = true;
	while (bIsRunning)
	{
		if (TSharedPtr<ICommand> Command = CommandIO.GetNextCommand(1.0))
		{
			switch(Command->GetType())
			{
				case ECommandId::Ping:
					ProcessCommand(*StaticCast<FPingCommand*>(Command.Get()));
					break;

				case ECommandId::BackPing:
					ProcessCommand(*StaticCast<FBackPingCommand*>(Command.Get()));
					break;

				case ECommandId::RunTask:
					ProcessCommand(*StaticCast<FRunTaskCommand*>(Command.Get()));
					break;

				case ECommandId::Terminate:
					UE_LOG(LogInterchangeWorker, Verbose, TEXT("Terminate command received. Exiting."));
					bIsRunning = false;
					break;

				case ECommandId::NotifyEndTask:
				default:
					break;
			}
		}
		else
		{
			if (bIsRunning)
			{
				bIsRunning = ServerPID == 0 ? true : FPlatformProcess::IsApplicationRunning(ServerPID);
				UE_CLOG(!bIsRunning, LogInterchangeWorker, Error, TEXT("Worker failure: server lost"));
			}
		}
	}

	UE_CLOG(!bIsRunning, LogInterchangeWorker, Verbose, TEXT("Worker loop exit..."));
	CommandIO.Disconnect(0);
	return true;
}

void FInterchangeWorkerImpl::InitiatePing()
{
	PingStartCycle = FPlatformTime::Cycles64();
	FPingCommand Ping;
	CommandIO.SendCommand(Ping, Config::SendCommandTimeout_s);
}

void FInterchangeWorkerImpl::ProcessCommand(const FPingCommand& PingCommand)
{
	FBackPingCommand BackPing;
	CommandIO.SendCommand(BackPing, Config::SendCommandTimeout_s);
}

void FInterchangeWorkerImpl::ProcessCommand(const FBackPingCommand& BackPingCommand)
{
	if (PingStartCycle)
	{
		double ElapsedTime_s = FGenericPlatformTime::ToSeconds(FPlatformTime::Cycles64() - PingStartCycle);
		UE_LOG(LogInterchangeWorker, Verbose, TEXT("Ping %f s"), ElapsedTime_s);
	}
	PingStartCycle = 0;
}

void FInterchangeWorkerImpl::ProcessCommand(const FRunTaskCommand& RunTaskCommand)
{
	const FString& JsonToProcess = RunTaskCommand.JsonDescription;
	UE_LOG(LogInterchangeWorker, Verbose, TEXT("Process %s"), *JsonToProcess);

	ETaskState ProcessResult = ETaskState::Unknown;
	//Process the json and run the task
	FString JSonResult;
	FString JSonMessages;
	InterchangeDispatcher::FJsonLoadSourceCmd LoadSourceCommand;
	//Any command FromJson function return true if the Json descibe the command
	if (LoadSourceCommand.FromJson(JsonToProcess))
	{
		//Load file command
		if (LoadSourceCommand.GetTranslatorID().Equals(TEXT("FBX"), ESearchCase::IgnoreCase))
		{
			//We want to load an FBX file
			ProcessResult = LoadFbxFile(LoadSourceCommand, JSonResult, JSonMessages);
		}
	}
	//TODO add GetPayload command
	else
	{
		ProcessResult = ETaskState::Unknown;
	}

	FCompletedTaskCommand CompletedTask;
	CompletedTask.ProcessResult = ProcessResult;
	CompletedTask.JSonMessages = JSonMessages;
	if (CompletedTask.ProcessResult == ETaskState::ProcessOk)
	{
		CompletedTask.JSonResult = JSonResult;
	}

	CommandIO.SendCommand(CompletedTask, Config::SendCommandTimeout_s);

	UE_LOG(LogInterchangeWorker, Verbose, TEXT("End of Process %s"), *JsonToProcess);
}

ETaskState FInterchangeWorkerImpl::LoadFbxFile(const InterchangeDispatcher::FJsonLoadSourceCmd& LoadSourceCommand, FString& OutJSonResult, FString& OutJSonMessages) const
{
	ETaskState ResultState = ETaskState::Unknown;
	FString SourceFilename = LoadSourceCommand.GetSourceFilename();
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	InterchangeFbxParser::FbxParser FbxParser;
	FbxParser.LoadFbxFile(SourceFilename, ResultFolder);
	InterchangeDispatcher::FJsonLoadSourceCmd::JsonResultParser ResultParser;
	ResultParser.SetResultFilename(FbxParser.GetResultFilepath());
	OutJSonMessages = FbxParser.GetJsonLoadMessages();
	OutJSonResult = ResultParser.ToJson();
	ResultState = ETaskState::ProcessOk;
#else
	InterchangeDispatcher::FJsonLoadSourceCmd::JsonResultParser ResultParser;
	ResultParser.SetResultFilename(FString());
	OutJSonMessages = TEXT("{\"Msg\" : {\"Type\" : \"Error\",\n\"Msg\" : \"Cannot Execute fbx command on other platform then window 64 bits!\"}}");
	ResultState = ETaskState::ProcessFailed;
#endif
	return ResultState;
}