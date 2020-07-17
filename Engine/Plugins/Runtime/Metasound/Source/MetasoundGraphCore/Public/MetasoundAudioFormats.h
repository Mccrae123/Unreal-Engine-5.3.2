// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MetasoundDataReference.h"
#include "MetasoundAudioBuffer.h"

namespace Metasound
{
	/** FUnformattedAudio
	 *
	 * FUnformattedAudio represents deinterleaved multichannel audio which can dynamically change
	 * the number of channels of audio. In possible, using FMultichannelAudioFormat is preferred over
	 * using FUnformattedAudio when the number of audio channels is known at construction.
	 *
	 * The audio buffers in FUnformattedAudio are shared data references which can be accessed outside
	 * of the FUnformattedAudio. All audio buffers within a FUnformattedAudio object must contain the 
	 * same number of audio frames.
	 */
	class METASOUNDGRAPHCORE_API FUnformattedAudio
	{
		public:
			/** FUnformattedAudio Constructor.
			 * 
			 * @param InNumFrames - Number of frames to hold in each buffer.
			 * @param InNumChannels - Initial number of audio channels.
			 * @param InMaxNumChannels - Maximum number of audio channels.
			 */
			FUnformattedAudio(int32 InNumFrames, int32 InNumChannels, int32 InMaxNumChannels);

			/** Sets the number of audio channels.
			 * 
			 * The requested number of channels is internally clamped to the supported range 
			 * of [0, GetMaxNumChannels()].
			 *
			 * @parma InNumChannels - The desired number of channels.
			 *
			 * @return The actual number of channels. This may differ from the requested number 
			 * 		   of channels if the requested number exceeded the maximum number of channels.
			 */
			int32 SetNumChannels(int32 InNumChannels);

			/** Return the number of channels. */
			int32 GetNumChannels() const { return NumChannels; }

			/** Return the maximum number of channels. */
			int32 GetMaxNumChannels() const { return MaxNumChannels; }

			/** Return an array view of the readable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferReadRef>& GetBuffers() const { return ReadableBuffers; }

			/** Return an array view of the writable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferWriteRef>& GetBuffers() { return WritableBuffers; }

			/** Return an array of the readable buffer reference storage.
			 *
			 * This array will have GetMaxNumChannels() elements.
			 */
			const TArray<FAudioBufferReadRef>& GetStorage() const { return ReadableBufferStorage; }

			/** Return an array of the writable buffer reference storage.
			 *
			 * This array will have GetMaxNumChannels() elements.
			 */
			const TArray<FAudioBufferWriteRef>& GetStorage() { return WritableBufferStorage; }
			
		private:
			int32 NumFrames;
			int32 NumChannels;
			int32 MaxNumChannels;

			TArrayView<const FAudioBufferReadRef> ReadableBuffers;
			TArrayView<const FAudioBufferWriteRef> WritableBuffers;

			TArray<FAudioBufferReadRef> ReadableBufferStorage;
			TArray<FAudioBufferWriteRef> WritableBufferStorage;
	};


	/** FMultichannelAudioFormat
	 *
	 * FMultichannelAudioFormat represents deinterleaved multichannel audio which supports a constant
	 * number of channels for the lifetime of the object. 
	 *
	 * The audio buffers in FMultichannelAudioFormat are shared data references which can be accessed outside
	 * of the FMultichannelAudioFormat. All audio buffers within a FMultichannelAudioFormat object must contain the 
	 * same number of audio frames.
	 */
	class METASOUNDGRAPHCORE_API FMultichannelAudioFormat
	{
		public:
			/** FMultichannelAudioFormat Constructor.
			 *
			 * @param InNumFrames - The number of frames per an audio buffer.
			 * @param InNumChannels - The number of audio channels.
			 */
			FMultichannelAudioFormat(int32 InNumFrames, int32 InNumChannels);

			/** FMultichannelAudioFormat Constructor.
			 *
			 * This constructor accepts an array of writable audio buffer references. Each 
			 * buffer in the array must contain equal number of frames. 
			 *
			 * @param InWriteRefs - An array of writable audio buffer references. 
			 */
			FMultichannelAudioFormat(TArrayView<const FAudioBufferWriteRef> InWriteRefs);

