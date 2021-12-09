// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusDeformer.h"

#include "Actions/OptimusNodeActions.h"
#include "Actions/OptimusNodeGraphActions.h"
#include "Actions/OptimusResourceActions.h"
#include "Actions/OptimusVariableActions.h"
#include "Containers/Queue.h"
#include "OptimusActionStack.h"
#include "OptimusKernelSource.h"
#include "OptimusDataTypeRegistry.h"
#include "OptimusNodeGraph.h"
#include "OptimusResourceDescription.h"
#include "OptimusVariableDescription.h"
#include "OptimusHelpers.h"
#include "OptimusNode.h"
#include "OptimusNodePin.h"
#include "OptimusDeveloperModule.h"
#include "OptimusObjectVersion.h"
#include "Types/OptimusType_ShaderText.h"
#include "ComputeFramework/ComputeKernel.h"
#include "DataInterfaces/DataInterfaceRawBuffer.h"
#include "Nodes/OptimusNode_DataInterface.h"
#include "IOptimusComputeKernelProvider.h"
#include "OptimusFunctionNodeGraph.h"
#include "Misc/UObjectToken.h"

#include "RenderingThread.h"
#include "Nodes/OptimusNode_ConstantValue.h"
#include "UObject/Package.h"
#include "Internationalization/Regex.h"
#include "Nodes/OptimusNode_ComputeKernelFunction.h"
#include "Nodes/OptimusNode_CustomComputeKernel.h"

#define LOCTEXT_NAMESPACE "OptimusDeformer"

static const FName DefaultResourceName("Resource");
static const FName DefaultVariableName("Variable");


UOptimusDeformer::UOptimusDeformer()
{
	UOptimusNodeGraph *UpdateGraph = CreateDefaultSubobject<UOptimusNodeGraph>(UOptimusNodeGraph::UpdateGraphName);
	UpdateGraph->SetGraphType(EOptimusNodeGraphType::Update);
	Graphs.Add(UpdateGraph);

	ActionStack = CreateDefaultSubobject<UOptimusActionStack>(TEXT("ActionStack"));
}


UOptimusNodeGraph* UOptimusDeformer::AddSetupGraph()
{
	FOptimusNodeGraphAction_AddGraph* AddGraphAction = 
		new FOptimusNodeGraphAction_AddGraph(this, EOptimusNodeGraphType::Setup, UOptimusNodeGraph::SetupGraphName, 0);

	if (GetActionStack()->RunAction(AddGraphAction))
	{
		return AddGraphAction->GetGraph(this);
	}
	else
	{
		return nullptr;
	}
}


UOptimusNodeGraph* UOptimusDeformer::AddTriggerGraph(const FString &InName)
{
	if (!UOptimusNodeGraph::IsValidUserGraphName(InName))
	{
		return nullptr;
	}

	FOptimusNodeGraphAction_AddGraph* AddGraphAction =
	    new FOptimusNodeGraphAction_AddGraph(this, EOptimusNodeGraphType::ExternalTrigger, *InName, INDEX_NONE);

	if (GetActionStack()->RunAction(AddGraphAction))
	{
		return AddGraphAction->GetGraph(this);
	}
	else
	{
		return nullptr;
	}
}


UOptimusNodeGraph* UOptimusDeformer::GetUpdateGraph() const
{
	for (UOptimusNodeGraph* Graph: Graphs)
	{
		if (Graph->GetGraphType() == EOptimusNodeGraphType::Update)
		{
			return Graph;
		}
	}
	UE_LOG(LogOptimusDeveloper, Fatal, TEXT("No upgrade graph on deformer (%s)."), *GetPathName());
	return nullptr;
}


bool UOptimusDeformer::RemoveGraph(UOptimusNodeGraph* InGraph)
{
    return GetActionStack()->RunAction<FOptimusNodeGraphAction_RemoveGraph>(InGraph);
}


UOptimusVariableDescription* UOptimusDeformer::AddVariable(
	FOptimusDataTypeRef InDataTypeRef, 
	FName InName /*= NAME_None */
	)
{
	if (InName.IsNone())
	{
		InName = DefaultVariableName;
	}

	if (!InDataTypeRef.IsValid())
	{
		// Default to float.
		InDataTypeRef.Set(FOptimusDataTypeRegistry::Get().FindType(*FFloatProperty::StaticClass()));
	}

	// Is this data type compatible with resources?
	FOptimusDataTypeHandle DataType = InDataTypeRef.Resolve();
	if (!DataType.IsValid() || !EnumHasAnyFlags(DataType->UsageFlags, EOptimusDataTypeUsageFlags::Variable))
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Invalid data type for variables."));
		return nullptr;
	}

	FOptimusVariableAction_AddVariable* AddVariabAction =
	    new FOptimusVariableAction_AddVariable(this, InDataTypeRef, InName);

	if (GetActionStack()->RunAction(AddVariabAction))
	{
		return AddVariabAction->GetVariable(this);
	}
	else
	{
		return nullptr;
	}
}


bool UOptimusDeformer::RemoveVariable(
	UOptimusVariableDescription* InVariableDesc
	)
{
	if (!ensure(InVariableDesc))
	{
		return false;
	}
	if (InVariableDesc->GetOuter() != this)
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Variable not owned by this deformer."));
		return false;
	}

	return GetActionStack()->RunAction<FOptimusVariableAction_RemoveVariable>(InVariableDesc);
}


bool UOptimusDeformer::RenameVariable(
	UOptimusVariableDescription* InVariableDesc, 
	FName InNewName
	)
{
	if (InNewName.IsNone())
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Invalid resource name."));
		return false;
	}
	if (InVariableDesc->GetOuter() != this)
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Variable not owned by this deformer."));
		return false;
	}

	return GetActionStack()->RunAction<FOptimusVariableAction_RenameVariable>(InVariableDesc, InNewName);
}


