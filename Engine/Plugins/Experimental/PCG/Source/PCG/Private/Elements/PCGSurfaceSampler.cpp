// Copyright Epic Games, Inc. All Rights Reserved.

#include "Elements/PCGSurfaceSampler.h"

#include "PCGComponent.h"
#include "PCGContext.h"
#include "PCGCustomVersion.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGHelpers.h"
#include "PCGPin.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Helpers/PCGAsync.h"
#include "Helpers/PCGSettingsHelpers.h"

#include "Math/RandomStream.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGSurfaceSampler)

#define LOCTEXT_NAMESPACE "PCGSurfaceSamplerElement"

namespace PCGSurfaceSamplerConstants
{
	const FName SurfaceLabel = TEXT("Surface");
	const FName BoundingShapeLabel = TEXT("Bounding Shape");
}

namespace PCGSurfaceSampler
{
	bool FSurfaceSamplerSettings::Initialize(const UPCGSurfaceSamplerSettings* InSettings, FPCGContext* Context, const FBox& InputBounds)
	{
		if (!Context)
		{
			return false;
		}

		Settings = InSettings;

		if (Settings)
		{
			// Compute used values
			PointsPerSquaredMeter = Settings->PointsPerSquaredMeter;
			PointExtents = Settings->PointExtents;
			Looseness = Settings->Looseness;
			bApplyDensityToPoints = Settings->bApplyDensityToPoints;
			PointSteepness = Settings->PointSteepness;
#if WITH_EDITOR
			bKeepZeroDensityPoints = Settings->bKeepZeroDensityPoints;
#endif
		}

		Seed = Context->GetSeed();

		// Conceptually, we will break down the surface bounds in a N x M grid
		InterstitialDistance = PointExtents * 2;
		InnerCellSize = InterstitialDistance * Looseness;
		CellSize = InterstitialDistance + InnerCellSize;
		check(CellSize.X > 0 && CellSize.Y > 0);

		// By using scaled indices in the world, we can easily make this process deterministic
		CellMinX = FMath::CeilToInt((InputBounds.Min.X) / CellSize.X);
		CellMaxX = FMath::FloorToInt((InputBounds.Max.X) / CellSize.X);
		CellMinY = FMath::CeilToInt((InputBounds.Min.Y) / CellSize.Y);
		CellMaxY = FMath::FloorToInt((InputBounds.Max.Y) / CellSize.Y);

		if (CellMinX > CellMaxX || CellMinY > CellMaxY)
		{
			if (Context)
			{
				PCGE_LOG_C(Verbose, Context, "Skipped - invalid cell bounds");
			}
			
			return false;
		}

		CellCount = (1 + CellMaxX - CellMinX) * (1 + CellMaxY - CellMinY);
		check(CellCount > 0);

		const FVector::FReal InvSquaredMeterUnits = 1.0 / (100.0 * 100.0);
		TargetPointCount = (InputBounds.Max.X - InputBounds.Min.X) * (InputBounds.Max.Y - InputBounds.Min.Y) * PointsPerSquaredMeter * InvSquaredMeterUnits;

		if (TargetPointCount == 0)
		{
			if (Context)
			{
				PCGE_LOG_C(Verbose, Context, "Skipped - density yields no points");
			}
			
			return false;
		}
		else if (TargetPointCount > CellCount)
		{
			TargetPointCount = CellCount;
		}

		Ratio = TargetPointCount / (FVector::FReal)CellCount;

		InputBoundsMinZ = InputBounds.Min.Z;
		InputBoundsMaxZ = InputBounds.Max.Z;

		return true;
	}

	FIntVector2 FSurfaceSamplerSettings::ComputeCellIndices(int32 Index) const
	{
		check(Index >= 0 && Index < CellCount);
		const int32 CellCountX = 1 + CellMaxX - CellMinX;

		return FIntVector2(CellMinX + (Index % CellCountX), CellMinY + (Index / CellCountX));
	}

	UPCGPointData* SampleSurface(FPCGContext* Context, const UPCGSpatialData* InSurface, const UPCGSpatialData* InBoundingShape, const FSurfaceSamplerSettings& LoopData)
	{
		UPCGPointData* SampledData = NewObject<UPCGPointData>();
		SampledData->InitializeFromData(InSurface);

		SampleSurface(Context, InSurface, InBoundingShape, LoopData, SampledData);

		return SampledData;
	}

