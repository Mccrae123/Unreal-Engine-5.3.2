// Copyright Epic Games, Inc. All Rights Reserved.

#include "SMaterialEditorStrataWidget.h"

#include "EditorWidgetsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MaterialEditor.h"
#include "StrataDefinitions.h"
#include <functional>

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Notifications/SErrorText.h"

#define LOCTEXT_NAMESPACE "SMaterialEditorStrataWidget"

DEFINE_LOG_CATEGORY_STATIC(LogMaterialEditorStrataWidget, Log, All);

void SMaterialEditorStrataWidget::Construct(const FArguments& InArgs, TWeakPtr<FMaterialEditor> InMaterialEditorPtr)
{
	MaterialEditorPtr = InMaterialEditorPtr;

	ButtonApplyToPreview = SNew(SButton)
		.HAlign(HAlign_Center)
		.OnClicked(this, &SMaterialEditorStrataWidget::OnButtonApplyToPreview)
		.Text(LOCTEXT("ButtonApplyToPreview", "Apply to preview"));

	CheckBoxForceFullSimplification = SNew(SCheckBox)
		.Padding(5.0f)
		.ToolTipText(LOCTEXT("CheckBoxForceFullSimplificationToolTip", "This will force full simplification of the material."));	// Just a test, needs to be more explicit

	DescriptionTextBlock = SNew(STextBlock)
		.TextStyle(FAppStyle::Get(), "Log.Normal")
		.ColorAndOpacity(FLinearColor::White)
		.ShadowColorAndOpacity(FLinearColor::Black)
		.ShadowOffset(FVector2D::UnitVector)
		.Text(LOCTEXT("DescriptionTextBlock_Default", "Shader is compiling"));

	if (Strata::IsStrataEnabled())
	{
		this->ChildSlot
		[
			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			//.AutoHeight()			// Cannot use that otherwise scrollbars disapear.
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SScrollBox)
				.Orientation(Orient_Vertical)
				.ScrollBarAlwaysVisible(false)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					+SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+SHorizontalBox::Slot()
						.AutoWidth()
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Center)
						[
							SNew(SWrapBox)
							.UseAllottedSize(true)
							+SWrapBox::Slot()
							.Padding(5.0f)
							.HAlign(HAlign_Left)
							.VAlign(VAlign_Center)
							[
								CheckBoxForceFullSimplification->AsShared()
							]
						]
						+SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(16.0f, 0.0f)
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.ColorAndOpacity(FLinearColor::White)
							.ShadowColorAndOpacity(FLinearColor::Black)
							.ShadowOffset(FVector2D::UnitVector)
							.Text(LOCTEXT("FullsimplificationLabel", "Full simplification"))
						]
						+SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(16.0f, 0.0f)
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Center)
						[
							SNew(SWrapBox)
							//.UseAllottedSize(true)
							+ SWrapBox::Slot()
							.Padding(5.0f)
							.HAlign(HAlign_Left)
							.VAlign(VAlign_Center)
							[
								ButtonApplyToPreview->AsShared()
							]
						]
					]
					+SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f, 0.0f, 0.0f)
					[
						SNew(SWrapBox)
						.UseAllottedSize(true)
						+ SWrapBox::Slot()
						.Padding(5.0f)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SAssignNew(MaterialBox, SBox)
						]
					]
					+SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f, 0.0f, 0.0f)
					[
						SNew(SWrapBox)
						.UseAllottedSize(true)
						+SWrapBox::Slot()
						.Padding(5.0f)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							DescriptionTextBlock->AsShared()
						]
					]
				]
			]
		];
	}
	else
	{
		this->ChildSlot
		[
			SNew(SVerticalBox)
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+SWrapBox::Slot()
				.Padding(5.0f)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FLinearColor::Yellow)
					.ShadowColorAndOpacity(FLinearColor::Black)
					.ShadowOffset(FVector2D::UnitVector)
					.Text(LOCTEXT("SubstrateWidgetNotEnable", "Substrate is not enabled."))
				]
			]
		];
	}
}

TSharedRef<SWidget> SMaterialEditorStrataWidget::GetContent()
{
	return SharedThis(this);
}

SMaterialEditorStrataWidget::~SMaterialEditorStrataWidget()
{
}

void SMaterialEditorStrataWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	if (!bUpdateRequested || !Strata::IsStrataEnabled())
	{
		return;
	}
	bUpdateRequested = false;

	FText StrataMaterialDescription;
	if (MaterialEditorPtr.IsValid())
	{
		auto MaterialEditor = MaterialEditorPtr.Pin();

		UMaterial* MaterialForStats = MaterialEditor->bStatsFromPreviewMaterial ? MaterialEditor->Material : MaterialEditor->OriginalMaterial;

		StrataMaterialDescription = FText::FromString(FString(TEXT("StrataMaterialDescription")));

		const FMaterialResource* MaterialResource = MaterialForStats->GetMaterialResource(GMaxRHIFeatureLevel);
		if (MaterialResource)
		{
			FString MaterialDescription;

			FMaterialShaderMap* ShaderMap = MaterialResource->GetGameThreadShaderMap();
			if (ShaderMap)
			{
				const FStrataMaterialCompilationOutput& CompilationOutput = ShaderMap->GetStrataMaterialCompilationOutput();
				const uint32 FinalPixelByteCount = CompilationOutput.StrataUintPerPixel * sizeof(uint32);

				if (CompilationOutput.bMaterialOutOfBudgetHasBeenSimplified)
				{
					MaterialDescription += FString::Printf(TEXT("The material was OUT-OF-BUDGET so it has been fully simplified: request bytes = %i / budget = %i\r\n"),
						CompilationOutput.RequestedBytePixePixel, CompilationOutput.PlatformBytePixePixel);
					MaterialDescription += FString::Printf(TEXT("Final per pixel byte count   = %i\r\n"),
						FinalPixelByteCount);
				}
				else
				{
					MaterialDescription += FString::Printf(TEXT("Material per pixel byte count= %i / budget = %i\r\n"),
						FinalPixelByteCount, CompilationOutput.PlatformBytePixePixel);
				}
				MaterialDescription += FString::Printf(TEXT("BSDF Count	                  = %i\r\n"), CompilationOutput.StrataBSDFCount);
				MaterialDescription += FString::Printf(TEXT("Local bases Count            = %i\r\n"), CompilationOutput.SharedLocalBasesCount);

				switch (CompilationOutput.StrataMaterialType)
				{
				case 0:
					MaterialDescription += FString::Printf(TEXT("Material complexity          = SIMPLE (diffuse, albedo, roughness)\r\n"));
					break;
				case 1:
					MaterialDescription += FString::Printf(TEXT("Material complexity          = SINGLE (BSDF all features except anisotropy)\r\n"));
					break;
				case 2:
					MaterialDescription += FString::Printf(TEXT("Material complexity          = COMPLEX\r\n"));
					break;
				default:
					MaterialDescription += FString::Printf(TEXT("Material complexity          = UNKOWN => ERROR!\r\n"));
				}

				MaterialDescription += FString::Printf(TEXT("Is Thin                      = %i\r\n"), CompilationOutput.bIsThin);

				MaterialDescription += FString::Printf(TEXT(" \r\n"));
				MaterialDescription += FString::Printf(TEXT(" \r\n"));
				MaterialDescription += FString::Printf(TEXT("================================================================================\r\n"));
				MaterialDescription += FString::Printf(TEXT("================================Detailed Output=================================\r\n"));
				MaterialDescription += FString::Printf(TEXT("================================================================================\r\n"));
				MaterialDescription += CompilationOutput.StrataMaterialDescription;

				// Now generate a visual representation of the material from the topology tree of operators.
				{
					if (CompilationOutput.RootOperatorIndex >= 0)
					{
						std::function<const TSharedRef<SWidget>(const FStrataOperator&)> ProcessOperator = [&](const FStrataOperator& Op) -> const TSharedRef<SWidget>
						{
							switch (Op.OperatorType)
							{
							case STRATA_OPERATOR_WEIGHT:
								return ProcessOperator(CompilationOutput.Operators[Op.LeftIndex]);
								break;
							case STRATA_OPERATOR_VERTICAL:
							{
								auto VerticalOperator = SNew(SVerticalBox)
									+SVerticalBox::Slot()
									.AutoHeight()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.LeftIndex])
									]
									+ SVerticalBox::Slot()
									.AutoHeight()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.RightIndex])
									];
								return VerticalOperator->AsShared();
							}
							break;
							case STRATA_OPERATOR_HORIZONTAL:
							{
								auto HorizontalOperator = SNew(SHorizontalBox)
									+SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.LeftIndex])
									]
									+SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.RightIndex])
									];
								return HorizontalOperator->AsShared();
							}
							break;
							case STRATA_OPERATOR_ADD:
							{
								auto HorizontalOperator = SNew(SHorizontalBox)
									+SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.LeftIndex])
									]
									+SHorizontalBox::Slot()
									.AutoWidth()
									.VAlign(VAlign_Fill)
									.HAlign(HAlign_Fill)
									.Padding(0.0f, 0.0f, 1.0f, 1.0f)
									[
										ProcessOperator(CompilationOutput.Operators[Op.RightIndex])
									];
								return HorizontalOperator->AsShared();
							}
							break;
							case STRATA_OPERATOR_BSDF_LEGACY:	// legacy BSDF should have been converted to BSDF already.
							case STRATA_OPERATOR_BSDF:
							{
								FString BSDFDesc = FString::Printf(TEXT("BSDF (%s%s%s%s%s%s%s)")
									, Op.bBSDFHasEdgeColor ? TEXT("F90 ") : TEXT("")
									, Op.bBSDFHasSSS ? TEXT("SSS ") : TEXT("")
									, Op.bBSDFHasMFPPluggedIn ? TEXT("MFP ") : TEXT("")
									, Op.bBSDFHasAnisotropy ? TEXT("Ani ") : TEXT("")
									, Op.bBSDFHasSecondRoughnessOrSimpleClearCoat ? TEXT("2Ro ") : TEXT("")
									, Op.bBSDFHasFuzz ? TEXT("Fuz ") : TEXT("")
									, Op.bBSDFHasGlint ? TEXT("Gli ") : TEXT("")
								);

								static FString ToolTip;
								if (ToolTip.IsEmpty())
								{
									ToolTip += TEXT("SSS means the BSDF features subsurface profile or subsurface setup using MFP.\n");
									ToolTip += TEXT("MFP means the BSDF MFP is specified by the user.\n");
									ToolTip += TEXT("F90 means the BSDF edge specular color representing reflectivity at grazing angle is used.\n");
									ToolTip += TEXT("Fuz means the BSDF fuzz layer is enabled.\n");
									ToolTip += TEXT("2Ro means the BSDF either uses a second specular lob with a second roughness, or the legacy simple clear coat.\n");
									ToolTip += TEXT("Ani means the BSDF anisotropic specular lighting is used.\n");
									ToolTip += TEXT("Gli means the BSDF features glints.");
								}

								auto BSDF = SNew(SErrorText)
									.ErrorText(FText::FromString(BSDFDesc))
									.BackgroundColor(FSlateColor(EStyleColor::AccentGreen))
									.ToolTipText(FText::FromString(ToolTip));
								return BSDF->AsShared();
							}
							break;
							}

							auto TreeOperatorError = SNew(SErrorText)
								.ErrorText(LOCTEXT("TreeOperatorError", "Tree Operator Error"))
								.BackgroundColor(FSlateColor(EStyleColor::AccentRed));
							return TreeOperatorError->AsShared();
						};

						const FStrataOperator& RootOperator = CompilationOutput.Operators[CompilationOutput.RootOperatorIndex];
						MaterialBox->SetContent(ProcessOperator(RootOperator));
					}
					else
					{
						// The tree does not looks sane so generate a visual error without crashing.
						auto TreeError = SNew(SErrorText)
							.ErrorText(LOCTEXT("TreeError", "Tree Error"))
							.BackgroundColor(FSlateColor(EStyleColor::AccentRed));

						const TSharedRef<SWidget>& TreeErrorAsShared = TreeError->AsShared();
						MaterialBox->SetContent(TreeErrorAsShared);
					}
				}
			}
			else
			{
				MaterialDescription = TEXT("Shader map not found.");
				MaterialBox->SetContent(SNullWidget::NullWidget);
			}

			DescriptionTextBlock->SetText(FText::FromString(MaterialDescription));
		}
	}
}

FReply SMaterialEditorStrataWidget::OnButtonApplyToPreview()
{
	if (MaterialEditorPtr.IsValid())
	{
		UMaterialInterface* MaterialInterface = MaterialEditorPtr.Pin()->GetMaterialInterface();

		FStrataCompilationConfig StrataCompilationConfig;
		StrataCompilationConfig.bFullSimplify = CheckBoxForceFullSimplification->IsChecked();
		MaterialInterface->SetStrataCompilationConfig(StrataCompilationConfig);

		MaterialInterface->ForceRecompileForRendering();
	}



	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
