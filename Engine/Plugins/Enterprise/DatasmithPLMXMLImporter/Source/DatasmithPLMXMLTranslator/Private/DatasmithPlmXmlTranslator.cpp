// Copyright Epic Games, Inc. All Rights Reserved.

#include "DatasmithPlmXmlTranslator.h"
#include "DatasmithPlmXmlImporter.h"
#include "DatasmithPlmXmlTranslatorModule.h"

#include "DatasmithSceneFactory.h"
#include "DatasmithSceneSource.h"
#include "DatasmithImportOptions.h"
#include "IDatasmithSceneElements.h"

void FDatasmithPlmXmlTranslator::Initialize(FDatasmithTranslatorCapabilities& OutCapabilities)
{
	OutCapabilities.bIsEnabled = true;
	OutCapabilities.bParallelLoadStaticMeshSupported = true;

	TArray<FFileFormatInfo>& Formats = OutCapabilities.SupportedFileFormats;
    Formats.Emplace(TEXT("plmxml"), TEXT("PML(Product Lifecycle Management) XML"));
}

bool FDatasmithPlmXmlTranslator::LoadScene(TSharedRef<IDatasmithScene> OutScene)
{
	OutScene->SetHost(TEXT("PlmXmlTranslator"));
	OutScene->SetProductName(TEXT("PlmXml"));

    Importer = MakeUnique<FDatasmithPlmXmlImporter>(OutScene);

	const FString& FilePath = GetSource().GetSourceFile();
	if (!Importer->OpenFile(FilePath, GetSource(), CommonTessellationOptionsPtr->Options))
	{
		return false;
	}

	return true;
}

void FDatasmithPlmXmlTranslator::UnloadScene()
{
	Importer->UnloadScene();
	Importer.Reset();
}

bool FDatasmithPlmXmlTranslator::LoadStaticMesh(const TSharedRef<IDatasmithMeshElement> MeshElement, FDatasmithMeshElementPayload& OutMeshPayload)
{
	if (ensure(Importer.IsValid()))
	{
		return Importer->LoadStaticMesh(MeshElement, OutMeshPayload);
	}

	return false;
}

void FDatasmithPlmXmlTranslator::GetSceneImportOptions(TArray<TStrongObjectPtr<UObject>>& Options)
{
	if (!CommonTessellationOptionsPtr.IsValid())
	{
		CommonTessellationOptionsPtr = Datasmith::MakeOptions<UDatasmithCommonTessellationOptions>();
	}
	Options.Add(CommonTessellationOptionsPtr);
}

void FDatasmithPlmXmlTranslator::SetSceneImportOptions(TArray<TStrongObjectPtr<UObject>>& Options)
{
	for (const TStrongObjectPtr<UObject>& OptionPtr : Options)
	{
		if (UDatasmithCommonTessellationOptions* TessellationOptionsObject = Cast<UDatasmithCommonTessellationOptions>(OptionPtr.Get()))
		{
			CommonTessellationOptionsPtr.Reset(TessellationOptionsObject);
		}
	}
}
