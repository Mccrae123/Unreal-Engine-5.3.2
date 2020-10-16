// Copyright Epic Games, Inc. All Rights Reserved.

#include "Quartz/QuartzSubsystem.h"

#include "Quartz/QuartzMetronome.h"
#include "Quartz/AudioMixerClockManager.h"
#include "Sound/QuartzQuantizationUtilities.h"

#include "AudioDevice.h"
#include "AudioMixerDevice.h"

static int32 MaxQuartzSubscribersToUpdatePerTickCvar = -1;
FAutoConsoleVariableRef CVarMaxQuartzSubscribersToUpdatePerTick(
	TEXT("au.Quartz.MaxSubscribersToUpdatePerTick"),
	MaxQuartzSubscribersToUpdatePerTickCvar,
	TEXT("Limits the number of Quartz subscribers to update per Tick.\n")
	TEXT("<= 0: No Limit, >= 1: Limit"),
	ECVF_Default);


static FAudioDevice* GetAudioDeviceFromWorldContext(const UObject* WorldContextObject)
{
	UWorld* ThisWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!ThisWorld || !ThisWorld->bAllowAudioPlayback || ThisWorld->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	return ThisWorld->GetAudioDeviceRaw();
}


static Audio::FMixerDevice* GetAudioMixerDeviceFromWorldContext(const UObject* WorldContextObject)
{
	if (FAudioDevice* AudioDevice = GetAudioDeviceFromWorldContext(WorldContextObject))
	{
		if (!AudioDevice->IsAudioMixerEnabled())
		{
			return nullptr;
		}
		else
		{
			return static_cast<Audio::FMixerDevice*>(AudioDevice);
		}
	}
	return nullptr;
}


UQuartzSubsystem::UQuartzSubsystem()
{
}

UQuartzSubsystem::~UQuartzSubsystem()
{
}


void UQuartzSubsystem::Tick(float DeltaTime)
{
	const int32 NumSubscribers = QuartzTickSubscribers.Num();

	if (MaxQuartzSubscribersToUpdatePerTickCvar <= 0 || NumSubscribers <= MaxQuartzSubscribersToUpdatePerTickCvar)
	{
		// we can afford to update ALL subscribers
		for (UQuartzClockHandle* Entry : QuartzTickSubscribers)
		{
			if (Entry->QuartzIsTickable())
			{
				Entry->QuartzTick(DeltaTime);
			}
		}

		UpdateIndex = 0;
	}
	else
	{
		// only update up to our limit
		for (int i = 0; i < MaxQuartzSubscribersToUpdatePerTickCvar; ++i)
		{
			if (QuartzTickSubscribers[UpdateIndex]->QuartzIsTickable())
			{
				QuartzTickSubscribers[UpdateIndex]->QuartzTick(DeltaTime);
			}

			if (++UpdateIndex == NumSubscribers)
			{
				UpdateIndex = 0;
			}
		}
	}
}

bool UQuartzSubsystem::IsTickable() const
{
	if (!QuartzTickSubscribers.Num())
	{
		return false;
	}

	for (const UQuartzClockHandle* Entry : QuartzTickSubscribers)
	{
		if (Entry->QuartzIsTickable())
		{
			return true;
		}
	}

	return false;
}

TStatId UQuartzSubsystem::GetStatId() const
{
	return Super::GetStatID();
}


void UQuartzSubsystem::SubscribeToQuartzTick(UQuartzClockHandle* InObjectToTick)
{
	QuartzTickSubscribers.AddUnique(InObjectToTick);
}


void UQuartzSubsystem::UnsubscribeFromQuartzTick(UQuartzClockHandle* InObjectToTick)
{
	QuartzTickSubscribers.RemoveSingleSwap(InObjectToTick);
}


UQuartzSubsystem* UQuartzSubsystem::Get(UWorld* World)
{
	return World->GetSubsystem<UQuartzSubsystem>();
}


