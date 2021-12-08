// Copyright Epic Games, Inc. All Rights Reserved.

#include "Views/List/ConsoleVariablesEditorList.h"

#include "Views/List/SConsoleVariablesEditorList.h"

FConsoleVariablesEditorList::~FConsoleVariablesEditorList()
{
	
}

TSharedRef<SWidget> FConsoleVariablesEditorList::GetOrCreateWidget()
{
	if (!ListWidget.IsValid())
	{
		SAssignNew(ListWidget, SConsoleVariablesEditorList);
	}

	return ListWidget.ToSharedRef();
}

void FConsoleVariablesEditorList::RebuildList(const FString& InConsoleCommandToScrollTo) const
{
	if (ListWidget.IsValid())
	{
		ListWidget->RebuildList(InConsoleCommandToScrollTo);
	}
}

void FConsoleVariablesEditorList::RefreshList() const
{
	if (ListWidget.IsValid())
	{
		ListWidget->RefreshList();
	}
}

void FConsoleVariablesEditorList::UpdatePresetValuesForSave(TObjectPtr<UConsoleVariablesAsset> InAsset) const
{
	if (ListWidget.IsValid())
	{
		ListWidget->UpdatePresetValuesForSave(InAsset);
	}
}
