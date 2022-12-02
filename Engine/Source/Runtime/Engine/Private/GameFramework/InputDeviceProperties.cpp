// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFramework/InputDeviceProperties.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Framework/Application/SlateApplication.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveFloat.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(InputDeviceProperties)

///////////////////////////////////////////////////////////////////////
// UInputDeviceProperty

UInputDeviceProperty::UInputDeviceProperty(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	RecalculateDuration();
}

void UInputDeviceProperty::ApplyDeviceProperty(const FPlatformUserId UserId)
{
	UInputDeviceProperty::ApplyDeviceProperty_Internal(UserId, GetInternalDeviceProperty());
}

void UInputDeviceProperty::ApplyDeviceProperty_Internal(const FPlatformUserId UserId, FInputDeviceProperty* RawProperty)
{
	if (ensure(RawProperty))
	{
		IInputInterface* InputInterface = FSlateApplication::Get().IsInitialized() ? FSlateApplication::Get().GetInputInterface() : nullptr;
		if (InputInterface)
		{
			int32 ControllerId = INDEX_NONE;
			IPlatformInputDeviceMapper::Get().RemapUserAndDeviceToControllerId(UserId, ControllerId);

			// TODO_BH: Refactor input interface to take an FPlatformUserId directly (UE-158881)
			InputInterface->SetDeviceProperty(ControllerId, RawProperty);
		}
	}
}

float UInputDeviceProperty::GetDuration() const
{
	return PropertyDuration;
}

float UInputDeviceProperty::RecalculateDuration()
{
	return PropertyDuration;
}

#if WITH_EDITOR
void UInputDeviceProperty::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	Super::PostEditChangeChainProperty(PropertyChangedEvent);
	RecalculateDuration();
}
#endif	// WITH_EDITOR

void UInputDeviceProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{

}

void UInputDeviceProperty::ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser)
{

}

///////////////////////////////////////////////////////////////////////
// UColorInputDeviceProperty

void UColorInputDeviceProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{
	// Check for an override on the current input device
	if (const FDeviceColorData* Data = GetDeviceSpecificData<FDeviceColorData>(PlatformUser, DeviceOverrideData))
	{
		InternalProperty.bEnable = Data->bEnable;
		InternalProperty.Color = Data->LightColor;
	}
	// Otherwise use the default color data
	else
	{
		InternalProperty.bEnable = ColorData.bEnable;
		InternalProperty.Color = ColorData.LightColor;
	}
}

FInputDeviceProperty* UColorInputDeviceProperty::GetInternalDeviceProperty()
{
	return &InternalProperty;
}

///////////////////////////////////////////////////////////////////////
// UColorInputDeviceCurveProperty

void UColorInputDeviceCurveProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{
	// Check for an override on the current input device
	if (const FDeviceColorCurveData* Data = GetDeviceSpecificData<FDeviceColorCurveData>(PlatformUser, DeviceOverrideData))
	{
		InternalProperty.bEnable = Data->bEnable;

		if (ensure(Data->DeviceColorCurve))
		{
			FLinearColor CurveColor = Data->DeviceColorCurve->GetLinearColorValue(Duration);
			InternalProperty.Color = CurveColor.ToFColorSRGB();
		}
	}
	// Otherwise use the default color data
	else
	{
		InternalProperty.bEnable = ColorData.bEnable;

		if (ensure(ColorData.DeviceColorCurve))
		{
			FLinearColor CurveColor = ColorData.DeviceColorCurve->GetLinearColorValue(Duration);
			InternalProperty.Color = CurveColor.ToFColorSRGB();
		}
	}
}

void UColorInputDeviceCurveProperty::ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser)
{
	bool bReset = ColorData.bResetAfterCompletion;
	if (const FDeviceColorCurveData* Data = GetDeviceSpecificData<FDeviceColorCurveData>(PlatformUser, DeviceOverrideData))
	{
		bReset = Data->bResetAfterCompletion;
	}

	if (bReset)
	{
		// Disabling the light will reset the color
    	InternalProperty.bEnable = false;
    	ApplyDeviceProperty(PlatformUser);
	}	
}

FInputDeviceProperty* UColorInputDeviceCurveProperty::GetInternalDeviceProperty()
{
	return &InternalProperty;
}

