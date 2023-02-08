// Copyright Epic Games, Inc. All Rights Reserved.

#include "UserSettings/EnhancedInputUserSettings.h"

#include "EnhancedPlayerInput.h"
#include "EnhancedInputModule.h"
#include "InputMappingContext.h"
#include "PlayerMappableKeySettings.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/InputDeviceSubsystem.h"
#include "EnhancedInputDeveloperSettings.h"
#include "Internationalization/Text.h"
#include "GameFramework/InputSettings.h"
#include "HAL/IConsoleManager.h"
#include "EnhancedInputLibrary.h"
#include "Algo/Find.h"
#include "NativeGameplayTags.h"

#define LOCTEXT_NAMESPACE "EnhancedInputMappableUserSettings"

namespace UE::EnhancedInput
{
	/** The name of the slot that these settings will save to */
	static const FString SETTINGS_SLOT_NAME = TEXT("EnhancedInputUserSettings");

	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_DefaultProfileIdentifier, "InputUserSettings.FailureReasons.InvalidActionName");
	static const FText DefaultProfileDisplayName = LOCTEXT("Default_Profile_name", "Default Profile");
	
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_InvalidActionName, "InputUserSettings.FailureReasons.InvalidActionName");
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_NoKeyProfile, "InputUserSettings.FailureReasons.NoKeyProfile");
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_NoMatchingMappings, "InputUserSettings.FailureReasons.NoMatchingMappings");
	
	// TODO: Break this out to somewhere else, probably the EI library. 
	static ULocalPlayer* GetLocalPlayer(const UEnhancedPlayerInput* PlayerInput)
	{
		if (PlayerInput)
		{
			if (APlayerController* PC = Cast<APlayerController>(PlayerInput->GetOuter()))
			{
				return PC->GetLocalPlayer();
			}
		}
		return nullptr;
	}

	static void DumpAllKeyProfilesToLog(const TArray<FString>& Args)
	{
		// Dump every local player subsystem's logs
		/*UEnhancedInputLibrary::ForEachSubsystem([Args](IEnhancedInputSubsystemInterface* Subsystem)
		{
			if (const UEnhancedInputUserSettings* Settings = Subsystem->GetUserSettings())
			{
				if (const UEnhancedPlayerMappableKeyProfile* Profile = Settings->GetCurrentKeyProfile())
				{
					Profile->DumpProfileToLog();	
				}
			}
		});*/
	}
	
	static FAutoConsoleCommand ConsoelCommandDumpProfileToLog(
		TEXT("EnhancedInput.DumpKeyProfileToLog"),
		TEXT(""),
		FConsoleCommandWithArgsDelegate::CreateStatic(UE::EnhancedInput::DumpAllKeyProfilesToLog));
}

///////////////////////////////////////////////////////////
// FMapPlayerKeyArgs

FMapPlayerKeyArgs::FMapPlayerKeyArgs()
	: ActionName(NAME_None)
	, Slot(EPlayerMappableKeySlot::Unspecified)
	, NewKey(EKeys::Invalid)
	, HardwareDeviceId(NAME_None)
	, bCreateMatchingSlotIfNeeded(true)
{
}

///////////////////////////////////////////////////////////
// FPlayerKeyMapping

FPlayerKeyMapping::FPlayerKeyMapping()
	: ActionName(NAME_None)
	, DisplayName(FText::GetEmpty())
	, Slot(EPlayerMappableKeySlot::Unspecified)
	, DefaultKey(EKeys::Invalid)
	, CurrentKey(EKeys::Invalid)
	, HardwareDeviceId(FHardwareDeviceIdentifier::Invalid)
{
}

// The default constructor creates an invalid mapping. Use this as a way to return references
// to an invalid mapping for BP functions
FPlayerKeyMapping FPlayerKeyMapping::InvalidMapping = FPlayerKeyMapping();

bool FPlayerKeyMapping::IsCustomized() const
{
	return CurrentKey.IsValid() && (CurrentKey != DefaultKey);
}

bool FPlayerKeyMapping::IsValid() const
{
	return ActionName.IsValid() && CurrentKey.IsValid();
}

const FKey& FPlayerKeyMapping::GetCurrentKey() const
{
	return IsCustomized() ? CurrentKey : DefaultKey;
}

const FKey& FPlayerKeyMapping::GetDefaultKey() const
{
	return DefaultKey;
}

