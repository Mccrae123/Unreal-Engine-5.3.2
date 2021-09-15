// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	TextureDerivedDataTask.cpp: Tasks to update texture DDC.
=============================================================================*/

#include "TextureDerivedDataTask.h"
#include "Misc/CommandLine.h"
#include "Stats/Stats.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Serialization/MemoryReader.h"
#include "UObject/Package.h"
#include "RenderUtils.h"
#include "TextureResource.h"
#include "Engine/Texture.h"
#include "Engine/Texture2DArray.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "VT/VirtualTextureBuiltData.h"

#if WITH_EDITOR

#include "Algo/Accumulate.h"
#include "DerivedDataBuild.h"
#include "DerivedDataBuildAction.h"
#include "DerivedDataBuildInputResolver.h"
#include "DerivedDataBuildInputs.h"
#include "DerivedDataBuildOutput.h"
#include "DerivedDataBuildSession.h"
#include "DerivedDataCacheInterface.h"
#include "DerivedDataCacheKey.h"
#include "DerivedDataPayload.h"
#include "DerivedDataRequestOwner.h"
#include "Engine/TextureCube.h"
#include "Engine/VolumeTexture.h"
#include "GenericPlatform/GenericPlatformMath.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITextureFormat.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Optional.h"
#include "Misc/ScopeRWLock.h"
#include "ProfilingDebugging/CookStats.h"
#include "Serialization/BulkDataRegistry.h"
#include "Templates/CheckValueCast.h"
#include "TextureDerivedDataBuildUtils.h"
#include "VT/VirtualTextureChunkDDCCache.h"
#include "VT/VirtualTextureDataBuilder.h"
#include <atomic>

static TAutoConsoleVariable<int32> CVarVTValidateCompressionOnLoad(
	TEXT("r.VT.ValidateCompressionOnLoad"),
	0,
	TEXT("Validates that VT data contains no compression errors when loading from DDC")
	TEXT("This is slow, but allows debugging corrupt VT data (and allows recovering from bad DDC)")
);

static TAutoConsoleVariable<int32> CVarVTValidateCompressionOnSave(
	TEXT("r.VT.ValidateCompressionOnSave"),
	0,
	TEXT("Validates that VT data contains no compression errors before saving to DDC")
	TEXT("This is slow, but allows debugging corrupt VT data")
);

void GetTextureDerivedDataKeyFromSuffix(const FString& KeySuffix, FString& OutKey);

class FTextureStatusMessageContext : public FScopedSlowTask
{
public:
	explicit FTextureStatusMessageContext(const FText& InMessage)
		: FScopedSlowTask(0, InMessage, IsInGameThread())
	{
		UE_LOG(LogTexture,Display,TEXT("%s"),*InMessage.ToString());
	}
};

static FText ComposeTextureBuildText(const UTexture& Texture, int32 SizeX, int32 SizeY, int32 NumBlocks, int32 NumLayers, const FTextureBuildSettings& BuildSettings, int64 RequiredMemoryEstimate, bool bIsVT)
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("TextureName"), FText::FromString(Texture.GetPathName()));
	Args.Add(TEXT("TextureFormatName"), FText::FromString(BuildSettings.TextureFormatName.GetPlainNameString()));
	Args.Add(TEXT("IsVT"), FText::FromString( FString( bIsVT ? TEXT(" VT") : TEXT("") ) ) );
	Args.Add(TEXT("TextureResolutionX"), FText::FromString(FString::FromInt(SizeX)));
	Args.Add(TEXT("TextureResolutionY"), FText::FromString(FString::FromInt(SizeY)));
	Args.Add(TEXT("NumBlocks"), FText::FromString(FString::FromInt(NumBlocks)));
	Args.Add(TEXT("NumLayers"), FText::FromString(FString::FromInt(NumLayers)));
	Args.Add(TEXT("EstimatedMemory"), FText::FromString(FString::SanitizeFloat(double(RequiredMemoryEstimate) / (1024.0*1024.0), 3)));

	return FText::Format(
		NSLOCTEXT("Engine", "BuildTextureStatus", "Building textures: {TextureName} ({TextureFormatName}{IsVT}, {TextureResolutionX}X{TextureResolutionY} X{NumBlocks}X{NumLayers}) (Required Memory Estimate: {EstimatedMemory} MB)"), 
		Args
	);
}

static FText ComposeTextureBuildText(const UTexture& Texture, const FTextureSourceData& TextureData, const FTextureBuildSettings& BuildSettings, int64 RequiredMemoryEstimate, bool bIsVT)
{
	return ComposeTextureBuildText(Texture, TextureData.Blocks[0].MipsPerLayer[0][0].SizeX, TextureData.Blocks[0].MipsPerLayer[0][0].SizeY, TextureData.Blocks.Num(), TextureData.Layers.Num(), BuildSettings, RequiredMemoryEstimate, bIsVT);
}

static FText ComposeTextureBuildText(const UTexture& Texture, const FTextureBuildSettings& BuildSettings, int64 RequiredMemoryEstimate, bool bIsVT)
{
	return ComposeTextureBuildText(Texture, Texture.Source.GetSizeX(), Texture.Source.GetSizeY(), Texture.Source.GetNumBlocks(), Texture.Source.GetNumLayers(), BuildSettings, RequiredMemoryEstimate, bIsVT);
}

static bool ValidateTexture2DPlatformData(const FTexturePlatformData& TextureData, const UTexture2D& Texture, bool bFromDDC)
{
	// Temporarily disable as the size check reports false negatives on some platforms
#if 0
	bool bValid = true;
	for (int32 MipIndex = 0; MipIndex < TextureData.Mips.Num(); ++MipIndex)
	{
		const FTexture2DMipMap& MipMap = TextureData.Mips[MipIndex];
		const int64 BulkDataSize = MipMap.BulkData.GetBulkDataSize();
		if (BulkDataSize > 0)
		{
			const int64 ExpectedMipSize = CalcTextureMipMapSize(TextureData.SizeX, TextureData.SizeY, TextureData.PixelFormat, MipIndex);
			if (BulkDataSize != ExpectedMipSize)
			{
				//UE_LOG(LogTexture,Warning,TEXT("Invalid mip data. Texture will be rebuilt. MipIndex %d [%dx%d], Expected size %lld, BulkData size %lld, PixelFormat %s, LoadedFromDDC %d, Texture %s"), 
				//	MipIndex, 
				//	MipMap.SizeX, 
				//	MipMap.SizeY, 
				//	ExpectedMipSize, 
				//	BulkDataSize, 
				//	GPixelFormats[TextureData.PixelFormat].Name, 
				//	bFromDDC ? 1 : 0,
				//	*Texture.GetFullName());
				
				bValid = false;
			}
		}
	}

	return bValid;
#else
	return true;
#endif
}

