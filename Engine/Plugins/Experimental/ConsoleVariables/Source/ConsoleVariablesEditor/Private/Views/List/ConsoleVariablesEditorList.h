// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Widgets/SWidget.h"

struct FConsoleVariablesEditorListRow;
typedef TSharedPtr<FConsoleVariablesEditorListRow> FConsoleVariablesEditorListRowPtr;

class SConsoleVariablesEditorList;
class UConsoleVariablesAsset;

class FConsoleVariablesEditorList : public TSharedFromThis<FConsoleVariablesEditorList>
{
public:

	enum class EConsoleVariablesEditorListMode : uint8
	{
		// We're displaying the cvars listed in the loaded (or default) preset
		Preset = 1,
		// We're displaying the cvars that match the criteria of the global search
		GlobalSearch = 2
	};

	FConsoleVariablesEditorList() {}

	~FConsoleVariablesEditorList();

	TSharedRef<SWidget> GetOrCreateWidget();

	EConsoleVariablesEditorListMode GetListMode() const
	{
		return CurrentListMode;
	}

	void SetListMode(EConsoleVariablesEditorListMode NewListMode)
	{
		CurrentListMode = NewListMode;;
	}

	/** Regenerate the list items and refresh the list. Call when adding or removing variables. */
	void RebuildList(const FString& InConsoleCommandToScrollTo = "") const;

	/**
	 * Refresh filters and sorting.
	 * Useful for when the list state has gone stale but the variable count has not changed.
	 */
	void RefreshList() const;

	/** Updates the saved values in a UConsoleVariablesAsset so that the command/value map can be saved to disk */
	void UpdatePresetValuesForSave(TObjectPtr<UConsoleVariablesAsset> InAsset) const;

private:

	EConsoleVariablesEditorListMode CurrentListMode = EConsoleVariablesEditorListMode::Preset;
	TSharedPtr<SConsoleVariablesEditorList> ListWidget;
};
