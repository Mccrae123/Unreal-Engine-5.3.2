// Copyright Epic Games, Inc. All Rights Reserved.


#include "Sound/SoundEffectPreset.h"
#include "Sound/SoundEffectSource.h"
#include "Engine/Engine.h"
#include "AudioDeviceManager.h"
#include "CoreGlobals.h"
#include "Audio.h"
#include "Async/TaskGraphInterfaces.h" //< Used for FORT-309671. Can be removed when task graph thread info is no longer used for debugging.

USoundEffectPreset::USoundEffectPreset(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bInitialized(false)
{

}

void USoundEffectPreset::Update()
{
	FScopeLock ScopeLock(&InstancesMutationCriticalSection);
	for (int32 i = Instances.Num() - 1; i >= 0; --i)
	{
		TSoundEffectPtr EffectSharedPtr = Instances[i].Pin();
		if (!EffectSharedPtr.IsValid() || EffectSharedPtr->GetPreset() == nullptr)
		{
			Instances.RemoveAtSwap(i, 1);
		}
		else
		{
			RegisterInstance(*this, EffectSharedPtr);
		}
	}
}

void USoundEffectPreset::AddEffectInstance(TSoundEffectPtr& InEffectPtr)
{
	if (!bInitialized)
	{
		bInitialized = true;
		Init();

		// Call the optional virtual function which subclasses can implement if they need initialization
		OnInit();
	}

	FScopeLock ScopeLock(&InstancesMutationCriticalSection);
	Instances.AddUnique(TSoundEffectWeakPtr(InEffectPtr));
}

void USoundEffectPreset::AddReferencedEffects(FReferenceCollector& InCollector)
{
	FReferenceCollector* Collector = &InCollector;
	IterateEffects<FSoundEffectBase>([Collector](FSoundEffectBase& Instance)
	{
		if (const USoundEffectPreset* EffectPreset = Instance.GetPreset())
		{
			Collector->AddReferencedObject(EffectPreset);
		}
	});
}

void USoundEffectPreset::BeginDestroy()
{
	FScopeLock ScopeLock(&InstancesMutationCriticalSection);
	IterateEffects<FSoundEffectBase>([](FSoundEffectBase& Instance)
	{
		Instance.ClearPreset();
	});
	Instances.Reset();

	Super::BeginDestroy();
}

void USoundEffectPreset::RemoveEffectInstance(TSoundEffectPtr& InEffectPtr)
{
	FScopeLock ScopeLock(&InstancesMutationCriticalSection);
	Instances.RemoveSwap(TSoundEffectWeakPtr(InEffectPtr));
}

#if WITH_EDITORONLY_DATA
void USoundEffectPreset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Copy the settings to the thread safe version
	Init();
	OnInit();
	Update();
}

void USoundEffectSourcePresetChain::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (GEngine)
	{
		FAudioDeviceManager* AudioDeviceManager = GEngine->GetAudioDeviceManager();
		AudioDeviceManager->UpdateSourceEffectChain(GetUniqueID(), Chain, bPlayEffectChainTails);
	}
}
#endif // WITH_EDITORONLY_DATA

void USoundEffectSourcePresetChain::AddReferencedEffects(FReferenceCollector& Collector)
{
	for (FSourceEffectChainEntry& SourceEffect : Chain)
	{
		if (SourceEffect.Preset)
		{
			SourceEffect.Preset->AddReferencedEffects(Collector);
		}
	}
}

void USoundEffectPreset::UnregisterInstance(TSoundEffectPtr InEffectPtr)
{
	if (ensure(IsInAudioThread()))
	{
		if (InEffectPtr.IsValid())
		{
			if (USoundEffectPreset* Preset = InEffectPtr->GetPreset())
			{
				Preset->RemoveEffectInstance(InEffectPtr);
			}

			InEffectPtr->ClearPreset();
		}
	}
	else
	{
		// Message added to ensure to get additional debug info - Jira: FORT-309671
		// Logging instead of using ensureMsgf to get info in shipping builds.  
		UE_LOG(LogAudio, Error, TEXT("Attempt to unregister sound effect outside of audio thread. Current thread id: %d. Named thread type: %d. Audio Thread Id: %d. Game Thread Id: %d."), FPlatformTLS::GetCurrentThreadId(), FTaskGraphInterface::Get().GetCurrentThreadIfKnown(), GAudioThreadId, GGameThreadId);
	}
}

void USoundEffectPreset::RegisterInstance(USoundEffectPreset& InPreset, TSoundEffectPtr InEffectPtr)
{
	ensure(IsInAudioThread());
	if (!InEffectPtr.IsValid())
	{
		return;
	}

	if (InEffectPtr->Preset.Get() != &InPreset)
	{
		UnregisterInstance(InEffectPtr);

		InEffectPtr->Preset = &InPreset;
		if (InEffectPtr->Preset.IsValid())
		{
			InPreset.AddEffectInstance(InEffectPtr);
		}
	}

	// Anytime notification occurs that the preset has been modified,
	// flag for update.
	InEffectPtr->bChanged = true;
}