TSharedPtr<Audio::FShareableQuartzCommandQueue, ESPMode::ThreadSafe> UQuartzSubsystem::CreateQuartzCommandQueue()
{
	return MakeShared<Audio::FShareableQuartzCommandQueue, ESPMode::ThreadSafe>();
}



Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataDataForSchedulePlaySound(UQuartzClockHandle* InClockHandle, const FOnQuartzCommandEventBP& InDelegate, FQuartzQuantizationBoundary& InQuantizationBoundary)
{
	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = MakeShared<Audio::FQuantizedPlayCommand>();

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadCommandQueue);
	}

	return CommandInitInfo;
}


Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataForTickRateChange(UQuartzClockHandle* InClockHandle, const FOnQuartzCommandEventBP& InDelegate, const FQuartzClockTickRate& InNewTickRate, FQuartzQuantizationBoundary& InQuantizationBoundary)
{
	TSharedPtr<Audio::FQuantizedTickRateChange> TickRateChangeCommandPtr = MakeShared<Audio::FQuantizedTickRateChange>();
	TickRateChangeCommandPtr->SetTickRate(InNewTickRate);

	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = TickRateChangeCommandPtr;

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadCommandQueue);
	}

	return CommandInitInfo;
}

Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataForTransportReset(UQuartzClockHandle* InClockHandle, const FOnQuartzCommandEventBP& InDelegate)
{
	TSharedPtr<Audio::FQuantizedTransportReset> TransportResetCommandPtr = MakeShared<Audio::FQuantizedTransportReset>();

	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = EQuartzCommandQuantization::Bar;
	CommandInitInfo.QuantizedCommandPtr = TransportResetCommandPtr;

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadCommandQueue);
	}

	return CommandInitInfo;
}

UQuartzClockHandle* UQuartzSubsystem::CreateNewClock(const UObject* WorldContextObject, FName ClockName, FQuartzClockSettings InSettings, bool bOverrideSettingsIfClockExists) const
{
	if (ClockName.IsNone())
	{
		return nullptr; // TODO: Create a unique name
	}

	// add or create clock
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return nullptr;
	}

	ClockManager->GetOrCreateClock(ClockName, InSettings, bOverrideSettingsIfClockExists);

	UQuartzClockHandle* ClockHandlePtr = NewObject<UQuartzClockHandle>()->Init(WorldContextObject->GetWorld())->SubscribeToClock(WorldContextObject, ClockName);
	return ClockHandlePtr;
}


UQuartzClockHandle* UQuartzSubsystem::GetHandleForClock(const UObject* WorldContextObject, FName ClockName) const
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return nullptr;
	}

	if (ClockManager->DoesClockExist(ClockName))
	{
		return NewObject<UQuartzClockHandle>()->Init(WorldContextObject->GetWorld())->SubscribeToClock(WorldContextObject, ClockName);
	}

	return nullptr;
}


bool UQuartzSubsystem::DoesClockExist(const UObject* WorldContextObject, FName ClockName) const
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return false;
	}

	return ClockManager->DoesClockExist(ClockName);
}


float UQuartzSubsystem::GetGameThreadToAudioRenderThreadAverageLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetLifetimeAverageLatency();
}


float UQuartzSubsystem::GetGameThreadToAudioRenderThreadMinLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetMinLatency();
}


float UQuartzSubsystem::GetGameThreadToAudioRenderThreadMaxLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetMinLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadAverageLatency()
{
	return GetLifetimeAverageLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadMinLatency()
{
	return GetMinLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadMaxLatency()
{
	return GetMaxLatency();
}


float UQuartzSubsystem::GetRoundTripAverageLatency(const UObject* WorldContextObject)
{
	// very much an estimate
	return GetAudioRenderThreadToGameThreadAverageLatency() + GetGameThreadToAudioRenderThreadAverageLatency(WorldContextObject);
}


float UQuartzSubsystem::GetRoundTripMinLatency(const UObject* WorldContextObject)
{
	return GetAudioRenderThreadToGameThreadMaxLatency() + GetGameThreadToAudioRenderThreadMaxLatency(WorldContextObject);
}


float UQuartzSubsystem::GetRoundTripMaxLatency(const UObject* WorldContextObject)
{
	return GetAudioRenderThreadToGameThreadMinLatency() + GetGameThreadToAudioRenderThreadMinLatency(WorldContextObject);
}


void UQuartzSubsystem::PauseClock(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager || !InClockHandle)
	{
		return;
	}

	ClockManager->PauseClock(InClockHandle->GetClockName());
}


void UQuartzSubsystem::ResumeClock(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager || !InClockHandle)
	{
		return;
	}

	ClockManager->ResumeClock(InClockHandle->GetClockName());
}