void FTextureSourceData::Init(UTexture& InTexture, const FTextureBuildSettings* InBuildSettingsPerLayer, bool bAllowAsyncLoading)
{
	const int32 NumBlocks = InTexture.Source.GetNumBlocks();
	const int32 NumLayers = InTexture.Source.GetNumLayers();
	if (NumBlocks < 1 || NumLayers < 1)
	{
		UE_LOG(LogTexture, Warning, TEXT("Texture has no source data: %s"), *InTexture.GetPathName());
		return;
	}

	Layers.Reserve(NumLayers);
	for (int LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
	{
		FTextureSourceLayerData* LayerData = new(Layers) FTextureSourceLayerData();
		switch (InTexture.Source.GetFormat(LayerIndex))
		{
		case TSF_G8:		LayerData->ImageFormat = ERawImageFormat::G8;		break;
		case TSF_G16:		LayerData->ImageFormat = ERawImageFormat::G16;		break;
		case TSF_BGRA8:		LayerData->ImageFormat = ERawImageFormat::BGRA8;	break;
		case TSF_BGRE8:		LayerData->ImageFormat = ERawImageFormat::BGRE8;	break;
		case TSF_RGBA16:	LayerData->ImageFormat = ERawImageFormat::RGBA16;	break;
		case TSF_RGBA16F:	LayerData->ImageFormat = ERawImageFormat::RGBA16F;  break;
		default:
			UE_LOG(LogTexture, Fatal, TEXT("Texture %s has source art in an invalid format."), *InTexture.GetName());
			return;
		}

		FTextureFormatSettings FormatSettings;
		InTexture.GetLayerFormatSettings(LayerIndex, FormatSettings);
		LayerData->GammaSpace = FormatSettings.SRGB ? (InTexture.bUseLegacyGamma ? EGammaSpace::Pow22 : EGammaSpace::sRGB) : EGammaSpace::Linear;
	}

	Blocks.Reserve(NumBlocks);
	for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
	{
		FTextureSourceBlock SourceBlock;
		InTexture.Source.GetBlock(BlockIndex, SourceBlock);

		if (SourceBlock.NumMips > 0 && SourceBlock.NumSlices > 0)
		{
			FTextureSourceBlockData* BlockData = new(Blocks) FTextureSourceBlockData();
			BlockData->BlockX = SourceBlock.BlockX;
			BlockData->BlockY = SourceBlock.BlockY;
			BlockData->SizeX = SourceBlock.SizeX;
			BlockData->SizeY = SourceBlock.SizeY;
			BlockData->NumMips = SourceBlock.NumMips;
			BlockData->NumSlices = SourceBlock.NumSlices;

			if (InBuildSettingsPerLayer[0].MipGenSettings != TMGS_LeaveExistingMips)
			{
				BlockData->NumMips = 1;
			}

			if (!InBuildSettingsPerLayer[0].bCubemap && !InBuildSettingsPerLayer[0].bTextureArray && !InBuildSettingsPerLayer[0].bVolume)
			{
				BlockData->NumSlices = 1;
			}

			BlockData->MipsPerLayer.SetNum(NumLayers);

			SizeInBlocksX = FMath::Max(SizeInBlocksX, SourceBlock.BlockX + 1);
			SizeInBlocksY = FMath::Max(SizeInBlocksY, SourceBlock.BlockY + 1);
			BlockSizeX = FMath::Max(BlockSizeX, SourceBlock.SizeX);
			BlockSizeY = FMath::Max(BlockSizeY, SourceBlock.SizeY);
		}
	}

	for (FTextureSourceBlockData& Block : Blocks)
	{
		const int32 MipBiasX = FMath::CeilLogTwo(BlockSizeX / Block.SizeX);
		const int32 MipBiasY = FMath::CeilLogTwo(BlockSizeY / Block.SizeY);
		if (MipBiasX != MipBiasY)
		{
			UE_LOG(LogTexture, Warning, TEXT("Texture has blocks with mismatched aspect ratios"), *InTexture.GetPathName());
			return;
		}

		Block.MipBias = MipBiasX;
	}

	TextureName = InTexture.GetFName();

	if (bAllowAsyncLoading && !InTexture.Source.IsBulkDataLoaded())
	{
		// Prepare the async source to be later able to load it from file if required.
		AsyncSource = InTexture.Source.CopyTornOff(); // This copies information required to make a safe IO load async.
	}

	bValid = true;
}

void FTextureSourceData::GetSourceMips(FTextureSource& Source, IImageWrapperModule* InImageWrapper)
{
	if (bValid)
	{
		if (Source.HasHadBulkDataCleared())
		{	// don't do any work we can't reload this
			UE_LOG(LogTexture, Error, TEXT("Unable to get texture source mips because its bulk data was released. %s"), *TextureName.ToString())
				return;
		}

		const FTextureSource::FMipData ScopedMipData = Source.GetMipData(InImageWrapper);

		for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
		{
			FTextureSourceBlock SourceBlock;
			Source.GetBlock(BlockIndex, SourceBlock);

			FTextureSourceBlockData& BlockData = Blocks[BlockIndex];
			for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
			{
				const FTextureSourceLayerData& LayerData = Layers[LayerIndex];
				if (!BlockData.MipsPerLayer[LayerIndex].Num()) // If we already got valid data, nothing to do.
				{
					int32 MipSizeX = SourceBlock.SizeX;
					int32 MipSizeY = SourceBlock.SizeY;
					for (int32 MipIndex = 0; MipIndex < BlockData.NumMips; ++MipIndex)
					{
						FImage* SourceMip = new(BlockData.MipsPerLayer[LayerIndex]) FImage(
							MipSizeX, MipSizeY,
							BlockData.NumSlices,
							LayerData.ImageFormat,
							LayerData.GammaSpace
						);

						if (!ScopedMipData.GetMipData(SourceMip->RawData, BlockIndex, LayerIndex, MipIndex))
						{
							UE_LOG(LogTexture, Warning, TEXT("Cannot retrieve source data for mip %d of texture %s"), MipIndex, *TextureName.ToString());
							ReleaseMemory();
							bValid = false;
							break;
						}

						MipSizeX = FMath::Max(MipSizeX / 2, 1);
						MipSizeY = FMath::Max(MipSizeY / 2, 1);
					}
				}
			}
		}
	}
}

void FTextureSourceData::GetAsyncSourceMips(IImageWrapperModule* InImageWrapper)
{
	if (bValid && !Blocks[0].MipsPerLayer[0].Num() && AsyncSource.HasPayloadData())
	{
		GetSourceMips(AsyncSource, InImageWrapper);
	}
}

namespace UE::TextureDerivedData
{

using namespace UE::DerivedData;

class FTextureBuildInputResolver final : public IBuildInputResolver
{
public:
	explicit FTextureBuildInputResolver(UTexture& InTexture)
		: Texture(InTexture)
	{
	}

	const FCompressedBuffer& FindSource(FCompressedBuffer& Buffer, FTextureSource& Source, const FGuid& BulkDataId)
	{
		if (Source.GetPersistentId() != BulkDataId)
		{
			return FCompressedBuffer::Null;
		}
		if (!Buffer)
		{
			Source.OperateOnLoadedBulkData([&Buffer](const FSharedBuffer& BulkDataBuffer)
			{
				Buffer = FCompressedBuffer::Compress(BulkDataBuffer);
			});
		}
		return Buffer;
	}

	void ResolveInputMeta(
		const FBuildDefinition& Definition,
		IRequestOwner& Owner,
		FOnBuildInputMetaResolved&& OnResolved) final
	{
		EStatus Status = EStatus::Ok;
		TArray<FString> InputKeys;
		TArray<FBuildInputMetaByKey> Inputs;
		Definition.IterateInputBulkData([this, &Status, &InputKeys, &Inputs](FStringView Key, const FGuid& BulkDataId)
		{
			const FCompressedBuffer& Buffer = Key == TEXT("Source"_SV)
				? FindSource(SourceBuffer, Texture.Source, BulkDataId)
				: FindSource(CompositeSourceBuffer, Texture.CompositeTexture->Source, BulkDataId);
			if (Buffer)
			{
				InputKeys.Emplace(Key);
				Inputs.Add({InputKeys.Last(), Buffer.GetRawHash(), Buffer.GetRawSize()});
			}
			else
			{
				Status = EStatus::Error;
			}
		});
		OnResolved({Inputs, Status});
	}

	void ResolveInputData(
		const FBuildDefinition& Definition,
		IRequestOwner& Owner,
		FOnBuildInputDataResolved&& OnResolved,
		FBuildInputFilter&& Filter) final
	{
		EStatus Status = EStatus::Ok;
		TArray<FString> InputKeys;
		TArray<FBuildInputDataByKey> Inputs;
		Definition.IterateInputBulkData([this, &Filter, &Status, &InputKeys, &Inputs](FStringView Key, const FGuid& BulkDataId)
		{
			if (!Filter || Filter(Key))
			{
				const FCompressedBuffer& Buffer = Key == TEXT("Source"_SV)
					? FindSource(SourceBuffer, Texture.Source, BulkDataId)
					: FindSource(CompositeSourceBuffer, Texture.CompositeTexture->Source, BulkDataId);
				if (Buffer)
				{
					InputKeys.Emplace(Key);
					Inputs.Add({InputKeys.Last(), Buffer});
				}
				else
				{
					Status = EStatus::Error;
				}
			}
		});
		OnResolved({Inputs, Status});
	}

private:
	UTexture& Texture;
	FCompressedBuffer SourceBuffer;
	FCompressedBuffer CompositeSourceBuffer;
};

} // UE::TextureDerivedData

void FTextureCacheDerivedDataWorker::BuildTexture(bool bReplaceExistingDDC)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCacheDerivedDataWorker::BuildTexture);

	const bool bHasValidMip0 = TextureData.Blocks.Num() && TextureData.Blocks[0].MipsPerLayer.Num() && TextureData.Blocks[0].MipsPerLayer[0].Num();
	const bool bForVirtualTextureStreamingBuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::ForVirtualTextureStreamingBuild);

	if (!ensure(Compressor))
	{
		UE_LOG(LogTexture, Warning, TEXT("Missing Compressor required to build texture %s"), *Texture.GetPathName());
		return;
	}

	if (!bHasValidMip0)
	{
		return;
	}

	FTextureStatusMessageContext StatusMessage(
		ComposeTextureBuildText(Texture, TextureData, BuildSettingsPerLayer[0], RequiredMemoryEstimate, bForVirtualTextureStreamingBuild)
		);

	if (bForVirtualTextureStreamingBuild)
	{
		if (DerivedData->VTData == nullptr)
		{
			DerivedData->VTData = new FVirtualTextureBuiltData();
		}

		FVirtualTextureDataBuilder Builder(*DerivedData->VTData, Compressor, ImageWrapper);
		Builder.Build(TextureData, CompositeTextureData, &BuildSettingsPerLayer[0], true);

		DerivedData->SizeX = DerivedData->VTData->Width;
		DerivedData->SizeY = DerivedData->VTData->Height;
		DerivedData->PixelFormat = DerivedData->VTData->LayerTypes[0];
		DerivedData->SetNumSlices(1);

		bool bCompressionValid = true;
		if (CVarVTValidateCompressionOnSave.GetValueOnAnyThread())
		{
			bCompressionValid = DerivedData->VTData->ValidateData(Texture.GetPathName(), true);
		}

		if (ensureMsgf(bCompressionValid, TEXT("Corrupt Virtual Texture compression for %s, can't store to DDC"), *Texture.GetPathName()))
		{
			// Store it in the cache.
			// @todo: This will remove the streaming bulk data, which we immediately reload below!
			// Should ideally avoid this redundant work, but it only happens when we actually have 
			// to build the texture, which should only ever be once.
			this->BytesCached = PutDerivedDataInCache(DerivedData, KeySuffix, Texture.GetPathName(), BuildSettingsPerLayer[0].bCubemap || BuildSettingsPerLayer[0].bVolume || BuildSettingsPerLayer[0].bTextureArray, bReplaceExistingDDC);

			if (DerivedData->VTData->Chunks.Num())
			{
				const bool bInlineMips = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::InlineMips);
				bSucceeded = !bInlineMips || DerivedData->TryInlineMipData(BuildSettingsPerLayer[0].LODBiasWithCinematicMips, &Texture);
				if (!bSucceeded)
				{
					UE_LOG(LogTexture, Display, TEXT("Failed to put and then read back mipmap data from DDC for %s"), *Texture.GetPathName());
				}
			}
			else
			{
				UE_LOG(LogTexture, Warning, TEXT("Failed to build %s derived data for %s"), *BuildSettingsPerLayer[0].TextureFormatName.GetPlainNameString(), *Texture.GetPathName());
			}
		}
	}
	else
	{
		// Only support single Block/Layer here (Blocks and Layers are intended for VT support)
		if (TextureData.Blocks.Num() > 1)
		{
			// This warning can happen if user attempts to import a UDIM without VT enabled
			UE_LOG(LogTexture, Warning, TEXT("Texture %s was imported as UDIM with %d blocks but VirtualTexturing is not enabled, only the first block will be available"),
				*Texture.GetPathName(), TextureData.Blocks.Num());
		}

		// No user-facing way to generated multi-layered textures currently, so this should not occur
		if (TextureData.Layers.Num() > 1)
		{
			UE_LOG(LogTexture, Warning, TEXT("Texture %s has %d layers but VirtualTexturing is not enabled, only the first layer will be available"),
				*Texture.GetPathName(), TextureData.Layers.Num());
		}

		check(DerivedData->Mips.Num() == 0);
		DerivedData->SizeX = 0;
		DerivedData->SizeY = 0;
		DerivedData->PixelFormat = PF_Unknown;
		DerivedData->SetIsCubemap(false);
		DerivedData->VTData = nullptr;

		FOptTexturePlatformData OptData;

		// Compress the texture by calling texture compressor directly.
		TArray<FCompressedImage2D> CompressedMips;
		if (Compressor->BuildTexture(TextureData.Blocks[0].MipsPerLayer[0],
			((bool)Texture.CompositeTexture && CompositeTextureData.Blocks.Num() && CompositeTextureData.Blocks[0].MipsPerLayer.Num()) ? CompositeTextureData.Blocks[0].MipsPerLayer[0] : TArray<FImage>(),
			BuildSettingsPerLayer[0],
			CompressedMips,
			OptData.NumMipsInTail,
			OptData.ExtData))
		{
			check(CompressedMips.Num());

			// Build the derived data.
			const int32 MipCount = CompressedMips.Num();
			for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
			{
				const FCompressedImage2D& CompressedImage = CompressedMips[MipIndex];
				FTexture2DMipMap* NewMip = new FTexture2DMipMap();
				DerivedData->Mips.Add(NewMip);
				NewMip->SizeX = CompressedImage.SizeX;
				NewMip->SizeY = CompressedImage.SizeY;
				NewMip->SizeZ = CompressedImage.SizeZ;
				NewMip->FileRegionType = FFileRegion::SelectType(EPixelFormat(CompressedImage.PixelFormat));
				check(NewMip->SizeZ == 1 || BuildSettingsPerLayer[0].bVolume || BuildSettingsPerLayer[0].bTextureArray); // Only volume & arrays can have SizeZ != 1
				NewMip->BulkData.Lock(LOCK_READ_WRITE);
				check(CompressedImage.RawData.GetTypeSize() == 1);
				void* NewMipData = NewMip->BulkData.Realloc(CompressedImage.RawData.Num());
				FMemory::Memcpy(NewMipData, CompressedImage.RawData.GetData(), CompressedImage.RawData.Num());
				NewMip->BulkData.Unlock();

				if (MipIndex == 0)
				{
					DerivedData->SizeX = CompressedImage.SizeX;
					DerivedData->SizeY = CompressedImage.SizeY;
					DerivedData->PixelFormat = (EPixelFormat)CompressedImage.PixelFormat;
					if (BuildSettingsPerLayer[0].bVolume || BuildSettingsPerLayer[0].bTextureArray)
					{
						DerivedData->SetNumSlices(CompressedImage.SizeZ);
					}
					else if (BuildSettingsPerLayer[0].bCubemap)
					{
						DerivedData->SetNumSlices(6);
					}
					else
					{
						DerivedData->SetNumSlices(1);
					}
					DerivedData->SetIsCubemap(BuildSettingsPerLayer[0].bCubemap);
				}
				else
				{
					check(CompressedImage.PixelFormat == DerivedData->PixelFormat);
				}
			}

			DerivedData->SetOptData(OptData);
				
			// Store it in the cache.
			// @todo: This will remove the streaming bulk data, which we immediately reload below!
			// Should ideally avoid this redundant work, but it only happens when we actually have 
			// to build the texture, which should only ever be once.
			this->BytesCached = PutDerivedDataInCache(DerivedData, KeySuffix, Texture.GetPathName(), BuildSettingsPerLayer[0].bCubemap || (BuildSettingsPerLayer[0].bVolume && !GSupportsVolumeTextureStreaming) || (BuildSettingsPerLayer[0].bTextureArray && !GSupportsTexture2DArrayStreaming), bReplaceExistingDDC);
		}

		if (DerivedData->Mips.Num())
		{
			const bool bInlineMips = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::InlineMips);
			bSucceeded = !bInlineMips || DerivedData->TryInlineMipData(BuildSettingsPerLayer[0].LODBiasWithCinematicMips, &Texture);
			if (!bSucceeded)
			{
				UE_LOG(LogTexture, Display, TEXT("Failed to put and then read back mipmap data from DDC for %s"), *Texture.GetPathName());
			}
		}
		else
		{
			UE_LOG(LogTexture, Warning, TEXT("Failed to build %s derived data for %s"), *BuildSettingsPerLayer[0].TextureFormatName.GetPlainNameString(), *Texture.GetPathName());
		}
	}
}

