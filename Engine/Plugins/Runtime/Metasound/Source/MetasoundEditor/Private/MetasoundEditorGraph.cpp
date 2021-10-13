// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorGraph.h"

#include "AudioParameterInterface.h"
#include "Components/AudioComponent.h"
#include "EdGraph/EdGraphNode.h"
#include "Interfaces/ITargetPlatform.h"
#include "MetasoundEditorGraphBuilder.h"
#include "MetasoundEditorGraphInputNodes.h"
#include "MetasoundEditorGraphNode.h"
#include "MetasoundEditorGraphValidation.h"
#include "MetasoundEditorModule.h"
#include "MetasoundUObjectRegistry.h"
#include "MetasoundVertex.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "MetaSoundEditor"


TArray<UMetasoundEditorGraphNode*> UMetasoundEditorGraphVertex::GetNodes() const
{
	TArray<UMetasoundEditorGraphNode*> Nodes;

	UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter());
	if (ensure(Graph))
	{
		Graph->GetNodesOfClassEx<UMetasoundEditorGraphNode>(Nodes);
		for (int32 i = Nodes.Num() -1; i >= 0; --i)
		{
			UMetasoundEditorGraphNode* Node = Nodes[i];
			if (Node && Node->GetNodeID() != NodeID)
			{
				Nodes.RemoveAtSwap(i, 1, false /* bAllowShrinking */);
			}
		}
	}

	return Nodes;
}

void UMetasoundEditorGraphVertex::SetDescription(const FText& InDescription)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	const FText TransactionLabel = FText::Format(LOCTEXT("SetGraphVertexTooltipFormat", "Set MetaSound {0}'s ToolTip"), GetGraphMemberLabel());
	const FScopedTransaction Transaction(TransactionLabel);

	Modify();
	if (UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter()))
	{
		UObject& MetaSound = Graph->GetMetasoundChecked();
		MetaSound.Modify();

		FNodeHandle NodeHandle = GetNodeHandle();
		NodeHandle->SetDescription(InDescription);

		FGraphBuilder::RegisterGraphWithFrontend(MetaSound);
	}
}

void UMetasoundEditorGraphVertex::SetName(const FName& InNewName)
{
	using namespace Metasound::Frontend;

	const FText TransactionLabel = FText::Format(LOCTEXT("RenameGraphVertexNameFormat", "Rename Metasound {0}"), GetGraphMemberLabel());
	const FScopedTransaction Transaction(TransactionLabel);

	Modify();
	if (UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter()))
	{
		Graph->GetMetasoundChecked().Modify();
	}

	FNodeHandle NodeHandle = GetNodeHandle();
	NodeHandle->SetNodeName(InNewName);

	NameChanged.Broadcast(NodeID);
}

FName UMetasoundEditorGraphVertex::GetMemberName() const
{
	using namespace Metasound::Frontend;

	FConstNodeHandle NodeHandle = GetConstNodeHandle();
	return NodeHandle->GetNodeName();
}

void UMetasoundEditorGraphVertex::SetDisplayName(const FText& InNewName)
{
	using namespace Metasound::Frontend;

	const FText TransactionLabel = FText::Format(LOCTEXT("RenameGraphVertexDisplayNameFormat", "Set Metasound {0} DisplayName"), GetGraphMemberLabel());
	const FScopedTransaction Transaction(TransactionLabel);

	Modify();
	if (UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter()))
	{
		Graph->GetMetasoundChecked().Modify();
	}

	FNodeHandle NodeHandle = GetNodeHandle();
	NodeHandle->SetDisplayName(InNewName);

	NameChanged.Broadcast(NodeID);
}

