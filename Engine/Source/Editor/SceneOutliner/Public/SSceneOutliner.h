// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Views/ITypedTableView.h"
#include "Input/Reply.h"
#include "Layout/Visibility.h"
#include "Misc/Attribute.h"
#include "Misc/TextFilter.h"
#include "SlateFwd.h"
#include "Styling/SlateColor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWidget.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STableViewBase.h"
#include "Delegates/DelegateCombinations.h"
#include "Framework/Views/ITypedTableView.h"
#include "Types/SlateEnums.h"

#include "ISceneOutliner.h"
#include "SceneOutlinerFwd.h"

#include "SOutlinerTreeView.h"
#include "SceneOutlinerPublicTypes.h"
#include "SceneOutlinerStandaloneTypes.h"

#include "ISceneOutlinerHierarchy.h"
#include "SceneOutlinerDragDrop.h"

class FMenuBuilder;
class UToolMenu;
class ISceneOutlinerColumn;
class SComboButton;

template<typename ItemType> class STreeView;

/**
 * Scene Outliner definition
 * Note the Scene Outliner is also called the World Outliner
 */
namespace SceneOutliner
{
	DECLARE_EVENT_OneParam(SSceneOutliner, FTreeItemPtrEvent, FTreeItemPtr);

	DECLARE_EVENT_TwoParams(SSceneOutliner, FOnItemSelectionChanged, FTreeItemPtr, ESelectInfo::Type);

	typedef TTextFilter< const ITreeItem& > TreeItemTextFilter;

	/** Structure that defines an operation that should be applied to the tree */
	struct FPendingTreeOperation
	{
		enum EType { Added, Removed, Moved };
		FPendingTreeOperation(EType InType, TSharedRef<ITreeItem> InItem) : Type(InType), Item(InItem) { }

		/** The type of operation that is to be applied */
		EType Type;

		/** The tree item to which this operation relates */
		FTreeItemRef Item;
	};

	/** Set of actions to apply to new tree items */
	namespace ENewItemAction
	{
		enum Type
		{
			/** Do nothing when it is created */
			None			= 0,
			/** Select the item when it is created */
			Select			= 1 << 0,
			/** Scroll the item into view when it is created */
			ScrollIntoView	= 1 << 1,
			/** Interactively rename the item when it is created (implies the above) */
			Rename			= 1 << 2,
		};
	}

	/**
	 * Stores a set of selected items with parsing functions for the scene outliner
	 */
	struct FItemSelection
	{
		/** Set of selected items */
		mutable TArray<TWeakPtr<ITreeItem>> SelectedItems;

		FItemSelection() {}

		FItemSelection(const TArray<FTreeItemPtr>& InSelectedItems)
			: SelectedItems(InSelectedItems) {}

		FItemSelection(SOutlinerTreeView& Tree)
			: FItemSelection(Tree.GetSelectedItems()) {}

		/** Returns true if the selection has an item of a specified type */
		template <typename TreeType>
		bool Has() const
		{
			for (const TWeakPtr<ITreeItem>& Item : SelectedItems)
			{
				if (const auto ItemPtr = Item.Pin())
				{
					if (ItemPtr->IsA<TreeType>())
					{
						return true;
					}
				}
			}
			return false;
		}

		/** Returns the total number of items in the selection */
		uint32 Num() const
		{
			return SelectedItems.Num();
		}

		/** Returns the number of items of a specific type in the selection */
		template <typename TreeType>
		uint32 Num() const
		{
			uint32 Result = 0;
			for (const TWeakPtr<ITreeItem>& Item : SelectedItems)
			{
				if (const auto ItemPtr = Item.Pin())
				{
					if (ItemPtr->IsA<TreeType>())
					{
						++Result;
					}
				}
			}
			return Result;
		}

		/** Add a new item to the selection */
		void Add(FTreeItemPtr NewItem)
		{
			SelectedItems.Add(NewItem);
		}