FTextureCacheDerivedDataWorker::FTextureCacheDerivedDataWorker(
	ITextureCompressorModule* InCompressor,
	FTexturePlatformData* InDerivedData,
	UTexture* InTexture,
	const FTextureBuildSettings* InSettingsPerLayer,
	ETextureCacheFlags InCacheFlags
	)
	: Compressor(InCompressor)
	, ImageWrapper(nullptr)
	, DerivedData(InDerivedData)
	, Texture(*InTexture)
	, CacheFlags(InCacheFlags)
	, RequiredMemoryEstimate(InTexture->GetBuildRequiredMemory())
	, bSucceeded(false)
{
	check(DerivedData);

	BuildSettingsPerLayer.SetNum(InTexture->Source.GetNumLayers());
	for (int32 LayerIndex = 0; LayerIndex < BuildSettingsPerLayer.Num(); ++LayerIndex)
	{
		BuildSettingsPerLayer[LayerIndex] = InSettingsPerLayer[LayerIndex];
	}

	// At this point, the texture *MUST* have a valid GUID.
	if (!Texture.Source.GetId().IsValid())
	{
		UE_LOG(LogTexture, Warning, TEXT("Building texture with an invalid GUID: %s"), *Texture.GetPathName());
		Texture.Source.ForceGenerateGuid();
	}
	check(Texture.Source.GetId().IsValid());

	FString LocalDerivedDataKeySuffix;
	FString LocalDerivedDataKey;
	GetTextureDerivedDataKeySuffix(Texture, BuildSettingsPerLayer.GetData(), LocalDerivedDataKeySuffix);
	GetTextureDerivedDataKeyFromSuffix(LocalDerivedDataKeySuffix, LocalDerivedDataKey);
	DerivedData->ComparisonDerivedDataKey.Emplace<FString>(LocalDerivedDataKey);

	// Dump any existing mips.
	DerivedData->Mips.Empty();
	if (DerivedData->VTData)
	{
		delete DerivedData->VTData;
		DerivedData->VTData = nullptr;
	}
	UTexture::GetPixelFormatEnum();
		
	const bool bAllowAsyncBuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::AllowAsyncBuild);
	const bool bAllowAsyncLoading = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::AllowAsyncLoading);
	const bool bForVirtualTextureStreamingBuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::ForVirtualTextureStreamingBuild);

	// FVirtualTextureDataBuilder always wants to load ImageWrapper module
	// This is not strictly necessary, used only for debug output, but seems simpler to just always load this here, doesn't seem like it should be too expensive
	if (bAllowAsyncLoading || bForVirtualTextureStreamingBuild)
	{
		ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>( FName("ImageWrapper") );
	}

	TextureData.Init(Texture, BuildSettingsPerLayer.GetData(), bAllowAsyncLoading);
	if (Texture.CompositeTexture && Texture.CompositeTextureMode != CTM_Disabled)
	{
		bool bMatchingBlocks = Texture.CompositeTexture->Source.GetNumBlocks() == Texture.Source.GetNumBlocks();
		bool bMatchingAspectRatio = true;
		bool bOnlyPowerOfTwoSize = true;
		if (bMatchingBlocks)
		{
			for (int32 BlockIdx = 0; BlockIdx < Texture.Source.GetNumBlocks(); ++BlockIdx)
			{
				FTextureSourceBlock TextureBlock;
				Texture.Source.GetBlock(BlockIdx, TextureBlock);
				FTextureSourceBlock CompositeTextureBlock;
				Texture.CompositeTexture->Source.GetBlock(BlockIdx, CompositeTextureBlock);

				bMatchingBlocks = bMatchingBlocks && TextureBlock.BlockX == CompositeTextureBlock.BlockX && TextureBlock.BlockY == CompositeTextureBlock.BlockY;
				bMatchingAspectRatio = bMatchingAspectRatio && TextureBlock.SizeX * CompositeTextureBlock.SizeY == TextureBlock.SizeY * CompositeTextureBlock.SizeX;
				bOnlyPowerOfTwoSize = bOnlyPowerOfTwoSize && FMath::IsPowerOfTwo(TextureBlock.SizeX) && FMath::IsPowerOfTwo(TextureBlock.SizeY);
			}
		}

		if (!bMatchingBlocks)
		{
			// Only report the warning for textures with a single block
			// In the future, we should support composite textures if matching blocks are in a different order
			// Once that's working, then this warning should be reported in all cases
			if (Texture.Source.GetNumBlocks() == 1)
			{
				UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Composite texture resolution/UDIMs do not match. Composite texture will be ignored"), *Texture.GetPathName());
			}
		}
		else if (!bOnlyPowerOfTwoSize)
		{
			UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Some blocks (UDIMs) have a non power of two size. Composite texture will be ignored"), *Texture.GetPathName());
		}
		else if (!bMatchingAspectRatio)
		{
			UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Some blocks (UDIMs) have mismatched aspect ratio. Composite texture will be ignored"), *Texture.GetPathName());
		}

		if (bMatchingBlocks && bMatchingAspectRatio && bOnlyPowerOfTwoSize)
		{
			CompositeTextureData.Init(*Texture.CompositeTexture, BuildSettingsPerLayer.GetData(), bAllowAsyncLoading);
		}
	}
}