			// Enable the copy constructor.
			FMultichannelAudioFormat(const FMultichannelAudioFormat& InOther) = default;

			// Disable move constructor as incoming object should not be altered. 
			FMultichannelAudioFormat(FMultichannelAudioFormat&& InOther) = delete;

			// Disable equal operator so channel count does not change
			FMultichannelAudioFormat& operator=(const FMultichannelAudioFormat& Other) = delete;

			// Disable move operator so channel count does not change
			FMultichannelAudioFormat& operator=(FMultichannelAudioFormat&& Other) = delete;

			/** Return the number of audio channels. */
			int32 GetNumChannels() const { return NumChannels; }

			/** Return an array view of the readable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferReadRef>& GetBuffers() const { return ReadableBuffers; }

			/** Return an array view of the writable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferWriteRef>& GetBuffers() { return WritableBuffers; }

			/** Return an array of the readable buffer reference storage.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArray<FAudioBufferReadRef>& GetStorage() const { return ReadableBufferStorage; }

			/** Return an array of the writable buffer reference storage.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArray<FAudioBufferWriteRef>& GetStorage() { return WritableBufferStorage; }

		private:
			// Friendship with the data reference class gives it access to the protected constructor
			// for the scenario where a TDataReadReference<FMulitchannelAudioFormat> is constructed
			// with FAudioBufferReadRefs. The constructor cannot be public as it would provide writable
			// access to the passed in audio buffers, even though the passed buffers explicitly were
			// read references.
			friend class TDataReadReference<FMultichannelAudioFormat>;

			// Construct a FMultichannelAudioFormat with an array of readable buffers. 
			// 
			// This constructor should only be used when it can be assured that the constructed object
			// will not provide writable access to the passed in audio buffers. 
			FMultichannelAudioFormat(TArrayView<const FAudioBufferReadRef> InReadRefs);

			int32 NumChannels;

			TArrayView<const FAudioBufferWriteRef> WritableBuffers;
			TArrayView<const FAudioBufferReadRef> ReadableBuffers;

			TArray<FAudioBufferWriteRef> WritableBufferStorage;
			TArray<FAudioBufferReadRef> ReadableBufferStorage;
	};

	
	/** A TStaticChannelAudioFormat represents deinterleaved multichannel audio 
	 * where the number of channels is known at compile time. This is primarily 
	 * useful to define such cases as Stereo, Mono, Qaud, 5.1, etc. 
	 *
	 * The audio buffers in FMultichannelAudioFormat are shared data references 
	 * which can be accessed outside of the FMultichannelAudioFormat. All audio 
	 * buffers within a FMultichannelAudioFormat object must contain the same 
	 * number of audio frames.
	 */
	template<int32 NumChannels>
	class TStaticChannelAudioFormat
	{
		public:
			/** TStaticChannelAudioFormat Constructor
			 *
			 * @param InNumFrames - The number of frames per an audio buffer.
			 */
			TStaticChannelAudioFormat(int32 InNumFrames)
			{
				static_assert(NumChannels > 0, "NumChannels must be greater than zero");

				InNumFrames = FMath::Max(InNumFrames, 0);

				for (int32 i = 0; i < NumChannels; i++)
				{
					FAudioBufferWriteRef Audio(InNumFrames);
					Audio->Zero();

					WritableBufferStorage.Add(Audio);
					ReadableBufferStorage.Add(Audio);
				}

				WritableBuffers = WritableBufferStorage;
				ReadableBuffers = ReadableBufferStorage;
			}

			/** Return the number of audio channels. */
			int32 GetNumChannels() const
			{
				return NumChannels;
			}

			/** Return an readable buffer reference for a specific channel. */
			template<int32 ChannelIndex>
			FAudioBufferReadRef GetBuffer() const
			{
				static_assert(ChannelIndex >= 0, "Index must be within range of channels");
				static_assert(ChannelIndex < NumChannels, "Index must be within range of channels");

				return ReadableBuffers.GetData()[ChannelIndex];
			}

