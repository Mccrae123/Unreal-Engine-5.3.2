// Copyright Epic Games, Inc. All Rights Reserved.

#include "SNiagaraScratchPad.h"
#include "ViewModels/NiagaraScratchPadViewModel.h"
#include "ViewModels/NiagaraScratchPadScriptViewModel.h"
#include "Widgets/SDynamicLayoutBox.h"
#include "Widgets/SItemSelector.h"
#include "Widgets/SNiagaraSelectedObjectsDetails.h"
#include "Widgets/SVerticalResizeBox.h"
#include "Widgets/SNiagaraScriptGraph.h"
#include "NiagaraObjectSelection.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraEditorStyle.h"
#include "NiagaraEditorWidgetsStyle.h"
#include "NiagaraScratchPadCommandContext.h"

#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Input/SButton.h"
#include "EditorStyleSet.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "EditorFontGlyphs.h"

#define LOCTEXT_NAMESPACE "NiagaraScratchPad"

FName ScriptSelectorName = "ScriptSelector";
FName ScriptEditorName = "ScriptEditor";
FName SelectionEditorName = "SelectionEditor";
FName WideLayoutName = "Wide";
FName NarrowLayoutName = "Narrow";

class SNiagaraPinButton : public SButton
{
public:
	DECLARE_DELEGATE_OneParam(FOnPinnedChanged, bool /* bIsPinned */)

public:
	SLATE_BEGIN_ARGS(SNiagaraPinButton)
		: _IsPinned(false) 
		, _ShowWhenUnpinned(true)
		, _PinTargetDisplayName(LOCTEXT("DefaultTargetDisplayName", "Target"))
		, _PinItemDisplayName(LOCTEXT("DefaultItemDisplayName", "Item"))
	{}
		SLATE_ATTRIBUTE(bool, IsPinned)
		SLATE_ATTRIBUTE(bool, ShowWhenUnpinned)
		SLATE_ARGUMENT(FText, PinTargetDisplayName)
		SLATE_ARGUMENT(FText, PinItemDisplayName)
		SLATE_EVENT(FOnPinnedChanged, OnPinnedChanged);
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		IsPinned = InArgs._IsPinned;
		ShowWhenUnpinned = InArgs._ShowWhenUnpinned;
		OnPinnedChangedDelegate = InArgs._OnPinnedChanged;
		PinnedToolTip = FText::Format(LOCTEXT("UnpinnedFormat", "Unpin this {0} from the {1}."), InArgs._PinItemDisplayName, InArgs._PinTargetDisplayName);
		UnpinnedToolTip = FText::Format(LOCTEXT("PinnedFormat", "Pin this {0} to the {1}."), InArgs._PinItemDisplayName, InArgs._PinTargetDisplayName);

		// Visibility and ToolTipText are base attributes so can't be set in the construct call below,
		// so them them directly here since the base widget construct has already been run.
		TAttribute<EVisibility> PinVisibility;
		PinVisibility.Bind(this, &SNiagaraPinButton::GetVisibilityFromPinned);
		SetVisibility(PinVisibility);
		TAttribute<FText> PinToolTipText;
		PinToolTipText.Bind(this, &SNiagaraPinButton::GetToolTipTextFromPinned);
		SetToolTipText(PinToolTipText);

		SButton::Construct(
			SButton::FArguments()
			.ButtonStyle(FEditorStyle::Get(), "HoverHintOnly")
			.ForegroundColor(FSlateColor::UseForeground())
			.OnClicked(this, &SNiagaraPinButton::OnButtonClicked)
			.ContentPadding(FMargin(3, 2, 2, 2))
			[
				SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.WidthOverride(16)
				.HeightOverride(16)
				.RenderTransform(this, &SNiagaraPinButton::GetPinGlyphRenderTransform)
				.RenderTransformPivot(FVector2D(0.5f, 0.5f))
				[
					SNew(STextBlock)
					.Font(FEditorStyle::Get().GetFontStyle("FontAwesome.10"))
					.Text(FEditorFontGlyphs::Thumb_Tack)
				]
			]);
	}

