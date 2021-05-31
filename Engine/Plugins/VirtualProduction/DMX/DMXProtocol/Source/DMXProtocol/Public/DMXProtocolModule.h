// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "DMXProtocolCommon.h"

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"


class IDMXProtocolFactory;

/** Implements the Protocol Module, that enables specific Protocol implementations */
class DMXPROTOCOL_API FDMXProtocolModule 
	: public IModuleInterface
{
public:
	void RegisterProtocol(const FName& ProtocolName, IDMXProtocolFactory* Factory);

	void UnregisterProtocol(const FName& ProtocolName);

	/** Delegate called when a protocols was registered */
	FSimpleMulticastDelegate OnProtocolRegistered;

public:
	/** Get the instance of this module. */
	static FDMXProtocolModule& Get();

	/**
	 * If protocol exists return the pointer otherwise it create a new protocol first and then return the pointer.
	 * @param  InProtocolName Name of the requested protocol
	 * @return Return the pointer to protocol.
	 */
	virtual IDMXProtocolPtr GetProtocol(const FName InProtocolName = NAME_None);
	
	/**  Get the reference to all protocol factories map */
	const TMap<FName, IDMXProtocolFactory*>& GetProtocolFactories() const;

	/**  Get the reference to all protocols map */
	const TMap<FName, IDMXProtocolPtr>& GetProtocols() const;

public:
	//~ Begin IModuleInterface implementation
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface implementation

private:
	void ShutdownDMXProtocol(const FName& ProtocolName);
	void ShutdownAllDMXProtocols();

private:
	TMap<FName, IDMXProtocolFactory*> DMXProtocolFactories;
	TMap<FName, IDMXProtocolPtr> DMXProtocols;
};
