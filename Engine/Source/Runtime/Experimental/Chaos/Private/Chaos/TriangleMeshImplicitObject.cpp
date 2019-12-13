// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/Capsule.h"
#include "Chaos/GJK.h"
#include "Chaos/Triangle.h"
#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectScaled.h"

namespace Chaos
{
FTriangleMeshImplicitObject::FTriangleMeshImplicitObject(TParticles<FReal, 3>&& Particles, TArray<TVector<int32, 3>>&& Elements, TArray<uint16>&& InMaterialIndices)
	: FImplicitObject(EImplicitObject::HasBoundingBox, ImplicitObjectType::TriangleMesh)
	, MParticles(MoveTemp(Particles))
	, MElements(MoveTemp(Elements))
	, MLocalBoundingBox(MParticles.X(0), MParticles.X(0))
    , MaterialIndices(MoveTemp(InMaterialIndices))
{
	for (uint32 Idx = 1; Idx < MParticles.Size(); ++Idx)
	{
		MLocalBoundingBox.GrowToInclude(MParticles.X(Idx));
	}
	RebuildBV();
}

struct FTriangleMeshRaycastVisitor
{
	FTriangleMeshRaycastVisitor(const FVec3& InStart, const FVec3& InDir, const FReal InThickness, const TParticles<FReal,3>& InParticles, const TArray<TVector<int32, 3>>& InElements)
	: Particles(InParticles)
	, Elements(InElements)
	, StartPoint(InStart)
	, Dir(InDir)
	, Thickness(InThickness)
	, OutTime(TNumericLimits<FReal>::Max())
	{
	}

	enum class ERaycastType
	{
		Raycast,
		Sweep
	};

