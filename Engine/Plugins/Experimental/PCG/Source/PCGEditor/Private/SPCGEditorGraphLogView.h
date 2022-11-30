// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PCGElement.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Logging/LogVerbosity.h"

class FPCGEditor;
class UPCGComponent;
class UPCGEditorGraphNode;
class UPCGEditorGraph;
class UPCGNode;

namespace PCGEditorGraphLogView
{	
	/** Names of the columns in the attribute list */
	extern const FName NAME_Order;
}

struct FPCGLogListViewItem
{
	const UPCGNode* PCGNode = nullptr;	
	const UPCGEditorGraphNode* EditorNode = nullptr;

	int32 Order = 0;
	FName NodeName;
	FName Namespace;
	FString Message;

	ELogVerbosity::Type Verbosity = ELogVerbosity::NoLogging;
};

typedef TSharedPtr<FPCGLogListViewItem> PCGLogListViewItemPtr;

class SPCGLogListViewItemRow : public SMultiColumnTableRow<PCGLogListViewItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SPCGLogListViewItemRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView, const PCGLogListViewItemPtr& Item);

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnId) override;

protected:
	PCGLogListViewItemPtr InternalItem;
};

class SPCGEditorGraphLogView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPCGEditorGraphLogView) { }
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<FPCGEditor> InPCGEditor);

private:
	TSharedRef<SHeaderRow> CreateHeaderRowWidget();
	
	// Callbacks
	FReply Refresh();
	FReply Clear();
	TSharedRef<ITableRow> OnGenerateRow(PCGLogListViewItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable) const;
	void OnItemDoubleClicked(PCGLogListViewItemPtr Item);
	void OnSortColumnHeader(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type NewSortMode);
	EColumnSortMode::Type GetColumnSortMode(const FName ColumnId) const;

	/** Pointer back to the PCG editor that owns us */
	TWeakPtr<FPCGEditor> PCGEditorPtr;

	/** Cached PCGGraph being viewed */
	UPCGEditorGraph* PCGEditorGraph = nullptr;

	TSharedPtr<SHeaderRow> ListViewHeader;
	TSharedPtr<SListView<PCGLogListViewItemPtr>> ListView;
	TArray<PCGLogListViewItemPtr> ListViewItems;

	// To allow sorting
	FName SortingColumn = PCGEditorGraphLogView::NAME_Order;
	EColumnSortMode::Type SortMode = EColumnSortMode::Type::Ascending;
};
