// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/OnlineAsyncOpHandle.h"
#include "Online/CoreOnline.h"
#include "Online/OnlineMeta.h"
#include "Online/SchemaTypes.h"
#include "Misc/TVariant.h"

namespace UE::Online {

struct FFindSessionsSearchFilter
{
	/** Name of the custom setting to be used as filter */
	FSchemaAttributeId Key;

	/** The type of comparison to perform */
	ESchemaAttributeComparisonOp ComparisonOp;

	/** Value to use when comparing the filter */
	FSchemaVariant Value;
};

struct FCustomSessionSetting
{
	/** Setting value */
	FSchemaVariant Data;

	/** How is this session setting advertised with the backend or searches */
	ESchemaAttributeVisibility Visibility;

	/** Optional ID used in some platforms as the index instead of the setting name */
	int32 ID;
};

using FCustomSessionSettingsMap = TMap<FSchemaAttributeId, FCustomSessionSetting>;

struct FCustomSessionSettingUpdate
{
	FCustomSessionSetting OldValue;

	FCustomSessionSetting NewValue;
};

using FCustomSessionSettingUpdateMap = TMap<FName, FCustomSessionSettingUpdate>;

/** A member is a player that is part of the session, and it stops being a member when they leave it */
struct FSessionMember
{
	FCustomSessionSettingsMap MemberSettings;
};

using FSessionMembersMap = TMap<FAccountId, FSessionMember>;

struct FSessionMemberUpdate
{
	FCustomSessionSettingsMap UpdatedMemberSettings;
	TArray<FSchemaAttributeId> RemovedMemberSettings;

	FSessionMemberUpdate& operator+=(FSessionMemberUpdate&& UpdatedValue);
};

using FSessionMemberUpdatesMap = TMap<FAccountId, FSessionMemberUpdate>;

enum class ESessionJoinPolicy : uint8
{
	Public,
	FriendsOnly,
	InviteOnly
};
ONLINESERVICESINTERFACE_API const TCHAR* LexToString(ESessionJoinPolicy Value);
ONLINESERVICESINTERFACE_API void LexFromString(ESessionJoinPolicy& Value, const TCHAR* InStr);

/** Contains new values for an FSessions modifiable settings. Taken as a parameter by FUpdateSessions method */
struct FSessionSettingsUpdate
{
	/** Set with an updated value if the SchemaName field will be changed in the update operation */
	TOptional<FSchemaId> SchemaName;
	/** Set with an updated value if the NumMaxConnections field will be changed in the update operation */
	TOptional<uint32> NumMaxConnections;
	/** Set with an updated value if the JoinPolicy field will be changed in the update operation */
	TOptional<ESessionJoinPolicy> JoinPolicy;
	/** Set with an updated value if the bAllowNewMembers field will be changed in the update operation */
	TOptional<bool> bAllowNewMembers;

	/** Updated values for custom settings to change in the update operation*/
	FCustomSessionSettingsMap UpdatedCustomSettings;
	/** Names of custom settings to be removed in the update operation*/
	TArray<FSchemaAttributeId> RemovedCustomSettings;

	/** Updated values for session member info to change in the update operation*/
	FSessionMemberUpdatesMap UpdatedSessionMembers;
	/** Id handles for session members to be removed in the update operation*/
	TArray<FAccountId> RemovedSessionMembers;

	FSessionSettingsUpdate& operator+=(FSessionSettingsUpdate&& UpdatedValue);
};

/** Contains updated data for any modifiable members of FSessionSettings. Member of FSessionUpdated event */
struct FSessionSettingsChanges
{
	/* If set, the FSessionSettings's SchemaName member will be updated to this value */
	TOptional<FName> SchemaName;
	/** If set, the FSessionSettings's NumMaxConnections member will be updated to this value */
	TOptional<uint32> NumMaxConnections;
	/** If set, the FSessionSettings's JoinPolicy member will be updated to this value */
	TOptional<ESessionJoinPolicy> JoinPolicy;
	/** If set, the FSessionSettings's bAllowNewMembers member will be updated to this value */
	TOptional<bool> bAllowNewMembers;

