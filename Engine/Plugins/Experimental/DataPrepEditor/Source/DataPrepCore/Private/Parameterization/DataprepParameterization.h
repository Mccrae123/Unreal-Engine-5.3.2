// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Parameterization/DataprepParameterizationUtils.h"

#include "Containers/ContainerAllocationPolicies.h"
#include "CoreMinimal.h"
#include "Delegates/DelegateCombinations.h"
#include "Delegates/IDelegateInstance.h"
#include "Engine/UserDefinedStruct.h"
#include "Templates/SharedPointer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "DataprepParameterization.generated.h"

class UDataprepParameterizableObject;
class UProperty;

USTRUCT()
struct FDataprepParameterizationBinding
{
	GENERATED_BODY()

	FDataprepParameterizationBinding()
		: ObjectBinded( nullptr )
		, PropertyChain()
		, ValueType( nullptr )
	{}

	FDataprepParameterizationBinding(UDataprepParameterizableObject* InObjectBinded, TArray<FDataprepPropertyLink> InPropertyChain)
		: ObjectBinded( InObjectBinded )
		, PropertyChain( MoveTemp( InPropertyChain ) )
		, ValueType( nullptr )
	{
		if ( PropertyChain.Num() > 0 )
		{
			if ( UProperty* Property = PropertyChain.Last().CachedProperty.Get() )
			{
				ValueType = Property->GetClass();
			}
		}
	}

	FDataprepParameterizationBinding(FDataprepParameterizationBinding&&) = default;
	FDataprepParameterizationBinding(const FDataprepParameterizationBinding&) = default;
	FDataprepParameterizationBinding& operator=(FDataprepParameterizationBinding&&) = default;
	FDataprepParameterizationBinding& operator=(const FDataprepParameterizationBinding&) = default;

	bool operator==(const FDataprepParameterizationBinding& Other) const;

	UPROPERTY()
	UDataprepParameterizableObject* ObjectBinded;

	UPROPERTY()
	TArray<FDataprepPropertyLink> PropertyChain;

	// The class of the property managing the value
	UPROPERTY()
	UClass* ValueType;
};

uint32 GetTypeHash(const FDataprepParameterizationBinding& Binding);

uint32 GetTypeHash(const TArray<FDataprepPropertyLink>& PropertyLinks);

/** 
 * A override to defines how the map's pairs are hashed and compared.
 * This allow us to have some map that work with TSharedRef but compare the object value instead of the pointer
 */
struct FDataprepParametrizationBindingMapKeyFuncs : TDefaultMapHashableKeyFuncs<TSharedRef<FDataprepParameterizationBinding>, FName, false>
{
	static FORCEINLINE bool Matches(KeyInitType A, KeyInitType B)
	{
		return A.Get() == B.Get();
	}

	static FORCEINLINE uint32 GetKeyHash(KeyInitType Key)
	{
		return GetTypeHash( Key.Get() );
	}
};

/** 
 * A override to defines how the set values are hashed and compared.
 * This allow us to have some set that work with TSharedRef but compare the object value instead of the pointer
 */
struct FDataprepParametrizationBindingSetKeyFuncs : DefaultKeyFuncs<TSharedRef<FDataprepParameterizationBinding>>
{
	template<typename ComparableKey>
	static FORCEINLINE bool Matches(KeyInitType A, ComparableKey B)
	{
		return A.Get() == B.Get();
	}

	static FORCEINLINE uint32 GetKeyHash(KeyInitType Key)
	{
		return GetTypeHash(Key.Get());
	}
};

/**
 * Encapsulate the unidirectionality necessary for a constant cost of access to the data related to the bindings
 */
UCLASS(MinimalAPI, Experimental)
class UDataprepParameterizationBindings : public UObject
{
public:

	GENERATED_BODY()

	using FBindingToParameterNameMap = TMap<TSharedRef<FDataprepParameterizationBinding>, FName, FDefaultSetAllocator, FDataprepParametrizationBindingMapKeyFuncs>;
	using FSetOfBinding = TSet<TSharedRef<FDataprepParameterizationBinding>, FDataprepParametrizationBindingSetKeyFuncs>;

	/**
	 * Does the data structure contains this binding
	 */
	bool ContainsBinding(const TSharedRef<FDataprepParameterizationBinding>& Binding) const;

