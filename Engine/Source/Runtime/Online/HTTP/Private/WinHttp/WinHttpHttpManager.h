// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_WINHTTP

#pragma once

#include "CoreMinimal.h"
#include "HttpManager.h"

class FWinHttpSession;

DECLARE_DELEGATE_OneParam(FWinHttpQuerySessionComplete, FWinHttpSession* /*HttpSessionPtr*/);

class IWinHttpConnection;

class FWinHttpHttpManager
	: public FHttpManager
{
public:
	static FWinHttpHttpManager* GetManager();

	FWinHttpHttpManager();
	virtual ~FWinHttpHttpManager();

	/**
	 * Asynchronously finds an existing WinHttp session for the provided URL, or creates a new one for it.
	 *
	 * @param Url The URL to find or create a WinHttp session for
	 * @param Delegate The delegate that is called with the WinHttp session pointer if successful, or null otherwise
	 */
	virtual void QuerySessionForUrl(const FString& Url, FWinHttpQuerySessionComplete&& Delegate);

	/**
	 * Validate the provided connection before we start sending our request.
	 *
	 * NOTE: this is called on multiple threads, and should be written in a way that handles this safely!
	 * 
	 * @param Connection the connection to validate
	 */
	virtual bool ValidateRequestCertificates(IWinHttpConnection& Connection);

	//~ Begin FHttpManager Interface
	virtual void OnBeforeFork() override;
	//~ End FHttpManager Interface

protected:
	FWinHttpSession* FindOrCreateSession(const uint32 SecurityProtocols);

protected:
	bool bPlatformForcesSecureConnections = false;

	/** Map of Security Flags to WinHttp Session objects */
	TMap<uint32, TUniquePtr<FWinHttpSession>> ActiveSessions;
};

#endif // WITH_WINHTTP
