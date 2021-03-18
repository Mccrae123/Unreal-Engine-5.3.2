// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ReloadUtilities.cpp: Helpers for reloading
=============================================================================*/

#include "Kismet2/ReloadUtilities.h"
#include "Async/AsyncWork.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "Misc/QueuedThreadPool.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Misc/StringBuilder.h"

/**
 * Helper class used for re-instancing native and blueprint classes after hot-reload
 */
class FReloadClassReinstancer : public FBlueprintCompileReinstancer
{
	/** Holds a property and its offset in the serialized properties data array */
	struct FCDOProperty
	{
		FCDOProperty()
			: Property(nullptr)
			, SubobjectName(NAME_None)
			, SerializedValueOffset(0)
			, SerializedValueSize(0)
		{}

		FProperty* Property;
		FName SubobjectName;
		int64 SerializedValueOffset;
		int64 SerializedValueSize;
	};

	/** Contains all serialized CDO property data and the map of all serialized properties */
	struct FCDOPropertyData
	{
		TArray<uint8> Bytes;
		TMap<FName, FCDOProperty> Properties;
	};

	/** Hot-reloaded version of the old class */
	UClass* NewClass;

	/** Serialized properties of the original CDO (before hot-reload) */
	FCDOPropertyData OriginalCDOProperties;

	/** Serialized properties of the new CDO (after hot-reload) */
	FCDOPropertyData ReconstructedCDOProperties;

	/** True if the provided native class needs re-instancing */
	bool bNeedsReinstancing;

	/** Necessary for delta serialization */
	UObject* CopyOfPreviousCDO;

	/**
	 * Sets the re-instancer up for new class re-instancing
	 *
	 * @param InNewClass Class that has changed after hot-reload
	 * @param InOldClass Class before it was hot-reloaded
	 */
	void SetupNewClassReinstancing(UClass* InNewClass, UClass* InOldClass);

	/**
	* Sets the re-instancer up for old class re-instancing. Always re-creates the CDO.
	*
	* @param InOldClass Class that has NOT changed after hot-reload
	*/
	void RecreateCDOAndSetupOldClassReinstancing(UClass* InOldClass);

	/**
	* Creates a mem-comparable array of data containing CDO property values.
	*
	* @param InObject CDO
	* @param OutData Data containing all of the CDO property values
	*/
	void SerializeCDOProperties(UObject* InObject, FCDOPropertyData& OutData);

	/**
	 * Re-creates class default object.
	 *
	 * @param InClass Class that has NOT changed after hot-reload.
	 * @param InOuter Outer for the new CDO.
	 * @param InName Name of the new CDO.
	 * @param InFlags Flags of the new CDO.
	 */
	void ReconstructClassDefaultObject(UClass* InClass, UObject* InOuter, FName InName, EObjectFlags InFlags);

	/** Updates property values on instances of the hot-reloaded class */
	void UpdateDefaultProperties();

	/** Returns true if the properties of the CDO have changed during hot-reload */
	FORCEINLINE bool DefaultPropertiesHaveChanged() const
	{
		return OriginalCDOProperties.Bytes.Num() != ReconstructedCDOProperties.Bytes.Num() ||
			FMemory::Memcmp(OriginalCDOProperties.Bytes.GetData(), ReconstructedCDOProperties.Bytes.GetData(), OriginalCDOProperties.Bytes.Num());
	}

public:

	/** Sets the re-instancer up to re-instance native classes */
	FReloadClassReinstancer(UClass* InNewClass, UClass* InOldClass, const TMap<UClass*, UClass*>& OldToNewClassesMap, TMap<UObject*, UObject*>& OutReconstructedCDOsMap, TSet<UBlueprint*>& InBPSetToRecompile, TSet<UBlueprint*>& InBPSetToRecompileBytecodeOnly);

	/** Destructor */
	virtual ~FReloadClassReinstancer();

	/** If true, the class needs re-instancing */
	FORCEINLINE bool ClassNeedsReinstancing() const
	{
		return bNeedsReinstancing;
	}

	/** Reinstances all objects of the hot-reloaded class and update their properties to match the new CDO */
	void ReinstanceObjectsAndUpdateDefaults();

	/** Creates the reinstancer as a sharable object */
	static TSharedPtr<FReloadClassReinstancer> Create(UClass* InNewClass, UClass* InOldClass, const TMap<UClass*, UClass*>& OldToNewClassesMap, TMap<UObject*, UObject*>& OutReconstructedCDOsMap, TSet<UBlueprint*>& InBPSetToRecompile, TSet<UBlueprint*>& InBPSetToRecompileBytecodeOnly)
	{
		return MakeShareable(new FReloadClassReinstancer(InNewClass, InOldClass, OldToNewClassesMap, OutReconstructedCDOsMap, InBPSetToRecompile, InBPSetToRecompileBytecodeOnly));
	}

	// FSerializableObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	// End of FSerializableObject interface

	virtual bool IsClassObjectReplaced() const override { return true; }

	virtual void EnlistDependentBlueprintToRecompile(UBlueprint* BP, bool bBytecodeOnly) override;
	virtual void BlueprintWasRecompiled(UBlueprint* BP, bool bBytecodeOnly) override;

protected:

	// FBlueprintCompileReinstancer interface
	virtual bool ShouldPreserveRootComponentOfReinstancedActor() const override { return false; }
	// End of FBlueprintCompileReinstancer interface

private:
	/** Reference to reconstructed CDOs map in this hot-reload session. */
	TMap<UObject*, UObject*>& ReconstructedCDOsMap;
	TSet<UBlueprint*>& BPSetToRecompile;
	TSet<UBlueprint*>& BPSetToRecompileBytecodeOnly;
	const TMap<UClass*, UClass*>& OldToNewClassesMap;
};

void FReloadClassReinstancer::SetupNewClassReinstancing(UClass* InNewClass, UClass* InOldClass)
{
	// Set base class members to valid values
	ClassToReinstance = InNewClass;
	DuplicatedClass = InOldClass;
	OriginalCDO = InOldClass->GetDefaultObject();
	bHasReinstanced = false;
	bNeedsReinstancing = true;
	NewClass = InNewClass;

	// Collect the original CDO property values
	SerializeCDOProperties(InOldClass->GetDefaultObject(), OriginalCDOProperties);
	// Collect the property values of the new CDO
	SerializeCDOProperties(InNewClass->GetDefaultObject(), ReconstructedCDOProperties);

	SaveClassFieldMapping(InOldClass);

	ObjectsThatShouldUseOldStuff.Add(InOldClass); //CDO of REINST_ class can be used as archetype

	TArray<UClass*> ChildrenOfClass;
	GetDerivedClasses(InOldClass, ChildrenOfClass);
	for (auto ClassIt = ChildrenOfClass.CreateConstIterator(); ClassIt; ++ClassIt)
	{
		UClass* ChildClass = *ClassIt;
		UBlueprint* ChildBP = Cast<UBlueprint>(ChildClass->ClassGeneratedBy);
		if (ChildBP && !ChildBP->HasAnyFlags(RF_BeingRegenerated))
		{
			// If this is a direct child, change the parent and relink so the property chain is valid for reinstancing
			if (!ChildBP->HasAnyFlags(RF_NeedLoad))
			{
				if (ChildClass->GetSuperClass() == InOldClass)
				{
					ReparentChild(ChildBP);
				}

				Children.AddUnique(ChildBP);
				if (ChildBP->ParentClass == InOldClass)
				{
					ChildBP->ParentClass = NewClass;
				}
			}
			else
			{
				// If this is a child that caused the load of their parent, relink to the REINST class so that we can still serialize in the CDO, but do not add to later processing
				ReparentChild(ChildClass);
			}
		}
	}

	// Finally, remove the old class from Root so that it can get GC'd and mark it as CLASS_NewerVersionExists
	InOldClass->RemoveFromRoot();
	InOldClass->ClassFlags |= CLASS_NewerVersionExists;
}

void FReloadClassReinstancer::SerializeCDOProperties(UObject* InObject, FReloadClassReinstancer::FCDOPropertyData& OutData)
{
	// Creates a mem-comparable CDO data
	class FCDOWriter : public FMemoryWriter
	{
		/** Objects already visited by this archive */
		TSet<UObject*>& VisitedObjects;
		/** Output property data */
		FCDOPropertyData& PropertyData;
		/** Current subobject being serialized */
		FName SubobjectName;

	public:
		/** Serializes all script properties of the provided DefaultObject */
		FCDOWriter(FCDOPropertyData& InOutData, TSet<UObject*>& InVisitedObjects, FName InSubobjectName)
			: FMemoryWriter(InOutData.Bytes, /* bIsPersistent = */ false, /* bSetOffset = */ true)
			, VisitedObjects(InVisitedObjects)
			, PropertyData(InOutData)
			, SubobjectName(InSubobjectName)
		{
			// Disable delta serialization, we want to serialize everything
			ArNoDelta = true;
		}
		virtual void Serialize(void* Data, int64 Num) override
		{
			// Collect serialized properties so we can later update their values on instances if they change
			FProperty* SerializedProperty = GetSerializedProperty();
			if (SerializedProperty != nullptr)
			{
				FCDOProperty& PropertyInfo = PropertyData.Properties.FindOrAdd(SerializedProperty->GetFName());
				if (PropertyInfo.Property == nullptr)
				{
					PropertyInfo.Property = SerializedProperty;
					PropertyInfo.SubobjectName = SubobjectName;
					PropertyInfo.SerializedValueOffset = Tell();
					PropertyInfo.SerializedValueSize = Num;
				}
				else
				{
					PropertyInfo.SerializedValueSize += Num;
				}
			}
			FMemoryWriter::Serialize(Data, Num);
		}
		/** Serializes an object. Only name and class for normal references, deep serialization for DSOs */
		virtual FArchive& operator<<(class UObject*& InObj) override
		{
			FArchive& Ar = *this;
			if (InObj)
			{
				FName ClassName = InObj->GetClass()->GetFName();
				FName ObjectName = InObj->GetFName();
				Ar << ClassName;
				Ar << ObjectName;
				if (!VisitedObjects.Contains(InObj))
				{
					VisitedObjects.Add(InObj);
					if (Ar.GetSerializedProperty() && Ar.GetSerializedProperty()->ContainsInstancedObjectProperty())
					{
						// Serialize all DSO properties too
						FCDOWriter DefaultSubobjectWriter(PropertyData, VisitedObjects, InObj->GetFName());
						InObj->SerializeScriptProperties(DefaultSubobjectWriter);
						Seek(PropertyData.Bytes.Num());
					}
				}
			}
			else
			{
				FName UnusedName = NAME_None;
				Ar << UnusedName;
				Ar << UnusedName;
			}

			return *this;
		}
		virtual FArchive& operator<<(FObjectPtr& InObj) override
		{
			// Invoke the method above
			return FArchiveUObject::SerializeObjectPtr(*this, InObj);
		}
		/** Serializes an FName as its index and number */
		virtual FArchive& operator<<(FName& InName) override
		{
			FArchive& Ar = *this;
			FNameEntryId ComparisonIndex = InName.GetComparisonIndex();
			FNameEntryId DisplayIndex = InName.GetDisplayIndex();
			int32 Number = InName.GetNumber();
			Ar << ComparisonIndex;
			Ar << DisplayIndex;
			Ar << Number;
			return Ar;
		}
		virtual FArchive& operator<<(FLazyObjectPtr& LazyObjectPtr) override
		{
			FArchive& Ar = *this;
			FUniqueObjectGuid UniqueID = LazyObjectPtr.GetUniqueID();
			Ar << UniqueID;
			return *this;
		}
		virtual FArchive& operator<<(FSoftObjectPtr& Value) override
		{
			FArchive& Ar = *this;
			FSoftObjectPath UniqueID = Value.GetUniqueID();
			Ar << UniqueID;
			return Ar;
		}
		virtual FArchive& operator<<(FSoftObjectPath& Value) override
		{
			FArchive& Ar = *this;

			FString Path = Value.ToString();

			Ar << Path;

			if (IsLoading())
			{
				Value.SetPath(MoveTemp(Path));
			}

			return Ar;
		}
		FArchive& operator<<(FWeakObjectPtr& WeakObjectPtr) override
		{
			return FArchiveUObject::SerializeWeakObjectPtr(*this, WeakObjectPtr);
		}
		/** Archive name, for debugging */
		virtual FString GetArchiveName() const override { return TEXT("FCDOWriter"); }
	};
	TSet<UObject*> VisitedObjects;
	VisitedObjects.Add(InObject);
	FCDOWriter Ar(OutData, VisitedObjects, NAME_None);
	InObject->SerializeScriptProperties(Ar);
}

