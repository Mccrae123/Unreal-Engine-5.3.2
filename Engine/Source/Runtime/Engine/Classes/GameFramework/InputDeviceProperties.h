// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GenericPlatform/IInputInterface.h"
#include "GameFramework/InputDeviceSubsystem.h"

#include "InputDeviceProperties.generated.h"

class UCurveLinearColor;
class UCurveFloat;

#if WITH_EDITOR
	struct FPropertyChangedChainEvent;
#endif	// WITH_EDITOR

/**
* Base class that represents a single Input Device Property. An Input Device Property
* represents a feature that can be set on an input device. Things like what color a
* light is, advanced rumble patterns, or trigger haptics.
* 
* This top level object can then be evaluated at a specific time to create a lower level
* FInputDeviceProperty, which the IInputInterface implementation can interpret however it desires.
* 
* The behavior of device properties can vary depending on the current platform. Some platforms may not
* support certain device properties. An older gamepad may not have any advanced trigger haptics for 
* example. 
*/
UCLASS(Abstract, Blueprintable, BlueprintType, EditInlineNew, CollapseCategories, meta = (ShowWorldContextPin))
class ENGINE_API UInputDeviceProperty : public UObject
{
	friend class UInputDeviceSubsystem;

	GENERATED_BODY()
public:

	UInputDeviceProperty(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:

	/**
	* Evaluate this device property for a given duration. 
	* If overriding in Blueprints, make sure to call the parent function!
	* 
 	* @param PlatformUser		The platform user that should receive this device property change
	* @param DeltaTime			Delta time
	* @param Duration			The number of seconds that this property has been active. Use this to get things like curve data over time.
	* @return					A pointer to the evaluated input device property.
	*/
	UFUNCTION(BlueprintNativeEvent, Category = "InputDevice")
	void EvaluateDeviceProperty(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration);

	/** 
	* Native C++ implementation of EvaluateDeviceProperty.
	* 
	* Override this to alter your device property in native code.
	* @see UInputDeviceProperty::EvaluateDeviceProperty
	*/
	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration);

	/**
	* Reset the current device property. Provides an opportunity to reset device state after evaluation is complete. 
	* If overriding in Blueprints, make sure to call the parent function!
	* 
	* @param PlatformUser		The platform user that should receive this device property change
	*/
	UFUNCTION(BlueprintNativeEvent, Category = "InputDevice")
	void ResetDeviceProperty(const FPlatformUserId PlatformUser);

	/**
	* Native C++ implementation of ResetDeviceProperty
	* Override this in C++ to alter the device property behavior in native code. 
	* 
	* @see ResetDeviceProperty
	*/
	virtual void ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser);

	/**
	* Apply the device property from GetInternalDeviceProperty to the given platform user. 
	* Note: To remove any applied affects of this device property, call ResetDeviceProperty.
	* 
	* @param UserId		The owning Platform User whose input device this property should be applied to.
	*/
	UFUNCTION(Category = "InputDevice")
	virtual void ApplyDeviceProperty(const FPlatformUserId UserId);
	
	/** Gets a pointer to the current input device property that the IInputInterface can use. */
	virtual FInputDeviceProperty* GetInternalDeviceProperty() { return nullptr; };

public:

	/**
	* The duration that this device property should last. Override this if your property has any dynamic curves 
	* to be the max time range.
	*/
	float GetDuration() const;
	
	/**
	 * Recalculates this device property's duration. This should be called whenever there are changes made
	 * to things like curves, or other time sensitive properties.
	 */
	virtual float RecalculateDuration();

	// Post edit change property to update the duration if there are any dynamic options like for curves
#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif	// WITH_EDITOR

protected:

	/**
	* Apply the given device property
	*
	* @param UserId			The owning Platform User whose input device this property should be applied to.
	* @param RawProperty	The internal input device property to apply.
	*/
	static void ApplyDeviceProperty_Internal(const FPlatformUserId UserId, FInputDeviceProperty* RawProperty);

	/** Returns the device specific data for the given platform user. Returns the default data if none are given */
	template<class TDataLayout>
	const TDataLayout* GetDeviceSpecificData(const FPlatformUserId UserId, const TMap<FName, TDataLayout>& InMap) const;

	template<class TDataLayout>
	TDataLayout* GetDeviceSpecificDataMutable(const FPlatformUserId UserId, TMap<FName, TDataLayout>& InMap)
	{
		return GetDeviceSpecificData<TDataLayout>(UserId, InMap);
	}

	/**
	* The duration that this device property should last. Override this if your property has any dynamic curves 
	* to be the max time range.
	*/
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Info")
	float PropertyDuration = 0.1f;
};

