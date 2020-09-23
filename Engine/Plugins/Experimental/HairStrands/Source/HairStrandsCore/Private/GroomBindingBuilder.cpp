// Copyright Epic Games, Inc. All Rights Reserved.

#include "GroomBindingBuilder.h"
#include "GroomAsset.h"
#include "GroomBindingAsset.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "HairStrandsMeshProjection.h"
#include "Async/ParallelFor.h"
#include "GlobalShader.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Eigen for large matrix inversion
// Just to be sure, also added this in Eigen.Build.cs
#ifndef EIGEN_MPL2_ONLY
#define EIGEN_MPL2_ONLY
#endif

#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(push)
#pragma warning(disable:6294) // Ill-defined for-loop:  initial condition does not satisfy test.  Loop body not executed.
#endif
THIRD_PARTY_INCLUDES_START
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseLU>
THIRD_PARTY_INCLUDES_END
#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(pop)
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////

DEFINE_LOG_CATEGORY_STATIC(LogGroomBindingBuilder, Log, All);

#define LOCTEXT_NAMESPACE "GroomBindingBuilder"

///////////////////////////////////////////////////////////////////////////////////////////////////

FString FGroomBindingBuilder::GetVersion()
{
	// Important to update the version when groom building changes
	return TEXT("1");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Common utils functions
// These utils function are a copy of function in HairStrandsMeshProjectionCommon.ush
uint32 FHairStrandsRootUtils::EncodeTriangleIndex(uint32 TriangleIndex, uint32 SectionIndex)
{
	return ((SectionIndex & 0xFF) << 24) | (TriangleIndex & 0xFFFFFF);
}

// This function is a copy of DecodeTriangleIndex in HairStrandsMeshProjectionCommon.ush
void FHairStrandsRootUtils::DecodeTriangleIndex(uint32 Encoded, uint32& OutTriangleIndex, uint32& OutSectionIndex)
{
	OutSectionIndex = (Encoded >> 24) & 0xFF;
	OutTriangleIndex = Encoded & 0xFFFFFF;
}

uint32 FHairStrandsRootUtils::EncodeBarycentrics(const FVector2D& B)
{
	return uint32(FFloat16(B.X).Encoded) | (uint32(FFloat16(B.Y).Encoded)<<16);
}

FVector2D FHairStrandsRootUtils::DecodeBarycentrics(uint32 B)
{
	FFloat16 BX;
	BX.Encoded = (B & 0xFFFF);

	FFloat16 BY;
	BY.Encoded = (B >> 16) & 0xFFFF;

	return FVector2D(BX, BY);
}

uint32 FHairStrandsRootUtils::PackUVs(const FVector2D& UV)
{
	return (FFloat16(UV.X).Encoded & 0xFFFF) | ((FFloat16(UV.Y).Encoded & 0xFFFF) << 16);
}

float FHairStrandsRootUtils::PackUVsToFloat(const FVector2D& UV)
{
	uint32 Encoded = PackUVs(UV);
	return *((float*)(&Encoded));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// RBF weighting

namespace GroomBinding_RBFWeighting
{
	struct FPointsSampler
	{
		FPointsSampler(TArray<bool>& ValidPoints, const FVector* PointPositions, const int32 NumSamples);

		/** Build the sample position from the sample indices */
		void BuildPositions(const FVector* PointPositions);

		/** Compute the furthest point */
		void FurthestPoint(const int32 NumPoints, const FVector* PointPositions, const uint32 SampleIndex, TArray<bool>& ValidPoints, TArray<float>& PointsDistance);

		/** Compute the starting point */
		int32 StartingPoint(const TArray<bool>& ValidPoints, int32& NumPoints) const;

		/** List of sampled points */
		TArray<uint32> SampleIndices;

		/** List of sampled positions */
		TArray<FVector> SamplePositions;
	};

	int32 FPointsSampler::StartingPoint(const TArray<bool>& ValidPoints, int32& NumPoints) const
	{
		int32 StartIndex = -1;
		NumPoints = 0;
		for (int32 i = 0; i < ValidPoints.Num(); ++i)
		{
			if (ValidPoints[i])
			{
				++NumPoints;
				if (StartIndex == -1)
				{
					StartIndex = i;
				}
			}
		}
		return StartIndex;
	}

	void FPointsSampler::BuildPositions(const FVector* PointPositions)
	{
		SamplePositions.SetNum(SampleIndices.Num());
		for (int32 i = 0; i < SampleIndices.Num(); ++i)
		{
			SamplePositions[i] = PointPositions[SampleIndices[i]];
		}
	}

	void FPointsSampler::FurthestPoint(const int32 NumPoints, const FVector* PointPositions, const uint32 SampleIndex, TArray<bool>& ValidPoints, TArray<float>& PointsDistance)
	{
		float FurthestDistance = 0.0;
		uint32 PointIndex = 0;
		for (int32 j = 0; j < NumPoints; ++j)
		{
			if (ValidPoints[j])
			{
				PointsDistance[j] = FMath::Min((PointPositions[SampleIndices[SampleIndex - 1]] - PointPositions[j]).Size(), PointsDistance[j]);
				if (PointsDistance[j] >= FurthestDistance)
				{
					PointIndex = j;
					FurthestDistance = PointsDistance[j];
				}
			}
		}
		ValidPoints[PointIndex] = false;
		SampleIndices[SampleIndex] = PointIndex;
	}

	FPointsSampler::FPointsSampler(TArray<bool>& ValidPoints, const FVector* PointPositions, const int32 NumSamples)
	{
		int32 NumPoints = 0;
		int32 StartIndex = StartingPoint(ValidPoints, NumPoints);

		const int32 SamplesCount = FMath::Min(NumPoints, NumSamples);
		if (SamplesCount != 0)
		{
			SampleIndices.SetNum(SamplesCount);
			SampleIndices[0] = StartIndex;
			ValidPoints[StartIndex] = false;

			TArray<float> PointsDistance;
			PointsDistance.Init(MAX_FLT, ValidPoints.Num());

			for (int32 i = 1; i < SamplesCount; ++i)
			{
				FurthestPoint(ValidPoints.Num(), PointPositions, i, ValidPoints, PointsDistance);
			}
			BuildPositions(PointPositions);
		}
	}

	struct FWeightsBuilder
	{
		FWeightsBuilder(const uint32 NumRows, const uint32 NumColumns,
			const FVector* SourcePositions, const FVector* TargetPositions);

		using EigenMatrix = Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>;

		/** Compute the weights by inverting the matrix*/
		void ComputeWeights(const uint32 NumRows, const uint32 NumColumns);

		/** Entries in the dense structure */
		TArray<float> MatrixEntries;

		/** Entries of the matrix inverse */
		TArray<float> InverseEntries;
	};

	FWeightsBuilder::FWeightsBuilder(const uint32 NumRows, const uint32 NumColumns,
		const FVector* SourcePositions, const FVector* TargetPositions)
	{
		const uint32 PolyRows = NumRows + 4;
		const uint32 PolyColumns = NumColumns + 4;

		MatrixEntries.Init(0.0, PolyRows * PolyColumns);
		InverseEntries.Init(0.0, PolyRows * PolyColumns);
		TArray<float>& LocalEntries = MatrixEntries;
		ParallelFor(NumRows,
			[
				NumRows,
				NumColumns,
				PolyRows,
				PolyColumns,
				SourcePositions,
				TargetPositions,
				&LocalEntries
			] (uint32 RowIndex)
			{
				int32 EntryIndex = RowIndex * PolyColumns;
				for (uint32 j = 0; j < NumColumns; ++j)
				{
					const float FunctionScale = (SourcePositions[RowIndex] - TargetPositions[j]).Size();
					LocalEntries[EntryIndex++] = FMath::Sqrt(FunctionScale * FunctionScale + 1.0);
				}
				LocalEntries[EntryIndex++] = 1.0;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].X;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Y;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Z;

				EntryIndex = NumRows * PolyColumns + RowIndex;
				LocalEntries[EntryIndex] = 1.0;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].X;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].Y;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].Z;

				const float REGUL_VALUE = 1e-4;
				EntryIndex = NumRows * PolyColumns + NumColumns;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

			});
		ComputeWeights(PolyRows, PolyColumns);
	}

	void FWeightsBuilder::ComputeWeights(const uint32 NumRows, const uint32 NumColumns)
	{
		EigenMatrix WeightsMatrix(MatrixEntries.GetData(), NumRows, NumColumns);
		EigenMatrix WeightsInverse(InverseEntries.GetData(), NumColumns, NumRows);

		WeightsInverse = WeightsMatrix.inverse();
	}

	static void UpdateInterpolationWeights(const FWeightsBuilder& InterpolationWeights, const FPointsSampler& PointsSampler, const uint32 LODIndex, FHairStrandsRootData& RootDatas)
	{
		FHairStrandsRootData::FMeshProjectionLOD& CPULOD = RootDatas.MeshProjectionLODs[LODIndex];
		CPULOD.MeshSampleIndicesBuffer.SetNum(PointsSampler.SampleIndices.Num());
		CPULOD.MeshInterpolationWeightsBuffer.SetNum(InterpolationWeights.InverseEntries.Num());
		CPULOD.RestSamplePositionsBuffer.SetNum(PointsSampler.SampleIndices.Num());

		CPULOD.SampleCount = PointsSampler.SampleIndices.Num();
		CPULOD.MeshSampleIndicesBuffer = PointsSampler.SampleIndices;
		CPULOD.MeshInterpolationWeightsBuffer = InterpolationWeights.InverseEntries;
		for (int32 i = 0; i < PointsSampler.SamplePositions.Num(); ++i)
		{
			CPULOD.RestSamplePositionsBuffer[i] = FVector4(PointsSampler.SamplePositions[i], 1.0f);
		}
	}

	static void FillLocalValidPoints(FSkeletalMeshLODRenderData& LODRenderData, const uint32 TargetSection,
		const FHairStrandsRootData::FMeshProjectionLOD& ProjectionLOD, TArray<bool>& ValidPoints)
	{
		TArray<uint32> TriangleIndices; 
		TriangleIndices.SetNum(LODRenderData.MultiSizeIndexContainer.GetIndexBuffer()->Num());
		LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(TriangleIndices);

		ValidPoints.Init(false, LODRenderData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

		const TArray<uint32>& RootBuffers = ProjectionLOD.RootTriangleIndexBuffer;
		for (int32 RootIt = 0; RootIt < RootBuffers.Num(); ++RootIt)
		{
			uint32 SectionIndex  = 0;
			uint32 TriangleIndex = 0;
			FHairStrandsRootUtils::DecodeTriangleIndex(RootBuffers[RootIt], TriangleIndex, SectionIndex);
			if (SectionIndex == TargetSection)
			{
				for (uint32 VertexIt = 0; VertexIt < 3; ++VertexIt)
				{
					const uint32 VertexIndex = TriangleIndices[LODRenderData.RenderSections[SectionIndex].BaseIndex + 3 * TriangleIndex + VertexIt];
					ValidPoints[VertexIndex] = true;
				}
			}
		}
	}

	static void FillGlobalValidPoints(FSkeletalMeshLODRenderData& LODRenderData, const uint32 TargetSection, TArray<bool>& ValidPoints)
	{
		TArray<uint32> TriangleIndices; 
		TriangleIndices.SetNum(LODRenderData.MultiSizeIndexContainer.GetIndexBuffer()->Num());
		LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(TriangleIndices);

		ValidPoints.Init(false, LODRenderData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

		for (uint32 TriangleIt = 0; TriangleIt < LODRenderData.RenderSections[TargetSection].NumTriangles; ++TriangleIt)
		{
			for (uint32 VertexIt = 0; VertexIt < 3; ++VertexIt)
			{
				const uint32 VertexIndex = TriangleIndices[LODRenderData.RenderSections[TargetSection].BaseIndex + 3 * TriangleIt + VertexIt];
				ValidPoints[VertexIndex] = true;
			}
		}
	}

	static void Build(UGroomBindingAsset* BindingAsset, FSkeletalMeshRenderData* TargetRenderData, TArray<TArray<FVector>>& TransferedPositions)
	{
		UGroomAsset* GroomAsset = BindingAsset->Groom;

		UGroomBindingAsset::FHairGroupDatas& OutHairGroupDatas = BindingAsset->HairGroupDatas;

		const uint32 GroupCount  = OutHairGroupDatas.Num();
		const uint32 MeshLODCount= BindingAsset->TargetSkeletalMesh->GetLODNum();
		const uint32 MaxSamples  = BindingAsset->NumInterpolationPoints;

		const uint32 TargetSection = 0;
		const bool LocalSamples = false;
		for (uint32 LODIndex = 0; LODIndex < MeshLODCount; ++LODIndex)
		{
			FSkeletalMeshLODRenderData& LODRenderData = TargetRenderData->LODRenderData[LODIndex];

			TArray<FSkelMeshRenderSection>& RenderSections = LODRenderData.RenderSections;
			const int32 NumVertices = (RenderSections.Num() > TargetSection) ? RenderSections[TargetSection].NumVertices : 0;

			FVector* PositionsPointer = nullptr;
			if (TransferedPositions.Num() == MeshLODCount)
			{
				PositionsPointer = TransferedPositions[LODIndex].GetData();
			}
			else
			{
				FPositionVertexBuffer& VertexBuffer = LODRenderData.StaticVertexBuffers.PositionVertexBuffer;
				PositionsPointer = static_cast<FVector*>(VertexBuffer.GetVertexData());
			}

			if (LocalSamples)
			{
				TArray<bool> ValidPoints;
				for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
				{
					FillLocalValidPoints(LODRenderData, TargetSection, OutHairGroupDatas[GroupIt].RenRootData.MeshProjectionLODs[LODIndex], ValidPoints);

					FPointsSampler PointsSampler(ValidPoints, PositionsPointer, MaxSamples);
					const uint32 SampleCount = PointsSampler.SamplePositions.Num();

					FWeightsBuilder InterpolationWeights(SampleCount, SampleCount,
						PointsSampler.SamplePositions.GetData(), PointsSampler.SamplePositions.GetData());

					//const FVector Displace(0.0,0.0,10.0);
					//TArray<FVector> Deltas; Deltas.Init(FVector::ZeroVector, SampleCount );
					//for (uint32 i = 0; i < SampleCount; ++i)
					//{
					//	Deltas[i] = FVector(0,0,0);
					//	for (uint32 j = 0; j < SampleCount; ++j)
					//	{
					//		Deltas[i] += InterpolationWeights.InverseEntries[SampleCount * i + j] * Displace;
					//	}
					//	UE_LOG(LogHairStrands, Log, TEXT("[Groom] Sample Deltas[%d] = %s"), i, *Deltas[i].ToString());
					//}
					//for (uint32 i = 0; i < GroomAsset->HairGroupsData[GroupIt].HairSimulationData.StrandsPoints.Num(); ++i)
					//{
					//	FVector Offset(0,0,0);
					//	for (uint32 j = 0; j < SampleCount; ++j)
					//	{
					//		const FVector DeltaPosition = GroomAsset->HairGroupsData[GroupIt].HairSimulationData.StrandsPoints.PointsPosition[i] - PointsSampler.SamplePositions[j];
					//		const float FunctionValue = FMath::Sqrt(FVector::FVector::DotProductProduct(DeltaPosition, DeltaPosition)+1.0);
					//		Offset += FunctionValue * Deltas[j];
					//	}
					//	UE_LOG(LogHairStrands, Log, TEXT("[Groom] Sample Displace[%d] = %s"), i, *Offset.ToString());
					//}

					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].SimRootData);
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].RenRootData);
				}
			}
			else
			{
				TArray<bool> ValidPoints;

				FillGlobalValidPoints(LODRenderData, TargetSection, ValidPoints);

				FPointsSampler PointsSampler(ValidPoints, PositionsPointer, MaxSamples);
				const uint32 SampleCount = PointsSampler.SamplePositions.Num();

				FWeightsBuilder InterpolationWeights(SampleCount, SampleCount,
					PointsSampler.SamplePositions.GetData(), PointsSampler.SamplePositions.GetData());

				for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
				{
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].SimRootData);
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].RenRootData);
				}
			}
		}
	}
}// namespace GroomBinding_RBFWeighting

///////////////////////////////////////////////////////////////////////////////////////////////////
// Root projection

namespace GroomBinding_RootProjection
{
	struct FTriangleGrid
	{
		struct FTriangle
		{
			uint32  TriangleIndex;
			uint32  SectionIndex;
			uint32  SectionBaseIndex;

			uint32  I0;
			uint32  I1;
			uint32  I2;

			FVector P0;
			FVector P1;
			FVector P2;

			FVector2D UV0;
			FVector2D UV1;
			FVector2D UV2;
		};

		struct FCell
		{
			TArray<FTriangle> Triangles;
		};
		typedef TArray<const FCell*> FCells;

		FTriangleGrid(const FVector& InMinBound, const FVector& InMaxBound, float InVoxelWorldSize)
		{
			MinBound = InMinBound;
			MaxBound = InMaxBound;

			// Compute the voxel volume resolution, and snap the max bound to the voxel grid
			GridResolution = FIntVector::ZeroValue;
			FVector VoxelResolutionF = (MaxBound - MinBound) / InVoxelWorldSize;
			GridResolution = FIntVector(FMath::CeilToInt(VoxelResolutionF.X), FMath::CeilToInt(VoxelResolutionF.Y), FMath::CeilToInt(VoxelResolutionF.Z));
			MaxBound = MinBound + FVector(GridResolution) * InVoxelWorldSize;

			Cells.SetNum(GridResolution.X * GridResolution.Y * GridResolution.Z);
		}

		FORCEINLINE bool IsValid(const FIntVector& P) const
		{
			return
				0 <= P.X && P.X < GridResolution.X &&
				0 <= P.Y && P.Y < GridResolution.Y &&
				0 <= P.Z && P.Z < GridResolution.Z;
		}

		FORCEINLINE bool IsOutside(const FVector& MinP, const FVector& MaxP) const
		{
			return
				(MaxP.X <= MinBound.X || MaxP.Y <= MinBound.Y || MaxP.Z <= MinBound.Z) ||
				(MinP.X >= MaxBound.X || MinP.Y >= MaxBound.Y || MinP.Z >= MaxBound.Z);
		}

		FORCEINLINE FIntVector ClampToVolume(const FIntVector& CellCoord, bool& bIsValid) const
		{
			bIsValid = IsValid(CellCoord);
			return FIntVector(
				FMath::Clamp(CellCoord.X, 0, GridResolution.X - 1),
				FMath::Clamp(CellCoord.Y, 0, GridResolution.Y - 1),
				FMath::Clamp(CellCoord.Z, 0, GridResolution.Z - 1));
		}

		FORCEINLINE FIntVector ToCellCoord(const FVector& P) const
		{
			bool bIsValid = false;
			const FVector F = ((P - MinBound) / (MaxBound - MinBound));
			const FIntVector CellCoord = FIntVector(FMath::FloorToInt(F.X * GridResolution.X), FMath::FloorToInt(F.Y * GridResolution.Y), FMath::FloorToInt(F.Z * GridResolution.Z));
			return ClampToVolume(CellCoord, bIsValid);
		}

		uint32 ToIndex(const FIntVector& CellCoord) const
		{
			uint32 CellIndex = CellCoord.X + CellCoord.Y * GridResolution.X + CellCoord.Z * GridResolution.X * GridResolution.Y;
			check(CellIndex < uint32(Cells.Num()));
			return CellIndex;
		}

		FCells ToCells(const FVector& P)
		{
			FCells Out;

			bool bIsValid = false;
			const FIntVector Coord = ToCellCoord(P);
			{
				const uint32 LinearIndex = ToIndex(Coord);
				if (Cells[LinearIndex].Triangles.Num() > 0)
				{
					Out.Add(&Cells[LinearIndex]);
					bIsValid = true;
				}
			}
			
			int32 Kernel = 1;
			while (!bIsValid)
			{
				for (int32 Z = -Kernel; Z <= Kernel; ++Z)
				for (int32 Y = -Kernel; Y <= Kernel; ++Y)
				for (int32 X = -Kernel; X <= Kernel; ++X)
				{
					// Do kernel box filtering layer, by layer
					if (FMath::Abs(X) != Kernel && FMath::Abs(Y) != Kernel && FMath::Abs(Z) != Kernel)
						continue;

					const FIntVector Offset(X, Y, Z);
					FIntVector C = Coord + Offset;
					C.X = FMath::Clamp(C.X, 0, GridResolution.X - 1);
					C.Y = FMath::Clamp(C.Y, 0, GridResolution.Y - 1);
					C.Z = FMath::Clamp(C.Z, 0, GridResolution.Z - 1);

					const uint32 LinearIndex = ToIndex(C);
					if (Cells[LinearIndex].Triangles.Num() > 0)
					{
						Out.Add(&Cells[LinearIndex]);
						bIsValid = true;
					}
				}
				++Kernel;

				// If no cells have been found in the entire grid, return
				if (Kernel >= FMath::Max3(GridResolution.X, GridResolution.Y, GridResolution.Z))
				{
					break;
				}
			}

			return Out;
		}

		void Insert(const FTriangle& T)
		{
			FVector TriMinBound;
			TriMinBound.X = FMath::Min(T.P0.X, FMath::Min(T.P1.X, T.P2.X));
			TriMinBound.Y = FMath::Min(T.P0.Y, FMath::Min(T.P1.Y, T.P2.Y));
			TriMinBound.Z = FMath::Min(T.P0.Z, FMath::Min(T.P1.Z, T.P2.Z));

			FVector TriMaxBound;
			TriMaxBound.X = FMath::Max(T.P0.X, FMath::Max(T.P1.X, T.P2.X));
			TriMaxBound.Y = FMath::Max(T.P0.Y, FMath::Max(T.P1.Y, T.P2.Y));
			TriMaxBound.Z = FMath::Max(T.P0.Z, FMath::Max(T.P1.Z, T.P2.Z));

			if (IsOutside(TriMinBound, TriMaxBound))
				return;

			const FIntVector MinCoord = ToCellCoord(TriMinBound);
			const FIntVector MaxCoord = ToCellCoord(TriMaxBound);

			// Insert triangle in all cell covered by the AABB of the triangle
			for (int32 Z = MinCoord.Z; Z <= MaxCoord.Z; ++Z)
			{
				for (int32 Y = MinCoord.Y; Y <= MaxCoord.Y; ++Y)
				{
					for (int32 X = MinCoord.X; X <= MaxCoord.X; ++X)
					{
						const FIntVector CellIndex(X, Y, Z);
						if (IsValid(CellIndex))
						{
							const uint32 CellLinearIndex = ToIndex(CellIndex);
							Cells[CellLinearIndex].Triangles.Add(T);
						}
					}
				}
			}
		}

		FVector MinBound;
		FVector MaxBound;
		FIntVector GridResolution;
		TArray<FCell> Cells;
	};

	// Closest point on A triangle from another point
	// Code from the book "Real-Time Collision Detection" by Christer Ericson
	struct FTrianglePoint
	{
		FVector P;
		FVector Barycentric;
	};

	static FTrianglePoint ComputeClosestPoint(const FTriangleGrid::FTriangle& Tri, const FVector& P)
	{
		const FVector A = Tri.P0;
		const FVector B = Tri.P1;
		const FVector C = Tri.P2;

		// Check if P is in vertex region outside A.
		FVector AB = B - A;
		FVector AC = C - A;
		FVector AP = P - A;
		float D1 = FVector::DotProduct(AB, AP);
		float D2 = FVector::DotProduct(AC, AP);
		if (D1 <= 0.f && D2 <= 0.f)
		{
			FTrianglePoint Out;
			Out.P = A;
			Out.Barycentric = FVector(1, 0, 0);
			return Out;
		}

		// Check if P is in vertex region outside B.
		FVector BP = P - B;
		float D3 = FVector::DotProduct(AB, BP);
		float D4 = FVector::DotProduct(AC, BP);
		if (D3 >= 0.f && D4 <= D3)
		{
			FTrianglePoint Out;
			Out.P = B;
			Out.Barycentric = FVector(0, 1, 0);
			return Out;
		}

		// Check if P is in edge region of AB, and if so, return the projection of P onto AB.
		float VC = D1 * D4 - D3 * D2;
		if (VC <= 0.f && D1 >= 0.f && D3 <= 0.f)
		{
			float V = D1 / (D1 - D3);

			FTrianglePoint Out;
			Out.P = A + V * AB;
			Out.Barycentric = FVector(1 - V, V, 0);
			return Out;
		}

		// Check if P is in vertex region outside C.
		FVector CP = P - C;
		float D5 = FVector::DotProduct(AB, CP);
		float D6 = FVector::DotProduct(AC, CP);
		if (D6 >= 0.f && D5 <= D6)
		{
			FTrianglePoint Out;
			Out.P = C;
			Out.Barycentric = FVector(0, 0, 1);
			return Out;
		}

		// Check if P is in edge region of AC, and if so, return the projection of P onto AC.
		float VB = D5 * D2 - D1 * D6;
		if (VB <= 0.f && D2 >= 0.f && D6 <= 0.f)
		{
			float W = D2 / (D2 - D6);
			FTrianglePoint Out;
			Out.P = A + W * AC;
			Out.Barycentric = FVector(1 - W, 0, W);
			return Out;
		}

		// Check if P is in edge region of BC, and if so, return the projection of P onto BC.
		float VA = D3 * D6 - D5 * D4;
		if (VA <= 0.f && D4 - D3 >= 0.f && D5 - D6 >= 0.f)
		{
			float W = (D4 - D3) / (D4 - D3 + D5 - D6);
			FTrianglePoint Out;
			Out.P = B + W * (C - B);
			Out.Barycentric = FVector(0, 1 - W, W);
			return Out;
		}

		// P must be inside the face region. Compute the closest point through its barycentric coordinates (u,V,W).
		float Denom = 1.f / (VA + VB + VC);
		float V = VB * Denom;
		float W = VC * Denom;

		FTrianglePoint Out;
		Out.P = A + AB * V + AC * W;
		Out.Barycentric = FVector(1 - V - W, V, W);
		return Out;
	}

	static bool Project(
		const FHairStrandsDatas& InStrandsData,
		const FSkeletalMeshRenderData* InMeshRenderData,
		const TArray<TArray<FVector>>& InTransferredPositions,
		FHairStrandsRootData& OutRootData)
	{
		// 2. Project root for each mesh LOD
		const uint32 CurveCount = InStrandsData.GetNumCurves();
		const uint32 ChannelIndex = 0;
		const float VoxelWorldSize = 2; //cm
		const uint32 MeshLODCount = InMeshRenderData->LODRenderData.Num();
		check(MeshLODCount == OutRootData.MeshProjectionLODs.Num());

		const bool bHasTransferredPosition = InTransferredPositions.Num() > 0;
		if (bHasTransferredPosition)
		{
			check(InTransferredPositions.Num() == MeshLODCount);
		}

		for (uint32 LODIt = 0; LODIt < MeshLODCount; ++LODIt)
		{
			check(LODIt == OutRootData.MeshProjectionLODs[LODIt].LODIndex);

			// 2.1. Build a grid around the hair AABB
			FTriangleGrid Grid(InStrandsData.BoundingBox.Min, InStrandsData.BoundingBox.Max, VoxelWorldSize);

			TArray<uint32> IndexBuffer;
			InMeshRenderData->LODRenderData[LODIt].MultiSizeIndexContainer.GetIndexBuffer(IndexBuffer);

			const uint32 MaxSectionCount = GetHairStrandsMaxSectionCount();
			const uint32 MaxTriangleCount = GetHairStrandsMaxTriangleCount();

			FBox MeshBound;
			MeshBound.Init();
			const uint32 SectionCount = InMeshRenderData->LODRenderData[LODIt].RenderSections.Num();
			for (uint32 SectionIt = 0; SectionIt < SectionCount; ++SectionIt)
			{
				// 2.2. Insert all triangle within the grid
				const uint32 TriangleCount = InMeshRenderData->LODRenderData[LODIt].RenderSections[SectionIt].NumTriangles;
				const uint32 SectionBaseIndex = InMeshRenderData->LODRenderData[LODIt].RenderSections[SectionIt].BaseIndex;

				check(TriangleCount < MaxTriangleCount);
				check(SectionCount < MaxSectionCount);

				for (uint32 TriangleIt = 0; TriangleIt < TriangleCount; ++TriangleIt)
				{
					FTriangleGrid::FTriangle T;
					T.TriangleIndex = TriangleIt;
					T.SectionIndex = SectionIt;
					T.SectionBaseIndex = SectionBaseIndex;

					T.I0 = IndexBuffer[T.SectionBaseIndex + T.TriangleIndex * 3 + 0];
					T.I1 = IndexBuffer[T.SectionBaseIndex + T.TriangleIndex * 3 + 1];
					T.I2 = IndexBuffer[T.SectionBaseIndex + T.TriangleIndex * 3 + 2];

					if (bHasTransferredPosition)
					{
						T.P0 = InTransferredPositions[LODIt][T.I0];
						T.P1 = InTransferredPositions[LODIt][T.I1];
						T.P2 = InTransferredPositions[LODIt][T.I2];
					}
					else
					{
						T.P0 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I0);
						T.P1 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I1);
						T.P2 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I2);
					}

					T.UV0 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I0, ChannelIndex);
					T.UV1 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I1, ChannelIndex);
					T.UV2 = InMeshRenderData->LODRenderData[LODIt].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I2, ChannelIndex);

					MeshBound += T.P0;
					MeshBound += T.P1;
					MeshBound += T.P2;

					Grid.Insert(T);
				}
			}

			OutRootData.MeshProjectionLODs[LODIt].RootTriangleIndexBuffer.SetNum(CurveCount);
			OutRootData.MeshProjectionLODs[LODIt].RootTriangleBarycentricBuffer.SetNum(CurveCount);
			OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition0Buffer.SetNum(CurveCount);
			OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition1Buffer.SetNum(CurveCount);
			OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition2Buffer.SetNum(CurveCount);

			// 2.3. Compute the closest triangle for each root
			//InMeshRenderData->LODRenderData[LODIt].GetNumVertices();
			for (uint32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
			{
				const uint32 Offset = InStrandsData.StrandsCurves.CurvesOffset[CurveIndex];
				const FVector& RootP = InStrandsData.StrandsPoints.PointsPosition[Offset];
				const FTriangleGrid::FCells Cells = Grid.ToCells(RootP);

				if (Cells.Num() == 0)
				{
					return false;
				}

				float ClosestDistance = FLT_MAX;
				FTriangleGrid::FTriangle ClosestTriangle;
				FVector2D ClosestBarycentrics;
				for (const FTriangleGrid::FCell* Cell : Cells)
				{
					for (const FTriangleGrid::FTriangle& CellTriangle : Cell->Triangles)
					{
						const FTrianglePoint Tri = ComputeClosestPoint(CellTriangle, RootP);
						const float Distance = FVector::Distance(Tri.P, RootP);
						if (Distance < ClosestDistance)
						{
							ClosestDistance = Distance;
							ClosestTriangle = CellTriangle;
							ClosestBarycentrics = FVector2D(Tri.Barycentric.X, Tri.Barycentric.Y);
						}
					}
				}
				check(ClosestDistance < FLT_MAX);

				const uint32 EncodedBarycentrics = FHairStrandsRootUtils::EncodeBarycentrics(ClosestBarycentrics);
				const uint32 EncodedTriangleIndex = FHairStrandsRootUtils::EncodeTriangleIndex(ClosestTriangle.TriangleIndex, ClosestTriangle.SectionIndex);
				OutRootData.MeshProjectionLODs[LODIt].RootTriangleIndexBuffer[CurveIndex] = EncodedTriangleIndex;
				OutRootData.MeshProjectionLODs[LODIt].RootTriangleBarycentricBuffer[CurveIndex] = EncodedBarycentrics;
				OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition0Buffer[CurveIndex] = FVector4(ClosestTriangle.P0, FHairStrandsRootUtils::PackUVsToFloat(ClosestTriangle.UV0));
				OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition1Buffer[CurveIndex] = FVector4(ClosestTriangle.P1, FHairStrandsRootUtils::PackUVsToFloat(ClosestTriangle.UV1));
				OutRootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition2Buffer[CurveIndex] = FVector4(ClosestTriangle.P2, FHairStrandsRootUtils::PackUVsToFloat(ClosestTriangle.UV2));
			}
		}

		return true;
	}
}// namespace GroomBinding_Project

///////////////////////////////////////////////////////////////////////////////////////////////////
// Mesh transfer

namespace GroomBinding_Transfer
{

	struct FTriangleGrid2D
	{
		struct FTriangle
		{
			uint32  TriangleIndex;
			uint32  SectionIndex;
			uint32  SectionBaseIndex;

			uint32  I0;
			uint32  I1;
			uint32  I2;

			FVector P0;
			FVector P1;
			FVector P2;

			FVector2D UV0;
			FVector2D UV1;
			FVector2D UV2;
		};

		struct FCell
		{
			TArray<FTriangle> Triangles;
		};
		typedef TArray<const FCell*> FCells;

		FTriangleGrid2D(uint32 Resolution)
		{
			GridResolution.X = Resolution;
			GridResolution.Y = Resolution;
			MinBound = FVector2D(0,0);
			MaxBound = FVector2D(1,1);

			Cells.SetNum(GridResolution.X * GridResolution.Y);
		}

		FORCEINLINE bool IsValid(const FIntPoint& P) const
		{
			return
				0 <= P.X && P.X < GridResolution.X &&
				0 <= P.Y && P.Y < GridResolution.Y;
		}

		FORCEINLINE bool IsOutside(const FVector2D& MinP, const FVector2D& MaxP) const
		{
			return
				(MaxP.X <= MinBound.X || MaxP.Y <= MinBound.Y) ||
				(MinP.X >= MaxBound.X || MinP.Y >= MaxBound.Y);
		}

		FORCEINLINE FIntPoint ClampToVolume(const FIntPoint& CellCoord, bool& bIsValid) const
		{
			bIsValid = IsValid(CellCoord);
			return FIntPoint(
				FMath::Clamp(CellCoord.X, 0, GridResolution.X - 1),
				FMath::Clamp(CellCoord.Y, 0, GridResolution.Y - 1));
		}

		FORCEINLINE FIntPoint ToCellCoord(const FVector2D& P) const
		{
			bool bIsValid = false;
			FVector2D PP;
			PP.X = FMath::Clamp(P.X, 0.f, 1.f);
			PP.Y = FMath::Clamp(P.Y, 0.f, 1.f);
			const FIntPoint CellCoord = FIntPoint(FMath::FloorToInt(PP.X * GridResolution.X), FMath::FloorToInt(PP.Y * GridResolution.Y));
			return ClampToVolume(CellCoord, bIsValid);
		}

		uint32 ToIndex(const FIntPoint& CellCoord) const
		{
			uint32 CellIndex = CellCoord.X + CellCoord.Y * GridResolution.X;
			check(CellIndex < uint32(Cells.Num()));
			return CellIndex;
		}

		FCells ToCells(const FVector2D& P)
		{
			FCells Out;

			bool bIsValid = false;
			const FIntPoint Coord = ToCellCoord(P);
			{
				const uint32 LinearIndex = ToIndex(Coord);
				if (Cells[LinearIndex].Triangles.Num() > 0)
				{
					Out.Add(&Cells[LinearIndex]);
					bIsValid = true;
				}
			}

			int32 Kernel = 1;
			while (!bIsValid)
			{
				for (int32 Y = -Kernel; Y <= Kernel; ++Y)
				for (int32 X = -Kernel; X <= Kernel; ++X)
				{
					if (FMath::Abs(X) != Kernel && FMath::Abs(Y) != Kernel)
						continue;

					const FIntPoint Offset(X, Y);
					FIntPoint C = Coord + Offset;
					C.X = FMath::Clamp(C.X, 0, GridResolution.X - 1);
					C.Y = FMath::Clamp(C.Y, 0, GridResolution.Y - 1);

					const uint32 LinearIndex = ToIndex(C);
					if (Cells[LinearIndex].Triangles.Num() > 0)
					{
						Out.Add(&Cells[LinearIndex]);
						bIsValid = true;
					}
				}
				++Kernel;
			}

			return Out;
		}

		void Insert(const FTriangle& T)
		{
			FVector2D TriMinBound;
			TriMinBound.X = FMath::Min(T.UV0.X, FMath::Min(T.UV1.X, T.UV2.X));
			TriMinBound.Y = FMath::Min(T.UV0.Y, FMath::Min(T.UV1.Y, T.UV2.Y));

			FVector2D TriMaxBound;
			TriMaxBound.X = FMath::Max(T.UV0.X, FMath::Max(T.UV1.X, T.UV2.X));
			TriMaxBound.Y = FMath::Max(T.UV0.Y, FMath::Max(T.UV1.Y, T.UV2.Y));

			if (IsOutside(TriMinBound, TriMaxBound))
				return;

			const FIntPoint MinCoord = ToCellCoord(TriMinBound);
			const FIntPoint MaxCoord = ToCellCoord(TriMaxBound);

			// Insert triangle in all cell covered by the AABB of the triangle
			for (int32 Y = MinCoord.Y; Y <= MaxCoord.Y; ++Y)
			{
				for (int32 X = MinCoord.X; X <= MaxCoord.X; ++X)
				{
					const FIntPoint CellIndex(X, Y);
					if (IsValid(CellIndex))
					{
						const uint32 CellLinearIndex = ToIndex(CellIndex);
						Cells[CellLinearIndex].Triangles.Add(T);
					}
				}
			}
		}

		FVector2D MinBound;
		FVector2D MaxBound;
		FIntPoint GridResolution;
		TArray<FCell> Cells;
	};


	// Closest point on A triangle from another point in UV space
	// Code from the book "Real-Time Collision Detection" by Christer Ericson
	struct FTrianglePoint
	{
		FVector P;
		FVector Barycentric;
	};

	FTrianglePoint ComputeClosestPoint(const FVector2D& TriUV0, const FVector2D& TriUV1, const FVector2D& TriUV2, const FVector2D& UVs)
	{
		const FVector A = FVector(TriUV0, 0);
		const FVector B = FVector(TriUV1, 0);
		const FVector C = FVector(TriUV2, 0);
		const FVector P = FVector(UVs, 0);

		// Check if P is in vertex region outside A.
		FVector AB = B - A;
		FVector AC = C - A;
		FVector AP = P - A;
		float D1 = FVector::DotProduct(AB, AP);
		float D2 = FVector::DotProduct(AC, AP);
		if (D1 <= 0.f && D2 <= 0.f)
		{
			FTrianglePoint Out;
			Out.P = A;
			Out.Barycentric = FVector(1, 0, 0);
			return Out;
		}

		// Check if P is in vertex region outside B.
		FVector BP = P - B;
		float D3 = FVector::DotProduct(AB, BP);
		float D4 = FVector::DotProduct(AC, BP);
		if (D3 >= 0.f && D4 <= D3)
		{
			FTrianglePoint Out;
			Out.P = B;
			Out.Barycentric = FVector(0, 1, 0);
			return Out;
		}

		// Check if P is in edge region of AB, and if so, return the projection of P onto AB.
		float VC = D1 * D4 - D3 * D2;
		if (VC <= 0.f && D1 >= 0.f && D3 <= 0.f)
		{
			float V = D1 / (D1 - D3);

			FTrianglePoint Out;
			Out.P = A + V * AB;
			Out.Barycentric = FVector(1 - V, V, 0);
			return Out;
		}

		// Check if P is in vertex region outside C.
		FVector CP = P - C;
		float D5 = FVector::DotProduct(AB, CP);
		float D6 = FVector::DotProduct(AC, CP);
		if (D6 >= 0.f && D5 <= D6)
		{
			FTrianglePoint Out;
			Out.P = C;
			Out.Barycentric = FVector(0, 0, 1);
			return Out;
		}

		// Check if P is in edge region of AC, and if so, return the projection of P onto AC.
		float VB = D5 * D2 - D1 * D6;
		if (VB <= 0.f && D2 >= 0.f && D6 <= 0.f)
		{
			float W = D2 / (D2 - D6);
			FTrianglePoint Out;
			Out.P = A + W * AC;
			Out.Barycentric = FVector(1 - W, 0, W);
			return Out;
		}

		// Check if P is in edge region of BC, and if so, return the projection of P onto BC.
		float VA = D3 * D6 - D5 * D4;
		if (VA <= 0.f && D4 - D3 >= 0.f && D5 - D6 >= 0.f)
		{
			float W = (D4 - D3) / (D4 - D3 + D5 - D6);
			FTrianglePoint Out;
			Out.P = B + W * (C - B);
			Out.Barycentric = FVector(0, 1 - W, W);
			return Out;
		}

		// P must be inside the face region. Compute the closest point through its barycentric coordinates (u,V,W).
		float Denom = 1.f / (VA + VB + VC);
		float V = VB * Denom;
		float W = VC * Denom;

		FTrianglePoint Out;
		Out.P = A + AB * V + AC * W;
		Out.Barycentric = FVector(1 - V - W, V, W);
		return Out;
	}

	void Transfer(
		const FSkeletalMeshRenderData* InSourceMeshRenderData,
		const FSkeletalMeshRenderData* InTargetMeshRenderData,
		TArray<TArray<FVector>>& OutTransferredPositions)
	{
		const uint32 ChannelIndex = 0;
		const uint32 SourceLODIndex = 0;
		const uint32 SourceSectionId = 0;
		const int32 TargetSectionId = 0;

		// Notes:
		// LODs are transfered using the LOD0 of the source mesh, as the LOD count can mismatch between source and target meshes.
		// Assume that the section 0 contains the head section, which is where the hair/facial hair should be projected on

		const uint32 SourceTriangleCount = InSourceMeshRenderData->LODRenderData[SourceLODIndex].RenderSections[SourceSectionId].NumTriangles;
		const uint32 SourceSectionBaseIndex = InSourceMeshRenderData->LODRenderData[SourceLODIndex].RenderSections[SourceSectionId].BaseIndex;

		TArray<uint32> SourceIndexBuffer;
		InSourceMeshRenderData->LODRenderData[SourceLODIndex].MultiSizeIndexContainer.GetIndexBuffer(SourceIndexBuffer);

		// 1. Insert triangles into a 2D UV grid
		FTriangleGrid2D Grid(256);
		for (uint32 SourceTriangleIt = 0; SourceTriangleIt < SourceTriangleCount; ++SourceTriangleIt)
		{
			FTriangleGrid2D::FTriangle T;
			T.SectionIndex		= SourceSectionId;
			T.SectionBaseIndex	= SourceSectionBaseIndex;
			T.TriangleIndex		= SourceTriangleIt;

			T.I0 = SourceIndexBuffer[T.SectionBaseIndex + SourceTriangleIt * 3 + 0];
			T.I1 = SourceIndexBuffer[T.SectionBaseIndex + SourceTriangleIt * 3 + 1];
			T.I2 = SourceIndexBuffer[T.SectionBaseIndex + SourceTriangleIt * 3 + 2];

			T.P0 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I0);
			T.P1 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I1);
			T.P2 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(T.I2);

			T.UV0 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I0, ChannelIndex);
			T.UV1 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I1, ChannelIndex);
			T.UV2 = InSourceMeshRenderData->LODRenderData[SourceLODIndex].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(T.I2, ChannelIndex);

			Grid.Insert(T);
		}

		// 2. Look for closest triangle point in UV space
		// Make this run in parallel
		const uint32 TargetLODCount = InTargetMeshRenderData->LODRenderData.Num();
		OutTransferredPositions.SetNum(TargetLODCount);
		for (uint32 TargetLODIndex = 0; TargetLODIndex < TargetLODCount; ++TargetLODIndex)
		{
			const uint32 TargetTriangleCount = InTargetMeshRenderData->LODRenderData[TargetLODIndex].RenderSections[TargetSectionId].NumTriangles;
			const uint32 TargetVertexCount = InTargetMeshRenderData->LODRenderData[TargetLODIndex].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
			OutTransferredPositions[TargetLODIndex].SetNum(TargetVertexCount);
			for (uint32 TargetVertexIt = 0; TargetVertexIt < TargetVertexCount; ++TargetVertexIt)
			{
				int32 SectionIt = 0;
				int32 TargetVertexIt2 = 0;
				InTargetMeshRenderData->LODRenderData[TargetLODIndex].GetSectionFromVertexIndex(TargetVertexIt, SectionIt, TargetVertexIt2);
				if (SectionIt != TargetSectionId)
				{
					OutTransferredPositions[TargetLODIndex][TargetVertexIt] = FVector(0,0,0);
					continue;
				}

				const FVector Target_P    = InTargetMeshRenderData->LODRenderData[TargetLODIndex].StaticVertexBuffers.PositionVertexBuffer.VertexPosition(TargetVertexIt);
				const FVector2D Target_UV = InTargetMeshRenderData->LODRenderData[TargetLODIndex].StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(TargetVertexIt, ChannelIndex);

				// 2.1 Query closest triangles
				FVector RetargetedVertexPosition = Target_P;
				FTriangleGrid2D::FCells Cells = Grid.ToCells(Target_UV);

				// 2.2 Compute the closest triangle and comput the retarget position 
				float ClosestUVDistance = FLT_MAX;
				for (const FTriangleGrid2D::FCell* Cell : Cells)
				{
					for (const FTriangleGrid2D::FTriangle& CellTriangle : Cell->Triangles)
					{
						const FTrianglePoint ClosestPoint = ComputeClosestPoint(CellTriangle.UV0, CellTriangle.UV1, CellTriangle.UV2, Target_UV);
						const float UVDistanceToTriangle = FVector2D::Distance(FVector2D(ClosestPoint.P.X, ClosestPoint.P.Y), Target_UV);
						if (UVDistanceToTriangle < ClosestUVDistance)
						{
							RetargetedVertexPosition =
								ClosestPoint.Barycentric.X * CellTriangle.P0 +
								ClosestPoint.Barycentric.Y * CellTriangle.P1 +
								ClosestPoint.Barycentric.Z * CellTriangle.P2;
							ClosestUVDistance = UVDistanceToTriangle;
						}
					}
				}
				check(ClosestUVDistance < FLT_MAX);
				OutTransferredPositions[TargetLODIndex][TargetVertexIt] = RetargetedVertexPosition;
			}
		}
	}
}
// namespace GroomBinding_Transfer

///////////////////////////////////////////////////////////////////////////////////////////////////
// Main entry (CPU path)
static bool InternalBuildBinding_CPU(UGroomBindingAsset* BindingAsset, bool bInitResources)
{
	if (!BindingAsset ||
		!BindingAsset->Groom ||
		!BindingAsset->TargetSkeletalMesh ||
		BindingAsset->Groom->GetNumHairGroups() == 0)
	{
		UE_LOG(LogHairStrands, Warning, TEXT("[Groom] Error - Binding asset can be created/rebuilt."));
		return false;
	}

	BindingAsset->Groom->ConditionalPostLoad();
	BindingAsset->TargetSkeletalMesh->ConditionalPostLoad();
	if (BindingAsset->SourceSkeletalMesh)
	{
		BindingAsset->SourceSkeletalMesh->ConditionalPostLoad();
	}

	const int32 NumInterpolationPoints = BindingAsset->NumInterpolationPoints;
	UGroomAsset* GroomAsset = BindingAsset->Groom;
	USkeletalMesh* SourceSkeletalMesh = BindingAsset->SourceSkeletalMesh;
	USkeletalMesh* TargetSkeletalMesh = BindingAsset->TargetSkeletalMesh;
	const uint32 GroupCount = GroomAsset->GetNumHairGroups();

	const uint32 MeshLODCount = BindingAsset->TargetSkeletalMesh->GetLODNum();
	UGroomBindingAsset::FHairGroupDatas& OutHairGroupDatas = BindingAsset->HairGroupDatas;
	OutHairGroupDatas.Empty();

	TArray<uint32> NumSamples;
	NumSamples.Init(NumInterpolationPoints, MeshLODCount);
	for (const FHairGroupData& GroupData : GroomAsset->HairGroupsData)
	{
		UGroomBindingAsset::FHairGroupData& Data = OutHairGroupDatas.AddDefaulted_GetRef();
		Data.RenRootData = FHairStrandsRootData(&GroupData.Strands.Data, MeshLODCount, NumSamples);
		Data.SimRootData = FHairStrandsRootData(&GroupData.Guides.Data, MeshLODCount, NumSamples);

		const uint32 CardsLODCount = GroupData.Cards.LODs.Num();
		Data.CardsRootData.SetNum(GroupData.Cards.LODs.Num());
		for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
		{
			if (GroupData.Cards.IsValid(CardsLODIt))
			{
				Data.CardsRootData[CardsLODIt] = FHairStrandsRootData(&GroupData.Cards.LODs[CardsLODIt].Guides.Data, MeshLODCount, NumSamples);
			}
		}
	}

	UGroomBindingAsset::FHairGroupResources& OutHairGroupResources = BindingAsset->HairGroupResources;
	if (BindingAsset->HairGroupResources.Num() > 0)
	{
		for (UGroomBindingAsset::FHairGroupResource& GroupResrouces : OutHairGroupResources)
		{
			BindingAsset->HairGroupResourcesToDelete.Enqueue(GroupResrouces);
		}
		OutHairGroupResources.Empty();
	}

	check(OutHairGroupResources.Num() == 0);

	TArray<FGoomBindingGroupInfo>& OutGroupInfos = BindingAsset->GroupInfos;
	OutGroupInfos.Empty();
	for (const UGroomBindingAsset::FHairGroupData& Data : OutHairGroupDatas)
	{
		FGoomBindingGroupInfo& Info = OutGroupInfos.AddDefaulted_GetRef();
		Info.SimRootCount = Data.SimRootData.RootCount;
		Info.SimLODCount  = Data.SimRootData.MeshProjectionLODs.Num();
		Info.RenRootCount = Data.RenRootData.RootCount;
		Info.RenLODCount  = Data.RenRootData.MeshProjectionLODs.Num();
	}
	
	// Create mapping between the source & target using their UV
	const bool bNeedTransfertPosition = SourceSkeletalMesh && SourceSkeletalMesh->GetResourceForRendering() != nullptr;
	TArray<TArray<FVector>> TransferredPositions;
	if (bNeedTransfertPosition)
	{
		GroomBinding_Transfer::Transfer(
			SourceSkeletalMesh->GetResourceForRendering(),
			TargetSkeletalMesh->GetResourceForRendering(),
			TransferredPositions);
	}

	bool bSucceed = false;
	for (uint32 GroupIt=0; GroupIt < GroupCount; ++GroupIt)
	{
		bSucceed = GroomBinding_RootProjection::Project(
			BindingAsset->Groom->HairGroupsData[GroupIt].Strands.Data,
			TargetSkeletalMesh->GetResourceForRendering(),
			TransferredPositions,
			BindingAsset->HairGroupDatas[GroupIt].RenRootData);
		if (!bSucceed) { return false; } 

		bSucceed = GroomBinding_RootProjection::Project(
			BindingAsset->Groom->HairGroupsData[GroupIt].Guides.Data,
			TargetSkeletalMesh->GetResourceForRendering(),
			TransferredPositions,
			BindingAsset->HairGroupDatas[GroupIt].SimRootData);
		if (!bSucceed) { return false; }

		const uint32 CardsLODCount = BindingAsset->HairGroupDatas[GroupIt].CardsRootData.Num();
		for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
		{
			if (BindingAsset->Groom->HairGroupsData[GroupIt].Cards.IsValid(CardsLODIt))
			{
				bSucceed = GroomBinding_RootProjection::Project(
					BindingAsset->Groom->HairGroupsData[GroupIt].Cards.LODs[CardsLODIt].Guides.Data,
					TargetSkeletalMesh->GetResourceForRendering(),
					TransferredPositions,
					BindingAsset->HairGroupDatas[GroupIt].CardsRootData[CardsLODIt]);
				if (!bSucceed) { return false; }
			}
		}
	}

	GroomBinding_RBFWeighting::Build(BindingAsset, TargetSkeletalMesh->GetResourceForRendering(), TransferredPositions);
	BindingAsset->QueryStatus = UGroomBindingAsset::EQueryStatus::Completed;

	if (bInitResources)
	{
		BindingAsset->InitResource();
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// GPU path

namespace GroomBinding_GPU
{
	template<typename ReadBackType>
	void ReadbackBuffer(TArray<ReadBackType>& OutData, FRWBuffer& InBuffer)
	{
		ReadBackType* Data = (ReadBackType*)RHILockVertexBuffer(InBuffer.Buffer, 0, InBuffer.Buffer->GetSize(), RLM_ReadOnly);
		const uint32 ElementCount = InBuffer.Buffer->GetSize() / sizeof(ReadBackType);
		OutData.SetNum(ElementCount);
		for (uint32 ElementIt = 0; ElementIt < ElementCount; ++ElementIt)
		{
			OutData[ElementIt] = Data[ElementIt];
		}
		RHIUnlockVertexBuffer(InBuffer.Buffer);
	}

	template<typename WriteBackType>
	void WritebackBuffer(TArray<WriteBackType>& InData, FRWBuffer& OutBuffer)
	{
		const uint32 DataSize = sizeof(WriteBackType) * InData.Num();
		check(DataSize == OutBuffer.Buffer->GetSize());

		WriteBackType* Data = (WriteBackType*)RHILockVertexBuffer(OutBuffer.Buffer, 0, DataSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, InData.GetData(), DataSize);
		RHIUnlockVertexBuffer(OutBuffer.Buffer);
	}

	static void ReadbackGroupData(
		FHairStrandsRootData& OutCPUData,
		FHairStrandsRestRootResource* InGPUData)
	{
		if (!InGPUData)
		{
			return;
		}

		check(InGPUData->LODs.Num() == OutCPUData.MeshProjectionLODs.Num());

		const uint32 MeshLODCount = InGPUData->LODs.Num();
		for (uint32 LODIt = 0; LODIt < MeshLODCount; ++LODIt)
		{
			FHairStrandsRootData::FMeshProjectionLOD& CPULOD = OutCPUData.MeshProjectionLODs[LODIt];
			FHairStrandsRestRootResource::FLOD& GPULOD = InGPUData->LODs[LODIt];
			check(CPULOD.LODIndex == GPULOD.LODIndex);

			ReadbackBuffer(CPULOD.RootTriangleIndexBuffer, GPULOD.RootTriangleIndexBuffer);
			ReadbackBuffer(CPULOD.RootTriangleBarycentricBuffer, GPULOD.RootTriangleBarycentricBuffer);
			ReadbackBuffer(CPULOD.RestRootTrianglePosition0Buffer, GPULOD.RestRootTrianglePosition0Buffer);
			ReadbackBuffer(CPULOD.RestRootTrianglePosition1Buffer, GPULOD.RestRootTrianglePosition1Buffer);
			ReadbackBuffer(CPULOD.RestRootTrianglePosition2Buffer, GPULOD.RestRootTrianglePosition2Buffer);

			InGPUData->RootData.MeshProjectionLODs[LODIt].RootTriangleIndexBuffer = CPULOD.RootTriangleIndexBuffer;
			InGPUData->RootData.MeshProjectionLODs[LODIt].RootTriangleBarycentricBuffer = CPULOD.RootTriangleBarycentricBuffer;
			InGPUData->RootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition0Buffer = CPULOD.RestRootTrianglePosition0Buffer;
			InGPUData->RootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition1Buffer = CPULOD.RestRootTrianglePosition1Buffer;
			InGPUData->RootData.MeshProjectionLODs[LODIt].RestRootTrianglePosition2Buffer = CPULOD.RestRootTrianglePosition2Buffer;
		}
	}

	void WritebackGroupData(FHairStrandsRootData& InCPUData, FHairStrandsRestRootResource* OutGPUData)
	{
		if (!OutGPUData)
		{
			return;
		}
		check(OutGPUData->LODs.Num() == InCPUData.MeshProjectionLODs.Num());
		const uint32 MeshLODCount = OutGPUData->LODs.Num();
		for (uint32 LODIt = 0; LODIt < MeshLODCount; ++LODIt)
		{
			FHairStrandsRootData::FMeshProjectionLOD& CPULOD = InCPUData.MeshProjectionLODs[LODIt];
			FHairStrandsRestRootResource::FLOD& GPULOD = OutGPUData->LODs[LODIt];
			check(CPULOD.LODIndex == GPULOD.LODIndex);

			if (CPULOD.SampleCount > 0)
			{
				WritebackBuffer(CPULOD.MeshInterpolationWeightsBuffer, GPULOD.MeshInterpolationWeightsBuffer);
				WritebackBuffer(CPULOD.MeshSampleIndicesBuffer, GPULOD.MeshSampleIndicesBuffer);
				WritebackBuffer(CPULOD.RestSamplePositionsBuffer, GPULOD.RestSamplePositionsBuffer);

				OutGPUData->RootData.MeshProjectionLODs[LODIt].SampleCount = CPULOD.SampleCount;
				OutGPUData->RootData.MeshProjectionLODs[LODIt].MeshInterpolationWeightsBuffer = CPULOD.MeshInterpolationWeightsBuffer;
				OutGPUData->RootData.MeshProjectionLODs[LODIt].RestSamplePositionsBuffer = CPULOD.RestSamplePositionsBuffer;
				OutGPUData->RootData.MeshProjectionLODs[LODIt].MeshSampleIndicesBuffer = CPULOD.MeshSampleIndicesBuffer;
			}
		}
	}

	struct FPointsSampler
	{
		FPointsSampler(TArray<bool>& ValidPoints, const FVector* PointPositions, const int32 NumSamples);

		/** Build the sample position from the sample indices */
		void BuildPositions(const FVector* PointPositions);

		/** Compute the furthest point */
		void FurthestPoint(const int32 NumPoints, const FVector* PointPositions, const uint32 SampleIndex, TArray<bool>& ValidPoints, TArray<float>& PointsDistance);

		/** Compute the starting point */
		int32 StartingPoint(const TArray<bool>& ValidPoints, int32& NumPoints) const;

		/** List of sampled points */
		TArray<uint32> SampleIndices;

		/** List of sampled positions */
		TArray<FVector> SamplePositions;
	};

	int32 FPointsSampler::StartingPoint(const TArray<bool>& ValidPoints, int32& NumPoints) const
	{
		int32 StartIndex = -1;
		NumPoints = 0;
		for (int32 i = 0; i < ValidPoints.Num(); ++i)
		{
			if (ValidPoints[i])
			{
				++NumPoints;
				if (StartIndex == -1)
				{
					StartIndex = i;
				}
			}
		}
		return StartIndex;
	}

	void FPointsSampler::BuildPositions(const FVector* PointPositions)
	{
		SamplePositions.SetNum(SampleIndices.Num());
		for (int32 i = 0; i < SampleIndices.Num(); ++i)
		{
			SamplePositions[i] = PointPositions[SampleIndices[i]];
		}
	}

	void FPointsSampler::FurthestPoint(const int32 NumPoints, const FVector* PointPositions, const uint32 SampleIndex, TArray<bool>& ValidPoints, TArray<float>& PointsDistance)
	{
		float FurthestDistance = 0.0;
		uint32 PointIndex = 0;
		for (int32 j = 0; j < NumPoints; ++j)
		{
			if (ValidPoints[j])
			{
				PointsDistance[j] = FMath::Min((PointPositions[SampleIndices[SampleIndex - 1]] - PointPositions[j]).Size(), PointsDistance[j]);
				if (PointsDistance[j] >= FurthestDistance)
				{
					PointIndex = j;
					FurthestDistance = PointsDistance[j];
				}
			}
		}
		ValidPoints[PointIndex] = false;
		SampleIndices[SampleIndex] = PointIndex;
	}

	FPointsSampler::FPointsSampler(TArray<bool>& ValidPoints, const FVector* PointPositions, const int32 NumSamples)
	{
		int32 NumPoints = 0;
		int32 StartIndex = StartingPoint(ValidPoints, NumPoints);

		const int32 SamplesCount = FMath::Min(NumPoints, NumSamples);
		if (SamplesCount != 0)
		{
			SampleIndices.SetNum(SamplesCount);
			SampleIndices[0] = StartIndex;
			ValidPoints[StartIndex] = false;

			TArray<float> PointsDistance;
			PointsDistance.Init(MAX_FLT, ValidPoints.Num());

			for (int32 i = 1; i < SamplesCount; ++i)
			{
				FurthestPoint(ValidPoints.Num(), PointPositions, i, ValidPoints, PointsDistance);
			}
			BuildPositions(PointPositions);
		}
	}

	struct FWeightsBuilder
	{
		FWeightsBuilder(const uint32 NumRows, const uint32 NumColumns,
			const FVector* SourcePositions, const FVector* TargetPositions);

		using EigenMatrix = Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>;

		/** Compute the weights by inverting the matrix*/
		void ComputeWeights(const uint32 NumRows, const uint32 NumColumns);

		/** Entries in the dense structure */
		TArray<float> MatrixEntries;

		/** Entries of the matrix inverse */
		TArray<float> InverseEntries;
	};

	FWeightsBuilder::FWeightsBuilder(const uint32 NumRows, const uint32 NumColumns,
		const FVector* SourcePositions, const FVector* TargetPositions)
	{
		const uint32 PolyRows = NumRows + 4;
		const uint32 PolyColumns = NumColumns + 4;

		MatrixEntries.Init(0.0, PolyRows * PolyColumns);
		InverseEntries.Init(0.0, PolyRows * PolyColumns);
		TArray<float>& LocalEntries = MatrixEntries;
		ParallelFor(NumRows,
			[
				NumRows,
				NumColumns,
				PolyRows,
				PolyColumns,
				SourcePositions,
				TargetPositions,
				&LocalEntries
			] (uint32 RowIndex)
			{
				int32 EntryIndex = RowIndex * PolyColumns;
				for (uint32 j = 0; j < NumColumns; ++j)
				{
					const float FunctionScale = (SourcePositions[RowIndex] - TargetPositions[j]).Size();
					LocalEntries[EntryIndex++] = FMath::Sqrt(FunctionScale * FunctionScale + 1.0);
				}
				LocalEntries[EntryIndex++] = 1.0;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].X;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Y;
				LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Z;

				EntryIndex = NumRows * PolyColumns + RowIndex;
				LocalEntries[EntryIndex] = 1.0;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].X;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].Y;

				EntryIndex += PolyColumns;
				LocalEntries[EntryIndex] = SourcePositions[RowIndex].Z;

				const float REGUL_VALUE = 1e-4;
				EntryIndex = NumRows * PolyColumns + NumColumns;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

				EntryIndex += PolyColumns + 1;
				LocalEntries[EntryIndex] = REGUL_VALUE;

			});
		ComputeWeights(PolyRows, PolyColumns);
	}

	void FWeightsBuilder::ComputeWeights(const uint32 NumRows, const uint32 NumColumns)
	{
		EigenMatrix WeightsMatrix(MatrixEntries.GetData(), NumRows, NumColumns);
		EigenMatrix WeightsInverse(InverseEntries.GetData(), NumColumns, NumRows);

		WeightsInverse = WeightsMatrix.inverse();
	}

	void UpdateInterpolationWeights(const FWeightsBuilder& InterpolationWeights, const FPointsSampler& PointsSampler, const uint32 LODIndex, FHairStrandsRootData& RootDatas)
	{
		FHairStrandsRootData::FMeshProjectionLOD& CPULOD = RootDatas.MeshProjectionLODs[LODIndex];
		CPULOD.MeshSampleIndicesBuffer.SetNum(PointsSampler.SampleIndices.Num());
		CPULOD.MeshInterpolationWeightsBuffer.SetNum(InterpolationWeights.InverseEntries.Num());
		CPULOD.RestSamplePositionsBuffer.SetNum(PointsSampler.SampleIndices.Num());

		CPULOD.SampleCount = PointsSampler.SampleIndices.Num();
		CPULOD.MeshSampleIndicesBuffer = PointsSampler.SampleIndices;
		CPULOD.MeshInterpolationWeightsBuffer = InterpolationWeights.InverseEntries;
		for (int32 i = 0; i < PointsSampler.SamplePositions.Num(); ++i)
		{
			CPULOD.RestSamplePositionsBuffer[i] = FVector4(PointsSampler.SamplePositions[i], 1.0f);
		}
	}

	void FillLocalValidPoints(FSkeletalMeshLODRenderData& LODRenderData, const uint32 TargetSection,
		const FHairStrandsRootData::FMeshProjectionLOD& ProjectionLOD, TArray<bool>& ValidPoints)
	{
		TArray<uint32> TriangleIndices; TriangleIndices.SetNum(LODRenderData.MultiSizeIndexContainer.GetIndexBuffer()->Num());
		LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(TriangleIndices);

		ValidPoints.Init(false, LODRenderData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

		const TArray<uint32>& RootBuffers = ProjectionLOD.RootTriangleIndexBuffer;
		for (int32 RootIt = 0; RootIt < RootBuffers.Num(); ++RootIt)
		{
			const uint32 SectionIndex = (RootBuffers[RootIt] >> 28) & 0xF;
			const uint32 TriangleIndex = RootBuffers[RootIt] & 0xFFFFFFF;
			if (SectionIndex == TargetSection)
			{
				for (uint32 VertexIt = 0; VertexIt < 3; ++VertexIt)
				{
					const uint32 VertexIndex = TriangleIndices[LODRenderData.RenderSections[SectionIndex].BaseIndex + 3 * TriangleIndex + VertexIt];
					ValidPoints[VertexIndex] = true;
				}
			}
		}
	}

	void FillGlobalValidPoints(FSkeletalMeshLODRenderData& LODRenderData, const uint32 TargetSection, TArray<bool>& ValidPoints)
	{
		TArray<uint32> TriangleIndices; TriangleIndices.SetNum(LODRenderData.MultiSizeIndexContainer.GetIndexBuffer()->Num());
		LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(TriangleIndices);

		ValidPoints.Init(false, LODRenderData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

		for (uint32 TriangleIt = 0; TriangleIt < LODRenderData.RenderSections[TargetSection].NumTriangles; ++TriangleIt)
		{
			for (uint32 VertexIt = 0; VertexIt < 3; ++VertexIt)
			{
				const uint32 VertexIndex = TriangleIndices[LODRenderData.RenderSections[TargetSection].BaseIndex + 3 * TriangleIt + VertexIt];
				ValidPoints[VertexIndex] = true;
			}
		}
	}

	void ComputeInterpolationWeights(UGroomBindingAsset* BindingAsset, FSkeletalMeshRenderData* TargetRenderData, TArray<FRWBuffer>& TransferedPositions)
	{
		UGroomAsset* GroomAsset = BindingAsset->Groom;
		// Enforce GPU sync to read back data on CPU
		GDynamicRHI->RHISubmitCommandsAndFlushGPU();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		UGroomBindingAsset::FHairGroupDatas& OutHairGroupDatas = BindingAsset->HairGroupDatas;
		UGroomBindingAsset::FHairGroupResources& OutHairGroupResources = BindingAsset->HairGroupResources;

		const uint32 GroupCount  = OutHairGroupResources.Num();
		const uint32 MeshLODCount= BindingAsset->TargetSkeletalMesh->GetLODNum();
		const uint32 MaxSamples  = BindingAsset->NumInterpolationPoints;

		for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
		{
			ReadbackGroupData(OutHairGroupDatas[GroupIt].SimRootData, OutHairGroupResources[GroupIt].SimRootResources);
			ReadbackGroupData(OutHairGroupDatas[GroupIt].RenRootData, OutHairGroupResources[GroupIt].RenRootResources);

			const uint32 CardsLODCount = OutHairGroupDatas[GroupIt].CardsRootData.Num();
			for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
			{
				if (OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt].IsValid())
				{
					//check(OutHairGroupResources[GroupIt].CardsRootResources[CardsLODIt].IsValid());
					ReadbackGroupData(OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt], OutHairGroupResources[GroupIt].CardsRootResources[CardsLODIt]);
				}
			}
		}

		const uint32 TargetSection = 0;
		const bool LocalSamples = false;
		for (uint32 LODIndex = 0; LODIndex < MeshLODCount; ++LODIndex)
		{
			FSkeletalMeshLODRenderData& LODRenderData = TargetRenderData->LODRenderData[LODIndex];

			TArray<FSkelMeshRenderSection>& RenderSections = LODRenderData.RenderSections;
			const int32 NumVertices = (RenderSections.Num() > TargetSection) ? RenderSections[TargetSection].NumVertices : 0;

			TArray<FVector> SourcePositions;
			FVector* PositionsPointer = nullptr;
			if (TransferedPositions.Num() == MeshLODCount)
			{
				ReadbackBuffer(SourcePositions, TransferedPositions[LODIndex]);
				PositionsPointer = SourcePositions.GetData();
			}
			else
			{
				FPositionVertexBuffer& VertexBuffer = LODRenderData.StaticVertexBuffers.PositionVertexBuffer;
				PositionsPointer = static_cast<FVector*>(VertexBuffer.GetVertexData());
			}

			if (LocalSamples)
			{
				TArray<bool> ValidPoints;
				for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
				{
					FillLocalValidPoints(LODRenderData, TargetSection, OutHairGroupDatas[GroupIt].RenRootData.MeshProjectionLODs[LODIndex], ValidPoints);

					FPointsSampler PointsSampler(ValidPoints, PositionsPointer, MaxSamples);
					const uint32 SampleCount = PointsSampler.SamplePositions.Num();

					FWeightsBuilder InterpolationWeights(SampleCount, SampleCount,
						PointsSampler.SamplePositions.GetData(), PointsSampler.SamplePositions.GetData());

					//const FVector Displace(0.0,0.0,10.0);
					//TArray<FVector> Deltas; Deltas.Init(FVector::ZeroVector, SampleCount );
					//for (uint32 i = 0; i < SampleCount; ++i)
					//{
					//	Deltas[i] = FVector(0,0,0);
					//	for (uint32 j = 0; j < SampleCount; ++j)
					//	{
					//		Deltas[i] += InterpolationWeights.InverseEntries[SampleCount * i + j] * Displace;
					//	}
					//	UE_LOG(LogHairStrands, Log, TEXT("[Groom] Sample Deltas[%d] = %s"), i, *Deltas[i].ToString());
					//}
					//for (uint32 i = 0; i < GroomAsset->HairGroupsData[GroupIt].HairSimulationData.StrandsPoints.Num(); ++i)
					//{
					//	FVector Offset(0,0,0);
					//	for (uint32 j = 0; j < SampleCount; ++j)
					//	{
					//		const FVector DeltaPosition = GroomAsset->HairGroupsData[GroupIt].HairSimulationData.StrandsPoints.PointsPosition[i] - PointsSampler.SamplePositions[j];
					//		const float FunctionValue = FMath::Sqrt(FVector::DotProduct(DeltaPosition, DeltaPosition)+1.0);
					//		Offset += FunctionValue * Deltas[j];
					//	}
					//	UE_LOG(LogHairStrands, Log, TEXT("[Groom] Sample Displace[%d] = %s"), i, *Offset.ToString());
					//}

					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].SimRootData);
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].RenRootData);

					const uint32 CardsLODCount = OutHairGroupDatas[GroupIt].CardsRootData.Num();
					for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
					{
						if (OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt].IsValid())
						{
							UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt]);
						}
					}
				}
			}
			else
			{
				TArray<bool> ValidPoints;

				FillGlobalValidPoints(LODRenderData, TargetSection, ValidPoints);

				FPointsSampler PointsSampler(ValidPoints, PositionsPointer, MaxSamples);
				const uint32 SampleCount = PointsSampler.SamplePositions.Num();

				FWeightsBuilder InterpolationWeights(SampleCount, SampleCount,
					PointsSampler.SamplePositions.GetData(), PointsSampler.SamplePositions.GetData());

				for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
				{
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].SimRootData);
					UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].RenRootData);

					const uint32 CardsLODCount = OutHairGroupDatas[GroupIt].CardsRootData.Num();
					for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
					{
						if (OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt].IsValid())
						{
							UpdateInterpolationWeights(InterpolationWeights, PointsSampler, LODIndex, OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt]);
						}
					}
				}
			}
		}
		for (uint32 GroupIt = 0; GroupIt < GroupCount; ++GroupIt)
		{
			WritebackGroupData(OutHairGroupDatas[GroupIt].SimRootData, OutHairGroupResources[GroupIt].SimRootResources);
			WritebackGroupData(OutHairGroupDatas[GroupIt].RenRootData, OutHairGroupResources[GroupIt].RenRootResources);

			const uint32 CardsLODCount = OutHairGroupDatas[GroupIt].CardsRootData.Num();
			for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
			{
				if (OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt].IsValid())
				{
					WritebackGroupData(OutHairGroupDatas[GroupIt].CardsRootData[CardsLODIt], OutHairGroupResources[GroupIt].CardsRootResources[CardsLODIt]);
				}
			}
		}
	}
} // namespace GroomBinding_GPU

///////////////////////////////////////////////////////////////////////////////////////////////////
// Main entry (GPU path)
static void InternalBuildBinding_GPU(FRDGBuilder& GraphBuilder, UGroomBindingAsset* BindingAsset)
{
	if (!BindingAsset ||
		!BindingAsset->Groom ||
		!BindingAsset->TargetSkeletalMesh ||
		BindingAsset->Groom->GetNumHairGroups() == 0)
	{
		UE_LOG(LogHairStrands, Warning, TEXT("[Groom] Error - Binding asset can be created/rebuilt."));
		return;
	}

	const int32 NumInterpolationPoints = BindingAsset->NumInterpolationPoints;
	UGroomAsset* GroomAsset = BindingAsset->Groom;
	USkeletalMesh* SourceSkeletalMesh = BindingAsset->SourceSkeletalMesh;
	USkeletalMesh* TargetSkeletalMesh = BindingAsset->TargetSkeletalMesh;

	const uint32 MeshLODCount = BindingAsset->TargetSkeletalMesh->GetLODNum();
	UGroomBindingAsset::FHairGroupDatas& OutHairGroupDatas = BindingAsset->HairGroupDatas;
	OutHairGroupDatas.Empty();
	TArray<uint32> NumSamples;
	NumSamples.Init(NumInterpolationPoints, MeshLODCount);
	for (const FHairGroupData& GroupData : GroomAsset->HairGroupsData)
	{
		UGroomBindingAsset::FHairGroupData& Data = OutHairGroupDatas.AddDefaulted_GetRef();
		Data.RenRootData = FHairStrandsRootData(&GroupData.Strands.Data, MeshLODCount, NumSamples);
		Data.SimRootData = FHairStrandsRootData(&GroupData.Guides.Data, MeshLODCount, NumSamples);

		const uint32 CardsLODCount = GroupData.Cards.LODs.Num();
		Data.CardsRootData.SetNum(GroupData.Cards.LODs.Num());
		for (uint32 CardsLODIt = 0; CardsLODIt < CardsLODCount; ++CardsLODIt)
		{
			Data.CardsRootData[CardsLODIt].Reset();
			if (GroupData.Cards.IsValid(CardsLODIt))
			{
				Data.CardsRootData[CardsLODIt] = FHairStrandsRootData(&GroupData.Cards.LODs[CardsLODIt].Guides.Data, MeshLODCount, NumSamples);
			}
		}
	}

	UGroomBindingAsset::FHairGroupResources& OutHairGroupResources = BindingAsset->HairGroupResources;
	if (BindingAsset->HairGroupResources.Num() > 0)
	{
		for (UGroomBindingAsset::FHairGroupResource& GroupResrouces : OutHairGroupResources)
		{
			BindingAsset->HairGroupResourcesToDelete.Enqueue(GroupResrouces);
		}
		OutHairGroupResources.Empty();
	}

	check(OutHairGroupResources.Num() == 0);
	for (UGroomBindingAsset::FHairGroupData& GroupData : OutHairGroupDatas)
	{
		UGroomBindingAsset::FHairGroupResource& Resource = OutHairGroupResources.AddDefaulted_GetRef();
		Resource.SimRootResources = new FHairStrandsRestRootResource(GroupData.SimRootData);
		Resource.RenRootResources = new FHairStrandsRestRootResource(GroupData.RenRootData);

		Resource.SimRootResources->InitRHI();
		Resource.RenRootResources->InitRHI();

		const uint32 CardsLODCount = GroupData.CardsRootData.Num();
		Resource.CardsRootResources.SetNum(CardsLODCount);
		for (uint32 CardsLODIt=0; CardsLODIt< CardsLODCount; ++CardsLODIt)
		{
			Resource.CardsRootResources[CardsLODIt] = nullptr;

			if (GroupData.CardsRootData[CardsLODIt].IsValid())
			{
				Resource.CardsRootResources[CardsLODIt] = new FHairStrandsRestRootResource(GroupData.CardsRootData[CardsLODIt]);
				Resource.CardsRootResources[CardsLODIt]->InitRHI();
			}
		}
	}

	TArray<FGoomBindingGroupInfo>& OutGroupInfos = BindingAsset->GroupInfos;
	OutGroupInfos.Empty();
	for (const UGroomBindingAsset::FHairGroupData& Data : OutHairGroupDatas)
	{
		FGoomBindingGroupInfo& Info = OutGroupInfos.AddDefaulted_GetRef();
		Info.SimRootCount = Data.SimRootData.RootCount;
		Info.SimLODCount = Data.SimRootData.MeshProjectionLODs.Num();
		Info.RenRootCount = Data.RenRootData.RootCount;
		Info.RenLODCount = Data.RenRootData.MeshProjectionLODs.Num();
	}

	FSkeletalMeshRenderData* TargetRenderData = TargetSkeletalMesh->GetResourceForRendering();
	FHairStrandsProjectionMeshData TargetMeshData = ExtractMeshData(TargetRenderData);

	// Create mapping between the source & target using their UV
	// The lifetime of 'TransferredPositions' needs to encompass RunProjection
	struct FTransferData
	{
		TArray<FRWBuffer> TransferredPositions;
	};
	FTransferData* TransferData = new FTransferData();

	if (FSkeletalMeshRenderData* SourceRenderData = SourceSkeletalMesh ? SourceSkeletalMesh->GetResourceForRendering() : nullptr)
	{
		FHairStrandsProjectionMeshData SourceMeshData = ExtractMeshData(SourceRenderData);
		FGroomBindingBuilder::TransferMesh(
			GraphBuilder,
			SourceMeshData,
			TargetMeshData,
			TransferData->TransferredPositions);

		for (uint32 LODIndex = 0; LODIndex < MeshLODCount; ++LODIndex)
		{
			for (FHairStrandsProjectionMeshData::Section& Section : TargetMeshData.LODs[LODIndex].Sections)
			{
				Section.PositionBuffer = TransferData->TransferredPositions[LODIndex].SRV;
			}
		}
	}

	TArray<FHairStrandsRestRootResource*> RootResources;
	for (const UGroomBindingAsset::FHairGroupResource& GroupResources : OutHairGroupResources)
	{
		RootResources.Add(GroupResources.RenRootResources);
		RootResources.Add(GroupResources.SimRootResources);

		for (FHairStrandsRestRootResource* CardsRootResources : GroupResources.CardsRootResources)
		{
			if (CardsRootResources)
			{
				RootResources.Add(CardsRootResources);
			}
		}
	}
	FGroomBindingBuilder::ProjectStrands(GraphBuilder, FTransform::Identity, TargetMeshData, RootResources);

	// Readback the data
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GroomBinding_Readback"),
		ERDGPassFlags::None,
		[BindingAsset, TargetRenderData, TransferData](FRHICommandList& RHICmdList)
		{
			GroomBinding_GPU::ComputeInterpolationWeights(BindingAsset, TargetRenderData, TransferData->TransferredPositions);
			BindingAsset->QueryStatus = UGroomBindingAsset::EQueryStatus::Completed;
		});
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Asynchronous queuing for binding generation (GPU)
struct FBindingQuery
{
	UGroomBindingAsset* Asset = nullptr;
};
TQueue<FBindingQuery> GBindingQueries;

