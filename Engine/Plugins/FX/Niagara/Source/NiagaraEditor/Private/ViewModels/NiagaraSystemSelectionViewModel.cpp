// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "ViewModels/NiagaraSystemSelectionViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackItemGroup.h"
#include "ViewModels/Stack/NiagaraStackItem.h"
#include "ViewModels/Stack/NiagaraStackSelection.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraEmitterHandle.h"

void UNiagaraSystemSelectionViewModel::Initialize(TSharedRef<FNiagaraSystemViewModel> InSystemViewModel)
{
	SystemViewModelWeak = InSystemViewModel;

	StackSelection = NewObject<UNiagaraStackSelection>(this);
	StackSelection->Initialize(UNiagaraStackEntry::FRequiredEntryData(
		InSystemViewModel,
		TSharedPtr<FNiagaraEmitterViewModel>(),
		UNiagaraStackEntry::FExecutionCategoryNames::System,
		UNiagaraStackEntry::FExecutionSubcategoryNames::Settings,
		InSystemViewModel->GetEditorData().GetStackEditorData()));

	SelectionStackViewModel = NewObject<UNiagaraStackViewModel>(this);
	SelectionStackViewModel->InitializeWithRootEntry(StackSelection);

	bSystemIsSelected = false;
	bRefreshIsPending = false;
}

void UNiagaraSystemSelectionViewModel::Finalize()
{
	SystemViewModelWeak.Reset();

	if (StackSelection != nullptr)
	{
		StackSelection->Finalize();
		StackSelection = nullptr;
	}

	if (SelectionStackViewModel != nullptr)
	{
		SelectionStackViewModel->Finalize();
		SelectionStackViewModel = nullptr;
	}
}

const TArray<UNiagaraStackEntry*> UNiagaraSystemSelectionViewModel::GetSelectedEntries() const
{
	return SelectedEntries;
}

bool UNiagaraSystemSelectionViewModel::GetSystemIsSelected() const
{
	return bSystemIsSelected;
}

const TArray<FGuid>& UNiagaraSystemSelectionViewModel::GetSelectedEmitterHandleIds() const
{
	return SelectedEmitterHandleIds;
}

void UNiagaraSystemSelectionViewModel::UpdateSelectionFromEntries(const TArray<UNiagaraStackEntry*> InSelectedEntries, const TArray<UNiagaraStackEntry*> InDeselectedEntries, bool bClearCurrentSelection)
{
	if (bClearCurrentSelection)
	{
		SelectedEntries.Empty();
		bSystemIsSelected = false;
		SelectedEmitterHandleIds.Empty();
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::Clear);
		UpdateStackSelectionEntry();
	}

	bool bSelectionChanged = false;
	TSet<UNiagaraStackEntry*> CurrentSelection(SelectedEntries);

	for (UNiagaraStackEntry* DeselectedEntry : InDeselectedEntries)
	{
		if (CurrentSelection.Contains(DeselectedEntry))
		{
			SelectedEntries.Remove(DeselectedEntry);
			TSharedPtr<FNiagaraEmitterViewModel> DeselectedEmitterViewModel = DeselectedEntry->GetEmitterViewModel();
			if (DeselectedEmitterViewModel.IsValid())
			{
				bool bEmitterIsStillSelected = SelectedEntries.ContainsByPredicate([DeselectedEmitterViewModel](const UNiagaraStackEntry* SelectedEntry)
				{ 
					return SelectedEntry->GetEmitterViewModel() == DeselectedEmitterViewModel; 
				});
				if (bEmitterIsStillSelected == false)
				{
					const FNiagaraEmitterHandle* DeselectedEmitterHandle = FNiagaraEditorUtilities::GetEmitterHandleForEmitter(DeselectedEntry->GetSystemViewModel()->GetSystem(), *DeselectedEmitterViewModel->GetEmitter());
					if (DeselectedEmitterHandle != nullptr)
					{
						SelectedEmitterHandleIds.Remove(DeselectedEmitterHandle->GetId());
					}
				}
			}
			else
			{
				bool bSystemIsStillSelected = SelectedEntries.ContainsByPredicate([](const UNiagaraStackEntry* SelectedEntry)
				{
					return SelectedEntry->GetEmitterViewModel().IsValid() == false;
				});
				if (bSystemIsStillSelected == false)
				{
					bSystemIsSelected = false;
				}
			}
			bSelectionChanged = true;
		}
	}

	for (UNiagaraStackEntry* SelectedEntry : InSelectedEntries)
	{
		if (CurrentSelection.Contains(SelectedEntry) == false)
		{
			SelectedEntries.Add(SelectedEntry);
			TSharedPtr<FNiagaraEmitterViewModel> SelectedEmitterViewModel = SelectedEntry->GetEmitterViewModel();
			if (SelectedEmitterViewModel.IsValid())
			{
				const FNiagaraEmitterHandle* SelectedEmitterHandle = FNiagaraEditorUtilities::GetEmitterHandleForEmitter(SelectedEntry->GetSystemViewModel()->GetSystem(), *SelectedEmitterViewModel->GetEmitter());
				if (SelectedEmitterHandle != nullptr)
				{
					SelectedEmitterHandleIds.AddUnique(SelectedEmitterHandle->GetId());
				}
			}
			else
			{
				bSystemIsSelected = true;
			}
			bSelectionChanged = true;
		}
	}

	if (bSelectionChanged)
	{
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::EntrySelection);
		UpdateStackSelectionEntry();
	}
}