void UQuartzSubsystem::ResetTransport(const UObject* WorldContextObject, UQuartzClockHandle*& InClockHandle, const FOnQuartzCommandEventBP& InDelegate)
{
	Audio::FQuartzQuantizedCommandInitInfo Data(CreateDataForTransportReset(InClockHandle, InDelegate));
	AddCommandToClock(WorldContextObject, Data);
}

void UQuartzSubsystem::ChangeTickRate(const UObject* WorldContextObject, UQuartzClockHandle*& InClockHandle, const FQuartzClockTickRate& InNewTickRate, FQuartzQuantizationBoundary& InQuantizationBoundary, const FOnQuartzCommandEventBP& InDelegate)
{
	Audio::FQuartzQuantizedCommandInitInfo Data(CreateDataForTickRateChange(InClockHandle, InDelegate, InNewTickRate, InQuantizationBoundary));
	AddCommandToClock(WorldContextObject, Data);
}

void UQuartzSubsystem::AddCommandToClock(const UObject* WorldContextObject, Audio::FQuartzQuantizedCommandInitInfo& InQuantizationCommandInitInfo)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager)
	{
		return;
	}

	ClockManager->AddCommandToClock(InQuantizationCommandInitInfo);
}


void UQuartzSubsystem::SubscribeToQuantizationEvent(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle, EQuartzCommandQuantization InQuantizationBoundary, const FOnQuartzMetronomeEventBP& OnQuantizationEvent)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager || !InClockHandle)
	{
		return;
	}

	InClockHandle->SubscribeToQuantizationEvent(InQuantizationBoundary, OnQuantizationEvent);

	ClockManager->SubscribeToTimeDivision(InClockHandle->GetClockName(), InClockHandle->GetCommandQueue(), InQuantizationBoundary);
}


void UQuartzSubsystem::SubscribeToAllQuantizationEvents(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle, const FOnQuartzMetronomeEventBP& OnQuantizationEvent)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager || !InClockHandle)
	{
		return;
	}

	InClockHandle->SubscribeToAllQuantizationEvents(OnQuantizationEvent);

	ClockManager->SubscribeToAllTimeDivisions(InClockHandle->GetClockName(), InClockHandle->GetCommandQueue());
}


void UQuartzSubsystem::UnsubscribeFromTimeDivision(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle, EQuartzCommandQuantization InQuantizationBoundary)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);
	if (!ClockManager || !InClockHandle)
	{
		return;
	}

	ClockManager->UnsubscribeFromTimeDivision(InClockHandle->GetClockName(), InClockHandle->GetCommandQueue(), InQuantizationBoundary);
}

void UQuartzSubsystem::UnsubscribeFromAllTimeDivisions(const UObject* WorldContextObject, UPARAM(ref) UQuartzClockHandle*& InClockHandle)
{
	if (InClockHandle)
	{
		UnsubscribeFromAllTimeDivisionsInternal(WorldContextObject, *InClockHandle);
	}
}

void UQuartzSubsystem::UnsubscribeFromAllTimeDivisionsInternal(const UObject* WorldContextObject, UQuartzClockHandle& InClockHandle)
{
	Audio::FQuartzClockManager* ClockManager = GetClockManager(WorldContextObject);

	if (!ClockManager)
	{
		ClockManager->UnsubscribeFromAllTimeDivisions(InClockHandle.GetClockName(), InClockHandle.GetCommandQueue());
	}
}

Audio::FQuartzClockManager* UQuartzSubsystem::GetClockManager(const UObject* WorldContextObject) const
{
	Audio::FMixerDevice* MixerDevice = GetAudioMixerDeviceFromWorldContext(WorldContextObject);

	if (MixerDevice)
	{
		return &MixerDevice->QuantizedEventClockManager;
	}

	return nullptr;
}
