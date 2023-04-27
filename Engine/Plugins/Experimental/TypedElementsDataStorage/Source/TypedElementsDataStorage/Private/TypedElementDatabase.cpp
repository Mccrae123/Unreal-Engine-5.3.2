// Copyright Epic Games, Inc. All Rights Reserved.

#include "TypedElementDatabase.h"

#include "Editor.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Engine/World.h"
#include "MassCommonTypes.h"
#include "MassEntityEditorSubsystem.h"
#include "MassEntityTypes.h"
#include "MassSimulationSubsystem.h"
#include "MassSubsystemAccess.h"
#include "Processors/TypedElementProcessorAdaptors.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Stats/Stats2.h"
#include "TickTaskManagerInterface.h"
#include "UObject/UObjectIterator.h"

const FName UTypedElementDatabase::TickGroupName_SyncWidget(TEXT("SyncWidgets"));

FAutoConsoleCommandWithOutputDevice PrintQueryCallbacksConsoleCommand(
	TEXT("TEDS.PrintQueryCallbacks"),
	TEXT("Prints out a list of all processors."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Output)
		{
			if (UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
			{
				if (UTypedElementDatabase* DataStorage = Cast<UTypedElementDatabase>(Registry->GetMutableDataStorage()))
				{
					DataStorage->DebugPrintQueryCallbacks(Output);
				}
			}
		}));

FAutoConsoleCommandWithOutputDevice PrintSupportedColumnsConsoleCommand(
	TEXT("TEDS.PrintSupportedColumns"),
	TEXT("Prints out a list of available Data Storage columns."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Output)
		{
			Output.Log(TEXT("The Typed Elements Data Storage supports the following columns:"));
			
			UScriptStruct* FragmentTypeInfo = FMassFragment::StaticStruct();
			UScriptStruct* TagTypeInfo = FMassTag::StaticStruct();
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (It->IsChildOf(FragmentTypeInfo) || It->IsChildOf(TagTypeInfo))
				{
					Output.Logf(TEXT("    %s"), *It->GetFullName());
				}
			}
			Output.Log(TEXT("End of Typed Elements Data Storage supported column list."));
		}));

void UTypedElementDatabase::Initialize()
{
	check(GEditor);
	UMassEntityEditorSubsystem* Mass = GEditor->GetEditorSubsystem<UMassEntityEditorSubsystem>();
	check(Mass);
	Mass->GetOnPreTickDelegate().AddUObject(this, &UTypedElementDatabase::OnPreMassTick);

	ActiveEditorEntityManager = Mass->GetMutableEntityManager();
	ActiveEditorPhaseManager = Mass->GetMutablePhaseManager();

	using PhaseType = std::underlying_type_t<EQueryTickPhase>;
	for (PhaseType PhaseId = 0; PhaseId < static_cast<PhaseType>(EQueryTickPhase::Max); ++PhaseId)
	{
		EQueryTickPhase Phase = static_cast<EQueryTickPhase>(PhaseId);
		EMassProcessingPhase MassPhase = FTypedElementQueryProcessorData::MapToMassProcessingPhase(Phase);
		
		ActiveEditorPhaseManager->GetOnPhaseStart(MassPhase).AddLambda(
			[this, Phase](float DeltaTime)
			{
				PreparePhase(Phase, DeltaTime);
			});

		ActiveEditorPhaseManager->GetOnPhaseEnd(MassPhase).AddLambda(
			[this, Phase](float DeltaTime)
			{
				FinalizePhase(Phase, DeltaTime);
			});

		// Guarantee that syncing to the data storage always happens before syncing to external.
		RegisterTickGroup(GetQueryTickGroupName(EQueryTickGroups::SyncExternalToDataStorage), 
			Phase, GetQueryTickGroupName(EQueryTickGroups::SyncDataStorageToExternal), {}, false);
		// Guarantee that widgets syncs happen after external data has been updated to the data storage.
		RegisterTickGroup(GetQueryTickGroupName(EQueryTickGroups::SyncWidgets),
			Phase, {}, GetQueryTickGroupName(EQueryTickGroups::SyncExternalToDataStorage), false);
	}
}

void UTypedElementDatabase::Deinitialize()
{
	Reset();
}

void UTypedElementDatabase::OnPreMassTick(float DeltaTime)
{
	checkf(IsAvailable(), TEXT("Typed Element Database was ticked while it's not ready."));
	OnUpdateDelegate.Broadcast();
}

TSharedPtr<FMassEntityManager> UTypedElementDatabase::GetActiveMutableEditorEntityManager()
{
	return ActiveEditorEntityManager;
}

TSharedPtr<const FMassEntityManager> UTypedElementDatabase::GetActiveEditorEntityManager() const
{
	return ActiveEditorEntityManager;
}

TypedElementTableHandle UTypedElementDatabase::RegisterTable(TConstArrayView<const UScriptStruct*> ColumnList)
{
	return RegisterTable(ColumnList, {});
}

TypedElementTableHandle UTypedElementDatabase::RegisterTable(TConstArrayView<const UScriptStruct*> ColumnList, const FName Name)
{
	if (ActiveEditorEntityManager && (!Name.IsValid() || !TableNameLookup.Contains(Name)))
	{
		TypedElementTableHandle Result = Tables.Num();
		Tables.Add(ActiveEditorEntityManager->CreateArchetype(ColumnList, Name));
		if (Name.IsValid())
		{
			TableNameLookup.Add(Name, Result);
		}
		return Result;
	}
	return TypedElementInvalidTableHandle;
}

