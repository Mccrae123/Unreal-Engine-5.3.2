// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/ChaosDebugDraw.h"
#include "Chaos/DebugDrawQueue.h"
#include "Chaos/Joint/ChaosJointLog.h"
#include "Chaos/Joint/ColoringGraph.h"
#include "Chaos/Joint/JointConstraintsCVars.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDJointConstraintUtilities.h"
#include "Chaos/Utilities.h"
#include "ChaosLog.h"
#include "ChaosStats.h"
#include "ProfilingDebugging/CsvProfiler.h"

#include "HAL/IConsoleManager.h"

#if INTEL_ISPC
#include "PBDJointSolverGaussSeidel.ispc.generated.h"
#endif

namespace Chaos
{
	DECLARE_CYCLE_STAT(TEXT("Joints::Sort"), STAT_Joints_Sort, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::PrepareTick"), STAT_Joints_PrepareTick, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::UnprepareTick"), STAT_Joints_UnprepareTick, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::PrepareIterations"), STAT_Joints_PrepareIteration, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::UnprepareIteration"), STAT_Joints_UnprepareIteration, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::Apply"), STAT_Joints_Apply, STATGROUP_ChaosJoint);
	DECLARE_CYCLE_STAT(TEXT("Joints::ApplyPushOut"), STAT_Joints_ApplyPushOut, STATGROUP_ChaosJoint);

	//
	// Constraint Handle
	//

	
	FPBDJointConstraintHandle::FPBDJointConstraintHandle()
	{
	}

	
	FPBDJointConstraintHandle::FPBDJointConstraintHandle(FConstraintContainer* InConstraintContainer, int32 InConstraintIndex)
		: TContainerConstraintHandle<FPBDJointConstraints>(InConstraintContainer, InConstraintIndex)
	{
	}

	
	void FPBDJointConstraintHandle::CalculateConstraintSpace(FVec3& OutXa, FMatrix33& OutRa, FVec3& OutXb, FMatrix33& OutRb) const
	{
		ConcreteContainer()->CalculateConstraintSpace(ConstraintIndex, OutXa, OutRa, OutXb, OutRb);
	}


	int32 FPBDJointConstraintHandle::GetConstraintIsland() const
	{
		return ConcreteContainer()->GetConstraintIsland(ConstraintIndex);
	}


	int32 FPBDJointConstraintHandle::GetConstraintLevel() const
	{
		return ConcreteContainer()->GetConstraintLevel(ConstraintIndex);
	}


	int32 FPBDJointConstraintHandle::GetConstraintColor() const
	{
		return ConcreteContainer()->GetConstraintColor(ConstraintIndex);
	}


	bool FPBDJointConstraintHandle::IsConstraintEnabled() const
	{
		return ConcreteContainer()->IsConstraintEnabled(ConstraintIndex);
	}

	bool FPBDJointConstraintHandle::IsConstraintBreaking() const
	{
		return ConcreteContainer()->IsConstraintBreaking(ConstraintIndex);
	}

	void FPBDJointConstraintHandle::ClearConstraintBreaking()
	{
		return ConcreteContainer()->ClearConstraintBreaking(ConstraintIndex);
	}

	FVec3 FPBDJointConstraintHandle::GetLinearImpulse() const
	{
		return ConcreteContainer()->GetConstraintLinearImpulse(ConstraintIndex);
	}

	FVec3 FPBDJointConstraintHandle::GetAngularImpulse() const
	{
		return ConcreteContainer()->GetConstraintAngularImpulse(ConstraintIndex);
	}

	
	const FPBDJointSettings& FPBDJointConstraintHandle::GetSettings() const
	{
		return ConcreteContainer()->GetConstraintSettings(ConstraintIndex);
	}

	void FPBDJointConstraintHandle::SetSettings(const FPBDJointSettings& Settings)
	{
		ConcreteContainer()->SetConstraintSettings(ConstraintIndex, Settings);
	}

	TVector<FGeometryParticleHandle*, 2> FPBDJointConstraintHandle::GetConstrainedParticles() const 
	{ 
		return ConcreteContainer()->GetConstrainedParticles(ConstraintIndex);
	}

	void FPBDJointConstraintHandle::SetConstraintEnabled(bool bInEnabled)
	{
		return ConcreteContainer()->SetConstraintEnabled(ConstraintIndex, bInEnabled);
	}

	//
	// Constraint Settings
	//

	
	FPBDJointSettings::FPBDJointSettings()
		: Stiffness(1)
		, LinearProjection(0)
		, AngularProjection(0)
		, ParentInvMassScale(1)
		, bCollisionEnabled(true)
		, bProjectionEnabled(false)
		, bSoftProjectionEnabled(false)
		, LinearMotionTypes({ EJointMotionType::Locked, EJointMotionType::Locked, EJointMotionType::Locked })
		, LinearLimit(FLT_MAX)
		, AngularMotionTypes({ EJointMotionType::Free, EJointMotionType::Free, EJointMotionType::Free })
		, AngularLimits(FVec3(FLT_MAX, FLT_MAX, FLT_MAX))
		, bSoftLinearLimitsEnabled(false)
		, bSoftTwistLimitsEnabled(false)
		, bSoftSwingLimitsEnabled(false)
		, LinearSoftForceMode(EJointForceMode::Acceleration)
		, AngularSoftForceMode(EJointForceMode::Acceleration)
		, SoftLinearStiffness(0)
		, SoftLinearDamping(0)
		, SoftTwistStiffness(0)
		, SoftTwistDamping(0)
		, SoftSwingStiffness(0)
		, SoftSwingDamping(0)
		, LinearRestitution(0)
		, TwistRestitution(0)
		, SwingRestitution(0)
		, LinearContactDistance(0)
		, TwistContactDistance(0)
		, SwingContactDistance(0)
		, LinearDrivePositionTarget(FVec3(0, 0, 0))
		, LinearDriveVelocityTarget(FVec3(0, 0, 0))
		, bLinearPositionDriveEnabled(TVector<bool, 3>(false, false, false))
		, bLinearVelocityDriveEnabled(TVector<bool, 3>(false, false, false))
		, LinearDriveForceMode(EJointForceMode::Acceleration)
		, LinearDriveStiffness(0)
		, LinearDriveDamping(0)
		, AngularDrivePositionTarget(FRotation3::FromIdentity())
		, AngularDriveVelocityTarget(FVec3(0, 0, 0))
		, bAngularSLerpPositionDriveEnabled(false)
		, bAngularSLerpVelocityDriveEnabled(false)
		, bAngularTwistPositionDriveEnabled(false)
		, bAngularTwistVelocityDriveEnabled(false)
		, bAngularSwingPositionDriveEnabled(false)
		, bAngularSwingVelocityDriveEnabled(false)
		, AngularDriveForceMode(EJointForceMode::Acceleration)
		, AngularDriveStiffness(0)
		, AngularDriveDamping(0)
		, LinearBreakForce(FLT_MAX)
		, LinearPlasticityLimit(FLT_MAX)
		, LinearPlasticityType(EPlasticityType::Free)
		, LinearPlasticityInitialDistanceSquared(FLT_MAX)
		, AngularBreakTorque(FLT_MAX)
		, AngularPlasticityLimit(FLT_MAX)
		, UserData(nullptr)
	{
	}


	void FPBDJointSettings::Sanitize()
	{
		// Disable soft joints for locked dofs
		if ((LinearMotionTypes[0] == EJointMotionType::Locked) && (LinearMotionTypes[1] == EJointMotionType::Locked) && (LinearMotionTypes[2] == EJointMotionType::Locked))
		{
			bSoftLinearLimitsEnabled = false;
		}
		if (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Locked)
		{
			bSoftTwistLimitsEnabled = false;
		}
		if ((AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] == EJointMotionType::Locked) && (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] == EJointMotionType::Locked))
		{
			bSoftSwingLimitsEnabled = false;
		}

		// Reset limits if they won't be used (means we don't have to check if limited/locked in a few cases).
		// A side effect: if we enable a constraint, we need to reset the value of the limit.
		if ((LinearMotionTypes[0] != EJointMotionType::Limited) && (LinearMotionTypes[1] != EJointMotionType::Limited) && (LinearMotionTypes[2] != EJointMotionType::Limited))
		{
			LinearLimit = 0;
		}
		if (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] != EJointMotionType::Limited)
		{
			AngularLimits[(int32)EJointAngularConstraintIndex::Twist] = 0;
		}
		if (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] != EJointMotionType::Limited)
		{
			AngularLimits[(int32)EJointAngularConstraintIndex::Swing1] = 0;
		}
		if (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] != EJointMotionType::Limited)
		{
			AngularLimits[(int32)EJointAngularConstraintIndex::Swing2] = 0;
		}

		// If we have a zero degree limit angle, lock the joint, or set a non-zero limit (to avoid division by zero in axis calculations)
		const FReal MinAngularLimit = 0.01f;
		if ((AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Limited) && (AngularLimits[(int32)EJointAngularConstraintIndex::Twist] < MinAngularLimit))
		{
			if (bSoftTwistLimitsEnabled)
			{
				AngularLimits[(int32)EJointAngularConstraintIndex::Twist] = MinAngularLimit;
			}
			else
			{
				AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] = EJointMotionType::Locked;
			}
		}
		if ((AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] == EJointMotionType::Limited) && (AngularLimits[(int32)EJointAngularConstraintIndex::Swing1] < MinAngularLimit))
		{
			if (bSoftSwingLimitsEnabled)
			{
				AngularLimits[(int32)EJointAngularConstraintIndex::Swing1] = MinAngularLimit;
			}
			else
			{
				AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] = EJointMotionType::Locked;
			}
		}
		if ((AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] == EJointMotionType::Limited) && (AngularLimits[(int32)EJointAngularConstraintIndex::Swing2] < MinAngularLimit))
		{
			if (bSoftSwingLimitsEnabled)
			{
				AngularLimits[(int32)EJointAngularConstraintIndex::Swing2] = MinAngularLimit;
			}
			else
			{
				AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] = EJointMotionType::Locked;
			}
		}

		// SLerp drive is only allowed if no angular dofs are locked
		if (bAngularSLerpPositionDriveEnabled || bAngularSLerpVelocityDriveEnabled)
		{
			if ((AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Locked)
				|| (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] == EJointMotionType::Locked)
				|| (AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] == EJointMotionType::Locked))
			{
				bAngularSLerpPositionDriveEnabled = false;
				bAngularSLerpVelocityDriveEnabled = false;
			}
		}
	}

	
	FPBDJointState::FPBDJointState()
		: Island(INDEX_NONE)
		, Level(INDEX_NONE)
		, Color(INDEX_NONE)
		, IslandSize(0)
		, bDisabled(false)
		, LinearImpulse(FVec3(0))
		, AngularImpulse(FVec3(0))
	{
	}


	//
	// Solver Settings
	//

	
	FPBDJointSolverSettings::FPBDJointSolverSettings()
		: ApplyPairIterations(1)
		, ApplyPushOutPairIterations(1)
		, SwingTwistAngleTolerance(1.0e-6f)
		, PositionTolerance(0)
		, AngleTolerance(0)
		, MinParentMassRatio(0)
		, MaxInertiaRatio(0)
		, MinSolverStiffness(1)
		, MaxSolverStiffness(1)
		, NumIterationsAtMaxSolverStiffness(1)
		, bEnableTwistLimits(true)
		, bEnableSwingLimits(true)
		, bEnableDrives(true)
		, LinearStiffnessOverride(-1)
		, TwistStiffnessOverride(-1)
		, SwingStiffnessOverride(-1)
		, LinearProjectionOverride(-1)
		, AngularProjectionOverride(-1)
		, LinearDriveStiffnessOverride(-1)
		, LinearDriveDampingOverride(-1)
		, AngularDriveStiffnessOverride(-1)
		, AngularDriveDampingOverride(-1)
		, SoftLinearStiffnessOverride(-1)
		, SoftLinearDampingOverride(-1)
		, SoftTwistStiffnessOverride(-1)
		, SoftTwistDampingOverride(-1)
		, SoftSwingStiffnessOverride(-1)
		, SoftSwingDampingOverride(-1)
	{
	}


	//
	// Constraint Container
	//

	
	FPBDJointConstraints::FPBDJointConstraints(const FPBDJointSolverSettings& InSettings)
		: FPBDConstraintContainer(EConstraintContainerType::Joint)
		, Settings(InSettings)
		, bJointsDirty(false)
		, bUpdateVelocityInApplyConstraints(false)
		, PreApplyCallback(nullptr)
		, PostApplyCallback(nullptr)
		, SolverType(EConstraintSolverType::GbfPbd)
	{
	}

	
	FPBDJointConstraints::~FPBDJointConstraints()
	{
	}

	
	const FPBDJointSolverSettings& FPBDJointConstraints::GetSettings() const
	{
		return Settings;
	}

	
	void FPBDJointConstraints::SetSettings(const FPBDJointSolverSettings& InSettings)
	{
		Settings = InSettings;
	}


	int32 FPBDJointConstraints::NumConstraints() const
	{
		return ConstraintParticles.Num();
	}

	void FPBDJointConstraints::GetConstrainedParticleIndices(const int32 ConstraintIndex, int32& Index0, int32& Index1) const
	{
		// In solvers we need Particle0 to be the parent particle but ConstraintInstance has Particle1 as the parent, so by default
		// we need to flip the indices before we pass them to the solver. 

		Index0 = 1;
		Index1 = 0;
	}

	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FRigidTransform3& WorldConstraintFrame)
	{
		FPBDJointSettings JointSettings;
		JointSettings.ConnectorTransforms[0] = FRigidTransform3(
			WorldConstraintFrame.GetTranslation() - InConstrainedParticles[0]->X(),
			WorldConstraintFrame.GetRotation() * InConstrainedParticles[0]->R().Inverse()
			);
		JointSettings.ConnectorTransforms[1] = FRigidTransform3(
			WorldConstraintFrame.GetTranslation() - InConstrainedParticles[1]->X(),
			WorldConstraintFrame.GetRotation() * InConstrainedParticles[1]->R().Inverse()
			);
		return AddConstraint(InConstrainedParticles, JointSettings);
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FTransformPair& InConnectorTransforms)
	{
		FPBDJointSettings JointSettings;
		JointSettings.ConnectorTransforms = InConnectorTransforms;
		return AddConstraint(InConstrainedParticles, JointSettings);
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FPBDJointSettings& InConstraintSettings)
	{
		bJointsDirty = true;

		int ConstraintIndex = Handles.Num();
		Handles.Add(HandleAllocator.AllocHandle(this, ConstraintIndex));
		ConstraintParticles.Add(InConstrainedParticles);
		ConstraintStates.Add(FPBDJointState());

		ConstraintSettings.AddDefaulted();
		SetConstraintSettings(ConstraintIndex, InConstraintSettings);

		return Handles.Last();
	}

	
	void FPBDJointConstraints::RemoveConstraint(int ConstraintIndex)
	{
		bJointsDirty = true;

		FConstraintContainerHandle* ConstraintHandle = Handles[ConstraintIndex];
		if (ConstraintHandle != nullptr)
		{
			if (!ConstraintStates[ConstraintIndex].bDisabled)
			{
				ConstraintParticles[ConstraintIndex][0]->RemoveConstraintHandle(ConstraintHandle);
				ConstraintParticles[ConstraintIndex][1]->RemoveConstraintHandle(ConstraintHandle);
			}
			// Release the handle for the freed constraint
			HandleAllocator.FreeHandle(ConstraintHandle);
			Handles[ConstraintIndex] = nullptr;
		}

		// Swap the last constraint into the gap to keep the array packed
		ConstraintParticles.RemoveAtSwap(ConstraintIndex);
		ConstraintSettings.RemoveAtSwap(ConstraintIndex);
		ConstraintStates.RemoveAtSwap(ConstraintIndex);
		Handles.RemoveAtSwap(ConstraintIndex);

		// Update the handle for the constraint that was moved
		if (ConstraintIndex < Handles.Num())
		{
			SetConstraintIndex(Handles[ConstraintIndex], ConstraintIndex);
		}
	}



	
	void FPBDJointConstraints::DisconnectConstraints(const TSet<TGeometryParticleHandle<FReal, 3>*>& RemovedParticles)
	{
		for (TGeometryParticleHandle<FReal, 3>*RemovedParticle : RemovedParticles)
		{
			for (FConstraintHandle* ConstraintHandle : RemovedParticle->ParticleConstraints())
			{
				if (ConstraintHandle->As<FPBDJointConstraintHandle>())
				{
					ConstraintHandle->SetEnabled(false); // constraint lifespan is managed by the proxy
					int ConstraintIndex = ConstraintHandle->GetConstraintIndex();
					if (ConstraintIndex != INDEX_NONE)
					{
						if (ConstraintParticles[ConstraintIndex][0] == RemovedParticle)
						{
							ConstraintParticles[ConstraintIndex][0] = nullptr;
						}
						if (ConstraintParticles[ConstraintIndex][1] == RemovedParticle)
						{
							ConstraintParticles[ConstraintIndex][1] = nullptr;
						}
					}
				}
			}
		}
	}

	void FPBDJointConstraints::SortConstraints()
	{
		// Sort constraints so that constraints with lower level (closer to a kinematic joint) are first
		// @todo(ccaulfield): should probably also take islands/particle order into account
		// @todo(ccaulfield): optimize (though isn't called very often)
		SCOPE_CYCLE_COUNTER(STAT_Joints_Sort);

		FHandles SortedHandles = Handles;
		SortedHandles.StableSort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
			{
				if (L.GetConstraintIsland() != R.GetConstraintIsland())
				{
					return L.GetConstraintIsland() < R.GetConstraintIsland();
				}
				else if (L.GetConstraintLevel() != R.GetConstraintLevel())
				{
					return L.GetConstraintLevel() < R.GetConstraintLevel();
				}
				return L.GetConstraintColor() < R.GetConstraintColor();
			});

		TArray<FPBDJointSettings> SortedConstraintSettings;
		TArray<FParticlePair> SortedConstraintParticles;
		TArray<FPBDJointState> SortedConstraintStates;
		SortedConstraintSettings.Reserve(SortedHandles.Num());
		SortedConstraintParticles.Reserve(SortedHandles.Num());
		SortedConstraintStates.Reserve(SortedHandles.Num());

		for (int32 SortedConstraintIndex = 0; SortedConstraintIndex < SortedHandles.Num(); ++SortedConstraintIndex)
		{
			FConstraintContainerHandle* Handle = SortedHandles[SortedConstraintIndex];
			int32 UnsortedConstraintIndex = Handle->GetConstraintIndex();

			SortedConstraintSettings.Add(ConstraintSettings[UnsortedConstraintIndex]);
			SortedConstraintParticles.Add(ConstraintParticles[UnsortedConstraintIndex]);
			SortedConstraintStates.Add(ConstraintStates[UnsortedConstraintIndex]);
			SetConstraintIndex(Handle, SortedConstraintIndex);
		}

		Swap(ConstraintSettings, SortedConstraintSettings);
		Swap(ConstraintParticles, SortedConstraintParticles);
		Swap(ConstraintStates, SortedConstraintStates);
		Swap(Handles, SortedHandles);
	}


	bool FPBDJointConstraints::IsConstraintEnabled(int32 ConstraintIndex) const
	{
		return !ConstraintStates[ConstraintIndex].bDisabled;
	}

	bool FPBDJointConstraints::IsConstraintBreaking(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].bBreaking;
	}

	void FPBDJointConstraints::ClearConstraintBreaking(int32 ConstraintIndex)
	{
		ConstraintStates[ConstraintIndex].bBreaking = false;
	}

	void FPBDJointConstraints::SetConstraintEnabled(int32 ConstraintIndex, bool bEnabled)
	{
		const FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][0]);
		const FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][1]);

		if (bEnabled)
		{ 
			// only enable constraint if the particles are valid and not disabled
			if (Particle0->Handle() != nullptr && !Particle0->Disabled()
				&& Particle1->Handle() != nullptr && !Particle1->Disabled())
			{
				ConstraintStates[ConstraintIndex].bDisabled = false;
			}
		}
		else
		{ 
			// desirable to allow disabling no matter what state the endpoints
			ConstraintStates[ConstraintIndex].bDisabled = true;
		}
	}

	void FPBDJointConstraints::SetConstraintBreaking(int32 ConstraintIndex, bool bBreaking)
	{
		ConstraintStates[ConstraintIndex].bBreaking = bBreaking;
	}

	void FPBDJointConstraints::BreakConstraint(int32 ConstraintIndex)
	{
		SetConstraintEnabled(ConstraintIndex, false);
		SetConstraintBreaking(ConstraintIndex, true);
		if (BreakCallback)
		{
			BreakCallback(Handles[ConstraintIndex]);
		}
	}

	void FPBDJointConstraints::FixConstraints(int32 ConstraintIndex)
	{
		SetConstraintEnabled(ConstraintIndex, true);
	}

	
	void FPBDJointConstraints::SetPreApplyCallback(const FJointPreApplyCallback& Callback)
	{
		PreApplyCallback = Callback;
	}

	
	void FPBDJointConstraints::ClearPreApplyCallback()
	{
		PreApplyCallback = nullptr;
	}


	void FPBDJointConstraints::SetPostApplyCallback(const FJointPostApplyCallback& Callback)
	{
		PostApplyCallback = Callback;
	}


	void FPBDJointConstraints::ClearPostApplyCallback()
	{
		PostApplyCallback = nullptr;
	}


	void FPBDJointConstraints::SetPostProjectCallback(const FJointPostApplyCallback& Callback)
	{
		PostProjectCallback = Callback;
	}


	void FPBDJointConstraints::ClearPostProjectCallback()
	{
		PostProjectCallback = nullptr;
	}


	void FPBDJointConstraints::SetBreakCallback(const FJointBreakCallback& Callback)
	{
		BreakCallback = Callback;
	}


	void FPBDJointConstraints::ClearBreakCallback()
	{
		BreakCallback = nullptr;
	}


	const typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::GetConstraintHandle(int32 ConstraintIndex) const
	{
		return Handles[ConstraintIndex];
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::GetConstraintHandle(int32 ConstraintIndex)
	{
		return Handles[ConstraintIndex];
	}

	
	const typename FPBDJointConstraints::FParticlePair& FPBDJointConstraints::GetConstrainedParticles(int32 ConstraintIndex) const
	{
		return ConstraintParticles[ConstraintIndex];
	}

	
	const FPBDJointSettings& FPBDJointConstraints::GetConstraintSettings(int32 ConstraintIndex) const
	{
		return ConstraintSettings[ConstraintIndex];
	}


	void FPBDJointConstraints::SetConstraintSettings(int32 ConstraintIndex, const FPBDJointSettings& InConstraintSettings)
	{
		ConstraintSettings[ConstraintIndex] = InConstraintSettings;
		ConstraintSettings[ConstraintIndex].Sanitize();
	}


	int32 FPBDJointConstraints::GetConstraintIsland(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].Island;
	}


	int32 FPBDJointConstraints::GetConstraintLevel(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].Level;
	}


	int32 FPBDJointConstraints::GetConstraintColor(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].Color;
	}


	FVec3 FPBDJointConstraints::GetConstraintLinearImpulse(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].LinearImpulse;
	}

	FVec3 FPBDJointConstraints::GetConstraintAngularImpulse(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].AngularImpulse;
	}


	void FPBDJointConstraints::UpdatePositionBasedState(const FReal Dt)
	{
	}

	void FPBDJointConstraints::PrepareTick()
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_PrepareTick);

		if (bJointsDirty)
		{
			ColorConstraints();
			SortConstraints();

			bJointsDirty = false;
		}

		ConstraintSolvers.SetNum(NumConstraints());
	}

	void FPBDJointConstraints::UnprepareTick()
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_UnprepareTick);

		ConstraintSolvers.Empty();
	}

	void FPBDJointConstraints::PrepareIteration(FReal Dt)
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_PrepareIteration);

		for (int32 JointIndex = 0; JointIndex < NumConstraints(); ++JointIndex)
		{
			if (ConstraintStates[JointIndex].bDisabled) continue;

			const FPBDJointSettings& JointSettings = ConstraintSettings[JointIndex];

			const FTransformPair& JointFrames = JointSettings.ConnectorTransforms;
			FJointSolverGaussSeidel& Solver = ConstraintSolvers[JointIndex];

			int32 Index0, Index1;
			GetConstrainedParticleIndices(JointIndex, Index0, Index1);
			FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[JointIndex][Index0]);
			FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[JointIndex][Index1]);

			Solver.Init(
				Dt,
				Settings,
				JointSettings,
				FParticleUtilitiesXR::GetCoMWorldPosition(Particle0),	// Prev position
				FParticleUtilitiesXR::GetCoMWorldPosition(Particle1),	// Prev position
				FParticleUtilitiesXR::GetCoMWorldRotation(Particle0),	// Prev rotation
				FParticleUtilitiesXR::GetCoMWorldRotation(Particle1),	// Prev rotation
				Particle0->InvM(),
				Particle0->InvI().GetDiagonal(),
				Particle1->InvM(),
				Particle1->InvI().GetDiagonal(),
				FParticleUtilities::ParticleLocalToCoMLocal(Particle0, JointFrames[Index0]),
				FParticleUtilities::ParticleLocalToCoMLocal(Particle1, JointFrames[Index1]));

			// Plasticity should not be turned on in the middle of simulation.
			const bool bUseLinearPlasticity = JointSettings.LinearPlasticityLimit != FLT_MAX;
			if (bUseLinearPlasticity)
			{
				const bool bIsCOMDistanceInitialized = !FMath::IsNearlyEqual(JointSettings.LinearPlasticityInitialDistanceSquared, (FReal)FLT_MAX);
				if (!bIsCOMDistanceInitialized)
				{
					// Joint plasticity is baed on the distance of one of the moment arms of the joint. Typically, plasticity
					// will get setup from the joint pivot to the child COM (centor of mass), so that is found first. However, when 
					// the pivot is at the child COM then we fall back to the distance between thge pivot and parent COM.
					ConstraintSettings[JointIndex].LinearPlasticityInitialDistanceSquared = JointSettings.ConnectorTransforms[1].GetTranslation().SizeSquared();
					if (FMath::IsNearlyZero(ConstraintSettings[JointIndex].LinearPlasticityInitialDistanceSquared))
					{
						ConstraintSettings[JointIndex].LinearPlasticityInitialDistanceSquared = JointSettings.ConnectorTransforms[0].GetTranslation().SizeSquared();
					}
					ensureMsgf(!FMath::IsNearlyZero(ConstraintSettings[JointIndex].LinearPlasticityInitialDistanceSquared), TEXT("Plasticity made inactive due to Zero length difference between parent and child rigid body."));
				}
			}
		}
	}


	void FPBDJointConstraints::UnprepareIteration(FReal Dt)
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_UnprepareIteration);

		for (int32 JointIndex = 0; JointIndex < NumConstraints(); ++JointIndex)
		{
			if (ConstraintStates[JointIndex].bDisabled) continue;

			FPBDJointState& JointState = ConstraintStates[JointIndex];
			FJointSolverGaussSeidel& Solver = ConstraintSolvers[JointIndex];

			int32 Index0, Index1;
			GetConstrainedParticleIndices(JointIndex, Index0, Index1);

			// NOTE: LinearImpulse/AngularImpulse in the solver are not really impulses - they are mass-weighted position/rotation delta, or (impulse x dt).
			if (Dt > SMALL_NUMBER)
			{
				JointState.LinearImpulse = Solver.GetNetLinearImpulse() / Dt;
				JointState.AngularImpulse = Solver.GetNetAngularImpulse() / Dt;
				if (Index0 != 0)
				{
					// Particles were flipped in the solver...
					JointState.LinearImpulse = -JointState.LinearImpulse;
					JointState.AngularImpulse = -JointState.AngularImpulse;
				}
			}
			else
			{
				JointState.LinearImpulse = FVec3(0);
				JointState.AngularImpulse = FVec3(0);
			}

			ApplyPlasticityLimits(JointIndex);
		}
	}

	
	void FPBDJointConstraints::CalculateConstraintSpace(int32 ConstraintIndex, FVec3& OutX0, FMatrix33& OutR0, FVec3& OutX1, FMatrix33& OutR1) const
	{
		int32 Index0, Index1;
		GetConstrainedParticleIndices(ConstraintIndex, Index0, Index1);
		FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index0]);
		FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index1]);
		const FVec3 P0 = FParticleUtilities::GetCoMWorldPosition(Particle0);
		const FRotation3 Q0 = FParticleUtilities::GetCoMWorldRotation(Particle0);
		const FVec3 P1 = FParticleUtilities::GetCoMWorldPosition(Particle1);
		const FRotation3 Q1 = FParticleUtilities::GetCoMWorldRotation(Particle1);
		const FRigidTransform3& XL0 = FParticleUtilities::ParticleLocalToCoMLocal(Particle0, ConstraintSettings[ConstraintIndex].ConnectorTransforms[Index0]);
		const FRigidTransform3& XL1 = FParticleUtilities::ParticleLocalToCoMLocal(Particle1, ConstraintSettings[ConstraintIndex].ConnectorTransforms[Index1]);

		OutX0 = P0 + Q0 * XL0.GetTranslation();
		OutX1 = P1 + Q1 * XL1.GetTranslation();
		OutR0 = FRotation3(Q0 * XL0.GetRotation()).ToMatrix();
		OutR1 = FRotation3(Q1 * XL1.GetRotation()).ToMatrix();
	}

	//////////////////////////////////////////////////////////////////////////
	//
	// Begin Simple API Solver. Iterate over constraints in array order.
	//
	//////////////////////////////////////////////////////////////////////////

	bool FPBDJointConstraints::Apply(const FReal Dt, const int32 It, const int32 NumIts)
	{
		CSV_SCOPED_TIMING_STAT(Chaos, ApplyJointConstraints);

		if (PreApplyCallback != nullptr)
		{
			PreApplyCallback(Dt, Handles);
		}

		bool bActive = false;
		if (Settings.ApplyPairIterations > 0)
		{
			SCOPE_CYCLE_COUNTER(STAT_Joints_Apply);
			for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints(); ++ConstraintIndex)
			{
				if ( ConstraintStates[ConstraintIndex].bDisabled) continue;

				bActive |= ApplySingle(Dt, ConstraintIndex, Settings.ApplyPairIterations, It, NumIts);
			}
		}

		UE_LOG(LogChaosJoint, Verbose, TEXT("Apply Iteration: %d / %d; Active: %d"), It, NumIts, bActive);

		if (PostApplyCallback != nullptr)
		{
			PostApplyCallback(Dt, Handles);
		}

		return bActive;
	}

	bool FPBDJointConstraints::ApplyPushOut(const FReal Dt, const int32 It, const int32 NumIts)
	{

		bool bActive = false;
		if (Settings.ApplyPushOutPairIterations > 0)
		{
			SCOPE_CYCLE_COUNTER(STAT_Joints_ApplyPushOut);
			for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints(); ++ConstraintIndex)
			{
				if ( ConstraintStates[ConstraintIndex].bDisabled) continue;

				bActive |= ApplyPushOutSingle(Dt, ConstraintIndex, Settings.ApplyPushOutPairIterations, It, NumIts);
			}
		}

		UE_LOG(LogChaosJoint, Verbose, TEXT("PushOut Iteration: %d / %d; Active: %d"), It, NumIts, bActive);

		if (PostProjectCallback != nullptr)
		{
			PostProjectCallback(Dt, Handles);
		}

		return bActive;
	}

	//////////////////////////////////////////////////////////////////////////
	//
	// End Simple API Solver.
	//
	//////////////////////////////////////////////////////////////////////////

	//////////////////////////////////////////////////////////////////////////
	//
	// Begin Graph API Solver. Iterate over constraints in connectivity order.
	//
	//////////////////////////////////////////////////////////////////////////

	bool FPBDJointConstraints::Apply(const FReal Dt, const TArray<FConstraintContainerHandle*>& InConstraintHandles, const int32 It, const int32 NumIts)
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_Apply);
		CSV_SCOPED_TIMING_STAT(Chaos, ApplyJointConstraints);

		// @todo(ccaulfield): make sorting optional
		// @todo(ccaulfield): handles should be sorted by level by the constraint rule/graph
		// @todo(ccaulfield): the best sort order depends on whether we are freezing.
		// If we are freezing we want the root-most (nearest to kinematic) bodies solved first.
		// For normal update we want the root body last, otherwise it gets dragged away from the root by the other bodies

		TArray<FConstraintContainerHandle*> SortedConstraintHandles = InConstraintHandles;
		SortedConstraintHandles.Sort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
			{
				// Sort bodies from root to leaf
				return L.GetConstraintLevel() < R.GetConstraintLevel();
			});

		if (PreApplyCallback != nullptr)
		{
			PreApplyCallback(Dt, SortedConstraintHandles);
		}


		int32 NumActive = 0;
		if (Settings.ApplyPairIterations > 0)
		{
			for (FConstraintContainerHandle* ConstraintHandle : SortedConstraintHandles)
			{
				NumActive += ApplySingle(Dt, ConstraintHandle->GetConstraintIndex(), Settings.ApplyPairIterations, It, NumIts);
			}
		}

		if (PostApplyCallback != nullptr)
		{
			PostApplyCallback(Dt, SortedConstraintHandles);
		}

		return (NumActive > 0);
	}

	
	bool FPBDJointConstraints::ApplyPushOut(const FReal Dt, const TArray<FConstraintContainerHandle*>& InConstraintHandles, const int32 It, const int32 NumIts)
	{
		SCOPE_CYCLE_COUNTER(STAT_Joints_ApplyPushOut);

		TArray<FConstraintContainerHandle*> SortedConstraintHandles = InConstraintHandles;
		SortedConstraintHandles.Sort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
			{
				// Sort bodies from root to leaf
				return L.GetConstraintLevel() < R.GetConstraintLevel();
			});

		int32 NumActive = 0;
		if (Settings.ApplyPushOutPairIterations > 0)
		{
			for (FConstraintContainerHandle* ConstraintHandle : SortedConstraintHandles)
			{
				NumActive += ApplyPushOutSingle(Dt, ConstraintHandle->GetConstraintIndex(), Settings.ApplyPushOutPairIterations, It, NumIts);
			}
		}

		if (PostProjectCallback != nullptr)
		{
			PostProjectCallback(Dt, SortedConstraintHandles);
		}

		return (NumActive > 0);
	}

	//////////////////////////////////////////////////////////////////////////
	//
	// End Graph API Solver.
	//
	//////////////////////////////////////////////////////////////////////////


	//////////////////////////////////////////////////////////////////////////
	//
	// Begin single-particle solve methods used by APIs
	//
	//////////////////////////////////////////////////////////////////////////

	void FPBDJointConstraints::UpdateParticleState(TPBDRigidParticleHandle<FReal, 3>* Rigid, const FReal Dt, const FVec3& PrevP, const FRotation3& PrevQ, const FVec3& P, const FRotation3& Q, const bool bUpdateVelocity)
	{
		if ((Rigid != nullptr) && (Rigid->ObjectState() == EObjectStateType::Dynamic))
		{
			FParticleUtilities::SetCoMWorldTransform(Rigid, P, Q);
			if (bUpdateVelocity && (Dt > SMALL_NUMBER))
			{
				const FVec3 V = FVec3::CalculateVelocity(PrevP, P, Dt);
				const FVec3 W = FRotation3::CalculateAngularVelocity(PrevQ, Q, Dt);
				Rigid->SetV(V);
				Rigid->SetW(W);
			}
		}
	}


	void FPBDJointConstraints::UpdateParticleStateExplicit(TPBDRigidParticleHandle<FReal, 3>* Rigid, const FReal Dt, const FVec3& P, const FRotation3& Q, const FVec3& V, const FVec3& W)
	{
		if ((Rigid != nullptr) && (Rigid->ObjectState() == EObjectStateType::Dynamic))
		{
			FParticleUtilities::SetCoMWorldTransform(Rigid, P, Q);
			Rigid->SetV(V);
			Rigid->SetW(W);
		}
	}

	FReal FPBDJointConstraints::CalculateIterationStiffness(int32 It, int32 NumIts) const
	{
		// Linearly interpolate betwwen MinStiffness and MaxStiffness over the first few iterations,
		// then clamp at MaxStiffness for the final NumIterationsAtMaxStiffness
		FReal IterationStiffness = Settings.MaxSolverStiffness;
		if (NumIts > Settings.NumIterationsAtMaxSolverStiffness)
		{
			const FReal Interpolant = FMath::Clamp((FReal)It / (FReal)(NumIts - Settings.NumIterationsAtMaxSolverStiffness), 0.0f, 1.0f);
			IterationStiffness = FMath::Lerp(Settings.MinSolverStiffness, Settings.MaxSolverStiffness, Interpolant);
		}
		return FMath::Clamp(IterationStiffness, 0.0f, 1.0f);
	}

	bool FPBDJointConstraints::CanEvaluate(const int32 ConstraintIndex) const
	{
		if (!IsConstraintEnabled(ConstraintIndex))
		{
			return false;
		}

		int32 Index0, Index1;
		GetConstrainedParticleIndices(ConstraintIndex, Index0, Index1);
		const FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index0]);
		const FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index1]);

		// check for valid and enabled particles
		if (Particle0->Handle() == nullptr || Particle0->Disabled()
			|| Particle1->Handle() == nullptr || Particle1->Disabled())
		{
			return false;
		}

		// check valid particle and solver state
		const FJointSolverGaussSeidel& Solver = ConstraintSolvers[ConstraintIndex];
		if ((Particle0->Sleeping() && Particle1->Sleeping())
			|| (Particle0->IsKinematic() && Particle1->Sleeping())
			|| (Particle0->Sleeping() && Particle1->IsKinematic())
			|| (FMath::IsNearlyZero(Solver.InvM(0)) && FMath::IsNearlyZero(Solver.InvM(1))))
		{
			return false;
		}
		return true;
	}

	// This position solver iterates over each of the inner constraints (position, twist, swing) and solves them independently.
	// This will converge slowly in some cases, particularly where resolving angular constraints violates position constraints and vice versa.
	bool FPBDJointConstraints::ApplySingle(const FReal Dt, const int32 ConstraintIndex, const int32 NumPairIts, const int32 It, const int32 NumIts)
	{
		if (!CanEvaluate(ConstraintIndex))
		{
			return false;
		}

		const TVector<TGeometryParticleHandle<FReal, 3>*, 2>& Constraint = ConstraintParticles[ConstraintIndex];
		UE_LOG(LogChaosJoint, VeryVerbose, TEXT("Solve Joint Constraint %d %s %s (dt = %f; it = %d / %d)"), ConstraintIndex, *Constraint[0]->ToString(), *Constraint[1]->ToString(), Dt, It, NumIts);

		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];
		FJointSolverGaussSeidel& Solver = ConstraintSolvers[ConstraintIndex];

		int32 Index0, Index1;
		GetConstrainedParticleIndices(ConstraintIndex, Index0, Index1);
		FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index0]);
		FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index1]);

		const FVec3 P0 = FParticleUtilities::GetCoMWorldPosition(Particle0);
		const FRotation3 Q0 = FParticleUtilities::GetCoMWorldRotation(Particle0);
		const FVec3 P1 = FParticleUtilities::GetCoMWorldPosition(Particle1);
		const FRotation3 Q1 = FParticleUtilities::GetCoMWorldRotation(Particle1);

		const bool bWasActive = Solver.GetIsActive();

		const FReal IterationStiffness = CalculateIterationStiffness(It, NumIts);

		Solver.Update(
			Dt,
			IterationStiffness,
			Settings,
			JointSettings,
			P0,
			Q0,
			Particle0->V(),
			Particle0->W(),
			P1,
			Q1,
			Particle1->V(),
			Particle1->W());

		// If we were solved last iteration and nothing has changed since, we are done
		if (!bWasActive && !Solver.GetIsActive() && bChaos_Joint_EarlyOut_Enabled)
		{
			return false;
		}

		for (int32 PairIt = 0; PairIt < NumPairIts; ++PairIt)
		{
			UE_LOG(LogChaosJoint, VeryVerbose, TEXT("  Pair Iteration %d / %d"), PairIt, NumPairIts);

			// This is the same for all SolverType settings (which makes it wrong for the GbfPbd version)
			Solver.ApplyConstraints(Dt, Settings, JointSettings);

			if (!Solver.GetIsActive() && bChaos_Joint_EarlyOut_Enabled)
			{
				break;
			}
		}

		UpdateParticleState(Particle0->CastToRigidParticle(), Dt, Solver.GetInitP(0), Solver.GetInitQ(0), Solver.GetP(0), Solver.GetQ(0), bUpdateVelocityInApplyConstraints);
		UpdateParticleState(Particle1->CastToRigidParticle(), Dt, Solver.GetInitP(1), Solver.GetInitQ(1), Solver.GetP(1), Solver.GetQ(1), bUpdateVelocityInApplyConstraints);

		// @todo(ccaulfield): The break limit should really be applied to the impulse in the solver to prevent 1-frame impulses larger than the threshold
		if ((JointSettings.LinearBreakForce!=FLT_MAX) || (JointSettings.AngularBreakTorque!=FLT_MAX))
		{
			ApplyBreakThreshold(Dt, ConstraintIndex, Solver.GetNetLinearImpulse(), Solver.GetNetAngularImpulse());
		}

		return Solver.GetIsActive() || !bChaos_Joint_EarlyOut_Enabled;
	}

	bool FPBDJointConstraints::ApplyPushOutSingle(const FReal Dt, const int32 ConstraintIndex, const int32 NumPairIts, const int32 It, const int32 NumIts)
	{
		if (!CanEvaluate(ConstraintIndex))
		{
			return false;
		}

		const TVector<TGeometryParticleHandle<FReal, 3>*, 2>& Constraint = ConstraintParticles[ConstraintIndex];
		UE_LOG(LogChaosJoint, VeryVerbose, TEXT("Project Joint Constraint %d %s %s (dt = %f; it = %d / %d)"), ConstraintIndex, *Constraint[0]->ToString(), *Constraint[1]->ToString(), Dt, It, NumIts);

		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];
		FJointSolverGaussSeidel& Solver = ConstraintSolvers[ConstraintIndex];

		int32 Index0, Index1;
		GetConstrainedParticleIndices(ConstraintIndex, Index0, Index1);
		FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index0]);
		FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index1]);

		const FVec3 P0 = FParticleUtilities::GetCoMWorldPosition(Particle0);
		const FRotation3 Q0 = FParticleUtilities::GetCoMWorldRotation(Particle0);
		const FVec3 P1 = FParticleUtilities::GetCoMWorldPosition(Particle1);
		const FRotation3 Q1 = FParticleUtilities::GetCoMWorldRotation(Particle1);

		const bool bWasActive = Solver.GetIsActive();

		const FReal IterationStiffness = CalculateIterationStiffness(It, NumIts);

		Solver.Update(
			Dt,
			IterationStiffness,
			Settings,
			JointSettings,
			P0,
			Q0,
			Particle0->V(),
			Particle0->W(),
			P1,
			Q1,
			Particle1->V(),
			Particle1->W());

		// If we were solved last iteration and nothing has changed since, we are done
		if (!bWasActive && !Solver.GetIsActive() && bChaos_Joint_EarlyOut_Enabled)
		{
			return false;
		}

		for (int32 PairIt = 0; PairIt < NumPairIts; ++PairIt)
		{
			switch(SolverType)
			{
			case EConstraintSolverType::None:
				break;
			case EConstraintSolverType::GbfPbd:
			case EConstraintSolverType::StandardPbd:
				Solver.ApplyProjections(Dt, Settings, JointSettings);
				break;
			case EConstraintSolverType::QuasiPbd:
				Solver.ApplyVelocityConstraints(Dt, Settings, JointSettings);
				break;
			}

			if (!Solver.GetIsActive() && bChaos_Joint_EarlyOut_Enabled)
			{
				break;
			}
		}

		UpdateParticleStateExplicit(Particle0->CastToRigidParticle(), Dt, Solver.GetP(0), Solver.GetQ(0), Solver.GetV(0), Solver.GetW(0));
		UpdateParticleStateExplicit(Particle1->CastToRigidParticle(), Dt, Solver.GetP(1), Solver.GetQ(1), Solver.GetV(1), Solver.GetW(1));

		// @todo(ccaulfield): should probably add to net impulses in push out too...(for breaking etc)

		return Solver.GetIsActive() || !bChaos_Joint_EarlyOut_Enabled;
	}

	void FPBDJointConstraints::ApplyBreakThreshold(const FReal Dt, int32 ConstraintIndex, const FVec3& LinearImpulse, const FVec3& AngularImpulse)
	{
		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];

		// NOTE: LinearImpulse/AngularImpulse are not really impulses - they are mass-weighted position/rotation delta, or (impulse x dt).
		// The Threshold is a force limit, so we need to convert it to a position delta caused by that force in one timestep

		bool bBreak = false;
		if (!bBreak && JointSettings.LinearBreakForce!=FLT_MAX)
		{
			const FReal LinearThreshold = JointSettings.LinearBreakForce * Dt * Dt;
			UE_LOG(LogChaosJoint, VeryVerbose, TEXT("Constraint %d Linear Break Check: %f / %f"), ConstraintIndex, LinearImpulse.Size(), LinearThreshold);

			const FReal LinearThresholdSq = LinearThreshold * LinearThreshold;
			bBreak = LinearImpulse.SizeSquared() > LinearThresholdSq;
		}

		if (!bBreak && JointSettings.AngularBreakTorque!=FLT_MAX)
		{
			const FReal AngularThreshold = JointSettings.AngularBreakTorque * Dt * Dt;
			UE_LOG(LogChaosJoint, VeryVerbose, TEXT("Constraint %d Angular Break Check: %f / %f"), ConstraintIndex, AngularImpulse.Size(), AngularThreshold);

			const FReal AngularThresholdSq = AngularThreshold * AngularThreshold;
			bBreak = AngularImpulse.SizeSquared() > AngularThresholdSq;
		}

		if (bBreak)
		{
			BreakConstraint(ConstraintIndex);
		}
	}


	void FPBDJointConstraints::ApplyPlasticityLimits(const int32 ConstraintIndex)
	{
		FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];
		const bool bHasLinearPlasticityLimit = JointSettings.LinearPlasticityLimit != FLT_MAX;
		const bool bHasAngularPlasticityLimit = JointSettings.AngularPlasticityLimit != FLT_MAX;
		const bool bHasPlasticityLimits =  bHasLinearPlasticityLimit || bHasAngularPlasticityLimit;
		if(!bHasPlasticityLimits)
		{
			return;
		}

		if(!Settings.bEnableDrives)
		{
			return;
		}

		int32 Index0, Index1;
		GetConstrainedParticleIndices(ConstraintIndex, Index0, Index1);
		FGenericParticleHandle Particle0 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index0]);
		FGenericParticleHandle Particle1 = FGenericParticleHandle(ConstraintParticles[ConstraintIndex][Index1]);
		if (Particle0->Disabled() || Particle1->Disabled())
		{
			return;
		}

		const FTransformPair &ConstraintFramesLocal = JointSettings.ConnectorTransforms;
		FTransformPair ConstraintFramesGlobal(ConstraintFramesLocal[Index0] * FRigidTransform3(Particle0->P(), Particle0->Q()), ConstraintFramesLocal[Index1] * FRigidTransform3(Particle1->P(), Particle1->Q()));
		FQuat Q1 = ConstraintFramesGlobal[1].GetRotation();
		Q1.EnforceShortestArcWith(ConstraintFramesGlobal[0].GetRotation());
		ConstraintFramesGlobal[1].SetRotation(Q1);

		if (bHasLinearPlasticityLimit)
		{
			FVec3 LinearDisplacement = ConstraintFramesGlobal[0].InverseTransformPositionNoScale(ConstraintFramesGlobal[1].GetTranslation());

			// @todo(chaos): still need to warn against the case where all position drives are not enabled or all dimensions are locked. Warning should print out the joint names and should only print out once to avoid spamming.
			for (int32 Axis = 0; Axis < 3; Axis++)
			{
				if (!JointSettings.bLinearPositionDriveEnabled[Axis] || JointSettings.LinearMotionTypes[Axis] == EJointMotionType::Locked)
				{
					LinearDisplacement[Axis] = 0;
				}
			}
			// Assuming that the dimensions which are locked or have no targets are 0. in LinearDrivePositionTarget
			FReal LinearPlasticityDistanceThreshold = JointSettings.LinearPlasticityLimit * JointSettings.LinearPlasticityLimit * JointSettings.LinearPlasticityInitialDistanceSquared;
			if ((LinearDisplacement - JointSettings.LinearDrivePositionTarget).SizeSquared() > LinearPlasticityDistanceThreshold)
			{
				if (JointSettings.LinearPlasticityType == EPlasticityType::Free)
				{
					JointSettings.LinearDrivePositionTarget = LinearDisplacement;
				}
				else // EPlasticityType::Shrink || EPlasticityType::Grow
				{
					// Shrink and Grow are based on the distance between the joint pivot and the child. 
					// Note, if the pivot is located at the COM of the child then shrink will not do anything. 
					FVector CurrentDelta = ConstraintFramesLocal[Index0].TransformPosition(LinearDisplacement);
					FVector StartDelta = ConstraintFramesLocal[Index0].TransformPosition(JointSettings.LinearDrivePositionTarget);

					if (JointSettings.LinearPlasticityType == EPlasticityType::Shrink && CurrentDelta.SizeSquared() < StartDelta.SizeSquared())
					{
						JointSettings.LinearDrivePositionTarget = LinearDisplacement;
					}
					else if (JointSettings.LinearPlasticityType == EPlasticityType::Grow && CurrentDelta.SizeSquared() > StartDelta.SizeSquared())
					{
						JointSettings.LinearDrivePositionTarget = LinearDisplacement;
					}
				}
			}
		}
		if(bHasAngularPlasticityLimit)
		{
			FRotation3 Swing, Twist; FPBDJointUtilities::DecomposeSwingTwistLocal(ConstraintFramesGlobal[0].GetRotation(), ConstraintFramesGlobal[1].GetRotation(), Swing, Twist);

			// @todo(chaos): still need to warn against the case where all position drives are not enabled or all dimensions are locked. Warning should print out the joint names and should only print out once to avoid spamming.
			if((!JointSettings.bAngularSLerpPositionDriveEnabled && !JointSettings.bAngularTwistPositionDriveEnabled) || JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Locked)
			{
				Twist = FRotation3::Identity;
			}
			// @todo(chaos): clamp rotation if only swing1(swing2) is locked
			if((!JointSettings.bAngularSLerpPositionDriveEnabled && !JointSettings.bAngularSwingPositionDriveEnabled) || (JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] == EJointMotionType::Locked && JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] == EJointMotionType::Locked))
			{
				Swing = FRotation3::Identity;
			}
			
			const FRotation3 AngularDisplacement = Swing * Twist;
			// Assuming that the dimensions which are locked or have no targets are 0. in AngularDrivePositionTarget
			const FReal AngleDeg = JointSettings.AngularDrivePositionTarget.AngularDistance(AngularDisplacement);
			if (AngleDeg > JointSettings.AngularPlasticityLimit)
			{
				JointSettings.AngularDrivePositionTarget = AngularDisplacement;
			}
		}
	}


	// Assign an Island, Level and Color to each constraint. Constraints must be processed in Level order, but
	// constraints of the same color are independent and can be processed in parallel (SIMD or Task)
	// NOTE: Constraints are the Vertices in this graph, and Edges connect constraints sharing a Particle. 
	// This makes the coloring of constraints simpler, but might not be what you expect so keep that in mind! 
	void FPBDJointConstraints::ColorConstraints()
	{
		// Add a Vertex for all constraints involving at least one dynamic body
		// Maintain a map from Constraint Index to Vertex Index
		FColoringGraph Graph;
		TArray<int32> ConstraintVertices; // Map of ConstraintIndex -> VertexIndex
		Graph.ReserveVertices(NumConstraints());
		ConstraintVertices.SetNumZeroed(NumConstraints());
		for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints(); ++ConstraintIndex)
		{
			if (ConstraintStates[ConstraintIndex].bDisabled)
			{
				continue;
			}

			TPBDRigidParticleHandle<FReal, 3>* Particle0 = ConstraintParticles[ConstraintIndex][0]->CastToRigidParticle();
			TPBDRigidParticleHandle<FReal, 3>* Particle1 = ConstraintParticles[ConstraintIndex][1]->CastToRigidParticle();
			bool IsParticle0Dynamic = (Particle0 != nullptr) && (Particle0->ObjectState() == EObjectStateType::Dynamic || Particle0->ObjectState() == EObjectStateType::Sleeping);
			bool IsParticle1Dynamic = (Particle1 != nullptr) && (Particle1->ObjectState() == EObjectStateType::Dynamic || Particle1->ObjectState() == EObjectStateType::Sleeping);

			bool bContainsDynamic = IsParticle0Dynamic || IsParticle1Dynamic;
			if (bContainsDynamic)
			{
				ConstraintVertices[ConstraintIndex] = Graph.AddVertex();

				// Set kinematic-connected constraints to level 0 to initialize level calculation
				bool bContainsKinematic = !IsParticle0Dynamic || !IsParticle1Dynamic;
				if (bContainsKinematic)
				{
					Graph.SetVertexLevel(ConstraintVertices[ConstraintIndex], 0);
				}
			}
			else
			{
				// Constraint has no dynamics
				// This shouldn't happen often, but particles can change from dynamic to kinematic
				// and back again witout destroying joints, so it needs to be supported
				ConstraintVertices[ConstraintIndex] = INDEX_NONE;
			}
		}

		// Build a map of particles to constraints. We ignore non-dynamic particles since
		// two constraints that share only a static/kinematic particle will not interact.
		TMap<const FGeometryParticleHandle*, TArray<int32>> ParticleConstraints; // Map of ParticleHandle -> Constraint Indices involving the particle
		for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints(); ++ConstraintIndex)
		{
			if (ConstraintStates[ConstraintIndex].bDisabled)
			{
				continue;
			}

			const FConstGenericParticleHandle Particle0 = ConstraintParticles[ConstraintIndex][0];
			const FConstGenericParticleHandle Particle1 = ConstraintParticles[ConstraintIndex][1];
			
			if (Particle0->IsDynamic())
			{
				ParticleConstraints.FindOrAdd(Particle0->Handle()).Add(ConstraintIndex);
			}
			if (Particle1->IsDynamic())
			{
				ParticleConstraints.FindOrAdd(Particle1->Handle()).Add(ConstraintIndex);
			}
		}

		// Connect constraints that share a dynamic particle
		// Algorithm:
		//		Loop over particles
		//			Loop over all constraint pairs on that particle
		//				Add an edge to connect the constraints
		//
		Graph.ReserveEdges((ParticleConstraints.Num() * (ParticleConstraints.Num() - 1)) / 2);
		for (auto& ParticleConstraintsElement : ParticleConstraints)
		{
			// Loop over constraint pairs connected to the particle
			// Visit each pair only once (see inner loop indexing)
			const TArray<int32>& ParticleConstraintIndices = ParticleConstraintsElement.Value;
			const int32 NumParticleConstraintIndices = ParticleConstraintIndices.Num();
			for (int32 ParticleConstraintIndex0 = 0; ParticleConstraintIndex0 < NumParticleConstraintIndices; ++ParticleConstraintIndex0)
			{
				const int32 ConstraintIndex0 = ParticleConstraintIndices[ParticleConstraintIndex0];
				const int32 VertexIndex0 = ConstraintVertices[ConstraintIndex0];
				if(VertexIndex0 == INDEX_NONE)
				{
					// Constraint has no dynamics
					continue;
				}

				for (int32 ParticleConstraintIndex1 = ParticleConstraintIndex0 + 1; ParticleConstraintIndex1 < NumParticleConstraintIndices; ++ParticleConstraintIndex1)
				{
					const int32 ConstraintIndex1 = ParticleConstraintIndices[ParticleConstraintIndex1];
					const int32 VertexIndex1 = ConstraintVertices[ConstraintIndex1];
					if(VertexIndex1 == INDEX_NONE)
					{
						// Constraint has no dynamics
						continue;
					}
					Graph.AddEdge(VertexIndex0, VertexIndex1);
				}
			}
		}

		// Colorize the graph
		Graph.Islandize();
		Graph.Levelize();
		Graph.Colorize();

		// Set the constraint colors
		for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints(); ++ConstraintIndex)
		{
			if ( ConstraintStates[ConstraintIndex].bDisabled) continue;

			int32 VertexIndex = ConstraintVertices[ConstraintIndex];
			ConstraintStates[ConstraintIndex].Island = Graph.GetVertexIsland(VertexIndex);
			ConstraintStates[ConstraintIndex].IslandSize = Graph.GetVertexIslandSize(VertexIndex);
			ConstraintStates[ConstraintIndex].Level = Graph.GetVertexLevel(VertexIndex);
			ConstraintStates[ConstraintIndex].Color = Graph.GetVertexColor(VertexIndex);
		}
	}

}

namespace Chaos
{
	template class TContainerConstraintHandle<FPBDJointConstraints>;
}
