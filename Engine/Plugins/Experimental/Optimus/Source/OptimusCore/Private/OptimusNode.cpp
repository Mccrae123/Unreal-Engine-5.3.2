// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusNode.h"

#include "Actions/OptimusNodeActions.h"
#include "OptimusActionStack.h"
#include "OptimusCoreModule.h"
#include "OptimusDataTypeRegistry.h"
#include "OptimusDeformer.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"

#include "Algo/Reverse.h"
#include "UObject/UObjectIterator.h"
#include "Misc/StringBuilder.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"


const FName UOptimusNode::CategoryName::Attributes("Attributes");
const FName UOptimusNode::CategoryName::Events("Events");
const FName UOptimusNode::CategoryName::Meshes("Meshes");
const FName UOptimusNode::CategoryName::Deformers("Deformers");
const FName UOptimusNode::CategoryName::Resources("Resources");
const FName UOptimusNode::CategoryName::Variables("Variables");

const FName UOptimusNode::PropertyMeta::Input("Input");
const FName UOptimusNode::PropertyMeta::Output("Output");


// Cached list of node classes
TArray<UClass*> UOptimusNode::CachedNodesClasses;


UOptimusNode::UOptimusNode()
{
}


FName UOptimusNode::GetNodeName() const
{
	return GetClass()->GetFName();
}


FText UOptimusNode::GetDisplayName() const
{
	if (DisplayName.IsEmpty())
	{
		FString Name = GetNodeName().ToString();
		FString PackageName, NodeName;

		if (!Name.Split("_", &PackageName, &NodeName))
		{
			NodeName = Name;
		}

		// Try to make the name a bit prettier.
		return FText::FromString(FName::NameToDisplayString(NodeName, false));
	}

	return DisplayName;
}


bool UOptimusNode::SetDisplayName(FText InDisplayName)
{
	if (DisplayName.EqualTo(InDisplayName))
	{
		return false;
	}
	
	DisplayName = InDisplayName;

	Notify(EOptimusGraphNotifyType::NodeDisplayNameChanged);

	return true;
}



bool UOptimusNode::SetGraphPosition(const FVector2D& InPosition)
{
	return GetActionStack()->RunAction<FOptimusNodeAction_MoveNode>(this, InPosition);
}


bool UOptimusNode::SetGraphPositionDirect(
	const FVector2D& InPosition,
	bool bInNotify
	)
{
	if (InPosition.ContainsNaN() || InPosition.Equals(GraphPosition))
	{
		return false;
	}

	GraphPosition = InPosition;

	if (bInNotify)
	{
		Notify(EOptimusGraphNotifyType::NodePositionChanged);
	}

	return true;
}


FString UOptimusNode::GetNodePath() const
{
	UOptimusNodeGraph* Graph = GetOwningGraph();
	FString GraphPath(TEXT("<Unknown>"));
	if (Graph)
	{
		GraphPath = Graph->GetGraphPath();
	}

	return FString::Printf(TEXT("%s/%s"), *GraphPath, *GetName());
}


UOptimusNodeGraph* UOptimusNode::GetOwningGraph() const
{
	return Cast<UOptimusNodeGraph>(GetOuter());
}


UOptimusNodePin* UOptimusNode::FindPin(const FString& InPinPath) const
{
	TArray<FName> PinPath = UOptimusNodePin::GetPinNamePathFromString(InPinPath);
	if (PinPath.IsEmpty())
	{
		return nullptr;
	}

	return FindPinFromPath(PinPath);
}


UOptimusNodePin* UOptimusNode::FindPinFromPath(const TArray<FName>& InPinPath) const
{
	UOptimusNodePin* const* PinPtrPtr = CachedPinLookup.Find(InPinPath);
	if (PinPtrPtr)
	{
		return *PinPtrPtr;
	}

	const TArray<UOptimusNodePin*>* CurrentPins = &Pins;
	int PathIndex = 0;
	UOptimusNodePin* FoundPin = nullptr;

	for (FName PinName : InPinPath)
	{
		if (CurrentPins == nullptr || CurrentPins->IsEmpty())
		{
			FoundPin = nullptr;
			break;
		}

		UOptimusNodePin* const* FoundPinPtr = CurrentPins->FindByPredicate(
		    [&PinName](const UOptimusNodePin* Pin) {
			    return Pin->GetFName() == PinName;
		    });

		if (FoundPinPtr == nullptr)
		{
			FoundPin = nullptr;
			break;
		}

		FoundPin = *FoundPinPtr;
		CurrentPins = &FoundPin->GetSubPins();
	}

	CachedPinLookup.Add(InPinPath, FoundPin);

	return FoundPin;
}


