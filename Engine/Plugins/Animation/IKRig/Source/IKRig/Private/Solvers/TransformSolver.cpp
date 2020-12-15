// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	TransformSolver.cpp: Solver execution class for Transform
=============================================================================*/

#include "TransformSolver.h"
#include "IKRigDataTypes.h"
#include "IKRigHierarchy.h"
#include "Solvers/TransformSolverDefinition.h"

void UTransformSolver::InitInternal(const FIKRigTransformModifier& InGlobalTransform)
{
	
}

bool UTransformSolver::IsSolverActive() const
{
	if (Super::IsSolverActive())
	{
		if (UTransformSolverDefinition* SolverDef = Cast<UTransformSolverDefinition>(SolverDefinition))
		{
			return (SolverDef->bEnablePosition || SolverDef->bEnableRotation);
		}
	}

	return false;
}

void UTransformSolver::SolveInternal(FIKRigTransformModifier& InOutGlobalTransform, FControlRigDrawInterface* InOutDrawInterface)
{
	if (UTransformSolverDefinition* SolverDef = Cast<UTransformSolverDefinition>(SolverDefinition))
	{
		FIKRigTarget Target;
		if (GetEffectorTarget(SolverDef->TransformTarget, Target))
		{
			int32 Index = InOutGlobalTransform.Hierarchy->GetIndex(SolverDef->TransformTarget.Bone);
			if (Index != INDEX_NONE)
			{
				FTransform CurrentTransform = InOutGlobalTransform.GetGlobalTransform(Index);

				if (SolverDef->bEnablePosition)
				{
					CurrentTransform.SetLocation(Target.PositionTarget.Position);
				}
				if (SolverDef->bEnableRotation)
				{
					CurrentTransform.SetRotation(Target.RotationTarget.Rotation.Quaternion());
				}

				InOutGlobalTransform.SetGlobalTransform(Index, CurrentTransform, true);
			}
		}
	}
}