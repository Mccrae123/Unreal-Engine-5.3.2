// Copyright Epic Games, Inc. All Rights Reserved.

#include "OpenVDBImportWindow.h"

#include "Widgets/Input/SButton.h"
#include "SPrimaryButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "IDocumentation.h"
#include "Editor.h"
#include "SparseVolumeTextureOpenVDBUtility.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"

#define LOCTEXT_NAMESPACE "SOpenVDBImportWindow"

static FText GetGridComboBoxItemText(TSharedPtr<FOpenVDBGridInfo> InItem)
{
	return InItem ? FText::FromString(InItem->DisplayString) : LOCTEXT("NoneGrid", "<None>");
};

static FText GetFormatComboBoxItemText(TSharedPtr<ESparseVolumePackedDataFormat> InItem)
{
	const TCHAR* FormatStr = TEXT("<None>");
	if (InItem)
	{
		switch (*InItem)
		{
		case ESparseVolumePackedDataFormat::Unorm8: FormatStr = TEXT("8bit unorm"); break;
		case ESparseVolumePackedDataFormat::Float16: FormatStr = TEXT("16bit float"); break;
		case ESparseVolumePackedDataFormat::Float32: FormatStr = TEXT("32bit float"); break;
		}
	}
	return FText::FromString(FormatStr);
}

void SOpenVDBImportWindow::Construct(const FArguments& InArgs)
{
	PackedDataA = InArgs._PackedDataA;
	OpenVDBGridInfo = InArgs._OpenVDBGridInfo;
	OpenVDBSupportedTargetFormats = InArgs._OpenVDBSupportedTargetFormats;
	WidgetWindow = InArgs._WidgetWindow;

	TSharedPtr<SBox> ImportTypeDisplay;
	TSharedPtr<SHorizontalBox> OpenVDBHeaderButtons;
	TSharedPtr<SBox> InspectorBox;
	this->ChildSlot
	[
		SNew(SBox)
		.MaxDesiredHeight(InArgs._MaxWindowHeight)
		.MaxDesiredWidth(InArgs._MaxWindowWidth)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SAssignNew(ImportTypeDisplay, SBox)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SNew(SBorder)
				.Padding(FMargin(3))
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Import_CurrentFileTitle", "Current Asset: "))
					]
					+ SHorizontalBox::Slot()
					.Padding(5, 0, 0, 0)
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(InArgs._FullPath)
						.ToolTipText(InArgs._FullPath)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SAssignNew(InspectorBox, SBox)
				.MaxDesiredHeight(650.0f)
				.WidthOverride(400.0f)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SNew(SUniformGridPanel)
				.SlotPadding(2)
				+ SUniformGridPanel::Slot(1, 0)
				[
					SAssignNew(ImportButton, SPrimaryButton)
					.Text(LOCTEXT("OpenVDBImportWindow_Import", "Import"))
					.IsEnabled(this, &SOpenVDBImportWindow::CanImport)
					.OnClicked(this, &SOpenVDBImportWindow::OnImport)
				]
				+ SUniformGridPanel::Slot(2, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("OpenVDBImportWindow_Cancel", "Cancel"))
					.ToolTipText(LOCTEXT("OpenVDBImportWindow_Cancel_ToolTip", "Cancels importing this OpenVDB file"))
					.OnClicked(this, &SOpenVDBImportWindow::OnCancel)
				]
			]
		]
	];

	InspectorBox->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2)
		[
			SAssignNew(PackedDataAConfigurator, SOpenVDBPackedDataConfigurator)
			.PackedData(PackedDataA)
			.OpenVDBGridInfo(OpenVDBGridInfo)
			.OpenVDBSupportedTargetFormats(OpenVDBSupportedTargetFormats)
			.PackedDataName(LOCTEXT("OpenVDBImportWindow_PackedDataA", "Packed Data A"))
		]
	);

	SetDefaultGridAssignment();

	ImportTypeDisplay->SetContent(
		SNew(SBorder)
		.Padding(FMargin(3))
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SOpenVDBImportWindow::GetImportTypeDisplayText)
			]
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				IDocumentation::Get()->CreateAnchor(FString("Engine/Content/OpenVDB/ImportWindow"))
			]
			+ SHorizontalBox::Slot()
			.HAlign(HAlign_Right)
			[
				SAssignNew(OpenVDBHeaderButtons, SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(2.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenVDBImportWindow_ResetOptions", "Reset to Default"))
					.OnClicked(this, &SOpenVDBImportWindow::OnResetToDefaultClick)
				]
			]
		]
	);

	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SOpenVDBImportWindow::SetFocusPostConstruct));
}