FString FPlayerKeyMapping::ToString() const
{
	const UEnum* PlayerMappableEnumClass = StaticEnum<EPlayerMappableKeySlot>();
	check(PlayerMappableEnumClass);
	return
		FString::Printf(TEXT("Action Name: '%s'  Slot: '%s'  Default Key: '%s'  Player Mapped Key: '%s'  HardwareDevice:  '%s'"),
			*ActionName.ToString(),
			*PlayerMappableEnumClass->GetNameStringByValue(static_cast<int64>(Slot)),
			*DefaultKey.ToString(),
			*CurrentKey.ToString(),
			*HardwareDeviceId.ToString());
}

const FName FPlayerKeyMapping::GetActionName() const
{
	return ActionName;
}

const FText& FPlayerKeyMapping::GetDisplayName() const
{
	// Just in case the display name is empty on this mapping, see if we can fall back to the original mapping copy's display name
	if (DisplayName.IsEmpty())
	{
		if (const UPlayerMappableKeySettings* Settings = OriginalMappingCopy.GetPlayerMappableKeySettings())
		{
			return Settings->DisplayName;
		}
	}
	return DisplayName;
}

EPlayerMappableKeySlot FPlayerKeyMapping::GetSlot() const
{
	return Slot;
}

const FHardwareDeviceIdentifier& FPlayerKeyMapping::GetHardwareDeviceId() const
{
	return HardwareDeviceId;
}

uint32 GetTypeHash(const FPlayerKeyMapping& InMapping)
{
	uint32 Hash = 0;
	Hash = HashCombine(Hash, GetTypeHash(InMapping.ActionName));
	Hash = HashCombine(Hash, GetTypeHash(InMapping.Slot));
	Hash = HashCombine(Hash, GetTypeHash(InMapping.CurrentKey));
	Hash = HashCombine(Hash, GetTypeHash(InMapping.HardwareDeviceId));
	return Hash;
}

void FPlayerKeyMapping::ResetToDefault()
{
	CurrentKey = DefaultKey;
}

void FPlayerKeyMapping::SetCurrentKey(const FKey& NewKey)
{
	CurrentKey = NewKey;
}

bool FPlayerKeyMapping::operator==(const FPlayerKeyMapping& Other) const
{
	return
	       ActionName			== Other.ActionName
		&& Slot					== Other.Slot
		&& HardwareDeviceId		== Other.HardwareDeviceId
		&& CurrentKey		== Other.CurrentKey
		&& OriginalMappingCopy	== Other.OriginalMappingCopy;
}

bool FPlayerKeyMapping::operator!=(const FPlayerKeyMapping& Other) const
{
	return !FPlayerKeyMapping::operator==(Other);
}

///////////////////////////////////////////////////////////
// UEnhancedPlayerMappableKeyProfile

void UEnhancedPlayerMappableKeyProfile::ResetToDefault()
{
	// Reset every player mapping to the default key value
	for (TPair<FName, FKeyMappingRow>& Pair : PlayerMappedKeys)
	{
		for (FPlayerKeyMapping& Mapping : Pair.Value.Mappings)
		{
			Mapping.ResetToDefault();
		}
	}
	
	UE_LOG(LogEnhancedInput, Verbose, TEXT("Reset Player Mappable Key Profile '%s' to default values"), *ProfileIdentifier.ToString());
}

void UEnhancedPlayerMappableKeyProfile::EquipProfile()
{
	UE_LOG(LogEnhancedInput, Verbose, TEXT("Equipping Player Mappable Key Profile '%s'"), *ProfileIdentifier.ToString());
}

void UEnhancedPlayerMappableKeyProfile::UnEquipProfile()
{
	UE_LOG(LogEnhancedInput, Verbose, TEXT("Unequipping Player Mappable Key Profile '%s'"), *ProfileIdentifier.ToString());
}

void UEnhancedPlayerMappableKeyProfile::SetDisplayName(const FText& NewDisplayName)
{
	DisplayName = NewDisplayName;
}

const FGameplayTag& UEnhancedPlayerMappableKeyProfile::GetProfileIdentifer() const
{
	return ProfileIdentifier;
}

const FText& UEnhancedPlayerMappableKeyProfile::GetProfileDisplayName() const
{
	return DisplayName;
}

const TMap<FName, FKeyMappingRow>& UEnhancedPlayerMappableKeyProfile::GetPlayerMappedActions() const
{
	return PlayerMappedKeys;
}