void UNiagaraSystemSelectionViewModel::UpdateSelectionFromTopLevelObjects(bool bInSystemIsSelected, const TArray<FGuid> InSelectedEmitterIds, bool bClearCurrentSelection)
{
	if (bClearCurrentSelection)
	{
		SelectedEntries.Empty();
		bSystemIsSelected = false;
		SelectedEmitterHandleIds.Empty();
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::Clear);
		UpdateStackSelectionEntry();
	}

	bool bSelectionChanged = false;
	if (bSystemIsSelected != bInSystemIsSelected)
	{
		bSystemIsSelected = bInSystemIsSelected;
		if (bSystemIsSelected)
		{
			SelectedEntries.Append(GetSystemViewModel()->GetSystemStackViewModel()->GetRootEntries());
		}
		else
		{
			TArray<UNiagaraStackEntry*> SystemRootEntries = GetSystemViewModel()->GetSystemStackViewModel()->GetRootEntries();
			SelectedEntries.RemoveAll([&SystemRootEntries](UNiagaraStackEntry* SelectedEntry) { return SystemRootEntries.Contains(SelectedEntry); });
		}
		bSelectionChanged = true;
	}

	TSet<FGuid> CurrentSelectedEmitterHandleIds(SelectedEmitterHandleIds);

	for (const FGuid& CurrentSelectedEmitterHandleId : CurrentSelectedEmitterHandleIds)
	{
		if (InSelectedEmitterIds.Contains(CurrentSelectedEmitterHandleId) == false)
		{
			SelectedEmitterHandleIds.Remove(CurrentSelectedEmitterHandleId);
			TSharedPtr<FNiagaraEmitterHandleViewModel> DeselectedEmitterHandleViewModel = GetSystemViewModel()->GetEmitterHandleViewModelById(CurrentSelectedEmitterHandleId);
			TArray<UNiagaraStackEntry*> DeselectedEmitterRootEntries = DeselectedEmitterHandleViewModel->GetEmitterStackViewModel()->GetRootEntries();
			SelectedEntries.RemoveAll([&DeselectedEmitterRootEntries](UNiagaraStackEntry* SelectedEntry) { return DeselectedEmitterRootEntries.Contains(SelectedEntry); });
			bSelectionChanged = true;
		}
	}

	for (const FGuid& InSelectedEmitterId : InSelectedEmitterIds)
	{
		if (CurrentSelectedEmitterHandleIds.Contains(InSelectedEmitterId) == false)
		{
			SelectedEmitterHandleIds.Add(InSelectedEmitterId);
			TSharedPtr<FNiagaraEmitterHandleViewModel> SelectedEmitterHandleViewModel = GetSystemViewModel()->GetEmitterHandleViewModelById(InSelectedEmitterId);
			if (SelectedEmitterHandleViewModel.IsValid())
			{
				SelectedEntries.Append(SelectedEmitterHandleViewModel->GetEmitterStackViewModel()->GetRootEntries());
			}
			bSelectionChanged = true;
		}
	}

	if (bSelectionChanged)
	{
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::TopObjectLevelSelection);
		UpdateStackSelectionEntry();
	}
}

UNiagaraStackViewModel* UNiagaraSystemSelectionViewModel::GetSelectionStackViewModel()
{
	return SelectionStackViewModel;
}

