// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGEditorGraphNodeBase.h"

#include "PCGEditorCommon.h"
#include "PCGEditorGraph.h"
#include "PCGEditorGraphSchema.h"
#include "PCGEditorSettings.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Commands/GenericCommands.h"
#include "GraphEditorActions.h"
#include "GraphEditorSettings.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "Widgets/Colors/SColorPicker.h"

#define LOCTEXT_NAMESPACE "PCGEditorGraphNodeBase"

void UPCGEditorGraphNodeBase::BeginDestroy()
{
	if (PCGNode)
	{
		PCGNode->OnNodeChangedDelegate.RemoveAll(this);
	}

	Super::BeginDestroy();
}

void UPCGEditorGraphNodeBase::Construct(UPCGNode* InPCGNode, EPCGEditorGraphNodeType InNodeType)
{
	check(InPCGNode);
	PCGNode = InPCGNode;
	InPCGNode->OnNodeChangedDelegate.AddUObject(this, &UPCGEditorGraphNodeBase::OnNodeChanged);

	NodePosX = InPCGNode->PositionX;
	NodePosY = InPCGNode->PositionY;
	
	NodeType = InNodeType;

	bCanRenameNode = (InNodeType == EPCGEditorGraphNodeType::Settings);
}

void UPCGEditorGraphNodeBase::GetNodeContextMenuActions(UToolMenu* Menu, class UGraphNodeContextMenuContext* Context) const
{
	if (!Context->Node)
	{
		return;
	}

	{
		FToolMenuSection& Section = Menu->AddSection("EdGraphSchemaNodeActions", LOCTEXT("NodeActionsHeader", "Node Actions"));
		Section.AddMenuEntry(FGraphEditorCommands::Get().BreakNodeLinks);
	}

	{
		FToolMenuSection& Section = Menu->AddSection("EdGraphSchemaOrganization", LOCTEXT("OrganizationHeader", "Organization"));
		Section.AddMenuEntry(
			"PCGNode_SetColor",
			LOCTEXT("PCGNode_SetColor", "Set Node Color"),
			LOCTEXT("PCGNode_SetColorTooltip", "Sets a specific color on the given node. Note that white maps to the default value"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "ColorPicker.Mode"),
			FUIAction(FExecuteAction::CreateUObject(const_cast<UPCGEditorGraphNodeBase*>(this), &UPCGEditorGraphNodeBase::OnPickColor)));

		Section.AddSubMenu("Alignment", LOCTEXT("AlignmentHeader", "Alignment"), FText(), FNewToolMenuDelegate::CreateLambda([](UToolMenu* AlignmentMenu)
		{
			{
				FToolMenuSection& SubSection = AlignmentMenu->AddSection("EdGraphSchemaAlignment", LOCTEXT("AlignHeader", "Align"));
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesTop);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesMiddle);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesBottom);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesLeft);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesCenter);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().AlignNodesRight);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().StraightenConnections);
			}

			{
				FToolMenuSection& SubSection = AlignmentMenu->AddSection("EdGraphSchemaDistribution", LOCTEXT("DistributionHeader", "Distribution"));
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().DistributeNodesHorizontally);
				SubSection.AddMenuEntry(FGraphEditorCommands::Get().DistributeNodesVertically);
			}
		}));
	}
}

void UPCGEditorGraphNodeBase::AutowireNewNode(UEdGraphPin* FromPin)
{
	if (FromPin->Direction == EEdGraphPinDirection::EGPD_Output)
	{
		if (PCGNode && PCGNode->GetInputPins().Num() > 0)
		{
			const FName& InPinName = PCGNode->GetInputPins()[0]->Properties.Label;
			UEdGraphPin* ToPin = FindPinChecked(InPinName, EEdGraphPinDirection::EGPD_Input);
			GetSchema()->TryCreateConnection(FromPin, ToPin);
		}
	}
	else if (FromPin->Direction == EEdGraphPinDirection::EGPD_Input)
	{
		if (PCGNode && PCGNode->GetOutputPins().Num() > 0)
		{
			const FName& OutPinName = PCGNode->GetOutputPins()[0]->Properties.Label;
			UEdGraphPin* ToPin = FindPinChecked(OutPinName, EEdGraphPinDirection::EGPD_Output);
			GetSchema()->TryCreateConnection(FromPin, ToPin);
		}
	}

	NodeConnectionListChanged();
}

void UPCGEditorGraphNodeBase::PrepareForCopying()
{
	if (PCGNode)
	{
		// Temporarily take ownership of the MaterialExpression, so that it is not deleted when cutting
		PCGNode->Rename(nullptr, this, REN_DontCreateRedirectors | REN_DoNotDirty);
	}
}

bool UPCGEditorGraphNodeBase::CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const
{
	return Schema->IsA(UPCGEditorGraphSchema::StaticClass());
}