TypedElementTableHandle UTypedElementDatabase::RegisterTable(TypedElementTableHandle SourceTable,
	TConstArrayView<const UScriptStruct*> ColumnList)
{
	return RegisterTable(SourceTable, ColumnList, {});
}

TypedElementTableHandle UTypedElementDatabase::RegisterTable(TypedElementTableHandle SourceTable, 
	TConstArrayView<const UScriptStruct*> ColumnList, const FName Name)
{
	if (ActiveEditorEntityManager && (!Name.IsValid() || !TableNameLookup.Contains(Name)) && SourceTable < Tables.Num())
	{
		TypedElementTableHandle Result = Tables.Num();
		Tables.Add(ActiveEditorEntityManager->CreateArchetype(Tables[SourceTable], ColumnList, Name));
		if (Name.IsValid())
		{
			TableNameLookup.Add(Name, Result);
		}
		return Result;
	}
	return TypedElementInvalidTableHandle;
}

TypedElementTableHandle UTypedElementDatabase::FindTable(const FName Name)
{
	TypedElementTableHandle* TableHandle = TableNameLookup.Find(Name);
	return TableHandle ? *TableHandle : TypedElementInvalidTableHandle;
}

TypedElementRowHandle UTypedElementDatabase::AddRow(TypedElementTableHandle Table)
{
	checkf(Table < Tables.Num(), TEXT("Attempting to add a row to a non-existing table."));
	return ActiveEditorEntityManager ? 
		ActiveEditorEntityManager->CreateEntity(Tables[Table]).AsNumber() :
		TypedElementInvalidRowHandle;
}

TypedElementRowHandle UTypedElementDatabase::AddRow(FName TableName)
{
	TypedElementTableHandle* Table = TableNameLookup.Find(TableName);
	return Table ? AddRow(*Table) : TypedElementInvalidRowHandle;
}

bool UTypedElementDatabase::BatchAddRow(TypedElementTableHandle Table, int32 Count, TypedElementDataStorageCreationCallbackRef OnCreated)
{
	OnCreated.CheckCallable();
	checkf(Table < Tables.Num(), TEXT("Attempting to add multiple rows to a non-existing table."));
	if (ActiveEditorEntityManager)
	{
		TArray<FMassEntityHandle> Entities;
		Entities.Reserve(Count);
		TSharedRef<FMassEntityManager::FEntityCreationContext> Context = 
			ActiveEditorEntityManager->BatchCreateEntities(Tables[Table], Count, Entities);
		
		for (FMassEntityHandle Entity : Entities)
		{
			OnCreated(Entity.AsNumber());
		}

		return true;
	}
	return false;
}

bool UTypedElementDatabase::BatchAddRow(FName TableName, int32 Count, TypedElementDataStorageCreationCallbackRef OnCreated)
{
	TypedElementTableHandle* Table = TableNameLookup.Find(TableName);
	return Table ? BatchAddRow(*Table, Count, OnCreated) : false;
}

void UTypedElementDatabase::RemoveRow(TypedElementRowHandle Row)
{
	if (ActiveEditorEntityManager)
	{
		ActiveEditorEntityManager->DestroyEntity(FMassEntityHandle::FromNumber(Row));
	}
}

bool UTypedElementDatabase::AddColumn(TypedElementRowHandle Row, const UScriptStruct* ColumnType)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		if (ColumnType->IsChildOf(FMassTag::StaticStruct()))
		{
			ActiveEditorEntityManager->AddTagToEntity(Entity, ColumnType);
			return true;
		}
		else if (ColumnType->IsChildOf(FMassFragment::StaticStruct()))
		{
			FStructView Column = ActiveEditorEntityManager->GetFragmentDataStruct(Entity, ColumnType);
			if (!Column.IsValid())
			{
				ActiveEditorEntityManager->AddFragmentToEntity(Entity, ColumnType);
				return true;
			}
		}
	}
	return false;
}

bool UTypedElementDatabase::AddColumn(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName)
{
	bool bExactMatch = true;
	UScriptStruct* ColumnStructInfo = Cast<UScriptStruct>(StaticFindObject(UScriptStruct::StaticClass(), ColumnName, bExactMatch));
	return ColumnStructInfo ? AddColumn(Row, ColumnStructInfo) : false;
}

void UTypedElementDatabase::RemoveColumn(TypedElementRowHandle Row, const UScriptStruct* ColumnType)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		if (ColumnType->IsChildOf(FMassTag::StaticStruct()))
		{
			ActiveEditorEntityManager->RemoveTagFromEntity(Entity, ColumnType);
		}
		else if (ColumnType->IsChildOf(FMassFragment::StaticStruct()))
		{
			ActiveEditorEntityManager->RemoveFragmentFromEntity(Entity, ColumnType);
		}
	}
}

void UTypedElementDatabase::RemoveColumn(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName)
{
	bool bExactMatch = true;
	if (UScriptStruct* ColumnStructInfo = Cast<UScriptStruct>(StaticFindObject(UScriptStruct::StaticClass(), ColumnName, bExactMatch)))
	{
		RemoveColumn(Row, ColumnStructInfo);
	}
}

