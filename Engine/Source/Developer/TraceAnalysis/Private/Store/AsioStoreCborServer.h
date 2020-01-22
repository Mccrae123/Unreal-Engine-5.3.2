// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Asio/Asio.h"

#if TRACE_WITH_ASIO

#include "AsioTcpServer.h"
#include "AsioTickable.h"
#include "Containers/Array.h"

namespace Trace
{

class FAsioRecorder;
class FAsioStore;
class FAsioStoreCborPeer;

////////////////////////////////////////////////////////////////////////////////
class FAsioStoreCborServer
	: public FAsioTcpServer
	, public FAsioTickable
{
public:
								FAsioStoreCborServer(asio::io_context& IoContext, FAsioStore& InStore, FAsioRecorder& InRecorder);
								~FAsioStoreCborServer();
	FAsioStore&					GetStore() const;
	FAsioRecorder&				GetRecorder() const;

private:
	virtual bool				OnAccept(asio::ip::tcp::socket& Socket) override;
	virtual void				OnTick() override;
	TArray<FAsioStoreCborPeer*>	Peers;
	FAsioStore&					Store;
	FAsioRecorder&				Recorder;
};

} // namespace Trace

#endif // TRACE_WITH_ASIO
