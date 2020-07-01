// Copyright Epic Games, Inc. All Rights Reserved.

#include "RuntimeVirtualTextureDetailsCustomization.h"

#include "AssetToolsModule.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/Texture2D.h"
#include "Factories/Texture2dFactoryNew.h"
#include "RuntimeVirtualTextureBuildMinMaxHeight.h"
#include "RuntimeVirtualTextureBuildStreamingMips.h"
#include "RuntimeVirtualTextureSetBounds.h"
#include "ScopedTransaction.h"
#include "SResetToDefaultMenu.h"
#include "VirtualTextureBuilderFactory.h"
#include "VT/RuntimeVirtualTexture.h"
#include "VT/VirtualTextureBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"

#define LOCTEXT_NAMESPACE "VirtualTexturingEditorModule"

FRuntimeVirtualTextureDetailsCustomization::FRuntimeVirtualTextureDetailsCustomization()
	: VirtualTexture(nullptr)
{
}

TSharedRef<IDetailCustomization> FRuntimeVirtualTextureDetailsCustomization::MakeInstance()
{
	return MakeShareable(new FRuntimeVirtualTextureDetailsCustomization);
}

namespace
{
	// Helper for adding text containing real values to the properties that are edited as power (or multiple) of 2
	void AddTextToProperty(IDetailLayoutBuilder& DetailBuilder, IDetailCategoryBuilder& CategoryBuilder, FName const& PropertyName, TSharedPtr<STextBlock>& TextBlock)
	{
		TSharedPtr<IPropertyHandle> PropertyHandle = DetailBuilder.GetProperty(PropertyName);
		DetailBuilder.HideProperty(PropertyHandle);

		TSharedPtr<SResetToDefaultMenu> ResetToDefaultMenu;

		CategoryBuilder.AddCustomRow(PropertyHandle->GetPropertyDisplayName())
		.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(200.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.Padding(4.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)

				+ SWrapBox::Slot()
				.Padding(FMargin(0.0f, 2.0f, 2.0f, 0.0f))
				[
					SAssignNew(TextBlock, STextBlock)
				]
			]

			+ SHorizontalBox::Slot()
			[
				PropertyHandle->CreatePropertyValueWidget()
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f)
			[
				// Would be better to use SResetToDefaultPropertyEditor here but that is private in the PropertyEditor lib
				SAssignNew(ResetToDefaultMenu, SResetToDefaultMenu)
			]
		];

		ResetToDefaultMenu->AddProperty(PropertyHandle.ToSharedRef());
	}
}

void FRuntimeVirtualTextureDetailsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Get and store the linked URuntimeVirtualTexture
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);
	if (ObjectsBeingCustomized.Num() > 1)
	{
		return;
	}
	VirtualTexture = Cast<URuntimeVirtualTexture>(ObjectsBeingCustomized[0].Get());
	if (VirtualTexture == nullptr)
	{
		return;
	}

	// Add size helpers
	IDetailCategoryBuilder& SizeCategory = DetailBuilder.EditCategory("Size", FText::GetEmpty());
	AddTextToProperty(DetailBuilder, SizeCategory, "TileCount", TileCountText);
	AddTextToProperty(DetailBuilder, SizeCategory, "TileSize", TileSizeText);
	AddTextToProperty(DetailBuilder, SizeCategory, "TileBorderSize", TileBorderSizeText);

	// Add details block
	IDetailCategoryBuilder& DetailsCategory = DetailBuilder.EditCategory("Details", FText::GetEmpty(), ECategoryPriority::Important);
	static const FText RowText = LOCTEXT("Category_Details", "Details");
	DetailsCategory.AddCustomRow(RowText)
	.WholeRowContent()
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.VAlign(VAlign_Center)
		.Padding(4.0f)
		[
			SAssignNew(SizeText, STextBlock)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.VAlign(VAlign_Center)
		.Padding(4.0f)
		[
			SAssignNew(PageTableTextureMemoryText, STextBlock)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.VAlign(VAlign_Center)
		.Padding(4.0f)
		[
			SAssignNew(PhysicalTextureMemoryText, STextBlock)
		]
	];

	// Add refresh callback for all properties 
	DetailBuilder.GetProperty(FName(TEXT("TileCount")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));
	DetailBuilder.GetProperty(FName(TEXT("TileSize")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));
	DetailBuilder.GetProperty(FName(TEXT("TileBorderSize")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));
	DetailBuilder.GetProperty(FName(TEXT("MaterialType")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));
	DetailBuilder.GetProperty(FName(TEXT("bCompressTextures")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));
	DetailBuilder.GetProperty(FName(TEXT("RemoveLowMips")))->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FRuntimeVirtualTextureDetailsCustomization::RefreshDetails));

	// Initialize text blocks
	RefreshDetails();
}