void* UTypedElementDatabase::AddOrGetColumnData(TypedElementRowHandle Row, const UScriptStruct* ColumnType)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity) &&
		ColumnType && ColumnType->IsChildOf(FMassFragment::StaticStruct()))
	{
		FStructView Column = ActiveEditorEntityManager->GetFragmentDataStruct(Entity, ColumnType);
		if (!Column.IsValid())
		{
			ActiveEditorEntityManager->AddFragmentToEntity(Entity, ColumnType);
			Column = ActiveEditorEntityManager->GetFragmentDataStruct(Entity, ColumnType);
			checkf(Column.IsValid(), TEXT("Added a new column to the Typed Element's data store, but it couldn't be retrieved."));

		}
		return Column.GetMemory();
	}
	return nullptr;
}

ColumnDataResult UTypedElementDatabase::AddOrGetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName)
{
	constexpr bool bExactMatch = true;
	UScriptStruct* FragmentStructInfo = Cast<UScriptStruct>(StaticFindObject(UScriptStruct::StaticClass(), ColumnName, bExactMatch));
	return FragmentStructInfo ?
		ColumnDataResult{ FragmentStructInfo, AddOrGetColumnData(Row, FragmentStructInfo) }:
		ColumnDataResult{ nullptr, nullptr };
}

ColumnDataResult UTypedElementDatabase::AddOrGetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName,
	TConstArrayView<TypedElement::ColumnUtils::Argument> Arguments)
{
	ColumnDataResult Result = AddOrGetColumnData(Row, ColumnName);
	if (Result.Description && Result.Data)
	{
		TypedElement::ColumnUtils::SetColumnValues(Result.Data, Result.Description, Arguments);
		return Result;
	}
	else
	{
		return ColumnDataResult{ nullptr, nullptr };
	}
}

void* UTypedElementDatabase::GetColumnData(TypedElementRowHandle Row, const UScriptStruct* ColumnType)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity) &&
		ColumnType && ColumnType->IsChildOf(FMassFragment::StaticStruct()))
	{
		FStructView Column = ActiveEditorEntityManager->GetFragmentDataStruct(Entity, ColumnType);
		if (Column.IsValid())
		{
			return Column.GetMemory();
		}
	}
	return nullptr;
}

ColumnDataResult UTypedElementDatabase::GetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		const UScriptStruct* FragmentType = nullptr;
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntityUnsafe(Entity);
		ActiveEditorEntityManager->ForEachArchetypeFragmentType(Archetype, 
			[ColumnName, &FragmentType](const UScriptStruct* Fragment)
			{
				if (Fragment->GetStructPathName() == ColumnName)
				{
					FragmentType = Fragment;
				}
			});

		if (FragmentType && FragmentType->IsChildOf(FMassFragment::StaticStruct()))
		{
			FStructView Column = ActiveEditorEntityManager->GetFragmentDataStruct(Entity, FragmentType);
			if (Column.IsValid())
			{
				return ColumnDataResult{ FragmentType, Column.GetMemory() };
			}
		}
	}
	return ColumnDataResult{ nullptr, nullptr };
}

bool UTypedElementDatabase::AddColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> Columns)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);

		FMassFragmentBitSet FragmentsToAdd;
		FMassTagBitSet TagsToAdd;
		if (ColumnsToBitSets(Columns, FragmentsToAdd, TagsToAdd))
		{
			FMassArchetypeCompositionDescriptor AddComposition(
				MoveTemp(FragmentsToAdd), MoveTemp(TagsToAdd), FMassChunkFragmentBitSet(), FMassSharedFragmentBitSet());
			ActiveEditorEntityManager->AddCompositionToEntity_GetDelta(Entity, AddComposition);
			return true;
		}
	}
	return false;
}

void UTypedElementDatabase::RemoveColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> Columns)
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);

		FMassFragmentBitSet FragmentsToRemove;
		FMassTagBitSet TagsToRemove;
		if (ColumnsToBitSets(Columns, FragmentsToRemove, TagsToRemove))
		{
			FMassArchetypeCompositionDescriptor RemoveComposition(
				MoveTemp(FragmentsToRemove), MoveTemp(TagsToRemove), FMassChunkFragmentBitSet(), FMassSharedFragmentBitSet());
			ActiveEditorEntityManager->RemoveCompositionFromEntity(Entity, RemoveComposition);
		}
	}
}

bool UTypedElementDatabase::AddRemoveColumns(TypedElementRowHandle Row,
	TConstArrayView<const UScriptStruct*> ColumnsToAdd, TConstArrayView<const UScriptStruct*> ColumnsToRemove)
{
	bool bResult = false;
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);

		FMassFragmentBitSet FragmentsToAdd;
		FMassTagBitSet TagsToAdd;
		if (ColumnsToBitSets(ColumnsToAdd, FragmentsToAdd, TagsToAdd))
		{
			FMassArchetypeCompositionDescriptor AddComposition(
				MoveTemp(FragmentsToAdd), MoveTemp(TagsToAdd), FMassChunkFragmentBitSet(), FMassSharedFragmentBitSet());
			ActiveEditorEntityManager->AddCompositionToEntity_GetDelta(Entity, AddComposition);
			bResult = true;
		}

		FMassTagBitSet TagsToRemove;
		FMassFragmentBitSet FragmentsToRemove;
		if (ColumnsToBitSets(ColumnsToRemove, FragmentsToRemove, TagsToRemove))
		{
			FMassArchetypeCompositionDescriptor RemoveComposition(
				MoveTemp(FragmentsToRemove), MoveTemp(TagsToRemove), FMassChunkFragmentBitSet(), FMassSharedFragmentBitSet());
			ActiveEditorEntityManager->RemoveCompositionFromEntity(Entity, RemoveComposition);
			bResult = true;
		}
	}
	return bResult;
}

