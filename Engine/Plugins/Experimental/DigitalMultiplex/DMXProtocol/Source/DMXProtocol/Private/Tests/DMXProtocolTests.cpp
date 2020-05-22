// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Interfaces/IDMXProtocolFactory.h"
#include "DMXProtocolCommon.h"
#include "Dom/JsonObject.h"

#include "Interfaces/IDMXProtocol.h"
#include "Interfaces/IDMXProtocolUniverse.h"
#include "DMXProtocolTypes.h"

class FDMXProtocolTest
	: public IDMXProtocol
{
public:
	//~ Begin IDMXProtocolBase implementation
	virtual bool Init() override { return true; }
	virtual bool Shutdown() override { return true; }
	virtual bool Tick(float DeltaTime) override { return true; }
	//~ End IDMXProtocolBase implementation

	//~ Begin IDMXProtocol implementation
	virtual const FName& GetProtocolName() const override { return ProtocolName;  }
	virtual TSharedPtr<FJsonObject> GetSettings() const override { return Settings; }
	virtual TSharedPtr<IDMXProtocolSender> GetSenderInterface() const override { return nullptr; }
	virtual EDMXSendResult SendDMXFragment(uint16 UniverseID, const IDMXFragmentMap& DMXFragment) override { return EDMXSendResult::Success; }
	virtual EDMXSendResult SendDMXFragmentCreate(uint16 InUniverseID, const IDMXFragmentMap& DMXFragment) override { return EDMXSendResult::Success; }
	virtual uint16 GetFinalSendUniverseID(uint16 InUniverseID) const override { return InUniverseID; }
	virtual bool IsEnabled() const override { return true; }
	virtual TSharedPtr<IDMXProtocolUniverse, ESPMode::ThreadSafe> AddUniverse(const FJsonObject& InSettings) override { return nullptr; }
	virtual void UpdateUniverse(uint32 InUniverseId, const FJsonObject& InSetting) override {}
	virtual void CollectUniverses(const TArray<FDMXUniverse>& Universes) override {}
	virtual bool RemoveUniverseById(uint32 InUniverseId) override { return true; }
	virtual void RemoveAllUniverses() override { }
	virtual TSharedPtr<IDMXProtocolUniverse, ESPMode::ThreadSafe> GetUniverseById(uint32 InUniverseId) const override { return nullptr; }
	virtual uint32 GetUniversesNum() const override { return 0; }
	virtual uint16 GetMinUniverseID() const override { return 0; }
	virtual uint16 GetMaxUniverses() const override { return 1; }
	virtual void GetDefaultUniverseSettings(uint16 InUniverseID, FJsonObject& OutSettings) const {}
	virtual FOnUniverseInputUpdateEvent& GetOnUniverseInputUpdate() override { return OnUniverseInputUpdateEvent; }
	virtual FOnUniverseOutputSentEvent& GetOnOutputSentEvent() override
	{
		return OnUniverseOutputSentEvent;
	}
	//~ End IDMXProtocol implementation

	//~ Begin IDMXProtocolRDM implementation
	virtual void SendRDMCommand(const TSharedPtr<FJsonObject>& CMD) override {}
	virtual void RDMDiscovery(const TSharedPtr<FJsonObject>& CMD) override {}
	//~ End IDMXProtocol implementation

	//~ Only the factory makes instances
	FDMXProtocolTest() = delete;
	explicit FDMXProtocolTest(FName InProtocolName, FJsonObject& InSettings)
		: ProtocolName(InProtocolName)
	{
		Settings = MakeShared<FJsonObject>(InSettings);
	}

private:
	FName ProtocolName;
	TSharedPtr<FJsonObject> Settings;
	FOnUniverseInputUpdateEvent OnUniverseInputUpdateEvent;
	FOnUniverseOutputSentEvent OnUniverseOutputSentEvent;
};


/**
 */
class FDMXProtocolFactoryTestFactory : public IDMXProtocolFactory
{
public:
	virtual IDMXProtocolPtr CreateProtocol(const FName& ProtocolName) override
	{
		FJsonObject ProtocolSettings;
		IDMXProtocolPtr ProtocolArtNetPtr = MakeShared<FDMXProtocolTest, ESPMode::ThreadSafe>(ProtocolName, ProtocolSettings);
		if (ProtocolArtNetPtr->IsEnabled())
		{
			if (!ProtocolArtNetPtr->Init())
			{
				UE_LOG_DMXPROTOCOL(Verbose, TEXT("TEST Protocol failed to initialize!"));
				ProtocolArtNetPtr->Shutdown();
				ProtocolArtNetPtr = nullptr;
			}
		}
		else
		{
			UE_LOG_DMXPROTOCOL(Verbose, TEXT("TEST Protocol disabled!"));
			ProtocolArtNetPtr->Shutdown();
			ProtocolArtNetPtr = nullptr;
		}

		return ProtocolArtNetPtr;
	}
};