	template <ERaycastType SQType>
	bool Visit(int32 TriIdx, FQueryFastData& CurData)
	{
		constexpr FReal Epsilon = 1e-4;
		constexpr FReal Epsilon2 = Epsilon * Epsilon;
		const FReal Thickness2 = SQType == ERaycastType::Sweep ? Thickness * Thickness : 0;
		FReal MinTime = 0;	//no need to initialize, but fixes warning

		const FReal R = Thickness + Epsilon;
		const FReal R2 = R * R;

		const FVec3& A = Particles.X(Elements[TriIdx][0]);
		const FVec3& B = Particles.X(Elements[TriIdx][1]);
		const FVec3& C = Particles.X(Elements[TriIdx][2]);

		const FVec3 AB = B - A;
		const FVec3 AC = C - A;
		FVec3 TriNormal = FVec3::CrossProduct(AB, AC);
		const FReal NormalLength = TriNormal.SafeNormalize();
		if (!CHAOS_ENSURE(NormalLength > Epsilon))
		{
			//hitting degenerate triangle so keep searching - should be fixed before we get to this stage
			return true;
		}

		const TPlane<FReal, 3> TriPlane{ A, TriNormal };
		FVec3 RaycastPosition;
		FVec3 RaycastNormal;
		FReal Time;

		//Check if we even intersect with triangle plane
		int32 DummyFaceIndex;
		if (TriPlane.Raycast(StartPoint, Dir, CurData.CurrentLength, Thickness, Time, RaycastPosition, RaycastNormal, DummyFaceIndex))
		{
			FVec3 IntersectionPosition = RaycastPosition;
			FVec3 IntersectionNormal = RaycastNormal;
			bool bTriangleIntersects = false;
			if (Time == 0)
			{
				//Initial overlap so no point of intersection, do an explicit sphere triangle test.
				const FVec3 ClosestPtOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, StartPoint);
				const FReal DistToTriangle2 = (StartPoint - ClosestPtOnTri).SizeSquared();
				if (DistToTriangle2 <= R2)
				{
					OutTime = 0;
					OutFaceIndex = TriIdx;
					return false; //no one will beat Time == 0
				}
			}
			else
			{
				const FVec3 ClosestPtOnTri = FindClosestPointOnTriangle(RaycastPosition, A, B, C, RaycastPosition);	//We know Position is on the triangle plane
				const FReal DistToTriangle2 = (RaycastPosition - ClosestPtOnTri).SizeSquared();
				bTriangleIntersects = DistToTriangle2 <= Epsilon2;	//raycast gave us the intersection point so sphere radius is already accounted for
			}

			if (SQType == ERaycastType::Sweep && !bTriangleIntersects)
			{
				//sphere is not immediately touching the triangle, but it could start intersecting the perimeter as it sweeps by
				FVec3 BorderPositions[3];
				FVec3 BorderNormals[3];
				FReal BorderTimes[3];
				bool bBorderIntersections[3];

				{
					FVec3 ABCapsuleAxis = B - A;
					FReal ABHeight = ABCapsuleAxis.SafeNormalize();
					bBorderIntersections[0] = TCapsule<FReal>::RaycastFast(Thickness, ABHeight, ABCapsuleAxis, A, B, StartPoint, Dir, CurData.CurrentLength, 0, BorderTimes[0], BorderPositions[0], BorderNormals[0], DummyFaceIndex);
				}
				
				{
					FVec3 BCCapsuleAxis = C - B;
					FReal BCHeight = BCCapsuleAxis.SafeNormalize();
					bBorderIntersections[1] = TCapsule<FReal>::RaycastFast(Thickness, BCHeight, BCCapsuleAxis, B, C, StartPoint, Dir, CurData.CurrentLength, 0, BorderTimes[1], BorderPositions[1], BorderNormals[1], DummyFaceIndex);
				}
				
				{
					FVec3 ACCapsuleAxis = C - A;
					FReal ACHeight = ACCapsuleAxis.SafeNormalize();
					bBorderIntersections[2] = TCapsule<FReal>::RaycastFast(Thickness, ACHeight, ACCapsuleAxis, A, C, StartPoint, Dir, CurData.CurrentLength, 0, BorderTimes[2], BorderPositions[2], BorderNormals[2], DummyFaceIndex);
				}

				int32 MinBorderIdx = INDEX_NONE;
				FReal MinBorderTime = 0;	//initialization not needed, but fixes warning

				for (int32 BorderIdx = 0; BorderIdx < 3; ++BorderIdx)
				{
					if (bBorderIntersections[BorderIdx])
					{
						if (!bTriangleIntersects || BorderTimes[BorderIdx] < MinBorderTime)
						{
							MinBorderTime = BorderTimes[BorderIdx];
							MinBorderIdx = BorderIdx;
							bTriangleIntersects = true;
						}
					}
				}

				if (MinBorderIdx != INDEX_NONE)
				{
					IntersectionNormal = BorderNormals[MinBorderIdx];
					IntersectionPosition = BorderPositions[MinBorderIdx] - IntersectionNormal * Thickness;

					if (Time == 0)
					{
						//we were initially overlapping with triangle plane so no normal was given. Compute it now
						FVec3 TmpNormal;
						const FReal SignedDistance = TriPlane.PhiWithNormal(StartPoint, TmpNormal);
						RaycastNormal = SignedDistance >= 0 ? TmpNormal : -TmpNormal;
					}

					Time = MinBorderTime;
				}
			}

			if (bTriangleIntersects)
			{
				if (Time < OutTime)
				{
					OutPosition = IntersectionPosition;
					OutNormal = RaycastNormal;	//We use the plane normal even when hitting triangle edges. This is to deal with triangles that approximate a single flat surface.
					OutTime = Time;
					CurData.SetLength(Time);	//prevent future rays from going any farther
					OutFaceIndex = TriIdx;
				}
			}
		}

		return true;
	}

	bool VisitRaycast(TSpatialVisitorData<int32> TriIdx, FQueryFastData& CurData)
	{
		return Visit<ERaycastType::Raycast>(TriIdx.Payload, CurData);
	}

	bool VisitSweep(TSpatialVisitorData<int32> TriIdx, FQueryFastData& CurData)
	{
		return Visit<ERaycastType::Sweep>(TriIdx.Payload, CurData);
	}

	bool VisitOverlap(TSpatialVisitorData<int32> TriIdx)
	{
		check(false);
		return true;
	}

	const TParticles<FReal, 3>& Particles;
	const TArray<TVector<int32, 3>>& Elements;
	const FVec3& StartPoint;
	const FVec3& Dir;
	const FReal Thickness;
	FReal OutTime;
	FVec3 OutPosition;
	FVec3 OutNormal;
	int32 OutFaceIndex;
};

FReal FTriangleMeshImplicitObject::PhiWithNormal(const FVec3& x, FVec3& Normal) const
{
	ensure(false);	//not supported yet - might support it in the future or we may change the interface
	return 0;
}