bool UTypedElementDatabase::BatchAddRemoveColumns(TConstArrayView<TypedElementRowHandle> Rows, 
	TConstArrayView<const UScriptStruct*> ColumnsToAdd, TConstArrayView<const UScriptStruct*> ColumnsToRemove)
{
	if (ActiveEditorEntityManager)
	{
		FMassFragmentBitSet FragmentsToAdd;
		FMassFragmentBitSet FragmentsToRemove;

		FMassTagBitSet TagsToAdd;
		FMassTagBitSet TagsToRemove;

		bool bMustUpdateFragments = ColumnsToBitSets(ColumnsToAdd, FragmentsToAdd, TagsToAdd);
		bool bMustUpdateTags = ColumnsToBitSets(ColumnsToRemove, FragmentsToRemove, TagsToRemove);
		
		if (bMustUpdateFragments || bMustUpdateTags)
		{
			using EntityHandleArray = TArray<FMassEntityHandle, TInlineAllocator<32>>;
			using EntityArchetypeLookup = TMap<FMassArchetypeHandle, EntityHandleArray, TInlineSetAllocator<32>>;
			using ArchetypeEntityArray = TArray<FMassArchetypeEntityCollection, TInlineAllocator<32>>;

			// Sort rows (entities) into to matching table (archetype) bucket.
			EntityArchetypeLookup LookupTable;
			for (TypedElementRowHandle EntityId : Rows)
			{
				FMassEntityHandle Entity = FMassEntityHandle::FromNumber(EntityId);
				if (ActiveEditorEntityManager->IsEntityValid(Entity))
				{
					FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);
					EntityHandleArray& EntityCollection = LookupTable.FindOrAdd(Archetype);
					EntityCollection.Add(Entity);
				}
			}
			
			// Construct table (archetype) specific row (entity) collections.
			ArchetypeEntityArray EntityCollections;
			EntityCollections.Reserve(LookupTable.Num());
			for (auto It = LookupTable.CreateConstIterator(); It; ++It)
			{
				EntityCollections.Emplace(It.Key(), It.Value(), FMassArchetypeEntityCollection::EDuplicatesHandling::FoldDuplicates);
			}

			// Batch update usint the appropriate fragment/bit sets.
			if (bMustUpdateFragments)
			{
				ActiveEditorEntityManager->BatchChangeFragmentCompositionForEntities(EntityCollections, FragmentsToAdd, FragmentsToRemove);
			}
			if (bMustUpdateTags)
			{
				ActiveEditorEntityManager->BatchChangeTagsForEntities(EntityCollections, TagsToAdd, TagsToRemove);
			}
			return true;
		}
	}
	return false;
}

bool UTypedElementDatabase::HasColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> ColumnTypes) const
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);
		const FMassArchetypeCompositionDescriptor& Composition = ActiveEditorEntityManager->GetArchetypeComposition(Archetype);

		bool bHasAllColumns = true;
		const UScriptStruct* const* ColumnTypesEnd = ColumnTypes.end();
		for (const UScriptStruct* const* ColumnType = ColumnTypes.begin(); ColumnType != ColumnTypesEnd && bHasAllColumns; ++ColumnType)
		{
			if ((*ColumnType)->IsChildOf(FMassFragment::StaticStruct()))
			{
				bHasAllColumns = Composition.Fragments.Contains(**ColumnType);
			}
			else if ((*ColumnType)->IsChildOf(FMassTag::StaticStruct()))
			{
				bHasAllColumns = Composition.Tags.Contains(**ColumnType);
			}
			else
			{
				return false;
			}
		}

		return bHasAllColumns;
	}
	return false;
}

bool UTypedElementDatabase::HasColumns(TypedElementRowHandle Row, TConstArrayView<TWeakObjectPtr<const UScriptStruct>> ColumnTypes) const
{
	FMassEntityHandle Entity = FMassEntityHandle::FromNumber(Row);
	if (ActiveEditorEntityManager && ActiveEditorEntityManager->IsEntityValid(Entity))
	{
		FMassArchetypeHandle Archetype = ActiveEditorEntityManager->GetArchetypeForEntity(Entity);
		const FMassArchetypeCompositionDescriptor& Composition = ActiveEditorEntityManager->GetArchetypeComposition(Archetype);

		bool bHasAllColumns = true;
		const TWeakObjectPtr<const UScriptStruct>* ColumnTypesEnd = ColumnTypes.end();
		for (const TWeakObjectPtr<const UScriptStruct>* ColumnType = ColumnTypes.begin(); ColumnType != ColumnTypesEnd && bHasAllColumns; ++ColumnType)
		{
			if (ColumnType->IsValid())
			{
				if ((*ColumnType)->IsChildOf(FMassFragment::StaticStruct()))
				{
					bHasAllColumns = Composition.Fragments.Contains(**ColumnType);
					continue;
				}
				else if ((*ColumnType)->IsChildOf(FMassTag::StaticStruct()))
				{
					bHasAllColumns = Composition.Tags.Contains(**ColumnType);
					continue;
				}
			}
			return false;
		}

		return bHasAllColumns;
	}
	return false;
}