void UMetasoundEditorGraphVertex::SetDataType(FName InNewType, bool bPostTransaction, bool bRegisterParentGraph)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter());
	if (!ensure(Graph))
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetVertexDataType", "Set MetaSound Variable Type"), bPostTransaction);
	Graph->GetMetasoundChecked().Modify();
	Graph->Modify();

	// 1. Cache current editor input node reference positions & delete nodes.
	TArray<UMetasoundEditorGraphNode*> InputNodes = GetNodes();
	TArray<FVector2D> NodeLocations;
	for (UMetasoundEditorGraphNode* Node : InputNodes)
	{
		if (ensure(Node))
		{
			NodeLocations.Add(FVector2D(Node->NodePosX, Node->NodePosY));
		}
	}

	// 2. Cache the old version's Frontend data.
	FNodeHandle NodeHandle = GetNodeHandle();
	const FName NodeName = NodeHandle->GetNodeName();
	const FText NodeDisplayName = NodeHandle->GetDisplayName();

	// 3. Delete the Frontend variable
	FGraphBuilder::DeleteGraphVertexNodeHandle(*this);

	// 4. Add the new input node with the same identifier data but new datatype.
	UObject& Metasound = Graph->GetMetasoundChecked();
	FNodeHandle NewNodeHandle = AddNodeHandle(NodeName, InNewType);
	NewNodeHandle->SetDisplayName(NodeDisplayName);

	if (!ensure(NewNodeHandle->IsValid()))
	{
		return;
	}

	ClassName = NewNodeHandle->GetClassMetadata().GetClassName();
	NodeID = NewNodeHandle->GetID();
	TypeName = InNewType;

	// 5. Report data type changed immediately after assignment to child
	// class(es) so underlying data can be fixed-up prior to recreating
	// referencing nodes.
	OnDataTypeChanged();

	// 6. Create new node references in the same locations as the old locations
	for (FVector2D Location : NodeLocations)
	{
		FGraphBuilder::AddNode(Metasound, NewNodeHandle, Location, false /* bInSelectNewNode */);
	}

	// Notify now that the node has a new ID (doing so before creating & syncing Frontend Node &
	// EdGraph variable can result in refreshing editors while in a desync'ed state)
	NameChanged.Broadcast(NodeID);

	if (bRegisterParentGraph)
	{
		FGraphBuilder::RegisterGraphWithFrontend(Metasound);
	}
}

Metasound::Frontend::FNodeHandle UMetasoundEditorGraphVertex::GetNodeHandle() const
{
	using namespace Metasound;

	UObject* Object = CastChecked<UMetasoundEditorGraph>(GetOuter())->GetMetasound();
	if (!ensure(Object))
	{
		return Frontend::INodeController::GetInvalidHandle();
	}

	FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Object);
	check(MetasoundAsset);

	return MetasoundAsset->GetRootGraphHandle()->GetNodeWithID(NodeID);
}

Metasound::Frontend::FConstNodeHandle UMetasoundEditorGraphVertex::GetConstNodeHandle() const
{
	return GetNodeHandle();
}

bool UMetasoundEditorGraphVertex::IsRequired() const
{
	using namespace Metasound;

	return GetConstNodeHandle()->IsRequired();
}

bool UMetasoundEditorGraphVertex::CanRename(const FText& InNewName, FText& OutError) const
{
	using namespace Metasound::Frontend;

	if (InNewName.IsEmptyOrWhitespace())
	{
		OutError = FText::Format(LOCTEXT("GraphVertexRenameInvalid_NameEmpty", "{0} cannot be empty string."), InNewName);
		return false;
	}

	if (IsRequired())
	{
		OutError = FText::Format(LOCTEXT("GraphVertexRenameInvalid_GraphVertexRequired", "{0} is required and cannot be renamed."), InNewName);
		return false;
	}

	bool bIsNameValid = true;
	const FName NewFName = *InNewName.ToString();
	FConstNodeHandle NodeHandle = GetConstNodeHandle();
	FConstGraphHandle GraphHandle = NodeHandle->GetOwningGraph();
	GraphHandle->IterateConstNodes([&](FConstNodeHandle NodeToCompare)
	{
		if (NodeID != NodeToCompare->GetID())
		{
			if (NewFName == NodeToCompare->GetNodeName())
			{
				bIsNameValid = false;
				OutError = FText::Format(LOCTEXT("GraphVertexRenameInvalid_NameTaken", "{0} is already in use"), InNewName);
			}
		}
	}, GetClassType());

	return bIsNameValid;
}

