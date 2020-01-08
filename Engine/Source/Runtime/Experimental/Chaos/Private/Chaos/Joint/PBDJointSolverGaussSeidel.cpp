// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Joint/PBDJointSolverGaussSeidel.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDJointConstraintUtilities.h"
#include "Chaos/Utilities.h"
#include "ChaosLog.h"
#include "ChaosStats.h"

//#pragma optimize("", off)

// Set to true to calculate all constraint corrections based on state at the beginning of the iteration. This would eliminate the need to recalculate the inertia and a few other
// things in each constraint's Apply method. However, it probably introduces instability and requires more iterations to solve...
bool bChaos_Joint_Accumulate = false;
FAutoConsoleVariableRef CVarChaosImmPhysStepTime(TEXT("p.Chaos.Joint.Accumulate"), bChaos_Joint_Accumulate, TEXT("Whether to accumulate forces for each constraint over an iteration and apply all at once"));

namespace Chaos
{
	void GetSphericalAxisDelta(
		const FVec3& X0,
		const FVec3& X1,
		FVec3& Axis,
		FReal& Delta)
	{
		Axis = FVec3(0);
		Delta = 0;

		const FVec3 DX = X1 - X0;
		const FReal DXLen = DX.Size();
		if (DXLen > KINDA_SMALL_NUMBER)
		{
			Axis = DX / DXLen;
			Delta = DXLen;
		}
	}


	FReal GetSphericalError(
		const FReal& Delta,
		const FReal& Limit)
	{
		return FMath::Max((FReal)0, Delta - Limit);
	}


	void GetCylindricalAxisDelta(
		const FRotation3& R0,
		const FVec3& X0,
		const FVec3& X1,
		const int32 CylinderAxisIndex,
		FVec3& Axis,
		FReal& Delta)
	{
		Axis = FVec3(0);
		Delta = 0;

		const FMatrix33 R0M = R0.ToMatrix();
		const FVec3 CylinderAxis = R0M.GetAxis(CylinderAxisIndex);
		FVec3 DX = X1 - X0;
		DX = DX - FVec3::DotProduct(DX, CylinderAxis) * CylinderAxis;
		const FReal DXLen = DX.Size();
		if (DXLen > KINDA_SMALL_NUMBER)
		{
			Axis = DX / DXLen;
			Delta = DXLen;
		}
	}


	FReal GetCylindricalError(
		const FReal& Delta,
		const FReal& Limit)
	{
		return FMath::Max((FReal)0, Delta - Limit);
	}


	void GetPlanarAxisDelta(
		const FRotation3& R0,
		const FVec3& X0,
		const FVec3& X1,
		const int32 PlaneAxisIndex,
		FVec3& Axis,
		FReal& Delta)
	{
		const FMatrix33 R0M = R0.ToMatrix();
		Axis = R0M.GetAxis(PlaneAxisIndex);
		Delta = FVec3::DotProduct(X1 - X0, Axis);
	}


	FReal GetPlanarError(
		const FReal& Delta,
		const FReal& Limit)
	{
		if (Delta > Limit)
		{
			return Delta - Limit;
		}
		else if (Delta < Limit)
		{
			return Delta + Limit;
		}
		return 0;
	}


	void GetTwistAxisAngleLocal(
		const FRotation3& R0,
		const FRotation3& R1,
		FVec3& TwistAxisLocal,
		FReal& TwistAngle)
	{
		// Decompose rotation of body 1 relative to body 0 into swing and twist rotations, assuming twist is X axis
		FRotation3 R01Twist, R01Swing;
		FPBDJointUtilities::DecomposeSwingTwistLocal(R0, R1, R01Swing, R01Twist);

		TwistAxisLocal = FJointConstants::TwistAxis();

		TwistAngle = R01Twist.GetAngle();
		if (TwistAngle > PI)
		{
			TwistAngle = TwistAngle - (FReal)2 * PI;
		}
		if (R01Twist.X < 0)
		{
			TwistAngle = -TwistAngle;
		}
	}


	void GetConeAxisAngleLocal(
		const FRotation3& R0,
		const FRotation3& R1,
		const FReal AngleTolerance,
		FVec3& Axis,
		FReal& Angle)
	{
		// Decompose rotation of body 1 relative to body 0 into swing and twist rotations, assuming twist is X axis
		FRotation3 R01Twist, R01Swing;
		FPBDJointUtilities::DecomposeSwingTwistLocal(R0, R1, R01Swing, R01Twist);

		R01Swing.ToAxisAndAngleSafe(Axis, Angle, FJointConstants::Swing1Axis(), AngleTolerance);
		if (Angle > PI)
		{
			Angle = Angle - (FReal)2 * PI;
		}
	}


	FReal GetConeAngleError(
		const FPBDJointSettings& JointSettings,
		const FVec3& SwingAxisLocal,
		const FReal SwingAngle)
	{
		// Calculate swing limit for the current swing axis
		const FReal Swing1Limit = JointSettings.AngularLimits[(int32)EJointAngularConstraintIndex::Swing1];
		const FReal Swing2Limit = JointSettings.AngularLimits[(int32)EJointAngularConstraintIndex::Swing2];

		// Circular swing limit
		FReal SwingAngleMax = Swing1Limit;

		// Elliptical swing limit
		// @todo(ccaulfield): do elliptical constraints properly (axis is still for circular limit)
		if (!FMath::IsNearlyEqual(Swing1Limit, Swing2Limit, KINDA_SMALL_NUMBER))
		{
			// Map swing axis to ellipse and calculate limit for this swing axis
			const FReal DotSwing1 = FMath::Abs(FVec3::DotProduct(SwingAxisLocal, FJointConstants::Swing1Axis()));
			const FReal DotSwing2 = FMath::Abs(FVec3::DotProduct(SwingAxisLocal, FJointConstants::Swing2Axis()));
			SwingAngleMax = FMath::Sqrt(Swing1Limit * DotSwing1 * Swing1Limit * DotSwing1 + Swing2Limit * DotSwing2 * Swing2Limit * DotSwing2);
		}

		// Calculate swing error we need to correct
		if (SwingAngle > SwingAngleMax)
		{
			return SwingAngle - SwingAngleMax;
		}
		else if (SwingAngle < -SwingAngleMax)
		{
			return SwingAngle + SwingAngleMax;
		}

		return 0;
	}