private:
	FReply OnButtonClicked()
	{
		OnPinnedChangedDelegate.ExecuteIfBound(!IsPinned.Get());
		return FReply::Handled();
	}

	FText GetToolTipTextFromPinned() const
	{
		return IsPinned.Get(false) 
			? PinnedToolTip 
			: UnpinnedToolTip;
	}

	EVisibility GetVisibilityFromPinned() const
	{
		return IsPinned.Get(false) || ShowWhenUnpinned.Get(true)
			? EVisibility::Visible
			: EVisibility::Hidden;
	}

	TOptional<FSlateRenderTransform> GetPinGlyphRenderTransform() const
	{
		return IsPinned.Get(false) 
			? TOptional<FSlateRenderTransform>()
			: FSlateRenderTransform(FQuat2D(PI / 2));
	}

private:
	TAttribute<bool> IsPinned;
	TAttribute<bool> ShowWhenUnpinned;
	FOnPinnedChanged OnPinnedChangedDelegate;
	FText PinnedToolTip;
	FText UnpinnedToolTip;
};

class SNiagaraScratchPadScriptRow : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNiagaraScratchPadScriptRow) {}
		SLATE_ATTRIBUTE(bool, IsSelected);
	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs,
		UNiagaraScratchPadViewModel* InScratchPadViewModel,
		TSharedRef<FNiagaraScratchPadScriptViewModel> InScriptViewModel,
		TSharedPtr<FNiagaraScratchPadCommandContext> InCommandContext)
	{
		ScratchPadViewModel = InScratchPadViewModel;
		ScriptViewModel = InScriptViewModel;
		CommandContext = InCommandContext;
		IsSelected = InArgs._IsSelected;

		ChildSlot
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.Padding(3, 0, 0, 0)
			[
				SAssignNew(NameEditableText, SInlineEditableTextBlock)
				.Text(this, &SNiagaraScratchPadScriptRow::GetNameText)
				.ToolTipText(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::GetToolTip)
				.IsSelected(this, &SNiagaraScratchPadScriptRow::GetIsSelected)
				.OnTextCommitted(this, &SNiagaraScratchPadScriptRow::OnNameTextCommitted)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(1)
			[
				SNew(SNiagaraPinButton)
				.IsPinned(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::GetIsPinned)
				.OnPinnedChanged(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::SetIsPinned)
				.ShowWhenUnpinned(this, &SNiagaraScratchPadScriptRow::IsActive)
				.PinItemDisplayName(LOCTEXT("PinItem", "script"))
				.PinTargetDisplayName(LOCTEXT("PinTarget", "edit list"))
			]
		];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		if (ScriptViewModel->GetIsPendingRename())
		{
			ScriptViewModel->SetIsPendingRename(false);
			NameEditableText->EnterEditingMode();
		}
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			return FReply::Handled()
				.CaptureMouse(SharedThis(this));
		}
		return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			// Set this script to be the active one.
			ScratchPadViewModel->SetActiveScriptViewModel(ScriptViewModel.ToSharedRef());

			FMenuBuilder MenuBuilder(true, CommandContext->GetCommands());
			CommandContext->AddMenuItems(MenuBuilder);

			FWidgetPath WidgetPath = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
			FSlateApplication::Get().PushMenu(AsShared(), WidgetPath, MenuBuilder.MakeWidget(), MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
			return FReply::Handled().ReleaseMouseCapture();
		}
		return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
	}

private:
	FText GetNameText() const
	{
		return ScriptViewModel->GetDisplayName();
	}

	bool GetIsSelected() const
	{
		return IsSelected.Get(false);
	}

	void OnNameTextCommitted(const FText& InText, ETextCommit::Type InCommitType)
	{
		ScriptViewModel->SetScriptName(InText);
	}

	bool IsActive() const
	{
		return IsHovered();
	}

private:
	UNiagaraScratchPadViewModel* ScratchPadViewModel;
	TSharedPtr<FNiagaraScratchPadScriptViewModel> ScriptViewModel;
	TSharedPtr<FNiagaraScratchPadCommandContext> CommandContext;
	TAttribute<bool> IsSelected;
	TSharedPtr<SInlineEditableTextBlock> NameEditableText;
};