UOptimusNodePin* UOptimusNode::FindPinFromProperty(const FProperty* InProperty) const
{
	TArray<FName> PinPath;
	while (InProperty)
	{
		PinPath.Add(InProperty->GetFName());
		InProperty = InProperty->GetOwner<FProperty>();
	}

	Algo::Reverse(PinPath);
	
	return FindPinFromPath(PinPath);
}


TArray<UClass*> UOptimusNode::GetAllNodeClasses()
{
	if (CachedNodesClasses.IsEmpty())
	{
		UClass* ClassType = UOptimusNode::StaticClass();

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) &&
				Class->IsChildOf(UOptimusNode::StaticClass()))
			{
				CachedNodesClasses.Add(Class);
			}
		}
	}
	return CachedNodesClasses;
}


void UOptimusNode::PostCreateNode()
{
	CachedPinLookup.Empty();
	Pins.Empty();
	CreatePins();
}


void UOptimusNode::Notify(EOptimusGraphNotifyType InNotifyType)
{
	UOptimusNodeGraph *Graph = Cast<UOptimusNodeGraph>(GetOuter());

	if (Graph)
	{
		Graph->Notify(InNotifyType, this);
	}
}



void UOptimusNode::CreatePins()
{
	CreatePinsFromStructLayout(GetClass(), nullptr);
}


UOptimusNodePin* UOptimusNode::CreatePinFromDataType(
	FName InName,
	FOptimusDataTypeRef InDataType, 
	UOptimusNodePin* InParentPin, 
	EOptimusNodePinDirection InDirection
	)
{
	UObject* PinParent = InParentPin ? Cast<UObject>(InParentPin) : this;
	UOptimusNodePin* Pin = NewObject<UOptimusNodePin>(PinParent, InName, RF_Public | RF_Transactional);

	Pin->SetDirectionAndDataType(InDirection, InDataType);

	if (InParentPin)
	{
		InParentPin->AddSubPin(Pin);
	}
	else
	{
		Pins.Add(Pin);
	}

	// Add sub-pins, if the registered type is set to show them.
	if (EnumHasAnyFlags(InDataType->TypeFlags, EOptimusDataTypeFlags::ShowElements))
	{
		if (const UScriptStruct* Struct = Cast<const UScriptStruct>(InDataType->TypeObject))
		{
			CreatePinsFromStructLayout(Struct, Pin);
		}
	}

	return Pin;
}


void UOptimusNode::CreatePinsFromStructLayout(
	const UStruct* InStruct, 
	UOptimusNodePin* InParentPin
	)
{
	for (const FProperty* Property : TFieldRange<FProperty>(InStruct))
	{
		if (InParentPin)
		{
			// Sub-pins keep the same direction as the parent.
			CreatePinFromProperty(Property, InParentPin, InParentPin->GetDirection());
		}
		else if (Property->HasMetaData(PropertyMeta::Input))
		{
			if (Property->HasMetaData(PropertyMeta::Output))
			{
				UE_LOG(LogOptimusCore, Error, TEXT("Pin on %s.%s marked both input and output. Ignoring it as output."),
					*GetName(), *Property->GetName());
			}

			CreatePinFromProperty(Property, InParentPin, EOptimusNodePinDirection::Input);
		}
		else if (Property->HasMetaData(PropertyMeta::Output))
		{
			CreatePinFromProperty(Property, InParentPin, EOptimusNodePinDirection::Output);
		}
	}
}


UOptimusNodePin* UOptimusNode::CreatePinFromProperty(
	const FProperty* InProperty,
	UOptimusNodePin* InParentPin,
	EOptimusNodePinDirection InDirection
	)
{
	if (!ensure(InProperty))
	{
		return nullptr;
	}

	// Is this a legitimate type for pins?
	const FOptimusDataTypeRegistry& Registry = FOptimusDataTypeRegistry::Get();

	FOptimusDataTypeHandle DataType = Registry.FindType(*InProperty);

	if (!DataType.IsValid())
	{
		UE_LOG(LogOptimusCore, Error, TEXT("No registered type found for pin '%s'."), *InProperty->GetName());
		return nullptr;
	}

	return CreatePinFromDataType(InProperty->GetFName(), DataType, InParentPin, InDirection);
}

UOptimusActionStack* UOptimusNode::GetActionStack() const
{
	UOptimusNodeGraph *Graph = GetOwningGraph();
	if (Graph == nullptr)
	{
		return nullptr;
	}
	UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(Graph->GetOuter());
	if (!Deformer)
	{
		return nullptr;
	}

	return Deformer->GetActionStack();
}
