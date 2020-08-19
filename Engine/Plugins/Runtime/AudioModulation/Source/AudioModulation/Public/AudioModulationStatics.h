// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SoundControlBus.h"
#include "SoundControlBusMix.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "AudioModulationStatics.generated.h"

// Forward Declarations
namespace AudioModulation
{
	class FAudioModulationSystem;
} // namespace AudioModulation


UCLASS()
class UAudioModulationStatics : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

public:
	/**
	 * Returns world associated with provided context object
	 */
	static UWorld* GetAudioWorld(const UObject* WorldContextObject);

	/**
	 * Returns modulation implementation associated with the provided world
	 */
	static AudioModulation::FAudioModulationSystem* GetModulationSystem(UWorld* World);

	/** Activates a bus. Does nothing if an instance of the provided bus is already active
	 * @param Bus - Bus to activate
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Activate Control Bus", meta = (
		WorldContext = "WorldContextObject", 
		Keywords = "activate modulation modulator control bus")
	)
	static void ActivateBus(const UObject* WorldContextObject, USoundControlBus* Bus);

	/** Activates a bus modulator mix. Does nothing if an instance of the provided bus mix is already active
	 * @param BusMix - Mix to activate
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Activate Control Bus Mix", meta = (
		WorldContext = "WorldContextObject", 
		Keywords = "activate modulation modulator control bus mix")
	)
	static void ActivateBusMix(const UObject* WorldContextObject, USoundControlBusMix* BusMix);

	/** Activates a bus modulator (eg. LFO). Does nothing if an instance of the provided modulator is already active
	 * @param Modulator - Modulator to activate
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Activate Control Bus Modulator", meta = (
		WorldContext = "WorldContextObject", 
		Keywords = "activate modulation modulator lfo")
	)
	static void ActivateBusModulator(const UObject* WorldContextObject, USoundModulationGenerator* Modulator);

	/** Creates a modulation bus with the provided default value.
	 * @param Name - Name of bus
	 * @param DefaultValue - Default value for created bus
	 * @param Activate - Whether or not to activate bus on creation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Create Control Bus", meta = (
		AdvancedDisplay = "3",
		WorldContext = "WorldContextObject",
		Keywords = "make create bus modulation LPF modulator")
	)
	static USoundControlBus* CreateBus(const UObject* WorldContextObject, FName Name, USoundModulationParameter* Parameter, bool Activate = true);

	/** Creates an LFO modulator.
	 * @param Name - Name of LFO
	 * @param Amplitude - Amplitude of new LFO.
	 * @param Frequency - Frequency of new LFO.
	 * @param Offset - Offset of new LFO.
	 * @param Activate - Whether or not to activate lfo on creation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Create Control Bus LFO", meta = (
		AdvancedDisplay = "5",
		WorldContext = "WorldContextObject",
		Keywords = "make create lfo modulation modulator")
	)
	static USoundModulationGeneratorLFO* CreateLFO(
		const UObject* WorldContextObject, 
		FName Name, 
		float Amplitude, 
		float Frequency, 
		float Offset,
		bool Activate = true
	);

	/** Creates a stage used to mix a control bus.
	 * @param Bus - Bus stage is in charge of applying mix value to.
	 * @param Stages - Value for added bus stage to target when mix is active.
	 * @param Attack/ReleaseTime - Time in seconds for stage to mix in/out.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Create Control Bus Mix Stage", meta = (
		AdvancedDisplay = "3",
		WorldContext = "WorldContextObject",
		Keywords = "make create control bus mix modulation modulator stage")
	)
	static FSoundControlBusMixStage CreateBusMixStage(
		const UObject* WorldContextObject,
		USoundControlBus* Bus,
		float Value,
		float AttackTime = 0.1f,
		float ReleaseTime = 0.1f);

	/** Creates a modulation bus mix and adds a bus stage set to the provided target value
	 * @param Name - Name of mix.
	 * @param Stages - Stages mix is responsible for.
	 * @param Activate - Whether or not to activate mix on creation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Create Control Bus Mix", meta = (
		WorldContext = "WorldContextObject", 
		Keywords = "make create control bus mix modulation modulator")
	)
	static USoundControlBusMix* CreateBusMix(
		const UObject* WorldContextObject,
		FName Name, 
		TArray<FSoundControlBusMixStage> Stages, 
		bool Activate);

	/** Deactivates a bus. Does nothing if an instance of the provided bus is already inactive
	 * @param Bus - Scope of modulator
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Deactivate Control Bus", meta = (
		WorldContext = "WorldContextObject", 
		Keywords = "deactivate modulation modulator bus")
	)
	static void DeactivateBus(const UObject* WorldContextObject, USoundControlBus* Bus);

	/** Deactivates a modulation bus mix. Does nothing if an instance of the provided bus mix is already inactive
	 * @param BusMix - Mix to deactivate
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Deactivate Control Bus Mix", meta = (
		WorldContext = "WorldContextObject",
		Keywords = "deactivate modulation modulator")
	)
	static void DeactivateBusMix(const UObject* WorldContextObject, USoundControlBusMix* BusMix);

	/** Deactivates a bus modulator. Does nothing if an instance of the provided bus mix is already inactive
	 * @param Modulator - Modulator to activate
	 * @param Scope - Scope of modulator
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Deactivate Control Bus Modulator", meta = (
		WorldContext = "WorldContextObject",
		Keywords = "deactivate bus modulation modulator")
)
	static void DeactivateBusModulator(const UObject* WorldContextObject, USoundModulationGenerator* Modulator);

	/** Saves control bus mix to a profile, serialized to an ini file.  If mix is loaded, uses current proxy's state. 
	 * If not, uses default UObject representation.
	 * @param BusMix - Mix object to serialize to profile .ini.
	 * @param ProfileIndex - Index of profile, allowing multiple profiles can be saved for single mix object. If 0, saves to default ini profile (no suffix).
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Save Control Bus Mix to Profile", meta = (
		WorldContext = "WorldContextObject",
		AdvancedDisplay = "2",
		Keywords = "save serialize bus control modulation mix modulator ini")
	)
	static void SaveMixToProfile(const UObject* WorldContextObject, USoundControlBusMix* BusMix, int32 ProfileIndex = 0);

	/** Loads control bus mix from a profile into UObject mix definition, deserialized from an ini file.
	 * @param BusMix - Mix object to deserialize profile .ini to.
	 * @param bActivate - If true, activate mix upon loading from profile.
	 * @param ProfileIndex - Index of profile, allowing multiple profiles to be loaded to single mix object. If <= 0, loads from default profile (no suffix).
	 * @return Stages - Stage values loaded from profile (empty if profile did not exist or had no values serialized).
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Load Control Bus Mix From Profile", meta = (
		WorldContext = "WorldContextObject",
		AdvancedDisplay = "2",
		Keywords = "load deserialize control bus modulation mix modulator ini")
	)
	static UPARAM(DisplayName = "Stages") TArray<FSoundControlBusMixStage> LoadMixFromProfile(const UObject* WorldContextObject, USoundControlBusMix* BusMix, bool bActivate = true, int32 ProfileIndex = 0);

	/** Sets a mix with the provided stage data if stages provided in active instance proxy of mix. Does not update UObject definition of mix.
	 * @param Mix - Mix to update
	 * @param Stages - Stages to set.  If stage's bus is not referenced by mix, stage's update request is ignored.
	 * @param FadeTime - Fade time to user when interpolating between current value and new values.
	 * If negative, falls back to last fade time set on stage. If fade time never set on stage,
	 * uses attack time set on stage in mix asset.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Set Control Bus Mix", meta = (
		WorldContext = "WorldContextObject",
		Keywords = "set bus control modulation modulator mix stage")
	)
	static void UpdateMix(const UObject* WorldContextObject, USoundControlBusMix* Mix, TArray<FSoundControlBusMixStage> Stages, float InFadeTime = -1.0f);

	/** Sets filtered stages of a given class to a provided target value for active instance of mix. Does not update UObject definition of mix.
	 * @param Mix - Mix to modify
	 * @param AddressFilter - Address filter to apply to provided mix's stages.
	 * @param BusClass - Filters buses by subclass.
	 * @param Value - Target value to mix filtered stages to.
	 * @param FadeTime - If non-negative, updates the fade time for the resulting bus stages found matching the provided filter.
	 * @param AttackTime - If non-negative, updates the attack time for the resulting bus stages found matching the provided filter.
	 * @param ReleaseTime - If non-negative, updates the release time for the resulting bus stages found matching the provided filter.
	 * @param bUpdateObject - If true, will dirty mix object and update stages on the SoundControlBusMix object in addition to updating 
	 * the audio thread proxy.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Set Control Bus Mix By Filter", meta = (
		AdvancedDisplay = "6",
		WorldContext = "WorldContextObject",
		Keywords = "set bus control class modulation modulator mix stage value filter")
	)
	static void UpdateMixByFilter(
		const UObject* WorldContextObject,
		USoundControlBusMix* Mix,
		FString AddressFilter,
		TSubclassOf<USoundModulationParameter> ParamClassFilter,
		USoundModulationParameter* ParamFilter,
		float Value = 1.0f,
		float FadeTime = -1.0f);

	/** Commits updates from a UObject definition of a bus mix to active instance in audio thread
	 * (ignored if mix has not been activated).
	 * @param Mix - Mix to update
	 * @param FadeTime - Fade time to user when interpolating between current value and new values.
	 * If negative, falls back to last fade time set on stage. If fade time never set on stage,
	 * uses attack time set on stage in mix asset.
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Update Control Bus Mix", meta = (
		WorldContext = "WorldContextObject",
		Keywords = "update set control bus mix modulation modulator")
	)
	static void UpdateMixFromObject(const UObject* WorldContextObject, USoundControlBusMix* Mix, float FadeTime = -1.0f);

	/** Commits updates from a UObject definition of a modulator (e.g. Bus, Bus Mix, LFO) to active instance in audio thread
	 * (ignored if modulator type has not been activated).
	 * @param Modulator - Modulator to update
	 */
	UFUNCTION(BlueprintCallable, Category = "Audio", DisplayName = "Update Modulator", meta = (
		WorldContext = "WorldContextObject",
		Keywords = "update set control bus mix modulation modulator")
	)
	static void UpdateModulator(const UObject* WorldContextObject, USoundModulatorBase* Modulator);
};
