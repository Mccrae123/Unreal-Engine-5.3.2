// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "StateTreeTypes.h"
#include "StateTreeSchema.generated.h"

/**
 * Schema describing which inputs, evaluators, and tasks a StateTree can contain.
 * Each StateTree asset saves the schema class name in asset data tags, which can be
 * used to limit which StatTree assets can be selected per use case, i.e.:
 *
 *	UPROPERTY(EditDefaultsOnly, Category = AI, meta=(RequiredAssetDataTags="Schema=StateTreeSchema_SupaDupa"))
 *	UStateTree* StateTree;
 *
 */
UCLASS(Abstract)
class STATETREEMODULE_API UStateTreeSchema : public UObject
{
	GENERATED_BODY()

public:

	/** @return Returns the script struct the storage struct will be derived from. */
	virtual UScriptStruct* GetStorageSuperStruct() const { return nullptr; }

	/** @return True if specified struct is supported */
	virtual bool IsStructAllowed(const UScriptStruct* InScriptStruct) const PURE_VIRTUAL(UStateTreeSchema::IsStructAllowed, return false; );

	/** @return True if specified struct/class is supported as external item */
	virtual bool IsExternalItemAllowed(const UStruct& InStruct) const { return false; };

	/** @return True if we should use StateTree V2 */
	virtual bool IsV2() const { return false; }
};