void UEnhancedPlayerMappableKeyProfile::ResetActionMappingsToDefault(const FName InActionName)
{
	if (FKeyMappingRow* MappingRow = FindKeyMappingRowMutable(InActionName))
	{
		for (FPlayerKeyMapping& Mapping : MappingRow->Mappings)
		{
			Mapping.ResetToDefault();
		}
	}
}

FKeyMappingRow* UEnhancedPlayerMappableKeyProfile::FindKeyMappingRowMutable(const FName InActionName)
{
	return const_cast<FKeyMappingRow*>(FindKeyMappingRow(InActionName));
}

const FKeyMappingRow* UEnhancedPlayerMappableKeyProfile::FindKeyMappingRow(const FName InActionName) const
{
	return PlayerMappedKeys.Find(InActionName);
}

void UEnhancedPlayerMappableKeyProfile::DumpProfileToLog() const
{
	UE_LOG(LogEnhancedInput, Log, TEXT("%s"), *ToString());
}

FString UEnhancedPlayerMappableKeyProfile::ToString() const
{
	TStringBuilder<1024> Builder;
	Builder.Appendf(TEXT("Key Profile '%s' has %d key mappings\n"), *ProfileIdentifier.ToString(), PlayerMappedKeys.Num());
	
	for (const TPair<FName, FKeyMappingRow>& Pair : PlayerMappedKeys)
	{
		Builder.Append(Pair.Key.ToString());
		
		for (const FPlayerKeyMapping& Mapping : Pair.Value.Mappings)
		{
			Builder.Append(Mapping.ToString());
			Builder.Append("\n\t");
		}
	}

	return Builder.ToString();
}

int32 UEnhancedPlayerMappableKeyProfile::GetKeysMappedToAction(const FName ActionName, TArray<FKey>& OutKeys) const
{
	OutKeys.Reset();
	
	if (const FKeyMappingRow* MappingRow = FindKeyMappingRow(ActionName))
	{
		for (const FPlayerKeyMapping& Mapping : MappingRow->Mappings)
		{
			OutKeys.Add(Mapping.GetCurrentKey());
		}
	}
	else
	{
		UE_LOG(LogEnhancedInput, Warning, TEXT("Player Mappable Key Profile '%s' doesn't have any mappings for action '%s'"), *ProfileIdentifier.ToString(), *ActionName.ToString());
	}
	return OutKeys.Num();
}

int32 UEnhancedPlayerMappableKeyProfile::GetActionsMappedToKey(const FKey& InKey, TArray<FName>& OutMappedActionNames) const
{
	OutMappedActionNames.Reset();

	for (const TPair<FName, FKeyMappingRow>& Pair : PlayerMappedKeys)
	{
		for (const FPlayerKeyMapping& Mapping : Pair.Value.Mappings)
		{
			if (Mapping.GetCurrentKey() == InKey)
			{
				// We know that this action has the key mapped to it, so there is no need to continue checking
				// the rest of it's mappings
				OutMappedActionNames.Add(Pair.Key);
				break;
			}
		}
	}

	return OutMappedActionNames.Num();
}

void UEnhancedPlayerMappableKeyProfile::Serialize(FArchive& Ar)
{
	// See note in header!
	Super::Serialize(Ar);
}

FPlayerKeyMapping* UEnhancedPlayerMappableKeyProfile::FindKeyMapping(const FMapPlayerKeyArgs& InArgs) const
{
	// Get the current mappings for the desired action name.
	if (const FKeyMappingRow* MappingRow = PlayerMappedKeys.Find(InArgs.ActionName))
	{
		// If mapping already exists for the given slot and hardware device, then we can
		// just change that key
		const FPlayerKeyMapping* Mapping = Algo::FindByPredicate(MappingRow->Mappings, [&InArgs](const FPlayerKeyMapping& Mapping)
		{
			return Mapping.GetSlot() == InArgs.Slot && Mapping.GetHardwareDeviceId().HardwareDeviceIdentifier == InArgs.HardwareDeviceId;
		});
		return const_cast<FPlayerKeyMapping*>(Mapping);
	}
	return nullptr;
}

void UEnhancedPlayerMappableKeyProfile::K2_FindKeyMapping(FPlayerKeyMapping& OutKeyMapping, const FMapPlayerKeyArgs& InArgs) const
{
	if (FPlayerKeyMapping* FoundMapping = FindKeyMapping(InArgs))
	{
		OutKeyMapping = *FoundMapping;
	}
	else
	{
		OutKeyMapping = FPlayerKeyMapping::InvalidMapping;
	}
}