void FTextureCacheDerivedDataWorker::DoWork()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureCacheDerivedDataWorker::DoWork);

	const bool bForceRebuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::ForceRebuild);
	const bool bAllowAsyncBuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::AllowAsyncBuild);
	const bool bAllowAsyncLoading = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::AllowAsyncLoading);
	const bool bForVirtualTextureStreamingBuild = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::ForVirtualTextureStreamingBuild);
	const bool bValidateVirtualTextureCompression = CVarVTValidateCompressionOnLoad.GetValueOnAnyThread() != 0;
	bool bInvalidVirtualTextureCompression = false;

	TArray<uint8> RawDerivedData;

	FString LocalDerivedDataKeySuffix;
	FString LocalDerivedDataKey;
	GetTextureDerivedDataKeySuffix(Texture, BuildSettingsPerLayer.GetData(), LocalDerivedDataKeySuffix);
	GetTextureDerivedDataKeyFromSuffix(LocalDerivedDataKeySuffix, LocalDerivedDataKey);
	if (!bForceRebuild)
	{
		// First try to load a texture generated for the shipping build from the cache.
		// FTexturePlatformData::ShippingDerivedDataKey is set when we are running a build in the Editor.
		// This allows to preview how the texture will look in the final build and avoid rebuilding texture locally using fast cooking.
		if (BuildSettingsPerLayer[0].FastTextureEncode == ETextureFastEncode::TryOffEncodeFast)
		{
			const int32 NumLayers = Texture.Source.GetNumLayers();
			TArray<FTextureBuildSettings> ShippingBuildSettingsPerLayer;
			ShippingBuildSettingsPerLayer.SetNum(NumLayers);
			for (int32 LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
			{
				ShippingBuildSettingsPerLayer[LayerIndex] = BuildSettingsPerLayer[LayerIndex];
				ShippingBuildSettingsPerLayer[LayerIndex].FastTextureEncode = ETextureFastEncode::Off;
			}
			FString ShippingDerivedDataKeySuffix;
			FString ShippingDerivedDataKey;
			GetTextureDerivedDataKeySuffix(Texture, ShippingBuildSettingsPerLayer.GetData(), ShippingDerivedDataKeySuffix);
			GetTextureDerivedDataKeyFromSuffix(ShippingDerivedDataKeySuffix, ShippingDerivedDataKey);

			bLoadedFromDDC = GetDerivedDataCacheRef().GetSynchronous(*ShippingDerivedDataKey, RawDerivedData, Texture.GetPathName());
			if (bLoadedFromDDC)
			{
				LocalDerivedDataKeySuffix = ShippingDerivedDataKeySuffix;
				LocalDerivedDataKey = ShippingDerivedDataKey;
			}
		}

		if (!bLoadedFromDDC)
		{
			bLoadedFromDDC = GetDerivedDataCacheRef().GetSynchronous(*LocalDerivedDataKey, RawDerivedData, Texture.GetPathName());
		}
	}
	KeySuffix = LocalDerivedDataKeySuffix;
	DerivedData->DerivedDataKey.Emplace<FString>(LocalDerivedDataKey);

	if (bLoadedFromDDC)
	{
		const bool bInlineMips = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::InlineMips);
		const bool bForDDC = EnumHasAnyFlags(CacheFlags, ETextureCacheFlags::ForDDCBuild);

		BytesCached = RawDerivedData.Num();
		FMemoryReader Ar(RawDerivedData, /*bIsPersistent=*/ true);
		DerivedData->Serialize(Ar, NULL);
		bSucceeded = true;
		// Load any streaming (not inline) mips that are necessary for our platform.
		if (bForDDC)
		{
			bSucceeded = DerivedData->TryLoadMips(0, nullptr, &Texture);

			if (bForVirtualTextureStreamingBuild)
			{
				if (DerivedData->VTData != nullptr &&
					DerivedData->VTData->IsInitialized())
				{
					TArray<FString, TInlineAllocator<16>> ChunkKeys;
					for (const FVirtualTextureDataChunk& Chunk : DerivedData->VTData->Chunks)
					{
						if (!Chunk.DerivedDataKey.IsEmpty())
						{
							ChunkKeys.Add(Chunk.DerivedDataKey);
						}
					}
					GetDerivedDataCacheRef().TryToPrefetch(ChunkKeys, Texture.GetPathName());
				}
			}

			if (!bSucceeded)
			{
				UE_LOG(LogTexture, Display, TEXT("Texture %s is missing mips. The texture will be rebuilt."), *Texture.GetFullName());
			}
		}
		else if (bInlineMips)
		{
			bSucceeded = DerivedData->TryInlineMipData(BuildSettingsPerLayer[0].LODBiasWithCinematicMips, &Texture);

			if (!bSucceeded)
			{
				UE_LOG(LogTexture, Display, TEXT("Texture %s is missing inline mips. The texture will be rebuilt."), *Texture.GetFullName());
			}
		}
		else
		{
			if (bForVirtualTextureStreamingBuild)
			{
				bSucceeded =	DerivedData->VTData != nullptr &&
								DerivedData->VTData->IsInitialized() &&
								DerivedData->AreDerivedVTChunksAvailable(Texture.GetPathName());

				if (!bSucceeded)
				{
					UE_LOG(LogTexture, Display, TEXT("Texture %s is missing VT Chunks. The texture will be rebuilt."), *Texture.GetFullName());
				}
			}
			else
			{
				bSucceeded = DerivedData->AreDerivedMipsAvailable(Texture.GetPathName());
				if (!bSucceeded)
				{
					UE_LOG(LogTexture, Display, TEXT("Texture %s is missing derived mips. The texture will be rebuilt."), *Texture.GetFullName());
				}

				if (bSucceeded && BuildSettingsPerLayer.Num() > 0)
				{
					// Code inspired by the texture compressor module as a hot fix for the bad data that might have been push into the ddc in 4.23 or 4.24 
					const bool bLongLatCubemap = DerivedData->IsCubemap() && DerivedData->GetNumSlices() == 1;
					int32 MaximumNumberOfMipMaps = TNumericLimits<int32>::Max();
					if (bLongLatCubemap)
					{
						MaximumNumberOfMipMaps = FMath::CeilLogTwo(FMath::Clamp<uint32>(uint32(1 << FMath::FloorLog2(DerivedData->SizeX / 2)), uint32(32), BuildSettingsPerLayer[0].MaxTextureResolution)) + 1;
					}
					else
					{
						MaximumNumberOfMipMaps = FMath::CeilLogTwo(FMath::Max3(DerivedData->SizeX, DerivedData->SizeY, BuildSettingsPerLayer[0].bVolume ? DerivedData->GetNumSlices() : 1)) + 1;
					}

					bSucceeded = DerivedData->Mips.Num() <= MaximumNumberOfMipMaps;

					if (!bSucceeded)
					{
						UE_LOG(LogTexture, Warning, TEXT("The data retrieved from the derived data cache for the texture %s was invalid. ")
							TEXT("The cached data has %d mips when a maximum of %d are expected. The texture will be rebuilt."),
							*Texture.GetFullName(), DerivedData->Mips.Num(), MaximumNumberOfMipMaps);
					}
				}
			}
		}

		if (bSucceeded && bForVirtualTextureStreamingBuild && CVarVTValidateCompressionOnLoad.GetValueOnAnyThread())
		{
			check(DerivedData->VTData);
			bSucceeded = DerivedData->VTData->ValidateData(Texture.GetPathName(), false);
			if (!bSucceeded)
			{
				UE_LOG(LogTexture, Display, TEXT("Texture %s has invalid cached VT data. The texture will be rebuilt."), *Texture.GetFullName());
				bInvalidVirtualTextureCompression = true;
			}
		}
		
		// Reset everything derived data so that we can do a clean load from the source data
		if (!bSucceeded)
		{
			DerivedData->Mips.Empty();
			if (DerivedData->VTData)
			{
				delete DerivedData->VTData;
				DerivedData->VTData = nullptr;
			}
			
			bLoadedFromDDC = false;
		}
	}
	
	if (!bSucceeded && bAllowAsyncBuild)
	{
		bool bHasTextureSourceMips = false;
		if (TextureData.IsValid() && Texture.Source.IsBulkDataLoaded())
		{
			TextureData.GetSourceMips(Texture.Source, ImageWrapper);
			bHasTextureSourceMips = true;
		}

		bool bHasCompositeTextureSourceMips = false;
		if (CompositeTextureData.IsValid() && Texture.CompositeTexture && Texture.CompositeTexture->Source.IsBulkDataLoaded())
		{
			CompositeTextureData.GetSourceMips(Texture.CompositeTexture->Source, ImageWrapper);
			bHasCompositeTextureSourceMips = true;
		}

		if (bAllowAsyncLoading && !bHasTextureSourceMips)
		{
			TextureData.GetAsyncSourceMips(ImageWrapper);
			TextureData.AsyncSource.RemoveBulkData();
		}

		if (bAllowAsyncLoading && !bHasCompositeTextureSourceMips)
		{
			CompositeTextureData.GetAsyncSourceMips(ImageWrapper);
			CompositeTextureData.AsyncSource.RemoveBulkData();
		}

		if (TextureData.Blocks.Num() && TextureData.Blocks[0].MipsPerLayer.Num() && TextureData.Blocks[0].MipsPerLayer[0].Num() && 
			(!CompositeTextureData.IsValid() || (CompositeTextureData.Blocks.Num() && CompositeTextureData.Blocks[0].MipsPerLayer.Num() && CompositeTextureData.Blocks[0].MipsPerLayer[0].Num())))
		{
			// Replace any existing DDC data, if corrupt compression was detected
			const bool bReplaceExistingDDC = bInvalidVirtualTextureCompression;
			BuildTexture(bReplaceExistingDDC);
			if (bInvalidVirtualTextureCompression && DerivedData->VTData)
			{
				// If we loaded data that turned out to be corrupt, flag it here so we can also recreate the VT data cached to local /DerivedDataCache/VT/ directory
				for (FVirtualTextureDataChunk& Chunk : DerivedData->VTData->Chunks)
				{
					Chunk.bCorruptDataLoadedFromDDC = true;
				}

			}

			bSucceeded = true;
		}
		else
		{
			bSucceeded = false;
		}
	}

	if (bSucceeded)
	{
		TextureData.ReleaseMemory();
		CompositeTextureData.ReleaseMemory();

		// Populate the VT DDC Cache now if we're asynchronously loading to avoid too many high prio/synchronous request on the render thread
		if (!IsInGameThread() && DerivedData->VTData && !DerivedData->VTData->Chunks.Last().DerivedDataKey.IsEmpty())
		{
			GetVirtualTextureChunkDDCCache()->MakeChunkAvailable_Concurrent(&DerivedData->VTData->Chunks.Last());
		}
	}
}

