// Copyright Epic Games, Inc. All Rights Reserved.

#include "AsioRecorder.h"

#if TRACE_WITH_ASIO

#include "AsioIoable.h"
#include "AsioSocket.h"
#include "AsioStore.h"

namespace Trace
{

////////////////////////////////////////////////////////////////////////////////
class FAsioRecorderRelay
	: public FAsioIoSink
{
public:
						FAsioRecorderRelay(asio::ip::tcp::socket& Socket, FAsioWriteable* InOutput);
	virtual				~FAsioRecorderRelay();
	void				Close();

private:
	virtual void		OnIoComplete(uint32 Id, int32 Size) override;
	static const uint32	BufferSize = 64 * 1024;
	enum				{ OpStart, OpSocketRead, OpFileWrite };
	FAsioSocket			Input;
	FAsioWriteable*		Output;
	uint8				Buffer[BufferSize];
};

////////////////////////////////////////////////////////////////////////////////
FAsioRecorderRelay::FAsioRecorderRelay(asio::ip::tcp::socket& Socket, FAsioWriteable* InOutput)
: Input(Socket)
, Output(InOutput)
{
	OnIoComplete(OpStart, 0);
}

////////////////////////////////////////////////////////////////////////////////
FAsioRecorderRelay::~FAsioRecorderRelay()
{
	delete Output;
}

////////////////////////////////////////////////////////////////////////////////
void FAsioRecorderRelay::Close()
{
	Input.Close();
	Output->Close();
}

////////////////////////////////////////////////////////////////////////////////
void FAsioRecorderRelay::OnIoComplete(uint32 Id, int32 Size)
{
	if (Size < 0)
	{
		Close();
		return;
	}

	switch (Id)
	{
	case OpSocketRead:
		Output->Write(Buffer, Size, this, OpFileWrite);
		break;

	case OpStart:
	case OpFileWrite:
		Input.ReadSome(Buffer, BufferSize, this, OpSocketRead);
		break;
	}
}



////////////////////////////////////////////////////////////////////////////////
FAsioRecorder::FAsioRecorder(asio::io_context& IoContext, FAsioStore& InStore)
: FAsioTcpServer(IoContext)
, FAsioTickable(IoContext)
, Store(InStore)
{
	StartTick(500);
}

////////////////////////////////////////////////////////////////////////////////
FAsioRecorder::~FAsioRecorder()
{
	for (FAsioRecorderRelay* Relay : Relays)
	{
		Relay->Close();
		delete Relay;
	}
}

////////////////////////////////////////////////////////////////////////////////
bool FAsioRecorder::OnAccept(asio::ip::tcp::socket& Socket)
{
	FAsioStore::FNewTrace Trace = Store.CreateTrace();
	if (Trace.Writeable == nullptr)
	{
		return true;
	}

	FAsioRecorderRelay* Relay = new FAsioRecorderRelay(Socket, Trace.Writeable);
	Relays.Add(Relay);
	return true;
}

////////////////////////////////////////////////////////////////////////////////
void FAsioRecorder::OnTick()
{
	/* cleanup dead clients */
}

} // namespace Trace

#endif // TRACE_WITH_ASIO