template<class TDataLayout>
const TDataLayout* UInputDeviceProperty::GetDeviceSpecificData(const FPlatformUserId UserId, const TMap<FName, TDataLayout>& InDeviceData) const
{
	if (const UInputDeviceSubsystem* SubSystem = UInputDeviceSubsystem::Get())
	{
		const FHardwareDeviceIdentifier Hardware = SubSystem->GetMostRecentlyUsedHardwareDevice(UserId);
		// Check if there are any per-input device overrides available
		if (const TDataLayout* DeviceDetails = InDeviceData.Find(Hardware.HardwareDeviceIdentifier))
		{
			return DeviceDetails;
		}
	}

	return nullptr;
}

///////////////////////////////////////////////////////////////////////
// UColorInputDeviceProperty

/** Data required for setting the Input Device Color */
USTRUCT(BlueprintType)
struct FDeviceColorData
{
	GENERATED_BODY()

	/** True if the light should be enabled at all */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	bool bEnable = true;

	/** The color to set the light on  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	FColor LightColor = FColor::White;
};

/**
* Set the color of an Input Device to a static color. This will NOT reset the device color when the property
* is done evaluating. You can think of this as a "One Shot" effect, where you set the device property color.
* 
* NOTE: This property has platform specific implementations and may behave differently per platform.
* See the docs for more details on each platform.
*/
UCLASS(Blueprintable, BlueprintType)
class UColorInputDeviceProperty : public UInputDeviceProperty
{
	GENERATED_BODY()

public:

	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration) override;
	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;

	/** Default color data that will be used by default. Device Specific overrides will be used when the current input device matches */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Color")
	FDeviceColorData ColorData;

	/** A map of device specific color data. If no overrides are specified, the Default hardware data will be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Color", meta = (GetOptions = "Engine.InputPlatformSettings.GetAllHardwareDeviceNames"))
	TMap<FName, FDeviceColorData> DeviceOverrideData;

private:

	/** The internal light color property that this represents; */
	FInputDeviceLightColorProperty InternalProperty;
};


///////////////////////////////////////////////////////////////////////
// UColorInputDeviceCurveProperty

/** Data required for setting the Input Device Color */
USTRUCT(BlueprintType)
struct FDeviceColorCurveData
{
	GENERATED_BODY()

	/** True if the light should be enabled at all */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	bool bEnable = true;

	/** If true, the light color will be reset to "off" after the curve values are finished evaluating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	bool bResetAfterCompletion = true;

	/** The color the device light should be */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	TObjectPtr<UCurveLinearColor> DeviceColorCurve;
};

/** 
* A property that can be used to change the color of an input device's light over time with a curve
* 
* NOTE: This property has platform specific implementations and may behave differently per platform.
* See the docs for more details on each platform.
*/
UCLASS(Blueprintable, BlueprintType)
class UColorInputDeviceCurveProperty : public UInputDeviceProperty
{
	GENERATED_BODY()

public:

	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration) override;
	virtual void ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser) override;
	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;
	virtual float RecalculateDuration() override;

protected:
	/** Default color data that will be used by default. Device Specific overrides will be used when the current input device matches */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Color")
	FDeviceColorCurveData ColorData;

	/** A map of device specific color data. If no overrides are specified, the Default hardware data will be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Color", meta = (GetOptions = "Engine.InputPlatformSettings.GetAllHardwareDeviceNames"))
	TMap<FName, FDeviceColorCurveData> DeviceOverrideData;

private:

	/** The internal light color property that this represents; */
	FInputDeviceLightColorProperty InternalProperty;
};


///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerEffect

USTRUCT(BlueprintType)
struct FDeviceTriggerBaseData
{
	GENERATED_BODY()

	/** Which trigger this property should effect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	EInputDeviceTriggerMask AffectedTriggers = EInputDeviceTriggerMask::None;

	/** True if the triggers should be reset after the duration of this device property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	bool bResetUponCompletion = true;
};

/** A property that effect the triggers on a gamepad */
UCLASS(Abstract, Blueprintable, meta = (DisplayName = "Reset Trigger Device Properties"))
class ENGINE_API UInputDeviceTriggerEffect : public UInputDeviceProperty
{
	GENERATED_BODY()

public:	

	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;
	virtual void ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	FDeviceTriggerBaseData BaseTriggerData;

protected:

	/** Internal property that can be used to reset a given trigger */
	FInputDeviceTriggerResetProperty ResetProperty = {};
};

///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerFeedbackProperty

USTRUCT(BlueprintType)
struct FDeviceTriggerFeedbackData
{
	GENERATED_BODY()

