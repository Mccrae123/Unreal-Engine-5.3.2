// Copyright Epic Games, Inc. All Rights Reserved.

#include "SchemaActions/DataprepDragDropOp.h"

#include "BlueprintNodes/K2Node_DataprepAction.h"
#include "DataprepActionAsset.h"
#include "DataprepEditorLogCategory.h"
#include "DataprepGraph/DataprepGraph.h"
#include "DataprepGraph/DataprepGraphActionNode.h"
#include "DataprepSchemaActionUtils.h"
#include "Widgets/DataprepGraph/SDataprepGraphActionStepNode.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorStyleSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "SGraphPanel.h"
#include "SPinTypeSelector.h"
#include "Templates/UnrealTypeTraits.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Text/STextBlock.h"

#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DataprepDragAndDrop"

class SDataprepGraphActionStepNode;
class SGraphPanel;

FDataprepDragDropOp::FDataprepDragDropOp()
	: FGraphEditorDragDropAction()
	, HoveredDataprepActionContext()
{
	bDropTargetValid = false;
}

TSharedRef<FDataprepDragDropOp> FDataprepDragDropOp::New(TSharedRef<FDataprepSchemaAction> InAction)
{
	TSharedRef< FDataprepDragDropOp > DragDrop = MakeShared< FDataprepDragDropOp >();
	DragDrop->DataprepGraphOperation.BindSP( InAction, &FDataprepSchemaAction::ExecuteAction );
	DragDrop->Construct();
	return DragDrop;
}

TSharedRef<FDataprepDragDropOp> FDataprepDragDropOp::New(FDataprepGraphOperation&& DataprepGraphOperation)
{
	TSharedRef< FDataprepDragDropOp > DragDrop = MakeShared< FDataprepDragDropOp >();
	DragDrop->DataprepGraphOperation = MoveTemp( DataprepGraphOperation );
	DragDrop->Construct();
	return DragDrop;
}

TSharedRef<FDataprepDragDropOp> FDataprepDragDropOp::New(const TSharedRef<SGraphPanel>& InGraphPanel, const TSharedRef<SDataprepGraphActionStepNode>& InDraggedNode)
{
	TSharedRef<FDataprepDragDropOp> Operation = MakeShareable(new FDataprepDragDropOp);

	Operation->GraphPanelPtr = InGraphPanel;
	Operation->DraggedNodeWidgets.Add(InDraggedNode);
	if(UDataprepGraphActionStepNode* ActionStepNode = Cast<UDataprepGraphActionStepNode>(InDraggedNode->GetNodeObj()))
	{
		Operation->DraggedSteps.Emplace(ActionStepNode->GetDataprepActionAsset(), ActionStepNode->GetStepIndex(), ActionStepNode->GetDataprepActionStep() );
	}

	// adjust the decorator away from the current mouse location a small amount based on cursor size
	Operation->DecoratorAdjust = FSlateApplication::Get().GetCursorSize();

	Operation->Construct();

	return Operation;
}

TSharedRef<FDataprepDragDropOp> FDataprepDragDropOp::New(UDataprepActionStep* InActionStep)
{
	TSharedRef<FDataprepDragDropOp> Operation = MakeShareable(new FDataprepDragDropOp);

	if(InActionStep != nullptr)
	{
		Operation->DraggedSteps.Emplace(nullptr, INDEX_NONE, InActionStep );

		// adjust the decorator away from the current mouse location a small amount based on cursor size
		Operation->DecoratorAdjust = FSlateApplication::Get().GetCursorSize();

		Operation->Construct();
	}

	return Operation;
}

void FDataprepDragDropOp::HoverTargetChanged()
{
	FText DrapDropText;

	if(DraggedNodeWidgets.Num() > 0)
	{
		HoverTargetChangedWithNodes();
		return;
	}

	if ( HoveredDataprepActionContext )
	{
		bDropTargetValid = true;
		DrapDropText = LOCTEXT("TargetIsDataprepActionContext", "Add a Step to Dataprep Action");
	}
	else if ( UEdGraph* EdGraph = GetHoveredGraph() )
	{
		if ( const UEdGraphSchema_K2* GraphSchema_k2 = Cast<UEdGraphSchema_K2>( EdGraph->GetSchema() ) )
		{
			bDropTargetValid = true;
			DrapDropText = LOCTEXT("TargetIsBlueprintGraph", "Add a Dataprep Action");
		}
		else
		{
			bDropTargetValid = false;
			DrapDropText = LOCTEXT("TargetGraphIsInvalid", "Can only be drop on a blueprint graph");
		}
	}
	else
	{
		bDropTargetValid = false;
		DrapDropText = FText::FromString( TEXT("Can't drop here") );
	}

	const FSlateBrush* Symbol = bDropTargetValid ? FEditorStyle::GetBrush( TEXT("Graph.ConnectorFeedback.OK") ) : FEditorStyle::GetBrush( TEXT("Graph.ConnectorFeedback.Error") );
	SetSimpleFeedbackMessage( Symbol, FLinearColor::White, DrapDropText );
}