void UTypedElementDatabase::RegisterTickGroup(
	FName GroupName, EQueryTickPhase Phase, FName BeforeGroup, FName AfterGroup, bool bRequiresMainThread)
{
	FTickGroupDescription& Group = TickGroupDescriptions.FindOrAdd({ GroupName, Phase });

	if (!Group.BeforeGroups.Find(BeforeGroup))
	{
		Group.BeforeGroups.Add(BeforeGroup);
	}

	if (!Group.AfterGroups.Find(AfterGroup))
	{
		Group.AfterGroups.Add(AfterGroup);
	}

	if (bRequiresMainThread)
	{
		Group.bRequiresMainThread = true;
	}
}

void UTypedElementDatabase::UnregisterTickGroup(FName GroupName, EQueryTickPhase Phase)
{
	TickGroupDescriptions.Remove({ GroupName, Phase });
}

TypedElementQueryHandle UTypedElementDatabase::RegisterQuery(FQueryDescription&& Query)
{
	auto LocalToNativeAccess = [](EQueryAccessType Access) -> EMassFragmentAccess
	{
		switch (Access)
		{
			case EQueryAccessType::ReadOnly:
				return EMassFragmentAccess::ReadOnly;
			case EQueryAccessType::ReadWrite:
				return EMassFragmentAccess::ReadWrite;
			default:
				checkf(false, TEXT("Invalid query access type: %i."), static_cast<uint32>(Access));
				return EMassFragmentAccess::MAX;
		}
	};

	auto SetupNativeQuery = [](FQueryDescription& Query, FTypedElementDatabaseExtendedQuery& StoredQuery) -> FMassEntityQuery&
	{
		if (Query.Action == FQueryDescription::EActionType::Select)
		{
			switch (Query.Callback.Type)
			{
				case EQueryCallbackType::None:
					break;
				case EQueryCallbackType::Processor:
				{
					UTypedElementQueryProcessorCallbackAdapterProcessor* Processor = 
						NewObject<UTypedElementQueryProcessorCallbackAdapterProcessor>();
					StoredQuery.Processor.Reset(Processor);
					return Processor->GetQuery();
				}
				case EQueryCallbackType::ObserveAdd:
					// Fallthrough
				case EQueryCallbackType::ObserveRemove:
				{
					UTypedElementQueryObserverCallbackAdapterProcessor* Observer = 
						NewObject<UTypedElementQueryObserverCallbackAdapterProcessor>();
					StoredQuery.Processor.Reset(Observer);
					return Observer->GetQuery();
				}
				case EQueryCallbackType::PhasePreparation:
					break;
				case EQueryCallbackType::PhaseFinalization:
					break;
				default:
					checkf(false, TEXT("Unsupported query callback type %i."), static_cast<int>(Query.Callback.Type));
					break;
			}
		}
		return StoredQuery.NativeQuery;
	};

	QueryStore::Handle Result = Queries.Emplace();
	FTypedElementDatabaseExtendedQuery& StoredQuery = Queries.GetMutable(Result);
	
	FMassEntityQuery& NativeQuery = SetupNativeQuery(Query, StoredQuery);

	// Setup selected columns section
	if (Query.Action == FQueryDescription::EActionType::Count)
	{
		checkf(Query.SelectionTypes.IsEmpty(), TEXT("Count queries for the Typed Elements Data Storage can't have entries for selection."));
		checkf(Query.SelectionAccessTypes.IsEmpty(), TEXT("Count queries for the Typed Elements Data Storage can't have entries for selection."));
	}
	else if (Query.Action == FQueryDescription::EActionType::Select)
	{
		const int32 SelectionCount = Query.SelectionTypes.Num();
		checkf(SelectionCount == Query.SelectionAccessTypes.Num(),
			TEXT("The number of query selection types (%i) doesn't match the number of selection access types (%i)."),
			SelectionCount, Query.SelectionAccessTypes.Num());
		for (int SelectionIndex = 0; SelectionIndex < SelectionCount; ++SelectionIndex)
		{
			TWeakObjectPtr<const UScriptStruct>& Type = Query.SelectionTypes[SelectionIndex];
			checkf(Type.IsValid(), TEXT("Provided query selection type can not be null."));
			checkf(
				Type->IsChildOf(FTypedElementDataStorageColumn::StaticStruct()) ||
				Type->IsChildOf(FMassFragment::StaticStruct()) || 
				Type->IsChildOf(FMassTag::StaticStruct()),
				TEXT("Provided query selection type '%s' is not based on FTypedElementDataStorageColumn or another supported base type."), 
				*Type->GetStructPathName().ToString());
			NativeQuery.AddRequirement(Type.Get(), LocalToNativeAccess(Query.SelectionAccessTypes[SelectionIndex]));
		}
	}
	else
	{
		checkf(Query.Action == FQueryDescription::EActionType::None, TEXT("Unexpected query action: %i."), static_cast<int32>(Query.Action));
	}

	// Configure conditions
	if (Query.bSimpleQuery) // This backend currently only supports simple queries.
	{
		checkf(Query.ConditionTypes.Num() == Query.ConditionOperators.Num(),
			TEXT("The types and operators for a typed element query have gone out of sync."));
		
		const FQueryDescription::FOperator* Operand = Query.ConditionOperators.GetData();
		for (FQueryDescription::EOperatorType Type : Query.ConditionTypes)
		{
			EMassFragmentPresence Presence;
			switch (Type)
			{
				case FQueryDescription::EOperatorType::SimpleAll:
					Presence = EMassFragmentPresence::All;
					break;
				case FQueryDescription::EOperatorType::SimpleAny:
					Presence = EMassFragmentPresence::Any;
					break;
				case FQueryDescription::EOperatorType::SimpleNone:
					Presence = EMassFragmentPresence::None;
					break;
				default:
					continue;
			}

			if (Operand->Type->IsChildOf(FMassTag::StaticStruct()))
			{
				NativeQuery.AddTagRequirement(*(Operand->Type), Presence);
			}
			else if (Operand->Type->IsChildOf(FMassFragment::StaticStruct()))
			{
				NativeQuery.AddRequirement(Operand->Type.Get(), EMassFragmentAccess::None, Presence);
			}

			++Operand;
		}
	}

	// Assign dependencies.
	const int32 DependencyCount = Query.DependencyTypes.Num();
	checkf(DependencyCount == Query.DependencyFlags.Num() && DependencyCount == Query.CachedDependencies.Num(),
		TEXT("The number of query depedencies (%i) doesn't match the number of dependency access types (%i) and/or cached dependencies count (%i)."),
		DependencyCount, Query.DependencyFlags.Num(), Query.CachedDependencies.Num());
	for (int32 DependencyIndex = 0; DependencyIndex < DependencyCount; ++DependencyIndex)
	{
		TWeakObjectPtr<const UClass>& Type = Query.DependencyTypes[DependencyIndex];
		checkf(Type.IsValid(), TEXT("Provided query dependcy type can not be null."));
		checkf(Type->IsChildOf<USubsystem>(), TEXT("Provided query dependency type '%s' is not based on USubSystem."), 
			*Type->GetStructPathName().ToString());
		
		EQueryDependencyFlags Flags = Query.DependencyFlags[DependencyIndex];
		NativeQuery.AddSubsystemRequirement(
			const_cast<UClass*>(Type.Get()), 
			EnumHasAllFlags(Flags, EQueryDependencyFlags::ReadOnly) ? EMassFragmentAccess::ReadOnly : EMassFragmentAccess::ReadWrite,
			EnumHasAllFlags(Flags, EQueryDependencyFlags::GameThreadBound));
	}

	// Copy pre-registered phase and group information.
	const FTickGroupDescription* TickGroup = TickGroupDescriptions.Find({ Query.Callback.Group, Query.Callback.Phase });
	if (TickGroup)
	{
		for (auto It = Query.Callback.BeforeGroups.CreateIterator(); It; ++It)
		{
			if (TickGroup->BeforeGroups.Contains(*It))
			{
				It.RemoveCurrentSwap();
			}
		}
		Query.Callback.BeforeGroups.Append(TickGroup->BeforeGroups);
		for (auto It = Query.Callback.AfterGroups.CreateIterator(); It; ++It)
		{
			if (TickGroup->AfterGroups.Contains(*It))
			{
				It.RemoveCurrentSwap();
			}
		}
		Query.Callback.AfterGroups.Append(TickGroup->AfterGroups);
		if (TickGroup->bRequiresMainThread)
		{
			Query.Callback.bForceToGameThread = true;
		}
	}

	// Register Phase processors locally.
	switch (Query.Callback.Type)
	{
	case EQueryCallbackType::PhasePreparation:
		PhasePreparationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(Query.Callback.Phase)].Add(Result.Handle);
		break;
	case EQueryCallbackType::PhaseFinalization:
		PhaseFinalizationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(Query.Callback.Phase)].Add(Result.Handle);
		break;
	}

	StoredQuery.Description = MoveTemp(Query);

	// Register regular processors and observer with Mass.
	if (StoredQuery.Processor)
	{
		if (StoredQuery.Processor->IsA<UTypedElementQueryProcessorCallbackAdapterProcessor>())
		{
			if (UMassEntityEditorSubsystem* Mass = GEditor->GetEditorSubsystem<UMassEntityEditorSubsystem>())
			{
				static_cast<UTypedElementQueryProcessorCallbackAdapterProcessor*>(StoredQuery.Processor.Get())->
					ConfigureQueryCallback(StoredQuery);
				Mass->RegisterDynamicProcessor(*StoredQuery.Processor);
			}
		}
		else if (StoredQuery.Processor->IsA<UTypedElementQueryObserverCallbackAdapterProcessor>())
		{
			UTypedElementQueryObserverCallbackAdapterProcessor* Observer =
				static_cast<UTypedElementQueryObserverCallbackAdapterProcessor*>(StoredQuery.Processor.Get());
			Observer->ConfigureQueryCallback(StoredQuery);
			ActiveEditorEntityManager->GetObserverManager().AddObserverInstance(
				*Observer->GetObservedType(), Observer->GetObservedOperation(), *Observer);
		}
		else
		{
			checkf(false, TEXT("Query processor %s is of unsupported type %s."), 
				*Query.Callback.Name.ToString(), *StoredQuery.Processor->GetSparseClassDataStruct()->GetName());
		}
	}

	return Result.Handle;
}