void UMetasoundEditorGraphInputLiteral::PostEditUndo()
{
	Super::PostEditUndo();

	if (UMetasoundEditorGraphInput* Input = Cast<UMetasoundEditorGraphInput>(GetOuter()))
	{
		Input->UpdateDocumentInput(false /* bPostTransaction */);
	}
}

void UMetasoundEditorGraphInput::UpdateDocumentInput(bool bPostTransaction)
{
	using namespace Metasound;
	using namespace Metasound::Frontend;

	UMetasoundEditorGraph* MetasoundGraph = CastChecked<UMetasoundEditorGraph>(GetOuter());
	UObject* Metasound = MetasoundGraph->GetMetasound();
	if (!ensure(Metasound))
	{
		return;
	}

	if (!ensure(Literal))
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("Set Input Default", "Set MetaSound Input Default"), bPostTransaction);
	Metasound->Modify();

	FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Metasound);
	check(MetasoundAsset);

	FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();
	FNodeHandle NodeHandle = GraphHandle->GetNodeWithID(NodeID);

	const Metasound::FVertexName& NodeName = NodeHandle->GetNodeName();
	const FGuid VertexID = GraphHandle->GetVertexIDForInputVertex(NodeName);
	GraphHandle->SetDefaultInput(VertexID, Literal->GetDefault());

	// Disabled as internal call to validation to all other open graphs
	// is expensive and can be spammed by dragging values 
// 	Metasound::Editor::FGraphBuilder::RegisterGraphWithFrontend(*Metasound);

	const bool bIsPreviewing = CastChecked<UMetasoundEditorGraph>(GetOuter())->IsPreviewing();
	if (bIsPreviewing)
	{
		UAudioComponent* PreviewComponent = GEditor->GetPreviewAudioComponent();
		check(PreviewComponent);

		if (TScriptInterface<IAudioParameterInterface> ParamInterface = PreviewComponent)
		{
			Metasound::Frontend::FConstNodeHandle ConstNodeHandle = GetConstNodeHandle();
			Metasound::FVertexName VertexKey = NodeHandle->GetNodeName();
			UpdatePreviewInstance(VertexKey, ParamInterface);
		}
	}
}

Metasound::Editor::ENodeSection UMetasoundEditorGraphInput::GetSectionID() const 
{
	return Metasound::Editor::ENodeSection::Inputs;
}

void UMetasoundEditorGraphInput::UpdatePreviewInstance(const Metasound::FVertexName& InParameterName, TScriptInterface<IAudioParameterInterface>& InParamInterface) const
{
	if (ensure(Literal))
	{
		Literal->UpdatePreviewInstance(InParameterName, InParamInterface);
	}
}

Metasound::Frontend::FNodeHandle UMetasoundEditorGraphInput::AddNodeHandle(const FName& InName, FName InDataType)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter());
	if (!ensure(Graph))
	{
		return UMetasoundEditorGraphVertex::AddNodeHandle(InName, InDataType);
	}

	UObject& Metasound = Graph->GetMetasoundChecked();
	Metasound::Frontend::FNodeHandle NewNodeHandle = FGraphBuilder::AddInputNodeHandle(Metasound, InDataType, FText::GetEmpty(), nullptr, &InName);
	return NewNodeHandle;
}

const FText& UMetasoundEditorGraphInput::GetGraphMemberLabel() const
{
	static const FText Label = LOCTEXT("GraphMemberLabel_Input", "Input");
	return Label;
}

