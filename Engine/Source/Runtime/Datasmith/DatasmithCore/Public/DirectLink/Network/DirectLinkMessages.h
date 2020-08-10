// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "DirectLink/DirectLinkCommon.h"

#include "CoreMinimal.h"

#include "DirectLinkMessages.generated.h"

namespace DirectLink
{
class FScenePipeBase;


// Helper function to allocate a UStruct message which memory will be owned and released
// through FMessageContext.
// The explicit FMemory::Malloc will match the FMemory::Free used there.
// This is useful in scenarios where operator new isn't redirected to FMemory::Malloc.
//
// usage:
//    FMyMessage* Message = NewMessage<FMyMessage>();
//    Endpoint->Publish(Message); // The FMessageEndpoint now owns the Message allocation
template<typename T, typename... ArgsType>
T* NewMessage(ArgsType&&... Args)
{
	void* Memory = FMemory::Malloc(sizeof(T), alignof(T));
	return new (Memory) T(Forward<ArgsType>(Args)...);
}

} // namespace DirectLink






USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_EndpointLifecycle
{
	GENERATED_BODY()

	enum ELifecycle : uint8
	{
		None,
		Start,
		Heartbeat,
		Stop,
	};

	FDirectLinkMsg_EndpointLifecycle(ELifecycle InLifecycleState = ELifecycle::None, uint32 InEndpointStateRevision = 0)
		: LifecycleState(InLifecycleState)
		, EndpointStateRevision(InEndpointStateRevision)
	{}

	UPROPERTY()
	uint8 LifecycleState;

	UPROPERTY()
	uint32 EndpointStateRevision;
};



USTRUCT(meta=(Experimental))
struct FNamedId
{
	GENERATED_BODY();

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FGuid Id;
};

USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_EndpointState
{
	GENERATED_BODY();
	FDirectLinkMsg_EndpointState(uint32 StateRevision=0, uint32 ProtocolVersion=0)
	: StateRevision(StateRevision)
	, ProtocolVersion(ProtocolVersion)
	{}

	UPROPERTY()
	uint32 StateRevision;

	UPROPERTY()
	uint32 ProtocolVersion;

	UPROPERTY()
	FString ComputerName;

	UPROPERTY()
	FString UserName;

	UPROPERTY()
	uint32 ProcessId = 0;

	UPROPERTY()
	FString ExecutableName;

	UPROPERTY()
	FString NiceName;

	UPROPERTY()
	TArray<FNamedId> Destinations;

	UPROPERTY()
	TArray<FNamedId> Sources;
};



USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_QueryEndpointState
{
	GENERATED_BODY();
};



USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_OpenStreamRequest
{
	GENERATED_BODY();
	// #ue_directlink_cleanup explicit ctr to force correct init

	UPROPERTY()
	bool bRequestFromSource = false;

	UPROPERTY()
	int32 RequestFromStreamPort = 0; // FStreamPort

	UPROPERTY()
	FGuid SourceGuid;

	UPROPERTY()
	FGuid DestinationGuid;
};



USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_OpenStreamAnswer
{
	GENERATED_BODY();

	UPROPERTY()
	int32 RecipientStreamPort = 0; // FStreamPort

	UPROPERTY()
	bool bAccepted = false;

	UPROPERTY()
	int32 OpenedStreamPort = 0; // FStreamPort
};


USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_CloseStreamRequest
{
	GENERATED_BODY();

	UPROPERTY()
	int32 RecipientStreamPort = 0; // FStreamPort
};



USTRUCT(meta=(Experimental))
struct FDirectLinkMsg_DeltaMessage
{
	GENERATED_BODY();

	enum EKind
	{
		None,
		OpenDelta,
		SetElement,
		CloseDelta,
	};

	// required for UStructs
	FDirectLinkMsg_DeltaMessage() = default;

	FDirectLinkMsg_DeltaMessage(EKind Kind, DirectLink::FStreamPort DestinationStreamPort, uint32 BatchNumber, uint32 MessageIndex)
		: Kind(Kind)
		, DestinationStreamPort(DestinationStreamPort)
		, BatchCode(BatchNumber)
		, MessageCode(MessageIndex)
	{
	}

	UPROPERTY()
	uint8 Kind = 0;

	UPROPERTY()
	int32 DestinationStreamPort = 0; // FStreamPort

	UPROPERTY()
	int8 BatchCode = 0;

	UPROPERTY()
	int32 MessageCode = 0;

	UPROPERTY()
	TArray<uint8> Payload;
};