///////////////////////////////////////////////////////////
// UEnhancedInputUserSettings

UEnhancedInputUserSettings* UEnhancedInputUserSettings::LoadOrCreateSettings(UEnhancedPlayerInput* PlayerInput)
{
	UEnhancedInputUserSettings* Settings = nullptr;

	ULocalPlayer* LocalPlayer = UE::EnhancedInput::GetLocalPlayer(PlayerInput);

	if (!LocalPlayer)
	{
		UE_LOG(LogEnhancedInput, Log, TEXT("Unable to determine an owning Local Player for the given Enhanced Player Input object"));
		return nullptr;
	}
	
	// If the save game exists, load it.
	if (UGameplayStatics::DoesSaveGameExist(UE::EnhancedInput::SETTINGS_SLOT_NAME, LocalPlayer->GetLocalPlayerIndex()))
	{
		USaveGame* Slot = UGameplayStatics::LoadGameFromSlot(UE::EnhancedInput::SETTINGS_SLOT_NAME, LocalPlayer->GetLocalPlayerIndex());
		Settings = Cast<UEnhancedInputUserSettings>(Slot);
	}
	
	if (Settings == nullptr)
	{
		Settings = Cast<UEnhancedInputUserSettings>(UGameplayStatics::CreateSaveGameObject(UEnhancedInputUserSettings::StaticClass()));
	}

	Settings->Initialize(PlayerInput);
	Settings->ApplySettings();

	return Settings;
}

void UEnhancedInputUserSettings::Initialize(UEnhancedPlayerInput* InPlayerInput)
{
	OwningPlayerInput = InPlayerInput;
	ensureMsgf(GetPlayerInput(), TEXT("UEnhancedInputUserSettings is missing a player input!"));

	// Create a default key mapping profile in the case where one doesn't exist
	if (!GetCurrentKeyProfile())
	{
		FPlayerMappableKeyProfileCreationArgs Args = {};
		Args.ProfileIdentifier = UE::EnhancedInput::TAG_DefaultProfileIdentifier;
		Args.DisplayName = UE::EnhancedInput::DefaultProfileDisplayName;
		Args.bSetAsCurrentProfile = true;
		
		CreateNewKeyProfile(Args);
	}
}

void UEnhancedInputUserSettings::ApplySettings()
{
	ensureMsgf(GetPlayerInput(), TEXT("UEnhancedInputUserSettings is missing a player input!"));
	UE_LOG(LogEnhancedInput, Verbose, TEXT("Enhanced Input User Settings applied!"));
}

void UEnhancedInputUserSettings::SaveSettings()
{
	ensureMsgf(GetPlayerInput(), TEXT("UEnhancedInputUserSettings is missing a player input!"));
	
	if (ULocalPlayer* OwningPlayer = GetLocalPlayer())
	{
		UGameplayStatics::SaveGameToSlot(this, UE::EnhancedInput::SETTINGS_SLOT_NAME, OwningPlayer->GetLocalPlayerIndex());
		UE_LOG(LogEnhancedInput, Verbose, TEXT("Enhanced Input User Settings saved!"));
	}
	else
	{
		UE_LOG(LogEnhancedInput, Warning, TEXT("Attempting to save Enhanced Input User settings without an owning local player!"));
	}
}

namespace UE::EnhancedInput
{
	static const int32 GPlayerMappableSaveVersion = 1;

	/** Struct used to store info about the mappable profile subobjects */
	struct FMappableKeysHeader
	{
		friend FArchive& operator<<(FArchive& Ar, FMappableKeysHeader& Header)
		{
			Ar << Header.ProfileIdentifier;
			Ar << Header.ClassPath;
			Ar << Header.ObjectPath;
			return Ar;
		}

		FGameplayTag ProfileIdentifier;
		FString ClassPath;
		FString ObjectPath;
	};
}