void UMetasoundEditorGraphInput::PostEditUndo()
{
	Super::PostEditUndo();

	if (!Literal)
	{
		if (UMetasoundEditorGraph* MetasoundGraph = Cast<UMetasoundEditorGraph>(GetOuter()))
		{
			MetasoundGraph->RemoveGraphMember(*CastChecked<UMetasoundEditorGraphMember>(this));

			if (UObject* Object = MetasoundGraph->GetMetasound())
			{
				Metasound::Editor::FGraphBuilder::RegisterGraphWithFrontend(*Object);
			}
		}
		return;
	}

	UpdateDocumentInput(false /* bPostTransaction */);
}

void UMetasoundEditorGraphInput::OnDataTypeChanged()
{
	using namespace Metasound;
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");
	const FEditorDataType& EditorDataType = EditorModule.FindDataType(TypeName);
	const EMetasoundFrontendLiteralType LiteralType = static_cast<EMetasoundFrontendLiteralType>(EditorDataType.RegistryInfo.PreferredLiteralType);

	TSubclassOf<UMetasoundEditorGraphInputLiteral> InputLiteralClass = EditorModule.FindInputLiteralClass(LiteralType);
	if (!InputLiteralClass)
	{
		InputLiteralClass = UMetasoundEditorGraphInputLiteral::StaticClass();
	}

	if (Literal && Literal->GetClass() != InputLiteralClass)
	{
		Literal = NewObject<UMetasoundEditorGraphInputLiteral>(this, InputLiteralClass, FName(), RF_Transactional);
	}
}

Metasound::Frontend::FNodeHandle UMetasoundEditorGraphOutput::AddNodeHandle(const FName& InName, FName InDataType)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter());
	if (!ensure(Graph))
	{
		return UMetasoundEditorGraphVertex::AddNodeHandle(InName, InDataType);
	}

	UObject& Metasound = Graph->GetMetasoundChecked();
	FNodeHandle NewNodeHandle = FGraphBuilder::AddOutputNodeHandle(Metasound, InDataType, FText::GetEmpty(), &InName);
	return NewNodeHandle;
}

const FText& UMetasoundEditorGraphOutput::GetGraphMemberLabel() const
{
	static const FText Label = LOCTEXT("GraphMemberLabel_Output", "Output");
	return Label;
}

Metasound::Editor::ENodeSection UMetasoundEditorGraphOutput::GetSectionID() const 
{
	return Metasound::Editor::ENodeSection::Outputs;
}

const FText& UMetasoundEditorGraphVariable::GetGraphMemberLabel() const
{
	static const FText Label = LOCTEXT("GraphMemberLabel_Variable", "Variable");
	return Label;
}

Metasound::Frontend::FVariableHandle UMetasoundEditorGraphVariable::GetVariableHandle()
{
	using namespace Metasound;

	UObject* Object = CastChecked<UMetasoundEditorGraph>(GetOuter())->GetMetasound();
	if (!ensure(Object))
	{
		return Frontend::IVariableController::GetInvalidHandle();
	}

	FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Object);
	check(MetasoundAsset);

	return MetasoundAsset->GetRootGraphHandle()->FindVariable(VariableID);
}

Metasound::Frontend::FConstVariableHandle UMetasoundEditorGraphVariable::GetConstVariableHandle() const
{
	using namespace Metasound;

	const UObject* Object = CastChecked<const UMetasoundEditorGraph>(GetOuter())->GetMetasound();
	if (!ensure(Object))
	{
		return Frontend::IVariableController::GetInvalidHandle();
	}

	const FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Object);
	check(MetasoundAsset);

	return MetasoundAsset->GetRootGraphHandle()->FindVariable(VariableID);
}

FName UMetasoundEditorGraphVariable::GetMemberName() const
{
	// Need to determine way to make FNames for variables.
	return NAME_None;
}

Metasound::Editor::ENodeSection UMetasoundEditorGraphVariable::GetSectionID() const 
{ 
	return Metasound::Editor::ENodeSection::Variables;
}

