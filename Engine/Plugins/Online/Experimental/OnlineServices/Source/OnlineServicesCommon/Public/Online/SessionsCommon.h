// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/Sessions.h"
#include "Online/OnlineComponent.h"
#include "Online/OnlineIdCommon.h"

namespace UE::Online {

class FOnlineServicesCommon;

static FName CONNECT_STRING_TAG = TEXT("CONNECT_STRING");

class FOnlineSessionIdStringRegistry : public IOnlineSessionIdRegistry
{
public:
	// Begin IOnlineSessionIdRegistry
	virtual inline FString ToLogString(const FOnlineSessionIdHandle& Handle) const override
	{
		FString IdValue = BasicRegistry.FindIdValue(Handle);

		if (IdValue.Len() == 0)
		{
			IdValue = FString(TEXT("[InvalidSessionID]"));
		}

		return IdValue;
	};

	virtual inline TArray<uint8> ToReplicationData(const FOnlineSessionIdHandle& Handle) const override
	{
		const FString IdValue = BasicRegistry.FindIdValue(Handle);
		const FTCHARToUTF8 IdValueUtf8(*IdValue);

		TArray<uint8> ReplicationData;
		ReplicationData.SetNumUninitialized(IdValueUtf8.Length());

		FMemory::Memcpy(ReplicationData.GetData(), IdValueUtf8.Get(), IdValueUtf8.Length());

		return ReplicationData;
	}

	virtual inline FOnlineSessionIdHandle FromReplicationData(const TArray<uint8>& ReplicationData) override
	{
		const FUTF8ToTCHAR IdValueTCHAR((char*)ReplicationData.GetData(), ReplicationData.Num());
		const FString IdValue = FString(IdValueTCHAR.Length(), IdValueTCHAR.Get());

		if (!IdValue.IsEmpty())
		{
			return BasicRegistry.FindOrAddHandle(IdValue);
		}

		return FOnlineSessionIdHandle();
	}
	// End IOnlineSessionIdRegistry

	inline bool IsSessionIdExpired(const FOnlineSessionIdHandle& InHandle) const
	{
		return BasicRegistry.FindIdValue(InHandle).IsEmpty();
	}

	FOnlineSessionIdStringRegistry(EOnlineServices OnlineServicesType)
		: BasicRegistry(OnlineServicesType)
	{

	}

	virtual ~FOnlineSessionIdStringRegistry() = default;

public:
	TOnlineBasicSessionIdRegistry<FString> BasicRegistry;
};

class FOnlineSessionInviteIdStringRegistry : public IOnlineSessionInviteIdRegistry
{
public:
	// Begin IOnlineSessionIdRegistry
	virtual inline FString ToLogString(const FOnlineSessionInviteIdHandle& Handle) const override
	{
		FString IdValue = BasicRegistry.FindIdValue(Handle);

		if (IdValue.Len() == 0)
		{
			IdValue = FString(TEXT("[InvalidSessionID]"));
		}

		return IdValue;
	};

	virtual inline TArray<uint8> ToReplicationData(const FOnlineSessionInviteIdHandle& Handle) const override
	{
		const FString IdValue = BasicRegistry.FindIdValue(Handle);
		const FTCHARToUTF8 IdValueUtf8(IdValue);

		TArray<uint8> ReplicationData;
		ReplicationData.SetNumUninitialized(IdValueUtf8.Length());

		FMemory::Memcpy(ReplicationData.GetData(), IdValueUtf8.Get(), IdValueUtf8.Length());

		return ReplicationData;
	}

	virtual inline FOnlineSessionInviteIdHandle FromReplicationData(const TArray<uint8>& ReplicationData) override
	{
		const FUTF8ToTCHAR IdValueTCHAR((char*)ReplicationData.GetData(), ReplicationData.Num());
		const FString IdValue = FString(IdValueTCHAR.Length(), IdValueTCHAR.Get());

		if (!IdValue.IsEmpty())
		{
			return BasicRegistry.FindOrAddHandle(IdValue);
		}

		return FOnlineSessionInviteIdHandle();
	}
	// End IOnlineSessionIdRegistry

	FOnlineSessionInviteIdStringRegistry(EOnlineServices OnlineServicesType)
		: BasicRegistry(OnlineServicesType)
	{

	}

	virtual ~FOnlineSessionInviteIdStringRegistry() = default;

public:
	TOnlineBasicSessionInviteIdRegistry<FString> BasicRegistry;
};

class ONLINESERVICESCOMMON_API FSessionsCommon : public TOnlineComponent<ISessions>
{
public:
	using Super = ISessions;

	FSessionsCommon(FOnlineServicesCommon& InServices);

	// TOnlineComponent
	virtual void Initialize() override;
	virtual void RegisterCommands() override;

