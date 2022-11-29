// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettingsBackedByCVars.h"
#include "Engine/PlatformSettings.h"
#include "EnhancedInputDeveloperSettings.generated.h"

class UInputMappingContext;

/** Represents a single input mapping context and the priority that it should be applied with */
USTRUCT()
struct FDefaultContextSetting
{
	GENERATED_BODY()

	/** Input Mapping Context that should be Added to the EnhancedInputEditorSubsystem when it starts listening for input */
	UPROPERTY(EditAnywhere, Config, Category = "Input")
	TSoftObjectPtr<const UInputMappingContext> InputMappingContext = nullptr;

	/** The prioirty that should be given to this mapping context when it is added */
	UPROPERTY(EditAnywhere, Config, Category = "Input")
	int32 Priority = 0;
};

/** Developer settings for Enhanced Input */
UCLASS(config = Input, defaultconfig, meta = (DisplayName = "Enhanced Input"))
class ENHANCEDINPUT_API UEnhancedInputDeveloperSettings : public UDeveloperSettingsBackedByCVars
{
	GENERATED_BODY()
public:
	
	UEnhancedInputDeveloperSettings(const FObjectInitializer& Initializer);

	/** If true, then the DefaultMappingContexts will be applied to all Enhanced Player Inputs. */
	UPROPERTY(EditAnywhere, Category = "Enhanced Input", meta=(ConsoleVariable="EnhancedInput.EnableDefaultMappingContexts"))
	bool bEnableDefaultMappingContexts = true;
	
	/**
	 * Array of any input mapping contexts that you want to be applied by default to the Enhanced Input local player subsystem.
	 * NOTE: These mapping context's can only be from your game's root content directory, not plugins.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Enhanced Input",  meta = (editCondition = "bEnableDefaultMappingContexts"))
	TArray<FDefaultContextSetting> DefaultMappingContexts;
	
	/**
	 * Array of any input mapping contexts that you want to be applied by default to the Enhanced Input world subsystem.
	 * NOTE: These mapping context's can only be from your game's root content directory, not plugins.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Enhanced Input", meta = (editCondition = "bEnableDefaultMappingContexts"))
	TArray<FDefaultContextSetting> DefaultWorldSubsystemMappingContexts;
	
	/** The default player input class that the Enhanced Input world subsystem will use. */
	UPROPERTY(config, EditAnywhere, NoClear, Category = "Enhanced Input")
	TSoftClassPtr<class UEnhancedPlayerInput> DefaultWorldInputClass;

	/**
	 * Platform specific settings for Enhanced Input.
	 * @see UEnhancedInputPlatformSettings
	 */
	UPROPERTY(EditAnywhere, Category = "Enhanced Input")
	FPerPlatformSettings PlatformSettings;

	/**
	 * If true, then only the last action in a ChordedAction trigger will be fired.
	 * This means that only the action that has the ChordedTrigger on it will be fired, not the individual steps.
	 * 
	 * Default value is true.
	 */
	UPROPERTY(EditAnywhere, Category = "Enhanced Input", meta=(ConsoleVariable="EnhancedInput.OnlyTriggerLastActionInChord"))
	bool bShouldOnlyTriggerLastActionInChord = true;
	
	/**
 	 * If true then the Enhanced Input world subsystem will log all input that is being processed by it (keypresses, analog values, etc)
 	 * Note: This can produce A LOT of logs, so only use this if you are debugging something.
 	 */
	UPROPERTY(config, EditAnywhere, Category = "Enhanced Input World Subsystem", meta=(ConsoleVariable="EnhancedInput.bShouldLogAllWorldSubsystemInputs"))
	uint8 bShouldLogAllWorldSubsystemInputs : 1;
};