// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelStreamingSignallingConnection.h"
#include "ToStringExtensions.h"
#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "Engine/World.h"
#include "Serialization/JsonSerializer.h"
#include "Settings.h"
#include "TimerManager.h"
#include "PixelStreamingDelegates.h"
#include "PixelStreamingProtocolDefs.h"
#include "Utils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPixelStreamingSS, Log, VeryVerbose);
DEFINE_LOG_CATEGORY(LogPixelStreamingSS);

FPixelStreamingSignallingConnection::FPixelStreamingSignallingConnection(const FWebSocketFactory& InWebSocketFactory, IPixelStreamingSignallingConnectionObserver& InObserver, FString InStreamerId)
	: WebSocketFactory(InWebSocketFactory)
	, Observer(InObserver)
	, StreamerId(InStreamerId)
{
	RegisterHandler("identify", [this](FJsonObjectPtr JsonMsg) { OnIdRequested(); });
	RegisterHandler("config", [this](FJsonObjectPtr JsonMsg) { OnConfig(JsonMsg); });
	RegisterHandler("offer", [this](FJsonObjectPtr JsonMsg) { OnSessionDescription(JsonMsg); });
	RegisterHandler("answer", [this](FJsonObjectPtr JsonMsg) { OnSessionDescription(JsonMsg); });
	RegisterHandler("iceCandidate", [this](FJsonObjectPtr JsonMsg) { OnIceCandidate(JsonMsg); });
	RegisterHandler("ping", [this](FJsonObjectPtr JsonMsg) { /* nothing */ });
	RegisterHandler("pong", [this](FJsonObjectPtr JsonMsg) { /* nothing */ });
	RegisterHandler("playerCount", [this](FJsonObjectPtr JsonMsg) { OnPlayerCount(JsonMsg); });
	RegisterHandler("playerConnected", [this](FJsonObjectPtr JsonMsg) { OnPlayerConnected(JsonMsg); });
	RegisterHandler("playerDisconnected", [this](FJsonObjectPtr JsonMsg) { OnPlayerDisconnected(JsonMsg); });
	RegisterHandler("streamerDataChannels", [this](FJsonObjectPtr JsonMsg) { OnSFUPeerDataChannels(JsonMsg); });
	RegisterHandler("peerDataChannels", [this](FJsonObjectPtr JsonMsg) { OnPeerDataChannels(JsonMsg); });
}

FPixelStreamingSignallingConnection::~FPixelStreamingSignallingConnection()
{
	Disconnect();
}

// This function returns the instance ID to the signalling server. This is useful for identifying individual instances in scalable cloud deployments
void FPixelStreamingSignallingConnection::OnIdRequested()
{
	FJsonObjectPtr Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("type"), TEXT("endpointId"));
	Json->SetStringField(TEXT("id"), StreamerId);
	FString Msg = UE::PixelStreaming::ToString(Json, false);
	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("-> SS: endpointId\n%s"), *Msg);
	SendMessage(Msg);
}

void FPixelStreamingSignallingConnection::Connect(const FString& Url)
{
	// Already have a websocket connection, no need to make another one
	if (WebSocket)
	{
		return;
	}

	WebSocket = WebSocketFactory(Url);
	checkf(WebSocket, TEXT("Web Socket Factory failed to return a valid Web Socket."));

	OnConnectedHandle = WebSocket->OnConnected().AddLambda([this]() { OnConnected(); });
	OnConnectionErrorHandle = WebSocket->OnConnectionError().AddLambda([this](const FString& Error) { OnConnectionError(Error); });
	OnClosedHandle = WebSocket->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean) { OnClosed(StatusCode, Reason, bWasClean); });
	OnMessageHandle = WebSocket->OnMessage().AddLambda([this](const FString& Msg) { OnMessage(Msg); });

	UE_LOG(LogPixelStreamingSS, Log, TEXT("Connecting to SS %s"), *Url);
	WebSocket->Connect();
}

void FPixelStreamingSignallingConnection::Disconnect()
{
	if (!WebSocket)
	{
		return;
	}

	if (!IsEngineExitRequested())
	{
		StopKeepAliveTimer();
	}

	WebSocket->OnConnected().Remove(OnConnectedHandle);
	WebSocket->OnConnectionError().Remove(OnConnectionErrorHandle);
	WebSocket->OnClosed().Remove(OnClosedHandle);
	WebSocket->OnMessage().Remove(OnMessageHandle);

	WebSocket->Close();
	WebSocket = nullptr;
}