class SNiagaraScratchPadScriptSelector : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNiagaraScratchPadScriptSelector) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UNiagaraScratchPadViewModel* InViewModel, TSharedPtr<FNiagaraScratchPadCommandContext> InCommandContext)
	{
		ViewModel = InViewModel;
		CommandContext = InCommandContext;
		ViewModel->OnScriptViewModelsChanged().AddSP(this, &SNiagaraScratchPadScriptSelector::ScriptViewModelsChanged);
		ViewModel->OnActiveScriptChanged().AddSP(this, &SNiagaraScratchPadScriptSelector::ActiveScriptChanged);
		bIsUpdatingSelection = false;

		ChildSlot
		[
			SAssignNew(ScriptSelector, SNiagaraScriptViewModelSelector)
			.ClickActivateMode(EItemSelectorClickActivateMode::SingleClick)
			.CategoryRowStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.CategoryRow")
			.ClearSelectionOnClick(false)
			.Items(ViewModel->GetScriptViewModels())
			.DefaultCategories(ViewModel->GetAvailableUsages())
			.OnGetCategoriesForItem(this, &SNiagaraScratchPadScriptSelector::OnGetCategoriesForItem)
			.OnCompareCategoriesForEquality(this, &SNiagaraScratchPadScriptSelector::OnCompareCategoriesForEquality)
			.OnCompareCategoriesForSorting(this, &SNiagaraScratchPadScriptSelector::OnCompareCategoriesForSorting)
			.OnCompareItemsForEquality(this, &SNiagaraScratchPadScriptSelector::OnCompareItemsForEquality)
			.OnCompareItemsForSorting(this, &SNiagaraScratchPadScriptSelector::OnCompareItemsForSorting)
			.OnDoesItemMatchFilterText(this, &SNiagaraScratchPadScriptSelector::OnDoesItemMatchFilterText)
			.OnGenerateWidgetForCategory(this, &SNiagaraScratchPadScriptSelector::OnGenerateWidgetForCategory)
			.OnGenerateWidgetForItem(this, &SNiagaraScratchPadScriptSelector::OnGenerateWidgetForItem)
			.OnItemActivated(this, &SNiagaraScratchPadScriptSelector::OnScriptActivated)
			.OnSelectionChanged(this, &SNiagaraScratchPadScriptSelector::OnSelectionChanged)
		];

		if (ViewModel->GetActiveScriptViewModel().IsValid())
		{
			TArray<TSharedRef<FNiagaraScratchPadScriptViewModel>> SelectedViewModels;
			SelectedViewModels.Add(ViewModel->GetActiveScriptViewModel().ToSharedRef());
			ScriptSelector->SetSelectedItems(SelectedViewModels);
		}
	}

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		if (CommandContext->ProcessCommandBindings(InKeyEvent))
		{
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			return FReply::Handled()
				.CaptureMouse(SharedThis(this));
		}
		return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			FMenuBuilder MenuBuilder(true, CommandContext->GetCommands());
			CommandContext->AddMenuItems(MenuBuilder);

			FWidgetPath WidgetPath = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
			FSlateApplication::Get().PushMenu(AsShared(), WidgetPath, MenuBuilder.MakeWidget(), MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
			return FReply::Handled().ReleaseMouseCapture();
		}
		return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
	}

