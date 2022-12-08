// Copyright Epic Games, Inc. All Rights Reserved.

#include "Elements/PCGCreateAttribute.h"

#include "PCGData.h"
#include "PCGParamData.h"
#include "Data/PCGPointData.h"
#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Helpers/PCGSettingsHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGCreateAttribute)

namespace PCGCreateAttributeConstants
{
	const FName NodeName = TEXT("CreateAttribute");
	const FName SourceLabel = TEXT("Source");
}

void UPCGCreateAttributeSettings::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if ((Type_DEPRECATED != EPCGMetadataTypes::Double) || (DoubleValue_DEPRECATED != 0.0))
	{
		AttributeTypes.Type = Type_DEPRECATED;
		AttributeTypes.DoubleValue = DoubleValue_DEPRECATED;
		AttributeTypes.FloatValue = FloatValue_DEPRECATED;
		AttributeTypes.IntValue = IntValue_DEPRECATED;
		AttributeTypes.Int32Value = Int32Value_DEPRECATED;
		AttributeTypes.Vector2Value = Vector2Value_DEPRECATED;
		AttributeTypes.VectorValue = VectorValue_DEPRECATED;
		AttributeTypes.Vector4Value = Vector4Value_DEPRECATED;
		AttributeTypes.RotatorValue = RotatorValue_DEPRECATED;
		AttributeTypes.QuatValue = QuatValue_DEPRECATED;
		AttributeTypes.TransformValue = TransformValue_DEPRECATED;
		AttributeTypes.BoolValue = BoolValue_DEPRECATED;
		AttributeTypes.StringValue = StringValue_DEPRECATED;
		AttributeTypes.NameValue = NameValue_DEPRECATED;

		Type_DEPRECATED = EPCGMetadataTypes::Double;
		DoubleValue_DEPRECATED = 0.0;
	}
#endif // WITH_EDITOR
}

FName UPCGCreateAttributeSettings::AdditionalTaskName() const
{
	if (bFromSourceParam)
	{
		const FName NodeName = PCGCreateAttributeConstants::NodeName;

		if ((OutputAttributeName == NAME_None) && (SourceParamAttributeName == NAME_None))
		{
			return NodeName;
		}
		else
		{
			const FString AttributeName = ((OutputAttributeName == NAME_None) ? SourceParamAttributeName : OutputAttributeName).ToString();
			return FName(FString::Printf(TEXT("%s %s"), *NodeName.ToString(), *AttributeName));
		}
	}
	else
	{
		return FName(FString::Printf(TEXT("%s: %s"), *OutputAttributeName.ToString(), *AttributeTypes.ToString()));
	}
}

#if WITH_EDITOR
FName UPCGCreateAttributeSettings::GetDefaultNodeName() const
{
	return PCGCreateAttributeConstants::NodeName;
}
#endif // WITH_EDITOR

TArray<FPCGPinProperties> UPCGCreateAttributeSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Any, /*bInAllowMultipleConnections=*/ true);

	if (bFromSourceParam)
	{
		PinProperties.Emplace(PCGCreateAttributeConstants::SourceLabel, EPCGDataType::Param, /*bInAllowMultipleConnections=*/ false);
	}

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGCreateAttributeSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Any);

	return PinProperties;
}

FPCGElementPtr UPCGCreateAttributeSettings::CreateElement() const
{
	return MakeShared<FPCGCreateAttributeElement>();
}