void UPCGEditorGraphNodeBase::PostCopy()
{
	if (PCGNode)
	{
		UPCGEditorGraph* PCGEditorGraph = CastChecked<UPCGEditorGraph>(GetGraph());
		UPCGGraph* PCGGraph = PCGEditorGraph->GetPCGGraph();
		check(PCGGraph);
		PCGNode->Rename(nullptr, PCGGraph, REN_DontCreateRedirectors | REN_DoNotDirty);
	}
}

void UPCGEditorGraphNodeBase::PostPasteNode()
{
	bDisableReconstructFromNode = true;
}

void UPCGEditorGraphNodeBase::PostPaste()
{
	if (PCGNode)
	{
		RebuildEdgesFromPins();

		PCGNode->OnNodeChangedDelegate.AddUObject(this, &UPCGEditorGraphNodeBase::OnNodeChanged);
		PCGNode->PositionX = NodePosX;
		PCGNode->PositionY = NodePosY;
	}

	bDisableReconstructFromNode = false;
}

void UPCGEditorGraphNodeBase::RebuildEdgesFromPins()
{
	check(PCGNode);
	check(bDisableReconstructFromNode);

	if (PCGNode->GetGraph())
	{
		PCGNode->GetGraph()->DisableNotificationsForEditor();
	}
	
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin->Direction == EEdGraphPinDirection::EGPD_Output)
		{
			for (UEdGraphPin* ConnectedPin : Pin->LinkedTo)
			{
				UEdGraphNode* ConnectedGraphNode = ConnectedPin->GetOwningNode();
				UPCGEditorGraphNodeBase* ConnectedPCGGraphNode = CastChecked<UPCGEditorGraphNodeBase>(ConnectedGraphNode);

				if (UPCGNode* ConnectedPCGNode = ConnectedPCGGraphNode->GetPCGNode())
				{
					PCGNode->AddEdgeTo(Pin->PinName, ConnectedPCGNode, ConnectedPin->PinName);
				}
			}
		}
	}

	if (PCGNode->GetGraph())
	{
		PCGNode->GetGraph()->EnableNotificationsForEditor();
	}
}

void UPCGEditorGraphNodeBase::OnNodeChanged(UPCGNode* InNode, EPCGChangeType ChangeType)
{
	if (InNode == PCGNode)
	{
		ReconstructNode();
	}
}

void UPCGEditorGraphNodeBase::OnPickColor()
{
	FColorPickerArgs PickerArgs;
	PickerArgs.bIsModal = true;
	PickerArgs.bUseAlpha = false;
	PickerArgs.InitialColorOverride = GetNodeTitleColor();
	PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateUObject(this, &UPCGEditorGraphNodeBase::OnColorPicked);

	OpenColorPicker(PickerArgs);
}

void UPCGEditorGraphNodeBase::OnColorPicked(FLinearColor NewColor)
{
	if (PCGNode && GetNodeTitleColor() != NewColor)
	{
		PCGNode->Modify();
		PCGNode->NodeTitleColor = NewColor;
	}
}

void UPCGEditorGraphNodeBase::ReconstructNode()
{
	// In copy-paste cases, we don't want to remove the pins
	if (bDisableReconstructFromNode)
	{
		return;
	}

	// Remove all current pins
	TArray<UEdGraphPin*> OldPins = Pins;

	for (UEdGraphPin* OldPin : OldPins)
	{
		OldPin->BreakAllPinLinks();
		RemovePin(OldPin);
	}
	check(Pins.IsEmpty());

	// Generate new pins
	AllocateDefaultPins();

	// Generate new links
	// TODO: we should either keep a map in the PCGEditorGraph or do this elsewhere
	// TODO: this will not work if we have non-PCG nodes in the graph
	if (PCGNode)
	{
		UPCGEditorGraph* PCGEditorGraph = CastChecked<UPCGEditorGraph>(GetGraph());
		PCGEditorGraph->CreateLinks(this, /*bCreateInbound=*/true, /*bCreateOutbound=*/true);
	}
	
	// Notify editor
	OnNodeChangedDelegate.ExecuteIfBound();
}

FLinearColor UPCGEditorGraphNodeBase::GetNodeTitleColor() const
{
	if (PCGNode)
	{
		if (PCGNode->NodeTitleColor != FLinearColor::White)
		{
			return PCGNode->NodeTitleColor;
		}
		else if (PCGNode->DefaultSettings)
		{
			FLinearColor SettingsColor = PCGNode->DefaultSettings->GetNodeTitleColor();
			if (SettingsColor == FLinearColor::White)
			{
				SettingsColor = GetDefault<UPCGEditorSettings>()->GetColor(PCGNode->DefaultSettings);
			}

			if (SettingsColor != FLinearColor::White)
			{
				return SettingsColor;
			}
		}
	}

	return GetDefault<UPCGEditorSettings>()->DefaultNodeColor;
}