	/** New custom settings, with their values */
	FCustomSessionSettingsMap AddedCustomSettings;

	/** Existing custom settings that changed value, including new and old values */
	FCustomSessionSettingUpdateMap ChangedCustomSettings;

	/** Keys for removed custom settings */
	TArray<FName> RemovedCustomSettings;
};

/** Contains updated data for any modifiable members of FSessionMember. Member of FSessionUpdated event */
struct FSessionMemberChanges
{
	/** New custom settings, with their values */
	FCustomSessionSettingsMap AddedMemberSettings;

	/** Existing custom settings that changed value, including new and old values */
	FCustomSessionSettingUpdateMap ChangedMemberSettings;

	/** Keys for removed custom settings */
	TArray<FName> RemovedMemberSettings;
};

/** Set of all of an FSession's defining properties that can be updated by the session owner during its lifetime */
struct ONLINESERVICESINTERFACE_API FSessionSettings
{
	/* The schema which will be applied to the session */
	FName SchemaName;

	/* Maximum number of slots for session members */
	uint32 NumMaxConnections = 0;

	/* Enum value describing the level of restriction to join the session. Public by default */
	ESessionJoinPolicy JoinPolicy = ESessionJoinPolicy::Public;;

	/* Override value to restrict the session from accepting new members, regardless of other factors. True by default */
	bool bAllowNewMembers = true;

	/* Map of user-defined settings to be passed to the platform APIs as additional information for various purposes */
	FCustomSessionSettingsMap CustomSettings;

	FSessionSettings& operator+=(const FSessionSettingsChanges& UpdatedValue);
};

/** Information about an FSession that will be set at creation time and remain constant during its lifetime */
struct FSessionInfo
{
	/** The id for the session, platform dependent */
	FOnlineSessionId SessionId;

	/* In platforms that support this feature, it will set the session id to this value. Might be subject to minimum and maximum length */
	FString SessionIdOverride;

	/* Whether the session is only available in the local network and not via internet connection. Only available in some platforms. False by default */
	bool bIsLANSession = false;

	/* Whether the session is configured to run as a dedicated server. Only available in some platforms. False by default */
	bool bIsDedicatedServerSession = false;

	/* Whether this session will allow sanctioned players to join it. True by default */
	bool bAllowSanctionedPlayers = true;

	/* Whether this is a secure session protected by anti-cheat services. False by default */
	bool bAntiCheatProtected = false;
};

class ISession
{
public:
	virtual const FAccountId GetOwnerAccountId() const = 0;
	virtual const FOnlineSessionId GetSessionId() const = 0;
	virtual const uint32 GetNumOpenConnections() const = 0;
	virtual const FSessionInfo& GetSessionInfo() const = 0;
	virtual const FSessionSettings GetSessionSettings() const = 0;
	virtual const FSessionMembersMap& GetSessionMembers() const = 0;

	/** Evaluates a series of factors to determine if a session is accepting new members */
	virtual bool IsJoinable() const = 0;

	virtual FString ToLogString() const = 0;
};
ONLINESERVICESINTERFACE_API const FString ToLogString(const ISession& Session);

struct FSessionInvite
{
	/* The user which the invite got sent to */
	FAccountId RecipientId;

	/* The user which sent the invite */
	FAccountId SenderId;

	/* The invite id handle, needed for retrieving session information and rejecting the invite */
	FSessionInviteId InviteId;

	/* Pointer to the session information */
	FOnlineSessionId SessionId;

	// TODO: Default constructor will be deleted after we cache invites
};

struct FGetAllSessions
{
	static constexpr TCHAR Name[] = TEXT("GetAllSessions");

	struct Params
	{
		FAccountId LocalAccountId;
	};

	struct Result
	{
		TArray<TSharedRef<const ISession>> Sessions;
	};
};

struct FGetSessionByName
{
	static constexpr TCHAR Name[] = TEXT("GetSessionByName");

