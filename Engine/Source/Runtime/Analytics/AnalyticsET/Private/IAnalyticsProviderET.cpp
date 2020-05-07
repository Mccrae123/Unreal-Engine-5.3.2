// Copyright Epic Games, Inc. All Rights Reserved.

#include "IAnalyticsProviderET.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeLock.h"
#include "Stats/Stats.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"
#include "Misc/TimeGuard.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "AnalyticsProviderETEventCache.h"
#include "AnalyticsET.h"
#include "Analytics.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "HttpModule.h"
#include "PlatformHttp.h"
#include "Misc/EngineVersion.h"
#include "HttpRetrySystem.h"

#include "AnalyticsPerfTracker.h"

/**
 * Implementation of analytics for Epic Telemetry.
 * Supports caching events and flushing them periodically (currently hardcoded limits).
 * Also supports a set of default attributes that will be added to every event.
 * For efficiency, this set of attributes is added directly into the set of cached events
 * with a special flag to indicate its purpose. This allows the set of cached events to be used like
 * a set of commands to be executed on flush, and allows us to inject the default attributes
 * efficiently into many events without copying the array at all.
 */
class FAnalyticsProviderET :
	public IAnalyticsProviderET,
	public FTickerObjectBase,
	public TSharedFromThis<FAnalyticsProviderET>
{
public:
	FAnalyticsProviderET(const FAnalyticsET::Config& ConfigValues);

	// FTickerObjectBase

	bool Tick(float DeltaSeconds) override;

	// IAnalyticsProvider

	virtual bool StartSession(const TArray<FAnalyticsEventAttribute>& Attributes) override;
	virtual bool StartSession(TArray<FAnalyticsEventAttribute>&& Attributes) override;
	virtual bool StartSession(FString InSessionID, TArray<FAnalyticsEventAttribute>&& Attributes) override;
	virtual void EndSession() override;
	virtual void FlushEvents() override;

	virtual void SetAppID(FString&& AppID) override;
	virtual const FString& GetAppID() const override;
	virtual void SetAppVersion(FString&& AppVersion) override;
	virtual const FString& GetAppVersion() const override;
	virtual void SetUserID(const FString& InUserID) override;
	virtual FString GetUserID() const override;

	virtual FString GetSessionID() const override;
	virtual bool SetSessionID(const FString& InSessionID) override;

	virtual bool ShouldRecordEvent(const FString& EventName) const override;
	virtual void RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes) override;
	virtual void RecordEvent(FString EventName, TArray<FAnalyticsEventAttribute>&& Attributes) override;
	virtual void RecordEventJson(FString EventName, TArray<FAnalyticsEventAttribute>&& AttributesJson) override;
	virtual void SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes) override;
	virtual const TArray<FAnalyticsEventAttribute>& GetDefaultEventAttributes() const override;
	virtual void SetEventCallback(const OnEventRecorded& Callback) override;

	virtual void SetURLEndpoint(const FString& UrlEndpoint, const TArray<FString>& AltDomains) override;
	virtual void BlockUntilFlushed(float InTimeoutSec) override;
	virtual void SetShouldRecordEventFunc(const ShouldRecordEventFunction& InShouldRecordEventFunc) override;
	virtual ~FAnalyticsProviderET();

	virtual const FAnalyticsET::Config& GetConfig() const override { return Config; }