	void SampleSurface(FPCGContext* Context, const UPCGSpatialData* InSurface, const UPCGSpatialData* InBoundingShape, const FSurfaceSamplerSettings& LoopData, UPCGPointData* SampledData)
	{
		check(InSurface);

		TArray<FPCGPoint>& SampledPoints = SampledData->GetMutablePoints();

		FPCGProjectionParams ProjectionParams{};

		// Drop points slightly by an epsilon otherwise point can be culled. If the sampler has a volume connected as the Bounding Shape,
		// the volume will call through to PCGHelpers::IsInsideBounds() which is a one sided test and points at the top of the volume
		// will fail it. TODO perhaps the one-sided check can be isolated to component-bounds
		const FVector::FReal ZMultiplier = 1.0 - UE_DOUBLE_SMALL_NUMBER;
		// Try to use a multiplier instead of a simply offset to combat loss of precision in floats. However if MaxZ is very small,
		// then multiplier will not work, so just use an offset.
		FVector::FReal SampleZ = (FMath::Abs(LoopData.InputBoundsMaxZ) > UE_DOUBLE_SMALL_NUMBER) ? LoopData.InputBoundsMaxZ * ZMultiplier : -UE_DOUBLE_SMALL_NUMBER;
		// Make sure we're still in bounds though!
		SampleZ = FMath::Max(SampleZ, LoopData.InputBoundsMinZ);

		FPCGAsync::AsyncPointProcessing(Context, LoopData.CellCount, SampledPoints, [&LoopData, SampledData, InBoundingShape, InSurface, &ProjectionParams, SampleZ](int32 Index, FPCGPoint& OutPoint)
		{
			const FIntVector2 Indices = LoopData.ComputeCellIndices(Index);

			const FVector::FReal CurrentX = Indices.X * LoopData.CellSize.X;
			const FVector::FReal CurrentY = Indices.Y * LoopData.CellSize.Y;
			const FVector InnerCellSize = LoopData.InnerCellSize;

			FRandomStream RandomSource(PCGHelpers::ComputeSeed(LoopData.Seed, Indices.X, Indices.Y));
			float Chance = RandomSource.FRand();

			const float Ratio = LoopData.Ratio;

			if (Chance >= Ratio)
			{
				return false;
			}

			const float RandX = RandomSource.FRand();
			const float RandY = RandomSource.FRand();

			const FVector TentativeLocation = FVector(CurrentX + RandX * InnerCellSize.X, CurrentY + RandY * InnerCellSize.Y, SampleZ);
			const FBox LocalBound(-LoopData.PointExtents, LoopData.PointExtents);

			// Firstly project onto elected generating shape to move to final position.
			if (!InSurface->ProjectPoint(FTransform(TentativeLocation), LocalBound, ProjectionParams, OutPoint, SampledData->Metadata))
			{
				return false;
			}

			// Now run gauntlet of shape network (if there is one) to accept or reject the point.
			if (InBoundingShape)
			{
				FPCGPoint BoundingShapeSample;
#if WITH_EDITOR
				if (!InBoundingShape->SamplePoint(OutPoint.Transform, OutPoint.GetLocalBounds(), BoundingShapeSample, nullptr) && !LoopData.bKeepZeroDensityPoints)
#else
				if (!InBoundingShape->SamplePoint(OutPoint.Transform, OutPoint.GetLocalBounds(), BoundingShapeSample, nullptr))
#endif
				{
					return false;
				}

				// Produce smooth density field
				OutPoint.Density *= BoundingShapeSample.Density;
			}

			// Apply final parameters on the point
			OutPoint.SetExtents(LoopData.PointExtents);
			OutPoint.Density *= (LoopData.bApplyDensityToPoints ? ((Ratio - Chance) / Ratio) : 1.0f);
			OutPoint.Steepness = LoopData.PointSteepness;
			OutPoint.Seed = RandomSource.GetCurrentSeed();

			return true;
		});

		if (Context)
		{
			PCGE_LOG_C(Verbose, Context, "Generated %d points in %d cells", SampledPoints.Num(), LoopData.CellCount);
		}
	}

#if WITH_EDITOR
	static bool IsPinOnlyConnectedToInputNode(UPCGPin* DownstreamPin, UPCGNode* GraphInputNode)
	{
		if (DownstreamPin->Edges.Num() == 1)
		{
			const UPCGEdge* Edge = DownstreamPin->Edges[0];
			const UPCGNode* UpstreamNode = (Edge && Edge->InputPin) ? Edge->InputPin->Node : nullptr;
			const bool bConnectedToInputNode = UpstreamNode && (GraphInputNode == UpstreamNode);
			const bool bConnectedToInputPin = Edge && (Edge->InputPin->Properties.Label == FName(TEXT("In")) || Edge->InputPin->Properties.Label == FName(TEXT("Input")));
			return bConnectedToInputNode && bConnectedToInputPin;
		}

		return false;
	}
#endif
}

