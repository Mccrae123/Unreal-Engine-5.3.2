﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "Archive/ApplySnapshotDataArchiveV2.h"

#include "Data/PropertySelection.h"
#include "LevelSnapshotsStats.h"
#include "WorldSnapshotData.h"

#include "Serialization/ArchiveSerializedPropertyChain.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"

namespace
{
	void HandleHiddenCustomSerializedProperty(const FArchive& Archive, const FProperty* InProperty, const FPropertySelection& SelectionSet, bool& bShouldSkipProperty)
	{
		/* Edge case: Certain structs may implement a custom operator<< or Serialize function. These functions might push any properties (native or UProperty),
		 * that could NOT be discovered when SelectedProperties->SelectedPropertyPaths was built.
		 * ShouldSkipProperty must return false so these internal properties are also serialized. 
		 * 
		 * "Nice" example:
		 * 1. FNiagaraParameterStore::SortedParameterOffsets is an TArray<FNiagaraVariableWithOffset>.
		 * 2. ShouldSkipProperty returned false for SortedParameterOffsets, meaning SortedParameterOffsets is now getting serialized.
		 * 3. FNiagaraVariableWithOffset implements a custom Serialize function, which in turn does Ar << Handle, where Handle is of type FNiagaraTypeDefinitionHandle.
		 * 4. Ar << Handle causes this nice code to be executed:
		 * 
		 * FArchive& operator<<(FArchive& Ar, FNiagaraTypeDefinitionHandle& Handle)
		 * {
		 *		UScriptStruct* TypeDefStruct = FNiagaraTypeDefinition::StaticStruct();
		 *
		 *		if (Ar.IsSaving())
		 *		{
		 *			FNiagaraTypeDefinition TypeDef = *Handle;
		 *			TypeDefStruct->SerializeItem(Ar, &TypeDef, nullptr);
		 *		}
		 *		else if (Ar.IsLoading())
		 *		{
		 *			FNiagaraTypeDefinition TypeDef;
		 *			TypeDefStruct->SerializeItem(Ar, &TypeDef, nullptr);
		 *			Handle = FNiagaraTypeDefinitionHandle(TypeDef);
		 *		} 
		 * }
		 * 
		 * Result: ShouldSkipProperty is now called with the properties of FNiagaraTypeDefinition. 
		 * These properties are not in SelectedProperties->SelectedPropertyPaths but we MUST serialize them.
		 * 
		 * If you understood the above, congratulations; this is a complicated edge case.
		 */
		const FArchiveSerializedPropertyChain* PropertyChain = Archive.GetSerializedPropertyChain();
		const bool bIsRootProperty = PropertyChain == nullptr || PropertyChain->GetNumProperties() == 0;
		// The last property in the chain is the last property on which ShouldSkipProperty was called, e.g. SortedParameterOffsets
		const FProperty* LastPropertyInChain = bIsRootProperty ? Archive.GetSerializedProperty() : PropertyChain->GetPropertyFromStack(0);
		if (bShouldSkipProperty && LastPropertyInChain)
		{
			const bool bIsInternalProperty = [LastPropertyInChain, InProperty]()
			{
				const FStructProperty* LastInChainAsStruct = CastField<FStructProperty>(LastPropertyInChain);
				if (!LastInChainAsStruct)
				{
					return false;
				}

				UStruct* CurrentStruct = LastInChainAsStruct->Struct;
				while (CurrentStruct)
				{
					FField* ChildProperty = CurrentStruct->ChildProperties;
					while (ChildProperty)
					{
						if (ChildProperty == InProperty)
						{
							return false;
						}
						ChildProperty = ChildProperty->Next;
					}
					CurrentStruct = CurrentStruct->GetSuperStruct();
				}
				return true;
			}();
			
			bShouldSkipProperty = !bIsInternalProperty;
		}
	}
	
	class FCopyProperties : public FObjectWriter
	{
		using Super = FObjectWriter;

		const FPropertySelection& PropertiesToSerialize;
				
	public:

		FCopyProperties(TArray<uint8>& SaveLocation, const FPropertySelection& PropertiesToSerialize)
			:
			Super(SaveLocation),
			PropertiesToSerialize(PropertiesToSerialize)
		{
			ArNoDelta = true;
		}

		virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
		{
			bool bShouldSkipProperty = !PropertiesToSerialize.IsPropertySelected(GetSerializedPropertyChain(), InProperty);
			HandleHiddenCustomSerializedProperty(*this, InProperty, PropertiesToSerialize, bShouldSkipProperty);
			return bShouldSkipProperty;
		}
	};
}

