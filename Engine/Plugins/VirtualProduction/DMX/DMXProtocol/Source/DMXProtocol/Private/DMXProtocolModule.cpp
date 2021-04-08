// Copyright Epic Games, Inc. All Rights Reserved.

#include "DMXProtocolModule.h"

#include "DMXProtocolLog.h"
#include "DMXProtocolSettings.h"
#include "Interfaces/IDMXProtocol.h"
#include "Interfaces/IDMXProtocolFactory.h"
#include "IO/DMXPortManager.h"

#if WITH_EDITOR
#include "ISettingsModule.h"
#endif // WITH_EDITOR

IMPLEMENT_MODULE( FDMXProtocolModule, DMXProtocol );


#define LOCTEXT_NAMESPACE "DMXProtocolModule"


const int32 FDMXProtocolModule::NumProtocols = 2;

FDMXProtocolModule::FDMXProtocolModule()
	: NumRegisteredProtocols(0)
{}

void FDMXProtocolModule::RegisterProtocol(const FName& FactoryName, IDMXProtocolFactory* Factory)
{
	if (!DMXProtocolFactories.Contains(FactoryName))
	{
		// Increment registred protocol counter
		NumRegisteredProtocols++;

		// If this check is, please change the NumProtocols variable to match the number of protocol implementations
		check(NumRegisteredProtocols <= NumProtocols);

		// Add a factory for the protocol
		DMXProtocolFactories.Add(FactoryName, Factory);
	}
	else
	{
		UE_LOG_DMXPROTOCOL(Verbose, TEXT("Trying to add existing protocol %s"), *FactoryName.ToString());
	}

	// Broadcast protocol registered when all protocols are registered
	if (NumRegisteredProtocols == NumProtocols)
	{
		OnProtocolsRegisteredDelegate.Broadcast();
	}
}

void FDMXProtocolModule::UnregisterProtocol(const FName& FactoryName)
{
	if (DMXProtocolFactories.Contains(FactoryName))
	{
		// Decrement the registered protocol counter
		NumRegisteredProtocols--;

		// Destroy the factory and shut down the protocol
		DMXProtocolFactories.Remove(FactoryName);
		ShutdownDMXProtocol(FactoryName);
	}
	else
	{
		UE_LOG_DMXPROTOCOL(Verbose, TEXT("Trying to remove unexisting protocol %s"), *FactoryName.ToString());
	}
}

FDMXProtocolModule& FDMXProtocolModule::Get()
{
	return FModuleManager::GetModuleChecked<FDMXProtocolModule>("DMXProtocol");
}

IDMXProtocolPtr FDMXProtocolModule::GetProtocol(const FName InProtocolName)
{
	IDMXProtocolPtr* DMXProtocolPtr = nullptr;
	if (!InProtocolName.IsNone())
	{
		DMXProtocolPtr = DMXProtocols.Find(InProtocolName);
		if (DMXProtocolPtr == nullptr)
		{
			IDMXProtocolFactory** DMXProtocolFactory = DMXProtocolFactories.Find(InProtocolName);

			if (DMXProtocolFactory != nullptr)
			{
				UE_LOG_DMXPROTOCOL(Log, TEXT("Creating protocol instance for: %s"), *InProtocolName.ToString());

				IDMXProtocolPtr NewProtocol = (*DMXProtocolFactory)->CreateProtocol(InProtocolName);
				if (NewProtocol.IsValid())
				{
					DMXProtocols.Add(InProtocolName, NewProtocol);
					DMXProtocolPtr = DMXProtocols.Find(InProtocolName);
				}
				else
				{
					bool* bNotedPreviously = DMXProtocolFailureNotes.Find(InProtocolName);
					if (!bNotedPreviously || !(*bNotedPreviously))
					{
						UE_LOG_DMXPROTOCOL(Verbose, TEXT("Unable to create Protocol %s"), *InProtocolName.ToString());
						DMXProtocolFailureNotes.Add(InProtocolName, true);
					}
				}
			}
		}
	}

	return (DMXProtocolPtr == nullptr) ? nullptr : *DMXProtocolPtr;
}

const TMap<FName, IDMXProtocolFactory*>& FDMXProtocolModule::GetProtocolFactories() const
{
	return DMXProtocolFactories;
}

const TMap<FName, IDMXProtocolPtr>& FDMXProtocolModule::GetProtocols() const
{
	return DMXProtocols;
}

void FDMXProtocolModule::StartupModule()
{
	// Deffer setup to after all protocols begin registered
	OnProtocolsRegisteredDelegate.AddRaw(this, &FDMXProtocolModule::OnProtocolsRegistered);
}

void FDMXProtocolModule::ShutdownModule()
{
	OnProtocolsRegisteredDelegate.RemoveAll(this);

	FDMXPortManager::ShutdownManager();

	// Now Shutdown the protocols
	ShutdownAllDMXProtocols();

#if WITH_EDITOR
	// Unregister DMX Protocol global settings
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "DMX Protocol");
	}
#endif // WITH_EDITOR
}

void FDMXProtocolModule::OnProtocolsRegistered()
{
#if WITH_EDITOR
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");

	// Register DMX Protocol global settings
	if (SettingsModule != nullptr)
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "DMX Plugin",
			LOCTEXT("ProjectSettings_Label", "DMX Plugin"),
			LOCTEXT("ProjectSettings_Description", "Configure DMX plugin global settings"),
			GetMutableDefault<UDMXProtocolSettings>()
		);
	}
#endif // WITH_EDITOR

	// Start the port manager after settings are registered, so it can create its default ports from that
	FDMXPortManager::StartupManager();
}

void FDMXProtocolModule::ShutdownDMXProtocol(const FName& ProtocolName)
{
	if (!ProtocolName.IsNone())
	{
		IDMXProtocolPtr DMXProtocol;
		DMXProtocols.RemoveAndCopyValue(ProtocolName, DMXProtocol);
		if (DMXProtocol.IsValid())
		{
			DMXProtocol->Shutdown();
		}
		else
		{
			UE_LOG_DMXPROTOCOL(Verbose, TEXT("DMXProtocol instance %s not found, unable to destroy."), *ProtocolName.ToString());
		}
	}
}

void FDMXProtocolModule::ShutdownAllDMXProtocols()
{
	for (TMap<FName, IDMXProtocolPtr>::TIterator It = DMXProtocols.CreateIterator(); It; ++It)
	{
		It->Value->Shutdown();
	}
}

#undef LOCTEXT_NAMESPACE