void UNiagaraSystemSelectionViewModel::RemoveEntryFromSelectionByDisplayedObject(const UObject* InObject)
{
	bool bSelectionChanged = false;

	TArray<UNiagaraStackEntry*> EntriesToDeselect;
	for (UNiagaraStackEntry* SelectedEntry : SelectedEntries)
	{
		if (SelectedEntry->GetDisplayedObject() == InObject)
		{
			EntriesToDeselect.Add(SelectedEntry);
			bSelectionChanged = true;
		}
	}

	for (UNiagaraStackEntry* EntryToDeselect : EntriesToDeselect)
	{
		RemoveEntryFromSelectionInternal(EntryToDeselect);
	}

	if (bSelectionChanged)
	{
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::EntrySelection);
		UpdateStackSelectionEntry();
	}
}

void UNiagaraSystemSelectionViewModel::AddEntriesToSelectionByDisplayedObjectsDeferred(const TArray<const UObject*>& InObjects)
{
	for (const UObject* InObject : InObjects)
	{
		DeferredDisplayedObjectKeysToAddToSelection.Add(FObjectKey(InObject));
	}
}

void UNiagaraSystemSelectionViewModel::AddEntryToSelectionByDisplayedObjectDeferred(const UObject* InObject)
{
	TArray<const UObject*> InObjects;
	InObjects.Add(InObject);
	AddEntriesToSelectionByDisplayedObjectsDeferred(InObjects);
}

void UNiagaraSystemSelectionViewModel::Refresh()
{
	TSet<FGuid> ValidEmitterHandleIds;
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel : GetSystemViewModel()->GetEmitterHandleViewModels())
	{
		ValidEmitterHandleIds.Add(EmitterHandleViewModel->GetId());
	}

	int32 NumEmitterHandleIdsRemoved = SelectedEmitterHandleIds.RemoveAll(
		[&ValidEmitterHandleIds](const FGuid& SelectedEmitterHandleId) { return ValidEmitterHandleIds.Contains(SelectedEmitterHandleId) == false; });

	int32 NumEntriesRemoved = SelectedEntries.RemoveAll(
		[](const UNiagaraStackEntry* SelectedEntry) { return SelectedEntry->IsFinalized(); });

	if (NumEmitterHandleIdsRemoved > 0 || NumEntriesRemoved > 0)
	{
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::Refresh);
		UpdateStackSelectionEntry();
	}
}

void UNiagaraSystemSelectionViewModel::RefreshDeferred()
{
	bRefreshIsPending = true;
}

UNiagaraSystemSelectionViewModel::FOnSelectionChanged& UNiagaraSystemSelectionViewModel::OnSelectionChanged()
{
	return OnSelectionChangedDelegate;
}

void FindStackGroupsAndItemsForDisplayedObjectKeysRecursive(UNiagaraStackEntry* StackEntry, const TArray<FObjectKey>& ObjectKeys, TArray<UNiagaraStackEntry*>& OutFoundStackEntries)
{
	if (StackEntry->IsA<UNiagaraStackItemGroup>() || StackEntry->IsA<UNiagaraStackItem>())
	{
		UObject* DisplayedObject = StackEntry->GetDisplayedObject();
		if (DisplayedObject != nullptr && ObjectKeys.Contains(FObjectKey(DisplayedObject)))
		{
			OutFoundStackEntries.Add(StackEntry);
		}
	}

	if (StackEntry->IsA<UNiagaraStackItem>() == false)
	{
		TArray<UNiagaraStackEntry*> Children;
		StackEntry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* ChildEntry : Children)
		{
			FindStackGroupsAndItemsForDisplayedObjectKeysRecursive(ChildEntry, ObjectKeys, OutFoundStackEntries);
		}
	}
}

void FindStackGroupsAndItemsForDisplayedObjectKeys(UNiagaraStackViewModel* StackViewModel, const TArray<FObjectKey>& ObjectKeys, TArray<UNiagaraStackEntry*>& OutFoundStackEntries)
{
	for (UNiagaraStackEntry* RootEntry : StackViewModel->GetRootEntries())
	{
		FindStackGroupsAndItemsForDisplayedObjectKeysRecursive(RootEntry, ObjectKeys, OutFoundStackEntries);
	}
}

