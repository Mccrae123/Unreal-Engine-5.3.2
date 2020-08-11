// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MetasoundDataReference.h"
#include "MetasoundDataTypeRegistrationMacro.h"

// Forward declares
namespace Audio
{	
	struct IDecoderInput;
}

class USoundWave;

namespace Metasound
{
	// Forward declare ReadRef
	class FWave;
	typedef TDataReadReference<FWave> FWaveReadRef;
		
	class METASOUNDENGINE_API FWave
	{
		TArray<uint8> CompressedBytes;
		friend class FWaveDecoderInput;

	public:
		FWave() = default;

		// For testing only.
		FWave(const TArray<uint8>& InBytes);
		
		FWave(USoundWave* InWave);

		using FDecoderInputPtr = TSharedPtr<Audio::IDecoderInput,ESPMode::ThreadSafe>;

		// Factory function to create a Decoder input
		static FDecoderInputPtr CreateDecoderInput(
			const FWaveReadRef& InWaveRef);
	};
	DECLARE_METASOUND_DATA_REFERENCE_TYPES(FWave, METASOUNDENGINE_API, 0x0ddba11, FWaveTypeInfo, FWaveReadRef, FWaveWriteRef)
}