namespace DMXProtocolTestHelper
{
	static const FName NAME_ArtnetTest = TEXT("ARTNET_TEST");
	static const FName NAME_SACNTest = TEXT("SACN_TEST");

	void GetDMXProtocolNamesForTesting(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands, const FString& PostTestName = TEXT(""))
	{
		TArray< FName > DMXProtocolList;
		DMXProtocolList.AddUnique(NAME_ArtnetTest);
		DMXProtocolList.AddUnique(NAME_SACNTest);

		for (FName& DMXProtocolName : DMXProtocolList)
		{
			FString PostName = FString::Printf(TEXT(".%s"), *PostTestName);
			FString PrettyName = FString::Printf(TEXT("%s%s"),
				*DMXProtocolName.ToString(),
				PostTestName.IsEmpty() ? TEXT("") : *PostName);
			OutBeautifiedNames.Add(PrettyName);
			OutTestCommands.Add(DMXProtocolName.ToString());
		}
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FDMXProtocolFactoryTestCommand, FName, ProtocolName);

bool FDMXProtocolFactoryTestCommand::Update()
{
	//UE_LOG(LogTemp, Warning, TEXT("ProtocolName '%s'"), *ProtocolName.ToString());

	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDMXProtocolFactoryTest, "VirtualProduction.DMX.Protocol.Factory", (EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter))

void FDMXProtocolFactoryTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	DMXProtocolTestHelper::GetDMXProtocolNamesForTesting(
		OutBeautifiedNames,
		OutTestCommands,
		TEXT("Functional test of the Protocol factory")
	);
}

bool FDMXProtocolFactoryTest::RunTest(const FString& Parameters)
{
	// parameter is the provider we want to use
	FName ProtocolName = FName(*Parameters);
	ADD_LATENT_AUTOMATION_COMMAND(FDMXProtocolFactoryTestCommand(ProtocolName));

	FDMXProtocolModule& DMXProtocolModule = FModuleManager::GetModuleChecked<FDMXProtocolModule>("DMXProtocol");

	// Store the protocol pointer
	IDMXProtocolPtr CachedProtocol = nullptr;

	// Try to register 3 times
	TArray<TUniquePtr<IDMXProtocolFactory>> Factories;
	Factories.Add(MakeUnique<FDMXProtocolFactoryTestFactory>());
	Factories.Add(MakeUnique<FDMXProtocolFactoryTestFactory>());
	Factories.Add(MakeUnique<FDMXProtocolFactoryTestFactory>());
	for (int32 Index = 0; Index < Factories.Num(); Index++)
	{
		// Create and register our singleton factory with the main online subsystem for easy access
		DMXProtocolModule.RegisterProtocol(ProtocolName, Factories[Index].Get());
		if (CachedProtocol == nullptr)
		{
			CachedProtocol = IDMXProtocol::Get(ProtocolName);
		}

		TestTrue(TEXT("Protocol should exists"), IDMXProtocol::Get(ProtocolName).IsValid());
		TestEqual(TEXT("Should return same protocol instance"), CachedProtocol, IDMXProtocol::Get(ProtocolName));
	}
	Factories.Empty();

	// Protocol removal test
	{
		TUniquePtr<IDMXProtocolFactory> Factory = MakeUnique<FDMXProtocolFactoryTestFactory>();
		DMXProtocolModule.RegisterProtocol(ProtocolName, Factory.Get());
		DMXProtocolModule.UnregisterProtocol(ProtocolName);
		TestFalse(TEXT("Protocol should not exists"), IDMXProtocol::Get(ProtocolName).IsValid());
	}


	return true;
}

namespace DMXProtocolTransportTestHelper
{
	static const FName NAME_ArtnetProtocol = TEXT("Art-Net");
	static const FName NAME_SACNProtocol = TEXT("sACN");

	struct ReceiveData
	{
		FName ProtocolName;
		uint16 UniverseID;
		TArray<uint8> Packet;

		ReceiveData() {}

		ReceiveData(FName InProtocolName, uint16 InUniverseID, const TArray<uint8>& InPacket) :
			ProtocolName(InProtocolName),
			UniverseID(InUniverseID)
		{
			Packet = InPacket;
		}
	};

