// Copyright Epic Games, Inc. All Rights Reserved.

#include "Elements/Metadata/PCGMetadataOpElementBase.h"

#include "PCGParamData.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Helpers/PCGSettingsHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/Accessors/PCGAttributeAccessor.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGMetadataOpElementBase)

void UPCGMetadataSettingsBase::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if (OutputAttributeName_DEPRECATED != NAME_None)
	{
		OutputTarget.Selection = EPCGAttributePropertySelection::Attribute;
		OutputTarget.AttributeName = OutputAttributeName_DEPRECATED;
		OutputAttributeName_DEPRECATED = NAME_None;
	}
#endif // WITH_EDITOR
}

TArray<FPCGPinProperties> UPCGMetadataSettingsBase::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;

	for (uint32 i = 0; i < GetInputPinNum(); ++i)
	{
		const FName PinLabel = GetInputPinLabel(i);
		if (PinLabel != NAME_None)
		{
			PinProperties.Emplace(PinLabel, EPCGDataType::Any, /*bAllowMultipleConnections=*/false);
		}
	}

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGMetadataSettingsBase::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	
	for (uint32 i = 0; i < GetOutputPinNum(); ++i)
	{
		const FName PinLabel = GetOutputPinLabel(i);
		if (PinLabel != NAME_None)
		{
			PinProperties.Emplace(PinLabel, EPCGDataType::Any);
		}
	}

	return PinProperties;
}

bool UPCGMetadataSettingsBase::IsMoreComplexType(uint16 FirstType, uint16 SecondType) const
{
	return FirstType != SecondType && FirstType <= (uint16)(EPCGMetadataTypes::Count) && SecondType <= (uint16)(EPCGMetadataTypes::Count) && PCG::Private::BroadcastableTypes[SecondType][FirstType];
}