bool FTriangleMeshImplicitObject::Raycast(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, const FReal Thickness, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex) const
{
	FTriangleMeshRaycastVisitor SQVisitor(StartPoint, Dir, Thickness, MParticles, MElements);

	if (Thickness > 0)
	{
		BVH.Sweep(StartPoint, Dir, Length, FVec3(Thickness), SQVisitor);
	}
	else
	{
		BVH.Raycast(StartPoint, Dir, Length, SQVisitor);
	}

	if (SQVisitor.OutTime <= Length)
	{
		OutTime = SQVisitor.OutTime;
		OutPosition = SQVisitor.OutPosition;
		OutNormal = SQVisitor.OutNormal;
		OutFaceIndex = SQVisitor.OutFaceIndex;
		return true;
	}
	else
	{
		return false;
	}
}

bool FTriangleMeshImplicitObject::Overlap(const FVec3& Point, const FReal Thickness) const
{
	TAABB<FReal, 3> QueryBounds(Point, Point);
	QueryBounds.Thicken(Thickness);
	const TArray<int32> PotentialIntersections = BVH.FindAllIntersections(QueryBounds);

	const FReal Epsilon = 1e-4;
	//ensure(Thickness > Epsilon);	//There's no hope for this to work unless thickness is large (really a sphere overlap test)
	//todo: turn ensure back on, off until some other bug is fixed

	for (int32 TriIdx : PotentialIntersections)
	{
		const FVec3& A = MParticles.X(MElements[TriIdx][0]);
		const FVec3& B = MParticles.X(MElements[TriIdx][1]);
		const FVec3& C = MParticles.X(MElements[TriIdx][2]);

		const FVec3 AB = B - A;
		const FVec3 AC = C - A;
		FVec3 Normal = FVec3::CrossProduct(AB, AC);
		const FReal NormalLength = Normal.SafeNormalize();
		if (!ensure(NormalLength > Epsilon))
		{
			//hitting degenerate triangle - should be fixed before we get to this stage
			continue;
		}

		const TPlane<FReal, 3> TriPlane{ A, Normal };
		const FVec3 ClosestPointOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, Point);
		const FReal Distance2 = (ClosestPointOnTri - Point).SizeSquared();
		if (Distance2 <= Thickness * Thickness)	//This really only has a hope in working if thickness is > 0
		{
			return true;
		}
	}
	return false;
}

template <typename QueryGeomType>
void TransformVertsHelper(const QueryGeomType& QueryGeom, int32 TriIdx, const TParticles<FReal, 3>& Particles,
	const TArray<TVector<int32, 3>>& Elements, TVec3<FReal>& OutA, TVec3<FReal>& OutB, TVec3<FReal>& OutC)
{
	OutA = Particles.X(Elements[TriIdx][0]);
	OutB = Particles.X(Elements[TriIdx][1]);
	OutC = Particles.X(Elements[TriIdx][2]);
}

template <typename QueryGeomType>
void TransformVertsHelper(const TImplicitObjectScaled<QueryGeomType>& QueryGeom, int32 TriIdx, const TParticles<FReal, 3>& Particles,
	const TArray<TVector<int32, 3>>& Elements, TVec3<FReal>& OutA, TVec3<FReal>& OutB, TVec3<FReal>& OutC)
{
	const TVec3<FReal> InvScale = QueryGeom.GetInvScale();
	OutA = Particles.X(Elements[TriIdx][0]) * InvScale;
	OutB = Particles.X(Elements[TriIdx][1]) * InvScale;
	OutC = Particles.X(Elements[TriIdx][2]) * InvScale;
}

template <typename QueryGeomType>
const QueryGeomType& GetGeomHelper(const QueryGeomType& QueryGeom)
{
	return QueryGeom;
}

template <typename QueryGeomType>
const QueryGeomType& GetGeomHelper(const TImplicitObjectScaled<QueryGeomType>& QueryGeom)
{
	return *QueryGeom.GetUnscaledObject();
}

template <typename QueryGeomType>
void TransformSweepOutputsHelper(const QueryGeomType& QueryGeom, const TVec3<FReal>& HitNormal, const TVec3<FReal>& HitPosition, const FReal LengthScale,
	const FReal Time,  TVec3<FReal>& OutNormal, TVec3<FReal>& OutPosition, FReal& OutTime)
{
	OutNormal = HitNormal;
	OutPosition = HitPosition;
	OutTime = Time;
}

template <typename QueryGeomType>
void TransformSweepOutputsHelper(const TImplicitObjectScaled<QueryGeomType>& QueryGeom, const TVec3<FReal>& HitNormal, const TVec3<FReal>& HitPosition,  const FReal LengthScale,
	const FReal Time,  TVec3<FReal>& OutNormal, TVec3<FReal>& OutPosition, FReal& OutTime)
{
	const TVec3<FReal> InvScale = QueryGeom.GetInvScale();
	const TVec3<FReal> Scale = QueryGeom.GetScale();

	OutTime = Time / LengthScale;
	OutNormal = (InvScale * HitNormal).GetSafeNormal();
	OutPosition = Scale * HitPosition;
}