	TQueue<ReceiveData> ReceiveQueue;

	static void ReceiveFragment(FName InProtocolName, uint16 InUniverseID, const TArray<uint8>& InDMXData)
	{
		ReceiveQueue.Enqueue(ReceiveData(InProtocolName, InUniverseID, InDMXData));
	}

	void GetDMXProtocolNamesForTesting(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands, const FString& PostTestName = TEXT(""))
	{
		TArray< FName > DMXProtocolList;
		DMXProtocolList.AddUnique(NAME_ArtnetProtocol);
		DMXProtocolList.AddUnique(NAME_SACNProtocol);

		for (FName& DMXProtocolName : DMXProtocolList)
		{
			FString PostName = FString::Printf(TEXT(".%s"), *PostTestName);
			FString PrettyName = FString::Printf(TEXT("%s%s"),
				*DMXProtocolName.ToString(),
				PostTestName.IsEmpty() ? TEXT("") : *PostName);
			OutBeautifiedNames.Add(PrettyName);
			OutTestCommands.Add(DMXProtocolName.ToString());
		}
	}
}


IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDMXProtocolTransportTest, "VirtualProduction.DMX.Protocol.Transport", (EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter))



void FDMXProtocolTransportTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	DMXProtocolTransportTestHelper::GetDMXProtocolNamesForTesting(
		OutBeautifiedNames,
		OutTestCommands,
		TEXT("Stress test of the Protocol Transport Layer")
	);
}


bool FDMXProtocolTransportTest::RunTest(const FString& Parameters)
{
	static const uint32 PacketCount = 1000;
	static const uint16 UniverseID = 10001;

	IDMXFragmentMap FragmentMap;
	uint32 PacketIndex = 0;
	uint32 FragmentIndex = 0;
	uint8 Value = 0;

	FName ProtocolName = FName(*Parameters);

	IDMXProtocolPtr DMXProtocol = IDMXProtocol::Get(ProtocolName);
	TestTrue(TEXT("Protocol not found"), DMXProtocol.IsValid());

	FJsonObject UniverseSettings;
	DMXProtocol->GetDefaultUniverseSettings(UniverseID, UniverseSettings);
	DMXProtocol->AddUniverse(UniverseSettings);

	IDMXProtocol::FOnUniverseInputUpdateEvent& InputUpdateEvent = DMXProtocol->GetOnUniverseInputUpdate();
	InputUpdateEvent.AddStatic(DMXProtocolTransportTestHelper::ReceiveFragment);

	for (PacketIndex = 0; PacketIndex < PacketCount; PacketIndex++)
	{
		FragmentMap.Reset();
		FragmentIndex = (PacketIndex % 16) + 2;
		Value = (PacketIndex % 32) + 1;
		FragmentMap.Add(1, PacketIndex % 256);
		FragmentMap.Add(FragmentIndex, Value);
		DMXProtocol->SendDMXFragment(UniverseID, FragmentMap);
	}

	AddCommand(new FDelayedFunctionLatentCommand([=]
		{
			int32 PacketsReceived = 0;
			uint32 FragmentIndexResult = 0;
			uint8 ValueResult = 0;
			DMXProtocolTransportTestHelper::ReceiveData Data;
			while (DMXProtocolTransportTestHelper::ReceiveQueue.Dequeue(Data))
			{
				TestEqual(TEXT("Protocol names don't match"), Data.ProtocolName, ProtocolName);
				TestEqual(TEXT("Receive Universe doesn't match send universe"), Data.UniverseID, UniverseID);
				FragmentIndexResult = (PacketsReceived % 16) + 2;
				ValueResult = (PacketsReceived % 32) + 1;
				TestEqual(TEXT("Packet contents failed"), Data.Packet[0], PacketsReceived % 256);
				TestEqual(TEXT("Packet values failed"), Data.Packet[FragmentIndexResult-1], ValueResult);
				PacketsReceived++;
			}
			TestEqual(TEXT("Packets missing"), PacketsReceived, PacketCount);
			DMXProtocol->RemoveUniverseById(UniverseID);
		}, 0.2f));

	return true;
}

namespace DMXProtocolPacketTestHelper
{
	static const FName NAME_ArtnetProtocol = TEXT("Art-Net");
	static const FName NAME_SACNProtocol = TEXT("sACN");

	struct ReceiveData
	{
		FName ProtocolName;
		uint16 UniverseID;
		TArray<uint8> Packet;