float UColorInputDeviceCurveProperty::RecalculateDuration()
{
	float MinTime = 0.f;
	float MaxTime = 0.f;

	if (ColorData.DeviceColorCurve)
	{
		ColorData.DeviceColorCurve->GetTimeRange(MinTime, MaxTime);
	}

	// Find the max time of any device specific data
	for (const TPair<FName, FDeviceColorCurveData>& Pair : DeviceOverrideData)
	{
		if (Pair.Value.DeviceColorCurve)
		{
			float DeviceSpecificMinTime = 0.f;
			float DeviceSpecificMaxTime = 0.f;
			Pair.Value.DeviceColorCurve->GetTimeRange(DeviceSpecificMinTime, DeviceSpecificMaxTime);
			if (DeviceSpecificMaxTime > MaxTime)
			{
				MaxTime = DeviceSpecificMaxTime;
			}
		}		
	}

	PropertyDuration = MaxTime;
	
	return PropertyDuration;
}

///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerEffect

FInputDeviceProperty* UInputDeviceTriggerEffect::GetInternalDeviceProperty()
{
	return &ResetProperty;
}

void UInputDeviceTriggerEffect::ResetDeviceProperty_Implementation(const FPlatformUserId PlatformUser)
{
	if (BaseTriggerData.bResetUponCompletion)
	{
		// Pass in our reset property
		ResetProperty.AffectedTriggers = BaseTriggerData.AffectedTriggers;
		ApplyDeviceProperty_Internal(PlatformUser, &ResetProperty);
	}	
}

///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerFeedbackProperty

UInputDeviceTriggerFeedbackProperty::UInputDeviceTriggerFeedbackProperty()
	: UInputDeviceTriggerEffect()
{
	InternalProperty.AffectedTriggers = BaseTriggerData.AffectedTriggers;
}

int32 UInputDeviceTriggerFeedbackProperty::GetPositionValue(const FDeviceTriggerFeedbackData* Data, const float Duration) const
{
	if (ensure(Data->FeedbackPositionCurve))
	{
		// TODO: Make the max Strength a cvar
		int32 Pos = Data->FeedbackPositionCurve->GetFloatValue(Duration);
		return FMath::Clamp(Pos, 0, 8);
	}

	return 0;
}

int32 UInputDeviceTriggerFeedbackProperty::GetStrengthValue(const FDeviceTriggerFeedbackData* Data, const float Duration) const
{
	if (ensure(Data->FeedbackStrenghCurve))
	{
		// TODO: Make the max Strength a cvar
		int32 Strength = Data->FeedbackStrenghCurve->GetFloatValue(Duration);
		return FMath::Clamp(Strength, 0, 8);
	}

	return 0;
}

void UInputDeviceTriggerFeedbackProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{		
	InternalProperty.AffectedTriggers = BaseTriggerData.AffectedTriggers;

	const FDeviceTriggerFeedbackData* DataToUse = &TriggerData;

	if (const FDeviceTriggerFeedbackData* OverrideData = GetDeviceSpecificData<FDeviceTriggerFeedbackData>(PlatformUser, DeviceOverrideData))
	{
		DataToUse = OverrideData;
	}

	InternalProperty.Position = GetPositionValue(DataToUse, Duration);
	InternalProperty.Strengh = GetStrengthValue(DataToUse, Duration);
}

FInputDeviceProperty* UInputDeviceTriggerFeedbackProperty::GetInternalDeviceProperty()
{
	return &InternalProperty;
}

float UInputDeviceTriggerFeedbackProperty::RecalculateDuration()
{
	// Get the max time from the two curves on this property
	float MinTime, MaxTime = 0.0f;
	if (TriggerData.FeedbackPositionCurve)
	{
		TriggerData.FeedbackPositionCurve->GetTimeRange(MinTime, MaxTime);
	}
	
	if (TriggerData.FeedbackStrenghCurve)
	{
		TriggerData.FeedbackStrenghCurve->GetTimeRange(MinTime, MaxTime);
	}

	// Find the max time of any device specific data
	for (const TPair<FName, FDeviceTriggerFeedbackData>& Pair : DeviceOverrideData)
	{
		if (Pair.Value.FeedbackPositionCurve)
		{
			Pair.Value.FeedbackPositionCurve->GetTimeRange(MinTime, MaxTime);
		}

		if (Pair.Value.FeedbackStrenghCurve)
		{
			Pair.Value.FeedbackStrenghCurve->GetTimeRange(MinTime, MaxTime);
		}
	}
	
	PropertyDuration = MaxTime;
	return PropertyDuration;
}

///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerResistanceProperty

UInputDeviceTriggerResistanceProperty::UInputDeviceTriggerResistanceProperty()
	: UInputDeviceTriggerEffect()
{
	PropertyDuration = 1.0f;
}

void UInputDeviceTriggerResistanceProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{
	InternalProperty.AffectedTriggers = BaseTriggerData.AffectedTriggers;

	if (const FDeviceTriggerTriggerResistanceData* Data = GetDeviceSpecificData<FDeviceTriggerTriggerResistanceData>(PlatformUser, DeviceOverrideData))
	{
		InternalProperty.StartPosition = Data->StartPosition;
		InternalProperty.StartStrengh = Data->StartStrengh;
		InternalProperty.EndPosition = Data->EndPosition;
		InternalProperty.EndStrengh = Data->EndStrengh;
	}
	else
	{
		InternalProperty.StartPosition = TriggerData.StartPosition;
		InternalProperty.StartStrengh = TriggerData.StartStrengh;
		InternalProperty.EndPosition = TriggerData.EndPosition;
		InternalProperty.EndStrengh = TriggerData.EndStrengh;
	}
}

FInputDeviceProperty* UInputDeviceTriggerResistanceProperty::GetInternalDeviceProperty()
{
	return &InternalProperty;
}


///////////////////////////////////////////////////////////////////////
// UInputDeviceTriggerVibrationProperty

UInputDeviceTriggerVibrationProperty::UInputDeviceTriggerVibrationProperty()
	: UInputDeviceTriggerEffect()
{
	PropertyDuration = 1.0f;
}

void UInputDeviceTriggerVibrationProperty::EvaluateDeviceProperty_Implementation(const FPlatformUserId PlatformUser, const float DeltaTime, const float Duration)
{
	const FDeviceTriggerTriggerVibrationData* DataToUse = &TriggerData;

	if (const FDeviceTriggerTriggerVibrationData* OverrideData = GetDeviceSpecificData<FDeviceTriggerTriggerVibrationData>(PlatformUser, DeviceOverrideData))
	{
		DataToUse = OverrideData;
	}

	InternalProperty.AffectedTriggers = BaseTriggerData.AffectedTriggers;
	InternalProperty.TriggerPosition = GetTriggerPositionValue(DataToUse, Duration);
	InternalProperty.VibrationFrequency = GetVibrationFrequencyValue(DataToUse, Duration);
	InternalProperty.VibrationAmplitude = GetVibrationAmplitudeValue(DataToUse, Duration);
}

FInputDeviceProperty* UInputDeviceTriggerVibrationProperty::GetInternalDeviceProperty()
{
	return &InternalProperty;
}

float UInputDeviceTriggerVibrationProperty::RecalculateDuration()
{
	// Get the max time from the curves on this property
	float MaxTime = 0.0f;

	auto EvaluateMaxTime = [&MaxTime](TObjectPtr<UCurveFloat> InCurve)
	{
		float MinCurveTime, MaxCurveTime = 0.0f;
		if (InCurve)
		{
			InCurve->GetTimeRange(MinCurveTime, MaxCurveTime);
			if (MaxCurveTime > MaxTime)
			{
				MaxTime = MaxCurveTime;
			}
		}
	};

	EvaluateMaxTime(TriggerData.TriggerPositionCurve);
	EvaluateMaxTime(TriggerData.VibrationFrequencyCurve);
	EvaluateMaxTime(TriggerData.VibrationAmplitudeCurve);

	for (const TPair<FName, FDeviceTriggerTriggerVibrationData>& Pair : DeviceOverrideData)
	{
		EvaluateMaxTime(Pair.Value.TriggerPositionCurve);
		EvaluateMaxTime(Pair.Value.VibrationFrequencyCurve);
		EvaluateMaxTime(Pair.Value.VibrationAmplitudeCurve);
	}

	PropertyDuration = MaxTime;
	return PropertyDuration;
}

int32 UInputDeviceTriggerVibrationProperty::GetTriggerPositionValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const
{
	if (ensure(Data->TriggerPositionCurve))
	{
		// TODO: Make the max Strength a cvar
		int32 Strength = Data->TriggerPositionCurve->GetFloatValue(Duration);
		return FMath::Clamp(Strength, 0, 9);
	}

	return 0;
}

int32 UInputDeviceTriggerVibrationProperty::GetVibrationFrequencyValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const
{
	if (ensure(Data->VibrationFrequencyCurve))
	{
		// TODO: Make the max Frequency a cvar
		int32 Strength = Data->VibrationFrequencyCurve->GetFloatValue(Duration);
		return FMath::Clamp(Strength, 0, 255);
	}
	
	return 0;
}

int32 UInputDeviceTriggerVibrationProperty::GetVibrationAmplitudeValue(const FDeviceTriggerTriggerVibrationData* Data, const float Duration) const
{
	if (ensure(Data->VibrationAmplitudeCurve))
	{
		// TODO: Make the max Amplitude a cvar
		int32 Strength = Data->VibrationAmplitudeCurve->GetFloatValue(Duration);
		return FMath::Clamp(Strength, 0, 8);
	}
	
	return 0;
}