template <typename QueryGeomType>
void TransformOverlapInputsHelper(const QueryGeomType& QueryGeom, const FRigidTransform3& QueryTM, FRigidTransform3& OutScaledQueryTM)
{
	OutScaledQueryTM = QueryTM;
}

template <typename QueryGeomType>
void TransformOverlapInputsHelper(const TImplicitObjectScaled<QueryGeomType>& QueryGeom, const FRigidTransform3& QueryTM, FRigidTransform3& OutScaledQueryTM)
{
	const TVec3<FReal> InvScale = QueryGeom.GetInvScale();
	OutScaledQueryTM = TRigidTransform<FReal, 3>(QueryTM.GetLocation() * InvScale, QueryTM.GetRotation());
}


template <typename QueryGeomType>
bool FTriangleMeshImplicitObject::OverlapGeomImp(const QueryGeomType& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	bool bResult = false;
	TAABB<FReal, 3> QueryBounds = QueryGeom.BoundingBox();
	QueryBounds.Thicken(Thickness);
	QueryBounds = QueryBounds.TransformedAABB(QueryTM);
	const TArray<int32> PotentialIntersections = BVH.FindAllIntersections(QueryBounds);

	const auto& InnerQueryGeom = GetGeomHelper(QueryGeom);

	TRigidTransform<FReal, 3> TransformedQueryTM;
	TransformOverlapInputsHelper(QueryGeom, QueryTM, TransformedQueryTM);
	
	for (int32 TriIdx : PotentialIntersections)
	{
		TVec3<FReal> A, B, C;
		TransformVertsHelper(QueryGeom, TriIdx, MParticles, MElements, A, B, C);

		const FVec3 AB = B - A;
		const FVec3 AC = C - A;

		//It's most likely that the query object is in front of the triangle since queries tend to be on the outside.
		//However, maybe we should check if it's behind the triangle plane. Also, we should enforce this winding in some way
		const FVec3 Offset = FVec3::CrossProduct(AB, AC);

		if (GJKIntersection(TTriangle<FReal>(A, B, C), InnerQueryGeom, TransformedQueryTM, Thickness, Offset))
		{
			return true;
		}
	}

	return false;
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TSphere<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TCapsule<FReal>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const FConvex& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TImplicitObjectScaled<TSphere<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TImplicitObjectScaled<TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TImplicitObjectScaled<TCapsule<FReal>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}

bool FTriangleMeshImplicitObject::OverlapGeom(const TImplicitObjectScaled<TImplicitObjectScaled<FConvex>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness) const
{
	return OverlapGeomImp(QueryGeom, QueryTM, Thickness);
}


template <typename QueryGeomType>
struct FTriangleMeshSweepVisitor
{
	FTriangleMeshSweepVisitor(const FTriangleMeshImplicitObject& InTriMesh, const QueryGeomType& InQueryGeom, const TRigidTransform<FReal,3>& InStartTM, const FVec3& InDir,
		const FVec3& InScaledDirNormalized, const FReal InLengthScale, const FRigidTransform3& InScaledStartTM, const FReal InThickness, const bool InComputeMTD)
	: TriMesh(InTriMesh)
	, StartTM(InStartTM)
	, QueryGeom(InQueryGeom)
	, Dir(InDir)
	, Thickness(InThickness)
	, bComputeMTD(InComputeMTD)
	, ScaledDirNormalized(InScaledDirNormalized)
	, LengthScale(InLengthScale)
	, ScaledStartTM(InScaledStartTM)
	, OutTime(TNumericLimits<FReal>::Max())
	{
	}

	bool VisitOverlap(const TSpatialVisitorData<int32>& VisitData)
	{
		check(false);
		return true;
	}

	bool VisitRaycast(const TSpatialVisitorData<int32>& VisitData, FQueryFastData& CurData)
	{
		check(false);
		return true;
	}