bool FPixelStreamingSignallingConnection::IsConnected() const
{
	return WebSocket != nullptr && WebSocket.IsValid() && WebSocket->IsConnected();
}

void FPixelStreamingSignallingConnection::SendOffer(FPixelStreamingPlayerId PlayerId, const webrtc::SessionDescriptionInterface& SDP)
{
	FJsonObjectPtr OfferJson = MakeShared<FJsonObject>();
	OfferJson->SetStringField(TEXT("type"), TEXT("offer"));
	SetPlayerIdJson(OfferJson, PlayerId);

	std::string SdpAnsi;
	SDP.ToString(&SdpAnsi);
	FString SdpStr = UE::PixelStreaming::ToString(SdpAnsi);
	OfferJson->SetStringField(TEXT("sdp"), SdpStr);

	UE_LOG(LogPixelStreamingSS, Log, TEXT("Sending offer to player id=%s"), *PlayerId);
	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("SDP offer\n%s"), *SdpStr);

	SendMessage(UE::PixelStreaming::ToString(OfferJson, false));
}

void FPixelStreamingSignallingConnection::SendAnswer(FPixelStreamingPlayerId PlayerId, const webrtc::SessionDescriptionInterface& SDP)
{
	FJsonObjectPtr AnswerJson = MakeShared<FJsonObject>();
	AnswerJson->SetStringField(TEXT("type"), TEXT("answer"));
	SetPlayerIdJson(AnswerJson, PlayerId);

	std::string SdpAnsi;
	verifyf(SDP.ToString(&SdpAnsi), TEXT("Failed to serialise local SDP"));
	FString SdpStr = UE::PixelStreaming::ToString(SdpAnsi);
	AnswerJson->SetStringField(TEXT("sdp"), SdpStr);

	UE_LOG(LogPixelStreamingSS, Log, TEXT("Sending answer to player id=%s"), *PlayerId);
	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("SDP answer\n%s"), *SdpStr);

	SendMessage(UE::PixelStreaming::ToString(AnswerJson, false));
}

void FPixelStreamingSignallingConnection::SetPlayerIdJson(FJsonObjectPtr& JsonObject, FPixelStreamingPlayerId PlayerId)
{
	bool bSendAsInteger = UE::PixelStreaming::Settings::CVarSendPlayerIdAsInteger.GetValueOnAnyThread();
	if (bSendAsInteger)
	{
		int32 PlayerIdAsInt = PlayerIdToInt(PlayerId);
		JsonObject->SetNumberField(TEXT("playerId"), PlayerIdAsInt);
	}
	else
	{
		JsonObject->SetStringField(TEXT("playerId"), PlayerId);
	}
}

bool FPixelStreamingSignallingConnection::GetPlayerIdJson(const FJsonObjectPtr& Json, FPixelStreamingPlayerId& OutPlayerId, const FString& FieldId)
{
	bool bSendAsInteger = UE::PixelStreaming::Settings::CVarSendPlayerIdAsInteger.GetValueOnAnyThread();
	if (bSendAsInteger)
	{
		uint32 PlayerIdInt;
		if (Json->TryGetNumberField(FieldId, PlayerIdInt))
		{
			OutPlayerId = ToPlayerId(PlayerIdInt);
			return true;
		}
	}
	else if (Json->TryGetStringField(FieldId, OutPlayerId))
	{
		return true;
	}
	return false;
}

void FPixelStreamingSignallingConnection::StartKeepAliveTimer()
{
	// GWorld dereferencing needs to happen on the game thread
	// we dont need to wait since its just setting the timer
	UE::PixelStreaming::DoOnGameThread([this]() {
		GWorld->GetTimerManager().SetTimer(TimerHandle_KeepAlive, std::bind(&FPixelStreamingSignallingConnection::KeepAlive, this), KEEP_ALIVE_INTERVAL, true);
	});
}

void FPixelStreamingSignallingConnection::StopKeepAliveTimer()
{
	// GWorld dereferencing needs to happen on the game thread
	// we need to wait because if we're destructing this object we dont
	// want to call the callback mid/post destruction
	UE::PixelStreaming::DoOnGameThreadAndWait(MAX_uint32, [&TimerHandle_KeepAlive = TimerHandle_KeepAlive]() {
		GWorld->GetTimerManager().ClearTimer(TimerHandle_KeepAlive);
	});
}

