// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Collision/CollisionConstraintAllocator.h"
#include "Chaos/Collision/PBDCollisionConstraintHandle.h"
#include "Chaos/Collision/SolverCollisionContainer.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Evolution/SolverBody.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDCollisionConstraints.h"

// Private includes
#include "PBDCollisionSolver.h"


//PRAGMA_DISABLE_OPTIMIZATION

namespace Chaos
{
	FRealSingle Chaos_Manifold_MatchPositionTolerance = 0.3f;		// Fraction of object size position tolerance
	FRealSingle Chaos_Manifold_MatchNormalTolerance = 0.02f;		// Dot product tolerance
	FAutoConsoleVariableRef CVarChaos_Manifold_MatchPositionTolerance(TEXT("p.Chaos.Collision.Manifold.MatchPositionTolerance"), Chaos_Manifold_MatchPositionTolerance, TEXT("A tolerance as a fraction of object size used to determine if two contact points are the same"));
	FAutoConsoleVariableRef CVarChaos_Manifold_MatchNormalTolerance(TEXT("p.Chaos.Collision.Manifold.MatchNormalTolerance"), Chaos_Manifold_MatchNormalTolerance, TEXT("A tolerance on the normal dot product used to determine if two contact points are the same"));

	FRealSingle Chaos_Manifold_FrictionPositionTolerance = 1.0f;	// Distance a shape-relative contact point can move and still be considered the same point
	FAutoConsoleVariableRef CVarChaos_Manifold_FrictionPositionTolerance(TEXT("p.Chaos.Collision.Manifold.FrictionPositionTolerance"), Chaos_Manifold_FrictionPositionTolerance, TEXT(""));

	FRealSingle Chaos_GBFCharacteristicTimeRatio = 1.0f;
	FAutoConsoleVariableRef CVarChaos_GBFCharacteristicTimeRatio(TEXT("p.Chaos.Collision.GBFCharacteristicTimeRatio"), Chaos_GBFCharacteristicTimeRatio, TEXT("The ratio between characteristic time and Dt"));

	bool bChaos_Manifold_EnabledWithJoints = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnabledWithJoints(TEXT("p.Chaos.Collision.Manifold.EnabledWithJoints"), bChaos_Manifold_EnabledWithJoints, TEXT(""));

	bool bChaos_Manifold_EnableGjkWarmStart = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnableGjkWarmStart(TEXT("p.Chaos.Collision.Manifold.EnableGjkWarmStart"), bChaos_Manifold_EnableGjkWarmStart, TEXT(""));

	bool bChaos_Manifold_EnableFrictionRestore = true;
	FAutoConsoleVariableRef CVarChaos_Manifold_EnableFrictionRestore(TEXT("p.Chaos.Collision.Manifold.EnableFrictionRestore"), bChaos_Manifold_EnableFrictionRestore, TEXT(""));
	
	struct FCollisionTolerances
	{
		// Multiplied by the contact margin to produce a distance within which contacts are considered to be the same point
		FReal ContactPositionToleranceScale = FReal(0.8);

		// Multiplied by the contact margin to produce a max distance that a shape can move if we want to reuse contacts
		FReal ShapePositionToleranceScale0 = FReal(0.5);	// 0 contacts
		FReal ShapePositionToleranceScaleN = FReal(0.2);	// >0 contacts

		// A threshold on the quaternion change the tells us when we cannot reuse contacts
		FReal ShapeRotationThreshold0 = FReal(0.9998);		// 0 contacts
		FReal ShapeRotationThresholdN = FReal(0.9999);		// >0 contacts

		// Thresholds used to restore individual manifold points
		FReal ManifoldPointPositionToleranceScale = FReal(1);
		FReal ManifoldPointNormalThreshold  = FReal(0.7);
	};

	// @todo(chaos): put these tolerances on cvars
	// @todo(chaos): tune the tolerances used in FPBDCollisionConstraint::UpdateAndTryRestoreManifold
	FCollisionTolerances Chaos_Manifold_Tolerances;


	extern bool bChaos_Collision_Manifold_FixNormalsInWorldSpace;

	FString FPBDCollisionConstraint::ToString() const
	{
		return FString::Printf(TEXT("Particle:%s, Levelset:%s, AccumulatedImpulse:%s"), *Particle[0]->ToString(), *Particle[1]->ToString(), *AccumulatedImpulse.ToString());
	}

	bool ContactConstraintSortPredicate(const FPBDCollisionConstraint& L, const FPBDCollisionConstraint& R)
	{
		//sort constraints by the smallest particle idx in them first
		//if the smallest particle idx is the same for both, use the other idx

		if (L.GetCCDType() != R.GetCCDType())
		{
			return L.GetCCDType() < R.GetCCDType();
		}

		const FParticleID ParticleIdxs[] = { L.Particle[0]->ParticleID(), L.Particle[1]->ParticleID() };
		const FParticleID OtherParticleIdxs[] = { R.Particle[0]->ParticleID(), R.Particle[1]->ParticleID() };

		const int32 MinIdx = ParticleIdxs[0] < ParticleIdxs[1] ? 0 : 1;
		const int32 OtherMinIdx = OtherParticleIdxs[0] < OtherParticleIdxs[1] ? 0 : 1;

		if(ParticleIdxs[MinIdx] < OtherParticleIdxs[OtherMinIdx])
		{
			return true;
		} 
		else if(ParticleIdxs[MinIdx] == OtherParticleIdxs[OtherMinIdx])
		{
			return ParticleIdxs[!MinIdx] < OtherParticleIdxs[!OtherMinIdx];
		}

		return false;
	}