private:
	void ScriptViewModelsChanged()
	{
		if (ScriptSelector.IsValid())
		{
			ScriptSelector->RefreshItemsAndDefaultCategories(ViewModel->GetScriptViewModels(), ViewModel->GetAvailableUsages());
		}
	}

	void ActiveScriptChanged()
	{
		if (bIsUpdatingSelection == false)
		{
			TGuardValue<bool> UpdateGuard(bIsUpdatingSelection, true);
			TSharedPtr<FNiagaraScratchPadScriptViewModel> ActiveScriptViewModel = ViewModel->GetActiveScriptViewModel();
			if (ActiveScriptViewModel.IsValid())
			{
				TArray<TSharedRef<FNiagaraScratchPadScriptViewModel>> SelectedItems;
				SelectedItems.Add(ActiveScriptViewModel.ToSharedRef());
				ScriptSelector->SetSelectedItems(SelectedItems);
			}
			else
			{
				ScriptSelector->ClearSelectedItems();
			}
		}
	}

	TArray<ENiagaraScriptUsage> OnGetCategoriesForItem(const TSharedRef<FNiagaraScratchPadScriptViewModel>& Item)
	{
		TArray<ENiagaraScriptUsage> Categories;
		Categories.Add(Item->GetScripts()[0]->GetUsage());
		return Categories;
	}

	bool OnCompareCategoriesForEquality(const ENiagaraScriptUsage& CategoryA, const ENiagaraScriptUsage& CategoryB) const
	{
		return CategoryA == CategoryB;
	}

	bool OnCompareCategoriesForSorting(const ENiagaraScriptUsage& CategoryA, const ENiagaraScriptUsage& CategoryB) const
	{
		return ((int32)CategoryA) < ((int32)CategoryB);
	}

	bool OnCompareItemsForEquality(const TSharedRef<FNiagaraScratchPadScriptViewModel>& ItemA, const TSharedRef<FNiagaraScratchPadScriptViewModel>& ItemB) const
	{
		return ItemA == ItemB;
	}

	bool OnCompareItemsForSorting(const TSharedRef<FNiagaraScratchPadScriptViewModel>& ItemA, const TSharedRef<FNiagaraScratchPadScriptViewModel>& ItemB) const
	{
		return ItemA->GetDisplayName().CompareTo(ItemB->GetDisplayName()) < 0;
	}

	bool OnDoesItemMatchFilterText(const FText& FilterText, const TSharedRef<FNiagaraScratchPadScriptViewModel>& Item)
	{
		return Item->GetDisplayName().ToString().Find(FilterText.ToString(), ESearchCase::IgnoreCase) != INDEX_NONE;
	}

	TSharedRef<SWidget> OnGenerateWidgetForCategory(const ENiagaraScriptUsage& Category)
	{
		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(3, 0, 0, 0)
		[
			SNew(STextBlock)
			.TextStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.SmallHeaderText")
			.Text(ViewModel->GetDisplayNameForUsage(Category))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 4.0f, 3.0f, 4.0f)
		[
			SNew(SButton)
			.ButtonStyle(FEditorStyle::Get(), "RoundButton")
			.OnClicked(this, &SNiagaraScratchPadScriptSelector::ScriptSelectorAddButtonClicked, Category)
			.ContentPadding(FMargin(3.0f, 2.0f, 2.0f, 2.0f))
			.Content()
			[
				SNew(SImage)
				.Image(FEditorStyle::GetBrush("Plus"))
			]
		];
	}

	TSharedRef<SWidget> OnGenerateWidgetForItem(const TSharedRef<FNiagaraScratchPadScriptViewModel>& Item)
	{
		return SNew(SNiagaraScratchPadScriptRow, ViewModel, Item, CommandContext)
		.IsSelected(this, &SNiagaraScratchPadScriptSelector::GetItemIsSelected, TWeakPtr<FNiagaraScratchPadScriptViewModel>(Item));
	}

	void OnScriptActivated(const TSharedRef<FNiagaraScratchPadScriptViewModel>& ActivatedScript)
	{
		if (bIsUpdatingSelection == false)
		{
			TGuardValue<bool> UpdateGuard(bIsUpdatingSelection, true);
			ViewModel->SetActiveScriptViewModel(ActivatedScript);
		}
	}

	void OnSelectionChanged()
	{
		if (bIsUpdatingSelection == false)
		{
			TGuardValue<bool> UpdateGuard(bIsUpdatingSelection, true);
			TArray<TSharedRef<FNiagaraScratchPadScriptViewModel>> SelectedScripts = ScriptSelector->GetSelectedItems();
			if (SelectedScripts.Num() == 0)
			{
				ViewModel->ResetActiveScriptViewModel();
			}
			else if (SelectedScripts.Num())
			{
				ViewModel->SetActiveScriptViewModel(SelectedScripts[0]);
			}
		}
	}

	FReply ScriptSelectorAddButtonClicked(ENiagaraScriptUsage Usage)
	{
		TSharedPtr<FNiagaraScratchPadScriptViewModel> NewScriptViewModel = ViewModel->CreateNewScript(Usage, ENiagaraScriptUsage::ParticleUpdateScript, FNiagaraTypeDefinition());
		if (NewScriptViewModel.IsValid())
		{
			ViewModel->SetActiveScriptViewModel(NewScriptViewModel.ToSharedRef());
			NewScriptViewModel->SetIsPendingRename(true);
		}
		return FReply::Handled();
	}

	bool GetItemIsSelected(TWeakPtr<FNiagaraScratchPadScriptViewModel> ItemWeak) const
	{
		TSharedPtr<FNiagaraScratchPadScriptViewModel> Item = ItemWeak.Pin();
		return Item.IsValid() && ViewModel->GetActiveScriptViewModel() == Item;
	}

