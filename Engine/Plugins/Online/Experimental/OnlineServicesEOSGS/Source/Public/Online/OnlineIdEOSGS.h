// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/CoreOnline.h"
#include "Online/OnlineIdCommon.h"

#if defined(EOS_PLATFORM_BASE_FILE_NAME)
#include EOS_PLATFORM_BASE_FILE_NAME
#endif
#include "eos_common.h"

namespace UE::Online {

class IOnlineAccountIdRegistryEOSGS : public IOnlineAccountIdRegistry
{
public:
	virtual FOnlineAccountIdHandle FindAccountId(EOS_ProductUserId ProductUserId) const = 0;
	virtual EOS_ProductUserId GetProductUserId(const FOnlineAccountIdHandle& Handle) const = 0;
};

/**
 * Account id registry specifically for EOS id's which are segmented.
 */
class FOnlineAccountIdRegistryEOSGS
	: public IOnlineAccountIdRegistryEOSGS
{
public:
	virtual ~FOnlineAccountIdRegistryEOSGS() = default;

	// Begin IOnlineAccountIdRegistryEOSGS
	virtual FOnlineAccountIdHandle FindAccountId(const EOS_ProductUserId ProductUserId) const override;
	virtual EOS_ProductUserId GetProductUserId(const FOnlineAccountIdHandle& Handle) const override;
	// End IOnlineAccountIdRegistryEOSGS

	// Begin IOnlineAccountIdRegistry
	virtual FString ToLogString(const FOnlineAccountIdHandle& Handle) const override;
	virtual TArray<uint8> ToReplicationData(const FOnlineAccountIdHandle& Handle) const override;
	virtual FOnlineAccountIdHandle FromReplicationData(const TArray<uint8>& ReplicationData) override;
	// End IOnlineAccountIdRegistry

	static IOnlineAccountIdRegistryEOSGS& GetRegistered();

private:
	// FAuthEOSGS is the only thing that should be able to create PUID-only net ids in this registry, in its resolve methods.
	friend class FAuthEOSGS;
	friend class FOnlineServicesEOSGSModule;
	static FOnlineAccountIdRegistryEOSGS& Get();
	FOnlineAccountIdHandle FindOrAddAccountId(const EOS_ProductUserId ProductUserId);

	TOnlineBasicAccountIdRegistry<EOS_ProductUserId, EOnlineServices::Epic> Registry;
};

EOS_ProductUserId ONLINESERVICESEOSGS_API GetProductUserId(const FOnlineAccountIdHandle& Handle);
EOS_ProductUserId ONLINESERVICESEOSGS_API GetProductUserIdChecked(const FOnlineAccountIdHandle& Handle);
FOnlineAccountIdHandle ONLINESERVICESEOSGS_API FindAccountId(const EOS_ProductUserId EpicAccountId);
FOnlineAccountIdHandle ONLINESERVICESEOSGS_API FindAccountIdChecked(const EOS_ProductUserId EpicAccountId);

template<typename IdType>
inline bool ValidateOnlineId(const TOnlineIdHandle<IdType> Handle)
{
	return Handle.GetOnlineServicesType() == EOnlineServices::Epic && Handle.IsValid();
}

} /* namespace UE::Online */