FReply FDataprepDragDropOp::DroppedOnPanel(const TSharedRef<SWidget>& Panel, FVector2D ScreenPosition, FVector2D GraphPosition, UEdGraph& Graph)
{
	if ( bDropTargetValid )
	{
		if ( DataprepPreDropConfirmation.IsBound() )
		{
			TFunction<void ()> OnConfirmation ( [Operation = StaticCastSharedRef<FDataprepDragDropOp>( AsShared() ), Panel, ScreenPosition, GraphPosition, GraphPtr = TWeakObjectPtr<UEdGraph>(&Graph)] ()
				{
					UEdGraph* Graph = GraphPtr.Get();
					if ( Graph )
					{
						Operation->DoDropOnPanel(Panel, ScreenPosition, GraphPosition, *Graph);
					}
				} );

			DataprepPreDropConfirmation.Execute( FDataprepSchemaActionContext(), OnConfirmation);
		
			return FReply::Handled();
		}
		else
		{
			DoDropOnPanel( Panel, ScreenPosition, GraphPosition, Graph );
			return FReply::Handled();
		}
	}

	return FReply::Unhandled();
}

void FDataprepDragDropOp::OnDragged(const FDragDropEvent& DragDropEvent)
{
	FVector2D TargetPosition = DragDropEvent.GetScreenSpacePosition();

	// Reposition the info window to the dragged position
	CursorDecoratorWindow->MoveWindowTo(TargetPosition + DecoratorAdjust);
	// Request the active panel to scroll if required

	if(SGraphPanel* GraphPanel = GraphPanelPtr.Get())
	{
		GraphPanel->RequestDeferredPan(TargetPosition);
	}

	Super::OnDragged(DragDropEvent);
}

EVisibility FDataprepDragDropOp::GetIconVisible() const
{
	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	const bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	return bDropTargetValid || bCopyRequested ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FDataprepDragDropOp::GetErrorIconVisible() const
{
	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	const bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	return bDropTargetValid || bCopyRequested ? EVisibility::Collapsed : EVisibility::Visible;
}

FReply FDataprepDragDropOp::DroppedOnNode(FVector2D ScreenPosition, FVector2D GraphPosition)
{
	if(Cast<UDataprepGraphActionStepNode>(GetHoveredNode()) != nullptr)
	{
		return DoDropOnActionStep(ScreenPosition, GraphPosition);
	}
	else if(Cast<UDataprepGraphActionNode>(GetHoveredNode()) != nullptr)
	{
		return DoDropOnActionAsset(ScreenPosition, GraphPosition);
	}

	return FReply::Unhandled();
}

UDataprepGraphActionStepNode* FDataprepDragDropOp::GetDropTargetNode() const
{
	return Cast<UDataprepGraphActionStepNode>(GetHoveredNode());
}

void FDataprepDragDropOp::SetHoveredDataprepActionContext(TOptional<FDataprepSchemaActionContext> Context)
{
	if ( HoveredDataprepActionContext != Context )
	{
		HoveredDataprepActionContext = Context;
		HoverTargetChanged();
	}
}

FReply FDataprepDragDropOp::DroppedOnDataprepActionContext(const FDataprepSchemaActionContext& Context)
{
	if ( DataprepPreDropConfirmation.IsBound() )
	{
		TFunction<void ()> OnConfirmation( [Operation = StaticCastSharedRef<FDataprepDragDropOp>(AsShared()), Context = FDataprepSchemaActionContext(Context)] ()
		{
			Operation->DoDropOnDataprepActionContext( Context );
		} );

		DataprepPreDropConfirmation.Execute( Context, OnConfirmation );
	}
	else
	{
		DoDropOnDataprepActionContext( Context );
	}

	return FReply::Handled();
}

void FDataprepDragDropOp::HoverTargetChangedWithNodes()
{
	bDropTargetValid = GetHoveredNode() && DraggedNodeWidgets[0]->GetNodeObj() != GetHoveredNode();
	const FSlateBrush* Icon = FEditorStyle::GetBrush( TEXT("Graph.ConnectorFeedback.OK") );

	TAttribute<FText> MessageText = TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FDataprepDragDropOp::GetMessageText));

	TSharedRef<SVerticalBox> FeedbackBox = SNew(SVerticalBox);

	FeedbackBox->AddSlot()
	.AutoHeight()
	[
		SNew(SHorizontalBox)
		+SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(3.0f)
		[
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			[
				SNew(SImage)
				.Visibility(EVisibility::Visible)
				.Image( TAttribute<const FSlateBrush*>::Create(TAttribute<const FSlateBrush*>::FGetter::CreateSP(this, &FDataprepDragDropOp::GetIcon)) )
				.ColorAndOpacity( FLinearColor::White )
			]
		]

		+SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(3.0f)
		.MaxWidth(500)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.WrapTextAt( 480 )
			.Text( MessageText )
		]
	];

	for (int32 i=0; i < DraggedNodeWidgets.Num(); i++)
	{
		FeedbackBox->AddSlot()
		.AutoHeight()
		[
			DraggedNodeWidgets[i]->GetStepTitleWidget().ToSharedRef()
		];
	}

	SetFeedbackMessage(FeedbackBox);
}

