// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CompactBinaryTCP.h"
#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Set.h"
#include "CookOnTheSide/CookOnTheFlyServer.h"
#include "CookPackageData.h"
#include "CookSockets.h"
#include "CookTypes.h"
#include "Misc/Guid.h"

class FSocket;
class ITargetPlatform;
struct FProcHandle;
namespace UE::CompactBinaryTCP { struct FMarshalledMessage; }
namespace UE::Cook { class FCookDirector; }
namespace UE::Cook { struct FDiscoveredPackage; }
namespace UE::Cook { struct FPackageData; }
namespace UE::Cook { struct FPackageResultsMessage; }
namespace UE::Cook { struct FWorkerConnectMessage; }

namespace UE::Cook
{

/** Class in a Director process that communicates over a Socket with FCookWorkerClient in a CookWorker process. */
class FCookWorkerServer
{
public:
	FCookWorkerServer(FCookDirector& InDirector, FWorkerId InWorkerId);
	~FCookWorkerServer();

	FWorkerId GetWorkerId() const { return WorkerId; }

	/** Add the given assignments for the CookWorker. They will be sent during Tick */
	void AppendAssignments(TArrayView<FPackageData*> Assignments);
	/** Remove assignment of the package from local state and from the connected Client. */
	void AbortAssignment(FPackageData& PackageData);
	/**
	 * Remove assignment of the all assigned packages from local state and from the connected Client.
	 * Report all packages that were unassigned.
	 */
	void AbortAssignments(TSet<FPackageData*>& OutPendingPackages);
	/** AbortAssignments and tell the connected Client to gracefully terminate. Report all packages that were unassigned. */
	void AbortWorker(TSet<FPackageData*>& OutPendingPackages);
	/** Take over the Socket for a CookWorker that has just connected. */
	bool TryHandleConnectMessage(FWorkerConnectMessage& Message, FSocket* InSocket, TArray<UE::CompactBinaryTCP::FMarshalledMessage>&& OtherPacketMessages);
	/** Periodic Tick function to send and receive messages to the Client. */
	void TickFromSchedulerThread();
	/** Called when the COTFS Server has detected all packages are complete. Tell the CookWorker to flush messages and exit. */
	void SignalCookComplete();

	/** Is this either shutting down or completed shutdown of its remote Client? */
	bool IsShuttingDown() const;
	/** Is this executing the portion of graceful shutdown where it waits for the CookWorker to transfer remaining messages? */
	bool IsFlushingBeforeShutdown() const;
	/** Is this not yet or no longer connected to a remote Client? */
	bool IsShutdownComplete() const;

private:
	enum class EConnectStatus
	{
		Uninitialized,
		WaitForConnect,
		Connected,
		PumpingCookComplete,
		WaitForDisconnect,
		LostConnection,
	};

private:
	/** Helper for PumpConnect, launch the remote Client process. */
	void LaunchProcess();
	/** Helper for PumpConnect, wait for connect message from Client, set state to LostConnection if we timeout. */
	void TickWaitForConnect();
	/** Helper for PumpConnect, wait for disconnect message from Client, set state to LostConnection if we timeout. */
	void TickWaitForDisconnect();
	/** Helper for Tick, pump send messages to a connected Client. */
	void PumpSendMessages();
	/** Helper for PumpSendMessages; send a message for any PackagesToAssign we have. */
	void SendPendingPackages();
	/** Helper for Tick, pump receive messages from a connected Client. */
	void PumpReceiveMessages();
	/** Send the message immediately to the Socket. If cannot complete immediately, it will be finished during Tick. */
	void SendMessage(const UE::CompactBinaryTCP::IMessage& Message);
	/** Send this into the given state. Update any state-dependent variables. */
	void SendToState(EConnectStatus TargetStatus);
	/** Close the connection and connection resources to the remote process. Does not kill the process. */
	void DetachFromRemoteProcess();
	/** Kill the Client process (non-graceful termination), and close the connection resources. */
	void ShutdownRemoteProcess();
	/** Helper for PumpReceiveMessages: dispatch the messages received from the socket. */
	void HandleReceiveMessages(TArray<UE::CompactBinaryTCP::FMarshalledMessage>&& Messages);
	void HandleReceivedPackagePlatformMessages(FPackageData& PackageData, const ITargetPlatform* TargetPlatform, TArray<UE::CompactBinaryTCP::FMarshalledMessage>&& Messages);
	/** Add results from the client to the local CookOnTheFlyServer. */
	void RecordResults(FPackageResultsMessage& Message);
	void LogInvalidMessage(const TCHAR* MessageTypeName);
	void AddDiscoveredPackage(FDiscoveredPackage&& DiscoveredPackage);

	TArray<FPackageData*> PackagesToAssign;
	TSet<FPackageData*> PendingPackages;
	TArray<ITargetPlatform*> OrderedSessionPlatforms;
	UE::CompactBinaryTCP::FSendBuffer SendBuffer;
	UE::CompactBinaryTCP::FReceiveBuffer ReceiveBuffer;
	FCookDirector& Director;
	UCookOnTheFlyServer& COTFS;
	FSocket* Socket = nullptr;
	FProcHandle CookWorkerHandle;
	uint32 CookWorkerProcessId = 0;
	double ConnectStartTimeSeconds = 0.;
	double ConnectTestStartTimeSeconds = 0.;
	FWorkerId WorkerId = FWorkerId::Invalid();
	EConnectStatus ConnectStatus = EConnectStatus::Uninitialized;
	bool bTerminateImmediately = false;
};

/** Information about a PackageData the director sends to cookworkers. */
struct FAssignPackageData
{
	FConstructPackageData ConstructData;
	FInstigator Instigator;
};
FCbWriter& operator<<(FCbWriter& Writer, const FAssignPackageData& AssignData);
bool LoadFromCompactBinary(FCbFieldView Field, FAssignPackageData& AssignData);
FCbWriter& operator<<(FCbWriter& Writer, const FInstigator& Instigator);
bool LoadFromCompactBinary(FCbFieldView Field, FInstigator& Instigator);

/** Message from Server to Client to cook the given packages. */
struct FAssignPackagesMessage : public UE::CompactBinaryTCP::IMessage
{
public:
	FAssignPackagesMessage() = default;
	FAssignPackagesMessage(TArray<FAssignPackageData>&& InPackageDatas);

