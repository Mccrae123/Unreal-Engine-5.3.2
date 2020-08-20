// Copyright Epic Games, Inc. All Rights Reserved. 
#include "Texture/InterchangeDDSTranslator.h"

#include "DDSLoader.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "LogInterchangeImportPlugin.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Nodes/BaseNodeContainer.h"
#include "TextureNode.h"


//////////////////////////////////////////////////////////////////////////
// DDS helper local function

namespace DDSParserHelper
{
} //DDSParserHelper namespace end

bool UInterchangeDDSTranslator::CanImportSourceData(const UInterchangeSourceData* InSourceData) const
{
	FString Extension = FPaths::GetExtension(InSourceData->GetFilename());
	FString DDSExtension = (TEXT("dds;Texture"));
	if (!DDSExtension.StartsWith(Extension))
	{
		return false;
	}

	/*
	 * DDS file can also be a cube map so we have to open the file and see if its a valid 2D texture.
	 */
	TArray64<uint8> SourceDataBuffer;
	FString Filename = InSourceData->GetFilename();

	if (!FPaths::FileExists(Filename))
	{
		return false;
	}

	if (!FFileHelper::LoadFileToArray(SourceDataBuffer, *Filename))
	{
		return false;
	}

	const uint8* Buffer = SourceDataBuffer.GetData();
	const uint8* BufferEnd = Buffer + SourceDataBuffer.Num();
	// Validate it.
	const int32 Length = BufferEnd - Buffer;


	FDDSLoadHelper  DDSLoadHelper(Buffer, Length);
	return DDSLoadHelper.IsValid2DTexture();
}

bool UInterchangeDDSTranslator::Translate(const UInterchangeSourceData* SourceData, Interchange::FBaseNodeContainer& BaseNodeContainer) const
{
	FString Filename = SourceData->GetFilename();
	if (!FPaths::FileExists(Filename))
	{
		return false;
	}

	FName DisplayLabel = *FPaths::GetBaseFilename(Filename);
	Interchange::FNodeUniqueID NodeUID(*Filename);
	//PCX is creating a UTexture2D
	TUniquePtr<Interchange::FTextureNode> TextureNode = MakeUnique<Interchange::FTextureNode>(NodeUID, DisplayLabel, UTexture2D::StaticClass());
	TextureNode->SetPayLoadKey(Filename);

	//Test node change
 	//TextureNode->SetCustomLODGroup((uint8)TextureGroup::TEXTUREGROUP_WorldNormalMap);
 	//TextureNode->SetCustomCompressionSettings((uint8)TextureCompressionSettings::TC_Normalmap);

	BaseNodeContainer.AddNode(MoveTemp(TextureNode));
	return true;
}

const TOptional<Interchange::FImportImage> UInterchangeDDSTranslator::GetPayloadData(const UInterchangeSourceData* SourceData, const FString& PayLoadKey) const
{
	if (!SourceData)
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import DDS, bad source data."));
		return TOptional<Interchange::FImportImage>();
	}

	TArray64<uint8> SourceDataBuffer;
	FString Filename = SourceData->GetFilename();
	
	//Make sure the key fit the filename, The key should always be valid
	if (!Filename.Equals(PayLoadKey))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import DDS, wrong payload key. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	if (!FPaths::FileExists(Filename))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import DDS, cannot open file. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	if (!FFileHelper::LoadFileToArray(SourceDataBuffer, *Filename))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import DDS, cannot load file content into an array. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	const uint8* Buffer = SourceDataBuffer.GetData();
	const uint8* BufferEnd = Buffer + SourceDataBuffer.Num();

	bool bAllowNonPowerOfTwo = false;
	GConfig->GetBool(TEXT("TextureImporter"), TEXT("AllowNonPowerOfTwoTextures"), bAllowNonPowerOfTwo, GEditorIni);

	// Validate it.
	const int32 Length = BufferEnd - Buffer;


	//
	// DDS Texture
	//
	FDDSLoadHelper  DDSLoadHelper(Buffer, Length);
	if (!DDSLoadHelper.IsValid2DTexture())
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import DDS, unsupported format. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}
	// DDS 2d texture
	if (!Interchange::FImportImageHelper::IsImportResolutionValid(DDSLoadHelper.DDSHeader->dwWidth, DDSLoadHelper.DDSHeader->dwHeight, bAllowNonPowerOfTwo))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import PCX, invalid resolution. Resolution[%d, %d], AllowPowerOfTwo[%s], [%s]"), DDSLoadHelper.DDSHeader->dwWidth, DDSLoadHelper.DDSHeader->dwHeight, bAllowNonPowerOfTwo ? TEXT("True") : TEXT("false"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	ETextureSourceFormat SourceFormat = DDSLoadHelper.ComputeSourceFormat();

	// Invalid DDS format
	if (SourceFormat == TSF_Invalid)
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("DDS file [%s] contains data in an unsupported format"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	uint32 MipMapCount = DDSLoadHelper.ComputeMipMapCount();
	if (MipMapCount <= 0)
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("DDS file [%s] do not have any mipmap"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	Interchange::FImportImage PayloadData;
	PayloadData.Init2DWithMips(
		DDSLoadHelper.DDSHeader->dwWidth,
		DDSLoadHelper.DDSHeader->dwHeight,
		MipMapCount,
		SourceFormat,
		DDSLoadHelper.GetDDSDataPointer()
	);

	if (MipMapCount > 1)
	{
		PayloadData.MipGenSettings = TMGS_LeaveExistingMips;
	}
	if (FTextureSource::IsHDR(SourceFormat))
	{
		// the loader can suggest a compression setting
		PayloadData.CompressionSettings = TC_HDR;
	}

	return PayloadData;
}