void FReloadClassReinstancer::ReconstructClassDefaultObject(UClass* InClass, UObject* InOuter, FName InName, EObjectFlags InFlags)
{
	// Get the parent CDO
	UClass* ParentClass = InClass->GetSuperClass();
	UObject* ParentDefaultObject = NULL;
	if (ParentClass != NULL)
	{
		ParentDefaultObject = ParentClass->GetDefaultObject(); // Force the default object to be constructed if it isn't already
	}

	// Re-create
	InClass->ClassDefaultObject = StaticAllocateObject(InClass, InOuter, InName, InFlags, EInternalObjectFlags::None, false);
	check(InClass->ClassDefaultObject);
	const bool bShouldInitializeProperties = false;
	const bool bCopyTransientsFromClassDefaults = false;
	(*InClass->ClassConstructor)(FObjectInitializer(InClass->ClassDefaultObject, ParentDefaultObject, bCopyTransientsFromClassDefaults, bShouldInitializeProperties));
}

void FReloadClassReinstancer::RecreateCDOAndSetupOldClassReinstancing(UClass* InOldClass)
{
	// Set base class members to valid values
	ClassToReinstance = InOldClass;
	DuplicatedClass = InOldClass;
	OriginalCDO = InOldClass->GetDefaultObject();
	bHasReinstanced = false;
	bNeedsReinstancing = false;
	NewClass = InOldClass; // The class doesn't change in this case

	// Collect the original property values
	SerializeCDOProperties(InOldClass->GetDefaultObject(), OriginalCDOProperties);

	// Remember all the basic info about the object before we rename it
	EObjectFlags CDOFlags = OriginalCDO->GetFlags();
	UObject* CDOOuter = OriginalCDO->GetOuter();
	FName CDOName = OriginalCDO->GetFName();

	// Rename original CDO, so we can store this one as OverridenArchetypeForCDO
	// and create new one with the same name and outer.
	OriginalCDO->Rename(
		*MakeUniqueObjectName(
			GetTransientPackage(),
			OriginalCDO->GetClass(),
			*FString::Printf(TEXT("BPGC_ARCH_FOR_CDO_%s"), *InOldClass->GetName())
		).ToString(),
		GetTransientPackage(),
		REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional | REN_SkipGeneratedClasses | REN_ForceNoResetLoaders);

	// Re-create the CDO, re-running its constructor
	ReconstructClassDefaultObject(InOldClass, CDOOuter, CDOName, CDOFlags);

	ReconstructedCDOsMap.Add(OriginalCDO, InOldClass->GetDefaultObject());

	// Collect the property values after re-constructing the CDO
	SerializeCDOProperties(InOldClass->GetDefaultObject(), ReconstructedCDOProperties);

	// We only want to re-instance the old class if its CDO's values have changed or any of its DSOs' property values have changed
	if (DefaultPropertiesHaveChanged())
	{
		bNeedsReinstancing = true;
		SaveClassFieldMapping(InOldClass);

		TArray<UClass*> ChildrenOfClass;
		GetDerivedClasses(InOldClass, ChildrenOfClass);
		for (auto ClassIt = ChildrenOfClass.CreateConstIterator(); ClassIt; ++ClassIt)
		{
			UClass* ChildClass = *ClassIt;
			UBlueprint* ChildBP = Cast<UBlueprint>(ChildClass->ClassGeneratedBy);
			if (ChildBP && !ChildBP->HasAnyFlags(RF_BeingRegenerated))
			{
				if (!ChildBP->HasAnyFlags(RF_NeedLoad))
				{
					Children.AddUnique(ChildBP);
					UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(ChildBP->GeneratedClass);
					UObject* CurrentCDO = BPGC ? BPGC->GetDefaultObject(false) : nullptr;
					if (CurrentCDO && (OriginalCDO == CurrentCDO->GetArchetype()))
					{
						BPGC->OverridenArchetypeForCDO = OriginalCDO;
					}
				}
			}
		}
	}
}

