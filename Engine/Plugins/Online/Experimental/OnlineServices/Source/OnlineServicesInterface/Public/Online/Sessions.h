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

/** A player registered with the session, whether they are in it or not */
struct FRegisteredPlayer
{
	/* Whether a slot was reserved for this player upon registration */
	bool bHasReservedSlot = false;

	/* Whether the player is currently in the session */
	bool bIsInSession = false;
};

using FRegisteredPlayersMap = TMap<FAccountId, FRegisteredPlayer>;

struct FSessionSettingsUpdate
{
	/** Set with an updated value if the SchemaName field will be changed in the update operation */
	TOptional<FSchemaId> SchemaName;
	/** Set with an updated value if the NumMaxPublicConnections field will be changed in the update operation */
	TOptional<uint32> NumMaxPublicConnections;
	/** Set with an updated value if the NumOpenPublicConnections field will be changed in the update operation */
	TOptional<uint32> NumOpenPublicConnections;
	/** Set with an updated value if the NumMaxPrivateConnections field will be changed in the update operation */
	TOptional<uint32> NumMaxPrivateConnections;
	/** Set with an updated value if the NumOpenPrivateConnections field will be changed in the update operation */
	TOptional<uint32> NumOpenPrivateConnections;
	/** Set with an updated value if the JoinPolicy field will be changed in the update operation */
	TOptional<ESessionJoinPolicy> JoinPolicy;
	/** Set with an updated value if the SessionIdOverride field will be changed in the update operation */
	TOptional<FString> SessionIdOverride;
	/** Set with an updated value if the bIsDedicatedServerSession field will be changed in the update operation */
	TOptional<bool> bIsDedicatedServerSession;
	/** Set with an updated value if the bAllowNewMembers field will be changed in the update operation */
	TOptional<bool> bAllowNewMembers;
	/** Set with an updated value if the bAllowSanctionedPlayers field will be changed in the update operation */
	TOptional<bool> bAllowSanctionedPlayers;
	/** Set with an updated value if the bAllowUnregisteredPlayers field will be changed in the update operation */
	TOptional<bool> bAllowUnregisteredPlayers;
	/** Set with an updated value if the bAntiCheatProtected field will be changed in the update operation */
	TOptional<bool> bAntiCheatProtected;
	/** Set with an updated value if the bPresenceEnabled field will be changed in the update operation */
	TOptional<bool> bPresenceEnabled;

	/** Updated values for custom settings to change in the update operation*/
	FCustomSessionSettingsMap UpdatedCustomSettings;
	/** Names of custom settings to be removed in the update operation*/
	TArray<FSchemaAttributeId> RemovedCustomSettings;

	/** Updated values for session member info to change in the update operation*/
	FSessionMemberUpdatesMap UpdatedSessionMembers;
	/** Id handles for session members to be removed in the update operation*/
	TArray<FAccountId> RemovedSessionMembers;

	/** Updated values for registered players to change in the update operation*/
	FRegisteredPlayersMap UpdatedRegisteredPlayers;
	/** Id handles for registered players to be removed in the update operation*/
	TArray<FAccountId> RemovedRegisteredPlayers;

	FSessionSettingsUpdate& operator+=(FSessionSettingsUpdate&& UpdatedValue);
};

struct ONLINESERVICESINTERFACE_API FSessionSettings
{
	/* The schema which will be applied to the session */
	FSchemaId SchemaName;

	/* Maximum number of public slots for session members */
	uint32 NumMaxPublicConnections = 0;

	/* Number of available public slots for session members */
	uint32 NumOpenPublicConnections = 0;

	/* Maximum number of private slots for session members */
	uint32 NumMaxPrivateConnections = 0;

	/* Number of available private slots for session members */
	uint32 NumOpenPrivateConnections = 0;

	/* Enum value describing the level of restriction to join the session */
	ESessionJoinPolicy JoinPolicy = ESessionJoinPolicy::Public;

	/* In platforms that support this feature, it will set the session id to this value. Might be subject to minimum and maximum length */
	FString SessionIdOverride;

	/* Whether the session is only available in the local network and not via internet connection. Only available in some platforms. False by default */
	bool bIsLANSession = false;

	/* Whether the session is configured to run as a dedicated server. Only available in some platforms. False by default */
	bool bIsDedicatedServerSession = false;

	/* Whether players (registered or not) are accepted as new members in the session. Can vary depending on various factors, like the number of free slots available, or Join-In-Progress preferences when the session has started. True by default */
	bool bAllowNewMembers = true;