	bool VisitSweep(const TSpatialVisitorData<int32>& VisitData, FQueryFastData& CurData)
	{
		const int32 TriIdx = VisitData.Payload;

		FReal Time;
		FVec3 HitPosition;
		FVec3 HitNormal;

		FVec3 A, B, C;
		TransformVertsHelper(QueryGeom, TriIdx, TriMesh.MParticles, TriMesh.MElements, A, B, C);
		TTriangle<FReal> Tri(A, B, C);

		const auto& InnerQueryGeom = GetGeomHelper(QueryGeom);

		if(GJKRaycast2<FReal>(Tri, InnerQueryGeom, ScaledStartTM, ScaledDirNormalized, LengthScale * CurData.CurrentLength, Time, HitPosition, HitNormal, Thickness, bComputeMTD))
		{
			if(Time < OutTime)
			{
				TransformSweepOutputsHelper(QueryGeom, HitNormal, HitPosition, LengthScale, Time, OutNormal, OutPosition, OutTime);

				OutFaceIndex = TriIdx;

				if(Time <= 0)	//MTD or initial overlap
				{
					CurData.SetLength(0);

					//initial overlap, no one will beat this
					return false;
				}

				CurData.SetLength(Time);
			}
		}

		return true;
	}

	const FTriangleMeshImplicitObject& TriMesh;
	const FRigidTransform3 StartTM;
	const QueryGeomType& QueryGeom;
	const FVec3& Dir;
	const FReal Thickness;
	const bool bComputeMTD;

	// Cache these values for Scaled Triangle Mesh, as they are needed for transformation when sweeping against triangles.
	FVec3 ScaledDirNormalized;
	FReal LengthScale;
	FRigidTransform3 ScaledStartTM;

	FReal OutTime;
	FVec3 OutPosition;
	FVec3 OutNormal;
	int32 OutFaceIndex;
};

template <typename QueryGeomType>
void ComputeScaledSweepInputs(const QueryGeomType& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length,
	FVec3& OutScaledDirNormalized, FReal& OutLengthScale, FRigidTransform3& OutScaledStartTM)
{
	OutScaledDirNormalized = Dir;
	OutLengthScale = 1.0f;
	OutScaledStartTM = StartTM;
}

template<typename QueryGeomType>
void ComputeScaledSweepInputs(const TImplicitObjectScaled<QueryGeomType>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length,
	FVec3& OutScaledDirNormalized, FReal& OutLengthScale, FRigidTransform3& OutScaledStartTM)
{
	const FVec3& InvScale = QueryGeom.GetInvScale();

	const FVec3 UnscaledDirDenorm = InvScale * Dir;
	const FReal LengthScale = UnscaledDirDenorm.Size();
	if (CHAOS_ENSURE(LengthScale > TNumericLimits<FReal>::Min()))
	{
		const FReal LengthScaleInv = 1.f / LengthScale;
		OutScaledDirNormalized = UnscaledDirDenorm * LengthScaleInv;
	}


	OutLengthScale = LengthScale;
	OutScaledStartTM = FRigidTransform3(StartTM.GetLocation() * InvScale, StartTM.GetRotation());
}

