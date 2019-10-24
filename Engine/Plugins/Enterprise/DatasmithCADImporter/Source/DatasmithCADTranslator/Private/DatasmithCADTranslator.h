// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "CADOptions.h"
#include "DatasmithCoreTechTranslator.h"
#include "DatasmithDispatcher.h"
#include "DatasmithMeshBuilder.h"
#include "DatasmithSceneGraphBuilder.h"
#include "UObject/ObjectMacros.h"

class IDatasmithMeshElement;
class FCoreTechParser;

class FDatasmithCADTranslator : public FDatasmithCoreTechTranslator
{
public:
#ifndef CAD_LIBRARY
	// Begin IDatasmithTranslator overrides
	virtual FName GetFName() const override { return "DatasmithCADTranslator"; };
	// End IDatasmithTranslator overrides
#else
	FDatasmithCADTranslator();

	// Begin IDatasmithTranslator overrides
	virtual FName GetFName() const override { return "DatasmithCADTranslator"; };

	virtual void Initialize(FDatasmithTranslatorCapabilities& OutCapabilities) override;

	virtual bool IsSourceSupported(const FDatasmithSceneSource& Source) override;

	virtual bool LoadScene(TSharedRef<IDatasmithScene> OutScene) override;
	virtual void UnloadScene() override;

	virtual bool LoadStaticMesh(const TSharedRef<IDatasmithMeshElement> MeshElement, FDatasmithMeshElementPayload& OutMeshPayload) override;
	// End IDatasmithTranslator overrides

	// Begin ADatasmithCoreTechTranslator overrides
	virtual void SetSceneImportOptions(TArray<TStrongObjectPtr<UObject>>& Options) override;
	// End ADatasmithCoreTechTranslator overrides

private:
	TMap<FString, FString> CADFileToUE4FileMap;
	TMap<FString, FString> CADFileToUE4GeomMap;
	TMap< TSharedPtr< IDatasmithMeshElement >, uint32 > MeshElementToCADBRepUuidMap;

	CADLibrary::FImportParameters ImportParameters;

	FDatasmithMeshBuilder MeshBuilder;
#endif
};