void UTypedElementDatabase::UnregisterQuery(TypedElementQueryHandle Query)
{
	QueryStore::Handle Handle;
	Handle.Handle = Query;

	if (Queries.IsAlive(Handle))
	{
		FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
		if (QueryData.Processor)
		{
			if (QueryData.Processor->IsA<UTypedElementQueryProcessorCallbackAdapterProcessor>())
			{
				if (UMassEntityEditorSubsystem* Mass = GEditor->GetEditorSubsystem<UMassEntityEditorSubsystem>())
				{
					Mass->UnregisterDynamicProcessor(*QueryData.Processor);
				}
			}
			else if (QueryData.Processor->IsA<UTypedElementQueryObserverCallbackAdapterProcessor>())
			{
				checkf(false, TEXT("Observer queries can not be unregistered."));
			}
			else
			{
				checkf(false, TEXT("Query processor %s is of unsupported type %s."),
					*QueryData.Description.Callback.Name.ToString(), *QueryData.Processor->GetSparseClassDataStruct()->GetName());
			}
		}
		else if (QueryData.Description.Callback.Type == EQueryCallbackType::PhasePreparation)
		{
			int32 Index;
			if (PhasePreparationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(QueryData.Description.Callback.Phase)].Find(Query, Index))
			{
				PhasePreparationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(QueryData.Description.Callback.Phase)].RemoveAt(Index);
			}
		}
		else if (QueryData.Description.Callback.Type == EQueryCallbackType::PhaseFinalization)
		{
			int32 Index;
			if (PhaseFinalizationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(QueryData.Description.Callback.Phase)].Find(Query, Index))
			{
				PhaseFinalizationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(QueryData.Description.Callback.Phase)].RemoveAt(Index);
			}
		}
		else
		{
			QueryData.NativeQuery.Clear();
		}
	}

	Queries.Remove(Handle);
}