	/* Whether this session will allow sanctioned players to join it. True by default */
	bool bAllowSanctionedPlayers = true;

	/* Whether this session will allow unregistered players to join it. True by default */
	bool bAllowUnregisteredPlayers = true;

	/*Whether this is a secure session protected by anti-cheat services. False by default */
	bool bAntiCheatProtected = false;

	/* Whether this session will show its information in presence updates. Can only be set in one session at a time. False by default */
	bool bPresenceEnabled = false;
  
 	/* Map of user-defined settings to be passed to the platform APIs as additional information for various purposes */
 	FCustomSessionSettingsMap CustomSettings;
 
 	/* Map of session member ids to their corresponding user-defined settings */
 	FSessionMembersMap SessionMembers;
 
 	/* Map of registered players for this session. Can only be altered via RegisterPlayers and UnregisterPlayers */
 	FRegisteredPlayersMap RegisteredPlayers;

	FSessionSettings& operator+=(const FSessionSettingsUpdate& UpdatedValue);
};

class ISession
{
public:
	virtual const FAccountId GetOwnerAccountId() const = 0;
	virtual const FOnlineSessionIdHandle GetSessionId() const = 0;
	virtual const FSessionSettings GetSessionSettings() const = 0;
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
	FOnlineSessionInviteIdHandle InviteId;

	/* Pointer to the session information */
	FOnlineSessionIdHandle SessionId;

	// TODO: Default constructor will be deleted after we cache invites
};

struct FGetAllSessions
{
	static constexpr TCHAR Name[] = TEXT("GetAllSessions");

	struct Params
	{

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

		FOnlineSessionIdHandle IdHandle;
	};

	struct Result
	{
		TSharedRef<const ISession> Session;

		Result() = delete; // cannot default construct due to TSharedRef
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

		/** Settings object to define session properties during creation */
		FSessionSettings SessionSettings;

		/** Information for all local users who will join the session (includes the session creator) */
		FSessionMembersMap LocalAccounts;
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
		/* The local user agent which leaves the session*/
		FAccountId LocalAccountId;

		/* The local name for the session. */
		FName SessionName;

		/** Ids for all local users who will leave the session (includes the main caller) */
		TArray<FAccountId> LocalAccounts;

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
		TOptional<FOnlineSessionIdHandle> SessionId;
	};

	struct Result
	{
		TArray<FOnlineSessionIdHandle> FoundSessionIds;
	};
};

struct FStartMatchmaking
{
	static constexpr TCHAR Name[] = TEXT("StartMatchmaking");

	struct Params
	{
		/** The local user agent which will perform the action. */
		FAccountId LocalAccountId;

		/* Information for all local users who will join the session (includes the session creator) */
		FSessionMembersMap LocalAccounts;

		/* Local name for the session */
		FName SessionName;

		/* Preferred settings to be used during session creation */
		FSessionSettings SessionSettings;

		/* Filters to apply when searching for sessions */
		TArray<FFindSessionsSearchFilter> SearchFilters;
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
		FOnlineSessionIdHandle SessionId;

		/* Information for all local users who will join the session (includes the session creator). Any player that wants to join the session needs defined information to become a new member */
		FSessionMembersMap LocalAccounts;
	};

	struct Result
	{

	};
};

struct FAddSessionMembers
{
	static constexpr TCHAR Name[] = TEXT("AddSessionMembers");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* Local name for the session */
		FName SessionName;

		/** Information for the session members to be added to the session. Any player that joins the session becomes a new member in doing so */
		FSessionMembersMap NewSessionMembers;

		/** Whether or not the new session members should also be added to the list of registered players. True by default*/
		bool bRegisterPlayers = true;
	};

	struct Result
	{

	};
};

struct FRemoveSessionMembers
{
	static constexpr TCHAR Name[] = TEXT("RemoveSessionMembers");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* Local name for the session */
		FName SessionName;

		/* Id handles for the session members to be removed from the session */
		TArray<FAccountId> SessionMemberIds;

		/** Whether or not the session members should also be removed from the list of registered players. True by default*/
		bool bUnregisterPlayers = true;
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
		FOnlineSessionInviteIdHandle SessionInviteId;
	};

	struct Result
	{
	};
};

struct FRegisterPlayers
{
	static constexpr TCHAR Name[] = TEXT("RegisterPlayers");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* The local name for the session. */
		FName SessionName;

		/* Array of users which will be registered */
		TArray<FAccountId> TargetUsers;