	// ISessions
	virtual TOnlineResult<FGetAllSessions> GetAllSessions(FGetAllSessions::Params&& Params) const override;
	virtual TOnlineResult<FGetSessionByName> GetSessionByName(FGetSessionByName::Params&& Params) const override;
	virtual TOnlineResult<FGetSessionById> GetSessionById(FGetSessionById::Params&& Params) const override;
	virtual TOnlineAsyncOpHandle<FCreateSession> CreateSession(FCreateSession::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FUpdateSession> UpdateSession(FUpdateSession::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FLeaveSession> LeaveSession(FLeaveSession::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FFindSessions> FindSessions(FFindSessions::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FStartMatchmaking> StartMatchmaking(FStartMatchmaking::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FJoinSession> JoinSession(FJoinSession::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FAddSessionMembers> AddSessionMembers(FAddSessionMembers::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FRemoveSessionMembers> RemoveSessionMembers(FRemoveSessionMembers::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FSendSessionInvite> SendSessionInvite(FSendSessionInvite::Params&& Params) override;
	virtual TOnlineResult<FGetSessionInvites> GetSessionInvites(FGetSessionInvites::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FRejectSessionInvite> RejectSessionInvite(FRejectSessionInvite::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FRegisterPlayers> RegisterPlayers(FRegisterPlayers::Params&& Params) override;
	virtual TOnlineAsyncOpHandle<FUnregisterPlayers> UnregisterPlayers(FUnregisterPlayers::Params&& Params) override;

	virtual TOnlineEvent<void(const FSessionJoined&)> OnSessionJoined() override;
	virtual TOnlineEvent<void(const FSessionLeft&)> OnSessionLeft() override;
	virtual TOnlineEvent<void(const FSessionUpdated&)> OnSessionUpdated() override;
	virtual TOnlineEvent<void(const FSessionInviteReceived&)> OnSessionInviteReceived() override;
	virtual TOnlineEvent<void(const FUISessionJoinRequested&)> OnUISessionJoinRequested() override;

protected:
	TOnlineResult<FAddSessionMembers> AddSessionMembersImpl(const FAddSessionMembers::Params& Params);
	TOnlineResult<FRemoveSessionMembers> RemoveSessionMembersImpl(const FRemoveSessionMembers::Params& Params);

	TOnlineResult<FRegisterPlayers> RegisterPlayersImpl(const FRegisterPlayers::Params& Params);
	TOnlineResult<FUnregisterPlayers> UnregisterPlayersImpl(const FUnregisterPlayers::Params& Params);

	FOnlineError CheckCreateSessionParams(const FCreateSession::Params& Params);
	FOnlineError CheckCreateSessionState(const FCreateSession::Params& Params);

	FOnlineError CheckUpdateSessionState(const FUpdateSession::Params& Params);

	FOnlineError CheckFindSessionsParams(const FFindSessions::Params& Params);
	FOnlineError CheckFindSessionsState(const FFindSessions::Params& Params);

	FOnlineError CheckStartMatchmakingParams(const FStartMatchmaking::Params& Params);
	FOnlineError CheckStartMatchmakingState(const FStartMatchmaking::Params& Params);

	FOnlineError CheckJoinSessionParams(const FJoinSession::Params& Params);
	FOnlineError CheckJoinSessionState(const FJoinSession::Params& Params);

	FOnlineError CheckAddSessionMembersState(const FAddSessionMembers::Params& Params);

	FOnlineError CheckRemoveSessionMembersState(const FRemoveSessionMembers::Params& Params);

	FOnlineError CheckLeaveSessionState(const FLeaveSession::Params& Params);

	FOnlineError CheckSendSessionInviteState(const FSendSessionInvite::Params& Params);

	FOnlineError CheckRejectSessionInviteState(const FRejectSessionInvite::Params& Params);

	FOnlineError CheckRegisterPlayersState(const FRegisterPlayers::Params& Params);

	FOnlineError CheckUnregisterPlayersState(const FUnregisterPlayers::Params& Params);

private:
	TOptional<FOnlineError> CheckSessionExistsByName(const FName& SessionName);

protected:

	struct FSessionEvents
	{
		TOnlineEventCallable<void(const FSessionJoined&)> OnSessionJoined;
		TOnlineEventCallable<void(const FSessionLeft&)> OnSessionLeft;
		TOnlineEventCallable<void(const FSessionUpdated&)> OnSessionUpdated;
		TOnlineEventCallable<void(const FSessionInviteReceived&)> OnSessionInviteReceived;
		TOnlineEventCallable<void(const FUISessionJoinRequested&)> OnUISessionJoinRequested;
	} SessionEvents;

	/** Map of sessions that local users are part of, indexed by their local name */
	TMap<FName, TSharedRef<FSession>> LocalSessionsByName;
	/** Cache for the last set of session search results, mapped per user */
	TMap<FAccountId, TMap<FOnlineSessionIdHandle, TSharedRef<FSession>>> SessionSearchResultsUserMap;
	/** Cache for received session invites, mapped per user */
	TMap<FAccountId, TMap<FOnlineSessionInviteIdHandle, TSharedRef<FSessionInvite>>> SessionInvitesUserMap;

	TSharedPtr<FFindSessions::Result> CurrentSessionSearch;
	TSharedPtr<TOnlineAsyncOp<FFindSessions>> CurrentSessionSearchHandle;
};

/* UE::Online */ }
