// Copyright Epic Games, Inc. All Rights Reserved. 
#include "Texture/InterchangeJPGTranslator.h"

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

bool UInterchangeJPGTranslator::CanImportSourceData(const UInterchangeSourceData* InSourceData) const
{
	FString Extension = FPaths::GetExtension(InSourceData->GetFilename());
	FString JPGExtension = (TEXT("jpg;Texture"));
	return JPGExtension.StartsWith(Extension);
}

bool UInterchangeJPGTranslator::Translate(const UInterchangeSourceData* SourceData, Interchange::FBaseNodeContainer& BaseNodeContainer) const
{
	FString Filename = SourceData->GetFilename();
	if (!FPaths::FileExists(Filename))
	{
		return false;
	}

	FName DisplayLabel = *FPaths::GetBaseFilename(Filename);
	Interchange::FNodeUniqueID NodeUID(*Filename);
	//PNG is creating a UTexture2D
	Interchange::FTextureNode* TextureNode = new Interchange::FTextureNode(NodeUID, DisplayLabel, UTexture2D::StaticClass());
	TextureNode->SetPayLoadKey(Filename);

	//Test node change
 	//TextureNode->SetCustomLODGroup((uint8)TextureGroup::TEXTUREGROUP_WorldNormalMap);
 	//TextureNode->SetCustomCompressionSettings((uint8)TextureCompressionSettings::TC_Normalmap);

	BaseNodeContainer.AddNode(static_cast<Interchange::FBaseNode*>(TextureNode));
	return true;
}

const TOptional<Interchange::FImportImage> UInterchangeJPGTranslator::GetPayloadData(const UInterchangeSourceData* SourceData, const FString& PayLoadKey) const
{
	if (!SourceData)
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import JPEG, bad source data."));
		return TOptional<Interchange::FImportImage>();
	}

	TArray64<uint8> SourceDataBuffer;
	FString Filename = SourceData->GetFilename();
	
	//Make sure the key fit the filename, The key should always be valid
	if (!Filename.Equals(PayLoadKey))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import JPEG, wrong payload key. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	if (!FPaths::FileExists(Filename))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import JPEG, cannot open file. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	if (!FFileHelper::LoadFileToArray(SourceDataBuffer, *Filename))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import JPEG, cannot load file content into an array. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	const uint8* Buffer = SourceDataBuffer.GetData();
	const uint8* BufferEnd = Buffer + SourceDataBuffer.Num();

	bool bAllowNonPowerOfTwo = false;
	GConfig->GetBool(TEXT("TextureImporter"), TEXT("AllowNonPowerOfTwoTextures"), bAllowNonPowerOfTwo, GEditorIni);

	// Validate it.
	const int32 Length = BufferEnd - Buffer;

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

	//
	// JPG
	//
	TSharedPtr<IImageWrapper> JpegImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!JpegImageWrapper.IsValid() || !JpegImageWrapper->SetCompressed(Buffer, Length))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to decode JPEG. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}
	if (!Interchange::FImportImageHelper::IsImportResolutionValid(JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight(), bAllowNonPowerOfTwo))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to import JPEG, invalid resolution. Resolution[%d, %d], AllowPowerOfTwo[%s], [%s]"), JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight(), bAllowNonPowerOfTwo ? TEXT("True") : TEXT("false"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	// Select the texture's source format
	ETextureSourceFormat TextureFormat = TSF_Invalid;
	int32 BitDepth = JpegImageWrapper->GetBitDepth();
	ERGBFormat Format = JpegImageWrapper->GetFormat();

	if (Format == ERGBFormat::Gray)
	{
		if (BitDepth <= 8)
		{
			TextureFormat = TSF_G8;
			Format = ERGBFormat::Gray;
			BitDepth = 8;
		}
	}
	else if (Format == ERGBFormat::RGBA)
	{
		if (BitDepth <= 8)
		{
			TextureFormat = TSF_BGRA8;
			Format = ERGBFormat::BGRA;
			BitDepth = 8;
		}
	}

	if (TextureFormat == TSF_Invalid)
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("JPEG file [%s] contains data in an unsupported format"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}

	TOptional<Interchange::FImportImage> PayloadData = Interchange::FImportImage();

	PayloadData.GetValue().Init2DWithParams(
		JpegImageWrapper->GetWidth(),
		JpegImageWrapper->GetHeight(),
		TextureFormat,
		BitDepth < 16
	);

	if (!JpegImageWrapper->GetRaw(Format, BitDepth, PayloadData.GetValue().RawData))
	{
		UE_LOG(LogInterchangeImportPlugin, Error, TEXT("Failed to decode JPEG. [%s]"), *Filename);
		return TOptional<Interchange::FImportImage>();
	}
	return PayloadData;
}