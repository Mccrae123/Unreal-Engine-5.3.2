// Copyright Epic Games, Inc. All Rights Reserved.

#include "Views/OutputMapping/GraphNodes/SDisplayClusterConfiguratorViewportNode.h"

#include "DisplayClusterConfiguratorStyle.h"
#include "DisplayClusterConfiguratorBlueprintEditor.h"
#include "DisplayClusterConfigurationTypes.h"
#include "Interfaces/Views/TreeViews/IDisplayClusterConfiguratorTreeItem.h"
#include "Interfaces/Views/OutputMapping/IDisplayClusterConfiguratorViewOutputMapping.h"
#include "Views/OutputMapping/EdNodes/DisplayClusterConfiguratorViewportNode.h"
#include "Views/OutputMapping/EdNodes/DisplayClusterConfiguratorWindowNode.h"
#include "Views/OutputMapping/Widgets/SDisplayClusterConfiguratorResizer.h"
#include "Views/OutputMapping/Widgets/SDisplayClusterConfiguratorLayeringBox.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "SGraphPanel.h"

#define LOCTEXT_NAMESPACE "SDisplayClusterConfiguratorViewportNode"

int32 const SDisplayClusterConfiguratorViewportNode::DefaultZOrder = 300;

SDisplayClusterConfiguratorViewportNode::~SDisplayClusterConfiguratorViewportNode()
{
	if (UDisplayClusterConfiguratorViewportNode* ViewportNode = Cast<UDisplayClusterConfiguratorViewportNode>(GraphNode))
	{
		ViewportNode->GetOnPreviewUpdated().Unbind();
	}
}

void SDisplayClusterConfiguratorViewportNode::Construct(const FArguments& InArgs,
                                                        UDisplayClusterConfiguratorViewportNode* InViewportNode,
                                                        const TSharedRef<FDisplayClusterConfiguratorBlueprintEditor>& InToolkit)
{
	SDisplayClusterConfiguratorBaseNode::Construct(SDisplayClusterConfiguratorBaseNode::FArguments(), InViewportNode, InToolkit);

	InViewportNode->GetOnPreviewUpdated().BindLambda([this]()
	{
		UpdateGraphNode();
	});
	
	UpdateGraphNode();
}

