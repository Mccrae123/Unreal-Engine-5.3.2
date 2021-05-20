// Copyright Epic Games, Inc. All Rights Reserved.

#include "SLensEvaluation.h"

#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LensFile.h"
#include "Roles/LiveLinkCameraRole.h"
#include "Roles/LiveLinkCameraTypes.h"
#include "UI/CameraCalibrationEditorStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"


#define LOCTEXT_NAMESPACE "LensEvaluation"

void SLensEvaluation::Construct(const FArguments& InArgs, ULensFile* InLensFile)
{
	LensFile = TStrongObjectPtr<ULensFile>(InLensFile);

	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(4.f, 4.f, 4.f, 4.f))
		[
			SNew(SHorizontalBox)

			//Tracking section
			+ SHorizontalBox::Slot()
			.Padding(5.0f, 5.0f)
			.FillWidth(0.2f)
			[
				MakeTrackingWidget()
			]
			//FIZ section
			+ SHorizontalBox::Slot()
			.Padding(5.0f, 5.0f)
			.FillWidth(0.2f)
			[
				MakeFIZWidget()
			]
			//Distortion section
			+ SHorizontalBox::Slot()
			.Padding(5.0f, 5.0f)
			.FillWidth(0.2f)
			[
				MakeDistortionWidget()
			]
			//Image Center section
			+ SHorizontalBox::Slot()
			.Padding(5.0f, 5.0f)
			.FillWidth(0.2f)
			[
				MakeIntrinsicsWidget()
			]
			//Nodal offset section
			+ SHorizontalBox::Slot()
			.Padding(5.0f, 5.0f)
			.FillWidth(0.2f)
			[
				MakeNodalOffsetWidget()	
			]
		]
	];
}

void SLensEvaluation::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	//Cache LiveLink data every tick to be sure we have the right one for the frame during calibration
	CacheLiveLinkData();
	CacheLensFileData();

	Super::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
}