FReply SOpenVDBImportWindow::OnImport()
{
	bShouldImport = true;
	if (WidgetWindow.IsValid())
	{
		WidgetWindow.Pin()->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FReply SOpenVDBImportWindow::OnCancel()
{
	bShouldImport = false;
	if (WidgetWindow.IsValid())
	{
		WidgetWindow.Pin()->RequestDestroyWindow();
	}
	return FReply::Handled();
}

bool SOpenVDBImportWindow::ShouldImport() const
{
	return bShouldImport;
}

EActiveTimerReturnType SOpenVDBImportWindow::SetFocusPostConstruct(double InCurrentTime, float InDeltaTime)
{
	if (ImportButton.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(ImportButton, EFocusCause::SetDirectly);
	}

	return EActiveTimerReturnType::Stop;
}

bool SOpenVDBImportWindow::CanImport() const
{
	const FUintVector4& GridIndices = PackedDataA->SourceGridIndex;
	const FUintVector4& ComponentIndices = PackedDataA->SourceComponentIndex;
	if ((GridIndices.X != INDEX_NONE && ComponentIndices.X != INDEX_NONE)
		|| (GridIndices.Y != INDEX_NONE && ComponentIndices.Y != INDEX_NONE)
		|| (GridIndices.Z != INDEX_NONE && ComponentIndices.Z != INDEX_NONE)
		|| (GridIndices.W != INDEX_NONE && ComponentIndices.W != INDEX_NONE))
	{
		return true;
	}
	return false;
}

FReply SOpenVDBImportWindow::OnResetToDefaultClick()
{
	SetDefaultGridAssignment();
	return FReply::Handled();
}

FText SOpenVDBImportWindow::GetImportTypeDisplayText() const
{
	return LOCTEXT("OpenVDBImportWindow_ImportType", "Import Static OpenVDB");
}

void SOpenVDBImportWindow::SetDefaultGridAssignment()
{
	check(OpenVDBGridInfo);

	PackedDataA->Format = ESparseVolumePackedDataFormat::Float32;
	PackedDataA->SourceGridIndex = FUintVector4(INDEX_NONE);
	PackedDataA->SourceComponentIndex = FUintVector4(INDEX_NONE);
	PackedDataA->bRescaleInputForUnorm = false;

	const TCHAR* SearchNames[] = { TEXT("density"), TEXT("heat"), TEXT("temperature"), TEXT("motion") };

	uint32 CurrentOutputComponent = 0;
	for (const TCHAR* SearchName : SearchNames)
	{
		for (uint32 InputComponent = 0; InputComponent < 4; ++InputComponent)
		{
			for (const TSharedPtr<FOpenVDBGridInfo>& Grid : *OpenVDBGridInfo)
			{
				if (Grid->Name == SearchName && Grid->ComponentIndex == InputComponent)
				{
					PackedDataA->SourceGridIndex[CurrentOutputComponent] = Grid->Index;
					PackedDataA->SourceComponentIndex[CurrentOutputComponent] = Grid->ComponentIndex;
					++CurrentOutputComponent;
					break;
				}
			}
		}
	}

	PackedDataAConfigurator->RefreshUIFromData();
}

void SOpenVDBComponentPicker::Construct(const FArguments& InArgs)
{
	PackedData = InArgs._PackedData;
	ComponentIndex = InArgs._ComponentIndex;
	OpenVDBGridInfo = InArgs._OpenVDBGridInfo;
	
	check(ComponentIndex < 4);
	const TCHAR* ComponentLabels[] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };

	this->ChildSlot
		[
			SNew(SHorizontalBox)
			
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.FillWidth(1.0f)
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ComponentLabels[ComponentIndex]))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(2.0f)
			[
				SNew(SBox)
				.WidthOverride(300.0f)
				[
					SAssignNew(GridComboBox, SComboBox<TSharedPtr<FOpenVDBGridInfo>>)
					.OptionsSource(OpenVDBGridInfo)
					.OnGenerateWidget_Lambda([](TSharedPtr<FOpenVDBGridInfo> InItem)
					{
						return SNew(STextBlock)
						.Text(GetGridComboBoxItemText(InItem));
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FOpenVDBGridInfo> InItem, ESelectInfo::Type)
					{
						if (InItem)
						{
							PackedData->SourceGridIndex[ComponentIndex] = InItem->Index;
							PackedData->SourceComponentIndex[ComponentIndex] = InItem->ComponentIndex;
						}
						else
						{
							PackedData->SourceGridIndex[ComponentIndex] = INDEX_NONE;
							PackedData->SourceComponentIndex[ComponentIndex] = INDEX_NONE;
						}
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return GetGridComboBoxItemText(GridComboBox->GetSelectedItem());
						})
					]
				]
			]
		];
}