void SDisplayClusterConfiguratorViewportNode::UpdateGraphNode()
{
	SDisplayClusterConfiguratorBaseNode::UpdateGraphNode();

	BackgroundImage = SNew(SImage)
		.ColorAndOpacity(this, &SDisplayClusterConfiguratorViewportNode::GetBackgroundColor)
		.Image(this, &SDisplayClusterConfiguratorViewportNode::GetBackgroundBrush);

	UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();

	SetPreviewTexture(ViewportEdNode->GetPreviewTexture());

	GetOrAddSlot( ENodeZone::Center )
	.HAlign(HAlign_Fill)
	.VAlign(VAlign_Fill)
	[
		SNew(SDisplayClusterConfiguratorLayeringBox)
		.LayerOffset(DefaultZOrder)
		.ShadowBrush(this, &SDisplayClusterConfiguratorViewportNode::GetNodeShadowBrush)
		[
			SNew(SConstraintCanvas)

			+ SConstraintCanvas::Slot()
			.Offset(TAttribute<FMargin>::Create(TAttribute<FMargin>::FGetter::CreateSP(this, &SDisplayClusterConfiguratorViewportNode::GetBackgroundPosition)))
			.Alignment(FVector2D::ZeroVector)
			[
				SNew(SBox)
				[
					SNew(SVerticalBox)
					+SVerticalBox::Slot()
					[
						SNew(SOverlay)

						+ SOverlay::Slot()

						+ SOverlay::Slot()
						.VAlign(VAlign_Fill)
						.HAlign(HAlign_Fill)
						[
							SNew(SBorder)
							.BorderImage(FDisplayClusterConfiguratorStyle::GetBrush("DisplayClusterConfigurator.Node.Window.Border.Brush"))
							.Padding(FMargin(0.f))
							[
								BackgroundImage.ToSharedRef()
							]
						]

						+ SOverlay::Slot()
						.VAlign(VAlign_Center)
						.HAlign(HAlign_Center)
						.Padding(FMargin(15.f, 12.f))
						[
							SNew(SBox)
							[
								SNew(SScaleBox)
								.Stretch(EStretch::ScaleToFit)
								.StretchDirection(EStretchDirection::DownOnly)
								.VAlign(VAlign_Center)
								[
									SNew(SBorder)
									.BorderImage(FEditorStyle::GetBrush("WhiteBrush"))
									.BorderBackgroundColor(this, &SDisplayClusterConfiguratorViewportNode::GetTextBoxColor)
									.Padding(8.0f)
									[
										SNew( SVerticalBox )

										+ SVerticalBox::Slot()
										.VAlign(VAlign_Center)
										.Padding(5.f, 2.f)
										[
											SNew(STextBlock)
											.Text(FText::FromString(ViewportEdNode->GetNodeName()))
											.Justification(ETextJustify::Center)
											.TextStyle(&FDisplayClusterConfiguratorStyle::GetWidgetStyle<FTextBlockStyle>("DisplayClusterConfigurator.Node.Text.Bold"))
											.ColorAndOpacity(FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Text.Color.Regular"))
										]

										+ SVerticalBox::Slot()
										.VAlign(VAlign_Center)
										.Padding(5.f, 2.f)
										[
											SNew(STextBlock)
											.Text(this, &SDisplayClusterConfiguratorViewportNode::GetPositionAndSizeText)
											.Justification(ETextJustify::Center)
											.TextStyle(&FDisplayClusterConfiguratorStyle::GetWidgetStyle<FTextBlockStyle>("DisplayClusterConfigurator.Node.Text.Regular"))
											.ColorAndOpacity(FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Text.Color.WhiteGray"))
										]

										+ SVerticalBox::Slot()
										.VAlign(VAlign_Center)
										.HAlign(HAlign_Center)
										.AutoHeight()
										.Padding(5.0f, 2.0f)
										[
											SNew(SBox)
											.WidthOverride(32)
											.HeightOverride(32)
											.Visibility(this, &SDisplayClusterConfiguratorViewportNode::GetLockIconVisibility)
											[
												SNew(SImage)
												.Image(FEditorStyle::GetBrush(TEXT("GenericLock")))
											]
										]
									]
								]
							]
						]

						+ SOverlay::Slot()
						.VAlign(VAlign_Fill)
						.HAlign(HAlign_Fill)
						[
							SNew(SBorder)
							.BorderImage(this, &SDisplayClusterConfiguratorViewportNode::GetBorderBrush)
						]
					]
				]
			]

			+ SConstraintCanvas::Slot()
			.Offset(TAttribute<FMargin>::Create(TAttribute<FMargin>::FGetter::CreateSP(this, &SDisplayClusterConfiguratorViewportNode::GetAreaResizeHandlePosition)))
			.AutoSize(true)
			.Alignment(FVector2D::ZeroVector)
			[
				SNew(SDisplayClusterConfiguratorResizer, ToolkitPtr.Pin().ToSharedRef(), SharedThis(this))
				.Visibility(this, &SDisplayClusterConfiguratorViewportNode::GetAreaResizeHandleVisibility)
				.IsFixedAspectRatio(this, &SDisplayClusterConfiguratorViewportNode::IsAspectRatioFixed)
			]
		]
	];
}

void SDisplayClusterConfiguratorViewportNode::MoveTo(const FVector2D& NewPosition, FNodeSet& NodeFilter)
{
	if (IsViewportLocked())
	{
		NodeFilter.Add(SharedThis(this));
	}

	SDisplayClusterConfiguratorBaseNode::MoveTo(NewPosition, NodeFilter);
}

void SDisplayClusterConfiguratorViewportNode::SetPreviewTexture(UTexture* InTexture)
{
	if (InTexture != nullptr)
	{
		if (BackgroundActiveBrush.GetResourceObject() != InTexture)
		{
			BackgroundActiveBrush = FSlateBrush();
			BackgroundActiveBrush.SetResourceObject(InTexture);
			BackgroundActiveBrush.ImageSize.X = InTexture->Resource->GetSizeX();
			BackgroundActiveBrush.ImageSize.Y = InTexture->Resource->GetSizeY();
		}
	}
	else
	{
		// Reset the brush to be empty.
		BackgroundActiveBrush = FSlateBrush();
	}
}

bool SDisplayClusterConfiguratorViewportNode::IsNodeVisible() const
{
	UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();
	TSharedPtr<FDisplayClusterConfiguratorBlueprintEditor> Toolkit = ToolkitPtr.Pin();
	check(Toolkit.IsValid());

	TSharedRef<IDisplayClusterConfiguratorViewOutputMapping> OutputMapping = Toolkit->GetViewOutputMapping();

	const bool bIsSelected = GetOwnerPanel()->SelectionManager.SelectedNodes.Contains(GraphNode);
	const bool bIsVisible = bIsSelected || OutputMapping->GetOutputMappingSettings().bShowOutsideViewports || !ViewportEdNode->IsOutsideParent();
	return SDisplayClusterConfiguratorBaseNode::IsNodeVisible() && bIsVisible;
}

FSlateColor SDisplayClusterConfiguratorViewportNode::GetBackgroundColor() const
{
	const bool bIsSelected = GetOwnerPanel()->SelectionManager.SelectedNodes.Contains(GraphNode);
	const bool bHasImageBackground = BackgroundActiveBrush.GetResourceObject() != nullptr;
	const bool bIsLocked = IsViewportLocked();

	UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();
	if (ViewportEdNode->IsOutsideParentBoundary())
	{
		if (bIsSelected)
		{
			// Selected Case
			return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.OutsideBackgroundColor.Selected");
		}
		else
		{
			// Regular case
			return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.OutsideBackgroundColor.Regular");
		}
	}
	else
	{
		if (bHasImageBackground)
		{
			if (bIsSelected)
			{
				return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.BackgroundImage.Selected");
			}
			else if (bIsLocked)
			{
				return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.BackgroundImage.Locked");
			}
			else
			{
				return FLinearColor::White;
			}
		}
		else
		{
			if (bIsSelected)
			{
				// Selected Case
				return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.BackgroundColor.Selected");
			}
			else
			{
				// Regular case
				return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.BackgroundColor.Regular");
			}
		}
	}

}

const FSlateBrush* SDisplayClusterConfiguratorViewportNode::GetBackgroundBrush() const
{
	if (BackgroundActiveBrush.GetResourceObject() != nullptr)
	{
		return &BackgroundActiveBrush;
	}
	else
	{
		return FDisplayClusterConfiguratorStyle::GetBrush("DisplayClusterConfigurator.Node.Body");
	}
}

const FSlateBrush* SDisplayClusterConfiguratorViewportNode::GetNodeShadowBrush() const
{
	return FEditorStyle::GetBrush(TEXT("Graph.Node.Shadow"));
}

const FSlateBrush* SDisplayClusterConfiguratorViewportNode::GetBorderBrush() const
{
	if (GetOwnerPanel()->SelectionManager.SelectedNodes.Contains(GraphNode))
	{
		return FDisplayClusterConfiguratorStyle::GetBrush("DisplayClusterConfigurator.Node.Viewport.Border.Brush.Selected");
	}
	else
	{
		UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();
		if (ViewportEdNode->IsOutsideParentBoundary())
		{
			return FDisplayClusterConfiguratorStyle::GetBrush("DisplayClusterConfigurator.Node.Viewport.Border.OutsideBrush.Regular");
		}
		else
		{
			return FDisplayClusterConfiguratorStyle::GetBrush("DisplayClusterConfigurator.Node.Viewport.Border.Brush.Regular");
		}
	}
}

FSlateColor SDisplayClusterConfiguratorViewportNode::GetTextBoxColor() const
{
	const bool bIsSelected = GetOwnerPanel()->SelectionManager.SelectedNodes.Contains(GraphNode);
	const bool bIsLocked = IsViewportLocked();
	
	if (bIsSelected)
	{
		return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Color.Selected");
	}
	else if (bIsLocked)
	{
		return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.Text.Background.Locked");
	}

	return FDisplayClusterConfiguratorStyle::GetColor("DisplayClusterConfigurator.Node.Viewport.Text.Background");
}

FText SDisplayClusterConfiguratorViewportNode::GetPositionAndSizeText() const
{
	UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();
	const FDisplayClusterConfigurationRectangle CfgViewportRegion = ViewportEdNode->GetCfgViewportRegion();

	return FText::Format(LOCTEXT("ResAndOffset", "[{0} x {1}] @ {2}, {3}"), CfgViewportRegion.W, CfgViewportRegion.H, CfgViewportRegion.X, CfgViewportRegion.Y);
}

FMargin SDisplayClusterConfiguratorViewportNode::GetBackgroundPosition() const
{
	const FVector2D NodeSize = GetSize();
	return FMargin(0.f, 0.f, NodeSize.X, NodeSize.Y);
}

FMargin SDisplayClusterConfiguratorViewportNode::GetAreaResizeHandlePosition() const
{
	const FVector2D NodeSize = GetSize();
	return FMargin(NodeSize.X, NodeSize.Y, 0.f, 0.f);
}

EVisibility SDisplayClusterConfiguratorViewportNode::GetAreaResizeHandleVisibility() const
{
	if (IsViewportLocked())
	{
		return EVisibility::Collapsed;
	}

	return GetSelectionVisibility();
}

bool SDisplayClusterConfiguratorViewportNode::IsAspectRatioFixed() const
{
	UDisplayClusterConfiguratorViewportNode* ViewportEdNode = GetGraphNodeChecked<UDisplayClusterConfiguratorViewportNode>();
	return ViewportEdNode->IsFixedAspectRatio();
}


bool SDisplayClusterConfiguratorViewportNode::IsViewportLocked() const
{
	TSharedPtr<FDisplayClusterConfiguratorBlueprintEditor> Toolkit = ToolkitPtr.Pin();
	check(Toolkit.IsValid());

	TSharedRef<IDisplayClusterConfiguratorViewOutputMapping> OutputMapping = Toolkit->GetViewOutputMapping();
	return OutputMapping->GetOutputMappingSettings().bLockViewports;
}

EVisibility SDisplayClusterConfiguratorViewportNode::GetLockIconVisibility() const
{
	return IsViewportLocked() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