	virtual void Write(FCbWriter& Writer) const override;
	virtual bool TryRead(FCbObject&& Object) override;
	virtual FGuid GetMessageType() const override { return MessageType; }

public:
	TArray<FAssignPackageData> PackageDatas;
	static FGuid MessageType;
};

/** Message from Server to Client to cancel the cook of the given packages. */
struct FAbortPackagesMessage : public UE::CompactBinaryTCP::IMessage
{
public:
	FAbortPackagesMessage() = default;
	FAbortPackagesMessage(TArray<FName>&& InPackageNames);

	virtual void Write(FCbWriter& Writer) const override;
	virtual bool TryRead(FCbObject&& Object) override;
	virtual FGuid GetMessageType() const override { return MessageType; }

public:
	TArray<FName> PackageNames;
	static FGuid MessageType;
};

/**
 * Message from either Server to Client.
 * If from Server, request that Client shutdown.
 * If from Client, notify Server it is shutting down.
 */
struct FAbortWorkerMessage : public UE::CompactBinaryTCP::IMessage
{
public:
	enum EType
	{
		CookComplete,
		Abort,
		AbortAcknowledge,
	};
	FAbortWorkerMessage(EType InType = EType::Abort);
	virtual void Write(FCbWriter& Writer) const override;
	virtual bool TryRead(FCbObject&& Object) override;
	virtual FGuid GetMessageType() const override { return MessageType; }

public:
	EType Type;
	static FGuid MessageType;
};

/** Message From Server to Client giving all of the COTFS settings the client needs. */
struct FInitialConfigMessage : public UE::CompactBinaryTCP::IMessage
{
public:
	virtual void Write(FCbWriter& Writer) const override;
	virtual bool TryRead(FCbObject&& Object) override;
	virtual FGuid GetMessageType() const override { return MessageType; }

	void ReadFromLocal(const UCookOnTheFlyServer& COTFS, const TArray<ITargetPlatform*>& InOrderedSessionPlatforms,
		const FCookByTheBookOptions& InCookByTheBookOptions, const FCookOnTheFlyOptions& InCookOnTheFlyOptions,
		const FBeginCookContextForWorker& InBeginContext);

	ECookMode::Type GetDirectorCookMode() const { return DirectorCookMode; }
	ECookInitializationFlags GetCookInitializationFlags() const { return CookInitializationFlags; }
	FInitializeConfigSettings&& ConsumeInitializeConfigSettings() { return MoveTemp(InitialSettings); }
	FBeginCookConfigSettings&& ConsumeBeginCookConfigSettings() { return MoveTemp(BeginCookSettings); }
	FCookByTheBookOptions&& ConsumeCookByTheBookOptions() { return MoveTemp(CookByTheBookOptions); }
	FCookOnTheFlyOptions&& ConsumeCookOnTheFlyOptions() { return MoveTemp(CookOnTheFlyOptions); }
	const FBeginCookContextForWorker& GetBeginCookContext() { return BeginCookContext; }
	const TArray<ITargetPlatform*>& GetOrderedSessionPlatforms() { return OrderedSessionPlatforms; }
	bool IsZenStore() const { return bZenStore; }

public:
	static FGuid MessageType;
private:
	FInitializeConfigSettings InitialSettings;
	FBeginCookConfigSettings BeginCookSettings;
	FBeginCookContextForWorker BeginCookContext;
	FCookByTheBookOptions CookByTheBookOptions;
	FCookOnTheFlyOptions CookOnTheFlyOptions;
	TArray<ITargetPlatform*> OrderedSessionPlatforms;
	ECookMode::Type DirectorCookMode = ECookMode::CookByTheBook;
	ECookInitializationFlags CookInitializationFlags = ECookInitializationFlags::None;
	bool bZenStore = false;
};

/** Information about a discovered package sent from a CookWorker to the Director. */
struct FDiscoveredPackage
{
	FName PackageName;
	FName NormalizedFileName;
	FInstigator Instigator;
};
FCbWriter& operator<<(FCbWriter& Writer, const FDiscoveredPackage& Package);
bool LoadFromCompactBinary(FCbFieldView Field, FDiscoveredPackage& OutPackage);

/**
 * Message from CookWorker to Director that reports dependency packages discovered during load/save of
 * a package that were not found in the earlier traversal of the packages dependencies.
 */
struct FDiscoveredPackagesMessage : public UE::CompactBinaryTCP::IMessage
{
public:
	virtual void Write(FCbWriter& Writer) const override;
	virtual bool TryRead(FCbObject&& Object) override;
	virtual FGuid GetMessageType() const override { return MessageType; }

public:
	TArray<FDiscoveredPackage> Packages;
	static FGuid MessageType;
};

}