	void GetSwingAxisAngle(
		const FRotation3& R0,
		const FRotation3& R1,
		const FReal AngleTolerance,
		const EJointAngularConstraintIndex SwingConstraintIndex,
		const EJointAngularAxisIndex SwingAxisIndex,
		FVec3& Axis,
		FReal& Angle)
	{
#if 0
		const FMatrix33 RM0 = R0.ToMatrix();
		const FMatrix33 RM1 = R1.ToMatrix();
		const FVec3& Twist0 = RM0.GetAxis((int32)EJointAngularAxisIndex::Twist);
		const FVec3& Twist1 = RM1.GetAxis((int32)EJointAngularAxisIndex::Twist);

		Axis = RM0.GetAxis((int32)SwingAxisIndex);
		FReal SinAngle = FVec3::DotProduct(FVec3::CrossProduct(Twist1, Twist0), Axis);
		Angle = FMath::Asin(FMath::Clamp(SinAngle, (FReal)-1, (FReal)1));
#elif 0
		// Decompose rotation of body 1 relative to body 0 into swing and twist rotations, assuming twist is X axis
		FRotation3 R01Twist, R01Swing;
		FPBDJointUtilities::DecomposeSwingTwistLocal(R0, R1, R01Swing, R01Twist);
		const FReal R01SwingYorZ = ((int32)SwingAxisIndex == 2) ? R01Swing.Z : R01Swing.Y;	// Can't index a quat :(
		FReal Angle = 4.0f * FMath::Atan2(R01SwingYorZ, 1.0f + R01Swing.W);
		if (R01Twist.X < 0)
		{
			if (Angle > 0)
			{
				Angle = (FReal)PI - Angle;
			}
			else
			{
				Angle = (FReal)-PI - Angle;
			}
		}
		const FVec3& AxisLocal = (SwingConstraintIndex == EJointAngularConstraintIndex::Swing1) ? FJointConstants::Swing1Axis() : FJointConstants::Swing2Axis();
		Axis = R0 * AxisLocal;
#else
		// Do something better than this!!
		FRotation3 R01Twist, R01Swing;
		FPBDJointUtilities::DecomposeSwingTwistLocal(R0, R1, R01Swing, R01Twist);
		FVec3 TwistAxis01 = FJointConstants::TwistAxis();
		FReal TwistAngle = R01Twist.GetAngle();
		if (TwistAngle > PI)
		{
			TwistAngle = TwistAngle - (FReal)2 * PI;
		}
		if (R01Twist.X < 0)
		{
			TwistAngle = -TwistAngle;
		}
		const FVec3 TwistAxis = R0 * TwistAxis01;

		const FRotation3 R1NoTwist = R1 * R01Twist.Inverse();
		const FMatrix33 Axes0 = R0.ToMatrix();
		const FMatrix33 Axes1 = R1NoTwist.ToMatrix();

		Axis = Axes0.GetAxis((int32)SwingAxisIndex);
		Angle = 0;

		const EJointAngularAxisIndex OtherSwingAxis = (SwingAxisIndex == EJointAngularAxisIndex::Swing1) ? EJointAngularAxisIndex::Swing2 : EJointAngularAxisIndex::Swing1;
		FVec3 SwingCross = FVec3::CrossProduct(Axes0.GetAxis((int32)OtherSwingAxis), Axes1.GetAxis((int32)OtherSwingAxis));
		SwingCross = SwingCross - FVec3::DotProduct(TwistAxis, SwingCross) * TwistAxis;
		const FReal SwingCrossLen = SwingCross.Size();
		if (SwingCrossLen > KINDA_SMALL_NUMBER)
		{
			Axis = SwingCross / SwingCrossLen;

			Angle = FMath::Asin(FMath::Clamp(SwingCrossLen, (FReal)0, (FReal)1));
			const FReal SwingDot = FVec3::DotProduct(Axes0.GetAxis((int32)OtherSwingAxis), Axes1.GetAxis((int32)OtherSwingAxis));
			if (SwingDot < (FReal)0)
			{
				Angle = (FReal)PI - Angle;
			}
		}
#endif
	}

	//
	//
	//////////////////////////////////////////////////////////////////////////
	//
	//

	FJointSolverGaussSeidel::FJointSolverGaussSeidel()
	{
	}


	void FJointSolverGaussSeidel::UpdateDerivedState()
	{
		Xs[0] = Ps[0] + Qs[0] * XLs[0].GetTranslation();
		Xs[1] = Ps[1] + Qs[1] * XLs[1].GetTranslation();
		Rs[0] = Qs[0] * XLs[0].GetRotation();
		Rs[1] = Qs[1] * XLs[1].GetRotation();
	}

	void FJointSolverGaussSeidel::UpdateDerivedState(const int32 BodyIndex)
	{
		Xs[BodyIndex] = Ps[BodyIndex] + Qs[BodyIndex] * XLs[BodyIndex].GetTranslation();
		Rs[BodyIndex] = Qs[BodyIndex] * XLs[BodyIndex].GetRotation();
	}


	void FJointSolverGaussSeidel::Init(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const FVec3& PrevP0,
		const FVec3& PrevP1,
		const FRotation3& PrevQ0,
		const FRotation3& PrevQ1,
		const FReal InvM0,
		const FMatrix33& InvIL0,
		const FReal InvM1,
		const FMatrix33& InvIL1,
		const FRigidTransform3& XL0,
		const FRigidTransform3& XL1)
	{
		XLs[0] = XL0;
		XLs[1] = XL1;

		// @todo(ccaulfield): mass conditioning
		InvILs[0] = JointSettings.ParentInvMassScale * InvIL0;
		InvILs[1] = InvIL1;
		InvMs[0] = JointSettings.ParentInvMassScale * InvM0;
		InvMs[1] = InvM1;

		PrevPs[0] = PrevP0;
		PrevPs[1] = PrevP1;
		PrevQs[0] = PrevQ0;
		PrevQs[1] = PrevQ1;
		PrevXs[0] = PrevP0 + PrevQ0 * XL0.GetTranslation();
		PrevXs[1] = PrevP1 + PrevQ1 * XL1.GetTranslation();

		LinearSoftLambda = (FReal)0;
		LinearDriveLambda = (FReal)0;
		TwistSoftLambda = (FReal)0;
		SwingSoftLambda = (FReal)0;
		TwistDriveLambda = (FReal)0;
		SwingDriveLambda = (FReal)0;

		// Disable the angular limit hardness improvement if linear limits are set up
		// @todo(ccaulfield): fix angular constraint position correction in ApplyRotationCorrection and ApplyRotationCorrectionSoft
		bEnableAngularConstraintPositionCorrection = ((JointSettings.LinearMotionTypes[0] == EJointMotionType::Locked) && (JointSettings.LinearMotionTypes[1] == EJointMotionType::Locked) && (JointSettings.LinearMotionTypes[2] == EJointMotionType::Locked));
	}


	void FJointSolverGaussSeidel::Update(
		const FReal Dt,
		const FVec3& P0,
		const FRotation3& Q0,
		const FVec3& V0,
		const FVec3& W0,
		const FVec3& P1,
		const FRotation3& Q1,
		const FVec3& V1,
		const FVec3& W1)
	{
		Ps[0] = P0;
		Ps[1] = P1;
		Qs[0] = Q0;
		Qs[1] = Q1;
		Qs[1].EnforceShortestArcWith(Qs[0]);

		Vs[0] = V0;
		Vs[1] = V1;
		Ws[0] = W0;
		Ws[1] = W1;

		UpdateDerivedState();
	}


	void FJointSolverGaussSeidel::InitAccumulatedDeltas()
	{
		DPs[0] = FVec3(0);
		DPs[1] = FVec3(0);
		DRs[0] = FVec3(0);
		DRs[1] = FVec3(0);
		DVs[0] = FVec3(0);
		DVs[1] = FVec3(0);
		DWs[0] = FVec3(0);
		DWs[1] = FVec3(0);
	}


	void FJointSolverGaussSeidel::ApplyConstraints(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		InitAccumulatedDeltas();

		ApplyRotationConstraints(Dt, SolverSettings, JointSettings);

		ApplyPositionConstraints(Dt, SolverSettings, JointSettings);
	
		ApplyAccumulatedDeltas();
	}


	void FJointSolverGaussSeidel::ApplyDrives(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		InitAccumulatedDeltas();

		ApplyRotationDrives(Dt, SolverSettings, JointSettings);

		ApplyPositionDrives(Dt, SolverSettings, JointSettings);

		ApplyAccumulatedDeltas();
	}


	void FJointSolverGaussSeidel::ApplyProjections(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		InitAccumulatedDeltas();

		// With the parent body set to infinite mass...
		ApplyRotationProjection(Dt, SolverSettings, JointSettings);

		ApplyPositionProjection(Dt, SolverSettings, JointSettings);

		ApplyAccumulatedDeltas();
	}


	void FJointSolverGaussSeidel::ApplyRotationConstraints(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// @todo(ccaulfield): eliminate branches in solver loop by pre-building list of functions to call?

		EJointMotionType TwistMotion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];
		bool bTwistSoft = JointSettings.bSoftTwistLimitsEnabled;
		bool bSwingSoft = JointSettings.bSoftSwingLimitsEnabled;

		// Apply twist constraint
		if (SolverSettings.bEnableTwistLimits)
		{
			if ((TwistMotion == EJointMotionType::Limited) && bTwistSoft)
			{
				ApplyTwistConstraintSoft(Dt, SolverSettings, JointSettings);
			}
			else if (TwistMotion != EJointMotionType::Free)
			{
				ApplyTwistConstraint(Dt, SolverSettings, JointSettings);
			}
		}

		// Apply swing constraints
		if (SolverSettings.bEnableSwingLimits)
		{
			if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited))
			{
				if (bSwingSoft)
				{
					ApplyConeConstraintSoft(Dt, SolverSettings, JointSettings);
				}
				else
				{
					ApplyConeConstraint(Dt, SolverSettings, JointSettings);
				}
			}
			else
			{
				
				if ((Swing1Motion == EJointMotionType::Limited) && bSwingSoft)
				{
					ApplySwingConstraintSoft(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1);
				}
				else if (Swing1Motion != EJointMotionType::Free)
				{
					ApplySwingConstraint(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1);
				}
				
				if ((Swing2Motion == EJointMotionType::Limited) && bSwingSoft)
				{
					ApplySwingConstraintSoft(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2);
				}
				else if (Swing2Motion != EJointMotionType::Free)
				{
					ApplySwingConstraint(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2);
				}
			}
		}
	}


	void FJointSolverGaussSeidel::ApplyRotationDrives(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		EJointMotionType TwistMotion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];

		if (SolverSettings.bEnableDrives)
		{
			bool bTwistLocked = TwistMotion == EJointMotionType::Locked;
			bool bSwing1Locked = Swing1Motion == EJointMotionType::Locked;
			bool bSwing2Locked = Swing2Motion == EJointMotionType::Locked;

			// No SLerp drive if we have a locked rotation (it will be grayed out in the editor in this case, but could still have been set before the rotation was locked)
			if ((JointSettings.bAngularSLerpPositionDriveEnabled || JointSettings.bAngularSLerpVelocityDriveEnabled) && !bTwistLocked && !bSwing1Locked && !bSwing2Locked)
			{
				ApplySLerpDrive(Dt, SolverSettings, JointSettings);
			}
			else
			{
				if ((JointSettings.bAngularTwistPositionDriveEnabled || JointSettings.bAngularTwistVelocityDriveEnabled) && !bTwistLocked)
				{
					ApplyTwistDrive(Dt, SolverSettings, JointSettings);
				}

				const bool bSwingDriveEnabled = (JointSettings.bAngularSwingPositionDriveEnabled || JointSettings.bAngularSwingVelocityDriveEnabled);
				if (bSwingDriveEnabled && !bSwing1Locked && !bSwing2Locked)
				{
					ApplyConeDrive(Dt, SolverSettings, JointSettings);
				}
				else if (bSwingDriveEnabled && !bSwing1Locked)
				{
					ApplySwingDrive(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1);
				}
				else if (bSwingDriveEnabled && !bSwing2Locked)
				{
					ApplySwingDrive(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2);
				}
			}
		}
	}


	void FJointSolverGaussSeidel::ApplyRotationProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		EJointMotionType TwistMotion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];
		bool bTwistSoft = JointSettings.bSoftTwistLimitsEnabled;
		bool bSwingSoft = JointSettings.bSoftSwingLimitsEnabled;

		if (SolverSettings.bEnableTwistLimits)
		{
			if ((TwistMotion == EJointMotionType::Locked) || ((TwistMotion == EJointMotionType::Limited) && !bTwistSoft))
			{
				ApplyTwistProjection(Dt, SolverSettings, JointSettings);
			}
		}

		if (SolverSettings.bEnableSwingLimits)
		{
			if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited) && !bSwingSoft)
			{
				ApplyConeProjection(Dt, SolverSettings, JointSettings);
			}
			else
			{
				if ((Swing1Motion == EJointMotionType::Locked) || ((Swing1Motion == EJointMotionType::Limited) && !bSwingSoft))
				{
					ApplySwingProjection(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1);
				}
				if ((Swing2Motion == EJointMotionType::Locked) || ((Swing2Motion == EJointMotionType::Limited) && !bSwingSoft))
				{
					ApplySwingProjection(Dt, SolverSettings, JointSettings, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2);
				}
			}
		}
	}


	void FJointSolverGaussSeidel::ApplyPositionConstraints(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const TVector<EJointMotionType, 3>& LinearMotion = JointSettings.LinearMotionTypes;

		// All 3 axes locked -> hard point constraint
		if ((LinearMotion[0] == EJointMotionType::Locked) && (LinearMotion[1] == EJointMotionType::Locked) && (LinearMotion[2] == EJointMotionType::Locked))
		{
			ApplyPointPositionConstraint(Dt, SolverSettings, JointSettings);
			return;
		}

		// At least one axes is limited or free
		const TVector<bool, 3> bLinearLimitSoft =
		{
			(LinearMotion[0] == EJointMotionType::Limited) && JointSettings.bSoftLinearLimitsEnabled,
			(LinearMotion[1] == EJointMotionType::Limited) && JointSettings.bSoftLinearLimitsEnabled,
			(LinearMotion[2] == EJointMotionType::Limited) && JointSettings.bSoftLinearLimitsEnabled,
		};
		const TVector<bool, 3> bLinearLimitHard =
		{
			(LinearMotion[0] == EJointMotionType::Locked) || ((LinearMotion[0] == EJointMotionType::Limited) && !JointSettings.bSoftLinearLimitsEnabled),
			(LinearMotion[1] == EJointMotionType::Locked) || ((LinearMotion[1] == EJointMotionType::Limited) && !JointSettings.bSoftLinearLimitsEnabled),
			(LinearMotion[2] == EJointMotionType::Locked) || ((LinearMotion[2] == EJointMotionType::Limited) && !JointSettings.bSoftLinearLimitsEnabled),
		};

		// Handle hard limits
		if (bLinearLimitHard[0] && bLinearLimitHard[1] && bLinearLimitHard[2])
		{
			ApplySphericalPositionConstraint(Dt, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[1] && bLinearLimitHard[2])
		{
			ApplyCylindricalPositionConstraint(Dt, 0, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[0] && bLinearLimitHard[2])
		{
			ApplyCylindricalPositionConstraint(Dt, 1, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[0] && bLinearLimitHard[1])
		{
			ApplyCylindricalPositionConstraint(Dt, 2, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[0])
		{
			ApplyPlanarPositionConstraint(Dt, 0, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[1])
		{
			ApplyPlanarPositionConstraint(Dt, 1, SolverSettings, JointSettings);
		}
		else if (bLinearLimitHard[2])
		{
			ApplyPlanarPositionConstraint(Dt, 2, SolverSettings, JointSettings);
		}

		// Handle soft limits
		if (bLinearLimitSoft[0] && bLinearLimitSoft[1] && bLinearLimitSoft[2])
		{
			ApplySphericalPositionConstraintSoft(Dt, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[1] && bLinearLimitSoft[2])
		{
			ApplyCylindricalPositionConstraintSoft(Dt, 0, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[0] && bLinearLimitSoft[2])
		{
			ApplyCylindricalPositionConstraintSoft(Dt, 1, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[0] && bLinearLimitSoft[1])
		{
			ApplyCylindricalPositionConstraintSoft(Dt, 2, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[0])
		{
			ApplyPlanarPositionConstraintSoft(Dt, 0, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[1])
		{
			ApplyPlanarPositionConstraintSoft(Dt, 1, SolverSettings, JointSettings);
		}
		else if (bLinearLimitSoft[2])
		{
			ApplyPlanarPositionConstraintSoft(Dt, 2, SolverSettings, JointSettings);
		}
	}


	void FJointSolverGaussSeidel::ApplyPositionDrives(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		if (SolverSettings.bEnableDrives)
		{
			TVector<bool, 3> bDriven =
			{
				(JointSettings.bLinearPositionDriveEnabled[0] || JointSettings.bLinearVelocityDriveEnabled[0]) && (JointSettings.LinearMotionTypes[0] != EJointMotionType::Locked),
				(JointSettings.bLinearPositionDriveEnabled[1] || JointSettings.bLinearVelocityDriveEnabled[1]) && (JointSettings.LinearMotionTypes[1] != EJointMotionType::Locked),
				(JointSettings.bLinearPositionDriveEnabled[2] || JointSettings.bLinearVelocityDriveEnabled[2]) && (JointSettings.LinearMotionTypes[2] != EJointMotionType::Locked),
			};

			if (bDriven[0] && bDriven[1] && bDriven[2])
			{
				ApplySphericalPositionDrive(Dt, SolverSettings, JointSettings);
			}
			else if (bDriven[1] && bDriven[2])
			{
				ApplyCylindricalPositionDrive(Dt, 0, SolverSettings, JointSettings);
			}
			else if (bDriven[0] && bDriven[2])
			{
				ApplyCylindricalPositionDrive(Dt, 1, SolverSettings, JointSettings);
			}
			else if (bDriven[0] && bDriven[1])
			{
				ApplyCylindricalPositionDrive(Dt, 2, SolverSettings, JointSettings);
			}
			else if (bDriven[0])
			{
				ApplyPlanarPositionDrive(Dt, 0, SolverSettings, JointSettings);
			}
			else if (bDriven[1])
			{
				ApplyPlanarPositionDrive(Dt, 1, SolverSettings, JointSettings);
			}
			else if (bDriven[2])
			{
				ApplyPlanarPositionDrive(Dt, 2, SolverSettings, JointSettings);
			}
		}
	}

	//
	//
	//////////////////////////////////////////////////////////////////////////
	//
	//

	void FJointSolverGaussSeidel::ApplyAccumulatedDeltas()
	{
		if (bChaos_Joint_Accumulate)
		{
			Ps[0] += DPs[0];
			Ps[1] += DPs[1];

			const FRotation3 DQ0 = (FRotation3::FromElements(DRs[0], 0) * Qs[0]) * (FReal)0.5;
			const FRotation3 DQ1 = (FRotation3::FromElements(DRs[1], 0) * Qs[1]) * (FReal)0.5;
			Qs[0] = (Qs[0] + DQ0).GetNormalized();
			Qs[1] = (Qs[1] + DQ1).GetNormalized();
			Qs[1].EnforceShortestArcWith(Qs[0]);

			Vs[0] += DVs[0];
			Vs[1] += DVs[1];

			Ws[0] += DWs[0];
			Ws[1] += DWs[1];

			UpdateDerivedState();
		}
	}

	void FJointSolverGaussSeidel::ApplyPositionDelta(
		const int32 BodyIndex,
		const FReal Stiffness,
		const FVec3& DP)
	{
		if (bChaos_Joint_Accumulate)
		{
			DPs[BodyIndex] += Stiffness * DP;
		}
		else
		{
			Ps[BodyIndex] += Stiffness * DP;
		}
	}


	void FJointSolverGaussSeidel::ApplyPositionDelta(
		const FReal Stiffness,
		const FVec3& DP0,
		const FVec3& DP1)
	{
		if (bChaos_Joint_Accumulate)
		{
			DPs[0] += Stiffness * DP0;
			DPs[1] += Stiffness * DP1;
		}
		else
		{
			Ps[0] += Stiffness * DP0;
			Ps[1] += Stiffness * DP1;
		}
	}


	void FJointSolverGaussSeidel::ApplyRotationDelta(
		const int32 BodyIndex,
		const FReal Stiffness,
		const FVec3& DR)
	{
		if (bChaos_Joint_Accumulate)
		{
			DRs[BodyIndex] = Stiffness * DR;
		}
		else
		{
			const FRotation3 DQ = (FRotation3::FromElements(Stiffness * DR, 0) * Qs[BodyIndex]) * (FReal)0.5;
			Qs[BodyIndex] = (Qs[BodyIndex] + DQ).GetNormalized();
		}
	}


	void FJointSolverGaussSeidel::ApplyRotationDelta(
		const FReal Stiffness,
		const FVec3& DR0,
		const FVec3& DR1)
	{
		if (bChaos_Joint_Accumulate)
		{
			DRs[0] += Stiffness * DR0;
			DRs[1] += Stiffness * DR1;
		}
		else
		{
			const FRotation3 DQ0 = (FRotation3::FromElements(Stiffness * DR0, 0) * Qs[0]) * (FReal)0.5;
			const FRotation3 DQ1 = (FRotation3::FromElements(Stiffness * DR1, 0) * Qs[1]) * (FReal)0.5;
			Qs[0] = (Qs[0] + DQ0).GetNormalized();
			Qs[1] = (Qs[1] + DQ1).GetNormalized();
			Qs[1].EnforceShortestArcWith(Qs[0]);
		}
	}


	void FJointSolverGaussSeidel::ApplyVelocityDelta(
		const int32 BodyIndex,
		const FReal Stiffness,
		const FVec3& DV,
		const FVec3& DW)
	{
		if (bChaos_Joint_Accumulate)
		{
			DVs[BodyIndex] += Stiffness * DV;
			DWs[BodyIndex] += Stiffness * DW;
		}
		else
		{
			Vs[BodyIndex] = Vs[BodyIndex] + Stiffness * DV;
			Ws[BodyIndex] = Ws[BodyIndex] + Stiffness * DW;
		}
	}


	void FJointSolverGaussSeidel::ApplyVelocityDelta(
		const FReal Stiffness,
		const FVec3& DV0,
		const FVec3& DW0,
		const FVec3& DV1,
		const FVec3& DW1)
	{
		if (bChaos_Joint_Accumulate)
		{
			DVs[0] += Stiffness * DV0;
			DVs[1] += Stiffness * DV1;
			DWs[0] += Stiffness * DW0;
			DWs[1] += Stiffness * DW1;
		}
		else
		{
			Vs[0] += Stiffness * DV0;
			Vs[1] += Stiffness * DV1;
			Ws[0] += Stiffness * DW0;
			Ws[1] += Stiffness * DW1;
		}
	}


	void FJointSolverGaussSeidel::ApplyPositionConstraint(
		const FReal Stiffness,
		const FVec3& Axis,
		const FReal Delta)
	{
		const FVec3 AngularAxis0 = FVec3::CrossProduct(Xs[0] - Ps[0], Axis);
		const FVec3 AngularAxis1 = FVec3::CrossProduct(Xs[1] - Ps[1], Axis);
		const FMatrix33 InvI0 = Utilities::ComputeWorldSpaceInertia(Qs[0], InvILs[0]);
		const FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);
		const FVec3 IA0 = Utilities::Multiply(InvI0, AngularAxis0);
		const FVec3 IA1 = Utilities::Multiply(InvI1, AngularAxis1);

		// Joint-space inverse mass
		const FReal II0 = FVec3::DotProduct(AngularAxis0, IA0);
		const FReal II1 = FVec3::DotProduct(AngularAxis1, IA1);
		const FReal IM = InvMs[0] + II0 + InvMs[1] + II1;

		const FVec3 DX = Axis * Delta / IM;

		// Apply constraint correction
		const FVec3 DP0 = InvMs[0] * DX;
		const FVec3 DP1 = -InvMs[1] * DX;
		const FVec3 DR0 = Utilities::Multiply(InvI0, FVec3::CrossProduct(Xs[0] - Ps[0], DX));
		const FVec3 DR1 = Utilities::Multiply(InvI1, FVec3::CrossProduct(Xs[1] - Ps[1], -DX));

		ApplyPositionDelta(Stiffness, DP0, DP1);
		ApplyRotationDelta(Stiffness, DR0, DR1);
		UpdateDerivedState();
	}


	void FJointSolverGaussSeidel::ApplyPositionConstraintSoft(
		const FReal Dt,
		const FReal Stiffness,
		const FReal Damping,
		const bool bAccelerationMode,
		const FVec3& Axis,
		const FReal Delta,
		FReal& Lambda)
	{
		// World-space inverse mass
		const FMatrix33 InvI0 = Utilities::ComputeWorldSpaceInertia(Qs[0], InvILs[0]);
		const FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);

		// Joint-space inverse mass
		const FVec3 AngularAxis0 = FVec3::CrossProduct(Xs[0] - Ps[0], Axis);
		const FVec3 AngularAxis1 = FVec3::CrossProduct(Xs[1] - Ps[1], Axis);
		const FVec3 IA0 = Utilities::Multiply(InvI0, AngularAxis0);
		const FVec3 IA1 = Utilities::Multiply(InvI1, AngularAxis1);
		const FReal II0 = FVec3::DotProduct(AngularAxis0, IA0);
		const FReal II1 = FVec3::DotProduct(AngularAxis1, IA1);
		const FReal II = (InvMs[0] + II0 + InvMs[1] + II1);

		FReal VelDt = 0;
		if (Damping > KINDA_SMALL_NUMBER)
		{
			const FVec3 V0 = FVec3::CalculateVelocity(PrevXs[0], Xs[0], (FReal)1.0);
			const FVec3 V1 = FVec3::CalculateVelocity(PrevXs[1], Xs[1], (FReal)1.0);
			VelDt = FVec3::DotProduct(V0 - V1, Axis);
		}

		FReal DLambda = 0;
		if (Stiffness > KINDA_SMALL_NUMBER)
		{
			// As below, but numerically stable for large values of stiffness (same form as in XPBD paper)
			const FReal Alpha = (FReal)1 / (Stiffness * Dt * Dt);
			const FReal Gamma = Damping / (Stiffness * Dt);
			if (bAccelerationMode)
			{
				const FReal Multiplier = (FReal)1 / (((FReal)1 + Gamma) + Alpha);
				DLambda = Multiplier * (Delta - Gamma * VelDt) / II - Multiplier * Alpha * Lambda;
			}
			else
			{
				const FReal Multiplier = (FReal)1 / (((FReal)1 + Gamma) * II + Alpha);
				DLambda = Multiplier * (Delta - Gamma * VelDt - Alpha * Lambda);
			}
		}
		else
		{
			// As above, but numerically stable at low stiffness (alpha -> infinity)
			const FReal S = Stiffness * Dt * Dt;
			const FReal D = Damping * Dt;
			if (bAccelerationMode)
			{
				const FReal Multiplier = (FReal)1 / ((S + D) + (FReal)1);
				DLambda = Multiplier * (S * Delta - D * VelDt) / II - Multiplier * Lambda;
			}
			else
			{
				const FReal Multiplier = (FReal)1 / ((S + D) * II + (FReal)1);
				DLambda = Multiplier * (S * Delta - D * VelDt - Lambda);
			}
		}
	
		const FVec3 DP0 = (InvMs[0] * DLambda) * Axis;
		const FVec3 DP1 = (-InvMs[1] * DLambda) * Axis;
		const FVec3 DR0 = DLambda * Utilities::Multiply(InvI0, AngularAxis0);
		const FVec3 DR1 = -DLambda * Utilities::Multiply(InvI1, AngularAxis1);

		Lambda += DLambda;
		ApplyPositionDelta((FReal)1, DP0, DP1);
		ApplyRotationDelta((FReal)1, DR0, DR1);
		UpdateDerivedState();
	}
	
	
	void FJointSolverGaussSeidel::ApplyRotationConstraint(
		const FReal Stiffness,
		const FVec3& Axis0,
		const FVec3& Axis1,
		const FReal Angle)
	{
		// World-space inverse mass
		const FMatrix33 InvI0 = Utilities::ComputeWorldSpaceInertia(Qs[0], InvILs[0]);
		const FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);
		const FVec3 IA0 = Utilities::Multiply(InvI0, Axis0);
		const FVec3 IA1 = Utilities::Multiply(InvI1, Axis1);

		// Joint-space inverse mass
		const FReal II0 = FVec3::DotProduct(Axis0, IA0);
		const FReal II1 = FVec3::DotProduct(Axis1, IA1);

		const FVec3 DR0 = IA0 * (Angle / (II0 + II1));
		const FVec3 DR1 = IA1 * -(Angle / (II0 + II1));
		//const FVec3 DR0 = Axis0 * (Angle * II0 / (II0 + II1));
		//const FVec3 DR1 = Axis1 * -(Angle * II1 / (II0 + II1));

		// @todo(ccaulfield): this position correction needs to have components in direction of inactive position constraints removed
		if (bEnableAngularConstraintPositionCorrection)
		{
			const FReal IM0 = InvMs[0];
			const FReal IM1 = InvMs[1];
			const FVec3 DX = FVec3::CrossProduct(DR0, Xs[0] - Ps[0]) + FVec3::CrossProduct(DR1, Xs[1] - Ps[1]);
			const FVec3 DP0 = DX * (IM0 / (IM0 + IM1));
			const FVec3 DP1 = DX * (-IM1 / (IM0 + IM1));
			ApplyPositionDelta(Stiffness, DP0, DP1);
		}

		ApplyRotationDelta(Stiffness, DR0, DR1);
		UpdateDerivedState();
	}


	// See "XPBD: Position-Based Simulation of Compliant Constrained Dynamics"
	void FJointSolverGaussSeidel::ApplyRotationConstraintSoft(
		const FReal Dt,
		const FReal Stiffness,
		const FReal Damping,
		const bool bAccelerationMode,
		const FVec3& Axis0,
		const FVec3& Axis1,
		const FReal Angle,
		FReal& Lambda)
	{
		// World-space inverse mass
		const FMatrix33 InvI0 = Utilities::ComputeWorldSpaceInertia(Qs[0], InvILs[0]);
		const FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);
		const FVec3 IA0 = Utilities::Multiply(InvI0, Axis0);
		const FVec3 IA1 = Utilities::Multiply(InvI1, Axis1);

		// Joint-space inverse mass
		const FReal II0 = FVec3::DotProduct(Axis0, IA0);
		const FReal II1 = FVec3::DotProduct(Axis1, IA1);
		const FReal II = (II0 + II1);

		// Damping angular velocity
		FReal AngVelDt = 0;
		if (Damping > KINDA_SMALL_NUMBER)
		{
			const FVec3 W0 = FRotation3::CalculateAngularVelocity(PrevQs[0], Qs[0], (FReal)1.0);
			const FVec3 W1 = FRotation3::CalculateAngularVelocity(PrevQs[1], Qs[1], (FReal)1.0);
			AngVelDt = FVec3::DotProduct(Axis0, W0) - FVec3::DotProduct(Axis1, W1);
		}

		FReal DLambda = 0;
		if (Stiffness > KINDA_SMALL_NUMBER)
		{
			// As below, but numerically stable for large values of stiffness (same form as in XPBD paper)
			const FReal Alpha = (FReal)1 / (Stiffness * Dt * Dt);
			const FReal Gamma = Damping / (Stiffness * Dt);
			if (bAccelerationMode)
			{
				const FReal Multiplier = (FReal)1 / (((FReal)1 + Gamma) + Alpha);
				DLambda = Multiplier * (Angle - Gamma * AngVelDt) / II - Multiplier * Alpha * Lambda;
			}
			else
			{
				const FReal Multiplier = (FReal)1 / (((FReal)1 + Gamma) * II + Alpha);
				DLambda = Multiplier * (Angle - Gamma * AngVelDt - Alpha * Lambda);
			}
		}
		else
		{
			// As above, but numerically stable at low stiffness (alpha -> infinity)
			const FReal S = Stiffness * Dt * Dt;
			const FReal D = Damping * Dt;
			if (bAccelerationMode)
			{
				const FReal Multiplier = (FReal)1 / ((S + D) + (FReal)1);
				DLambda = Multiplier * (S * Angle - D * AngVelDt) / II - Multiplier * Lambda;
			}
			else
			{
				const FReal Multiplier = (FReal)1 / ((S + D) * II + (FReal)1);
				DLambda = Multiplier * (S * Angle - D * AngVelDt - Lambda);
			}
		}

		//const FVec3 DR0 = IA0 * DLambda;
		//const FVec3 DR1 = IA1 * -DLambda;
		const FVec3 DR0 = Axis0 * (DLambda * II0);
		const FVec3 DR1 = Axis1 * -(DLambda * II1);

		// @todo(ccaulfield): this position correction needs to have components in direction of inactive position constraints removed
		if (bEnableAngularConstraintPositionCorrection)
		{
			const FReal IM0 = InvMs[0];
			const FReal IM1 = InvMs[1];
			const FVec3 DX = FVec3::CrossProduct(DR0, Xs[0] - Ps[0]) + FVec3::CrossProduct(DR1, Xs[1] - Ps[1]);
			const FVec3 DP0 = DX * (IM0 / (IM0 + IM1));
			const FVec3 DP1 = DX * (-IM1 / (IM0 + IM1));
			ApplyPositionDelta(1.0f, DP0, DP1);
		}

		Lambda += DLambda;
		ApplyRotationDelta(1.0f, DR0, DR1);
		UpdateDerivedState();
	}


	//
	//
	//////////////////////////////////////////////////////////////////////////
	//
	//


	void FJointSolverGaussSeidel::ApplyTwistConstraint(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 TwistAxisLocal;
		FReal TwistAngle;
		GetTwistAxisAngleLocal(Rs[0], Rs[1], TwistAxisLocal, TwistAngle);
		
		const FVec3 TwistAxis0 = Rs[0] * TwistAxisLocal;
		const FVec3 TwistAxis1 = Rs[1] * TwistAxisLocal;

		FReal TwistAngleMax = FLT_MAX;
		if (JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Limited)
		{
			TwistAngleMax = JointSettings.AngularLimits[(int32)EJointAngularConstraintIndex::Twist];
		}
		else if (JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Locked)
		{
			TwistAngleMax = 0;
		}

		// Calculate the twist correction to apply to each body
		FReal DTwistAngle = 0;
		if (TwistAngle > TwistAngleMax)
		{
			DTwistAngle = TwistAngle - TwistAngleMax;
		}
		else if (TwistAngle < -TwistAngleMax)
		{
			DTwistAngle = TwistAngle + TwistAngleMax;
		}

		// Apply twist correction
		if (DTwistAngle != 0)
		{
			FReal TwistStiffness = FPBDJointUtilities::GetTwistStiffness(SolverSettings, JointSettings);
			ApplyRotationConstraint(TwistStiffness, TwistAxis0, TwistAxis1, DTwistAngle);
		}
	}


	void FJointSolverGaussSeidel::ApplyTwistConstraintSoft(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 TwistAxisLocal;
		FReal TwistAngle;
		GetTwistAxisAngleLocal(Rs[0], Rs[1], TwistAxisLocal, TwistAngle);

		const FVec3 TwistAxis0 = Rs[0] * TwistAxisLocal;
		const FVec3 TwistAxis1 = Rs[1] * TwistAxisLocal;

		FReal TwistAngleMax = FLT_MAX;
		if (JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Limited)
		{
			TwistAngleMax = JointSettings.AngularLimits[(int32)EJointAngularConstraintIndex::Twist];
		}
		else if (JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Locked)
		{
			TwistAngleMax = 0;
		}

		// Calculate the twist correction to apply to each body
		FReal DTwistAngle = 0;
		if (TwistAngle > TwistAngleMax)
		{
			DTwistAngle = TwistAngle - TwistAngleMax;
		}
		else if (TwistAngle < -TwistAngleMax)
		{
			DTwistAngle = TwistAngle + TwistAngleMax;
		}

		// Apply twist correction
		if (DTwistAngle != 0)
		{
			const FReal TwistStiffness = FPBDJointUtilities::GetSoftTwistStiffness(SolverSettings, JointSettings);
			const FReal TwistDamping = FPBDJointUtilities::GetSoftTwistDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetAngularSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyRotationConstraintSoft(Dt, TwistStiffness, TwistDamping, bAccelerationMode, TwistAxis0, TwistAxis1, DTwistAngle, TwistSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyTwistDrive(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Decompose rotation of body 1 relative to body 0 into swing and twist rotations, assuming twist is X axis
		FRotation3 R01Twist, R01Swing;
		FPBDJointUtilities::DecomposeSwingTwistLocal(Rs[0], Rs[1], R01Swing, R01Twist);

		FVec3 TwistAxis01 = FJointConstants::TwistAxis();
		FReal TwistAngle = R01Twist.GetAngle();
		if (TwistAngle > PI)
		{
			TwistAngle = TwistAngle - (FReal)2 * PI;
		}
		if (R01Twist.X < 0)
		{
			TwistAngle = -TwistAngle;
		}

		const FVec3 TwistAxis0 = Rs[0] * TwistAxis01;
		const FVec3 TwistAxis1 = Rs[1] * TwistAxis01;
		const FReal TwistAngleTarget = JointSettings.AngularDriveTargetAngles[(int32)EJointAngularConstraintIndex::Twist];
		const FReal DTwistAngle = TwistAngle - TwistAngleTarget;

		// Apply twist correction
		const FReal AngularDriveStiffness = FPBDJointUtilities::GetAngularTwistDriveStiffness(SolverSettings, JointSettings);
		const FReal AngularDriveDamping = FPBDJointUtilities::GetAngularTwistDriveDamping(SolverSettings, JointSettings);
		const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
		ApplyRotationConstraintSoft(Dt, AngularDriveStiffness, AngularDriveDamping, bAccelerationMode, TwistAxis0, TwistAxis1, DTwistAngle, TwistDriveLambda);
	}


	void FJointSolverGaussSeidel::ApplyTwistProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// @todo(ccaulfield): implement twist projection
	}


	void FJointSolverGaussSeidel::ApplyConeConstraint(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Calculate swing angle and axis
		FReal SwingAngle;
		FVec3 SwingAxisLocal;
		GetConeAxisAngleLocal(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingAxisLocal, SwingAngle);
		const FVec3 SwingAxis = Rs[0] * SwingAxisLocal;

		// Calculate swing angle error
		FReal DSwingAngle = GetConeAngleError(JointSettings, SwingAxisLocal, SwingAngle);

		// Apply swing correction to each body
		if (DSwingAngle != 0)
		{
			FReal SwingStiffness = FPBDJointUtilities::GetSwingStiffness(SolverSettings, JointSettings);
			ApplyRotationConstraint(SwingStiffness, SwingAxis, SwingAxis, DSwingAngle);
		}
	}


	void FJointSolverGaussSeidel::ApplyConeConstraintSoft(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Calculate swing angle and axis
		FReal SwingAngle;
		FVec3 SwingAxisLocal;
		GetConeAxisAngleLocal(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingAxisLocal, SwingAngle);
		const FVec3 SwingAxis = Rs[0] * SwingAxisLocal;

		// Calculate swing angle error
		const FReal DSwingAngle = GetConeAngleError(JointSettings, SwingAxisLocal, SwingAngle);

		// Apply swing correction to each body
		if (DSwingAngle != 0)
		{
			const FReal SoftSwingStiffness = FPBDJointUtilities::GetSoftSwingStiffness(SolverSettings, JointSettings);
			const FReal SoftSwingDamping = FPBDJointUtilities::GetSoftSwingDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetAngularSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyRotationConstraintSoft(Dt, SoftSwingStiffness, SoftSwingDamping, bAccelerationMode, SwingAxis, SwingAxis, DSwingAngle, SwingSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyConeDrive(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Calculate swing angle and axis
		FReal SwingAngle;
		FVec3 SwingAxisLocal;
		GetConeAxisAngleLocal(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingAxisLocal, SwingAngle);
		const FVec3 SwingAxis = Rs[0] * SwingAxisLocal;

		// Circular swing target (max of Swing1, Swing2 targets)
		// @todo(ccaulfield): what should cone target really do?
		const FReal Swing1Target = JointSettings.AngularDriveTargetAngles[(int32)EJointAngularConstraintIndex::Swing1];
		const FReal Swing2Target = JointSettings.AngularDriveTargetAngles[(int32)EJointAngularConstraintIndex::Swing2];
		const FReal SwingAngleTarget = FMath::Max(Swing1Target, Swing2Target);
		const FReal DSwingAngle = SwingAngle - SwingAngleTarget;

		// Apply drive forces to each body
		const FReal AngularDriveStiffness = FPBDJointUtilities::GetAngularSwingDriveStiffness(SolverSettings, JointSettings);
		const FReal AngularDriveDamping = FPBDJointUtilities::GetAngularSwingDriveDamping(SolverSettings, JointSettings);
		const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
		ApplyRotationConstraintSoft(Dt, AngularDriveStiffness, AngularDriveDamping, bAccelerationMode, SwingAxis, SwingAxis, DSwingAngle, SwingDriveLambda);
	}


	void FJointSolverGaussSeidel::ApplyConeProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Calculate swing angle and axis
		FReal SwingAngle;
		FVec3 SwingAxisLocal;
		GetConeAxisAngleLocal(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingAxisLocal, SwingAngle);
		const FVec3 SwingAxis = Rs[0] * SwingAxisLocal;

		// Calculate swing angle error
		const FReal DSwingAngle = GetConeAngleError(JointSettings, SwingAxisLocal, SwingAngle);

		// Apply rotation to body 1, set relative angular velocity to zero, if it would increase the error
		if (DSwingAngle != 0)
		{
			const FReal AngularProjection = FPBDJointUtilities::GetAngularProjection(SolverSettings, JointSettings);

			const FVec3 DR1 = -DSwingAngle * SwingAxis;
			const FVec3 DP1 = -FVec3::CrossProduct(DR1, Xs[1] - Ps[1]);
			FVec3 DV1 = FVec3(0);
			FVec3 DW1 = FVec3(0);

			// Remove relative angular velocity if it would increase the constraint error
			const FReal WAxis = FVec3::DotProduct(Ws[1] - Ws[0], SwingAxis);
			if (FMath::Sign(WAxis) == FMath::Sign(DSwingAngle))
			{
				DW1 = -WAxis * SwingAxis;
			}

			// Remove relative velocity if it would increase the constraint error
			FReal DP1Len = DP1.Size();
			if (DP1Len > KINDA_SMALL_NUMBER)
			{
				const FVec3 DP1Dir = DP1 / DP1Len;
				const FVec3 V0 = Vs[0] + FVec3::CrossProduct(Ws[0], Xs[0] - Ps[0] + Ps[1] - Xs[1]);
				const FVec3 V1 = Vs[1];
				const FReal VAxis = FVec3::DotProduct(V1 - V0, DP1Dir);
				if (VAxis < 0)
				{
					DV1 = -VAxis * DP1Dir;
				}
			}

			ApplyRotationDelta(1, AngularProjection, DR1);
			ApplyPositionDelta(1, AngularProjection, DP1);
			ApplyVelocityDelta(1, AngularProjection, DV1, DW1);
			UpdateDerivedState(1);
		}
	}


	void FJointSolverGaussSeidel::ApplySwingConstraint(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const EJointAngularConstraintIndex SwingConstraintIndex,
		const EJointAngularAxisIndex SwingAxisIndex)
	{
		FVec3 SwingAxis;
		FReal SwingAngle;
		GetSwingAxisAngle(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingConstraintIndex, SwingAxisIndex, SwingAxis, SwingAngle);

		const bool isLocked = (JointSettings.AngularMotionTypes[(int32)SwingConstraintIndex] == EJointMotionType::Locked);
		const FReal SwingAngleMax = isLocked ? 0 : JointSettings.AngularLimits[(int32)SwingConstraintIndex];

		// Calculate swing error we need to correct
		FReal DSwingAngle = 0;
		if (SwingAngle > SwingAngleMax)
		{
			DSwingAngle = SwingAngle - SwingAngleMax;
		}
		else if (SwingAngle < -SwingAngleMax)
		{
			DSwingAngle = SwingAngle + SwingAngleMax;
		}

		// Apply swing correction
		if (DSwingAngle != 0)
		{
			const FReal SwingStiffness = FPBDJointUtilities::GetSwingStiffness(SolverSettings, JointSettings);
			ApplyRotationConstraint(SwingStiffness, SwingAxis, SwingAxis, DSwingAngle);
		}
	}


	void FJointSolverGaussSeidel::ApplySwingConstraintSoft(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const EJointAngularConstraintIndex SwingConstraintIndex,
		const EJointAngularAxisIndex SwingAxisIndex)
	{
		FVec3 SwingAxis;
		FReal SwingAngle;
		GetSwingAxisAngle(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingConstraintIndex, SwingAxisIndex, SwingAxis, SwingAngle);

		const FReal SwingAngleMax = JointSettings.AngularLimits[(int32)SwingConstraintIndex];

		// Calculate swing error we need to correct
		FReal DSwingAngle = 0;
		if (SwingAngle > SwingAngleMax)
		{
			DSwingAngle = SwingAngle - SwingAngleMax;
		}
		else if (SwingAngle < -SwingAngleMax)
		{
			DSwingAngle = SwingAngle + SwingAngleMax;
		}

		// Apply swing correction
		if (DSwingAngle != 0)
		{
			const FReal SoftSwingStiffness = FPBDJointUtilities::GetSoftSwingStiffness(SolverSettings, JointSettings);
			const FReal SoftSwingDamping = FPBDJointUtilities::GetSoftSwingDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetAngularSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyRotationConstraintSoft(Dt, SoftSwingStiffness, SoftSwingDamping, bAccelerationMode, SwingAxis, SwingAxis, DSwingAngle, SwingSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplySwingDrive(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const EJointAngularConstraintIndex SwingConstraintIndex,
		const EJointAngularAxisIndex SwingAxisIndex)
	{
		FVec3 SwingAxis;
		FReal SwingAngle;
		GetSwingAxisAngle(Rs[0], Rs[1], SolverSettings.SwingTwistAngleTolerance, SwingConstraintIndex, SwingAxisIndex, SwingAxis, SwingAngle);
	
		const FReal SwingAngleTarget = JointSettings.AngularDriveTargetAngles[(int32)SwingConstraintIndex];
		const FReal DSwingAngle = SwingAngle - SwingAngleTarget;

		// Apply drive forces to each body
		const FReal AngularDriveStiffness = FPBDJointUtilities::GetAngularSwingDriveStiffness(SolverSettings, JointSettings);
		const FReal AngularDriveDamping = FPBDJointUtilities::GetAngularSwingDriveDamping(SolverSettings, JointSettings);
		const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
		ApplyRotationConstraintSoft(Dt, AngularDriveStiffness, AngularDriveDamping, bAccelerationMode, SwingAxis, SwingAxis, DSwingAngle, SwingDriveLambda);
	}


	void FJointSolverGaussSeidel::ApplySwingProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const EJointAngularConstraintIndex SwingConstraintIndex,
		const EJointAngularAxisIndex SwingAxisIndex)
	{
		// @todo(ccaulfield): implement swing projection
	}


	void FJointSolverGaussSeidel::ApplySLerpDrive(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Calculate the rotation we need to apply to resolve the rotation delta
		const FRotation3 TargetR1 = Rs[0] * JointSettings.AngularDrivePositionTarget;
		const FRotation3 DR1 = TargetR1 * Rs[1].Inverse();

		FVec3 SLerpAxis;
		FReal SLerpAngle;
		if (DR1.ToAxisAndAngleSafe(SLerpAxis, SLerpAngle, FVec3(1, 0, 0)))
		{
			if (SLerpAngle > (FReal)PI)
			{
				SLerpAngle = SLerpAngle - (FReal)2 * PI;
			}

			const FReal AngularDriveStiffness = FPBDJointUtilities::GetAngularSLerpDriveStiffness(SolverSettings, JointSettings);
			const FReal AngularDriveDamping = FPBDJointUtilities::GetAngularSLerpDriveDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
			ApplyRotationConstraintSoft(Dt, AngularDriveStiffness, AngularDriveDamping, bAccelerationMode, SLerpAxis, SLerpAxis, -SLerpAngle, SwingDriveLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyPointPositionConstraint(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FVec3 CX = Xs[1] - Xs[0];
		if (CX != FVec3(0))
		{
			const FMatrix33 InvI0 = Utilities::ComputeWorldSpaceInertia(Qs[0], InvILs[0]);
			const FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);

			// Calculate constraint correction
			FMatrix33 M0 = FMatrix33(0, 0, 0);
			FMatrix33 M1 = FMatrix33(0, 0, 0);
			if (InvMs[0] > 0)
			{
				M0 = Utilities::ComputeJointFactorMatrix(Xs[0] - Ps[0], InvI0, InvMs[0]);
			}
			if (InvMs[1] > 0)
			{
				M1 = Utilities::ComputeJointFactorMatrix(Xs[1] - Ps[1], InvI1, InvMs[1]);
			}
			const FMatrix33 MI = (M0 + M1).Inverse();
			const FVec3 DX = Utilities::Multiply(MI, CX);

			// Apply constraint correction
			const FVec3 DP0 = InvMs[0] * DX;
			const FVec3 DP1 = -InvMs[1] * DX;
			const FVec3 DR0 = Utilities::Multiply(InvI0, FVec3::CrossProduct(Xs[0] - Ps[0], DX));
			const FVec3 DR1 = Utilities::Multiply(InvI1, FVec3::CrossProduct(Xs[1] - Ps[1], -DX));

			FReal LinearStiffness = FPBDJointUtilities::GetLinearStiffness(SolverSettings, JointSettings);
			ApplyPositionDelta(LinearStiffness, DP0, DP1);
			ApplyRotationDelta(LinearStiffness, DR0, DR1);
			UpdateDerivedState();
		}
	}


	void FJointSolverGaussSeidel::ApplySphericalPositionConstraint(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetSphericalAxisDelta(Xs[0], Xs[1], Axis, Delta);
		const FReal Error = GetSphericalError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearStiffness(SolverSettings, JointSettings);
			ApplyPositionConstraint(Stiffness, Axis, Error);
		}
	}


	void FJointSolverGaussSeidel::ApplySphericalPositionConstraintSoft(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetSphericalAxisDelta(Xs[0], Xs[1], Axis, Delta);
		const FReal Error = GetSphericalError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetSoftLinearStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetSoftLinearDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetLinearSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Error, LinearSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplySphericalPositionDrive(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FVec3 XTarget = Xs[0] + Rs[0] * JointSettings.LinearDriveTarget;
		FVec3 Axis;
		FReal Delta;
		GetSphericalAxisDelta(XTarget, Xs[1], Axis, Delta);
		if (Delta != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearDriveStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetLinearDriveDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Delta, LinearDriveLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyCylindricalPositionConstraint(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetCylindricalAxisDelta(Rs[0], Xs[0], Xs[1], AxisIndex, Axis, Delta);
		const FReal Error = GetCylindricalError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearStiffness(SolverSettings, JointSettings);
			ApplyPositionConstraint(Stiffness, Axis, Error);
		}
	}


	void FJointSolverGaussSeidel::ApplyCylindricalPositionConstraintSoft(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetCylindricalAxisDelta(Rs[0], Xs[0], Xs[1], AxisIndex, Axis, Delta);
		const FReal Error = GetCylindricalError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetSoftLinearStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetSoftLinearDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetLinearSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Error, LinearSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyCylindricalPositionDrive(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FVec3 XTarget = Xs[0] + Rs[0] * JointSettings.LinearDriveTarget;
		FVec3 Axis;
		FReal Delta;
		GetCylindricalAxisDelta(Rs[0], XTarget, Xs[1], AxisIndex, Axis, Delta);
		if (Delta != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearDriveStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetLinearDriveDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Delta, LinearDriveLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyPlanarPositionConstraint(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetPlanarAxisDelta(Rs[0], Xs[0], Xs[1], AxisIndex, Axis, Delta);
		const FReal Error = GetPlanarError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearStiffness(SolverSettings, JointSettings);
			ApplyPositionConstraint(Stiffness, Axis, Error);
		}
	}


	void FJointSolverGaussSeidel::ApplyPlanarPositionConstraintSoft(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		FVec3 Axis;
		FReal Delta;
		GetPlanarAxisDelta(Rs[0], Xs[0], Xs[1], AxisIndex, Axis, Delta);
		const FReal Error = GetPlanarError(Delta, JointSettings.LinearLimit);
		if (Error != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetSoftLinearStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetSoftLinearDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetLinearSoftAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Error, LinearSoftLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyPlanarPositionDrive(
		const FReal Dt,
		const int32 AxisIndex,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FVec3 XTarget = Xs[0] + Rs[0] * JointSettings.LinearDriveTarget;
		FVec3 Axis;
		FReal Delta;
		GetPlanarAxisDelta(Rs[0], XTarget, Xs[1], AxisIndex, Axis, Delta);
		if (Delta != 0)
		{
			const FReal Stiffness = FPBDJointUtilities::GetLinearDriveStiffness(SolverSettings, JointSettings);
			const FReal Damping = FPBDJointUtilities::GetLinearDriveDamping(SolverSettings, JointSettings);
			const bool bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);
			ApplyPositionConstraintSoft(Dt, Stiffness, Damping, bAccelerationMode, Axis, Delta, LinearDriveLambda);
		}
	}


	void FJointSolverGaussSeidel::ApplyPositionProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		// Apply a position correction with the parent body set to infinite mass, then correct the velocity.
		FVec3 CX = FPBDJointUtilities::GetLimitedPositionError(JointSettings, Rs[0], Xs[1] - Xs[0]);
		const FReal CXLen = CX.Size();
		if (CXLen > KINDA_SMALL_NUMBER)
		{
			const FVec3 CXDir = CX / CXLen;
			const FVec3 V0 = Vs[0] + FVec3::CrossProduct(Ws[0], Xs[0] - Ps[0]);
			const FVec3 V1 = Vs[1] + FVec3::CrossProduct(Ws[1], Xs[1] - Ps[1]);
			FVec3 CV = FVec3::DotProduct(V1 - V0, CXDir) * CXDir;

			FMatrix33 InvI1 = Utilities::ComputeWorldSpaceInertia(Qs[1], InvILs[1]);
			FMatrix33 M1 = Utilities::ComputeJointFactorMatrix(Xs[1] - Ps[1], InvI1, InvMs[1]);
			FMatrix33 MI = M1.Inverse();

			const FVec3 DX = Utilities::Multiply(MI, CX);
			const FVec3 DV = Utilities::Multiply(MI, CV);

			const FVec3 DP1 = -InvMs[1] * DX;
			const FVec3 DR1 = Utilities::Multiply(InvI1, FVec3::CrossProduct(Xs[1] - Ps[1], -DX));
			const FVec3 DV1 = -InvMs[1] * DV;
			const FVec3 DW1 = Utilities::Multiply(InvI1, FVec3::CrossProduct(Xs[1] - Ps[1], -DV));

			FReal LinearProjection = FPBDJointUtilities::GetLinearProjection(SolverSettings, JointSettings);
			ApplyPositionDelta(1, LinearProjection, DP1);
			ApplyRotationDelta(1, LinearProjection, DR1);
			ApplyVelocityDelta(1, LinearProjection, DV1, DW1);
			UpdateDerivedState(1);
		}
	}
}
