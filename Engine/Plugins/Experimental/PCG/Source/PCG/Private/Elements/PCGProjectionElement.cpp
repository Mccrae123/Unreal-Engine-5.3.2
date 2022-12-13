// Copyright Epic Games, Inc. All Rights Reserved.

#include "Elements/PCGProjectionElement.h"

#include "PCGCustomVersion.h"
#include "PCGEdge.h"
#include "Helpers/PCGSettingsHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGProjectionElement)

#define LOCTEXT_NAMESPACE "PCGProjectionElement"

TArray<FPCGPinProperties> UPCGProjectionSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Spatial, /*bInAllowMultipleConnections=*/true, LOCTEXT("ProjectionSourcePinTooltip", "The data to project."));
	PinProperties.Emplace(PCGProjectionConstants::ProjectionTargetLabel, EPCGDataType::Spatial, /*bAllowMultipleConnections=*/false, LOCTEXT("ProjectionTargetPinTooltip", "The projection target."));
	PinProperties.Emplace(PCGPinConstants::DefaultParamsLabel, EPCGDataType::Param, /*bInAllowMultipleConnections=*/false);

	return PinProperties;
}

#if WITH_EDITOR
FText UPCGProjectionSettings::GetNodeTooltipText() const
{
	return LOCTEXT("ProjectionNodeTooltip", "Projects each of the inputs connected to In onto the Projection Target and concatenates all of the results to Out. Overrides to the projection settings can be specified using the Params.");
}
#endif

FPCGElementPtr UPCGProjectionSettings::CreateElement() const
{
	return MakeShared<FPCGProjectionElement>();
}

bool FPCGProjectionElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGProjectionElement::Execute);
	check(Context);

	const UPCGProjectionSettings* Settings = Context->GetInputSettings<UPCGProjectionSettings>();
	check(Settings);

	TArray<FPCGTaggedData> Sources = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	TArray<FPCGTaggedData> Targets = Context->InputData.GetInputsByPin(PCGProjectionConstants::ProjectionTargetLabel);

	// If there are no sources or no targets, then nothing to do.
	if (Sources.Num() == 0 || Targets.Num() != 1)
	{
		return true;
	}

	// Ensure we have spatial data to project onto
	UPCGSpatialData* ProjectionTarget = Cast<UPCGSpatialData>(Targets[0].Data);
	if (!ProjectionTarget)
	{
		return true;
	}

	UPCGParamData* Params = Context->InputData.GetParams();

	FPCGProjectionParams ProjectionParams = Settings->Params;
	ProjectionParams.bProjectPositions = PCGSettingsHelpers::GetValue(GET_MEMBER_NAME_CHECKED(FPCGProjectionParams, bProjectPositions), ProjectionParams.bProjectPositions, Params);
	ProjectionParams.bProjectRotations = PCGSettingsHelpers::GetValue(GET_MEMBER_NAME_CHECKED(FPCGProjectionParams, bProjectRotations), ProjectionParams.bProjectRotations, Params);
	ProjectionParams.bProjectScales = PCGSettingsHelpers::GetValue(GET_MEMBER_NAME_CHECKED(FPCGProjectionParams, bProjectScales), ProjectionParams.bProjectScales, Params);
	ProjectionParams.bProjectColors = PCGSettingsHelpers::GetValue(GET_MEMBER_NAME_CHECKED(FPCGProjectionParams, bProjectColors), ProjectionParams.bProjectColors, Params);

	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

#if WITH_EDITORONLY_DATA
	const bool bKeepZeroDensityPoints = PCGSettingsHelpers::GetValue(GET_MEMBER_NAME_CHECKED(UPCGProjectionSettings, bKeepZeroDensityPoints), Settings->bKeepZeroDensityPoints, Params);
#else
	const bool bKeepZeroDensityPoints = false;
#endif

	for (FPCGTaggedData& Source : Sources)
	{
		UPCGSpatialData* ProjectionSource = Cast<UPCGSpatialData>(Source.Data);

		if (!ProjectionSource)
		{
			PCGE_LOG(Error, "Invalid projection source data input found (non-spatial data). Input will be ignored.");
			continue;
		}

		UPCGProjectionData* ProjectionData = ProjectionSource->ProjectOn(ProjectionTarget, ProjectionParams);
#if WITH_EDITORONLY_DATA
		ProjectionData->bKeepZeroDensityPoints = bKeepZeroDensityPoints;
#endif

		if (ProjectionData->RequiresCollapseToSample())
		{
			// Calling ToPointData will populate the point cache. Doing so here means we can pass in the Context object, which
			// means the operation will be multi-threaded. This primes the cache in the most efficient way.
			ProjectionData->ToPointData(Context);
		}

		FPCGTaggedData& ProjectionTaggedData = Outputs.Emplace_GetRef(Source);
		ProjectionTaggedData.Data = ProjectionData;
		ProjectionTaggedData.Tags.Append(Targets[0].Tags);
	}

	// Pass-through exclusions/settings
	Outputs.Append(Context->InputData.GetAllSettings());

	return true;
}

#if WITH_EDITOR
void UPCGProjectionSettings::ApplyDeprecation(UPCGNode* InOutNode)
{
	check(InOutNode);

	if (DataVersion < FPCGCustomVersion::SplitProjectionNodeInputs)
	{
		// Split first pin inputs across two pins. The last edge connected to the first pin becomes the projection target.

		// Loose check we have at least projection source and target pins. If not then this migration code is not valid for this version and should
		// be guarded against.
		check(InOutNode->GetInputPins().Num() >= 2);

		UPCGPin* SourcePin = InOutNode->GetInputPins()[0];
		check(SourcePin);

		if (SourcePin->EdgeCount() > 1)
		{
			UPCGPin* TargetPin = InOutNode->GetInputPins()[1];
			check(TargetPin);

			UPCGEdge* ProjectionTargetEdge = SourcePin->Edges.Last();
			check(ProjectionTargetEdge);

			UPCGPin* UpstreamPin = ProjectionTargetEdge->InputPin;
			check(UpstreamPin);

			UpstreamPin->BreakEdgeTo(SourcePin);
			UpstreamPin->AddEdgeTo(TargetPin);
		}
	}

	Super::ApplyDeprecation(InOutNode);
}
#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