		/** Get all items of a specified type */
		template <typename TreeType>
		void Get(TArray<TreeType*>& OutArray) const
		{
			for (const TWeakPtr<ITreeItem>& Item : SelectedItems)
			{
				if (const auto ItemPtr = Item.Pin())
				{
					if (TreeType* CastedItem = ItemPtr->CastTo<TreeType>())
					{
						OutArray.Add(CastedItem);
					}
				}
			}
		}

		/** Apply a function to each item of a specified type */
		template <typename TreeType>
		void ForEachItem(TFunctionRef<void(TreeType&)> Func) const
		{
			for (const TWeakPtr<ITreeItem>& Item : SelectedItems)
			{
				if (const auto ItemPtr = Item.Pin())
				{
					if (TreeType* CastedItem = ItemPtr->CastTo<TreeType>())
					{
						Func(*CastedItem);
					}
				}
			}
		}

		/** Use a selector to retrieve a specific data type from items in the selection. Will only add an item's data if the selector returns true for that item. */
		template <typename DataType>
		TArray<DataType> GetData(TFunctionRef<bool(const TWeakPtr<ITreeItem>&, DataType&)> Selector) const
		{
			TArray<DataType> Result;
			for (TWeakPtr<ITreeItem>& Item : SelectedItems)
			{
				DataType Data;
				if (Selector(Item, Data))
				{
					Result.Add(Data);
				}
			}
			return Result;
		}
	};

	/**
	 * Scene Outliner widget
	 */
	class SSceneOutliner : public ISceneOutliner, public FEditorUndoClient, public FGCObject
	{

	public:

		SLATE_BEGIN_ARGS(SSceneOutliner)
		{}
		SLATE_END_ARGS()

			/**
			 * Construct this widget.  Called by the SNew() Slate macro.
			 *
			 * @param	InArgs		Declaration used by the SNew() macro to construct this widget
			 * @param	InitOptions	Programmer-driven initialization options for this widget
			 */
			void Construct(const FArguments& InArgs, const FInitializationOptions& InitOptions);

		/** Default constructor - initializes data that is shared between all tree items */
		SSceneOutliner() : SharedData(MakeShareable(new FSharedOutlinerData)) {}

		/** SSceneOutliner destructor */
		~SSceneOutliner();

		/** SWidget interface */
		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
		virtual bool SupportsKeyboardFocus() const override;
		virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

		/** Sends a requests to the Scene Outliner to refresh itself the next chance it gets */
		virtual void Refresh() override;

		void RefreshSelection();

		//~ Begin FEditorUndoClient Interface
		virtual void PostUndo(bool bSuccess) override;
		virtual void PostRedo(bool bSuccess) override { PostUndo(bSuccess); }
		// End of FEditorUndoClient

		/** @return Returns the common data for this outliner */
		virtual const FSharedOutlinerData& GetSharedData() const override
		{
			return *SharedData;
		}

		/** Get a const reference to the actual tree hierarchy */
		virtual const STreeView<FTreeItemPtr>& GetTree() const override
		{
			return *OutlinerTreeView;
		}

		virtual const TSharedPtr< SOutlinerTreeView>& GetTreeView() const
		{
			return OutlinerTreeView;
		}

		/** @return Returns a string to use for highlighting results in the outliner list */
		virtual TAttribute<FText> GetFilterHighlightText() const override;

		/** Set the keyboard focus to the outliner */
		virtual void SetKeyboardFocus() override;

		/** Gets the cached icon for this class name */
		virtual const FSlateBrush* GetCachedIconForClass(FName InClassName) const override;

		/** Sets the cached icon for this class name */
		virtual void CacheIconForClass(FName InClassName, const FSlateBrush* InSlateBrush) override;

		/** Should the scene outliner accept a request to rename a object */
		virtual bool CanExecuteRenameRequest(const ITreeItem& ItemPtr) const override;

		/**
		 * Add a filter to the scene outliner
		 * @param Filter The filter to apply to the scene outliner
		 * @return The index of the filter.
		 */
		virtual int32 AddFilter(const TSharedRef<SceneOutliner::FOutlinerFilter>& Filter) override;

