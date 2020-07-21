// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusNode.h"

#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"

#include "UObject/UObjectIterator.h"
#include "Misc/StringBuilder.h"


const FName UOptimusNode::CategoryName::Attributes("Attributes");
const FName UOptimusNode::CategoryName::Events("Events");
const FName UOptimusNode::CategoryName::Meshes("Meshes");
const FName UOptimusNode::CategoryName::Deformers("Deformers");

const FName UOptimusNode::PropertyMeta::Input("Input");
const FName UOptimusNode::PropertyMeta::Output("Output");


// Cached list of node classes
TArray<UClass*> UOptimusNode::CachedNodesClasses;


UOptimusNode::UOptimusNode()
{
	// Construct the pins that will represent the input/outputs for this node.
	if (!(GetFlags() & RF_ClassDefaultObject))
	{
		CreatePinsFromStructLayout(GetClass(), nullptr);
	}
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

	Notify(EOptimusNodeGraphNotifyType::NodeDisplayNameChanged);

	return true;
}



bool UOptimusNode::SetGraphPosition(const FVector2D& InPosition)
{
	if (InPosition.ContainsNaN())
	{
		return false;
	}

	GraphPosition = InPosition;

	Notify(EOptimusNodeGraphNotifyType::NodePositionChanged);

	return true;
}


FString UOptimusNode::GetNodePath() const
{
	UOptimusNodeGraph* Graph = Cast<UOptimusNodeGraph>(GetOuter());
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


UOptimusNodePin* UOptimusNode::FindPin(const FString& InPinPath)
{
	TArray<FName> PinPath = UOptimusNodePin::GetPinNamePathFromString(InPinPath);
	if (PinPath.Num() == 0)
	{
		return nullptr;
	}

	UOptimusNodePin* const* PinPtrPtr = CachedPinLookup.Find(PinPath);
	if (PinPtrPtr)
	{
		return *PinPtrPtr;
	}

	const TArray<UOptimusNodePin*>* CurrentPins = &Pins;
	int PathIndex = 0;
	UOptimusNodePin* FoundPin = nullptr;

	for (FName PinName: PinPath)
	{
		if (CurrentPins == nullptr || CurrentPins->Num() == 0)
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

	CachedPinLookup.Add(PinPath, FoundPin);

	return FoundPin;
}


TArray<UClass*> UOptimusNode::GetAllNodeClasses()
{
	if (CachedNodesClasses.Num() == 0)
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


void UOptimusNode::Notify(EOptimusNodeGraphNotifyType InNotifyType)
{
	UOptimusNodeGraph *Graph = Cast<UOptimusNodeGraph>(GetOuter());

	if (Graph)
	{
		Graph->Notify(InNotifyType, this);
	}
}


void UOptimusNode::CreatePinsFromStructLayout(
	UStruct* InStruct, 
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
		else
		{
			if (Property->HasMetaData(PropertyMeta::Input))
			{
				CreatePinFromProperty(Property, InParentPin, EOptimusNodePinDirection::Input);
			}
			if (Property->HasMetaData(PropertyMeta::Output))
			{
				CreatePinFromProperty(Property, InParentPin, EOptimusNodePinDirection::Output);
			}
		}

	}
}


UOptimusNodePin* UOptimusNode::CreatePinFromProperty(
	const FProperty* InProperty,
	UOptimusNodePin* InParentPin,
	EOptimusNodePinDirection InDirection
	)
{
	UObject* PinParent = InParentPin ? Cast<UObject>(InParentPin) : this;
	UOptimusNodePin* Pin = NewObject<UOptimusNodePin>(PinParent, InProperty->GetFName(), RF_Public| RF_Transactional);

	Pin->InitializeFromProperty(InDirection, InProperty);

	if (InParentPin)
	{
		InParentPin->AddSubPin(Pin);
	}
	else
	{
		Pins.Add(Pin);
	}

	if (const FStructProperty* StructProperty = CastField<const FStructProperty>(InProperty))
	{
		// FIXME: Whitelisting.
		CreatePinsFromStructLayout(StructProperty->Struct, Pin);
	}

	return Pin;
}