void UEnhancedInputUserSettings::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	if (IsTemplate() || Ar.IsCountingMemory())
	{
		return;
	}
	
	int32 SaveVersion = UE::EnhancedInput::GPlayerMappableSaveVersion;
	Ar << SaveVersion;
	
	// Detect a mis-match of byte streams, i.e. file corruption
	ensure(SaveVersion == UE::EnhancedInput::GPlayerMappableSaveVersion);
	
	TArray<UE::EnhancedInput::FMappableKeysHeader> Headers;
	if (Ar.IsSaving())
	{
		UObject* Outer = this;
		
		for (TPair<FGameplayTag, UEnhancedPlayerMappableKeyProfile*> ProfilePair : SavedKeyProfiles)
		{
			Headers.Push({ ProfilePair.Key, ProfilePair.Value->GetClass()->GetPathName(), ProfilePair.Value->GetPathName(Outer) });
		}
	}
	
	Ar << Headers;

	if (Ar.IsLoading())
	{
		for (UE::EnhancedInput::FMappableKeysHeader& Header : Headers)
		{
			if (const UClass* FoundClass = FindObject<UClass>(nullptr, *Header.ClassPath))
			{
				UEnhancedPlayerMappableKeyProfile* NewProfile = NewObject<UEnhancedPlayerMappableKeyProfile>(/* outer */ this, /* class */ FoundClass);
				SavedKeyProfiles.Add(Header.ProfileIdentifier, NewProfile);
			}
		}
	}

	FString SavedObjectTerminator = TEXT("ObjectEnd");
	
	for (TPair<FGameplayTag, UEnhancedPlayerMappableKeyProfile*> ProfilePair : SavedKeyProfiles)
	{
		ProfilePair.Value->Serialize(Ar);

		// Save a terminator after each subobject
		Ar << SavedObjectTerminator;
		
		if (!ensure(SavedObjectTerminator == TEXT("ObjectEnd")))
		{
			UE_LOG(LogEnhancedInput, Error, TEXT("Serialization size mismatch! Possible over-read or over-write of this buffer."));
			break;
		}
	}
}

UEnhancedPlayerInput* UEnhancedInputUserSettings::GetPlayerInput() const
{
	return OwningPlayerInput;
}

APlayerController* UEnhancedInputUserSettings::GetPlayerController() const
{
	if (UEnhancedPlayerInput* PlayerInput = GetPlayerInput())
	{
		return Cast<APlayerController>(PlayerInput->GetOuter());
	}
	return nullptr;
}

ULocalPlayer* UEnhancedInputUserSettings::GetLocalPlayer() const
{
	if (APlayerController* PC = GetPlayerController())
	{
		return PC->GetLocalPlayer();
	}
	return nullptr;
}

void UEnhancedInputUserSettings::MapPlayerKey(const FMapPlayerKeyArgs& InArgs, FGameplayTagContainer& FailureReason)
{
	if (!InArgs.ActionName.IsValid())
	{
		FailureReason.AddTag(UE::EnhancedInput::TAG_InvalidActionName);
		return;
	}
	
	// Get the key profile that was specific
	UEnhancedPlayerMappableKeyProfile* KeyProfile = InArgs.ProfileId.IsValid() ? GetKeyProfileWithIdentifier(InArgs.ProfileId) : GetCurrentKeyProfile();
	if (!KeyProfile)
	{
		FailureReason.AddTag(UE::EnhancedInput::TAG_NoKeyProfile);
		return;
	}

	// If this mapping already exists, we can simply change it's key and be done
	if (FPlayerKeyMapping* FoundMapping = KeyProfile->FindKeyMapping(InArgs))
	{
		// Then set the player mapped key
		FoundMapping->SetCurrentKey(InArgs.NewKey);
		OnSettingsChanged.Broadcast(this);
		return;
	}
	// If it doesn't exist, then we need to make it if there is a valid action name
	else if (FKeyMappingRow* MappingRow = KeyProfile->PlayerMappedKeys.Find(InArgs.ActionName))
	{
		// If one doesn't exist, then we need to create a new mapping in the given
		// slot. 
		// In order to populate the default values correctly, we only do this if we know that
		// mappings exist for it
		const int32 NumMappings = MappingRow->Mappings.Num();
		if (InArgs.bCreateMatchingSlotIfNeeded && NumMappings > 0)
		{
			const auto ExistingMapping = MappingRow->Mappings.begin();
			
			// Add a default mapping to this row
			FPlayerKeyMapping PlayerMappingData = {};
			PlayerMappingData.ActionName = InArgs.ActionName;
			PlayerMappingData.Slot = InArgs.Slot;

			// If there is some valid hardware then keep track of that
			if (const UInputPlatformSettings* PlatformSettings = UInputPlatformSettings::Get())
			{
				if (const FHardwareDeviceIdentifier* Hardware = PlatformSettings->GetHardwareDeviceForClassName(InArgs.HardwareDeviceId))
				{
					PlayerMappingData.HardwareDeviceId = *Hardware;	
				}
				else
				{
					UE_LOG(LogEnhancedInput, Log, TEXT("[UEnhancedInputUserSettings::MapPlayerKey] Unable to find a matching Hardware Device Identifier with the HardwareDeviceId of '%s'"), *InArgs.HardwareDeviceId.ToString());
				}
			}
			
			// This mapping never existed in the default IMC, so the default mapping will be the default
			// EKeys::Invalid and we only need to track the player mapped key
			PlayerMappingData.SetCurrentKey(InArgs.NewKey);
			PlayerMappingData.DisplayName = ExistingMapping->DisplayName;
			MappingRow->Mappings.Add(PlayerMappingData);
			OnSettingsChanged.Broadcast(this);
		}
	}
}