		/**
		 * Remove a filter from the scene outliner
		 * @param Filter The Filter to remove
		 * @return True if the filter was removed.
		 */
		virtual bool RemoveFilter(const TSharedRef<SceneOutliner::FOutlinerFilter>& Filter) override;

		/**
		 * Retrieve the filter at the specified index
		 * @param Index The index of the filter to retrive
		 * @return A valid poiter to a filter if the index was valid
		 */
		virtual TSharedPtr<SceneOutliner::FOutlinerFilter> GetFilterAtIndex(int32 Index) override;

		/** Get number of filters applied to the scene outliner */
		virtual int32 GetFilterCount() const override;

		/**
		 * Add or replace a column of the scene outliner
		 * Note: The column id must match the id of the column returned by the factory
		 * @param ColumnId The id of the column to add
		 * @param ColumInfo The struct that contains the information on how to present and retrieve the column
		 */
		virtual void AddColumn(FName ColumId, const SceneOutliner::FColumnInfo& ColumInfo) override;

		/**
		 * Remove a column of the scene outliner
		 * @param ColumnId The name of the column to remove
		 */
		virtual void RemoveColumn(FName ColumId) override;

		/** Return the name/Id of the columns of the scene outliner */
		virtual TArray<FName> GetColumnIds() const override;

		/** @return Returns the current sort mode of the specified column */
		virtual EColumnSortMode::Type GetColumnSortMode(const FName ColumnId) const;

		/** Request that the tree be sorted at a convenient time */
		virtual void RequestSort();

		/** Returns true if edit delete can be executed */
		virtual bool Delete_CanExecute();

		/** Returns true if edit rename can be executed */
		virtual bool Rename_CanExecute();

		/** Executes rename. */
		virtual void Rename_Execute();

		/** Returns true if edit cut can be executed */
		virtual bool Cut_CanExecute();

		/** Returns true if edit copy can be executed */
		virtual bool Copy_CanExecute();

		/** Returns true if edit paste can be executed */
		virtual bool Paste_CanExecute();

		/** Can the scene outliner rows generated on drag event */
		virtual bool CanSupportDragAndDrop() const override;

		/** Tells the scene outliner that it should do a full refresh, which will clear the entire tree and rebuild it from scratch. */
		virtual void FullRefresh() override;

	public:
		/** Event to react to a user double click on a item */
		FTreeItemPtrEvent& GetDoubleClickEvent() { return OnDoubleClickOnTreeEvent; }

		/**
		 * Allow the system that use the scene outliner to react when it's selection is changed
		 * Note: This event will only be broadcast on a user input.
		 */
		FOnItemSelectionChanged& GetOnItemSelectionChanged() { return OnItemSelectionChanged; }

		/** Set the item selection of the outliner based on a selector function. Any items which return true will be added */
		virtual void SetSelection(const TFunctionRef<bool(SceneOutliner::ITreeItem&)> Selector) override;

		/** Set the selection status of a set of items in the scene outliner */
		void SetItemSelection(const TArray<FTreeItemPtr>& InItems, bool bSelected, ESelectInfo::Type SelectInfo = ESelectInfo::Direct);

		/** Set the selection status of a single item in the scene outliner */
		void SetItemSelection(const FTreeItemPtr& InItem, bool bSelected, ESelectInfo::Type SelectInfo = ESelectInfo::Direct);

		/** Adds a set of items to the current selection */
		void AddToSelection(const TArray<FTreeItemPtr>& InItems, ESelectInfo::Type SelectInfo = ESelectInfo::Direct);

		/** Remove a set of items from the current selection */
		void RemoveFromSelection(const TArray<FTreeItemPtr>& InItems);

		/** Remove an item from the current selection */
		void RemoveFromSelection(const FTreeItemPtr& InItem);