private:

	/** Create a request utilizing HttpRetry domains */
	TSharedRef<IHttpRequest> CreateRequest();

	bool bSessionInProgress;
	/** The current configuration (might be updated with respect to the one provided at construction). */
	FAnalyticsET::Config Config;
	/** the unique UserID as passed to ET. */
	FString UserID;
	/** The session ID */
	FString SessionID;
	/** Max number of analytics events to cache before pushing to server */
	const int32 MaxCachedNumEvents;
	/** Max time that can elapse before pushing cached events to server */
	const float MaxCachedElapsedTime;
	/** Allows events to not be cached when -AnalyticsDisableCaching is used. This should only be used for debugging as caching significantly reduces bandwidth overhead per event. */
	bool bShouldCacheEvents;
	/** Current countdown timer to keep track of MaxCachedElapsedTime push */
	float FlushEventsCountdown;
	/** Track destructing for unbinding callbacks when firing events at shutdown */
	bool bInDestructor;

	/**
	* Analytics event entry to be cached
	*/
	struct FAnalyticsEventEntry
	{
		/** name of event */
		FString EventName;
		/** optional list of attributes */
		TArray<FAnalyticsEventAttribute> Attributes;
		/** local time when event was triggered */
		FDateTime TimeStamp;
		/** Whether this event was added using the Json API. */
		uint32 bIsJsonEvent : 1;
		/** Whether this event is setting the default attributes to add to all events. Every cached event list will start with one of these, though it may be empty. */
		uint32 bIsDefaultAttributes : 1;
		/**
		* Constructor. Requires rvalue-refs to ensure we move values efficiently into this struct.
		*/
		FAnalyticsEventEntry(FString&& InEventName, TArray<FAnalyticsEventAttribute>&& InAttributes, bool bInIsJsonEvent, bool bInIsDefaultAttributes)
			: EventName(MoveTemp(InEventName))
			, Attributes(MoveTemp(InAttributes))
			, TimeStamp(FDateTime::UtcNow())
			, bIsJsonEvent(bInIsJsonEvent)
			, bIsDefaultAttributes(bInIsDefaultAttributes)
		{}
	};

	FAnalyticsProviderETEventCache EventCache;
	/** Needed to support the old, unsafe GetDefaultAttributes() API. */
	mutable TArray<FAnalyticsEventAttribute> UnsafeDefaultAttributes;

	TArray<OnEventRecorded> EventRecordedCallbacks;

	/** Event filter function */
	ShouldRecordEventFunction ShouldRecordEventFunc;

	/**
	* Delegate called when an event Http request completes
	*/
	void EventRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded);

	TSharedPtr<class FHttpRetrySystem::FManager> HttpRetryManager;
	FHttpRetrySystem::FRetryDomainsPtr RetryServers;
};

TSharedPtr<IAnalyticsProviderET> FAnalyticsET::CreateAnalyticsProvider(const Config& ConfigValues) const
{
	// If we didn't have a proper APIKey, return NULL
	if (ConfigValues.APIKeyET.IsEmpty())
	{
		UE_LOG(LogAnalytics, Warning, TEXT("CreateAnalyticsProvider config not contain required parameter %s"), *Config::GetKeyNameForAPIKey());
		return NULL;
	}
	return MakeShared<FAnalyticsProviderET>(ConfigValues);
}

/**
 * Perform any initialization.
 */