FReloadClassReinstancer::FReloadClassReinstancer(UClass* InNewClass, UClass* InOldClass, const TMap<UClass*, UClass*>& InOldToNewClassesMap, TMap<UObject*, UObject*>& OutReconstructedCDOsMap, TSet<UBlueprint*>& InBPSetToRecompile, TSet<UBlueprint*>& InBPSetToRecompileBytecodeOnly)
	: NewClass(nullptr)
	, bNeedsReinstancing(false)
	, CopyOfPreviousCDO(nullptr)
	, ReconstructedCDOsMap(OutReconstructedCDOsMap)
	, BPSetToRecompile(InBPSetToRecompile)
	, BPSetToRecompileBytecodeOnly(InBPSetToRecompileBytecodeOnly)
	, OldToNewClassesMap(InOldToNewClassesMap)
{
	ensure(InOldClass);
	ensure(!HotReloadedOldClass && !HotReloadedNewClass);
	HotReloadedOldClass = InOldClass;
	HotReloadedNewClass = InNewClass ? InNewClass : InOldClass;

	for (const TPair<UClass*, UClass*>& OldToNewClass : OldToNewClassesMap)
	{
		ObjectsThatShouldUseOldStuff.Add(OldToNewClass.Key);
	}

	// If InNewClass is NULL, then the old class has not changed after hot-reload.
	// However, we still need to check for changes to its constructor code (CDO values).
	if (InNewClass)
	{
		SetupNewClassReinstancing(InNewClass, InOldClass);

		TMap<UObject*, UObject*> ClassRedirects;
		ClassRedirects.Add(InOldClass, InNewClass);

		for (TObjectIterator<UBlueprint> BlueprintIt; BlueprintIt; ++BlueprintIt)
		{
			FArchiveReplaceObjectRef<UObject> ReplaceObjectArch(*BlueprintIt, ClassRedirects, false, true, true);
			if (ReplaceObjectArch.GetCount())
			{
				EnlistDependentBlueprintToRecompile(*BlueprintIt, false);
			}
		}
	}
	else
	{
		RecreateCDOAndSetupOldClassReinstancing(InOldClass);
	}
}

FReloadClassReinstancer::~FReloadClassReinstancer()
{
	// Make sure the base class does not remove the DuplicatedClass from root, we not always want it.
	// For example when we're just reconstructing CDOs. Other cases are handled by HotReloadClassReinstancer.
	DuplicatedClass = nullptr;

	ensure(HotReloadedOldClass);
	HotReloadedOldClass = nullptr;
	HotReloadedNewClass = nullptr;
}

/** Helper for finding subobject in an array. Usually there's not that many subobjects on a class to justify a TMap */
FORCEINLINE static UObject* FindDefaultSubobject(TArray<UObject*>& InDefaultSubobjects, FName SubobjectName)
{
	for (UObject* Subobject : InDefaultSubobjects)
	{
		if (Subobject->GetFName() == SubobjectName)
		{
			return Subobject;
		}
	}
	return nullptr;
}

