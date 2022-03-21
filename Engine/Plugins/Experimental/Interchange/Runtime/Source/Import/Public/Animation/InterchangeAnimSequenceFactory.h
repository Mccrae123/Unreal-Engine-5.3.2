// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InterchangeFactoryBase.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "InterchangeAnimSequenceFactory.generated.h"

class UAnimSequence;

UCLASS(BlueprintType, Experimental)
class INTERCHANGEIMPORT_API UInterchangeAnimSequenceFactory : public UInterchangeFactoryBase
{
	GENERATED_BODY()
public:

	//////////////////////////////////////////////////////////////////////////
	// Interchange factory base interface begin

	virtual UClass* GetFactoryClass() const override;
	virtual UObject* CreateEmptyAsset(const FCreateAssetParams& Arguments) override;
	virtual UObject* CreateAsset(const FCreateAssetParams& Arguments) override;
	virtual void PreImportPreCompletedCallback(const FImportPreCompletedCallbackParams& Arguments) override;
	//virtual void PostImportPreCompletedCallback(const FImportPreCompletedCallbackParams& Arguments) const override;
	virtual bool CanExecuteOnAnyThread() const override
	{
		//Currently we cannot use the anim sequence controller outside of the game thread
		return false;
	}

	// Interchange factory base interface end
	//////////////////////////////////////////////////////////////////////////
};


