// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "NNECoreModelData.generated.h"

UCLASS(BlueprintType, Category = "NNE - Neural Network Engine")
class NNXCORE_API UNNEModelData : public UObject
{
	GENERATED_BODY()

public:

	/**
	 * Function to initialize the data.
	 * Called by the NNEModelDataFactory.
	 * Type corresponds to the file extension.
	 */
	void Init(const TCHAR* Type, const uint8*& Buffer, const uint8* BufferEnd);

	/**
	 * The function returns the cached (editor) or cooked (game) optimized model data for a given runtime.
	 * In editor, the function will create the model data with the passed runtime in case it has not been cached yet.
	 * Returns an empty view in case of a failure.
	 */
	TConstArrayView<uint8> GetModelData(const FString& RuntimeName);

public:

	virtual void Serialize(FArchive& Ar) override;

public:

	/**
	 * A GUID used for versioning.
	 */
	const static FGuid GUID;

private:

	/**
	 * The file type passed by the factory when importing a model.
	 * Corresponds to the file extension.
	 */
	FString FileType;

	/**
	 * Raw binary file data of the imported model.
	 */
	TArray<uint8> FileData;

	/**
	 * Guid that uniquely identifies this model.
	 * This is used to cache optimized models in the editor.
	 */
	FGuid FileDataId;

private:

	/**
	 * The processed / optimized model data for the different runtimes.
	 */
	TMap<FString, TArray<uint8>> ModelData;

};