void SOpenVDBComponentPicker::RefreshUIFromData()
{
	for (const TSharedPtr<FOpenVDBGridInfo>& Grid : *OpenVDBGridInfo)
	{
		printf("%i\n", Grid->Index);
		if (Grid->Index == PackedData->SourceGridIndex[ComponentIndex] && Grid->ComponentIndex == PackedData->SourceComponentIndex[ComponentIndex])
		{
			GridComboBox->SetSelectedItem(Grid);
			break;
		}
	}
}

void SOpenVDBPackedDataConfigurator::Construct(const FArguments& InArgs)
{
	PackedData = InArgs._PackedData;
	OpenVDBSupportedTargetFormats = InArgs._OpenVDBSupportedTargetFormats;

	for (uint32 ComponentIndex = 0; ComponentIndex < 4; ++ComponentIndex)
	{
		ComponentPickers[ComponentIndex] =
			SNew(SOpenVDBComponentPicker)
			.PackedData(PackedData)
			.ComponentIndex(ComponentIndex)
			.OpenVDBGridInfo(InArgs._OpenVDBGridInfo);
	}

	this->ChildSlot
		[
			SNew(SVerticalBox)
			
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.FillWidth(1.0f)
				.Padding(2.0f)
				[
					SNew(STextBlock)
					.Text(InArgs._PackedDataName)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(2.0f)
				[
					SNew(SBox)
					.WidthOverride(50.0f)
					[
						SAssignNew(FormatComboBox, SComboBox<TSharedPtr<ESparseVolumePackedDataFormat>>)
						.OptionsSource(OpenVDBSupportedTargetFormats)
						.OnGenerateWidget_Lambda([](TSharedPtr<ESparseVolumePackedDataFormat> InItem)
						{
							return SNew(STextBlock)
							.Text(GetFormatComboBoxItemText(InItem));
						})
						.OnSelectionChanged_Lambda([this](TSharedPtr<ESparseVolumePackedDataFormat> InItem, ESelectInfo::Type)
						{
							PackedData->Format = InItem ? *InItem : ESparseVolumePackedDataFormat::Float32;
						})
						[
							SNew(STextBlock)
							.Text_Lambda([this]()
							{
								return GetFormatComboBoxItemText(FormatComboBox->GetSelectedItem());
							})
						]
					]
				]
				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.AutoWidth()
				.Padding(2.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UnormRescaleCheckBoxLabel", "Unorm Rescale"))
				]
				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.AutoWidth()
				.Padding(2.0f)
				[
					SAssignNew(RescaleUnormCheckBox, SCheckBox)
					.OnCheckStateChanged_Lambda([this](ECheckBoxState CheckBoxState)
					{
							PackedData->bRescaleInputForUnorm = CheckBoxState == ECheckBoxState::Checked;
					})
					.IsEnabled_Lambda([this]()
					{
						return PackedData->Format == ESparseVolumePackedDataFormat::Unorm8;
					})
					.ToolTipText(LOCTEXT("UnormRescaleCheckBoxTooltip", "Rescales input values for unorm formats into the [0-1] range instead of clamping values outside this range."))
					.IsChecked(false)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2)
			[
				SNew(SBorder)
				.Padding(FMargin(3))
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(2)
					[
						ComponentPickers[0]->AsShared()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(2)
					[
						ComponentPickers[1]->AsShared()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(2)
					[
						ComponentPickers[2]->AsShared()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(2)
					[
						ComponentPickers[3]->AsShared()
					]
				]
			]
		];
}

void SOpenVDBPackedDataConfigurator::RefreshUIFromData()
{
	for (auto& Format : *OpenVDBSupportedTargetFormats)
	{
		if (*Format == PackedData->Format)
		{
			FormatComboBox->SetSelectedItem(Format);
			break;
		}
	}
	for (uint32 i = 0; i < 4; ++i)
	{
		ComponentPickers[i]->RefreshUIFromData();
	}
	RescaleUnormCheckBox->SetIsChecked(PackedData->bRescaleInputForUnorm);
}

#undef LOCTEXT_NAMESPACE
