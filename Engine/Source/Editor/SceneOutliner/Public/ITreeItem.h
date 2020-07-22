// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "Widgets/Views/STableRow.h"
#include "SceneOutlinerStandaloneTypes.h"

class UToolMenu;

namespace SceneOutliner
{
	/** Base tree item interface  */
	struct ITreeItem : TSharedFromThis<ITreeItem>
	{
		/** Friendship required for access to various internals */
		friend SSceneOutliner;

		/* Flags structure */
		struct FlagsType
		{
			/** Whether this item is expanded or not */
			bool bIsExpanded : 1;

			/* true if this item is filtered out */
			bool bIsFilteredOut : 1;

			/* true if this item can be interacted with as per the current outliner filters */
			bool bInteractive : 1;

			/** true if this item's children need to be sorted */
			bool bChildrenRequireSort : 1;

			/** Default constructor */		
			FlagsType() : bIsExpanded(1), bIsFilteredOut(0), bInteractive(1), bChildrenRequireSort(1) {}
		};

	public:
		
		/** Flags for this item */
		FlagsType Flags;

		/** Delegate for hooking up an inline editable text block to be notified that a rename is requested. */
		DECLARE_DELEGATE( FOnRenameRequest );

		/** Broadcasts whenever a rename is requested */
		FOnRenameRequest RenameRequestEvent;

	protected:

		/** Default constructor */
		ITreeItem(FTreeItemType InType) : Parent(nullptr), TreeType(InType) {}
		virtual ~ITreeItem() {}

		/** This item's parent, if any. */
		mutable TWeakPtr<ITreeItem> Parent;

		/** Array of children contained underneath this item */
		mutable TArray<TWeakPtr<ITreeItem>> Children;

		/** Static type identifier for the base class tree item */
		static const FTreeItemType Type;

		/** Tree item type identifier */
		FTreeItemType TreeType;
	public:

		/** Get this item's parent. Can be nullptr. */
		FTreeItemPtr GetParent() const
		{
			return Parent.Pin();
		}

		/** Add a child to this item */
		void AddChild(FTreeItemRef Child)
		{
			check(!Children.Contains(Child));
			Child->Parent = AsShared();
			Children.Add(MoveTemp(Child));
		}

		/** Remove a child from this item */
		void RemoveChild(const FTreeItemRef& Child)
		{
			if (Children.Remove(Child))
			{
				Child->Parent = nullptr;	
			}
		}

		/** Get this item's children, if any. Although we store as weak pointers, they are guaranteed to be valid. */
		FORCEINLINE const TArray<TWeakPtr<ITreeItem>>& GetChildren() const
		{
			return Children;
		}
	public:
		/** Attempt to cast this item to another type if it is of that type. Returns nullptr if it fails */
		template <typename T>
		T* CastTo()
		{
			return TreeType.IsA(T::Type) ? StaticCast<T*>(this) : nullptr;
		}
		/** Attempt to cast this item to another type if it is of that type. Returns nullptr if it fails */
		template <typename T>
		const T* CastTo() const
		{
			return TreeType.IsA(T::Type) ? StaticCast<const T*>(this) : nullptr;
		}
		/** Returns true if this item is of the specified type */
		template <typename T>
		bool IsA() const
		{
			return TreeType.IsA(T::Type);
		}

		/** Returns true if the data the item references is valid */
		virtual bool IsValid() const = 0;

	public:

		/** Get the ID that represents this tree item. Used to reference this item in a map */
		virtual FTreeItemID GetID() const = 0;

		/** Get the raw string to display for this tree item - used for sorting */
		virtual FString GetDisplayString() const = 0;

		/** Check whether it should be possible to interact with this tree item */
		virtual bool CanInteract() const = 0;
		
		/** Called when this item is expanded or collapsed */
		virtual void OnExpansionChanged() {};

		/** Generate the label widget for this item */
		virtual TSharedRef<SWidget> GenerateLabelWidget(ISceneOutliner& Outliner, const STableRow<FTreeItemPtr>& InRow) { return SNullWidget::NullWidget; }

		/** Generate a context menu for this item. Only called if *only* this item is selected. */
		virtual void GenerateContextMenu(UToolMenu* Menu, SSceneOutliner& Outliner) {}

		/** Called when this items visibility changed */
		virtual void OnVisibilityChanged(const bool bNewVisibility) {}

		/** Returns true if this item can set its own visibility */
		virtual bool HasVisibilityInfo() const { return false; }
		/** Query this items visibility state. Only called if the item type has visibility info */
		virtual bool GetVisibility() const { return false; }

	};

}	// namespace SceneOutliner
