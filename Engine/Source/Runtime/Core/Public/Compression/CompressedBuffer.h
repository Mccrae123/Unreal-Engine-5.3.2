// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Memory/CompositeBuffer.h"
#include "Memory/MemoryFwd.h"

class FArchive;
struct FBlake3Hash;

namespace FOodleDataCompression { enum class ECompressionLevel : int8; }
namespace FOodleDataCompression { enum class ECompressor : uint8; }

using ECompressedBufferCompressionLevel = FOodleDataCompression::ECompressionLevel;
using ECompressedBufferCompressor = FOodleDataCompression::ECompressor;

/**
 * A compressed buffer stores compressed data in a self-contained format.
 *
 * A buffer is self-contained in the sense that it can be decompressed without external knowledge
 * of the compression format or the size of the raw data.
 */
class FCompressedBuffer
{
public:
	/**
	 * Compress the buffer using a balanced level of compression.
	 *
	 * @return An owned compressed buffer, or null on error.
	 */
	[[nodiscard]] CORE_API static FCompressedBuffer Compress(const FCompositeBuffer& RawData);
	[[nodiscard]] CORE_API static FCompressedBuffer Compress(const FSharedBuffer& RawData);

	/**
	 * Compress the buffer using the specified compressor and compression level.
	 *
	 * Data that does not compress will be return uncompressed, as if with level None.
	 *
	 * @note Using a level of None will return a buffer that references owned raw data.
	 *
	 * @param RawData            The raw data to be compressed.
	 * @param Compressor         The compressor to encode with. May use NotSet if level is None.
	 * @param CompressionLevel   The compression level to encode with.
	 * @param BlockSize          The power-of-two block size to encode raw data in. 0 is default.
	 * @return An owned compressed buffer, or null on error.
	 */
	[[nodiscard]] CORE_API static FCompressedBuffer Compress(
		const FCompositeBuffer& RawData,
		ECompressedBufferCompressor Compressor,
		ECompressedBufferCompressionLevel CompressionLevel,
		uint64 BlockSize = 0);
	[[nodiscard]] CORE_API static FCompressedBuffer Compress(
		const FSharedBuffer& RawData,
		ECompressedBufferCompressor Compressor,
		ECompressedBufferCompressionLevel CompressionLevel,
		uint64 BlockSize = 0);

	/**
	 * Construct from a compressed buffer previously created by Compress().
	 *
	 * @return A compressed buffer, or null on error, such as an invalid format or corrupt header.
	 */
	[[nodiscard]] CORE_API static FCompressedBuffer FromCompressed(const FCompositeBuffer& CompressedData);
	[[nodiscard]] CORE_API static FCompressedBuffer FromCompressed(FCompositeBuffer&& CompressedData);
	[[nodiscard]] CORE_API static FCompressedBuffer FromCompressed(const FSharedBuffer& CompressedData);
	[[nodiscard]] CORE_API static FCompressedBuffer FromCompressed(FSharedBuffer&& CompressedData);
	[[nodiscard]] CORE_API static FCompressedBuffer FromCompressed(FArchive& Ar);

	/** Reset this to null. */
	inline void Reset() { CompressedData.Reset(); }

	/** Returns true if the compressed buffer is not null. */
	[[nodiscard]] inline explicit operator bool() const { return !IsNull(); }

	/** Returns true if the compressed buffer is null. */
	[[nodiscard]] inline bool IsNull() const { return CompressedData.IsNull(); }

	/** Returns true if the composite buffer is owned. */
	[[nodiscard]] inline bool IsOwned() const { return CompressedData.IsOwned(); }

	/** Returns a copy of the compressed buffer that owns its underlying memory. */
	[[nodiscard]] inline FCompressedBuffer MakeOwned() const & { return FromCompressed(CompressedData.MakeOwned()); }
	[[nodiscard]] inline FCompressedBuffer MakeOwned() && { return FromCompressed(MoveTemp(CompressedData).MakeOwned()); }

	/** Returns a composite buffer containing the compressed data. May be null. May not be owned. */
	[[nodiscard]] inline const FCompositeBuffer& GetCompressed() const & { return CompressedData; }
	[[nodiscard]] inline FCompositeBuffer GetCompressed() && { return MoveTemp(CompressedData); }

	/** Returns the size of the compressed data. Zero if this is null. */
	[[nodiscard]] inline uint64 GetCompressedSize() const { return CompressedData.GetSize(); }

	/** Returns the size of the raw data. Zero on error or if this is empty or null. */
	[[nodiscard]] CORE_API uint64 GetRawSize() const;

	/** Returns the hash of the raw data. Zero on error or if this is null. */
	[[nodiscard]] CORE_API FBlake3Hash GetRawHash() const;

	/**
	 * Returns the compressor and compression level used by this buffer.
	 *
	 * The compressor and compression level may differ from those specified when creating the buffer
	 * because an incompressible buffer is stored with no compression. Parameters cannot be accessed
	 * if this is null or uses a method other than Oodle, in which case this returns false.
	 *
	 * @return True if parameters were written, otherwise false.
	 */
	[[nodiscard]] CORE_API bool TryGetCompressParameters(
		ECompressedBufferCompressor& OutCompressor,
		ECompressedBufferCompressionLevel& OutCompressionLevel) const;

	/**
	 * Decompress into a memory view that is less or equal to GetRawSize()
	 */
	[[nodiscard]] CORE_API bool TryDecompressTo(FMutableMemoryView RawView, uint64 RawOffset = 0) const;

	/**
	 * Decompress into an owned buffer.
	 *
	 * @return An owned buffer containing the raw data, or null on error.
	 */
	[[nodiscard]] CORE_API FSharedBuffer Decompress(uint64 RawOffset = 0, uint64 RawSize = MAX_uint64) const;

	/**
	 * Decompress into an owned composite buffer.
	 *
	 * @return An owned buffer containing the raw data, or null on error.
	 */
	[[nodiscard]] CORE_API FCompositeBuffer DecompressToComposite() const;

	/** A null compressed buffer. */
	static const FCompressedBuffer Null;

private:
	FCompositeBuffer CompressedData;
};

inline const FCompressedBuffer FCompressedBuffer::Null;

CORE_API FArchive& operator<<(FArchive& Ar, FCompressedBuffer& Buffer);