private:
	TSharedPtr<SNiagaraScriptViewModelSelector> ScriptSelector;
	UNiagaraScratchPadViewModel* ViewModel;
	TSharedPtr<FNiagaraScratchPadCommandContext> CommandContext;
	bool bIsUpdatingSelection;
};

class SNiagaraScratchPadScriptEditor : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNiagaraScratchPadScriptEditor) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedRef<FNiagaraScratchPadScriptViewModel> InScriptViewModel)
	{
		ScriptViewModel = InScriptViewModel;
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 2, 0)
				.AutoWidth()
				[
					SNew(STextBlock)
					.TextStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.LargeHeaderText")
					.Text(this, &SNiagaraScratchPadScriptEditor::GetNameText)
					.ToolTipText(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::GetToolTip)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(1)
				[
					SNew(SNiagaraPinButton)
					.IsPinned(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::GetIsPinned)
					.OnPinnedChanged(ScriptViewModel.ToSharedRef(), &FNiagaraScratchPadScriptViewModel::SetIsPinned)
					.PinItemDisplayName(LOCTEXT("PinItem", "script"))
					.PinTargetDisplayName(LOCTEXT("PinTarget", "edit list"))
				]
				+ SHorizontalBox::Slot()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 2.0f, 1.0f, 4.0f)
				[
					SNew(SButton)
					.ButtonStyle(FEditorStyle::Get(), "RoundButton")
					.OnClicked(this, &SNiagaraScratchPadScriptEditor::OnApplyButtonClicked)
					.ToolTipText(LOCTEXT("ApplyButtonToolTip", "Apply the current changes to this script.  This will update the selection stack UI and compile neccessary scripts."))
					.IsEnabled(this, &SNiagaraScratchPadScriptEditor::GetApplyButtonIsEnabled)
					.ForegroundColor(FSlateColor::UseForeground())
					.ContentPadding(FMargin(0.0f))
					.Content()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(2.0f, 1.0f, 2.0f, 1.0f)
						[
							SNew(SImage)
							.Image(FNiagaraEditorStyle::Get().GetBrush("NiagaraEditor.Apply.Small"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(2.0f, 2.0f, 2.0f, 3.0f)
						[
							SNew(STextBlock)
							.TextStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.SmallHeaderText")
							.Text(LOCTEXT("ApplyButtonLabel", "Apply"))
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			[
				SNew(SNiagaraScriptGraph, ScriptViewModel->GetGraphViewModel())
				.ZoomToFitOnLoad(true)
			]
		];
	}

private:
	FText GetNameText() const
	{
		return ScriptViewModel->GetDisplayName();
	}

	FReply OnApplyButtonClicked()
	{
		ScriptViewModel->ApplyChanges();

		return FReply::Handled();
	}

	bool GetApplyButtonIsEnabled() const
	{
		return ScriptViewModel->CanApplyChanges();
	}

private:
	TSharedPtr<FNiagaraScratchPadScriptViewModel> ScriptViewModel;
};

class SNiagaraScratchPadScriptEditorList : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNiagaraScratchPadScriptEditor) {}

	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs, UNiagaraScratchPadViewModel* InViewModel)
	{
		ViewModel = InViewModel;
		ViewModel->OnScriptViewModelsChanged().AddSP(this, &SNiagaraScratchPadScriptEditorList::ScriptViewModelsChanged);
		ViewModel->OnEditScriptViewModelsChanged().AddSP(this, &SNiagaraScratchPadScriptEditorList::EditScriptViewModelsChanged);
		ViewModel->OnActiveScriptChanged().AddSP(this, &SNiagaraScratchPadScriptEditorList::ActiveScriptChanged);
		bIsUpdatingSelection = false;

		UpdateContentFromEditScriptViewModels();
	}

