// Copyright Epic Games, Inc. All Rights Reserved.

#include "MeshSelectors/PCGMeshSelectorWeightedByCategory.h"

#include "Data/PCGPointData.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Data/PCGSpatialData.h"
#include "Helpers/PCGBlueprintHelpers.h"

#include "Math/RandomStream.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGMeshSelectorWeightedByCategory)

struct FPCGInstancesAndWeights
{
	TArray<TArray<FPCGMeshInstanceList>> MeshInstances;
	TArray<int> CumulativeWeights;
};

void UPCGMeshSelectorWeightedByCategory::SelectInstances_Implementation(
	FPCGContext& Context,
	const UPCGStaticMeshSpawnerSettings* Settings,
	const UPCGPointData* InPointData,
	TArray<FPCGMeshInstanceList>& OutMeshInstances,
	UPCGPointData* OutPointData) const
{
	if (!InPointData)
	{
		PCGE_LOG_C(Error, &Context, "Missing input data");
		return;
	}

	if (!InPointData->Metadata)
	{
		PCGE_LOG_C(Error, &Context, "Unable to get metadata from input");
		return;
	}

	if (!InPointData->Metadata->HasAttribute(CategoryAttribute))
	{
		PCGE_LOG_C(Error, &Context, "Attribute %s is not in the metadata", *CategoryAttribute.ToString());
		return;
	}

	const FPCGMetadataAttributeBase* AttributeBase = InPointData->Metadata->GetConstAttribute(CategoryAttribute);
	check(AttributeBase);

	// TODO: support enum type as well
	if (AttributeBase->GetTypeId() != PCG::Private::MetadataTypes<FString>::Id)
	{
		PCGE_LOG_C(Error, &Context, "Attribute is not of valid type FString");
		return;
	}

	const FPCGMetadataAttribute<FString>* Attribute = static_cast<const FPCGMetadataAttribute<FString>*>(AttributeBase);

	// maps a CategoryEntry ValueKey to the meshes and precomputed weight data 
	TMap<PCGMetadataValueKey, FPCGInstancesAndWeights> CategoryEntryToInstancesAndWeights;

	// unmarked points will fallback to the MeshEntries associated with the DefaultValueKey
	PCGMetadataValueKey DefaultValueKey = PCGDefaultValueKey;

	for (const FPCGWeightedByCategoryEntryList& Entry : Entries)
	{
		if (Entry.WeightedMeshEntries.Num() == 0)
		{
			PCGE_LOG_C(Verbose, &Context, "Empty entry found in category %s", *Entry.CategoryEntry);
			continue;
		}

		PCGMetadataValueKey ValueKey = Attribute->FindValue<FString>(Entry.CategoryEntry);

		if (ValueKey == PCGDefaultValueKey)
		{
			PCGE_LOG_C(Verbose, &Context, "Invalid category %s", *Entry.CategoryEntry);
			continue;
		}

		FPCGInstancesAndWeights* InstancesAndWeights = CategoryEntryToInstancesAndWeights.Find(ValueKey);

		if (InstancesAndWeights)
		{
			PCGE_LOG_C(Warning, &Context, "Duplicate entry found in category %s. Subsequent entries are ignored.", *Entry.CategoryEntry);
			continue;
		}

		if (Entry.IsDefault)
		{
			if (DefaultValueKey == PCGDefaultValueKey)
			{
				DefaultValueKey = ValueKey;
			}
			else
			{
				PCGE_LOG_C(Warning, &Context, "Duplicate default entry found. Subsequent default entries are ignored.");
			}
		}

		InstancesAndWeights = &CategoryEntryToInstancesAndWeights.Add(ValueKey, FPCGInstancesAndWeights());

		int TotalWeight = 0;
		for (const FPCGMeshSelectorWeightedEntry& WeightedEntry : Entry.WeightedMeshEntries)
		{
			if (WeightedEntry.Weight <= 0)
			{
				PCGE_LOG_C(Verbose, &Context, "Entry found with weight <= 0 in category %s", *Entry.CategoryEntry);
				continue;
			}
			
			TArray<FPCGMeshInstanceList>& PickEntry = InstancesAndWeights->MeshInstances.Emplace_GetRef();
			FindOrAddInstanceList(PickEntry, WeightedEntry.Mesh, WeightedEntry.bOverrideCollisionProfile, WeightedEntry.CollisionProfile, WeightedEntry.bOverrideMaterials, WeightedEntry.MaterialOverrides, WeightedEntry.CullStartDistance, WeightedEntry.CullEndDistance, WeightedEntry.WorldPositionOffsetDisableDistance, /*bReverseCulling=*/false);

			// precompute the weights
			TotalWeight += WeightedEntry.Weight;
			InstancesAndWeights->CumulativeWeights.Add(TotalWeight);
		}

		// if no weighted entries were collected, discard this InstancesAndWeights
		if (InstancesAndWeights->CumulativeWeights.Num() == 0)
		{
			CategoryEntryToInstancesAndWeights.Remove(ValueKey);
		}
	}

	TArray<FPCGPoint>* OutPoints = nullptr;
	FPCGMetadataAttribute<FString>* OutAttribute = nullptr;
	TMap<TSoftObjectPtr<UStaticMesh>, PCGMetadataValueKey> MeshToValueKey;

	FPCGMeshMaterialOverrideHelper MaterialOverrideHelper(Context, bUseAttributeMaterialOverrides, MaterialOverrideAttributes, InPointData->Metadata);

	if (!MaterialOverrideHelper.IsValid())
	{
		return;
	}

	if (OutPointData)
	{
		check(OutPointData->Metadata);

		if (!OutPointData->Metadata->HasAttribute(Settings->OutAttributeName)) 
		{
			PCGE_LOG_C(Error, &Context, "Out attribute %s is not in the metadata", *Settings->OutAttributeName.ToString());
		}

		FPCGMetadataAttributeBase* OutAttributeBase = OutPointData->Metadata->GetMutableAttribute(Settings->OutAttributeName);

		if (OutAttributeBase)
		{
			if (OutAttributeBase->GetTypeId() == PCG::Private::MetadataTypes<FString>::Id)
			{
				OutAttribute = static_cast<FPCGMetadataAttribute<FString>*>(OutAttributeBase);
				OutPoints = &OutPointData->GetMutablePoints();
			}
			else
			{
				PCGE_LOG_C(Error, &Context, "Out attribute is not of valid type FString");
			}
		}
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGStaticMeshSpawnerElement::Execute::SelectEntries);

	// Assign points to entries
	for (const FPCGPoint& Point : InPointData->GetPoints())
	{
		if (Point.Density <= 0.0f)
		{
			continue;
		}

		PCGMetadataValueKey ValueKey = Attribute->GetValueKey(Point.MetadataEntry);

		// if no mesh list was processed for this attribute value, fallback to the default mesh list
		FPCGInstancesAndWeights* InstancesAndWeights = CategoryEntryToInstancesAndWeights.Find(ValueKey);
		if (!InstancesAndWeights)
		{
			if (DefaultValueKey != PCGDefaultValueKey)
			{
				InstancesAndWeights = CategoryEntryToInstancesAndWeights.Find(DefaultValueKey);
				check(InstancesAndWeights);
			}
			else
			{
				continue;
			}
		}

		const int TotalWeight = InstancesAndWeights->CumulativeWeights.Last();

		FRandomStream RandomSource = UPCGBlueprintHelpers::GetRandomStream(Point, Settings, Context.SourceComponent.Get());
		int RandomWeightedPick = RandomSource.RandRange(0, TotalWeight - 1);

		int RandomPick = 0;
		while(RandomPick < InstancesAndWeights->MeshInstances.Num() && InstancesAndWeights->CumulativeWeights[RandomPick] <= RandomWeightedPick)
		{
			++RandomPick;
		}

		if(RandomPick < InstancesAndWeights->MeshInstances.Num())
		{
			const bool bNeedsReverseCulling = (Point.Transform.GetDeterminant() < 0);
			FPCGMeshInstanceList& InstanceList = PCGMeshSelectorWeighted::GetInstanceList(InstancesAndWeights->MeshInstances[RandomPick], bUseAttributeMaterialOverrides, MaterialOverrideHelper.GetMaterialOverrides(Point.MetadataEntry), bNeedsReverseCulling);
			InstanceList.Instances.Emplace(Point);

			const TSoftObjectPtr<UStaticMesh>& Mesh = InstanceList.Mesh;

			if (OutPointData && OutAttribute)
			{
				PCGMetadataValueKey* OutValueKey = MeshToValueKey.Find(Mesh);
				if(!OutValueKey)
				{
					PCGMetadataValueKey TempValueKey = OutAttribute->AddValue(Mesh.ToSoftObjectPath().ToString());
					OutValueKey = &MeshToValueKey.Add(Mesh, TempValueKey);
				}
				
				check(OutValueKey);
				
				FPCGPoint& OutPoint = OutPoints->Add_GetRef(Point);
				OutPointData->Metadata->InitializeOnSet(OutPoint.MetadataEntry);
				OutAttribute->SetValueFromValueKey(OutPoint.MetadataEntry, *OutValueKey);
			}
		}
	}

	// Collapse to OutMeshInstances
	for (TPair<PCGMetadataValueKey, FPCGInstancesAndWeights>& Entry : CategoryEntryToInstancesAndWeights)
	{
		for (TArray<FPCGMeshInstanceList>& PickedMeshInstances : Entry.Value.MeshInstances)
		{
			for (FPCGMeshInstanceList& PickedMeshInstanceEntry : PickedMeshInstances)
			{
				OutMeshInstances.Emplace(MoveTemp(PickedMeshInstanceEntry));
			}
		}
	}
}