	struct Params
	{
		FName LocalName;
	};

	struct Result
	{
		TSharedRef<const ISession> Session;

		Result() = delete; // cannot default construct due to TSharedRef
	};
};

struct FGetSessionById
{
	static constexpr TCHAR Name[] = TEXT("GetSessionById");

	struct Params
	{
		FAccountId LocalAccountId;

		FOnlineSessionId SessionId;
	};

	struct Result
	{
		TSharedRef<const ISession> Session;

		Result() = delete; // cannot default construct due to TSharedRef
	};
};

struct FGetPresenceSession
{
	static constexpr TCHAR Name[] = TEXT("GetPresenceSession");

	struct Params
	{
		FAccountId LocalAccountId;
	};

	struct Result
	{
		TSharedPtr<const ISession> Session;
	};
};

struct FIsPresenceSession
{
	static constexpr TCHAR Name[] = TEXT("IsPresenceSession");

	struct Params
	{
		FAccountId LocalAccountId;

		FOnlineSessionId SessionId;
	};

	struct Result
	{
		bool bIsPresenceSession;
	};
};

struct FSetPresenceSession
{
	static constexpr TCHAR Name[] = TEXT("SetPresenceSession");

	struct Params
	{
		FAccountId LocalAccountId;

		FOnlineSessionId SessionId;
	};

	struct Result
	{
	};
};

struct FClearPresenceSession
{
	static constexpr TCHAR Name[] = TEXT("ClearPresenceSession");

	struct Params
	{
		FAccountId LocalAccountId;
	};

	struct Result
	{
	};
};

struct FCreateSession
{
	static constexpr TCHAR Name[] = TEXT("CreateSession");

	struct Params
	{
		/** The local user agent which will perform the action. */
		FAccountId LocalAccountId;

		/** The local name for the session */
		FName SessionName;

		/** Information for the local user who will join the session after creation */
		FSessionMember SessionMemberData;

		/* In platforms that support this feature, it will set the session id to this value. Might be subject to minimum and maximum length */
		FString SessionIdOverride;

		/** Whether this session should be set as the user's new presence session. False by default */
		bool bPresenceEnabled = false;

		/* Whether the session is only available in the local network and not via internet connection. Only available in some platforms. False by default */
		bool bIsLANSession = false;

		/* Whether the session is configured to run as a dedicated server. Only available in some platforms. False by default */
		bool bIsDedicatedServerSession = false;

		/* Whether this session will allow sanctioned players to join it. True by default */
		bool bAllowSanctionedPlayers = true;

		/* Whether this is a secure session protected by anti-cheat services. False by default */
		bool bAntiCheatProtected = false;

		/** Settings object to define session properties during creation */
		FSessionSettings SessionSettings;
	};

	struct Result
	{

	};
};

struct FUpdateSession
{
	static constexpr TCHAR Name[] = TEXT("UpdateSession");

	struct Params
	{
		/** The local user agent which will perform the action. */
		FAccountId LocalAccountId;

		/** The local name for the session */
		FName SessionName;

		/** Changes to current session settings */
		FSessionSettingsUpdate Mutations;
	};

	struct Result
	{

	};
};

struct FLeaveSession
{
	static constexpr TCHAR Name[] = TEXT("LeaveSession");

	struct Params
	{
		/* The local user agent which leaves the session */
		FAccountId LocalAccountId;

		/* The local name for the session. */
		FName SessionName;

		/* Whether the call should attempt to destroy the session instead of just leave it */
		bool bDestroySession;
	};

	struct Result
	{

	};
};

struct FFindSessions
{
	static constexpr TCHAR Name[] = TEXT("FindSessions");

	struct Params
	{
		/* The local user agent which starts the session search*/
		FAccountId LocalAccountId;

		/* Maximum number of results to return in one search */
		uint32 MaxResults;

		/** Whether we want to look for LAN sessions or Online sessions */
		bool bFindLANSessions;