void FRuntimeVirtualTextureDetailsCustomization::RefreshDetails()
{
	FNumberFormattingOptions SizeOptions;
	SizeOptions.UseGrouping = false;
	SizeOptions.MaximumFractionalDigits = 0;

 	TileCountText->SetText(FText::Format(LOCTEXT("Details_Number", "{0}"), FText::AsNumber(VirtualTexture->GetTileCount(), &SizeOptions)));
 	TileSizeText->SetText(FText::Format(LOCTEXT("Details_Number", "{0}"), FText::AsNumber(VirtualTexture->GetTileSize(), &SizeOptions)));
 	TileBorderSizeText->SetText(FText::Format(LOCTEXT("Details_Number", "{0}"), FText::AsNumber(VirtualTexture->GetTileBorderSize(), &SizeOptions)));

	SizeText->SetText(FText::Format(LOCTEXT("Details_Size", "Virtual Texture Size: {0}"), FText::AsNumber(VirtualTexture->GetSize(), &SizeOptions)));
	PageTableTextureMemoryText->SetText(FText::Format(LOCTEXT("Details_PageTableMemory", "Page Table Texture Memory (estimated): {0} KiB"), FText::AsNumber(VirtualTexture->GetEstimatedPageTableTextureMemoryKb(), &SizeOptions)));
	PhysicalTextureMemoryText->SetText(FText::Format(LOCTEXT("Details_PhysicalMemory", "Physical Texture Memory (estimated): {0} KiB"), FText::AsNumber(VirtualTexture->GetEstimatedPhysicalTextureMemoryKb(), &SizeOptions)));
}


FRuntimeVirtualTextureComponentDetailsCustomization::FRuntimeVirtualTextureComponentDetailsCustomization()
{
}

TSharedRef<IDetailCustomization> FRuntimeVirtualTextureComponentDetailsCustomization::MakeInstance()
{
	return MakeShareable(new FRuntimeVirtualTextureComponentDetailsCustomization);
}

bool FRuntimeVirtualTextureComponentDetailsCustomization::IsMinMaxTextureEnabled() const
{
	return RuntimeVirtualTextureComponent->IsMinMaxTextureEnabled();
}

void FRuntimeVirtualTextureComponentDetailsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Get and store the linked URuntimeVirtualTextureComponent.
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);
	if (ObjectsBeingCustomized.Num() > 1)
	{
		return;
	}
	RuntimeVirtualTextureComponent = Cast<URuntimeVirtualTextureComponent>(ObjectsBeingCustomized[0].Get());
	if (RuntimeVirtualTextureComponent == nullptr)
	{
		return;
	}

	// TransformFromBounds button.
	IDetailCategoryBuilder& BoundsCategory = DetailBuilder.EditCategory("TransformFromBounds", FText::GetEmpty(), ECategoryPriority::Important);

	BoundsCategory
	.AddCustomRow(LOCTEXT("Button_SetBounds", "Set Bounds"))
	.NameContent()
	[
		SNew(STextBlock)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text(LOCTEXT("Button_SetBounds", "Set Bounds"))
		.ToolTipText(LOCTEXT("Button_SetBounds_Tooltip", "Set the rotation to match the Bounds Align Actor and expand bounds to include all primitives that write to this virtual texture."))
	]
	.ValueContent()
	.MinDesiredWidth(125.f)
	[
		SNew(SButton)
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		.ContentPadding(2)
		.Text(LOCTEXT("Button_SetBounds", "Set Bounds"))
		.OnClicked(this, &FRuntimeVirtualTextureComponentDetailsCustomization::SetBounds)
	];

	// VirtualTextureBuild buttons.
	IDetailCategoryBuilder& VirtualTextureCategory = DetailBuilder.EditCategory("VirtualTextureBuild", FText::GetEmpty());
	
	VirtualTextureCategory
	.AddCustomRow(LOCTEXT("Button_BuildStreamingMips", "Build Streaming Mips"), true)
	.NameContent()
	[
		SNew(STextBlock)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text(LOCTEXT("Button_BuildStreamingMips", "Build Streaming Mips"))
		.ToolTipText(LOCTEXT("Button_Build_Tooltip", "Build the low mips as streaming virtual texture data"))
	]
	.ValueContent()
	.MaxDesiredWidth(125.f)
	[
		SNew(SButton)
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		.ContentPadding(2)
		.Text(LOCTEXT("Button_Build", "Build"))
		.OnClicked(this, &FRuntimeVirtualTextureComponentDetailsCustomization::BuildStreamedMips)
	];

	VirtualTextureCategory
	.AddCustomRow(LOCTEXT("Button_BuildDebugStreamingMips", "Build Debug Streaming Mips"), true)
	.NameContent()
	[
		SNew(STextBlock)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text(LOCTEXT("Button_BuildDebugStreamingMips", "Build Debug Streaming Mips"))
		.ToolTipText(LOCTEXT("Button_BuildDebug_Tooltip", "Build the low mips with debug data"))
	]
	.ValueContent()
	.MaxDesiredWidth(125.f)
	[
		SNew(SButton)
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		.ContentPadding(2)
		.Text(LOCTEXT("Button_Build", "Build"))
		.OnClicked(this, &FRuntimeVirtualTextureComponentDetailsCustomization::BuildLowMipsDebug)
	];

	VirtualTextureCategory
	.AddCustomRow(LOCTEXT("Button_BuildMinMaxTexture", "Build MinMax Texture"), true)
	.NameContent()
	[
		SNew(STextBlock)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text(LOCTEXT("Button_BuildMinMaxTexture", "Build MinMax Texture"))
		.ToolTipText(LOCTEXT("Button_BuildMinMaxTexture_Tooltip", "Build the min/max height texture"))
	]
	.ValueContent()
	.MaxDesiredWidth(125.f)
	[
		SNew(SButton)
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		.ContentPadding(2)
		.Text(LOCTEXT("Button_Build", "Build"))
		.OnClicked(this, &FRuntimeVirtualTextureComponentDetailsCustomization::BuildMinMaxTexture)
		.IsEnabled(this, &FRuntimeVirtualTextureComponentDetailsCustomization::IsMinMaxTextureEnabled)
	];
}

FReply FRuntimeVirtualTextureComponentDetailsCustomization::SetBounds()
{
	const FScopedTransaction Transaction(LOCTEXT("Transaction_SetBounds", "Set RuntimeVirtualTextureComponent Bounds"));
	RuntimeVirtualTexture::SetBounds(RuntimeVirtualTextureComponent);
	return FReply::Handled();
}

FReply FRuntimeVirtualTextureComponentDetailsCustomization::BuildStreamedMips()
{
	return BuildStreamedMipsInternal(false);
}