bool UMetasoundEditorGraphVariable::CanRename(const FText& InNewName, FText& OutError) const
{
	using namespace Metasound::Frontend;

	if (InNewName.IsEmptyOrWhitespace())
	{
		OutError = FText::Format(LOCTEXT("GraphVariableRenameInvalid_NameEmpty", "{0} cannot be empty string."), InNewName);
		return false;
	}

	if (IsRequired())
	{
		OutError = FText::Format(LOCTEXT("GraphVariableRenameInvalid_GraphVariableRequired", "{0} is required and cannot be renamed."), InNewName);
		return false;
	}

	bool bIsNameValid = true;
	FString NewNameString = InNewName.ToString();
	FConstVariableHandle VariableHandle = GetConstVariableHandle();
	TArray<FConstVariableHandle> Variables = VariableHandle->GetOwningGraph()->GetVariables();
	for (const FConstVariableHandle& OtherVariable : Variables)
	{
		if (VariableID != OtherVariable->GetID())
		{
			if (InNewName.EqualTo(OtherVariable->GetDisplayName()))
			{
				bIsNameValid = false;
				OutError = FText::Format(LOCTEXT("GraphVariableRenameInvalid_NameTaken", "{0} is already in use"), InNewName);
			}
		}
	}

	return bIsNameValid;
}

bool UMetasoundEditorGraphVariable::IsRequired() const
{
	return false;
}

TArray<UMetasoundEditorGraphNode*> UMetasoundEditorGraphVariable::GetNodes() const 
{
	checkNoEntry();
	return Super::GetNodes();
}
void UMetasoundEditorGraphVariable::SetDescription(const FText& InDescription)
{
	checkNoEntry();
}

void UMetasoundEditorGraphVariable::SetName(const FName& InNewName)
{
	checkNoEntry();
}

void UMetasoundEditorGraphVariable::SetDisplayName(const FText& InNewName)
{
	using namespace Metasound::Frontend;

	const FText TransactionLabel = FText::Format(LOCTEXT("RenameGraphVAriableDisplayNameFormat", "Set Metasound {0} DisplayName"), GetGraphMemberLabel());
	const FScopedTransaction Transaction(TransactionLabel);

	Modify();
	if (UMetasoundEditorGraph* Graph = Cast<UMetasoundEditorGraph>(GetOuter()))
	{
		Graph->GetMetasoundChecked().Modify();
	}

	FVariableHandle VariableHandle = GetVariableHandle();
	VariableHandle->SetDisplayName(InNewName);

	NameChanged.Broadcast(VariableID);
}

void UMetasoundEditorGraphVariable::SetDataType(FName InNewType, bool bPostTransaction, bool bRegisterParentGraph)
{
	checkNoEntry();
}


void UMetasoundEditorGraphVariable::OnDataTypeChanged()
{
	checkNoEntry();
}




#if WITH_EDITOR
void UMetasoundEditorGraphVariable::PostEditUndo()
{
	checkNoEntry();
}

#endif // WITH_EDITOR

UMetasoundEditorGraphInputNode* UMetasoundEditorGraph::CreateInputNode(Metasound::Frontend::FNodeHandle InNodeHandle, bool bInSelectNewNode)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	TArray<FConstOutputHandle> NodeOutputs = InNodeHandle->GetConstOutputs();
	if (!ensure(!NodeOutputs.IsEmpty()))
	{
		return nullptr;
	}

	if (!ensure(InNodeHandle->GetClassMetadata().GetType() == EMetasoundFrontendClassType::Input))
	{
		return nullptr;
	}

	UEdGraphNode* NewEdGraphNode = CreateNode(UMetasoundEditorGraphInputNode::StaticClass(), bInSelectNewNode);
	UMetasoundEditorGraphInputNode* NewInputNode = CastChecked<UMetasoundEditorGraphInputNode>(NewEdGraphNode);
	if (ensure(NewInputNode))
	{
		NewInputNode->CreateNewGuid();
		NewInputNode->PostPlacedNewNode();

		NewInputNode->Input = FindOrAddInput(InNodeHandle);

		if (NewInputNode->Pins.IsEmpty())
		{
			NewInputNode->AllocateDefaultPins();
		}

		return NewInputNode;
	}
	
	return nullptr;	
}

