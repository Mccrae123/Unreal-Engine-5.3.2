// Copyright Epic Games, Inc. All Rights Reserved.

#include "SDMXControlConsoleEditorFaderGroupView.h"

#include "DMXControlConsole.h"
#include "DMXControlConsoleEditorManager.h"
#include "DMXControlConsoleEditorSelection.h"
#include "DMXControlConsoleFaderBase.h"
#include "DMXControlConsoleFaderGroup.h"
#include "DMXControlConsoleFaderGroupRow.h"
#include "DMXControlConsoleFixturePatchMatrixCell.h"
#include "Style/DMXControlConsoleEditorStyle.h"
#include "Widgets/SDMXControlConsoleEditorAddButton.h"
#include "Widgets/SDMXControlConsoleEditorExpandArrowButton.h"
#include "Widgets/SDMXControlConsoleEditorFader.h"
#include "Widgets/SDMXControlConsoleEditorFaderGroup.h"
#include "Widgets/SDMXControlConsoleEditorMatrixCell.h"

#include "ScopedTransaction.h"
#include "Styling/SlateColor.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"


#define LOCTEXT_NAMESPACE "SDMXControlConsoleEditorFaderGroupView"

void SDMXControlConsoleEditorFaderGroupView::Construct(const FArguments& InArgs, const TObjectPtr<UDMXControlConsoleFaderGroup>& InFaderGroup)
{
	FaderGroup = InFaderGroup;

	if (!ensureMsgf(FaderGroup.IsValid(), TEXT("Invalid fader group, cannot create fader group view correctly.")))
	{
		return;
	}

	ChildSlot
		[
			SNew(SBorder)
			.BorderBackgroundColor(this, &SDMXControlConsoleEditorFaderGroupView::GetFaderGroupViewBorderColor)
			.BorderImage(FDMXControlConsoleEditorStyle::Get().GetBrush("DMXControlConsole.WhiteBrush"))
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.01f, 0.01f, 0.01f, 1.f))
				.BorderImage(FDMXControlConsoleEditorStyle::Get().GetBrush("DMXControlConsole.WhiteBrush"))
				[
					SNew(SHorizontalBox)

					//Fader Group View main slot
					+SHorizontalBox::Slot()
					.AutoWidth()
					.HAlign(HAlign_Left)
					.VAlign(VAlign_Center)
					[
						SAssignNew(FaderGroupWidget, SDMXControlConsoleEditorFaderGroup, SharedThis(this))
						.OnAddFaderGroup(this, &SDMXControlConsoleEditorFaderGroupView::OnAddFaderGroupClicked)
						.OnAddFaderGroupRow(this, &SDMXControlConsoleEditorFaderGroupView::OnAddFaderGroupRowClicked)
					]
	
					//Fader Group View Faders UI widget
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.HAlign(HAlign_Left)
					.Padding(4.f, 0.f, 0.f, 0.f)
					[
						 GenerateFadersWidget()
					]
				]
			]
		];
}

int32 SDMXControlConsoleEditorFaderGroupView::GetIndex() const
{ 
	if (!FaderGroup.IsValid())
	{
		return INDEX_NONE;
	}

	return FaderGroup->GetIndex();
}

FString SDMXControlConsoleEditorFaderGroupView::GetFaderGroupName() const
{ 
	if (!FaderGroup.IsValid())
	{
		return FString();
	}

	return FaderGroup->GetFaderGroupName(); 
}

void SDMXControlConsoleEditorFaderGroupView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	if (!ensureMsgf(FaderGroup.IsValid(), TEXT("Invalid fader group, cannot update fader group view state correctly.")))
	{
		return;
	}

	const TArray<TScriptInterface<IDMXControlConsoleFaderGroupElement>> AllElements = FaderGroup->GetElements();
	if (AllElements.Num() == ElementWidgets.Num() && !FaderGroup->HasForceRefresh())
	{
		return;
	}

	if (AllElements.Num() > ElementWidgets.Num())
	{
		OnElementAdded();
	}
	else
	{
		OnElementRemoved();
	}

	FaderGroup->ForceRefresh();
}