	/**
	 * Return the name of the parameter for a binding
	 */
	FName GetParameterNameForBinding(const TSharedRef<FDataprepParameterizationBinding>& Binding) const;

	/**
	 * Return a valid ptr if the object had some bindings
	 */
	const FSetOfBinding* GetBindingsFromObject(const UDataprepParameterizableObject* Object) const;

	/**
	 * Get the bindings from a parameter
	 * Return nullptr if the parameter doesn't exist
	 */
	const FSetOfBinding* GetBindingsFromParameter(const FName& ParameterName) const;

	/**
	 * Does the data structure has some bindings for the parameter name
	 */
	bool HasBindingsForParameter(const FName& ParameterName) const;

	/**
	 * Add a binding and map it to the parameter
	 */
	void Add(const TSharedRef<FDataprepParameterizationBinding>& Binding, const FName& ParameterName);

	/**
	 * Remove a binding.
	 * @return The name of the parameter the binding was associated with
	 */
	FName RemoveBinding(const TSharedRef<FDataprepParameterizationBinding>& Binding);

	/**
	 * Remove all the bindings from a object
	 * @return The name of the parameters that were associated to the binding of the object
	 */
	TSet<FName> RemoveAllBindingsFromObject(UDataprepParameterizableObject* Object);

	const FBindingToParameterNameMap& GetBindingToParameterName() const;

	// UObject interface
	virtual void Serialize(FArchive& Ar) override;
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	// End of UObject interface

private:
	/**
	 * Do the actual serialization when saving
	 */
	void Save(FArchive& Ar);

	/**
	 * Do the actual serialization when reloading
	 */
	void Load(FArchive& Ar);

	/** Core storage also track a binding to it's parameter name */
	FBindingToParameterNameMap BindingToParameterName;

	/** Track the name usage for parameters */
	TMap<FName, FSetOfBinding> NameToBindings;

	/** Track which binding a object has */
	TMap<UDataprepParameterizableObject*, FSetOfBinding> ObjectToBindings;
};


/** 
 * The DataprepParameterization contains the data for the parameterization of a pipeline
 */
UCLASS(MinimalAPI, Experimental)
class UDataprepParameterization : public UObject
{
public:
	GENERATED_BODY()

	UDataprepParameterization();
	~UDataprepParameterization();

	// UObject interface
	virtual void PostInitProperties() override;
	virtual void PostLoad() override;
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostEditUndo() override;
	// End of UObject interface

	void OnObjectModified(UObject* Object);

	UObject* GetDefaultObject();

	bool BindObjectProperty(UDataprepParameterizableObject* Object, const TArray<FDataprepPropertyLink>& PropertyChain, const FName& Name);

	bool IsObjectPropertyBinded(UDataprepParameterizableObject* Object, const TArray<FDataprepPropertyLink>& PropertyChain) const;

	FName GetNameOfParameterForObjectProperty(UDataprepParameterizableObject* Object, const TArray<struct FDataprepPropertyLink>& PropertyChain) const;

	void RemoveBindedObjectProperty(UDataprepParameterizableObject* Object, const TArray<FDataprepPropertyLink>& PropertyChain);

	void RemoveBindingFromObjects(TArray<UDataprepParameterizableObject*> Objects);

	void OnObjectPostEdit(UDataprepParameterizableObject* Object, const TArray<FDataprepPropertyLink>& PropertyChain, EPropertyChangeType::Type ChangeType);

	void GetExistingParameterNamesForType(UClass* PropertyClass,TSet<FString>& OutValidExistingNames, TSet<FString>& OutInvalidNames) const;

private:

	/**
	 * Generate the Custom Container Class
	 */
	void GenerateClass();

	/**
	 * Update the Custom Container Class to a newer version
	 */
	void UpdateClass();

	/**
	 * Do the process of regenerating the Custom Container Class and the data of its default object from the serialized data
	 */
	void LoadParameterization();

	/**
	 * Remove the current Custom Container Class so that we can create a new one
	 */
	void PrepareCustomClassForNewClassGeneration();

	/**
	 * Do the actual creation of the class object
	 */
	void CreateClassObject();

	/**
	 * Do reinstancing of the objects created from the Custom Container Class
	 * @param OldClass The previous Custom Constainer Class
	 * @param bMigrateData Should we migrate the data from the old instances to the new instances
	 */
	void DoReinstancing(UClass* OldClass, bool bMigrateData = true);

