// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Trace/Config.h"

#if UE_TRACE_ENABLED

#include "Writer.inl"

namespace Trace
{

struct FFieldDesc;
struct FLiteralName;

////////////////////////////////////////////////////////////////////////////////
class FEventDef
{
public:
	enum
	{
		Flag_Always		= 1 << 0,
		Flag_Important	= 1 << 1,
	};

	class FLogScope
	{
	public:
								FLogScope(uint16 EventUid, uint16 Size);
								FLogScope(uint16 EventUid, uint16 Size, uint16 ExtraBytes);
								~FLogScope();
		FLogInstance			Instance;
	};

	void*						Handle;
	uint32						LoggerHash;
	uint32						Hash;
	uint16						Uid;
	union
	{
		struct
		{
			bool				bOptedIn;
			uint8				Internal;
		};
		uint16					Test;
	}							Enabled;
	bool						bInitialized;
	TRACELOG_API static void	Create(FEventDef* Target, const FLiteralName& LoggerName, const FLiteralName& EventName, const FFieldDesc* FieldDescs, uint32 FieldCount, uint32 Flags=0);
};

} // namespace Trace

#endif // UE_TRACE_ENABLED