void UEnhancedInputUserSettings::UnMapPlayerKey(const FMapPlayerKeyArgs& InArgs, FGameplayTagContainer& FailureReason)
{
	if (!InArgs.ActionName.IsValid())
	{
		FailureReason.AddTag(UE::EnhancedInput::TAG_InvalidActionName);
		return;
	}
	
	// Get the key profile that was specified
	UEnhancedPlayerMappableKeyProfile* KeyProfile = InArgs.ProfileId.IsValid() ? GetKeyProfileWithIdentifier(InArgs.ProfileId) : GetCurrentKeyProfile();
	if (!KeyProfile)
	{
		FailureReason.AddTag(UE::EnhancedInput::TAG_NoKeyProfile);
		return;
	}

	if (FPlayerKeyMapping* FoundMapping = KeyProfile->FindKeyMapping(InArgs))
	{
		// Then set the player mapped key
		FoundMapping->ResetToDefault();
		OnSettingsChanged.Broadcast(this);
		
		UE_LOG(LogEnhancedInput, Verbose, TEXT("[UEnhancedInputUserSettings::MapPlayerKey] Reset keymapping to default: '%s'"), *FoundMapping->ToString());
		
		return;
	}
	// if a mapping doesn't exist, then we can't unmap it
	else
	{
		FailureReason.AddTag(UE::EnhancedInput::TAG_NoMatchingMappings);
	}
}

const TSet<FPlayerKeyMapping>& UEnhancedInputUserSettings::FindMappingsForAction(const FName ActionName) const
{
	if (UEnhancedPlayerMappableKeyProfile* KeyProfile = GetCurrentKeyProfile())
	{
		FKeyMappingRow& ExistingMappings = KeyProfile->PlayerMappedKeys.FindOrAdd(ActionName);
		return ExistingMappings.Mappings;
	}
	
	UE_LOG(LogEnhancedInput, Error, TEXT("There is no current mappable key profile! No mappings will be returned."));
	
	static TSet<FPlayerKeyMapping> EmptyMappings;
	return EmptyMappings;
}

const FPlayerKeyMapping* UEnhancedInputUserSettings::FindCurrentMappingForSlot(const FName ActionName, const EPlayerMappableKeySlot InSlot) const
{
	const TSet<FPlayerKeyMapping>& AllMappings = FindMappingsForAction(ActionName);
	for (const FPlayerKeyMapping& Mapping : AllMappings)
	{
		if (Mapping.Slot == InSlot)
		{
			return &Mapping;
		}
	}
	
	UE_LOG(LogEnhancedInput, Warning, TEXT("No mappings could be found for action '%s'"), *ActionName.ToString());
	return nullptr;
}