void FPixelStreamingSignallingConnection::SendIceCandidate(const webrtc::IceCandidateInterface& IceCandidate)
{
	FJsonObjectPtr IceCandidateJson = MakeShared<FJsonObject>();

	IceCandidateJson->SetStringField(TEXT("type"), TEXT("iceCandidate"));

	FJsonObjectPtr CandidateJson = MakeShared<FJsonObject>();
	CandidateJson->SetStringField(TEXT("sdpMid"), IceCandidate.sdp_mid().c_str());
	CandidateJson->SetNumberField(TEXT("sdpMLineIndex"), static_cast<double>(IceCandidate.sdp_mline_index()));

	std::string CandidateAnsi;
	verifyf(IceCandidate.ToString(&CandidateAnsi), TEXT("Failed to serialize IceCandidate"));
	FString CandidateStr = UE::PixelStreaming::ToString(CandidateAnsi);
	CandidateJson->SetStringField(TEXT("candidate"), CandidateStr);

	IceCandidateJson->SetObjectField(TEXT("candidate"), CandidateJson);

	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("-> SS: ice-candidate\n%s"), *UE::PixelStreaming::ToString(IceCandidateJson));

	SendMessage(UE::PixelStreaming::ToString(IceCandidateJson, false));
}

void FPixelStreamingSignallingConnection::SendIceCandidate(FPixelStreamingPlayerId PlayerId, const webrtc::IceCandidateInterface& IceCandidate)
{
	FJsonObjectPtr IceCandidateJson = MakeShared<FJsonObject>();

	IceCandidateJson->SetStringField(TEXT("type"), TEXT("iceCandidate"));
	SetPlayerIdJson(IceCandidateJson, PlayerId);

	FJsonObjectPtr CandidateJson = MakeShared<FJsonObject>();
	CandidateJson->SetStringField(TEXT("sdpMid"), IceCandidate.sdp_mid().c_str());
	CandidateJson->SetNumberField(TEXT("sdpMLineIndex"), static_cast<double>(IceCandidate.sdp_mline_index()));

	std::string CandidateAnsi;
	verifyf(IceCandidate.ToString(&CandidateAnsi), TEXT("Failed to serialize IceCandidate"));
	FString CandidateStr = UE::PixelStreaming::ToString(CandidateAnsi);
	CandidateJson->SetStringField(TEXT("candidate"), CandidateStr);

	IceCandidateJson->SetObjectField(TEXT("candidate"), CandidateJson);

	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("-> SS: iceCandidate\n%s"), *UE::PixelStreaming::ToString(IceCandidateJson));

	SendMessage(UE::PixelStreaming::ToString(IceCandidateJson, false));
}

void FPixelStreamingSignallingConnection::KeepAlive()
{
	FJsonObjectPtr Json = MakeShared<FJsonObject>();
	double unixTime = FDateTime::UtcNow().ToUnixTimestamp();
	Json->SetStringField(TEXT("type"), TEXT("ping"));
	Json->SetNumberField(TEXT("time"), unixTime);
	SendMessage(UE::PixelStreaming::ToString(Json, false));
}

void FPixelStreamingSignallingConnection::SendDisconnectPlayer(FPixelStreamingPlayerId PlayerId, const FString& Reason)
{
	FJsonObjectPtr Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("type"), TEXT("disconnectPlayer"));
	SetPlayerIdJson(Json, PlayerId);
	Json->SetStringField(TEXT("reason"), Reason);

	FString Msg = UE::PixelStreaming::ToString(Json, false);
	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("-> SS: iceCandidate\n%s"), *Msg);

	SendMessage(Msg);
}

void FPixelStreamingSignallingConnection::SendAnswer(const webrtc::SessionDescriptionInterface& SDP)
{
	FJsonObjectPtr AnswerJson = MakeShared<FJsonObject>();
	AnswerJson->SetStringField(TEXT("type"), TEXT("answer"));

	FString SdpStr = UE::PixelStreaming::ToString(SDP);
	AnswerJson->SetStringField(TEXT("sdp"), UE::PixelStreaming::ToString(SDP));

	UE_LOG(LogPixelStreamingSS, Verbose, TEXT("-> SS: answer\n%s"), *SdpStr);

	SendMessage(UE::PixelStreaming::ToString(AnswerJson, false));
}

