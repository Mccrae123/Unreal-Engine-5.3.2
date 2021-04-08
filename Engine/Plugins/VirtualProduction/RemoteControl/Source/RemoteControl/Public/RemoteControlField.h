// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "RemoteControlEntity.h"
#include "RemoteControlFieldPath.h"
#include "RemoteControlProtocolBinding.h"
#include "RemoteControlField.generated.h"

/**
 * The type of the exposed field.
 */
UENUM()
enum class EExposedFieldType : uint8
{
	Invalid,
	Property,
	Function
};

/**
 * Represents a property or function that has been exposed to remote control.
 */
USTRUCT(BlueprintType)
struct REMOTECONTROL_API FRemoteControlField : public FRemoteControlEntity
{
	GENERATED_BODY()

	FRemoteControlField() = default;

	/**
	 * Resolve the field's owners using the section's top level objects.
	 * @param SectionObjects The top level objects of the section.
	 * @return The list of UObjects that own the exposed field.
	 */
	UE_DEPRECATED(4.27, "Please use GetBoundObjects.")
	TArray<UObject*> ResolveFieldOwners(const TArray<UObject*>& SectionObjects) const;

	//~ Begin FRemoteControlEntity interface
	virtual void BindObject(UObject* InObjectToBind) override;
	virtual bool CanBindObject(const UObject* InObjectToBind) const override;
	//~ End FRemoteControlEntity interface

public:
	/**
	 * The field's type.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RemoteControlEntity")
	EExposedFieldType FieldType = EExposedFieldType::Invalid;

	/**
	 * The exposed field's name.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RemoteControlEntity")
	FName FieldName;

	/**
	 * Path information pointing to this field
	 */
	UPROPERTY()
	FRCFieldPathInfo FieldPathInfo;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TArray<FString> ComponentHierarchy_DEPRECATED;

#endif

	/**
	 * Stores the bound protocols for this exposed field
	 * It could store any types of the implemented protocols such as DMX, OSC, MIDI, etc
	 * The map holds protocol bindings stores the protocol mapping and protocol-specific mapping
	 */
	UPROPERTY()
	TSet<FRemoteControlProtocolBinding> ProtocolBinding;
	
protected:
	/**
	 * The class of the object that can have this property.
	 */
	UPROPERTY()
	FSoftClassPath OwnerClass;

protected:
	FRemoteControlField(URemoteControlPreset* InPreset, EExposedFieldType InType, FName InLabel, FRCFieldPathInfo InFieldPathInfo, const TArray<URemoteControlBinding*> InBindings);

	void PostSerialize(const FArchive& Ar);
	
private:
#if WITH_EDITORONLY_DATA
	/**
	 * Resolve the field's owners using the section's top level objects and the deprecated component hierarchy.
	 * @param SectionObjects The top level objects of the section.
	 * @return The list of UObjects that own the exposed field.
	 */
	TArray<UObject*> ResolveFieldOwnersUsingComponentHierarchy(const TArray<UObject*>& SectionObjects) const;
#endif
};

/**
 * Represents a property exposed to remote control.
 */
USTRUCT(BlueprintType)
struct REMOTECONTROL_API FRemoteControlProperty : public FRemoteControlField
{
public:
	GENERATED_BODY()

	FRemoteControlProperty() = default;

	UE_DEPRECATED(4.27, "This constructor is deprecated. Use the other constructor.")
	FRemoteControlProperty(FName InLabel, FRCFieldPathInfo FieldPathInfo, TArray<FString> InComponentHierarchy);

	FRemoteControlProperty(URemoteControlPreset* InPreset, FName InLabel, FRCFieldPathInfo InFieldPathInfo, const TArray<URemoteControlBinding*>& InBindings);

	//~ Begin FRemoteControlEntity interface
	virtual uint32 GetUnderlyingEntityIdentifier() const override;
	virtual UClass* GetSupportedBindingClass() const override;
	virtual bool IsBound() const override;
	//~ End FRemoteControlEntity interface

	/**
	 * Get the underlying property.
	 * @return The exposed property or nullptr if it couldn't be resolved.
	 * @note This field's binding must be valid to get the property.
	 */
	FProperty* GetProperty() const;
	
	/** Handle metadata initialization. */
	void PostSerialize(const FArchive& Ar);
	
public:
	/** Key for the metadata's Min entry. */
	static FName MetadataKey_Min;
	/** Key for the metadata's Max entry. */
	static FName MetadataKey_Max;
	
private:
	/** Assign the default metadata for this exposed property. (ie. Min, Max...) */
	void InitializeMetadata();
};

/**
 * Represents a function exposed to remote control.
 */
USTRUCT(BlueprintType)
struct REMOTECONTROL_API FRemoteControlFunction : public FRemoteControlField
{
	GENERATED_BODY()

	FRemoteControlFunction() = default;

	UE_DEPRECATED(4.27, "This constructor is deprecated. Use the other constructor.")
	FRemoteControlFunction(FName InLabel, FRCFieldPathInfo FieldPathInfo, UFunction* InFunction);

	FRemoteControlFunction(URemoteControlPreset* InPreset, FName InLabel, FRCFieldPathInfo InFieldPathInfo, UFunction* InFunction, const TArray<URemoteControlBinding*>& InBindings);

	//~ Begin FRemoteControlEntity interface
	virtual uint32 GetUnderlyingEntityIdentifier() const override;
	virtual UClass* GetSupportedBindingClass() const override;
	virtual bool IsBound() const override;
	//~ End FRemoteControlEntity interface

	friend FArchive& operator<<(FArchive& Ar, FRemoteControlFunction& RCFunction);
	bool Serialize(FArchive& Ar);
	void PostSerialize(const FArchive& Ar);
	
public:
	/**
	 * The exposed function.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "RemoteControlEntity")
	UFunction* Function = nullptr;

	/**
	 * The function arguments.
	 */
	TSharedPtr<class FStructOnScope> FunctionArguments;

private:
	/** Parse function metadata to get the function`s default parameters */
	void AssignDefaultFunctionArguments();
};

template<> struct TStructOpsTypeTraits<FRemoteControlFunction> : public TStructOpsTypeTraitsBase2<FRemoteControlFunction>
{
	enum
	{
		WithSerializer = true,
		WithPostSerialize = true
	};
};

template<> struct TStructOpsTypeTraits<FRemoteControlProperty> : public TStructOpsTypeTraitsBase2<FRemoteControlProperty>
{
	enum
	{
		WithPostSerialize = true
    };
};