Metasound::Frontend::FDocumentHandle UMetasoundEditorGraph::GetDocumentHandle() const
{
	return GetGraphHandle()->GetOwningDocument();
}

Metasound::Frontend::FGraphHandle UMetasoundEditorGraph::GetGraphHandle() const
{
	FMetasoundAssetBase* MetasoundAsset = Metasound::IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&GetMetasoundChecked());
	check(MetasoundAsset);

	return MetasoundAsset->GetRootGraphHandle();
}

UObject* UMetasoundEditorGraph::GetMetasound() const
{
	return GetOuter();
}

UObject& UMetasoundEditorGraph::GetMetasoundChecked() const
{
	UObject* ParentMetasound = GetOuter();
	check(ParentMetasound);
	return *ParentMetasound;
}

void UMetasoundEditorGraph::RegisterGraphWithFrontend()
{
	using namespace Metasound::Editor;

	if (UObject* ParentMetasound = GetOuter())
	{
		FGraphBuilder::RegisterGraphWithFrontend(*ParentMetasound);
	}
}

bool UMetasoundEditorGraph::Validate(bool bInClearUpdateNotes)
{
	using namespace Metasound::Editor;

	if (UObject* ParentMetasound = GetOuter())
	{
		return FGraphBuilder::ValidateGraph(*ParentMetasound, bInClearUpdateNotes);
	}

	return false;
}

UMetasoundEditorGraphInput* UMetasoundEditorGraph::FindInput(FGuid InNodeID) const
{
	const TObjectPtr<UMetasoundEditorGraphInput>* Input = Inputs.FindByPredicate([InNodeID](const TObjectPtr<UMetasoundEditorGraphInput>& InInput)
	{
		return InInput->NodeID == InNodeID;
	});
	return Input ? Input->Get() : nullptr;
}

UMetasoundEditorGraphInput* UMetasoundEditorGraph::FindInput(FName InName) const
{
	const TObjectPtr<UMetasoundEditorGraphInput>* Input = Inputs.FindByPredicate([InName](const TObjectPtr<UMetasoundEditorGraphInput>& InInput)
	{
		const FName NodeName = InInput->GetNodeHandle()->GetNodeName();
		return NodeName == InName;
	});
	return Input ? Input->Get() : nullptr;
}


UMetasoundEditorGraphInput* UMetasoundEditorGraph::FindOrAddInput(Metasound::Frontend::FNodeHandle InNodeHandle)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	FGraphHandle Graph = InNodeHandle->GetOwningGraph();

	FName TypeName;
	FGuid VertexID;

	ensure(InNodeHandle->GetNumInputs() == 1);
	InNodeHandle->IterateConstInputs([InGraph = &Graph, InTypeName = &TypeName, InVertexID = &VertexID](FConstInputHandle InputHandle)
	{
		*InTypeName = InputHandle->GetDataType();
		*InVertexID = (*InGraph)->GetVertexIDForInputVertex(InputHandle->GetName());
	});

	const FGuid NodeID = InNodeHandle->GetID();
	if (TObjectPtr<UMetasoundEditorGraphInput> Input = FindInput(NodeID))
	{
		ensure(Input->TypeName == TypeName);
		return Input;
	}

	if (UMetasoundEditorGraphInput* NewInput = NewObject<UMetasoundEditorGraphInput>(this, FName(), RF_Transactional))
	{
		NewInput->NodeID = NodeID;
		NewInput->ClassName = InNodeHandle->GetClassMetadata().GetClassName();
		NewInput->TypeName = TypeName;

		FMetasoundFrontendLiteral DefaultLiteral = Graph->GetDefaultInput(VertexID);
		EMetasoundFrontendLiteralType LiteralType = DefaultLiteral.GetType();
		IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");
		TSubclassOf<UMetasoundEditorGraphInputLiteral> InputLiteralClass = EditorModule.FindInputLiteralClass(LiteralType);

		NewInput->Literal = NewObject<UMetasoundEditorGraphInputLiteral>(NewInput, InputLiteralClass, FName(), RF_Transactional);
		NewInput->Literal->SetFromLiteral(DefaultLiteral);

		Inputs.Add(NewInput);
		return NewInput;
	}

	checkNoEntry();
	return nullptr;
}