void FPixelStreamingSignallingConnection::OnConnected()
{
	UE_LOG(LogPixelStreamingSS, Log, TEXT("Connected to SS"));

	Observer.OnSignallingConnected();

	StartKeepAliveTimer();

	if (UPixelStreamingDelegates* Delegates = UPixelStreamingDelegates::GetPixelStreamingDelegates())
	{
		Delegates->OnConnectedToSignallingServer.Broadcast();
		Delegates->OnConnectedToSignallingServerNative.Broadcast();
	}
}

void FPixelStreamingSignallingConnection::OnConnectionError(const FString& Error)
{
	UE_LOG(LogPixelStreamingSS, Error, TEXT("Failed to connect to SS: %s"), *Error);

	Observer.OnSignallingError(Error);

	StopKeepAliveTimer();

	if (UPixelStreamingDelegates* Delegates = UPixelStreamingDelegates::GetPixelStreamingDelegates())
	{
		Delegates->OnDisconnectedFromSignallingServer.Broadcast();
		Delegates->OnDisconnectedFromSignallingServerNative.Broadcast();
	}
}

void FPixelStreamingSignallingConnection::OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogPixelStreamingSS, Log, TEXT("Connection to SS closed: \n\tstatus %d\n\treason: %s\n\twas clean: %s"), StatusCode, *Reason, bWasClean ? TEXT("true") : TEXT("false"));

	Observer.OnSignallingDisconnected(StatusCode, Reason, bWasClean);

	StopKeepAliveTimer();

	if (UPixelStreamingDelegates* Delegates = UPixelStreamingDelegates::GetPixelStreamingDelegates())
	{
		Delegates->OnDisconnectedFromSignallingServer.Broadcast();
		Delegates->OnDisconnectedFromSignallingServerNative.Broadcast();
	}
}

void FPixelStreamingSignallingConnection::OnMessage(const FString& Msg)
{
	FJsonObjectPtr JsonMsg;
	const auto JsonReader = TJsonReaderFactory<TCHAR>::Create(Msg);

	if (!FJsonSerializer::Deserialize(JsonReader, JsonMsg))
	{
		FatalError(TEXT("Failed to parse SS message:\n%s"), *Msg);
		return;
	}

	FString MsgType;
	if (!JsonMsg->TryGetStringField(TEXT("type"), MsgType))
	{
		FatalError(TEXT("Cannot find `type` field in SS message:\n%s"), *Msg);
		return;
	}

	TFunction<void(FJsonObjectPtr)>* Handler = MessageHandlers.Find(MsgType);
	if (Handler != nullptr)
	{
		(*Handler)(JsonMsg);
	}
	else
	{
		FatalError(TEXT("Unsupported message `%s` received from SS"), *MsgType);
	}
}

void FPixelStreamingSignallingConnection::RegisterHandler(const FString& messageType, const TFunction<void(FJsonObjectPtr)>& handler)
{
	MessageHandlers.Add(messageType, handler);
}