UOptimusVariableDescription* UOptimusDeformer::ResolveVariable(
	FName InVariableName
	) const
{
	for (UOptimusVariableDescription* Variable : GetVariables())
	{
		if (Variable->GetFName() == InVariableName)
		{
			return Variable;
		}
	}
	return nullptr;
}


UOptimusVariableDescription* UOptimusDeformer::CreateVariableDirect(
	FName InName
	)
{
	if (InName.IsNone())
	{
		InName = DefaultResourceName;
	}

	// If there's already an object with this name, then attempt to make the name unique.
	InName = Optimus::GetUniqueNameForScopeAndClass(this, UOptimusVariableDescription::StaticClass(), InName);

	UOptimusVariableDescription* Variable = NewObject<UOptimusVariableDescription>(this, UOptimusVariableDescription::StaticClass(), InName, RF_Transactional);

	// Make sure to give this variable description a unique GUID. We use this when updating the
	// class.
	Variable->Guid = FGuid::NewGuid();
	
	MarkPackageDirty();

	return Variable;
}


bool UOptimusDeformer::AddVariableDirect(
	UOptimusVariableDescription* InVariableDesc
	)
{
	if (!ensure(InVariableDesc))
	{
		return false;
	}

	if (!ensure(InVariableDesc->GetOuter() == this))
	{
		return false;
	}


	VariableDescriptions.Add(InVariableDesc);

	Notify(EOptimusGlobalNotifyType::VariableAdded, InVariableDesc);

	return true;
}


bool UOptimusDeformer::RemoveVariableDirect(
	UOptimusVariableDescription* InVariableDesc
	)
{
	// Do we actually own this resource?
	int32 ResourceIndex = VariableDescriptions.Add(InVariableDesc);
	if (ResourceIndex == INDEX_NONE)
	{
		return false;
	}

	VariableDescriptions.RemoveAt(ResourceIndex);

	Notify(EOptimusGlobalNotifyType::VariableRemoved, InVariableDesc);

	InVariableDesc->Rename(nullptr, GetTransientPackage());
	InVariableDesc->MarkAsGarbage();

	MarkPackageDirty();

	return true;
}


bool UOptimusDeformer::RenameVariableDirect(
	UOptimusVariableDescription* InVariableDesc, 
	FName InNewName
	)
{
	// Do we actually own this variable?
	int32 ResourceIndex = VariableDescriptions.IndexOfByKey(InVariableDesc);
	if (ResourceIndex == INDEX_NONE)
	{
		return false;
	}

	InNewName = Optimus::GetUniqueNameForScopeAndClass(this, UOptimusVariableDescription::StaticClass(), InNewName);

	bool bChanged = false;
	if (InVariableDesc->VariableName != InNewName)
	{
		InVariableDesc->Modify();
		InVariableDesc->VariableName = InNewName;
		bChanged = true;
	}

	if (InVariableDesc->GetFName() != InNewName)
	{
		InVariableDesc->Rename(*InNewName.ToString(), nullptr);
		bChanged = true;
	}

	if (bChanged)
	{
		Notify(EOptimusGlobalNotifyType::VariableRenamed, InVariableDesc);

		MarkPackageDirty();
	}

	return bChanged;
}


UOptimusResourceDescription* UOptimusDeformer::AddResource(
	FOptimusDataTypeRef InDataTypeRef,
	FName InName
	)
{
	if (InName.IsNone())
	{
		InName = DefaultResourceName;
	}

	if (!InDataTypeRef.IsValid())
	{
		// Default to float.
		InDataTypeRef.Set(FOptimusDataTypeRegistry::Get().FindType(*FFloatProperty::StaticClass()));
	}

	// Is this data type compatible with resources?
	FOptimusDataTypeHandle DataType = InDataTypeRef.Resolve();
	if (!DataType.IsValid() || !EnumHasAnyFlags(DataType->UsageFlags, EOptimusDataTypeUsageFlags::Resource))
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Invalid data type for resources."));
		return nullptr;
	}

	FOptimusResourceAction_AddResource *AddResourceAction = 	
	    new FOptimusResourceAction_AddResource(this, InDataTypeRef, InName);

	if (GetActionStack()->RunAction(AddResourceAction))
	{
		return AddResourceAction->GetResource(this);
	}
	else
	{
		return nullptr;
	}
}


bool UOptimusDeformer::RemoveResource(UOptimusResourceDescription* InResourceDesc)
{
	if (!ensure(InResourceDesc))
	{
		return false;
	}
	if (InResourceDesc->GetOuter() != this)
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Resource not owned by this deformer."));
		return false;
	}

	return GetActionStack()->RunAction<FOptimusResourceAction_RemoveResource>(InResourceDesc);
}


bool UOptimusDeformer::RenameResource(
	UOptimusResourceDescription* InResourceDesc, 
	FName InNewName
	)
{
	if (InNewName.IsNone())
	{
		UE_LOG(LogOptimusDeveloper, Error, TEXT("Invalid resource name."));
		return false;
	}

	return GetActionStack()->RunAction<FOptimusResourceAction_RenameResource>(InResourceDesc, InNewName);
}


UOptimusResourceDescription* UOptimusDeformer::ResolveResource(
	FName InResourceName
	) const
{
	for (UOptimusResourceDescription* Resource : GetResources())
	{
		if (Resource->GetFName() == InResourceName)
		{
			return Resource;
		}
	}
	return nullptr;
}


UOptimusResourceDescription* UOptimusDeformer::CreateResourceDirect(
	FName InName
	)
{
	if (InName.IsNone())
	{
		InName = DefaultResourceName;
	}

	// If there's already an object with this name, then attempt to make the name unique.
	InName = Optimus::GetUniqueNameForScopeAndClass(this, UOptimusResourceDescription::StaticClass(), InName);

	UOptimusResourceDescription* Resource = NewObject<UOptimusResourceDescription>(this, UOptimusResourceDescription::StaticClass(), InName, RF_Transactional);

	MarkPackageDirty();
	
	return Resource;
}


bool UOptimusDeformer::AddResourceDirect(UOptimusResourceDescription* InResourceDesc)
{
	if (!ensure(InResourceDesc))
	{
		return false;
	}

	if (!ensure(InResourceDesc->GetOuter() == this))
	{
		return false;
	}


	ResourceDescriptions.Add(InResourceDesc);

	Notify(EOptimusGlobalNotifyType::ResourceAdded, InResourceDesc);

	return true;
}


bool UOptimusDeformer::RemoveResourceDirect(
	UOptimusResourceDescription* InResourceDesc
	)
{
	// Do we actually own this resource?
	int32 ResourceIndex = ResourceDescriptions.IndexOfByKey(InResourceDesc);
	if (ResourceIndex == INDEX_NONE)
	{
		return false;
	}

	ResourceDescriptions.RemoveAt(ResourceIndex);

	Notify(EOptimusGlobalNotifyType::ResourceRemoved, InResourceDesc);

	InResourceDesc->Rename(nullptr, GetTransientPackage());
	InResourceDesc->MarkAsGarbage();

	MarkPackageDirty();

	return true;
}


bool UOptimusDeformer::RenameResourceDirect(
	UOptimusResourceDescription* InResourceDesc, 
	FName InNewName
	)
{
	// Do we actually own this resource?
	int32 ResourceIndex = ResourceDescriptions.IndexOfByKey(InResourceDesc);
	if (ResourceIndex == INDEX_NONE)
	{
		return false;
	}
	
	InNewName = Optimus::GetUniqueNameForScopeAndClass(this, UOptimusResourceDescription::StaticClass(), InNewName);

	bool bChanged = false;
	if (InResourceDesc->ResourceName != InNewName)
	{
		InResourceDesc->Modify();
		InResourceDesc->ResourceName = InNewName;
		bChanged = true;
	}

	if (InResourceDesc->GetFName() != InNewName)
	{
		InResourceDesc->Rename(*InNewName.ToString(), nullptr);
		bChanged = true;
	}

	if (bChanged)
	{
		Notify(EOptimusGlobalNotifyType::ResourceRenamed, InResourceDesc);

		MarkPackageDirty();
	}

	return bChanged;
}

// Do a breadth-first collection of nodes starting from the seed nodes (terminal data interfaces).
struct FNodeWithTraversalContext
{
	const UOptimusNode* Node;
	FOptimusPinTraversalContext TraversalContext;
};

static void CollectNodes(
	const TArray<const UOptimusNode*>& InSeedNodes,
	TArray<FNodeWithTraversalContext>& OutCollectedNodes
	)
{
	TSet<const UOptimusNode*> VisitedNodes;
	TQueue<FNodeWithTraversalContext> WorkingSet;

	for (const UOptimusNode* Node: InSeedNodes)
	{
		WorkingSet.Enqueue({Node, FOptimusPinTraversalContext{}});
		VisitedNodes.Add(Node);
		OutCollectedNodes.Add({Node, FOptimusPinTraversalContext{}});
	}

	FNodeWithTraversalContext WorkItem;
	while (WorkingSet.Dequeue(WorkItem))
	{
		// Traverse in the direction of input pins (up the graph).
		for (const UOptimusNodePin* Pin: WorkItem.Node->GetPins())
		{
			if (Pin->GetDirection() == EOptimusNodePinDirection::Input)
			{
				for (const FOptimusRoutedNodePin& ConnectedPin: Pin->GetConnectedPinsWithRouting(WorkItem.TraversalContext))
				{
					if (ensure(ConnectedPin.NodePin != nullptr))
					{
						const UOptimusNode *NextNode = ConnectedPin.NodePin->GetOwningNode();
						FNodeWithTraversalContext CollectedNode{NextNode, ConnectedPin.TraversalContext};
						if (!VisitedNodes.Contains(NextNode))
						{
							WorkingSet.Enqueue(CollectedNode);
							VisitedNodes.Add(NextNode);
							OutCollectedNodes.Add(CollectedNode);
						}
					}
				}
			}
		}	
	}	
}



bool UOptimusDeformer::Compile()
{
	int32 UpdateGraphIndex = -1;
	const UOptimusNodeGraph* UpdateGraph = nullptr;
	for (int32 GraphIndex = 0; GraphIndex < Graphs.Num(); ++GraphIndex)
	{
		const UOptimusNodeGraph* NodeGraph = Graphs[GraphIndex];
		if (NodeGraph->GetGraphType() == EOptimusNodeGraphType::Update)
		{
			UpdateGraph = NodeGraph;
			UpdateGraphIndex = GraphIndex;
			break;
		}
	}
	if (!UpdateGraph)
	{
		CompileBeginDelegate.Broadcast(this);
		CompileMessageDelegate.Broadcast(
			FTokenizedMessage::Create(EMessageSeverity::CriticalError, LOCTEXT("NoGraphFound", "No update graph found. Compilation aborted.")));
		CompileEndDelegate.Broadcast(this);
		return false;
	}
	
	// HACK: Find an interface node that has no output pins. That's our terminal node.
	// FIXME: Resource nodes can be terminals too.
	TArray<const UOptimusNode*> TerminalNodes;
	
	for (const UOptimusNode* Node: UpdateGraph->GetAllNodes())
	{
		const UOptimusNode_DataInterface* TerminalNode = Cast<const UOptimusNode_DataInterface>(Node);

		if (TerminalNode)
		{
			for (const UOptimusNodePin* Pin: Node->GetPins())
			{
				if (Pin->GetDirection() == EOptimusNodePinDirection::Output)
				{
					TerminalNode = nullptr;
					break;
				}
			}
		}
		if (TerminalNode)
		{
			TerminalNodes.Add(TerminalNode);
		}
	}

	if (TerminalNodes.IsEmpty())
	{
		CompileBeginDelegate.Broadcast(this);
		CompileMessageDelegate.Broadcast(
			FTokenizedMessage::Create(EMessageSeverity::CriticalError, LOCTEXT("NoDataInterfaceFound", "No data interface terminal nodes found. Compilation aborted.")));
		CompileEndDelegate.Broadcast(this);
		return false;
	}

	CompileBeginDelegate.Broadcast(this);

	// Wait for rendering to be done.
	FlushRenderingCommands();
	
	// Clean out any existing data.
	KernelInvocations.Reset();
	DataInterfaces.Reset();
	GraphEdges.Reset();
	CompilingKernelToNode.Reset();
	AllParameterBindings.Reset();

	TArray<FNodeWithTraversalContext> ConnectedNodes;
	CollectNodes(TerminalNodes, ConnectedNodes);

	// Since we now have the connected nodes in a breadth-first list, reverse the list which
	// will give use the same list but topologically sorted in kernel execution order.
	Algo::Reverse(ConnectedNodes.GetData(), ConnectedNodes.Num());

	// Find all data interface nodes and create their data interfaces.
	FOptimus_NodeToDataInterfaceMap NodeDataInterfaceMap;

	// Find all resource links from one compute kernel directly to another. The pin here is
	// the output pin from a kernel node that connects to another. We don't map from input pins
	// because a resource output may be used multiple times, but only written into once.
	FOptimus_PinToDataInterfaceMap LinkDataInterfaceMap;

	// Find all value nodes (constant and variable) 
	TSet<const UOptimusNode *> ValueNodeSet; 
	
	for (FNodeWithTraversalContext ConnectedNode: ConnectedNodes)
	{
		if (const UOptimusNode_DataInterface* DataInterfaceNode = Cast<const UOptimusNode_DataInterface>(ConnectedNode.Node))
		{
			UOptimusComputeDataInterface* DataInterface =
				NewObject<UOptimusComputeDataInterface>(this, DataInterfaceNode->GetDataInterfaceClass());

			NodeDataInterfaceMap.Add(ConnectedNode.Node, DataInterface);
		}
		else if (Cast<const IOptimusComputeKernelProvider>(ConnectedNode.Node) != nullptr)
		{
			for (const UOptimusNodePin* Pin: ConnectedNode.Node->GetPins())
			{
				if (Pin->GetDirection() == EOptimusNodePinDirection::Output &&
					ensure(Pin->GetStorageType() == EOptimusNodePinStorageType::Resource) &&
					!LinkDataInterfaceMap.Contains(Pin))
				{
					for (const FOptimusRoutedNodePin& ConnectedPin: Pin->GetConnectedPinsWithRouting(ConnectedNode.TraversalContext))
					{
						// Make sure it connects to another kernel node.
						if (Cast<const IOptimusComputeKernelProvider>(ConnectedPin.NodePin->GetOwningNode()) != nullptr &&
							ensure(Pin->GetDataType().IsValid()))
						{
							UTransientBufferDataInterface* TransientBufferDI =
								NewObject<UTransientBufferDataInterface>(this);

							TransientBufferDI->ValueType = Pin->GetDataType()->ShaderValueType;
							LinkDataInterfaceMap.Add(Pin, TransientBufferDI);
						}
					}
				}
			}
		}
		// TBD: Add common base class for variable and value nodes that expose a virtual for evaluating the value
		// and getting the value type.
		else if (const UOptimusNode_ConstantValue* ValueNode = Cast<const UOptimusNode_ConstantValue>(ConnectedNode.Node))
		{
			ValueNodeSet.Add(ValueNode);
		}
	}

	// Loop through all kernels, create a kernel source, and create a compute kernel for it.
	struct FKernelWithDataBindings
	{
		int32 KernelNodeIndex;
		UComputeKernel *Kernel;
		FOptimus_InterfaceBindingMap InputDataBindings;
		FOptimus_InterfaceBindingMap OutputDataBindings;
	};
	
	TArray<FKernelWithDataBindings> BoundKernels;
	for (FNodeWithTraversalContext ConnectedNode: ConnectedNodes)
	{
		if (const IOptimusComputeKernelProvider *KernelProvider = Cast<const IOptimusComputeKernelProvider>(ConnectedNode.Node))
		{
			FOptimus_KernelParameterBindingList KernelParameterBindings;
			FKernelWithDataBindings BoundKernel;

			BoundKernel.KernelNodeIndex = UpdateGraph->Nodes.IndexOfByKey(ConnectedNode.Node);
			BoundKernel.Kernel = NewObject<UComputeKernel>(this);

			UComputeKernelSource *KernelSource = KernelProvider->CreateComputeKernel(
				BoundKernel.Kernel, ConnectedNode.TraversalContext,
				NodeDataInterfaceMap, LinkDataInterfaceMap,
				ValueNodeSet, KernelParameterBindings, BoundKernel.InputDataBindings, BoundKernel.OutputDataBindings
			);
			if (!KernelSource)
			{
				TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(EMessageSeverity::CriticalError,
					LOCTEXT("CantCreateKernel", "Unable to create compute kernel from kernel node. Compilation aborted."));
				Message->AddToken(FUObjectToken::Create(ConnectedNode.Node));
				CompileMessageDelegate.Broadcast(Message);
				CompileEndDelegate.Broadcast(this);
				return false;
			}

			if (BoundKernel.InputDataBindings.IsEmpty() || BoundKernel.OutputDataBindings.IsEmpty())
			{
				TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(EMessageSeverity::CriticalError,
					LOCTEXT("KernelHasNoBindings", "Kernel has either no input or output bindings. Compilation aborted."));
				Message->AddToken(FUObjectToken::Create(ConnectedNode.Node));
				CompileMessageDelegate.Broadcast(Message);
				CompileEndDelegate.Broadcast(this);
				return false;
			}
			
			BoundKernel.Kernel->KernelSource = KernelSource;

			for (int32 ParameterIndex = 0; ParameterIndex < KernelParameterBindings.Num(); ParameterIndex++)
			{
				const FOptimus_KernelParameterBinding& Binding = KernelParameterBindings[ParameterIndex];
				FOptimus_ShaderParameterBinding ShaderParameterBinding;
				ShaderParameterBinding.ValueNode = Binding.ValueNode;
				ShaderParameterBinding.KernelIndex = BoundKernels.Num();
				ShaderParameterBinding.ParameterIndex = ParameterIndex;
				AllParameterBindings.Add(ShaderParameterBinding);
			}

			BoundKernels.Add(BoundKernel);

			KernelInvocations.Add(BoundKernel.Kernel);
			CompilingKernelToNode.Add(ConnectedNode.Node);
		}
	}

	// Now that we've collected all the pieces, time to line them up.
	for (TPair<const UOptimusNode *, UOptimusComputeDataInterface *>&Item: NodeDataInterfaceMap)
	{
		DataInterfaces.Add(Item.Value);
	}
	for (TPair<const UOptimusNodePin *, UOptimusComputeDataInterface *>&Item: LinkDataInterfaceMap)
	{
		DataInterfaces.Add(Item.Value);
	}

	// Create the graph edges.
	for (int32 KernelIndex = 0; KernelIndex < KernelInvocations.Num(); KernelIndex++)
	{
		const FKernelWithDataBindings& BoundKernel = BoundKernels[KernelIndex];
		const TArray<FShaderFunctionDefinition>& KernelInputs = BoundKernel.Kernel->KernelSource->ExternalInputs;

		// FIXME: Hoist these two loops into a helper function/lambda.
		for (const TPair<int32, FOptimus_InterfaceBinding>& DataBinding: BoundKernel.InputDataBindings)
		{
			const int32 KernelBindingIndex = DataBinding.Key;
			const FOptimus_InterfaceBinding& InterfaceBinding = DataBinding.Value;
			const UOptimusComputeDataInterface* DataInterface = InterfaceBinding.DataInterface;
			const int32 DataInterfaceBindingIndex = InterfaceBinding.DataInterfaceBindingIndex;
			const FString BindingFunctionName = InterfaceBinding.BindingFunctionName;

			// FIXME: Collect this beforehand.
			TArray<FShaderFunctionDefinition> DataInterfaceFunctions;
			DataInterface->GetSupportedInputs(DataInterfaceFunctions);
			
			if (ensure(KernelInputs.IsValidIndex(KernelBindingIndex)) &&
				ensure(DataInterfaceFunctions.IsValidIndex(DataInterfaceBindingIndex)))
			{
				FComputeGraphEdge GraphEdge;
				GraphEdge.bKernelInput = true;
				GraphEdge.KernelIndex = KernelIndex;
				GraphEdge.KernelBindingIndex = KernelBindingIndex;
				GraphEdge.DataInterfaceIndex = DataInterfaces.IndexOfByKey(DataInterface);
				GraphEdge.DataInterfaceBindingIndex = DataInterfaceBindingIndex;
				GraphEdge.BindingFunctionNameOverride = BindingFunctionName;
				GraphEdges.Add(GraphEdge);
			}
		}

		const TArray<FShaderFunctionDefinition>& KernelOutputs = BoundKernels[KernelIndex].Kernel->KernelSource->ExternalOutputs;
		for (const TPair<int32, FOptimus_InterfaceBinding>& DataBinding: BoundKernel.OutputDataBindings)
		{
			const int32 KernelBindingIndex = DataBinding.Key;
			const FOptimus_InterfaceBinding& InterfaceBinding = DataBinding.Value;
			const UOptimusComputeDataInterface* DataInterface = InterfaceBinding.DataInterface;
			const int32 DataInterfaceBindingIndex = InterfaceBinding.DataInterfaceBindingIndex;
			const FString BindingFunctionName = InterfaceBinding.BindingFunctionName;

			// FIXME: Collect this beforehand.
			TArray<FShaderFunctionDefinition> DataInterfaceFunctions;
			DataInterface->GetSupportedOutputs(DataInterfaceFunctions);
			
			if (ensure(KernelOutputs.IsValidIndex(KernelBindingIndex)) &&
				ensure(DataInterfaceFunctions.IsValidIndex(DataInterfaceBindingIndex)))
			{
				FComputeGraphEdge GraphEdge;
				GraphEdge.bKernelInput = false;
				GraphEdge.KernelIndex = KernelIndex;
				GraphEdge.KernelBindingIndex = KernelBindingIndex;
				GraphEdge.DataInterfaceIndex = DataInterfaces.IndexOfByKey(DataInterface);
				GraphEdge.DataInterfaceBindingIndex = DataInterfaceBindingIndex;
				GraphEdge.BindingFunctionNameOverride = BindingFunctionName;
				GraphEdges.Add(GraphEdge);
			}
		}
	}

	// Let folks know _before_ we update resources.
	CompileEndDelegate.Broadcast(this);

	UpdateResources();
	
	return true;
}


void UOptimusDeformer::OnKernelCompilationComplete(int32 InKernelIndex, const TArray<FString>& InCompileErrors)
{
	// Find the Optimus objects from the raw kernel index.
	if (CompilingKernelToNode.IsValidIndex(InKernelIndex))
	{
		UOptimusNode* Node = const_cast<UOptimusNode*>(CompilingKernelToNode[InKernelIndex].Get());
		
		if (ensure(Node))
		{
			IOptimusComputeKernelProvider* KernelProvider = Cast<IOptimusComputeKernelProvider>(Node);
			if (ensure(KernelProvider != nullptr))
			{
				TArray<FOptimusType_CompilerDiagnostic>  Diagnostics;

				// This is a compute kernel as expected so broadcast the compile errors.
				for (FString const& CompileError : InCompileErrors)
				{
					FOptimusType_CompilerDiagnostic Diagnostic = ProcessCompilationMessage(Node, CompileError);
					if (Diagnostic.Level != EOptimusDiagnosticLevel::None)
					{
						Diagnostics.Add(Diagnostic);
					}
				}

				KernelProvider->SetCompilationDiagnostics(Diagnostics);
			}
		}
	}
}


FOptimusType_CompilerDiagnostic UOptimusDeformer::ProcessCompilationMessage(
		const UOptimusNode* InKernelNode,
		const FString& InMessage
		)
{
	// "/Engine/Generated/ComputeFramework/Kernel_LinearBlendSkinning.usf(19,39-63):  error X3013: 'DI000_ReadNumVertices': no matching 1 parameter function"	
	// "OptimusNode_ComputeKernel_2(1,42):  error X3004: undeclared identifier 'a'"

	// TODO: Parsing diagnostics rightfully belongs at the shader compiler level, especially if
	// the shader compiler is rewriting.
	static const FRegexPattern MessagePattern(TEXT(R"(^\s*(.*?)\((\d+),(\d+)(-(\d+))?\):\s*(error|warning)\s+[A-Z0-9]+:\s*(.*)$)"));

	FRegexMatcher Matcher(MessagePattern, InMessage);
	if (!Matcher.FindNext())
	{
		UE_LOG(LogOptimusDeveloper, Warning, TEXT("Cannot parse message from shader compiler: [%s]"), *InMessage);
		return {};
	}

	// FString NodeName = Matcher.GetCaptureGroup(1);
	const int32 LineNumber = FCString::Atoi(*Matcher.GetCaptureGroup(2));
	const int32 ColumnStart = FCString::Atoi(*Matcher.GetCaptureGroup(3));
	const FString ColumnEndStr = Matcher.GetCaptureGroup(5);
	const int32 ColumnEnd = ColumnEndStr.IsEmpty() ? ColumnStart : FCString::Atoi(*ColumnEndStr);
	const FString SeverityStr = Matcher.GetCaptureGroup(6);
	const FString MessageStr = Matcher.GetCaptureGroup(7);

	EMessageSeverity::Type Severity = EMessageSeverity::Error; 
	EOptimusDiagnosticLevel Level = EOptimusDiagnosticLevel::Error;
	if (SeverityStr == TEXT("warning"))
	{
		Level = EOptimusDiagnosticLevel::Warning;
		Severity = EMessageSeverity::Warning;
	}

	// Set a dummy lambda for token activation because the default behavior for FUObjectToken is
	// to pop up the asset browser :-/
	static auto DummyActivation = [](const TSharedRef<class IMessageToken>&) {};
	FString DiagnosticStr = FString::Printf(TEXT("%s (line %d)"), *MessageStr, LineNumber);
	TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity, FText::FromString(DiagnosticStr));
	Message->AddToken(FUObjectToken::Create(InKernelNode)->OnMessageTokenActivated(FOnMessageTokenActivated::CreateLambda(DummyActivation)));
	CompileMessageDelegate.Broadcast(Message);

	return FOptimusType_CompilerDiagnostic(Level, MessageStr, LineNumber, ColumnStart, ColumnEnd);
}

template<typename Allocator>
static void StringViewSplit(
	TArray<FStringView, Allocator> &OutResult, 
	const FStringView InString,
	const TCHAR* InDelimiter,
	int32 InMaxSplit = INT32_MAX
	)
{
	if (!InDelimiter)
	{
		OutResult.Add(InString);
		return;
	}
	
	const int32 DelimiterLength = FCString::Strlen(InDelimiter); 
	if (DelimiterLength == 0)
	{
		OutResult.Add(InString);
		return;
	}

	InMaxSplit = FMath::Max(0, InMaxSplit);

	int32 StartIndex = 0;
	for (;;)
	{
		const int32 FoundIndex = (InMaxSplit--) ? InString.Find(InDelimiter, StartIndex) : INDEX_NONE;
		if (FoundIndex == INDEX_NONE)
		{
			OutResult.Add(InString.SubStr(StartIndex, INT32_MAX));
			break;
		}

		OutResult.Add(InString.SubStr(StartIndex, FoundIndex - StartIndex));
		StartIndex = FoundIndex + DelimiterLength;
	}
}


UOptimusNodeGraph* UOptimusDeformer::ResolveGraphPath(
	const FStringView InPath,
	FStringView& OutRemainingPath
	) const
{
	TArray<FStringView, TInlineAllocator<4>> Path;
	StringViewSplit<TInlineAllocator<4>>(Path, InPath, TEXT("/"));

	if (Path.Num() == 0)
	{
		return nullptr;
	}

	UOptimusNodeGraph* Graph = nullptr;
	if (Path[0] == UOptimusNodeGraph::LibraryRoot)
	{
		// FIXME: Search the library graphs.
	}
	else
	{
		for (UOptimusNodeGraph* RootGraph : Graphs)
		{
			if (Path[0].Equals(RootGraph->GetName(), ESearchCase::IgnoreCase))
			{
				Graph = RootGraph;
				break;
			}
		}
	}

	if (!Graph)
	{
		return nullptr;
	}

	// See if we need to traverse any sub-graphs
	int32 GraphIndex = 1;
	for (; GraphIndex < Path.Num(); GraphIndex++)
	{
		bool bFoundSubGraph = false;
		for (UOptimusNodeGraph* SubGraph: Graph->GetGraphs())
		{
			if (Path[GraphIndex].Equals(SubGraph->GetName(), ESearchCase::IgnoreCase))
			{
				Graph = SubGraph;
				bFoundSubGraph = true;
				break;
			}
		}
		if (!bFoundSubGraph)
		{
			break;
		}
	}

	if (GraphIndex < Path.Num())
	{
		OutRemainingPath = FStringView(
			Path[GraphIndex].GetData(),
			static_cast<int32>(Path.Last().GetData() - Path[GraphIndex].GetData()) + Path.Last().Len());
	}
	else
	{
		OutRemainingPath.Reset();
	}

	return Graph;
}


UOptimusNode* UOptimusDeformer::ResolveNodePath(
	const FStringView InPath,
	FStringView& OutRemainingPath
	) const
{
	FStringView NodePath;

	UOptimusNodeGraph* Graph = ResolveGraphPath(InPath, NodePath);
	if (!Graph || NodePath.IsEmpty())
	{
		return nullptr;
	}

	// We only want at most 2 elements (single split)
	TArray<FStringView, TInlineAllocator<2>> Path;
	StringViewSplit(Path, NodePath, TEXT("."), 1);
	if (Path.IsEmpty())
	{
		return nullptr;
	}

	const FStringView NodeName = Path[0];
	for (UOptimusNode* Node : Graph->GetAllNodes())
	{
		if (Node != nullptr && NodeName.Equals(Node->GetName(), ESearchCase::IgnoreCase))
		{
			OutRemainingPath = Path.Num() == 2 ? Path[1] : FStringView();
			return Node;
		}
	}

	return nullptr;
}


void UOptimusDeformer::Notify(EOptimusGlobalNotifyType InNotifyType, UObject* InObject) const
{
	switch (InNotifyType)
	{
	case EOptimusGlobalNotifyType::GraphAdded: 
	case EOptimusGlobalNotifyType::GraphRemoved:
	case EOptimusGlobalNotifyType::GraphIndexChanged:
	case EOptimusGlobalNotifyType::GraphRenamed:
		checkSlow(Cast<UOptimusNodeGraph>(InObject) != nullptr);
		break;

	case EOptimusGlobalNotifyType::ResourceAdded:
	case EOptimusGlobalNotifyType::ResourceRemoved:
	case EOptimusGlobalNotifyType::ResourceIndexChanged:
	case EOptimusGlobalNotifyType::ResourceRenamed:
	case EOptimusGlobalNotifyType::ResourceTypeChanged:
		checkSlow(Cast<UOptimusResourceDescription>(InObject) != nullptr);
		break;

	case EOptimusGlobalNotifyType::VariableAdded:
	case EOptimusGlobalNotifyType::VariableRemoved:
	case EOptimusGlobalNotifyType::VariableIndexChanged:
	case EOptimusGlobalNotifyType::VariableRenamed:
	case EOptimusGlobalNotifyType::VariableTypeChanged:
		checkSlow(Cast<UOptimusVariableDescription>(InObject) != nullptr);
		break;
	default:
		checkfSlow(false, TEXT("Unchecked EOptimusGlobalNotifyType!"));
		break;
	}

	GlobalNotifyDelegate.Broadcast(InNotifyType, InObject);
}


void UOptimusDeformer::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	// Mark with a custom version. This has the nice side-benefit of making the asset indexer
	// skip this object if the plugin is not loaded.
	Ar.UsingCustomVersion(FOptimusObjectVersion::GUID);
}


void UOptimusDeformer::GetKernelBindings(int32 InKernelIndex, TMap<int32, TArray<uint8>>& OutBindings) const
{
	for (const FOptimus_ShaderParameterBinding& Binding: AllParameterBindings)
	{
		if (Binding.KernelIndex == InKernelIndex)
		{
			const UOptimusNode_ConstantValue *ValueNode = Cast<const UOptimusNode_ConstantValue>(Binding.ValueNode);

			// This may happen if the node has been GC'd.
			if (ValueNode)
			{
				TArray<uint8> ValueData = ValueNode->GetShaderValue();
				if (!ValueData.IsEmpty())
				{
					OutBindings.Emplace(Binding.ParameterIndex, MoveTemp(ValueData));
				}
			}
		}
	}
}


void UOptimusDeformer::SetPreviewMesh(USkeletalMesh* PreviewMesh, bool bMarkAsDirty)
{
	Mesh = PreviewMesh;
	
	// FIXME: Notify upstream so the viewport can react.
}


USkeletalMesh* UOptimusDeformer::GetPreviewMesh() const
{
	return Mesh;
}


IOptimusNodeGraphCollectionOwner* UOptimusDeformer::ResolveCollectionPath(const FString& InPath)
{
	if (InPath.IsEmpty())
	{
		return this;
	}

	return Cast<IOptimusNodeGraphCollectionOwner>(ResolveGraphPath(InPath));
}


UOptimusNodeGraph* UOptimusDeformer::ResolveGraphPath(const FString& InGraphPath)
{
	FStringView PathRemainder;

	UOptimusNodeGraph* Graph = ResolveGraphPath(InGraphPath, PathRemainder);

	// The graph is only valid if the path was fully consumed.
	return PathRemainder.IsEmpty() ? Graph : nullptr;
}


UOptimusNode* UOptimusDeformer::ResolveNodePath(const FString& InNodePath)
{
	FStringView PathRemainder;

	UOptimusNode* Node = ResolveNodePath(InNodePath, PathRemainder);

	// The graph is only valid if the path was fully consumed.
	return PathRemainder.IsEmpty() ? Node : nullptr;
}


UOptimusNodePin* UOptimusDeformer::ResolvePinPath(const FString& InPinPath)
{
	FStringView PinPath;

	UOptimusNode* Node = ResolveNodePath(InPinPath, PinPath);

	return Node ? Node->FindPin(PinPath) : nullptr;
}



UOptimusNodeGraph* UOptimusDeformer::CreateGraph(
	EOptimusNodeGraphType InType, 
	FName InName, 
	TOptional<int32> InInsertBefore
	)
{
	// Update graphs is a singleton and is created by default. Transient graphs are only used
	// when duplicating nodes and should never exist as a part of a collection. 
	if (InType == EOptimusNodeGraphType::Update ||
		InType == EOptimusNodeGraphType::Transient)
	{
		return nullptr;
	}

	UClass* GraphClass = UOptimusNodeGraph::StaticClass();
	
	if (InType == EOptimusNodeGraphType::Setup)
	{
		// Do we already have a setup graph?
		if (Graphs.Num() > 1 && Graphs[0]->GetGraphType() == EOptimusNodeGraphType::Setup)
		{
			return nullptr;
		}

		// The name of the setup graph is fixed.
		InName = UOptimusNodeGraph::SetupGraphName;
	}
	else if (InType == EOptimusNodeGraphType::ExternalTrigger)
	{
		if (!UOptimusNodeGraph::IsValidUserGraphName(InName.ToString()))
		{
			return nullptr;
		}

		// If there's already an object with this name, then attempt to make the name unique.
		InName = Optimus::GetUniqueNameForScopeAndClass(this, UOptimusNodeGraph::StaticClass(), InName);
	}
	else if (InType == EOptimusNodeGraphType::Function)
	{
		GraphClass = UOptimusFunctionNodeGraph::StaticClass();
	}

	// If there's already an object with this name, then attempt to make the name unique.
	InName = Optimus::GetUniqueNameForScopeAndClass(this, GraphClass, InName);
	
	UOptimusNodeGraph* Graph = NewObject<UOptimusNodeGraph>(this, GraphClass, InName, RF_Transactional);

	Graph->SetGraphType(InType);

	if (InInsertBefore.IsSet())
	{
		if (!AddGraph(Graph, InInsertBefore.GetValue()))
		{
			Graph->Rename(nullptr, GetTransientPackage());
			return nullptr;
		}
	}
	
	return Graph;
}


bool UOptimusDeformer::AddGraph(
	UOptimusNodeGraph* InGraph,
	int32 InInsertBefore
	)
{
	if (InGraph == nullptr || InGraph->GetOuter() != this)
	{
		return false;
	}

	const bool bHaveSetupGraph = (Graphs.Num() > 1 && Graphs[0]->GetGraphType() == EOptimusNodeGraphType::Setup);

	// If INDEX_NONE, insert at the end.
	if (InInsertBefore == INDEX_NONE)
	{
		InInsertBefore = Graphs.Num();
	}
		
	switch (InGraph->GetGraphType())
	{
	case EOptimusNodeGraphType::Update:
		// We cannot replace the update graph.
		return false;
		
	case EOptimusNodeGraphType::Setup:
		// Do we already have a setup graph?
		if (bHaveSetupGraph)
		{
			return false;
		}
		// The setup graph is always first, if present.
		InInsertBefore = 0;
		break;
		
	case EOptimusNodeGraphType::ExternalTrigger:
		// Trigger graphs are always sandwiched between setup and update.
		InInsertBefore = FMath::Clamp(InInsertBefore, bHaveSetupGraph ? 1 : 0, GetUpdateGraphIndex());
		break;

	case EOptimusNodeGraphType::Function:
		// Function graphs always go last.
		InInsertBefore = Graphs.Num();
		break;

	case EOptimusNodeGraphType::SubGraph:
		// We cannot add subgraphs to the root.
		return false;

	case EOptimusNodeGraphType::Transient:
		checkNoEntry();
		return false;
	}
	
	Graphs.Insert(InGraph, InInsertBefore);

	Notify(EOptimusGlobalNotifyType::GraphAdded, InGraph);

	return true;
}


bool UOptimusDeformer::RemoveGraph(
	UOptimusNodeGraph* InGraph,
	bool bInDeleteGraph
	)
{
	// Not ours?
	const int32 GraphIndex = Graphs.IndexOfByKey(InGraph);
	if (GraphIndex == INDEX_NONE)
	{
		return false;
	}

	if (InGraph->GetGraphType() == EOptimusNodeGraphType::Update)
	{
		return false;
	}

	Graphs.RemoveAt(GraphIndex);

	Notify(EOptimusGlobalNotifyType::GraphRemoved, InGraph);

	if (bInDeleteGraph)
	{
		// Un-parent this graph to a temporary storage and mark it for kill.
		InGraph->Rename(nullptr, GetTransientPackage());
	}

	return true;
}



bool UOptimusDeformer::MoveGraph(
	UOptimusNodeGraph* InGraph, 
	int32 InInsertBefore
	)
{
	const int32 GraphOldIndex = Graphs.IndexOfByKey(InGraph);
	if (GraphOldIndex == INDEX_NONE)
	{
		return false;
	}

	if (InGraph->GetGraphType() != EOptimusNodeGraphType::ExternalTrigger)
	{
		return false;
	}

	// Less than num graphs, because the index is based on the node being moved not being
	// in the list.
	if (InInsertBefore == INDEX_NONE)
	{
		InInsertBefore = GetUpdateGraphIndex();
	}
	else
	{
		const bool bHaveSetupGraph = (Graphs.Num() > 1 && Graphs[0]->GetGraphType() == EOptimusNodeGraphType::Setup);
		InInsertBefore = FMath::Clamp(InInsertBefore, bHaveSetupGraph ? 1 : 0, GetUpdateGraphIndex());
	}

	if (GraphOldIndex == InInsertBefore)
	{
		return true;
	}

	Graphs.RemoveAt(GraphOldIndex);
	Graphs.Insert(InGraph, InInsertBefore);

	Notify(EOptimusGlobalNotifyType::GraphIndexChanged, InGraph);

	return true;
}


bool UOptimusDeformer::RenameGraph(UOptimusNodeGraph* InGraph, const FString& InNewName)
{
	// Not ours?
	int32 GraphIndex = Graphs.IndexOfByKey(InGraph);
	if (GraphIndex == INDEX_NONE)
	{
		return false;
	}

	// Setup and Update graphs cannot be renamed.
	if (InGraph->GetGraphType() == EOptimusNodeGraphType::Setup ||
		InGraph->GetGraphType() == EOptimusNodeGraphType::Update)
	{
		return false;
	}

	if (!UOptimusNodeGraph::IsValidUserGraphName(InNewName))
	{
		return false;
	}

	const bool bSuccess = GetActionStack()->RunAction<FOptimusNodeGraphAction_RenameGraph>(InGraph, FName(*InNewName));
	if (bSuccess)
	{
		Notify(EOptimusGlobalNotifyType::GraphRenamed, InGraph);
	}
	return bSuccess;
}


int32 UOptimusDeformer::GetUpdateGraphIndex() const
{
	if (const UOptimusNodeGraph* UpdateGraph = GetUpdateGraph(); ensure(UpdateGraph != nullptr))
	{
		return UpdateGraph->GetGraphIndex();
	}

	return INDEX_NONE;
}


#undef LOCTEXT_NAMESPACE