void FReloadClassReinstancer::UpdateDefaultProperties()
{
	struct FPropertyToUpdate
	{
		FProperty* Property;
		FName SubobjectName;
		uint8* OldSerializedValuePtr;
		uint8* NewValuePtr;
		int64 OldSerializedSize;
	};
	/** Memory writer archive that supports UObject values the same way as FCDOWriter. */
	class FPropertyValueMemoryWriter : public FMemoryWriter
	{
	public:
		FPropertyValueMemoryWriter(TArray<uint8>& OutData)
			: FMemoryWriter(OutData)
		{}
		virtual FArchive& operator<<(class UObject*& InObj) override
		{
			FArchive& Ar = *this;
			if (InObj)
			{
				FName ClassName = InObj->GetClass()->GetFName();
				FName ObjectName = InObj->GetFName();
				Ar << ClassName;
				Ar << ObjectName;
			}
			else
			{
				FName UnusedName = NAME_None;
				Ar << UnusedName;
				Ar << UnusedName;
			}
			return *this;
		}
		virtual FArchive& operator<<(FObjectPtr& InObj) override
		{
			// Invoke the method above
			return FArchiveUObject::SerializeObjectPtr(*this, InObj);
		}
		virtual FArchive& operator<<(FName& InName) override
		{
			FArchive& Ar = *this;
			FNameEntryId ComparisonIndex = InName.GetComparisonIndex();
			FNameEntryId DisplayIndex = InName.GetDisplayIndex();
			int32 Number = InName.GetNumber();
			Ar << ComparisonIndex;
			Ar << DisplayIndex;
			Ar << Number;
			return Ar;
		}
		virtual FArchive& operator<<(FLazyObjectPtr& LazyObjectPtr) override
		{
			FArchive& Ar = *this;
			FUniqueObjectGuid UniqueID = LazyObjectPtr.GetUniqueID();
			Ar << UniqueID;
			return *this;
		}
		virtual FArchive& operator<<(FSoftObjectPtr& Value) override
		{
			FArchive& Ar = *this;
			FSoftObjectPath UniqueID = Value.GetUniqueID();
			Ar << UniqueID;
			return Ar;
		}
		virtual FArchive& operator<<(FSoftObjectPath& Value) override
		{
			FArchive& Ar = *this;

			FString Path = Value.ToString();

			Ar << Path;

			if (IsLoading())
			{
				Value.SetPath(MoveTemp(Path));
			}

			return Ar;
		}
		FArchive& operator<<(FWeakObjectPtr& WeakObjectPtr) override
		{
			return FArchiveUObject::SerializeWeakObjectPtr(*this, WeakObjectPtr);
		}
	};

	// Collect default subobjects to update their properties too
	const int32 DefaultSubobjectArrayCapacity = 16;
	TArray<UObject*> DefaultSubobjectArray;
	DefaultSubobjectArray.Empty(DefaultSubobjectArrayCapacity);
	NewClass->GetDefaultObject()->CollectDefaultSubobjects(DefaultSubobjectArray);

	TArray<FPropertyToUpdate> PropertiesToUpdate;
	// Collect all properties that have actually changed
	for (const TPair<FName, FCDOProperty>& Pair : ReconstructedCDOProperties.Properties)
	{
		FCDOProperty* OldPropertyInfo = OriginalCDOProperties.Properties.Find(Pair.Key);
		if (OldPropertyInfo)
		{
			const FCDOProperty& NewPropertyInfo = Pair.Value;

			uint8* OldSerializedValuePtr = OriginalCDOProperties.Bytes.GetData() + OldPropertyInfo->SerializedValueOffset;
			uint8* NewSerializedValuePtr = ReconstructedCDOProperties.Bytes.GetData() + NewPropertyInfo.SerializedValueOffset;
			if (OldPropertyInfo->SerializedValueSize != NewPropertyInfo.SerializedValueSize ||
				FMemory::Memcmp(OldSerializedValuePtr, NewSerializedValuePtr, OldPropertyInfo->SerializedValueSize) != 0)
			{
				// Property value has changed so add it to the list of properties that need updating on instances
				FPropertyToUpdate PropertyToUpdate;
				PropertyToUpdate.Property = NewPropertyInfo.Property;
				PropertyToUpdate.NewValuePtr = nullptr;
				PropertyToUpdate.SubobjectName = NewPropertyInfo.SubobjectName;

				if (NewPropertyInfo.Property->GetOwner<UObject>() == NewClass)
				{
					PropertyToUpdate.NewValuePtr = PropertyToUpdate.Property->ContainerPtrToValuePtr<uint8>(NewClass->GetDefaultObject());
				}
				else if (NewPropertyInfo.SubobjectName != NAME_None)
				{
					UObject* DefaultSubobjectPtr = FindDefaultSubobject(DefaultSubobjectArray, NewPropertyInfo.SubobjectName);
					if (DefaultSubobjectPtr && NewPropertyInfo.Property->GetOwner<UObject>() == DefaultSubobjectPtr->GetClass())
					{
						PropertyToUpdate.NewValuePtr = PropertyToUpdate.Property->ContainerPtrToValuePtr<uint8>(DefaultSubobjectPtr);
					}
				}
				if (PropertyToUpdate.NewValuePtr)
				{
					PropertyToUpdate.OldSerializedValuePtr = OldSerializedValuePtr;
					PropertyToUpdate.OldSerializedSize = OldPropertyInfo->SerializedValueSize;

					PropertiesToUpdate.Add(PropertyToUpdate);
				}
			}
		}
	}
	if (PropertiesToUpdate.Num())
	{
		TArray<uint8> CurrentValueSerializedData;

		// Update properties on all existing instances of the class
		const UPackage* TransientPackage = GetTransientPackage();
		for (FThreadSafeObjectIterator It(NewClass); It; ++It)
		{
			UObject* ObjectPtr = *It;
			if (ObjectPtr->IsPendingKill() || ObjectPtr->GetOutermost() == TransientPackage)
			{
				continue;
			}

			DefaultSubobjectArray.Empty(DefaultSubobjectArrayCapacity);
			ObjectPtr->CollectDefaultSubobjects(DefaultSubobjectArray);

			for (auto& PropertyToUpdate : PropertiesToUpdate)
			{
				uint8* InstanceValuePtr = nullptr;
				if (PropertyToUpdate.SubobjectName == NAME_None)
				{
					InstanceValuePtr = PropertyToUpdate.Property->ContainerPtrToValuePtr<uint8>(ObjectPtr);
				}
				else
				{
					UObject* DefaultSubobjectPtr = FindDefaultSubobject(DefaultSubobjectArray, PropertyToUpdate.SubobjectName);
					if (DefaultSubobjectPtr && PropertyToUpdate.Property->GetOwner<UObject>() == DefaultSubobjectPtr->GetClass())
					{
						InstanceValuePtr = PropertyToUpdate.Property->ContainerPtrToValuePtr<uint8>(DefaultSubobjectPtr);
					}
				}

				if (InstanceValuePtr)
				{
					// Serialize current value to a byte array as we don't have the previous CDO to compare against, we only have its serialized property data
					CurrentValueSerializedData.Empty(CurrentValueSerializedData.Num() + CurrentValueSerializedData.GetSlack());
					FPropertyValueMemoryWriter CurrentValueWriter(CurrentValueSerializedData);
					PropertyToUpdate.Property->SerializeItem(FStructuredArchiveFromArchive(CurrentValueWriter).GetSlot(), InstanceValuePtr);

					// Update only when the current value on the instance is identical to the original CDO
					if (CurrentValueSerializedData.Num() == PropertyToUpdate.OldSerializedSize &&
						FMemory::Memcmp(CurrentValueSerializedData.GetData(), PropertyToUpdate.OldSerializedValuePtr, CurrentValueSerializedData.Num()) == 0)
					{
						// Update with the new value
						PropertyToUpdate.Property->CopyCompleteValue(InstanceValuePtr, PropertyToUpdate.NewValuePtr);
					}
				}
			}
		}
	}
}