ECheckBoxState SLensEvaluation::IsTrackingActive() const
{
	return bIsUsingLiveLinkTracking ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SLensEvaluation::OnTrackingStateChanged(ECheckBoxState NewState)
{
	bIsUsingLiveLinkTracking = NewState == ECheckBoxState::Checked ? true : false;
}

bool SLensEvaluation::CanSelectTrackingSource() const
{
	return IsTrackingActive() == ECheckBoxState::Checked;
}

SLiveLinkSubjectRepresentationPicker::FLiveLinkSourceSubjectRole SLensEvaluation::GetTrackingSubject() const
{
	return SLiveLinkSubjectRepresentationPicker::FLiveLinkSourceSubjectRole(TrackingSource);
}

void SLensEvaluation::SetTrackingSubject(SLiveLinkSubjectRepresentationPicker::FLiveLinkSourceSubjectRole NewValue)
{
	TrackingSource = NewValue.ToSubjectRepresentation();
}

void SLensEvaluation::CacheLiveLinkData()
{
	if (ShouldUpdateTracking() == false)
	{
		return;
	}

	//Start clean
	CachedLiveLinkData.NormalizedFocus.Reset();
	CachedLiveLinkData.NormalizedIris.Reset();
	CachedLiveLinkData.NormalizedZoom.Reset();
	CachedLiveLinkData.Focus.Reset();
	CachedLiveLinkData.Iris.Reset();
	CachedLiveLinkData.Zoom.Reset();

	if (TrackingSource.Role && TrackingSource.Role->IsChildOf(ULiveLinkCameraRole::StaticClass()))
	{
		IModularFeatures& ModularFeatures = IModularFeatures::Get();
		if (ModularFeatures.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
		{
			ILiveLinkClient* LiveLinkClient = &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
			if(LiveLinkClient)
			{
				FLiveLinkSubjectFrameData SubjectData;
				if (LiveLinkClient->EvaluateFrame_AnyThread(TrackingSource.Subject, TrackingSource.Role, SubjectData))
				{
					FLiveLinkCameraStaticData* StaticData = SubjectData.StaticData.Cast<FLiveLinkCameraStaticData>();
					FLiveLinkCameraFrameData* FrameData = SubjectData.FrameData.Cast<FLiveLinkCameraFrameData>();
					if (StaticData->FIZDataMode == ECameraFIZMode::EncoderData)
					{
						if (StaticData->bIsFocusDistanceSupported)
						{
							CachedLiveLinkData.NormalizedFocus = FrameData->FocusDistance;
							if (LensFile->HasFocusEncoderMapping())
							{
								CachedLiveLinkData.Focus = LensFile->EvaluateNormalizedFocus(FrameData->FocusDistance);
							}
						}

						if (StaticData->bIsApertureSupported)
						{
							CachedLiveLinkData.NormalizedIris = FrameData->Aperture;
							if (LensFile->HasIrisEncoderMapping())
							{
								CachedLiveLinkData.Iris = LensFile->EvaluateNormalizedIris(FrameData->Aperture);
							}
						}

						if (StaticData->bIsFocalLengthSupported)
						{
							CachedLiveLinkData.NormalizedZoom = FrameData->FocalLength;
							if (LensFile->HasZoomEncoderMapping())
							{
								CachedLiveLinkData.Zoom = LensFile->EvaluateNormalizedZoom(FrameData->FocalLength);
							}
						}
					}
					else
					{
						if (StaticData->bIsFocusDistanceSupported)
						{
							CachedLiveLinkData.Focus = FrameData->FocusDistance;
						}

						if (StaticData->bIsApertureSupported)
						{
							CachedLiveLinkData.Iris = FrameData->Aperture;
						}

						if (StaticData->bIsFocalLengthSupported)
						{
							CachedLiveLinkData.Zoom = FrameData->FocalLength;
						}
					}
				}
			}
		}
	}
}

void SLensEvaluation::CacheLensFileData()
{
	if (CachedLiveLinkData.Focus.IsSet() && CachedLiveLinkData.Zoom.IsSet())
	{
		const float Focus = CachedLiveLinkData.Focus.GetValue();
		const float Zoom = CachedLiveLinkData.Zoom.GetValue();
		LensFile->EvaluateDistortionParameters(Focus, Zoom, CachedDistortionInfo);
		LensFile->EvaluateIntrinsicParameters(Focus, Zoom, CachedIntrinsics);
		LensFile->EvaluateNodalPointOffset(Focus, Zoom, CachedNodalOffset);
	}
}

TSharedRef<SWidget> SLensEvaluation::MakeTrackingWidget()
{
	return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.Padding(5.0f, 5.0f)
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TrackingSection", "Tracking"))
					.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SCheckBox)
						.ToolTipText(LOCTEXT("ViewModeTooltip", "Enable/Disable tracking usage"))
						.IsChecked(this, &SLensEvaluation::IsTrackingActive)
						.OnCheckStateChanged(this, &SLensEvaluation::OnTrackingStateChanged)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SLiveLinkSubjectRepresentationPicker)
						.ShowRole(false)
						.Value(this, &SLensEvaluation::GetTrackingSubject)
						.OnValueChanged(this, &SLensEvaluation::SetTrackingSubject)
						.IsEnabled(this, &SLensEvaluation::CanSelectTrackingSource)
					]
				];
}

TSharedRef<SWidget> SLensEvaluation::MakeFIZWidget() const
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5.0f, 5.0f)
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("FIZ Section", "FIZ"))
			.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SGridPanel)
			.FillColumn(0, 0.2f)
			.FillColumn(1, 0.4f)
			.FillColumn(2, 0.4f)

			+ SGridPanel::Slot(1, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("EncodersLabel", "Encoders"))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(2, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PhysicalLabel", "Physicals"))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]

			+ SGridPanel::Slot(0, 1)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FocusLabel", "Focus"))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(0, 2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("IrisLabel", "Iris"))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(0, 3)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ZoomLabel", "Zoom"))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]

			+ SGridPanel::Slot(1, 1)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.NormalizedFocus.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.NormalizedFocus.GetValue());
					}
					return LOCTEXT("UndefinedEncoderFocus", "N/A");
				}))
			]
			+ SGridPanel::Slot(1, 2)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.NormalizedIris.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.NormalizedIris.GetValue());
					}
					return LOCTEXT("UndefinedEncoderIris", "N/A");
				}))
			]
			+ SGridPanel::Slot(1, 3)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.NormalizedZoom.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.NormalizedZoom.GetValue());
					}
					return LOCTEXT("UndefinedEncoderZoom", "N/A");
				}))
			]

			+ SGridPanel::Slot(2, 1)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.Focus.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.Focus.GetValue());
					}
					return LOCTEXT("UndefinedFocus", "N/A");
				}))
			]
			+ SGridPanel::Slot(2, 2)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.Iris.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.Iris.GetValue());
					}
					return LOCTEXT("UndefinedIris", "N/A");
				}))
			]
			+ SGridPanel::Slot(2, 3)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					if (CachedLiveLinkData.Zoom.IsSet())
					{
						return FText::AsNumber(CachedLiveLinkData.Zoom.GetValue());
					}
					return LOCTEXT("UndefinedZoom", "N/A");
				}))
			]
		];
}