void FTextureCacheDerivedDataWorker::Finalize()
{
	// if we couldn't get from the DDC or didn't build synchronously, then we have to build now. 
	// This is a super edge case that should rarely happen.
	if (!bSucceeded)
	{
		TextureData.GetSourceMips(Texture.Source, ImageWrapper);
		if (Texture.CompositeTexture)
		{
			CompositeTextureData.GetSourceMips(Texture.CompositeTexture->Source, ImageWrapper);
		}
		BuildTexture();
	}
		
	if (bSucceeded && BuildSettingsPerLayer[0].bVirtualStreamable) // Texture.VirtualTextureStreaming is more a hint that might be overruled by the buildsettings
	{
		check((DerivedData->VTData != nullptr) == Texture.VirtualTextureStreaming); 
	}
}

class FTextureBuildTask final : public FTextureAsyncCacheDerivedDataTask
{
public:
	FTextureBuildTask(
		UTexture& Texture,
		FStringView FunctionName,
		FTexturePlatformData& InDerivedData,
		const FTextureBuildSettings& Settings,
		EQueuedWorkPriority InPriority,
		ETextureCacheFlags Flags)
		: DerivedData(InDerivedData)
		, Priority(InPriority)
		, bCacheHit(false)
		, bInlineMips(EnumHasAnyFlags(Flags, ETextureCacheFlags::InlineMips))
		, FirstMipToLoad(Settings.LODBiasWithCinematicMips)
		, InputResolver(Texture)
	{
		using namespace UE::DerivedData;

		static bool bLoadedModules = LoadModules();

		TStringBuilder<256> TexturePath;
		Texture.GetPathName(nullptr, TexturePath);

		IBuild& Build = GetBuild();
		IBuildInputResolver* GlobalResolver = GetGlobalBuildInputResolver();
		BuildSession = Build.CreateSession(TexturePath, GlobalResolver ? GlobalResolver : &InputResolver);

		EPriority OwnerPriority = EnumHasAnyFlags(Flags, ETextureCacheFlags::Async) ? ConvertPriority(Priority) : UE::DerivedData::EPriority::Blocking;
		Owner.Emplace(OwnerPriority);

		bool bUseCompositeTexture;
		if (!IsTextureValidForBuilding(Texture, Flags, bUseCompositeTexture))
		{
			return;
		}
		
		if (IsInGameThread() && OwnerPriority == EPriority::Blocking)
		{
			StatusMessage.Emplace(ComposeTextureBuildText(Texture, Settings, Texture.GetBuildRequiredMemory(), EnumHasAnyFlags(Flags, ETextureCacheFlags::ForVirtualTextureStreamingBuild)));
		}

		FBuildDefinition Definition = CreateDefinition(Build, Texture, TexturePath, FunctionName, Settings, bUseCompositeTexture);
		DerivedData.ComparisonDerivedDataKey.Emplace<FTexturePlatformData::FStructuredDerivedDataKey>(GetKey(Definition, Texture, bUseCompositeTexture));

		if (!EnumHasAnyFlags(Flags, ETextureCacheFlags::ForceRebuild) && Settings.FastTextureEncode == ETextureFastEncode::TryOffEncodeFast)
		{
			FTextureBuildSettings ShippingSettings = Settings;
			ShippingSettings.FastTextureEncode = ETextureFastEncode::Off;
			FBuildDefinition ShippingDefinition = CreateDefinition(Build, Texture, TexturePath, FunctionName, ShippingSettings, bUseCompositeTexture);
			BuildSession.Get().Build(ShippingDefinition, EBuildPolicy::Cache, *Owner,
				[this, Definition = MoveTemp(Definition), Flags](FBuildCompleteParams&& Params)
				{
					switch (Params.Status)
					{
					default:
					case EStatus::Ok:
						return EndBuild(Params.CacheKey, MoveTemp(Params.Output), Params.BuildStatus);
					case EStatus::Error:
						return BeginBuild(Definition, Flags);
					}
				});
		}
		else
		{
			BeginBuild(Definition, Flags);
		}
	}