		ReceiveData() {}

		ReceiveData(FName InProtocolName, uint16 InUniverseID, const TArray<uint8>& InPacket) :
			ProtocolName(InProtocolName),
			UniverseID(InUniverseID)
		{
			Packet = InPacket;
		}
	};

	TQueue<ReceiveData> ReceiveQueue;

	static void ReceiveFragment(FName InProtocolName, uint16 InUniverseID, const TArray<uint8>& InDMXData)
	{
		ReceiveQueue.Enqueue(ReceiveData(InProtocolName, InUniverseID, InDMXData));
	}

	void GetDMXProtocolNamesForTesting(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands, const FString& PostTestName = TEXT(""))
	{
		TArray< FName > DMXProtocolList;
		DMXProtocolList.AddUnique(NAME_ArtnetProtocol);
		DMXProtocolList.AddUnique(NAME_SACNProtocol);

		for (FName& DMXProtocolName : DMXProtocolList)
		{
			FString PostName = FString::Printf(TEXT(".%s"), *PostTestName);
			FString PrettyName = FString::Printf(TEXT("%s%s"),
				*DMXProtocolName.ToString(),
				PostTestName.IsEmpty() ? TEXT("") : *PostName);
			OutBeautifiedNames.Add(PrettyName);
			OutTestCommands.Add(DMXProtocolName.ToString());
		}
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDMXProtocolPacketTest, "VirtualProduction.DMX.Protocol.Packets", (EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter))


void FDMXProtocolPacketTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	DMXProtocolPacketTestHelper::GetDMXProtocolNamesForTesting(
		OutBeautifiedNames,
		OutTestCommands,
		TEXT("Tests of the Protocol Packets")
	);
}

bool FDMXProtocolPacketTest::RunTest(const FString& Parameters)
{
	static const int32 PacketCount = 1;
	static const uint16 UniverseID = 10001;   // Pick an universe unlikly to be used in library

	IDMXFragmentMap FragmentMap;
	uint32 PacketIndex = 0;
	uint32 FragmentIndex = 0;
	uint8 Value = 0;

	FName ProtocolName = FName(*Parameters);

	IDMXProtocolPtr DMXProtocol = IDMXProtocol::Get(ProtocolName);
	TestTrue(TEXT("Protocol not found"), DMXProtocol.IsValid());

	FJsonObject UniverseSettings;
	DMXProtocol->GetDefaultUniverseSettings(UniverseID, UniverseSettings);
	TSharedPtr<IDMXProtocolUniverse, ESPMode::ThreadSafe> Universe = DMXProtocol->AddUniverse(UniverseSettings);

	IDMXProtocol::FOnUniverseInputUpdateEvent& InputUpdateEvent = DMXProtocol->GetOnUniverseInputUpdate();
	InputUpdateEvent.AddStatic(DMXProtocolPacketTestHelper::ReceiveFragment);

	for (PacketIndex = 0; PacketIndex < PacketCount; PacketIndex++)
	{
		FragmentMap.Empty();
		FragmentMap.Add(0, PacketIndex % 255);
		for (int Index = 1; Index < 512; Index++)
		{
			FragmentMap.Add(Index, (Index + FragmentMap[0]) % 256);
		}
		DMXProtocol->SendDMXFragment(UniverseID, FragmentMap);
	}

	AddCommand(new FDelayedFunctionLatentCommand([=]
		{
			int32 PacketsReceived = 0;
			uint8 ValueResult = 0;
 
			DMXProtocolPacketTestHelper::ReceiveData Data;
			while (DMXProtocolPacketTestHelper::ReceiveQueue.Dequeue(Data))
			{
				TestEqual(TEXT("Protocol names don't match"), Data.ProtocolName, ProtocolName);
				TestEqual(TEXT("Receive Universe doesn't match send universe"), Data.UniverseID, UniverseID);
				TestEqual(TEXT("Packet contents failed"), Data.Packet[0], PacketsReceived % 256);
				bool Matched = true;
				for (int Index = 1; Index < 512; Index++)
				{
					ValueResult = (Index + Data.Packet[0]) % 256;
					Matched = Matched && (ValueResult == Data.Packet[Index]);
				}
				TestTrue(TEXT("Packet data match failed"), Matched);
				PacketsReceived++;
			}
			TestEqual(TEXT("Packets received"), PacketsReceived, PacketCount);

			DMXProtocol->RemoveUniverseById(UniverseID);
		}, 0.2f));

	return true;
}
