// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Sound/SoundEffectSource.h"
#include "DSP/Chorus.h"
#include "Sound/SoundModulationDestination.h"

#include "SourceEffectChorus.generated.h"

USTRUCT(BlueprintType)
struct SYNTHESIS_API FSourceEffectChorusSettings
{
	GENERATED_USTRUCT_BODY()

	// The depth of the chorus effect
	UPROPERTY(meta = (DeprecatedProperty))
	float Depth;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	FSoundModulationDestinationSettings DepthModulation;

	// The frequency of the chorus effect
	UPROPERTY(meta = (DeprecatedProperty))
	float Frequency;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (DisplayName = "Frequency", AudioParam = "LowRateFrequency", UIMin = "0.0", UIMax = "5.0"))
	FSoundModulationDestinationSettings FrequencyModulation;

	UPROPERTY(meta = (DeprecatedProperty))
	float Feedback;

	// The feedback of the chorus effect
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (DisplayName = "Feedback", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	FSoundModulationDestinationSettings FeedbackModulation;

	UPROPERTY(meta = (DeprecatedProperty))
	float WetLevel;

	// The wet level of the chorus effect
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (DisplayName = "Wet Level", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	FSoundModulationDestinationSettings WetModulation;

	UPROPERTY(meta = (DeprecatedProperty))
	float DryLevel;

	// The dry level of the chorus effect
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (DisplayName = "Dry Level", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	FSoundModulationDestinationSettings DryModulation;

	UPROPERTY(meta = (DeprecatedProperty))
	float Spread;

	// The spread of the effect (larger means greater difference between left and right delay lines)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset", meta = (DisplayName = "Spread", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	FSoundModulationDestinationSettings SpreadModulation;

	FSourceEffectChorusSettings()
		: Depth(0.2f)
		, Frequency(2.0f)
		, Feedback(0.3f)
		, WetLevel(0.5f)
		, DryLevel(0.5f)
		, Spread(0.0f)
	{
		DepthModulation.Value = 0.2f;
		FrequencyModulation.Value = 2.0f;
		FeedbackModulation.Value = 0.3f;
		WetModulation.Value = 0.5f;
		DryModulation.Value = 0.5f;
		DepthModulation.Value = 0.0f;
	}
};

class SYNTHESIS_API FSourceEffectChorus : public FSoundEffectSource
{
public:
	// Called on an audio effect at initialization on main thread before audio processing begins.
	virtual void Init(const FSoundEffectSourceInitData& InitData) override;
	
	// Called when an audio effect preset is changed
	virtual void OnPresetChanged() override;

	// Process the input block of audio. Called on audio thread.
	virtual void ProcessAudio(const FSoundEffectSourceInputData& InData, float* OutAudioBufferData) override;

	void SetDepthModulator(const FSoundModulationDestinationSettings& InModulatorSettings);
	void SetFeedbackModulator(const FSoundModulationDestinationSettings& InModulatorSettings);
	void SetFrequencyModulator(const FSoundModulationDestinationSettings& InModulatorSettings);
	void SetWetModulator(const FSoundModulationDestinationSettings& InModulatorSettings);
	void SetDryModulator(const FSoundModulationDestinationSettings& InModulatorSettings);
	void SetSpreadModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

protected:
	Audio::FChorus Chorus;

	FSourceEffectChorusSettings SettingsCopy;

	Audio::FModulationDestination DepthMod;
	Audio::FModulationDestination FeedbackMod;
	Audio::FModulationDestination FrequencyMod;
	Audio::FModulationDestination WetMod;
	Audio::FModulationDestination DryMod;
	Audio::FModulationDestination SpreadMod;
};

UCLASS(ClassGroup = AudioSourceEffect, meta = (BlueprintSpawnableComponent))
class SYNTHESIS_API USourceEffectChorusPreset : public USoundEffectSourcePreset
{
	GENERATED_BODY()

public:
	EFFECT_PRESET_METHODS(SourceEffectChorus)

	virtual FColor GetPresetColor() const override { return FColor(102.0f, 85.0f, 121.0f); }

	virtual void OnInit() override;

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetDepthModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetFeedbackModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetFrequencyModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetWetModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetDryModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetSpreadModulator(const FSoundModulationDestinationSettings& InModulatorSettings);

	UFUNCTION(BlueprintCallable, Category = "Audio|Effects")
	void SetSettings(const FSourceEffectChorusSettings& InSettings);

	// The depth of the chorus effect
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SourceEffect|Preset")
	FSourceEffectChorusSettings Settings;
};