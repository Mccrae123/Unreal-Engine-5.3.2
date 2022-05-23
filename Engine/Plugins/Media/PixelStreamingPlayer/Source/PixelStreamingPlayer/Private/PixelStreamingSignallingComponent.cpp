// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelStreamingSignallingComponent.h"
#include "WebSocketsModule.h"

UPixelStreamingSignallingComponent::UPixelStreamingSignallingComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FPixelStreamingSignallingConnection::FWebSocketFactory WebSocketFactory = [](const FString& Url) { return FWebSocketsModule::Get().CreateWebSocket(Url, TEXT("")); };
	SignallingConnection = MakeUnique<FPixelStreamingSignallingConnection>(WebSocketFactory, *this);
}

void UPixelStreamingSignallingComponent::Connect(const FString& Url)
{
	if (MediaSource == nullptr)
	{
		SignallingConnection->Connect(Url);
	}
	else
	{
		SignallingConnection->Connect(MediaSource->GetUrl());
	}
}

void UPixelStreamingSignallingComponent::Disconnect()
{
	SignallingConnection->Disconnect();
}

void UPixelStreamingSignallingComponent::OnSignallingConnected()
{
	OnConnected.Broadcast();
}

void UPixelStreamingSignallingComponent::OnSignallingDisconnected(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	OnDisconnected.Broadcast(StatusCode, Reason, bWasClean);
}

void UPixelStreamingSignallingComponent::OnSignallingError(const FString& ErrorMsg)
{
	OnConnectionError.Broadcast(ErrorMsg);
}

void UPixelStreamingSignallingComponent::OnSignallingConfig(const webrtc::PeerConnectionInterface::RTCConfiguration& Config)
{
	RTCConfig = Config;
	OnConfig.Broadcast();
}

void UPixelStreamingSignallingComponent::OnSignallingSessionDescription(webrtc::SdpType Type, const FString& Sdp)
{
	if (Type == webrtc::SdpType::kOffer)
	{
		OnOffer.Broadcast(Sdp);
	}
	else if (Type == webrtc::SdpType::kAnswer)
	{
		// TODO if needed. Currently we never send an offer so we shouldnt expect an answer.
	}
}

void UPixelStreamingSignallingComponent::OnSignallingRemoteIceCandidate(const FString& SdpMid, int SdpMLineIndex, const FString& Sdp)
{
	OnIceCandidate.Broadcast(SdpMid, SdpMLineIndex, Sdp);
}

void UPixelStreamingSignallingComponent::OnSignallingPeerDataChannels(int32 SendStreamId, int32 RecvStreamId)
{
	// TODO what to do with data channels.
}

void UPixelStreamingSignallingComponent::OnSignallingPlayerCount(uint32 Count)
{
}