void FPixelStreamingSignallingConnection::OnConfig(const FJsonObjectPtr& Json)
{
	// SS sends `config` that looks like:
	// `{peerConnectionOptions: { 'iceServers': [{'urls': ['stun:34.250.222.95:19302', 'turn:34.250.222.95:19303']}] }}`
	// where `peerConnectionOptions` is `RTCConfiguration` (except in native `RTCConfiguration` "iceServers" = "servers").
	// As `RTCConfiguration` doesn't implement parsing from a string (or `UE::PixelStreaming::ToString` method),
	// we just get `stun`/`turn` URLs from it and ignore other options

	const TSharedPtr<FJsonObject>* PeerConnectionOptions = nullptr;
	if (!Json->TryGetObjectField(TEXT("peerConnectionOptions"), PeerConnectionOptions))
	{
		FatalError(TEXT("Cannot find `peerConnectionOptions` field in SS config\n%s"), *UE::PixelStreaming::ToString(Json));
		return;
	}

	webrtc::PeerConnectionInterface::RTCConfiguration RTCConfig;

	const TArray<TSharedPtr<FJsonValue>>* IceServers = nullptr;
	if ((*PeerConnectionOptions)->TryGetArrayField(TEXT("iceServers"), IceServers))
	{
		for (const TSharedPtr<FJsonValue>& IceServerVal : *IceServers)
		{
			const TSharedPtr<FJsonObject>* IceServerJson;
			if (!IceServerVal->TryGetObject(IceServerJson))
			{
				FatalError(TEXT("Failed to parse SS config: `iceServer` - not an object\n%s"), *UE::PixelStreaming::ToString(*PeerConnectionOptions));
			}

			RTCConfig.servers.push_back(webrtc::PeerConnectionInterface::IceServer{});
			webrtc::PeerConnectionInterface::IceServer& IceServer = RTCConfig.servers.back();

			TArray<FString> Urls;
			if ((*IceServerJson)->TryGetStringArrayField(TEXT("urls"), Urls))
			{
				for (const FString& Url : Urls)
				{
					IceServer.urls.push_back(UE::PixelStreaming::ToString(Url));
				}
			}
			else
			{
				// in the RTC Spec, "urls" can be an array or a single string
				// https://www.w3.org/TR/webrtc/#dictionary-rtciceserver-members
				FString UrlsSingle;
				if ((*IceServerJson)->TryGetStringField(TEXT("urls"), UrlsSingle))
				{
					IceServer.urls.push_back(UE::PixelStreaming::ToString(UrlsSingle));
				}
			}

			FString Username;
			if ((*IceServerJson)->TryGetStringField(TEXT("username"), Username))
			{
				IceServer.username = UE::PixelStreaming::ToString(Username);
			}

			FString Credential;
			if ((*IceServerJson)->TryGetStringField(TEXT("credential"), Credential))
			{
				IceServer.password = UE::PixelStreaming::ToString(Credential);
			}
		}
	}

	// force `UnifiedPlan` as we control both ends of WebRTC streaming
	RTCConfig.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

	Observer.OnSignallingConfig(RTCConfig);
}

