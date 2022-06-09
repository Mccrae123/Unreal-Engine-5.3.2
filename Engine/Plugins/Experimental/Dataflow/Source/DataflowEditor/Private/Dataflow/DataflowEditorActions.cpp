// Copyright Epic Games, Inc. All Rights Reserved.

#include "Dataflow/DataflowEditorActions.h"
#include "Dataflow/DataflowEdNode.h"
#include "Dataflow/DataflowEPropertyCustomizations.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowObject.h"
#include "IStructureDetailsView.h"


#define LOCTEXT_NAMESPACE "DataflowEditorCommands"

void FDataflowEditorCommandsImpl::RegisterCommands()
{
	UI_COMMAND(EvaluateNode, "Evaluate", "Trigger an evaluation of the selected node.", EUserInterfaceActionType::Button, FInputChord());

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



void FDataflowEditorCommands::EvaluateNodes(const FGraphPanelSelectionSet& SelectedNodes, FDataflowEditorCommands::FGraphEvaluationCallback Evaluate)
{
	for (UObject* Ode : SelectedNodes)
	{
		if (UDataflowEdNode* EdNode = dynamic_cast<UDataflowEdNode*>(Ode))
		{
			if (const TSharedPtr<Dataflow::FGraph> DataflowGraph = EdNode->GetDataflowGraph())
			{
				if (const TSharedPtr<FDataflowNode> DataflowNode = DataflowGraph->FindBaseNode(EdNode->GetDataflowNodeGuid()))
				{
					if (DataflowNode->GetOutputs().Num())
					{
						for (Dataflow::FConnection* NodeOutput : DataflowNode->GetOutputs())
						{
							Evaluate(DataflowNode.Get(),NodeOutput);
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

void FDataflowEditorCommands::DeleteNodes(UDataflow* Graph, const FGraphPanelSelectionSet& SelectedNodes)
{
	for (UObject* Ode : SelectedNodes)
	{
		if (UDataflowEdNode* EdNode = dynamic_cast<UDataflowEdNode*>(Ode))
		{
			if (const TSharedPtr<Dataflow::FGraph> DataflowGraph = EdNode->GetDataflowGraph())
			{
				if (TSharedPtr<FDataflowNode> DataflowNode = DataflowGraph->FindBaseNode(EdNode->GetDataflowNodeGuid()))
				{
					Graph->RemoveNode(EdNode);
					DataflowGraph->RemoveNode(DataflowNode);
				}
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
							TSharedPtr<FStructOnScope> Struct(DataflowNode->NewScructOnScope());
							PropertiesEditor->SetStructureData(Struct);
						}
					}
				}
				
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
