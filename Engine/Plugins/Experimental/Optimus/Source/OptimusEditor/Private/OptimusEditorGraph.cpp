// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusEditorGraph.h"

#include "OptimusEditorGraphNode.h"

#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodeLink.h"
#include "OptimusNodePin.h"

#include "EdGraph/EdGraphPin.h"
#include "EditorStyleSet.h"
#include "GraphEditAction.h"

UOptimusEditorGraph::UOptimusEditorGraph()
{
	AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateUObject(this, &UOptimusEditorGraph::HandleThisGraphModified));
}


void UOptimusEditorGraph::InitFromNodeGraph(UOptimusNodeGraph* InNodeGraph)
{
	NodeGraph = InNodeGraph;

	// Create all the nodes.
	TMap<UOptimusNode*, UOptimusEditorGraphNode*> NodeMap;
	for (UOptimusNode* ModelNode : InNodeGraph->GetAllNodes())
	{
		UOptimusEditorGraphNode* GraphNode = AddGraphNodeFromModelNode(ModelNode);
		NodeMap.Add(ModelNode, GraphNode);
	}

	// Add all the graph links
	for (const UOptimusNodeLink* Link : InNodeGraph->GetAllLinks())
	{
		UOptimusEditorGraphNode* OutputGraphNode = NodeMap.FindRef(Link->GetNodeOutputPin()->GetNode());
		UOptimusEditorGraphNode* InputGraphNode = NodeMap.FindRef(Link->GetNodeInputPin()->GetNode());

		if (OutputGraphNode == nullptr || InputGraphNode == nullptr)
		{
			continue;
		}

		FName OutputPinName = Link->GetNodeOutputPin()->GetUniqueName();
		FName InputPinName = Link->GetNodeInputPin()->GetUniqueName();

		UEdGraphPin* OutputPin = OutputGraphNode->FindPin(OutputPinName);
		UEdGraphPin* InputPin = InputGraphNode->FindPin(InputPinName);

		OutputPin->MakeLinkTo(InputPin);
	}

	// Listen to notifications from the node graph.
	InNodeGraph->OnModify().AddUObject(this, &UOptimusEditorGraph::HandleNodeGraphModified);
}


const FSlateBrush* UOptimusEditorGraph::GetGraphTypeIcon() const
{
	// FIXME: Need icon types.
	return FEditorStyle::GetBrush(TEXT("GraphEditor.Animation_24x"));
}


void UOptimusEditorGraph::HandleThisGraphModified(const FEdGraphEditAction& InEditAction)
{
	switch (InEditAction.Action)
	{
		case GRAPHACTION_SelectNode:
		{
			SelectedNodes.Reset();
			for (const UEdGraphNode* Node : InEditAction.Nodes)
			{
				UOptimusEditorGraphNode* GraphNode = Cast<UOptimusEditorGraphNode>(
					const_cast<UEdGraphNode*>(Node));
				if (GraphNode != nullptr)
				{
					SelectedNodes.Add(GraphNode);
				}
			}
			break;
		}
		case GRAPHACTION_RemoveNode:
		{
			for (const UEdGraphNode* Node : InEditAction.Nodes)
			{
				UOptimusEditorGraphNode* GraphNode = Cast<UOptimusEditorGraphNode>(
					const_cast<UEdGraphNode*>(Node));
				if (GraphNode != nullptr)
				{
					SelectedNodes.Remove(GraphNode);
				}
			}
			break;
		}

		default:
			break;
	}
}


void UOptimusEditorGraph::HandleNodeGraphModified(EOptimusNodeGraphNotifyType InNotifyType, UOptimusNodeGraph* InNodeGraph, UObject* InSubject)
{
	switch (InNotifyType)
	{
		case EOptimusNodeGraphNotifyType::NodeAdded:
		{
			UOptimusNode *ModelNode = Cast<UOptimusNode>(InSubject);

			if (ensure(ModelNode))
			{
				Modify();

				AddGraphNodeFromModelNode(ModelNode);

				NotifyGraphChanged();
			}
		}
		break;

		case EOptimusNodeGraphNotifyType::NodeRemoved:
		{
			UOptimusNode* ModelNode = Cast<UOptimusNode>(InSubject);

			UOptimusEditorGraphNode* GraphNode = FindGraphNodeFromModelNode(ModelNode);

			if (ensure(GraphNode))
			{
				Modify();
				RemoveNode(GraphNode, true);
				NotifyGraphChanged();
			}
		}
		break;

		case EOptimusNodeGraphNotifyType::NodeLinkAdded:
		case EOptimusNodeGraphNotifyType::NodeLinkRemoved:
		{
			UOptimusNodeLink *ModelNodeLink = Cast<UOptimusNodeLink>(InSubject);
			UOptimusEditorGraphNode* OutputGraphNode = FindGraphNodeFromModelNode(ModelNodeLink->GetNodeOutputPin()->GetNode());
			UOptimusEditorGraphNode* InputGraphNode = FindGraphNodeFromModelNode(ModelNodeLink->GetNodeInputPin()->GetNode());

			if (ensure(OutputGraphNode) && ensure(InputGraphNode))
			{
				UEdGraphPin *OutputGraphPin = OutputGraphNode->FindGraphPinFromModelPin(ModelNodeLink->GetNodeOutputPin());
				UEdGraphPin* InputGraphPin = InputGraphNode->FindGraphPinFromModelPin(ModelNodeLink->GetNodeInputPin());

				if (ensure(OutputGraphPin) && ensure(InputGraphPin))
				{
					Modify();
					if (InNotifyType == EOptimusNodeGraphNotifyType::NodeLinkAdded)
					{
						OutputGraphPin->MakeLinkTo(InputGraphPin);
					}
					else
					{
						OutputGraphPin->BreakLinkTo(InputGraphPin);					
					}
				}
			}
		}
		break;

		case EOptimusNodeGraphNotifyType::NodeDisplayNameChanged:
		{
			ensure(false);
		}
		break;

		case EOptimusNodeGraphNotifyType::NodePositionChanged:
		{
			UOptimusNode* ModelNode = Cast<UOptimusNode>(InSubject);
			UOptimusEditorGraphNode* GraphNode = FindGraphNodeFromModelNode(ModelNode);

			if (ensure(GraphNode))
			{
				GraphNode->NodePosX = FMath::RoundToInt(ModelNode->GetGraphPosition().X);
				GraphNode->NodePosY = FMath::RoundToInt(ModelNode->GetGraphPosition().Y);
			}
		}
		break;

	}
}


UOptimusEditorGraphNode* UOptimusEditorGraph::AddGraphNodeFromModelNode(UOptimusNode* InModelNode)
{
	FGraphNodeCreator<UOptimusEditorGraphNode> NodeCreator(*this);

	UOptimusEditorGraphNode* GraphNode = NodeCreator.CreateNode(false);
	GraphNode->Construct(InModelNode);
	NodeCreator.Finalize();

	return GraphNode;
}


UOptimusEditorGraphNode* UOptimusEditorGraph::FindGraphNodeFromModelNode(UOptimusNode* ModelNode)
{
	if (!ModelNode)
	{
		return nullptr;
	}

	// FIXME: Store this info in a map.
	for (UEdGraphNode* Node : Nodes)
	{
		UOptimusEditorGraphNode *GraphNode = Cast<UOptimusEditorGraphNode>(Node);
		ensure(GraphNode);

		if (GraphNode && GraphNode->ModelNode == ModelNode)
		{
			return GraphNode;
		}
	}

	return nullptr;
}