		/**
		 * Returns the list of currently selected tree items
		 */
		virtual TArray<FTreeItemPtr> GetSelectedItems() const { return OutlinerTreeView->GetSelectedItems(); }

		/**
		 * Returns the currently selected items.
		 */
		virtual FItemSelection GetSelection() const { return FItemSelection(*OutlinerTreeView); }

		/**
		 * Add a folder to the selection of the scene outliner
		 * @param FolderName The name of the folder to add to selection
		 */
		void AddFolderToSelection(const FName& FolderName);

		/**
		 * Remove a folder from the selection of the scene outliner
		 * @param FolderName The name of the folder to remove from the selection
		 */
		void RemoveFolderFromSelection(const FName& FolderName);

		/** Deselect all selected items */
		void ClearSelection();

		/** Sets the next item to rename */
		void SetPendingRenameItem(const FTreeItemPtr& InItem) { PendingRenameItem = InItem; Refresh(); }

		/** Retrieve an ITreeItem by its ID if it exists in the tree */
		FTreeItemPtr GetTreeItem(FTreeItemID, bool bIncludePending = false);

		/** Get the outliner filter collection */
		TSharedPtr<FOutlinerFilters>& GetFilters() { return Filters; }

		/** Create a drag drop operation */
		TSharedPtr<FDragDropOperation> CreateDragDropOperation(const TArray<FTreeItemPtr>& InTreeItems) const;

		/** Parse a drag drop operation into a payload */
		bool ParseDragDrop(FDragDropPayload& OutPayload, const FDragDropOperation& Operation) const;

		/** Validate a drag drop operation on a drop target */
		FDragValidationInfo ValidateDrop(const ITreeItem& DropTarget, const FDragDropPayload& Payload) const;

		/** Called when a payload is dropped onto a target */
		void OnDropPayload(ITreeItem& DropTarget, const FDragDropPayload& Payload, const FDragValidationInfo& ValidationInfo) const;

		/** Called when a payload is dragged over an item */
		FReply OnDragOverItem(const FDragDropEvent& Event, const ITreeItem& Item) const;
	private:
		/** Methods that implement structural modification logic for the tree */

		/** Empty all the tree item containers maintained by this outliner */
		void EmptyTreeItems();

		/** Apply incremental changes to, or a complete repopulation of the tree  */
		void Populate();

		/** Repopulates the entire tree */
		void RepopulateEntireTree();

		/** Adds a single new item to the pending map and creates an add operation for it */
		void AddPendingItem(FTreeItemPtr Item);

		/** Adds a new item and all of its children to the pending items. */
		void AddPendingItemAndChildren(FTreeItemPtr Item);

		/** Attempts to add a pending item to the current tree. Will add any parents if required. */
		bool AddItemToTree(FTreeItemRef InItem);

		/** Add an item to the tree, even if it doesn't match the filter terms. Used to add parent's that would otherwise be filtered out */
		void AddUnfilteredItemToTree(FTreeItemRef Item);

		/** Ensure that the specified item's parent is added to the tree, if applicable */
		FTreeItemPtr EnsureParentForItem(FTreeItemRef Item);

		/** Remove the specified item from the tree */
		void RemoveItemFromTree(FTreeItemRef InItem);

		/** Called when a child has been removed from the specified parent. Will potentially remove the parent from the tree */
		void OnChildRemovedFromParent(ITreeItem& Parent);

		/** Called when a child has been moved in the tree hierarchy */
		void OnItemMoved(const FTreeItemRef& Item);

	public:
		// Test the filters using stack-allocated data to prevent unnecessary heap allocations
		template <typename TreeItemType, typename TreeItemData>
		FTreeItemPtr CreateItemFor(const TreeItemData& Data, TFunctionRef<void(const TreeItemType&)> OnItemPassesFilters, bool bForce = false)
		{
			const TreeItemType Temporary(Data);
			bool bPassesFilters = Filters->PassesAllFilters(Temporary);
			if (bPassesFilters)
			{
				OnItemPassesFilters(Temporary);
			}

			bPassesFilters &= SearchBoxFilter->PassesFilter(Temporary);

			if (bForce || bPassesFilters)
			{
				FTreeItemPtr Result = MakeShareable(new TreeItemType(Data));
				Result->Flags.bIsFilteredOut = !bPassesFilters;
				Result->Flags.bInteractive = Filters->GetInteractiveState(*Result);
				return Result;
			}

			return nullptr;
		}