const ITypedElementDataStorageInterface::FQueryDescription& UTypedElementDatabase::GetQueryDescription(TypedElementQueryHandle Query) const
{
	QueryStore::Handle Handle;
	Handle.Handle = Query;

	if (Queries.IsAlive(Handle))
	{
		const FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
		return QueryData.Description;
	}
	else
	{
		static ITypedElementDataStorageInterface::FQueryDescription EmptyDescription;
		return EmptyDescription;
	}
}

FName UTypedElementDatabase::GetQueryTickGroupName(EQueryTickGroups Group) const
{
	switch (Group)
	{
		case EQueryTickGroups::Default:
			return NAME_None;
		case EQueryTickGroups::SyncExternalToDataStorage:
			return UE::Mass::ProcessorGroupNames::SyncWorldToMass;
		case EQueryTickGroups::SyncDataStorageToExternal:
			return UE::Mass::ProcessorGroupNames::UpdateWorldFromMass;
		case EQueryTickGroups::SyncWidgets:
			return TickGroupName_SyncWidget;
		default:
			checkf(false, TEXT("EQueryTickGroups value %i can't be translated to a group name by this Data Storage backend."), static_cast<int>(Group));
			return NAME_None;
	}
}

ITypedElementDataStorageInterface::FQueryResult UTypedElementDatabase::RunQuery(TypedElementQueryHandle Query)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(TEDS.RunQuery);
	
	FQueryResult Result;

	QueryStore::Handle Handle;
	Handle.Handle = Query;

	if (Queries.IsAlive(Handle))
	{
		FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
		if (QueryData.Description.bSimpleQuery)
		{
			switch (QueryData.Description.Action)
			{
				case FQueryDescription::EActionType::None:
					Result.Completed = FQueryResult::ECompletion::Fully;
					break;
				case FQueryDescription::EActionType::Select:
					// Fallthrough: There's nothing to callback to, so only return the total count.
				case FQueryDescription::EActionType::Count:
					if (ActiveEditorEntityManager)
					{
						Result.Count = QueryData.NativeQuery.GetNumMatchingEntities(*ActiveEditorEntityManager);
						Result.Completed = FQueryResult::ECompletion::Fully;
					}
					else
					{
						Result.Completed = FQueryResult::ECompletion::Unavailable;
					}
					break;
				default:
					Result.Completed = FQueryResult::ECompletion::Unsupported;
					break;
			}
		}
		else
		{
			checkf(false, TEXT("Support for this option will be coming in a future update."));
			Result.Completed = FQueryResult::ECompletion::Unsupported;
		}
	}
	else
	{
		Result.Completed = FQueryResult::ECompletion::Unavailable;
	}

	return Result;
}