UMetasoundEditorGraphOutput* UMetasoundEditorGraph::FindOutput(FGuid InNodeID) const
{
	const TObjectPtr<UMetasoundEditorGraphOutput>* Output = Outputs.FindByPredicate([InNodeID](const TObjectPtr<UMetasoundEditorGraphOutput>& InOutput)
	{
		return InOutput->NodeID == InNodeID;
	});
	return Output ? Output->Get() : nullptr;
}

UMetasoundEditorGraphOutput* UMetasoundEditorGraph::FindOutput(FName InName) const
{
	const TObjectPtr<UMetasoundEditorGraphOutput>* Output = Outputs.FindByPredicate([InName](const TObjectPtr<UMetasoundEditorGraphOutput>& InOutput)
	{
		const FName NodeName = InOutput->GetNodeHandle()->GetNodeName();
		return NodeName == InName;
	});
	return Output ? Output->Get() : nullptr;
}

UMetasoundEditorGraphOutput* UMetasoundEditorGraph::FindOrAddOutput(Metasound::Frontend::FNodeHandle InNodeHandle)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	FGraphHandle Graph = InNodeHandle->GetOwningGraph();

	FName TypeName;
	FGuid VertexID;

	ensure(InNodeHandle->GetNumOutputs() == 1);
	InNodeHandle->IterateConstOutputs([InGraph = &Graph, InTypeName = &TypeName, InVertexID = &VertexID](FConstOutputHandle OutputHandle)
	{
		*InTypeName = OutputHandle->GetDataType();
		*InVertexID = (*InGraph)->GetVertexIDForInputVertex(OutputHandle->GetName());
	});

	const FGuid NodeID = InNodeHandle->GetID();
	if (TObjectPtr<UMetasoundEditorGraphOutput> Output = FindOutput(NodeID))
	{
		ensure(Output->TypeName == TypeName);
		return Output;
	}

	if (UMetasoundEditorGraphOutput* NewOutput = NewObject<UMetasoundEditorGraphOutput>(this, FName(), RF_Transactional))
	{
		NewOutput->NodeID = NodeID;
		NewOutput->ClassName = InNodeHandle->GetClassMetadata().GetClassName();
		NewOutput->TypeName = TypeName;
		Outputs.Add(NewOutput);

		return NewOutput;
	}

	checkNoEntry();
	return nullptr;
}

UMetasoundEditorGraphMember* UMetasoundEditorGraph::FindGraphMember(FGuid InNodeID) const
{
	if (UMetasoundEditorGraphOutput* Output = FindOutput(InNodeID))
	{
		return Output;
	}

	return FindInput(InNodeID);
}

