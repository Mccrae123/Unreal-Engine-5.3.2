// Copyright Epic Games, Inc. All Rights Reserved.

// Module includes
#include "OnlineIdentityFacebook.h"
#include "OnlineSubsystemFacebookPrivate.h"
#include "Interfaces/OnlineSharingInterface.h"

FOnlineIdentityFacebook::FOnlineIdentityFacebook(FOnlineSubsystemFacebook* InSubsystem)
	: FOnlineIdentityFacebookCommon(InSubsystem)
{
}

bool FOnlineIdentityFacebook::Login(int32 LocalUserNum, const FOnlineAccountCredentials& AccountCredentials)
{
	// Not implemented
	FString ErrorStr = TEXT("NotImplemented");
	FacebookSubsystem->ExecuteNextTick([this, LocalUserNum, ErrorStr]()
	{
		TriggerOnLoginCompleteDelegates(LocalUserNum, false, *FUniqueNetIdFacebook::EmptyId(), ErrorStr);
	});
	return false;
}

bool FOnlineIdentityFacebook::Logout(int32 LocalUserNum)
{
	// Not implemented
	FacebookSubsystem->ExecuteNextTick([this, LocalUserNum]()
	{
		TriggerOnLogoutCompleteDelegates(LocalUserNum, false);
	});
	return false;
}