FAnalyticsProviderET::FAnalyticsProviderET(const FAnalyticsET::Config& ConfigValues)
	: bSessionInProgress(false)
	, Config(ConfigValues)
	, MaxCachedNumEvents(20)
	, MaxCachedElapsedTime(60.0f)
	, bShouldCacheEvents(true)
	, FlushEventsCountdown(MaxCachedElapsedTime)
	, bInDestructor(false)
{
	if (Config.APIKeyET.IsEmpty() || Config.APIServerET.IsEmpty())
	{
		UE_LOG(LogAnalytics, Fatal, TEXT("AnalyticsET: APIKey (%s) and APIServer (%s) cannot be empty!"), *Config.APIKeyET, *Config.APIServerET);
	}

	// Set the number of retries to the number of retry URLs that have been passed in.
	uint32 RetryLimitCount = ConfigValues.AltAPIServersET.Num();

	HttpRetryManager = MakeShared<FHttpRetrySystem::FManager>(
		FHttpRetrySystem::FRetryLimitCountSetting(RetryLimitCount),
		FHttpRetrySystem::FRetryTimeoutRelativeSecondsSetting()
		);

	// If we have retry domains defined, insert the default domain into the list
	if (RetryLimitCount > 0)
	{
		TArray<FString> TmpAltAPIServers = ConfigValues.AltAPIServersET;

		FString DefaultUrlDomain = FPlatformHttp::GetUrlDomain(Config.APIServerET);
		if (!TmpAltAPIServers.Contains(DefaultUrlDomain))
		{
			TmpAltAPIServers.Insert(DefaultUrlDomain, 0);
		}

		RetryServers = MakeShared<FHttpRetrySystem::FRetryDomains, ESPMode::ThreadSafe>(MoveTemp(TmpAltAPIServers));
	}

	// force very verbose logging if we are force-disabling events.
	bool bForceDisableCaching = FParse::Param(FCommandLine::Get(), TEXT("ANALYTICSDISABLECACHING"));
	if (bForceDisableCaching)
	{
		UE_SET_LOG_VERBOSITY(LogAnalytics, VeryVerbose);
		bShouldCacheEvents = false;
	}

	UE_LOG(LogAnalytics, Verbose, TEXT("[%s] Initializing ET Analytics provider"), *Config.APIKeyET);

	// default to FEngineVersion::Current() if one is not provided, append FEngineVersion::Current() otherwise.
	FString ConfigAppVersion = ConfigValues.AppVersionET;
	// Allow the cmdline to force a specific AppVersion so it can be set dynamically.
	FParse::Value(FCommandLine::Get(), TEXT("ANALYTICSAPPVERSION="), ConfigAppVersion, false);
	Config.AppVersionET = ConfigAppVersion.IsEmpty()
		? FString(FApp::GetBuildVersion())
		: ConfigAppVersion.Replace(TEXT("%VERSION%"), FApp::GetBuildVersion(), ESearchCase::CaseSensitive);

	UE_LOG(LogAnalytics, Log, TEXT("[%s] APIServer = %s. AppVersion = %s"), *Config.APIKeyET, *Config.APIServerET, *Config.AppVersionET);

	// only need these if we are using the data router protocol.
	if (!Config.UseLegacyProtocol)
	{
		Config.AppEnvironment = ConfigValues.AppEnvironment.IsEmpty()
			? FAnalyticsET::Config::GetDefaultAppEnvironment()
			: ConfigValues.AppEnvironment;
		Config.UploadType = ConfigValues.UploadType.IsEmpty()
			? FAnalyticsET::Config::GetDefaultUploadType()
			: ConfigValues.UploadType;
	}

	// see if there is a cmdline supplied UserID.
#if !UE_BUILD_SHIPPING
	FString ConfigUserID;
	if (FParse::Value(FCommandLine::Get(), TEXT("ANALYTICSUSERID="), ConfigUserID, false))
	{
		SetUserID(ConfigUserID);
	}
#endif // !UE_BUILD_SHIPPING
}

bool FAnalyticsProviderET::Tick(float DeltaSeconds)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FAnalyticsProviderET_Tick);

	HttpRetryManager->Update();

	// hold a lock the entire time here because we're making several calls to the event cache that we need to be consistent when we decide to flush.
	// With more care, we can likely avoid holding this lock the entire time.
	FAnalyticsProviderETEventCache::Lock EventCacheLock(EventCache);

	if (EventCache.CanFlush())
	{
		// Countdown to flush
		FlushEventsCountdown -= DeltaSeconds;
		// If reached countdown or already at max cached events then flush
		if (FlushEventsCountdown <= 0 ||
			EventCache.GetNumCachedEvents()	>= MaxCachedNumEvents)
		{
			// Never tick-flush more than one provider in a single frame. There's non-trivial overhead to flushing events.
			// On servers where there may be dozens of provider instances, this will spread out the cost a bit.
			// If caching is disabled, we still want events to be flushed immediately, so we are only guarding the flush calls from tick,
			// any other calls to flush are allowed to happen in the same frame.
			static uint32 LastFrameCounterFlushed = 0;
			if (GFrameCounter == LastFrameCounterFlushed)
			{
				UE_LOG(LogAnalytics, Verbose, TEXT("Tried to flush more than one analytics provider in a single frame. Deferring until next frame."));
			}
			else
			{
				FlushEvents();
				LastFrameCounterFlushed = GFrameCounter;
			}
		}
	}
	return true;
}