ITypedElementDataStorageInterface::FQueryResult UTypedElementDatabase::RunQuery(
	TypedElementQueryHandle Query, ITypedElementDataStorageInterface::DirectQueryCallbackRef Callback)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(TEDS.RunQuery);

	FQueryResult Result;

	QueryStore::Handle Handle;
	Handle.Handle = Query;

	if (Queries.IsAlive(Handle))
	{
		FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
		if (QueryData.Description.bSimpleQuery)
		{
			switch (QueryData.Description.Action)
			{
			case FQueryDescription::EActionType::None:
				Result.Completed = FQueryResult::ECompletion::Fully;
				break;
			case FQueryDescription::EActionType::Select:
				if (ActiveEditorEntityManager)
				{
					if (!QueryData.Processor.IsValid())
					{
						Result = FTypedElementQueryProcessorData::Execute(
							Callback, QueryData.Description, QueryData.NativeQuery, *ActiveEditorEntityManager);
					}
					else
					{
						Result.Completed = FQueryResult::ECompletion::Unsupported;
					}
				}
				else
				{
					Result.Completed = FQueryResult::ECompletion::Unavailable;
				}
				break;
			case FQueryDescription::EActionType::Count:
				// Only the count is requested so no need to trigger the callback.
				if (ActiveEditorEntityManager)
				{
					Result.Count = QueryData.NativeQuery.GetNumMatchingEntities(*ActiveEditorEntityManager);
					Result.Completed = FQueryResult::ECompletion::Fully;
				}
				else
				{
					Result.Completed = FQueryResult::ECompletion::Unavailable;
				}
				break;
			default:
				Result.Completed = FQueryResult::ECompletion::Unsupported;
				break;
			}
		}
		else
		{
			checkf(false, TEXT("Support for this option will be coming in a future update."));
			Result.Completed = FQueryResult::ECompletion::Unsupported;
		}
	}
	else
	{
		Result.Completed = FQueryResult::ECompletion::Unavailable;
	}

	return Result;
}

FTypedElementOnDataStorageUpdate& UTypedElementDatabase::OnUpdate()
{
	return OnUpdateDelegate;
}

bool UTypedElementDatabase::IsAvailable() const
{
	return bool(ActiveEditorEntityManager);
}

void* UTypedElementDatabase::GetExternalSystemAddress(UClass* Target)
{
	if (Target && Target->IsChildOf<USubsystem>())
	{
		return FMassSubsystemAccess::FetchSubsystemInstance(/*World=*/nullptr, Target);
	}
	return nullptr;
}

bool UTypedElementDatabase::ColumnsToBitSets(TConstArrayView<const UScriptStruct*> Columns, FMassFragmentBitSet& Fragments, FMassTagBitSet& Tags)
{
	bool bResult = false;
	for (const UScriptStruct* ColumnType : Columns)
	{
		if (ColumnType->IsChildOf(FMassFragment::StaticStruct()))
		{
			Fragments.Add(*ColumnType);
			bResult = true;
		}
		else if (ColumnType->IsChildOf(FMassTag::StaticStruct()))
		{
			Tags.Add(*ColumnType);
			bResult = true;
		}
	}
	return bResult;
}

void UTypedElementDatabase::PreparePhase(EQueryTickPhase Phase, float DeltaTime)
{
	PhasePreOrPostAmble(Phase, DeltaTime, PhasePreparationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(Phase)]);
}

void UTypedElementDatabase::FinalizePhase(EQueryTickPhase Phase, float DeltaTime)
{
	PhasePreOrPostAmble(Phase, DeltaTime, PhaseFinalizationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(Phase)]);
}

void UTypedElementDatabase::PhasePreOrPostAmble(EQueryTickPhase Phase, float DeltaTime, TArray<TypedElementQueryHandle>& QueryHandles)
{
	if (ActiveEditorEntityManager && !QueryHandles.IsEmpty())
	{
		FPhasePreOrPostAmbleExecutor Executor(*ActiveEditorEntityManager, DeltaTime);
		for (TypedElementQueryHandle Query : QueryHandles)
		{
			QueryStore::Handle Handle;
			Handle.Handle = Query;
			FTypedElementDatabaseExtendedQuery& QueryData = Queries.GetMutable(Handle);
			Executor.ExecuteQuery(QueryData.Description, QueryData.NativeQuery, QueryData.Description.Callback.Function);
		}
	}
}

void UTypedElementDatabase::Reset()
{
	Tables.Reset();
	TableNameLookup.Reset();
	ActiveEditorEntityManager.Reset();
}

void UTypedElementDatabase::DebugPrintQueryCallbacks(FOutputDevice& Output)
{
	Output.Log(TEXT("The Typed Elements Data Storage has the following query callbacks:"));
	Queries.ListAliveEntries(
		[&Output](const FTypedElementDatabaseExtendedQuery& Query)
		{
			if (Query.Processor)
			{
				Output.Logf(TEXT("    [%s] %s"), 
					IsValid(Query.Processor.Get()) ? TEXT("Valid") : TEXT("Invalid"),
					*(Query.Processor->GetProcessorName()));
			}
		});

	using PhaseType = std::underlying_type_t<EQueryTickPhase>;
	for (PhaseType PhaseId = 0; PhaseId < static_cast<PhaseType>(EQueryTickPhase::Max); ++PhaseId)
	{
		for (TypedElementQueryHandle QueryHandle : PhasePreparationQueries[PhaseId])
		{
			QueryStore::Handle Handle;
			Handle.Handle = QueryHandle;
			const FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
			Output.Logf(TEXT("    [Valid] %s [Editor Phase Preamble]"), *QueryData.Description.Callback.Name.ToString());
		}
		for (TypedElementQueryHandle QueryHandle : PhaseFinalizationQueries[PhaseId])
		{
			QueryStore::Handle Handle;
			Handle.Handle = QueryHandle;
			const FTypedElementDatabaseExtendedQuery& QueryData = Queries.Get(Handle);
			Output.Logf(TEXT("    [Valid] %s [Editor Phase Postamble]"), *QueryData.Description.Callback.Name.ToString());
		}
	}

	Output.Log(TEXT("End of Typed Elements Data Storage query callback list."));
}