bool UEnhancedInputUserSettings::SetKeyProfile(const FGameplayTag& InProfileId)
{
	UEnhancedPlayerInput* PlayerInput = GetPlayerInput();
	if (!PlayerInput)
	{
		UE_LOG(LogEnhancedInput, Error, TEXT("Failed to find the PlayerInput associated with the Enhanced Input user settings!"), *InProfileId.ToString());
		return false;
	}

	const FGameplayTag OriginalProfileId = CurrentProfileIdentifier;
	UEnhancedPlayerMappableKeyProfile* OriginalProfile = GetCurrentKeyProfile();
	
	if (const TObjectPtr<UEnhancedPlayerMappableKeyProfile>* NewProfile = SavedKeyProfiles.Find(InProfileId))
	{
		// Unequip the original profile if there was one
		if (OriginalProfile)
		{
			OriginalProfile->UnEquipProfile();
		}

		// Equip the new profile
		NewProfile->Get()->EquipProfile();

		// Keep track of what the current profile is now
		CurrentProfileIdentifier = InProfileId;

		// Let any listeners know that the mapping profile has changed
		OnKeyProfileChanged.Broadcast(NewProfile->Get());
	}
	else
	{
		UE_LOG(LogEnhancedInput, Error, TEXT("No profile with name '%s' exists! Did you call CreateNewKeyProfile at any point?"), *InProfileId.ToString());
		return false;
	}

	UE_LOG(LogEnhancedInput, Verbose, TEXT("Successfully changed Key Profile from '%s' to '%s'"), *OriginalProfileId.ToString(), *CurrentProfileIdentifier.ToString());
	return true;
}

const FGameplayTag& UEnhancedInputUserSettings::GetCurrentKeyProfileIdentifier() const
{
	return CurrentProfileIdentifier;
}

UEnhancedPlayerMappableKeyProfile* UEnhancedInputUserSettings::GetCurrentKeyProfile() const
{
	return GetKeyProfileWithIdentifier(CurrentProfileIdentifier);
}

FPlayerMappableKeyProfileCreationArgs::FPlayerMappableKeyProfileCreationArgs()
	: ProfileType(GetDefault<UEnhancedInputDeveloperSettings>()->DefaultPlayerMappableKeyProfileClass.Get())
	, ProfileIdentifier(FGameplayTag::EmptyTag)
	, DisplayName(FText::GetEmpty())
	, bSetAsCurrentProfile(true)
{
}

const TMap<FGameplayTag, TObjectPtr<UEnhancedPlayerMappableKeyProfile>>& UEnhancedInputUserSettings::GetAllSavedKeyProfiles() const
{
	return SavedKeyProfiles;
}

UEnhancedPlayerMappableKeyProfile* UEnhancedInputUserSettings::CreateNewKeyProfile(const FPlayerMappableKeyProfileCreationArgs& InArgs)
{
	if (!InArgs.ProfileType)
	{
		UE_LOG(LogEnhancedInput, Error, TEXT("Invalid ProfileType given CreateNewKeyProfile!"));
		return nullptr;
	}
	
	UEnhancedPlayerMappableKeyProfile* OutProfile = GetKeyProfileWithIdentifier(InArgs.ProfileIdentifier);
	
	// Check for an existing profile of this name
	if (OutProfile)
	{
		UE_LOG(LogEnhancedInput, Warning, TEXT("A key profile with the name '%s' already exists! Use a different name."), *InArgs.ProfileIdentifier.ToString());
	}
	else
	{
		// Create a new mapping profile
		OutProfile = NewObject<UEnhancedPlayerMappableKeyProfile>(/* outer */ this, /* class */ InArgs.ProfileType);
		OutProfile->ProfileIdentifier = InArgs.ProfileIdentifier;
		OutProfile->DisplayName = InArgs.DisplayName;
		
		SavedKeyProfiles.Add(InArgs.ProfileIdentifier, OutProfile);
	}
	
	// set as current
	if (InArgs.bSetAsCurrentProfile)
	{
		SetKeyProfile(InArgs.ProfileIdentifier);
	}

	UE_LOG(LogEnhancedInput, Verbose, TEXT("Completed creation of key mapping profile '%s'"), *OutProfile->ProfileIdentifier.ToString());
	
	return OutProfile;
}

UEnhancedPlayerMappableKeyProfile* UEnhancedInputUserSettings::GetKeyProfileWithIdentifier(const FGameplayTag& ProfileId) const
{
	if (const TObjectPtr<UEnhancedPlayerMappableKeyProfile>* ExistingProfile = SavedKeyProfiles.Find(ProfileId))
	{		
		return ExistingProfile->Get();
	}
	return nullptr;
}

bool UEnhancedInputUserSettings::RegisterInputMappingContexts(const TSet<UInputMappingContext*>& MappingContexts)
{
	bool bResult = false;

	for (UInputMappingContext* IMC : MappingContexts)
	{
		bResult |= RegisterInputMappingContext(IMC);
	}

	return bResult;
}

