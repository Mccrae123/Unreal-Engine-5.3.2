// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Object.h"

#include "WorldPartition/DataLayer/DataLayerType.h" 

#include "DataLayerAsset.generated.h"

class AActor;

UCLASS(BlueprintType, editinlinenew)
class ENGINE_API UDataLayerAsset : public UObject
{
	GENERATED_UCLASS_BODY()

	friend class UDataLayerConversionInfo;

#if WITH_EDITOR
	//~ Begin UObject Interface
	virtual void PostLoad() override;
	virtual bool CanEditChange(const FProperty* InProperty) const override;
	//~ End UObject Interface

public:
	void SetType(EDataLayerType Type) 
	{ 
		check(Type == EDataLayerType::Editor || !IsPrivate());
		DataLayerType = Type; 
	}
	void SetDebugColor(FColor InDebugColor) { DebugColor = InDebugColor; }
	bool CanBeReferencedByActor(AActor* InActor) const;
	static bool CanBeReferencedByActor(const TSoftObjectPtr<UDataLayerAsset>& InDataLayerAsset, AActor* InActor);
#endif
	bool IsPrivate() const;

	UFUNCTION(Category = "Data Layer", BlueprintCallable)
	EDataLayerType GetType() const { return DataLayerType; }

	UFUNCTION(Category = "Data Layer|Runtime", BlueprintCallable)
	bool IsRuntime() const { return !IsPrivate() && DataLayerType == EDataLayerType::Runtime; }

	UFUNCTION(Category = "Data Layer|Runtime", BlueprintCallable)
	FColor GetDebugColor() const { return DebugColor; }

	bool SupportsActorFilters() const { return bSupportsActorFilters; }
private:
	/** Whether the Data Layer affects actor runtime loading */
	UPROPERTY(Category = "Data Layer", EditAnywhere)
	EDataLayerType DataLayerType;

	UPROPERTY(Category = "Actor Filter", EditAnywhere)
	bool bSupportsActorFilters;
		
	UPROPERTY(Category = "Runtime", EditAnywhere)
	FColor DebugColor;
};