void FReloadClassReinstancer::ReinstanceObjectsAndUpdateDefaults()
{
	ReinstanceObjects(true);
	UpdateDefaultProperties();
}

void FReloadClassReinstancer::AddReferencedObjects(FReferenceCollector& Collector)
{
	FBlueprintCompileReinstancer::AddReferencedObjects(Collector);
	Collector.AllowEliminatingReferences(false);
	Collector.AddReferencedObject(CopyOfPreviousCDO);
	Collector.AllowEliminatingReferences(true);
}

void FReloadClassReinstancer::EnlistDependentBlueprintToRecompile(UBlueprint* BP, bool bBytecodeOnly)
{
	if (IsValid(BP))
	{
		if (bBytecodeOnly)
		{
			if (!BPSetToRecompile.Contains(BP) && !BPSetToRecompileBytecodeOnly.Contains(BP))
			{
				BPSetToRecompileBytecodeOnly.Add(BP);
			}
		}
		else
		{
			if (!BPSetToRecompile.Contains(BP))
			{
				if (BPSetToRecompileBytecodeOnly.Contains(BP))
				{
					BPSetToRecompileBytecodeOnly.Remove(BP);
				}

				BPSetToRecompile.Add(BP);
			}
		}
	}
}

void FReloadClassReinstancer::BlueprintWasRecompiled(UBlueprint* BP, bool bBytecodeOnly)
{
	BPSetToRecompile.Remove(BP);
	BPSetToRecompileBytecodeOnly.Remove(BP);

	FBlueprintCompileReinstancer::BlueprintWasRecompiled(BP, bBytecodeOnly);
}

FReload::FReload(EActiveReloadType InType, const TCHAR* InPrefix, const TArray<UPackage*>& InPackages, FOutputDevice& InAr)
	: Type(InType)
	, Prefix(InPrefix)
	, Packages(InPackages)
	, Ar(InAr)
	, bCollectPackages(false)
{
	BeginReload(Type, *this);
}

FReload::FReload(EActiveReloadType InType, const TCHAR* InPrefix, FOutputDevice& InAr)
	: Type(InType)
	, Prefix(InPrefix)
	, Ar(InAr)
	, bCollectPackages(true)
{
	BeginReload(Type, *this);
}

FReload::~FReload()
{
	EndReload();

	TStringBuilder<256> Builder;
	if (ClassStats.HasValues() || StructStats.HasValues() || EnumStats.HasValues() || NumFunctionsRemapped != 0 || NumScriptStructsRemapped != 0)
	{
		FormatStats(Builder, TEXT("class"), TEXT("classes"), ClassStats);
		FormatStats(Builder, TEXT("enum"), TEXT("enums"), EnumStats);
		FormatStats(Builder, TEXT("scriptstruct"), TEXT("scriptstructs"), StructStats);
		FormatStat(Builder, TEXT("function"), TEXT("functions"), TEXT("remapped"), NumFunctionsRemapped);
		FormatStat(Builder, TEXT("scriptstruct"), TEXT("scriptstructs"), TEXT("remapped"), NumScriptStructsRemapped);
	}
	else
	{
		Builder << TEXT("No object changes detected");
	}
	Ar.Logf(ELogVerbosity::Display, TEXT("Reload/Re-instancing Complete: %s"), *Builder);

	if (bSendReloadComplete)
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Broadcast(EReloadCompleteReason::None);
	}
}

void FReload::Reset()
{
	FunctionRemap.Empty();
	BPSetToRecompile.Empty();
	BPSetToRecompileBytecodeOnly.Empty();
	ReconstructedCDOsMap.Empty();
	ReinstancedClasses.Empty();
}

void FReload::UpdateStats(ReinstanceStats& Stats, void* New, void* Old)
{
	if (Old == nullptr)
	{
		++Stats.New;
	}
	else if (Old != New)
	{
		++Stats.Changed;
	}
	else
	{
		++Stats.Unchanged;
	}
}