		/* Whether a slot should be saved for the registered players */
		bool bReserveSlot;
	};

	struct Result
	{
	};
};

struct FUnregisterPlayers
{
	static constexpr TCHAR Name[] = TEXT("UnregisterPlayers");

	struct Params
	{
		/* The local user agent */
		FAccountId LocalAccountId;

		/* The local name for the session. */
		FName SessionName;

		/* Array of users which will be unregistered */
		TArray<FAccountId> TargetUsers;

		/* Whether unregistered players should be removed from the session, if they are in it */
		bool bRemoveUnregisteredPlayers;
	};

	struct Result
	{
	};
};

/* Events */

struct FSessionJoined
{
	/* The local users which joined the session */
	TArray<FAccountId> LocalAccountIds;

	/* A shared reference to the session joined. */
	FOnlineSessionIdHandle SessionId;
};

struct FSessionLeft
{
	/* The local users which left the session */
	TArray<FAccountId> LocalAccountIds;
};

struct FSessionUpdated
{
	/* Local name for the updated session object */
	FName SessionName;

	/* Updated session settings */
	FSessionSettingsUpdate UpdatedSettings;
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
	TResult<FOnlineSessionIdHandle, FOnlineError> Result;

	/** Join request source */
	EUISessionJoinRequestedSource JoinRequestedSource = EUISessionJoinRequestedSource::Unspecified;

	FUISessionJoinRequested() = delete; // cannot default construct due to TResult
};

