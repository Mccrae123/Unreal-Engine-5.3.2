// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Collision/SolverCollisionContainer.h"

#include "Chaos/Collision/CollisionApplyType.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDCollisionConstraintsContact.h"
#include "Chaos/Utilities.h"

// Private includes
#include "PBDCollisionSolver.h"



namespace Chaos
{
	namespace CVars
	{
		extern int32 Chaos_PBDCollisionSolver_Position_ZeroFrictionIterations;
		extern int32 Chaos_PBDCollisionSolver_Position_ShockPropagationIterations;

		extern int32 Chaos_PBDCollisionSolver_Velocity_ShockPropagationIterations;

		extern bool bChaos_PBDCollisionSolver_Position_SolveEnabled;
		extern bool bChaos_PBDCollisionSolver_Velocity_SolveEnabled;
	}
	using namespace CVars;

	extern bool bChaos_Collision_Manifold_FixNormalsInWorldSpace;


	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////


	/**
	 * @brief A wrapper for FPBDCollisionSolver which binds to a Collision Constraint and adds Gather/Scatter from/to the constraint
	*/
	class FPBDCollisionSolverAdapter
	{
	public:
		FPBDCollisionSolverAdapter()
			: Constraint(nullptr)
			, bIsIncremental(false)
		{
		}

		FPBDCollisionSolver& GetSolver() { return Solver; }
		FPBDCollisionConstraint* GetConstraint() { return Constraint; }

		bool IsIncrementalManifold() const { return bIsIncremental; }

		/**
		 * @brief Initialize the data required for the solver, and bind to the bodies
		*/
		void GatherInput(
			const FReal Dt,
			FPBDCollisionConstraint& InConstraint,
			const int32 Particle0Level,
			const int32 Particle1Level,
			FSolverBodyContainer& SolverBodyContainer)
		{
			Constraint = &InConstraint;
			bIsIncremental = Constraint->GetUseIncrementalCollisionDetection();

			// Find the solver bodies for the particles we constrain. This will add them to the container
			// if they aren't there already, and ensure that they are populated with the latest data.
			FSolverBody* Body0 = SolverBodyContainer.FindOrAdd(Constraint->GetParticle0());
			FSolverBody* Body1 = SolverBodyContainer.FindOrAdd(Constraint->GetParticle1());

			Body0->SetLevel(Particle0Level);
			Body1->SetLevel(Particle1Level);

			// Friction values. Static and Dynamic friction are applied in the position solve for most shapes.
			// For quadratic shapes, we run dynamic friction in the velocity solve for better rolling behaviour.
			// @todo(chaos): fix static/dynamic friction for quadratic shapes
			const FSolverReal StaticFriction = FSolverReal(Constraint->GetStaticFriction());
			const FSolverReal DynamicFriction = FSolverReal(Constraint->GetDynamicFriction());
			const bool bIsQuadratic = Constraint->HasQuadraticShape();
			if (!bIsQuadratic)
			{
				Solver.SetFriction(StaticFriction, DynamicFriction, FSolverReal(0));
			}
			else
			{
				Solver.SetFriction(StaticFriction, FReal(0), DynamicFriction);
			}

			Solver.SetStiffness(FSolverReal(Constraint->GetStiffness()));

			Solver.SetSolverBodies(Body0, Body1);

			GatherManifoldPoints(Dt, Body0, Body1);

			// We should try to remove this - the Constraint should not need to know about solver objects
			Constraint->SetSolverBodies(Body0, Body1);
		}