void FReload::FormatStats(FStringBuilderBase& Out, const TCHAR* Singular, const TCHAR* Plural, const ReinstanceStats& Stats)
{
	FormatStat(Out, Singular, Plural, TEXT("new"), Stats.New);
	FormatStat(Out, Singular, Plural, TEXT("changed"), Stats.Changed);
	FormatStat(Out, Singular, Plural, TEXT("unchanged"), Stats.Unchanged);
}

void FReload::FormatStat(FStringBuilderBase& Out, const TCHAR* Singular, const TCHAR* Plural, const TCHAR* What, int32 Value)
{
	if (Value == 0)
	{
		return;
	}

	if (Out.Len() != 0)
	{
		Out << TEXT(", ");
	}
	Out << Value << TEXT(" ") << (Value > 1 ? Plural : Singular) << TEXT(" ") << What;
}

void FReload::NotifyFunctionRemap(FNativeFuncPtr NewFunctionPointer, FNativeFuncPtr OldFunctionPointer)
{
	FNativeFuncPtr OtherNewFunction = FunctionRemap.FindRef(OldFunctionPointer);
	check(!OtherNewFunction || OtherNewFunction == NewFunctionPointer);
	check(NewFunctionPointer);
	check(OldFunctionPointer);
	FunctionRemap.Add(OldFunctionPointer, NewFunctionPointer);
}

void FReload::NotifyChange(UClass* New, UClass* Old)
{
	UpdateStats(ClassStats, New, Old);

	// Ignore new classes
	if (Old != nullptr)
	{
		// Don't allow re-instancing of UEngine classes
		if (!Old->IsChildOf(UEngine::StaticClass()))
		{
			UClass* NewIfChanged = Old != New ? New : nullptr; // supporting code detects unchanged based on null new pointer
			TMap<UClass*, UClass*>& ClassesToReinstance = GetClassesToReinstanceForHotReload();
			checkf(!ClassesToReinstance.Contains(Old) || ClassesToReinstance[Old] == NewIfChanged, TEXT("Attempting to reload a class which is already being reloaded as a different class"));
			ClassesToReinstance.Add(Old, NewIfChanged);
		}
		else if (Old != New) // This has changed
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("Engine class '%s' has changed but will be ignored for reload"), *New->GetName());
		}
	}
}

void FReload::NotifyChange(UEnum* New, UEnum* Old)
{
	UpdateStats(EnumStats, New, Old);
}

void FReload::NotifyChange(UScriptStruct* New, UScriptStruct* Old)
{
	UpdateStats(StructStats, New, Old);
}

void FReload::Reinstance()
{
	if (Type != EActiveReloadType::Reinstancing)
	{
		UClass::AssembleReferenceTokenStreams();
	}

	TMap<UClass*, UClass*>& ClassesToReinstance = GetClassesToReinstanceForHotReload();

	TMap<UClass*, UClass*> OldToNewClassesMap;
	for (const TPair<UClass*, UClass*>& Pair : ClassesToReinstance)
	{
		if (Pair.Value != nullptr)
		{
			OldToNewClassesMap.Add(Pair.Key, Pair.Value);
		}
	}

	for (const TPair<UClass*, UClass*>& Pair : ClassesToReinstance)
	{
		ReinstanceClass(Pair.Value, Pair.Key, OldToNewClassesMap);
	}

	ReinstancedClasses = MoveTemp(ClassesToReinstance);

	FCoreUObjectDelegates::ReloadReinstancingCompleteDelegate.Broadcast();
}

void FReload::ReinstanceClass(UClass* NewClass, UClass* OldClass, const TMap<UClass*, UClass*>& OldToNewClassesMap)
{
	TSharedPtr<FReloadClassReinstancer> ReinstanceHelper = FReloadClassReinstancer::Create(NewClass, OldClass, OldToNewClassesMap, ReconstructedCDOsMap, BPSetToRecompile, BPSetToRecompileBytecodeOnly);
	if (ReinstanceHelper->ClassNeedsReinstancing())
	{
		Ar.Logf(ELogVerbosity::Log, TEXT("Re-instancing %s after reload."), NewClass ? *NewClass->GetName() : *OldClass->GetName());
		ReinstanceHelper->ReinstanceObjectsAndUpdateDefaults();
	}
}

void FReload::Finalize()
{
	// If we have to collect the packages
	if (bCollectPackages)
	{
		for (const TPair<UClass*, UClass*>& Pair : ReinstancedClasses)
		{
			UClass* OldClass = Pair.Key;
			UClass* NewClass = Pair.Value;
			Packages.AddUnique(NewClass ? NewClass->GetPackage() : OldClass->GetPackage());
		}
	}

	// Remap all native functions (and gather scriptstructs)
	TArray<UScriptStruct*> ScriptStructs;
	for (FRawObjectIterator It; It; ++It)
	{
		UObject* a = static_cast<UObject*>(It->Object);
		UFunction* f = Cast<UFunction>(a);

		if (UFunction* Function = Cast<UFunction>(static_cast<UObject*>(It->Object)))
		{
			if (FNativeFuncPtr NewFunction = FunctionRemap.FindRef(Function->GetNativeFunc()))
			{
				++NumFunctionsRemapped;
				Function->SetNativeFunc(NewFunction);
			}
		}

		if (UScriptStruct* ScriptStruct = Cast<UScriptStruct>(static_cast<UObject*>(It->Object)))
		{
			if (!ScriptStruct->HasAnyFlags(RF_ClassDefaultObject) && ScriptStruct->GetCppStructOps() && Packages.ContainsByPredicate([=](UPackage* Package) { return ScriptStruct->IsIn(Package); }))
			{
				ScriptStructs.Add(ScriptStruct);
			}
		}
	}
	NumScriptStructsRemapped = ScriptStructs.Num();

	// now let's set up the script structs...this relies on super behavior, so null them all, then set them all up. Internally this sets them up hierarchically.
	for (UScriptStruct* Script : ScriptStructs)
	{
		Script->ClearCppStructOps();
	}
	for (UScriptStruct* Script : ScriptStructs)
	{
		Script->PrepareCppStructOps();
		check(Script->GetCppStructOps());
	}
	// Make sure new classes have the token stream assembled
	UClass::AssembleReferenceTokenStreams();

	ReplaceReferencesToReconstructedCDOs();

	// Force GC to collect reinstanced objects
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
}