UPCGSurfaceSamplerSettings::UPCGSurfaceSamplerSettings()
{
	bUseSeed = true;
}

#if WITH_EDITOR
FText UPCGSurfaceSamplerSettings::GetNodeTooltipText() const
{
	return LOCTEXT("SurfaceSamplerNodeTooltip", "Generates points in two dimensional domain that sample the Surface input and lie within the Bounding Shape input.");
}
#endif

TArray<FPCGPinProperties> UPCGSurfaceSamplerSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGSurfaceSamplerConstants::SurfaceLabel, EPCGDataType::Surface, /*bAllowMultipleConnections=*/true, /*bAllowMultipleData=*/true, LOCTEXT("SurfaceSamplerSurfacePinTooltip",
		"The surface to sample with points. Points will be generated in the two dimensional footprint of the combined bounds of the Surface and the Bounding Shape (if any) "
		"and then projected onto this surface. If this input is omitted then the network of shapes connected to the Bounding Shape pin will be inspected for a surface "
		"shape to use to project the points onto."
	));
	// Only one connection allowed, user can union multiple shapes
	PinProperties.Emplace(PCGSurfaceSamplerConstants::BoundingShapeLabel, EPCGDataType::Spatial, /*bInAllowMultipleConnections=*/false, /*bAllowMultipleData=*/false, LOCTEXT("SurfaceSamplerBoundingShapePinTooltip",
		"All sampled points must be contained within this shape. If this input is omitted then bounds will be taken from the actor so that points are contained within actor bounds. "
		"The Unbounded property disables this and instead generates over the entire bounds of Surface."
	));

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGSurfaceSamplerSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);

	return PinProperties;
}

void UPCGSurfaceSamplerSettings::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if (PointRadius_DEPRECATED != 0)
	{
		PointExtents = FVector(PointRadius_DEPRECATED);
		PointRadius_DEPRECATED = 0;
	}
#endif // WITH_EDITOR
}

#if WITH_EDITOR
bool UPCGSurfaceSamplerSettings::IsPinUsedByNodeExecution(const UPCGPin* InPin) const
{
	return !bUnbounded || InPin->Properties.Label != PCGSurfaceSamplerConstants::BoundingShapeLabel;
}
#endif

FPCGElementPtr UPCGSurfaceSamplerSettings::CreateElement() const
{
	return MakeShared<FPCGSurfaceSamplerElement>();
}

bool FPCGSurfaceSamplerElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGSurfaceSamplerElement::Execute);
	// TODO: time-sliced implementation
	check(Context);
	const UPCGSurfaceSamplerSettings* Settings = Context->GetInputSettings<UPCGSurfaceSamplerSettings>();
	check(Settings);
	
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	// Grab the Bounding Shape input if there is one.
	TArray<FPCGTaggedData> BoundingShapeInputs = Context->InputData.GetInputsByPin(PCGSurfaceSamplerConstants::BoundingShapeLabel);
	const UPCGSpatialData* BoundingShapeSpatialInput = nullptr;
	if (!Settings->bUnbounded)
	{
		if (BoundingShapeInputs.Num() > 0)
		{
			ensure(BoundingShapeInputs.Num() == 1);
			BoundingShapeSpatialInput = Cast<UPCGSpatialData>(BoundingShapeInputs[0].Data);
		}
		else if (Context->SourceComponent.IsValid())
		{
			// Fallback to getting bounds from actor
			BoundingShapeSpatialInput = Cast<UPCGSpatialData>(Context->SourceComponent->GetActorPCGData());
		}
	}
	else if (BoundingShapeInputs.Num() > 0)
	{
		PCGE_LOG(Verbose, "The bounds of the Bounding Shape input pin will be ignored because the Unbounded option is enabled.");
	}

	FBox BoundingShapeBounds(EForceInit::ForceInit);
	if (BoundingShapeSpatialInput)
	{
		BoundingShapeBounds = BoundingShapeSpatialInput->GetBounds();
	}

	TArray<FPCGTaggedData> SurfaceInputs = Context->InputData.GetInputsByPin(PCGSurfaceSamplerConstants::SurfaceLabel);

	// Construct a list of shapes to generate samples from. Prefer to get these directly from the first input pin.
	TArray<const UPCGSpatialData*, TInlineAllocator<16>> GeneratingShapes;
	for (FPCGTaggedData& TaggedData : SurfaceInputs)
	{
		if (const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(TaggedData.Data))
		{
			// Find a concrete shape for sampling. Prefer a 2D surface if we can find one.
			if (const UPCGSpatialData* SurfaceData = SpatialData->FindShapeFromNetwork(/*InDimension=*/2))
			{
				GeneratingShapes.Add(SurfaceData);
				Outputs.Add(TaggedData);
			}
			else if (const UPCGSpatialData* ConcreteData = SpatialData->FindFirstConcreteShapeFromNetwork())
			{
				// Alternatively surface-sample any concrete data - can be used to sprinkle samples down onto shapes like volumes.
				// Searching like this allows the user to plonk in any composite network and it will often find the shape of interest.
				// A potential extension would be to find all (unique?) concrete shapes and use all of them rather than just the first.
				GeneratingShapes.Add(ConcreteData);
				Outputs.Add(TaggedData);
			}
		}
	}

	// If no shapes were obtained from the first input pin, try to find a shape to sample from nodes connected to the second pin.
	if (GeneratingShapes.Num() == 0 && BoundingShapeSpatialInput)
	{
		if (const UPCGSpatialData* GeneratorFromBoundingShapeInput = BoundingShapeSpatialInput->FindShapeFromNetwork(/*InDimension=*/2))
		{
			GeneratingShapes.Add(GeneratorFromBoundingShapeInput);

			// If there was a bounding shape input, use it as the starting point to get the tags
			if (BoundingShapeInputs.Num() > 0)
			{
				Outputs.Add(BoundingShapeInputs[0]);
			}
			else
			{
				Outputs.Emplace();
			}
		}
	}

	// Warn if something is connected but no shape could be obtained for sampling
	if (GeneratingShapes.Num() == 0 && (BoundingShapeInputs.Num() > 0 || SurfaceInputs.Num() > 0))
	{
		PCGE_LOG(Warning, "No Surface input was provided, and no surface could be found in the Bounding Shape input for sampling. Connect the surface to be sampled to the Surface input.");
	}

	// Early out on invalid settings
	// TODO: we could compute an approximate radius based on the points per squared meters if that's useful
	const FVector& PointExtents = Settings->PointExtents;
	if(PointExtents.X <= 0 || PointExtents.Y <= 0)
	{
		PCGE_LOG(Warning, "Skipped - Invalid point extents");
		return true;
	}
	
	// TODO: embarassingly parallel loop
	for (int GenerationIndex = 0; GenerationIndex < GeneratingShapes.Num(); ++GenerationIndex)
	{
		// If we have generating shape inputs, use them
		const UPCGSpatialData* GeneratingShape = GeneratingShapes[GenerationIndex];
		check(GeneratingShape);

		// Calculate the intersection of bounds of the provided inputs
		FBox InputBounds = GeneratingShape->GetBounds();
		if (BoundingShapeBounds.IsValid)
		{
			InputBounds = PCGHelpers::OverlapBounds(InputBounds, BoundingShapeBounds);
		}

		PCGSurfaceSampler::FSurfaceSamplerSettings LoopData;
		if (!InputBounds.IsValid || !LoopData.Initialize(Settings, Context, InputBounds))
		{
			if (!InputBounds.IsValid)
			{
				PCGE_LOG(Verbose, "Input data has invalid bounds");
			}

			Outputs.RemoveAt(GenerationIndex);
			GeneratingShapes.RemoveAt(GenerationIndex);
			--GenerationIndex;
			continue;
		}

		// Sample surface
		Outputs[GenerationIndex].Data = PCGSurfaceSampler::SampleSurface(Context, GeneratingShape, BoundingShapeSpatialInput, LoopData);
	}

	// Finally, forward any exclusions/settings
	Outputs.Append(Context->InputData.GetAllSettings());

	return true;
}

