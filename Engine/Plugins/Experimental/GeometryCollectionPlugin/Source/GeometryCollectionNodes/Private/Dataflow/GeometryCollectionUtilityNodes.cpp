// Copyright Epic Games, Inc. All Rights Reserved.

#include "Dataflow/GeometryCollectionUtilityNodes.h"
#include "Dataflow/DataflowCore.h"

#include "GeometryCollection/ManagedArrayCollection.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"

#include "FractureEngineConvex.h"

#include "MeshSimplification.h"


//#include UE_INLINE_GENERATED_CPP_BY_NAME(GeometryCollectionUtilityNodes)

namespace Dataflow
{

	void GeometryCollectionUtilityNodes()
	{
		static const FLinearColor CDefaultNodeBodyTintColor = FLinearColor(0.f, 0.f, 0.f, 0.5f);

		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FCreateLeafConvexHullsDataflowNode);
		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FSimplifyConvexHullsDataflowNode);
		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FCreateNonOverlappingConvexHullsDataflowNode);
		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FGenerateClusterConvexHullsFromLeafHullsDataflowNode);
		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FGenerateClusterConvexHullsFromChildrenHullsDataflowNode);
		DATAFLOW_NODE_REGISTER_CREATION_FACTORY(FUpdateVolumeAttributesDataflowNode);
	}
}

FCreateLeafConvexHullsDataflowNode::FCreateLeafConvexHullsDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterInputConnection(&OptionalSelectionFilter);
	RegisterInputConnection(&SimplificationDistanceThreshold);
	RegisterOutputConnection(&Collection);
}

void FCreateLeafConvexHullsDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA(&Collection) && IsConnected(&Collection))
	{
		const FManagedArrayCollection& InCollection = GetValue(Context, &Collection);
		if (InCollection.NumElements(FGeometryCollection::TransformGroup) == 0)
		{
			SetValue<FManagedArrayCollection>(Context, InCollection, &Collection);
			return;
		}

		if (TUniquePtr<FGeometryCollection> GeomCollection = TUniquePtr<FGeometryCollection>(InCollection.NewCopy<FGeometryCollection>()))
		{
			TArray<int32> SelectedBones;
			bool bRestrictToSelection = false;
			if (IsConnected(&OptionalSelectionFilter))
			{
				const FDataflowTransformSelection& InOptionalSelectionFilter = GetValue<FDataflowTransformSelection>(Context, &OptionalSelectionFilter);
				bRestrictToSelection = true;
				SelectedBones = InOptionalSelectionFilter.AsArray();
			}

			float InSimplificationDistanceThreshold = GetValue(Context, &SimplificationDistanceThreshold);
			FGeometryCollectionConvexUtility::FIntersectionFilters IntersectionFilters;
			IntersectionFilters.OnlyIntersectIfComputedIsSmallerFactor = IntersectIfComputedIsSmallerByFactor;
			IntersectionFilters.MinExternalVolumeToIntersect = MinExternalVolumeToIntersect;
			FGeometryCollectionConvexUtility::GenerateLeafConvexHulls(*GeomCollection, bRestrictToSelection, SelectedBones, InSimplificationDistanceThreshold, GenerateMethod, IntersectionFilters);
			SetValue(Context, (const FManagedArrayCollection&)(*GeomCollection), &Collection);
		}
	}
}

FSimplifyConvexHullsDataflowNode::FSimplifyConvexHullsDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterInputConnection(&OptionalSelectionFilter);
	RegisterInputConnection(&SimplificationDistanceThreshold);
	RegisterInputConnection(&MinTargetTriangleCount);
	RegisterOutputConnection(&Collection);
}

void FSimplifyConvexHullsDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA(&Collection) && IsConnected(&Collection))
	{
		FManagedArrayCollection InCollection = GetValue(Context, &Collection);
		if (InCollection.NumElements(FGeometryCollection::TransformGroup) == 0)
		{
			SetValue<FManagedArrayCollection>(Context, MoveTemp(InCollection), &Collection);
			return;
		}

		TArray<int32> SelectedBones;
		bool bRestrictToSelection = false;
		if (IsConnected(&OptionalSelectionFilter))
		{
			const FDataflowTransformSelection& InOptionalSelectionFilter = GetValue(Context, &OptionalSelectionFilter);
			bRestrictToSelection = true;
			SelectedBones = InOptionalSelectionFilter.AsArray();
		}

		UE::FractureEngine::Convex::FSimplifyHullSettings Settings;
		Settings.ErrorTolerance = GetValue(Context, &SimplificationDistanceThreshold);
		Settings.bUseGeometricTolerance = true;
		Settings.bUseTargetTriangleCount = true;
		Settings.bUseExistingVertexPositions = bUseExistingVertices;
		Settings.TargetTriangleCount = GetValue(Context, &MinTargetTriangleCount);
		UE::FractureEngine::Convex::SimplifyConvexHulls(InCollection, Settings, bRestrictToSelection, SelectedBones);
		SetValue<FManagedArrayCollection>(Context, MoveTemp(InCollection), &Collection);
	}
}

FCreateNonOverlappingConvexHullsDataflowNode::FCreateNonOverlappingConvexHullsDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterInputConnection(&CanRemoveFraction);
	RegisterInputConnection(&SimplificationDistanceThreshold);
	RegisterInputConnection(&CanExceedFraction);
	RegisterInputConnection(&OverlapRemovalShrinkPercent);
	RegisterOutputConnection(&Collection);
}

void FCreateNonOverlappingConvexHullsDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA<FManagedArrayCollection>(&Collection) && IsConnected(&Collection))
	{
		const FManagedArrayCollection& InCollection = GetValue<FManagedArrayCollection>(Context, &Collection);

		if (TUniquePtr<FGeometryCollection> GeomCollection = TUniquePtr<FGeometryCollection>(InCollection.NewCopy<FGeometryCollection>()))
		{
			float InCanRemoveFraction = GetValue<float>(Context, &CanRemoveFraction);
			float InCanExceedFraction = GetValue<float>(Context, &CanExceedFraction);
			float InSimplificationDistanceThreshold = GetValue<float>(Context, &SimplificationDistanceThreshold);
			float InOverlapRemovalShrinkPercent = GetValue<float>(Context, &OverlapRemovalShrinkPercent);

			FGeometryCollectionConvexUtility::FGeometryCollectionConvexData ConvexData = FGeometryCollectionConvexUtility::CreateNonOverlappingConvexHullData(GeomCollection.Get(), 
				InCanRemoveFraction, 
				InSimplificationDistanceThreshold, 
				InCanExceedFraction,
				(EConvexOverlapRemoval)OverlapRemovalMethod,
				InOverlapRemovalShrinkPercent);

			SetValue<FManagedArrayCollection>(Context, (const FManagedArrayCollection&)(*GeomCollection), &Collection);
		}
	}
}

FGenerateClusterConvexHullsFromLeafHullsDataflowNode::FGenerateClusterConvexHullsFromLeafHullsDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterInputConnection(&ConvexCount);
	RegisterInputConnection(&ErrorTolerance);
	RegisterInputConnection(&OptionalSelectionFilter);
	RegisterInputConnection(&bProtectNegativeSpace);
	RegisterInputConnection(&TargetNumSamples);
	RegisterInputConnection(&MinSampleSpacing);
	RegisterInputConnection(&NegativeSpaceTolerance);
	RegisterInputConnection(&MinRadius);

	RegisterOutputConnection(&Collection);
}

void FGenerateClusterConvexHullsFromLeafHullsDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA<FManagedArrayCollection>(&Collection) && IsConnected(&Collection))
	{
		const FManagedArrayCollection& InCollection = GetValue<FManagedArrayCollection>(Context, &Collection);

		if (TUniquePtr<FGeometryCollection> GeomCollection = TUniquePtr<FGeometryCollection>(InCollection.NewCopy<FGeometryCollection>()))
		{
			TArray<int32> SelectionArray;
			bool bHasSelectionFilter = IsConnected(&OptionalSelectionFilter);
			if (bHasSelectionFilter)
			{
				const FDataflowTransformSelection& InOptionalSelectionFilter = GetValue<FDataflowTransformSelection>(Context, &OptionalSelectionFilter);
				SelectionArray = InOptionalSelectionFilter.AsArray();
			}

			bool bHasNegativeSpace = false;
			UE::Geometry::FSphereCovering NegativeSpace;
			if (GetValue(Context, &bProtectNegativeSpace))
			{
				UE::Geometry::FNegativeSpaceSampleSettings NegativeSpaceSettings;
				NegativeSpaceSettings.TargetNumSamples = GetValue(Context, &TargetNumSamples);
				NegativeSpaceSettings.MinRadius = GetValue(Context, &MinRadius);
				NegativeSpaceSettings.ReduceRadiusMargin = GetValue(Context, &NegativeSpaceTolerance);
				NegativeSpaceSettings.MinSpacing = GetValue(Context, &MinSampleSpacing);
				bHasNegativeSpace = UE::FractureEngine::Convex::ComputeConvexHullsNegativeSpace(*GeomCollection, NegativeSpace, NegativeSpaceSettings, bHasSelectionFilter, SelectionArray);
			}

			const int32 InConvexCount = GetValue(Context, &ConvexCount);
			const double InErrorToleranceInCm = GetValue(Context, &ErrorTolerance);
			FGeometryCollectionConvexUtility::FClusterConvexHullSettings HullMergeSettings(InConvexCount, InErrorToleranceInCm, bPreferExternalCollisionShapes);
			HullMergeSettings.AllowMergesMethod = AllowMerges;
			HullMergeSettings.EmptySpace = bHasNegativeSpace ? &NegativeSpace : nullptr;

			if (bHasSelectionFilter)
			{
				FGeometryCollectionConvexUtility::GenerateClusterConvexHullsFromLeafHulls(
					*GeomCollection,
					HullMergeSettings,
					SelectionArray
				);
			}
			else
			{
				FGeometryCollectionConvexUtility::GenerateClusterConvexHullsFromLeafHulls(
					*GeomCollection,
					HullMergeSettings
				);
			}

			SetValue<FManagedArrayCollection>(Context, static_cast<const FManagedArrayCollection>(*GeomCollection), &Collection);
		}
	}
}