TSharedRef<SWidget> SLensEvaluation::MakeDistortionWidget() const
{
	//Find the named distortion parameters the current model has
	TArray<FText> Parameters;
	if (LensFile->LensInfo.LensModel)
	{
		Parameters = LensFile->LensInfo.LensModel.GetDefaultObject()->GetParameterDisplayNames();
	}

	const TSharedRef<SWidget> Title = SNew(STextBlock)
		.Text(LOCTEXT("DistortionSection", "Distortion"))
		.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
		.ShadowOffset(FVector2D(1.0f, 1.0f));

	//if there are no parameters, create a simpler widget
	if (Parameters.Num() <= 0)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.Padding(5.0f, 5.0f)
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				Title
			]
			+ SVerticalBox::Slot()
			.Padding(5.0f, 5.0f)
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoParameters", "No parameters"))
			];
	}

	TSharedRef<SGridPanel> ParameterGrid = SNew(SGridPanel);

	for (int32 Index = 0; Index < Parameters.Num(); ++Index)
	{
		ParameterGrid->AddSlot(0, Index)
			[
				SNew(STextBlock)
				.MinDesiredWidth(35.0f)
				.Text(MakeAttributeLambda([Parameters, Index]
				{
					if (Parameters.IsValidIndex(Index))
					{
						return Parameters[Index];
					}
					else
					{
						return LOCTEXT("InvalidParam", "Invalid");
					}
				}))
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			];

		ParameterGrid->AddSlot(1, Index)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this, Index]
					{
						if (CachedDistortionInfo.Parameters.IsValidIndex(Index))
						{
							return FText::AsNumber(CachedDistortionInfo.Parameters[Index]);
						}
						return LOCTEXT("UndefinedDistortionParameter", "N/A");
					}))
			];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5.0f, 5.0f)
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			Title
		]
		+ SVerticalBox::Slot()
		.Padding(5.0f, 5.0f)
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			ParameterGrid
		];
}

TSharedRef<SWidget> SLensEvaluation::MakeIntrinsicsWidget() const
{
	return
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5.0f, 5.0f)
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("IntrinsicsSection", "Intrinsics"))
			.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(SGridPanel)

			+ SGridPanel::Slot(0, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CxLabel", "Cx"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 0)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedIntrinsics.PrincipalPoint.X);
				}))
			]
			+ SGridPanel::Slot(0, 1)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CyLabel", "Cy"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 1)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedIntrinsics.PrincipalPoint.Y);
				}))
			]
			+ SGridPanel::Slot(0, 2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FxLabel", "Fx"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 2)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedDistortionInfo.FxFy.X);
				}))
			]
			+ SGridPanel::Slot(0, 3)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FyLabel", "Fy"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 3)
			[
				SNew(STextBlock)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedDistortionInfo.FxFy.Y);
				}))
			]
		];
}

TSharedRef<SWidget> SLensEvaluation::MakeNodalOffsetWidget() const
{
	return
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5.0f, 5.0f)
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NodalOffsetSection", "Nodal Offset"))
			.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(SGridPanel)

			+ SGridPanel::Slot(0, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PositionOffsetLabel", "Pos"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 0)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.LocationOffset.X);
				}))
			]
			+ SGridPanel::Slot(2, 0)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.LocationOffset.Y);
				}))
			]
			+ SGridPanel::Slot(3, 0)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.LocationOffset.Z);
				}))
			]

			+ SGridPanel::Slot(0, 1)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RotationOffsetLabel", "Rot"))
				.MinDesiredWidth(35.0f)
				.Font(FEditorStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			]
			+ SGridPanel::Slot(1, 1)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.RotationOffset.X);
				}))
			]
			+ SGridPanel::Slot(2, 1)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.RotationOffset.Y);
				}))
			]
			+ SGridPanel::Slot(3, 1)
			[
				SNew(STextBlock)
				.MinDesiredWidth(15.0f)
				.Text(MakeAttributeLambda([this]
				{
					return FText::AsNumber(CachedNodalOffset.RotationOffset.Z);
				}))
			]
		];
}

#undef LOCTEXT_NAMESPACE /* LensDataViewer */