		/* Filters to apply when searching for sessions. */
		TArray<FFindSessionsSearchFilter> Filters;

		/* Find sessions containing the target user. */
		TOptional<FAccountId> TargetUser;

		/* Find join info for the target session id. */
		TOptional<FOnlineSessionId> SessionId;
	};

	struct Result
	{
		TArray<FOnlineSessionId> FoundSessionIds;
	};
};

struct FStartMatchmaking
{
	static constexpr TCHAR Name[] = TEXT("StartMatchmaking");

	struct Params
	{
		/* Session creation parameters */
		FCreateSession::Params SessionCreationParameters;

		/* Filters to apply when searching for sessions */
		TArray<FFindSessionsSearchFilter> SessionSearchFilters;
	};

	struct Result
	{

	};
};

struct FJoinSession
{
	static constexpr TCHAR Name[] = TEXT("JoinSession");

	struct Params
	{
		/* The local user agent which starts the join operation*/
		FAccountId LocalAccountId;

		/* Local name for the session */
		FName SessionName;

		/* Id handle for the session to be joined. To be retrieved via session search or invite */
		FOnlineSessionId SessionId;

		/* Information for the local user who will join the session */
		FSessionMember SessionMemberData;

		/* Whether this session should be set as the user's new presence session. False by default */
		bool bPresenceEnabled = false;
	};

	struct Result
	{

	};
};

struct FAddSessionMember
{
	static constexpr TCHAR Name[] = TEXT("AddSessionMember");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* Local name for the session */
		FName SessionName;

		/** Information for the session member to be added to the session. Any player that joins the session becomes a new member in doing so */
		FSessionMember NewSessionMember;
	};

	struct Result
	{

	};
};

struct FRemoveSessionMember
{
	static constexpr TCHAR Name[] = TEXT("RemoveSessionMember");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* Local name for the session */
		FName SessionName;
	};

	struct Result
	{

	};
};

struct FSendSessionInvite
{
	static constexpr TCHAR Name[] = TEXT("SendSessionInvite");

	struct Params
	{
		/* The local user agent which sends the invite*/
		FAccountId LocalAccountId;

		/* The local name for the session. */
		FName SessionName;

		/* Array of id handles for users to which the invites will be sent */
		TArray<FAccountId> TargetUsers;
	};

	struct Result
	{

	};
};

struct FGetSessionInvites
{
	static constexpr TCHAR Name[] = TEXT("GetSessionInvites");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;
	};

	struct Result
	{
		/** Set of active session invites */
		TArray<TSharedRef<const FSessionInvite>> SessionInvites;
	};
};

struct FRejectSessionInvite
{
	static constexpr TCHAR Name[] = TEXT("RejectSessionInvite");

	struct Params
	{
		/* The local user agent which started the query*/
		FAccountId LocalAccountId;

		/* The id handle for the invite to be rejected */
		FSessionInviteId SessionInviteId;
	};

	struct Result
	{
	};
};

/* Events */

struct FSessionJoined
{
	/* The local user which joined the session */
	FAccountId LocalAccountId;

	/* Id for the session joined. */
	FOnlineSessionId SessionId;
};

struct FSessionLeft
{
	/* The local users which left the session */
	FAccountId LocalAccountId;
};

using FSessionMemberChangesMap = TMap<FAccountId, FSessionMemberChanges>;

/** Contains updated data for any modifiable members of ISession */
struct ONLINESERVICESINTERFACE_API FSessionUpdate
{
	/** If set, the OwnerUserId member will have updated to this value */
	TOptional<FAccountId> OwnerAccountId;

	/** If set, the SessionSettings member will have updated using the struct information */
	TOptional<FSessionSettingsChanges> SessionSettingsChanges;

	/** Session member information for members that just joined the session */
	FSessionMembersMap AddedSessionMembers;

	/** Updated values for session member information */
	FSessionMemberChangesMap SessionMembersChanges;