	/**
	 * Try adding a binded property to the parameterization class
	 * @return false if the binding is no more valid
	 */
	UProperty* AddPropertyToClass(const FName& ParameterisationPropertyName, UProperty& Property);

	/**
	 * Get a new value for the parameterization from it's associated binding
	 */
	void UpdateParameterizationFromBinding(const TSharedRef<FDataprepParameterizationBinding>& Binding);

	/**
	 * Push the value of the parametrization to the bindings
	 */
	void PushParametrizationValueToBindings(FName ParameterName);

	/**
	 * Do the actual removing of a binding
	 * @return True if binding was remove
	 */
	bool RemoveBinding(const TSharedRef<FDataprepParameterizationBinding>& Binding, bool& bOutClassNeedUpdate);

public:

	static const FName MetadataClassGeneratorName;

private:

	// The containers for the bindings
	UPROPERTY()
	UDataprepParameterizationBindings* BindingsContainer;

	UPROPERTY(Transient, NonTransactional)
	TMap<FName, UProperty*> NameToParameterizationProperty;

	UPROPERTY(Transient, NonTransactional)
	UClass* CustomContainerClass;

	UPROPERTY(Transient, NonTransactional)
	UObject* DefaultParameterisation;

	/** 
	 * This is used only to store a serialization of the values of the parameterization since we can't save our custom container class
	 */
	UPROPERTY()
	TArray<uint8> ParameterizationStorage;

	DECLARE_EVENT(UDataprepParameterization, FOnCustomClassAboutToBeUpdated);
	FOnCustomClassAboutToBeUpdated OnCustomClassAboutToBeUpdated;

	using FMapOldToNewObjects = TMap<UObject*, UObject*>;
	DECLARE_EVENT_OneParam(UDataprepParameterization, FOnCustomClassWasUpdated, const FMapOldToNewObjects& /** OldToNew */);
	FOnCustomClassWasUpdated OnCustomClassWasUpdated;

	DECLARE_EVENT(UDataprepParameterization,FOnTellInstancesToReloadTheirSerializedData);
	FOnTellInstancesToReloadTheirSerializedData OnTellInstancesToReloadTheirSerializedData;

	// the dataprep instance need some special access to the dataprep parameterization
	friend class UDataprepParameterizationInstance;

	FDelegateHandle OnObjectModifiedHandle;
};


UCLASS(MinimalAPI, Experimental)
class UDataprepParameterizationInstance : public UObject
{
public:
	GENERATED_BODY()

	UDataprepParameterizationInstance();
	~UDataprepParameterizationInstance();

	// UObject interface
	virtual void PostLoad() override;
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostEditUndo() override;
	// End of UObject interface

	void OnObjectModified(UObject* Object);

	// Apply the parameterization to a copy of the source pipeline
	void ApplyParameterization(const TMap<UObject*, UObject*>& SourceToCopy);

	void SetParameterizationSource(UDataprepParameterization& Parameterization);

	UObject* GetParameterizationInstance() { return ParameterizationInstance; }

private:

	void CustomClassAboutToBeUpdated();

	/**
	 * Used as call back for event coming from the source parameterization
	 * Change the parametrization instance to the new object after a reinstancing
	 */
	void CustomClassWasUpdated(const TMap<UObject*, UObject*>& OldToNew);

	/**
	 * Load the parameterization data on the instance from the ParameterizationInstanceStorage
	 */
	void LoadParameterization();

	/**
	 * Setup the parameterization instance so that we can react to event coming from the source parameterization
	 */
	void SetupCallbacksFromSourceParameterisation();

	/**
	 * Clean the parameterization instance so that we can bind to a new source parameterization
	 */
	void UndoSetupForCallbacksFromParameterization();

	// The parameterization from which this instance was constructed
	UPROPERTY()
	UDataprepParameterization* SourceParameterization;

	// The actual object on which the parameterization data is stored
	UPROPERTY(Transient, NonTransactional)
	UObject* ParameterizationInstance;

	// This is used only to store a serialization of the values of the parameterization since we can't save the custom class
	UPROPERTY()
	TArray<uint8> ParameterizationInstanceStorage;

	FDelegateHandle OnObjectModifiedHandle;
};

