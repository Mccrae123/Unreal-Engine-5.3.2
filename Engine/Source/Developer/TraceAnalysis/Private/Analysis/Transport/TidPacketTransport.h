// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Transport.h"
#include "Analysis/StreamReader.h"

namespace Trace
{

class FStreamReader;

////////////////////////////////////////////////////////////////////////////////
class FTidPacketTransport
	: public FTransport
{
public:
	typedef UPTRINT ThreadIter;

	void					Update();
	ThreadIter				ReadThreads();
	FStreamReader*			GetNextThread(ThreadIter& Iter);

private:
	struct FThreadStream
	{
		FStreamBuffer		Buffer;
		uint32				ThreadId;
	};

	bool					ReadPacket();
	FThreadStream&			FindOrAddThread(uint32 ThreadId);
	static const uint32		MaxPacketSize = 8192;
	TArray<FThreadStream>	Threads;
};

} // namespace Trace