FAnalyticsProviderET::~FAnalyticsProviderET()
{
	UE_LOG(LogAnalytics, Verbose, TEXT("[%s] Destroying ET Analytics provider"), *Config.APIKeyET);
	bInDestructor = true;
	EndSession();
}

/**
 * Start capturing stats for upload
 * Uses the unique ApiKey associated with your app
 */
bool FAnalyticsProviderET::StartSession(const TArray<FAnalyticsEventAttribute>& Attributes)
{
	// Have to copy Attributes array because this doesn't come in as an rvalue ref.
	return StartSession(TArray<FAnalyticsEventAttribute>(Attributes));
}

/**
 * Start capturing stats for upload with provided SessionID
 * Uses the unique ApiKey associated with your app
 */
bool FAnalyticsProviderET::StartSession(TArray<FAnalyticsEventAttribute>&& Attributes)
{
	FGuid SessionGUID;
	FPlatformMisc::CreateGuid(SessionGUID);
	return StartSession(SessionGUID.ToString(EGuidFormats::DigitsWithHyphensInBraces), MoveTemp(Attributes));;
}

bool FAnalyticsProviderET::StartSession(FString InSessionID, TArray<FAnalyticsEventAttribute>&& Attributes)
{
	UE_LOG(LogAnalytics, Log, TEXT("[%s] AnalyticsET::StartSession"), *Config.APIKeyET);

	// end/flush previous session before staring new one
	if (bSessionInProgress)
	{
		EndSession();
	}
	SessionID = MoveTemp(InSessionID);
	// always ensure we send a few specific attributes on session start.
	TArray<FAnalyticsEventAttribute> AppendedAttributes(MoveTemp(Attributes));
	// we should always know what platform is hosting this session.
	AppendedAttributes.Emplace(TEXT("Platform"), FString(FPlatformProperties::IniPlatformName()));

	RecordEvent(TEXT("SessionStart"), MoveTemp(AppendedAttributes));
	bSessionInProgress = true;
	return bSessionInProgress;
}

/**
 * End capturing stats and queue the upload
 */
void FAnalyticsProviderET::EndSession()
{
	if (bSessionInProgress)
	{
		RecordEvent(TEXT("SessionEnd"), TArray<FAnalyticsEventAttribute>());
	}
	FlushEvents();
	SessionID.Empty();

	bSessionInProgress = false;
}

TSharedRef<IHttpRequest> FAnalyticsProviderET::CreateRequest()
{
	// TODO add config values for retries, for now, using default
	TSharedRef<IHttpRequest> HttpRequest = HttpRetryManager->CreateRequest(FHttpRetrySystem::FRetryLimitCountSetting(),
		FHttpRetrySystem::FRetryTimeoutRelativeSecondsSetting(),
		FHttpRetrySystem::FRetryResponseCodes(),
		FHttpRetrySystem::FRetryVerbs(),
		RetryServers);

	return HttpRequest;
}

