// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CoreTypes.h"

THIRD_PARTY_INCLUDES_START
#include "ThirdParty/LZ4/lz4.c.h"
THIRD_PARTY_INCLUDES_END

namespace Trace {
namespace Private {

////////////////////////////////////////////////////////////////////////////////
int32 Encode(const void* Src, int32 SrcSize, void* Dest, int32 DestSize)
{
	return LZ4_compress_fast(
		(const char*)Src,
		(char*)Dest,
		SrcSize,
		DestSize,
		1 // increase by 1 for small speed increase
	);
}

////////////////////////////////////////////////////////////////////////////////
TRACELOG_API int32 Decode(const void* Src, int32 SrcSize, void* Dest, int32 DestSize)
{
	return LZ4_decompress_safe((const char*)Src, (char*)Dest, SrcSize, DestSize);
}

} // namespace Private
} // namespace Trace
