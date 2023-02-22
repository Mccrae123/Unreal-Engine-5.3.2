// Copyright Epic Games, Inc. All Rights Reserved.

#include "MovieGraphSchema.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/MovieEdGraph.h"
#include "Graph/MovieEdGraphNode.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"
#include "GraphEditor.h"

TArray<UClass*> UMovieGraphSchema::MoviePipelineNodeClasses;

#define LOCTEXT_NAMESPACE "MoviePipelineGraphSchema"

void UMovieGraphSchema::CreateDefaultNodesForGraph(UEdGraph& Graph) const
{
	/*UMovieGraphConfig* RuntimeGraph = CastChecked<UMoviePipelineEdGraph>(&Graph)->GetPipelineGraph();
	const FScopedTransaction Transaction(LOCTEXT("GraphEditor_NewNode", "Create Pipeline Graph Node."));
	RuntimeGraph->Modify();
	const bool bSelectNewNode = false;

	// Input Node
	{
		UMovieGraphNode* RuntimeNode = RuntimeGraph->ConstructRuntimeNode<UMovieGraphInputNode>();

		// Now create the editor graph node
		FGraphNodeCreator<UMoviePipelineEdGraphNodeInput> NodeCreator(Graph);
		UMoviePipelineEdGraphNodeBase* GraphNode = NodeCreator.CreateUserInvokedNode(bSelectNewNode);
		GraphNode->SetRuntimeNode(RuntimeNode);
		NodeCreator.Finalize();
	}

	// Output Node
	{
		UMovieGraphNode* RuntimeNode = RuntimeGraph->ConstructRuntimeNode<UMovieGraphOutputNode>();

		// Now create the editor graph node
		FGraphNodeCreator<UMoviePipelineEdGraphNodeOutput> NodeCreator(Graph);
		UMoviePipelineEdGraphNodeBase* GraphNode = NodeCreator.CreateUserInvokedNode(bSelectNewNode);
		GraphNode->SetRuntimeNode(RuntimeNode);
		NodeCreator.Finalize();
	}*/
}

void UMovieGraphSchema::InitMoviePipelineNodeClasses()
{
	if (MoviePipelineNodeClasses.Num() > 0)
	{
		return;
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UMovieGraphNode::StaticClass())
			&& !It->HasAnyClassFlags(CLASS_Abstract))
		{
			MoviePipelineNodeClasses.Add(*It);
		}
	}

	MoviePipelineNodeClasses.Sort();
}

void UMovieGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	InitMoviePipelineNodeClasses();

	/* {
		FText Category = NSLOCTEXT("MoveiPipeline", "Category", "Cat1");
		FText MenuDesc = NSLOCTEXT("MoveiPipeline", "MenuDesc", "New Render Layer");
		FText Description = NSLOCTEXT("MoveiPipeline", "Description", "Description");

		TSharedPtr<FMovieGraphSchemaAction_NewNativeElement> NewAction(new FMovieGraphSchemaAction_NewNativeElement(Category, MenuDesc, Description, 0));
		NewAction->NodeClass = UMovieGraphRenderLayerNode::StaticClass();
		ContextMenuBuilder.AddAction(NewAction);
	}

	{
		FText Category = NSLOCTEXT("MoveiPipeline", "Category", "Cat1");
		FText MenuDesc = NSLOCTEXT("MoveiPipeline", "MenuDesc", "New Collection");
		FText Description = NSLOCTEXT("MoveiPipeline", "Description", "Description");

		NewAction->NodeClass = UMovieGraphCollectionNode::StaticClass();
		ContextMenuBuilder.AddAction(NewAction);
	}*/

	for (UClass* PipelineNodeClass : MoviePipelineNodeClasses)
	{
		const UMovieGraphNode* PipelineNode = PipelineNodeClass->GetDefaultObject<UMovieGraphNode>();
		if (!ContextMenuBuilder.FromPin || ContextMenuBuilder.FromPin->Direction == EGPD_Input)
		{
			const FText Name = PipelineNode->GetMenuDescription();
			const FText Category = PipelineNode->GetMenuCategory();
			const FText Tooltip = NSLOCTEXT("MoveiPipeline", "Description", "Placeholder Tooltip");

			TSharedPtr<FMovieGraphSchemaAction_NewNativeElement> NewAction(new FMovieGraphSchemaAction_NewNativeElement(Category, Name, Tooltip, 0));
			NewAction->NodeClass = PipelineNodeClass;

			ContextMenuBuilder.AddAction(NewAction);
		}
	}
}

