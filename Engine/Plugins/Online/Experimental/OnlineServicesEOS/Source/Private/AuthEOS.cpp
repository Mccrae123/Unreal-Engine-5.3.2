// Copyright Epic Games, Inc. All Rights Reserved.

#include "AuthEOS.h"

#include "OnlineServicesEOS.h"
#include "OnlineServicesEOSTypes.h"
#include "Online/AuthErrors.h"
#include "Online/OnlineErrorDefinitions.h"

#include "eos_auth.h"
#include "eos_types.h"
#include "eos_init.h"
#include "eos_sdk.h"
#include "eos_logging.h"

namespace UE::Online {

// TEMP until Net Id Registry is done
TMap<EOS_EpicAccountId, int32> EOSAccountIdMap;

static inline ELoginStatus ToELoginStatus(EOS_ELoginStatus InStatus)
{
	switch (InStatus)
	{
	case EOS_ELoginStatus::EOS_LS_NotLoggedIn:
	{
		return ELoginStatus::NotLoggedIn;
	}
	case EOS_ELoginStatus::EOS_LS_UsingLocalProfile:
	{
		return ELoginStatus::UsingLocalProfile;
	}
	case EOS_ELoginStatus::EOS_LS_LoggedIn:
	{
		return ELoginStatus::LoggedIn;
	}
	}
	return ELoginStatus::NotLoggedIn;
}

// Copied from OSS EOS

#define EOS_OSS_STRING_BUFFER_LENGTH 256
// Chose arbitrarily since the SDK doesn't define it
#define EOS_MAX_TOKEN_SIZE 4096

struct FEOSAuthCredentials :
	public EOS_Auth_Credentials
{
	FEOSAuthCredentials() :
		EOS_Auth_Credentials()
	{
		ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Id = IdAnsi;
		Token = TokenAnsi;
	}

	FEOSAuthCredentials(EOS_EExternalCredentialType InExternalType, const TArray<uint8>& InToken) :
		EOS_Auth_Credentials()
	{
		ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
		Type = EOS_ELoginCredentialType::EOS_LCT_ExternalAuth;
		ExternalType = InExternalType;
		Id = IdAnsi;
		Token = TokenAnsi;

		uint32_t InOutBufferLength = EOS_OSS_STRING_BUFFER_LENGTH;
		EOS_ByteArray_ToString(InToken.GetData(), InToken.Num(), TokenAnsi, &InOutBufferLength);
	}

	char IdAnsi[EOS_OSS_STRING_BUFFER_LENGTH];
	char TokenAnsi[EOS_MAX_TOKEN_SIZE];
};

FAuthEOS::FAuthEOS(FOnlineServicesEOS& InServices)
	: FAuthCommon(InServices)
{
	AuthHandle = EOS_Platform_GetAuthInterface(InServices.GetEOSPlatformHandle());
	check(AuthHandle != nullptr);

	// Register for login status changes
	EOS_Auth_AddNotifyLoginStatusChangedOptions Options = { };
	Options.ApiVersion = EOS_AUTH_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;
	NotifyLoginStatusChangedNotificationId = EOS_Auth_AddNotifyLoginStatusChanged(AuthHandle, &Options, this, [](const EOS_Auth_LoginStatusChangedCallbackInfo* Data)
	{
		FAuthEOS* This = reinterpret_cast<FAuthEOS*>(Data->ClientData);
		FAccountId LocalUserId = MakeEOSAccountId(Data->LocalUserId);
		ELoginStatus PreviousStatus = ToELoginStatus(Data->PrevStatus);
		ELoginStatus CurrentStatus = ToELoginStatus(Data->CurrentStatus);
		This->OnEOSLoginStatusChanged(LocalUserId, PreviousStatus, CurrentStatus);
	});
}

bool LexFromString(EOS_EAuthScopeFlags& OutEnum, const TCHAR* InString)
{
	if (FCString::Stricmp(InString, TEXT("BasicProfile")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;
	}
	else if (FCString::Stricmp(InString, TEXT("FriendsList")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_FriendsList;
	}
	else if (FCString::Stricmp(InString, TEXT("Presence")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_Presence;
	}
	else if (FCString::Stricmp(InString, TEXT("FriendsManagement")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_FriendsManagement;
	}
	else if (FCString::Stricmp(InString, TEXT("Email")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_Email;
	}
	else if (FCString::Stricmp(InString, TEXT("NoFlags")) == 0 || FCString::Stricmp(InString, TEXT("None")) == 0)
	{
		OutEnum = EOS_EAuthScopeFlags::EOS_AS_NoFlags;
	}
	else
	{
		return false;
	}
	return true;
}

bool LexFromString(EOS_ELoginCredentialType& OutEnum, const TCHAR* const InString)
{
	if (FCString::Stricmp(InString, TEXT("ExchangeCode")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_ExchangeCode;
	}
	else if (FCString::Stricmp(InString, TEXT("PersistentAuth")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
	}
	//else if (FCString::Stricmp(InString, TEXT("DeviceCode")) == 0) // DeviceCode is deprecated
	//{
	//	OutEnum = EOS_ELoginCredentialType::EOS_LCT_DeviceCode;
	//}
	else if (FCString::Stricmp(InString, TEXT("Password")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_Password;
	}
	else if (FCString::Stricmp(InString, TEXT("Developer")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_Developer;
	}
	else if (FCString::Stricmp(InString, TEXT("RefreshToken")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_RefreshToken;
	}
	else if (FCString::Stricmp(InString, TEXT("AccountPortal")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
	}
	else if (FCString::Stricmp(InString, TEXT("ExternalAuth")) == 0)
	{
		OutEnum = EOS_ELoginCredentialType::EOS_LCT_ExternalAuth;
	}
	else
	{
		return false;
	}
	return true;
}

void FAuthEOS::PreShutdown()
{
}

TOnlineAsyncOpHandle<FAuthLogin> FAuthEOS::Login(FAuthLogin::Params&& Params)
{
	TOnlineAsyncOp<FAuthLogin>& Op = GetOp<FAuthLogin>(MoveTemp(Params));

	EOS_Auth_LoginOptions LoginOptions = { };
	LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
	bool bContainsFlagsNone = false;
	for (const FString& Scope : Op.GetParams().Scopes)
	{
		EOS_EAuthScopeFlags ScopeFlag;
		if (LexFromString(ScopeFlag, *Scope))
		{
			if (ScopeFlag == EOS_EAuthScopeFlags::EOS_AS_NoFlags)
			{
				bContainsFlagsNone = true;
			}
			LoginOptions.ScopeFlags |= ScopeFlag;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Invalid ScopeFlag=[%s]"), *Scope);
			Op.SetError(Errors::Unknown());
			return Op.GetHandle();
		}
	}
	// TODO:  Where to put default scopes?
	if (!bContainsFlagsNone && LoginOptions.ScopeFlags == EOS_EAuthScopeFlags::EOS_AS_NoFlags)
	{
		LoginOptions.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile | EOS_EAuthScopeFlags::EOS_AS_FriendsList | EOS_EAuthScopeFlags::EOS_AS_Presence;
	}	

	FEOSAuthCredentials Credentials;
	if (LexFromString(Credentials.Type, *Op.GetParams().CredentialsType))
	{
		switch (Credentials.Type)
		{
		case EOS_ELoginCredentialType::EOS_LCT_ExchangeCode:
			// This is how the Epic launcher will pass credentials to you
			Credentials.IdAnsi[0] = '\0';
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op.GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_Password:
			FCStringAnsi::Strncpy(Credentials.IdAnsi, TCHAR_TO_UTF8(*Op.GetParams().CredentialsId), EOS_OSS_STRING_BUFFER_LENGTH);
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op.GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_Developer:
			// This is auth via the EOS auth tool
			FCStringAnsi::Strncpy(Credentials.IdAnsi, TCHAR_TO_UTF8(*Op.GetParams().CredentialsId), EOS_OSS_STRING_BUFFER_LENGTH);
			FCStringAnsi::Strncpy(Credentials.TokenAnsi, TCHAR_TO_UTF8(*Op.GetParams().CredentialsToken), EOS_MAX_TOKEN_SIZE);
			break;
		case EOS_ELoginCredentialType::EOS_LCT_AccountPortal:
			// This is auth via the EOS Account Portal
			Credentials.IdAnsi[0] = '\0';
			Credentials.TokenAnsi[0] = '\0';
			break;
		case EOS_ELoginCredentialType::EOS_LCT_PersistentAuth:
			// This is auth via stored credentials in EOS
			Credentials.Id = nullptr;
			Credentials.Token = nullptr;
			break;
		default:
			UE_LOG(LogTemp, Warning, TEXT("Unsupported CredentialsType=[%s]"), *Op.GetParams().CredentialsType);
			Op.SetError(Errors::Unknown()); // TODO
			return Op.GetHandle();
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid CredentialsType=[%s]"), *Op.GetParams().CredentialsType);
		Op.SetError(Errors::Unknown()); // TODO
		return Op.GetHandle();
	}

	Op.Then([this, LoginOptions, Credentials](TOnlineAsyncOp<FAuthLogin>& InAsyncOp) mutable
		{
			LoginOptions.Credentials = &Credentials;
			return EOS_Async<EOS_Auth_LoginCallbackInfo>(InAsyncOp, EOS_Auth_Login, AuthHandle, LoginOptions);
		})
		.Then([this](TOnlineAsyncOp<FAuthLogin>& InAsyncOp, const EOS_Auth_LoginCallbackInfo* Data)
		{
			UE_LOG(LogTemp, Warning, TEXT("LoginResult: [%s]"), *LexToString(Data->ResultCode));

			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				// Success
				UE_LOG(LogTemp, Warning, TEXT("Successfully logged in as [%s]"), *LexToString(Data->LocalUserId));
				TSharedRef<FAccountInfoEOS> AccountInfo = MakeShared<FAccountInfoEOS>();
				AccountInfo->LocalUserNum = InAsyncOp.GetParams().LocalUserNum;
				AccountInfo->UserId = MakeEOSAccountId(Data->LocalUserId);

				check(!AccountInfos.Contains(AccountInfo->UserId));
				AccountInfos.Emplace(AccountInfo->UserId, AccountInfo);

				FAuthLogin::Result Result = { AccountInfo };
				InAsyncOp.SetResult(MoveTemp(Result));

				// Trigger event
				FLoginStatusChanged EventParameters;
				EventParameters.LocalUserId = AccountInfo->UserId;
				EventParameters.PreviousStatus = ELoginStatus::NotLoggedIn;
				EventParameters.CurrentStatus = ELoginStatus::LoggedIn;
				OnLoginStatusChangedEvent.Broadcast(EventParameters);
			}
			else if (Data->ResultCode == EOS_EResult::EOS_InvalidUser && Data->ContinuanceToken != nullptr)
			{
				// 
			}
			else
			{
				FOnlineError Error = Errors::Unknown();
				if (Data->ResultCode == EOS_EResult::EOS_InvalidAuth)
				{
					Error = Errors::InvalidCreds();
				}

				InAsyncOp.SetError(MoveTemp(Error));
			}
		})
		.Enqueue();
	return Op.GetHandle();
}

TOnlineAsyncOpHandle<FAuthLogout> FAuthEOS::Logout(FAuthLogout::Params&& Params)
{
	FAccountId ParamLocalUserId = Params.LocalUserId;
	TOptional<EOS_EpicAccountId> AccountId = EOSAccountIdFromOnlineServiceAccountId(ParamLocalUserId);
	TOnlineAsyncOp<FAuthLogout>& Op = GetOp<FAuthLogout>(MoveTemp(Params));
	if (AccountId.IsSet() && AccountInfos.Contains(ParamLocalUserId))
	{
		// Should we destroy persistent auth first?
		TOnlineChainableAsyncOp<FAuthLogout, void> NextOp = Op.Then([](TOnlineAsyncOp<FAuthLogout>& InAsyncOp){ return; });
		if (Params.bDestroyAuth)
		{
			EOS_Auth_DeletePersistentAuthOptions DeletePersistentAuthOptions = {0};
			DeletePersistentAuthOptions.ApiVersion = EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST;
			DeletePersistentAuthOptions.RefreshToken = nullptr; // Is this needed?  Docs say it's needed for consoles
			NextOp = NextOp.Then([this, DeletePersistentAuthOptions](TOnlineAsyncOp<FAuthLogout>& InAsyncOp)
				{
					return EOS_Async<EOS_Auth_DeletePersistentAuthCallbackInfo>(InAsyncOp, EOS_Auth_DeletePersistentAuth, AuthHandle, DeletePersistentAuthOptions);
				})
				.Then([](TOnlineAsyncOp<FAuthLogout>& InAsyncOp, const EOS_Auth_DeletePersistentAuthCallbackInfo* Data)
				{
					UE_LOG(LogTemp, Warning, TEXT("DeletePersistentAuthResult: [%s]"), ANSI_TO_TCHAR(EOS_EResult_ToString(Data->ResultCode)));
					// Regardless of success/failure, continue
				});
		}
		// Logout
		NextOp.Then([this, AccountId](TOnlineAsyncOp<FAuthLogout>& InAsyncOp)
			{
				EOS_Auth_LogoutOptions LogoutOptions = { };
				LogoutOptions.ApiVersion = EOS_AUTH_LOGOUT_API_LATEST;
				LogoutOptions.LocalUserId = AccountId.GetValue();
				return EOS_Async<EOS_Auth_LogoutCallbackInfo>(InAsyncOp, EOS_Auth_Logout, AuthHandle, LogoutOptions);
			})
			.Then([](TOnlineAsyncOp<FAuthLogout>& InAsyncOp, const EOS_Auth_LogoutCallbackInfo* Data)
			{
				UE_LOG(LogTemp, Warning, TEXT("LogoutResult: [%s]"), *LexToString(Data->ResultCode));

				if (Data->ResultCode == EOS_EResult::EOS_Success)
				{
					// Success
					InAsyncOp.SetResult(FAuthLogout::Result());
				}
				else
				{
					// TODO: Error codes
					FOnlineError Error = Errors::Unknown();
					InAsyncOp.SetError(MoveTemp(Error));
				}
			}).Enqueue();
	}
	else
	{
		// TODO: Error codes
		Op.SetError(Errors::Unknown());
	}
	
	return Op.GetHandle();
}

TOnlineAsyncOpHandle<FAuthGenerateAuth> FAuthEOS::GenerateAuth(FAuthGenerateAuth::Params&& Params)
{
	TSharedRef<TOnlineAsyncOp<FAuthGenerateAuth>> AsyncOperation = MakeShared<TOnlineAsyncOp<FAuthGenerateAuth>>(Services, MoveTemp(Params));
	return AsyncOperation->GetHandle();
}

TOnlineResult<FAuthGetAccountByLocalUserNum::Result> FAuthEOS::GetAccountByLocalUserNum(FAuthGetAccountByLocalUserNum::Params&& Params)
{
	TOnlineResult<FAccountId> LocalUserIdResult = GetAccountIdByLocalUserNum(Params.LocalUserNum);
	if (LocalUserIdResult.IsOk())
	{
		FAuthGetAccountByLocalUserNum::Result Result = { AccountInfos.FindChecked(LocalUserIdResult.GetOkValue()) };
		return TOnlineResult<FAuthGetAccountByLocalUserNum::Result>(Result);
	}
	else
	{
		return TOnlineResult<FAuthGetAccountByLocalUserNum::Result>(LocalUserIdResult.GetErrorValue());
	}
}

TOnlineResult<FAuthGetAccountByAccountId::Result> FAuthEOS::GetAccountByAccountId(FAuthGetAccountByAccountId::Params&& Params)
{
	if (TSharedRef<FAccountInfoEOS>* const FoundAccount = AccountInfos.Find(Params.LocalUserId))
	{
		FAuthGetAccountByAccountId::Result Result;
		Result.AccountInfo = *FoundAccount;
		return TOnlineResult<FAuthGetAccountByAccountId::Result>(Result);
	}
	else
	{
		// TODO: proper error
		return TOnlineResult<FAuthGetAccountByAccountId::Result>(Errors::Unknown());
	}
}

bool FAuthEOS::IsLoggedIn(const FAccountId& AccountId) const
{
	// TODO:  More logic?
	return AccountInfos.Contains(AccountId);
}

TOnlineResult<FAccountId> FAuthEOS::GetAccountIdByLocalUserNum(int32 LocalUserNum) const
{
	for (const TPair<FAccountId, TSharedRef<FAccountInfoEOS>>& AccountPair : AccountInfos)
	{
		if (AccountPair.Value->LocalUserNum == LocalUserNum)
		{
			TOnlineResult<FAccountId> Result(AccountPair.Key);
			return Result;
		}
	}
	TOnlineResult<FAccountId> Result(Errors::Unknown()); // TODO: error code
	return Result;
}

void FAuthEOS::OnEOSLoginStatusChanged(FAccountId LocalUserId, ELoginStatus PreviousStatus, ELoginStatus CurrentStatus)
{
	UE_LOG(LogTemp, Warning, TEXT("OnEOSLoginStatusChanged: [%s] %d %d"),
		*LexToString(EOSAccountIdFromOnlineServiceAccountId(LocalUserId).GetValue()),
		(int)PreviousStatus,
		(int)CurrentStatus);
	if (TSharedRef<FAccountInfoEOS>* AccountInfoPtr = AccountInfos.Find(LocalUserId))
	{
		FAccountInfoEOS& AccountInfo = **AccountInfoPtr;
		if (AccountInfo.LoginStatus != CurrentStatus)
		{
			FLoginStatusChanged EventParameters;
			EventParameters.LocalUserId = LocalUserId;
			EventParameters.PreviousStatus = AccountInfo.LoginStatus;
			EventParameters.CurrentStatus = CurrentStatus;

			AccountInfo.LoginStatus = CurrentStatus;

			if (CurrentStatus == ELoginStatus::NotLoggedIn)
			{
				// Remove user
				AccountInfos.Remove(LocalUserId); // Invalidates AccountInfo
			}

			OnLoginStatusChangedEvent.Broadcast(EventParameters);
		}
	}
}


/* UE::Online */ }
