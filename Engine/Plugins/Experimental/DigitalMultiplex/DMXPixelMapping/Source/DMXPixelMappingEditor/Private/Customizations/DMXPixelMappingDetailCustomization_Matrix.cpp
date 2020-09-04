// Copyright Epic Games, Inc. All Rights Reserved.

#include "Customizations/DMXPixelMappingDetailCustomization_Matrix.h"
#include "Components/DMXPixelMappingMatrixComponent.h"
#include "Components/DMXPixelMappingMatrixPixelComponent.h"
#include "DMXEditorStyle.h"
#include "Toolkits/DMXPixelMappingToolkit.h"
#include "DMXPixelMapping.h"
#include "Library/DMXEntityFixturePatch.h"
#include "Components/DMXPixelMappingRootComponent.h"
#include "DMXPixelMappingEditorUtils.h"
#include "DMXPixelMappingTypes.h"

#include "DetailWidgetRow.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "Misc/Attribute.h"
#include "Layout/Visibility.h"

#define LOCTEXT_NAMESPACE "DMXPixelMappingDetailCustomization_Matrix"

void FDMXPixelMappingDetailCustomization_Matrix::CustomizeDetails(IDetailLayoutBuilder& InDetailLayout)
{
	DetailLayout = &InDetailLayout;

	// Get editing UObject
	TArray<TWeakObjectPtr<UObject>> OuterObjects;
	DetailLayout->GetObjectsBeingCustomized(OuterObjects);
	if (OuterObjects.Num() == 1)
	{
		MatrixComponent = Cast<UDMXPixelMappingMatrixComponent>(OuterObjects[0]);

		// Get editing categories
		IDetailCategoryBuilder& OutputSettingsCategory = DetailLayout->EditCategory("Output Settings", FText::GetEmpty(), ECategoryPriority::Important);

		// Add Fixture Patch change delegates
		TSharedRef<IPropertyHandle> FixturePatchMatrixRefPropertyHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, FixturePatchMatrixRef));
		FSimpleDelegate OnFixturePatchMatrixChangedDelegate = FSimpleDelegate::CreateSP(this, &FDMXPixelMappingDetailCustomization_Matrix::OnFixturePatchMatrixChanged);
		FixturePatchMatrixRefPropertyHandle->SetOnChildPropertyValueChanged(OnFixturePatchMatrixChangedDelegate);
		FixturePatchMatrixRefPropertyHandle->SetOnPropertyValueChanged(OnFixturePatchMatrixChangedDelegate);

		// Add color mode property
		ColorModePropertyHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, ColorMode), UDMXPixelMappingMatrixComponent::StaticClass());
		OutputSettingsCategory.AddProperty(ColorModePropertyHandle);

		// Register attributes
		TSharedPtr<FDMXPixelGroupAttribute> AttributeR = MakeShared<FDMXPixelGroupAttribute>();
		AttributeR->Handle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeR));
		AttributeR->ExposeHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeRExpose));
		AttributeR->InvertHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeRInvert));

		TSharedPtr<FDMXPixelGroupAttribute> AttributeG = MakeShared<FDMXPixelGroupAttribute>();
		AttributeG->Handle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeG));
		AttributeG->ExposeHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeGExpose));
		AttributeG->InvertHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeGInvert));

		TSharedPtr<FDMXPixelGroupAttribute> AttributeB = MakeShared<FDMXPixelGroupAttribute>();
		AttributeB->Handle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeB));
		AttributeB->ExposeHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeBExpose));
		AttributeB->InvertHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, AttributeBInvert));

		RGBAttributes.Add(AttributeR);
		RGBAttributes.Add(AttributeG);
		RGBAttributes.Add(AttributeB);

		// Register Monochrome attribute
		TSharedPtr<FDMXPixelGroupAttribute> MonochromeAttribute = MakeShared<FDMXPixelGroupAttribute>();
		MonochromeAttribute->Handle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, MonochromeIntensity));
		MonochromeAttribute->ExposeHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, MonochromeExpose));
		MonochromeAttribute->InvertHandle = DetailLayout->GetProperty(GET_MEMBER_NAME_CHECKED(UDMXPixelMappingMatrixComponent, MonochromeInvert));
		MonochromeAttributes.Add(MonochromeAttribute);

		// Generate all RGB Expose and Invert rows
		OutputSettingsCategory.AddCustomRow(FText::GetEmpty())
			.Visibility(TAttribute<EVisibility>(this, &FDMXPixelMappingDetailCustomization_Matrix::GetRGBAttributesVisibility))
			.NameContent()
			[
				SNew(STextBlock).Text(LOCTEXT("ColorSample", "Color Sample"))
			]
		.ValueContent()
			[
				SAssignNew(ExposeAndInvertListView, SListView<TSharedPtr<FDMXPixelGroupAttribute>>)
				.ListItemsSource(&RGBAttributes)
			.OnGenerateRow(this, &FDMXPixelMappingDetailCustomization_Matrix::GenerateExposeAndInvertRow)
			];

		// Update RGB attributes
		for (TSharedPtr<FDMXPixelGroupAttribute>& Attribute : RGBAttributes)
		{
			DetailLayout->HideProperty(Attribute->ExposeHandle);
			DetailLayout->HideProperty(Attribute->InvertHandle);

			OutputSettingsCategory
				.AddProperty(Attribute->Handle)
				.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateSP(this, &FDMXPixelMappingDetailCustomization_Matrix::GetRGBAttributeRowVisibilty, Attribute.Get())));
		}


		// Generate all Monochrome Expose and Invert rows
		OutputSettingsCategory.AddCustomRow(FText::GetEmpty())
			.Visibility(TAttribute<EVisibility>(this, &FDMXPixelMappingDetailCustomization_Matrix::GetMonochromeAttributesVisibility))
			.NameContent()
			[
				SNew(STextBlock).Text(LOCTEXT("ColorSample", "Color Sample"))
			]
		.ValueContent()
			[
				SAssignNew(ExposeAndInvertListView, SListView<TSharedPtr<FDMXPixelGroupAttribute>>)
				.ListItemsSource(&MonochromeAttributes)
			.OnGenerateRow(this, &FDMXPixelMappingDetailCustomization_Matrix::GenerateExposeAndInvertRow)
			];

		// Update Monochrome attributes
		for (TSharedPtr<FDMXPixelGroupAttribute>& Attribute : MonochromeAttributes)
		{
			DetailLayout->HideProperty(Attribute->ExposeHandle);
			DetailLayout->HideProperty(Attribute->InvertHandle);

			OutputSettingsCategory
				.AddProperty(Attribute->Handle)
				.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateSP(this, &FDMXPixelMappingDetailCustomization_Matrix::GetMonochromeRowVisibilty, Attribute.Get())));
		}
	}
}