		void GatherManifoldPoints(
			const FReal InDt,
			FSolverBody* Body0,
			FSolverBody* Body1)
		{
			FSolverReal Dt = FSolverReal(InDt);

			// We handle incremental manifolds by just collecting any new contacts
			const int32 BeginPointIndex = Solver.NumManifoldPoints();
			const int32 EndPointIndex = Solver.SetNumManifoldPoints(Constraint->GetManifoldPoints().Num());

			const FSolverReal RestitutionVelocityThreshold = FSolverReal(Constraint->GetRestitutionThreshold()) * Dt;
			const FSolverReal Restitution = FSolverReal(Constraint->GetRestitution());

			const FRigidTransform3& ShapeWorldTransform0 = Constraint->GetShapeWorldTransform0();
			const FRigidTransform3& ShapeWorldTransform1 = Constraint->GetShapeWorldTransform1();

			for (int32 ManifoldPointIndex = BeginPointIndex; ManifoldPointIndex < EndPointIndex; ++ManifoldPointIndex)
			{
				TArrayView<FManifoldPoint> ManifoldPoints = Constraint->GetManifoldPoints();
				FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

				const FVec3 WorldContactPoint0 = ShapeWorldTransform0.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
				const FVec3 WorldContactPoint1 = ShapeWorldTransform1.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
				const FVec3 WorldContactPoint = FReal(0.5) * (WorldContactPoint0 + WorldContactPoint1);

				const FSolverVec3 WorldContactNormal = FSolverVec3(ShapeWorldTransform1.TransformVectorNoScale(ManifoldPoint.ContactPoint.ShapeContactNormal));
				const FSolverVec3 RelativeContactPosition0 = FSolverVec3(WorldContactPoint - Body0->P());
				const FSolverVec3 RelativeContactPosition1 = FSolverVec3(WorldContactPoint - Body1->P());

				// If we have contact data from a previous tick, use it to calculate the lateral position delta we need
				// to apply to move the contacts back to their original relative locations (i.e., to enforce static friction)
				// @todo(chaos): we should not be writing back to the constraint here - find a better way to update the friction anchor. See FPBDCollisionConstraint::SetSolverResults
				FSolverVec3 WorldFrictionDelta = FVec3(0);
				const FSavedManifoldPoint* SavedManifoldPoint = Constraint->FindSavedManifoldPoint(ManifoldPoint);
				if (SavedManifoldPoint != nullptr)
				{
					const FSolverVec3 FrictionDelta0 = FSolverVec3(SavedManifoldPoint->ShapeContactPoints[0] - ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
					const FSolverVec3 FrictionDelta1 = FSolverVec3(SavedManifoldPoint->ShapeContactPoints[1] - ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
					WorldFrictionDelta = ShapeWorldTransform0.TransformVectorNoScale(FrictionDelta0) - ShapeWorldTransform1.TransformVectorNoScale(FrictionDelta1);

					ManifoldPoint.ShapeAnchorPoints[0] = SavedManifoldPoint->ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = SavedManifoldPoint->ShapeContactPoints[1];
				}
				else
				{
					const FSolverVec3 ContactVel0 = Body0->V() + FSolverVec3::CrossProduct(Body0->W(), RelativeContactPosition0);
					const FSolverVec3 ContactVel1 = Body1->V() + FSolverVec3::CrossProduct(Body1->W(), RelativeContactPosition1);
					const FSolverVec3 ContactVel = ContactVel0 - ContactVel1;
					WorldFrictionDelta = ContactVel * Dt;

					ManifoldPoint.ShapeAnchorPoints[0] = ManifoldPoint.ContactPoint.ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = ManifoldPoint.ContactPoint.ShapeContactPoints[1];
				}

				// World-space contact tangents. We are treating the normal as the constraint-space Z axis
				// and the Tangent U and V as the constraint-space X and Y axes respectively
				FSolverVec3 WorldContactTangentU = FSolverVec3::CrossProduct(FSolverVec3(0, 1, 0), WorldContactNormal);
				if (!WorldContactTangentU.Normalize(FSolverReal(KINDA_SMALL_NUMBER)))
				{
					WorldContactTangentU = FSolverVec3::CrossProduct(FSolverVec3(1, 0, 0), WorldContactNormal);
					WorldContactTangentU = WorldContactTangentU.GetUnsafeNormal();
				}
				const FSolverVec3 WorldContactTangentV = FSolverVec3::CrossProduct(WorldContactNormal, WorldContactTangentU);

				// The contact point error we are trying to correct in this solver
				const FSolverVec3 WorldContactDelta = FSolverVec3(WorldContactPoint0 - WorldContactPoint1);
				const FSolverReal WorldContactDeltaNormal = FSolverVec3::DotProduct(WorldContactDelta, WorldContactNormal);
				const FSolverReal WorldContactDeltaTangentU = FSolverVec3::DotProduct(WorldContactDelta + WorldFrictionDelta, WorldContactTangentU);
				const FSolverReal WorldContactDeltaTangentV = FSolverVec3::DotProduct(WorldContactDelta + WorldFrictionDelta, WorldContactTangentV);

				// Initialize the structural data in the contact (relative contact points, contact mass etc)
				Solver.InitContact(
					ManifoldPointIndex,
					Dt,
					RelativeContactPosition0,
					RelativeContactPosition1,
					WorldContactNormal,
					WorldContactTangentU,
					WorldContactTangentV,
					WorldContactDeltaNormal,
					WorldContactDeltaTangentU,
					WorldContactDeltaTangentV);

				// Initialize the material properties (restitution and friction related)
				Solver.InitMaterial(
					ManifoldPointIndex,
					Restitution,
					RestitutionVelocityThreshold);
			}
		}

		/**
		 * @brief Send all solver results to the constraint
		*/
		void ScatterOutput(const FReal Dt)
		{
			FVec3 AccumulatedImpulse = FVec3(0);

			Constraint->ResetSolverResults();

			for (int32 PointIndex = 0; PointIndex < Solver.NumManifoldPoints(); ++PointIndex)
			{
				const FPBDCollisionSolverManifoldPoint& SolverManifoldPoint = Solver.GetManifoldPoint(PointIndex);

				const FSolverVec3 NetPushOut = 
					SolverManifoldPoint.NetPushOutNormal * SolverManifoldPoint.WorldContactNormal +
					SolverManifoldPoint.NetPushOutTangentU * SolverManifoldPoint.WorldContactTangentU +
					SolverManifoldPoint.NetPushOutTangentV * SolverManifoldPoint.WorldContactTangentV;

				const FSolverVec3 NetImpulse =
					SolverManifoldPoint.NetImpulseNormal * SolverManifoldPoint.WorldContactNormal +
					SolverManifoldPoint.NetImpulseTangentU * SolverManifoldPoint.WorldContactTangentU +
					SolverManifoldPoint.NetImpulseTangentV * SolverManifoldPoint.WorldContactTangentV;

				Constraint->SetSolverResults(PointIndex, 
					NetPushOut, 
					NetImpulse, 
					SolverManifoldPoint.StaticFrictionRatio,
					Dt);
			}

			Constraint->SetSolverBodies(nullptr, nullptr);
			Constraint = nullptr;
			Solver.ResetSolverBodies();
		}


	private:
		FPBDCollisionSolver Solver;
		FPBDCollisionConstraint* Constraint;
		bool bIsIncremental;
	};


	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////


	FPBDCollisionSolverContainer::FPBDCollisionSolverContainer()
		: FConstraintSolverContainer()
		, MaxPushOutVelocity(0)
		, bRequiresIncrementalCollisionDetection(false)
	{
	}

	FPBDCollisionSolverContainer::~FPBDCollisionSolverContainer()
	{
	}

	void FPBDCollisionSolverContainer::Reset(const int32 MaxCollisions)
	{
		CollisionSolvers.Reset(MaxCollisions);
	}
	
	void FPBDCollisionSolverContainer::SetNum(const int32 MaxCollisions)
	{
		CollisionSolvers.SetNum(MaxCollisions, false);
	}

	void FPBDCollisionSolverContainer::AddConstraintSolver(FReal Dt, FPBDCollisionConstraint& Constraint, const int32 Particle0Level, const int32 Particle1Level, FSolverBodyContainer& SolverBodyContainer, int32& ConstraintIndex)
	{
		// This container is required to allocate pointers that are valid for the whole tick,
		// so we cannot allow the container to resize during the tick. See Reset()
		check(ConstraintIndex < CollisionSolvers.Num());

		auto& CollisionSolver = CollisionSolvers[ConstraintIndex];
		CollisionSolver.GetSolver().Reset();

		CollisionSolver.GatherInput(Dt, Constraint, Particle0Level, Particle1Level, SolverBodyContainer);

		bRequiresIncrementalCollisionDetection |= CollisionSolver.IsIncrementalManifold();
		++ConstraintIndex;
	}

	void FPBDCollisionSolverContainer::UpdatePositionShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		// If this is the first shock propagation iteration, enable it on each solver
		const int32 NumShockPropIterations = Chaos_PBDCollisionSolver_Position_ShockPropagationIterations;
		const bool bEnableShockPropagation = (It == NumIts - NumShockPropIterations);
		if (bEnableShockPropagation)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().EnablePositionShockPropagation();
			}
		}
	}

	void FPBDCollisionSolverContainer::UpdateVelocityShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		// Set/reset the shock propagation based on current iteration. The position solve may
		// have left the bodies with a mass scale and we want to change or reset it.
		const int32 NumShockPropIterations = Chaos_PBDCollisionSolver_Velocity_ShockPropagationIterations;
		const bool bEnableShockPropagation = (It == NumIts - NumShockPropIterations);
		if (bEnableShockPropagation)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().EnableVelocityShockPropagation();
			}
		}
		else if (It == 0)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().DisableShockPropagation();
			}
		}
	}

	bool FPBDCollisionSolverContainer::SolvePositionSerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		return SolvePositionImpl(Dt, It, NumIts, BeginIndex, EndIndex, false);
	}

	bool FPBDCollisionSolverContainer::SolveVelocitySerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		return SolveVelocityImpl(Dt, It, NumIts, BeginIndex, EndIndex, false);
	}

	bool FPBDCollisionSolverContainer::SolvePositionParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		return SolvePositionImpl(Dt, It, NumIts, BeginIndex, EndIndex, true);
	}

	bool FPBDCollisionSolverContainer::SolveVelocityParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex)
	{
		return SolveVelocityImpl(Dt, It, NumIts, BeginIndex, EndIndex, true);
	}

	// @todo(chaos): parallel version of SolvePosition
	bool FPBDCollisionSolverContainer::SolvePositionImpl(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_Apply);
		if (!bChaos_PBDCollisionSolver_Position_SolveEnabled)
		{
			return false;
		}

		UpdatePositionShockPropagation(Dt, It, NumIts, BeginIndex, EndIndex);

		const int32 NumZeroFrictionIterations = Chaos_PBDCollisionSolver_Position_ZeroFrictionIterations;
		const bool bApplyStaticFriction = (It >= NumZeroFrictionIterations);
		const FReal MaxPushOut = (MaxPushOutVelocity > 0) ? (MaxPushOutVelocity * Dt) / FReal(NumIts) : 0;

		// Apply the position correction
		if (bRequiresIncrementalCollisionDetection)
		{
			return SolvePositionIncrementalImpl(Dt, BeginIndex, EndIndex, MaxPushOut, bApplyStaticFriction);
		}
		else if (bApplyStaticFriction)
		{
			return SolvePositionWithFrictionImpl(Dt, BeginIndex, EndIndex, MaxPushOut);
		}
		else
		{
			return SolvePositionNoFrictionImpl(Dt, BeginIndex, EndIndex, MaxPushOut);
		}
	}

	// Solve position including support for incremental collision detection
	bool FPBDCollisionSolverContainer::SolvePositionIncrementalImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut, const bool bApplyStaticFriction)
	{
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			FPBDCollisionSolverAdapter& CollisionSolver = CollisionSolvers[SolverIndex];
			if (CollisionSolver.IsIncrementalManifold())
			{
				Collisions::Update(*CollisionSolver.GetConstraint(), Dt);
				CollisionSolver.GatherManifoldPoints(Dt, &CollisionSolver.GetSolver().SolverBody0().SolverBody(), &CollisionSolver.GetSolver().SolverBody1().SolverBody());
			}
			if (bApplyStaticFriction)
			{
				bNeedsAnotherIteration |= CollisionSolver.GetSolver().SolvePositionWithFriction(Dt, MaxPushOut);
			}
			else
			{
				bNeedsAnotherIteration |= CollisionSolver.GetSolver().SolvePositionNoFriction(Dt, MaxPushOut);
			}

		}
		return bNeedsAnotherIteration;
	}

	// Solve position with friction (last few iterations each tick)
	bool FPBDCollisionSolverContainer::SolvePositionWithFrictionImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut)
	{
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			bNeedsAnotherIteration |= CollisionSolvers[SolverIndex].GetSolver().SolvePositionWithFriction(Dt, MaxPushOut);
		}
		return bNeedsAnotherIteration;
	}

	// Solve position without friction (first few iterations each tick)
	bool FPBDCollisionSolverContainer::SolvePositionNoFrictionImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut)
	{
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			bNeedsAnotherIteration |= CollisionSolvers[SolverIndex].GetSolver().SolvePositionNoFriction(Dt, MaxPushOut);
		}
		return bNeedsAnotherIteration;
	}

	bool FPBDCollisionSolverContainer::SolveVelocityImpl(const FReal InDt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_ApplyPushOut);
		if (!bChaos_PBDCollisionSolver_Velocity_SolveEnabled)
		{
			return false;
		}
		const FSolverReal Dt = FSolverReal(InDt);

		UpdateVelocityShockPropagation(Dt, It, NumIts, BeginIndex, EndIndex);

		const int32 NumDynamicFrictionIterations = 1;
		const bool bApplyDynamicFriction = (It >= NumIts - NumDynamicFrictionIterations);

		// Apply the velocity correction
		// @todo(chaos): parallel version of SolveVelocity
		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			bNeedsAnotherIteration |= CollisionSolvers[SolverIndex].GetSolver().SolveVelocity(Dt, bApplyDynamicFriction);
		}

		return bNeedsAnotherIteration;
	}

	void FPBDCollisionSolverContainer::ScatterOutput(const FReal Dt, const int32 BeginIndex, const int32 EndIndex)
	{
		return ScatterOutputImpl(Dt, BeginIndex, EndIndex, false);
	}

	void FPBDCollisionSolverContainer::ScatterOutputImpl(const FReal Dt, const int32 BeginIndex, const int32 EndIndex, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_Scatter);
		check(BeginIndex >= 0);
		check(EndIndex <= CollisionSolvers.Num());

		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			CollisionSolvers[SolverIndex].ScatterOutput(Dt);
		}
	}

}
