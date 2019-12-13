// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"

class FStringView;
class FAnsiStringView;

namespace String
{
	/** 
	 * Convert an array of hex digits into an array of bytes.
	 *
	 * @param Hex Array of hex digits to convert.
	 * @param OutBytes [out] Array of at least (Hex.Len()+1)/2 output bytes.
	 *
	 * @return Number of bytes written to the output.
	 */
	CORE_API int32 HexToBytes(const FStringView& Hex, uint8* OutBytes);
	CORE_API int32 HexToBytes(const FAnsiStringView& Hex, uint8* OutBytes);
}
