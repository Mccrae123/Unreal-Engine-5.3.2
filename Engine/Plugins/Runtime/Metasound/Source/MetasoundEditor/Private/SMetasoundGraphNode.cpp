// Copyright Epic Games, Inc. All Rights Reserved.
#include "SMetasoundGraphNode.h"

#include "EditorStyleSet.h"
#include "GraphEditorSettings.h"
#include "IDocumentation.h"
#include "MetasoundEditorGraphNode.h"
#include "MetasoundEditorModule.h"
#include "ScopedTransaction.h"
#include "SCommentBubble.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "SLevelOfDetailBranchNode.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateColor.h"
#include "TutorialMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"

#define LOCTEXT_NAMESPACE "MetasoundGraphNode"

void SMetasoundGraphNode::Construct(const FArguments& InArgs, class UEdGraphNode* InNode)
{
	GraphNode = InNode;
	SetCursor(EMouseCursor::CardinalCross);
	UpdateGraphNode();
}

void SMetasoundGraphNode::CreateOutputSideAddButton(TSharedPtr<SVerticalBox> OutputBox)
{
	TSharedRef<SWidget> AddPinButton = AddPinButtonContent(
		LOCTEXT("MetasoundGraphNode_AddPinButton", "Add input"),
		LOCTEXT("MetasoundGraphNode_AddPinButton_Tooltip", "Add an input to the parent Metasound node.")
	);

	FMargin AddPinPadding = Settings->GetOutputPinPadding();
	AddPinPadding.Top += 6.0f;

	OutputBox->AddSlot()
	.AutoHeight()
	.VAlign(VAlign_Center)
	.Padding(AddPinPadding)
	[
		AddPinButton
	];
}

UMetasoundEditorGraphNode& SMetasoundGraphNode::GetMetasoundNode()
{
	check(GraphNode);
	return *Cast<UMetasoundEditorGraphNode>(GraphNode);
}

const UMetasoundEditorGraphNode& SMetasoundGraphNode::GetMetasoundNode() const
{
	check(GraphNode);
	return *Cast<UMetasoundEditorGraphNode>(GraphNode);
}

void SMetasoundGraphNode::CreateStandardPinWidget(UEdGraphPin* CurPin)
{
	const bool bShowPin = ShouldPinBeHidden(CurPin);
	if (bShowPin)
	{
		TSharedPtr<SGraphPin> NewPin = CreatePinWidget(CurPin);
		check(NewPin.IsValid());

		Metasound::Frontend::FNodeHandle NodeHandle = GetMetasoundNode().GetNodeHandle();
		if (CurPin->Direction == EGPD_Input)
		{
			if (!NodeHandle->GetClassDisplayInfo().bShowInputName)
			{
				NewPin->SetShowLabel(false);
			}
		}
		else if (CurPin->Direction == EGPD_Output)
		{
			if (!NodeHandle->GetClassDisplayInfo().bShowOutputName)
			{
				NewPin->SetShowLabel(false);
			}
		}

		AddPin(NewPin.ToSharedRef());
	}
}

TSharedRef<SWidget> SMetasoundGraphNode::CreateTitleWidget(TSharedPtr<SNodeTitle> NodeTitle)
{
	Metasound::Frontend::FNodeHandle NodeHandle = GetMetasoundNode().GetNodeHandle();
	if (!NodeHandle->GetClassDisplayInfo().bShowName)
	{
		return SNullWidget::NullWidget;
	}

	return SGraphNode::CreateTitleWidget(NodeTitle);
}