void FAnalyticsProviderET::FlushEvents()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FAnalyticsProviderET_FlushEvents);
	// Warn if this takes more than 2 ms
	SCOPE_TIME_GUARD_MS(TEXT("FAnalyticsProviderET::FlushEvents"), 2);

	// Make sure we don't try to flush too many times. When we are not caching events it's possible this can be called when there are no events in the array.
	if (!EventCache.CanFlush())
	{
		return;
	}

	ANALYTICS_FLUSH_TRACKING_BEGIN();
	int EventCount = 0;
	int PayloadSize = 0;

	if(ensure(FModuleManager::Get().IsModuleLoaded("HTTP")))
	{
		if (!Config.UseLegacyProtocol)
		{
			FString Payload = EventCache.FlushCache();
			// UrlEncode NOTE: need to concatenate everything
			FString URLPath  = TEXT("datarouter/api/v1/public/data?SessionID=") + FPlatformHttp::UrlEncode(SessionID);
					URLPath += TEXT("&AppID=") + FPlatformHttp::UrlEncode(Config.APIKeyET);
					URLPath += TEXT("&AppVersion=") + FPlatformHttp::UrlEncode(Config.AppVersionET);
					URLPath += TEXT("&UserID=") + FPlatformHttp::UrlEncode(UserID);
					URLPath += TEXT("&AppEnvironment=") + FPlatformHttp::UrlEncode(Config.AppEnvironment);
					URLPath += TEXT("&UploadType=") + FPlatformHttp::UrlEncode(Config.UploadType);
			PayloadSize = URLPath.Len() + Payload.Len();

			if (UE_LOG_ACTIVE(LogAnalytics, VeryVerbose))
			{
				// Recreate the URLPath for logging because we do not want to escape the parameters when logging.
				// We cannot simply UrlEncode the entire Path after logging it because UrlEncode(Params) != UrlEncode(Param1) & UrlEncode(Param2) ...
				FString LogString = FString::Printf(TEXT("[%s] AnalyticsET URL:datarouter/api/v1/public/data?SessionID=%s&AppID=%s&AppVersion=%s&UserID=%s&AppEnvironment=%s&UploadType=%s. Payload:%s"),
					*Config.APIKeyET,
					*SessionID,
					*Config.APIKeyET,
					*Config.AppVersionET,
					*UserID,
					*Config.AppEnvironment,
					*Config.UploadType,
					*Payload);
				UE_LOG(LogAnalytics, VeryVerbose, TEXT("%s"), *LogString);
			}

			{
				QUICK_SCOPE_CYCLE_COUNTER(STAT_FlushEventsHttpRequest);
				// Create/send Http request for an event
				TSharedRef<IHttpRequest> HttpRequest = CreateRequest();
				HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
				HttpRequest->SetURL(Config.APIServerET / URLPath);
				HttpRequest->SetVerb(TEXT("POST"));
				HttpRequest->SetContentAsString(Payload);

				// Don't set a response callback if we are in our destructor, as the instance will no longer be there to call.
				if (!bInDestructor)
				{
					HttpRequest->OnProcessRequestComplete().BindSP(this, &FAnalyticsProviderET::EventRequestComplete);
				}
			}
		}
		else
		{
			QUICK_SCOPE_CYCLE_COUNTER(STAT_FlushEventsLegacy);
			// this is a legacy pathway that doesn't accept batch payloads of cached data. We'll just send one request for each event, which will be slow for a large batch of requests at once.
			EventCache.FlushCacheLegacy([this,&EventCount,&PayloadSize](const FString& EventName, const FString& EventParams)
			{
				++EventCount;
				// log out the un-encoded values to make reading the log easier.
				UE_LOG(LogAnalytics, VeryVerbose, TEXT("[%s] AnalyticsET URL:SendEvent.1?SessionID=%s&AppID=%s&AppVersion=%s&UserID=%s&EventName=%s%s"),
					*Config.APIKeyET,
					*SessionID,
					*Config.APIKeyET,
					*Config.AppVersionET,
					*UserID,
					*EventName,
					*EventParams);

				// Create/send Http request for an event
				TSharedRef<IHttpRequest> HttpRequest = CreateRequest();
				HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("text/plain"));

				// Don't need to URL encode the APIServer or the EventParams, which are already encoded, and contain parameter separaters that we DON'T want encoded.
				FString URLPath = Config.APIServerET;
				URLPath += TEXT("SendEvent.1?SessionID=") + FPlatformHttp::UrlEncode(SessionID);
				URLPath += TEXT("&AppID=") + FPlatformHttp::UrlEncode(Config.APIKeyET);
				URLPath += TEXT("&AppVersion=") + FPlatformHttp::UrlEncode(Config.AppVersionET);
				URLPath += TEXT("&UserID=") + FPlatformHttp::UrlEncode(UserID);
				URLPath += TEXT("&EventName=") + FPlatformHttp::UrlEncode(EventName);
				URLPath += EventParams;
				HttpRequest->SetURL(URLPath);
				PayloadSize = HttpRequest->GetURL().Len();
				HttpRequest->SetVerb(TEXT("GET"));
				if (!bInDestructor)
				{
					HttpRequest->OnProcessRequestComplete().BindSP(this, &FAnalyticsProviderET::EventRequestComplete);
				}
				HttpRequest->ProcessRequest();
			});
		}

		FlushEventsCountdown = MaxCachedElapsedTime;
	}
	ANALYTICS_FLUSH_TRACKING_END(PayloadSize, EventCount);
}

