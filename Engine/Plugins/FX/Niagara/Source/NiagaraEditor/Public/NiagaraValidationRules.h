﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraValidationRule.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraValidationRules.generated.h"

class UNiagaraScript;

UCLASS(Category = "Validation", DisplayName = "No Warmup Time")
class UNiagaraValidationRule_NoWarmupTime : public UNiagaraValidationRule
{
	GENERATED_BODY()
public:
	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};


UCLASS(Category = "Validation", DisplayName = "Fixed GPU Bounds Set")
class UNiagaraValidationRule_FixedGPUBoundsSet : public UNiagaraValidationRule
{
	GENERATED_BODY()
public:
	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};


UCLASS(Category = "Validation", DisplayName = "Banned Renderers")
class UNiagaraValidationRule_BannedRenderers : public UNiagaraValidationRule
{
	GENERATED_BODY()

public:

	//Platforms this validation rule will apply to.
	UPROPERTY(EditAnywhere, Category = Validation)
	FNiagaraPlatformSet Platforms;

	UPROPERTY(EditAnywhere, Category = Validation)
	TArray<TSubclassOf<UNiagaraRendererProperties>> BannedRenderers;

	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};

UCLASS(Category = "Validation", DisplayName = "Banned Modules")
class UNiagaraValidationRule_BannedModules : public UNiagaraValidationRule
{
	GENERATED_BODY()

public:

	//Platforms this validation rule will apply to.
	UPROPERTY(EditAnywhere, Category=Validation)
	FNiagaraPlatformSet Platforms;

	UPROPERTY(EditAnywhere, Category = Validation)
	TArray<TObjectPtr<UNiagaraScript>> BannedModules;

	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};

UCLASS(Category = "Validation", DisplayName = "Invalid Effect Type")
class UNiagaraValidationRule_InvalidEffectType : public UNiagaraValidationRule
{
	GENERATED_BODY()
public:
	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};

UCLASS(Category = "Validation", DisplayName = "Large World Coordinates")
class UNiagaraValidationRule_LWC : public UNiagaraValidationRule
{
	GENERATED_BODY()
public:
	virtual void CheckValidity(TSharedPtr<FNiagaraSystemViewModel> ViewModel, TArray<FNiagaraValidationResult>& OutResults) const override;
};