template <typename QueryGeomType>
bool FTriangleMeshImplicitObject::SweepGeomImp(const QueryGeomType& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length,
	FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{

	// Compute scaled sweep inputs to cache in visitor.
	FVec3 ScaledDirNormalized;
	FReal LengthScale;
	FRigidTransform3 ScaledStartTM;
	ComputeScaledSweepInputs(QueryGeom, StartTM, Dir, Length, ScaledDirNormalized, LengthScale, ScaledStartTM);

	bool bHit = false;
	FTriangleMeshSweepVisitor<QueryGeomType> SQVisitor(*this, QueryGeom, StartTM, Dir, ScaledDirNormalized, LengthScale, ScaledStartTM, Thickness, bComputeMTD);


	const TAABB<FReal, 3> QueryBounds = QueryGeom.BoundingBox().TransformedAABB(FRigidTransform3(FVec3::ZeroVector, StartTM.GetRotation()));
	const FVec3 StartPoint = StartTM.TransformPositionNoScale(QueryBounds.Center());
	const FVec3 Inflation = QueryBounds.Extents() * 0.5 + FVec3(Thickness);
	BVH.template Sweep<FTriangleMeshSweepVisitor<QueryGeomType>>(StartPoint, Dir, Length, Inflation, SQVisitor);

	if (SQVisitor.OutTime <= Length)
	{
		OutTime = SQVisitor.OutTime;
		OutPosition = SQVisitor.OutPosition;
		OutNormal = SQVisitor.OutNormal;
		OutFaceIndex = SQVisitor.OutFaceIndex;
		bHit = true;
	}
	return bHit;
}

bool FTriangleMeshImplicitObject::SweepGeom(const TSphere<FReal,3>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TCapsule<FReal>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const FConvex& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TImplicitObjectScaled<TSphere<FReal, 3>>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TImplicitObjectScaled<TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TImplicitObjectScaled<TCapsule<FReal>>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

bool FTriangleMeshImplicitObject::SweepGeom(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, const bool bComputeMTD) const
{
	return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
}

int32 FTriangleMeshImplicitObject::FindMostOpposingFace(const FVec3& Position, const FVec3& UnitDir, int32 HintFaceIndex, FReal SearchDist) const
{
	//todo: this is horribly slow, need adjacency information
	const FReal SearchDist2 = SearchDist * SearchDist;
	
	TAABB<FReal, 3> QueryBounds(Position - FVec3(SearchDist), Position + FVec3(SearchDist));

	const TArray<int32> PotentialIntersections = BVH.FindAllIntersections(QueryBounds);
	const FReal Epsilon = 1e-4;

	FReal MostOpposingDot = TNumericLimits<FReal>::Max();
	int32 MostOpposingFace = HintFaceIndex;
	
	for (int32 TriIdx : PotentialIntersections)
	{
		const FVec3& A = MParticles.X(MElements[TriIdx][0]);
		const FVec3& B = MParticles.X(MElements[TriIdx][1]);
		const FVec3& C = MParticles.X(MElements[TriIdx][2]);

		const FVec3 AB = B - A;
		const FVec3 AC = C - A;
		FVec3 Normal = FVec3::CrossProduct(AB, AC);
		const FReal NormalLength = Normal.SafeNormalize();
		if (!ensure(NormalLength > Epsilon))
		{
			//hitting degenerate triangle - should be fixed before we get to this stage
			continue;
		}

		const TPlane<FReal, 3> TriPlane{ A, Normal };
		const FVec3 ClosestPointOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, Position);
		const FReal Distance2 = (ClosestPointOnTri - Position).SizeSquared();
		if (Distance2 < SearchDist2)
		{
			const FReal Dot = FVec3::DotProduct(Normal, UnitDir);
			if (Dot < MostOpposingDot)
			{
				MostOpposingDot = Dot;
				MostOpposingFace = TriIdx;
			}
		}
	}

	return MostOpposingFace;
}

FVec3 FTriangleMeshImplicitObject::FindGeometryOpposingNormal(const FVec3& DenormDir, int32 FaceIndex, const FVec3& OriginalNormal) const
{
	return GetFaceNormal(FaceIndex);
}

void FTriangleMeshImplicitObject::Serialize(FChaosArchive& Ar)
{
	FChaosArchiveScopedMemory ScopedMemory(Ar, GetTypeName());
	SerializeImp(Ar);
}

uint32 FTriangleMeshImplicitObject::GetTypeHash() const
{
	uint32 Result = MParticles.GetTypeHash();
	Result = HashCombine(Result, MLocalBoundingBox.GetTypeHash());

	for(TVector<int32, 3> Tri : MElements)
	{
		uint32 TriHash = HashCombine(::GetTypeHash(Tri[0]), HashCombine(::GetTypeHash(Tri[1]), ::GetTypeHash(Tri[2])));
		Result = HashCombine(Result, TriHash);
	}

	return Result;
}

FVec3 FTriangleMeshImplicitObject::GetFaceNormal(const int32 FaceIdx) const
{
	if (ensure(FaceIdx != INDEX_NONE))
	{
		const FVec3& A = MParticles.X(MElements[FaceIdx][0]);
		const FVec3& B = MParticles.X(MElements[FaceIdx][1]);
		const FVec3& C = MParticles.X(MElements[FaceIdx][2]);

		const FVec3 AB = B - A;
		const FVec3 AC = C - A;
		FVec3 Normal = FVec3::CrossProduct(AB, AC);
		const FReal Length = Normal.SafeNormalize();
		ensure(Length);
		return Normal;
	}

	return FVec3(0, 0, 1);
}

uint16 FTriangleMeshImplicitObject::GetMaterialIndex(uint32 HintIndex) const
{
	if (MaterialIndices.IsValidIndex(HintIndex))
	{
		return MaterialIndices[HintIndex];
	}

	// 0 should always be the default material for a shape
	return 0;
}

void Chaos::FTriangleMeshImplicitObject::RebuildBV()
{
	const int32 NumTris = MElements.Num();
	BVEntries.Reset(NumTris);

	for (int Tri = 0; Tri<NumTris; Tri++)
	{
		BVEntries.Add({ this, Tri });
	}
	BVH.Reinitialize(BVEntries);

}


}