	/** Id handles for members that just left the session */
	TArray<FAccountId> RemovedSessionMembers;

	FSessionUpdate& operator+=(const FSessionUpdate& SessionUpdate);
};

struct FSessionUpdated
{
	/* Name for the session updated */
	FName SessionName;

	/* Updated session settings */
	FSessionUpdate SessionUpdate;
};

struct FSessionInviteReceived
{
	/* The local user which received the invite */
	FAccountId LocalAccountId;

	/** The session invite the local user was sent, or the online error if there was a failure retrieving the session for it*/
	TSharedRef<const FSessionInvite> SessionInvite;

	FSessionInviteReceived() = delete; // cannot default construct due to TSharedRef. TODO: Add a GetSessionInvite method and return an id here
};

/** Session join requested source */
enum class EUISessionJoinRequestedSource : uint8
{
	/** Unspecified by the online service */
	Unspecified,
	/** From an invitation */
	FromInvitation,
};
ONLINESERVICESINTERFACE_API const TCHAR* LexToString(EUISessionJoinRequestedSource UISessionJoinRequestedSource);
ONLINESERVICESINTERFACE_API void LexFromString(EUISessionJoinRequestedSource& OutUISessionJoinRequestedSource, const TCHAR* InStr);

struct FUISessionJoinRequested
{
	/** The local user associated with the join request. */
	FAccountId LocalAccountId;

	/** The id for the session the local user requested to join, or the online error if there was a failure retrieving it */
	TResult<FOnlineSessionId, FOnlineError> Result;

	/** Join request source */
	EUISessionJoinRequestedSource JoinRequestedSource = EUISessionJoinRequestedSource::Unspecified;

	FUISessionJoinRequested() = delete; // cannot default construct due to TResult
};

class ISessions
{
public:
	/**
	 * Gets an array of references to all the sessions the given user is part of.
	 * 
	 * @params Parameters for the GetAllSessions call
	 * return
	 */
	virtual TOnlineResult<FGetAllSessions> GetAllSessions(FGetAllSessions::Params&& Params) const = 0;

	/**
	 * Get the session object with a given local name.
	 *
	 * @params Parameters for the GetSessionByName call
	 * return
	 */
	virtual TOnlineResult<FGetSessionByName> GetSessionByName(FGetSessionByName::Params&& Params) const = 0;

	/**
	 * Get the session object with a given id handle.
	 *
	 * @params Parameters for the GetSessionById call
	 * return
	 */
	virtual TOnlineResult<FGetSessionById> GetSessionById(FGetSessionById::Params&& Params) const = 0;

	/**
	 * Get the session set as presence session for the user.
	 *
	 * @params Parameters for the GetPresenceSession call
	 * return
	 */
	virtual TOnlineResult<FGetPresenceSession> GetPresenceSession(FGetPresenceSession::Params&& Params) const = 0;

	/**
	 * Returns whether the session with the given id is set as the presence session for the user.
	 *
	 * @params Parameters for the IsPresenceSession call
	 * return
	 */
	virtual TOnlineResult<FIsPresenceSession> IsPresenceSession(FIsPresenceSession::Params&& Params) const = 0;

	/**
	 * Sets the session with the given id as the presence session for the user.
	 *
	 * @params Parameters for the SetPresenceSession call
	 * return
	 */
	virtual TOnlineResult<FSetPresenceSession> SetPresenceSession(FSetPresenceSession::Params&& Params) = 0;

	/**
	 * Clears the presence session for the user. If no presence session is set, GetPresenceSession will return an error.
	 *
	 * @params Parameters for the ClearPresenceSession call
	 * return
	 */
	virtual TOnlineResult<FClearPresenceSession> ClearPresenceSession(FClearPresenceSession::Params&& Params) = 0;