void FReload::ReplaceReferencesToReconstructedCDOs()
{
	if (ReconstructedCDOsMap.Num() == 0)
	{
		return;
	}

	// Thread pool manager. We need new thread pool with increased
	// amount of stack size. Standard GThreadPool was encountering
	// stack overflow error during serialization.
	static struct FReplaceReferencesThreadPool
	{
		FReplaceReferencesThreadPool()
		{
			Pool = FQueuedThreadPool::Allocate();
			int32 NumThreadsInThreadPool = FPlatformMisc::NumberOfWorkerThreadsToSpawn();
			verify(Pool->Create(NumThreadsInThreadPool, 256 * 1024));
		}

		~FReplaceReferencesThreadPool()
		{
			Pool->Destroy();
		}

		FQueuedThreadPool* GetPool() { return Pool; }

	private:
		FQueuedThreadPool* Pool;
	} ThreadPoolManager;

	// Async task to enable multithreaded CDOs reference search.
	class FFindRefTask : public FNonAbandonableTask
	{
	public:
		explicit FFindRefTask(const TMap<UObject*, UObject*>& InReconstructedCDOsMap, int32 ReserveElements)
			: ReconstructedCDOsMap(InReconstructedCDOsMap)
		{
			ObjectsArray.Reserve(ReserveElements);
		}

		void DoWork()
		{
			for (UObject* Object : ObjectsArray)
			{
				class FReplaceCDOReferencesArchive : public FArchiveUObject
				{
				public:
					FReplaceCDOReferencesArchive(UObject* InPotentialReferencer, const TMap<UObject*, UObject*>& InReconstructedCDOsMap)
						: ReconstructedCDOsMap(InReconstructedCDOsMap)
						, PotentialReferencer(InPotentialReferencer)
					{
						ArIsObjectReferenceCollector = true;
						ArIgnoreOuterRef = true;
					}

					virtual FString GetArchiveName() const override
					{
						return TEXT("FReplaceCDOReferencesArchive");
					}

					FArchive& operator<<(UObject*& ObjRef)
					{
						UObject* Obj = ObjRef;

						if (Obj && Obj != PotentialReferencer)
						{
							if (UObject* const* FoundObj = ReconstructedCDOsMap.Find(Obj))
							{
								ObjRef = *FoundObj;
							}
						}

						return *this;
					}

					const TMap<UObject*, UObject*>& ReconstructedCDOsMap;
					UObject* PotentialReferencer;
				};

				FReplaceCDOReferencesArchive FindRefsArchive(Object, ReconstructedCDOsMap);
				Object->Serialize(FindRefsArchive);
			}
		}

		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FFindRefTask, STATGROUP_ThreadPoolAsyncTasks);
		}

		TArray<UObject*> ObjectsArray;

	private:
		const TMap<UObject*, UObject*>& ReconstructedCDOsMap;
	};

	const int32 NumberOfThreads = FPlatformMisc::NumberOfWorkerThreadsToSpawn();
	const int32 NumObjects = GUObjectArray.GetObjectArrayNum();
	const int32 ObjectsPerTask = FMath::CeilToInt((float)NumObjects / NumberOfThreads);

	// Create tasks.
	TArray<FAsyncTask<FFindRefTask>> Tasks;
	Tasks.Reserve(NumberOfThreads);

	for (int32 TaskId = 0; TaskId < NumberOfThreads; ++TaskId)
	{
		Tasks.Emplace(ReconstructedCDOsMap, ObjectsPerTask);
	}

	// Distribute objects uniformly between tasks.
	int32 CurrentTaskId = 0;
	for (FThreadSafeObjectIterator ObjIter; ObjIter; ++ObjIter)
	{
		UObject* CurObject = *ObjIter;

		if (CurObject->IsPendingKill())
		{
			continue;
		}

		Tasks[CurrentTaskId].GetTask().ObjectsArray.Add(CurObject);
		CurrentTaskId = (CurrentTaskId + 1) % NumberOfThreads;
	}

	// Run async tasks in worker threads.
	for (FAsyncTask<FFindRefTask>& Task : Tasks)
	{
		Task.StartBackgroundTask(ThreadPoolManager.GetPool());
	}

	// Wait until tasks are finished
	for (FAsyncTask<FFindRefTask>& AsyncTask : Tasks)
	{
		AsyncTask.EnsureCompletion();
	}
}