	static UE::DerivedData::FBuildDefinition CreateDefinition(
		UE::DerivedData::IBuild& Build,
		UTexture& Texture,
		FStringView TexturePath,
		FStringView FunctionName,
		const FTextureBuildSettings& Settings,
		const bool bUseCompositeTexture)
	{
		UE::DerivedData::FBuildDefinitionBuilder DefinitionBuilder = Build.CreateDefinition(TexturePath, FunctionName);
		DefinitionBuilder.AddConstant(TEXT("Settings"_SV),
			SaveTextureBuildSettings(Texture, Settings, 0, NUM_INLINE_DERIVED_MIPS));
		DefinitionBuilder.AddInputBulkData(TEXT("Source"_SV), Texture.Source.GetPersistentId());
		if (Texture.CompositeTexture && bUseCompositeTexture)
		{
			DefinitionBuilder.AddInputBulkData(TEXT("CompositeSource"_SV), Texture.CompositeTexture->Source.GetPersistentId());
		}
		return DefinitionBuilder.Build();
	}

	void BeginBuild(const UE::DerivedData::FBuildDefinition& Definition, ETextureCacheFlags Flags)
	{
		using namespace UE::DerivedData;
		EBuildPolicy BuildPolicy = EBuildPolicy::Default;
		if (EnumHasAnyFlags(Flags, ETextureCacheFlags::ForceRebuild))
		{
			BuildPolicy &= ~EBuildPolicy::CacheQuery;
		}
		BuildSession.Get().Build(Definition, BuildPolicy, *Owner,
			[this](FBuildCompleteParams&& Params)
			{
				EndBuild(Params.CacheKey, MoveTemp(Params.Output), Params.BuildStatus);
			});
	}

	void EndBuild(const UE::DerivedData::FCacheKey& CacheKey, UE::DerivedData::FBuildOutput&& Output, UE::DerivedData::EBuildStatus Status)
	{
		using namespace UE::DerivedData;
		DerivedData.DerivedDataKey.Emplace<FCacheKeyProxy>(CacheKey);
		bCacheHit = EnumHasAnyFlags(Status, EBuildStatus::CacheQueryHit);
		BuildOutputSize = Algo::TransformAccumulate(Output.GetPayloads(),
			[](const FPayload& Payload) { return Payload.GetData().GetRawSize(); }, uint64(0));
		WriteDerivedData(MoveTemp(Output));
		StatusMessage.Reset();
	}

	void Finalize(bool& bOutFoundInCache, uint64& OutProcessedByteCount) final
	{
		bOutFoundInCache = bCacheHit;
		OutProcessedByteCount = BuildOutputSize;
	}

	EQueuedWorkPriority GetPriority() const final
	{
		return Priority;
	}

	bool SetPriority(EQueuedWorkPriority QueuedWorkPriority) final
	{
		Priority = QueuedWorkPriority;
		Owner->SetPriority(ConvertPriority(QueuedWorkPriority));
		return true;
	}

	bool Cancel() final
	{
		Owner->Cancel();
		return true;
	}

	void Wait() final
	{
		Owner->Wait();
	}

	bool WaitWithTimeout(float TimeLimitSeconds) final
	{
		const double TimeLimit = FPlatformTime::Seconds() + TimeLimitSeconds;
		if (Poll())
		{
			return true;
		}
		do
		{
			FPlatformProcess::Sleep(0.005);
			if (Poll())
			{
				return true;
			}
		}
		while (FPlatformTime::Seconds() < TimeLimit);
		return false;
	}

	bool Poll() const final
	{
		return Owner->Poll();
	}