	/**
	 * Create and join a new session.
	 *
	 * @param Parameters for the CreateSession call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FCreateSession> CreateSession(FCreateSession::Params&& Params) = 0;

	/**
	 * Update a given session's settings.
	 *
	 * @param Parameters for the UpdateSession call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FUpdateSession> UpdateSession(FUpdateSession::Params&& Params) = 0;

	/**
	 * Leave and optionally destroy a given session.
	 *
	 * @param Parameters for the LeaveSession call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FLeaveSession> LeaveSession(FLeaveSession::Params&& Params) = 0;

	/**
	 * Queries the API session service for sessions matching the given parameters.
	 *
	 * @param Parameters for the FindSessions call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FFindSessions> FindSessions(FFindSessions::Params&& Params) = 0;

	/**
	 * Starts the matchmaking process, which will either create a session with the passed parameters, or join one that matches the passed search filters.
	 *
	 * @param Parameters for the StartMatchmaking call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FStartMatchmaking> StartMatchmaking(FStartMatchmaking::Params&& Params) = 0;

	/**
	 * Starts the join process for the given session for all users provided.
	 *
	 * @param Parameters for the JoinSession call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FJoinSession> JoinSession(FJoinSession::Params&& Params) = 0;

	/**
	 * Adds a set of new session members to the named session
	 * Session member information passed will be saved in the session settings
	 * Number of open slots in the session will decrease accordingly
	 * 
	 * @params Parameters for the AddSessionMember call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FAddSessionMember> AddSessionMember(FAddSessionMember::Params&& Params) = 0;

	/**
	 * Removes a set of session member from the named session
	 * Session member information for them will be removed from session settings
	 * Number of open slots in the session will increase accordingly
	 *
	 * @params Parameters for the RemoveSessionMember call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FRemoveSessionMember> RemoveSessionMember(FRemoveSessionMember::Params&& Params) = 0;

	/**
	 * Sends an invite to the named session to all given users.
	 *
	 * @param Parameters for the SendSessionInvite call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FSendSessionInvite> SendSessionInvite(FSendSessionInvite::Params&& Params) = 0;

	/**
	 * Returns all cached session invites for the given user.
	 *
	 * @param Parameters for the SendSessionInvite call
	 * @return
	 */
	virtual TOnlineResult<FGetSessionInvites> GetSessionInvites(FGetSessionInvites::Params&& Params) = 0;

	/**
	 * Rejects a given session invite for a user.
	 *
	 * @param Parameters for the RejectSessionInvite call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FRejectSessionInvite> RejectSessionInvite(FRejectSessionInvite::Params&& Params) = 0;

	/* Events */

	/**
	 * Get the event that is triggered when a session is joined.
	 * This event will trigger as a result of creating or joining a session.
	 *
	 * @return
	 */
	virtual TOnlineEvent<void(const FSessionJoined&)> OnSessionJoined() = 0;

	/**
	 * Get the event that is triggered when a session is left.
	 * This event will trigger as a result of leaving or destroying a session.
	 *
	 * @return
	 */
	virtual TOnlineEvent<void(const FSessionLeft&)> OnSessionLeft() = 0;

	/**
	 * Get the event that is triggered when a session invite is accepted.
	 * This event will trigger as a result of accepting a platform session invite.
	 *
	 * @return
	 */
	virtual TOnlineEvent<void(const FSessionUpdated&)> OnSessionUpdated() = 0;

	/**
	 * Get the event that is triggered when a session invite is received.
	 * This event will trigger as a result of receiving a platform session invite.
	 *
	 * @return
	 */
	virtual TOnlineEvent<void(const FSessionInviteReceived&)> OnSessionInviteReceived() = 0;

	/**
	 * Get the event that is triggered when a session is joined via UI.
	 * This event will trigger as a result of joining a session via the platform UI.
	 *
	 * @return
	 */
	virtual TOnlineEvent<void(const FUISessionJoinRequested&)> OnUISessionJoinRequested() = 0;
};

