// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "BrushSettingsCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "IDetailPropertyRow.h"
#include "IDetailChildrenBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "PropertyRestriction.h"
#include "Engine/Texture2D.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "ScopedTransaction.h"
#include "MeshPaintHelpers.h"
#include "MeshPaintColorBrush.h"
#include "MeshPaintHelpers.h"
#include "InteractiveToolManager.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "MeshPaintMode.h"
#include "Components/SkeletalMeshComponent.h"

#define LOCTEXT_NAMESPACE "MeshPaintCustomization"

TSharedRef<SHorizontalBox> CreateColorChannelWidget(TSharedRef<IPropertyHandle> ChannelProperty)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			ChannelProperty->CreatePropertyValueWidget()
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			ChannelProperty->CreatePropertyNameWidget()
		];
}


TSharedRef<IDetailCustomization> FVertexPaintingSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FVertexPaintingSettingsCustomization);
}

void FVertexPaintingSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{	
	IDetailCategoryBuilder& VertexCategory = DetailLayout.EditCategory(TEXT("VertexPainting"));

	VertexCategory.AddCustomRow(NSLOCTEXT("VertexPaintSettings", "InstanceColorSize", "Instance Color Size"))
		.WholeRowContent()
		[
			SNew(STextBlock)
			.Text_Lambda([]() -> FText { return FText::Format(FTextFormat::FromString(TEXT("Instance Color Size: {0} KB")), UMeshPaintMode::GetMeshPaintMode()->GetCachedVertexDataSize() / 1024.f); })
		];

	/** Creates a custom widget row containing all color channel flags */	
	TSharedRef<IPropertyHandle> PaintColor = DetailLayout.GetProperty("PaintColor", UMeshVertexPaintingToolProperties::StaticClass());
	PaintColor->MarkHiddenByCustomization();
	TSharedRef<IPropertyHandle> EraseColor = DetailLayout.GetProperty("EraseColor", UMeshVertexPaintingToolProperties::StaticClass());
	EraseColor->MarkHiddenByCustomization();

	// Customize paint color with a swap button
	{
		TSharedPtr<SWidget> NameWidget;
		TSharedPtr<SWidget> ValueWidget;
		
		IDetailPropertyRow& PaintColorProp = VertexCategory.AddProperty(PaintColor);
		PaintColorProp.GetDefaultWidgets(NameWidget, ValueWidget, false);
		FDetailWidgetRow& Row = PaintColorProp.CustomWidget(true);
		Row.NameContent()
		[
			NameWidget.ToSharedRef()
		];

		Row.ValueContent()
		.MinDesiredWidth(250)
		.MaxDesiredWidth(0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0)
			.HAlign(HAlign_Left)
			[
				SNew(SBox)
				.WidthOverride(250.f)
				[
					ValueWidget.ToSharedRef()
				]
			]
			+ SHorizontalBox::Slot()
			.HAlign(HAlign_Center)
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FEditorStyle::Get(), "HoverHintOnly")
				.ToolTipText(NSLOCTEXT("VertexPaintSettings", "SwapColors", "Swap Paint and Erase Colors"))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.OnClicked(this, &FVertexPaintingSettingsCustomization::OnSwapColorsClicked, PaintColor, EraseColor)
				.ContentPadding(0)
				[
					SNew(SImage).Image(FEditorStyle::GetBrush("MeshPaint.Swap"))
				]
			]
		];
	}

	{
		IDetailPropertyRow& EraseColorProp = VertexCategory.AddProperty(EraseColor);

		TSharedPtr<SWidget> NameWidget;
		TSharedPtr<SWidget> ValueWidget;
		
		FDetailWidgetRow& Row = EraseColorProp.CustomWidget(true);
		Row.ValueContent().MinDesiredWidth(250 - 16.f);
		EraseColorProp.GetDefaultWidgets(NameWidget, ValueWidget, Row, false);
	}

}

void FVertexPaintingSettingsCustomization::OnTextureWeightTypeChanged(TSharedRef<IPropertyHandle> WeightTypeProperty, TSharedRef<IPropertyHandle> PaintWeightProperty, TSharedRef<IPropertyHandle> EraseWeightProperty)
{
	UEnum* ImportTypeEnum = StaticEnum<ETexturePaintWeightIndex>();
	uint8 EnumValue = 0;
	WeightTypeProperty->GetValue(EnumValue);	

	BlendPaintEnumRestriction->RemoveAll();
	for (uint8 EnumIndex = 0; EnumIndex < (ImportTypeEnum->GetMaxEnumValue() + 1); ++EnumIndex)
	{
		if ((EnumIndex + 1) > EnumValue)
		{
			FString EnumName = ImportTypeEnum->GetNameByValue(EnumIndex).ToString();
			EnumName.RemoveFromStart("ETexturePaintIndex::");
			BlendPaintEnumRestriction->AddDisabledValue(EnumName);
		}
	}

	uint8 Value = 0;
	PaintWeightProperty->GetValue(Value);
	Value = FMath::Clamp<uint8>(Value, 0, EnumValue - 1);
	PaintWeightProperty->SetValue(Value);

	EraseWeightProperty->GetValue(Value);
	Value = FMath::Clamp<uint8>(Value, 0, EnumValue - 1);
	EraseWeightProperty->SetValue(Value);
}

