// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utils/ClothingMeshUtils.h"

#include "ClothPhysicalMeshData.h"

#include "Math/UnrealMathUtility.h"
#include "Logging/LogMacros.h"
#include "Async/ParallelFor.h"

#if WITH_EDITOR
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

DEFINE_LOG_CATEGORY(LogClothingMeshUtils)
DECLARE_CYCLE_STAT(TEXT("Skin Physics Mesh"), STAT_ClothSkinPhysMesh, STATGROUP_Physics);

#define LOCTEXT_NAMESPACE "ClothingMeshUtils"

namespace ClothingMeshUtils
{
	// Explicit template instantiations of SkinPhysicsMesh
	template
	void CLOTHINGSYSTEMRUNTIMECOMMON_API SkinPhysicsMesh<true, false>(const TArray<int32>& BoneMap, const FClothPhysicalMeshData& InMesh, const FTransform& RootBoneTransform,
		const FMatrix* InBoneMatrices, const int32 InNumBoneMatrices, TArray<FVector>& OutPositions, TArray<FVector>& OutNormals, uint32 ArrayOffset);
	template
	void CLOTHINGSYSTEMRUNTIMECOMMON_API SkinPhysicsMesh<false, true>(const TArray<int32>& BoneMap, const FClothPhysicalMeshData& InMesh, const FTransform& RootBoneTransform,
		const FMatrix* InBoneMatrices, const int32 InNumBoneMatrices, TArray<FVector>& OutPositions, TArray<FVector>& OutNormals, uint32 ArrayOffset);

	// inline function used to force the unrolling of the skinning loop
	FORCEINLINE void AddInfluence(FVector& OutPosition, FVector& OutNormal, const FVector& RefParticle, const FVector& RefNormal, const FMatrix& BoneMatrix, const float Weight)
	{
		OutPosition += BoneMatrix.TransformPosition(RefParticle) * Weight;
		OutNormal += BoneMatrix.TransformVector(RefNormal) * Weight;
	}