void FDMXPixelMappingDetailCustomization_Matrix::OnFixturePatchMatrixChanged()
{
	TSharedPtr<FDMXPixelMappingToolkit> Toolkit = ToolkitWeakPtr.Pin();
	check(Toolkit.IsValid());

	UDMXPixelMapping* PixelMapping = Toolkit->GetDMXPixelMapping();
	check(PixelMapping != nullptr);

	UDMXPixelMappingMatrixComponent* MatrixComponentPtr = MatrixComponent.Get();
	check(MatrixComponentPtr != nullptr);

	// Delete old one
	Toolkit->DeleteMatrixPixels(MatrixComponentPtr);
	Toolkit->CreateMatrixPixels(MatrixComponentPtr);
}

EVisibility FDMXPixelMappingDetailCustomization_Matrix::GetRGBAttributeRowVisibilty(FDMXPixelGroupAttribute* Attribute) const
{
	bool bIsVisible = false;

	// 1. Check if current attribute is sampling now
	Attribute->ExposeHandle->GetValue(bIsVisible);

	// 2. Check if current color mode is RGB
	if (MatrixComponent->ColorMode != EDMXColorMode::CM_RGB)
	{
		bIsVisible = false;
	}

	return bIsVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FDMXPixelMappingDetailCustomization_Matrix::GetRGBAttributesVisibility() const
{
	bool bIsVisible = false;

	if (MatrixComponent->ColorMode == EDMXColorMode::CM_RGB)
	{
		bIsVisible = true;
	}
	
	return bIsVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FDMXPixelMappingDetailCustomization_Matrix::GetMonochromeRowVisibilty(FDMXPixelGroupAttribute* Attribute) const
{
	bool bIsVisible = false;

	// 1. Check if current attribute is sampling now
	Attribute->ExposeHandle->GetValue(bIsVisible);

	// 2. Check if current color mode is Monochrome
	if (MatrixComponent->ColorMode != EDMXColorMode::CM_Monochrome)
	{
		bIsVisible = false;
	}


	return bIsVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FDMXPixelMappingDetailCustomization_Matrix::GetMonochromeAttributesVisibility() const
{
	return (GetRGBAttributesVisibility() == EVisibility::Visible) ? EVisibility::Collapsed : EVisibility::Visible;
}

TSharedRef<ITableRow> FDMXPixelMappingDetailCustomization_Matrix::GenerateExposeAndInvertRow(TSharedPtr<FDMXPixelGroupAttribute> InAtribute, const TSharedRef<STableViewBase>& OwnerTable)
{
	if (!InAtribute.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FString>>, OwnerTable);
	}

	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
		.Padding(2.0f)
		.Style(FEditorStyle::Get(), "UMGEditor.PaletteItem")
		.ShowSelection(false)
		[
			SNew(SBox)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				.HAlign(HAlign_Left)
				[
					InAtribute->ExposeHandle->CreatePropertyNameWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				.HAlign(HAlign_Left)
				[
					InAtribute->ExposeHandle->CreatePropertyValueWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				.HAlign(HAlign_Left)
				[
					InAtribute->InvertHandle->CreatePropertyNameWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				.HAlign(HAlign_Left)
				[
					InAtribute->InvertHandle->CreatePropertyValueWidget()
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