namespace Meta {

BEGIN_ONLINE_STRUCT_META(FFindSessionsSearchFilter)
	ONLINE_STRUCT_FIELD(FFindSessionsSearchFilter, Key),
	ONLINE_STRUCT_FIELD(FFindSessionsSearchFilter, ComparisonOp),
	ONLINE_STRUCT_FIELD(FFindSessionsSearchFilter, Value)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FCustomSessionSetting)
	ONLINE_STRUCT_FIELD(FCustomSessionSetting, Data),
	ONLINE_STRUCT_FIELD(FCustomSessionSetting, Visibility),
	ONLINE_STRUCT_FIELD(FCustomSessionSetting, ID)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FCustomSessionSettingUpdate)
	ONLINE_STRUCT_FIELD(FCustomSessionSettingUpdate, OldValue),
	ONLINE_STRUCT_FIELD(FCustomSessionSettingUpdate, NewValue)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionMember)
	ONLINE_STRUCT_FIELD(FSessionMember, MemberSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionMemberUpdate)
	ONLINE_STRUCT_FIELD(FSessionMemberUpdate, UpdatedMemberSettings),
	ONLINE_STRUCT_FIELD(FSessionMemberUpdate, RemovedMemberSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionSettings)
	ONLINE_STRUCT_FIELD(FSessionSettings, SchemaName),
	ONLINE_STRUCT_FIELD(FSessionSettings, NumMaxConnections),
	ONLINE_STRUCT_FIELD(FSessionSettings, JoinPolicy),
	ONLINE_STRUCT_FIELD(FSessionSettings, bAllowNewMembers),
	ONLINE_STRUCT_FIELD(FSessionSettings, CustomSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionInfo)
	ONLINE_STRUCT_FIELD(FSessionInfo, SessionId),
	ONLINE_STRUCT_FIELD(FSessionInfo, SessionIdOverride),
	ONLINE_STRUCT_FIELD(FSessionInfo, bIsLANSession),
	ONLINE_STRUCT_FIELD(FSessionInfo, bIsDedicatedServerSession),
	ONLINE_STRUCT_FIELD(FSessionInfo, bAllowSanctionedPlayers),
	ONLINE_STRUCT_FIELD(FSessionInfo, bAntiCheatProtected)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionSettingsUpdate)
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, SchemaName),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, NumMaxConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, JoinPolicy),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bAllowNewMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, UpdatedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, RemovedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, UpdatedSessionMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, RemovedSessionMembers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionInvite)
	ONLINE_STRUCT_FIELD(FSessionInvite, RecipientId),
	ONLINE_STRUCT_FIELD(FSessionInvite, SenderId),
	ONLINE_STRUCT_FIELD(FSessionInvite, InviteId),
	ONLINE_STRUCT_FIELD(FSessionInvite, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetAllSessions::Params)
	ONLINE_STRUCT_FIELD(FGetAllSessions::Params, LocalAccountId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetAllSessions::Result)
	ONLINE_STRUCT_FIELD(FGetAllSessions::Result, Sessions)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionByName::Params)
	ONLINE_STRUCT_FIELD(FGetSessionByName::Params, LocalName)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionByName::Result)
	ONLINE_STRUCT_FIELD(FGetSessionByName::Result, Session)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionById::Params)
	ONLINE_STRUCT_FIELD(FGetSessionById::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FGetSessionById::Params, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionById::Result)
	ONLINE_STRUCT_FIELD(FGetSessionById::Result, Session)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetPresenceSession::Params)
	ONLINE_STRUCT_FIELD(FGetPresenceSession::Params, LocalAccountId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetPresenceSession::Result)
	ONLINE_STRUCT_FIELD(FGetPresenceSession::Result, Session)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FIsPresenceSession::Params)