void FAnalyticsProviderET::SetAppID(FString&& InAppID)
{
	if (Config.APIKeyET != InAppID)
	{
		// Flush any cached events that would be using the old AppID.
		FlushEvents();
		Config.APIKeyET = MoveTemp(InAppID);
	}
}

void FAnalyticsProviderET::SetAppVersion(FString&& InAppVersion)
{
	// make sure to do the version replacement if the given string is parameterized.
	InAppVersion = InAppVersion.IsEmpty()
		? FString(FApp::GetBuildVersion())
		: InAppVersion.Replace(TEXT("%VERSION%"), FApp::GetBuildVersion(), ESearchCase::CaseSensitive);

	if (Config.AppVersionET != InAppVersion)
	{
		UE_LOG(LogAnalytics, Log, TEXT("[%s] Updating AppVersion to %s from old value of %s"), *Config.APIKeyET, *InAppVersion, *Config.AppVersionET);
		// Flush any cached events that would be using the old AppVersion.
		FlushEvents();
		Config.AppVersionET = MoveTemp(InAppVersion);
	}
}

const FString& FAnalyticsProviderET::GetAppID() const
{
	return Config.APIKeyET;
}

const FString& FAnalyticsProviderET::GetAppVersion() const
{
	return Config.AppVersionET;
}

void FAnalyticsProviderET::SetUserID(const FString& InUserID)
{
	// command-line specified user ID overrides all attempts to reset it.
	if (!FParse::Value(FCommandLine::Get(), TEXT("ANALYTICSUSERID="), UserID, false))
	{
		UE_LOG(LogAnalytics, Log, TEXT("[%s] SetUserId %s"), *Config.APIKeyET, *InUserID);
		// Flush any cached events that would be using the old UserID.
		FlushEvents();
		UserID = InUserID;
	}
	else if (UserID != InUserID)
	{
		UE_LOG(LogAnalytics, Log, TEXT("[%s] Overriding SetUserId %s with cmdline UserId of %s."), *Config.APIKeyET, *InUserID, *UserID);
	}
}

FString FAnalyticsProviderET::GetUserID() const
{
	return UserID;
}

FString FAnalyticsProviderET::GetSessionID() const
{
	return SessionID;
}

bool FAnalyticsProviderET::SetSessionID(const FString& InSessionID)
{
	if (SessionID != InSessionID)
	{
		// Flush any cached events that would be using the old SessionID.
		FlushEvents();
		SessionID = InSessionID;
		UE_LOG(LogAnalytics, Log, TEXT("[%s] Forcing SessionID to %s."), *Config.APIKeyET, *SessionID);
	}
	return true;
}

bool FAnalyticsProviderET::ShouldRecordEvent(const FString& EventName) const
{
	return !ShouldRecordEventFunc || ShouldRecordEventFunc(*this, EventName);
}

void FAnalyticsProviderET::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes)
{
	// Have to copy Attributes array because this doesn't come in as an rvalue ref.
	RecordEvent(EventName, TArray<FAnalyticsEventAttribute>(Attributes));
}