bool FPCGCreateAttributeElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGCreateAttributeElement::Execute);

	check(Context);

	const UPCGCreateAttributeSettings* Settings = Context->GetInputSettings<UPCGCreateAttributeSettings>();
	check(Settings);

	TArray<FPCGTaggedData> SourceParams = Context->InputData.GetInputsByPin(PCGCreateAttributeConstants::SourceLabel);
	UPCGParamData* SourceParamData = nullptr;
	FName SourceParamAttributeName = NAME_None;

	if (Settings->bFromSourceParam)
	{
		if (SourceParams.IsEmpty())
		{
			PCGE_LOG(Error, "Source param was not provided.");
			return true;
		}

		SourceParamData = CastChecked<UPCGParamData>(SourceParams[0].Data);

		if (!SourceParamData->Metadata)
		{
			PCGE_LOG(Error, "Source param data doesn't have metadata");
			return true;
		}

		SourceParamAttributeName = (Settings->SourceParamAttributeName == NAME_None) ? SourceParamData->Metadata->GetLatestAttributeNameOrNone() : Settings->SourceParamAttributeName;

		if (!SourceParamData->Metadata->HasAttribute(SourceParamAttributeName))
		{
			PCGE_LOG(Error, "Source param data doesn't have an attribute \"%s\"", *SourceParamAttributeName.ToString());
			return true;
		}
	}

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	// If the input is empty, we will create a new ParamData.
	// We can re-use this newly object as the output
	bool bCanReuseInputData = false;
	if (Inputs.IsEmpty())
	{
		FPCGTaggedData& NewData = Inputs.Emplace_GetRef();
		NewData.Data = NewObject<UPCGParamData>();
		NewData.Pin = PCGPinConstants::DefaultInputLabel;
		bCanReuseInputData = true;
	}

	for (const FPCGTaggedData& InputTaggedData : Inputs)
	{
		const UPCGData* InputData = InputTaggedData.Data;
		UPCGData* OutputData = nullptr;

		UPCGMetadata* Metadata = nullptr;

		bool bShouldAddNewEntry = false;

		if (const UPCGSpatialData* InputSpatialData = Cast<UPCGSpatialData>(InputData))
		{
			UPCGSpatialData* NewSpatialData = InputSpatialData->DuplicateData(/*bInitializeFromData=*/false);
			NewSpatialData->InitializeFromData(InputSpatialData, /*InMetadataParentOverride=*/ nullptr, /*bInheritMetadata=*/true);

			OutputData = NewSpatialData;
			Metadata = NewSpatialData->Metadata;
		}
		else if (const UPCGParamData* InputParamData = Cast<UPCGParamData>(InputData))
		{
			// If we can reuse input data, it is safe to const_cast, as it was created by ourselves above.
			UPCGParamData* NewParamData = bCanReuseInputData ? const_cast<UPCGParamData*>(InputParamData) : NewObject<UPCGParamData>();
			NewParamData->Metadata->InitializeAsCopy(bCanReuseInputData ? nullptr : InputParamData->Metadata);

			OutputData = NewParamData;
			Metadata = NewParamData->Metadata;

			// In case of param data, we want to add a new entry too, if needed
			bShouldAddNewEntry = true;
		}
		else
		{
			PCGE_LOG(Error, "Invalid data as input. Only support spatial and params");
			continue;
		}

		const FName OutputAttributeName = (Settings->bFromSourceParam && Settings->OutputAttributeName == NAME_None) ? SourceParamAttributeName : Settings->OutputAttributeName;

		FPCGMetadataAttributeBase* Attribute = nullptr;

		if (Settings->bFromSourceParam)
		{
			const FPCGMetadataAttributeBase* SourceAttribute = SourceParamData->Metadata->GetConstAttribute(SourceParamAttributeName);
			Attribute = Metadata->CopyAttribute(SourceAttribute, OutputAttributeName, /*bKeepParent=*/false, /*bCopyEntries=*/bShouldAddNewEntry, /*bCopyValues=*/bShouldAddNewEntry);
		}
		else
		{
			Attribute = ClearOrCreateAttribute(Settings, Metadata, nullptr, &OutputAttributeName);
		}

		if (!Attribute)
		{
			PCGE_LOG(Error, "Error while creating attribute %s", *OutputAttributeName.ToString());
			continue;
		}

		TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;
		FPCGTaggedData& Output = Outputs.Emplace_GetRef();
		Output.Data = OutputData;

		// Add a new entry if it is a param data and not from source (because entries are already copied)
		if (bShouldAddNewEntry && !Settings->bFromSourceParam)
		{
			// If the metadata is empty, we need to add a new entry, so set it to PCGInvalidEntryKey.
			// Otherwise, use the entry key 0.
			PCGMetadataEntryKey EntryKey = Metadata->GetItemCountForChild() == 0 ? PCGInvalidEntryKey : 0;
			SetAttribute(Settings, Attribute, Metadata, EntryKey, nullptr);
		}
	}

	return true;
}

FPCGMetadataAttributeBase* FPCGCreateAttributeElement::ClearOrCreateAttribute(const UPCGCreateAttributeSettings* Settings, UPCGMetadata* Metadata, const UPCGParamData* Params, const FName* OutputAttributeNameOverride) const
{
	check(Metadata);

	auto CreateAttribute = [Settings, Metadata, OutputAttributeNameOverride](auto&& Value) -> FPCGMetadataAttributeBase*
	{
		return PCGMetadataElementCommon::ClearOrCreateAttribute(Metadata, OutputAttributeNameOverride ? *OutputAttributeNameOverride : Settings->OutputAttributeName, Value);
	};

	return Settings->AttributeTypes.DispatcherWithOverride(Params, CreateAttribute);
}

PCGMetadataEntryKey FPCGCreateAttributeElement::SetAttribute(const UPCGCreateAttributeSettings* Settings, FPCGMetadataAttributeBase* Attribute, UPCGMetadata* Metadata, PCGMetadataEntryKey EntryKey, const UPCGParamData* Params) const
{
	check(Attribute && Metadata);

	auto SetAttribute = [Attribute, EntryKey, Metadata](auto&& Value) -> PCGMetadataEntryKey
	{
		using AttributeType = std::remove_reference_t<decltype(Value)>;

		check(Attribute->GetTypeId() == PCG::Private::MetadataTypes<AttributeType>::Id);

		const PCGMetadataEntryKey FinalKey = (EntryKey == PCGInvalidEntryKey) ? Metadata->AddEntry() : EntryKey;

		static_cast<FPCGMetadataAttribute<AttributeType>*>(Attribute)->SetValue(FinalKey, Value);

		return FinalKey;
	};

	return Settings->AttributeTypes.DispatcherWithOverride(Params, SetAttribute);
}