	static bool IsTextureValidForBuilding(UTexture& Texture, ETextureCacheFlags Flags, bool& bOutUseCompositeTexture)
	{
		const int32 NumBlocks = Texture.Source.GetNumBlocks();
		const int32 NumLayers = Texture.Source.GetNumLayers();
		if (NumBlocks < 1 || NumLayers < 1)
		{
			UE_LOG(LogTexture, Error, TEXT("Texture has no source data: %s"), *Texture.GetPathName());
			return false;
		}

		for (int LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
		{
			switch (Texture.Source.GetFormat(LayerIndex))
			{
			case TSF_G8:
			case TSF_G16:
			case TSF_BGRA8:
			case TSF_BGRE8:
			case TSF_RGBA16:
			case TSF_RGBA16F:
				break;
			default:
				UE_LOG(LogTexture, Fatal, TEXT("Texture %s has source art in an invalid format."), *Texture.GetPathName());
				return false;
			}
		}

		const bool bCompositeTextureViable = Texture.CompositeTexture && Texture.CompositeTextureMode != CTM_Disabled;
		bool bMatchingBlocks = bCompositeTextureViable && (Texture.CompositeTexture->Source.GetNumBlocks() == Texture.Source.GetNumBlocks());
		bool bMatchingAspectRatio = bCompositeTextureViable;
		bool bOnlyPowerOfTwoSize = bCompositeTextureViable;

		int32 BlockSizeX = 0;
		int32 BlockSizeY = 0;
		TArray<FIntPoint> BlockSizes;
		BlockSizes.Reserve(NumBlocks);
		for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
		{
			FTextureSourceBlock SourceBlock;
			Texture.Source.GetBlock(BlockIndex, SourceBlock);
			if (SourceBlock.NumMips > 0 && SourceBlock.NumSlices > 0)
			{
				BlockSizes.Emplace(SourceBlock.SizeX, SourceBlock.SizeY);
				BlockSizeX = FMath::Max(BlockSizeX, SourceBlock.SizeX);
				BlockSizeY = FMath::Max(BlockSizeY, SourceBlock.SizeY);
			}

			if (bCompositeTextureViable)
			{
				FTextureSourceBlock CompositeTextureBlock;
				Texture.CompositeTexture->Source.GetBlock(BlockIndex, CompositeTextureBlock);

				bMatchingBlocks = bMatchingBlocks && SourceBlock.BlockX == CompositeTextureBlock.BlockX && SourceBlock.BlockY == CompositeTextureBlock.BlockY;
				bMatchingAspectRatio = bMatchingAspectRatio && SourceBlock.SizeX * CompositeTextureBlock.SizeY == SourceBlock.SizeY * CompositeTextureBlock.SizeX;
				bOnlyPowerOfTwoSize = bOnlyPowerOfTwoSize && FMath::IsPowerOfTwo(SourceBlock.SizeX) && FMath::IsPowerOfTwo(SourceBlock.SizeY);
			}
		}

		for (int32 BlockIndex = 0; BlockIndex < BlockSizes.Num(); ++BlockIndex)
		{
			const int32 MipBiasX = FMath::CeilLogTwo(BlockSizeX / BlockSizes[BlockIndex].X);
			const int32 MipBiasY = FMath::CeilLogTwo(BlockSizeY / BlockSizes[BlockIndex].Y);
			if (MipBiasX != MipBiasY)
			{
				UE_LOG(LogTexture, Error, TEXT("Texture %s has blocks with mismatched aspect ratios"), *Texture.GetPathName());
				return false;
			}
		}

		if (bCompositeTextureViable)
		{
			if (!bMatchingBlocks)
			{
				UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Composite texture resolution/UDIMs do not match. Composite texture will be ignored"), *Texture.GetPathName());
			}
			else if (!bOnlyPowerOfTwoSize)
			{
				UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Some blocks (UDIMs) have a non power of two size. Composite texture will be ignored"), *Texture.GetPathName());
			}
			else if (!bMatchingAspectRatio)
			{
				UE_LOG(LogTexture, Warning, TEXT("Issue while building %s : Some blocks (UDIMs) have mismatched aspect ratio. Composite texture will be ignored"), *Texture.GetPathName());
			}
		}

		bOutUseCompositeTexture = bMatchingBlocks && bMatchingAspectRatio && bOnlyPowerOfTwoSize;

		// TODO: Add validation equivalent to that found in FTextureCacheDerivedDataWorker::BuildTexture for virtual textures
		//		 if virtual texture support is added for this code path.
		if (!EnumHasAnyFlags(Flags, ETextureCacheFlags::ForVirtualTextureStreamingBuild))
		{
			// Only support single Block/Layer here (Blocks and Layers are intended for VT support)
			if (NumBlocks > 1)
			{
				// This warning can happen if user attempts to import a UDIM without VT enabled
				UE_LOG(LogTexture, Warning, TEXT("Texture %s was imported as UDIM with %d blocks but VirtualTexturing is not enabled, only the first block will be available"),
					*Texture.GetPathName(), NumBlocks);
			}

			// No user-facing way to generated multi-layered textures currently, so this should not occur
			if (NumLayers > 1)
			{
				UE_LOG(LogTexture, Warning, TEXT("Texture %s has %d layers but VirtualTexturing is not enabled, only the first layer will be available"),
					*Texture.GetPathName(), NumLayers);
			}
		}

		return true;
	}

	static FTexturePlatformData::FStructuredDerivedDataKey GetKey(const UE::DerivedData::FBuildDefinition& BuildDefinition, const UTexture& Texture, bool bUseCompositeTexture)
	{
		FTexturePlatformData::FStructuredDerivedDataKey Key;
		Key.BuildDefinitionKey = BuildDefinition.GetKey().Hash;
		Key.SourceGuid = Texture.Source.GetId();
		if (bUseCompositeTexture && Texture.CompositeTexture)
		{
			Key.CompositeSourceGuid = Texture.CompositeTexture->Source.GetId();
		}
		return Key;
	}

private:

	static bool DeserializeTextureFromPayloads(FTexturePlatformData& DerivedData, const UE::DerivedData::FBuildOutput& Output, int32 FirstMipToLoad, bool bInlineMips)
	{
		using namespace UE::DerivedData;
		const FPayload& Payload = Output.GetPayload(FPayloadId::FromName("Description"_ASV));
		if (!Payload)
		{
			UE_LOG(LogTexture, Error, TEXT("Missing texture description for build of '%s' by %s."),
				*WriteToString<128>(Output.GetName()), *WriteToString<32>(Output.GetFunction()));
			return false;
		}

		FCbObject TextureDescription(Payload.GetData().Decompress());

		FCbFieldViewIterator SizeIt = TextureDescription["Size"_ASV].AsArrayView().CreateViewIterator();
		DerivedData.SizeX = SizeIt++->AsInt32();
		DerivedData.SizeY = SizeIt++->AsInt32();
		int32 NumSlices = SizeIt++->AsInt32();

		UEnum* PixelFormatEnum = UTexture::GetPixelFormatEnum();
		FUtf8StringView PixelFormatStringView = TextureDescription["PixelFormat"_ASV].AsString();
		FName PixelFormatName(PixelFormatStringView.Len(), PixelFormatStringView.GetData());
		DerivedData.PixelFormat = (EPixelFormat)PixelFormatEnum->GetValueByName(PixelFormatName);

		const bool bCubeMap = TextureDescription["bCubeMap"_ASV].AsBool();
		DerivedData.OptData.ExtData = TextureDescription["ExtData"_ASV].AsUInt32();
		DerivedData.OptData.NumMipsInTail = TextureDescription["NumMipsInTail"_ASV].AsUInt32();
		const bool bHasOptData = (DerivedData.OptData.NumMipsInTail != 0) || (DerivedData.OptData.ExtData != 0);
		static constexpr uint32 BitMask_CubeMap = 1u << 31u;
		static constexpr uint32 BitMask_HasOptData = 1u << 30u;
		static constexpr uint32 BitMask_NumSlices = BitMask_HasOptData - 1u;
		DerivedData.PackedData = (NumSlices & BitMask_NumSlices) | (bCubeMap ? BitMask_CubeMap : 0) | (bHasOptData ? BitMask_HasOptData : 0);

		int32 NumMips = TextureDescription["NumMips"_ASV].AsInt32();
		int32 NumStreamingMips = TextureDescription["NumStreamingMips"_ASV].AsInt32();

		FCbArrayView MipArrayView = TextureDescription["Mips"_ASV].AsArrayView();
		if (NumMips != MipArrayView.Num())
		{
			UE_LOG(LogTexture, Error, TEXT("Mismatched mip quantity (%d and %d) for build of '%s' by %s."),
				NumMips, MipArrayView.Num(), *WriteToString<128>(Output.GetName()), *WriteToString<32>(Output.GetFunction()));
			return false;
		}
		check(NumMips >= (int32)DerivedData.OptData.NumMipsInTail);
		check(NumMips >= NumStreamingMips);

		FSharedBuffer MipTailData;
		if (NumMips > NumStreamingMips)
		{
			const FPayload& MipTailPayload = Output.GetPayload(FPayloadId::FromName("MipTail"_ASV));
			if (!MipTailPayload)
			{
				UE_LOG(LogTexture, Error, TEXT("Missing texture mip tail for build of '%s' by %s."),
					*WriteToString<128>(Output.GetName()), *WriteToString<32>(Output.GetFunction()));
				return false;
			}
			MipTailData = MipTailPayload.GetData().Decompress();
		}

		int32 MipIndex = 0;
		DerivedData.Mips.Empty(NumMips);
		for (FCbFieldView MipFieldView : MipArrayView)
		{
			FCbObjectView MipObjectView = MipFieldView.AsObjectView();
			FTexture2DMipMap* NewMip = new FTexture2DMipMap();

			FCbFieldViewIterator MipSizeIt = MipObjectView["Size"_ASV].AsArrayView().CreateViewIterator();
			NewMip->SizeX = MipSizeIt++->AsInt32();
			NewMip->SizeY = MipSizeIt++->AsInt32();
			NewMip->SizeZ = MipSizeIt++->AsInt32();
			NewMip->FileRegionType = static_cast<EFileRegionType>(MipObjectView["FileRegion"_ASV].AsInt32());
			
			if (MipIndex >= NumStreamingMips)
			{
				uint64 MipSize = MipObjectView["NumBytes"_ASV].AsUInt64();
				FMemoryView MipView = MipTailData.GetView().Mid(MipObjectView["PayloadOffset"_ASV].AsUInt64(), MipSize);

				NewMip->BulkData.Lock(LOCK_READ_WRITE);
				void* MipAllocData = NewMip->BulkData.Realloc(int64(MipSize));
				MakeMemoryView(MipAllocData, MipSize).CopyFrom(MipView);
				NewMip->BulkData.Unlock();
				NewMip->SetPagedToDerivedData(false);
			}
			else if (bInlineMips && (MipIndex >= FirstMipToLoad))
			{
				const FPayload& StreamingMipPayload = Output.GetPayload(FPayloadId::FromName(WriteToString<8>(TEXT("Mip"), MipIndex)));
				if (!StreamingMipPayload)
				{
					UE_LOG(LogTexture, Error, TEXT("Missing texture streaming mip '%s' for build of '%s' by %s."),
						*WriteToString<8>(TEXT("Mip"), MipIndex), *WriteToString<128>(Output.GetName()), *WriteToString<32>(Output.GetFunction()));
					return false;
				}
				FSharedBuffer StreamingMipData = StreamingMipPayload.GetData().Decompress();
				uint64 MipSize = StreamingMipData.GetSize();

				NewMip->BulkData.Lock(LOCK_READ_WRITE);
				void* MipAllocData = NewMip->BulkData.Realloc(int64(MipSize));
				MakeMemoryView(MipAllocData, MipSize).CopyFrom(StreamingMipData.GetView());
				NewMip->BulkData.Unlock();
				NewMip->SetPagedToDerivedData(false);
			}
			else
			{
				NewMip->SetPagedToDerivedData(true);
			}

			DerivedData.Mips.Add(NewMip);
			++MipIndex;
		}

		return true;
	}

	void WriteDerivedData(UE::DerivedData::FBuildOutput&& Output)
	{
		using namespace UE::DerivedData;

		Output.IterateDiagnostics([](const FBuildDiagnostic& Diagnostic)
		{
			if (Diagnostic.Level == EBuildDiagnosticLevel::Error)
			{
				UE_LOG(LogTexture, Warning, TEXT("[Build Error] %.*s: %.*s"),
					Diagnostic.Category.Len(), Diagnostic.Category.GetData(),
					Diagnostic.Message.Len(), Diagnostic.Message.GetData());
			}
			else
			{
				UE_LOG(LogTexture, Warning, TEXT("[Build Warning] %.*s: %.*s"),
					Diagnostic.Category.Len(), Diagnostic.Category.GetData(),
					Diagnostic.Message.Len(), Diagnostic.Message.GetData());
			}
		});

		if (Output.HasError())
		{
			UE_LOG(LogTexture, Warning, TEXT("Failed to build derived data for build of '%s' by %s."),
				*WriteToString<128>(Output.GetName()), *WriteToString<32>(Output.GetFunction()));
			return;
		}

		DeserializeTextureFromPayloads(DerivedData, Output, FirstMipToLoad, bInlineMips);
	}

	static UE::DerivedData::EPriority ConvertPriority(EQueuedWorkPriority SourcePriority)
	{
		using namespace UE::DerivedData;
		switch (SourcePriority)
		{
		case EQueuedWorkPriority::Lowest:  return EPriority::Lowest;
		case EQueuedWorkPriority::Low:     return EPriority::Low;
		case EQueuedWorkPriority::Normal:  return EPriority::Normal;
		case EQueuedWorkPriority::High:    return EPriority::High;
		case EQueuedWorkPriority::Highest: return EPriority::Highest;
		default:                           return EPriority::Normal;
		}
	}

	static EQueuedWorkPriority ConvertPriority(UE::DerivedData::EPriority SourcePriority)
	{
		using namespace UE::DerivedData;
		switch (SourcePriority)
		{
		case EPriority::Lowest:   return EQueuedWorkPriority::Lowest;
		case EPriority::Low:      return EQueuedWorkPriority::Low;
		case EPriority::Normal:   return EQueuedWorkPriority::Normal;
		case EPriority::High:     return EQueuedWorkPriority::High;
		case EPriority::Highest:  return EQueuedWorkPriority::Highest;
		case EPriority::Blocking: return EQueuedWorkPriority::Highest;
		default:                  return EQueuedWorkPriority::Normal;
		}
	}

	static bool LoadModules()
	{
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		FModuleManager::LoadModuleChecked<ITextureCompressorModule>(TEXTURE_COMPRESSOR_MODULENAME);
		return true;
	}

	FTexturePlatformData& DerivedData;
	TOptional<UE::DerivedData::FRequestOwner> Owner;
	UE::DerivedData::FOptionalBuildSession BuildSession;
	EQueuedWorkPriority Priority;
	bool bCacheHit;
	bool bInlineMips;
	int32 FirstMipToLoad;
	uint64 BuildOutputSize = 0;
	TOptional<FTextureStatusMessageContext> StatusMessage;
	UE::TextureDerivedData::FTextureBuildInputResolver InputResolver;
	FRWLock Lock;
};

FTextureAsyncCacheDerivedDataTask* CreateTextureBuildTask(
	UTexture& Texture,
	FTexturePlatformData& DerivedData,
	const FTextureBuildSettings& Settings,
	EQueuedWorkPriority Priority,
	ETextureCacheFlags Flags)
{
	TStringBuilder<64> FunctionName;
	if (TryFindTextureBuildFunction(FunctionName, Settings))
	{
		return new FTextureBuildTask(Texture, FunctionName, DerivedData, Settings, Priority, Flags);
	}
	return nullptr;
}

FTexturePlatformData::FStructuredDerivedDataKey CreateTextureDerivedDataKey(
	UTexture& Texture,
	ETextureCacheFlags CacheFlags,
	const FTextureBuildSettings& Settings)
{
	using namespace UE::DerivedData;

	TStringBuilder<64> FunctionName;
	if (TryFindTextureBuildFunction(FunctionName, Settings))
	{
		IBuild& Build = GetBuild();

		TStringBuilder<256> TexturePath;
		Texture.GetPathName(nullptr, TexturePath);

		bool bUseCompositeTexture = false;
		if (FTextureBuildTask::IsTextureValidForBuilding(Texture, CacheFlags, bUseCompositeTexture))
		{
			FBuildDefinition Definition = FTextureBuildTask::CreateDefinition(Build, Texture, TexturePath, FunctionName, Settings, bUseCompositeTexture);

			return FTextureBuildTask::GetKey(Definition, Texture, bUseCompositeTexture);
		}
	}
	return FTexturePlatformData::FStructuredDerivedDataKey();
}

#endif // WITH_EDITOR