void FPixelStreamingSignallingConnection::OnSessionDescription(const FJsonObjectPtr& Json)
{
	webrtc::SdpType Type = Json->GetStringField(TEXT("type")) == TEXT("offer") ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer;

	FString Sdp;
	if (!Json->TryGetStringField(TEXT("sdp"), Sdp))
	{
		FatalError(TEXT("Cannot find `sdp` in Streamer's answer\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	FPixelStreamingPlayerId PlayerId;
	bool bGotPlayerId = GetPlayerIdJson(Json, PlayerId);
	if (!bGotPlayerId)
	{
		Observer.OnSignallingSessionDescription(Type, Sdp);
	}
	else
	{
		Observer.OnSignallingSessionDescription(PlayerId, Type, Sdp);
	}
}

void FPixelStreamingSignallingConnection::OnIceCandidate(const FJsonObjectPtr& Json)
{
	FPixelStreamingPlayerId PlayerId;
	bool bGotPlayerId = GetPlayerIdJson(Json, PlayerId);

	const FJsonObjectPtr* CandidateJson;
	if (!Json->TryGetObjectField(TEXT("candidate"), CandidateJson))
	{
		PlayerError(PlayerId, TEXT("Failed to get `candiate` from remote `iceCandidate` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	FString SdpMid;
	if (!(*CandidateJson)->TryGetStringField(TEXT("sdpMid"), SdpMid))
	{
		PlayerError(PlayerId, TEXT("Failed to get `sdpMid` from remote `iceCandidate` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	int32 SdpMLineIndex = 0;
	if (!(*CandidateJson)->TryGetNumberField(TEXT("sdpMlineIndex"), SdpMLineIndex))
	{
		PlayerError(PlayerId, TEXT("Failed to get `sdpMlineIndex` from remote `iceCandidate` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	FString CandidateStr;
	if (!(*CandidateJson)->TryGetStringField(TEXT("candidate"), CandidateStr))
	{
		PlayerError(PlayerId, TEXT("Failed to get `candidate` from remote `iceCandidate` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	if (bGotPlayerId)
	{
		Observer.OnSignallingRemoteIceCandidate(PlayerId, SdpMid, SdpMLineIndex, CandidateStr);
	}
	else
	{
		Observer.OnSignallingRemoteIceCandidate(SdpMid, SdpMLineIndex, CandidateStr);
	}
}

void FPixelStreamingSignallingConnection::OnPlayerCount(const FJsonObjectPtr& Json)
{
	uint32 Count;
	if (!Json->TryGetNumberField(TEXT("count"), Count))
	{
		FatalError(TEXT("Failed to get `count` from `playerCount` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	Observer.OnSignallingPlayerCount(Count);
}

void FPixelStreamingSignallingConnection::OnPlayerConnected(const FJsonObjectPtr& Json)
{
	FPixelStreamingPlayerId PlayerId;
	bool bGotPlayerId = GetPlayerIdJson(Json, PlayerId);
	if (!bGotPlayerId)
	{
		FatalError(TEXT("Failed to get `playerId` from `join` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	UE_LOG(LogPixelStreamingSS, Log, TEXT("Got player connected, player id=%s"), *PlayerId);

	int Flags = 0;

	// Default to always making datachannel, unless explicitly set to false.
	bool bMakeDataChannel = true;
	Json->TryGetBoolField(TEXT("datachannel"), bMakeDataChannel);

	// Default peer is not an SFU, unless explictly set as SFU
	bool bIsSFU = false;
	Json->TryGetBoolField(TEXT("sfu"), bIsSFU);

	Flags |= bMakeDataChannel ? UE::PixelStreaming::Protocol::EPlayerFlags::PSPFlag_SupportsDataChannel : 0;
	Flags |= bIsSFU ? UE::PixelStreaming::Protocol::EPlayerFlags::PSPFlag_IsSFU : 0;
	Observer.OnSignallingPlayerConnected(PlayerId, Flags);
}

void FPixelStreamingSignallingConnection::OnPlayerDisconnected(const FJsonObjectPtr& Json)
{
	FPixelStreamingPlayerId PlayerId;
	bool bGotPlayerId = GetPlayerIdJson(Json, PlayerId);
	if (!bGotPlayerId)
	{
		FatalError(TEXT("Failed to get `playerId` from `playerDisconnected` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	Observer.OnSignallingPlayerDisconnected(PlayerId);
}

void FPixelStreamingSignallingConnection::OnSFUPeerDataChannels(const FJsonObjectPtr& Json)
{
	FPixelStreamingPlayerId SFUId;
	bool bSuccess = GetPlayerIdJson(Json, SFUId, TEXT("sfuId"));
	if (!bSuccess)
	{
		FatalError(TEXT("Failed to get `sfuId` from `streamerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	FPixelStreamingPlayerId PlayerId;
	bSuccess = GetPlayerIdJson(Json, PlayerId, TEXT("playerId"));
	if (!bSuccess)
	{
		FatalError(TEXT("Failed to get `playerId` from `streamerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	int32 SendStreamId;
	bSuccess = Json->TryGetNumberField(TEXT("sendStreamId"), SendStreamId);
	if (!bSuccess)
	{
		FatalError(TEXT("Failed to get `sendStreamId` from `streamerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	int32 RecvStreamId;
	bSuccess = Json->TryGetNumberField(TEXT("recvStreamId"), RecvStreamId);
	if (!bSuccess)
	{
		FatalError(TEXT("Failed to get `recvStreamId` from `streamerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
	}

	Observer.OnSignallingSFUPeerDataChannels(SFUId, PlayerId, SendStreamId, RecvStreamId);
}

void FPixelStreamingSignallingConnection::OnPeerDataChannels(const FJsonObjectPtr& Json)
{
	int32 SendStreamId = 0;
	if (!Json->TryGetNumberField(TEXT("sendStreamId"), SendStreamId))
	{
		FatalError(TEXT("Failed to get `sendStreamId` from remote `peerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
		return;
	}
	int32 RecvStreamId = 0;
	if (!Json->TryGetNumberField(TEXT("recvStreamId"), RecvStreamId))
	{
		FatalError(TEXT("Failed to get `recvStreamId` from remote `peerDataChannels` message\n%s"), *UE::PixelStreaming::ToString(Json));
		return;
	}
	Observer.OnSignallingPeerDataChannels(SendStreamId, RecvStreamId);
}

void FPixelStreamingSignallingConnection::PlayerError(FPixelStreamingPlayerId PlayerId, const FString& Msg)
{
	UE_LOG(LogPixelStreamingSS, Error, TEXT("player %s: %s"), *PlayerId, *Msg);
	SendDisconnectPlayer(PlayerId, Msg);
}

void FPixelStreamingSignallingConnection::SendMessage(const FString& Msg)
{
	if (WebSocket && WebSocket->IsConnected())
	{
		WebSocket->Send(Msg);
	}
}

void FPixelStreamingSignallingConnection::FatalError(const FString& Msg)
{
	UE_LOG(LogPixelStreamingSS, Error, TEXT("%s"), *Msg);
	WebSocket->Close(4000, Msg);
}
