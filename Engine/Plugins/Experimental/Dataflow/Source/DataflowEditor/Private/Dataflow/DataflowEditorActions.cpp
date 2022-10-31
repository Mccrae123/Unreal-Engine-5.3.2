// Copyright Epic Games, Inc. All Rights Reserved.

#include "Dataflow/DataflowEditorActions.h"
#include "Dataflow/DataflowEdNode.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowObject.h"
#include "EdGraph/EdGraphNode.h"
#include "IStructureDetailsView.h"
#include "EdGraphNode_Comment.h"

#define LOCTEXT_NAMESPACE "DataflowEditorCommands"

void FDataflowEditorCommandsImpl::RegisterCommands()
{
	UI_COMMAND(EvaluateNode, "Evaluate", "Trigger an evaluation of the selected node.", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(CreateComment, "CreateComment", "Create a Comment node.", EUserInterfaceActionType::None, FInputChord());
	UI_COMMAND(ToggleEnabledState, "ToggleEnabledState", "Toggle node between Enabled/Disabled state.", EUserInterfaceActionType::Button, FInputChord());

	if (Dataflow::FNodeFactory* Factory = Dataflow::FNodeFactory::GetInstance())
	{
		for (Dataflow::FFactoryParameters Parameters : Factory->RegisteredParameters())
		{
			TSharedPtr< FUICommandInfo > AddNode;
			FUICommandInfo::MakeCommandInfo(
				this->AsShared(),
				AddNode,
				Parameters.TypeName,
				LOCTEXT("DataflowButton", "New Dataflow Node"),
				LOCTEXT("NewDataflowNodeTooltip", "New Dataflow Node Tooltip"),
				FSlateIcon(),
				EUserInterfaceActionType::Button,
				FInputChord()
			);
			CreateNodesMap.Add(Parameters.TypeName, AddNode);
		}
	}
}

const FDataflowEditorCommandsImpl& FDataflowEditorCommands::Get()
{
	return FDataflowEditorCommandsImpl::Get();
}

void FDataflowEditorCommands::Register()
{
	return FDataflowEditorCommandsImpl::Register();
}

void FDataflowEditorCommands::Unregister()
{
	return FDataflowEditorCommandsImpl::Unregister();
}



void FDataflowEditorCommands::EvaluateSelectedNodes(const FGraphPanelSelectionSet& SelectedNodes, FDataflowEditorCommands::FGraphEvaluationCallback Evaluate)
{
	for (UObject* Ode : SelectedNodes)
	{
		if (UDataflowEdNode* EdNode = dynamic_cast<UDataflowEdNode*>(Ode))
		{
			if (const TSharedPtr<Dataflow::FGraph> DataflowGraph = EdNode->GetDataflowGraph())
			{
				if (const TSharedPtr<FDataflowNode> DataflowNode = DataflowGraph->FindBaseNode(EdNode->GetDataflowNodeGuid()))
				{
					if (DataflowNode->bActive)
					{
						if (DataflowNode->GetOutputs().Num())
						{
							for (FDataflowConnection* NodeOutput : DataflowNode->GetOutputs())
							{
								Evaluate(DataflowNode.Get(), (FDataflowOutput*)NodeOutput);
							}
						}
						else
						{
							Evaluate(DataflowNode.Get(), nullptr);
						}
					}
				}
			}
		}
	}
}

void FDataflowEditorCommands::EvaluateNode(Dataflow::FContext& Context, Dataflow::FTimestamp& OutLastNodeTimestamp,
	const UDataflow* Dataflow, const FDataflowNode* InNode, const FDataflowOutput* Output, FString NodeName)
{
	if (Dataflow)
	{
		const FDataflowNode* Node = InNode;
		if (Node == nullptr)
		{
			if (const TSharedPtr<Dataflow::FGraph> Graph = Dataflow->GetDataflow())
			{
				if (TSharedPtr<const FDataflowNode> GraphNode = Graph->FindBaseNode(FName(NodeName)))
				{
					Node = GraphNode.Get();
				}
			}
		}

		if (Node != nullptr)
		{
			if (Output == nullptr)
			{
				if (Node->GetTimestamp() >= OutLastNodeTimestamp)
				{
					Context.Evaluate(Node, nullptr);
					OutLastNodeTimestamp = Context.GetTimestamp();
				}
			}
			else // Output != nullptr
			{
				if (!Context.HasData(Output->CacheKey(), Context.GetTimestamp()))
				{
					Context.Evaluate(Node, Output);
				}
			}
		}
	}
}


bool FDataflowEditorCommands::OnNodeVerifyTitleCommit(const FText& NewText, UEdGraphNode* GraphNode, FText& OutErrorMessage)
{
	if (GraphNode)
	{
		if (UDataflowEdNode* DataflowNode = Cast<UDataflowEdNode>(GraphNode))
		{
			if (TSharedPtr<Dataflow::FGraph> Graph = DataflowNode->GetDataflowGraph())
			{
				if( Graph->FindBaseNode(FName(NewText.ToString())).Get()==nullptr )
				{
					return true;
				}
			}
		}
		else if( Cast<UEdGraphNode_Comment>(GraphNode))
		{
			return true;
		}
	}
	OutErrorMessage = FText::FromString(FString::Printf(TEXT("Non-unique name for graph node (%s)"), *NewText.ToString()));
	return false;
}


void FDataflowEditorCommands::OnNodeTitleCommitted(const FText& InNewText, ETextCommit::Type InCommitType, UEdGraphNode* GraphNode)
{
	if (InCommitType == ETextCommit::OnCleared)
	{
		return;
	}

	if (GraphNode)
	{
		if (UDataflowEdNode* DataflowNode = Cast<UDataflowEdNode>(GraphNode))
		{
			if (TSharedPtr<Dataflow::FGraph> Graph = DataflowNode->GetDataflowGraph())
			{
				if (TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(DataflowNode->GetDataflowNodeGuid()))
				{
					GraphNode->Rename(*InNewText.ToString());
					Node->SetName(FName(InNewText.ToString()));
				}
			}
		}
		else if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode))
		{
			GraphNode->NodeComment = InNewText.ToString();
		}
	}
}


