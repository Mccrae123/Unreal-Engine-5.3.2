// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Trace/Config.h"

#if UE_TRACE_ENABLED

#include "Writer.inl"

namespace Trace {
namespace Private {

struct FWriteBuffer;

////////////////////////////////////////////////////////////////////////////////
class FLogScope
{
public:
							~FLogScope();
	constexpr explicit		operator bool () const { return true; }
	uint8*					GetPointer() const;
	const FLogScope&		operator << (bool) const;
	template <uint32 Flags>
	static FLogScope		Enter(uint32 Uid, uint32 Size);

private:
	template <class T> void	EnterPrelude(uint32 Size, bool bMaybeHasAux);
	void					Enter(uint32 Uid, uint32 Size, bool bMaybeHasAux);
	void					EnterNoSync(uint32 Uid, uint32 Size, bool bMaybeHasAux);
	struct
	{
		uint8*				Ptr;
		FWriteBuffer*		Buffer;
	}						Instance;
};

////////////////////////////////////////////////////////////////////////////////
class FImportantLogScope
	: public FLogScope
{
};



////////////////////////////////////////////////////////////////////////////////
template <bool>	struct TLogScopeSelector;
template <>		struct TLogScopeSelector<false>	{ typedef FLogScope Type; };
template <>		struct TLogScopeSelector<true>	{ typedef FImportantLogScope Type; };

////////////////////////////////////////////////////////////////////////////////
template <class T>
struct TLogScope
{
	static auto Enter(uint32 Uid, uint32 Size);
	static auto Enter(uint32 Uid, uint32 Size, uint32 ExtraBytes);
};

} // namespace Private
} // namespace Trace

#endif // UE_TRACE_ENABLED