		/** Instruct the outliner to perform an action on the specified item when it is created */
		void OnItemAdded(const FTreeItemID& ItemID, uint8 ActionMask);

		/** Get the columns to be displayed in this outliner */
		const TMap<FName, TSharedPtr<ISceneOutlinerColumn>>& GetColumns() const
		{
			return Columns;
		}

		bool PassesFilters(const ITreeItem& Item) const
		{
			return Filters->PassesAllFilters(Item);
		}

		/** @return	Returns true if the text filter is currently active */
		bool IsTextFilterActive() const;

		bool PassesTextFilter(const FTreeItemPtr& Item) const
		{
			return SearchBoxFilter->PassesFilter(*Item);
		}

		bool HasSelectorFocus(FTreeItemPtr Item) const
		{
			return OutlinerTreeView->Private_HasSelectorFocus(Item);
		}

		/** Handler for when a property changes on any item. Called by the mode */
		void OnItemLabelChanged(FTreeItemPtr ChangedItem);
	private:

		/** Map of columns that are shown on this outliner. */
		TMap<FName, TSharedPtr<ISceneOutlinerColumn>> Columns;

		/** Set up the columns required for this outliner */
		void SetupColumns(SHeaderRow& HeaderRow);

		/** Refresh the scene outliner for when a colum was added or removed */
		void RefreshColums();

		/** Populates OutSearchStrings with the strings associated with TreeItem that should be used in searching */
		void PopulateSearchStrings( const ITreeItem& TreeItem, OUT TArray< FString >& OutSearchStrings ) const;


	public:
		/** Miscellaneous helper functions */

		/** Scroll the specified item into view */
		void ScrollItemIntoView(const FTreeItemPtr& Item);

		void SetItemExpansion(const FTreeItemPtr& Item, bool bIsExpanded);
		
		bool IsItemExpanded(const FTreeItemPtr& Item) const;

	private:

		/** Check whether we should be showing folders or not in this scene outliner */
		bool ShouldShowFolders() const;

		/** Get an array of selected folders */
		void GetSelectedFolders(TArray<FFolderTreeItem*>& OutFolders) const;

		/** Get an array of selected folder names */
		TArray<FName> GetSelectedFolderNames() const;
	private:
		/** Tree view event bindings */

		/** Called by STreeView to generate a table row for the specified item */
		TSharedRef< ITableRow > OnGenerateRowForOutlinerTree( FTreeItemPtr Item, const TSharedRef< STableViewBase >& OwnerTable );

		/** Called by STreeView to get child items for the specified parent item */
		void OnGetChildrenForOutlinerTree( FTreeItemPtr InParent, TArray< FTreeItemPtr >& OutChildren );

		/** Called by STreeView when the tree's selection has changed */
		void OnOutlinerTreeSelectionChanged( FTreeItemPtr TreeItem, ESelectInfo::Type SelectInfo );

		/** Called by STreeView when the user double-clicks on an item in the tree */
		void OnOutlinerTreeDoubleClick( FTreeItemPtr TreeItem );

		/** Called by STreeView when an item is scrolled into view */
		void OnOutlinerTreeItemScrolledIntoView( FTreeItemPtr TreeItem, const TSharedPtr<ITableRow>& Widget );

		/** Called when an item in the tree has been collapsed or expanded */
		void OnItemExpansionChanged(FTreeItemPtr TreeItem, bool bIsExpanded) const;

	private:
		/** Level, editor and other global event hooks required to keep the outliner up to date */

		void OnHierarchyChangedEvent(FHierarchyChangedData Event);