void FApplySnapshotDataArchiveV2::ApplyToExistingWorldObject(FObjectSnapshotData& InObjectData, FWorldSnapshotData& InSharedData, UObject* InOriginalObject, UObject* InDeserializedVersion, const FPropertySelection& InSelectionSet)
{
	// ObjectData only contains properties that were different from the CDO at the time of saving. This archive may skip many properties.
	// Hence, we serialise in two steps:

	
	// Step 1: Serialise any properties that were different from CDO at time of snapshotting and that are different now
	// Most UObject references will be handled here:
		// - Subobject references are handled here
		// - References to other actors in the world are handled here
	FApplySnapshotDataArchiveV2 ApplySavedData(InObjectData, InSharedData, InOriginalObject, InSelectionSet);
	InOriginalObject->Serialize(ApplySavedData);


	
	// Step 2: Serialise any remaining properties that were not covered: properties that were the equal to the CDO value when the snapshot was taken, but now are different from the CDO.
	// For this step, we indirectly use the CDO values saved in the snapshot: we copy over all remaining properties from the deserialized version.
	// 
	// Most UObject references were covered in step 1.
		// - CDO was nullptr and level property is non-nullptr
		// - CDO was asset reference and level property now has different asset reference
	const FPropertySelection& PropertiesLeftToSerialise = ApplySavedData.PropertiesLeftToSerialize;
	if (!PropertiesLeftToSerialise.IsEmpty())
	{
		TArray<uint8> CopiedPropertyData;
		FCopyProperties CopySimpleProperties(CopiedPropertyData, PropertiesLeftToSerialise);
		InDeserializedVersion->Serialize(CopySimpleProperties);
		
		FObjectReader PasteSimpleProperties(InOriginalObject, CopiedPropertyData);
	}
}

void FApplySnapshotDataArchiveV2::ApplyToRecreatedWorldObject(FObjectSnapshotData& InObjectData, FWorldSnapshotData& InSharedData, UObject* InOriginalObject, UObject* InDeserializedVersion)
{
	// Apply all properties that we saved into the target actor.
	// We assume that InOriginalObject was already created with the snapshot CDO as template: we do not need Step 2 from ApplyToExistingWorldObject.
	FApplySnapshotDataArchiveV2 ApplySavedData(InObjectData, InSharedData, InOriginalObject);
	InOriginalObject->Serialize(ApplySavedData);
}

bool FApplySnapshotDataArchiveV2::ShouldSkipProperty(const FProperty* InProperty) const
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("ShouldSkipProperty_Loading"), STAT_ShouldSkipProperty_Loading, STATGROUP_LevelSnapshots);
	
	bool bShouldSkipProperty = Super::ShouldSkipProperty(InProperty);
	
	const bool bShouldSerializeAllValues = !SelectionSet.IsSet();
	if (!bShouldSkipProperty && !bShouldSerializeAllValues)
	{
		bShouldSkipProperty = !SelectionSet.GetValue()->IsPropertySelected(GetSerializedPropertyChain(), InProperty);
		HandleHiddenCustomSerializedProperty(*this, InProperty, *SelectionSet.GetValue(), bShouldSkipProperty);
	}
	
	return bShouldSkipProperty;
}

void FApplySnapshotDataArchiveV2::PushSerializedProperty(FProperty* InProperty, const bool bIsEditorOnlyProperty)
{
	// Do before call to super because super appends InProperty to property chain
	PropertiesLeftToSerialize.RemoveProperty(GetSerializedPropertyChain(), InProperty);
	
	Super::PushSerializedProperty(InProperty, bIsEditorOnlyProperty);

#if WITH_EDITOR
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("PreEditChange"), STAT_PreEditChange, STATGROUP_LevelSnapshots);
	if (FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
	{
		// TODO: chain event
	}
	else
	{
		OriginalObject->PreEditChange(InProperty);
	}
#endif
	
}

void FApplySnapshotDataArchiveV2::PopSerializedProperty(FProperty* InProperty, const bool bIsEditorOnlyProperty)
{
	Super::PopSerializedProperty(InProperty, bIsEditorOnlyProperty);

#if WITH_EDITOR
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("PostEditChange"), STAT_PostEditChange, STATGROUP_LevelSnapshots);
	if (FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
	{
		// TODO: chain event
	}
	else
	{
		FPropertyChangedEvent ChangeEvent(InProperty);
		OriginalObject->PostEditChangeProperty(ChangeEvent);
	}
#endif
}

FApplySnapshotDataArchiveV2::FApplySnapshotDataArchiveV2(FObjectSnapshotData& InObjectData, FWorldSnapshotData& InSharedData, UObject* InOriginalObject, const FPropertySelection& InSelectionSet)
        :
        Super(InObjectData, InSharedData, true),
        SelectionSet(&InSelectionSet),
		PropertiesLeftToSerialize(InSelectionSet),
        OriginalObject(InOriginalObject)
{
	bShouldLoadObjectDependenciesForTempWorld = false;
}

FApplySnapshotDataArchiveV2::FApplySnapshotDataArchiveV2(FObjectSnapshotData& InObjectData, FWorldSnapshotData& InSharedData, UObject* InOriginalObject)
	:
	Super(InObjectData, InSharedData, true),
	OriginalObject(InOriginalObject)
{
	bShouldLoadObjectDependenciesForTempWorld = false;
}