void FAnalyticsProviderET::RecordEvent(FString EventName, TArray<FAnalyticsEventAttribute>&& Attributes)
{
	// let higher level code filter the decision of whether to send the event
	if (ShouldRecordEvent(EventName))
	{
		// fire any callbacks
		for (const auto& Cb : EventRecordedCallbacks)
		{
			Cb(EventName, Attributes, false);
		}

		EventCache.AddToCache(MoveTemp(EventName), MoveTemp(Attributes), false);
		// if we aren't caching events, flush immediately. This is really only for debugging as it will significantly affect bandwidth.
		if (!bShouldCacheEvents)
		{
			FlushEvents();
		}
	}
}

void FAnalyticsProviderET::RecordEventJson(FString EventName, TArray<FAnalyticsEventAttribute>&& AttributesJson)
{
	checkf(!Config.UseLegacyProtocol, TEXT("Cannot use Json events with legacy protocol"));

	// let higher level code filter the decision of whether to send the event
	if (ShouldRecordEvent(EventName))
	{
		// fire any callbacks
		for (const auto& Cb : EventRecordedCallbacks)
		{
			Cb(EventName, AttributesJson, true);
		}

		EventCache.AddToCache(MoveTemp(EventName), MoveTemp(AttributesJson), true);
		// if we aren't caching events, flush immediately. This is really only for debugging as it will significantly affect bandwidth.
		if (!bShouldCacheEvents)
		{
			FlushEvents();
		}
	}
}

void FAnalyticsProviderET::SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes)
{
	EventCache.SetDefaultAttributes(MoveTemp(Attributes));

}

const TArray<FAnalyticsEventAttribute>& FAnalyticsProviderET::GetDefaultEventAttributes() const
{
	UnsafeDefaultAttributes	= EventCache.GetDefaultAttributes();
	return UnsafeDefaultAttributes;
}

void FAnalyticsProviderET::SetEventCallback(const OnEventRecorded& Callback)
{
	EventRecordedCallbacks.Add(Callback);
}

void FAnalyticsProviderET::EventRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool)
{
	// process responses
	bool bEventsDelivered = false;
	if (HttpResponse.IsValid())
	{
		UE_LOG(LogAnalytics, VeryVerbose, TEXT("[%s] ET response for [%s]. Code: %d. Payload: %s"), *Config.APIKeyET, *HttpRequest->GetURL(), HttpResponse->GetResponseCode(), *HttpResponse->GetContentAsString());
		if (EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
		{
			bEventsDelivered = true;
		}
	}
	else
	{
		UE_LOG(LogAnalytics, VeryVerbose, TEXT("[%s] ET response for [%s]. No response"), *Config.APIKeyET, *HttpRequest->GetURL());
	}
}

void FAnalyticsProviderET::SetURLEndpoint(const FString& UrlEndpoint, const TArray<FString>& AltDomains)
{
	FlushEvents();
	Config.APIServerET = UrlEndpoint;

	// Set the number of retries to the number of retry URLs that have been passed in.
	uint32 RetryLimitCount = AltDomains.Num();

	HttpRetryManager->SetDefaultRetryLimit(RetryLimitCount);

	TArray<FString> TmpAltAPIServers = AltDomains;

	// If we have retry domains defined, insert the default domain into the list
	if (RetryLimitCount > 0)
	{
		FString DefaultUrlDomain = FPlatformHttp::GetUrlDomain(Config.APIServerET);
		if (!TmpAltAPIServers.Contains(DefaultUrlDomain))
		{
			TmpAltAPIServers.Insert(DefaultUrlDomain, 0);
		}

		RetryServers = MakeShared<FHttpRetrySystem::FRetryDomains, ESPMode::ThreadSafe>(MoveTemp(TmpAltAPIServers));
	}
	else
	{
		RetryServers.Reset();
	}
}

void FAnalyticsProviderET::BlockUntilFlushed(float InTimeoutSec)
{
	FlushEvents();
	HttpRetryManager->BlockUntilFlushed(InTimeoutSec);
}

void FAnalyticsProviderET::SetShouldRecordEventFunc(const ShouldRecordEventFunction& InShouldRecordEventFunc)
{
	ShouldRecordEventFunc = InShouldRecordEventFunc;
}