bool FPCGMetadataElementBase::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGMetadataElementBase::Execute);

	const UPCGMetadataSettingsBase* Settings = Context->GetInputSettings<UPCGMetadataSettingsBase>();
	check(Settings);

	const uint32 NumberOfInputs = Settings->GetInputPinNum();
	const uint32 NumberOfOutputs = Settings->GetOutputPinNum();

	check(NumberOfInputs > 0);
	check(NumberOfOutputs <= UPCGMetadataSettingsBase::MaxNumberOfOutputs);

	const TArray<FPCGTaggedData>& Inputs = Context->InputData.TaggedData;
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	// Gathering all the inputs metadata
	TArray<const UPCGMetadata*> SourceMetadata;
	TArray<const FPCGMetadataAttributeBase*> SourceAttribute;
	TArray<FPCGTaggedData> InputTaggedData;
	SourceMetadata.SetNum(NumberOfInputs);
	SourceAttribute.SetNum(NumberOfInputs);
	InputTaggedData.SetNum(NumberOfInputs);

	for (uint32 i = 0; i < NumberOfInputs; ++i)
	{
		TArray<FPCGTaggedData> InputData = Context->InputData.GetInputsByPin(Settings->GetInputPinLabel(i));
		if (InputData.Num() != 1)
		{
			PCGE_LOG(Error, "Invalid inputs for pin %d", i);
			return true;
		}

		// By construction, there can only be one of then(hence the 0 index)
		InputTaggedData[i] = MoveTemp(InputData[0]);

		// Only gather Spacial and Params input. 
		if (const UPCGSpatialData* SpatialInput = Cast<const UPCGSpatialData>(InputTaggedData[i].Data))
		{
			SourceMetadata[i] = SpatialInput->Metadata;
		}
		else if (const UPCGParamData* ParamsInput = Cast<const UPCGParamData>(InputTaggedData[i].Data))
		{
			SourceMetadata[i] = ParamsInput->Metadata;
		}
		else
		{
			PCGE_LOG(Error, "Invalid inputs for pin %d", i);
			return true;
		}
	}

	FOperationData OperationData;
	OperationData.InputAccessors.SetNum(NumberOfInputs);
	OperationData.InputKeys.SetNum(NumberOfInputs);
	TArray<int32> NumberOfElements;
	NumberOfElements.SetNum(NumberOfInputs);

	OperationData.MostComplexInputType = (uint16)EPCGMetadataTypes::Unknown;
	OperationData.NumberOfElementsToProcess = -1;

	bool bNoOperationNeeded = false;

	// Use this name to forward it to the output if needed.
	// Only set it if the first input is an attribute.
	FName InputName = NAME_None;

	for (uint32 i = 0; i < NumberOfInputs; ++i)
	{
		// First we verify if the input data match the first one.
		if (i != 0 &&
			InputTaggedData[0].Data->GetClass() != InputTaggedData[i].Data->GetClass() &&
			!InputTaggedData[i].Data->IsA<UPCGParamData>())
		{
			PCGE_LOG(Error, "Input %d is not of the same type than input 0 and is not a param data. Not supported.", i);
			return true;
		}

		FPCGAttributePropertySelector InputSource = Settings->GetInputSource(i);
		// Make sure to update it to transform it into a property if needed.
		// TODO: Remove when it will be handled in the UI widget logic.
		InputSource.Update();

		if (InputSource.Selection == EPCGAttributePropertySelection::Attribute && InputSource.AttributeName == NAME_None)
		{
			InputSource.AttributeName = SourceMetadata[i]->GetLatestAttributeNameOrNone();
		}

		if (i == 0 && InputSource.Selection == EPCGAttributePropertySelection::Attribute)
		{
			InputName = InputSource.AttributeName;
		}

		OperationData.InputAccessors[i] = PCGAttributeAccessorHelpers::CreateConstAccessor(InputTaggedData[i].Data, InputSource);
		OperationData.InputKeys[i] = PCGAttributeAccessorHelpers::CreateConstKeys(InputTaggedData[i].Data, InputSource);

		if (!OperationData.InputAccessors[i].IsValid() || !OperationData.InputKeys[i].IsValid())
		{
			PCGE_LOG(Error, "Attribute/Property %s does not exist for input %d", *InputSource.GetName().ToString(), i);
			return true;
		}

		uint16 AttributeTypeId = OperationData.InputAccessors[i]->GetUnderlyingType();

		// Then verify that the type is OK
		bool bHasSpecialRequirement = false;
		if (!Settings->IsSupportedInputType(AttributeTypeId, i, bHasSpecialRequirement))
		{
			PCGE_LOG(Error, "Attribute/Property %s is not a supported type for input %d", *InputSource.GetName().ToString(), i);
			return true;
		}

		if (!bHasSpecialRequirement)
		{
			// In this case, check if we have a more complex type, or if we can broadcast to the most complex type.
			if (OperationData.MostComplexInputType == (uint16)EPCGMetadataTypes::Unknown || Settings->IsMoreComplexType(AttributeTypeId, OperationData.MostComplexInputType))
			{
				OperationData.MostComplexInputType = AttributeTypeId;
			}
			else if (OperationData.MostComplexInputType != AttributeTypeId && !PCG::Private::IsBroadcastable(AttributeTypeId, OperationData.MostComplexInputType))
			{
				PCGE_LOG(Error, "Attribute %s cannot be broadcasted to match types for input %d", *InputSource.GetName().ToString(), i);
				return true;
			}
		}

		NumberOfElements[i] = OperationData.InputKeys[i]->GetNum();

		if (OperationData.NumberOfElementsToProcess == -1)
		{
			OperationData.NumberOfElementsToProcess = NumberOfElements[i];
		}

		// There is nothing to do if one input doesn't have any element to process.
		// Therefore mark that we have nothing to do and early out.
		if (NumberOfElements[i] == 0)
		{
			PCGE_LOG(Verbose, "No elements in input %d.", i);
			bNoOperationNeeded = true;
			break;
		}

		// Verify that the number of elements makes sense
		if (OperationData.NumberOfElementsToProcess % NumberOfElements[i] != 0)
		{
			PCGE_LOG(Error, "Mismatch between the number of elements in input 0 (%d) and in input %d (%d).", OperationData.NumberOfElementsToProcess, i, NumberOfElements[i]);
			return true;
		}

		if (InputSource.Selection == EPCGAttributePropertySelection::Attribute)
		{
			SourceAttribute[i] = SourceMetadata[i]->GetConstAttribute(InputSource.GetName());
		}
		else
		{
			SourceAttribute[i] = nullptr;
		}
	}

	// If no operation is needed, just forward input 0
	if (bNoOperationNeeded)
	{
		for (uint32 OutputIndex = 0; OutputIndex < Settings->GetOutputPinNum(); ++OutputIndex)
		{
			FPCGTaggedData& OutputData = Outputs.Add_GetRef(InputTaggedData[0]);
			OutputData.Pin = Settings->GetOutputPinLabel(OutputIndex);
		}

		return true;
	}


	// At this point, we verified everything, so we can go forward with the computation, depending on the most complex type
	// So first forward outputs and create the attribute
	OperationData.OutputAccessors.SetNum(Settings->GetOutputPinNum());
	OperationData.OutputKeys.SetNum(Settings->GetOutputPinNum());

	FPCGAttributePropertySelector OutputTarget = Settings->OutputTarget;
	// Make sure to update it to transform it into a property if needed.
	OutputTarget.Update();

	if (OutputTarget.Selection == EPCGAttributePropertySelection::Attribute && OutputTarget.AttributeName == NAME_None)
	{
		OutputTarget.AttributeName = InputName;
	}

	auto CreateAttribute = [&](uint32 OutputIndex, auto DummyOutValue) -> bool
	{
		using AttributeType = decltype(DummyOutValue);

		FPCGTaggedData& OutputData = Outputs.Add_GetRef(InputTaggedData[0]);
		OutputData.Pin = Settings->GetOutputPinLabel(OutputIndex);

		UPCGMetadata* OutMetadata = nullptr;

		FName OutputName = OutputTarget.GetName();

		if (OutputTarget.Selection == EPCGAttributePropertySelection::Attribute)
		{
			// In case of attribute, there is no point of failure before duplicating. So duplicate, create the attribute and then the accessor.
			PCGMetadataElementCommon::DuplicateTaggedData(InputTaggedData[0], OutputData, OutMetadata);
			FPCGMetadataAttributeBase* OutputAttribute = PCGMetadataElementCommon::ClearOrCreateAttribute(OutMetadata, OutputName, AttributeType{});
			if (!OutputAttribute)
			{
				return false;
			}

			// And copy the mapping from the original attribute, if it is not points
			if (!InputTaggedData[0].Data->IsA<UPCGPointData>() && SourceMetadata[0] && SourceAttribute[0])
			{
				PCGMetadataElementCommon::CopyEntryToValueKeyMap(SourceMetadata[0], SourceAttribute[0], OutputAttribute);
			}

			OperationData.OutputAccessors[OutputIndex] = PCGAttributeAccessorHelpers::CreateAccessor(Cast<UPCGData>(OutputData.Data), OutputTarget);
		}
		else if (OutputTarget.Selection == EPCGAttributePropertySelection::PointProperty)
		{
			// In case of property, we need to validate that the property can accept the output type. Verify this before duplicating.
			OperationData.OutputAccessors[OutputIndex] = PCGAttributeAccessorHelpers::CreateAccessor(Cast<UPCGData>(OutputData.Data), OutputTarget);

			if (OperationData.OutputAccessors[OutputIndex].IsValid())
			{
				// We matched a property, check if the output type is valid
				if (!PCG::Private::IsBroadcastable(PCG::Private::MetadataTypes<AttributeType>::Id, OperationData.OutputAccessors[OutputIndex]->GetUnderlyingType()))
				{
					PCGE_LOG(Error, "Property %s cannot be broadcasted to match types for input", *OutputName.ToString());
					return false;
				}

				PCGMetadataElementCommon::DuplicateTaggedData(InputTaggedData[0], OutputData, OutMetadata);
			}
		}

		if (!OperationData.OutputAccessors[OutputIndex].IsValid())
		{
			return false;
		}

		OperationData.OutputKeys[OutputIndex] = PCGAttributeAccessorHelpers::CreateKeys(Cast<UPCGData>(OutputData.Data), OutputTarget);

		return OperationData.OutputKeys[OutputIndex].IsValid();
	};

	auto CreateAllSameAttributes = [&](auto DummyOutValue) -> bool
	{
		for (uint32 i = 0; i < NumberOfOutputs; ++i)
		{
			if (!CreateAttribute(i, DummyOutValue))
			{
				return false;
			}
		}

		return true;
	};

	OperationData.OutputType = Settings->GetOutputType(OperationData.MostComplexInputType);

	bool bCreateAttributeSucceeded = true;

	if (!Settings->HasDifferentOutputTypes())
	{
		bCreateAttributeSucceeded = PCGMetadataAttribute::CallbackWithRightType(OperationData.OutputType, CreateAllSameAttributes);
	}
	else
	{
		TArray<uint16> OutputTypes = Settings->GetAllOutputTypes();
		check(OutputTypes.Num() == NumberOfOutputs);

		for (uint32 i = 0; i < NumberOfOutputs && bCreateAttributeSucceeded; ++i)
		{
			bCreateAttributeSucceeded &= PCGMetadataAttribute::CallbackWithRightType(OutputTypes[i],
				[&](auto DummyOutValue) -> bool {
					return CreateAttribute(i, DummyOutValue);
				});
		}
	}

	if (!bCreateAttributeSucceeded)
	{
		PCGE_LOG(Error, "Error while creating output attributes");
		Outputs.Empty();
		return true;
	}

	OperationData.Settings = Settings;

	if (!DoOperation(OperationData))
	{
		PCGE_LOG(Error, "Error while performing the metadata operation, check logs for more information");
		Outputs.Empty();
	}

	return true;
}