TSharedRef<SWidget> SDMXControlConsoleEditorFaderGroupView::GenerateFadersWidget()
{
	TSharedRef<SWidget> FadersWidget =
		SNew(SHorizontalBox)
		.Visibility(TAttribute<EVisibility>::CreateSP(this, &SDMXControlConsoleEditorFaderGroupView::GetFadersWidgetVisibility))

		//Add Faders Horizontal Box
		+SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.AutoWidth()
		[
			SAssignNew(FadersHorizontalBox, SHorizontalBox)
		]

		//Add Fader button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(25.f)
			.HeightOverride(25.f)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			.Padding(4.f)
			[
				SNew(SDMXControlConsoleEditorAddButton)
				.OnClicked(this, &SDMXControlConsoleEditorFaderGroupView::OnAddFaderClicked)
				.Visibility(TAttribute<EVisibility>::CreateSP(this, &SDMXControlConsoleEditorFaderGroupView::GetAddFaderButtonVisibility))
			]
		];

	return FadersWidget;
}

FReply SDMXControlConsoleEditorFaderGroupView::OnAddFaderGroupClicked() const
{
	if (FaderGroup.IsValid())
	{
		UDMXControlConsoleFaderGroupRow& FaderGroupRow = FaderGroup->GetOwnerFaderGroupRowChecked();

		const FScopedTransaction FaderGroupClickedTransaction(LOCTEXT("FaderGroupClickedTransaction", "Add Fader Group"));
		FaderGroupRow.Modify();

		FaderGroupRow.AddFaderGroup(GetIndex() + 1);
	}

	return FReply::Handled();
}

FReply SDMXControlConsoleEditorFaderGroupView::OnAddFaderGroupRowClicked() const
{
	if (FaderGroup.IsValid())
	{
		UDMXControlConsoleFaderGroupRow& FaderGroupRow = FaderGroup->GetOwnerFaderGroupRowChecked();
		UDMXControlConsole& ControlConsole = FaderGroupRow.GetOwnerControlConsoleChecked();

		const FScopedTransaction FaderGroupRowClickedTransaction(LOCTEXT("FaderGroupRowClickedTransaction", "Add Fader Group"));
		ControlConsole.Modify();

		const int32 RowIndex = FaderGroupRow.GetRowIndex();
		ControlConsole.AddFaderGroupRow(RowIndex + 1);
	}

	return FReply::Handled();
}

FReply SDMXControlConsoleEditorFaderGroupView::OnAddFaderClicked()
{
	if (FaderGroup.IsValid())
	{
		const FScopedTransaction FaderClickedTransaction(LOCTEXT("FaderClickedTransaction", "Add Fader"));
		FaderGroup->PreEditChange(nullptr);

		FaderGroup->AddRawFader();

		FaderGroup->PostEditChange();
	}

	return FReply::Handled();
}

void SDMXControlConsoleEditorFaderGroupView::OnElementAdded()
{
	const TArray<TScriptInterface<IDMXControlConsoleFaderGroupElement>> Elements = FaderGroup->GetElements();

	for (const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element : Elements)
	{
		if (!Element)
		{
			continue;
		}

		if (ContainsElement(Element))
		{
			continue;
		}

		AddElement(Element);
	}
}

void SDMXControlConsoleEditorFaderGroupView::AddElement(const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element)
{
	if (!ensureMsgf(Element, TEXT("Invalid fader, cannot add new fader correctly.")))
	{
		return;
	}

	if (!FadersHorizontalBox.IsValid())
	{
		return;
	}


	TSharedPtr<SWidget> ElementWidget = nullptr;
	UDMXControlConsoleFixturePatchMatrixCell* MatrixCell = Cast<UDMXControlConsoleFixturePatchMatrixCell>(Element.GetObject());
	if (MatrixCell)
	{
		SAssignNew(ElementWidget, SDMXControlConsoleEditorMatrixCell, MatrixCell);
	}
	else
	{
		UDMXControlConsoleFaderBase* Fader = Cast<UDMXControlConsoleFaderBase>(Element.GetObject());
		SAssignNew(ElementWidget, SDMXControlConsoleEditorFader, Fader)
			.Padding(FMargin(4.f, 0.f));
	}

	ElementWidgets.Add(ElementWidget);

	const int32 Index = Element->GetIndex();
	FadersHorizontalBox->InsertSlot(Index)
		.AutoWidth()
		.HAlign(HAlign_Left)
		[
			ElementWidget.ToSharedRef()
		];
}