FGenerateClusterConvexHullsFromChildrenHullsDataflowNode::FGenerateClusterConvexHullsFromChildrenHullsDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterInputConnection(&ConvexCount);
	RegisterInputConnection(&ErrorTolerance);
	RegisterInputConnection(&OptionalSelectionFilter);
	RegisterInputConnection(&bProtectNegativeSpace);
	RegisterInputConnection(&TargetNumSamples);
	RegisterInputConnection(&MinSampleSpacing);
	RegisterInputConnection(&NegativeSpaceTolerance);
	RegisterInputConnection(&MinRadius);

	RegisterOutputConnection(&Collection);
}

void FGenerateClusterConvexHullsFromChildrenHullsDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA<FManagedArrayCollection>(&Collection) && IsConnected(&Collection))
	{
		const FManagedArrayCollection& InCollection = GetValue<FManagedArrayCollection>(Context, &Collection);

		if (TUniquePtr<FGeometryCollection> GeomCollection = TUniquePtr<FGeometryCollection>(InCollection.NewCopy<FGeometryCollection>()))
		{
			TArray<int32> SelectionArray;
			bool bHasSelectionFilter = IsConnected(&OptionalSelectionFilter);
			if (bHasSelectionFilter)
			{
				const FDataflowTransformSelection& InOptionalSelectionFilter = GetValue<FDataflowTransformSelection>(Context, &OptionalSelectionFilter);
				SelectionArray = InOptionalSelectionFilter.AsArray();
			}

			bool bHasNegativeSpace = false;
			UE::Geometry::FSphereCovering NegativeSpace;
			if (GetValue(Context, &bProtectNegativeSpace))
			{
				UE::Geometry::FNegativeSpaceSampleSettings NegativeSpaceSettings;
				NegativeSpaceSettings.TargetNumSamples = GetValue(Context, &TargetNumSamples);
				NegativeSpaceSettings.MinRadius = GetValue(Context, &MinRadius);
				NegativeSpaceSettings.ReduceRadiusMargin = GetValue(Context, &NegativeSpaceTolerance);
				NegativeSpaceSettings.MinSpacing = GetValue(Context, &MinSampleSpacing);
				bHasNegativeSpace = UE::FractureEngine::Convex::ComputeConvexHullsNegativeSpace(*GeomCollection, NegativeSpace, NegativeSpaceSettings, bHasSelectionFilter, SelectionArray);
			}

			const int32 InConvexCount = GetValue(Context, &ConvexCount);
			const double InErrorToleranceInCm = GetValue(Context, &ErrorTolerance);
			FGeometryCollectionConvexUtility::FClusterConvexHullSettings HullMergeSettings(InConvexCount, InErrorToleranceInCm, bPreferExternalCollisionShapes);
			HullMergeSettings.AllowMergesMethod = EAllowConvexMergeMethod::Any; // Note: Only 'Any' is supported for this node currently
			HullMergeSettings.EmptySpace = bHasNegativeSpace ? &NegativeSpace : nullptr;

			if (bHasSelectionFilter)
			{
				FGeometryCollectionConvexUtility::GenerateClusterConvexHullsFromChildrenHulls(
					*GeomCollection,
					HullMergeSettings,
					SelectionArray
				);
			}
			else
			{
				FGeometryCollectionConvexUtility::GenerateClusterConvexHullsFromChildrenHulls(
					*GeomCollection,
					HullMergeSettings
				);
			}

			SetValue<FManagedArrayCollection>(Context, static_cast<const FManagedArrayCollection>(*GeomCollection), &Collection);
		}
	}
}


FUpdateVolumeAttributesDataflowNode::FUpdateVolumeAttributesDataflowNode(const Dataflow::FNodeParameters& InParam, FGuid InGuid)
	: FDataflowNode(InParam, InGuid)
{
	RegisterInputConnection(&Collection);
	RegisterOutputConnection(&Collection);
}

void FUpdateVolumeAttributesDataflowNode::Evaluate(Dataflow::FContext& Context, const FDataflowOutput* Out) const
{
	if (Out->IsA(&Collection))
	{
		FManagedArrayCollection InCollection = GetValue(Context, &Collection);
		if (InCollection.NumElements(FGeometryCollection::TransformGroup) > 0)
		{
			FGeometryCollectionConvexUtility::SetVolumeAttributes(&InCollection);
		}
		SetValue(Context, MoveTemp(InCollection), &Collection);
	}
}
