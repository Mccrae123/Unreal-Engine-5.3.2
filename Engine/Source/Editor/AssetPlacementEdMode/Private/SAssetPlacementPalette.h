// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"
#include "Widgets/Views/STreeView.h"
#include "Misc/TextFilter.h"

class FAssetThumbnailPool;
class FAssetPlacementPaletteItemModel;
class FMenuBuilder;
class FUICommandList;
class IDetailsView;
class UFoliageType;
struct FAssetData;
class UAssetPlacementSettings;
class IPropertyHandle;

typedef TSharedPtr<FAssetPlacementPaletteItemModel> FPlacementPaletteItemModelPtr;
typedef STreeView<FPlacementPaletteItemModelPtr> SPlacementTypeTreeView;
typedef STileView<FPlacementPaletteItemModelPtr> SPlacementTypeTileView;

/** View modes supported by the palette */
enum class EAssetPlacementPaletteViewMode : int32
{
	Thumbnail,
	Tree
};

/** The palette of Placement types available for use by the Placement edit mode */
class SAssetPlacementPalette : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAssetPlacementPalette) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UAssetPlacementSettings>, PlacementSettings)
		SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, PalettePropertyHandle)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SAssetPlacementPalette();

	// SWidget interface
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Updates the Placement palette, optionally doing a full rebuild of the items in the palette as well */
	void UpdatePalette(bool bRebuildItems = false);

	/** Refreshes the Placement palette */
	void RefreshPalette();

	bool AnySelectedTileHovered() const;
	void ActivateAllSelectedTypes(bool bActivate) const;

	/** @return True if the given view mode is the active view mode */
	bool IsActiveViewMode(EAssetPlacementPaletteViewMode ViewMode) const;

	/** @return True if tooltips should be shown when hovering over Placement type items in the palette */
	bool ShouldShowTooltips() const;

	/** @return The current search filter text */
	FText GetSearchText() const;

	/** Adds the Placement type asset to the instanced Placement actor's list of types. */
	void AddPlacementType(const FAssetData& AssetData);

private:	// GENERAL

	/** Refreshes the active palette view widget */
	void RefreshActivePaletteViewWidget();

	/** Creates the palette views */
	TSharedRef<class SWidgetSwitcher> CreatePaletteViews();

	/** Adds the displayed name of the Placement type for filtering */
	void GetPaletteItemFilterString(FPlacementPaletteItemModelPtr PaletteItemModel, TArray<FString>& OutArray) const;

	/** Handles changes to the search filter text */
	void OnSearchTextChanged(const FText& InFilterText);

	/** Gets the asset picker for adding a Placement type. */
	TSharedRef<SWidget> GetAddPlacementTypePicker();

	bool ShouldFilterAsset(const FAssetData& InAssetData);

	/** Gets the visibility of the Add Placement Type text in the header row button */
	EVisibility GetAddPlacementTypeButtonTextVisibility() const;

	/** Toggle whether all Placement types are active */
	ECheckBoxState GetState_AllMeshes() const;
	void OnCheckStateChanged_AllMeshes(ECheckBoxState InState);

	/** Sets the view mode of the palette */
	void SetViewMode(EAssetPlacementPaletteViewMode NewViewMode);

	/** Sets whether to show tooltips when hovering over Placement type items in the palette */
	void ToggleShowTooltips();

	/** Switches the palette display between the tile and tree view */
	FReply OnToggleViewModeClicked();

	/** @return The index of the view widget to display */
	int32 GetActiveViewIndex() const;

	/** Handler for selection changes in either view */
	void OnSelectionChanged(FPlacementPaletteItemModelPtr Item, ESelectInfo::Type SelectInfo);

	/** Toggle the activation state of a type on a double-click */
	void OnItemDoubleClicked(FPlacementPaletteItemModelPtr Item) const;

	/** Creates the view options menu */
	TSharedRef<SWidget> GetViewOptionsMenuContent();

	TSharedPtr<SListView<FPlacementPaletteItemModelPtr>> GetActiveViewWidget() const;

	/** Gets the visibility of the "Drop Placement Here" prompt for when the palette is empty */
	EVisibility GetDropPlacementHintVisibility() const;

	/** Gets the visibility of the drag-drop zone overlay */
	EVisibility GetPlacementDropTargetVisibility() const;

	/** Handles dropping of a mesh or Placement type into the palette */
	FReply HandlePlacementDropped(const FGeometry& DropZoneGeometry, const FDragDropEvent& DragDropEvent);