FText FDataprepDragDropOp::GetMessageText()
{
	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	const bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	if(bDropTargetValid || bCopyRequested)
	{
		if(Cast<UDataprepGraphActionStepNode>(GetHoveredNode()) != nullptr)
		{
			LastMessageText = bCopyRequested ? LOCTEXT("CopyDataprepActionStepNode", "Copy step to location") : LOCTEXT("MoveDataprepActionStepNode", "Move step to location");
		}
		else if(Cast<UDataprepGraphActionNode>(GetHoveredNode()) != nullptr)
		{
			LastMessageText = bCopyRequested ? LOCTEXT("CopyDataprepActionAssetNode", "Copy step to location") : LOCTEXT("MoveDataprepActionAssetNode", "Move step to location");
		}
	}
	else if(GetHoveredNode() != nullptr)
	{
		LastMessageText = LOCTEXT("NoSelfMove", "Cannot move to itself");
	}
	else
	{
		LastMessageText = LOCTEXT("NotImplemented", "Operation not implemented yet");
	}

	return LastMessageText;
}

const FSlateBrush* FDataprepDragDropOp::GetIcon() const
{
	static const FSlateBrush* IconOK = FEditorStyle::GetBrush( TEXT("Graph.ConnectorFeedback.OK") );
	static const FSlateBrush* IconError = FEditorStyle::GetBrush( TEXT("Graph.ConnectorFeedback.Error") );

	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	const bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	return bDropTargetValid || (GetHoveredNode() != nullptr && bCopyRequested) ? IconOK : IconError;
}

void FDataprepDragDropOp::SetPreDropConfirmation(FDataprepPreDropConfirmation && Confirmation)
{
	DataprepPreDropConfirmation = MoveTemp( Confirmation );
}

bool FDataprepDragDropOp::DoDropOnDataprepActionContext(const FDataprepSchemaActionContext& Context)
{
	if ( DataprepGraphOperation.IsBound() )
	{
		FScopedTransaction Transaction( LOCTEXT("AddStep", "Add a Step to a Dataprep Action") );
		bool bDidModification = DataprepGraphOperation.Execute( Context );
		if ( !bDidModification )
		{
			Transaction.Cancel();
		}
		return bDidModification;
	}
	return false;
}

void FDataprepDragDropOp::DoDropOnPanel(const TSharedRef<SWidget>& Panel, FVector2D ScreenPosition, FVector2D GraphPosition, UEdGraph& Graph)
{
	if (UEdGraph* EdGraph = GetHoveredGraph())
	{
		if(Cast<UDataprepGraph>(EdGraph) != nullptr)
		{
			return;
		}

		FScopedTransaction Transaction( LOCTEXT("AddNode", "Add Dataprep Action Node") );
		UK2Node_DataprepAction* DataprepActionNode = DataprepSchemaActionUtils::SpawnEdGraphNode< UK2Node_DataprepAction >( Graph, GraphPosition );
		check( DataprepActionNode );
		DataprepActionNode->CreateDataprepActionAsset();
		DataprepActionNode->AutowireNewNode( GetHoveredPin() );

		FDataprepSchemaActionContext Context;
		Context.DataprepActionPtr = DataprepActionNode->GetDataprepAction();
		if ( !DoDropOnDataprepActionContext( Context ) )
		{
			Transaction.Cancel();
		}

		if ( UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraphChecked( EdGraph ) )
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified( Blueprint );
		}
	}
}