	TUniquePtr<FPBDCollisionConstraint> FPBDCollisionConstraint::Make(
		FGeometryParticleHandle* Particle0,
		const FImplicitObject* Implicit0,
		const FBVHParticles* Simplicial0,
		const FRigidTransform3& ImplicitLocalTransform0,
		FGeometryParticleHandle* Particle1,
		const FImplicitObject* Implicit1,
		const FBVHParticles* Simplicial1,
		const FRigidTransform3& ImplicitLocalTransform1,
		const FReal InCullDistance,
		const bool bInUseManifold,
		const EContactShapesType ShapesType)
	{
		FPBDCollisionConstraint* Constraint = new FPBDCollisionConstraint(Particle0, Implicit0, Simplicial0, Particle1, Implicit1, Simplicial1);
		
		Constraint->Setup(ECollisionCCDType::Disabled, ShapesType, ImplicitLocalTransform0, ImplicitLocalTransform1, InCullDistance, bInUseManifold);

		return TUniquePtr<FPBDCollisionConstraint>(Constraint);
	}

	FPBDCollisionConstraint FPBDCollisionConstraint::MakeTriangle(const FImplicitObject* Implicit0)
	{
		FPBDCollisionConstraint Constraint;
		Constraint.InitMargins(Implicit0->GetCollisionType(), ImplicitObjectType::Triangle, Implicit0->GetMargin(), FReal(0));
		return Constraint;
	}

	FPBDCollisionConstraint FPBDCollisionConstraint::MakeCopy(
		const FPBDCollisionConstraint& Source)
	{
		// @todo(chaos): The resim cache version probably doesn't need all the data, so maybe try to cut this down?
		FPBDCollisionConstraint Constraint = Source;

		// Invalidate the data that maps the constraint to its container (we are no longer in the container)
		// @todo(chaos): this should probably be handled by the copy constructor
		Constraint.GetContainerCookie().ClearContainerData();

		return Constraint;
	}

	FPBDCollisionConstraint::FPBDCollisionConstraint()
		: ImplicitTransform{ FRigidTransform3(), FRigidTransform3() }
		, Particle{ nullptr, nullptr }
		, AccumulatedImpulse(0)
		, Manifold()
		, TimeOfImpact(0)
		, ContainerCookie()
		, CCDType(ECollisionCCDType::Disabled)
		, Stiffness(FReal(1))
		, ManifoldPoints()
		, SavedManifoldPoints()
		, CullDistance(TNumericLimits<FReal>::Max())
		, CollisionMargins{ 0, 0 }
		, CollisionTolerance(0)
		, Flags()
		, SolverBodies{ nullptr, nullptr }
		, GJKWarmStartData()
		, ShapeWorldTransform0()
		, ShapeWorldTransform1()
		, LastShapeWorldTransform0()
		, LastShapeWorldTransform1()
		, ExpectedNumManifoldPoints(0)
	{
		Manifold.Implicit[0] = nullptr;
		Manifold.Implicit[1] = nullptr;
		Manifold.Simplicial[0] = nullptr;
		Manifold.Simplicial[1] = nullptr;
		Manifold.ShapesType = EContactShapesType::Unknown;
	}

	FPBDCollisionConstraint::FPBDCollisionConstraint(
		FGeometryParticleHandle* Particle0,
		const FImplicitObject* Implicit0,
		const FBVHParticles* Simplicial0,
		FGeometryParticleHandle* Particle1,
		const FImplicitObject* Implicit1,
		const FBVHParticles* Simplicial1)
		: ImplicitTransform{ FRigidTransform3(), FRigidTransform3() }
		, Particle{ Particle0, Particle1 }
		, AccumulatedImpulse(0)
		, Manifold()
		, TimeOfImpact(0)
		, ContainerCookie()
		, CCDType(ECollisionCCDType::Disabled)
		, Stiffness(FReal(1))
		, ManifoldPoints()
		, SavedManifoldPoints()
		, CullDistance(TNumericLimits<FReal>::Max())
		, CollisionMargins{ 0, 0 }
		, CollisionTolerance(0)
		, Flags()
		, SolverBodies{ nullptr, nullptr }
		, GJKWarmStartData()
		, ShapeWorldTransform0()
		, ShapeWorldTransform1()
		, LastShapeWorldTransform0()
		, LastShapeWorldTransform1()
		, ExpectedNumManifoldPoints(0)
	{
		Manifold.Implicit[0] = Implicit0;
		Manifold.Implicit[1] = Implicit1;
		Manifold.Simplicial[0] = Simplicial0;
		Manifold.Simplicial[1] = Simplicial1;
		Manifold.ShapesType = EContactShapesType::Unknown;
	}

	void FPBDCollisionConstraint::Setup(
		const ECollisionCCDType InCCDType,
		const EContactShapesType InShapesType,
		const FRigidTransform3& InImplicitLocalTransform0,
		const FRigidTransform3& InImplicitLocalTransform1,
		const FReal InCullDistance,
		const bool bInUseManifold)
	{
		CCDType = InCCDType;

		Manifold.ShapesType = InShapesType;

		ImplicitTransform[0] = InImplicitLocalTransform0;
		ImplicitTransform[1] = InImplicitLocalTransform1;

		CullDistance = InCullDistance;

		Flags.bUseManifold = bInUseManifold && CanUseManifold(Particle[0], Particle[1]);
		Flags.bUseIncrementalManifold = true;	// This will get changed later if we call AddOneShotManifoldContact

		const FReal Margin0 = GetImplicit0()->GetMargin();
		const FReal Margin1 = GetImplicit1()->GetMargin();
		const EImplicitObjectType ImplicitType0 = GetInnerType(GetImplicit0()->GetCollisionType());
		const EImplicitObjectType ImplicitType1 = GetInnerType(GetImplicit1()->GetCollisionType());
		InitMargins(ImplicitType0, ImplicitType1, Margin0, Margin1);
	}