FEdGraphPinType UPCGEditorGraphNodeBase::GetPinType(const UPCGPin* InPin)
{
	FEdGraphPinType EdPinType;
	EdPinType.ResetToDefaults();
	EdPinType.PinCategory = NAME_None;
	EdPinType.PinSubCategory = NAME_None;
	EdPinType.ContainerType = EPinContainerType::None;

	const EPCGDataType PinType = InPin->Properties.AllowedTypes;
	auto CheckType = [PinType](EPCGDataType AllowedType)
	{
		return !!(PinType & AllowedType) && !(PinType & ~AllowedType);
	};

	if (CheckType(EPCGDataType::Spatial))
	{
		EdPinType.PinCategory = FPCGEditorCommon::SpatialDataType;

		// Assign subcategory if we have precise information
		if (CheckType(EPCGDataType::Point))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::PointDataType;
		}
		else if (CheckType(EPCGDataType::PolyLine))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::PolyLineDataType;
		}
		else if (CheckType(EPCGDataType::RenderTarget))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::RenderTargetDataType;
		}
		else if (CheckType(EPCGDataType::Surface))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::SurfaceDataType;
		}
		else if (CheckType(EPCGDataType::Volume))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::VolumeDataType;
		}
		else if (CheckType(EPCGDataType::Primitive))
		{
			EdPinType.PinSubCategory = FPCGEditorCommon::PrimitiveDataType;
		}
	}
	else if (CheckType(EPCGDataType::Param))
	{
		EdPinType.PinCategory = FPCGEditorCommon::ParamDataType;
	}
	else if (CheckType(EPCGDataType::Settings))
	{
		EdPinType.PinCategory = FPCGEditorCommon::SettingsDataType;
	}
	else if (CheckType(EPCGDataType::Other))
	{
		EdPinType.PinCategory = FPCGEditorCommon::OtherDataType;
	}	

	return EdPinType;
}

FText UPCGEditorGraphNodeBase::GetTooltipText() const
{
	return FText::Format(LOCTEXT("NodeTooltip", "{0}\n{1} - Node index {2}"),
		GetNodeTitle(ENodeTitleType::FullTitle),
		PCGNode ? FText::FromName(PCGNode->GetFName()) :FText(LOCTEXT("InvalidNodeName", "Unbound node")),
		PCGNode ? FText::AsNumber(PCGNode->GetGraph()->GetNodes().IndexOfByKey(PCGNode)) : FText(LOCTEXT("InvalidNodeIndex", "Invalid index")));
}

void UPCGEditorGraphNodeBase::GetPinHoverText(const UEdGraphPin& Pin, FString& HoverTextOut) const
{
	const bool bIsInputPin = (Pin.Direction == EGPD_Input);
	UPCGPin* MatchingPin = (PCGNode ? (bIsInputPin ? PCGNode->GetInputPin(Pin.PinName) : PCGNode->GetOutputPin(Pin.PinName)) : nullptr);

	auto PCGDataTypeToText = [](EPCGDataType DataType)
	{
		TArray<FText> BitFlags;

		for (uint64 BitIndex = 1; BitIndex < 8 * sizeof(EPCGDataType); ++BitIndex)
		{
			const int64 BitValue = static_cast<__underlying_type(EPCGDataType)>(DataType) & (1 << BitIndex);
			if (BitValue)
			{
				BitFlags.Add(StaticEnum<EPCGDataType>()->GetDisplayNameTextByValue(BitValue));
			}
		}

		return FText::Join(FText(LOCTEXT("Delimiter", " | ")), BitFlags);
	};

	auto PinTypeToText = [&PCGDataTypeToText](const FName& Category, UPCGPin* MatchingPin)
	{
		if (Category != NAME_None)
		{
			return FText::FromName(Category);
		}
		else if (MatchingPin && MatchingPin->Properties.AllowedTypes == EPCGDataType::Any)
		{
			return FText::FromName(FName("Any"));
		}
		else if (MatchingPin)
		{
			return PCGDataTypeToText(MatchingPin->Properties.AllowedTypes);
		}
		else
		{
			return FText(LOCTEXT("Unknown data type", "Unknown data type"));
		}
	};

	const FText DataTypeText = PinTypeToText(Pin.PinType.PinCategory, MatchingPin);
	const FText DataSubtypeText = PinTypeToText(Pin.PinType.PinSubCategory, MatchingPin);

	if (MatchingPin != nullptr && bIsInputPin)
	{
		const FText AdditionalInfo = MatchingPin->Properties.bAllowMultipleConnections ? FText(LOCTEXT("SupportsMultiInput", "Supports multiple inputs")) : FText(LOCTEXT("SingleInputOnly", "Supports only one input"));

		HoverTextOut = FText::Format(LOCTEXT("PinHoverToolTipFull", "Type: {0}\nSubtype: {1}\nAdditional information: {2}"),
			DataTypeText,
			DataSubtypeText,
			AdditionalInfo).ToString();
	}
	else
	{
		HoverTextOut = FText::Format(LOCTEXT("PinHoverToolTipFull", "Type: {0}\nSubtype: {1}"),
			DataTypeText,
			DataSubtypeText).ToString();
	}
}

#undef LOCTEXT_NAMESPACE