private:
	void ScriptViewModelsChanged()
	{
		ScriptViewModelWidgetPairs.RemoveAll([](const FScriptViewModelWidgetPair& ScriptViewModelWidgetPair)
		{
			return ScriptViewModelWidgetPair.ViewModel.IsValid() == false || ScriptViewModelWidgetPair.Widget.IsValid() == false;
		});
	}

	void EditScriptViewModelsChanged()
	{
		UpdateContentFromEditScriptViewModels();
	}

	TSharedRef<SWidget> FindOrAddScriptEditor(TSharedRef<FNiagaraScratchPadScriptViewModel> ScriptViewModel)
	{
		FScriptViewModelWidgetPair* ExistingPair = ScriptViewModelWidgetPairs.FindByPredicate([ScriptViewModel](FScriptViewModelWidgetPair& ScriptViewModelWidgetPair)
		{ 
			return ScriptViewModelWidgetPair.ViewModel == ScriptViewModel && ScriptViewModelWidgetPair.Widget.IsValid();
		});

		if (ExistingPair != nullptr)
		{
			return ExistingPair->Widget.ToSharedRef();
		}
		else
		{
			TSharedRef<SWidget> NewEditor = SNew(SNiagaraScratchPadScriptEditor, ScriptViewModel);
			ScriptViewModelWidgetPairs.Add({ TWeakPtr<FNiagaraScratchPadScriptViewModel>(ScriptViewModel), NewEditor });
			return NewEditor;
		}
	}

	void UpdateContentFromEditScriptViewModels()
	{
		TSharedPtr<SWidget> NewContent;
		if (ViewModel->GetEditScriptViewModels().Num() == 0)
		{
			NewContent = SNullWidget::NullWidget;
			ScriptEditorList.Reset();
		}
		else if(ViewModel->GetEditScriptViewModels().Num() == 1)
		{
			NewContent = FindOrAddScriptEditor(ViewModel->GetEditScriptViewModels()[0]);
			ScriptEditorList.Reset();
		}
		else 
		{
			if (ScriptEditorList.IsValid())
			{
				ScriptEditorList->RequestListRefresh();
			}
			else 
			{
				ScriptEditorList = SNew(SListView<TSharedRef<FNiagaraScratchPadScriptViewModel>>)
					.ListItemsSource(&ViewModel->GetEditScriptViewModels())
					.OnGenerateRow(this, &SNiagaraScratchPadScriptEditorList::OnGenerateScriptEditorRow)
					.OnSelectionChanged(this, &SNiagaraScratchPadScriptEditorList::OnSelectionChanged);
			}
			NewContent = ScriptEditorList;
		}

		ChildSlot
		[
			NewContent.ToSharedRef()
		];
	}

	void ActiveScriptChanged()
	{
		if (ScriptEditorList.IsValid() && bIsUpdatingSelection == false)
		{
			TGuardValue<bool> UpdateGuard(bIsUpdatingSelection, true);
			TSharedPtr<FNiagaraScratchPadScriptViewModel> ActiveScriptViewModel = ViewModel->GetActiveScriptViewModel();
			if (ActiveScriptViewModel.IsValid())
			{
				ScriptEditorList->SetSelection(ActiveScriptViewModel.ToSharedRef());
			}
			else
			{
				ScriptEditorList->ClearSelection();
			}
		}
	}

	TSharedRef<ITableRow> OnGenerateScriptEditorRow(TSharedRef<FNiagaraScratchPadScriptViewModel> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(STableRow<TSharedRef<FNiagaraScratchPadScriptViewModel>>, OwnerTable)
		[
			SNew(SVerticalResizeBox)
			.ContentHeight(Item, &FNiagaraScratchPadScriptViewModel::GetEditorHeight)
			.ContentHeightChanged(Item, &FNiagaraScratchPadScriptViewModel::SetEditorHeight)
			[
				FindOrAddScriptEditor(Item)
			]
		];
	}

	void OnSelectionChanged(TSharedPtr<FNiagaraScratchPadScriptViewModel> InNewSelection, ESelectInfo::Type SelectInfo)
	{
		if (bIsUpdatingSelection == false)
		{
			TGuardValue<bool> UpdateGuard(bIsUpdatingSelection, true);
			TArray<TSharedRef<FNiagaraScratchPadScriptViewModel>> SelectedScripts;
			ScriptEditorList->GetSelectedItems(SelectedScripts);
			if (SelectedScripts.Num() == 0)
			{
				ViewModel->ResetActiveScriptViewModel();
			}
			else if (SelectedScripts.Num())
			{
				ViewModel->SetActiveScriptViewModel(SelectedScripts[0]);
			}
		}
	}