bool HasHairStrandsBindigQueries()
{
	return !GBindingQueries.IsEmpty();
}

void RunHairStrandsBindingQueries(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap)
{
	FBindingQuery Q;
	while (GBindingQueries.Dequeue(Q))
	{
		if (Q.Asset)
		{
			InternalBuildBinding_GPU(GraphBuilder, Q.Asset);
		}
	}
}

bool FGroomBindingBuilder::BuildBinding(UGroomBindingAsset* BindingAsset, bool bUseGPU, bool bInitResources)
{
	bool bSucceed = false;
	if (!bUseGPU)
	{
		bSucceed = InternalBuildBinding_CPU(BindingAsset, bInitResources);
	}
	else
	{
		BindingAsset->QueryStatus = UGroomBindingAsset::EQueryStatus::Submitted;
		GBindingQueries.Enqueue({ BindingAsset });
		bSucceed = true;
	}

	return bSucceed;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Immediate version

void FGroomBindingBuilder::TransferMesh(
	FRDGBuilder& GraphBuilder,
	const FHairStrandsProjectionMeshData& SourceMeshData,
	const FHairStrandsProjectionMeshData& TargetMeshData,
	TArray<FRWBuffer>& OutTransferedPositions)
{
	const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);

	FBufferTransitionQueue TransitionQueue;

	const uint32 MeshLODCount = TargetMeshData.LODs.Num();
	OutTransferedPositions.SetNum(MeshLODCount);
	for (uint32 LODIndex = 0; LODIndex < MeshLODCount; ++LODIndex)
	{
		check(TargetMeshData.LODs[LODIndex].Sections.Num() > 0);

		OutTransferedPositions[LODIndex].Initialize(sizeof(float), TargetMeshData.LODs[LODIndex].Sections[0].TotalVertexCount * 3, PF_R32_FLOAT);
		::TransferMesh(GraphBuilder, ShaderMap, LODIndex, SourceMeshData, TargetMeshData, OutTransferedPositions[LODIndex], TransitionQueue);
	}

	TransitBufferToReadable(GraphBuilder, TransitionQueue);
}

void FGroomBindingBuilder::ProjectStrands(
	FRDGBuilder& GraphBuilder,
	const FTransform& LocalToWorld,
	const FHairStrandsProjectionMeshData& TargetMeshData,
	TArray<FHairStrandsRestRootResource*>& InRestRootResources)
{
	const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);

	FBufferTransitionQueue TransitionQueue;

	for (FHairStrandsRestRootResource* RestRootResource : InRestRootResources)
	{
		for (FHairStrandsRestRootResource::FLOD& LODData : RestRootResource->LODs)
		{
			const uint32 LODIndex = LODData.LODIndex;
			ProjectHairStrandsOntoMesh(
				GraphBuilder,
				ShaderMap,
				LODIndex,
				TargetMeshData,
				RestRootResource,
				TransitionQueue);

			AddHairStrandUpdateMeshTrianglesPass(
				GraphBuilder,
				ShaderMap,
				LODIndex,
				HairStrandsTriangleType::RestPose,
				TargetMeshData.LODs[LODIndex],
				RestRootResource,
				nullptr,
				TransitionQueue);
		}
	}

	TransitBufferToReadable(GraphBuilder, TransitionQueue);
}


#undef LOCTEXT_NAMESPACE