		/** Handler for when an asset is reloaded */
		void OnAssetReloaded(const EPackageReloadPhase InPackageReloadPhase, FPackageReloadedEvent* InPackageReloadedEvent);

	public:
		/** Copy specified folders to clipboard, keeping current clipboard contents if they differ from previous clipboard contents (meaning items were copied) */
		void CopyFoldersToClipboard(const TArray<FName>& InFolders, const FString& InPrevClipboardContents);

		/** Called by copy and duplicate */
		void CopyFoldersBegin();

		/** Called by copy and duplicate */
		void CopyFoldersEnd();

		/** Called by paste and duplicate */
		void PasteFoldersBegin(TArray<FName> InFolders);

		/** Paste folders end logic */
		void PasteFoldersEnd();

		/** Called by cut and delete */
		void DeleteFoldersBegin();

		/** Called by cut and delete */
		void DeleteFoldersEnd();

		/** Get an array of folders to paste */
		TArray<FName> GetClipboardPasteFolders() const;

		/** Construct folders export string to be used in clipboard */
		FString ExportFolderList(TArray<FName> InFolders) const;

		/** Construct array of folders to be created based on input clipboard string */
		TArray<FName> ImportFolderList(const FString& InStrBuffer) const;

	public:
		/** Duplicates current folder and all descendants */
		void DuplicateFoldersHierarchy();

	private:
		/** Cache selected folders during edit delete */
		TArray<FFolderTreeItem*> CacheFoldersDelete;

		/** Cache folders for cut/copy/paste/duplicate */
		TArray<FName> CacheFoldersEdit;

		/** Cache clipboard contents for cut/copy */
		FString CacheClipboardContents;

		/** Maps pre-existing children during paste or duplicate */
		TMap<FName, TArray<FTreeItemID>> CachePasteFolderExistingChildrenMap;

	private:
		/** Miscellaneous bindings required by the UI */

		/** Called by the editable text control when the filter text is changed by the user */
		void OnFilterTextChanged( const FText& InFilterText );

		/** Called by the editable text control when a user presses enter or commits their text change */
		void OnFilterTextCommitted( const FText& InFilterText, ETextCommit::Type CommitInfo );

		/** Called by the filter button to get the image to display in the button */
		const FSlateBrush* GetFilterButtonGlyph() const;

		/** @return	The filter button tool-tip text */
		FString GetFilterButtonToolTip() const;

		/** @return	Returns whether the filter status line should be drawn */
		EVisibility GetFilterStatusVisibility() const;

		/** @return	Returns the filter status text */
		FText GetFilterStatusText() const;

		/** @return Returns color for the filter status text message, based on success of search filter */
		FSlateColor GetFilterStatusTextColor() const;

		/**	Returns the current visibility of the Empty label */
		EVisibility GetEmptyLabelVisibility() const;

		/** @return the selection mode; disabled entirely if in PIE/SIE mode */
		ESelectionMode::Type GetSelectionMode() const;

		/** @return the content for the view button */
		TSharedRef<SWidget> GetViewButtonContent(bool bShowFilters);

		/** @return the foreground color for the view button */
		FSlateColor GetViewButtonForegroundColor() const;

	public:

		/** Open a context menu for this scene outliner */
		TSharedPtr<SWidget> OnOpenContextMenu();

		void FillFoldersSubMenu(UToolMenu* Menu) const;
		void AddMoveToFolderOutliner(UToolMenu* Menu) const;
		void FillSelectionSubMenu(UToolMenu* Menun) const;
		TSharedRef<TSet<FName>> GatherInvalidMoveToDestinations() const;

		/** Called to select descendants of the currently selected folders */
		void SelectFoldersDescendants(bool bSelectImmediateChildrenOnly = false);

		/** Moves the current selection to the specified folder path */
		void MoveSelectionTo(FName NewParent);

		/** Create a new folder under the specified parent name (NAME_None for root) */
		void CreateFolder();

	private:
		/** Called when the user has clicked the button to add a new folder */
		FReply OnCreateFolderClicked();