FReply FRuntimeVirtualTextureComponentDetailsCustomization::BuildLowMipsDebug()
{
	return BuildStreamedMipsInternal(true);
}

FReply FRuntimeVirtualTextureComponentDetailsCustomization::BuildStreamedMipsInternal(bool bDebug)
{
	// Create a new asset if none is already bound
	UVirtualTextureBuilder* CreatedTexture = nullptr;
	if (RuntimeVirtualTextureComponent->GetStreamingTexture() == nullptr)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

		const FString DefaultPath = FPackageName::GetLongPackagePath(RuntimeVirtualTextureComponent->GetVirtualTexture()->GetPathName());
		const FString DefaultName = FPackageName::GetShortName(RuntimeVirtualTextureComponent->GetVirtualTexture()->GetName() + TEXT("_SVT"));

		UFactory* Factory = NewObject<UVirtualTextureBuilderFactory>();
		UObject* Object = AssetToolsModule.Get().CreateAssetWithDialog(DefaultName, DefaultPath, UVirtualTextureBuilder::StaticClass(), Factory);
		CreatedTexture = Cast<UVirtualTextureBuilder>(Object);
	}

	// Build the texture contents
	bool bOK = false;
	if (RuntimeVirtualTextureComponent->GetStreamingTexture() != nullptr || CreatedTexture != nullptr)
	{
		const FScopedTransaction Transaction(LOCTEXT("Transaction_BuildDebugStreamingMips", "Build Streaming Mips"));

		if (CreatedTexture != nullptr)
		{
			RuntimeVirtualTextureComponent->Modify();
			RuntimeVirtualTextureComponent->SetStreamingTexture(CreatedTexture);
		}

		RuntimeVirtualTextureComponent->GetStreamingTexture()->Modify();

		const ERuntimeVirtualTextureDebugType DebugType = bDebug ? ERuntimeVirtualTextureDebugType::Debug : ERuntimeVirtualTextureDebugType::None;
		if (RuntimeVirtualTexture::BuildStreamedMips(RuntimeVirtualTextureComponent, DebugType))
		{
			bOK = true;
		}
	}

	return bOK ? FReply::Handled() : FReply::Unhandled();
}

FReply FRuntimeVirtualTextureComponentDetailsCustomization::BuildMinMaxTexture()
{
	// Create a new asset if none is already bound
	UTexture2D* CreatedTexture = nullptr;
	if (RuntimeVirtualTextureComponent->GetMinMaxTexture() == nullptr)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

		const FString DefaultPath = FPackageName::GetLongPackagePath(RuntimeVirtualTextureComponent->GetVirtualTexture()->GetPathName());
		const FString DefaultName = FPackageName::GetShortName(RuntimeVirtualTextureComponent->GetVirtualTexture()->GetName() + TEXT("_MinMax"));

		UFactory* Factory = NewObject<UTexture2DFactoryNew>();
		UObject* Object = AssetToolsModule.Get().CreateAssetWithDialog(DefaultName, DefaultPath, UTexture2D::StaticClass(), Factory);
		CreatedTexture = Cast<UTexture2D>(Object);
	}

	// Build the texture contents
	bool bOK = false;
	if (RuntimeVirtualTextureComponent->GetMinMaxTexture() != nullptr || CreatedTexture != nullptr)
	{
		const FScopedTransaction Transaction(LOCTEXT("Transaction_BuildMinMaxTexture", "Build MinMax Texture"));

		if (CreatedTexture != nullptr)
		{
			RuntimeVirtualTextureComponent->Modify();
			RuntimeVirtualTextureComponent->SetMinMaxTexture(CreatedTexture);
		}

		RuntimeVirtualTextureComponent->GetMinMaxTexture()->Modify();

		if (RuntimeVirtualTexture::BuildMinMaxHeightTexture(RuntimeVirtualTextureComponent))
		{
			bOK = true;
		}
	}

	return bOK ? FReply::Handled() : FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