FReply FDataprepDragDropOp::DoDropOnActionStep(FVector2D ScreenPosition, FVector2D GraphPosition)
{
	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	if(!bDropTargetValid && !bCopyRequested)
	{
		return FReply::Handled().EndDragDrop();
	}

	UDataprepGraphActionStepNode* TargetActionStepNode = Cast<UDataprepGraphActionStepNode>(GetHoveredNode());
	ensure(TargetActionStepNode != nullptr);
	UDataprepActionAsset* TargetActionAsset = TargetActionStepNode->GetDataprepActionAsset();

	for(FDraggedStepEntry& DraggedStepEntry : DraggedSteps)
	{
		if(DraggedStepEntry.Get<0>().IsValid() || DraggedStepEntry.Get<2>().IsValid())
		{
			FScopedTransaction Transaction( LOCTEXT("DropOnActionStep", "Copy/Move Dataprep Action Step") );
			bool bTransactionSuccessful = true;

			// External drag and drop
			if(!DraggedStepEntry.Get<0>().IsValid())
			{
				bTransactionSuccessful &= TargetActionAsset->InsertStep( DraggedStepEntry.Get<2>().Get(), TargetActionStepNode->GetStepIndex() );
			}
			// Drag and drop within an action asset or between two action assets
			else
			{
				UDataprepActionAsset* SourceActionAsset = DraggedStepEntry.Get<0>().Get();
				check(SourceActionAsset);

				int32 StepIndex = DraggedStepEntry.Get<1>();
				check(StepIndex != INDEX_NONE);

				// Hold onto the action step in case of a move
				TStrongObjectPtr<UDataprepActionStep> SourceActionStepPtr = TStrongObjectPtr<UDataprepActionStep>( SourceActionAsset->GetStep(StepIndex).Get() );
				check(SourceActionStepPtr.IsValid());

				// source action asset differs from target action asset
				if( TargetActionAsset != SourceActionAsset)
				{
					if(!bCopyRequested)
					{
						bTransactionSuccessful &= SourceActionAsset->RemoveStep( StepIndex );
					}

					bTransactionSuccessful &= TargetActionAsset->InsertStep( SourceActionStepPtr.Get(), TargetActionStepNode->GetStepIndex() );
				}
				else if(bCopyRequested)
				{
					bTransactionSuccessful &= TargetActionAsset->InsertStep( SourceActionStepPtr.Get(), TargetActionStepNode->GetStepIndex() );
				}
				else
				{
					bTransactionSuccessful &= TargetActionAsset->MoveStep( StepIndex, TargetActionStepNode->GetStepIndex() );
				}
			}

			if(!bTransactionSuccessful)
			{
				Transaction.Cancel();
			}
		}
	}

	DraggedNodeWidgets.Reset();
	DraggedSteps.Reset();

	return FReply::Handled().EndDragDrop();
}

FReply FDataprepDragDropOp::DoDropOnActionAsset(FVector2D ScreenPosition, FVector2D GraphPosition)
{
	FModifierKeysState ModifierKeyState = FSlateApplication::Get().GetModifierKeys();
	bool bCopyRequested = ModifierKeyState.IsControlDown() || ModifierKeyState.IsCommandDown();

	UDataprepGraphActionNode* TargetActionAssetNode = Cast<UDataprepGraphActionNode>(GetHoveredNode());
	ensure(TargetActionAssetNode != nullptr);
	UDataprepActionAsset* TargetActionAsset = TargetActionAssetNode->GetDataprepActionAsset();

	for(FDraggedStepEntry& DraggedStepEntry : DraggedSteps)
	{
		if(DraggedStepEntry.Get<0>().IsValid() || DraggedStepEntry.Get<2>().IsValid())
		{
			FScopedTransaction Transaction( LOCTEXT("DropOnActionStep", "Copy/Move Dataprep Action Step") );
			bool bTransactionSuccessful = true;

			// External drag and drop
			if(!DraggedStepEntry.Get<0>().IsValid())
			{
				bTransactionSuccessful &= TargetActionAsset->AddStep( DraggedStepEntry.Get<2>().Get() ) != INDEX_NONE;
			}
			// Drag and drop within an action asset or between two action assets
			else
			{
				UDataprepActionAsset* SourceActionAsset = DraggedStepEntry.Get<0>().Get();
				check(SourceActionAsset);

				int32 StepIndex = DraggedStepEntry.Get<1>();
				check(StepIndex != INDEX_NONE);

				// Hold onto the action step in case of a move
				TStrongObjectPtr<UDataprepActionStep> SourceActionStepPtr = TStrongObjectPtr<UDataprepActionStep>( SourceActionAsset->GetStep(StepIndex).Get() );
				check(SourceActionStepPtr.IsValid());

				// source action asset differs from target action asset
				if( TargetActionAsset != SourceActionAsset)
				{
					if(!bCopyRequested)
					{
						SourceActionAsset->RemoveStep( StepIndex );
					}

					TargetActionAsset->AddStep( SourceActionStepPtr.Get() );
				}
				else if(bCopyRequested)
				{
					TargetActionAsset->AddStep( SourceActionStepPtr.Get() );
				}
				else
				{
					TargetActionAsset->MoveStep( StepIndex, TargetActionAsset->GetStepsCount() - 1 );
				}
			}

			if(!bTransactionSuccessful)
			{
				Transaction.Cancel();
			}
		}
	}

	DraggedNodeWidgets.Reset();
	DraggedSteps.Reset();

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