FReply FVertexPaintingSettingsCustomization::OnSwapColorsClicked(TSharedRef<IPropertyHandle> PaintColor, TSharedRef<IPropertyHandle> EraseColor)
{
	FScopedTransaction Transaction(NSLOCTEXT("VertexPaintSettings", "SwapColorsTransation", "Swap paint and erase colors"));

	UMeshVertexPaintingToolProperties* Settings = UMeshPaintMode::GetVertexToolProperties();
	if (Settings)
	{
		Settings->Modify();
		
		FLinearColor TempPaintColor = Settings->PaintColor;
		Settings->PaintColor = Settings->EraseColor;
		Settings->EraseColor = TempPaintColor;
		PaintColor->NotifyPostChange(EPropertyChangeType::ValueSet);
		EraseColor->NotifyPostChange(EPropertyChangeType::ValueSet);
	}
	return FReply::Handled();
}



TSharedRef<IDetailCustomization> FColorPaintingSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FColorPaintingSettingsCustomization);
}

void FColorPaintingSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	FVertexPaintingSettingsCustomization::CustomizeDetails(DetailLayout);

	IDetailCategoryBuilder& ColorCategory = DetailLayout.EditCategory(TEXT("ColorPainting"));

	/** Creates a custom widget row containing all color channel flags */
	TSharedRef<IPropertyHandle> RedChannel = DetailLayout.GetProperty("bWriteRed", UMeshColorPaintingToolProperties::StaticClass());
	RedChannel->MarkHiddenByCustomization();
	TSharedRef<IPropertyHandle> GreenChannel = DetailLayout.GetProperty("bWriteGreen", UMeshColorPaintingToolProperties::StaticClass());
	GreenChannel->MarkHiddenByCustomization();
	TSharedRef<IPropertyHandle> BlueChannel = DetailLayout.GetProperty("bWriteBlue", UMeshColorPaintingToolProperties::StaticClass());
	BlueChannel->MarkHiddenByCustomization();
	TSharedRef<IPropertyHandle> AlphaChannel = DetailLayout.GetProperty("bWriteAlpha", UMeshColorPaintingToolProperties::StaticClass());
	AlphaChannel->MarkHiddenByCustomization();
	TArray<TSharedRef<IPropertyHandle>> Channels = { RedChannel, GreenChannel, BlueChannel, AlphaChannel };
	TSharedPtr<SHorizontalBox> ChannelsWidget;

	ColorCategory.AddCustomRow(NSLOCTEXT("VertexPaintSettings", "ChannelLabel", "Channels"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("VertexPaintSettings", "ChannelsLabel", "Channels"))
			.ToolTipText(NSLOCTEXT("VertexPaintSettings", "ChannelsToolTip", "Colors Channels which should be influenced during Painting."))
		]
		.ValueContent()
		.MaxDesiredWidth(250.0f)
		[
			SAssignNew(ChannelsWidget, SHorizontalBox)
		];

	for (TSharedRef<IPropertyHandle> Channel : Channels)
	{
		ChannelsWidget->AddSlot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				CreateColorChannelWidget(Channel)
			];
	}

	static FText RestrictReason = NSLOCTEXT("VertexPaintSettings", "TextureIndexRestriction", "Unable to paint this Texture, change Texture Weight Type");
	BlendPaintEnumRestriction = MakeShareable(new FPropertyRestriction(RestrictReason));

	/** Add custom row for painting on specific LOD level with callbacks to the painter to update the data */
	TSharedRef<IPropertyHandle> LODPaintingEnabled = DetailLayout.GetProperty("bPaintOnSpecificLOD", UMeshColorPaintingToolProperties::StaticClass());
	LODPaintingEnabled->MarkHiddenByCustomization();
	TSharedRef<IPropertyHandle> LODPaintingIndex = DetailLayout.GetProperty("LODIndex", UMeshColorPaintingToolProperties::StaticClass());
	LODPaintingIndex->MarkHiddenByCustomization();
	TSharedPtr<SWidget> LODIndexWidget = LODPaintingIndex->CreatePropertyValueWidget();

	ColorCategory.AddCustomRow(NSLOCTEXT("LODPainting", "LODPaintingLabel", "LOD Model Painting"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("LODPainting", "LODPaintingSetupLabel", "LOD Model Painting"))
			.ToolTipText(NSLOCTEXT("LODPainting", "LODPaintingSetupToolTip", "Allows for Painting Vertex Colors on Specific LOD Models."))
		]
	.ValueContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
		.Padding(0.0f, 0.0f, 4.0f, 0.0f)
		.AutoWidth()
		[
			SNew(SCheckBox)
				.IsChecked_Lambda([=]() -> ECheckBoxState { return (UMeshPaintMode::GetColorToolProperties()->bPaintOnSpecificLOD ? ECheckBoxState::Checked : ECheckBoxState::Unchecked); })
				.OnCheckStateChanged(FOnCheckStateChanged::CreateLambda([=](ECheckBoxState State) { 
					if (UMeshColorPaintingTool* ColorBrush = Cast<UMeshColorPaintingTool>(UMeshPaintMode::GetMeshPaintMode()->GetToolManager()->GetActiveTool(EToolSide::Left)))
					{
						ColorBrush->LODPaintStateChanged(State == ECheckBoxState::Checked);
					}
				}))
		]
	+ SHorizontalBox::Slot()
		.Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SNumericEntryBox<int32>)
			.IsEnabled_Lambda([=]() -> bool { return UMeshPaintMode::GetColorToolProperties()->bPaintOnSpecificLOD;  })
			.AllowSpin(true)
			.Value_Lambda([=]() -> int32 { return UMeshPaintMode::GetColorToolProperties()->LODIndex; })
			.MinValue(0)
			.MaxValue_Lambda([=]() -> int32 { 
					if (UMeshColorPaintingTool* ColorBrush = Cast<UMeshColorPaintingTool>(UMeshPaintMode::GetMeshPaintMode()->GetToolManager()->GetActiveTool(EToolSide::Left)))
					{
						return ColorBrush->GetMaxLODIndexToPaint();
					}
					return INT_MAX;
				})
			.MaxSliderValue_Lambda([=]() -> int32 { 
					if (UMeshColorPaintingTool* ColorBrush = Cast<UMeshColorPaintingTool>(UMeshPaintMode::GetMeshPaintMode()->GetToolManager()->GetActiveTool(EToolSide::Left)))
					{
						return ColorBrush->GetMaxLODIndexToPaint();
					}
					return INT_MAX;
				})
			.OnValueChanged(SNumericEntryBox<int32>::FOnValueChanged::CreateLambda([=](int32 Value) { UMeshPaintMode::GetColorToolProperties()->LODIndex = Value; }))
			.OnValueCommitted(SNumericEntryBox<int32>::FOnValueCommitted::CreateLambda([=](int32 Value, ETextCommit::Type CommitType) { 
					UMeshPaintMode::GetColorToolProperties()->LODIndex = Value; 
					if (UMeshColorPaintingTool* ColorBrush = Cast<UMeshColorPaintingTool>(UMeshPaintMode::GetMeshPaintMode()->GetToolManager()->GetActiveTool(EToolSide::Left)))
					{
						ColorBrush->PaintLODChanged();
					}
				}))
		]
		];

	ColorCategory.AddCustomRow(NSLOCTEXT("LODPainting", "LODPaintingLabel", "LOD Model Painting"))
		.WholeRowContent()
		[
			SNew(SBorder)
			.Visibility_Lambda([this]() -> EVisibility
			{
				if (UMeshColorPaintingToolProperties* ColorProperties = UMeshPaintMode::GetColorToolProperties())
				{
					return ColorProperties->bPaintOnSpecificLOD ? EVisibility::Collapsed : EVisibility::Visible;
				}
				return EVisibility::Collapsed;
			})
		.Padding(FMargin(4.0f))
		.BorderImage(FEditorStyle::GetBrush("SettingsEditor.CheckoutWarningBorder"))
		.BorderBackgroundColor(FColor(166, 137, 0))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(IDetailLayoutBuilder::GetDetailFont())

		.Text_Lambda([this]() -> FText
		{
			static const FText SkelMeshNotificationText = LOCTEXT("SkelMeshAssetPaintInfo", "Paint is propagated to Skeletal Mesh Asset(s)");
			static const FText StaticMeshNotificationText = LOCTEXT("StaticMeshAssetPaintInfo", "Paint is applied to all LODs");

			const bool bSkelMeshText = UMeshPaintMode::GetMeshPaintMode()->GetSelectedComponents<USkeletalMeshComponent>().Num() > 0;
			const bool bLODPaintText = UMeshPaintMode::GetColorToolProperties() ?  !UMeshPaintMode::GetColorToolProperties()->bPaintOnSpecificLOD : false;
			return FText::Format(FTextFormat::FromString(TEXT("{0}{1}{2}")), bSkelMeshText ? SkelMeshNotificationText : FText::GetEmpty(), bSkelMeshText && bLODPaintText ? FText::FromString(TEXT("\n")) : FText::GetEmpty(), bLODPaintText ? StaticMeshNotificationText : FText::GetEmpty());
		})
		]
		];
}

TSharedRef<IDetailCustomization> FWeightPaintingSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FWeightPaintingSettingsCustomization);
}

void FWeightPaintingSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	FVertexPaintingSettingsCustomization::CustomizeDetails(DetailLayout);
}

#undef LOCTEXT_NAMESPACE