void FPCGSurfaceSamplerElement::GetDependenciesCrc(const FPCGDataCollection& InInput, const UPCGSettings* InSettings, UPCGComponent* InComponent, FPCGCrc& OutCrc) const
{
	FPCGCrc Crc;
	FSimplePCGElement::GetDependenciesCrc(InInput, InSettings, InComponent, Crc);

	if (const UPCGSurfaceSamplerSettings* Settings = Cast<UPCGSurfaceSamplerSettings>(InSettings))
	{
		bool bUnbounded;
		PCGSettingsHelpers::GetOverrideValue(InInput, Settings, GET_MEMBER_NAME_CHECKED(UPCGSurfaceSamplerSettings, bUnbounded), Settings->bUnbounded, bUnbounded);
		const bool bBoundsConnected = InInput.GetInputsByPin(PCGSurfaceSamplerConstants::BoundingShapeLabel).Num() > 0;

		// If we're operating in bounded mode and there is no bounding shape connected then we'll use actor bounds, and therefore take
		// dependency on actor data.
		if (!bUnbounded && !bBoundsConnected && InComponent)
		{
			if (const UPCGData* Data = InComponent->GetActorPCGData())
			{
				Crc.Combine(Data->GetOrComputeCrc());
			}
		}
	}

	OutCrc = Crc;
}

#if WITH_EDITOR
void UPCGSurfaceSamplerSettings::ApplyDeprecationBeforeUpdatePins(UPCGNode* InOutNode, TArray<TObjectPtr<UPCGPin>>& InputPins, TArray<TObjectPtr<UPCGPin>>& OutputPins)
{
	if (DataVersion < FPCGCustomVersion::SplitSamplerNodesInputs && ensure(InOutNode))
	{
		if (InputPins.Num() > 0 && InputPins[0])
		{
			// The node will function the same if we move all connections from "In" to "Bounding Shape". To make this happen, rename "In" to
			// "Bounding Shape" just prior to pin update and the edges will be moved over. In ApplyDeprecation we'll see if we can do better than
			// this baseline functional setup.
			InputPins[0]->Properties.Label = PCGSurfaceSamplerConstants::BoundingShapeLabel;
		}

		// A new params pin was added, migrate the first param connection there if any
		PCGSettingsHelpers::DeprecationBreakOutParamsToNewPin(InOutNode, InputPins, OutputPins);
	}

	Super::ApplyDeprecationBeforeUpdatePins(InOutNode, InputPins, OutputPins);
}

void UPCGSurfaceSamplerSettings::ApplyDeprecation(UPCGNode* InOutNode)
{
	if (DataVersion < FPCGCustomVersion::SplitSamplerNodesInputs && ensure(InOutNode && InOutNode->GetInputPins().Num() >= 2))
	{
		UE_LOG(LogPCG, Log, TEXT("Surface Sampler node migrated from an older version. Review edges on the input pins and then save this graph to upgrade the data."));

		UPCGPin* SurfacePin = InOutNode->GetInputPin(FName(TEXT("Surface")));
		UPCGPin* BoundingShapePin = InOutNode->GetInputPin(FName(TEXT("Bounding Shape")));
		UPCGNode* GraphInputNode = InOutNode->GetGraph() ? InOutNode->GetGraph()->GetInputNode() : nullptr;

		if (SurfacePin && BoundingShapePin && GraphInputNode)
		{
			auto MoveEdgeOnInputNodeToLandscapePin = [InOutNode, GraphInputNode, SurfacePin](UPCGPin* DownstreamPin) {
				// Detect if we're connected to the Input node.
				if (PCGSurfaceSampler::IsPinOnlyConnectedToInputNode(DownstreamPin, GraphInputNode))
				{
					// If we are connected to the Input node, make just a connection from the Surface pin to the Landscape pin and rely on Unbounded setting to provide bounds.
					if (UPCGPin* LandscapePin = GraphInputNode->GetOutputPin(FName(TEXT("Landscape"))))
					{
						DownstreamPin->BreakAllEdges();

						LandscapePin->AddEdgeTo(SurfacePin);
					}
				}
			};

			// The input pin has been split into two. Detect if we have inputs on only one pin and are dealing with older data - if so there's a good chance we can rewire
			// in a better way.
			if (SurfacePin->Edges.Num() == 0 && BoundingShapePin->Edges.Num() > 0)
			{
				MoveEdgeOnInputNodeToLandscapePin(BoundingShapePin);
			}
			else if (SurfacePin->Edges.Num() > 0 && BoundingShapePin->Edges.Num() == 0)
			{
				MoveEdgeOnInputNodeToLandscapePin(SurfacePin);
			}
		}
	}

	Super::ApplyDeprecation(InOutNode);
}
#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