UMetasoundEditorGraphMember* UMetasoundEditorGraph::FindAdjacentGraphMember(const UMetasoundEditorGraphMember& InMember)
{
	int32 IndexInArray = Inputs.IndexOfByPredicate([&](const TObjectPtr<UMetasoundEditorGraphInput>& InputMember)
	{
		return &InMember == ToRawPtr(InputMember);
	});

	if (INDEX_NONE != IndexInArray)
	{
		if (IndexInArray < (Inputs.Num() - 1))
		{
			return Inputs[IndexInArray + 1];
		}
		else if (IndexInArray > 0)
		{
			return Inputs[IndexInArray - 1];
		}
		else
		{
			if (Outputs.Num() > 0)
			{
				return Outputs[0];
			}
		}

		return nullptr;
	}

	IndexInArray = Outputs.IndexOfByPredicate([&](const TObjectPtr<UMetasoundEditorGraphOutput>& OutputMember)
	{
		return &InMember == ToRawPtr(OutputMember);
	});

	if (INDEX_NONE != IndexInArray)
	{
		if (IndexInArray < (Outputs.Num() - 1))
		{
			return Outputs[IndexInArray + 1];
		}
		else if (IndexInArray > 0)
		{
			return Outputs[IndexInArray - 1];
		}
		else if (Inputs.Num() > 0)
		{
			return Inputs.Last();
		}

		return nullptr;
	}

	return nullptr;
}

bool UMetasoundEditorGraph::ContainsInput(UMetasoundEditorGraphInput* InInput) const
{
	return Inputs.Contains(InInput);
}

bool UMetasoundEditorGraph::ContainsOutput(UMetasoundEditorGraphOutput* InOutput) const
{
	return Outputs.Contains(InOutput);
}

void UMetasoundEditorGraph::IterateInputs(TUniqueFunction<void(UMetasoundEditorGraphInput&)> InFunction) const
{
	for (UMetasoundEditorGraphInput* Input : Inputs)
	{
		if (ensure(Input))
		{
			InFunction(*Input);
		}
	}
}

void UMetasoundEditorGraph::SetPreviewID(uint32 InPreviewID)
{
	PreviewID = InPreviewID;
}

bool UMetasoundEditorGraph::IsPreviewing() const
{
	UAudioComponent* PreviewComponent = GEditor->GetPreviewAudioComponent();
	if (!PreviewComponent)
	{
		return false;
	}

	if (!PreviewComponent->IsPlaying())
	{
		return false;
	}

	return PreviewComponent->GetUniqueID() == PreviewID;
}

void UMetasoundEditorGraph::IterateOutputs(TUniqueFunction<void(UMetasoundEditorGraphOutput&)> InFunction) const
{
	for (UMetasoundEditorGraphOutput* Output : Outputs)
	{
		if (ensure(Output))
		{
			InFunction(*Output);
		}
	}
}

bool UMetasoundEditorGraph::RemoveGraphMember(UMetasoundEditorGraphMember& InGraphMember)
{
	if (UMetasoundEditorGraphInput* Input = Cast<UMetasoundEditorGraphInput>(&InGraphMember))
	{
		if (!Inputs.Remove(Input))
		{
			return false;
		}
	}
	else if (UMetasoundEditorGraphOutput* Output = Cast<UMetasoundEditorGraphOutput>(&InGraphMember))
	{
		if (!Outputs.Remove(Output))
		{
			return false;
		}
	}

	return true;
}

bool UMetasoundEditorGraph::ValidateInternal(Metasound::Editor::FGraphValidationResults& OutResults, bool bClearUpgradeMessaging)
{
	using namespace Metasound::Editor;
	using namespace Metasound::Frontend;

	bool bMarkDirty = false;
	bool bIsValid = true;

	OutResults = FGraphValidationResults();

	TArray<UMetasoundEditorGraphExternalNode*> ExternalNodes;
	GetNodesOfClass<UMetasoundEditorGraphExternalNode>(ExternalNodes);
	for (UMetasoundEditorGraphExternalNode* ExternalNode : ExternalNodes)
	{
		FGraphNodeValidationResult NodeResult(*ExternalNode);
		bIsValid &= ExternalNode->Validate(NodeResult, bClearUpgradeMessaging);
		bMarkDirty |= NodeResult.bIsDirty;
		OutResults.NodeResults.Add(NodeResult);
	}

	if (bMarkDirty)
	{
		MarkPackageDirty();
	}

	return bIsValid;
}
#undef LOCTEXT_NAMESPACE