void SMetasoundGraphNode::SetDefaultTitleAreaWidget(TSharedRef<SOverlay> DefaultTitleAreaWidget)
{
	SGraphNode::SetDefaultTitleAreaWidget(DefaultTitleAreaWidget);

	Metasound::Frontend::FNodeHandle NodeHandle = GetMetasoundNode().GetNodeHandle();
	if (NodeHandle->GetClassDisplayInfo().bShowName)
	{
		DefaultTitleAreaWidget->ClearChildren();
		TSharedPtr<SNodeTitle> NodeTitle = SNew(SNodeTitle, GraphNode);

		DefaultTitleAreaWidget->AddSlot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.HAlign(HAlign_Fill)
			[
				SNew(SBorder)
				.BorderImage(FEditorStyle::GetBrush("NoBorder"))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						[
							CreateTitleWidget(NodeTitle)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							NodeTitle.ToSharedRef()
						]
					]
				]
			]
			+ SHorizontalBox::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			.Padding(0, 0, 5, 0)
			.AutoWidth()
			[
				CreateTitleRightWidget()
			]
		];

		DefaultTitleAreaWidget->AddSlot()
		.VAlign(VAlign_Top)
		[
			SNew(SBorder)
			.Visibility(EVisibility::HitTestInvisible)
			.BorderImage( FEditorStyle::GetBrush( "Graph.Node.TitleHighlight" ) )
			.BorderBackgroundColor( this, &SGraphNode::GetNodeTitleIconColor )
			[
				SNew(SSpacer)
				.Size(FVector2D(20,20))
			]
		];
	}
	else
	{
		DefaultTitleAreaWidget->SetVisibility(EVisibility::Collapsed);
	}
}

const FSlateBrush* SMetasoundGraphNode::GetNodeBodyBrush() const
{
// 	if (const ISlateStyle* MetasoundStyle = FSlateStyleRegistry::FindSlateStyle("MetasoundStyle"))
// 	{
// 		if (GetMetasoundNode().GetNodeHandle()->GetClassType() == EMetasoundFrontendClassType::Input)
// 		{
// 			if (const FSlateBrush* InputBrush = MetasoundStyle->GetBrush("MetasoundEditor.Graph.Node.Body.Input"))
// 			{
// 				return InputBrush;
// 			}
// 		}
// 
// 		if (const FSlateBrush* DefaultBrush = MetasoundStyle->GetBrush("MetasoundEditor.Graph.Node.Body.Default"))
// 		{
// 			return DefaultBrush;
// 		}
// 	}

	return FEditorStyle::GetBrush("Graph.Node.Body");
}

EVisibility SMetasoundGraphNode::IsAddPinButtonVisible() const
{
	EVisibility DefaultVisibility = SGraphNode::IsAddPinButtonVisible();
	if (DefaultVisibility == EVisibility::Visible)
	{
		if (!GetMetasoundNode().CanAddInputPin())
		{
			return EVisibility::Collapsed;
		}
	}

	return DefaultVisibility;
}

FReply SMetasoundGraphNode::OnAddPin()
{
	GetMetasoundNode().CreateInputPin();

	return FReply::Handled();
}

TSharedRef<SWidget> SMetasoundGraphNode::CreateNodeContentArea()
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	FNodeHandle NodeHandle = GetMetasoundNode().GetNodeHandle();
	FMetasoundFrontendClassDisplayInfo DisplayInfo = NodeHandle->GetClassDisplayInfo();

	TSharedRef<SHorizontalBox> ContentBox = SNew(SHorizontalBox);

	if (DisplayInfo.ImageName.IsNone())
	{
		ContentBox->AddSlot()
			.HAlign(HAlign_Left)
			.FillWidth(1.0f)
			[
				SAssignNew(LeftNodeBox, SVerticalBox)
			];
	}
	else
	{
		ContentBox->AddSlot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				SAssignNew(LeftNodeBox, SVerticalBox)
			];

		if (const ISlateStyle* MetasoundStyle = FSlateStyleRegistry::FindSlateStyle("MetasoundStyle"))
		{
			const FSlateBrush* ImageBrush = MetasoundStyle->GetBrush(DisplayInfo.ImageName);
			ContentBox->AddSlot()
				.AutoWidth()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(ImageBrush)
					.ColorAndOpacity(FSlateColor::UseForeground())
				];
		}
	}

	ContentBox->AddSlot()
		.AutoWidth()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		[
			SAssignNew(RightNodeBox, SVerticalBox)
		];

	return SNew(SBorder)
		.BorderImage(FEditorStyle::GetBrush("NoBorder"))
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		.Padding(FMargin(0,3))
		[
			ContentBox
		];
}
#undef LOCTEXT_NAMESPACE // MetasoundGraphNode