	/** What position on the trigger that the feedback should be applied to over time (1-9) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	TObjectPtr<UCurveFloat> FeedbackPositionCurve;

	/** How strong the feedback is over time (1-8) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	TObjectPtr<UCurveFloat> FeedbackStrenghCurve;
};

/** 
* Sets simple trigger feedback
* 
* NOTE: This property has platform specific implementations and may behave differently per platform.
* See the docs for more details on each platform.
*/
UCLASS(Blueprintable)
class UInputDeviceTriggerFeedbackProperty : public UInputDeviceTriggerEffect
{
	GENERATED_BODY()

public:
	
	UInputDeviceTriggerFeedbackProperty();
	
	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration) override;	
	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;
	virtual float RecalculateDuration() override;

	/** What position on the trigger that the feedback should be applied to over time (1-9) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	FDeviceTriggerFeedbackData TriggerData;

	/** A map of device specific color data. If no overrides are specified, the Default hardware data will be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Trigger", meta = (GetOptions = "Engine.InputPlatformSettings.GetAllHardwareDeviceNames"))
	TMap<FName, FDeviceTriggerFeedbackData> DeviceOverrideData;

private:

	int32 GetPositionValue(const FDeviceTriggerFeedbackData* Data, const float Duration) const;
	int32 GetStrengthValue(const FDeviceTriggerFeedbackData* Data, const float Duration) const;

	/** The internal property that represents this trigger feedback. */
	FInputDeviceTriggerFeedbackProperty InternalProperty;
};

///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerResistanceProperty

USTRUCT(BlueprintType)
struct FDeviceTriggerTriggerResistanceData
{
	GENERATED_BODY()

	/** The position that the trigger should start providing resistance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	int32 StartPosition = 0;

	/** How strong the resistance is */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	int32 StartStrengh = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	int32 EndPosition = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	int32 EndStrengh = 0;
};

/** 
* Provides resistance to a trigger while it is being pressed between a start and end value
* 
* NOTE: This property has platform specific implementations and may behave differently per platform.
* See the docs for more details on each platform.
*/
UCLASS(Blueprintable)
class UInputDeviceTriggerResistanceProperty : public UInputDeviceTriggerEffect
{
	GENERATED_BODY()

public:

	UInputDeviceTriggerResistanceProperty();

	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration) override;
	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;

protected:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	FDeviceTriggerTriggerResistanceData TriggerData;


	/** A map of device specific color data. If no overrides are specified, the Default hardware data will be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Trigger", meta = (GetOptions = "Engine.InputPlatformSettings.GetAllHardwareDeviceNames"))
	TMap<FName, FDeviceTriggerTriggerResistanceData> DeviceOverrideData;

private:

	/** The internal property that represents this trigger resistance */
	FInputDeviceTriggerResistanceProperty InternalProperty;
};


///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerVibrationProperty


USTRUCT(BlueprintType)
struct FDeviceTriggerTriggerVibrationData
{
	GENERATED_BODY()

	/** What position on the trigger that the feedback should be applied to over time (1-9) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	TObjectPtr<UCurveFloat> TriggerPositionCurve;

	/** The frequency of the vibration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	TObjectPtr<UCurveFloat> VibrationFrequencyCurve;

	/** The amplitude of the vibration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DeviceProperty")
	TObjectPtr<UCurveFloat> VibrationAmplitudeCurve;
};

/**
* Sets trigger vibration
*
* NOTE: This property has platform specific implementations and may behave differently per platform.
* See the docs for more details on each platform.
*/
UCLASS(Blueprintable)
class UInputDeviceTriggerVibrationProperty : public UInputDeviceTriggerEffect
{
	GENERATED_BODY()

public:

	UInputDeviceTriggerVibrationProperty();

	virtual void EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration) override;
	virtual FInputDeviceProperty* GetInternalDeviceProperty() override;
	virtual float RecalculateDuration() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	FDeviceTriggerTriggerVibrationData TriggerData;

	/** A map of device specific color data. If no overrides are specified, the Default hardware data will be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Trigger", meta = (GetOptions = "Engine.InputPlatformSettings.GetAllHardwareDeviceNames"))
	TMap<FName, FDeviceTriggerTriggerVibrationData> DeviceOverrideData;

private:

	int32 GetTriggerPositionValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const;
	int32 GetVibrationFrequencyValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const;
	int32 GetVibrationAmplitudeValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const;

	/** The internal property that represents this trigger feedback. */
	FInputDeviceTriggerVibrationProperty InternalProperty;
};