const FPinConnectionResponse UMovieGraphSchema::CanCreateConnection(const UEdGraphPin* PinA, const UEdGraphPin* PinB) const
{
	// No Circular Connections
	if (PinA->GetOwningNode() == PinB->GetOwningNode())
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, NSLOCTEXT("MoviePipeline", "CircularPinError", "No Circular Connections!"));
	}
	// Make sure the pins are not on the same node
	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, NSLOCTEXT("MoviePipeline", "PinConnect", "Connect nodes"));
}

bool UMovieGraphSchema::TryCreateConnection(UEdGraphPin* InA, UEdGraphPin* InB) const
{
	// See if the native UEdGraph connection goes through
	const bool bModified = Super::TryCreateConnection(InA, InB);

	// If it does, try to propagate the change to our runtime graph
	if (bModified)
	{
		check(InA && InB);
		const UEdGraphPin* A = (InA->Direction == EGPD_Output) ? InA : InB;
		const UEdGraphPin* B = (InA->Direction == EGPD_Input) ? InA : InB;
		check(A->Direction == EGPD_Output && B->Direction == EGPD_Input);

		UMoviePipelineEdGraphNodeBase* EdGraphNodeA = CastChecked<UMoviePipelineEdGraphNodeBase>(A->GetOwningNode());
		UMoviePipelineEdGraphNodeBase* EdGraphNodeB = CastChecked<UMoviePipelineEdGraphNodeBase>(B->GetOwningNode());

		UMovieGraphNode* RuntimeNodeA = EdGraphNodeA->GetRuntimeNode();
		UMovieGraphNode* RuntimeNodeB = EdGraphNodeB->GetRuntimeNode();
		check(RuntimeNodeA && RuntimeNodeB);

		UMovieGraphConfig* RuntimeGraph = RuntimeNodeA->GetGraph();
		check(RuntimeGraph);

		const bool bReconstructNodeB = RuntimeGraph->AddLabeledEdge(RuntimeNodeA, A->PinName, RuntimeNodeB, B->PinName);
		//if (bReconstructNodeB)
		//{
		//	RuntimeNodeB->ReconstructNode();
		//}
	}

	return bModified;
}

void UMovieGraphSchema::BreakPinLinks(UEdGraphPin& TargetPin, bool bSendsNodeNotification) const
{
	const FScopedTransaction Transaction(LOCTEXT("MoviePipelineGraphEditor_BreakPinLinks", "Break Pin Links"));
	Super::BreakPinLinks(TargetPin, bSendsNodeNotification);

	UEdGraphNode* GraphNode = TargetPin.GetOwningNode();
	UMoviePipelineEdGraphNodeBase* MoviePipelineEdGraphNode = CastChecked<UMoviePipelineEdGraphNodeBase>(GraphNode);

	UMovieGraphNode* RuntimeNode = MoviePipelineEdGraphNode->GetRuntimeNode();
	check(RuntimeNode);

	UMovieGraphConfig* RuntimeGraph = RuntimeNode->GetGraph();
	check(RuntimeGraph);

	if (TargetPin.Direction == EEdGraphPinDirection::EGPD_Input)
	{
		RuntimeGraph->RemoveInboundEdges(RuntimeNode, TargetPin.PinName);
	}
	else if (TargetPin.Direction == EEdGraphPinDirection::EGPD_Output)
	{
		RuntimeGraph->RemoveOutboundEdges(RuntimeNode, TargetPin.PinName);
	}
}

