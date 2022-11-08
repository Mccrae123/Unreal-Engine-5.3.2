// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/Interface.h"
#include "IChooserParameterFloat.generated.h"

UINTERFACE(NotBlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class CHOOSER_API UChooserParameterFloat : public UInterface
{
	GENERATED_BODY()
};

class CHOOSER_API IChooserParameterFloat
{
	GENERATED_BODY()

public:
	virtual bool GetValue(const UObject* ContextObject, float& OutResult) const { return false; }
};