private:
	struct FScriptViewModelWidgetPair
	{
		TWeakPtr<FNiagaraScratchPadScriptViewModel> ViewModel;
		TSharedPtr<SWidget> Widget;
	};

	UNiagaraScratchPadViewModel* ViewModel;
	TSharedPtr<SListView<TSharedRef<FNiagaraScratchPadScriptViewModel>>> ScriptEditorList;
	TArray<FScriptViewModelWidgetPair> ScriptViewModelWidgetPairs;
	bool bIsUpdatingSelection;
};

void SNiagaraScratchPad::Construct(const FArguments& InArgs, UNiagaraScratchPadViewModel* InViewModel)
{
	ViewModel = InViewModel;
	CommandContext = MakeShared<FNiagaraScratchPadCommandContext>(InViewModel);

	ChildSlot
	[
		SNew(SDynamicLayoutBox)
		.GenerateNamedWidget_Lambda([this](FName InWidgetName)
		{
			if (InWidgetName == ScriptSelectorName)
			{
				return ConstructScriptSelector();
			}
			else if (InWidgetName == ScriptEditorName)
			{
				return ConstructScriptEditor();
			}
			else if (InWidgetName == SelectionEditorName)
			{
				return ConstructSelectionEditor();
			}
			else
			{
				return SNullWidget::NullWidget;
			}
		})
		.GenerateNamedLayout_Lambda([this](FName InLayoutName, const SDynamicLayoutBox::FNamedWidgetProvider& InNamedWidgetProvider)
		{
			TSharedPtr<SWidget> Layout;
			if (InLayoutName == WideLayoutName)
			{
				Layout = SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(7.0f)
				.HitDetectionSplitterHandleSize(7.0f)
				+ SSplitter::Slot()
				.Value(0.15f)
				[
					InNamedWidgetProvider.GetNamedWidget(ScriptSelectorName)
				]
				+ SSplitter::Slot()
				.Value(0.6f)
				[
					InNamedWidgetProvider.GetNamedWidget(ScriptEditorName)
				]
				+ SSplitter::Slot()
				.Value(0.25f)
				[
					InNamedWidgetProvider.GetNamedWidget(SelectionEditorName)
				];
			}
			else if (InLayoutName == NarrowLayoutName)
			{
				Layout = SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(7.0f)
				.HitDetectionSplitterHandleSize(7.0f)
				+ SSplitter::Slot()
				.Value(0.3f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)
					+ SSplitter::Slot()
					.Value(0.5f)
					[
						InNamedWidgetProvider.GetNamedWidget(ScriptSelectorName)
					]
					+ SSplitter::Slot()
					.Value(0.5f)
					[
						InNamedWidgetProvider.GetNamedWidget(SelectionEditorName)
					]
				]
				+ SSplitter::Slot()
				.Value(0.7f)
				[
					InNamedWidgetProvider.GetNamedWidget(ScriptEditorName)
				];
			}
			else
			{
				Layout = SNullWidget::NullWidget;
			}
			return Layout.ToSharedRef();
		})
		.ChooseLayout_Lambda([this]() 
		{ 
			if (GetCachedGeometry().GetLocalSize().X < 1500)
			{
				return NarrowLayoutName;
			}
			else
			{
				return WideLayoutName;
			}
		})
	];
}

TSharedRef<SWidget> SNiagaraScratchPad::ConstructScriptSelector()
{
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(0.0f, 2.0f)
	[
		SNew(STextBlock)
		.TextStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.LargeHeaderText")
		.Text(LOCTEXT("ScriptSelector", "Scratch Script Selector"))
	]
	+ SVerticalBox::Slot()
	[
		SNew(SNiagaraScratchPadScriptSelector, ViewModel.Get(), CommandContext)
	];
}

TSharedRef<SWidget> SNiagaraScratchPad::ConstructScriptEditor()
{
	return SNew(SNiagaraScratchPadScriptEditorList, ViewModel.Get());
}

TSharedRef<SWidget> SNiagaraScratchPad::ConstructSelectionEditor()
{
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		SNew(STextBlock)
		.TextStyle(FNiagaraEditorWidgetsStyle::Get(), "NiagaraEditor.ScratchPad.LargeHeaderText")
		.Text(LOCTEXT("ScratchPadSelection", "Scratch Pad Selection"))
	]
	+ SVerticalBox::Slot()
	[
		SNew(SNiagaraSelectedObjectsDetails, ViewModel->GetObjectSelection())
	];
}

#undef LOCTEXT_NAMESPACE