	private:

		/** Context menu opening delegate provided by the client */
		FOnContextMenuOpening OnContextMenuOpening;

		TSharedPtr<FSharedOutlinerData> SharedData;

		/** List of pending operations to be applied to the tree */
		TArray<FPendingTreeOperation> PendingOperations;

		/** Map of actions to apply to new tree items */
		TMap<FTreeItemID, uint8> NewItemActions;

		/** Our tree view */
		TSharedPtr< SOutlinerTreeView > OutlinerTreeView;

		/** A map of all items we have in the tree */
		FTreeItemMap TreeItemMap;

		/** Pending tree items that are yet to be added the tree */
		FTreeItemMap PendingTreeItemMap;

		/** Folders pending selection */
		TArray<FName> PendingFoldersSelect;

		/** Root level tree items */
		TArray<FTreeItemPtr> RootTreeItems;

		/** The button that displays view options */
		TSharedPtr<SComboButton> ViewOptionsComboButton;

	private:

		/** Structure containing information relating to the expansion state of parent items in the tree */
		typedef TMap<FTreeItemID, bool> FParentsExpansionState;

		/** Gets the current expansion state of parent items */
		FParentsExpansionState GetParentsExpansionState() const;

		/** Updates the expansion state of parent items after a repopulate, according to the previous state */
		void SetParentsExpansionState(const FParentsExpansionState& ExpansionStateInfo) const;
	private:

		/** True if the outliner needs to be repopulated at the next appropriate opportunity, usually because our
		    item set has changed in some way. */
		uint8 bNeedsRefresh : 1;

		/** true if the Scene Outliner should do a full refresh. */
		uint8 bFullRefresh : 1;

		/** true if the Scene Outliner should refresh selection */
		uint8 bSelectionDirty : 1;

		/** True if the Scene Outliner is currently responding to a level visibility change */
		uint8 bDisableIntermediateSorting : 1;

		uint8 bNeedsColumRefresh : 1;

		/** Reentrancy guard */
		bool bIsReentrant;

		/* Widget containing the filtering text box */
		TSharedPtr< SSearchBox > FilterTextBoxWidget;

		/** The header row of the scene outliner */
		TSharedPtr< SHeaderRow > HeaderRowWidget;

		/** A collection of filters used to filter the displayed items and folders in the scene outliner */
		TSharedPtr< FOutlinerFilters > Filters;

		/** The TextFilter attached to the SearchBox widget of the Scene Outliner */
		TSharedPtr< TreeItemTextFilter > SearchBoxFilter;

		/** True if the search box will take keyboard focus next frame */
		bool bPendingFocusNextFrame;

		/** The tree item that is currently pending a rename */
		TWeakPtr<ITreeItem> PendingRenameItem;

		TMap<FName, const FSlateBrush*> CachedIcons;

		/** Maintain a count of the number of folders active in the outliner */
		uint32 FolderCount = 0;

		FTreeItemPtrEvent OnDoubleClickOnTreeEvent;

		FOnItemSelectionChanged OnItemSelectionChanged;
	private:

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override {};

		/** Functions relating to sorting */

		/** Timer for PIE/SIE mode to sort the outliner. */
		float SortOutlinerTimer;

		/** true if the outliner currently needs to be sorted */
		bool bSortDirty;

		/** Specify which column to sort with */
		FName SortByColumn;

		/** Currently selected sorting mode */
		EColumnSortMode::Type SortMode;

		/** Handles column sorting mode change */
		void OnColumnSortModeChanged( const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type InSortMode );

		/** Sort the specified array of items based on the current sort column */
		void SortItems(TArray<FTreeItemPtr>& Items) const;

		virtual uint32 GetTypeSortPriority(const ITreeItem& Item) const override;

		/** Handler for recursively expanding/collapsing items */
		void SetItemExpansionRecursive(FTreeItemPtr Model, bool bInExpansionState);
	};

}		// namespace SceneOutliner