bool UEnhancedInputUserSettings::RegisterInputMappingContext(UInputMappingContext* IMC)
{
	if (!IMC)
	{
		UE_LOG(LogEnhancedInput, Error, TEXT("Attempting to register a null mapping context with the user settings!"));
		ensure(false);
		return false;
	}

	// There is no need to register an IMC if it already is
	if( RegisteredMappingContexts.Contains(IMC))
	{
		return false;
	}
	
	const FSetElementId Res = RegisteredMappingContexts.Add(IMC);

	UEnhancedPlayerMappableKeyProfile* CurrentProfile = GetCurrentKeyProfile();
	if (!CurrentProfile)
	{
		UE_LOG(LogEnhancedInput, Error, TEXT("There is not an active key profile!"));
		ensure(false);
		return false;
	}
	
	for (const FEnhancedActionKeyMapping& KeyMapping : IMC->GetMappings())
	{
		// Skip over non-player mappable keys
		if (!KeyMapping.IsPlayerMappable())
		{
			continue;
		}

		// Get the unique FName for the "Action name". This is set on the UInputAction
		// but can be overriden on each FEnhancedActionKeyMapping to create multiple
		// mapping options for a single Input Action. 
		const FName ActionName = KeyMapping.GetMappingName();

		// If a mapping row exists for this action name, then check if
		bool bMappingIsInitalized = false;
		if (FKeyMappingRow* ExistingMappings = CurrentProfile->PlayerMappedKeys.Find(ActionName))
		{
			// Check if we actually have to initalize a new mapping or not
			for (FPlayerKeyMapping& ExistingMapping : ExistingMappings->Mappings)
			{
				// If the original mapping ha already been set, then we know that it's initalized already.
				// If we don't do this, then registering an IMC will overrwrite whatever saved settings the user has made
				if (ExistingMapping.OriginalMappingCopy == KeyMapping)
				{
					bMappingIsInitalized = true;
					break;
				}
			}
		}
		
		if (!bMappingIsInitalized)
		{
			FKeyMappingRow& MappingRow = CurrentProfile->PlayerMappedKeys.Add(ActionName);		

			// Add a default mapping to this row
			FPlayerKeyMapping PlayerMappingData = {};
			PlayerMappingData.ActionName = ActionName;
			PlayerMappingData.DefaultKey = KeyMapping.Key;
			
			if (UPlayerMappableKeySettings* Settings = KeyMapping.GetPlayerMappableKeySettings())
			{
				PlayerMappingData.DisplayName = Settings->DisplayName;
			}
			
			PlayerMappingData.OriginalMappingCopy = KeyMapping;

			// By default, the slot will be determined by how many mappings this action has already.
			// So if this is the first default mapping, then this will be EPlayerMappableKeySlot::First,
			// if this is the second element going into this set then it will be EPlayerMappableKeySlot::Second
			// and so on
			const uint8 DesiredSlot = FMath::Min<uint8>(static_cast<uint8>(MappingRow.Mappings.Num()), static_cast<uint8>(EPlayerMappableKeySlot::Max));
			PlayerMappingData.Slot = (EPlayerMappableKeySlot) DesiredSlot;
		
			MappingRow.Mappings.Add(PlayerMappingData);
		}
	}

	if (Res.IsValidId())
	{
		OnMappingContextRegistered.Broadcast(IMC);	
	}

	UE_LOG(LogEnhancedInput, Verbose, TEXT("Registered IMC with UEnhancedInputUserSettings: %s"), *IMC->GetFName().ToString());
	return Res.IsValidId();
}

bool UEnhancedInputUserSettings::UnregisterInputMappingContext(const UInputMappingContext* IMC)
{
	return RegisteredMappingContexts.Remove(IMC) != INDEX_NONE;
}

bool UEnhancedInputUserSettings::UnregisterInputMappingContexts(const TSet<UInputMappingContext*>& MappingContexts)
{
	bool bResult = false;

	for (UInputMappingContext* IMC : MappingContexts)
	{
		bResult |= UnregisterInputMappingContext(IMC);
	}

	return bResult;
}

const TSet<TObjectPtr<UInputMappingContext>>& UEnhancedInputUserSettings::GetRegisteredInputMappingContexts() const
{
	return RegisteredMappingContexts;
}

bool UEnhancedInputUserSettings::IsMappingContextRegistered(const UInputMappingContext* IMC) const
{
	return RegisteredMappingContexts.Contains(IMC);
}

#undef LOCTEXT_NAMESPACE