	template<bool bInPlaceOutput, bool bRemoveScaleAndInvertPostTransform>
	void SkinPhysicsMesh(
		const TArray<int32>& InBoneMap,
		const FClothPhysicalMeshData& InMesh,
		const FTransform& PostTransform,
		const FMatrix* InBoneMatrices,
		const int32 InNumBoneMatrices,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals,
		uint32 ArrayOffset)
	{
		SCOPE_CYCLE_COUNTER(STAT_ClothSkinPhysMesh);

		const uint32 NumVerts = InMesh.Vertices.Num();

		if(!bInPlaceOutput)
		{
			ensure(ArrayOffset == 0);
			OutPositions.Reset(NumVerts);
			OutNormals.Reset(NumVerts);
			OutPositions.AddZeroed(NumVerts);
			OutNormals.AddZeroed(NumVerts);
		}
		else
		{
			check((uint32) OutPositions.Num() >= NumVerts + ArrayOffset);
			check((uint32) OutNormals.Num() >= NumVerts + ArrayOffset);
			// PS4 performance note: It is faster to zero the memory first instead of changing this function to work with uninitialized memory
			FMemory::Memzero((uint8*)OutPositions.GetData() + ArrayOffset * sizeof(FVector), NumVerts * sizeof(FVector));
			FMemory::Memzero((uint8*)OutNormals.GetData() + ArrayOffset * sizeof(FVector), NumVerts * sizeof(FVector));
		}

		const int32 MaxInfluences = InMesh.MaxBoneWeights;
		UE_CLOG(MaxInfluences > 12, LogClothingMeshUtils, Warning, TEXT("The cloth physics mesh skinning code can't cope with more than 12 bone influences."));

		const int32* const RESTRICT BoneMap = InBoneMap.GetData();  // Remove RangeCheck for faster skinning in development builds
		const FMatrix* const RESTRICT BoneMatrices = InBoneMatrices;
		
		static const uint32 MinParallelVertices = 500;  // 500 seems to be the lowest threshold still giving gains even on profiled assets that are only using a small number of influences

		ParallelFor(NumVerts, [&InMesh, &PostTransform, BoneMap, BoneMatrices, &OutPositions, &OutNormals, ArrayOffset](uint32 VertIndex)
		{
			// Fixed particle, needs to be skinned
			const uint16* const RESTRICT BoneIndices = InMesh.BoneData[VertIndex].BoneIndices;
			const float* const RESTRICT BoneWeights = InMesh.BoneData[VertIndex].BoneWeights;

			// WARNING - HORRIBLE UNROLLED LOOP + JUMP TABLE BELOW
			// done this way because this is a pretty tight and perf critical loop. essentially
			// rather than checking each influence we can just jump into this switch and fall through
			// everything to compose the final skinned data
			const FVector& RefParticle = InMesh.Vertices[VertIndex];
			const FVector& RefNormal = InMesh.Normals[VertIndex];
			FVector& OutPosition = OutPositions[bInPlaceOutput ? VertIndex + ArrayOffset : VertIndex];
			FVector& OutNormal = OutNormals[bInPlaceOutput ? VertIndex + ArrayOffset : VertIndex];
			switch (InMesh.BoneData[VertIndex].NumInfluences)
			{
			case 12: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[11]]], BoneWeights[11]);  // Intentional fall through
			case 11: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[10]]], BoneWeights[10]);  // Intentional fall through
			case 10: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 9]]], BoneWeights[ 9]);  // Intentional fall through
			case  9: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 8]]], BoneWeights[ 8]);  // Intentional fall through
			case  8: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 7]]], BoneWeights[ 7]);  // Intentional fall through
			case  7: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 6]]], BoneWeights[ 6]);  // Intentional fall through
			case  6: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 5]]], BoneWeights[ 5]);  // Intentional fall through
			case  5: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 4]]], BoneWeights[ 4]);  // Intentional fall through
			case  4: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 3]]], BoneWeights[ 3]);  // Intentional fall through
			case  3: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 2]]], BoneWeights[ 2]);  // Intentional fall through
			case  2: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 1]]], BoneWeights[ 1]);  // Intentional fall through
			case  1: AddInfluence(OutPosition, OutNormal, RefParticle, RefNormal, BoneMatrices[BoneMap[BoneIndices[ 0]]], BoneWeights[ 0]);  // Intentional fall through
			default: break;
			}

			if (bRemoveScaleAndInvertPostTransform)
			{
				// Ignore any user scale. It's already accounted for in our skinning matrices
				// This is the use case for NVcloth
				FTransform PostTransformInternal = PostTransform;
				PostTransformInternal.SetScale3D(FVector(1.0f));

				OutPosition = PostTransformInternal.InverseTransformPosition(OutPosition);
				OutNormal = PostTransformInternal.InverseTransformVector(OutNormal);
			}
			else
			{
				OutPosition = PostTransform.TransformPosition(OutPosition);
				OutNormal = PostTransform.TransformVector(OutNormal);
			}

			if (OutNormal.SizeSquared() > SMALL_NUMBER)
			{
				OutNormal = OutNormal.GetUnsafeNormal();
			}
		}, NumVerts > MinParallelVertices ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
	}

	/** 
	 * Gets the best match triangle for a specified position from the triangles in Mesh.
	 * Performs no validation on the incoming mesh data, the mesh data should be verified
	 * to be valid before using this function
	 */
	int32 GetBestTriangleBaseIndex(const ClothMeshDesc& Mesh, const FVector& Position)
	{
		float MinimumDistanceSq = MAX_flt;
		int32 ClosestBaseIndex = INDEX_NONE;

		const TArray<int32> Tris = const_cast<ClothMeshDesc&>(Mesh).FindCandidateTriangles(Position);
		int32 NumTriangles = Tris.Num();
		if (!NumTriangles)
		{
			NumTriangles = Mesh.Indices.Num() / 3;
		}
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			int32 TriBaseIdx = (Tris.Num() ? Tris[TriIdx] : TriIdx) * 3;

			const uint32 IA = Mesh.Indices[TriBaseIdx + 0];
			const uint32 IB = Mesh.Indices[TriBaseIdx + 1];
			const uint32 IC = Mesh.Indices[TriBaseIdx + 2];

			const FVector& A = Mesh.Positions[IA];
			const FVector& B = Mesh.Positions[IB];
			const FVector& C = Mesh.Positions[IC];

			FVector PointOnTri = FMath::ClosestPointOnTriangleToPoint(Position, A, B, C);
			float DistSq = (PointOnTri - Position).SizeSquared();

			if (DistSq < MinimumDistanceSq)
			{
				MinimumDistanceSq = DistSq;
				ClosestBaseIndex = TriBaseIdx;
			}
		}

		return ClosestBaseIndex;
	}

	void GenerateMeshToMeshSkinningData(TArray<FMeshToMeshVertData>& OutSkinningData, const ClothMeshDesc& TargetMesh, const TArray<FVector>* TargetTangents, const ClothMeshDesc& SourceMesh)
	{
		if(!TargetMesh.HasValidMesh())
		{
			UE_LOG(LogClothingMeshUtils, Warning, TEXT("Failed to generate mesh to mesh skinning data. Invalid Target Mesh."));
			return;
		}

		if(!SourceMesh.HasValidMesh())
		{
			UE_LOG(LogClothingMeshUtils, Warning, TEXT("Failed to generate mesh to mesh skinning data. Invalid Source Mesh."));
			return;
		}

		const int32 NumMesh0Verts = TargetMesh.Positions.Num();
		const int32 NumMesh0Normals = TargetMesh.Normals.Num();
		const int32 NumMesh0Tangents = TargetTangents ? TargetTangents->Num() : 0;

		const int32 NumMesh1Verts = SourceMesh.Positions.Num();
		const int32 NumMesh1Normals = SourceMesh.Normals.Num();
		const int32 NumMesh1Indices = SourceMesh.Indices.Num();

		// Check we have properly formed triangles
		check(NumMesh1Indices % 3 == 0);

		const int32 NumMesh1Triangles = NumMesh1Indices / 3;

		// Check mesh data to make sure we have the same number of each element
		if(NumMesh0Verts != NumMesh0Normals || (TargetTangents && NumMesh0Tangents != NumMesh0Verts))
		{
			UE_LOG(LogClothingMeshUtils, Warning, TEXT("Can't generate mesh to mesh skinning data, Mesh0 data is missing verts."));
			return;
		}

		if(NumMesh1Verts != NumMesh1Normals)
		{
			UE_LOG(LogClothingMeshUtils, Warning, TEXT("Can't generate mesh to mesh skinning data, Mesh1 data is missing verts."));
			return;
		}

		OutSkinningData.Reserve(NumMesh0Verts);

		// For all mesh0 verts
		for(int32 VertIdx0 = 0; VertIdx0 < NumMesh0Verts; ++VertIdx0)
		{
			OutSkinningData.AddZeroed();
			FMeshToMeshVertData& SkinningData = OutSkinningData.Last();

			const FVector& VertPosition = TargetMesh.Positions[VertIdx0];
			const FVector& VertNormal = TargetMesh.Normals[VertIdx0];

			FVector VertTangent;
			if(TargetTangents)
			{
				VertTangent = (*TargetTangents)[VertIdx0];
			}
			else
			{
				FVector Tan0, Tan1;
				VertNormal.FindBestAxisVectors(Tan0, Tan1);
				VertTangent = Tan0;
			}

			int32 ClosestTriangleBaseIdx = GetBestTriangleBaseIndex(SourceMesh, VertPosition);

			check(ClosestTriangleBaseIdx != INDEX_NONE);

			const FVector& A = SourceMesh.Positions[SourceMesh.Indices[ClosestTriangleBaseIdx]];
			const FVector& B = SourceMesh.Positions[SourceMesh.Indices[ClosestTriangleBaseIdx + 1]];
			const FVector& C = SourceMesh.Positions[SourceMesh.Indices[ClosestTriangleBaseIdx + 2]];

			const FVector& NA = SourceMesh.Normals[SourceMesh.Indices[ClosestTriangleBaseIdx]];
			const FVector& NB = SourceMesh.Normals[SourceMesh.Indices[ClosestTriangleBaseIdx + 1]];
			const FVector& NC = SourceMesh.Normals[SourceMesh.Indices[ClosestTriangleBaseIdx + 2]];

			// Before generating the skinning data we need to check for a degenerate triangle.
			// If we find _any_ degenerate triangles we will notify and fail to generate the skinning data
			const FVector TriNormal = FVector::CrossProduct(B - A, C - A);
			if(TriNormal.SizeSquared() < SMALL_NUMBER)
			{
				// Failed, we have 2 identical vertices
				OutSkinningData.Reset();

				// Log and toast
				FText Error = FText::Format(LOCTEXT("DegenerateTriangleError", "Failed to generate skinning data, found conincident vertices in triangle A={0} B={1} C={2}"), FText::FromString(A.ToString()), FText::FromString(B.ToString()), FText::FromString(C.ToString()));

				UE_LOG(LogClothingMeshUtils, Warning, TEXT("%s"), *Error.ToString());

#if WITH_EDITOR
				FNotificationInfo Info(Error);
				Info.ExpireDuration = 5.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
#endif
				return;
			}

			SkinningData.PositionBaryCoordsAndDist = GetPointBaryAndDist(A, B, C, NA, NB, NC, VertPosition);
			SkinningData.NormalBaryCoordsAndDist = GetPointBaryAndDist(A, B, C, NA, NB, NC, VertPosition + VertNormal);
			SkinningData.TangentBaryCoordsAndDist = GetPointBaryAndDist(A, B, C, NA, NB, NC, VertPosition + VertTangent);
			SkinningData.SourceMeshVertIndices[0] = SourceMesh.Indices[ClosestTriangleBaseIdx];
			SkinningData.SourceMeshVertIndices[1] = SourceMesh.Indices[ClosestTriangleBaseIdx + 1];
			SkinningData.SourceMeshVertIndices[2] = SourceMesh.Indices[ClosestTriangleBaseIdx + 2];
			SkinningData.SourceMeshVertIndices[3] = 0;
		}
	}

	// TODO: Vertex normals are not used at present, a future improved algorithm might however
	FVector4 GetPointBaryAndDist(const FVector& A, const FVector& B, const FVector& C, const FVector& NA, const FVector& NB, const FVector& NC, const FVector& Point)
	{
		FPlane TrianglePlane(A, B, C);
		const FVector PointOnTriPlane = FVector::PointPlaneProject(Point, TrianglePlane);
		const FVector BaryCoords = FMath::ComputeBaryCentric2D(PointOnTriPlane, A, B, C);
		return FVector4(BaryCoords, TrianglePlane.PlaneDot(Point)); // Note: The normal of the plane points away from the Clockwise face (instead of the counter clockwise face) in Left Handed Coordinates (This is why we need to invert the normals later on when before sending it to the shader)
	}

	void GenerateEmbeddedPositions(const ClothMeshDesc& SourceMesh, TArrayView<const FVector> Positions, TArray<FVector4>& OutEmbeddedPositions, TArray<int32>& OutSourceIndices)
	{
		if(!SourceMesh.HasValidMesh())
		{
			// No valid source mesh
			return;
		}

		const int32 NumPositions = Positions.Num();

		OutEmbeddedPositions.Reset();
		OutEmbeddedPositions.AddUninitialized(NumPositions);

		OutSourceIndices.Reset(NumPositions * 3);

		for(int32 PositionIndex = 0 ; PositionIndex < NumPositions ; ++PositionIndex)
		{
			const FVector& Position = Positions[PositionIndex];

			int32 TriBaseIndex = GetBestTriangleBaseIndex(SourceMesh, Position);

			const int32 IA = SourceMesh.Indices[TriBaseIndex];
			const int32 IB = SourceMesh.Indices[TriBaseIndex + 1];
			const int32 IC = SourceMesh.Indices[TriBaseIndex + 2];

			const FVector& A = SourceMesh.Positions[IA];
			const FVector& B = SourceMesh.Positions[IB];
			const FVector& C = SourceMesh.Positions[IC];

			const FVector& NA = SourceMesh.Normals[IA];
			const FVector& NB = SourceMesh.Normals[IB];
			const FVector& NC = SourceMesh.Normals[IC];

			OutEmbeddedPositions[PositionIndex] = GetPointBaryAndDist(A, B, C, NA, NB, NC, Position);
			OutSourceIndices.Add(IA);
			OutSourceIndices.Add(IB);
			OutSourceIndices.Add(IC);
		}
	}

	void FVertexParameterMapper::Map(TArrayView<const float> Source, TArray<float>& Dest)
	{
		Map(Source, Dest, [](FVector Bary, float A, float B, float C)
		{
			return Bary.X * A + Bary.Y * B + Bary.Z * C;
		});
	}

}

#undef LOCTEXT_NAMESPACE
