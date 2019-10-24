// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/NiagaraSystemSelectionViewModel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "UObject/ObjectKey.h"

class UNiagaraStackEntry;
class UNiagaraStackItem;
class UNiagaraStackViewModel;
class UNiagaraSystemSelectionViewModel;

class SNiagaraOverviewStack : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNiagaraOverviewStack)
	{}
	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs, UNiagaraStackViewModel& InStackViewModel, UNiagaraSystemSelectionViewModel& InOverviewSelectionViewModel);

	~SNiagaraOverviewStack();

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	void AddEntriesRecursive(UNiagaraStackEntry& EntryToAdd, TArray<UNiagaraStackEntry*>& EntryList, const TArray<UClass*>& AcceptableClasses, TArray<UNiagaraStackEntry*> ParentChain);

	void RefreshEntryList();

	void EntryStructureChanged();

	TSharedRef<ITableRow> OnGenerateRowForEntry(UNiagaraStackEntry* Item, const TSharedRef<STableViewBase>& OwnerTable);

	EVisibility GetEnabledCheckBoxVisibility(UNiagaraStackItem* Item) const;

	void OnSelectionChanged(UNiagaraStackEntry* InNewSelection, ESelectInfo::Type SelectInfo);

	void SystemSelectionChanged(UNiagaraSystemSelectionViewModel::ESelectionChangeSource SelectionChangeSource);

	FReply OnRowDragDetected(const FGeometry& InGeometry, const FPointerEvent& InPointerEvent, TWeakObjectPtr<UNiagaraStackEntry> InStackEntryWeak);

	TOptional<EItemDropZone> OnRowCanAcceptDrop(const FDragDropEvent& InDragDropEvent, EItemDropZone InDropZone, UNiagaraStackEntry* InTargetEntry);

	FReply OnRowAcceptDrop(const FDragDropEvent& InDragDropEvent, EItemDropZone InDropZone, UNiagaraStackEntry* InTargetEntry);

private:
	UNiagaraStackViewModel* StackViewModel;
	UNiagaraSystemSelectionViewModel* OverviewSelectionViewModel;

	TArray<UNiagaraStackEntry*> FlattenedEntryList;
	TMap<FObjectKey, TArray<UNiagaraStackEntry*>> EntryObjectKeyToParentChain;
	TSharedPtr<SListView<UNiagaraStackEntry*>> EntryListView;

	TArray<TWeakObjectPtr<UNiagaraStackEntry>> PreviousSelection;

	bool bRefreshEntryListPending;
	bool bUpdatingOverviewSelectionFromStackSelection;
	bool bUpdatingStackSelectionFromOverviewSelection;
};