void UMovieGraphSchema::BreakSinglePinLink(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin) const
{
	const FScopedTransaction Transaction(LOCTEXT("MoviePipelineGraphEditor_BreakSinglePinLinks", "Break Single Pin Link"));
	Super::BreakSinglePinLink(SourcePin, TargetPin);

	UEdGraphNode* SourceGraphNode = SourcePin->GetOwningNode();
	UEdGraphNode* TargetGraphNode = TargetPin->GetOwningNode();

	UMoviePipelineEdGraphNodeBase* SourcePipelineGraphNode = CastChecked<UMoviePipelineEdGraphNodeBase>(SourceGraphNode);
	UMoviePipelineEdGraphNodeBase* TargetPipelineGraphNode = CastChecked<UMoviePipelineEdGraphNodeBase>(TargetGraphNode);

	UMovieGraphNode* SourceRuntimeNode = SourcePipelineGraphNode->GetRuntimeNode();
	UMovieGraphNode* TargetRuntimeNode = TargetPipelineGraphNode->GetRuntimeNode();
	check(SourceRuntimeNode && TargetRuntimeNode);

	UMovieGraphConfig* RuntimeGraph = SourceRuntimeNode->GetGraph();
	check(RuntimeGraph);

	RuntimeGraph->RemoveEdge(SourceRuntimeNode, SourcePin->PinName, TargetRuntimeNode, TargetPin->PinName);
}

FLinearColor UMovieGraphSchema::GetPinTypeColor(const FEdGraphPinType& PinType) const
{
	return FLinearColor::Red;
	/*const FName& TypeName = PinType.PinCategory;
	if (TypeName == UEdGraphSchema_K2::PC_Struct)
	{
		if (UStruct* Struct = Cast<UStruct>(PinType.PinSubCategoryObject))
		{
			if (Struct->IsChildOf(FRigVMExecuteContext::StaticStruct()))
			{
				return FLinearColor::White;
			}

			if (Struct->IsChildOf(RigVMTypeUtils::GetWildCardCPPTypeObject()))
			{
				return FLinearColor(FVector3f::OneVector * 0.25f);
			}

			if (Struct == FRigElementKey::StaticStruct() || Struct == FRigElementKeyCollection::StaticStruct())
			{
				return FLinearColor(0.0, 0.6588, 0.9490);
			}

			if (Struct == FRigElementKey::StaticStruct() || Struct == FRigPose::StaticStruct())
			{
				return FLinearColor(0.0, 0.3588, 0.5490);
			}

			// external types can register their own colors, check if there are any
			if (IControlRigDeveloperModule* Module = FModuleManager::GetModulePtr<IControlRigDeveloperModule>("ControlRigDeveloper"))
			{
				if (const FLinearColor* Color = Module->FindPinTypeColor(Struct))
				{
					return *Color;
				}
			}
		}
	}*/

	//return GetDefault<UEdGraphSchema_K2>()->GetPinTypeColor(PinType);
}



UEdGraphNode* FMovieGraphSchemaAction_NewNativeElement::PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2D Location, bool bSelectNewNode)
{
	UMovieGraphConfig* RuntimeGraph = CastChecked<UMoviePipelineEdGraph>(ParentGraph)->GetPipelineGraph();
	const FScopedTransaction Transaction(LOCTEXT("GraphEditor_NewNode", "Create Pipeline Graph Node."));
	RuntimeGraph->Modify();

	UMovieGraphNode* RuntimeNode = RuntimeGraph->ConstructRuntimeNode<UMovieGraphNode>(NodeClass);

	// Now create the editor graph node
	FGraphNodeCreator<UMoviePipelineEdGraphNode> NodeCreator(*ParentGraph);
	UMoviePipelineEdGraphNode* GraphNode = NodeCreator.CreateUserInvokedNode(bSelectNewNode);
	GraphNode->Construct(RuntimeNode);
	GraphNode->NodePosX = Location.X;
	GraphNode->NodePosY = Location.Y;

	// Finalize generates a guid, calls a post-place callback, and allocates default pins if needed
	NodeCreator.Finalize();
	return GraphNode;
}

#undef LOCTEXT_NAMESPACE // "MoviePipelineGraphSchema"