class ISessions
{
public:
	/**
	 * Get an array of all session objects.
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
	 * If indicated, players will also be registered in the session
	 * 
	 * @params Parameters for the AddSessionMember call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FAddSessionMembers> AddSessionMembers(FAddSessionMembers::Params&& Params) = 0;

	/**
	 * Removes a set of session member from the named session
	 * Session member information for them will be removed from session settings
	 * Number of open slots in the session will increase accordingly
	 * If indicated, players will also be unregistered from the session
	 *
	 * @params Parameters for the RemoveSessionMember call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FRemoveSessionMembers> RemoveSessionMembers(FRemoveSessionMembers::Params&& Params) = 0;

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

	/**
	 * Registers given players in the named session
	 * If indicated, and if any are available, a slot in the session will be reserved for them
	 *
	 * @param Parameters for the RegisterPlayers call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FRegisterPlayers> RegisterPlayers(FRegisterPlayers::Params&& Params) = 0;

	/**
	 * Unregisters given players from the named session.
	 * If indicated, and if they are members of it, players will also be removed from the session
	 *
	 * @param Parameters for the UnregisterPlayers call
	 * @return
	 */
	virtual TOnlineAsyncOpHandle<FUnregisterPlayers> UnregisterPlayers(FUnregisterPlayers::Params&& Params) = 0;

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

BEGIN_ONLINE_STRUCT_META(FSessionMember)
	ONLINE_STRUCT_FIELD(FSessionMember, MemberSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionMemberUpdate)
	ONLINE_STRUCT_FIELD(FSessionMemberUpdate, UpdatedMemberSettings),
	ONLINE_STRUCT_FIELD(FSessionMemberUpdate, RemovedMemberSettings)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRegisteredPlayer)
	ONLINE_STRUCT_FIELD(FRegisteredPlayer, bHasReservedSlot),
	ONLINE_STRUCT_FIELD(FRegisteredPlayer, bIsInSession)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionSettings)
	ONLINE_STRUCT_FIELD(FSessionSettings, SchemaName),
	ONLINE_STRUCT_FIELD(FSessionSettings, NumMaxPublicConnections),
	ONLINE_STRUCT_FIELD(FSessionSettings, NumOpenPublicConnections),
	ONLINE_STRUCT_FIELD(FSessionSettings, NumMaxPrivateConnections),
	ONLINE_STRUCT_FIELD(FSessionSettings, NumOpenPrivateConnections),
	ONLINE_STRUCT_FIELD(FSessionSettings, JoinPolicy),
	ONLINE_STRUCT_FIELD(FSessionSettings, SessionIdOverride),
	ONLINE_STRUCT_FIELD(FSessionSettings, bIsLANSession),
	ONLINE_STRUCT_FIELD(FSessionSettings, bIsDedicatedServerSession),
	ONLINE_STRUCT_FIELD(FSessionSettings, bAllowNewMembers),
	ONLINE_STRUCT_FIELD(FSessionSettings, bAllowSanctionedPlayers),
	ONLINE_STRUCT_FIELD(FSessionSettings, bAllowUnregisteredPlayers),
	ONLINE_STRUCT_FIELD(FSessionSettings, bAntiCheatProtected),
	ONLINE_STRUCT_FIELD(FSessionSettings, bPresenceEnabled),
	ONLINE_STRUCT_FIELD(FSessionSettings, CustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettings, SessionMembers),
	ONLINE_STRUCT_FIELD(FSessionSettings, RegisteredPlayers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionSettingsUpdate)
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, SchemaName),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, NumMaxPublicConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, NumOpenPublicConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, NumMaxPrivateConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, NumOpenPrivateConnections),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, JoinPolicy),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, SessionIdOverride),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bIsDedicatedServerSession),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bAllowNewMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bAllowSanctionedPlayers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bAllowUnregisteredPlayers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bAntiCheatProtected),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, bPresenceEnabled),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, UpdatedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, RemovedCustomSettings),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, UpdatedSessionMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, RemovedSessionMembers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, UpdatedRegisteredPlayers),
	ONLINE_STRUCT_FIELD(FSessionSettingsUpdate, RemovedRegisteredPlayers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FSessionInvite)
	ONLINE_STRUCT_FIELD(FSessionInvite, RecipientId),
	ONLINE_STRUCT_FIELD(FSessionInvite, SenderId),
	ONLINE_STRUCT_FIELD(FSessionInvite, InviteId),
	ONLINE_STRUCT_FIELD(FSessionInvite, SessionId)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetAllSessions::Params)
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
	ONLINE_STRUCT_FIELD(FGetSessionById::Params, IdHandle)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FGetSessionById::Result)
	ONLINE_STRUCT_FIELD(FGetSessionById::Result, Session)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FCreateSession::Params)
	ONLINE_STRUCT_FIELD(FCreateSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, SessionSettings),
	ONLINE_STRUCT_FIELD(FCreateSession::Params, LocalAccounts)
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
	ONLINE_STRUCT_FIELD(FLeaveSession::Params, LocalAccounts),
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
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, LocalAccounts),
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, SessionName),
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, SessionSettings),
	ONLINE_STRUCT_FIELD(FStartMatchmaking::Params, SearchFilters)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FStartMatchmaking::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FJoinSession::Params)
	ONLINE_STRUCT_FIELD(FJoinSession::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, SessionName),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, SessionId),
	ONLINE_STRUCT_FIELD(FJoinSession::Params, LocalAccounts)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FJoinSession::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FAddSessionMembers::Params)
	ONLINE_STRUCT_FIELD(FAddSessionMembers::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FAddSessionMembers::Params, SessionName),
	ONLINE_STRUCT_FIELD(FAddSessionMembers::Params, NewSessionMembers),
	ONLINE_STRUCT_FIELD(FAddSessionMembers::Params, bRegisterPlayers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FAddSessionMembers::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRemoveSessionMembers::Params)
	ONLINE_STRUCT_FIELD(FRemoveSessionMembers::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FRemoveSessionMembers::Params, SessionName),
	ONLINE_STRUCT_FIELD(FRemoveSessionMembers::Params, SessionMemberIds),
	ONLINE_STRUCT_FIELD(FRemoveSessionMembers::Params, bUnregisterPlayers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRemoveSessionMembers::Result)
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

BEGIN_ONLINE_STRUCT_META(FRegisterPlayers::Params)
	ONLINE_STRUCT_FIELD(FRegisterPlayers::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FRegisterPlayers::Params, SessionName),
	ONLINE_STRUCT_FIELD(FRegisterPlayers::Params, TargetUsers),
	ONLINE_STRUCT_FIELD(FRegisterPlayers::Params, bReserveSlot)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FRegisterPlayers::Result)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FUnregisterPlayers::Params)
	ONLINE_STRUCT_FIELD(FUnregisterPlayers::Params, LocalAccountId),
	ONLINE_STRUCT_FIELD(FUnregisterPlayers::Params, SessionName),
	ONLINE_STRUCT_FIELD(FUnregisterPlayers::Params, TargetUsers),
	ONLINE_STRUCT_FIELD(FUnregisterPlayers::Params, bRemoveUnregisteredPlayers)
END_ONLINE_STRUCT_META()

BEGIN_ONLINE_STRUCT_META(FUnregisterPlayers::Result)
END_ONLINE_STRUCT_META()

/* Meta*/ }

/* UE::Online */ }