	void FPBDCollisionConstraint::InitMargins(const EImplicitObjectType ImplicitType0, const EImplicitObjectType ImplicitType1, const FReal Margin0, const FReal Margin1)
	{
		// Set up the margins and tolerances to be used during the narrow phase.
		// One shape in a collision will always have a margin. Only triangles have zero margin and we don't 
		// collide two triangles. If we have a triangle, it is always the second shape.
		// The collision tolerance is used for knowing whether a new contact matches an existing one.
		// If we have two non-quadratic shapes, we use the smallest margin on both shapes.
		// If we have a quadratic shape versus a non-quadratic, we don't need a margin on the non-quadratic.
		// For non-quadratics the collision tolerance is the smallest non-zero margin. 
		// For quadratic shapes we want a collision tolerance much smaller than the radius.
		const bool bIsQuadratic0 = ((ImplicitType0 == ImplicitObjectType::Sphere) || (ImplicitType0 == ImplicitObjectType::Capsule));
		const bool bIsQuadratic1 = ((ImplicitType1 == ImplicitObjectType::Sphere) || (ImplicitType1 == ImplicitObjectType::Capsule));
		const FReal QuadraticToleranceScale = 0.05f;
		if (!bIsQuadratic0 && !bIsQuadratic1)
		{
			CollisionMargins[0] = FMath::Min(Margin0, Margin1);
			CollisionMargins[1] = CollisionMargins[0];
			CollisionTolerance = ((Margin0 < Margin1) || (Margin1 == 0)) ? Margin0 : Margin1;
		}
		else if (bIsQuadratic0 && bIsQuadratic1)
		{
			CollisionMargins[0] = Margin0;
			CollisionMargins[1] = Margin1;
			CollisionTolerance = QuadraticToleranceScale * FMath::Min(Margin0, Margin1);
		}
		else if (bIsQuadratic0 && !bIsQuadratic1)
		{
			CollisionMargins[0] = Margin0;
			CollisionMargins[1] = 0;
			CollisionTolerance = QuadraticToleranceScale * Margin0;
		}
		else if (!bIsQuadratic0 && bIsQuadratic1)
		{
			CollisionMargins[0] = 0;
			CollisionMargins[1] = Margin1;
			CollisionTolerance = QuadraticToleranceScale * Margin1;
		}
	}

	void FPBDCollisionConstraint::SetIsSleeping(const bool bInIsSleeping)
	{
		// This actually sets the sleeping state on all constraints between the same particle pair so calling this with multiple
		// constraints on the same particle pair is a little wasteful. It early-outs on subsequent calls, but still not ideal.
		// @todo(chaos): we only need to set sleeping on particle pairs or particles, not constraints (See UpdateSleepState in IslandManager.cpp)
		check(ContainerCookie.MidPhase != nullptr);
		ContainerCookie.MidPhase->SetIsSleeping(bInIsSleeping);
	}

	// Are the two manifold points the same point?
	// Ideally a contact is considered the same as one from the previous iteration if
	//		The contact is Vertex - Face and there was a prior iteration collision on the same Vertex
	//		The contact is Edge - Edge and a prior iteration collision contained both edges
	//		The contact is Face - Face and a prior iteration contained both faces
	//
	// But we don’t have feature IDs. So in the meantime contact points will be considered the "same" if
	//		Vertex - Face - the local space contact position on either body is within some tolerance
	//		Edge - Edge - ?? hard...
	//		Face - Face - ?? hard...
	//
	bool FPBDCollisionConstraint::AreMatchingContactPoints(const FContactPoint& A, const FContactPoint& B, FReal& OutScore) const
	{
		OutScore = 0.0f;

		// @todo(chaos): cache tolerances?
		FReal DistanceTolerance = 0.0f;
		if (Particle[0]->Geometry()->HasBoundingBox() && Particle[1]->Geometry()->HasBoundingBox())
		{
			const FReal Size0 = Particle[0]->Geometry()->BoundingBox().Extents().Max();
			const FReal Size1 = Particle[1]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = FMath::Min(Size0, Size1) * Chaos_Manifold_MatchPositionTolerance;
		}
		else if (Particle[0]->Geometry()->HasBoundingBox())
		{
			const FReal Size0 = Particle[0]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = Size0 * Chaos_Manifold_MatchPositionTolerance;
		}
		else if (Particle[1]->Geometry()->HasBoundingBox())
		{
			const FReal Size1 = Particle[1]->Geometry()->BoundingBox().Extents().Max();
			DistanceTolerance = Size1 * Chaos_Manifold_MatchPositionTolerance;
		}
		else
		{
			return false;
		}
		const FReal NormalTolerance = Chaos_Manifold_MatchNormalTolerance;

		// If normal has changed a lot, it is a different contact
		// (This was only here to detect bad normals - it is not right for edge-edge contact tracking, but we don't do a good job of that yet anyway!)
		FReal NormalDot = FVec3::DotProduct(A.ShapeContactNormal, B.ShapeContactNormal);
		if (NormalDot < 1.0f - NormalTolerance)
		{
			return false;
		}

		// If either point in local space is the same, it is the same contact
		if (DistanceTolerance > 0.0f)
		{
			const FReal DistanceTolerance2 = DistanceTolerance * DistanceTolerance;
			for (int32 BodyIndex = 0; BodyIndex < 2; ++BodyIndex)
			{
				FVec3 DR = A.ShapeContactPoints[BodyIndex] - B.ShapeContactPoints[BodyIndex];
				FReal DRLen2 = DR.SizeSquared();
				if (DRLen2 < DistanceTolerance2)
				{
					OutScore = FMath::Clamp(1.f - DRLen2 / DistanceTolerance2, 0.f, 1.f);
					return true;
				}
			}
		}

		return false;
	}

