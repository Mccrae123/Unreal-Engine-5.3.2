// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NiagaraActions.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "EdGraph/EdGraphSchema.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SItemSelector.h"
#include "Widgets/SNiagaraScriptSourceFilter.h"

class INiagaraStackItemGroupAddUtilities;

typedef SItemSelector<FString, TSharedPtr<FNiagaraMenuAction_Generic>, ENiagaraMenuSections> SNiagaraStackAddSelector;

class SNiagaraStackItemGroupAddMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNiagaraStackItemGroupAddMenu) { }
	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs, INiagaraStackItemGroupAddUtilities* InAddUtilities, int32 InInsertIndex);

	TSharedPtr<SWidget> GetFilterTextBox();

private:
	bool GetLibraryOnly() const;

	void SetLibraryOnly(bool bInLibraryOnly);

private:
	INiagaraStackItemGroupAddUtilities* AddUtilities;

	int32 InsertIndex;

	TSharedPtr<SNiagaraStackAddSelector> ActionSelector;
	TSharedPtr<SNiagaraSourceFilterBox> SourceFilter;

	bool bSetFocusOnNextTick;
	
	static bool bLibraryOnly;

private:
	TArray<TSharedPtr<FNiagaraMenuAction_Generic>> CollectActions();
	TArray<FString> OnGetCategoriesForItem(const TSharedPtr<FNiagaraMenuAction_Generic>& Item);
	TArray<ENiagaraMenuSections> OnGetSectionsForItem(const TSharedPtr<FNiagaraMenuAction_Generic>& Item);
	bool OnCompareSectionsForEquality(const ENiagaraMenuSections& SectionA, const ENiagaraMenuSections& SectionB);
	bool OnCompareSectionsForSorting(const ENiagaraMenuSections& SectionA, const ENiagaraMenuSections& SectionB);
	bool OnCompareCategoriesForEquality(const FString& CategoryA, const FString& CategoryB);
	bool OnCompareCategoriesForSorting(const FString& CategoryA, const FString& CategoryB);
	bool OnCompareItemsForEquality(const TSharedPtr<FNiagaraMenuAction_Generic>& ItemA, const TSharedPtr<FNiagaraMenuAction_Generic>& ItemB);
	bool OnCompareItemsForSorting(const TSharedPtr<FNiagaraMenuAction_Generic>& ItemA, const TSharedPtr<FNiagaraMenuAction_Generic>& ItemB);
	bool OnDoesItemMatchFilterText(const FText& FilterText, const TSharedPtr<FNiagaraMenuAction_Generic>& Item);
	int32 OnGetItemWeightForSelection(const TSharedPtr<FNiagaraMenuAction_Generic>& Item, const TArray<FString>& FilterTerms, const TArray<FString>& SanitizedFilterTerms) const;
	TSharedRef<SWidget> OnGenerateWidgetForSection(const ENiagaraMenuSections& Section);
	TSharedRef<SWidget> OnGenerateWidgetForCategory(const FString& Category);
	TSharedRef<SWidget> OnGenerateWidgetForItem(const TSharedPtr<FNiagaraMenuAction_Generic>& Item);
	bool DoesItemPassCustomFilter(const TSharedPtr<FNiagaraMenuAction_Generic>& Item);
	void OnItemActivated(const TSharedPtr<FNiagaraMenuAction_Generic>& Item);

	void TriggerRefresh(const TMap<EScriptSource, bool>& SourceState);

	FText GetFilterText() const { return ActionSelector->GetFilterText(); }
};