void UNiagaraSystemSelectionViewModel::Tick()
{
	if (bRefreshIsPending)
	{
		Refresh();
		bRefreshIsPending = false;
	}

	TArray<UNiagaraStackEntry*> FoundStackEntries;
	TSharedRef<FNiagaraSystemViewModel> SystemViewModel = GetSystemViewModel();
	FindStackGroupsAndItemsForDisplayedObjectKeys(SystemViewModel->GetSystemStackViewModel(), DeferredDisplayedObjectKeysToAddToSelection, FoundStackEntries);
	for (TSharedRef<FNiagaraEmitterHandleViewModel> EmitterViewModel : SystemViewModel->GetEmitterHandleViewModels())
	{
		FindStackGroupsAndItemsForDisplayedObjectKeys(EmitterViewModel->GetEmitterStackViewModel(), DeferredDisplayedObjectKeysToAddToSelection, FoundStackEntries);
	}

	int32 AddedEntries = 0;
	for (UNiagaraStackEntry* FoundStackEntry : FoundStackEntries)
	{
		if (SelectedEntries.Contains(FoundStackEntry) == false)
		{
			SelectedEntries.Add(FoundStackEntry);
			AddedEntries++;
		}
	}

	DeferredDisplayedObjectKeysToAddToSelection.Empty();
	if (AddedEntries > 0)
	{
		OnSelectionChangedDelegate.Broadcast(ESelectionChangeSource::EntrySelection);
		UpdateStackSelectionEntry();
	}
}

TSharedRef<FNiagaraSystemViewModel> UNiagaraSystemSelectionViewModel::GetSystemViewModel()
{
	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = SystemViewModelWeak.Pin();
	checkf(SystemViewModel.IsValid(), TEXT("Owning system view model destroyed before system overview view model."));
	return SystemViewModel.ToSharedRef();
}

void UNiagaraSystemSelectionViewModel::RemoveEntryFromSelectionInternal(UNiagaraStackEntry* DeselectedEntry)
{
	SelectedEntries.Remove(DeselectedEntry);
	TSharedPtr<FNiagaraEmitterViewModel> DeselectedEmitterViewModel = DeselectedEntry->GetEmitterViewModel();
	if (DeselectedEmitterViewModel.IsValid())
	{
		bool bEmitterIsStillSelected = SelectedEntries.ContainsByPredicate([DeselectedEmitterViewModel](const UNiagaraStackEntry* SelectedEntry)
		{
			return SelectedEntry->GetEmitterViewModel() == DeselectedEmitterViewModel;
		});
		if (bEmitterIsStillSelected == false)
		{
			const FNiagaraEmitterHandle* DeselectedEmitterHandle = FNiagaraEditorUtilities::GetEmitterHandleForEmitter(DeselectedEntry->GetSystemViewModel()->GetSystem(), *DeselectedEmitterViewModel->GetEmitter());
			if (DeselectedEmitterHandle != nullptr)
			{
				SelectedEmitterHandleIds.Remove(DeselectedEmitterHandle->GetId());
			}
		}
	}
	else
	{
		bool bSystemIsStillSelected = SelectedEntries.ContainsByPredicate([](const UNiagaraStackEntry* SelectedEntry)
		{
			return SelectedEntry->GetEmitterViewModel().IsValid() == false;
		});
		if (bSystemIsStillSelected == false)
		{
			bSystemIsSelected = false;
		}
	}
}

void UNiagaraSystemSelectionViewModel::UpdateStackSelectionEntry()
{
	// Check the new selection to make sure that there are no entries which are owned by other entries and if there are
	// remove them.
	TArray<UNiagaraStackEntry*> StackSelectionEntries;
	for (UNiagaraStackEntry* SelectedEntry : SelectedEntries)
	{
		bool bIsChildEntry = false;
		UNiagaraStackEntry* EntryInOuterChain = SelectedEntry->GetTypedOuter<UNiagaraStackEntry>();
		while (EntryInOuterChain != nullptr)
		{
			if (SelectedEntries.Contains(EntryInOuterChain))
			{
				bIsChildEntry = true;
				EntryInOuterChain = nullptr;
			}
			else
			{
				EntryInOuterChain = EntryInOuterChain->GetTypedOuter<UNiagaraStackEntry>();
			}
		}

		if (bIsChildEntry == false)
		{
			StackSelectionEntries.Add(SelectedEntry);
		}
	}

	StackSelection->SetSelectedEntries(StackSelectionEntries);
}