			/** Return an writable buffer reference for a specific channel. */
			template<int32 ChannelIndex>
			FAudioBufferWriteRef GetBuffer() 
			{
				static_assert(ChannelIndex >= 0, "Index must be within range of channels");
				static_assert(ChannelIndex < NumChannels, "Index must be within range of channels");

				return WritableBuffers.GetData()[ChannelIndex];
			}

			/** Return an array view of the readable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferReadRef> GetBuffers() const
			{
				return ReadableBuffers;
			}

			/** Return an array view of the writable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArrayView<const FAudioBufferWriteRef> GetBuffers() 
			{
				return WritableBuffers;
			}

			/** Return an array of the readable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArray<FAudioBufferReadRef> GetStorage() const
			{
				return ReadableBufferStorage;
			}

			/** Return an array of the writable buffer references.
			 *
			 * This array will have GetNumChannels() elements.
			 */
			const TArray<FAudioBufferWriteRef> GetStorage()
			{
				return WritableBufferStorage;
			}

		protected:

			// TStaticChannelAudioFormat constructor with an array of writable buffers. 
			TStaticChannelAudioFormat(const FAudioBufferWriteRef(&InBuffers)[NumChannels])
			{
				int32 NumFrames = 0;
				if (NumChannels > 0)
				{
					NumFrames = InBuffers[0]->Num();
				}

				for (int32 i = 0; i < NumChannels; i++)
				{
					checkf(NumFrames == InBuffers[i]->Num(), TEXT("All buffers must have same number of frames (%d != %d)"), NumFrames, InBuffers[i]->Num());

					WritableBufferStorage.Add(InBuffers[i]);
					ReadableBufferStorage.Add(InBuffers[i]);
				}

				WritableBuffers = WritableBufferStorage;
				ReadableBuffers = ReadableBufferStorage;
			}

		private:
			TArrayView<const FAudioBufferWriteRef> WritableBuffers;
			TArrayView<const FAudioBufferReadRef> ReadableBuffers;

			TArray<FAudioBufferWriteRef> WritableBufferStorage;
			TArray<FAudioBufferReadRef> ReadableBufferStorage;
	};

	/** FMonoAudioFormat represents mono audio containing one channel of audio.
	 *
	 * The audio buffer is a shared data references which can be accessed 
	 * outside of the FMonoAudioFormat. 
	 */
	class FMonoAudioFormat : public TStaticChannelAudioFormat<1>
	{
			// This is used to grant specific classes the ability to create FMonoAudioFormat
			// using a FAudioBufferReadRef
			//enum EPrivateToken { Token };
			//static const EPrivateToken PrivateToken = EPrivateToken::Token;

		public:
			using Super = TStaticChannelAudioFormat<1>;

			// Inherit constructors of base class.
			using Super::Super;

			/** FMonoAudioFormat Construtor
			 *
			 * Construct with a single writable audio buffer reference.
			 *
			 * @param InAudio - A writable audio buffer reference.
			 */
			FMonoAudioFormat(const FAudioBufferWriteRef& InAudio)
			:	Super({InAudio})
			{
			}

			// TODO: clean up this mess.
			// Construct a FMonoAudioFormat a readable buffer reference.
			// 
			// This constructor should only be used when it can be assured 
			// that the constructed object will not provide writable access 
			// to the passed in audio buffers. 
			/*
			FMonoAudioFormat(const FAudioBufferReadRef& InAudio, EPrivateToken InToken)
			:	Super({WriteCast(InAudio)})
			{
			}
			*/

			/** Return writable audio buffer reference of center channel. */
			FAudioBufferWriteRef GetCenter() { return GetBuffer<0>(); }

			/** Return readable audio buffer reference of center channel. */
			FAudioBufferReadRef GetCenter() const { return GetBuffer<0>(); }

		private:


			// Friendship with the data reference class gives it access to the 
			// protected constructor for the scenario where a 
			// TDataReadReference<*> is constructed with FAudioBufferReadRefs. 
			// The constructor cannot be public as it would provide writable
			// access to the passed in audio buffers, even though the passed 
			// buffers explicitly were read references.
			//friend class TDataReadReference<FMonoAudioFormat>;
	};

	// Template specialization for TDataReadReference<FMonoAudioFormat> constructor to use
	// private token.
	/*
	template<>
	template<>
	TDataReadReference<FMonoAudioFormat>::TDataReadReference(const FAudioBufferReadRef& InReadBuffer)
	:	FDataReference(InReadBuffer, FMonoAudioFormat::PrivateToken)
	{
	}
	*/
	

	/** FStereoAudioFormat represents stereo audio containing two channels of 
	 * audio.
	 *
	 * The audio buffers are shared data references which can be accessed 
	 * outside of the FStereoAudioFormat. 
	 */
	class FStereoAudioFormat : public TStaticChannelAudioFormat<2>
	{
		public:
			using Super = TStaticChannelAudioFormat<2>;

			// Inherit constructors of base class.
			using Super::Super;

			/** FStereoAudioFormat Construtor
			 *
			 * Construct with a two writable audio buffer reference.
			 *
			 * @param InLeftAudio - A writable audio buffer reference for the left channel.
			 * @param InRightAudio - A writable audio buffer reference for the right channel.
			 */
			FStereoAudioFormat(const FAudioBufferWriteRef& InLeftAudio, const FAudioBufferWriteRef& InRightAudio)
			:	Super({InLeftAudio, InRightAudio})
			{
			}

			/** Return writable audio buffer reference of left channel. */
			FAudioBufferWriteRef GetLeft() { return GetBuffer<0>(); }

			/** Return readable audio buffer reference of left channel. */
			FAudioBufferReadRef GetLeft() const { return GetBuffer<0>(); }

			/** Return writable audio buffer reference of right channel. */
			FAudioBufferWriteRef GetRight() { return GetBuffer<1>(); }

			/** Return readable audio buffer reference of right channel. */
			FAudioBufferReadRef GetRight() const { return GetBuffer<1>(); }

		protected:

			// Friendship with the data reference class gives it access to the 
			// protected constructor for the scenario where a 
			// TDataReadReference<*> is constructed with FAudioBufferReadRefs. 
			// The constructor cannot be public as it would provide writable
			// access to the passed in audio buffers, even though the passed 
			// buffers explicitly were read references.
			friend class TDataReadReference<FStereoAudioFormat>;

			// Construct a FStereoAudioFormat a readable buffer reference.
			// 
			// This constructor should only be used when it can be assured 
			// that the constructed object will not provide writable access 
			// to the passed in audio buffers. 
			FStereoAudioFormat(const FAudioBufferReadRef& InLeftAudio, const FAudioBufferReadRef& InRightAudio)
			:	Super({WriteCast(InLeftAudio), WriteCast(InRightAudio)})
			{
			}
	};


	DECLARE_METASOUND_DATA_REFERENCE_TYPES(FUnformattedAudio, "Audio:Unformatted", 0xd78a3ed1 , FUnformattedAudioTypeInfo, FUnformattedAudioReadRef, FUnformattedAudioWriteRef);

	DECLARE_METASOUND_DATA_REFERENCE_TYPES(FMultichannelAudioFormat, "Audio:Multichannel", 0x56bdcbe0 , FMultichannelAudioFormatTypeInfo, FMultichannelAudioFormatReadRef, FMultichannelAudioFormatWriteRef);

	DECLARE_METASOUND_DATA_REFERENCE_TYPES(FMonoAudioFormat, "Audio:Mono", 0x6f468c8c, FMonoAudioFormatTypeInfo, FMonoAudioFormatReadRef, FMonoAudioFormatWriteRef);

	DECLARE_METASOUND_DATA_REFERENCE_TYPES(FStereoAudioFormat, "Audio:Stereo", 0xb55304e2 , FStereoAudioFormatTypeInfo, FStereoAudioFormatReadRef, FStereoAudioFormatWriteRef);


}
