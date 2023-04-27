// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SharedPointer.h"
#include "Elements/Interfaces/TypedElementDataStorageInterface.h"
#include "MassArchetypeTypes.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "TypedElementHandleStore.h"
#include "UObject/ObjectMacros.h"
#include "UObject/StrongObjectPtr.h"

#include "TypedElementDatabase.generated.h"

struct FMassEntityManager;
struct FMassProcessingPhaseManager;
class FOutputDevice;
class UWorld;

struct FTypedElementDatabaseExtendedQuery
{
	FMassEntityQuery NativeQuery; // Used if there's no processor bound.
	ITypedElementDataStorageInterface::FQueryDescription Description;
	TStrongObjectPtr<UMassProcessor> Processor;
};

UCLASS()
class TYPEDELEMENTSDATASTORAGE_API UTypedElementDatabase 
	: public UObject
	, public ITypedElementDataStorageInterface
{
	GENERATED_BODY()

public:
	~UTypedElementDatabase() override = default;
	
	void Initialize();
	void Deinitialize();

	/** Triggered just before underlying Mass processing gets ticked */
	void OnPreMassTick(float DeltaTime);

	TSharedPtr<FMassEntityManager> GetActiveMutableEditorEntityManager();
	TSharedPtr<const FMassEntityManager> GetActiveEditorEntityManager() const;

	TypedElementTableHandle RegisterTable(TConstArrayView<const UScriptStruct*> ColumnList) override;
	TypedElementTableHandle RegisterTable(TConstArrayView<const UScriptStruct*> ColumnList, const FName Name) override;
	TypedElementTableHandle RegisterTable(TypedElementTableHandle SourceTable, TConstArrayView<const UScriptStruct*> ColumnList) override;
	TypedElementTableHandle RegisterTable(TypedElementTableHandle SourceTable, TConstArrayView<const UScriptStruct*> ColumnList, 
		const FName Name) override;
	TypedElementTableHandle FindTable(const FName Name) override;

	TypedElementRowHandle AddRow(TypedElementTableHandle Table) override;
	TypedElementRowHandle AddRow(FName TableName) override;
	bool BatchAddRow(TypedElementTableHandle Table, int32 Count, TypedElementDataStorageCreationCallbackRef OnCreated) override;
	bool BatchAddRow(FName TableName, int32 Count, TypedElementDataStorageCreationCallbackRef OnCreated) override;
	void RemoveRow(TypedElementRowHandle Row) override;

	bool AddColumn(TypedElementRowHandle Row, const UScriptStruct* ColumnType) override;
	bool AddColumn(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName) override;
	void RemoveColumn(TypedElementRowHandle Row, const UScriptStruct* ColumnType) override;
	void RemoveColumn(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName) override;
	void* AddOrGetColumnData(TypedElementRowHandle Row, const UScriptStruct* ColumnType) override;
	ColumnDataResult AddOrGetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName) override;
	void* GetColumnData(TypedElementRowHandle Row, const UScriptStruct* ColumnType) override;
	ColumnDataResult AddOrGetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName,
		TConstArrayView<TypedElement::ColumnUtils::Argument> Arguments) override;
	bool AddColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> Columns) override;
	void RemoveColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> Columns) override;
	bool AddRemoveColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> ColumnsToAdd,
		TConstArrayView<const UScriptStruct*> ColumnsToRemove) override;
	bool BatchAddRemoveColumns(TConstArrayView<TypedElementRowHandle> Rows,TConstArrayView<const UScriptStruct*> ColumnsToAdd,
		TConstArrayView<const UScriptStruct*> ColumnsToRemove) override;
	ColumnDataResult GetColumnData(TypedElementRowHandle Row, FTopLevelAssetPath ColumnName) override;
	bool HasColumns(TypedElementRowHandle Row, TConstArrayView<const UScriptStruct*> ColumnTypes) const override;
	bool HasColumns(TypedElementRowHandle Row, TConstArrayView<TWeakObjectPtr<const UScriptStruct>> ColumnTypes) const override;

	void RegisterTickGroup(FName GroupName, EQueryTickPhase Phase, FName BeforeGroup, FName AfterGroup, bool bRequiresMainThread);
	void UnregisterTickGroup(FName GroupName, EQueryTickPhase Phase);

	TypedElementQueryHandle RegisterQuery(FQueryDescription&& Query) override;
	void UnregisterQuery(TypedElementQueryHandle Query) override;
	const FQueryDescription& GetQueryDescription(TypedElementQueryHandle Query) const override;
	FName GetQueryTickGroupName(EQueryTickGroups Group) const override;
	FQueryResult RunQuery(TypedElementQueryHandle Query) override;
	FQueryResult RunQuery(TypedElementQueryHandle Query, DirectQueryCallbackRef Callback) override;

	FTypedElementOnDataStorageUpdate& OnUpdate() override;
	bool IsAvailable() const override;
	void* GetExternalSystemAddress(UClass* Target) override;

	void DebugPrintQueryCallbacks(FOutputDevice& Output);
private:
	using QueryStore = TTypedElementHandleStore<FTypedElementDatabaseExtendedQuery>;

	struct FTickGroupId
	{
		FName Name;
		EQueryTickPhase Phase;

		friend inline uint32 GetTypeHash(const FTickGroupId& Id){ return HashCombine(GetTypeHash(Id.Name), GetTypeHash(Id.Phase)); }
		friend inline bool operator==(const FTickGroupId& Lhs, const FTickGroupId& Rhs) { return Lhs.Phase == Rhs.Phase && Lhs.Name == Rhs.Name; }
		friend inline bool operator!=(const FTickGroupId& Lhs, const FTickGroupId& Rhs) { return Lhs.Phase != Rhs.Phase || Lhs.Name != Rhs.Name; }
	};

	struct FTickGroupDescription
	{
		TArray<FName> BeforeGroups;
		TArray<FName> AfterGroups;
		bool bRequiresMainThread{ false };
	};
	
	/** Converts a set of column types into Mass specific fragment and tag bit sets. Returns true if any values were added. */
	static bool ColumnsToBitSets(TConstArrayView<const UScriptStruct*> Columns, FMassFragmentBitSet& Fragments, FMassTagBitSet& Tags);

	void PreparePhase(EQueryTickPhase Phase, float DeltaTime);
	void FinalizePhase(EQueryTickPhase Phase, float DeltaTime);
	void PhasePreOrPostAmble(EQueryTickPhase Phase, float DeltaTime, TArray<TypedElementQueryHandle>& Queries);
	void Reset();
	
	static const FName TickGroupName_SyncWidget;
	
	TArray<FMassArchetypeHandle> Tables;
	TMap<FName, TypedElementTableHandle> TableNameLookup;
	TMap<FTickGroupId, FTickGroupDescription> TickGroupDescriptions;

	TArray<TypedElementQueryHandle> PhasePreparationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(EQueryTickPhase::Max)];
	TArray<TypedElementQueryHandle> PhaseFinalizationQueries[static_cast<std::underlying_type_t<EQueryTickPhase>>(EQueryTickPhase::Max)];

	QueryStore Queries;

	FTypedElementOnDataStorageUpdate OnUpdateDelegate;

	TSharedPtr<FMassEntityManager> ActiveEditorEntityManager;
	TSharedPtr<FMassProcessingPhaseManager> ActiveEditorPhaseManager;
};