void FDataflowEditorCommands::OnPropertyValueChanged(UDataflow* OutDataflow, TSharedPtr<Dataflow::FEngineContext>& Context, Dataflow::FTimestamp& OutLastNodeTimestamp, const FPropertyChangedEvent& InPropertyChangedEvent)
{
	if (InPropertyChangedEvent.ChangeType == EPropertyChangeType::ValueSet)
	{
		TSharedPtr<const FDataflowNode> UpdatedNode = nullptr;
		if (OutDataflow && InPropertyChangedEvent.Property && InPropertyChangedEvent.Property->GetOwnerUObject())
		{
			OutDataflow->Modify(true);

			FString Name = InPropertyChangedEvent.Property->GetOwnerUObject()->GetName();
			Name = Name.Left(Name.Len() - FString("DataflowNode").Len());
			{
				FDataflowAssetEdit Edit = OutDataflow->EditDataflow();

				Dataflow::FGraph* Graph = Edit.GetGraph();
				for (auto Node : Graph->GetNodes())
				{
					FString NodeName = Node->GetName().ToString();
					if (NodeName.StartsWith(Name))
					{
						UpdatedNode = Node;
						Node->Invalidate();
					}
				}
			}
		}

		if (!UpdatedNode && Context)
		{
			// Some base properties dont link back to the parent, so just clobber the cache for now. 
			Context.Reset();
		}
		OutLastNodeTimestamp = Dataflow::FTimestamp::Invalid;
	}
}


void FDataflowEditorCommands::DeleteNodes(UDataflow* Graph, const FGraphPanelSelectionSet& SelectedNodes)
{
	if (ensureMsgf(Graph != nullptr, TEXT("Warning : Failed to find valid graph.")))
	{
		for (UObject* Ode : SelectedNodes)
		{
			if (UDataflowEdNode* EdNode = dynamic_cast<UDataflowEdNode*>(Ode))
			{
				if (const TSharedPtr<Dataflow::FGraph> DataflowGraph = EdNode->GetDataflowGraph())
				{
					Graph->RemoveNode(EdNode);
					if (TSharedPtr<FDataflowNode> DataflowNode = DataflowGraph->FindBaseNode(EdNode->GetDataflowNodeGuid()))
					{
						DataflowGraph->RemoveNode(DataflowNode);
					}
				}
			}
			else if (UEdGraphNode_Comment* CommentNode = dynamic_cast<UEdGraphNode_Comment*>(Ode))
			{
				Graph->RemoveNode(CommentNode);
			}
		}
	}
}

void FDataflowEditorCommands::OnSelectedNodesChanged(TSharedPtr<IStructureDetailsView> PropertiesEditor, UObject* Asset, UDataflow* Graph, const TSet<UObject*>& NewSelection)
{
	PropertiesEditor->SetStructureData(nullptr);

	if (Graph && PropertiesEditor)
	{
		if (const TSharedPtr<Dataflow::FGraph> DataflowGraph = Graph->GetDataflow())
		{
			FGraphPanelSelectionSet SelectedNodes = NewSelection;
			if (SelectedNodes.Num())
			{
				TArray<UObject*> Objects;
				for (UObject* SelectedObject : SelectedNodes)
				{
					if (UDataflowEdNode* EdNode = Cast<UDataflowEdNode>(SelectedObject))
					{
						if (TSharedPtr<FDataflowNode> DataflowNode = DataflowGraph->FindBaseNode(EdNode->GetDataflowNodeGuid()))
						{
							TSharedPtr<FStructOnScope> Struct(DataflowNode->NewStructOnScope());
							PropertiesEditor->SetStructureData(Struct);
						}
					}
				}
				
			}
		}
	}
}

void FDataflowEditorCommands::ToggleEnabledState(UDataflow* Graph)
{
}

#undef LOCTEXT_NAMESPACE