ONLINE_STRUCT_FIELD(FIsPresenceSession::Params, LocalAccountId),
ONLINE_STRUCT_FIELD(FIsPresenceSession::Params, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FIsPresenceSession::Result)
	ONLINE_STRUCT_FIELD(FIsPresenceSession::Result, bIsPresenceSession)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSetPresenceSession::Params)
	ONLINE_STRUCT_FIELD(FSetPresenceSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FSetPresenceSession::Params, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSetPresenceSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FClearPresenceSession::Params)
	ONLINE_STRUCT_FIELD(FClearPresenceSession::Params, LocalAccountId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FClearPresenceSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FCreateSession::Params)
	ONLINE_STRUCT_FIELD(FCreateSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionMemberData),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionIdOverride),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, bPresenceEnabled),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, bIsLANSession),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, bIsDedicatedServerSession),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, bAllowSanctionedPlayers),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, bAntiCheatProtected),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FCreateSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FUpdateSession::Params)
	ONLINE_STRUCT_FIELD(FUpdateSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FUpdateSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FUpdateSession::Params, Mutations)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FUpdateSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FLeaveSession::Params)
	ONLINE_STRUCT_FIELD(FLeaveSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FLeaveSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FLeaveSession::Params, bDestroySession)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FLeaveSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FFindSessions::Params)
	ONLINE_STRUCT_FIELD(FFindSessions::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FFindSessions::Params, MaxResults),
	ONLINE_STRUCT_FIELD(FFindSessions::Params, bFindLANSessions),	
	ONLINE_STRUCT_FIELD(FFindSessions::Params, Filters),
	ONLINE_STRUCT_FIELD(FFindSessions::Params, TargetUser),
	ONLINE_STRUCT_FIELD(FFindSessions::Params, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FFindSessions::Result)
	ONLINE_STRUCT_FIELD(FFindSessions::Result, FoundSessionIds)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FStartMatchmaking::Params)
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, SessionCreationParameters),
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, SessionSearchFilters)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FStartMatchmaking::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FJoinSession::Params)
	ONLINE_STRUCT_FIELD(FJoinSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, SessionId),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, SessionMemberData),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, bPresenceEnabled)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FJoinSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FAddSessionMember::Params)
	ONLINE_STRUCT_FIELD(FAddSessionMember::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FAddSessionMember::Params, SessionName),
	ONLINE_STRUCT_FIELD(FAddSessionMember::Params, NewSessionMember)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FAddSessionMember::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRemoveSessionMember::Params)
	ONLINE_STRUCT_FIELD(FRemoveSessionMember::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FRemoveSessionMember::Params, SessionName)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRemoveSessionMember::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSendSessionInvite::Params)
	ONLINE_STRUCT_FIELD(FSendSessionInvite::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FSendSessionInvite::Params, SessionName),
	ONLINE_STRUCT_FIELD(FSendSessionInvite::Params, TargetUsers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSendSessionInvite::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionInvites::Params)
	ONLINE_STRUCT_FIELD(FGetSessionInvites::Params, LocalAccountId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionInvites::Result)
	ONLINE_STRUCT_FIELD(FGetSessionInvites::Result, SessionInvites)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRejectSessionInvite::Params)
	ONLINE_STRUCT_FIELD(FRejectSessionInvite::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FRejectSessionInvite::Params, SessionInviteId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRejectSessionInvite::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionSettingsChanges)
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, SchemaName),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, NumMaxConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, JoinPolicy),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, bAllowNewMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, AddedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, ChangedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsChanges, RemovedCustomSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionMemberChanges)
	ONLINE_STRUCT_FIELD(FSessionMemberChanges, AddedMemberSettings),
	ONLINE_STRUCT_FIELD(FSessionMemberChanges, ChangedMemberSettings),
	ONLINE_STRUCT_FIELD(FSessionMemberChanges, RemovedMemberSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionUpdate)
	ONLINE_STRUCT_FIELD(FSessionUpdate, OwnerAccountId),
	ONLINE_STRUCT_FIELD(FSessionUpdate, SessionSettingsChanges),
	ONLINE_STRUCT_FIELD(FSessionUpdate, SessionMembersChanges),
	ONLINE_STRUCT_FIELD(FSessionUpdate, RemovedSessionMembers)
END_ONLINE_STRUCT_META()

/* Meta*/ }

/* UE::Online */ }