	int32 FPBDCollisionConstraint::FindManifoldPoint(const FContactPoint& ContactPoint) const
	{
		const int32 NumManifoldPoints = ManifoldPoints.Num();
		int32 BestMatchIndex = INDEX_NONE;
		FReal BestMatchScore = 0.0f;
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			FReal Score = 0.0f;
			if (AreMatchingContactPoints(ContactPoint, ManifoldPoints[ManifoldPointIndex].ContactPoint, Score))
			{
				if (Score > BestMatchScore)
				{
					BestMatchIndex = ManifoldPointIndex;
					BestMatchScore = Score;

					// Just take the first one that meets the tolerances
					break;
				}
			}
		}
		return BestMatchIndex;
	}

	void FPBDCollisionConstraint::UpdateManifoldContacts()
	{
		// This is only used when calling collision detection in the solver loop, which is only for incremental manifolds.
		// @todo(chaos): Remove this when we don't need to support incremental manifolds (this will only be called on creation/restore)
		if ((GetSolverBody0() != nullptr) && (GetSolverBody1() != nullptr))
		{
			ShapeWorldTransform0 = FRigidTransform3(GetSolverBody0()->CorrectedP(), GetSolverBody0()->CorrectedQ());
			ShapeWorldTransform1 = FRigidTransform3(GetSolverBody1()->CorrectedP(), GetSolverBody1()->CorrectedQ());
		}

		Manifold.Reset();

		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < ManifoldPoints.Num(); ManifoldPointIndex++)
		{
			FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

			UpdateManifoldPointFromContact(ManifoldPointIndex);

			ManifoldPoint.bInsideStaticFrictionCone = Flags.bUseManifold;

			// Copy currently active point
			if (ManifoldPoint.ContactPoint.Phi < Manifold.Phi)
			{
				SetActiveContactPoint(ManifoldPoint.ContactPoint);
			}
		}
	}

	void FPBDCollisionConstraint::AddOneshotManifoldContact(const FContactPoint& ContactPoint)
	{
		if (ContactPoint.IsSet())
		{
			if (ManifoldPoints.IsFull())
			{
				return;
			}
			
			int32 ManifoldPointIndex = AddManifoldPoint(ContactPoint);

			// Copy currently active point
			if (ManifoldPoints[ManifoldPointIndex].ContactPoint.Phi < Manifold.Phi)
			{
				SetActiveContactPoint(ManifoldPoints[ManifoldPointIndex].ContactPoint);
			}

			Flags.bUseIncrementalManifold = false;
		}
	}

	void FPBDCollisionConstraint::AddIncrementalManifoldContact(const FContactPoint& ContactPoint)
	{
		if (ManifoldPoints.IsFull())
		{
			return;
		}

		if (Flags.bUseManifold)
		{
			// See if the manifold point already exists
			int32 ManifoldPointIndex = FindManifoldPoint(ContactPoint);
			if (ManifoldPointIndex >= 0)
			{
				// This contact point is already in the manifold - update the state
				ManifoldPoints[ManifoldPointIndex].ContactPoint = ContactPoint;

				UpdateManifoldPointFromContact(ManifoldPointIndex);
			}
			else
			{
				// This is a new manifold point - capture the state and generate initial properties
				ManifoldPointIndex = AddManifoldPoint(ContactPoint);
			}

			// Copy currently active point
			if (ManifoldPoints[ManifoldPointIndex].ContactPoint.Phi < Manifold.Phi)
			{
				SetActiveContactPoint(ManifoldPoints[ManifoldPointIndex].ContactPoint);
			}
		}
		else 
		{
			// We are not using manifolds - reuse the first and only point
			ManifoldPoints.SetNum(1);
			ManifoldPoints[0].ContactPoint = ContactPoint;

			InitManifoldPoint(0);

			SetActiveContactPoint(ManifoldPoints[0].ContactPoint);
		}

		Flags.bUseIncrementalManifold = true;
	}

	void FPBDCollisionConstraint::InitManifoldPoint(const int32 ManifoldPointIndex)
	{
		if ((Particle[0] == nullptr) || (Particle[1] == nullptr))
		{
			// @todo(chaos): This is just for unit tests testing one-shot manifolds - remove it somehow... 
			// maybe ConstructConvexConvexOneShotManifold should not take a Constraint
			return;
		}

		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
		ManifoldPoint.InitialShapeContactPoints[0] = ManifoldPoint.ContactPoint.ShapeContactPoints[0];
		ManifoldPoint.InitialShapeContactPoints[1] = ManifoldPoint.ContactPoint.ShapeContactPoints[1];

		// Initialize the previous contact transforms if the data is available, otherwise reset them to current
		TryRestoreFrictionData(ManifoldPointIndex);

		// Update the derived contact state (World-Space data)
		UpdateManifoldPointFromContact(ManifoldPointIndex);
	}

	int32 FPBDCollisionConstraint::AddManifoldPoint(const FContactPoint& ContactPoint)
	{
		int32 ManifoldPointIndex = ManifoldPoints.Add();

		ManifoldPoints[ManifoldPointIndex].ContactPoint = ContactPoint;

		InitManifoldPoint(ManifoldPointIndex);

		return ManifoldPointIndex;
	}

	void FPBDCollisionConstraint::UpdateManifoldPointFromContact(const int32 ManifoldPointIndex)
	{
		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
		ManifoldPoint.WorldContactPoints[0] = ShapeWorldTransform0.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
		ManifoldPoint.WorldContactPoints[1] = ShapeWorldTransform1.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
	}

	void FPBDCollisionConstraint::SetActiveContactPoint(const FContactPoint& ContactPoint)
	{
		// @todo(chaos): once we settle on manifolds we should just store the index
		Manifold.Location = ContactPoint.Location;
		Manifold.Normal = ContactPoint.Normal;
		Manifold.Phi = ContactPoint.Phi;
	}

	bool FPBDCollisionConstraint::CanUseManifold(FGeometryParticleHandle* Particle0, FGeometryParticleHandle* Particle1) const
	{
		// Do not use manifolds when a body is connected by a joint to another. Manifolds do not work when the bodies may be moved
		// and rotated by significant amounts and joints can do that.
		return bChaos_Manifold_EnabledWithJoints || ((Particle0->ParticleConstraints().Num() == 0) && (Particle1->ParticleConstraints().Num() == 0));
	}

	void FPBDCollisionConstraint::ResetManifold()
	{
		SavedManifoldPoints.Reset();
		ResetActiveManifoldContacts();
	}

	void FPBDCollisionConstraint::ResetActiveManifoldContacts()
	{
		Manifold.Reset();
		ManifoldPoints.Reset();
		ExpectedNumManifoldPoints = 0;
		Flags.bWasManifoldRestored = false;
	}

	void FPBDCollisionConstraint::RestoreManifold()
	{
		// We want to restore the manifold as-is and will skip the narrow phase, which means we leave the manifold in place, 
		// but we still have some cleanup to do to account for slight movement of the bodies. E.g., we need to update the 
		// world-space state for the contact modifiers
		UpdateManifoldContacts();

		Flags.bWasManifoldRestored = true;
	}


	void FPBDCollisionConstraint::SetShapeWorldTransforms(const FRigidTransform3& InShapeWorldTransform0, const FRigidTransform3& InShapeWorldTransform1)
	{
		ShapeWorldTransform0 = InShapeWorldTransform0;
		ShapeWorldTransform1 = InShapeWorldTransform1;
	}

	void FPBDCollisionConstraint::SetLastShapeWorldTransforms(const FRigidTransform3& InShapeWorldTransform0, const FRigidTransform3& InShapeWorldTransform1)
	{
		LastShapeWorldTransform0 = InShapeWorldTransform0;
		LastShapeWorldTransform1 = InShapeWorldTransform1;
	}

	bool FPBDCollisionConstraint::UpdateAndTryRestoreManifold()
	{
		const FCollisionTolerances& Tolerances = Chaos_Manifold_Tolerances;
		const FReal ContactPositionTolerance = Tolerances.ContactPositionToleranceScale * CollisionTolerance;
		const FReal ShapePositionTolerance = (ManifoldPoints.Num() > 0) ? Tolerances.ShapePositionToleranceScaleN * CollisionTolerance : Tolerances.ShapePositionToleranceScale0 * CollisionTolerance;
		const FReal ShapeRotationThreshold = (ManifoldPoints.Num() > 0) ? Tolerances.ShapeRotationThresholdN : Tolerances.ShapeRotationThreshold0;
		const FReal ContactPositionToleranceSq = FMath::Square(ContactPositionTolerance);

		// Reset current closest point
		Manifold.Reset();

		// How many manifold points we expect. E.g., for Box-box this will be 4 or 1 depending on whether
		// we have a face or edge contact. We don't reuse the manifold if we lose points after culling
		// here and potentially adding the new narrow phase result (See TryAddManifoldContact).
		ExpectedNumManifoldPoints = ManifoldPoints.Num();
		Flags.bWasManifoldRestored = false;

		// Either update or remove each manifold point depending on how far it has moved from its initial relative point
		// NOTE: We do not reset if we have 0 points - we can still "restore" a zero point manifold if the bodies have not moved
		int32 ManifoldPointToRemove = INDEX_NONE;
		if (ManifoldPoints.Num() > 0)
		{
			const FRigidTransform3 Shape0ToShape1Transform = ShapeWorldTransform0.GetRelativeTransformNoScale(ShapeWorldTransform1);
			
			// Update or prune manifold points. If we would end up removing more than 1 point, we just throw the 
			// whole manifold away because it will get rebuilt in the narrow phasee anyway.
			for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < ManifoldPoints.Num(); ++ManifoldPointIndex)
			{
				FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

				// Calculate the world-space contact location and separation at the current shape transforms
				// @todo(chaos): this should use the normal owner. Currently we assume body 1 is the owner
				const FVec3 Contact0In1 = Shape0ToShape1Transform.TransformPositionNoScale(ManifoldPoint.InitialShapeContactPoints[0]);
				const FVec3& Contact1In1 = ManifoldPoint.InitialShapeContactPoints[1];
				const FVec3 ContactNormalIn1 = ShapeWorldTransform1.InverseTransformVectorNoScale(ManifoldPoint.ContactPoint.Normal);

				const FVec3 ContactDeltaIn1 = Contact0In1 - Contact1In1;
				const FReal ContactPhi = FVec3::DotProduct(ContactDeltaIn1, ContactNormalIn1);
				const FVec3 ContactLateralDeltaIn1 = ContactDeltaIn1 - ContactPhi * ContactNormalIn1;
				const FReal ContactLateralDistanceSq = ContactLateralDeltaIn1.SizeSquared();

				// Either update the point or flag it for removal
				if (ContactLateralDistanceSq < ContactPositionToleranceSq)
				{
					// Recalculate the contact points at the new location
					// @todo(chaos): we should reproject the contact on the plane owner
					const FVec3 ShapeContactPoint1 = Contact0In1 - ContactPhi * ContactNormalIn1;
					ManifoldPoint.ContactPoint.ShapeContactPoints[1] = ShapeContactPoint1;
					ManifoldPoint.ContactPoint.Phi = ContactPhi;
				}
				else if (ManifoldPointToRemove == INDEX_NONE)
				{
					ManifoldPointToRemove = ManifoldPointIndex;
				}
				else
				{
					// We want to remove a second point, but we will never reuse it now so throw away the whole manifold
					ResetActiveManifoldContacts();
					return false;
				}
			}

			// Remove points - only one point removal suport required (see above)
			if (ManifoldPointToRemove != INDEX_NONE)
			{
				ManifoldPoints.RemoveAt(ManifoldPointToRemove);
			}

			// Update world-space state for the points we kept
			for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < ManifoldPoints.Num(); ++ManifoldPointIndex)
			{
				FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

				// Restore friction anchors if we have them for this point
				TryRestoreFrictionData(ManifoldPointIndex);

				// Update world-space contact locations
				UpdateManifoldPointFromContact(ManifoldPointIndex);
				ManifoldPoint.ContactPoint.Location = FReal(0.5) * (ManifoldPoint.WorldContactPoints[0] + ManifoldPoint.WorldContactPoints[1]);

				ManifoldPoint.bWasRestored = true;

				if (ManifoldPoint.ContactPoint.Phi < Manifold.Phi)
				{
					// Update closest point
					SetActiveContactPoint(ManifoldPoint.ContactPoint);
				}
			}
		}

		// If we did not remove any contact points and we have not moved or rotated much we may reuse the manifold as-is.
		if ((ManifoldPointToRemove == INDEX_NONE) && (ShapePositionTolerance > 0) && (ShapeRotationThreshold > 0))
		{
			// The transform check is necessary regardless of how many points we have left in the manifold because
			// as a body moves/rotates we may have to change which faces/edges are colliding. We can't know if the face/edge
			// will change until we run the closest-point checks (GJK) in the narrow phase.
			const FVec3 Shape1ToShape0Translation = ShapeWorldTransform0.GetTranslation() - ShapeWorldTransform1.GetTranslation();
			const FVec3 OriginalShape1ToShape0Translation = LastShapeWorldTransform0.GetTranslation() - LastShapeWorldTransform1.GetTranslation();
			const FVec3 TranslationDelta = Shape1ToShape0Translation - OriginalShape1ToShape0Translation;
			if (TranslationDelta.IsNearlyZero(ShapePositionTolerance))
			{
				const FRotation3 Shape1toShape0Rotation = ShapeWorldTransform0.GetRotation().Inverse() * ShapeWorldTransform1.GetRotation();
				const FRotation3 OriginalShape1toShape0Rotation = LastShapeWorldTransform0.GetRotation().Inverse() * LastShapeWorldTransform1.GetRotation();
				const FReal RotationOverlap = FRotation3::DotProduct(Shape1toShape0Rotation, OriginalShape1toShape0Rotation);
				if (RotationOverlap > ShapeRotationThreshold)
				{
					return true;
				}
			}
		}

		return false;
	}

	bool FPBDCollisionConstraint::TryAddManifoldContact(const FContactPoint& NewContactPoint)
	{
		const FCollisionTolerances& Tolerances = Chaos_Manifold_Tolerances;
		const FReal PositionTolerance = Tolerances.ManifoldPointPositionToleranceScale * CollisionTolerance;
		const FReal NormalThreshold = Tolerances.ManifoldPointNormalThreshold;

		// We must end up with a full manifold after this if we want to reuse it
		if ((ManifoldPoints.Num() < ExpectedNumManifoldPoints - 1) || (ExpectedNumManifoldPoints == 0))
		{
			// We need to add more than 1 point to restore the manifold so we must rebuild it from scratch
			return false;
		}

		// Find the matching manifold point if it exists and replace it
		// Also check to see if the normal has changed significantly and if it has force manifold regeneration
		// NOTE: the normal rejection check assumes all contacts have the same normal - this may not always be true. The worst
		// case here is that we will regenerate the manifold too often so it will work but could be bad for perf
		const FReal PositionToleranceSq = FMath::Square(PositionTolerance);
		int32 MatchedManifoldPointIndex = INDEX_NONE;
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < ManifoldPoints.Num(); ++ManifoldPointIndex)
		{
			FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

			const FReal NormalOverlap = FVec3::DotProduct(ManifoldPoint.ContactPoint.Normal, NewContactPoint.Normal);
			if (NormalOverlap < NormalThreshold)
			{
				return false;
			}

			const FVec3 DR0 = ManifoldPoint.ContactPoint.ShapeContactPoints[0] - NewContactPoint.ShapeContactPoints[0];
			const FVec3 DR1 = ManifoldPoint.ContactPoint.ShapeContactPoints[1] - NewContactPoint.ShapeContactPoints[1];
			if ((DR0.SizeSquared() < PositionToleranceSq) && (DR1.SizeSquared() < PositionToleranceSq))
			{
				// If we should replace a point but will then have too few points we abort
				if (ManifoldPoints.Num() < ExpectedNumManifoldPoints)
				{
					return false;
				}

				// If the existing point has a deeper penetration, just re-use it. This is common when we have a GJK
				// result on an edge or corner - the contact created when generating the manifold is on the
				// surface shape rather than the rounded (margin-reduced) shape.
				if (ManifoldPoint.ContactPoint.Phi > NewContactPoint.Phi)
				{
					ManifoldPoint.ContactPoint = NewContactPoint;
					ManifoldPoint.InitialShapeContactPoints[0] = NewContactPoint.ShapeContactPoints[0];
					ManifoldPoint.InitialShapeContactPoints[1] = NewContactPoint.ShapeContactPoints[1];
					ManifoldPoint.bWasRestored = false;
					TryRestoreFrictionData(ManifoldPointIndex);
					UpdateManifoldPointFromContact(ManifoldPointIndex);
					if (NewContactPoint.Phi < GetPhi())
					{
						SetActiveContactPoint(ManifoldPoint.ContactPoint);
					}
				}

				return true;
			}
		}

		// If we have a full manifold, see if we can use or reject the GJK point
		if (ManifoldPoints.Num() == 4)
		{
			return TryInsertManifoldContact(NewContactPoint);
		}
		
		return false;
	}

	bool FPBDCollisionConstraint::TryInsertManifoldContact(const FContactPoint& NewContactPoint)
	{
		check(ManifoldPoints.Num() == 4);

		const int32 NormalBodyIndex = 1;
		constexpr int32 NumContactPoints = 5;
		constexpr int32 NumManifoldPoints = 4;

		// We want to select 4 points from the 5 we have
		// Create a working set of points, and keep track which points have been selected
		FVec3 ContactPoints[NumContactPoints];
		FReal ContactPhis[NumContactPoints];
		bool bContactSelected[NumContactPoints];
		int32 SelectedContactIndices[NumManifoldPoints];
		for (int32 ContactIndex = 0; ContactIndex < NumManifoldPoints; ++ContactIndex)
		{
			const FManifoldPoint& ManifoldPoint = ManifoldPoints[ContactIndex];
			ContactPoints[ContactIndex] = ManifoldPoint.ContactPoint.ShapeContactPoints[NormalBodyIndex];
			ContactPhis[ContactIndex] = ManifoldPoint.ContactPoint.Phi;
			bContactSelected[ContactIndex] = false;
		}
		ContactPoints[4] = NewContactPoint.ShapeContactPoints[NormalBodyIndex];
		ContactPhis[4] = NewContactPoint.Phi;
		bContactSelected[4] = false;

		// We are projecting points into a plane perpendicular to the contact normal, which we assume is the new point's normal
		const FVec3 ContactNormal = NewContactPoint.ShapeContactNormal;

		// Start with the deepest point. This may not be point 4 despite that being the result of
		// collision detection because for some shape types we use margin-reduced core shapes which
		// are effectively rounded at the corners. But...when building a one-shot manifold we 
		// use the outer shape to get sharp corners. So, if we have a GJK result from a "corner"
		// the real corner (if it is in the manifold) may actually be deeper than the GJK result.
		SelectedContactIndices[0] = 0;
		for (int32 ContactIndex = 1; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (ContactPhis[ContactIndex] < ContactPhis[SelectedContactIndices[0]])
			{
				SelectedContactIndices[0] = ContactIndex;
			}
		}
		bContactSelected[SelectedContactIndices[0]] = true;

		// The second point will be the one farthest from the first
		SelectedContactIndices[1] = INDEX_NONE;
		FReal MaxDistanceSq = TNumericLimits<FReal>::Lowest();
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				const FReal DistanceSq = (ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[0]]).SizeSquared();
				if (DistanceSq > MaxDistanceSq)
				{
					SelectedContactIndices[1] = ContactIndex;
					MaxDistanceSq = DistanceSq;
				}
			}
		}
		check(SelectedContactIndices[1] != INDEX_NONE);
		bContactSelected[SelectedContactIndices[1]] = true;

		// The third point is the one which gives us the largest triangle (projected onto a plane perpendicular to the normal)
		SelectedContactIndices[2] = INDEX_NONE;
		FReal MaxTriangleArea = 0;
		FReal WindingOrder = FReal(1.0);
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				const FVec3 Cross = FVec3::CrossProduct(ContactPoints[SelectedContactIndices[1]] - ContactPoints[SelectedContactIndices[0]], ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[1]]);
				const FReal SignedArea = FVec3::DotProduct(Cross, ContactNormal);
				if (FMath::Abs(SignedArea) > MaxTriangleArea)
				{
					SelectedContactIndices[2] = ContactIndex;
					MaxTriangleArea = FMath::Abs(SignedArea);
					WindingOrder = FMath::Sign(SignedArea);
				}
			}
		}
		if (SelectedContactIndices[2] == INDEX_NONE)
		{
			// Degenerate points - all 4 exactly in a line
			return false;
		}
		bContactSelected[SelectedContactIndices[2]] = true;

		// The fourth point is the one which adds the most area to the 3 points we already have
		SelectedContactIndices[3] = INDEX_NONE;
		FReal MaxQuadArea = 0;	// Additional area to MaxTriangleArea
		for (int32 ContactIndex = 0; ContactIndex < NumContactPoints; ++ContactIndex)
		{
			if (!bContactSelected[ContactIndex])
			{
				// Calculate the area that is added by inserting the point into each edge of the selected triangle
				// The signed area will be negative for interior points, positive for points that extend the triangle into a quad.
				const FVec3 Cross0 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[0]], ContactPoints[SelectedContactIndices[1]] - ContactPoints[ContactIndex]);
				const FReal SignedArea0 = WindingOrder * FVec3::DotProduct(Cross0, ContactNormal);
				const FVec3 Cross1 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[1]], ContactPoints[SelectedContactIndices[2]] - ContactPoints[ContactIndex]);
				const FReal SignedArea1 = WindingOrder * FVec3::DotProduct(Cross1, ContactNormal);
				const FVec3 Cross2 = FVec3::CrossProduct(ContactPoints[ContactIndex] - ContactPoints[SelectedContactIndices[2]], ContactPoints[SelectedContactIndices[0]] - ContactPoints[ContactIndex]);
				const FReal SignedArea2 = WindingOrder * FVec3::DotProduct(Cross2, ContactNormal);
				const FReal SignedArea = FMath::Max3(SignedArea0, SignedArea1, SignedArea2);
				if (SignedArea > MaxQuadArea)
				{
					SelectedContactIndices[3] = ContactIndex;
					MaxQuadArea = SignedArea;
				}
			}
		}
		if (SelectedContactIndices[3] == INDEX_NONE)
		{
			// No point is outside the triangle we already have
			return false;
		}
		bContactSelected[SelectedContactIndices[3]] = true;

		// Now we should have exactly 4 selected contacts. If we find that one of the existing points is not
		// selected, it must be because it is being replaced by the new contact. Otherwise the new contact
		// is interior to the existing manifiold and is rejected.
		for (int32 ManifoldPointIndex = 0; ManifoldPointIndex < NumManifoldPoints; ++ManifoldPointIndex)
		{
			if (!bContactSelected[ManifoldPointIndex])
			{
				ManifoldPoints[ManifoldPointIndex].ContactPoint = NewContactPoint;
				ManifoldPoints[ManifoldPointIndex].InitialShapeContactPoints[0] = NewContactPoint.ShapeContactPoints[0];
				ManifoldPoints[ManifoldPointIndex].InitialShapeContactPoints[1] = NewContactPoint.ShapeContactPoints[1];
				ManifoldPoints[ManifoldPointIndex].bWasRestored = false;
				UpdateManifoldPointFromContact(ManifoldPointIndex);
				if (NewContactPoint.Phi < Manifold.Phi)
				{
					SetActiveContactPoint(NewContactPoint);
				}
			}
		}

		return true;
	}

	const FManifoldPointSavedData* FPBDCollisionConstraint::FindManifoldPointSavedData(const FManifoldPoint& ManifoldPoint) const
	{
		if (bChaos_Manifold_EnableFrictionRestore)
		{
			const FReal DistanceToleranceSq = FMath::Square(Chaos_Manifold_FrictionPositionTolerance);
			for (int32 SavedManifoldPointIndex = 0; SavedManifoldPointIndex < SavedManifoldPoints.Num(); ++SavedManifoldPointIndex)
			{
				const FManifoldPointSavedData& SavedManifoldPoint = SavedManifoldPoints[SavedManifoldPointIndex];
				if (SavedManifoldPoint.IsMatch(ManifoldPoint, DistanceToleranceSq))
				{
					return &SavedManifoldPoint;
				}
			}
		}
		return nullptr;
	}

	void FPBDCollisionConstraint::TryRestoreFrictionData(const int32 ManifoldPointIndex)
	{
		FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

		// Assume we have no matching point from the previous tick, but that we can retain friction from now on
		// Not supported for non-manifolds yet (hopefully we don't need to)
		ManifoldPoint.bInsideStaticFrictionCone = Flags.bUseManifold;
		ManifoldPoint.StaticFrictionMax = FReal(0);

		// Find the previous manifold point that matches if there is one
		const FManifoldPointSavedData* PrevManifoldPoint = FindManifoldPointSavedData(ManifoldPoint);
		if (PrevManifoldPoint != nullptr)
		{
			// We have data from the previous tick and static friction was enabled - restore the data
			PrevManifoldPoint->Restore(ManifoldPoint);
		}
	}

	ECollisionConstraintDirection FPBDCollisionConstraint::GetConstraintDirection(const FReal Dt) const
	{
		if (GetDisabled())
		{
			return NoRestingDependency;
		}
		// D\tau is the chacteristic time (as in GBF paper Sec 8.1)
		const FReal Dtau = Dt * Chaos_GBFCharacteristicTimeRatio; 

		const FVec3 Normal = GetNormal();
		const FReal Phi = GetPhi();
		if (GetPhi() >= GetCullDistance())
		{
			return NoRestingDependency;
		}

		FVec3 GravityDirection = ConcreteContainer()->GetGravityDirection();
		FReal GravitySize = ConcreteContainer()->GetGravitySize();
		// When gravity is zero, we still want to sort the constraints instead of having a random order. In this case, set gravity to default gravity.
		if (GravitySize < SMALL_NUMBER)
		{
			GravityDirection = FVec3(0, 0, -1);
			GravitySize = 980.f;
		}

		// How far an object travels in gravity direction within time Dtau starting with zero velocity (as in GBF paper Sec 8.1). 
		// Theoretically this should be 0.5 * GravityMagnitude * Dtau * Dtau.
		// Omitting 0.5 to be more consistent with our integration scheme.
		// Multiplying 0.5 can alternatively be achieved by setting Chaos_GBFCharacteristicTimeRatio=sqrt(0.5)
		const FReal StepSize = GravitySize * Dtau * Dtau; 
		const FReal NormalDotG = FVec3::DotProduct(Normal, GravityDirection);
		const FReal NormalDirectionThreshold = 0.1f; // Hack
		if (NormalDotG < -NormalDirectionThreshold) // Object 0 rests on object 1
		{
			if (Phi + NormalDotG * StepSize < 0) // Hack to simulate object 0 falling (as in GBF paper Sec 8.1)
			{
				return Particle1ToParticle0;
			}
			else
			{
				return NoRestingDependency;
			}
		}
		else if (NormalDotG > NormalDirectionThreshold) // Object 1 rests on object 0
		{
			if (Phi - NormalDotG * StepSize < 0) // Hack to simulate object 1 falling (as in GBF paper Sec 8.1)
			{
				return Particle0ToParticle1;
			}
			else
			{
				return NoRestingDependency;
			}
		}
		else // Horizontal contact
		{
			return NoRestingDependency;
		}
	}
}