void SDMXControlConsoleEditorFaderGroupView::OnElementRemoved()
{
	const TArray<TScriptInterface<IDMXControlConsoleFaderGroupElement>> Elements = FaderGroup->GetElements();

	TArray<TWeakPtr<SWidget>> ElementWidgetsToRemove;
	for (TWeakPtr<SWidget>& Widget : ElementWidgets)
	{
		if (!Widget.IsValid())
		{
			continue;
		}

		const TSharedPtr<SDMXControlConsoleEditorFader> FaderWidget = StaticCastSharedPtr<SDMXControlConsoleEditorFader>(Widget.Pin());
		if (FaderWidget.IsValid())
		{
			UDMXControlConsoleFaderBase* Fader = Fader = FaderWidget->GetFader();
			if (Fader && Elements.Contains(Fader))
			{
				continue;
			}
		}

		const TSharedPtr<SDMXControlConsoleEditorMatrixCell> MatrixCellWidget = StaticCastSharedPtr<SDMXControlConsoleEditorMatrixCell>(Widget.Pin());
		if (MatrixCellWidget.IsValid())
		{
			UDMXControlConsoleFixturePatchMatrixCell* MatrixCell = MatrixCellWidget->GetMatrixCell();
			if (MatrixCell && Elements.Contains(MatrixCell))
			{
				continue;
			}
		}

		FadersHorizontalBox->RemoveSlot(Widget.Pin().ToSharedRef());
		ElementWidgetsToRemove.Add(Widget);
	}

	ElementWidgets.RemoveAll([&ElementWidgetsToRemove](const TWeakPtr<SWidget> FaderWidget)
		{
			return !FaderWidget.IsValid() || ElementWidgetsToRemove.Contains(FaderWidget);
		});
}

bool SDMXControlConsoleEditorFaderGroupView::ContainsElement(const TScriptInterface<IDMXControlConsoleFaderGroupElement>& Element)
{
	auto IsFaderInUseLambda = [Element](const TWeakPtr<SWidget> Widget)
	{
		if (!Widget.IsValid())
		{
			return false;
		}

		const TSharedPtr<SDMXControlConsoleEditorFader> FaderWidget = StaticCastSharedPtr<SDMXControlConsoleEditorFader>(Widget.Pin());
		if (FaderWidget.IsValid())
		{
			UDMXControlConsoleFaderBase* Fader = FaderWidget->GetFader();
			if (Fader)
			{
				return Fader == Element.GetObject();
			}
		}

		const TSharedPtr<SDMXControlConsoleEditorMatrixCell> MatrixCellWidget = StaticCastSharedPtr<SDMXControlConsoleEditorMatrixCell>(Widget.Pin());
		if (MatrixCellWidget.IsValid())
		{
			UDMXControlConsoleFixturePatchMatrixCell* MatrixCell = MatrixCellWidget->GetMatrixCell();
			if (MatrixCell)
			{
				return MatrixCell == Element.GetObject();
			}
		}

		return false;
	};

	return ElementWidgets.ContainsByPredicate(IsFaderInUseLambda);
}

FSlateColor SDMXControlConsoleEditorFaderGroupView::GetFaderGroupViewBorderColor() const
{
	if (!FaderGroup.IsValid())
	{
		return FLinearColor::White;
	}

	return FaderGroup->GetEditorColor();
}

EVisibility SDMXControlConsoleEditorFaderGroupView::GetFadersWidgetVisibility() const
{
	if (!FaderGroupWidget.IsValid())
	{
		return EVisibility::Collapsed;
	}

	const TSharedPtr<SDMXControlConsoleEditorExpandArrowButton>& ExpandArrowButton = FaderGroupWidget->GetExpandArrowButton();
	const bool bIsVisible = ExpandArrowButton.IsValid() && ExpandArrowButton->IsExpanded();
	return bIsVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SDMXControlConsoleEditorFaderGroupView::GetAddFaderButtonVisibility() const
{
	if (!FaderGroup.IsValid())
	{
		return EVisibility::Collapsed;
	}

	return FaderGroup->GetFixturePatch() ? EVisibility::Collapsed : EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE
