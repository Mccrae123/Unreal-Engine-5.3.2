// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorGraphBuilder.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Metasound.h"
#include "MetasoundAssetBase.h"
#include "MetasoundEditorGraph.h"
#include "MetasoundEditorGraphNode.h"
#include "MetasoundEditorModule.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundLiteral.h"
#include "MetasoundUObjectRegistry.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Templates/Tuple.h"
#include "Widgets/Notifications/SNotificationList.h"


#define LOCTEXT_NAMESPACE "MetasoundEditor"


namespace Metasound
{
	namespace Editor
	{
		// Category names must match those found in UEdGraphSchema_K2:PC_<type>
		// so default selectors function the same way by default
		const FName FGraphBuilder::PinCategoryBoolean = "bool";
		const FName FGraphBuilder::PinCategoryDouble = "double";
		const FName FGraphBuilder::PinCategoryExec = "exec";
		const FName FGraphBuilder::PinCategoryFloat = "float";
		const FName FGraphBuilder::PinCategoryInt32 = "int";
		const FName FGraphBuilder::PinCategoryInt64 = "int64";
		const FName FGraphBuilder::PinCategoryObject = "object";
		const FName FGraphBuilder::PinCategoryString = "string";

		const FName FGraphBuilder::PinSubCategoryAudioFormat = "Format";
		const FName FGraphBuilder::PinSubCategoryAudioNumeric = "Numeric";
		const FName FGraphBuilder::PinSubCategoryObjectArray = "UObjectArray";

		namespace GraphBuilderPrivate
		{
			void DeleteNode(UObject& InMetasound, Frontend::FNodeHandle InNodeHandle)
			{
				if (InNodeHandle->IsValid())
				{
					Frontend::FGraphHandle GraphHandle = InNodeHandle->GetOwningGraph();
					if (GraphHandle->IsValid())
					{
						GraphHandle->RemoveNode(*InNodeHandle);
					}
				}

				InMetasound.MarkPackageDirty();
			}

			void SetInputLiteral(Frontend::FNodeHandle InInputNodeHandle, FGuid PointID, const FName InTypeName, const FLiteral& InDefaultValue)
			{
				Frontend::FGraphHandle GraphHandle = InInputNodeHandle->GetOwningGraph();

				const FString Name = InInputNodeHandle->GetNodeName();
				switch (InDefaultValue.GetType())
				{
					case ELiteralType::Boolean:
					{
						GraphHandle->SetDefaultInputToLiteral(Name, PointID, InDefaultValue.Value.Get<bool>());
					}
					break;

					case ELiteralType::Float:
					{
						GraphHandle->SetDefaultInputToLiteral(Name, PointID, InDefaultValue.Value.Get<float>());
					}
					break;

					case ELiteralType::Integer:
					{
						GraphHandle->SetDefaultInputToLiteral(Name, PointID, InDefaultValue.Value.Get<int32>());
					}
					break;

					case ELiteralType::String:
					{
						GraphHandle->SetDefaultInputToLiteral(Name, PointID, InDefaultValue.Value.Get<FString>());
					}
					break;

					case ELiteralType::UObjectProxy:
					{
						// TODO: Support default UObject value on node
						UClass* ClassToUse = FMetasoundFrontendRegistryContainer::Get()->GetLiteralUClassForDataType(InTypeName);
						if (ClassToUse)
						{
							GraphHandle->SetDefaultInputToLiteral(Name, PointID, ClassToUse->ClassDefaultObject);
						}
					}
					break;

					case ELiteralType::UObjectProxyArray:
					{
						// TODO: Support default UObject array value on node
						UClass* ClassToUse = FMetasoundFrontendRegistryContainer::Get()->GetLiteralUClassForDataType(InTypeName);
						if (ClassToUse)
						{
							TArray<UObject*> ObjectArray;
							ObjectArray.Add(ClassToUse->ClassDefaultObject);
							GraphHandle->SetDefaultInputToLiteral(Name, PointID, ObjectArray);
						}
					}
					break;

					case ELiteralType::Invalid:
					case ELiteralType::None:
					default:
					{
						static_assert(static_cast<int32>(ELiteralType::Invalid) == 7, "Possible missing ELiteralType case coverage");
					}
					break;
				}
			}
		}

		UEdGraphNode* FGraphBuilder::AddNode(UObject& InMetasound, Frontend::FNodeHandle& InNodeHandle, bool bInSelectNewNode)
		{
			const FScopedTransaction Transaction(LOCTEXT("AddMetasoundGraphNode", "Add Metasound Node"));

			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			UEdGraph& Graph = MetasoundAsset->GetGraphChecked();
			FGraphNodeCreator<UMetasoundEditorGraphNode> NodeCreator(Graph);
			UMetasoundEditorGraphNode* NewGraphNode = NodeCreator.CreateNode(bInSelectNewNode);
			NodeCreator.Finalize();

			const FMetasoundFrontendNodeStyle& Style = InNodeHandle->GetNodeStyle();

			NewGraphNode->SetNodeID(InNodeHandle->GetID());
			NewGraphNode->CreateNewGuid();
			NewGraphNode->NodePosX = Style.Display.Location.X;
			NewGraphNode->NodePosY = Style.Display.Location.Y;

			RebuildNodePins(*NewGraphNode, InNodeHandle);

			InMetasound.PostEditChange();
			InMetasound.MarkPackageDirty();

			return NewGraphNode;
		}

		UEdGraphNode* FGraphBuilder::AddNode(UObject& InMetasound, const Frontend::FNodeClassInfo& InClassInfo, const FMetasoundFrontendNodeStyle& InNodeStyle, bool bInSelectNewNode)
		{
			Frontend::FNodeHandle NodeHandle = AddNodeHandle(InMetasound, InClassInfo, InNodeStyle);
			return AddNode(InMetasound, NodeHandle, bInSelectNewNode);
		}

		Frontend::FNodeHandle FGraphBuilder::AddNodeHandle(UObject& InMetasound, const Frontend::FNodeClassInfo& InClassInfo, const FMetasoundFrontendNodeStyle& InNodeStyle)
		{
			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			Frontend::FNodeHandle NewNode = MetasoundAsset->GetRootGraphHandle()->AddNode(InClassInfo);
			NewNode->SetNodeStyle(InNodeStyle);
			return NewNode;
		}

		FString FGraphBuilder::GetDataTypeDisplayName(const FName& InDataTypeName)
		{
			FString CategoryString = InDataTypeName.ToString();
			int32 Index = 0;
			CategoryString.FindLastChar(':', Index);

			return CategoryString.RightChop(Index + 1);
		}

		TArray<FString> FGraphBuilder::GetDataTypeNameCategories(const FName& InDataTypeName)
		{
			FString CategoryString = InDataTypeName.ToString();

			TArray<FString> Categories;
			CategoryString.ParseIntoArray(Categories, TEXT(":"));

			if (Categories.Num() > 0)
			{
				// Remove name
				Categories.RemoveAt(Categories.Num() - 1);
			}

			return Categories;
		}

		FString FGraphBuilder::GenerateUniqueInputName(const UObject& InMetasound, const FName InBaseName)
		{
			FString NameBase = GetDataTypeDisplayName(InBaseName);
			const FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			const Frontend::FConstGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			int32 i = 1;
			FString NewInputName = NameBase + FString::Printf(TEXT("_%02d"), i);
			while (GraphHandle->ContainsInputVertexWithName(NewInputName))
			{
				NewInputName = NameBase + FString::Printf(TEXT("_%02d"), ++i);
			}

			return NewInputName;
		}

		FString FGraphBuilder::GenerateUniqueOutputName(const UObject& InMetasound, const FName InBaseName)
		{
			FString NameBase = GetDataTypeDisplayName(InBaseName);
			const FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			const Frontend::FConstGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			int32 i = 1;
			FString NewOutputName = NameBase + FString::Printf(TEXT("_%02d"), i);
			while (GraphHandle->ContainsOutputVertexWithName(NewOutputName))
			{
				NewOutputName = NameBase + FString::Printf(TEXT("_%02d"), ++i);
			}

			return NewOutputName;
		}

		UEdGraphNode* FGraphBuilder::AddInput(UObject& InMetasound, const FString& InName, const FName InTypeName, const FMetasoundFrontendNodeStyle& InNodeStyle, const FText& InToolTip, bool bInSelectNewNode)
		{
			Frontend::FNodeHandle NodeHandle = AddInputNodeHandle(InMetasound, InName, InTypeName, InNodeStyle, &InToolTip);
			return AddNode(InMetasound, NodeHandle, bInSelectNewNode);
		}

		void FGraphBuilder::AddOrUpdateLiteralInput(UObject& InMetasound, Frontend::FNodeHandle InNodeHandle, const UEdGraphPin& InInputPin)
		{
			using namespace Metasound::Frontend;

			const FScopedTransaction Transaction(LOCTEXT("SetMetasoundGraphNode", "Set Metasound Literal Input"));

			if (!ensureAlways(InNodeHandle->GetClassType() == EMetasoundFrontendClassType::External))
			{
				return;
			}

			const FString& InInputName = InInputPin.GetName();
			const FString& InStringValue = InInputPin.DefaultValue;
			const UObject* InObjectValue = InInputPin.DefaultObject;

			TArray<FInputHandle> InputHandles = InNodeHandle->GetInputsWithVertexName(InInputName);
			if (!ensureAlways(InputHandles.Num() == 1))
			{
				return;
			}

			FInputHandle InputHandle = InputHandles[0];
			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");

			FLiteral Literal;
			const FName TypeName = InputHandle->GetDataType();
			const FEditorDataType DataType = EditorModule.FindDataType(TypeName);
			switch (DataType.RegistryInfo.PreferredLiteralType)
			{
				case ELiteralType::Boolean:
				{
					Literal.Value.Set<bool>(FCString::ToBool(*InStringValue));
				}
				break;

				case ELiteralType::Float:
				{
					Literal.Value.Set<float>(FCString::Atof(*InStringValue));
				}
				break;

				case ELiteralType::Integer:
				{
					Literal.Value.Set<int32>(FCString::Atoi(*InStringValue));
				}
				break;

				case ELiteralType::String:
				{
					Literal.Value.Set<FString>(InStringValue);
				}
				break;

				// TODO: Support UObjects/UObject arrays for default literals
				case ELiteralType::UObjectProxy:
				case ELiteralType::UObjectProxyArray:
				{
					return;
				}
				break;

				// If no literal supported, no need for private input
				case ELiteralType::None:
				{
					return;
				}
				break;

				case ELiteralType::Invalid:
				default:
				{
					ensureAlwaysMsgf(false, TEXT("Failed to set input node default: Literal type not supported"));
					return;
				}
				break;
			}

			bool bNewValueSet = false;
			FOutputHandle OutputHandle = InputHandle->GetCurrentlyConnectedOutput();
			if (!OutputHandle->IsValid())
			{
				FMetasoundFrontendNodeStyle Style;
				Style.Display.Visibility = EMetasoundFrontendNodeStyleDisplayVisibility::Hidden;

				Frontend::FNodeHandle NewInputNode = AddInputNodeHandle(InMetasound, GenerateUniqueInputName(InMetasound, "LiteralInput"), TypeName, Style, nullptr /* ToolTip */, &Literal);
				NewInputNode->SetNodeStyle(Style);

				bNewValueSet = true;
				TArray<Metasound::Frontend::FOutputHandle> OutputHandles = NewInputNode->GetOutputs();
				if (ensureAlways(OutputHandles.Num() == 1))
				{
					OutputHandle = OutputHandles[0];
				}

				ensureAlways(InputHandle->Connect(*OutputHandle));
			}

			if (OutputHandle->IsValid())
			{
				FNodeHandle InputNode = OutputHandle->GetOwningNode();
				const FMetasoundFrontendNodeStyle& Style = InputNode->GetNodeStyle();
				if (InputNode->IsValid() && Style.Display.Visibility == EMetasoundFrontendNodeStyleDisplayVisibility::Hidden)
				{
					if (!bNewValueSet)
					{
						// TODO: Support multiple literal point ids for arrays
						Frontend::FGraphHandle GraphHandle = InputNode->GetOwningGraph();
						TArray<FGuid> PointIDs = GraphHandle->GetDefaultIDsForInputVertex(InputNode->GetNodeName());
						if (ensureAlways(PointIDs.Num() == 1))
						{
							GraphBuilderPrivate::SetInputLiteral(InputNode, PointIDs[0], TypeName, Literal);
						}
					}
					InMetasound.PostEditChange();
					InMetasound.MarkPackageDirty();
				}
			}
		}

		Frontend::FNodeHandle FGraphBuilder::AddInputNodeHandle(UObject& InMetasound, const FString& InName, const FName InTypeName, const FMetasoundFrontendNodeStyle& InNodeStyle, const FText* InToolTip, const FLiteral* InDefaultValue)
		{
			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			Frontend::FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			FMetasoundFrontendClassInput Description;

			Description.Name = InName;
			Description.TypeName = InTypeName;

			if (InToolTip)
			{
				Description.Metadata.Description = *InToolTip;
			}

			FGuid PointID = GraphHandle->GetNewPointID();

			Description.PointIDs.Add(PointID);
			FMetasoundFrontendVertexLiteral DefaultVertex;
			DefaultVertex.PointID = PointID;
			Description.Defaults.Add(DefaultVertex);

			Frontend::FNodeHandle NodeHandle = GraphHandle->AddInputVertex(Description);
			if (!ensureAlways(NodeHandle->IsValid()))
			{
				return NodeHandle;
			}

			GraphHandle->SetInputDisplayName(InName, FText::FromString(InName));

			Metasound::FLiteral LiteralParam = Frontend::GetDefaultParamForDataType(InTypeName);
			if (!LiteralParam.IsValid())
			{
				return NodeHandle;
			}

			if (InDefaultValue)
			{
				if (!ensureAlways(InDefaultValue->GetType() == LiteralParam.GetType()))
				{
					return NodeHandle;
				}
			};

			GraphBuilderPrivate::SetInputLiteral(NodeHandle, PointID, InTypeName, InDefaultValue ? *InDefaultValue : LiteralParam);

			return NodeHandle;
		}

		UEdGraphNode* FGraphBuilder::AddOutput(UObject& InMetasound, const FString& InName, const FName InTypeName, const FMetasoundFrontendNodeStyle& InNodeStyle, const FText& InToolTip, bool bInSelectNewNode)
		{
			Frontend::FNodeHandle NodeHandle = AddOutputNodeHandle(InMetasound, InName, InTypeName, InNodeStyle, InToolTip);
			return AddNode(InMetasound, NodeHandle, bInSelectNewNode);
		}

		Frontend::FNodeHandle FGraphBuilder::AddOutputNodeHandle(UObject& InMetasound, const FString& InName, const FName InTypeName, const FMetasoundFrontendNodeStyle& InNodeStyle, const FText& InToolTip)
		{
			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			Frontend::FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			FMetasoundFrontendClassOutput Description;
			Description.Name = InName;
			Description.TypeName = InTypeName;
			Description.Metadata.Description = InToolTip;
			Description.PointIDs.Add(GraphHandle->GetNewPointID());

			Frontend::FNodeHandle NodeHandle = GraphHandle->AddOutputVertex(Description);
			NodeHandle->SetNodeStyle(InNodeStyle);

			GraphHandle->SetOutputDisplayName(InName, FText::FromString(InName));

			return NodeHandle;
		}

		bool FGraphBuilder::ConnectNodes(UEdGraphPin& InInputPin, UEdGraphPin& InOutputPin)
		{
			using namespace Metasound::Frontend;

			UMetasoundEditorGraphNode* InputGraphNode = CastChecked<UMetasoundEditorGraphNode>(InInputPin.GetOwningNode());
			FNodeHandle InputNodeHandle = InputGraphNode->GetNodeHandle();
			TArray<FInputHandle> InputHandles = InputNodeHandle->GetInputsWithVertexName(InInputPin.GetName());

			UMetasoundEditorGraphNode* OutputGraphNode = CastChecked<UMetasoundEditorGraphNode>(InOutputPin.GetOwningNode());
			FNodeHandle OutputNodeHandle = OutputGraphNode->GetNodeHandle();
			TArray<FOutputHandle> OutputHandles = OutputNodeHandle->GetOutputsWithVertexName(InOutputPin.GetName());

			if (!ensure(InputHandles.Num() == 1 && OutputHandles.Num() == 1))
			{
				InInputPin.BreakLinkTo(&InOutputPin);
				return false;
			}

			FInputHandle InputHandle = InputHandles[0];
			FOutputHandle OutputHandle = OutputHandles[0];

			FOutputHandle ExistingOutput = InputHandle->GetCurrentlyConnectedOutput();
			if (ExistingOutput->IsValid())
			{
				FNodeHandle NodeHandle = ExistingOutput->GetOwningNode();
				const FMetasoundFrontendNodeStyle& NodeStyle = NodeHandle->GetNodeStyle();
				if (NodeHandle->IsValid() && NodeStyle.Display.Visibility == EMetasoundFrontendNodeStyleDisplayVisibility::Hidden)
				{
					GraphBuilderPrivate::DeleteNode(InputGraphNode->GetMetasoundChecked(), NodeHandle);
				}
			}

			if (!ensure(InputHandle->Connect(*OutputHandle)))
			{
				InInputPin.BreakLinkTo(&InOutputPin);
				return false;
			}

			return true;
		}

		void FGraphBuilder::ConstructGraph(UObject& InMetasound)
		{
			using namespace Frontend;

			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();
			UMetasoundEditorGraph* Graph = CastChecked<UMetasoundEditorGraph>(MetasoundAsset->GetGraph());

			Graph->Nodes.Reset();

			// TODO: Space graph nodes in a procedural and readable way
			FVector2D InputNodeLocation = FVector2D::ZeroVector;
			FVector2D OpNodeLocation = FVector2D(250.0f, 0.0f);
			FVector2D OutputNodeLocation = FVector2D(500.0f, 0.0f);

			struct FNodePair
			{
				FNodeHandle NodeHandle = INodeController::GetInvalidHandle();
				UEdGraphNode* GraphNode = nullptr;
			};

			TMap<FGuid, FNodePair> NewIdNodeMap;
			TArray<FNodeHandle> NodeHandles = GraphHandle->GetNodes();
			for (FNodeHandle& NodeHandle : NodeHandles)
			{
				const EMetasoundFrontendClassType NodeType = NodeHandle->GetClassType();
				FMetasoundFrontendNodeStyle Style;
				if (NodeType == EMetasoundFrontendClassType::Input)
				{
					Style.Display.Location = InputNodeLocation;
					InputNodeLocation.Y += 100.0f;
				}
				else if (NodeType == EMetasoundFrontendClassType::Output)
				{
					Style.Display.Location = OutputNodeLocation;
					OutputNodeLocation.Y += 100.0f;
				}
				else
				{
					Style.Display.Location = OpNodeLocation;
					OpNodeLocation.Y += 100.0f;
				}
				NodeHandle->SetNodeStyle(Style);

				UEdGraphNode* NewNode = AddNode(InMetasound, NodeHandle, false /* bInSelectNewNode */);
				NewIdNodeMap.Add(NodeHandle->GetID(), FNodePair { NodeHandle, NewNode });
			}

			for (const TPair<FGuid, FNodePair>& IdNodePair : NewIdNodeMap)
			{
				UEdGraphNode* GraphNode = IdNodePair.Value.GraphNode;
				check(GraphNode);

				FNodeHandle NodeHandle = IdNodePair.Value.NodeHandle;
				TArray<UEdGraphPin*> Pins = GraphNode->GetAllPins();

				const TArray<FInputHandle> NodeInputs = NodeHandle->GetInputs();

				int32 InputIndex = 0;
				for (UEdGraphPin* Pin : Pins)
				{
					switch (Pin->Direction)
					{
						case EEdGraphPinDirection::EGPD_Input:
						{
							FOutputHandle OutputHandle = NodeInputs[InputIndex]->GetCurrentlyConnectedOutput();
							if (OutputHandle->IsValid())
							{
								UEdGraphNode* OutputGraphNode = NewIdNodeMap.FindChecked(OutputHandle->GetOwningNodeID()).GraphNode;
								UEdGraphPin* OutputPin = OutputGraphNode->FindPinChecked(OutputHandle->GetName(), EEdGraphPinDirection::EGPD_Output);
								Pin->MakeLinkTo(OutputPin);
							}

							InputIndex++;
						}
						break;

						case EEdGraphPinDirection::EGPD_Output:
							// Do nothing.  Connecting all inputs will naturally connect all outputs where required
						break;
					}
				}
			}

			InMetasound.PostEditChange();
			InMetasound.MarkPackageDirty();
		}

		void FGraphBuilder::DeleteLiteralInputs(UEdGraphNode& InNode)
		{
			using namespace Metasound::Editor;
			using namespace Metasound::Frontend;

			FNodeHandle NodeHandle = Frontend::INodeController::GetInvalidHandle();
			if (UMetasoundEditorGraphNode* Node = Cast<UMetasoundEditorGraphNode>(&InNode))
			{
				NodeHandle = Node->GetNodeHandle();

				if (NodeHandle->GetClassType() == EMetasoundFrontendClassType::External)
				{
					TArray<FInputHandle> Inputs = NodeHandle->GetInputs();
					for (FInputHandle& Input : Inputs)
					{
						FOutputHandle Output = Input->GetCurrentlyConnectedOutput();
						if (Output->IsValid())
						{
							FNodeHandle InputHandle = Output->GetOwningNode();
							if (InputHandle->GetNodeStyle().Display.Visibility == EMetasoundFrontendNodeStyleDisplayVisibility::Hidden)
							{
								FGraphHandle Graph = InputHandle->GetOwningGraph();
								Graph->RemoveNode(*InputHandle);
							}
						}
					}
				}
			}
		}

		bool FGraphBuilder::DeleteNode(UEdGraphNode& InNode, bool bInRecordTransaction)
		{
			using namespace Metasound::Frontend;

			const FScopedTransaction Transaction(LOCTEXT("DeleteMetasoundGraphNode", "Delete Metasound Node"), bInRecordTransaction);

			FNodeHandle NodeHandle = Frontend::INodeController::GetInvalidHandle();
			if (UMetasoundEditorGraphNode* Node = Cast<UMetasoundEditorGraphNode>(&InNode))
			{
				NodeHandle = Node->GetNodeHandle();

				if (NodeHandle->GetClassType() == EMetasoundFrontendClassType::Input)
				{
					FConstDocumentHandle DocumentHandle = NodeHandle->GetOwningGraph()->GetOwningDocument();
					auto IsRequiredInput = [&](const Frontend::FConstInputHandle& InputHandle)
					{
						return DocumentHandle->IsRequiredInput(InputHandle->GetName());
					};
					TArray<Frontend::FConstInputHandle> NodeInputs = NodeHandle->GetConstInputs();

					if (Frontend::FConstInputHandle* InputHandle = NodeInputs.FindByPredicate(IsRequiredInput))
					{
						FNotificationInfo Info(FText::Format(LOCTEXT("Metasounds_CannotDeleteRequiredInput",
							"'Required Input '{0}' cannot be deleted."), (*InputHandle)->GetDisplayName()));
						Info.bFireAndForget = true;
						Info.ExpireDuration = 2.0f;
						Info.bUseThrobber = true;
						FSlateNotificationManager::Get().AddNotification(Info);
						return false;
					}
				}

				if (NodeHandle->GetClassType() == EMetasoundFrontendClassType::Output)
				{
					FConstDocumentHandle DocumentHandle = NodeHandle->GetOwningGraph()->GetOwningDocument();
					auto IsRequiredOutput = [&](const Frontend::FConstOutputHandle& OutputHandle)
					{
						return DocumentHandle->IsRequiredOutput(OutputHandle->GetName());
					};
					TArray<Frontend::FConstOutputHandle> NodeOutputs = NodeHandle->GetConstOutputs();

					if (Frontend::FConstOutputHandle* OutputHandle = NodeOutputs.FindByPredicate(IsRequiredOutput))
					{
						FNotificationInfo Info(FText::Format(LOCTEXT("Metasounds_CannotDeleteRequiredOutput",
							"'Required Output '{0}' cannot be deleted."), (*OutputHandle)->GetDisplayName()));
						Info.bFireAndForget = true;
						Info.ExpireDuration = 2.0f;
						Info.bUseThrobber = true;
						FSlateNotificationManager::Get().AddNotification(Info);
						return false;
					}
				}
			}

			DeleteLiteralInputs(InNode);

			UMetasoundEditorGraph* Graph = CastChecked<UMetasoundEditorGraph>(InNode.GetGraph());
			if (InNode.CanUserDeleteNode() && Graph->RemoveNode(&InNode))
			{
				Graph->PostEditChange();
				Graph->MarkPackageDirty();
			}

			if (NodeHandle->IsValid())
			{
				Frontend::FGraphHandle GraphHandle = NodeHandle->GetOwningGraph();
				if (GraphHandle->IsValid())
				{
					GraphHandle->RemoveNode(*NodeHandle);
				}
			}

			InNode.PostEditChange();
			InNode.MarkPackageDirty();
			return true;
		}

		void FGraphBuilder::RebuildNodePins(UMetasoundEditorGraphNode& InGraphNode, Frontend::FNodeHandle InNodeHandle)
		{
			const FScopedTransaction Transaction(LOCTEXT("RebuildMetasoundGraphNodePins", "Rebuild Metasound Pins"));

			for (int32 i = InGraphNode.Pins.Num() - 1; i >= 0; i--)
			{
				DeleteLiteralInputs(InGraphNode);
				InGraphNode.RemovePin(InGraphNode.Pins[i]);
			}

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");

			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InGraphNode.GetMetasoundChecked());
			check(MetasoundAsset);

			Frontend::FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			TArray<Frontend::FInputHandle> InputHandles = InNodeHandle->GetInputs();
			for (int32 i = 0; i < InputHandles.Num(); ++i)
			{
				AddPinToNode(InGraphNode, InputHandles[i]);
			}

			TArray<Frontend::FOutputHandle> OutputHandles = InNodeHandle->GetOutputs();
			for (int32 i = 0; i < OutputHandles.Num(); ++i)
			{
				AddPinToNode(InGraphNode, OutputHandles[i]);
			}

			InGraphNode.MarkPackageDirty();
		}

		bool FGraphBuilder::IsMatchingInputHandleAndPin(const Frontend::FInputHandle& InInputHandle, const UEdGraphPin& InEditorPin)
		{
			if (EEdGraphPinDirection::EGPD_Input == InEditorPin.Direction)
			{
				if (InEditorPin.GetName() == InInputHandle->GetName())
				{
					IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");

					if (InEditorPin.PinType == EditorModule.FindDataType(InInputHandle->GetDataType()).PinType)
					{
						UEdGraphNode* EditorNode = InEditorPin.GetOwningNode();
						if (UMetasoundEditorGraphNode* MetasoundEditorNode = Cast<UMetasoundEditorGraphNode>(EditorNode))
						{
							if  (MetasoundEditorNode->GetNodeHandle()->GetID() == InInputHandle->GetOwningNodeID())
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}

		bool FGraphBuilder::IsMatchingOutputHandleAndPin(const Frontend::FOutputHandle& InOutputHandle, const UEdGraphPin& InEditorPin)
		{
			if (EEdGraphPinDirection::EGPD_Output == InEditorPin.Direction)
			{
				if (InEditorPin.GetName() == InOutputHandle->GetName())
				{
					IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");

					if (InEditorPin.PinType == EditorModule.FindDataType(InOutputHandle->GetDataType()).PinType)
					{
						UEdGraphNode* EditorNode = InEditorPin.GetOwningNode();
						UMetasoundEditorGraphNode* MetasoundEditorNode = Cast<UMetasoundEditorGraphNode>(EditorNode);

						if (nullptr != MetasoundEditorNode)
						{
							if  (MetasoundEditorNode->GetNodeHandle()->GetID() == InOutputHandle->GetOwningNodeID())
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}

		UEdGraphPin* FGraphBuilder::AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FInputHandle InInputHandle)
		{
			using namespace Metasound::Editor;
			using namespace Metasound::Frontend;

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");

			FEdGraphPinType PinType("MetasoundNode", NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
			UEdGraphPin* NewPin = InEditorNode.CreatePin(EGPD_Input, PinType, FName(*InInputHandle->GetName()));

			if (ensureAlways(NewPin))
			{
				NewPin->PinToolTip = InInputHandle->GetTooltip().ToString();
				NewPin->PinType = EditorModule.FindDataType(InInputHandle->GetDataType()).PinType;

				FNodeHandle NodeHandle = InInputHandle->GetOwningNode();
				if (NodeHandle->GetClassType() == EMetasoundFrontendClassType::External)
				{
					FGraphBuilder::AddOrUpdateLiteralInput(InEditorNode.GetMetasoundChecked(), NodeHandle, *NewPin);
				}
			}

			return NewPin;
		}

		UEdGraphPin* FGraphBuilder::AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FOutputHandle InOutputHandle)
		{
			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetasoundEditor");
			FText DisplayName;

			FEdGraphPinType PinType("MetasoundNode", NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
			UEdGraphPin* NewPin = InEditorNode.CreatePin(EGPD_Output, PinType, FName(*InOutputHandle->GetName()));

			if (ensureAlways(NewPin))
			{
				NewPin->PinToolTip = InOutputHandle->GetTooltip().ToString();
				NewPin->PinType = EditorModule.FindDataType(InOutputHandle->GetDataType()).PinType;
			}

			return NewPin;
		}

		bool FGraphBuilder::SynchronizeGraph(UObject& InMetasound)
		{
			using namespace Frontend;

			bool bIsEditorGraphDirty = false;

			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			// Get all nodes from frontend graph
			FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();
			TArray<FNodeHandle> FrontendNodes = GraphHandle->GetNodes();

			// Get all editor nodes from editor graph (some nodes on graph may *NOT* be metasound ed nodes,
			// such as comment boxes, etc, so just get nodes of class UMetasoundEditorGraph).
			UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetasoundAsset->GetGraph());
			TArray<UMetasoundEditorGraphNode*> EditorNodes;
			EditorGraph->GetNodesOfClass(EditorNodes);

			// Find existing pairs of nodes and editor nodes
			struct FNodePair
			{
				UMetasoundEditorGraphNode* EditorNode = nullptr;
				FNodeHandle Node = INodeController::GetInvalidHandle();
			};
			TMap<FGuid, FNodePair> PairedNodes;

			// Reverse iterate so paired nodes can safely be removed from the array.
			for (int32 i = FrontendNodes.Num() - 1; i >= 0; i--)
			{
				FNodeHandle Node = FrontendNodes[i];
				auto IsEditorNodeWithSameNodeID = [&](const UMetasoundEditorGraphNode* InEditorNode)
				{
					return InEditorNode->GetNodeID() == Node->GetID();
				};

				UMetasoundEditorGraphNode* EditorNode = nullptr;
				if (UMetasoundEditorGraphNode** PointerEditorNode = EditorNodes.FindByPredicate(IsEditorNodeWithSameNodeID))
				{
					EditorNode = *PointerEditorNode;
				}

				if (EditorNode)
				{
					PairedNodes.Add(Node->GetID(), { EditorNode, Node });
					EditorNodes.RemoveSwap(EditorNode);
					FrontendNodes.RemoveAtSwap(i);
				}
			}

			// FrontendNodes contains nodes which need to be added.
			// EditorNodes contains nodes that need to be removed.
			// PairedNodes contains pairs which we have to check have synchronized pins

			// Add and remove nodes first in order to make sure correct editor nodes
			// exist before attempting to synchronize connections.
			for (UMetasoundEditorGraphNode* EditorNode : EditorNodes)
			{
				bIsEditorGraphDirty |= EditorGraph->RemoveNode(EditorNode);
			}

			// Add missing editor nodes marked as visible.
			bIsEditorGraphDirty |= (FrontendNodes.Num() > 0);
			for (FNodeHandle Node : FrontendNodes)
			{
				const FMetasoundFrontendNodeStyle& Style = Node->GetNodeStyle();
				if (Style.Display.Visibility == EMetasoundFrontendNodeStyleDisplayVisibility::Visible)
				{
					UEdGraphNode* NewNode = AddNode(InMetasound, Node, false /* bInSelectNewNode */);
					PairedNodes.Add(Node->GetID(), { Cast<UMetasoundEditorGraphNode>(NewNode), Node });
				}
			}

			// Synchronize pins on node pairs.
			for (const TPair<FGuid, FNodePair>& IdNodePair : PairedNodes)
			{
				UMetasoundEditorGraphNode* EditorNode = IdNodePair.Value.EditorNode;
				bIsEditorGraphDirty |= SynchronizeNodePins(*IdNodePair.Value.EditorNode, IdNodePair.Value.Node);
			}

			// Synchronize connections.
			bIsEditorGraphDirty |= SynchronizeConnections(InMetasound);

			if (bIsEditorGraphDirty)
			{
				InMetasound.PostEditChange();
				InMetasound.MarkPackageDirty();
			}

			return bIsEditorGraphDirty;
		}

		bool FGraphBuilder::SynchronizeNodePins(UMetasoundEditorGraphNode& InEditorNode, Frontend::FNodeHandle InNode)
		{
			bool bIsNodeDirty = false;

			TArray<Frontend::FInputHandle> InputHandles = InNode->GetInputs();
			TArray<Frontend::FOutputHandle> OutputHandles = InNode->GetOutputs();
			TArray<UEdGraphPin*> EditorPins = InEditorNode.Pins;

			// Filter out pins which are not paired.
			for (int32 i = EditorPins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = EditorPins[i];

				auto IsMatchingInputHandle = [&](const Frontend::FInputHandle& InputHandle) -> bool
				{
					return IsMatchingInputHandleAndPin(InputHandle, *Pin);
				};

				auto IsMatchingOutputHandle = [&](const Frontend::FOutputHandle& OutputHandle) -> bool
				{
					return IsMatchingOutputHandleAndPin(OutputHandle, *Pin);
				};

				switch (Pin->Direction)
				{
					case EEdGraphPinDirection::EGPD_Input:
					{
						int32 MatchingInputIndex = InputHandles.FindLastByPredicate(IsMatchingInputHandle);
						if (INDEX_NONE != MatchingInputIndex)
						{
							InputHandles.RemoveAtSwap(MatchingInputIndex);
							EditorPins.RemoveAtSwap(i);
						}
					}
					break;

					case EEdGraphPinDirection::EGPD_Output:
					{
						int32 MatchingOutputIndex = OutputHandles.FindLastByPredicate(IsMatchingOutputHandle);
						if (INDEX_NONE != MatchingOutputIndex)
						{
							OutputHandles.RemoveAtSwap(MatchingOutputIndex);
							EditorPins.RemoveAtSwap(i);
						}
					}
					break;
				}
			}

			bIsNodeDirty |= (InputHandles.Num() > 0);
			bIsNodeDirty |= (OutputHandles.Num() > 0);
			bIsNodeDirty |= (EditorPins.Num() > 0);

			// Remove any unused editor pins.
			for (UEdGraphPin* Pin : EditorPins)
			{
				InEditorNode.RemovePin(Pin);
			}

			// Add unmatched input pins
			for (Frontend::FInputHandle& InputHandle : InputHandles)
			{
				AddPinToNode(InEditorNode, InputHandle);
			}

			for (Frontend::FOutputHandle& OutputHandle : OutputHandles)
			{
				AddPinToNode(InEditorNode, OutputHandle);
			}
			
			if (bIsNodeDirty)
			{
				InEditorNode.MarkPackageDirty();
			}

			return bIsNodeDirty;
		}

		bool FGraphBuilder::SynchronizeConnections(UObject& InMetasound)
		{
			bool bIsGraphDirty = false;

			FMetasoundAssetBase* MetasoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetasound);
			check(MetasoundAsset);

			Frontend::FGraphHandle GraphHandle = MetasoundAsset->GetRootGraphHandle();

			UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetasoundAsset->GetGraph());
			TArray<UMetasoundEditorGraphNode*> EditorNodes;

			// Cache map of editor nodes indexed by node id.
			TMap<FGuid, UMetasoundEditorGraphNode*> EditorNodesByID;
			for (UEdGraphNode* EdGraphNode : EditorGraph->Nodes)
			{
				if (UMetasoundEditorGraphNode* MetasoundEditorNode = Cast<UMetasoundEditorGraphNode>(EdGraphNode))
				{
					EditorNodes.Add(MetasoundEditorNode);
					EditorNodesByID.Add(MetasoundEditorNode->GetNodeID(), MetasoundEditorNode);
				}
			}

			// Iterate through all nodes in metasound editor graph and synchronize connections.
			for (UEdGraphNode* EdGraphNode : EditorGraph->Nodes)
			{
				bool bIsNodeDirty = false;

				// Get all editor nodes from editor graph (some nodes on graph may *NOT* be metasound ed nodes,
				// such as comment boxes, etc, so just get nodes of class UMetasoundEditorGraph).
				UMetasoundEditorGraphNode* MetasoundEditorNode = Cast<UMetasoundEditorGraphNode>(EdGraphNode);
				if (!MetasoundEditorNode)
				{
					continue;
				}

				Frontend::FNodeHandle Node = MetasoundEditorNode->GetNodeHandle();

				TArray<UEdGraphPin*> Pins = MetasoundEditorNode->GetAllPins();
				TArray<Frontend::FInputHandle> NodeInputs = Node->GetInputs();

				for (Frontend::FInputHandle& NodeInput : NodeInputs)
				{
					auto IsMatchingInputPin = [&](const UEdGraphPin* Pin) -> bool
					{
						return IsMatchingInputHandleAndPin(NodeInput, *Pin);
					};

					UEdGraphPin* MatchingPin = nullptr;
					if (UEdGraphPin** DoublePointer = Pins.FindByPredicate(IsMatchingInputPin))
					{
						MatchingPin = *DoublePointer;
					}

					if (ensure(MatchingPin))
					{
						// Remove pin so it isn't used twice.
						Pins.Remove(MatchingPin);

						bool bShowConnectionInEditor = true;

						Frontend::FOutputHandle OutputHandle = NodeInput->GetCurrentlyConnectedOutput();
						if (OutputHandle->IsValid())
						{
							Frontend::FNodeHandle InputNodeHandle = OutputHandle->GetOwningNode();
							const FMetasoundFrontendNodeStyle& InputNodeStyle = InputNodeHandle->GetNodeStyle();
							bShowConnectionInEditor = InputNodeStyle.Display.Visibility == EMetasoundFrontendNodeStyleDisplayVisibility::Visible;
						}
						else
						{
							bShowConnectionInEditor = false;
						}
						
						if (bShowConnectionInEditor)
						{
							bool bAddLink = false;

							if (MatchingPin->LinkedTo.IsEmpty())
							{
								// No link currently exists. Add the appropriate link.
								bAddLink = true;
							}
							else if (!IsMatchingOutputHandleAndPin(OutputHandle, *MatchingPin->LinkedTo[0]))
							{
								// The wrong link exists.
								MatchingPin->BreakAllPinLinks();
								bAddLink = true;
							}

							if (bAddLink)
							{
								const FGuid InputNodeID = OutputHandle->GetOwningNodeID();
								UMetasoundEditorGraphNode* OutputEditorNode = EditorNodesByID[InputNodeID];
								UEdGraphPin* OutputPin = OutputEditorNode->FindPinChecked(OutputHandle->GetName(), EEdGraphPinDirection::EGPD_Output);
								MatchingPin->MakeLinkTo(OutputPin);
								bIsNodeDirty = true;
							}
						}
						// No link should exist.
						else
						{
							MatchingPin->BreakAllPinLinks();
							bIsNodeDirty = true;
						}
					}
				}

				if (bIsNodeDirty)
				{
					EdGraphNode->MarkPackageDirty();
				}

				bIsGraphDirty |= bIsNodeDirty;
			}

			if (bIsGraphDirty)
			{
				EditorGraph->MarkPackageDirty();
			}

			return bIsGraphDirty;
		}
	} // namespace Editor
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