private:	// CONTEXT MENU

	/** @return the SWidget containing the context menu */
	TSharedPtr<SWidget> ConstructPlacementTypeContextMenu();

	/** Handler for the 'Activate' command */
	void OnActivatePlacementTypes();
	bool OnCanActivatePlacementTypes() const;

	/** Handler for the 'Deactivate' command */
	void OnDeactivatePlacementTypes();
	bool OnCanDeactivatePlacementTypes() const;

	/** Fills 'Replace' menu command  */
	void FillReplacePlacementTypeSubmenu(FMenuBuilder& MenuBuilder);

	/** Handler for 'Replace' command  */
	void OnReplacePlacementTypeSelected(const struct FAssetData& AssetData);

	/** Handler for 'Remove' command  */
	void OnRemovePlacementType();

	/** Handler for 'Show in CB' command  */
	void OnShowPlacementTypeInCB();

	/** Handler for 'Select All' command  */
	void OnSelectAllInstances();

	/** Handler for 'Deselect All' command  */
	void OnDeselectAllInstances();

	/** Handler for 'Select Invalid Instances' command  */
	void OnSelectInvalidInstances();

	/** Executes Function on gathered list of Placement types from the selected palette items */
	void ExecuteOnSelectedItemPlacementTypes(TFunctionRef<void(const TArray<FAssetData>&)> ExecuteFunc);

	/** @return Whether selecting instances is currently possible */
	bool CanSelectInstances() const;

	/** Handler for 'Reflect Selection in Palette ' command */
	void OnReflectSelectionInPalette();

	/** Selects Placement Type in palette */
	void SelectPlacementTypesInPalette(const TArray<FAssetData>& PlacementTypes);

private:	// THUMBNAIL VIEW

	/** Creates a thumbnail tile for the given Placement type */
	TSharedRef<ITableRow> GenerateTile(FPlacementPaletteItemModelPtr Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** Gets the scaled thumbnail tile size */
	float GetScaledThumbnailSize() const;

	/** Gets the current scale of the thumbnail tiles */
	float GetThumbnailScale() const;

	/** Sets the current scale of the thumbnail tiles */
	void SetThumbnailScale(float InScale);

	/** Gets whether the thumbnail scaling slider is visible */
	EVisibility GetThumbnailScaleSliderVisibility() const;

private:	// TREE VIEW

	/** Generates a row widget for Placement mesh item */
	TSharedRef<ITableRow> TreeViewGenerateRow(FPlacementPaletteItemModelPtr Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** Text for Placement meshes list header */
	FText GetTypeColumnHeaderText() const;

	/** Mesh list sorting support */
	EColumnSortMode::Type GetMeshColumnSortMode() const;
	void OnTypeColumnSortModeChanged(EColumnSortPriority::Type InPriority, const FName& InColumnName, EColumnSortMode::Type InSortMode);

private:
	/** Handles the click for the uneditable blueprint Placement type warning */
	void OnEditPlacementTypeBlueprintHyperlinkClicked(const FSlateHyperlinkRun::FMetadata& Metadata);

private:
	/** Active timer handler to update the items in the palette */
	EActiveTimerReturnType UpdatePaletteItems(double InCurrentTime, float InDeltaTime);

	/** Active timer handler to refresh the palette */
	EActiveTimerReturnType RefreshPaletteItems(double InCurrentTime, float InDeltaTime);

private:
	typedef TTextFilter<FPlacementPaletteItemModelPtr> PlacementTypeTextFilter;
	TSharedPtr<PlacementTypeTextFilter> TypeFilter;

	/** All the items in the palette (unfiltered) */
	TArray<FPlacementPaletteItemModelPtr> PaletteItems;

	/** The filtered list of types to display in the palette */
	TArray<FPlacementPaletteItemModelPtr> FilteredItems;

	/** Switches between the thumbnail and tree views */
	TSharedPtr<class SWidgetSwitcher> WidgetSwitcher;

	/** The Add Placement Type combo button */
	TSharedPtr<class SComboButton> AddPlacementTypeCombo;

	/** The header row of the Placement mesh tree */
	TSharedPtr<class SHeaderRow> TreeViewHeaderRow;

	/** Placement type thumbnails widget  */
	TSharedPtr<SPlacementTypeTileView> TileViewWidget;

	/** Placement type tree widget  */
	TSharedPtr<SPlacementTypeTreeView> TreeViewWidget;

	/** Placement mesh details widget  */
	TSharedPtr<class IDetailsView> DetailsWidget;

	TWeakObjectPtr<UAssetPlacementSettings> PlacementSettings;
	TSharedPtr<IPropertyHandle> PalettePropertyHandle;

	/** Placement items search box widget */
	TSharedPtr<class SSearchBox> SearchBoxPtr;

	/** Command list for binding functions for the context menu. */
	TSharedPtr<FUICommandList> UICommandList;

	/** Thumbnail pool for rendering mesh thumbnails */
	TSharedPtr<class FAssetThumbnailPool> ThumbnailPool;

	bool bItemsNeedRebuild = true;
	bool bShowFullTooltips = true;
	bool bIsRebuildTimerRegistered = true;
	bool bIsRefreshTimerRegistered = true;
	EAssetPlacementPaletteViewMode ActiveViewMode;
	EColumnSortMode::Type ActiveSortOrder = EColumnSortMode::Type::Ascending;

	float PaletteThumbnailScale = .3f;
};
